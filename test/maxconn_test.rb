require 'erb'
require 'socket'
require 'rubygems'
require 'rack'
require 'thread'

class String
  def /(x)
    File.expand_path(File.join(self, x))
  end
end

def assert(x, msg="")
  raise "failure #{msg}" unless x
end

def assert_equal(a, b, msg = "")
  unless a == b
    $stderr.puts "\n<#{a}> != <#{b}>"
    raise "failure: #{msg}" 
  end
end

def assert_in_delta(a, b, delta, msg ="")
  unless (a - b).abs < delta
    $stderr.puts "\n<#{a}> is not within <#{delta}> of <#{b}>"
    raise "failure: #{msg}" 
  end
end

def httperf_parse_output(output) # from topfunky's bong
  stat = {}

  # Total: connections 5 requests 5 replies 5 test-duration 0.013 s
  stat['duration'] = output.scan(/test-duration ([\d.]+)/).flatten.first.to_f

  # Reply rate [replies/s]: min 0.0 avg 0.0 max 0.0 stddev 0.0 (0 samples)
  (stat['min'], stat['avg'], stat['max'], stat['stddev'], stat['samples']) = output.scan(/Reply rate \[replies\/s\]: min ([\d.]+) avg ([\d.]+) max ([\d.]+) stddev ([\d.]+) \((\d+) samples\)/).flatten.map { |i| i.to_f }

  # Reply status: 1xx=0 2xx=5 3xx=0 4xx=0 5xx=0
  (stat['1xx'], stat['2xx'], stat['3xx'], stat['4xx'], stat['5xx']) = output.scan(/Reply status: 1xx=(\d+) 2xx=(\d+) 3xx=(\d+) 4xx=(\d+) 5xx=(\d+)/).flatten.map { |i| i.to_f }

  stat['avg_low'] = stat['avg'].to_f - 2.0 * stat['stddev'].to_f
  stat['avg_high'] = stat['avg'].to_f + 2.0 * stat['stddev'].to_f

  stat
end


def test_nginx(backends, options ={})
  max_connections  = options[:max_connections]  || 1
  worker_processes = options[:worker_processes] || 1
  if options.has_key?(:use_ssl)
    use_ssl = options[:use_ssl] 
  else
    use_ssl = false
  end

  nginx = MaxconnTest::Nginx.new(backends,
    :max_connections => max_connections,
    :worker_processes => worker_processes,
    :use_ssl => use_ssl
  )
  nginx.start
  yield(nginx)

  sleep 1.5 # let the logs catch up


  #make sure it doesn't silently fail on assert
  out = %x{grep "Assertion" #{nginx.logfile}}
  assert_equal "", out, "should be no assertion failures"

  out = %x{egrep "SIGCHLD|SIGTERM" #{nginx.logfile}}
  assert_equal "", out, "should be no worker crashes"

  true
ensure
  backends.each { |b| b.shutdown } 
  nginx.shutdown
end


module MaxconnTest
  DIR       = File.dirname(__FILE__)
  TMPDIR    = DIR / "tmp"
  NGINX_BIN = DIR / "../.nginx/sbin/nginx"
  DEFAULTS = {
    :port                => 8000,
    :nbackends           => 4,
    :req_per_backend     => 40,
    :max_connections     => 3,
    :worker_processes    => 3,
    :use_ssl             => false,
    :nginx_conf_filename => "nginx.conf",
    :nginx_log_filename  => "nginx.log"
  }

  class Backend # abstract
    attr_reader :port
    def logfile
      TMPDIR / "backend-#{port}.log"
    end

    def initialize
      @concurrent_connections = 0
      @connections = 0
      @max_concurrent_connections = 0
      @need_update = true
      @lock = Mutex.new
    end

    def experienced_max_connections
      log[:max_connections]
    end

    def experienced_requests
      log[:requests]
    end

    def increase_connections
      @lock.synchronize do 
        @concurrent_connections += 1
        @connections += 1
        if @max_concurrent_connections < @concurrent_connections
          @max_concurrent_connections = @concurrent_connections
        end
        @need_update = true 
      end
    end

    def decrease_connections
      @lock.synchronize do 
        @need_update = true 
        @concurrent_connections -= 1;
      end
    end

    protected

    def log
      last_report = %x{tail -n1 #{logfile}}
      unless last_report =~ /max:(\d+) total:(\d+)/
        puts "unknown report #{last_report.inspect} for #{port} backend."
        return {}
      end
      {:max_connections => $1.to_i, :requests => $2.to_i}
    end

    def output_stats(file)
      file.puts "max:#{@max_concurrent_connections} total:#{@connections}"
      file.flush
      $stderr.print identifier # to see that everything is working
      $stderr.flush
    end

    def identifier
      @identifier ||= port.to_s.slice(-1,1) 
    end
  end

  class MongrelBackend < Backend

    def initialize
      super()
    end

    def start(port)
      unless @pid.nil?
        $stderr.puts "trying to start mongrel that is already running!"
        shutdown
      end
      @port = port
      File.unlink(logfile) if File.exists? logfile 
      @pid = fork do 
        File.open(logfile, "w+") do |f|
          $stderr.puts "Backend running on #{port}" if $DEBUG
          Thread.new do 
            loop do 
              @lock.synchronize do
                output_stats(f) if @need_update
                @need_update = false
              end
              sleep 1
            end
          end
          trap("INT") { exit 0 }
          Rack::Handler::Mongrel.run(self, :Port => port) 
        end
      end
    end

    def call(env)
      increase_connections
      real_call(env)
    ensure
      decrease_connections
    end

    def shutdown
      return if @pid.nil?
      Process.kill("SIGINT", @pid)
      $stderr.puts "killed mongrel #{@port}" if $DEBUG
      @pid = nil
    end
  end

  class DelayBackend < MongrelBackend

    def initialize(delay)
      @delay = delay
      super()
    end

    def real_call(env)
      status = 200
      sleep @delay
      body = "The time is #{Time.now}\n"
      [status, {"Content-Type" => "text/plain"}, body]
    end
  end

  class NoResponseBackend < DelayBackend
    def initialize
      super(99999999)
    end
  end

  class ClosingBackend < Backend
    def initialize(delay)
      @delay = delay
      super()
    end

    def start(port)
      unless @pid.nil?
        $stderr.puts "trying to start backend that is already running!"
        shutdown
      end
      @port = port
      File.unlink(logfile) if File.exists? logfile 
      @pid = fork do 
        File.open(logfile, "w+") do |f|
          $stderr.puts "server running on #{port}" if $DEBUG
          Thread.new do 
            loop do 
              @lock.synchronize do
                output_stats(f) if @need_update
                @need_update = false
              end
              sleep 1
            end
          end
          trap("INT") { exit 0 }
          server_loop
        end
      end
    end

    def server_loop
      server = TCPServer.new(@port) 
      while true
        IO.select([server])
        begin
          client = server.accept_nonblock
          Thread.new(client) { |c| call(c) }
        rescue Errno::EAGAIN, Errno::ECONNABORTED, Errno::EPROTO, Errno::EINTR
        end
      end
    end

    def call(client)
      s = client.read(3)
      return false unless s == "GET" or s == "HEA" or s == "POS"
      increase_connections
      sleep @delay
    ensure
      client.close
      decrease_connections
    end

    def shutdown
      return if @pid.nil?
      Process.kill("SIGINT", @pid)
      $stderr.puts "killed server #{@port}" if $DEBUG
      @pid = nil
    end
  end

  class PostCheckBackend < MongrelBackend
    def real_call(env)
      h = {"Content-Type" => "text/plain"}
      if env['PATH_INFO'] =~ %r{post_check/(\d+)}
        expected_size = $1.to_i
        content_length = env['CONTENT_LENGTH'].to_i
        if content_length == expected_size 
          return [200, h, "content-length matches\n"]
        else
          return [300, h, "wrong size. got #{content_length}\n"]
        end
      end

      if env['PATH_INFO'] =~ %r{sleep/(\d+)}
        wait_time = $1.to_i
        sleep wait_time
      end

      [200, h, "okay\n"]
    end
  end

  class Nginx
    attr_reader :backends
    def initialize(backends, options = {})
      @backends = backends
      @options = DEFAULTS.merge(options)
    end

    def shutdown
      %x{fuser -s -k #{logfile}}
      if $?.exitstatus != 0
        puts "problem killing nginx"
        exit 1
      end
      $stderr.puts "killed nginx" if $DEBUG
    end

    def start
      p = port + 1 # for assigning ports to the backend
      backends.each do |backend|
        backend.start(p)
        wait_for_server_to_open_on backend.port
        p += 1
      end
      write_config
      File.unlink(logfile) if File.exists? logfile
      %x{#{NGINX_BIN} -c #{conffile}} 
      $stderr.puts "nginx running on #{port}" if $DEBUG
      wait_for_server_to_open_on port
    end
   
    def apache_bench(options)
      path = options[:path] || "/"
      requests = options[:requests] || 500
      concurrency = options[:concurrency] || 50
      concurrency = requests - 1 if concurrency > requests
      out = %x{ab -q -c #{concurrency} -n #{requests}  #{use_ssl? ? "https" : "http"}://localhost:#{port}#{path}}
      if $?.exitstatus != 0 
        $stderr.puts "ab failed"
        $stderr.puts out
        exit 1
      end

      unless out =~ /Complete requests:\ *(\d+)/
        $stderr.puts "ab failed"
        $stderr.puts out
        exit 1
      end

      complete_requests = $1.to_i

      if complete_requests != requests
        $stderr.puts "only had #{complete_requests} of #{requests}"
        exit 1
      end
    end

    def use_ssl?
      @options[:use_ssl]
    end

    def cert
      DIR / "cert.pem"
    end

    def cert_key
      DIR / "cert.key"
    end

    def conffile
      TMPDIR / @options[:nginx_conf_filename]
    end

    def logfile
      TMPDIR / @options[:nginx_log_filename]
    end

    def port
      @options[:port]
    end

    def worker_processes
      @options[:worker_processes]
    end

    def max_connections
      @options[:max_connections]
    end

    def write_config
      template = File.read( DIR / "nginx.conf.erb" )
      File.open(conffile, "w+") do |f|
        f.write(ERB.new(template).result(binding))
      end
    end

    def wait_for_server_to_open_on(port)
      loop do
        begin 
          socket = ::TCPSocket.open("127.0.0.1", port)
          return
        rescue Errno::ECONNREFUSED
          $stderr.print "." if $DEBUG
          $stderr.flush
          sleep 0.2
        end
      end
    end
  end
end

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

  def self.test(backends, options ={})
    req_per_backend  = options[:req_per_backend]  || 100
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


    nginx.apache_bench(
      :path => "/",
      :requests => req_per_backend * backends.length, 
      :concurrency => 50
    )
    sleep 1.5 # let the logs catch up
    true
  ensure
    backends.each { |b| b.shutdown } 
    nginx.shutdown
  end

  class Backend # abstract
    attr_reader :port
    def logfile
      TMPDIR / "backend-#{port}.log"
    end

    def experienced_max_connections
      log[:max_connections]
    end

    def experienced_requests
      log[:requests]
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
      $stderr.print "'" # to see that everything is working
      $stderr.flush
    end
  end

  class DelayBackend < Backend
    def initialize(delay)
      @delay = delay
      @concurrent_connections = 0
      @connections = 0
      @max_concurrent_connections = 0
      @need_update = true
      @lock = Mutex.new
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
          $stderr.puts "DelayBackend running on #{port}" if $DEBUG
          Thread.new do 
            loop do 
              @lock.synchronize do
                output_stats(f) if @need_update
                @need_update = false
              end
              sleep 1
            end
          end
          Rack::Handler::Mongrel.run(self, :Port => port) 
        end
      end
    end

    def shutdown
      return if @pid.nil?
      Process.kill("SIGHUP", @pid)
      $stderr.puts "killed mongrel #{@port}" if $DEBUG
      @pid = nil
    end

    def call(env)
      @lock.synchronize do 
        @concurrent_connections += 1
        @connections += 1
        if @max_concurrent_connections < @concurrent_connections
          @max_concurrent_connections = @concurrent_connections
        end
        @need_update = true 
      end

      status = 200
      sleep @delay
      body = "The time is #{Time.now}\n"

      @lock.synchronize do 
        @need_update = true 
        @concurrent_connections -= 1;
      end

      [status, {"Content-Type" => "text/plain"}, body]
    end
  end

  class NoResponseBackend < DelayBackend
    def initialize
      super(99999999)
    end
  end

  class Nginx
    attr_reader :backends
    def initialize(backends, options = {})
      @backends = backends
      @options = DEFAULTS.merge(options)
    end

    def shutdown
      %x{pkill -f nginx}
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

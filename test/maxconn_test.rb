require 'erb'
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

  class Backend
    attr_reader :port
    attr_accessor :delay
    def initialize(p)
      @delay = 0
      @port = p
    end

    def logfile
      TMPDIR / "backend-#{@port}.log"
    end

    def shutdown
      Process.kill("SIGHUP", @pid)
      $stderr.puts "killed mongrel #{@port}"
      @pid = nil
    end

    def start
      unless @pid.nil?
        $stderr.puts "trying to start mongrel that is already running!"
        shutdown
      end
      File.unlink(logfile) if File.exists? logfile 
      @pid = fork do 
        File.open(logfile, "w+") do |f|
          $stderr.puts "Mongrel running on #{port}"
          app = MaxconnTest::UpstreamApplication.new(f)
          app.delay = @delay
          Thread.new(app) do |a|
            loop do 
              a.output_stats
              sleep 1
            end
          end
          Rack::Handler::Mongrel.run(app, :Port => port) 
        end
      end
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
  end

  class Nginx
    attr_reader :backends
    def initialize(options = {})
      @options = DEFAULTS.merge(options)
      @backends = []
      options[:nbackends].to_i.times do |i|
        @backends << Backend.new(i+1+port)
      end
    end

    def shutdown
      backends.each { |b| b.shutdown } 
      puts "killing nginx"
      %x{pkill -f nginx}
    end

    def wait_for_server_to_open_on(port)
      loop do
        begin 
          socket = TCPSocket.open("localhost", port)
          return
        rescue Errno::ECONNREFUSED
          $stderr.print "."
          $stderr.flush
          sleep 0.1
        end
      end
    end

    def start
      backends.each do |backend|
        backend.start
        wait_for_server_to_open_on backend.port
      end
      write_config
      File.unlink(logfile) if File.exists? logfile
      %x{#{NGINX_BIN} -c #{conffile}} 
      wait_for_server_to_open_on port
      $stderr.puts "nginx running on #{port}"
    end

    def apache_bench(options)
      path = options[:path] || "/"
      requests = options[:requests] || 500
      concurrency = options[:concurrency] || 50
      out = %x{ab -c #{concurrency} -n #{requests}  #{use_ssl? ? "https" : "http"}://localhost:#{port}#{path}}
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
  end

  class UpstreamApplication
    attr_accessor :delay
    def initialize(log)
      @log = log
      @concurrent_connections = 0
      @connections = 0
      @max_concurrent_connections = 0
      @need_update = true
      @lock = Mutex.new
      @delay = 0
    end

    def call(env)
      @lock.synchronize do 
        @concurrent_connections += 1
        @connections += 1
        if @max_concurrent_connections < @concurrent_connections
          @max_concurrent_connections = @concurrent_connections
        end
      end

      port = env["SERVER_PORT"]
      #@log.puts "#{PORT} connection to #{env["PATH_INFO"]}\n"
      if env["PATH_INFO"] =~ %r{/sleep/(\d+(\.\d+)?)}
        seconds = $1.to_f
        sleep seconds
      end
      sleep @delay

      body = "The time is #{Time.now}\n\nport = #{port}\nurl = #{env["PATH_INFO"]}\r\n"
      content_type = "text/plain"

      @lock.synchronize do 
        @need_update = true 
        @concurrent_connections -= 1;
      end

      [200, {"Content-Type" => content_type}, body]
    end

    def output_stats
      @lock.synchronize do
        if @need_update
          @log.puts "max:#{@max_concurrent_connections} total:#{@connections}"
          @log.flush
          @need_update = false
        end
      end
    end
  end
end

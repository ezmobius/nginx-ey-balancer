#!/usr/bin/env ruby 

require 'erb'

class String
  def /(x)
    File.expand_path(File.join(self, x))
  end
end

DIR = File.dirname(__FILE__) 
NGINX_PORT = 8000
NUMBER_OF_BACKENDS = 10 
REQ_PER_BACKEND = 100
NGINX_BIN = DIR / "../.nginx/sbin/nginx"
BACKEND_BIN = DIR / "upstream.rb"
MAX_CONNECTIONS = 3
TMPDIR  = DIR / "tmp"
NGINX_CONF_TEMPLATE = DIR / "nginx.conf.erb"
NGINX_CONF_FILE = TMPDIR / "nginx.conf"

def shutdown
  puts "killing nginx"
  %x{pkill -f nginx}
  puts "killing test mongrels"
  %x{pkill -f maxconn_mongrel}
end

def each_backend
  NUMBER_OF_BACKENDS.times do |i|
    port = NGINX_PORT + 1 + i
    logfile = TMPDIR / "mongrel_#{port}.log"
    yield port, logfile
  end
end


begin
  nginx_conf_template = ERB.new( File.read( NGINX_CONF_TEMPLATE ) )
  nginx_log = TMPDIR / "nginx.log"
  nginx_port = NGINX_PORT
  max_connections = MAX_CONNECTIONS
  File.open(NGINX_CONF_FILE, "w+") do |f|
    f.write nginx_conf_template.result(binding)
  end


  each_backend do |port, logfile|
    %x{rm #{logfile}}
    %x{ruby #{BACKEND_BIN} #{port} > #{logfile} &}
    sleep 1
  end

  # remove old log file.
  %x{rm #{nginx_log}}
  %x{#{NGINX_BIN} -c #{NGINX_CONF_FILE}}
  sleep 2 


  total_requests = REQ_PER_BACKEND*NUMBER_OF_BACKENDS
  out = %x{ab -c 50 -n #{total_requests}  http://localhost:#{NGINX_PORT}/sleep/}
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

  if complete_requests != total_requests
    $stderr.puts "only had #{complete_requests} of #{total_requests}"
    exit 1
  end


  each_backend do |port, logfile|
    last_report = %x{tail -n1 #{logfile}}
    unless last_report =~ /max:(\d+) total:(\d+)/
      puts "unknown report #{last_report.inspect} for #{port} backend."
      exit 1
    end

    maxconn = $1.to_i 
    total_requests = $2.to_i

    if maxconn != MAX_CONNECTIONS
      puts "on backend #{port}, max connection should be 2 but was #{maxconn}"
      exit 1
    end

    if total_requests != REQ_PER_BACKEND
      puts "on backend #{port}, #{total_requests} were recieved but should be #{REQ_PER_BACKEND}"
      puts "unbalanced."
      exit 1
    end
  end

  # check out the logs
  # to make sure the maxconn was correct
  #
  #
  # should also test to see if the requests were evenly distributed across
  # the mongrels.
  # each should have recieved about REQ_PER_BACKEND

  puts "okay!"
ensure
  shutdown
end

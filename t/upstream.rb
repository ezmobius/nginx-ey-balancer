require 'rubygems'
require 'net/http'
require 'rack'
require 'ruby-debug'

$max_connections = 0;


class Application
  def initialize
    @connections = 0;
  end

  def call(env)
    @connections += 1;
    if $max_connections < @connections
      $max_connections = @connections
    end

    port = env["SERVER_PORT"]
    #puts "#{PORT} connection to #{env["PATH_INFO"]}\n"
    sleep 1 if env["PATH_INFO"] =~ /sleep/
    body = "The time is #{Time.now}\n\nport = #{port}\nurl = #{env["PATH_INFO"]}\r\n"
    content_type = "text/plain"

    @connections -= 1;
    [200, {"Content-Type" => content_type}, body]
  end
end

PORT = ARGV[0] || 8001
puts "Running at http://localhost:#{PORT}"
Thread.new do
  last = -1
  loop do 
    if last != $max_connections
      puts "localhost:#{PORT} max connections = #{$max_connections}"
      last = $max_connections
    end
    sleep 0.5 
  end
end
Rack::Handler::Mongrel.run(Application.new, :Port => PORT) 

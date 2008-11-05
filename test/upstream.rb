require 'rubygems'
require 'rack'
require 'thread'

$concurrent_connections = 0
$connections = 0
$max_concurrent_connections = 0
$need_update = true

$lock = Mutex.new

class Application
  def call(env)
    $lock.synchronize do 
      $concurrent_connections += 1
      $connections += 1
      if $max_concurrent_connections < $concurrent_connections
        $max_concurrent_connections = $concurrent_connections
      end
    end

    port = env["SERVER_PORT"]
    #puts "#{PORT} connection to #{env["PATH_INFO"]}\n"
    if env["PATH_INFO"] =~ /sleep/
      sleep 1 
    end

    body = "The time is #{Time.now}\n\nport = #{port}\nurl = #{env["PATH_INFO"]}\r\n"
    content_type = "text/plain"

    $lock.synchronize do 
      $need_update = true 
      $concurrent_connections -= 1;
    end

    [200, {"Content-Type" => content_type}, body]
  end
end

def output_stats
  $lock.synchronize do
    if $need_update
      $stdout.puts "max:#{$max_concurrent_connections} total:#{$connections}"
      $stdout.flush
      $need_update = false
    end
  end
end

if __FILE__ == $0
  $0 = "maxconn_mongrel"
  PORT = ARGV[0].to_i

  $stderr.puts "Running at http://localhost:#{PORT}"

  Thread.new do
    loop do 
      output_stats
      sleep 1
    end
  end

  Rack::Handler::Mongrel.run(Application.new, :Port => PORT) 
end


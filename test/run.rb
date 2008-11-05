
NGINX_PORT = 8000
NUMBER_OF_BACKENDS = 4
REQ_PER_BACKEND = 500

def shutdown
  %x{pkill -f nginx}
  %x{pkill -f maxconn_mongrel}
end

NUMBER_OF_BACKENDS.times do |i|
  port = NGINX_PORT + i
  %x{ruby maxconn/upstream.rb #{port} > maxconn/log/mongrel_#{port}.log &}
end

%x{$NGINX_BIN -c maxconn/nginx.conf}

total_requests = REQ_PER_BACKEND*NUMBER_OF_BACKENDS
out = %x{ab -c 50 -n #{total_requests}  http://localhost:#{NGINX_PORT}/sleep/}
if $?.exitstatus != 0 
  shutdown
  $stderr.puts "ab failed"
  exit 1
end
# check that out shows that there were total_requests successful.


# check out the logs
# to make sure the maxconn was correct
#
#
# should also test to see if the requests were evenly distributed across
# the mongrels.
# each should have recieved about REQ_PER_BACKEND


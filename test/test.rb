require File.dirname(__FILE__) + '/maxconn_test'

#
# Test 1 
#
begin
  req_per_backend = 200
  nbackends = 15
  max_connections = 2
  nginx = MaxconnTest::Nginx.new(
    :max_connections => max_connections,
    :nbackends => nbackends
  )
  nginx.start
  nginx.apache_bench(
    :path => "/sleep?t=1",
    :requests => req_per_backend*nbackends, 
    :concurrency => 20
  )
  nginx.backends.each do |backend|
    if backend.experienced_max_connections !=  max_connections
      $stderr.puts "backend #{backend.port} had #{backend.experienced_max_connections} max_connections but should have been #{max_connections}"
    end
    if backend.experienced_requests != req_per_backend
      $stderr.puts "backend #{backend.port} had #{backend.experienced_requests} requests but should have been #{req_per_backend}"
    end
  end
ensure
  nginx.shutdown
end

#
# Test 2 
#
#nginx = MaxconnTest::Nginx.new(:nbackends => 10)
#nginx.backends.first.pause!
#nginx.start
#nginx.pound(
#  :url => "/sleep?t=1",
#  :requests => 200, 
#  :concurrency => 20
#)
#nginx.backends.first.resume!
#nginx.pound(
#  :url => "/sleep?t=1",
#  :requests => 200, 
#  :concurrency => 20
#)
#nginx.shutdown

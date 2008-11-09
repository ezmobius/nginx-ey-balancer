require File.dirname(__FILE__) + '/maxconn_test'

#
# Test 1 
#
begin
  req_per_backend = 200
  nbackends = 5
  max_connections = 2
  worker_processes = 1
  nginx = MaxconnTest::Nginx.new(
    :max_connections => max_connections,
    :nbackends => nbackends,
    :worker_processes => worker_processes
  )
  nginx.start
  nginx.apache_bench(
    :path => "/sleep?t=1",
    :requests => req_per_backend*nbackends, 
    :concurrency => 20
  )
  nginx.backends.each do |backend|
    expected_maxconn = max_connections * worker_processes
    if backend.experienced_max_connections != expected_maxconn
      $stderr.puts "backend #{backend.port} had #{backend.experienced_max_connections} max_connections but should have been #{expected_maxconn}"
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

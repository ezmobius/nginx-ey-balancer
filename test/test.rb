require File.dirname(__FILE__) + '/maxconn_test'

#
# Test 1 
#
MaxconnTest.run(
  :req_per_backend => 500,
  :nbackends => 5,
  :max_connections => 4,
  :worker_processes => 1,
  :request_delay => 1
)

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

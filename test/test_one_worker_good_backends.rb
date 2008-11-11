require File.dirname(__FILE__) + '/maxconn_test'

backends = []
4.times { backends << MaxconnTest::DelayBackend.new(0.4) }
MaxconnTest.test(backends,
  :req_per_backend => 50,
  :max_connections => 2, # per backend, per worker
  :worker_processes => 1
)
total_received = 0
backends.each do |b|
  assert_in_delta(50, b.experienced_requests, 5, 
    "backend #{b.port} is not balanced")

  assert(b.experienced_max_connections <= 2, 
    "backend #{b.port} had too many connections")

  total_received += b.experienced_requests
end
assert_equal 50*backends.length, total_received, "backends did not recieve all requests"

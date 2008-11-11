require File.dirname(__FILE__) + '/maxconn_test'

no_response = MaxconnTest::NoResponseBackend.new
other_backends = []
3.times { other_backends << MaxconnTest::DelayBackend.new(0.2) }
MaxconnTest.test([no_response, *other_backends],
  :req_per_backend => 20,
  :max_connections => 2, # per backend, per worker
  :worker_processes => 1
)
# total 80 requests total

# 2 get caught in the no_response backend
assert_equal(2, no_response.experienced_requests, "no_response should only get 2 requests")
assert_equal(2, no_response.experienced_max_connections)
total_received = 2

# 78 left. 78/3 = 26

other_backends.each do |b|
  assert_in_delta(26, b.experienced_requests, 2, 
    "backend #{b.port} is not balanced")

  assert(b.experienced_max_connections <= 2, 
    "backend #{b.port} had too many connections")

  total_received += b.experienced_requests
end
assert_equal 80, total_received, "backends did not recieve all requests"

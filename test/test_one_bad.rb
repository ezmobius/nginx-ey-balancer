require File.dirname(__FILE__) + '/maxconn_test'

no_response = MaxconnTest::NoResponseBackend.new
other_backends = []
3.times { other_backends << MaxconnTest::DelayBackend.new(0.2) }

test_nginx([no_response, *other_backends],
  :max_connections => 2, # per backend, per worker
  :worker_processes => 1
) do |nginx|
  out = %x{httperf --num-conns 80 --hog --timeout 10 --rate 100 --port #{nginx.port} --uri / }
  assert $?.exitstatus == 0
  results = httperf_parse_output(out)
  assert_equal 78, results["2xx"]
end
# total 80 requests total

# 2 get caught in the no_response backend
assert_equal(2, no_response.experienced_requests, "no_response should only get 2 requests")
assert_equal(2, no_response.experienced_max_connections)
total_received = 2

# 78 left. 78/3 = 26

other_backends.each do |b|
  assert_in_delta(26, b.experienced_requests, 4, 
    "backend #{b.port} is not balanced")

  assert_equal(b.experienced_max_connections, 2, 
    "backend #{b.port} had too many connections")

  total_received += b.experienced_requests
end
assert_equal 80, total_received, "backends did not recieve all requests"

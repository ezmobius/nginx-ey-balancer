require File.dirname(__FILE__) + '/maxconn_test'

backends = []
4.times { backends << MaxconnTest::DelayBackend.new(0.4) }
test_nginx(backends,
  :max_connections => 2, # per backend, per worker
  :worker_processes => 1
) do |nginx|
  out = %x{httperf --num-conns 200 --hog --timeout 10 --rate 100 --port #{nginx.port} --uri / }
  assert $?.exitstatus == 0
  results = httperf_parse_output(out)
  assert_equal 200, results["2xx"]
end

total_received = 0
backends.each do |b|
  assert_in_delta(50, b.experienced_requests, 10, 
    "backend #{b.port} is not balanced")

  assert(b.experienced_max_connections <= 2, 
    "backend #{b.port} had too many connections")

  total_received += b.experienced_requests
end
assert_equal 200, total_received, "backends did not recieve all requests"

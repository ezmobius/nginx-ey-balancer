require File.dirname(__FILE__) + '/maxconn_test'
backends = []
2.times { backends << MaxconnTest::ClosingBackend.new(0.0) }
test_nginx(backends,
  :max_connections => 5, # per backend, per worker
  :worker_processes => 1
) do |nginx|
  out = %x{httperf --num-conns 10 --hog --timeout 15 --rate 100 --port #{nginx.port}}

  # each backend will fail for 10 seconds since it closed
  # it's connection.
  #
  # make sure httperf's timeout is large enough such that
  # all the requests die by being sent to the backends or 
  # being killed on the queue

  assert $?.exitstatus == 0
  results = httperf_parse_output(out)
  assert_equal 0, results["2xx"]
  assert_equal 10, results["5xx"]
end

backends.each do |b|
  # should only get one max connection because after the backend dies, it
  # should not receive another request
  assert_equal(1, b.experienced_max_connections) 
end


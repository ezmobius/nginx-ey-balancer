require File.dirname(__FILE__) + '/maxconn_test'
backends = []
5.times { backends << MaxconnTest::ClosingBackend.new }
test_nginx(backends,
  :max_connections => 1, # per backend, per worker
  :worker_processes => 1, 
  :backend_timeouts => 1
) do |nginx|
  out = %x{httperf --num-conns 20 --hog --timeout 15 --rate 2 --port #{nginx.port}}

  # each backend will fail for 1 seconds since it closed
  # it's connection.
  # should take 4 seconds for the 5 backends to handle all 20 req

  assert $?.exitstatus == 0
  results = httperf_parse_output(out)
  assert_equal 0, results["2xx"]
  assert_equal 20, results["5xx"]
end

backends.each do |b|
  # because of forced connections the backends 
  # might see a few more connections from the downed hosts
  assert(b.experienced_max_connections <= 3) 
end


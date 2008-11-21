require File.dirname(__FILE__) + '/maxconn_test'
include MaxconnTest

DELAY = 0.2

delay = DelayBackend.new(DELAY)
non = NonBackend.new()

test_nginx( [delay, non],
  :max_connections => 2, # per backend, per worker
  :worker_processes => 1
) do |nginx|
  out = %x{httperf --num-conns 150 --hog --timeout 120 --rate 10 --port #{nginx.port}}
  assert $?.exitstatus == 0
  results = httperf_parse_output(out)
  p results
  assert_equal 150, results["2xx"]
end

# total requests should be 150
# expect that fast handles 100 and slow handles 50
assert_equal 150, delay.experienced_requests 
assert_equal 0, non.experienced_requests 
assert_equal 2, delay.experienced_max_connections
assert_equal 0, non.experienced_max_connections


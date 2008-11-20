require File.dirname(__FILE__) + '/maxconn_test'
include MaxconnTest

DELAY = 0.4

fast = DelayBackend.new(DELAY)
slow = DelayBackend.new(DELAY*2)

test_nginx( [fast, slow],
  :max_connections => 2, # per backend, per worker
  :worker_processes => 1,
  :queue_timeout => "10s"
) do |nginx|
  out = %x{httperf --num-conns 150 --hog --timeout 120 --rate 10 --port #{nginx.port}}
  assert $?.exitstatus == 0
  results = httperf_parse_output(out)
  assert_equal 150, results["2xx"]
end

# total requests should be 150
# expect that fast handles 100 and slow handles 50
assert_in_delta(100, fast.experienced_requests, 5)
assert_in_delta(50, slow.experienced_requests, 5)
assert_equal fast.experienced_max_connections, 2
assert_equal slow.experienced_max_connections, 2
assert_equal 150, fast.experienced_requests + slow.experienced_requests


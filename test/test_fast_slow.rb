require File.dirname(__FILE__) + '/maxconn_test'

fast = MaxconnTest::DelayBackend.new(0.8)
slow = MaxconnTest::DelayBackend.new(1.6)
MaxconnTest.test( [fast, slow],
  :req_per_backend => 75,
  :max_connections => 2, # per backend, per worker
  :worker_processes => 2
)
# total requests should be 150
# expect that fast handles 100 and slow handles 50
assert_in_delta(100, fast.experienced_requests, 5)
assert_in_delta(50, slow.experienced_requests, 5)
assert(fast.experienced_max_connections <= 4)
assert(slow.experienced_max_connections <= 4)
assert_equal 150, fast.experienced_requests + slow.experienced_requests


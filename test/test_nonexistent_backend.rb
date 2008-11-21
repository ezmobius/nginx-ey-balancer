require File.dirname(__FILE__) + '/maxconn_test'
include MaxconnTest

DELAY = 0.2
NUMBER_OF_DELAYS = 3
NUMBER_OF_NONS = 5

delays = []
NUMBER_OF_DELAYS.times { delays << DelayBackend.new(DELAY) }
nons = []
NUMBER_OF_NONS.times { nons << NonBackend.new() }

test_nginx( nons + delays,
  :max_connections => 2, # per backend, per worker
  :worker_processes => 1
) do |nginx|
  out = %x{httperf --num-conns 150 --hog --timeout 120 --rate 10 --port #{nginx.port}}
  assert $?.exitstatus == 0
  results = httperf_parse_output(out)
  assert_equal 150, results["2xx"]
end

delays.each do |delay|
  assert_in_delta 150/NUMBER_OF_DELAYS, delay.experienced_requests, 10
  # In the case that a backend goes down the module must find a place for
  # dispatched requests sent to the downed backend. In this extraordinary
  # case it's possible the max connections can go above the set amount.
  assert delay.experienced_max_connections <= 4
end

nons.each do |non|
  assert_equal 0, non.experienced_max_connections
  assert_equal 0, non.experienced_requests 
end

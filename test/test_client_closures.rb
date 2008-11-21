require File.dirname(__FILE__) + '/maxconn_test'
backend = MaxconnTest::DelayBackend.new(0.9)
test_nginx([backend],
  :max_connections => 2, # per backend, per worker
  :worker_processes => 1
) do |nginx|
  # Pound the server with connections which close on the client-side
  # immeditaely after hitting. (Note --timeout 0.01)
  50.times do 
    %x{httperf --num-conns 20 --hog --timeout 0.01 --rate 100 --port #{nginx.port}}
    assert $?.exitstatus == 0
  end
  #out = %x{grep "add queue" #{nginx.logfile} | wc -l}
  #assert $?.exitstatus == 0
  # 50 * 20 = 1000
  #assert( out.to_i > 900, 
  #       "at least 900 connections should be added to the queue")
end


# Okay - we allow it to grow above the given max_connection
# because the nginx module had to half-close the upstream 
# connection - that means Mongrel has to handle an exception
# before it clears that connection. The nginx module waits a little to
# allow the backend time to clear the connction but it could be 
# still there.
#
# This is, perhaps, acceptable since HAproxy does the same.
#
# The important thing is that of the 50*20=1000 connects that were
# created only very few actually got to the backend.
assert(backend.experienced_max_connections <= 6) 

# TODO assert that all the connections were dropped? 


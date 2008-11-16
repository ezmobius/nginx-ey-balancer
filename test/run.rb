require File.dirname(__FILE__) + '/maxconn_test'
DELAY = 0.4
fast = MaxconnTest::DelayBackend.new(DELAY)
slow = MaxconnTest::DelayBackend.new(DELAY*2)
test_nginx([fast], 
    :max_connections => 1,
    :worker_processes => 1,
    :use_ssl => false
) do
  done = false
  trap("INT") do 
    done = true
    puts
  end
  sleep 0.5 while not done 
end

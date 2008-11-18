require File.dirname(__FILE__) + '/maxconn_test'
DELAY = 9
#backends= [MaxconnTest::ClosingBackend.new(DELAY)]

backends = []
1.times { backends << MaxconnTest::DelayBackend.new(DELAY) }

test_nginx(backends, 
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

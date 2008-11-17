require File.dirname(__FILE__) + '/maxconn_test'
DELAY = 0.4
backend = MaxconnTest::PostCheckBackend.new()
test_nginx([backend], 
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

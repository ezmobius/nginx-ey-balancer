require File.dirname(__FILE__) + '/maxconn_test'
DELAY = 0.4
begin
  fast = MaxconnTest::DelayBackend.new(DELAY)
  slow = MaxconnTest::DelayBackend.new(DELAY*2)
  nginx = MaxconnTest::Nginx.new([fast, slow],
    :max_connections => 1,
    :worker_processes => 1,
    :use_ssl => false
  )
  nginx.start
  loop { sleep 10 }
ensure
  nginx.shutdown
end

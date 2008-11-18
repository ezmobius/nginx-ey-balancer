require File.dirname(__FILE__) + '/maxconn_test'

POST_SIZE = 2*1023
httperf_session = <<-EOF
/sleep/2 think=2.0
     /pict1.gif
     /pict2.gif
/sleep/2/post_check/#{POST_SIZE} method=POST contents=#{'D' * POST_SIZE}
     /pict3.gif
     /pict4.gif
EOF

backends = []
6.times { backends << MaxconnTest::PostCheckBackend.new }
test_nginx(backends,
  :max_connections => 2, # per backend, per worker
  :worker_processes => 3
) do |nginx|
  session_filename = MaxconnTest::TMPDIR / "httperf_session"
  File.open(session_filename, "w+") do |f|
    f.write(httperf_session)
  end
  out = %x{httperf --rate 50 --wsesslog=20,2,#{session_filename} --port #{nginx.port}}
  
  assert $?.exitstatus == 0
  results = httperf_parse_output(out)
  assert_equal 120, results["2xx"]
  assert_equal 0, results["3xx"]
end

total_received = 0
backends.each do |b|

  assert(b.experienced_max_connections <= 6, 
    "backend #{b.port} had too many connections")

  total_received += b.experienced_requests
end

assert_equal 120, total_received, "backends did not recieve all requests"


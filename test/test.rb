require File.dirname(__FILE__) + '/maxconn_test'
require 'test/unit'

puts "This takes a while (about 50 seconds)"

class FunctionalTests < Test::Unit::TestCase
  def test_ssl
    backends = []
    2.times { backends << MaxconnTest::DelayBackend.new(0.9) }
    MaxconnTest.test(backends,
      :req_per_backend => 10,
      :max_connections => 1, # per backend, per worker
      :worker_processes => 1,
      :use_ssl => true
    )
    total_received = 0
    backends.each do |b|
      assert_in_delta(10, b.experienced_requests, 2, 
        "backend #{b.port} is not balanced")

      assert(b.experienced_max_connections <= 1, 
        "backend #{b.port} had too many connections")

      total_received += b.experienced_requests
    end
    assert_equal 20, total_received, "backends did not recieve all requests"
  end

  def test_one_worker_good_backends
    backends = []
    4.times { backends << MaxconnTest::DelayBackend.new(0.4) }
    MaxconnTest.test(backends,
      :req_per_backend => 50,
      :max_connections => 2, # per backend, per worker
      :worker_processes => 1
    )
    total_received = 0
    backends.each do |b|
      assert_in_delta(50, b.experienced_requests, 5, 
        "backend #{b.port} is not balanced")

      assert(b.experienced_max_connections <= 2, 
        "backend #{b.port} had too many connections")

      total_received += b.experienced_requests
    end
    assert_equal 50*backends.length, total_received, "backends did not recieve all requests"
  end

  def test_fast_slow
    fast = MaxconnTest::DelayBackend.new(0.4)
    slow = MaxconnTest::DelayBackend.new(0.8)
    MaxconnTest.test( [fast, slow],
      :req_per_backend => 75,
      :max_connections => 2, # per backend, per worker
      :worker_processes => 2
    )
    # total requests should be 150
    # expect that fast handles 100 and slow handles 50
    total_received = 0
    assert_in_delta(100, fast.experienced_requests, 5)
    assert_in_delta(50, slow.experienced_requests, 5)

    assert(fast.experienced_max_connections <= 4)
    assert(slow.experienced_max_connections <= 4)

    assert_equal 150, fast.experienced_requests + slow.experienced_requests
  end

#  def test_one_bad
#    backends = [MaxconnTest::NoResponseBackend.new]
#    3.times { backends << MaxconnTest::DelayBackend.new(0.2) }
#    MaxconnTest.test(backends,
#      :req_per_backend => 10,
#      :max_connections => 2, # per backend, per worker
#      :worker_processes => 1
#    )
#    total_received = 0
#    backends.each do |b|
#      assert_in_delta(50, b.experienced_requests, 5, 
#        "backend #{b.port} is not balanced")
#
#      assert(b.experienced_max_connections <= 2, 
#        "backend #{b.port} had too many connections")
#
#      total_received += b.experienced_requests
#    end
#    assert_equal 50*backends.length, total_received, "backends did not recieve all requests"
#  end
end

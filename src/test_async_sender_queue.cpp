// Unit tests for AsyncSenderQueue: the bounded queue plus synchronization
// shared by the async delivery threads (SentryClient, JaegerLogger). Covers
// both overflow policies, batch extraction, the idle/stopped take results and
// the pending/flush bookkeeping. No threads are started here — the wait/take
// contract is exercised directly so the tests stay deterministic.
#include "async_sender_queue.hpp"
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <stop_token>
#include <thread>
#include <vector>

namespace {

using IntQueue = AsyncSenderQueue<int>;

} // namespace

TEST_CASE("AsyncSenderQueue: push then batch take preserves FIFO order",
          "[async-queue]") {
  IntQueue queue(8, IntQueue::OverflowPolicy::DropNewest);
  for (int i = 0; i < 5; ++i) {
    const auto outcome = queue.push(i);
    REQUIRE(outcome.m_accepted);
    REQUIRE_FALSE(outcome.m_dropped);
    REQUIRE(outcome.m_size == static_cast<size_t>(i + 1));
  }

  const std::stop_token st;
  std::vector<int> batch;
  const auto result = queue.wait_and_take(st, 0, 3, true, batch);
  REQUIRE(result == IntQueue::TakeResult::Batch);
  REQUIRE(batch == std::vector<int>{0, 1, 2});
  REQUIRE(queue.size() == 2);
}

TEST_CASE("AsyncSenderQueue: DropNewest keeps the oldest items on overflow",
          "[async-queue]") {
  IntQueue queue(2, IntQueue::OverflowPolicy::DropNewest);
  REQUIRE(queue.push(1).m_accepted);
  REQUIRE(queue.push(2).m_accepted);

  const auto dropped = queue.push(3);
  REQUIRE_FALSE(dropped.m_accepted);
  REQUIRE(dropped.m_dropped);
  REQUIRE(dropped.m_size == 2);

  const std::stop_token st;
  std::vector<int> batch;
  REQUIRE(queue.wait_and_take(st, 0, 10, true, batch) ==
          IntQueue::TakeResult::Batch);
  REQUIRE(batch == std::vector<int>{1, 2});
}

TEST_CASE("AsyncSenderQueue: DropOldest evicts the front but accepts the push",
          "[async-queue]") {
  IntQueue queue(2, IntQueue::OverflowPolicy::DropOldest);
  REQUIRE(queue.push(1).m_accepted);
  REQUIRE(queue.push(2).m_accepted);

  const auto outcome = queue.push(3);
  REQUIRE(outcome.m_accepted);
  REQUIRE(outcome.m_dropped);
  REQUIRE(outcome.m_size == 2);

  const std::stop_token st;
  std::vector<int> batch;
  REQUIRE(queue.wait_and_take(st, 0, 10, true, batch) ==
          IntQueue::TakeResult::Batch);
  REQUIRE(batch == std::vector<int>{2, 3});
}

TEST_CASE("AsyncSenderQueue: idle timeout returns Idle with an empty batch",
          "[async-queue]") {
  IntQueue queue(4, IntQueue::OverflowPolicy::DropNewest);
  const std::stop_token st;
  std::vector<int> batch;
  const auto result = queue.wait_and_take(st, 5, 4, true, batch);
  REQUIRE(result == IntQueue::TakeResult::Idle);
  REQUIRE(batch.empty());
}

TEST_CASE("AsyncSenderQueue: stop with drain_on_stop keeps taking, false stops",
          "[async-queue]") {
  const std::stop_source source;
  source.request_stop();
  const std::stop_token st = source.get_token();

  IntQueue draining(4, IntQueue::OverflowPolicy::DropNewest);
  draining.push(7);
  std::vector<int> batch;
  REQUIRE(draining.wait_and_take(st, 0, 4, true, batch) ==
          IntQueue::TakeResult::Batch);
  REQUIRE(batch == std::vector<int>{7});
  batch.clear();
  REQUIRE(draining.wait_and_take(st, 0, 4, true, batch) ==
          IntQueue::TakeResult::Stopped);

  IntQueue stopping(4, IntQueue::OverflowPolicy::DropNewest);
  stopping.push(8);
  REQUIRE(stopping.wait_and_take(st, 0, 4, false, batch) ==
          IntQueue::TakeResult::Stopped);
  REQUIRE(batch.empty());
  REQUIRE(stopping.size() == 1);
}

TEST_CASE("AsyncSenderQueue: mark_processed releases flush", "[async-queue]") {
  IntQueue queue(4, IntQueue::OverflowPolicy::DropNewest);
  queue.push(1);
  queue.push(2);

  const std::stop_token st;
  std::vector<int> batch;
  REQUIRE(queue.wait_and_take(st, 0, 4, true, batch) ==
          IntQueue::TakeResult::Batch);

  // flush must not return while the taken batch is still marked pending.
  std::atomic<bool> flushed{false};
  std::thread waiter([&] {
    queue.flush();
    flushed.store(true);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  REQUIRE_FALSE(flushed.load());

  queue.mark_processed(batch.size());
  waiter.join();
  REQUIRE(flushed.load());
}

TEST_CASE("AsyncSenderQueue: drain moves every remaining item out",
          "[async-queue]") {
  IntQueue queue(4, IntQueue::OverflowPolicy::DropNewest);
  queue.push(1);
  queue.push(2);
  queue.push(3);

  std::vector<int> drained;
  queue.drain(drained);
  REQUIRE(drained == std::vector<int>{1, 2, 3});
  REQUIRE(queue.size() == 0);
}

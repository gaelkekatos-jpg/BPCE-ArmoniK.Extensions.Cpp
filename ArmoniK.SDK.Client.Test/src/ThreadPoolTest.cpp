#include <gtest/gtest.h>

#include "ThreadPool.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

using namespace ArmoniK::Sdk::Client::Internal;

// Hangs are caught by the ctest TIMEOUT property set on the unit tests (see CMakeLists.txt)
constexpr auto TIMEOUT = std::chrono::seconds(1);

class ThreadPoolTest : public ::testing::Test {
protected:
  std::unique_ptr<armonik::api::common::logger::Logger> logger_;

  void SetUp() override {
    // Initialize logger for tests
    logger_.reset(new armonik::api::common::logger::Logger(armonik::api::common::logger::writer_console(),
                                                           armonik::api::common::logger::formatter_plain(true),
                                                           armonik::api::common::logger::Level::Verbose));
  }
};

TEST_F(ThreadPoolTest, ConstructorCreatesThreadPool) {
  ASSERT_NO_THROW({ ThreadPool pool(4, *logger_); });
}

TEST_F(ThreadPoolTest, SpawnSingleTask) {
  std::unique_ptr<ThreadPool> pool(new ThreadPool(2, *logger_));
  std::promise<void> task_promise;
  auto task_future = task_promise.get_future();

  pool->Spawn([&task_promise]() { task_promise.set_value(); });

  ASSERT_EQ(task_future.wait_for(TIMEOUT), std::future_status::ready);
  pool.reset();
}

TEST_F(ThreadPoolTest, SpawnMultipleTasks) {
  std::unique_ptr<ThreadPool> pool(new ThreadPool(4, *logger_));
  std::vector<std::future<void>> futures;

  for (int i = 0; i < 10; ++i) {
    std::promise<void> promise;
    futures.push_back(promise.get_future());
    pool->Spawn([promise = std::move(promise)]() mutable { promise.set_value(); });
  }

  for (size_t i = 0; i < futures.size(); ++i) {
    ASSERT_EQ(futures[i].wait_for(TIMEOUT), std::future_status::ready)
        << "Future " << i << " did not complete within timeout";
  }
  pool.reset();
}

TEST_F(ThreadPoolTest, JoinSetCreation) {
  ASSERT_NO_THROW({
    ThreadPool pool(2, *logger_);
    ThreadPool::JoinSet join_set(pool);
  });
}

TEST_F(ThreadPoolTest, JoinSetSpawnSingleTask) {
  std::unique_ptr<ThreadPool> pool(new ThreadPool(2, *logger_));
  std::unique_ptr<ThreadPool::JoinSet> join_set(new ThreadPool::JoinSet(*pool));
  std::promise<void> task_promise;
  auto task_future = task_promise.get_future();

  join_set->Spawn([&task_promise]() { task_promise.set_value(); });

  ASSERT_EQ(task_future.wait_for(TIMEOUT), std::future_status::ready);
  join_set.reset();
  pool.reset();
}

TEST_F(ThreadPoolTest, JoinSetSpawnMultipleTasks) {
  std::unique_ptr<ThreadPool> pool(new ThreadPool(4, *logger_));
  std::unique_ptr<ThreadPool::JoinSet> join_set(new ThreadPool::JoinSet(*pool));
  std::vector<std::future<void>> futures;

  for (int i = 0; i < 20; ++i) {
    std::promise<void> promise;
    futures.push_back(promise.get_future());
    join_set->Spawn([promise = std::move(promise)]() mutable { promise.set_value(); });
  }

  for (size_t i = 0; i < futures.size(); ++i) {
    ASSERT_EQ(futures[i].wait_for(TIMEOUT), std::future_status::ready)
        << "Future " << i << " did not complete within timeout";
  }
  join_set.reset();
  pool.reset();
}

TEST_F(ThreadPoolTest, JoinSetWaitsForCompletion) {
  std::unique_ptr<ThreadPool> pool(new ThreadPool(2, *logger_));
  auto count = std::make_shared<std::atomic<int>>();

  std::unique_ptr<ThreadPool::JoinSet> join_set(new ThreadPool::JoinSet(*pool));
  for (int i = 0; i < 5; ++i) {
    join_set->Spawn([count]() mutable {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      count->fetch_add(1);
    });
  }
  join_set->Wait();

  // Ensure all tasks have finished
  EXPECT_EQ(count->load(), 5);

  join_set.reset();
  pool.reset();
}

TEST_F(ThreadPoolTest, JoinSetDestructorWaitsForCompletion) {
  std::unique_ptr<ThreadPool> pool(new ThreadPool(2, *logger_));
  auto count = std::make_shared<std::atomic<int>>();

  std::unique_ptr<ThreadPool::JoinSet> join_set(new ThreadPool::JoinSet(*pool));
  for (int i = 0; i < 5; ++i) {
    join_set->Spawn([count]() mutable {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      count->fetch_add(1);
    });
  }
  join_set.reset();

  // Ensure all tasks have finished
  EXPECT_EQ(count->load(), 5);

  pool.reset();
}

TEST_F(ThreadPoolTest, JoinSetExceptionHandling) {
  std::unique_ptr<ThreadPool> pool(new ThreadPool(2, *logger_));
  auto count = std::make_shared<std::atomic<int>>();

  std::unique_ptr<ThreadPool::JoinSet> join_set(new ThreadPool::JoinSet(*pool));
  join_set->Spawn([]() mutable { throw std::runtime_error("expected"); });

  for (int i = 0; i < 5; ++i) {
    join_set->Spawn([count]() mutable {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      count->fetch_add(1);
    });
  }

  EXPECT_THROW(join_set->Wait(), std::runtime_error);

  EXPECT_LT(count->load(), 5);

  join_set.reset();

  // Ensure all tasks have finished
  ASSERT_EQ(count->load(), 5);

  pool.reset();
}

TEST_F(ThreadPoolTest, JoinSetMultipleWait) {
  std::unique_ptr<ThreadPool> pool(new ThreadPool(3, *logger_));

  auto bitset = std::make_shared<std::atomic<int>>();

  std::unique_ptr<ThreadPool::JoinSet> join_set_1(new ThreadPool::JoinSet(*pool));
  std::unique_ptr<ThreadPool::JoinSet> join_set_2(new ThreadPool::JoinSet(*pool));
  std::unique_ptr<ThreadPool::JoinSet> join_set_3(new ThreadPool::JoinSet(*pool));

  join_set_1->Spawn([bitset]() mutable {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    *bitset |= 1;
  });
  join_set_2->Spawn([bitset]() mutable { *bitset |= 2; });
  join_set_3->Spawn([bitset]() mutable {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    *bitset |= 4;
  });

  join_set_2.reset();
  EXPECT_EQ(bitset->load(), 2);

  join_set_3.reset();
  EXPECT_EQ(bitset->load(), 6);

  join_set_1.reset();
  EXPECT_EQ(bitset->load(), 7);

  pool.reset();
}

TEST_F(ThreadPoolTest, ThreadPoolDestructorWaitsForCompletion) {
  std::unique_ptr<ThreadPool> pool(new ThreadPool(2, *logger_));
  auto count = std::make_shared<std::atomic<int>>();

  for (int i = 0; i < 8; ++i) {
    pool->Spawn([count]() mutable {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      count->fetch_add(1);
    });
  }

  pool.reset();

  // Ensure all tasks have finished
  ASSERT_EQ(count->load(), 8);
}

TEST_F(ThreadPoolTest, ConcurrentTaskExecution) {
  std::unique_ptr<ThreadPool> pool(new ThreadPool(4, *logger_));
  std::atomic<int> concurrent_count(0);
  std::atomic<int> max_concurrent(0);
  std::vector<std::future<void>> futures;

  for (int i = 0; i < 8; ++i) {
    std::promise<void> promise;
    futures.push_back(promise.get_future());
    pool->Spawn([&, promise = std::move(promise)]() mutable {
      ++concurrent_count;
      {
        int current = concurrent_count.load();
        int expected = max_concurrent.load();
        while (current > expected && !max_concurrent.compare_exchange_weak(expected, current)) {
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      --concurrent_count;
      promise.set_value();
    });
  }

  for (size_t i = 0; i < futures.size(); ++i) {
    ASSERT_EQ(futures[i].wait_for(TIMEOUT), std::future_status::ready)
        << "Future " << i << " did not complete within timeout";
  }
  ASSERT_GT(max_concurrent, 1);
  pool.reset();
}

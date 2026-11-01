/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 */
#include "utils/ResourcePool.h"

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <thread>

using namespace pzstd;

TEST(ResourcePool, FullTest) {
  unsigned numCreated = 0;
  unsigned numDeleted = 0;
  {
    ResourcePool<int> pool(
      [&numCreated] { ++numCreated; return new int{5}; },
      [&numDeleted](int *x) { ++numDeleted; delete x; });

    {
      auto i = pool.get();
      EXPECT_EQ(5, *i);
      *i = 6;
    }
    {
      auto i = pool.get();
      EXPECT_EQ(6, *i);
      auto j = pool.get();
      EXPECT_EQ(5, *j);
      *j = 7;
    }
    {
      auto i = pool.get();
      EXPECT_EQ(6, *i);
      auto j = pool.get();
      EXPECT_EQ(7, *j);
    }
  }
  EXPECT_EQ(2, numCreated);
  EXPECT_EQ(numCreated, numDeleted);
}

TEST(ResourcePool, ThreadSafe) {
  std::atomic<unsigned> numCreated{0};
  std::atomic<unsigned> numDeleted{0};
  {
    ResourcePool<int> pool(
      [&numCreated] { ++numCreated; return new int{0}; },
      [&numDeleted](int *x) { ++numDeleted; delete x; });
    auto push = [&pool] {
      for (int i = 0; i < 100; ++i) {
        auto x = pool.get();
        ++*x;
      }
    };
    std::thread t1{push};
    std::thread t2{push};
    t1.join();
    t2.join();

    auto x = pool.get();
    auto y = pool.get();
    EXPECT_EQ(200, *x + *y);
  }
  EXPECT_GE(2, numCreated);
  EXPECT_EQ(numCreated, numDeleted);
}

TEST(ResourcePool, FactoriesRunConcurrently) {
  std::mutex mutex;
  std::condition_variable cv;
  unsigned entered = 0;
  std::atomic<unsigned> rendezvous{0};
  ResourcePool<int> pool([&] {
    std::unique_lock<std::mutex> lock(mutex);
    ++entered;
    cv.notify_all();
    if (cv.wait_for(lock, std::chrono::seconds(2), [&] { return entered == 2; })) {
      ++rendezvous;
    }
    return new int{42};
  }, [](int* value) { delete value; });
  auto acquire = [&] {
    auto resource = pool.get();
    EXPECT_EQ(42, *resource);
  };
  std::thread first(acquire);
  std::thread second(acquire);
  first.join();
  second.join();
  EXPECT_EQ(2U, rendezvous.load());
}

TEST(ResourcePool, NullFactoryDoesNotBorrowAResource) {
  ResourcePool<int> pool([]() -> int* { return nullptr; },
                         [](int* value) { delete value; });
  auto resource = pool.get();
  EXPECT_EQ(nullptr, resource.get());
}

/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 */
#pragma once

#include <cassert>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace pzstd {

/**
 * An unbounded pool of resources.
 * A `ResourcePool<T>` requires a factory function that takes allocates `T*` and
 * a free function that frees a `T*`. The factory must support concurrent calls;
 * the owner must not destroy the pool while get() or a borrowed resource is active.
 * Calling `ResourcePool::get()` will give you a new `ResourcePool::UniquePtr`
 * to a `T`, and when it goes out of scope the resource will be returned to the
 * pool.
 * The `ResourcePool<T>` *must* survive longer than any resources it hands out.
 * Remember that `ResourcePool<T>` hands out mutable `T`s, so make sure to clean
 * up the resource before or after every use.
 */
template <typename T>
class ResourcePool {
 public:
  class Deleter;
  using Factory = std::function<T*()>;
  using Free = std::function<void(T*)>;
  using UniquePtr = std::unique_ptr<T, Deleter>;

 private:
  std::mutex mutex_;
  Factory factory_;
  Free free_;
  std::vector<T*> resources_;
  unsigned inUse_;

 public:
  /**
   * Creates a `ResourcePool`.
   *
   * @param factory  The function to use to create new resources.
   * @param free     The function to use to free resources created by `factory`.
   */
  ResourcePool(Factory factory, Free free)
      : factory_(std::move(factory)), free_(std::move(free)), inUse_(0) {}

  /**
   * @returns  A unique pointer to a resource.  The resource is null iff
   *           there are no available resources and `factory()` returns null.
   */
  UniquePtr get() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!resources_.empty()) {
      UniquePtr resource{resources_.back(), Deleter{*this}};
      resources_.pop_back();
      ++inUse_;
      return resource;
    }
    // Context creation can allocate/initialize megabytes. Do it concurrently
    // rather than serializing every worker behind the pool bookkeeping lock.
    lock.unlock();
    UniquePtr resource{factory_(), Deleter{*this}};
    lock.lock();
    if (resource) ++inUse_;
    return resource;
  }

  ~ResourcePool() noexcept {
    assert(inUse_ == 0);
    for (const auto resource : resources_) {
      free_(resource);
    }
  }

  class Deleter {
    ResourcePool *pool_;
  public:
    explicit Deleter(ResourcePool &pool) : pool_(&pool) {}

    void operator() (T *resource) {
      std::lock_guard<std::mutex> lock(pool_->mutex_);
      // Make sure we don't put null resources into the pool
      if (resource) {
        pool_->resources_.push_back(resource);
      }
      assert(pool_->inUse_ > 0);
      --pool_->inUse_;
    }
  };
};

}

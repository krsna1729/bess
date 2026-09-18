// Copyright (c) 2017, Joshua Stone.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice, this
// list of conditions and the following disclaimer.
//
// * Redistributions in binary form must reproduce the above copyright notice,
// this list of conditions and the following disclaimer in the documentation
// and/or other materials provided with the distribution.
//
// * Neither the names of the copyright holders nor the names of their
// contributors may be used to endorse or promote products derived from this
// software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#ifndef BESS_UTILS_LOCK_LESS_QUEUE_H_
#define BESS_UTILS_LOCK_LESS_QUEUE_H_

#include <glog/logging.h>

#include <rte_ring.h>

#include "queue.h"
#include "rte_ring_alloc.h"

namespace bess {
namespace utils {

// A wrapper class for rte_ring that extends the abstract class Queue. Takes a
// template argument T which is the type to be enqueued and dequeued.
template <typename T>
class LockLessQueue final : public Queue<T> {
  static_assert(std::is_pointer<T>::value, "LockLessQueue only supports pointer types");
  public:
   static const size_t kDefaultRingSize = 256;

  // Construct a new queue. Takes the size of backing ring buffer (must power of
  // two and entries available will be one less than specified. default is 256),
  // boolean where if true, queue is in single producer mode or if false, queue
  // is in multi producer mode, boolean where if true, queue is in single
  // consumer mode, or if false, queue is in multi consumer mode. default for
  // both booleans is true.
  LockLessQueue(size_t capacity = kDefaultRingSize, bool single_producer = true,
                bool single_consumer = true)
      : capacity_(capacity) {
    CHECK((capacity & (capacity - 1)) == 0);
    ring_ = NewRing(capacity_,
                    (single_producer ? RING_F_SP_ENQ : 0) |
                        (single_consumer ? RING_F_SC_DEQ : 0));
    CHECK(ring_);
  }

  virtual ~LockLessQueue() {
    if (ring_) {
      std::free(ring_);
    }
  }

  // error codes: -1 is Quota exceeded. The objects have been enqueued,
  // but the high water mark is exceeded. -2 is not enough room in the
  // ring to enqueue; no object is enqueued.
  int Push(T obj) override {
    return rte_ring_enqueue(ring_, reinterpret_cast<void*>(obj));
  }

  int Push(T* objs, size_t count) override {
    // rte_ring_*_bulk return the count moved (0 or n, all-or-nothing),
    // unlike llring's 0-on-success convention.
    if (rte_ring_enqueue_bulk(ring_, reinterpret_cast<void**>(objs), count,
                              nullptr) == count) {
      return count;
    }
    return 0;
  }

  int Pop(T &obj) override {
    return rte_ring_dequeue(ring_, reinterpret_cast<void**>(&obj));
  }

  int Pop(T* objs, size_t count) override {
    if (rte_ring_dequeue_bulk(ring_, reinterpret_cast<void**>(objs), count,
                              nullptr) == count) {
      return count;
    }
    return 0;
  }

  // capacity will be one less than specified
  size_t Capacity() override { return capacity_; }

  size_t Size() override { return rte_ring_count(ring_); }

  bool Empty() override { return rte_ring_empty(ring_); }

  bool Full() override { return rte_ring_full(ring_); }

  int Resize(size_t new_capacity) override {
    if (new_capacity <= Size() || (new_capacity & (new_capacity - 1))) {
      return -1;
    }

    rte_ring* new_ring = NewRing(new_capacity, ring_->flags);
    if (!new_ring) {
      return -ENOMEM;
    }

    void* obj;
    while (rte_ring_dequeue(ring_, &obj) == 0) {
      rte_ring_enqueue(new_ring, obj);
    }

    free(ring_);
    ring_ = new_ring;
    capacity_ = new_capacity;
    return 0;
  }

   private:
   static rte_ring* NewRing(size_t capacity, unsigned flags) {
     ssize_t bytes = rte_ring_get_memsize(capacity);
     if (bytes < 0) {
       return nullptr;
     }
     void* mem = bess::utils::AllocRingMem(static_cast<size_t>(bytes));
     if (!mem) {
       return nullptr;
     }
     rte_ring* ring = static_cast<rte_ring*>(mem);
     std::string name = bess::utils::NewRingName("locklessq");
     if (rte_ring_init(ring, name.c_str(), capacity, flags) != 0) {
       std::free(mem);
       return nullptr;
     }
     return ring;
   }

   struct rte_ring* ring_;  // class's ring buffer
   size_t capacity_;      // the size of the backing ring buffer
};

}  // namespace utils
}  // namespace bess

#endif  // BESS_UTILS_LOCK_LESS_QUEUE_H_

// Copyright 2020 The TensorFlow Runtime Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// MLIR Async Runtime implemented on top of TFRT HostContext and host
// concurrency primitives.

#include "tfrt/jitrt/async_runtime.h"

#include <atomic>
#include <cstddef>
#include <memory>
#include <type_traits>
#include <utility>

#include "llvm/Support/MathExtras.h"
#include "tfrt/host_context/async_value.h"
#include "tfrt/host_context/async_value_ref.h"
#include "tfrt/host_context/chain.h"
#include "tfrt/host_context/concurrent_work_queue.h"
#include "tfrt/host_context/diagnostic.h"
#include "tfrt/host_context/host_allocator.h"
#include "tfrt/host_context/host_buffer.h"
#include "tfrt/support/concurrent_vector.h"
#include "tfrt/support/latch.h"
#include "tfrt/support/msan.h"
#include "tfrt/support/ref_count.h"

namespace tfrt {
namespace jitrt {
// Use malloc to allocate aligned storage for the async values.
HostAllocator* GetAsyncValueStorageAllocator() {
  static HostAllocator* allocator = CreateMallocAllocator().release();
  return allocator;
}
}  // namespace jitrt
}  // namespace tfrt

// -------------------------------------------------------------------------- //
// Define AsyncToken and AsyncGroup in the mlir::runtime namespace to implement
// opaque structs defined in the MLIR Async Runtime API header file.
// -------------------------------------------------------------------------- //

namespace mlir {
namespace runtime {

using tfrt::AsyncValueRef;
using tfrt::Chain;
using tfrt::GetReadyChain;
using tfrt::HostAllocator;
using tfrt::HostBuffer;
using tfrt::HostContext;
using tfrt::MakeConstructedAsyncValueRef;

using tfrt::jitrt::AsyncRuntimeObject;
using tfrt::jitrt::GetAsyncValueStorageAllocator;

class AsyncToken : public AsyncRuntimeObject {
 public:
  explicit AsyncToken(unsigned ref_count = 1)
      : AsyncRuntimeObject(ref_count),
        chain_(MakeConstructedAsyncValueRef<Chain>()) {}

  tfrt::AsyncValue* GetAsyncValue() const { return chain_.GetAsyncValue(); }

 private:
  AsyncValueRef<Chain> chain_;
};

class AsyncValue : public AsyncRuntimeObject {
 public:
  explicit AsyncValue(size_t size, size_t alignment, unsigned ref_count = 1)
      : AsyncRuntimeObject(ref_count),
        storage_(Storage::CanStoreInline(size, alignment)
                     ? MakeConstructedAsyncValueRef<Storage>()
                     : MakeConstructedAsyncValueRef<Storage>(
                           GetAsyncValueStorageAllocator(), size, alignment)) {
    // Storage memory will be initialized by the compiled kernel.
    TFRT_MSAN_MEMORY_IS_INITIALIZED(GetStorage(), size);
  }

  void* GetStorage() const {
    assert(!GetAsyncValue()->IsError() && "unexpected error state");
    if (storage_->is_inline) return &storage_->inline_buffer;
    return storage_->host_buffer->data();
  }

  tfrt::AsyncValue* GetAsyncValue() const { return storage_.GetAsyncValue(); }

 private:
  // If the requested async value storage is small, use the inlined storage,
  // fallback on the HostBuffer if the requested storage size is large.
  struct Storage {
    static const int kSize = 128;  // enough to fit memref descriptor of rank 5
    static const int kAlign = alignof(std::max_align_t);

    Storage() : is_inline(true) {}
    Storage(HostAllocator* allocator, size_t size, size_t alignment)
        : is_inline(false),
          host_buffer(
              HostBuffer::CreateUninitialized(size, alignment, allocator)
                  .release()) {}

    ~Storage() {
      if (!is_inline) host_buffer->DropRef();
    }

    static bool CanStoreInline(size_t size, size_t alignment) {
      assert(llvm::isPowerOf2_32(alignment));
      return size <= kSize && alignment <= kAlign;
    }

    bool is_inline;
    union {
      std::aligned_storage<kSize, kAlign>::type inline_buffer;
      HostBuffer* host_buffer;
    };
  };

  AsyncValueRef<Storage> storage_;
};

class AsyncGroup : public AsyncRuntimeObject {
 public:
  explicit AsyncGroup(int64_t size, unsigned ref_count = 1)
      : AsyncRuntimeObject(ref_count),
        size_(size),
        rank_(0),
        pending_tokens_(size),
        num_errors_(0),
        completed_(size == 0 ? GetReadyChain()
                             : MakeConstructedAsyncValueRef<Chain>()) {
    assert(size_ >= 0 && "size can't be negative");
  }

  size_t AddToken(AsyncToken* token) {
    size_t rank = rank_.fetch_add(1, std::memory_order_relaxed);
    assert(rank < size_ && "can't add more tokens than the group size");

    // When token becomes available drop the number of pending tokens and maybe
    // make the group completion async value available.
    token->GetAsyncValue()->AndThen([group = this, token]() {
      // Increment the number of errors in the group.
      if (token->GetAsyncValue()->IsError()) group->num_errors_.fetch_add(1);

      // Pending tokens can't drop below zero.
      assert(group->pending_tokens_ > 0 && "wrong group size");

      // We do track group error state with the number of errors, and never
      // set completion async value state to error.
      if (group->pending_tokens_.fetch_sub(1) == 1)
        group->completed_.SetStateConcrete();
    });

    return rank;
  }

  tfrt::AsyncValue* GetCompletionAsyncValue() const {
    return completed_.GetAsyncValue();
  }

  bool IsError() const { return num_errors_.load() != 0; }

 private:
  int64_t size_;
  std::atomic<int64_t> rank_;
  std::atomic<int64_t> pending_tokens_;
  std::atomic<int64_t> num_errors_;

  // Async value that keeps track the group completion, it will become available
  // when the number of pending tokens will drop to zero.
  AsyncValueRef<Chain> completed_;
};

}  // namespace runtime
}  // namespace mlir

// -------------------------------------------------------------------------- //

namespace tfrt {
namespace jitrt {

/*static*/ void* AsyncRuntime::GetStorage(Value* value) {
  return value->GetStorage();
}

/*static*/ AsyncValue* AsyncRuntime::GetAsyncValue(AsyncRuntime::Value* value) {
  return value->GetAsyncValue();
}

/*static*/ AsyncValue* AsyncRuntime::GetAsyncValue(AsyncRuntime::Token* token) {
  return token->GetAsyncValue();
}

/*static*/ AsyncValue* AsyncRuntime::GetAsyncValue(AsyncRuntime::Group* group) {
  return group->GetCompletionAsyncValue();
}

void AsyncRuntime::Await(AsyncValue* awaitable) {
  // Short circuit the trivial case.
  if (awaitable->IsAvailable()) return;
  tfrt::Await({awaitable});
}

/*static*/ void AsyncRuntime::AddRef(AsyncRuntimeObject* obj, unsigned count) {
  assert(count == 1 && "AsyncRuntimeObject can add just one ref");
  obj->AddRef();
}

/*static*/ void AsyncRuntime::DropRef(AsyncRuntimeObject* obj, unsigned count) {
  assert(count == 1 && "AsyncRuntimeObject can drop just one ref");
  obj->DropRef();
}

/*static*/ AsyncRuntimeObject* AsyncRuntime::ToAsyncRuntimeObject(
    AsyncRuntime::Token* token) {
  return static_cast<AsyncRuntimeObject*>(token);
}

/*static*/ AsyncRuntimeObject* AsyncRuntime::ToAsyncRuntimeObject(
    AsyncRuntime::Value* value) {
  return static_cast<AsyncRuntimeObject*>(value);
}

/*static*/ AsyncRuntimeObject* AsyncRuntime::ToAsyncRuntimeObject(
    AsyncRuntime::Group* group) {
  return static_cast<AsyncRuntimeObject*>(group);
}

AsyncRuntime::Token* AsyncRuntime::CreateToken() {
  // AsyncRuntime::Token created with a reference count of 2 because it will be
  // returned to the `async.execute` caller and also will be later on emplaced
  // by the asynchronously executed task. If the caller immediately will drop
  // its reference we must ensure that the token will be alive until the
  // asynchronous operation is completed.
  return new AsyncRuntime::Token(/*ref_count=*/2);
}

void AsyncRuntime::SetAvailable(AsyncRuntime::Token* token) {
  token->GetAsyncValue()->SetStateConcrete();
  // Async tokens created with a ref count `2` to keep token alive until the
  // async task completes. Drop extra reference explicitly when token emplaced.
  DropRef(token);
}

void AsyncRuntime::SetError(AsyncRuntime::Token* token) {
  // TODO(ezhulenev): Construct a better diagnostincs when async runtime API
  // will support passing custom error messages.
  token->GetAsyncValue()->SetError(DecodedDiagnostic("<async runtime error>"));
  // Async tokens created with a ref count `2` to keep token alive until the
  // async task completes. Drop extra reference explicitly when token emplaced.
  DropRef(token);
}

bool AsyncRuntime::IsError(AsyncRuntime::Token* token) {
  return token->GetAsyncValue()->IsError();
}

void AsyncRuntime::AwaitToken(AsyncRuntime::Token* token) {
  Await(token->GetAsyncValue());
}

AsyncRuntime::Value* AsyncRuntime::CreateValue(size_t size, size_t alignment) {
  // AsyncRuntime::Value created with a reference count of 2 because it will be
  // returned to the `async.execute` caller and also will be later on emplaced
  // by the asynchronously executed task. If the caller immediately will drop
  // its reference we must ensure that the token will be alive until the
  // asynchronous operation is completed.
  return new AsyncRuntime::Value(size, alignment, /*ref_count=*/2);
}

void AsyncRuntime::SetAvailable(AsyncRuntime::Value* value) {
  value->GetAsyncValue()->SetStateConcrete();
  // Async values created with a ref count `2` to keep token alive until the
  // async task completes. Drop extra reference explicitly when token emplaced.
  DropRef(value);
}

void AsyncRuntime::SetError(AsyncRuntime::Value* value) {
  // TODO(ezhulenev): Construct a better diagnostincs when async runtime API
  // will support passing custom error messages.
  value->GetAsyncValue()->SetError(DecodedDiagnostic("<async runtime error>"));
  // Async values created with a ref count `2` to keep token alive until the
  // async task completes. Drop extra reference explicitly when token emplaced.
  DropRef(value);
}

bool AsyncRuntime::IsError(AsyncRuntime::Value* value) {
  return value->GetAsyncValue()->IsError();
}

void AsyncRuntime::AwaitValue(AsyncRuntime::Value* value) {
  Await(value->GetAsyncValue());
}

AsyncRuntime::Group* AsyncRuntime::CreateGroup(int64_t size) {
  return new AsyncRuntime::Group(size);
}

size_t AsyncRuntime::AddTokenToGroup(AsyncRuntime::Group* group,
                                     AsyncRuntime::Token* token) {
  return group->AddToken(token);
}

bool AsyncRuntime::IsError(AsyncRuntime::Group* group) {
  return group->IsError();
}

void AsyncRuntime::AwaitGroup(AsyncRuntime::Group* group) {
  Await(group->GetCompletionAsyncValue());
}

HostContextAsyncTaskRunner::HostContextAsyncTaskRunner(HostContext* host)
    : host_(host) {}

void HostContextAsyncTaskRunner::Schedule(Task task) {
  EnqueueWork(host_, std::move(task));
}

EigenThreadPoolAsyncTaskRunner::EigenThreadPoolAsyncTaskRunner(
    Eigen::ThreadPoolInterface* thread_pool)
    : thread_pool_(thread_pool) {}

void EigenThreadPoolAsyncTaskRunner::Schedule(Task task) {
  thread_pool_->Schedule(std::move(task));
}

}  // namespace jitrt
}  // namespace tfrt

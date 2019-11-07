/*
 * Copyright 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef ART_RUNTIME_JIT_JIT_MEMORY_REGION_H_
#define ART_RUNTIME_JIT_JIT_MEMORY_REGION_H_

#include <string>

#include "base/globals.h"
#include "base/locks.h"
#include "base/mem_map.h"

namespace art {
namespace jit {

// Alignment in bytes that will suit all architectures for JIT code cache allocations.  The
// allocated block is used for method header followed by generated code. Allocations should be
// aligned to avoid sharing cache lines between different allocations. The alignment should be
// determined from the hardware, but this isn't readily exposed in userland plus some hardware
// misreports.
static constexpr int kJitCodeAlignment = 64;

// Represents a memory region for the JIT, where code and data are stored. This class
// provides allocation and deallocation primitives.
class JitMemoryRegion {
 public:
  JitMemoryRegion()
      : used_memory_for_code_(0),
        used_memory_for_data_(0) {}

  void InitializeState(size_t initial_capacity, size_t max_capacity)
      REQUIRES(Locks::jit_lock_);

  bool InitializeMappings(bool rwx_memory_allowed, bool is_zygote, std::string* error_msg)
      REQUIRES(Locks::jit_lock_);

  void InitializeSpaces() REQUIRES(Locks::jit_lock_);

  // Try to increase the current capacity of the code cache. Return whether we
  // succeeded at doing so.
  bool IncreaseCodeCacheCapacity() REQUIRES(Locks::jit_lock_);

  // Set the footprint limit of the code cache.
  void SetFootprintLimit(size_t new_footprint) REQUIRES(Locks::jit_lock_);
  uint8_t* AllocateCode(size_t code_size) REQUIRES(Locks::jit_lock_);
  void FreeCode(uint8_t* code) REQUIRES(Locks::jit_lock_);
  uint8_t* AllocateData(size_t data_size) REQUIRES(Locks::jit_lock_);
  void FreeData(uint8_t* data) REQUIRES(Locks::jit_lock_);

  bool HasDualCodeMapping() const {
    return non_exec_pages_.IsValid();
  }

  bool HasCodeMapping() const {
    return exec_pages_.IsValid();
  }

  bool IsInDataSpace(const void* ptr) const {
    return data_pages_.HasAddress(ptr);
  }

  bool IsInExecSpace(const void* ptr) const {
    return exec_pages_.HasAddress(ptr);
  }

  const MemMap* GetUpdatableCodeMapping() const {
    if (HasDualCodeMapping()) {
      return &non_exec_pages_;
    } else if (HasCodeMapping()) {
      return &exec_pages_;
    } else {
      return nullptr;
    }
  }

  const MemMap* GetExecPages() const {
    return &exec_pages_;
  }

  template <typename T> T* GetExecutableAddress(T* src_ptr) {
    return TranslateAddress(src_ptr, non_exec_pages_, exec_pages_);
  }

  template <typename T> T* GetNonExecutableAddress(T* src_ptr) {
    return TranslateAddress(src_ptr, exec_pages_, non_exec_pages_);
  }

  void* MoreCore(const void* mspace, intptr_t increment);

  bool OwnsSpace(const void* mspace) const NO_THREAD_SAFETY_ANALYSIS {
    return mspace == data_mspace_ || mspace == exec_mspace_;
  }

  size_t GetCurrentCapacity() const REQUIRES(Locks::jit_lock_) {
    return current_capacity_;
  }

  size_t GetMaxCapacity() const REQUIRES(Locks::jit_lock_) {
    return max_capacity_;
  }

  size_t GetUsedMemoryForCode() const REQUIRES(Locks::jit_lock_) {
    return used_memory_for_code_;
  }

  size_t GetUsedMemoryForData() const REQUIRES(Locks::jit_lock_) {
    return used_memory_for_data_;
  }

 private:
  template <typename T>
  T* TranslateAddress(T* src_ptr, const MemMap& src, const MemMap& dst) {
    if (!HasDualCodeMapping()) {
      return src_ptr;
    }
    CHECK(src.HasAddress(src_ptr));
    uint8_t* const raw_src_ptr = reinterpret_cast<uint8_t*>(src_ptr);
    return reinterpret_cast<T*>(raw_src_ptr - src.Begin() + dst.Begin());
  }

  // The initial capacity in bytes this code region starts with.
  size_t initial_capacity_ GUARDED_BY(Locks::jit_lock_);

  // The maximum capacity in bytes this region can go to.
  size_t max_capacity_ GUARDED_BY(Locks::jit_lock_);

  // The current capacity in bytes of the region.
  size_t current_capacity_ GUARDED_BY(Locks::jit_lock_);

  // The current footprint in bytes of the data portion of the region.
  size_t data_end_ GUARDED_BY(Locks::jit_lock_);

  // The current footprint in bytes of the code portion of the region.
  size_t exec_end_ GUARDED_BY(Locks::jit_lock_);

  // The size in bytes of used memory for the code portion of the region.
  size_t used_memory_for_code_ GUARDED_BY(Locks::jit_lock_);

  // The size in bytes of used memory for the data portion of the region.
  size_t used_memory_for_data_ GUARDED_BY(Locks::jit_lock_);

  // Mem map which holds data (stack maps and profiling info).
  MemMap data_pages_;

  // Mem map which holds code and has executable permission.
  MemMap exec_pages_;

  // Mem map which holds code with non executable permission. Only valid for dual view JIT when
  // this is the non-executable view of code used to write updates.
  MemMap non_exec_pages_;

  // The opaque mspace for allocating data.
  void* data_mspace_ GUARDED_BY(Locks::jit_lock_);

  // The opaque mspace for allocating code.
  void* exec_mspace_ GUARDED_BY(Locks::jit_lock_);
};

}  // namespace jit
}  // namespace art

#endif  // ART_RUNTIME_JIT_JIT_MEMORY_REGION_H_

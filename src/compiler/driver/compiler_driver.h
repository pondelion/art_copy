/*
 * Copyright (C) 2011 The Android Open Source Project
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

#ifndef ART_SRC_COMPILER_DRIVER_COMPILER_DRIVER_H_
#define ART_SRC_COMPILER_DRIVER_COMPILER_DRIVER_H_

#include <set>
#include <string>
#include <vector>

#include "base/mutex.h"
#include "compiled_class.h"
#include "compiled_method.h"
#include "dex_file.h"
#include "instruction_set.h"
#include "invoke_type.h"
#include "oat_file.h"
#include "runtime.h"
#include "safe_map.h"
#include "thread_pool.h"

namespace art {

class AOTCompilationStats;
class ParallelCompilationManager;
class DexCompilationUnit;
class TimingLogger;

enum CompilerBackend {
  kQuick,
  kQuickGBC,
  kPortable
};

// Thread-local storage compiler worker threads
class CompilerTls {
  public:
    CompilerTls() : llvm_info_(NULL) {}
    ~CompilerTls() {}

    void* GetLLVMInfo() { return llvm_info_; }

    void SetLLVMInfo(void* llvm_info) { llvm_info_ = llvm_info; }

  private:
    void* llvm_info_;
};

class CompilerDriver {
 public:
  // Create a compiler targeting the requested "instruction_set".
  // "image" should be true if image specific optimizations should be
  // enabled.  "image_classes" lets the compiler know what classes it
  // can assume will be in the image, with NULL implying all available
  // classes.
  explicit CompilerDriver(CompilerBackend compiler_backend, InstructionSet instruction_set, bool image,
                          size_t thread_count, bool support_debugging,
                          const std::set<std::string>* image_classes, bool dump_stats,
                          bool dump_timings);

  ~CompilerDriver();

  void CompileAll(jobject class_loader, const std::vector<const DexFile*>& dex_files)
      LOCKS_EXCLUDED(Locks::mutator_lock_);

  // Compile a single Method
  void CompileOne(const mirror::AbstractMethod* method)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  bool IsDebuggingSupported() {
    return support_debugging_;
  }

  InstructionSet GetInstructionSet() const {
    return instruction_set_;
  }

  CompilerBackend GetCompilerBackend() const {
    return compiler_backend_;
  }

  bool IsImage() const {
    return image_;
  }

  CompilerTls* GetTls();

  // Stub to throw AbstractMethodError
  static mirror::ByteArray* CreateAbstractMethodErrorStub(InstructionSet instruction_set)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);


  // Generate the trampoline that's invoked by unresolved direct methods
  static mirror::ByteArray* CreateResolutionStub(InstructionSet instruction_set,
                                                 Runtime::TrampolineType type)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  static mirror::ByteArray* CreateJniDlsymLookupStub(InstructionSet instruction_set)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // A class is uniquely located by its DexFile and the class_defs_ table index into that DexFile
  typedef std::pair<const DexFile*, uint32_t> ClassReference;

  CompiledClass* GetCompiledClass(ClassReference ref) const
      LOCKS_EXCLUDED(compiled_classes_lock_);

  // A method is uniquely located by its DexFile and the method_ids_ table index into that DexFile
  typedef std::pair<const DexFile*, uint32_t> MethodReference;

  CompiledMethod* GetCompiledMethod(MethodReference ref) const
      LOCKS_EXCLUDED(compiled_methods_lock_);

  CompiledInvokeStub* FindInvokeStub(bool is_static, const char* shorty) const;
  CompiledInvokeStub* FindInvokeStub(const std::string& key) const
      LOCKS_EXCLUDED(compiled_invoke_stubs_lock_);

  CompiledInvokeStub* FindProxyStub(const char* shorty) const;

  void AddRequiresConstructorBarrier(Thread* self, const DexFile* dex_file, size_t class_def_index);
  bool RequiresConstructorBarrier(Thread* self, const DexFile* dex_file, size_t class_def_index);

  // Callbacks from compiler to see what runtime checks must be generated.

  bool CanAssumeTypeIsPresentInDexCache(const DexFile& dex_file, uint32_t type_idx)
      LOCKS_EXCLUDED(Locks::mutator_lock_);

  bool CanAssumeStringIsPresentInDexCache(const DexFile& dex_file, uint32_t string_idx)
      LOCKS_EXCLUDED(Locks::mutator_lock_);

  // Are runtime access checks necessary in the compiled code?
  bool CanAccessTypeWithoutChecks(uint32_t referrer_idx, const DexFile& dex_file,
                                  uint32_t type_idx)
      LOCKS_EXCLUDED(Locks::mutator_lock_);

  // Are runtime access and instantiable checks necessary in the code?
  bool CanAccessInstantiableTypeWithoutChecks(uint32_t referrer_idx, const DexFile& dex_file,
                                              uint32_t type_idx)
     LOCKS_EXCLUDED(Locks::mutator_lock_);

  // Can we fast path instance field access? Computes field's offset and volatility.
  bool ComputeInstanceFieldInfo(uint32_t field_idx, const DexCompilationUnit* mUnit,
                                int& field_offset, bool& is_volatile, bool is_put)
      LOCKS_EXCLUDED(Locks::mutator_lock_);

  // Can we fastpath static field access? Computes field's offset, volatility and whether the
  // field is within the referrer (which can avoid checking class initialization).
  bool ComputeStaticFieldInfo(uint32_t field_idx, const DexCompilationUnit* mUnit,
                              int& field_offset, int& ssb_index,
                              bool& is_referrers_class, bool& is_volatile, bool is_put)
      LOCKS_EXCLUDED(Locks::mutator_lock_);

  // Can we fastpath a interface, super class or virtual method call? Computes method's vtable
  // index.
  bool ComputeInvokeInfo(uint32_t method_idx, const DexCompilationUnit* mUnit, InvokeType& type,
                         int& vtable_idx, uintptr_t& direct_code, uintptr_t& direct_method)
      LOCKS_EXCLUDED(Locks::mutator_lock_);

  // Record patch information for later fix up.
  void AddCodePatch(const DexFile* dex_file,
                    uint32_t referrer_method_idx,
                    InvokeType referrer_invoke_type,
                    uint32_t target_method_idx,
                    InvokeType target_invoke_type,
                    size_t literal_offset)
      LOCKS_EXCLUDED(compiled_methods_lock_);
  void AddMethodPatch(const DexFile* dex_file,
                      uint32_t referrer_method_idx,
                      InvokeType referrer_invoke_type,
                      uint32_t target_method_idx,
                      InvokeType target_invoke_type,
                      size_t literal_offset)
      LOCKS_EXCLUDED(compiled_methods_lock_);

  void SetBitcodeFileName(std::string const& filename);

  // TODO: remove these Elf wrappers when libart links against LLVM (when separate compiler library is gone)
  bool WriteElf(const std::string* host_prefix,
                bool is_host,
                const std::vector<const DexFile*>& dex_files,
                std::vector<uint8_t>& oat_contents,
                File* file);
  bool FixupElf(File* file, uintptr_t oat_data_begin) const;
  void GetOatElfInformation(File* file, size_t& oat_loaded_size, size_t& oat_data_offset) const;
  bool StripElf(File* file) const;

  // TODO: move to a common home for llvm helpers once quick/portable are merged
  static void InstructionSetToLLVMTarget(InstructionSet instruction_set,
                                         std::string& target_triple,
                                         std::string& target_cpu,
                                         std::string& target_attr);

  void SetCompilerContext(void* compiler_context) {
    compiler_context_ = compiler_context;
  }

  void* GetCompilerContext() const {
    return compiler_context_;
  }

  size_t GetThreadCount() const {
    return thread_count_;
  }

  class PatchInformation {
   public:
    const DexFile& GetDexFile() const {
      return *dex_file_;
    }
    uint32_t GetReferrerMethodIdx() const {
      return referrer_method_idx_;
    }
    InvokeType GetReferrerInvokeType() const {
      return referrer_invoke_type_;
    }
    uint32_t GetTargetMethodIdx() const {
      return target_method_idx_;
    }
    InvokeType GetTargetInvokeType() const {
      return target_invoke_type_;
    }
    size_t GetLiteralOffset() const {;
      return literal_offset_;
    }

   private:
    PatchInformation(const DexFile* dex_file,
                     uint32_t referrer_method_idx,
                     InvokeType referrer_invoke_type,
                     uint32_t target_method_idx,
                     InvokeType target_invoke_type,
                     size_t literal_offset)
      : dex_file_(dex_file),
        referrer_method_idx_(referrer_method_idx),
        referrer_invoke_type_(referrer_invoke_type),
        target_method_idx_(target_method_idx),
        target_invoke_type_(target_invoke_type),
        literal_offset_(literal_offset) {
      CHECK(dex_file_ != NULL);
    }

    const DexFile* dex_file_;
    uint32_t referrer_method_idx_;
    InvokeType referrer_invoke_type_;
    uint32_t target_method_idx_;
    InvokeType target_invoke_type_;
    size_t literal_offset_;

    friend class CompilerDriver;
    DISALLOW_COPY_AND_ASSIGN(PatchInformation);
  };

  const std::vector<const PatchInformation*>& GetCodeToPatch() const {
    return code_to_patch_;
  }
  const std::vector<const PatchInformation*>& GetMethodsToPatch() const {
    return methods_to_patch_;
  }

  // Checks if class specified by type_idx is one of the image_classes_
  bool IsImageClass(const std::string& descriptor) const;

  void RecordClassStatus(ClassReference ref, CompiledClass* compiled_class);

 private:
  // Compute constant code and method pointers when possible
  void GetCodeAndMethodForDirectCall(InvokeType type, InvokeType sharp_type,
                                     mirror::AbstractMethod* method,
                                     uintptr_t& direct_code, uintptr_t& direct_method)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  void PreCompile(jobject class_loader, const std::vector<const DexFile*>& dex_files,
                  ThreadPool& thread_pool, TimingLogger& timings)
      LOCKS_EXCLUDED(Locks::mutator_lock_);

  // Attempt to resolve all type, methods, fields, and strings
  // referenced from code in the dex file following PathClassLoader
  // ordering semantics.
  void Resolve(jobject class_loader, const std::vector<const DexFile*>& dex_files,
               ThreadPool& thread_pool, TimingLogger& timings)
      LOCKS_EXCLUDED(Locks::mutator_lock_);
  void ResolveDexFile(jobject class_loader, const DexFile& dex_file,
                      ThreadPool& thread_pool, TimingLogger& timings)
      LOCKS_EXCLUDED(Locks::mutator_lock_);

  void Verify(jobject class_loader, const std::vector<const DexFile*>& dex_files,
              ThreadPool& thread_pool, TimingLogger& timings);
  void VerifyDexFile(jobject class_loader, const DexFile& dex_file,
                     ThreadPool& thread_pool, TimingLogger& timings)
      LOCKS_EXCLUDED(Locks::mutator_lock_);

  void InitializeClasses(jobject class_loader, const std::vector<const DexFile*>& dex_files,
                         ThreadPool& thread_pool, TimingLogger& timings)
      LOCKS_EXCLUDED(Locks::mutator_lock_);
  void InitializeClasses(jobject class_loader, const DexFile& dex_file,
                         ThreadPool& thread_pool, TimingLogger& timings)
      LOCKS_EXCLUDED(Locks::mutator_lock_, compiled_classes_lock_);

  void Compile(jobject class_loader, const std::vector<const DexFile*>& dex_files,
               ThreadPool& thread_pool, TimingLogger& timings);
  void CompileDexFile(jobject class_loader, const DexFile& dex_file,
                      ThreadPool& thread_pool, TimingLogger& timings)
      LOCKS_EXCLUDED(Locks::mutator_lock_);
  void CompileMethod(const DexFile::CodeItem* code_item, uint32_t access_flags,
                     InvokeType invoke_type, uint32_t class_def_idx, uint32_t method_idx,
                     jobject class_loader, const DexFile& dex_file)
      LOCKS_EXCLUDED(compiled_methods_lock_);

  static void CompileClass(const ParallelCompilationManager* context, size_t class_def_index)
      LOCKS_EXCLUDED(Locks::mutator_lock_);

  void InsertInvokeStub(const std::string& key, CompiledInvokeStub* compiled_invoke_stub)
      LOCKS_EXCLUDED(compiled_invoke_stubs_lock_);

  void InsertProxyStub(const char* shorty, CompiledInvokeStub* compiled_proxy_stub);

  std::vector<const PatchInformation*> code_to_patch_;
  std::vector<const PatchInformation*> methods_to_patch_;

  CompilerBackend compiler_backend_;

  InstructionSet instruction_set_;

  // All class references that require
  mutable Mutex freezing_constructor_lock_ DEFAULT_MUTEX_ACQUIRED_AFTER;
  std::set<ClassReference> freezing_constructor_classes_ GUARDED_BY(freezing_constructor_lock_);

  typedef SafeMap<const ClassReference, CompiledClass*> ClassTable;
  // All class references that this compiler has compiled.
  mutable Mutex compiled_classes_lock_ DEFAULT_MUTEX_ACQUIRED_AFTER;
  ClassTable compiled_classes_ GUARDED_BY(compiled_classes_lock_);

  typedef SafeMap<const MethodReference, CompiledMethod*> MethodTable;
  // All method references that this compiler has compiled.
  mutable Mutex compiled_methods_lock_ DEFAULT_MUTEX_ACQUIRED_AFTER;
  MethodTable compiled_methods_ GUARDED_BY(compiled_methods_lock_);

  typedef SafeMap<std::string, CompiledInvokeStub*> InvokeStubTable;
  // Invocation stubs created to allow invocation of the compiled methods.
  mutable Mutex compiled_invoke_stubs_lock_ DEFAULT_MUTEX_ACQUIRED_AFTER;
  InvokeStubTable compiled_invoke_stubs_ GUARDED_BY(compiled_invoke_stubs_lock_);

  typedef SafeMap<std::string, CompiledInvokeStub*> ProxyStubTable;
  // Proxy stubs created for proxy invocation delegation
  mutable Mutex compiled_proxy_stubs_lock_ DEFAULT_MUTEX_ACQUIRED_AFTER;
  ProxyStubTable compiled_proxy_stubs_ GUARDED_BY(compiled_proxy_stubs_lock_);

  bool image_;
  size_t thread_count_;
  bool support_debugging_;
  uint64_t start_ns_;

  UniquePtr<AOTCompilationStats> stats_;

  bool dump_stats_;
  bool dump_timings_;

  const std::set<std::string>* image_classes_;

  typedef void (*CompilerCallbackFn)(CompilerDriver& driver);
  typedef MutexLock* (*CompilerMutexLockFn)(CompilerDriver& driver);

  void* compiler_library_;

  typedef CompiledMethod* (*CompilerFn)(CompilerDriver& driver,
                                        const DexFile::CodeItem* code_item,
                                        uint32_t access_flags, InvokeType invoke_type,
                                        uint32_t class_dex_idx, uint32_t method_idx,
                                        jobject class_loader, const DexFile& dex_file);
  CompilerFn compiler_;

  void* compiler_context_;

  typedef CompiledMethod* (*JniCompilerFn)(CompilerDriver& driver,
                                           uint32_t access_flags, uint32_t method_idx,
                                           const DexFile& dex_file);
  JniCompilerFn jni_compiler_;
  typedef CompiledInvokeStub* (*CreateInvokeStubFn)(CompilerDriver& driver, bool is_static,
                                                    const char* shorty, uint32_t shorty_len);
  CreateInvokeStubFn create_invoke_stub_;

  pthread_key_t tls_key_;

  typedef CompiledInvokeStub* (*CreateProxyStubFn)
      (CompilerDriver& driver, const char* shorty, uint32_t shorty_len);
  CreateProxyStubFn create_proxy_stub_;

  typedef void (*CompilerEnableAutoElfLoadingFn)(CompilerDriver& driver);
  CompilerEnableAutoElfLoadingFn compiler_enable_auto_elf_loading_;

  typedef const void* (*CompilerGetMethodCodeAddrFn)
      (const CompilerDriver& driver, const CompiledMethod* cm, const mirror::AbstractMethod* method);
  CompilerGetMethodCodeAddrFn compiler_get_method_code_addr_;

  typedef const mirror::AbstractMethod::InvokeStub* (*CompilerGetMethodInvokeStubAddrFn)
      (const CompilerDriver& driver, const CompiledInvokeStub* cm, const mirror::AbstractMethod* method);
  CompilerGetMethodInvokeStubAddrFn compiler_get_method_invoke_stub_addr_;


  DISALLOW_COPY_AND_ASSIGN(CompilerDriver);
};

inline bool operator<(const CompilerDriver::ClassReference& lhs, const CompilerDriver::ClassReference& rhs) {
  if (lhs.second < rhs.second) {
    return true;
  } else if (lhs.second > rhs.second) {
    return false;
  } else {
    return (lhs.first < rhs.first);
  }
}

}  // namespace art

#endif  // ART_SRC_COMPILER_DRIVER_COMPILER_DRIVER_H_

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

#ifndef ART_RUNTIME_MIRROR_ART_METHOD_H_
#define ART_RUNTIME_MIRROR_ART_METHOD_H_

#include "class.h"
#include "dex_file.h"
#include "invoke_type.h"
#include "modifiers.h"
#include "object.h"
#include "object_callbacks.h"

namespace art {

struct ArtMethodOffsets;
struct ConstructorMethodOffsets;
union JValue;
struct MethodClassOffsets;
class MethodHelper;
class ScopedObjectAccess;
class StringPiece;
class ShadowFrame;

namespace mirror {

class StaticStorageBase;

typedef void (EntryPointFromInterpreter)(Thread* self, MethodHelper& mh,
    const DexFile::CodeItem* code_item, ShadowFrame* shadow_frame, JValue* result);

// C++ mirror of java.lang.reflect.Method and java.lang.reflect.Constructor
class MANAGED ArtMethod : public Object {
 public:
  static ArtMethod* FromReflectedMethod(const ScopedObjectAccess& soa, jobject jlr_method)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  Class* GetDeclaringClass() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  void SetDeclaringClass(Class *new_declaring_class) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  static MemberOffset DeclaringClassOffset() {
    return MemberOffset(OFFSETOF_MEMBER(ArtMethod, declaring_class_));
  }

  uint32_t GetAccessFlags() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  void SetAccessFlags(uint32_t new_access_flags) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    // Not called within a transaction.
    SetField32<false>(OFFSET_OF_OBJECT_MEMBER(ArtMethod, access_flags_), new_access_flags, false);
  }

  // Approximate what kind of method call would be used for this method.
  InvokeType GetInvokeType() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Returns true if the method is declared public.
  bool IsPublic() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return (GetAccessFlags() & kAccPublic) != 0;
  }

  // Returns true if the method is declared private.
  bool IsPrivate() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return (GetAccessFlags() & kAccPrivate) != 0;
  }

  // Returns true if the method is declared static.
  bool IsStatic() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return (GetAccessFlags() & kAccStatic) != 0;
  }

  // Returns true if the method is a constructor.
  bool IsConstructor() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return (GetAccessFlags() & kAccConstructor) != 0;
  }

  // Returns true if the method is static, private, or a constructor.
  bool IsDirect() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return IsDirect(GetAccessFlags());
  }

  static bool IsDirect(uint32_t access_flags) {
    return (access_flags & (kAccStatic | kAccPrivate | kAccConstructor)) != 0;
  }

  // Returns true if the method is declared synchronized.
  bool IsSynchronized() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    uint32_t synchonized = kAccSynchronized | kAccDeclaredSynchronized;
    return (GetAccessFlags() & synchonized) != 0;
  }

  bool IsFinal() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return (GetAccessFlags() & kAccFinal) != 0;
  }

  bool IsMiranda() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return (GetAccessFlags() & kAccMiranda) != 0;
  }

  bool IsNative() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return (GetAccessFlags() & kAccNative) != 0;
  }

  bool IsFastNative() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    uint32_t mask = kAccFastNative | kAccNative;
    return (GetAccessFlags() & mask) == mask;
  }

  bool IsAbstract() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return (GetAccessFlags() & kAccAbstract) != 0;
  }

  bool IsSynthetic() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return (GetAccessFlags() & kAccSynthetic) != 0;
  }

  bool IsProxyMethod() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  bool IsPreverified() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return (GetAccessFlags() & kAccPreverified) != 0;
  }

  void SetPreverified() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    DCHECK(!IsPreverified());
    SetAccessFlags(GetAccessFlags() | kAccPreverified);
  }

  bool IsPortableCompiled() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return (GetAccessFlags() & kAccPortableCompiled) != 0;
  }

  void SetIsPortableCompiled() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    DCHECK(!IsPortableCompiled());
    SetAccessFlags(GetAccessFlags() | kAccPortableCompiled);
  }

  void ClearIsPortableCompiled() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    DCHECK(IsPortableCompiled());
    SetAccessFlags(GetAccessFlags() & ~kAccPortableCompiled);
  }

  bool CheckIncompatibleClassChange(InvokeType type) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  uint16_t GetMethodIndex() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  size_t GetVtableIndex() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return GetMethodIndex();
  }

  void SetMethodIndex(uint16_t new_method_index) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    // Not called within a transaction.
    SetField32<false>(OFFSET_OF_OBJECT_MEMBER(ArtMethod, method_index_), new_method_index, false);
  }

  static MemberOffset MethodIndexOffset() {
    return OFFSET_OF_OBJECT_MEMBER(ArtMethod, method_index_);
  }

  uint32_t GetCodeItemOffset() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return GetField32(OFFSET_OF_OBJECT_MEMBER(ArtMethod, dex_code_item_offset_), false);
  }

  void SetCodeItemOffset(uint32_t new_code_off) {
    // Not called within a transaction.
    SetField32<false>(OFFSET_OF_OBJECT_MEMBER(ArtMethod, dex_code_item_offset_), new_code_off, false);
  }

  // Number of 32bit registers that would be required to hold all the arguments
  static size_t NumArgRegisters(const StringPiece& shorty);

  uint32_t GetDexMethodIndex() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  void SetDexMethodIndex(uint32_t new_idx) {
    // Not called within a transaction.
    SetField32<false>(OFFSET_OF_OBJECT_MEMBER(ArtMethod, dex_method_index_), new_idx, false);
  }

  ObjectArray<String>* GetDexCacheStrings() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  void SetDexCacheStrings(ObjectArray<String>* new_dex_cache_strings)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  static MemberOffset DexCacheStringsOffset() {
    return OFFSET_OF_OBJECT_MEMBER(ArtMethod, dex_cache_strings_);
  }

  static MemberOffset DexCacheResolvedMethodsOffset() {
    return OFFSET_OF_OBJECT_MEMBER(ArtMethod, dex_cache_resolved_methods_);
  }

  static MemberOffset DexCacheResolvedTypesOffset() {
    return OFFSET_OF_OBJECT_MEMBER(ArtMethod, dex_cache_resolved_types_);
  }

  ObjectArray<ArtMethod>* GetDexCacheResolvedMethods() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  void SetDexCacheResolvedMethods(ObjectArray<ArtMethod>* new_dex_cache_methods)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  ObjectArray<Class>* GetDexCacheResolvedTypes() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  void SetDexCacheResolvedTypes(ObjectArray<Class>* new_dex_cache_types)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Find the method that this method overrides
  ArtMethod* FindOverriddenMethod() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  void Invoke(Thread* self, uint32_t* args, uint32_t args_size, JValue* result,
              const char* shorty) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  template<VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags>
  EntryPointFromInterpreter* GetEntryPointFromInterpreter() {
    return GetFieldPtr<EntryPointFromInterpreter*, kVerifyFlags>(
               OFFSET_OF_OBJECT_MEMBER(ArtMethod, entry_point_from_interpreter_), false);
  }

  template<VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags>
  void SetEntryPointFromInterpreter(EntryPointFromInterpreter* entry_point_from_interpreter) {
    SetFieldPtr<false, true, kVerifyFlags>(
        OFFSET_OF_OBJECT_MEMBER(ArtMethod, entry_point_from_interpreter_),
        entry_point_from_interpreter, false);
  }

  static MemberOffset EntryPointFromPortableCompiledCodeOffset() {
    return MemberOffset(OFFSETOF_MEMBER(ArtMethod, entry_point_from_portable_compiled_code_));
  }

  template<VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags>
  const void* GetEntryPointFromPortableCompiledCode() {
    return GetFieldPtr<const void*, kVerifyFlags>(
        EntryPointFromPortableCompiledCodeOffset(), false);
  }

  template<VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags>
  void SetEntryPointFromPortableCompiledCode(const void* entry_point_from_portable_compiled_code) {
    SetFieldPtr<false, true, kVerifyFlags>(
        EntryPointFromPortableCompiledCodeOffset(), entry_point_from_portable_compiled_code, false);
  }

  static MemberOffset EntryPointFromQuickCompiledCodeOffset() {
    return MemberOffset(OFFSETOF_MEMBER(ArtMethod, entry_point_from_quick_compiled_code_));
  }

  template<VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags>
  const void* GetEntryPointFromQuickCompiledCode() {
    return GetFieldPtr<const void*, kVerifyFlags>(EntryPointFromQuickCompiledCodeOffset(), false);
  }

  template<VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags>
  void SetEntryPointFromQuickCompiledCode(const void* entry_point_from_quick_compiled_code) {
    SetFieldPtr<false, true, kVerifyFlags>(
        EntryPointFromQuickCompiledCodeOffset(), entry_point_from_quick_compiled_code, false);
  }


  uint32_t GetCodeSize() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  bool IsWithinQuickCode(uintptr_t pc) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    uintptr_t code = reinterpret_cast<uintptr_t>(GetEntryPointFromQuickCompiledCode());
    if (code == 0) {
      return pc == 0;
    }
    /*
     * During a stack walk, a return PC may point past-the-end of the code
     * in the case that the last instruction is a call that isn't expected to
     * return.  Thus, we check <= code + GetCodeSize().
     *
     * NOTE: For Thumb both pc and code are offset by 1 indicating the Thumb state.
     */
    return (code <= pc && pc <= code + GetCodeSize());
  }

  void AssertPcIsWithinQuickCode(uintptr_t pc) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  uint32_t GetQuickOatCodeOffset();
  uint32_t GetPortableOatCodeOffset();
  void SetQuickOatCodeOffset(uint32_t code_offset);
  void SetPortableOatCodeOffset(uint32_t code_offset);

  // Callers should wrap the uint8_t* in a MappingTable instance for convenient access.
  const uint8_t* GetMappingTable() {
    return GetFieldPtr<const uint8_t*>(OFFSET_OF_OBJECT_MEMBER(ArtMethod, quick_mapping_table_),
        false);
  }

  template<VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags>
  void SetMappingTable(const uint8_t* mapping_table) {
    SetFieldPtr<false, true, kVerifyFlags>(
        OFFSET_OF_OBJECT_MEMBER(ArtMethod, quick_mapping_table_), mapping_table, false);
  }

  uint32_t GetOatMappingTableOffset();

  void SetOatMappingTableOffset(uint32_t mapping_table_offset);

  // Callers should wrap the uint8_t* in a VmapTable instance for convenient access.
  const uint8_t* GetVmapTable() {
    return GetFieldPtr<const uint8_t*>(OFFSET_OF_OBJECT_MEMBER(ArtMethod, quick_vmap_table_),
        false);
  }

  template<VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags>
  void SetVmapTable(const uint8_t* vmap_table) {
    SetFieldPtr<false, true, kVerifyFlags>(
        OFFSET_OF_OBJECT_MEMBER(ArtMethod, quick_vmap_table_), vmap_table, false);
  }

  uint32_t GetOatVmapTableOffset();

  void SetOatVmapTableOffset(uint32_t vmap_table_offset);

  const uint8_t* GetNativeGcMap() {
    return GetFieldPtr<uint8_t*>(OFFSET_OF_OBJECT_MEMBER(ArtMethod, gc_map_), false);
  }
  template<VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags>
  void SetNativeGcMap(const uint8_t* data) {
    SetFieldPtr<false, true, kVerifyFlags>(OFFSET_OF_OBJECT_MEMBER(ArtMethod, gc_map_), data,
                                           false);
  }

  // When building the oat need a convenient place to stuff the offset of the native GC map.
  void SetOatNativeGcMapOffset(uint32_t gc_map_offset);
  uint32_t GetOatNativeGcMapOffset();

  template <bool kCheckFrameSize = true>
  uint32_t GetFrameSizeInBytes() {
    uint32_t result = GetField32(OFFSET_OF_OBJECT_MEMBER(ArtMethod, quick_frame_size_in_bytes_),
                                 false);
    if (kCheckFrameSize) {
      DCHECK_LE(static_cast<size_t>(kStackAlignment), result);
    }
    return result;
  }

  void SetFrameSizeInBytes(size_t new_frame_size_in_bytes) {
    // Not called within a transaction.
    SetField32<false>(OFFSET_OF_OBJECT_MEMBER(ArtMethod, quick_frame_size_in_bytes_),
                      new_frame_size_in_bytes, false);
  }

  size_t GetReturnPcOffsetInBytes() {
    return GetFrameSizeInBytes() - kPointerSize;
  }

  size_t GetSirtOffsetInBytes() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return kPointerSize;
  }

  bool IsRegistered();

  void RegisterNative(Thread* self, const void* native_method, bool is_fast)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  void UnregisterNative(Thread* self) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  static MemberOffset NativeMethodOffset() {
    return OFFSET_OF_OBJECT_MEMBER(ArtMethod, entry_point_from_jni_);
  }

  const void* GetNativeMethod() {
    return GetFieldPtr<const void*>(NativeMethodOffset(), false);
  }

  template<VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags>
  void SetNativeMethod(const void*);

  static MemberOffset GetMethodIndexOffset() {
    return OFFSET_OF_OBJECT_MEMBER(ArtMethod, method_index_);
  }

  uint32_t GetCoreSpillMask() {
    return GetField32(OFFSET_OF_OBJECT_MEMBER(ArtMethod, quick_core_spill_mask_), false);
  }

  void SetCoreSpillMask(uint32_t core_spill_mask) {
    // Computed during compilation.
    // Not called within a transaction.
    SetField32<false>(OFFSET_OF_OBJECT_MEMBER(ArtMethod, quick_core_spill_mask_), core_spill_mask, false);
  }

  uint32_t GetFpSpillMask() {
    return GetField32(OFFSET_OF_OBJECT_MEMBER(ArtMethod, quick_fp_spill_mask_), false);
  }

  void SetFpSpillMask(uint32_t fp_spill_mask) {
    // Computed during compilation.
    // Not called within a transaction.
    SetField32<false>(OFFSET_OF_OBJECT_MEMBER(ArtMethod, quick_fp_spill_mask_), fp_spill_mask, false);
  }

  // Is this a CalleSaveMethod or ResolutionMethod and therefore doesn't adhere to normal
  // conventions for a method of managed code. Returns false for Proxy methods.
  bool IsRuntimeMethod() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Is this a hand crafted method used for something like describing callee saves?
  bool IsCalleeSaveMethod() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  bool IsResolutionMethod() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  bool IsImtConflictMethod() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  uintptr_t NativePcOffset(const uintptr_t pc) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Converts a native PC to a dex PC.
  uint32_t ToDexPc(const uintptr_t pc, bool abort_on_failure = true)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Converts a dex PC to a native PC.
  uintptr_t ToNativePc(const uint32_t dex_pc) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Find the catch block for the given exception type and dex_pc. When a catch block is found,
  // indicates whether the found catch block is responsible for clearing the exception or whether
  // a move-exception instruction is present.
  uint32_t FindCatchBlock(SirtRef<Class>& exception_type, uint32_t dex_pc,
                          bool* has_no_move_exception)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  static void SetClass(Class* java_lang_reflect_ArtMethod);

  static Class* GetJavaLangReflectArtMethod() {
    return java_lang_reflect_ArtMethod_;
  }

  static void ResetClass();

  static void VisitRoots(RootCallback* callback, void* arg)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

 protected:
  // Field order required by test "ValidateFieldOrderOfJavaCppUnionClasses".
  // The class we are a part of.
  HeapReference<Class> declaring_class_;

  // Short cuts to declaring_class_->dex_cache_ member for fast compiled code access.
  HeapReference<ObjectArray<ArtMethod> > dex_cache_resolved_methods_;

  // Short cuts to declaring_class_->dex_cache_ member for fast compiled code access.
  HeapReference<ObjectArray<Class> > dex_cache_resolved_types_;

  // Short cuts to declaring_class_->dex_cache_ member for fast compiled code access.
  HeapReference<ObjectArray<String> > dex_cache_strings_;

  // Method dispatch from the interpreter invokes this pointer which may cause a bridge into
  // compiled code.
  uint64_t entry_point_from_interpreter_;

  // Pointer to JNI function registered to this method, or a function to resolve the JNI function.
  uint64_t entry_point_from_jni_;

  // Method dispatch from portable compiled code invokes this pointer which may cause bridging into
  // quick compiled code or the interpreter.
  uint64_t entry_point_from_portable_compiled_code_;

  // Method dispatch from quick compiled code invokes this pointer which may cause bridging into
  // portable compiled code or the interpreter.
  uint64_t entry_point_from_quick_compiled_code_;

  // Pointer to a data structure created by the compiler and used by the garbage collector to
  // determine which registers hold live references to objects within the heap. Keyed by native PC
  // offsets for the quick compiler and dex PCs for the portable.
  uint64_t gc_map_;

  // --- Quick compiler meta-data. ---
  // TODO: merge and place in native heap, such as done with the code size.

  // Pointer to a data structure created by the quick compiler to map between dex PCs and native
  // PCs, and vice-versa.
  uint64_t quick_mapping_table_;

  // When a register is promoted into a register, the spill mask holds which registers hold dex
  // registers. The first promoted register's corresponding dex register is vmap_table_[1], the Nth
  // is vmap_table_[N]. vmap_table_[0] holds the length of the table.
  uint64_t quick_vmap_table_;

  // --- End of quick compiler meta-data. ---

  // Access flags; low 16 bits are defined by spec.
  uint32_t access_flags_;

  /* Dex file fields. The defining dex file is available via declaring_class_->dex_cache_ */

  // Offset to the CodeItem.
  uint32_t dex_code_item_offset_;

  // Index into method_ids of the dex file associated with this method.
  uint32_t dex_method_index_;

  /* End of dex file fields. */

  // Entry within a dispatch table for this method. For static/direct methods the index is into
  // the declaringClass.directMethods, for virtual methods the vtable and for interface methods the
  // ifTable.
  uint32_t method_index_;

  // --- Quick compiler meta-data. ---
  // TODO: merge and place in native heap, such as done with the code size.

  // Bit map of spilled machine registers.
  uint32_t quick_core_spill_mask_;

  // Bit map of spilled floating point machine registers.
  uint32_t quick_fp_spill_mask_;

  // Fixed frame size for this method when executed.
  uint32_t quick_frame_size_in_bytes_;

  // --- End of quick compiler meta-data. ---

  static Class* java_lang_reflect_ArtMethod_;

 private:
  friend struct art::ArtMethodOffsets;  // for verifying offset information
  DISALLOW_IMPLICIT_CONSTRUCTORS(ArtMethod);
};

class MANAGED ArtMethodClass : public Class {
 private:
  DISALLOW_IMPLICIT_CONSTRUCTORS(ArtMethodClass);
};

}  // namespace mirror
}  // namespace art

#endif  // ART_RUNTIME_MIRROR_ART_METHOD_H_

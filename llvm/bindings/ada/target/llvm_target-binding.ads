-- This file is generated by SWIG. Do *not* modify by hand.
--

with llvm;
with Interfaces.C.Strings;


package LLVM_Target.Binding is

   LLVMBigEndian    : constant := 0;
   LLVMLittleEndian : constant := 1;

   procedure LLVMInitializeAllTargets;

   function LLVMInitializeNativeTarget return  Interfaces.C.int;

   function LLVMCreateTargetData
     (StringRep : in Interfaces.C.Strings.chars_ptr)
      return      LLVM_Target.LLVMTargetDataRef;

   procedure LLVMAddTargetData
     (arg_2_1 : in LLVM_Target.LLVMTargetDataRef;
      arg_2_2 : in llvm.LLVMPassManagerRef);

   function LLVMCopyStringRepOfTargetData
     (arg_1 : in LLVM_Target.LLVMTargetDataRef)
      return  Interfaces.C.Strings.chars_ptr;

   function LLVMByteOrder
     (arg_1 : in LLVM_Target.LLVMTargetDataRef)
      return  LLVM_Target.LLVMByteOrdering;

   function LLVMPointerSize
     (arg_1 : in LLVM_Target.LLVMTargetDataRef)
      return  Interfaces.C.unsigned;

   function LLVMIntPtrType
     (arg_1 : in LLVM_Target.LLVMTargetDataRef)
      return  llvm.LLVMTypeRef;

   function LLVMSizeOfTypeInBits
     (arg_2_1 : in LLVM_Target.LLVMTargetDataRef;
      arg_2_2 : in llvm.LLVMTypeRef)
      return    Interfaces.C.Extensions.unsigned_long_long;

   function LLVMStoreSizeOfType
     (arg_2_1 : in LLVM_Target.LLVMTargetDataRef;
      arg_2_2 : in llvm.LLVMTypeRef)
      return    Interfaces.C.Extensions.unsigned_long_long;

   function LLVMABISizeOfType
     (arg_2_1 : in LLVM_Target.LLVMTargetDataRef;
      arg_2_2 : in llvm.LLVMTypeRef)
      return    Interfaces.C.Extensions.unsigned_long_long;

   function LLVMABIAlignmentOfType
     (arg_2_1 : in LLVM_Target.LLVMTargetDataRef;
      arg_2_2 : in llvm.LLVMTypeRef)
      return    Interfaces.C.unsigned;

   function LLVMCallFrameAlignmentOfType
     (arg_2_1 : in LLVM_Target.LLVMTargetDataRef;
      arg_2_2 : in llvm.LLVMTypeRef)
      return    Interfaces.C.unsigned;

   function LLVMPreferredAlignmentOfType
     (arg_2_1 : in LLVM_Target.LLVMTargetDataRef;
      arg_2_2 : in llvm.LLVMTypeRef)
      return    Interfaces.C.unsigned;

   function LLVMPreferredAlignmentOfGlobal
     (arg_1     : in LLVM_Target.LLVMTargetDataRef;
      GlobalVar : in llvm.LLVMValueRef)
      return      Interfaces.C.unsigned;

   function LLVMElementAtOffset
     (arg_1    : in LLVM_Target.LLVMTargetDataRef;
      StructTy : in llvm.LLVMTypeRef;
      Offset   : in Interfaces.C.Extensions.unsigned_long_long)
      return     Interfaces.C.unsigned;

   function LLVMOffsetOfElement
     (arg_1    : in LLVM_Target.LLVMTargetDataRef;
      StructTy : in llvm.LLVMTypeRef;
      Element  : in Interfaces.C.unsigned)
      return     Interfaces.C.Extensions.unsigned_long_long;

   procedure LLVMInvalidateStructLayout
     (arg_1    : in LLVM_Target.LLVMTargetDataRef;
      StructTy : in llvm.LLVMTypeRef);

   procedure LLVMDisposeTargetData
     (arg_1 : in LLVM_Target.LLVMTargetDataRef);

private

   pragma Import
     (C,
      LLVMInitializeAllTargets,
      "Ada_LLVMInitializeAllTargets");
   pragma Import
     (C,
      LLVMInitializeNativeTarget,
      "Ada_LLVMInitializeNativeTarget");
   pragma Import (C, LLVMCreateTargetData, "Ada_LLVMCreateTargetData");
   pragma Import (C, LLVMAddTargetData, "Ada_LLVMAddTargetData");
   pragma Import
     (C,
      LLVMCopyStringRepOfTargetData,
      "Ada_LLVMCopyStringRepOfTargetData");
   pragma Import (C, LLVMByteOrder, "Ada_LLVMByteOrder");
   pragma Import (C, LLVMPointerSize, "Ada_LLVMPointerSize");
   pragma Import (C, LLVMIntPtrType, "Ada_LLVMIntPtrType");
   pragma Import (C, LLVMSizeOfTypeInBits, "Ada_LLVMSizeOfTypeInBits");
   pragma Import (C, LLVMStoreSizeOfType, "Ada_LLVMStoreSizeOfType");
   pragma Import (C, LLVMABISizeOfType, "Ada_LLVMABISizeOfType");
   pragma Import (C, LLVMABIAlignmentOfType, "Ada_LLVMABIAlignmentOfType");
   pragma Import
     (C,
      LLVMCallFrameAlignmentOfType,
      "Ada_LLVMCallFrameAlignmentOfType");
   pragma Import
     (C,
      LLVMPreferredAlignmentOfType,
      "Ada_LLVMPreferredAlignmentOfType");
   pragma Import
     (C,
      LLVMPreferredAlignmentOfGlobal,
      "Ada_LLVMPreferredAlignmentOfGlobal");
   pragma Import (C, LLVMElementAtOffset, "Ada_LLVMElementAtOffset");
   pragma Import (C, LLVMOffsetOfElement, "Ada_LLVMOffsetOfElement");
   pragma Import
     (C,
      LLVMInvalidateStructLayout,
      "Ada_LLVMInvalidateStructLayout");
   pragma Import (C, LLVMDisposeTargetData, "Ada_LLVMDisposeTargetData");

end LLVM_Target.Binding;
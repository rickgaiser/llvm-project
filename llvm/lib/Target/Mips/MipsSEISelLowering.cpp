//===- MipsSEISelLowering.cpp - MipsSE DAG Lowering Interface -------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Subclass of MipsTargetLowering specialized for mips32/64.
//
//===----------------------------------------------------------------------===//

#include "MipsSEISelLowering.h"
#include "MipsMachineFunction.h"
#include "MipsRegisterInfo.h"
#include "MipsSubtarget.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/CodeGen/CallingConvLower.h"
#include "llvm/CodeGen/ISDOpcodes.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineMemOperand.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/SelectionDAG.h"
#include "llvm/CodeGen/SelectionDAGAddressAnalysis.h"
#include "llvm/CodeGen/SelectionDAGNodes.h"
#include "llvm/CodeGen/SelectionDAGSums.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/TargetLowering.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/CodeGen/ValueTypes.h"
#include "llvm/CodeGenTypes/MachineValueType.h"
#include "llvm/IR/DebugLoc.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/IntrinsicsMips.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Triple.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <utility>

using namespace llvm;

#define DEBUG_TYPE "mips-isel"

// Widen the v2 vectors to the register width, i.e. v2i16 -> v8i16,
// v2i32 -> v4i32, etc, to ensure the correct rail size is used, i.e.
// INST.h for v16, INST.w for v32, INST.d for v64.
TargetLoweringBase::LegalizeTypeAction
MipsSETargetLowering::getPreferredVectorAction(MVT VT) const {
  if (this->Subtarget.hasMSA()) {
    switch (VT.SimpleTy) {
    // Leave v2i1 vectors to be promoted to larger ones.
    // Other i1 types will be promoted by default.
    case MVT::v2i1:
      return TypePromoteInteger;
      break;
    // 16-bit vector types (v2 and longer)
    case MVT::v2i8:
    // 32-bit vector types (v2 and longer)
    case MVT::v2i16:
    case MVT::v4i8:
    // 64-bit vector types (v2 and longer)
    case MVT::v2i32:
    case MVT::v4i16:
    case MVT::v8i8:
      return TypeWidenVector;
      break;
    // Only word (.w) and doubleword (.d) are available for floating point
    // vectors. That means floating point vectors should be either v2f64
    // or v4f32.
    // Here we only explicitly widen the f32 types - f16 will be promoted
    // by default.
    case MVT::v2f32:
    case MVT::v3f32:
      return TypeWidenVector;
    // v2i64 is already 128-bit wide.
    default:
      break;
    }
  }
  return TargetLoweringBase::getPreferredVectorAction(VT);
}

MipsSETargetLowering::MipsSETargetLowering(const MipsTargetMachine &TM,
                                           const MipsSubtarget &STI)
    : MipsTargetLowering(TM, STI) {
  // Set up the register classes
  addRegisterClass(MVT::i32, &Mips::GPR32RegClass);

  if (Subtarget.isGP64bit()) {
    addRegisterClass(MVT::i64, &Mips::GPR64RegClass);
    // BUILD_PAIR combines two i32 into i64, needs to expand to shifts+OR
    // since there's no native instruction for this on MIPS64
    setOperationAction(ISD::BUILD_PAIR, MVT::i64, Expand);
  }

  // R5900 MMI: Register vector types with GPR128 for SIMD operations
  // v4i32 (4x32-bit words), v8i16 (8x16-bit halfwords), v16i8 (16x8-bit bytes)
  if (Subtarget.isR5900()) {
    // Register vector types with GPR128 (no v2i64 - no native 64-bit element
    // SIMD)
    addRegisterClass(MVT::v4i32, &Mips::GPR128RegClass);
    addRegisterClass(MVT::v8i16, &Mips::GPR128RegClass);
    addRegisterClass(MVT::v16i8, &Mips::GPR128RegClass);

    // Expand all truncating stores and extending loads for R5900 vectors
    for (MVT VT0 : MVT::fixedlen_vector_valuetypes()) {
      for (MVT VT1 : MVT::fixedlen_vector_valuetypes()) {
        setTruncStoreAction(VT0, VT1, Expand);
        setLoadExtAction(ISD::SEXTLOAD, VT0, VT1, Expand);
        setLoadExtAction(ISD::ZEXTLOAD, VT0, VT1, Expand);
        setLoadExtAction(ISD::EXTLOAD, VT0, VT1, Expand);
      }
    }

    // Set up operation actions for each vector type
    for (MVT VT : {MVT::v4i32, MVT::v8i16, MVT::v16i8}) {
      // Expand all builtin opcodes by default
      for (unsigned Opc = 0; Opc < ISD::BUILTIN_OP_END; ++Opc)
        setOperationAction(Opc, VT, Expand);

      // Load/Store: Custom to handle alignment requirements
      // LQ/SQ require 16-byte alignment; unaligned accesses are scalarized
      setOperationAction(ISD::LOAD, VT, Custom);
      setOperationAction(ISD::STORE, VT, Custom);
      setOperationAction(ISD::BITCAST, VT, Legal);
      // Shuffles: the zips, unzips and half copies of MMI, the others through
      // the stack.
      setOperationAction(ISD::VECTOR_SHUFFLE, VT, Custom);
      // Splats of a register with MMI, the others through the stack.
      setOperationAction(ISD::BUILD_VECTOR, VT, Custom);

      // Arithmetic: PADDW/PSUBW (v4i32), PADDH/PSUBH (v8i16), PADDB/PSUBB
      // (v16i8)
      setOperationAction(ISD::ADD, VT, Legal);
      setOperationAction(ISD::SUB, VT, Legal);

      // Logical: PAND, POR, PXOR (all sizes use same instruction)
      setOperationAction(ISD::AND, VT, Legal);
      setOperationAction(ISD::OR, VT, Legal);
      setOperationAction(ISD::XOR, VT, Legal);
    }

    // A word with a constant index: the low doubleword or PCPYUD, and DSRL32
    // for an odd one.
    setOperationAction(ISD::EXTRACT_VECTOR_ELT, MVT::v4i32, Custom);

    // Min/Max: Only available for v4i32 (PMAXW/PMINW) and v8i16 (PMAXH/PMINH)
    setOperationAction(ISD::SMAX, MVT::v4i32, Legal);
    setOperationAction(ISD::SMIN, MVT::v4i32, Legal);
    setOperationAction(ISD::SMAX, MVT::v8i16, Legal);
    setOperationAction(ISD::SMIN, MVT::v8i16, Legal);

    // Absolute value: PABSW (v4i32), PABSH (v8i16)
    setOperationAction(ISD::ABS, MVT::v4i32, Legal);
    setOperationAction(ISD::ABS, MVT::v8i16, Legal);

    // Scalar MMI operations - use PABSW/PMAXW/PMINW for i32
    setOperationAction(ISD::ABS, MVT::i32, Legal);
    setOperationAction(ISD::SMAX, MVT::i32, Legal);
    setOperationAction(ISD::SMIN, MVT::i32, Legal);
    // Scalar saturating arithmetic - use PADDSW/PSUBSW/PADDUW/PSUBUW for i32
    setOperationAction(ISD::SADDSAT, MVT::i32, Legal);
    setOperationAction(ISD::SSUBSAT, MVT::i32, Legal);
    setOperationAction(ISD::UADDSAT, MVT::i32, Legal);
    setOperationAction(ISD::USUBSAT, MVT::i32, Legal);
    // CTLZ using PLZCW with conditional (for non-negative) or 0 (for negative)
    setOperationAction(ISD::CTLZ, MVT::i32, Custom);

    // Shift operations: Custom lowering for constant splat shifts
    // PSLLVW/PSRLVW/PSRAVW only operate on 2 of 4 words - NOT usable for v4i32.
    // PSLLW/PSRLW/PSRAW work on all 4 words with immediate shift amounts.
    // PSLLH/PSRLH/PSRAH work on all 8 halfwords with immediate shift amounts.
    // Custom lowering uses immediate shifts for constant splats, else expands.
    setOperationAction(ISD::SHL, MVT::v4i32, Custom);
    setOperationAction(ISD::SRL, MVT::v4i32, Custom);
    setOperationAction(ISD::SRA, MVT::v4i32, Custom);
    setOperationAction(ISD::SHL, MVT::v8i16, Custom);
    setOperationAction(ISD::SRL, MVT::v8i16, Custom);
    setOperationAction(ISD::SRA, MVT::v8i16, Custom);

    // Saturating arithmetic: Available for all vector sizes
    // Signed: PADDSW/PSUBSW (v4i32), PADDSH/PSUBSH (v8i16), PADDSB/PSUBSB
    // (v16i8) Unsigned: PADDUW/PSUBUW (v4i32), PADDUH/PSUBUH (v8i16),
    // PADDUB/PSUBUB (v16i8)
    for (MVT VT : {MVT::v4i32, MVT::v8i16, MVT::v16i8}) {
      setOperationAction(ISD::SADDSAT, VT, Legal);
      setOperationAction(ISD::SSUBSAT, VT, Legal);
      setOperationAction(ISD::UADDSAT, VT, Legal);
      setOperationAction(ISD::USUBSAT, VT, Legal);
    }

    // Compares: PCGT and PCEQ give the masks of SETGT and SETEQ. The other
    // signed conditions swap the operands or invert the mask, the unsigned
    // ones flip the sign bits of the operands. Selects use the masks.
    for (MVT VT : {MVT::v4i32, MVT::v8i16, MVT::v16i8}) {
      setOperationAction(ISD::SETCC, VT, Legal);
      setCondCodeAction({ISD::SETNE, ISD::SETLT, ISD::SETGE, ISD::SETLE}, VT,
                        Expand);
      setCondCodeAction({ISD::SETUGT, ISD::SETUGE, ISD::SETULT, ISD::SETULE},
                        VT, Custom);
      setOperationAction(ISD::VSELECT, VT, Custom);
    }
  }

  if (Subtarget.hasDSP() || Subtarget.hasMSA()) {
    // Expand all truncating stores and extending loads.
    for (MVT VT0 : MVT::fixedlen_vector_valuetypes()) {
      for (MVT VT1 : MVT::fixedlen_vector_valuetypes()) {
        setTruncStoreAction(VT0, VT1, Expand);
        setLoadExtAction(ISD::SEXTLOAD, VT0, VT1, Expand);
        setLoadExtAction(ISD::ZEXTLOAD, VT0, VT1, Expand);
        setLoadExtAction(ISD::EXTLOAD, VT0, VT1, Expand);
      }
    }
  }

  if (Subtarget.hasDSP()) {
    MVT::SimpleValueType VecTys[2] = {MVT::v2i16, MVT::v4i8};

    for (const auto &VecTy : VecTys) {
      addRegisterClass(VecTy, &Mips::DSPRRegClass);

      // Expand all builtin opcodes.
      for (unsigned Opc = 0; Opc < ISD::BUILTIN_OP_END; ++Opc)
        setOperationAction(Opc, VecTy, Expand);

      setOperationAction(ISD::ADD, VecTy, Legal);
      setOperationAction(ISD::SUB, VecTy, Legal);
      setOperationAction(ISD::LOAD, VecTy, Legal);
      setOperationAction(ISD::STORE, VecTy, Legal);
      setOperationAction(ISD::BITCAST, VecTy, Legal);
    }

    setTargetDAGCombine(
        {ISD::SHL, ISD::SRA, ISD::SRL, ISD::SETCC, ISD::VSELECT});

    if (Subtarget.hasMips32r2()) {
      setOperationAction(ISD::ADDC, MVT::i32, Legal);
      setOperationAction(ISD::ADDE, MVT::i32, Legal);
    }
  }

  if (Subtarget.hasDSPR2())
    setOperationAction(ISD::MUL, MVT::v2i16, Legal);

  if (Subtarget.hasMSA()) {
    addMSAIntType(MVT::v16i8, &Mips::MSA128BRegClass);
    addMSAIntType(MVT::v8i16, &Mips::MSA128HRegClass);
    addMSAIntType(MVT::v4i32, &Mips::MSA128WRegClass);
    addMSAIntType(MVT::v2i64, &Mips::MSA128DRegClass);
    addMSAFloatType(MVT::v8f16, &Mips::MSA128HRegClass);
    addMSAFloatType(MVT::v4f32, &Mips::MSA128WRegClass);
    addMSAFloatType(MVT::v2f64, &Mips::MSA128DRegClass);

    // Shuffle half vectors as integers to avoid expanding them through
    // EXTRACT_VECTOR_ELT and BUILD_VECTOR with an illegal scalar f16 type.
    setOperationPromotedToType(ISD::VECTOR_SHUFFLE, MVT::v8f16, MVT::v8i16);

    // We're using soft promotion for f16, but msa has some instructions for
    // conversion to/from f16. Mark those conversions as custom so we can take
    // advantage of these instructions.
    for (MVT VT : {MVT::f32, MVT::f64}) {
      setOperationAction(ISD::FP16_TO_FP, VT, Custom);
      setOperationAction(ISD::FP_TO_FP16, VT, Custom);
    }

    setTargetDAGCombine(
        {ISD::AND, ISD::OR, ISD::SRA, ISD::VSELECT, ISD::XOR, ISD::FP_TO_UINT});
  }

  // R5900 VU0 (COP2) support for v4f32 vectors
  // VU0 macro mode provides SIMD operations on 128-bit VF registers.
  if (Subtarget.hasVU0()) {
    addRegisterClass(MVT::v4f32, &Mips::VFRegsRegClass);

    // Expand all builtin opcodes by default
    for (unsigned Opc = 0; Opc < ISD::BUILTIN_OP_END; ++Opc)
      setOperationAction(Opc, MVT::v4f32, Expand);

    // Enable load/store (LQC2/SQC2)
    setOperationAction(ISD::LOAD, MVT::v4f32, Legal);
    setOperationAction(ISD::STORE, MVT::v4f32, Legal);

    // QMTC2 and QMFC2 move a vector between VU0 and a 128-bit GPR.
    setOperationAction(ISD::BITCAST, MVT::v4f32, Legal);

    // VFTOI0 and VITOF0 convert to and from a v4i32 in a 128-bit GPR.
    setOperationAction(ISD::FP_TO_SINT, MVT::v4i32, Legal);
    setOperationAction(ISD::SINT_TO_FP, MVT::v4i32, Legal);
    setOperationAction(ISD::FP_TO_UINT, MVT::v4i32, Custom);

    // Compares of the bits as ordered integers with MMI.
    setOperationAction(ISD::SETCC, MVT::v4f32, Custom);
    setOperationAction(ISD::VSELECT, MVT::v4f32, Custom);

    // Enable VU0 arithmetic operations (VADD, VSUB, VMUL)
    setOperationAction(ISD::FADD, MVT::v4f32, Legal);
    setOperationAction(ISD::FSUB, MVT::v4f32, Legal);
    setOperationAction(ISD::FMUL, MVT::v4f32, Legal);
    // DIV.S and VDIV in parallel (lowerVU0FDIV).
    setOperationAction(ISD::FDIV, MVT::v4f32, Custom);

    // Enable VU0 abs, min, max operations (VABS, VMINI, VMAX)
    setOperationAction(ISD::FABS, MVT::v4f32, Legal);
    setOperationAction(ISD::FMINNUM, MVT::v4f32, Legal);
    setOperationAction(ISD::FMAXNUM, MVT::v4f32, Legal);

    // Enable custom lowering for broadcast pattern recognition
    // This allows recognizing shuffle+fmul -> VMULbc patterns
    setOperationAction(ISD::VECTOR_SHUFFLE, MVT::v4f32, Custom);
    setOperationAction(ISD::BUILD_VECTOR, MVT::v4f32, Custom);
    setOperationAction(ISD::SCALAR_TO_VECTOR, MVT::v4f32, Custom);

    // Enable extract/insert for element access
    // VU0 has no native element access, so we use QMFC2/QMTC2 + GPR ops
    setOperationAction(ISD::EXTRACT_VECTOR_ELT, MVT::v4f32, Custom);
    setOperationAction(ISD::INSERT_VECTOR_ELT, MVT::v4f32, Custom);

    // Enable FMA lowering - lower to FMUL + FADD since VU0 has no direct FMA
    // (VMADD requires ACC setup which is handled separately by the ACC chain
    // pass)
    setOperationAction(ISD::FMA, MVT::v4f32, Custom);

    // Enable DAG combine for broadcast patterns and load optimization
    // Note: VU0_BLEND is a target node that's always eligible for DAG combine
    setTargetDAGCombine({ISD::FADD, ISD::FSUB, ISD::FMUL, ISD::FMAXNUM,
                         ISD::FMINNUM, ISD::BITCAST, ISD::SETCC,
                         ISD::FP_TO_SINT, ISD::FDIV, ISD::STORE,
                         ISD::SINT_TO_FP, ISD::UINT_TO_FP});
  }

  if (!Subtarget.useSoftFloat()) {
    addRegisterClass(MVT::f32, &Mips::FGR32RegClass);

    // RINT uses the current rounding mode and may raise inexact.
    if (Subtarget.hasMips32r6() && !Subtarget.inMicroMipsMode()) {
      setOperationAction(ISD::FRINT, MVT::f32, Legal);
      setOperationAction(ISD::STRICT_FRINT, MVT::f32, Legal);
      if (!Subtarget.isSingleFloat()) {
        setOperationAction(ISD::FRINT, MVT::f64, Legal);
        setOperationAction(ISD::STRICT_FRINT, MVT::f64, Legal);
      }
    }

    // When dealing with single precision only, use libcalls
    if (!Subtarget.isSingleFloat()) {
      if (Subtarget.isFP64bit())
        addRegisterClass(MVT::f64, &Mips::FGR64RegClass);
      else
        addRegisterClass(MVT::f64, &Mips::AFGR64RegClass);
    }

    for (auto Op : {ISD::STRICT_FADD, ISD::STRICT_FSUB, ISD::STRICT_FMUL,
                    ISD::STRICT_FDIV, ISD::STRICT_FSQRT}) {
      setOperationAction(Op, MVT::f32, Legal);
      setOperationAction(Op, MVT::f64, Legal);
    }
  }

  // Targets with 64bits integer registers, but no 64bit floating point register
  // do not support conversion between them
  if (Subtarget.isGP64bit() && Subtarget.isSingleFloat() &&
      !Subtarget.useSoftFloat()) {
    setOperationAction(ISD::FP_TO_SINT, MVT::i64, Expand);
    setOperationAction(ISD::FP_TO_UINT, MVT::i64, Expand);
    setOperationAction(ISD::SINT_TO_FP, MVT::i64, Expand);
    setOperationAction(ISD::UINT_TO_FP, MVT::i64, Expand);
  }

  setOperationAction(ISD::SMUL_LOHI,          MVT::i32, Custom);
  setOperationAction(ISD::UMUL_LOHI,          MVT::i32, Custom);
  setOperationAction(ISD::MULHS,              MVT::i32, Custom);
  setOperationAction(ISD::MULHU,              MVT::i32, Custom);

  if (Subtarget.hasCnMips())
    setOperationAction(ISD::MUL,              MVT::i64, Legal);
  else if (Subtarget.isR5900()) {
    // R5900 doesn't have DMULT/DMULTU/DDIV/DDIVU - expand to 32-bit ops
    // A widening multiply is custom lowered to PMULTW/PMULTUW.
    setOperationAction(ISD::MUL, MVT::i64, Custom);
    setOperationAction(ISD::SMUL_LOHI, MVT::i64, Expand);
    setOperationAction(ISD::UMUL_LOHI, MVT::i64, Expand);
    setOperationAction(ISD::MULHS, MVT::i64, Expand);
    setOperationAction(ISD::MULHU, MVT::i64, Expand);
    setOperationAction(ISD::SDIVREM, MVT::i64, Expand);
    setOperationAction(ISD::UDIVREM, MVT::i64, Expand);
  } else if (Subtarget.isGP64bit())
    setOperationAction(ISD::MUL,              MVT::i64, Custom);

  if (Subtarget.isGP64bit() && !Subtarget.isR5900()) {
    setOperationAction(ISD::SMUL_LOHI,        MVT::i64, Custom);
    setOperationAction(ISD::UMUL_LOHI,        MVT::i64, Custom);
    setOperationAction(ISD::MULHS,            MVT::i64, Custom);
    setOperationAction(ISD::MULHU,            MVT::i64, Custom);
    setOperationAction(ISD::SDIVREM,          MVT::i64, Custom);
    setOperationAction(ISD::UDIVREM,          MVT::i64, Custom);
  }

  setOperationAction(ISD::INTRINSIC_WO_CHAIN, MVT::i64, Custom);
  setOperationAction(ISD::INTRINSIC_W_CHAIN,  MVT::i64, Custom);

  setOperationAction(ISD::SDIVREM, MVT::i32, Custom);
  setOperationAction(ISD::UDIVREM, MVT::i32, Custom);
  setOperationAction(ISD::ATOMIC_FENCE,       MVT::Other, Custom);
  if (Subtarget.hasMips32r6()) {
    setOperationAction(ISD::LOAD,               MVT::i32, Legal);
    setOperationAction(ISD::STORE,              MVT::i32, Legal);
  } else {
    setOperationAction(ISD::LOAD,               MVT::i32, Custom);
    setOperationAction(ISD::STORE,              MVT::i32, Custom);
  }

  setTargetDAGCombine(ISD::MUL);

  setOperationAction(ISD::INTRINSIC_WO_CHAIN, MVT::Other, Custom);
  setOperationAction(ISD::INTRINSIC_W_CHAIN, MVT::Other, Custom);
  setOperationAction(ISD::INTRINSIC_VOID, MVT::Other, Custom);

  if (Subtarget.hasMips32r2() && !Subtarget.useSoftFloat() &&
      !Subtarget.hasMips64()) {
    setOperationAction(ISD::BITCAST, MVT::i64, Custom);
  }

  if (Subtarget.getCLOpts().mno_ldc1_sdc1 ||
      (Subtarget.hasMips1() && !Subtarget.hasMips2())) {
    setOperationAction(ISD::LOAD, MVT::f64, Custom);
    setOperationAction(ISD::STORE, MVT::f64, Custom);
  }

  if (Subtarget.hasMips32r6()) {
    // MIPS32r6 replaces the accumulator-based multiplies with a three register
    // instruction
    setOperationAction(ISD::SMUL_LOHI, MVT::i32, Expand);
    setOperationAction(ISD::UMUL_LOHI, MVT::i32, Expand);
    setOperationAction(ISD::MUL, MVT::i32, Legal);
    setOperationAction(ISD::MULHS, MVT::i32, Legal);
    setOperationAction(ISD::MULHU, MVT::i32, Legal);

    // MIPS32r6 replaces the accumulator-based division/remainder with separate
    // three register division and remainder instructions.
    setOperationAction(ISD::SDIVREM, MVT::i32, Expand);
    setOperationAction(ISD::UDIVREM, MVT::i32, Expand);
    setOperationAction(ISD::SDIV, MVT::i32, Legal);
    setOperationAction(ISD::UDIV, MVT::i32, Legal);
    setOperationAction(ISD::SREM, MVT::i32, Legal);
    setOperationAction(ISD::UREM, MVT::i32, Legal);

    // MIPS32r6 replaces conditional moves with an equivalent that removes the
    // need for three GPR read ports.
    setOperationAction(ISD::SETCC, MVT::i32, Legal);
    setOperationAction(ISD::SELECT, MVT::i32, Legal);
    setOperationAction(ISD::SELECT_CC, MVT::i32, Expand);

    setOperationAction(ISD::SETCC, MVT::f32, Legal);
    setOperationAction(ISD::SELECT, MVT::f32, Legal);
    setOperationAction(ISD::SELECT_CC, MVT::f32, Expand);

    assert(Subtarget.isFP64bit() && "FR=1 is required for MIPS32r6");
    setOperationAction(ISD::SETCC, MVT::f64, Legal);
    setOperationAction(ISD::SELECT, MVT::f64, Legal);
    setOperationAction(ISD::SELECT_CC, MVT::f64, Expand);

    setOperationAction(ISD::BRCOND, MVT::Other, Legal);

    // Floating point > and >= are supported via < and <=
    setCondCodeAction(ISD::SETOGE, MVT::f32, Expand);
    setCondCodeAction(ISD::SETOGT, MVT::f32, Expand);
    setCondCodeAction(ISD::SETUGE, MVT::f32, Expand);
    setCondCodeAction(ISD::SETUGT, MVT::f32, Expand);
    setCondCodeAction(ISD::SETONE, MVT::f32, Expand);
    setCondCodeAction(ISD::SETO, MVT::f32, Expand);
    setCondCodeAction(ISD::SETUNE, MVT::f32, Expand);
    setCondCodeAction(ISD::SETNE, MVT::f32, Expand);

    setCondCodeAction(ISD::SETOGE, MVT::f64, Expand);
    setCondCodeAction(ISD::SETOGT, MVT::f64, Expand);
    setCondCodeAction(ISD::SETUGE, MVT::f64, Expand);
    setCondCodeAction(ISD::SETUGT, MVT::f64, Expand);
    setCondCodeAction(ISD::SETONE, MVT::f64, Expand);
    setCondCodeAction(ISD::SETO, MVT::f64, Expand);
    setCondCodeAction(ISD::SETUNE, MVT::f64, Expand);
    setCondCodeAction(ISD::SETNE, MVT::f64, Expand);
  }

  if (Subtarget.hasMips64r6()) {
    // MIPS64r6 replaces the accumulator-based multiplies with a three register
    // instruction
    setOperationAction(ISD::SMUL_LOHI, MVT::i64, Expand);
    setOperationAction(ISD::UMUL_LOHI, MVT::i64, Expand);
    setOperationAction(ISD::MUL, MVT::i64, Legal);
    setOperationAction(ISD::MULHS, MVT::i64, Legal);
    setOperationAction(ISD::MULHU, MVT::i64, Legal);

    // MIPS32r6 replaces the accumulator-based division/remainder with separate
    // three register division and remainder instructions.
    setOperationAction(ISD::SDIVREM, MVT::i64, Expand);
    setOperationAction(ISD::UDIVREM, MVT::i64, Expand);
    setOperationAction(ISD::SDIV, MVT::i64, Legal);
    setOperationAction(ISD::UDIV, MVT::i64, Legal);
    setOperationAction(ISD::SREM, MVT::i64, Legal);
    setOperationAction(ISD::UREM, MVT::i64, Legal);

    // MIPS64r6 replaces conditional moves with an equivalent that removes the
    // need for three GPR read ports.
    setOperationAction(ISD::SETCC, MVT::i64, Legal);
    setOperationAction(ISD::SELECT, MVT::i64, Legal);
    setOperationAction(ISD::SELECT_CC, MVT::i64, Expand);
  }

  if (Subtarget.isR5900()) {
    setTargetDAGCombine({ISD::TRUNCATE, ISD::SIGN_EXTEND, ISD::ZERO_EXTEND,
                         ISD::ANY_EXTEND, ISD::BITCAST,
                         ISD::EXTRACT_VECTOR_ELT});

    // R5900 FPU only supports 4 compare conditions: C.F, C.EQ, C.OLT, C.OLE
    // (and their inversions via bc1t/bc1f). Expand all conditions that would
    // require C.UN, C.UEQ, C.ULT, or C.ULE instructions (not available on
    // R5900). The legalizer resolves these via operand swapping, condition
    // inversion, and decomposition into supported conditions.
    setCondCodeAction(ISD::SETOGT, MVT::f32, Expand);
    setCondCodeAction(ISD::SETOGE, MVT::f32, Expand);
    setCondCodeAction(ISD::SETGT, MVT::f32, Expand);
    setCondCodeAction(ISD::SETGE, MVT::f32, Expand);
    setCondCodeAction(ISD::SETULT, MVT::f32, Expand);
    setCondCodeAction(ISD::SETULE, MVT::f32, Expand);
    setCondCodeAction(ISD::SETUO, MVT::f32, Expand);
    setCondCodeAction(ISD::SETO, MVT::f32, Expand);
    setCondCodeAction(ISD::SETONE, MVT::f32, Expand);
    setCondCodeAction(ISD::SETUEQ, MVT::f32, Expand);
    setCondCodeAction(ISD::SETNE, MVT::f32, Expand);

    // R5900 FPU does not support IEEE 754 special values (NaN, infinity). Use
    // custom lowering to decide per-instruction: hardware when nnan+ninf flags
    // guarantee no NaN or infinity, software libcall otherwise.
    setOperationAction(ISD::FADD, MVT::f32, Custom);
    setOperationAction(ISD::FSUB, MVT::f32, Custom);
    setOperationAction(ISD::FMUL, MVT::f32, Custom);
    setOperationAction(ISD::FDIV, MVT::f32, Custom);
    setOperationAction(ISD::FSQRT, MVT::f32, Custom);
    // MIN.S and MAX.S do not handle NaN either.
    setOperationAction(ISD::FMINNUM, MVT::f32, Custom);
    setOperationAction(ISD::FMAXNUM, MVT::f32, Custom);
  }

  computeRegisterProperties(Subtarget.getRegisterInfo());
}

const MipsTargetLowering *
llvm::createMipsSETargetLowering(const MipsTargetMachine &TM,
                                 const MipsSubtarget &STI) {
  return new MipsSETargetLowering(TM, STI);
}

const TargetRegisterClass *
MipsSETargetLowering::getRepRegClassFor(MVT VT) const {
  if (VT == MVT::Untyped)
    return Subtarget.hasDSP() ? &Mips::ACC64DSPRegClass : &Mips::ACC64RegClass;

  return TargetLowering::getRepRegClassFor(VT);
}

// Enable MSA support for the given integer type and Register class.
void MipsSETargetLowering::
addMSAIntType(MVT::SimpleValueType Ty, const TargetRegisterClass *RC) {
  addRegisterClass(Ty, RC);

  // Expand all builtin opcodes.
  for (unsigned Opc = 0; Opc < ISD::BUILTIN_OP_END; ++Opc)
    setOperationAction(Opc, Ty, Expand);

  setOperationAction(ISD::BITCAST, Ty, Legal);
  setOperationAction(ISD::LOAD, Ty, Legal);
  setOperationAction(ISD::STORE, Ty, Legal);
  setOperationAction(ISD::EXTRACT_VECTOR_ELT, Ty, Custom);
  setOperationAction(ISD::INSERT_VECTOR_ELT, Ty, Legal);
  setOperationAction(ISD::BUILD_VECTOR, Ty, Custom);
  setOperationAction({ISD::UNDEF, ISD::POISON}, Ty, Legal);

  setOperationAction(ISD::ADD, Ty, Legal);
  setOperationAction(ISD::AND, Ty, Legal);
  setOperationAction(ISD::CTLZ, Ty, Legal);
  setOperationAction(ISD::CTPOP, Ty, Legal);
  setOperationAction(ISD::MUL, Ty, Legal);
  setOperationAction(ISD::OR, Ty, Legal);
  setOperationAction(ISD::SDIV, Ty, Legal);
  setOperationAction(ISD::SREM, Ty, Legal);
  setOperationAction(ISD::SHL, Ty, Legal);
  setOperationAction(ISD::SRA, Ty, Legal);
  setOperationAction(ISD::SRL, Ty, Legal);
  setOperationAction(ISD::SUB, Ty, Legal);
  setOperationAction(ISD::SMAX, Ty, Legal);
  setOperationAction(ISD::SMIN, Ty, Legal);
  setOperationAction(ISD::UDIV, Ty, Legal);
  setOperationAction(ISD::UREM, Ty, Legal);
  setOperationAction(ISD::UMAX, Ty, Legal);
  setOperationAction(ISD::UMIN, Ty, Legal);
  setOperationAction(ISD::VECTOR_SHUFFLE, Ty, Custom);
  setOperationAction(ISD::VSELECT, Ty, Legal);
  setOperationAction(ISD::XOR, Ty, Legal);

  if (Ty == MVT::v4i32 || Ty == MVT::v2i64) {
    setOperationAction(ISD::FP_TO_SINT, Ty, Legal);
    setOperationAction(ISD::FP_TO_UINT, Ty, Legal);
    setOperationAction(ISD::SINT_TO_FP, Ty, Legal);
    setOperationAction(ISD::UINT_TO_FP, Ty, Legal);
  }

  setOperationAction(ISD::SETCC, Ty, Legal);
  setCondCodeAction(ISD::SETNE, Ty, Expand);
  setCondCodeAction(ISD::SETGE, Ty, Expand);
  setCondCodeAction(ISD::SETGT, Ty, Expand);
  setCondCodeAction(ISD::SETUGE, Ty, Expand);
  setCondCodeAction(ISD::SETUGT, Ty, Expand);
}

// Enable MSA support for the given floating-point type and Register class.
void MipsSETargetLowering::
addMSAFloatType(MVT::SimpleValueType Ty, const TargetRegisterClass *RC) {
  addRegisterClass(Ty, RC);

  // Expand all builtin opcodes.
  for (unsigned Opc = 0; Opc < ISD::BUILTIN_OP_END; ++Opc)
    setOperationAction(Opc, Ty, Expand);

  setOperationAction(ISD::LOAD, Ty, Legal);
  setOperationAction(ISD::STORE, Ty, Legal);
  setOperationAction(ISD::BITCAST, Ty, Legal);
  setOperationAction(ISD::EXTRACT_VECTOR_ELT, Ty, Legal);
  setOperationAction(ISD::INSERT_VECTOR_ELT, Ty, Legal);
  setOperationAction(ISD::BUILD_VECTOR, Ty, Custom);
  setOperationAction(ISD::UNDEF, Ty, Legal);

  if (Ty != MVT::v8f16) {
    setOperationAction(ISD::FABS,  Ty, Legal);
    setOperationAction(ISD::FADD,  Ty, Legal);
    setOperationAction(ISD::FDIV,  Ty, Legal);
    setOperationAction(ISD::FEXP2, Ty, Legal);
    setOperationAction(ISD::FLOG2, Ty, Legal);
    setOperationAction(ISD::FMA,   Ty, Legal);
    setOperationAction(ISD::FMUL,  Ty, Legal);
    setOperationAction(ISD::FRINT, Ty, Legal);
    setOperationAction(ISD::FSQRT, Ty, Legal);
    setOperationAction(ISD::FSUB,  Ty, Legal);
    setOperationAction(ISD::VSELECT, Ty, Legal);

    setOperationAction(ISD::SETCC, Ty, Legal);
    setCondCodeAction(ISD::SETOGE, Ty, Expand);
    setCondCodeAction(ISD::SETOGT, Ty, Expand);
    setCondCodeAction(ISD::SETUGE, Ty, Expand);
    setCondCodeAction(ISD::SETUGT, Ty, Expand);
    setCondCodeAction(ISD::SETGE,  Ty, Expand);
    setCondCodeAction(ISD::SETGT,  Ty, Expand);
  }
}

// Lower FP16_TO_FP (the soft-promote-half representation of an f16 -> f32/f64
// conversion).
SDValue MipsSETargetLowering::lowerFP16_TO_FP(SDValue Op,
                                              SelectionDAG &DAG) const {
  SDLoc DL(Op);
  EVT ResTy = Op.getValueType();
  assert((ResTy == MVT::f32 || ResTy == MVT::f64) && "Unexpected FP16_TO_FP");

  // The operand type is i32 because i16 isn't actually legal on MIPS.
  SDValue In = Op.getOperand(0);
  assert(In.getValueType() == MVT::i32 && "Unexpected FP16_TO_FP operand type");

  // Splat into a v8i16 (the 32-bit In value is truncated to the lower 16 bits).
  SDValue Splatted = DAG.getSplatBuildVector(MVT::v8i16, DL, In);

  // Bitcast from v8i16 to v8f16.
  SDValue HVec = DAG.getNode(ISD::BITCAST, DL, MVT::v8f16, Splatted);

  // Convert from v8f16 to v4f32.
  SDValue F32Vec = DAG.getNode(
      ISD::INTRINSIC_WO_CHAIN, DL, MVT::v4f32,
      DAG.getConstant(Intrinsic::mips_fexupr_w, DL, MVT::i32), HVec);
  SDValue Res;
  if (ResTy == MVT::f32) {
    // Every lane has the converted value, just read it from lane 0.
    Res = DAG.getNode(ISD::EXTRACT_VECTOR_ELT, DL, MVT::f32, F32Vec,
                      DAG.getVectorIdxConstant(0, DL));
  } else {
    // Convert from v4f32 to v2f64.
    SDValue F64Vec = DAG.getNode(
        ISD::INTRINSIC_WO_CHAIN, DL, MVT::v2f64,
        DAG.getConstant(Intrinsic::mips_fexupr_d, DL, MVT::i32), F32Vec);
    // Every lane has the converted value, just read it from lane 0.
    Res = DAG.getNode(ISD::EXTRACT_VECTOR_ELT, DL, MVT::f64, F64Vec,
                      DAG.getVectorIdxConstant(0, DL));
  }

  return Res;
}

// Lower FP_TO_FP16 (the soft-promote-half representation of an f32/f64 -> f16
// conversion)
SDValue MipsSETargetLowering::lowerFP_TO_FP16(SDValue Op,
                                              SelectionDAG &DAG) const {
  SDLoc DL(Op);
  EVT ResTy = Op.getValueType();
  SDValue In = Op.getOperand(0);
  assert((In.getValueType() == MVT::f32 || In.getValueType() == MVT::f64) &&
         "Unexpected FP_TO_FP16");

  SDValue F32Vec;
  if (In.getValueType() == MVT::f64) {
    // Splat f64 to v2f64, then convert to v4f32.
    SDValue F64Vec = DAG.getSplatBuildVector(MVT::v2f64, DL, In);
    F32Vec = DAG.getNode(ISD::INTRINSIC_WO_CHAIN, DL, MVT::v4f32,
                         DAG.getConstant(Intrinsic::mips_fexdo_w, DL, MVT::i32),
                         F64Vec, F64Vec);
  } else {
    // Splat f32 to v4f32.
    F32Vec = DAG.getSplatBuildVector(MVT::v4f32, DL, In);
  }

  // Then convert from v4f32 to v8f16.
  SDValue HVec = DAG.getNode(
      ISD::INTRINSIC_WO_CHAIN, DL, MVT::v8f16,
      DAG.getConstant(Intrinsic::mips_fexdo_h, DL, MVT::i32), F32Vec, F32Vec);

  // Finally cast to v8i16 (f16 is soft-promoted).
  SDValue IVec = DAG.getNode(ISD::BITCAST, DL, MVT::v8i16, HVec);
  SDValue Res = DAG.getNode(ISD::EXTRACT_VECTOR_ELT, DL, ResTy, IVec,
                            DAG.getVectorIdxConstant(0, DL));

  return Res;
}

bool MipsSETargetLowering::allowsMisalignedMemoryAccesses(
    EVT VT, unsigned, Align Alignment, MachineMemOperand::Flags,
    unsigned *Fast) const {
  MVT::SimpleValueType SVT = VT.getSimpleVT().SimpleTy;

  // R5900 LQ/SQ require 16-byte alignment to avoid TLB misses.
  // Reject unaligned accesses for 128-bit vector types.
  if (Subtarget.isR5900() && VT.isVector() && VT.getSizeInBits() == 128) {
    if (Alignment < Align(16))
      return false;
    // Aligned 128-bit accesses are fine
    if (Fast)
      *Fast = 1;
    return true;
  }

  if (Subtarget.systemSupportsUnalignedAccess()) {
    // MIPS32r6/MIPS64r6 is required to support unaligned access. It's
    // implementation defined whether this is handled by hardware, software, or
    // a hybrid of the two but it's expected that most implementations will
    // handle the majority of cases in hardware.
    if (Fast)
      *Fast = 1;
    return true;
  } else if (Subtarget.hasMips32r6()) {
    return false;
  }

  switch (SVT) {
  case MVT::i64:
  case MVT::i32:
    if (Fast)
      *Fast = 1;
    return true;
  default:
    return false;
  }
}

static SDValue lowerVU0SCALAR_TO_VECTOR(SDValue Op, SelectionDAG &DAG);

SDValue MipsSETargetLowering::LowerOperation(SDValue Op,
                                             SelectionDAG &DAG) const {
  switch(Op.getOpcode()) {
  case ISD::LOAD:  return lowerLOAD(Op, DAG);
  case ISD::STORE: return lowerSTORE(Op, DAG);
  case ISD::SMUL_LOHI: return lowerMulDiv(Op, MipsISD::Mult, true, true, DAG);
  case ISD::UMUL_LOHI: return lowerMulDiv(Op, MipsISD::Multu, true, true, DAG);
  case ISD::MULHS:     return lowerMulDiv(Op, MipsISD::Mult, false, true, DAG);
  case ISD::MULHU:     return lowerMulDiv(Op, MipsISD::Multu, false, true, DAG);
  case ISD::MUL:
    if (Subtarget.isR5900())
      break;
    return lowerMulDiv(Op, MipsISD::Mult, true, false, DAG);
  case ISD::SDIVREM:   return lowerMulDiv(Op, MipsISD::DivRem, true, true, DAG);
  case ISD::UDIVREM:   return lowerMulDiv(Op, MipsISD::DivRemU, true, true,
                                          DAG);
  case ISD::INTRINSIC_WO_CHAIN: return lowerINTRINSIC_WO_CHAIN(Op, DAG);
  case ISD::INTRINSIC_W_CHAIN:  return lowerINTRINSIC_W_CHAIN(Op, DAG);
  case ISD::INTRINSIC_VOID:     return lowerINTRINSIC_VOID(Op, DAG);
  case ISD::EXTRACT_VECTOR_ELT: return lowerEXTRACT_VECTOR_ELT(Op, DAG);
  case ISD::INSERT_VECTOR_ELT:
    return lowerINSERT_VECTOR_ELT(Op, DAG);
  case ISD::FMA:
    return lowerFMA(Op, DAG);
  case ISD::BUILD_VECTOR:       return lowerBUILD_VECTOR(Op, DAG);
  case ISD::SCALAR_TO_VECTOR:
    return lowerVU0SCALAR_TO_VECTOR(Op, DAG);
  case ISD::SETCC:
    if (Op.getValueType().isVector()) {
      assert(Subtarget.isR5900());
      return lowerR5900SETCC(Op, DAG);
    }
    break;
  case ISD::VSELECT:
    assert(Subtarget.isR5900());
    return lowerR5900VSELECT(Op, DAG);
  case ISD::VECTOR_SHUFFLE:
    return lowerVECTOR_SHUFFLE(Op, DAG);
  case ISD::FP16_TO_FP:
  case ISD::STRICT_FP16_TO_FP:
    return lowerFP16_TO_FP(Op, DAG);
  case ISD::FP_TO_FP16:
  case ISD::STRICT_FP_TO_FP16:
    return lowerFP_TO_FP16(Op, DAG);
  case ISD::BITCAST:            return lowerBITCAST(Op, DAG);
  case ISD::FP_TO_UINT:
    assert(Subtarget.hasVU0() && Op.getValueType() == MVT::v4i32);
    return lowerVU0FP_TO_UINT(Op, DAG);
  case ISD::CTLZ:
    return lowerCTLZ(Op, DAG);
  case ISD::SHL:
  case ISD::SRL:
  case ISD::SRA:
    return lowerVectorShift(Op, DAG);
  case ISD::FADD:
    return lowerR5900FPOp(Op, DAG, RTLIB::ADD_F32);
  case ISD::FSUB:
    return lowerR5900FPOp(Op, DAG, RTLIB::SUB_F32);
  case ISD::FMUL:
    return lowerR5900FPOp(Op, DAG, RTLIB::MUL_F32);
  case ISD::FDIV:
    if (Op.getValueType() == MVT::v4f32)
      return lowerVU0FDIV(Op, DAG);
    return lowerR5900FPOp(Op, DAG, RTLIB::DIV_F32);
  case ISD::FSQRT:
    return lowerR5900FPOp(Op, DAG, RTLIB::SQRT_F32);
  case ISD::FMINNUM:
  case ISD::FMAXNUM:
    // MIN.S and MAX.S without NaN, the libcall otherwise.
    assert(Subtarget.isR5900());
    return Op->getFlags().hasNoNaNs() ? Op : SDValue();
  }

  return MipsTargetLowering::LowerOperation(Op, DAG);
}

bool MipsSETargetLowering::shouldPushFreezeThroughOp(SDValue Op) const {
  if (!Subtarget.isR5900() || Op.getValueType() != MVT::f32)
    return true;
  // Keep the nnan and ninf flags that select the R5900 FPU instruction over
  // the libcall.
  switch (Op.getOpcode()) {
  case ISD::FADD:
  case ISD::FSUB:
  case ISD::FMUL:
  case ISD::FDIV:
  case ISD::FSQRT:
  case ISD::FMINNUM:
  case ISD::FMAXNUM:
    return !Op->getFlags().hasNoNaNs() && !Op->getFlags().hasNoInfs();
  default:
    return true;
  }
}

SDValue MipsSETargetLowering::lowerR5900FPOp(SDValue Op, SelectionDAG &DAG,
                                             RTLIB::Libcall LC) const {
  assert(Subtarget.isR5900());
  SDNodeFlags Flags = Op->getFlags();

  if (Flags.hasNoNaNs() && Flags.hasNoInfs()) {
    // Use the hardware FPU instruction if the operation is guaranteed to have
    // no NaN or infinity inputs/outputs (nnan+ninf flags).
    return Op;
  }

  // Fall back to a software libcall for IEEE correctness.
  SDLoc DL(Op);
  MVT VT = Op.getSimpleValueType();
  SmallVector<SDValue, 2> Ops(Op->op_begin(), Op->op_end());
  TargetLowering::MakeLibCallOptions CallOptions;
  auto [Result, Chain] = makeLibCall(DAG, LC, VT, Ops, CallOptions, DL);
  return Result;
}

// Fold zero extensions into MipsISD::VEXTRACT_[SZ]EXT_ELT
//
// Performs the following transformations:
// - Changes MipsISD::VEXTRACT_[SZ]EXT_ELT to zero extension if its
//   sign/zero-extension is completely overwritten by the new one performed by
//   the ISD::AND.
// - Removes redundant zero extensions performed by an ISD::AND.
static SDValue performANDCombine(SDNode *N, SelectionDAG &DAG,
                                 TargetLowering::DAGCombinerInfo &DCI,
                                 const MipsSubtarget &Subtarget) {
  if (!Subtarget.hasMSA())
    return SDValue();

  SDValue Op0 = N->getOperand(0);
  SDValue Op1 = N->getOperand(1);
  unsigned Op0Opcode = Op0->getOpcode();

  // (and (MipsVExtract[SZ]Ext $a, $b, $c), imm:$d)
  // where $d + 1 == 2^n and n == 32
  // or    $d + 1 == 2^n and n <= 32 and ZExt
  // -> (MipsVExtractZExt $a, $b, $c)
  if (Op0Opcode == MipsISD::VEXTRACT_SEXT_ELT ||
      Op0Opcode == MipsISD::VEXTRACT_ZEXT_ELT) {
    ConstantSDNode *Mask = dyn_cast<ConstantSDNode>(Op1);

    if (!Mask)
      return SDValue();

    int32_t Log2IfPositive = (Mask->getAPIntValue() + 1).exactLogBase2();

    if (Log2IfPositive <= 0)
      return SDValue(); // Mask+1 is not a power of 2

    SDValue Op0Op2 = Op0->getOperand(2);
    EVT ExtendTy = cast<VTSDNode>(Op0Op2)->getVT();
    unsigned ExtendTySize = ExtendTy.getSizeInBits();
    unsigned Log2 = Log2IfPositive;

    if ((Op0Opcode == MipsISD::VEXTRACT_ZEXT_ELT && Log2 >= ExtendTySize) ||
        Log2 == ExtendTySize) {
      SDValue Ops[] = { Op0->getOperand(0), Op0->getOperand(1), Op0Op2 };
      return DAG.getNode(MipsISD::VEXTRACT_ZEXT_ELT, SDLoc(Op0),
                         Op0->getVTList(),
                         ArrayRef(Ops, Op0->getNumOperands()));
    }
  }

  return SDValue();
}

// Determine if the specified node is a constant vector splat.
//
// Returns true and sets Imm if:
// * N is a ISD::BUILD_VECTOR representing a constant splat
//
// This function is quite similar to MipsSEDAGToDAGISel::selectVSplat. The
// differences are that it assumes the MSA has already been checked and the
// arbitrary requirement for a maximum of 32-bit integers isn't applied (and
// must not be in order for binsri.d to be selectable).
static bool isVSplat(SDValue N, APInt &Imm, bool IsLittleEndian) {
  BuildVectorSDNode *Node = dyn_cast<BuildVectorSDNode>(N.getNode());

  if (!Node)
    return false;

  APInt SplatValue, SplatUndef;
  unsigned SplatBitSize;
  bool HasAnyUndefs;

  if (!Node->isConstantSplat(SplatValue, SplatUndef, SplatBitSize, HasAnyUndefs,
                             8, !IsLittleEndian))
    return false;

  Imm = SplatValue;

  return true;
}

// Test whether the given node is an all-ones build_vector.
static bool isVectorAllOnes(SDValue N) {
  // Look through bitcasts. Endianness doesn't matter because we are looking
  // for an all-ones value.
  if (N->getOpcode() == ISD::BITCAST)
    N = N->getOperand(0);

  BuildVectorSDNode *BVN = dyn_cast<BuildVectorSDNode>(N);

  if (!BVN)
    return false;

  APInt SplatValue, SplatUndef;
  unsigned SplatBitSize;
  bool HasAnyUndefs;

  // Endianness doesn't matter in this context because we are looking for
  // an all-ones value.
  if (BVN->isConstantSplat(SplatValue, SplatUndef, SplatBitSize, HasAnyUndefs))
    return SplatValue.isAllOnes();

  return false;
}

// Test whether N is the bitwise inverse of OfNode.
static bool isBitwiseInverse(SDValue N, SDValue OfNode) {
  if (N->getOpcode() != ISD::XOR)
    return false;

  if (isVectorAllOnes(N->getOperand(0)))
    return N->getOperand(1) == OfNode;

  if (isVectorAllOnes(N->getOperand(1)))
    return N->getOperand(0) == OfNode;

  return false;
}

// Perform combines where ISD::OR is the root node.
//
// Performs the following transformations:
// - (or (and $a, $mask), (and $b, $inv_mask)) => (vselect $mask, $a, $b)
//   where $inv_mask is the bitwise inverse of $mask and the 'or' has a 128-bit
//   vector type.
static SDValue performORCombine(SDNode *N, SelectionDAG &DAG,
                                TargetLowering::DAGCombinerInfo &DCI,
                                const MipsSubtarget &Subtarget) {
  if (!Subtarget.hasMSA())
    return SDValue();

  EVT Ty = N->getValueType(0);

  if (!Ty.is128BitVector())
    return SDValue();

  SDValue Op0 = N->getOperand(0);
  SDValue Op1 = N->getOperand(1);

  if (Op0->getOpcode() == ISD::AND && Op1->getOpcode() == ISD::AND) {
    SDValue Op0Op0 = Op0->getOperand(0);
    SDValue Op0Op1 = Op0->getOperand(1);
    SDValue Op1Op0 = Op1->getOperand(0);
    SDValue Op1Op1 = Op1->getOperand(1);
    bool IsLittleEndian = !Subtarget.isLittle();

    SDValue IfSet, IfClr, Cond;
    bool IsConstantMask = false;
    APInt Mask, InvMask;

    // If Op0Op0 is an appropriate mask, try to find it's inverse in either
    // Op1Op0, or Op1Op1. Keep track of the Cond, IfSet, and IfClr nodes, while
    // looking.
    // IfClr will be set if we find a valid match.
    if (isVSplat(Op0Op0, Mask, IsLittleEndian)) {
      Cond = Op0Op0;
      IfSet = Op0Op1;

      if (isVSplat(Op1Op0, InvMask, IsLittleEndian) &&
          Mask.getBitWidth() == InvMask.getBitWidth() && Mask == ~InvMask)
        IfClr = Op1Op1;
      else if (isVSplat(Op1Op1, InvMask, IsLittleEndian) &&
               Mask.getBitWidth() == InvMask.getBitWidth() && Mask == ~InvMask)
        IfClr = Op1Op0;

      IsConstantMask = true;
    }

    // If IfClr is not yet set, and Op0Op1 is an appropriate mask, try the same
    // thing again using this mask.
    // IfClr will be set if we find a valid match.
    if (!IfClr.getNode() && isVSplat(Op0Op1, Mask, IsLittleEndian)) {
      Cond = Op0Op1;
      IfSet = Op0Op0;

      if (isVSplat(Op1Op0, InvMask, IsLittleEndian) &&
          Mask.getBitWidth() == InvMask.getBitWidth() && Mask == ~InvMask)
        IfClr = Op1Op1;
      else if (isVSplat(Op1Op1, InvMask, IsLittleEndian) &&
               Mask.getBitWidth() == InvMask.getBitWidth() && Mask == ~InvMask)
        IfClr = Op1Op0;

      IsConstantMask = true;
    }

    // If IfClr is not yet set, try looking for a non-constant match.
    // IfClr will be set if we find a valid match amongst the eight
    // possibilities.
    if (!IfClr.getNode()) {
      if (isBitwiseInverse(Op0Op0, Op1Op0)) {
        Cond = Op1Op0;
        IfSet = Op1Op1;
        IfClr = Op0Op1;
      } else if (isBitwiseInverse(Op0Op1, Op1Op0)) {
        Cond = Op1Op0;
        IfSet = Op1Op1;
        IfClr = Op0Op0;
      } else if (isBitwiseInverse(Op0Op0, Op1Op1)) {
        Cond = Op1Op1;
        IfSet = Op1Op0;
        IfClr = Op0Op1;
      } else if (isBitwiseInverse(Op0Op1, Op1Op1)) {
        Cond = Op1Op1;
        IfSet = Op1Op0;
        IfClr = Op0Op0;
      } else if (isBitwiseInverse(Op1Op0, Op0Op0)) {
        Cond = Op0Op0;
        IfSet = Op0Op1;
        IfClr = Op1Op1;
      } else if (isBitwiseInverse(Op1Op1, Op0Op0)) {
        Cond = Op0Op0;
        IfSet = Op0Op1;
        IfClr = Op1Op0;
      } else if (isBitwiseInverse(Op1Op0, Op0Op1)) {
        Cond = Op0Op1;
        IfSet = Op0Op0;
        IfClr = Op1Op1;
      } else if (isBitwiseInverse(Op1Op1, Op0Op1)) {
        Cond = Op0Op1;
        IfSet = Op0Op0;
        IfClr = Op1Op0;
      }
    }

    // At this point, IfClr will be set if we have a valid match.
    if (!IfClr.getNode())
      return SDValue();

    assert(Cond.getNode() && IfSet.getNode());

    // Fold degenerate cases.
    if (IsConstantMask) {
      if (Mask.isAllOnes())
        return IfSet;
      else if (Mask == 0)
        return IfClr;
    }

    // Transform the DAG into an equivalent VSELECT.
    return DAG.getNode(ISD::VSELECT, SDLoc(N), Ty, Cond, IfSet, IfClr);
  }

  return SDValue();
}

static cl::opt<unsigned> R5900MulMaxSteps(
    "mips-r5900-mul-max-steps", cl::Hidden, cl::init(4),
    cl::desc("The most shifts, adds and subtracts that replace an R5900 "
             "32-bit multiply by a constant"));

static bool shouldTransformMulToShiftsAddsSubs(APInt C, EVT VT,
                                               SelectionDAG &DAG,
                                               const MipsSubtarget &Subtarget) {
  // Estimate the number of operations the below transform will turn a
  // constant multiply into. The number is approximately equal to the minimal
  // number of powers of two that constant can be broken down to by adding
  // or subtracting them.
  //
  // If we have taken more than 12[1] / 8[2] steps to attempt the
  // optimization for a native sized value, it is more than likely that this
  // optimization will make things worse.
  //
  // [1] MIPS64 requires 6 instructions at most to materialize any constant,
  //     multiplication requires at least 4 cycles, but another cycle (or two)
  //     to retrieve the result from the HI/LO registers.
  //
  // [2] For MIPS32, more than 8 steps is expensive as the constant could be
  //     materialized in 2 instructions, multiplication requires at least 4
  //     cycles, but another cycle (or two) to retrieve the result from the
  //     HI/LO registers.
  //
  // TODO:
  // - MaxSteps needs to consider the `VT` of the constant for the current
  //   target.
  // - Consider to perform this optimization after type legalization.
  //   That allows to remove a workaround for types not supported natively.
  // - Take in account `-Os, -Oz` flags because this optimization
  //   increases code size.
  unsigned MaxSteps = Subtarget.isABI_O32() ? 8 : 12;
  // The R5900 MULT writes its GPR directly: latency 4, one every 2 cycles
  // (measured, bench uarch_tests.c). Two ALUs do 4 steps in that time.
  if (Subtarget.isR5900() && VT == MVT::i32)
    MaxSteps = R5900MulMaxSteps;

  SmallVector<APInt, 16> WorkStack(1, C);
  unsigned Steps = 0;
  unsigned BitWidth = C.getBitWidth();

  while (!WorkStack.empty()) {
    APInt Val = WorkStack.pop_back_val();

    if (Val == 0 || Val == 1)
      continue;

    if (Steps >= MaxSteps)
      return false;

    if (Val.isPowerOf2()) {
      ++Steps;
      continue;
    }

    APInt Floor = APInt(BitWidth, 1) << Val.logBase2();
    APInt Ceil = Val.isNegative() ? APInt(BitWidth, 0)
                                  : APInt(BitWidth, 1) << C.ceilLogBase2();
    if ((Val - Floor).ule(Ceil - Val)) {
      WorkStack.push_back(Floor);
      WorkStack.push_back(Val - Floor);
    } else {
      WorkStack.push_back(Ceil);
      WorkStack.push_back(Ceil - Val);
    }

    ++Steps;
  }

  // If the value being multiplied is not supported natively, we have to pay
  // an additional legalization cost, conservatively assume an increase in the
  // cost of 3 instructions per step. This values for this heuristic were
  // determined experimentally.
  unsigned RegisterSize = DAG.getTargetLoweringInfo()
                              .getRegisterType(*DAG.getContext(), VT)
                              .getSizeInBits();
  Steps *= (VT.getSizeInBits() != RegisterSize) * 3;
  if (Steps > 27)
    return false;

  return true;
}

static SDValue genConstMult(SDValue X, APInt C, const SDLoc &DL, EVT VT,
                            EVT ShiftTy, SelectionDAG &DAG) {
  // Return 0.
  if (C == 0)
    return DAG.getConstant(0, DL, VT);

  // Return x.
  if (C == 1)
    return X;

  // If c is power of 2, return (shl x, log2(c)).
  if (C.isPowerOf2())
    return DAG.getNode(ISD::SHL, DL, VT, X,
                       DAG.getConstant(C.logBase2(), DL, ShiftTy));

  unsigned BitWidth = C.getBitWidth();
  APInt Floor = APInt(BitWidth, 1) << C.logBase2();
  APInt Ceil = C.isNegative() ? APInt(BitWidth, 0) :
                                APInt(BitWidth, 1) << C.ceilLogBase2();

  // If |c - floor_c| <= |c - ceil_c|,
  // where floor_c = pow(2, floor(log2(c))) and ceil_c = pow(2, ceil(log2(c))),
  // return (add constMult(x, floor_c), constMult(x, c - floor_c)).
  if ((C - Floor).ule(Ceil - C)) {
    SDValue Op0 = genConstMult(X, Floor, DL, VT, ShiftTy, DAG);
    SDValue Op1 = genConstMult(X, C - Floor, DL, VT, ShiftTy, DAG);
    return DAG.getNode(ISD::ADD, DL, VT, Op0, Op1);
  }

  // If |c - floor_c| > |c - ceil_c|,
  // return (sub constMult(x, ceil_c), constMult(x, ceil_c - c)).
  SDValue Op0 = genConstMult(X, Ceil, DL, VT, ShiftTy, DAG);
  SDValue Op1 = genConstMult(X, Ceil - C, DL, VT, ShiftTy, DAG);
  return DAG.getNode(ISD::SUB, DL, VT, Op0, Op1);
}

static SDValue performMULCombine(SDNode *N, SelectionDAG &DAG,
                                 const TargetLowering::DAGCombinerInfo &DCI,
                                 const MipsSETargetLowering *TL,
                                 const MipsSubtarget &Subtarget) {
  EVT VT = N->getValueType(0);

  if (ConstantSDNode *C = dyn_cast<ConstantSDNode>(N->getOperand(1)))
    if (!VT.isVector() && shouldTransformMulToShiftsAddsSubs(
                              C->getAPIntValue(), VT, DAG, Subtarget))
      return genConstMult(N->getOperand(0), C->getAPIntValue(), SDLoc(N), VT,
                          TL->getScalarShiftAmountTy(DAG.getDataLayout(), VT),
                          DAG);

  return SDValue(N, 0);
}

static SDValue performDSPShiftCombine(unsigned Opc, SDNode *N, EVT Ty,
                                      SelectionDAG &DAG,
                                      const MipsSubtarget &Subtarget) {
  // See if this is a vector splat immediate node.
  APInt SplatValue, SplatUndef;
  unsigned SplatBitSize;
  bool HasAnyUndefs;
  unsigned EltSize = Ty.getScalarSizeInBits();
  BuildVectorSDNode *BV = dyn_cast<BuildVectorSDNode>(N->getOperand(1));

  if (!Subtarget.hasDSP())
    return SDValue();

  if (!BV ||
      !BV->isConstantSplat(SplatValue, SplatUndef, SplatBitSize, HasAnyUndefs,
                           EltSize, !Subtarget.isLittle()) ||
      (SplatBitSize != EltSize) ||
      (SplatValue.getZExtValue() >= EltSize))
    return SDValue();

  SDLoc DL(N);
  return DAG.getNode(Opc, DL, Ty, N->getOperand(0),
                     DAG.getConstant(SplatValue.getZExtValue(), DL, MVT::i32));
}

static SDValue performSHLCombine(SDNode *N, SelectionDAG &DAG,
                                 TargetLowering::DAGCombinerInfo &DCI,
                                 const MipsSubtarget &Subtarget) {
  EVT Ty = N->getValueType(0);

  if ((Ty != MVT::v2i16) && (Ty != MVT::v4i8))
    return SDValue();

  return performDSPShiftCombine(MipsISD::SHLL_DSP, N, Ty, DAG, Subtarget);
}

// Fold sign-extensions into MipsISD::VEXTRACT_[SZ]EXT_ELT for MSA and fold
// constant splats into MipsISD::SHRA_DSP for DSPr2.
//
// Performs the following transformations:
// - Changes MipsISD::VEXTRACT_[SZ]EXT_ELT to sign extension if its
//   sign/zero-extension is completely overwritten by the new one performed by
//   the ISD::SRA and ISD::SHL nodes.
// - Removes redundant sign extensions performed by an ISD::SRA and ISD::SHL
//   sequence.
//
// See performDSPShiftCombine for more information about the transformation
// used for DSPr2.
static SDValue performSRACombine(SDNode *N, SelectionDAG &DAG,
                                 TargetLowering::DAGCombinerInfo &DCI,
                                 const MipsSubtarget &Subtarget) {
  EVT Ty = N->getValueType(0);

  if (Subtarget.hasMSA()) {
    SDValue Op0 = N->getOperand(0);
    SDValue Op1 = N->getOperand(1);

    // (sra (shl (MipsVExtract[SZ]Ext $a, $b, $c), imm:$d), imm:$d)
    // where $d + sizeof($c) == 32
    // or    $d + sizeof($c) <= 32 and SExt
    // -> (MipsVExtractSExt $a, $b, $c)
    if (Op0->getOpcode() == ISD::SHL && Op1 == Op0->getOperand(1)) {
      SDValue Op0Op0 = Op0->getOperand(0);
      ConstantSDNode *ShAmount = dyn_cast<ConstantSDNode>(Op1);

      if (!ShAmount)
        return SDValue();

      if (Op0Op0->getOpcode() != MipsISD::VEXTRACT_SEXT_ELT &&
          Op0Op0->getOpcode() != MipsISD::VEXTRACT_ZEXT_ELT)
        return SDValue();

      EVT ExtendTy = cast<VTSDNode>(Op0Op0->getOperand(2))->getVT();
      unsigned TotalBits = ShAmount->getZExtValue() + ExtendTy.getSizeInBits();

      if (TotalBits == 32 ||
          (Op0Op0->getOpcode() == MipsISD::VEXTRACT_SEXT_ELT &&
           TotalBits <= 32)) {
        SDValue Ops[] = { Op0Op0->getOperand(0), Op0Op0->getOperand(1),
                          Op0Op0->getOperand(2) };
        return DAG.getNode(MipsISD::VEXTRACT_SEXT_ELT, SDLoc(Op0Op0),
                           Op0Op0->getVTList(),
                           ArrayRef(Ops, Op0Op0->getNumOperands()));
      }
    }
  }

  if ((Ty != MVT::v2i16) && ((Ty != MVT::v4i8) || !Subtarget.hasDSPR2()))
    return SDValue();

  return performDSPShiftCombine(MipsISD::SHRA_DSP, N, Ty, DAG, Subtarget);
}


static SDValue performSRLCombine(SDNode *N, SelectionDAG &DAG,
                                 TargetLowering::DAGCombinerInfo &DCI,
                                 const MipsSubtarget &Subtarget) {
  EVT Ty = N->getValueType(0);

  if (((Ty != MVT::v2i16) || !Subtarget.hasDSPR2()) && (Ty != MVT::v4i8))
    return SDValue();

  return performDSPShiftCombine(MipsISD::SHRL_DSP, N, Ty, DAG, Subtarget);
}

static bool isLegalDSPCondCode(EVT Ty, ISD::CondCode CC) {
  bool IsV216 = (Ty == MVT::v2i16);

  switch (CC) {
  case ISD::SETEQ:
  case ISD::SETNE:  return true;
  case ISD::SETLT:
  case ISD::SETLE:
  case ISD::SETGT:
  case ISD::SETGE:  return IsV216;
  case ISD::SETULT:
  case ISD::SETULE:
  case ISD::SETUGT:
  case ISD::SETUGE: return !IsV216;
  default:          return false;
  }
}

static SDValue performSETCCCombine(SDNode *N, SelectionDAG &DAG) {
  EVT Ty = N->getValueType(0);

  if ((Ty != MVT::v2i16) && (Ty != MVT::v4i8))
    return SDValue();

  if (!isLegalDSPCondCode(Ty, cast<CondCodeSDNode>(N->getOperand(2))->get()))
    return SDValue();

  return DAG.getNode(MipsISD::SETCC_DSP, SDLoc(N), Ty, N->getOperand(0),
                     N->getOperand(1), N->getOperand(2));
}

/// R5900: the v8i16 operand of PMULTH or PMADDH for the v8i32 multiply
/// operand \p X: the halfwords it is sign extended from, a splat of a
/// halfword, or its truncation when it fits in 16 bits. Null otherwise.
static SDValue narrowR5900PMULTHOperand(SDValue X, const SDLoc &DL,
                                        SelectionDAG &DAG) {
  if (X.getOpcode() == ISD::SIGN_EXTEND &&
      X.getOperand(0).getValueType() == MVT::v8i16)
    return X.getOperand(0);
  // A splat of a halfword.
  if (SDValue S = DAG.getSplatValue(X))
    return DAG.ComputeNumSignBits(S) > 16
               ? DAG.getSplatBuildVector(MVT::v8i16, DL,
                                         DAG.getSExtOrTrunc(S, DL, MVT::i16))
               : SDValue();
  if (DAG.ComputeNumSignBits(X) <= 16)
    return SDValue();
  return DAG.getNode(ISD::TRUNCATE, DL, MVT::v8i16, X);
}

/// R5900: the v8i32 products or sums that PMULTH or PMADDH \p Mul leaves in
/// HI and LO: 0, 1, 4 and 5 in LO and 2, 3, 6 and 7 in HI, so PCPYLD and
/// PCPYUD of LO and HI give them in order. PMFLO and PMFHI are glued to
/// \p Mul, so nothing else writes HI and LO in between.
static SDValue readR5900PMULTHResult(SDNode *Mul, const SDLoc &DL,
                                     SelectionDAG &DAG) {
  SDNode *Lo = DAG.getMachineNode(
      Mips::PMFLO, DL, DAG.getVTList(MVT::v4i32, MVT::Glue), SDValue(Mul, 1));
  SDNode *Hi = DAG.getMachineNode(Mips::PMFHI, DL, MVT::v4i32, SDValue(Lo, 1));
  auto Copy = [&](unsigned Opc, SDNode *Rs, SDNode *Rt) {
    return SDValue(
        DAG.getMachineNode(Opc, DL, MVT::v4i32, SDValue(Rs, 0), SDValue(Rt, 0)),
        0);
  };
  return DAG.getNode(ISD::CONCAT_VECTORS, DL, MVT::v8i32,
                     Copy(Mips::PCPYLD, Hi, Lo), Copy(Mips::PCPYUD, Lo, Hi));
}

/// R5900: (v8i32 (mul A, B)) of values that fit in 16 bits to PMULTH, which
/// multiplies 8 pairs of halfwords.
static SDValue performR5900MULCombine(SDNode *N, SelectionDAG &DAG,
                                      const MipsSubtarget &Subtarget) {
  if (!Subtarget.isR5900() || N->getValueType(0) != MVT::v8i32)
    return SDValue();
  SDLoc DL(N);
  SDValue A = narrowR5900PMULTHOperand(N->getOperand(0), DL, DAG);
  SDValue B =
      A ? narrowR5900PMULTHOperand(N->getOperand(1), DL, DAG) : SDValue();
  if (!B)
    return SDValue();
  SDValue MulOps[] = {A, B};
  SDNode *Mul = DAG.getMachineNode(
      Mips::PMULTH, DL, DAG.getVTList(MVT::v8i16, MVT::Glue), MulOps);
  return readR5900PMULTHResult(Mul, DL, DAG);
}

/// R5900: a tree of i32 adds with two or more multiplies among its terms to
/// MULT of the first and MADD of the others, which accumulate in HI and LO
/// (an ACC64 value), and adds of the other terms. This saves an add per
/// product, and the products wait for each other in the multiplier (MADD
/// reads the sum 2 cycles late) instead of in adds.
static SDValue performR5900MulAddCombine(SDNode *N, SelectionDAG &DAG,
                                         TargetLowering::DAGCombinerInfo &DCI,
                                         const MipsSubtarget &Subtarget) {
  // After legalization, as instruction selection: multiplies by constants
  // are shifts and adds by then.
  if (!Subtarget.isR5900() || N->getValueType(0) != MVT::i32 ||
      !DCI.isAfterLegalizeDAG())
    return SDValue();
  SumTerms S;
  collectSumTerms(
      SDValue(N, 0),
      [&](SDValue V) -> std::optional<bool> {
        if (V.getOpcode() == ISD::ADD && (V.getNode() == N || V.hasOneUse()))
          return false;
        return std::nullopt;
      },
      /*Flatten=*/true, S);
  SmallVector<SDValue, 8> Products, Others;
  for (const SumTerm &T : S.Terms)
    (T.V.getOpcode() == ISD::MUL && T.V.hasOneUse() ? Products : Others)
        .push_back(T.V);
  if (Products.size() < 2)
    return SDValue();

  SDLoc DL(N);
  SDNode *Acc = nullptr;
  for (SDValue Mul : Products) {
    SmallVector<SDValue, 3> Ops = {Mul.getOperand(0), Mul.getOperand(1)};
    if (Acc)
      Ops.push_back(SDValue(Acc, 1));
    Acc = DAG.getMachineNode(Acc ? Mips::PseudoR5900MADDAcc
                                 : Mips::PseudoR5900MULTAcc,
                             DL, DAG.getVTList(MVT::i32, MVT::Untyped), Ops);
  }
  SDValue Sum(Acc, 0);
  for (SDValue Other : Others)
    Sum = DAG.getNode(ISD::ADD, DL, MVT::i32, Sum, Other);
  return Sum;
}

/// R5900: a tree of v8i32 adds with two or more multiplies of values that fit
/// in 16 bits among its terms to PMULTH of the first and PMADDH of the
/// others, which accumulate in HI and LO, and adds of the other terms. The
/// combiner visits the root of the tree before its operands.
static SDValue performR5900ADDCombine(SDNode *N, SelectionDAG &DAG,
                                      const MipsSubtarget &Subtarget) {
  if (!Subtarget.isR5900() || N->getValueType(0) != MVT::v8i32)
    return SDValue();
  SDLoc DL(N);
  SumTerms S;
  collectSumTerms(
      SDValue(N, 0),
      [&](SDValue V) -> std::optional<bool> {
        if (V.getOpcode() == ISD::ADD && (V.getNode() == N || V.hasOneUse()))
          return false;
        return std::nullopt;
      },
      /*Flatten=*/true, S);
  SmallVector<std::pair<SDValue, SDValue>, 32> Products;
  SmallVector<SDValue, 4> Others;
  for (const SumTerm &T : S.Terms) {
    SDValue V = T.V;
    if (V.getOpcode() == ISD::MUL && V.hasOneUse())
      if (SDValue A = narrowR5900PMULTHOperand(V.getOperand(0), DL, DAG))
        if (SDValue B = narrowR5900PMULTHOperand(V.getOperand(1), DL, DAG)) {
          Products.push_back({A, B});
          continue;
        }
    Others.push_back(V);
  }
  if (Products.size() < 2)
    return SDValue();
  SDNode *Acc = nullptr;
  for (auto [A, B] : Products) {
    SmallVector<SDValue, 3> Ops = {A, B};
    if (Acc)
      Ops.push_back(SDValue(Acc, 1));
    Acc = DAG.getMachineNode(Acc ? Mips::PMADDH : Mips::PMULTH, DL,
                             DAG.getVTList(MVT::v8i16, MVT::Glue), Ops);
  }
  SDValue Sum = readR5900PMULTHResult(Acc, DL, DAG);
  for (SDValue Other : Others)
    Sum = DAG.getNode(ISD::ADD, DL, MVT::v8i32, Sum, Other);
  return Sum;
}

/// Extend the 128-bit vector \p Src to elements twice as wide with R5900 MMI
/// PEXTLH/PEXTUH (halfwords) or PEXTLB/PEXTUB (bytes): they interleave the
/// low or high half of two registers, here of \p Src and zero (zero
/// extension) or itself followed by an arithmetic shift (sign extension).
/// Returns the low and high halves of the result.
static std::pair<SDValue, SDValue>
extendR5900MMI(SDValue Src, bool Signed, const SDLoc &DL, SelectionDAG &DAG) {
  MVT VT = Src.getSimpleValueType();
  bool Half = VT == MVT::v8i16;
  MVT WideVT = Half ? MVT::v4i32 : MVT::v8i16;
  // $zero is zero in all 128 bits.
  SDValue Other = Signed ? Src : DAG.getRegister(Mips::R0_128, VT);
  auto Interleave = [&](unsigned Opc) {
    SDValue V = DAG.getBitcast(
        WideVT, SDValue(DAG.getMachineNode(Opc, DL, VT, Other, Src), 0));
    if (!Signed)
      return V;
    return DAG.getNode(ISD::SRA, DL, WideVT, V,
                       DAG.getConstant(VT.getScalarSizeInBits(), DL, WideVT));
  };
  return {Interleave(Half ? Mips::PEXTLH : Mips::PEXTLB),
          Interleave(Half ? Mips::PEXTUH : Mips::PEXTUB)};
}

/// R5900: a v4i8 or v4i16 (the low word or doubleword of a GPR) extended to
/// v4i32 by interleaving it with $zero (PEXTLB, PEXTLH). For a sign extension
/// the elements go to the high bytes, then PSRAW.
static SDValue extendR5900Narrow(SDValue Src, bool Signed, const SDLoc &DL,
                                 SelectionDAG &DAG) {
  bool Bytes = Src.getValueType() == MVT::v4i8;
  MVT VT = Bytes ? MVT::v16i8 : MVT::v8i16;
  SDValue Bits = DAG.getAnyExtOrTrunc(
      DAG.getBitcast(Bytes ? MVT::i32 : MVT::i64, Src), DL, MVT::i64);
  SDValue V = DAG.getTargetInsertSubreg(
      Mips::sub_64, DL, VT,
      SDValue(DAG.getMachineNode(TargetOpcode::IMPLICIT_DEF, DL, VT), 0), Bits);
  auto Interleave = [&](unsigned Opc, MVT VT, SDValue V) {
    SDValue Zero = DAG.getRegister(Mips::R0_128, VT);
    return SDValue(
        DAG.getMachineNode(Opc, DL, VT, Signed ? V : Zero, Signed ? Zero : V),
        0);
  };
  if (Bytes)
    V = DAG.getBitcast(MVT::v8i16, Interleave(Mips::PEXTLB, MVT::v16i8, V));
  V = DAG.getBitcast(MVT::v4i32, Interleave(Mips::PEXTLH, MVT::v8i16, V));
  if (Signed)
    V = DAG.getNode(ISD::SRA, DL, MVT::v4i32, V,
                    DAG.getConstant(Bytes ? 24 : 16, DL, MVT::v4i32));
  return V;
}

/// R5900: sign, zero or any extension of v8i16 or v16i8 to elements two or
/// four times as wide, or of v4i8 or v4i16 to v4i32, before type legalization
/// splits it through the stack.
static SDValue performR5900ExtendCombine(SDNode *N, SelectionDAG &DAG,
                                         const MipsSubtarget &Subtarget) {
  SDValue Src = N->getOperand(0);
  EVT VT = N->getValueType(0);
  EVT SrcVT = Src.getValueType();
  bool Signed = N->getOpcode() == ISD::SIGN_EXTEND;
  SDLoc DL(N);
  if (Subtarget.isR5900() && VT == MVT::v4i32 &&
      (SrcVT == MVT::v4i8 || SrcVT == MVT::v4i16))
    return extendR5900Narrow(Src, Signed, DL, DAG);
  if (!Subtarget.isR5900() || (SrcVT != MVT::v8i16 && SrcVT != MVT::v16i8) ||
      !VT.isSimple() || VT.getScalarSizeInBits() > 32)
    return SDValue();
  auto [Lo, Hi] = extendR5900MMI(Src, Signed, DL, DAG);
  if (Lo.getValueType().getScalarSizeInBits() == VT.getScalarSizeInBits())
    return DAG.getNode(ISD::CONCAT_VECTORS, DL, VT, Lo, Hi);
  // Bytes to words: extend each half again.
  auto [LoLo, LoHi] = extendR5900MMI(Lo, Signed, DL, DAG);
  auto [HiLo, HiHi] = extendR5900MMI(Hi, Signed, DL, DAG);
  return DAG.getNode(ISD::CONCAT_VECTORS, DL, VT, LoLo, LoHi, HiLo, HiHi);
}

/// Truncate \p Src to the 128-bit vector type \p VT with R5900 MMI PPACH and
/// PPACB, which pack the low halfwords or bytes of the elements of two
/// registers. A source with elements of more than twice the width is
/// truncated in steps.
static SDValue truncateR5900MMI(SDValue Src, EVT VT, const SDLoc &DL,
                                SelectionDAG &DAG) {
  unsigned Bits = VT.getScalarSizeInBits();
  auto [Lo, Hi] = DAG.SplitVector(Src, DL);
  if (Src.getValueType().getScalarSizeInBits() > 2 * Bits) {
    EVT MidVT = EVT::getVectorVT(*DAG.getContext(), MVT::getIntegerVT(2 * Bits),
                                 VT.getVectorNumElements() / 2);
    Lo = truncateR5900MMI(Lo, MidVT, DL, DAG);
    Hi = truncateR5900MMI(Hi, MidVT, DL, DAG);
  }
  return DAG.getNode(Bits == 16 ? MipsISD::PPACH : MipsISD::PPACB, DL, VT, Hi,
                     Lo);
}

/// R5900: (v8i16 (trunc v8i32)), (v16i8 (trunc v16i16 or v16i32)) and
/// (v4i16 or v4i8 (trunc v4i32)) to PPACH/PPACB, before type legalization
/// splits the source through the stack.
static SDValue performR5900TRUNCATECombine(SDNode *N, SelectionDAG &DAG,
                                           const MipsSubtarget &Subtarget) {
  EVT VT = N->getValueType(0);
  SDValue Src = N->getOperand(0);
  EVT SrcVT = Src.getValueType();
  SDLoc DL(N);
  // v4i32 to v4i16 or v4i8: the low doubleword or word of PPACH (and PPACB).
  if (Subtarget.isR5900() && SrcVT == MVT::v4i32 &&
      (VT == MVT::v4i16 || VT == MVT::v4i8)) {
    SDValue V = DAG.getNode(MipsISD::PPACH, DL, MVT::v8i16, Src, Src);
    if (VT == MVT::v4i8)
      V = DAG.getNode(MipsISD::PPACB, DL, MVT::v16i8, V, V);
    SDValue Bits = DAG.getTargetExtractSubreg(Mips::sub_64, DL, MVT::i64, V);
    return DAG.getBitcast(
        VT,
        DAG.getZExtOrTrunc(Bits, DL, VT == MVT::v4i8 ? MVT::i32 : MVT::i64));
  }
  if (!Subtarget.isR5900() || (VT != MVT::v8i16 && VT != MVT::v16i8) ||
      !SrcVT.isSimple() || SrcVT.getScalarSizeInBits() > 32)
    return SDValue();
  return truncateR5900MMI(Src, VT, DL, DAG);
}

static SDValue performVSELECTCombine(SDNode *N, SelectionDAG &DAG) {
  EVT Ty = N->getValueType(0);

  if (Ty == MVT::v2i16 || Ty == MVT::v4i8) {
    SDValue SetCC = N->getOperand(0);

    if (SetCC.getOpcode() != MipsISD::SETCC_DSP)
      return SDValue();

    return DAG.getNode(MipsISD::SELECT_CC_DSP, SDLoc(N), Ty,
                       SetCC.getOperand(0), SetCC.getOperand(1),
                       N->getOperand(1), N->getOperand(2), SetCC.getOperand(2));
  }

  return SDValue();
}

static SDValue performXORCombine(SDNode *N, SelectionDAG &DAG,
                                 const MipsSubtarget &Subtarget) {
  EVT Ty = N->getValueType(0);

  if (Subtarget.hasMSA() && Ty.is128BitVector() && Ty.isInteger()) {
    // Try the following combines:
    //   (xor (or $a, $b), (build_vector allones))
    //   (xor (or $a, $b), (bitcast (build_vector allones)))
    SDValue Op0 = N->getOperand(0);
    SDValue Op1 = N->getOperand(1);
    SDValue NotOp;

    if (ISD::isBuildVectorAllOnes(Op0.getNode()))
      NotOp = Op1;
    else if (ISD::isBuildVectorAllOnes(Op1.getNode()))
      NotOp = Op0;
    else
      return SDValue();

    if (NotOp->getOpcode() == ISD::OR)
      return DAG.getNode(MipsISD::VNOR, SDLoc(N), Ty, NotOp->getOperand(0),
                         NotOp->getOperand(1));
  }

  return SDValue();
}

// Convert (fp_to_uint (fp16_to_fp x)) into (fp_to_sint (fp16_to_fp x)).
static SDValue performFP_TO_UINTCombine(SDNode *N, SelectionDAG &DAG) {
  SDValue Src = N->getOperand(0);
  EVT VT = N->getValueType(0);

  // Use a trick from TargetLowering::expandFP_TO_UINT: we know that every
  // integer value that can be represented by f16 is <= 65504, i.e. a signed
  // integer of 17 bits or more can represent all values and fptoui and fptosi
  // are equivalent.
  //
  // NOTE: the result of fptoui is poison when the value does not fit in the
  // destination type (e.g. because it is negative).
  if (Src.getOpcode() != ISD::FP16_TO_FP || VT.getScalarSizeInBits() < 17)
    return SDValue();
  return DAG.getNode(ISD::FP_TO_SINT, SDLoc(N), VT, Src);
}

// VU0 Load Optimization for BITCAST
// Optimize: bitcast(v4f32 load) -> v4i32 load
// When a v4f32 is loaded (LQC2) and immediately bitcast to v4i32 (QMFC2),
// we can load directly to GPR128 (LQ) avoiding the VF register round-trip.
static bool isR5900MMIType(EVT VT) {
  return VT == MVT::v4i32 || VT == MVT::v8i16 || VT == MVT::v16i8;
}

/// R5900: a 128-bit vector of two doublewords, such as an argument in a pair
/// of GPRs, is PCPYLD of them. v2i64 is not a legal type, and the vector would
/// be built in a stack temporary otherwise.
static SDValue performR5900BITCASTCombine(SDNode *N, SelectionDAG &DAG,
                                          const MipsSubtarget &Subtarget) {
  EVT VT = N->getValueType(0);
  SDValue Src = N->getOperand(0);
  if (!Subtarget.isR5900() || !isR5900MMIType(VT) ||
      Src.getOpcode() != ISD::BUILD_VECTOR || Src.getValueType() != MVT::v2i64)
    return SDValue();
  SDLoc DL(N);
  auto ToVector = [&](SDValue X) {
    return DAG.getTargetInsertSubreg(
        Mips::sub_64, DL, VT,
        SDValue(DAG.getMachineNode(TargetOpcode::IMPLICIT_DEF, DL, VT), 0), X);
  };
  SDValue Lo = ToVector(Src.getOperand(0));
  if (Src.getOperand(1).isUndef())
    return Lo;
  return SDValue(
      DAG.getMachineNode(Mips::PCPYLD, DL, VT, ToVector(Src.getOperand(1)), Lo),
      0);
}

/// R5900: a doubleword of a 128-bit vector, such as a result in a pair of
/// GPRs, is the low doubleword of the register, or that of PCPYUD for the high
/// one.
static SDValue
performR5900EXTRACT_VECTOR_ELTCombine(SDNode *N, SelectionDAG &DAG,
                                      const MipsSubtarget &Subtarget) {
  SDValue Src = N->getOperand(0);
  auto *Idx = dyn_cast<ConstantSDNode>(N->getOperand(1));
  if (!Subtarget.isR5900() || N->getValueType(0) != MVT::i64 ||
      Src.getValueType() != MVT::v2i64 || Src.getOpcode() != ISD::BITCAST ||
      !isR5900MMIType(Src.getOperand(0).getValueType()) || !Idx ||
      Idx->getZExtValue() > 1)
    return SDValue();
  SDLoc DL(N);
  SDValue V = Src.getOperand(0);
  if (Idx->getZExtValue() == 1)
    V = SDValue(DAG.getMachineNode(Mips::PCPYUD, DL, V.getValueType(), V, V),
                0);
  return DAG.getTargetExtractSubreg(Mips::sub_64, DL, MVT::i64, V);
}

static SDValue performBITCASTCombine(SDNode *N, SelectionDAG &DAG,
                                     const MipsSubtarget &Subtarget) {
  if (!Subtarget.hasVU0())
    return SDValue();

  EVT DstVT = N->getValueType(0);
  SDValue Src = N->getOperand(0);
  EVT SrcVT = Src.getValueType();

  // Only handle v4f32 -> v4i32 bitcast (the QMFC2 pattern)
  if (DstVT != MVT::v4i32 || SrcVT != MVT::v4f32)
    return SDValue();

  // Check if source is a load
  if (Src.getOpcode() != ISD::LOAD)
    return SDValue();

  LoadSDNode *Load = cast<LoadSDNode>(Src.getNode());

  // Don't transform if the load has multiple uses (someone needs the v4f32)
  if (!Src.hasOneUse())
    return SDValue();

  SDLoc DL(N);

  // Create a new load directly to v4i32 (GPR128)
  SDValue NewLoad =
      DAG.getLoad(MVT::v4i32, DL, Load->getChain(), Load->getBasePtr(),
                  Load->getPointerInfo(), Load->getAlign(),
                  Load->getMemOperand()->getFlags());

  // Replace the chain
  DAG.ReplaceAllUsesOfValueWith(SDValue(Load, 1), NewLoad.getValue(1));

  return NewLoad;
}

/// The store of field \p Idx of \p V, as f32, at \p Offset bytes from the
/// store \p St, or null.
static StoreSDNode *findVU0FieldStore(StoreSDNode *St, SDValue V, unsigned Idx,
                                      int64_t Offset, SelectionDAG &DAG) {
  BaseIndexOffset Base = BaseIndexOffset::match(St, DAG);
  for (SDNode *U : V->users()) {
    if (U->getOpcode() != ISD::EXTRACT_VECTOR_ELT ||
        U->getValueType(0) != MVT::f32 ||
        !isa<ConstantSDNode>(U->getOperand(1)) ||
        U->getConstantOperandVal(1) != Idx || !U->hasOneUse())
      continue;
    auto *Other = dyn_cast<StoreSDNode>(*U->user_begin());
    if (!Other || Other == St || !ISD::isNormalStore(Other) ||
        !Other->isSimple() || Other->getValue() != SDValue(U, 0))
      continue;
    int64_t Off;
    if (Base.equalBaseIndex(BaseIndexOffset::match(Other, DAG), DAG, Off) &&
        Off == Offset)
      return Other;
  }
  return nullptr;
}

/// VU0: a field of a vector stored as f32 is stored with SW from the GPR that
/// QMFC2 moves the vector to (PCPYUD for z and w, DSRL for y and w), without
/// an MTC1 and an SWC1. Fields x and y, or z and w, stored next to each other
/// at an aligned address are one SD.
static SDValue performVU0STORECombine(SDNode *N, SelectionDAG &DAG,
                                      const MipsSubtarget &Subtarget) {
  auto *St = cast<StoreSDNode>(N);
  SDValue Val = St->getValue();
  if (!Subtarget.hasVU0() || !ISD::isNormalStore(St) || !St->isSimple() ||
      Val.getValueType() != MVT::f32 ||
      Val.getOpcode() != ISD::EXTRACT_VECTOR_ELT ||
      Val.getOperand(0).getValueType() != MVT::v4f32 ||
      !isa<ConstantSDNode>(Val.getOperand(1)) ||
      Val.getConstantOperandVal(1) > 3 || !Val.hasOneUse())
    return SDValue();
  SDLoc DL(N);
  unsigned Idx = Val.getConstantOperandVal(1);
  SDValue V = DAG.getBitcast(MVT::v4i32, Val.getOperand(0));
  if (Idx >= 2)
    V = SDValue(DAG.getMachineNode(Mips::PCPYUD, DL, MVT::v4i32, V, V), 0);
  SDValue Dword = DAG.getTargetExtractSubreg(Mips::sub_64, DL, MVT::i64, V);

  // The other field of the doubleword, stored next to this one (whichever of
  // the two stores is combined first).
  bool High = Idx & 1;
  if (StoreSDNode *Other = findVU0FieldStore(St, Val.getOperand(0), Idx ^ 1,
                                             High ? -4 : 4, DAG)) {
    StoreSDNode *Lo = High ? Other : St, *Hi = High ? St : Other;
    // After the chains of both, when one store is not on the other's chain
    // further back.
    SDValue Chain;
    if (Hi->getChain() == SDValue(Lo, 0) || Hi->getChain() == Lo->getChain())
      Chain = Lo->getChain();
    else if (Lo->getChain() == SDValue(Hi, 0))
      Chain = Hi->getChain();
    else if (!Lo->isPredecessorOf(Hi) && !Hi->isPredecessorOf(Lo))
      Chain = DAG.getNode(ISD::TokenFactor, DL, MVT::Other, Lo->getChain(),
                          Hi->getChain());
    if (Chain && Lo->getAlign() >= 8) {
      SDValue Pair = DAG.getStore(
          Chain, DL, Dword, Lo->getBasePtr(), Lo->getPointerInfo(),
          Lo->getAlign(), Lo->getMemOperand()->getFlags(), Lo->getAAInfo());
      DAG.ReplaceAllUsesOfValueWith(SDValue(Other, 0), Pair);
      return Pair;
    }
  }

  if (High)
    Dword = DAG.getNode(ISD::SRL, DL, MVT::i64, Dword,
                        DAG.getConstant(32, DL, MVT::i32));
  return DAG.getTruncStore(St->getChain(), DL, Dword, St->getBasePtr(),
                           MVT::i32, St->getMemOperand());
}

/// Broadcast field \p Field of \p V to all four fields.
static SDValue getVU0Broadcast(SelectionDAG &DAG, const SDLoc &DL, SDValue V,
                               unsigned Field) {
  return DAG.getNode(MipsISD::VU0_BCAST, DL, MVT::v4f32, V,
                     DAG.getTargetConstant(Field, DL, MVT::i32));
}

/// Whether \p V is a VU0_BCAST operand of a VU0 operation.
static bool isVU0Broadcast(SDValue V) {
  return V.getOpcode() == MipsISD::VU0_BCAST;
}

/// Returns n if \p C is a splat of 2^n, or of 2^-n if \p Inverse, for the
/// fraction bits of the VU0 fixed-point conversions, or 0.
static unsigned getVU0FractionBits(SDValue C, bool Inverse) {
  ConstantFPSDNode *CN = isConstOrConstSplatFP(C);
  if (!CN)
    return 0;
  for (int N : {4, 12, 15})
    if (CN->getValueAPF().isExactlyValue(std::ldexp(1.0, Inverse ? -N : N)))
      return N;
  return 0;
}

// VU0 fixed-point conversions with n = 4, 12 or 15 fraction bits:
//   fp_to_sint (v * 2^n) -> VU0_FTOI v, n
//   sint_to_fp (v) * 2^-n -> VU0_ITOF v, n
// The multiplications by a power of two are exact.
static SDValue performVU0FixedPointCombine(SDNode *N, SelectionDAG &DAG,
                                           const MipsSubtarget &Subtarget) {
  if (!Subtarget.hasVU0())
    return SDValue();
  SDLoc DL(N);
  if (N->getOpcode() == ISD::FP_TO_SINT) {
    SDValue Mul = N->getOperand(0);
    if (N->getValueType(0) != MVT::v4i32 || Mul.getOpcode() != ISD::FMUL ||
        !Mul.hasOneUse())
      return SDValue();
    for (unsigned I : {0, 1})
      if (unsigned Bits = getVU0FractionBits(Mul.getOperand(1 - I), false))
        return DAG.getNode(MipsISD::VU0_FTOI, DL, MVT::v4i32, Mul.getOperand(I),
                           DAG.getTargetConstant(Bits, DL, MVT::i32));
    return SDValue();
  }

  if (N->getValueType(0) != MVT::v4f32)
    return SDValue();
  for (unsigned I : {0, 1}) {
    SDValue Conv = N->getOperand(I);
    if (Conv.getOpcode() != ISD::SINT_TO_FP ||
        Conv.getOperand(0).getValueType() != MVT::v4i32)
      continue;
    if (unsigned Bits = getVU0FractionBits(N->getOperand(1 - I), true))
      return DAG.getNode(MipsISD::VU0_ITOF, DL, MVT::v4f32, Conv.getOperand(0),
                         DAG.getTargetConstant(Bits, DL, MVT::i32));
  }
  return SDValue();
}

/// VU0: (v4f32 (sint_to_fp or uint_to_fp v4i8 or v4i16)) converts the
/// elements extended to v4i32 (extendR5900Narrow) with VITOF0.
static SDValue performVU0IntToFPCombine(SDNode *N, SelectionDAG &DAG,
                                        const MipsSubtarget &Subtarget) {
  SDValue Src = N->getOperand(0);
  EVT SrcVT = Src.getValueType();
  if (!Subtarget.hasVU0() || N->getValueType(0) != MVT::v4f32 ||
      (SrcVT != MVT::v4i8 && SrcVT != MVT::v4i16))
    return SDValue();
  SDLoc DL(N);
  return DAG.getNode(
      ISD::SINT_TO_FP, DL, MVT::v4f32,
      extendR5900Narrow(Src, N->getOpcode() == ISD::SINT_TO_FP, DL, DAG));
}

// VU0 Broadcast Combine
// Recognize: fop(A, shuffle(B, <N,N,N,N>)) -> fop(A, VU0_BCAST(B, N)), and
// fop(A, splat(s)) -> fop(A, VU0_BCAST(scalar_to_vector(s), 0)), for fadd,
// fsub, fmul, fmaxnum and fminnum, which is selected as the broadcast form
// (VMULx ...). This enables efficient matrix-vector operations using VMULbc
// instructions, and horizontal sums using VADDbc instructions.
static SDValue performVU0BroadcastCombine(SDNode *N, SelectionDAG &DAG,
                                          const MipsSubtarget &Subtarget) {
  if (!Subtarget.hasVU0() || N->getValueType(0) != MVT::v4f32)
    return SDValue();

  SDLoc DL(N);

  // Check both operands for shuffle pattern (all but fsub are commutative,
  // fsub only takes the broadcast as its second operand)
  bool Commutative = N->getOpcode() != ISD::FSUB;
  for (int i = 0; i < (Commutative ? 2 : 1); i++) {
    SDValue Vec = N->getOperand(i);
    SDValue Shuf = N->getOperand(1 - i);

    // A splat of a scalar that is not constant: only field x is read, which
    // SCALAR_TO_VECTOR sets with a single QMTC2.
    if (auto *BV = dyn_cast<BuildVectorSDNode>(Shuf)) {
      SDValue S = BV->getSplatValue();
      if (!S || BV->isConstant())
        continue;
      SDValue BC = getVU0Broadcast(
          DAG, DL, DAG.getNode(ISD::SCALAR_TO_VECTOR, DL, MVT::v4f32, S), 0);
      return DAG.getNode(N->getOpcode(), DL, MVT::v4f32, i ? BC : Vec,
                         i ? Vec : BC, N->getFlags());
    }

    if (Shuf.getOpcode() != ISD::VECTOR_SHUFFLE)
      continue;

    ShuffleVectorSDNode *SVN = cast<ShuffleVectorSDNode>(Shuf.getNode());
    ArrayRef<int> Mask = SVN->getMask();

    // Check for broadcast: all non-undef mask elements are the same (0-3)
    int BroadcastIdx = -1;
    bool IsBroadcast = true;
    for (int M : Mask) {
      if (M < 0)
        continue; // undef
      if (BroadcastIdx < 0)
        BroadcastIdx = M;
      else if (M != BroadcastIdx) {
        IsBroadcast = false;
        break;
      }
    }

    if (IsBroadcast && BroadcastIdx >= 0 && BroadcastIdx < 4) {
      // We found a broadcast shuffle. Get the source vector.
      SDValue BroadcastSrc = SVN->getOperand(0);
      SDValue BC = getVU0Broadcast(DAG, DL, BroadcastSrc, BroadcastIdx);
      return DAG.getNode(N->getOpcode(), DL, MVT::v4f32, i ? BC : Vec,
                         i ? Vec : BC, N->getFlags());
    }
  }

  return SDValue();
}

static cl::opt<bool>
    VU0BroadcastPair("mips-vu0-bcast-pair", cl::Hidden, cl::init(true),
                     cl::desc("Do an R5900 VU0 operation on two broadcasts "
                              "on one field, broadcast"));

static cl::opt<bool> VU0LaneOp(
    "mips-vu0-lane-op", cl::Hidden, cl::init(true),
    cl::desc("Do a scalar operation on a field of an R5900 VU0 vector, whose "
             "result goes back into vectors, on the VU0"));

static bool isVU0BroadcastPairOp(unsigned Opc) {
  return Opc == ISD::FADD || Opc == ISD::FSUB || Opc == ISD::FMUL ||
         Opc == ISD::FMAXNUM || Opc == ISD::FMINNUM;
}

/// The field index of \p V if it is a shuffle that broadcasts one field of
/// its first operand, or -1.
static int getVU0SplatShuffleField(SDValue V) {
  auto *SVN = dyn_cast<ShuffleVectorSDNode>(V);
  if (!SVN || !SVN->isSplat())
    return -1;
  int F = SVN->getSplatIndex();
  return F >= 0 && F < 4 ? F : -1;
}

static SDValue performVU0BroadcastPairCombine(SDNode *N, SelectionDAG &DAG,
                                              const MipsSubtarget &Subtarget);

/// \p V as a VU0_BCAST: V itself, a broadcast shuffle, or an operation on two
/// broadcasts made into one. The combiner may visit an operation before its
/// operands.
static SDValue getVU0AsBroadcast(SDValue V, SelectionDAG &DAG,
                                 const MipsSubtarget &Subtarget) {
  if (isVU0Broadcast(V))
    return V;
  if (int F = getVU0SplatShuffleField(V); F >= 0)
    return getVU0Broadcast(DAG, SDLoc(V), V.getOperand(0), F);
  if (isVU0BroadcastPairOp(V.getOpcode()) && V.getValueType() == MVT::v4f32 &&
      V.hasOneUse())
    return performVU0BroadcastPairCombine(V.getNode(), DAG, Subtarget);
  return SDValue();
}

// VU0 Broadcast Pair Combine (combineBroadcastPair of the VU target)
// An operation on two broadcasts, or on a broadcast and a constant splat, is
// one on a field, broadcast:
//   op (bcast a, i), (bcast b, j)  ->  bcast (op a, (bcast b, j)), i
// The operation reads b.j by its broadcast field (VMULbc), and its users read
// field i the same way, instead of each broadcast being made on its own
// (VSUB + VADDbc).
static SDValue performVU0BroadcastPairCombine(SDNode *N, SelectionDAG &DAG,
                                              const MipsSubtarget &Subtarget) {
  if (!Subtarget.hasVU0() || !VU0BroadcastPair ||
      N->getValueType(0) != MVT::v4f32 || !isVU0BroadcastPairOp(N->getOpcode()))
    return SDValue();
  SDValue A = N->getOperand(0), B = N->getOperand(1);
  bool Commutes = N->getOpcode() != ISD::FSUB;
  auto IsSplat = [](SDValue V) { return isConstOrConstSplatFP(V) != nullptr; };
  SDValue BA = getVU0AsBroadcast(A, DAG, Subtarget);
  if (!BA && Commutes) {
    std::swap(A, B);
    BA = getVU0AsBroadcast(A, DAG, Subtarget);
  }
  if (!BA)
    return SDValue();
  SDValue BB = IsSplat(B) ? B : getVU0AsBroadcast(B, DAG, Subtarget);
  if (!BB)
    return SDValue();
  SDLoc DL(N);
  SDValue Op = DAG.getNode(N->getOpcode(), DL, MVT::v4f32, BA.getOperand(0), BB,
                           N->getFlags());
  return DAG.getNode(MipsISD::VU0_BCAST, DL, MVT::v4f32, Op, BA.getOperand(1));
}

// A broadcast of field j of a vector whose field j is field k of another is
// a broadcast of that field:
//   bcast (bcast v, k), j            ->  bcast v, k
//   bcast (scalar_to_vector v.k), 0  ->  bcast v, k
//   bcast (insert_vector_elt _, v.k, j), j  ->  bcast v, k
static SDValue performVU0BCASTCombine(SDNode *N, SelectionDAG &DAG) {
  SDValue V = N->getOperand(0);
  unsigned J = N->getConstantOperandVal(1);
  if (isVU0Broadcast(V))
    return V;
  SDValue E;
  if (V.getOpcode() == ISD::SCALAR_TO_VECTOR && J == 0)
    E = V.getOperand(0);
  else if (V.getOpcode() == ISD::INSERT_VECTOR_ELT &&
           isa<ConstantSDNode>(V.getOperand(2)) &&
           V.getConstantOperandVal(2) == J)
    E = V.getOperand(1);
  if (!E || E.getOpcode() != ISD::EXTRACT_VECTOR_ELT ||
      E.getOperand(0).getValueType() != MVT::v4f32 ||
      !isa<ConstantSDNode>(E.getOperand(1)) || E.getConstantOperandVal(1) >= 4)
    return SDValue();
  return getVU0Broadcast(DAG, SDLoc(N), E.getOperand(0),
                         E.getConstantOperandVal(1));
}

// VU0 Lane Op Combine (combineLaneOp of the VU target)
// A scalar operation on field k of a vector, whose result only goes back into
// vectors, is done on the whole vector with the other operand broadcast,
// instead of moving the field to the FPU and back:
//   splat (max v.x, 0)  ->  splat (VMAXx v, vf0).x
// A division is one through Q (VDIV), which its users read directly
// (VUFoldQ of the VU target; MipsSEDAGToDAGISel::trySelect):
//   v * splat (1 / v.w)  ->  VDIV Q, vf0w, v.w; VWAITQ; VMULq v, Q

static bool isVU0LaneOpOpcode(unsigned Opc) {
  return isVU0BroadcastPairOp(Opc) || Opc == ISD::FDIV;
}

static bool isVU0Lane(SDValue V) {
  return V.getOpcode() == ISD::EXTRACT_VECTOR_ELT &&
         V.getOperand(0).getValueType() == MVT::v4f32 &&
         isa<ConstantSDNode>(V.getOperand(1)) && V.getConstantOperandVal(1) < 4;
}

/// A float loaded on its own goes to the VU0 through a GPR (LW, QMTC2).
static bool isVU0ScalarLoad(SDValue V) {
  auto *Ld = dyn_cast<LoadSDNode>(V);
  return Ld && Ld->isSimple() && ISD::isNormalLoad(Ld) && V.hasOneUse();
}

/// Whether a VU0 operation reads \p V as a field (getVU0FieldOf).
static bool isVU0FieldOperand(SDValue V) {
  return isa<ConstantFPSDNode>(V) || isVU0Lane(V) || isVU0ScalarLoad(V);
}

/// \p V as field Field of a vector: a field of a vector, 0.0 or 1.0 from VF0,
/// another constant splat, or a float loaded into a GPR.
static std::pair<SDValue, unsigned> getVU0FieldOf(SDValue V,
                                                  SelectionDAG &DAG) {
  SDLoc DL(V);
  if (isVU0Lane(V))
    return {V.getOperand(0), unsigned(V.getConstantOperandVal(1))};
  if (auto *C = dyn_cast<ConstantFPSDNode>(V)) {
    SDValue VF0 = DAG.getRegister(Mips::VF0, MVT::v4f32);
    if (C->isZero() && !C->isNegative())
      return {VF0, 0};
    if (C->isExactlyValue(1.0))
      return {VF0, 3};
    return {DAG.getSplatBuildVector(MVT::v4f32, DL, V), 0};
  }
  auto *Ld = cast<LoadSDNode>(V);
  SDValue I = DAG.getLoad(MVT::i32, DL, Ld->getChain(), Ld->getBasePtr(),
                          Ld->getMemOperand());
  DAG.makeEquivalentMemoryOrdering(Ld, I);
  // The GPR as the low part of a GPR128: field x of the QMTC2.
  SDValue Q = DAG.getTargetInsertSubreg(
      Mips::sub_64, DL, MVT::v4i32,
      SDValue(DAG.getMachineNode(TargetOpcode::IMPLICIT_DEF, DL, MVT::v4i32),
              0),
      DAG.getNode(ISD::ANY_EXTEND, DL, MVT::i64, I));
  return {DAG.getBitcast(MVT::v4f32, Q), 0};
}

/// Whether the only use of \p N is a splat that one vector FMUL, FADD or
/// FSUB (as its second operand) reads, which then reads Q directly. Where Q
/// would be copied to a VF register instead, the one divider is slower than
/// DIV.S in parallel with the VU0 (Transform 4.52 -> 3.82 Mvert/s).
static bool isVU0QReadAsBroadcast(SDNode *N) {
  if (!N->hasOneUse())
    return false;
  // N in field x: SCALAR_TO_VECTOR, INSERT_VECTOR_ELT at 0, or BUILD_VECTOR
  // with N or undef in the other fields.
  SDNode *U = *N->user_begin();
  bool IsSplat = false;
  if (U->getOpcode() == ISD::BUILD_VECTOR) {
    if (U->getOperand(0).getNode() != N ||
        !all_of(U->op_values(),
                [&](SDValue V) { return V.getNode() == N || V.isUndef(); }))
      return false;
    IsSplat =
        all_of(U->op_values(), [&](SDValue V) { return V.getNode() == N; });
  } else if (U->getOpcode() != ISD::SCALAR_TO_VECTOR &&
             !(U->getOpcode() == ISD::INSERT_VECTOR_ELT &&
               isNullConstant(U->getOperand(2)) &&
               U->getOperand(1).getNode() == N)) {
    return false;
  }
  if (!U->hasOneUse())
    return false;
  // Broadcast of field x, unless already a splat.
  if (!IsSplat) {
    U = *U->user_begin();
    auto *SV = dyn_cast<ShuffleVectorSDNode>(U);
    if (!(SV && SV->isSplat() && SV->getSplatIndex() == 0) &&
        !(isVU0Broadcast(SDValue(U, 0)) && U->getConstantOperandVal(1) == 0))
      return false;
    if (!U->hasOneUse())
      return false;
  }
  SDNode *R = *U->user_begin();
  return R->getValueType(0) == MVT::v4f32 &&
         (R->getOpcode() == ISD::FMUL || R->getOpcode() == ISD::FADD ||
          (R->getOpcode() == ISD::FSUB && R->getOperand(1).getNode() == U));
}

static SDValue performVU0LaneOpCombine(SDNode *N, SelectionDAG &DAG,
                                       const MipsSubtarget &Subtarget) {
  if (!Subtarget.hasVU0() || !VU0LaneOp || N->getValueType(0) != MVT::f32 ||
      !isVU0LaneOpOpcode(N->getOpcode()))
    return SDValue();
  // Whether the value goes into vectors, also through operations with a
  // field, which become VU0 operations the same way.
  std::function<bool(SDNode *, unsigned)> GoesToVU0 = [&](SDNode *V,
                                                          unsigned Depth) {
    return all_of(V->users(), [&](SDNode *U) {
      if (U->getOpcode() == ISD::BUILD_VECTOR ||
          U->getOpcode() == ISD::SCALAR_TO_VECTOR ||
          U->getOpcode() == ISD::INSERT_VECTOR_ELT)
        return true;
      if (Depth >= 3 || !isVU0LaneOpOpcode(U->getOpcode()) ||
          U->getValueType(0) != MVT::f32)
        return false;
      bool First = U->getOperand(0).getNode() == V;
      if (!First && (U->getOpcode() == ISD::FSUB ||
                     (U->getOpcode() == ISD::FDIV &&
                      !isVU0FieldOperand(U->getOperand(0)))))
        return false;
      return isVU0FieldOperand(U->getOperand(First ? 1 : 0)) &&
             GoesToVU0(U, Depth + 1);
    });
  };
  if (!GoesToVU0(N, 0))
    return SDValue();
  SDLoc DL(N);
  SDValue X = N->getOperand(0), Y = N->getOperand(1);
  if (N->getOpcode() == ISD::FDIV) {
    // At least one operand is a field of a vector or a load, and Q is read
    // only as a broadcast (MipsSEDAGToDAGISel::trySelect).
    if (!isVU0FieldOperand(X) || !isVU0FieldOperand(Y) ||
        (isa<ConstantFPSDNode>(X) && isa<ConstantFPSDNode>(Y)) ||
        !isVU0QReadAsBroadcast(N))
      return SDValue();
    auto [A, FA] = getVU0FieldOf(X, DAG);
    auto [B, FB] = getVU0FieldOf(Y, DAG);
    SDValue Q = DAG.getNode(MipsISD::VU0_QDIV, DL, MVT::v4f32,
                            {A, DAG.getTargetConstant(FA, DL, MVT::i32), B,
                             DAG.getTargetConstant(FB, DL, MVT::i32)});
    return DAG.getNode(ISD::EXTRACT_VECTOR_ELT, DL, MVT::f32, Q,
                       DAG.getVectorIdxConstant(0, DL));
  }
  if (!isVU0Lane(X) && N->getOpcode() != ISD::FSUB)
    std::swap(X, Y);
  if (!isVU0Lane(X) || !isVU0FieldOperand(Y))
    return SDValue();
  auto [B, FB] = getVU0FieldOf(Y, DAG);
  SDValue Op = DAG.getNode(N->getOpcode(), DL, MVT::v4f32, X.getOperand(0),
                           getVU0Broadcast(DAG, DL, B, FB), N->getFlags());
  return DAG.getNode(ISD::EXTRACT_VECTOR_ELT, DL, MVT::f32, Op,
                     X.getOperand(1));
}

namespace {
/// A term a * b of a sum computed through the VU0 ACC. Field selects a
/// broadcast field of b (0-3), or all fields (VU0AccFull).
struct VU0AccTerm {
  SDValue A, B;
  unsigned Field;
  bool Neg;
};
constexpr unsigned VU0AccFull = 4;
} // namespace

static bool isVU0AccChain(SDValue V) {
  return (V.getOpcode() == ISD::FADD || V.getOpcode() == ISD::FSUB) &&
         V.getValueType() == MVT::v4f32 && V.hasOneUse();
}

static void addVU0AccLeaf(SDValue V, bool Neg, SelectionDAG &DAG,
                          SmallVectorImpl<VU0AccTerm> &Terms) {
  if (V.getOpcode() == ISD::FMUL && V.hasOneUse()) {
    SDValue X = V.getOperand(0), Y = V.getOperand(1);
    if (isVU0Broadcast(Y))
      Terms.push_back(
          {X, Y.getOperand(0), unsigned(Y.getConstantOperandVal(1)), Neg});
    else if (isVU0Broadcast(X))
      Terms.push_back(
          {Y, X.getOperand(0), unsigned(X.getConstantOperandVal(1)), Neg});
    else
      Terms.push_back({X, Y, VU0AccFull, Neg});
    return;
  }
  // c = c * vf0.w
  Terms.push_back({V, DAG.getRegister(Mips::VF0, MVT::v4f32), 3, Neg});
}

/// Collects the terms of a left-linear chain of FADD and FSUB nodes, in
/// evaluation order.
static void collectVU0AccTerms(SDValue V, SelectionDAG &DAG,
                               SmallVectorImpl<VU0AccTerm> &Terms) {
  SumTerms S;
  collectSumTerms(
      V,
      [&](SDValue X) -> std::optional<bool> {
        if (X != V && !isVU0AccChain(X))
          return std::nullopt;
        return X.getOpcode() == ISD::FSUB;
      },
      /*Flatten=*/false, S);
  for (const SumTerm &T : S.Terms)
    addVU0AccLeaf(T.V, T.Neg, DAG, Terms);
}

/// Whether \p V is a +0.0 vector, which an ACC chain leaves out.
static bool isVU0PositiveZero(const VU0AccTerm &T) {
  if (T.Field != 3 || T.Neg || T.B.getOpcode() != ISD::Register)
    return false;
  if (ConstantFPSDNode *C = isConstOrConstSplatFP(T.A))
    return C->isZero() && !C->isNegative();
  // A constant vector is loaded from the constant pool after legalization.
  auto *Ld = dyn_cast<LoadSDNode>(T.A);
  if (!Ld)
    return false;
  // The address is the low part of the constant pool entry, maybe added to
  // its high part.
  SDValue Ptr = Ld->getBasePtr();
  while (Ptr.getOpcode() == ISD::ADD || Ptr.getOpcode() == MipsISD::Lo ||
         Ptr.getOpcode() == MipsISD::Wrapper)
    Ptr = Ptr.getOperand(Ptr.getNumOperands() - 1);
  auto *CP = dyn_cast<ConstantPoolSDNode>(Ptr);
  return CP && !CP->isMachineConstantPoolEntry() &&
         CP->getConstVal()->isNullValue();
}

// VU0 ACC Sum Combine
// Computes a sum of products as VMULA, VMADDA..., VMADD, which forwards the
// accumulator between the steps, like the VU target:
//   (m0 * v.x + m1 * v.y) + m2 * v.z + m3
//   -> VMULAx acc, m0, v; VMADDAy acc, m1, v; VMADDAz acc, m2, v;
//      VMADDw d, m3, vf0
// The terms are accumulated in the original order, so the result is the
// same. An addend of +0.0 is left out. A chain needs two products.
namespace {
/// A field of a vector.
struct VU0Field {
  SDValue Vec;
  unsigned F;
};
} // namespace

/// The field that lane \p Lane of the v4f32 \p V is: a field of a shuffle,
/// or the field of the vector itself.
static std::optional<VU0Field> getVU0LaneField(SDValue V, unsigned Lane) {
  if (auto *SV = dyn_cast<ShuffleVectorSDNode>(V)) {
    int M = SV->getMaskElt(Lane);
    if (M < 0)
      return std::nullopt;
    return VU0Field{V.getOperand(M / 4), (unsigned)M % 4};
  }
  return VU0Field{V, Lane};
}

/// Whether lane \p I of \p D is field I of the cross product A x B,
/// A[j] * B[k] - A[k] * B[j] with j = I + 1 and k = I + 2 (mod 3). A and B
/// are set by the first lane that is matched.
static bool matchVU0CrossLane(SDValue D, unsigned I, SDValue &A, SDValue &B) {
  if (D.getOpcode() != ISD::FSUB || D.getOperand(0).getOpcode() != ISD::FMUL ||
      D.getOperand(1).getOpcode() != ISD::FMUL)
    return false;
  auto Factors =
      [&](SDValue M) -> std::optional<std::pair<VU0Field, VU0Field>> {
    auto X = getVU0LaneField(M.getOperand(0), I);
    auto Y = getVU0LaneField(M.getOperand(1), I);
    if (!X || !Y)
      return std::nullopt;
    return std::make_pair(*X, *Y);
  };
  auto P = Factors(D.getOperand(0)), Q = Factors(D.getOperand(1));
  if (!P || !Q)
    return false;
  unsigned J = (I + 1) % 3, K = (I + 2) % 3;
  // Product A[F1] * B[F2], in either order of its factors.
  auto Is = [&](std::pair<VU0Field, VU0Field> Prod, unsigned F1, unsigned F2,
                SDValue &PA, SDValue &PB) {
    for (bool Swap : {false, true}) {
      VU0Field X = Swap ? Prod.second : Prod.first;
      VU0Field Y = Swap ? Prod.first : Prod.second;
      if (X.F == F1 && Y.F == F2 && (!PA || (PA == X.Vec && PB == Y.Vec))) {
        PA = X.Vec;
        PB = Y.Vec;
        return true;
      }
    }
    return false;
  };
  SDValue NA = A, NB = B;
  if (!Is(*P, J, K, NA, NB) || !Is(*Q, K, J, NA, NB))
    return false;
  A = NA;
  B = NB;
  return true;
}

/// A cross product of vectors written with shuffles, whose field w is not
/// needed (undefined in the shuffles, or replaced by the only user):
/// VOPMULA.xyz and VOPMSUB.xyz, 2 instructions instead of 4 shuffles through
/// the GPRs, 2 multiplies and a subtract.
static cl::opt<bool> VU0Cross("mips-vu0-cross", cl::Hidden, cl::init(true),
                              cl::desc("Select R5900 VU0 cross products as "
                                       "VOPMULA and VOPMSUB"));

static SDValue performVU0CrossCombine(SDNode *N, SelectionDAG &DAG,
                                      const MipsSubtarget &Subtarget) {
  if (!VU0Cross || !Subtarget.hasVU0() || N->getValueType(0) != MVT::v4f32)
    return SDValue();
  SDValue D(N, 0);
  bool WUnused = (N->hasOneUse() &&
                  N->user_begin()->getOpcode() == ISD::INSERT_VECTOR_ELT &&
                  N->user_begin()->getOperand(0) == D &&
                  isa<ConstantSDNode>(N->user_begin()->getOperand(2)) &&
                  N->user_begin()->getConstantOperandVal(2) == 3) ||
                 all_of(N->op_values(), [](SDValue M) {
                   return M.getOpcode() == ISD::FMUL &&
                          all_of(M->op_values(), [](SDValue S) {
                            auto *SV = dyn_cast<ShuffleVectorSDNode>(S);
                            return SV && SV->getMaskElt(3) < 0;
                          });
                 });
  SDValue A, B;
  if (!WUnused || !matchVU0CrossLane(D, 0, A, B) ||
      !matchVU0CrossLane(D, 1, A, B) || !matchVU0CrossLane(D, 2, A, B))
    return SDValue();
  return DAG.getNode(MipsISD::VU0_CROSS, SDLoc(N), MVT::v4f32, A, B);
}

static cl::opt<bool> VU0InsertField(
    "mips-vu0-insert-field", cl::Hidden, cl::init(true),
    cl::desc("Write a field of an R5900 VU0 vector into another one with one "
             "masked VADDbc"));

static cl::opt<unsigned> VU0AccMinProducts(
    "mips-vu0-acc-min-products", cl::Hidden, cl::init(2),
    cl::desc("The number of products a sum needs to be computed through the "
             "R5900 VU0 ACC"));

static SDValue performVU0AccSumCombine(SDNode *N, SelectionDAG &DAG,
                                       const MipsSubtarget &Subtarget) {
  if (!Subtarget.hasVU0() || N->getValueType(0) != MVT::v4f32)
    return SDValue();
  // Only the root of a chain.
  if (N->hasOneUse() && isVU0AccChain(SDValue(*N->user_begin(), 0)))
    return SDValue();

  SmallVector<VU0AccTerm, 8> Terms;
  collectVU0AccTerms(SDValue(N, 0), DAG, Terms);
  erase_if(Terms, isVU0PositiveZero);
  // VMULA can not negate. ACC is a single register, so chains wait for each
  // other: with one product, a VMUL and VADD that run in parallel with other
  // chains are faster (PS2: Transform 3.95 -> 3.58 Mvert/s as ACC chains;
  // 3.95 -> 3.56 also with ACC as a scheduled value).
  if (Terms[0].Neg || count_if(Terms, [](const VU0AccTerm &T) {
                        return T.B.getOpcode() != ISD::Register;
                      }) < (int)VU0AccMinProducts)
    return SDValue();

  SDLoc DL(N);
  SmallVector<SDValue, 16> Ops;
  for (const VU0AccTerm &T : Terms) {
    Ops.push_back(T.A);
    Ops.push_back(T.B);
    Ops.push_back(DAG.getTargetConstant(T.Field, DL, MVT::i32));
    Ops.push_back(DAG.getTargetConstant(T.Neg, DL, MVT::i32));
  }
  return DAG.getNode(MipsISD::VU0_ACCSUM, DL, MVT::v4f32, Ops);
}

/// Collect the lanes of a single vector Base whose sum (Opc is fadd), maximum
/// or minimum is lane Lane of V, through the vector or scalar operation Opc,
/// broadcasts, shuffles and extracts. Sums must be reassociable and
/// add each lane once, and scalar operations must be ones the R5900 FPU does
/// in hardware (no NaNs, and no infinities for sums).
static bool collectVU0Lanes(SDValue V, unsigned Lane, unsigned Opc,
                            SDValue &Base, unsigned &Lanes, unsigned Depth) {
  if (Depth > 8)
    return false;
  bool IsAdd = Opc == ISD::FADD;

  if (V.getOpcode() == Opc) {
    SDNodeFlags Flags = V->getFlags();
    if (IsAdd && !Flags.hasAllowReassociation())
      return false;
    // Scalar operations that need the IEEE libcall stay scalar.
    if (V.getValueType() == MVT::f32 &&
        (!Flags.hasNoNaNs() || (IsAdd && !Flags.hasNoInfs())))
      return false;
    return collectVU0Lanes(V.getOperand(0), Lane, Opc, Base, Lanes,
                           Depth + 1) &&
           collectVU0Lanes(V.getOperand(1), Lane, Opc, Base, Lanes, Depth + 1);
  }
  switch (V.getOpcode()) {
  case MipsISD::VU0_BCAST:
    return collectVU0Lanes(V.getOperand(0), V.getConstantOperandVal(1), Opc,
                           Base, Lanes, Depth + 1);
  case ISD::VECTOR_SHUFFLE: {
    int M = cast<ShuffleVectorSDNode>(V)->getMaskElt(Lane);
    if (M < 0)
      return false;
    return collectVU0Lanes(V.getOperand(M / 4), M % 4, Opc, Base, Lanes,
                           Depth + 1);
  }
  case ISD::EXTRACT_VECTOR_ELT: {
    auto *Idx = dyn_cast<ConstantSDNode>(V.getOperand(1));
    if (!Idx || V.getOperand(0).getValueType() != MVT::v4f32 ||
        Idx->getZExtValue() > 3)
      return false;
    return collectVU0Lanes(V.getOperand(0), Idx->getZExtValue(), Opc, Base,
                           Lanes, Depth + 1);
  }
  default:
    break;
  }

  if (V.getValueType() != MVT::v4f32)
    return false;
  if (!Base)
    Base = V;
  if (V != Base || (IsAdd && (Lanes & (1u << Lane))))
    return false;
  Lanes |= 1u << Lane;
  return true;
}

// VU0 Horizontal Combine
// Rebuild a sum, maximum or minimum of lanes of one vector that ends up in
// lane x, such as a dot product, as the shortest tree of broadcast operations:
// x+y is VADDy, x+y+z is VADDz(VADDy) and x+y+z+w is VADDz(VADDy, VADDw), which
// takes 9 cycles instead of 12 for a chain, or going through the GPRs.
static SDValue performVU0HorizontalCombine(SDNode *N, SelectionDAG &DAG,
                                           const MipsSubtarget &Subtarget) {
  if (!Subtarget.hasVU0() || N->getValueType(0) != MVT::f32)
    return SDValue();

  unsigned Opc = N->getOpcode();
  if (Opc == ISD::EXTRACT_VECTOR_ELT)
    Opc = N->getOperand(0).getOpcode();
  if (Opc != ISD::FADD && Opc != ISD::FMAXNUM && Opc != ISD::FMINNUM)
    return SDValue();

  SDValue Base;
  unsigned Lanes = 0;
  if (!collectVU0Lanes(SDValue(N, 0), 0, Opc, Base, Lanes, 0) || !(Lanes & 1) ||
      Lanes == 1)
    return SDValue();

  SDLoc DL(N);
  SDNodeFlags Flags;
  Flags.setAllowReassociation(Opc == ISD::FADD);
  auto Op = [&](unsigned BC, SDValue A, SDValue B) {
    return DAG.getNode(Opc, DL, MVT::v4f32, A, getVU0Broadcast(DAG, DL, B, BC),
                       Flags);
  };
  SmallVector<unsigned, 3> BCs;
  for (unsigned BC = 1; BC < 4; ++BC)
    if (Lanes & (1u << BC))
      BCs.push_back(BC);
  SDValue Res = Op(BCs[0], Base, Base);
  if (BCs.size() == 2)
    Res = Op(BCs[1], Res, Base);
  else if (BCs.size() == 3)
    // Lane z of VADDw(Base, Base) is z+w.
    Res = Op(2, Res, Op(3, Base, Base));

  Res = DAG.getNode(ISD::EXTRACT_VECTOR_ELT, DL, MVT::f32, Res,
                    DAG.getVectorIdxConstant(0, DL));
  // Already in this form.
  if (Res.getNode() == N)
    return SDValue();
  return Res;
}

/// An operand of a broadcast add, max or min may have become part of a
/// horizontal operation, such as a select that became an fmaxnum: revisit
/// the extracts of the result, also through further broadcast operations.
static void revisitVU0HorizontalUsers(SDNode *N,
                                      TargetLowering::DAGCombinerInfo &DCI) {
  if (N->getValueType(0) != MVT::v4f32 ||
      none_of(N->ops(), [](const SDUse &U) { return isVU0Broadcast(U.get()); }))
    return;
  auto Revisit = [&](SDNode *U) {
    unsigned Opc = U->getOpcode();
    if (Opc == ISD::EXTRACT_VECTOR_ELT || Opc == ISD::FADD ||
        Opc == ISD::FMAXNUM || Opc == ISD::FMINNUM)
      DCI.AddToWorklist(U);
  };
  for (SDNode *U : N->users()) {
    if (U->getOpcode() == MipsISD::VU0_BCAST)
      for (SDNode *UU : U->users())
        Revisit(UU);
    else
      Revisit(U);
  }
}

// VU0 Blend Combine
// Fold: VU0_BLEND(base, arith(base, x), mask) -> VU0_*_MASKED(base, x, mask)
// Also handles the swapped case: VU0_BLEND(arith(base, x), base, mask)
// This optimizes patterns like: result.xy = a.xy + b.xy, result.zw = a.zw
static SDValue performVU0BlendCombine(SDNode *N, SelectionDAG &DAG,
                                      const MipsSubtarget &Subtarget) {
  if (!Subtarget.hasVU0())
    return SDValue();

  SDLoc DL(N);
  SDValue Op0 = N->getOperand(0);
  SDValue Op1 = N->getOperand(1);
  SDValue MaskVal = N->getOperand(2);
  EVT VT = N->getValueType(0);
  unsigned Mask = cast<ConstantSDNode>(MaskVal)->getZExtValue();

  // Try both orderings: (base, arith) and (arith, base)
  // VU0_BLEND(base, src, mask): result[i] = mask[i] ? src[i] : base[i]
  for (int Swap = 0; Swap <= 1; Swap++) {
    SDValue Base = Swap ? Op1 : Op0;
    SDValue Src = Swap ? Op0 : Op1;
    unsigned EffMask = Swap ? (0xF ^ Mask) : Mask; // Invert mask if swapped

    unsigned SrcOp = Src.getOpcode();
    // A broadcast field into the fields of the mask: one masked VADDbc,
    // 0 + v.<k> (VMULw for w into w), instead of the broadcast and VMOVE.
    if (VU0InsertField && SrcOp == MipsISD::VU0_BCAST && Src.hasOneUse()) {
      unsigned K = Src.getConstantOperandVal(1);
      SDValue Zero = !(EffMask & 1) || (K == 3 && EffMask == 1)
                         ? DAG.getRegister(Mips::VF0, MVT::v4f32)
                         : DAG.getConstantFP(0.0, DL, MVT::v4f32);
      return DAG.getNode(MipsISD::VU0_INSERT_FIELD, DL, VT, Base, Zero,
                         Src.getOperand(0), DAG.getConstant(K, DL, MVT::i32),
                         DAG.getConstant(EffMask, DL, MVT::i32));
    }
    if (SrcOp == ISD::FADD || SrcOp == ISD::FSUB || SrcOp == ISD::FMUL) {
      SDValue ArithOp0 = Src.getOperand(0);
      SDValue ArithOp1 = Src.getOperand(1);

      // Check if one operand of arithmetic is Base
      SDValue Other;
      if (ArithOp0 == Base) {
        Other = ArithOp1;
      } else if (ArithOp1 == Base && SrcOp != ISD::FSUB) {
        // For commutative ops (FADD, FMUL), either operand can be Base
        Other = ArithOp0;
      } else {
        continue; // Try other ordering
      }

      // Create masked arithmetic node
      unsigned MaskedOp;
      switch (SrcOp) {
      case ISD::FADD:
        MaskedOp = MipsISD::VU0_FADD_MASKED;
        break;
      case ISD::FSUB:
        MaskedOp = MipsISD::VU0_FSUB_MASKED;
        break;
      case ISD::FMUL:
        MaskedOp = MipsISD::VU0_FMUL_MASKED;
        break;
      default:
        llvm_unreachable("Unexpected opcode");
      }

      SDValue NewMask = DAG.getConstant(EffMask, DL, MVT::i32);
      return DAG.getNode(MaskedOp, DL, VT, Base, Other, NewMask);
    }
  }

  return SDValue();
}

SDValue
MipsSETargetLowering::PerformDAGCombine(SDNode *N, DAGCombinerInfo &DCI) const {
  SelectionDAG &DAG = DCI.DAG;
  SDValue Val;

  switch (N->getOpcode()) {
  case ISD::AND:
    Val = performANDCombine(N, DAG, DCI, Subtarget);
    break;
  case ISD::OR:
    Val = performORCombine(N, DAG, DCI, Subtarget);
    break;
  case ISD::FADD:
  case ISD::FMAXNUM:
  case ISD::FMINNUM:
    Val = performVU0HorizontalCombine(N, DAG, Subtarget);
    if (!Val)
      Val = performVU0BroadcastPairCombine(N, DAG, Subtarget);
    if (!Val)
      Val = performVU0BroadcastCombine(N, DAG, Subtarget);
    if (!Val)
      Val = performVU0LaneOpCombine(N, DAG, Subtarget);
    if (!Val && N->getOpcode() == ISD::FADD && DCI.isAfterLegalizeDAG())
      Val = performVU0AccSumCombine(N, DAG, Subtarget);
    if (!Val)
      revisitVU0HorizontalUsers(N, DCI);
    break;
  case ISD::FSUB:
    if (!DCI.isAfterLegalizeDAG())
      Val = performVU0CrossCombine(N, DAG, Subtarget);
    if (!Val)
      Val = performVU0BroadcastPairCombine(N, DAG, Subtarget);
    if (!Val)
      Val = performVU0BroadcastCombine(N, DAG, Subtarget);
    if (!Val)
      Val = performVU0LaneOpCombine(N, DAG, Subtarget);
    if (!Val && DCI.isAfterLegalizeDAG())
      Val = performVU0AccSumCombine(N, DAG, Subtarget);
    break;
  case ISD::FMUL:
    Val = performVU0FixedPointCombine(N, DAG, Subtarget);
    if (!Val)
      Val = performVU0BroadcastPairCombine(N, DAG, Subtarget);
    if (!Val)
      Val = performVU0BroadcastCombine(N, DAG, Subtarget);
    if (!Val)
      Val = performVU0LaneOpCombine(N, DAG, Subtarget);
    break;
  case ISD::FDIV:
    Val = performVU0LaneOpCombine(N, DAG, Subtarget);
    break;
  case ISD::FP_TO_SINT:
    Val = performVU0FixedPointCombine(N, DAG, Subtarget);
    break;
  case ISD::SINT_TO_FP:
  case ISD::UINT_TO_FP:
    Val = performVU0IntToFPCombine(N, DAG, Subtarget);
    break;
  case ISD::ADD:
    Val = performR5900ADDCombine(N, DAG, Subtarget);
    if (!Val)
      Val = performR5900MulAddCombine(N, DAG, DCI, Subtarget);
    break;
  case ISD::MUL:
    if (SDValue V = performR5900MULCombine(N, DAG, Subtarget))
      return V;
    return performMULCombine(N, DAG, DCI, this, Subtarget);
  case ISD::SHL:
    Val = performSHLCombine(N, DAG, DCI, Subtarget);
    break;
  case ISD::SRA:
    return performSRACombine(N, DAG, DCI, Subtarget);
  case ISD::SRL:
    return performSRLCombine(N, DAG, DCI, Subtarget);
  case ISD::VSELECT:
    return performVSELECTCombine(N, DAG);
  case ISD::XOR:
    Val = performXORCombine(N, DAG, Subtarget);
    break;
  case ISD::SETCC:
    // Before legalization, constant operands are still folded.
    if (Subtarget.hasVU0() && N->getOperand(0).getValueType() == MVT::v4f32)
      return lowerVU0FSETCC(SDValue(N, 0), DAG);
    Val = performSETCCCombine(N, DAG);
    break;
  case ISD::FP_TO_UINT:
    Val = performFP_TO_UINTCombine(N, DAG);
    break;
  case ISD::TRUNCATE:
    Val = performR5900TRUNCATECombine(N, DAG, Subtarget);
    break;
  case ISD::SIGN_EXTEND:
  case ISD::ZERO_EXTEND:
  case ISD::ANY_EXTEND:
    Val = performR5900ExtendCombine(N, DAG, Subtarget);
    break;
  case MipsISD::VU0_BLEND:
    Val = performVU0BlendCombine(N, DAG, Subtarget);
    break;
  case MipsISD::VU0_BCAST:
    Val = performVU0BCASTCombine(N, DAG);
    break;
  case ISD::BITCAST:
    Val = performR5900BITCASTCombine(N, DAG, Subtarget);
    if (!Val)
      Val = performBITCASTCombine(N, DAG, Subtarget);
    break;
  case ISD::STORE:
    Val = performVU0STORECombine(N, DAG, Subtarget);
    break;
  case ISD::EXTRACT_VECTOR_ELT:
    Val = performR5900EXTRACT_VECTOR_ELTCombine(N, DAG, Subtarget);
    if (!Val)
      Val = performVU0HorizontalCombine(N, DAG, Subtarget);
    break;
  }

  if (Val.getNode()) {
    LLVM_DEBUG(dbgs() << "\nMipsSE DAG Combine:\n";
               N->printrWithDepth(dbgs(), &DAG); dbgs() << "\n=> \n";
               Val.getNode()->printrWithDepth(dbgs(), &DAG); dbgs() << "\n");
    return Val;
  }

  return MipsTargetLowering::PerformDAGCombine(N, DCI);
}

MachineBasicBlock *
MipsSETargetLowering::EmitInstrWithCustomInserter(MachineInstr &MI,
                                                  MachineBasicBlock *BB) const {
  switch (MI.getOpcode()) {
  default:
    return MipsTargetLowering::EmitInstrWithCustomInserter(MI, BB);
  case Mips::BPOSGE32_PSEUDO:
    return emitBPOSGE32(MI, BB);
  case Mips::SNZ_B_PSEUDO:
    return emitMSACBranchPseudo(MI, BB, Mips::BNZ_B);
  case Mips::SNZ_H_PSEUDO:
    return emitMSACBranchPseudo(MI, BB, Mips::BNZ_H);
  case Mips::SNZ_W_PSEUDO:
    return emitMSACBranchPseudo(MI, BB, Mips::BNZ_W);
  case Mips::SNZ_D_PSEUDO:
    return emitMSACBranchPseudo(MI, BB, Mips::BNZ_D);
  case Mips::SNZ_V_PSEUDO:
    return emitMSACBranchPseudo(MI, BB, Mips::BNZ_V);
  case Mips::SZ_B_PSEUDO:
    return emitMSACBranchPseudo(MI, BB, Mips::BZ_B);
  case Mips::SZ_H_PSEUDO:
    return emitMSACBranchPseudo(MI, BB, Mips::BZ_H);
  case Mips::SZ_W_PSEUDO:
    return emitMSACBranchPseudo(MI, BB, Mips::BZ_W);
  case Mips::SZ_D_PSEUDO:
    return emitMSACBranchPseudo(MI, BB, Mips::BZ_D);
  case Mips::SZ_V_PSEUDO:
    return emitMSACBranchPseudo(MI, BB, Mips::BZ_V);
  case Mips::COPY_FW_PSEUDO:
    return emitCOPY_FW(MI, BB);
  case Mips::COPY_FD_PSEUDO:
    return emitCOPY_FD(MI, BB);
  case Mips::INSERT_FW_PSEUDO:
    return emitINSERT_FW(MI, BB);
  case Mips::INSERT_FD_PSEUDO:
    return emitINSERT_FD(MI, BB);
  case Mips::INSERT_B_VIDX_PSEUDO:
  case Mips::INSERT_B_VIDX64_PSEUDO:
    return emitINSERT_DF_VIDX(MI, BB, 1, false);
  case Mips::INSERT_H_VIDX_PSEUDO:
  case Mips::INSERT_H_VIDX64_PSEUDO:
    return emitINSERT_DF_VIDX(MI, BB, 2, false);
  case Mips::INSERT_W_VIDX_PSEUDO:
  case Mips::INSERT_W_VIDX64_PSEUDO:
    return emitINSERT_DF_VIDX(MI, BB, 4, false);
  case Mips::INSERT_D_VIDX_PSEUDO:
  case Mips::INSERT_D_VIDX64_PSEUDO:
    return emitINSERT_DF_VIDX(MI, BB, 8, false);
  case Mips::INSERT_FW_VIDX_PSEUDO:
  case Mips::INSERT_FW_VIDX64_PSEUDO:
    return emitINSERT_DF_VIDX(MI, BB, 4, true);
  case Mips::INSERT_FD_VIDX_PSEUDO:
  case Mips::INSERT_FD_VIDX64_PSEUDO:
    return emitINSERT_DF_VIDX(MI, BB, 8, true);
  case Mips::FILL_FW_PSEUDO:
    return emitFILL_FW(MI, BB);
  case Mips::FILL_FD_PSEUDO:
    return emitFILL_FD(MI, BB);
  case Mips::FEXP2_W_1_PSEUDO:
    return emitFEXP2_W_1(MI, BB);
  case Mips::FEXP2_D_1_PSEUDO:
    return emitFEXP2_D_1(MI, BB);
  }
}

bool MipsSETargetLowering::isEligibleForTailCallOptimization(
    const CCState &CCInfo, unsigned NextStackOffset,
    const MipsFunctionInfo &FI) const {
  // Exception has to be cleared with eret.
  if (FI.isISR())
    return false;

  // Return false if either the callee or caller has a byval argument.
  if (CCInfo.getInRegsParamsCount() > 0 || FI.hasByvalArg())
    return false;

  // Return true if the callee's argument area is no larger than the caller's.
  return NextStackOffset <= FI.getIncomingArgSize();
}

void MipsSETargetLowering::getOpndList(
    SmallVectorImpl<SDValue> &Ops,
    std::deque<std::pair<unsigned, SDValue>> &RegsToPass, bool IsPICCall,
    bool GlobalOrExternal, bool LocalLinkage, bool IsCallReloc,
    CallLoweringInfo &CLI, SDValue Callee, SDValue Chain) const {
  Ops.push_back(Callee);
  MipsTargetLowering::getOpndList(Ops, RegsToPass, IsPICCall, GlobalOrExternal,
                                  LocalLinkage, IsCallReloc, CLI, Callee,
                                  Chain);
}

SDValue MipsSETargetLowering::lowerLOAD(SDValue Op, SelectionDAG &DAG) const {
  LoadSDNode &Nd = *cast<LoadSDNode>(Op);
  EVT MemVT = Nd.getMemoryVT();

  // R5900 vector load handling: LQ requires 16-byte alignment
  if (Subtarget.isR5900() && MemVT.isVector() && MemVT.getSizeInBits() == 128) {
    if (Nd.getAlign() >= Align(16)) {
      // Aligned: let the pattern match LQ
      return SDValue();
    }

    SDLoc DL(Op);
    SDValue Ptr = Nd.getBasePtr();
    SDValue Chain = Nd.getChain();
    EVT PtrVT = Ptr.getValueType();

    // Unaligned with 64-bit GPRs: load the two doublewords (LDL/LDR, or LD
    // when 8-byte aligned) and join them with PCPYLD.
    if (Subtarget.isGP64bit()) {
      MachineMemOperand::Flags Flags = Nd.getMemOperand()->getFlags();
      SDValue Lo = DAG.getLoad(MVT::i64, DL, Chain, Ptr, Nd.getPointerInfo(),
                               Nd.getAlign(), Flags, Nd.getAAInfo());
      SDValue Hi =
          DAG.getLoad(MVT::i64, DL, Chain,
                      DAG.getObjectPtrOffset(DL, Ptr, TypeSize::getFixed(8)),
                      Nd.getPointerInfo().getWithOffset(8),
                      commonAlignment(Nd.getAlign(), 8), Flags, Nd.getAAInfo());
      Chain = DAG.getNode(ISD::TokenFactor, DL, MVT::Other, Lo.getValue(1),
                          Hi.getValue(1));
      if (!Subtarget.isLittle())
        std::swap(Lo, Hi);
      auto ToQuad = [&](SDValue V) {
        return DAG.getTargetInsertSubreg(
            Mips::sub_64, DL, MVT::v4i32,
            SDValue(
                DAG.getMachineNode(TargetOpcode::IMPLICIT_DEF, DL, MVT::v4i32),
                0),
            V);
      };
      SDValue Vec = SDValue(DAG.getMachineNode(Mips::PCPYLD, DL, MVT::v4i32,
                                               ToQuad(Hi), ToQuad(Lo)),
                            0);
      SDValue Ops[2] = {DAG.getBitcast(MemVT, Vec), Chain};
      return DAG.getMergeValues(Ops, DL);
    }

    // Unaligned: scalarize to multiple 32-bit loads

    // Load 4x i32 and build vector
    SmallVector<SDValue, 4> Loads;
    for (unsigned i = 0; i < 4; ++i) {
      SDValue Offset = DAG.getConstant(i * 4, DL, PtrVT);
      SDValue EltPtr = DAG.getNode(ISD::ADD, DL, PtrVT, Ptr, Offset);
      SDValue Elt = DAG.getLoad(
          MVT::i32, DL, Chain, EltPtr, MachinePointerInfo(),
          commonAlignment(Nd.getAlign(), 4), Nd.getMemOperand()->getFlags());
      Loads.push_back(Elt);
      Chain = Elt.getValue(1);
    }

    // Build the v4i32 vector
    SDValue Vec = DAG.getBuildVector(MVT::v4i32, DL, Loads);

    // Bitcast to the requested type if needed
    if (MemVT != MVT::v4i32)
      Vec = DAG.getBitcast(MemVT, Vec);

    SDValue Ops[2] = {Vec, Chain};
    return DAG.getMergeValues(Ops, DL);
  }

  if (MemVT != MVT::f64 ||
      (!Subtarget.getCLOpts().mno_ldc1_sdc1 && Subtarget.hasMips2()))
    return MipsTargetLowering::lowerLOAD(Op, DAG);

  // Replace a double precision load with two i32 loads and a buildpair64.
  SDLoc DL(Op);
  SDValue Ptr = Nd.getBasePtr(), Chain = Nd.getChain();
  EVT PtrVT = Ptr.getValueType();
  EVT VT = Subtarget.hasMips2() ? MVT::i32 : MVT::f32;

  // i32 load from lower address.
  SDValue Lo = DAG.getLoad(VT, DL, Chain, Ptr, MachinePointerInfo(),
                           Nd.getAlign(), Nd.getMemOperand()->getFlags());

  // i32 load from higher address.
  Ptr = DAG.getNode(ISD::ADD, DL, PtrVT, Ptr, DAG.getConstant(4, DL, PtrVT));
  SDValue Hi = DAG.getLoad(VT, DL, Lo.getValue(1), Ptr, MachinePointerInfo(),
                           commonAlignment(Nd.getAlign(), 4),
                           Nd.getMemOperand()->getFlags());

  if (!Subtarget.isLittle())
    std::swap(Lo, Hi);

  SDValue BP;
  if (Subtarget.hasMips2())
    BP = DAG.getNode(MipsISD::BuildPairF64, DL, MVT::f64, Lo, Hi);
  else
    BP = DAG.getNode(MipsISD::BuildPairF64_FPR, DL, MVT::f64, Hi, Lo);

  SDValue Ops[2] = {BP, Hi.getValue(1)};
  return DAG.getMergeValues(Ops, DL);
}

SDValue MipsSETargetLowering::lowerSTORE(SDValue Op, SelectionDAG &DAG) const {
  StoreSDNode &Nd = *cast<StoreSDNode>(Op);
  EVT MemVT = Nd.getMemoryVT();

  // R5900 vector store handling: SQ requires 16-byte alignment
  if (Subtarget.isR5900() && MemVT.isVector() && MemVT.getSizeInBits() == 128) {
    if (Nd.getAlign() >= Align(16)) {
      // Aligned: let the pattern match SQ
      return SDValue();
    }

    SDLoc DL(Op);
    SDValue Val = Nd.getValue();
    SDValue Ptr = Nd.getBasePtr();
    SDValue Chain = Nd.getChain();
    EVT PtrVT = Ptr.getValueType();

    // Bitcast to v4i32 if needed
    if (MemVT != MVT::v4i32)
      Val = DAG.getBitcast(MVT::v4i32, Val);

    // Unaligned with 64-bit GPRs: split the doublewords with PCPYUD and store
    // them with SDL/SDR, or SD when 8-byte aligned.
    if (Subtarget.isGP64bit()) {
      SDValue Lo = DAG.getTargetExtractSubreg(Mips::sub_64, DL, MVT::i64, Val);
      SDValue Hi = DAG.getTargetExtractSubreg(
          Mips::sub_64, DL, MVT::i64,
          SDValue(DAG.getMachineNode(Mips::PCPYUD, DL, MVT::v4i32, Val, Val),
                  0));
      if (!Subtarget.isLittle())
        std::swap(Lo, Hi);
      MachineMemOperand::Flags Flags = Nd.getMemOperand()->getFlags();
      SDValue StLo = DAG.getStore(Chain, DL, Lo, Ptr, Nd.getPointerInfo(),
                                  Nd.getAlign(), Flags, Nd.getAAInfo());
      SDValue StHi = DAG.getStore(
          Chain, DL, Hi, DAG.getObjectPtrOffset(DL, Ptr, TypeSize::getFixed(8)),
          Nd.getPointerInfo().getWithOffset(8),
          commonAlignment(Nd.getAlign(), 8), Flags, Nd.getAAInfo());
      return DAG.getNode(ISD::TokenFactor, DL, MVT::Other, StLo, StHi);
    }

    // Unaligned: scalarize to multiple 32-bit stores

    // Store 4x i32 elements
    for (unsigned i = 0; i < 4; ++i) {
      SDValue Elt = DAG.getNode(ISD::EXTRACT_VECTOR_ELT, DL, MVT::i32, Val,
                                DAG.getConstant(i, DL, MVT::i32));
      SDValue Offset = DAG.getConstant(i * 4, DL, PtrVT);
      SDValue EltPtr = DAG.getNode(ISD::ADD, DL, PtrVT, Ptr, Offset);
      Chain = DAG.getStore(Chain, DL, Elt, EltPtr, MachinePointerInfo(),
                           commonAlignment(Nd.getAlign(), 4),
                           Nd.getMemOperand()->getFlags(), Nd.getAAInfo());
    }

    return Chain;
  }

  if (MemVT != MVT::f64 ||
      (!Subtarget.getCLOpts().mno_ldc1_sdc1 && Subtarget.hasMips2()))
    return MipsTargetLowering::lowerSTORE(Op, DAG);

  // Replace a double precision store with two extractelement64s and i32 stores.
  SDLoc DL(Op);
  SDValue Val = Nd.getValue(), Ptr = Nd.getBasePtr(), Chain = Nd.getChain();
  EVT PtrVT = Ptr.getValueType();
  EVT VT = Subtarget.hasMips2() ? MVT::i32 : MVT::f32;

  unsigned ExtractOp = Subtarget.hasMips2() ? MipsISD::ExtractElementF64
                                            : MipsISD::ExtractElementF64_FPR;
  SDValue Lo =
      DAG.getNode(ExtractOp, DL, VT, Val, DAG.getConstant(0, DL, MVT::i32));
  SDValue Hi =
      DAG.getNode(ExtractOp, DL, VT, Val, DAG.getConstant(1, DL, MVT::i32));

  if (!Subtarget.isLittle())
    std::swap(Lo, Hi);

  // i32 store to lower address.
  Chain = DAG.getStore(Chain, DL, Lo, Ptr, MachinePointerInfo(), Nd.getAlign(),
                       Nd.getMemOperand()->getFlags(), Nd.getAAInfo());

  // i32 store to higher address.
  Ptr = DAG.getNode(ISD::ADD, DL, PtrVT, Ptr, DAG.getConstant(4, DL, PtrVT));
  return DAG.getStore(Chain, DL, Hi, Ptr, MachinePointerInfo(),
                      commonAlignment(Nd.getAlign(), 4),
                      Nd.getMemOperand()->getFlags(), Nd.getAAInfo());
}

SDValue MipsSETargetLowering::lowerBITCAST(SDValue Op,
                                           SelectionDAG &DAG) const {
  SDLoc DL(Op);
  MVT Src = Op.getOperand(0).getValueType().getSimpleVT();
  MVT Dest = Op.getValueType().getSimpleVT();

  // Bitcast i64 to double.
  if (Src == MVT::i64 && Dest == MVT::f64) {
    SDValue Lo, Hi;
    std::tie(Lo, Hi) =
        DAG.SplitScalar(Op.getOperand(0), DL, MVT::i32, MVT::i32);
    return DAG.getNode(MipsISD::BuildPairF64, DL, MVT::f64, Lo, Hi);
  }

  // Bitcast double to i64.
  if (Src == MVT::f64 && Dest == MVT::i64) {
    // Skip lower bitcast when operand0 has converted float results to integer
    // which was done by function SoftenFloatResult.
    if (getTypeAction(*DAG.getContext(), Op.getOperand(0).getValueType()) ==
        TargetLowering::TypeSoftenFloat)
      return SDValue();
    SDValue Lo =
        DAG.getNode(MipsISD::ExtractElementF64, DL, MVT::i32, Op.getOperand(0),
                    DAG.getConstant(0, DL, MVT::i32));
    SDValue Hi =
        DAG.getNode(MipsISD::ExtractElementF64, DL, MVT::i32, Op.getOperand(0),
                    DAG.getConstant(1, DL, MVT::i32));
    return DAG.getNode(ISD::BUILD_PAIR, DL, MVT::i64, Lo, Hi);
  }

  // Skip other cases of bitcast and use default lowering.
  return SDValue();
}

SDValue MipsSETargetLowering::lowerCTLZ(SDValue Op, SelectionDAG &DAG) const {
  // R5900 PLZCW instruction counts leading bits matching the sign bit.
  // For non-negative numbers, this equals (leading zeros - 1).
  // For negative numbers, this equals (leading ones - 1).
  //
  // CTLZ always counts leading zeros:
  // - Non-negative: CTLZ = PLZCW + 1
  // - Negative: CTLZ = 0 (MSB is 1, so no leading zeros)
  //
  // Lower to: (src < 0) ? 0 : (PLZCW(src) + 1)

  SDLoc DL(Op);
  SDValue Src = Op.getOperand(0);
  EVT VT = Op.getValueType();

  // Only handle i32 for R5900
  if (VT != MVT::i32 || !Subtarget.isR5900())
    return SDValue();

  // Generate PLZCW node - counts leading bits matching MSB, returns (count-1)
  SDValue PLZCWRes = DAG.getNode(MipsISD::PLZCW, DL, VT, Src);

  // Add 1 to get actual leading zeros count (for non-negative input)
  SDValue One = DAG.getConstant(1, DL, VT);
  SDValue CountPlusOne = DAG.getNode(ISD::ADD, DL, VT, PLZCWRes, One);

  // If input is negative (MSB=1), CTLZ result is 0
  SDValue Zero = DAG.getConstant(0, DL, VT);
  SDValue IsNeg = DAG.getSetCC(DL, MVT::i32, Src, Zero, ISD::SETLT);

  // Select: if negative return 0, else return PLZCW+1
  return DAG.getSelect(DL, VT, IsNeg, Zero, CountPlusOne);
}

SDValue MipsSETargetLowering::lowerVectorShift(SDValue Op,
                                               SelectionDAG &DAG) const {
  // R5900 MMI: Lower vector shifts to immediate shift instructions when
  // the shift amount is a constant splat.
  //
  // PSLLW/PSRLW/PSRAW: v4i32 shifts with 5-bit immediate (0-31)
  // PSLLH/PSRLH/PSRAH: v8i16 shifts with 4-bit immediate (0-15)
  //
  // For non-constant or variable shifts, return SDValue() to expand/scalarize.
  // Note: PSLLVW/PSRLVW/PSRAVW only shift 2 of 4 words - NOT usable for v4i32.

  EVT VT = Op.getValueType();

  // Only handle R5900 vector types
  if (!Subtarget.isR5900())
    return SDValue();
  if (VT != MVT::v4i32 && VT != MVT::v8i16)
    return SDValue();

  SDValue Vec = Op.getOperand(0);
  SDValue Shift = Op.getOperand(1);
  SDLoc DL(Op);

  // Check if shift amount is a constant splat using existing helper
  APInt SplatValue;
  if (!isVSplat(Shift, SplatValue, Subtarget.isLittle()))
    return SDValue(); // Not constant splat, expand to scalar

  // Validate immediate range
  unsigned MaxShift = (VT == MVT::v4i32) ? 31 : 15;
  uint64_t ShiftAmt = SplatValue.getZExtValue();
  if (ShiftAmt > MaxShift)
    return SDValue(); // Out of range, expand to scalar

  // Select instruction based on operation and vector type
  unsigned Opc;
  switch (Op.getOpcode()) {
  case ISD::SHL:
    Opc = (VT == MVT::v4i32) ? Mips::PSLLW : Mips::PSLLH;
    break;
  case ISD::SRL:
    Opc = (VT == MVT::v4i32) ? Mips::PSRLW : Mips::PSRLH;
    break;
  case ISD::SRA:
    Opc = (VT == MVT::v4i32) ? Mips::PSRAW : Mips::PSRAH;
    break;
  default:
    return SDValue();
  }

  // Create immediate operand and emit machine instruction
  SDValue ImmOp = DAG.getTargetConstant(ShiftAmt, DL, MVT::i32);
  return SDValue(DAG.getMachineNode(Opc, DL, VT, Vec, ImmOp), 0);
}

/// R5900: an unsigned compare of 128-bit vectors as the signed compare of the
/// operands with their sign bits flipped.
SDValue MipsSETargetLowering::lowerR5900SETCC(SDValue Op,
                                              SelectionDAG &DAG) const {
  ISD::CondCode CC = cast<CondCodeSDNode>(Op.getOperand(2))->get();
  if (Op.getOperand(0).getValueType() == MVT::v4f32)
    return lowerVU0FSETCC(Op, DAG);
  ISD::CondCode SignedCC;
  switch (CC) {
  case ISD::SETUGT:
    SignedCC = ISD::SETGT;
    break;
  case ISD::SETUGE:
    SignedCC = ISD::SETGE;
    break;
  case ISD::SETULT:
    SignedCC = ISD::SETLT;
    break;
  case ISD::SETULE:
    SignedCC = ISD::SETLE;
    break;
  default:
    llvm_unreachable("Unexpected condition code");
  }
  SDLoc DL(Op);
  EVT VT = Op.getOperand(0).getValueType();
  SDValue SignMask =
      DAG.getConstant(APInt::getSignMask(VT.getScalarSizeInBits()), DL, VT);
  SDValue A = DAG.getNode(ISD::XOR, DL, VT, Op.getOperand(0), SignMask);
  SDValue B = DAG.getNode(ISD::XOR, DL, VT, Op.getOperand(1), SignMask);
  return DAG.getSetCC(DL, Op.getValueType(), A, B, SignedCC);
}

/// VU0: a v4f32 compare as the signed compare of the bits of the operands in
/// a 128-bit GPR, with the magnitude of a negative one negated, so that -0
/// and +0 are equal. There are no NaNs: the FPU and VU0 order the bits of one
/// as a large magnitude too, so ordered and unordered conditions are the
/// same.
SDValue MipsSETargetLowering::lowerVU0FSETCC(SDValue Op,
                                             SelectionDAG &DAG) const {
  SDLoc DL(Op);
  EVT VT = Op.getValueType();
  ISD::CondCode CC = cast<CondCodeSDNode>(Op.getOperand(2))->get();
  ISD::CondCode IntCC;
  switch (CC) {
  case ISD::SETFALSE:
  case ISD::SETFALSE2:
  case ISD::SETUO:
    return DAG.getConstant(0, DL, VT);
  case ISD::SETTRUE:
  case ISD::SETTRUE2:
  case ISD::SETO:
    return DAG.getAllOnesConstant(DL, VT);
  case ISD::SETEQ:
  case ISD::SETOEQ:
  case ISD::SETUEQ:
    IntCC = ISD::SETEQ;
    break;
  case ISD::SETNE:
  case ISD::SETONE:
  case ISD::SETUNE:
    IntCC = ISD::SETNE;
    break;
  case ISD::SETGT:
  case ISD::SETOGT:
  case ISD::SETUGT:
    IntCC = ISD::SETGT;
    break;
  case ISD::SETGE:
  case ISD::SETOGE:
  case ISD::SETUGE:
    IntCC = ISD::SETGE;
    break;
  case ISD::SETLT:
  case ISD::SETOLT:
  case ISD::SETULT:
    IntCC = ISD::SETLT;
    break;
  case ISD::SETLE:
  case ISD::SETOLE:
  case ISD::SETULE:
    IntCC = ISD::SETLE;
    break;
  default:
    llvm_unreachable("Unexpected condition code");
  }
  auto Key = [&](SDValue X) {
    if (ISD::isBuildVectorAllZeros(X.getNode()))
      return DAG.getConstant(0, DL, MVT::v4i32);
    SDValue Bits = DAG.getBitcast(MVT::v4i32, X);
    SDValue Sign = DAG.getNode(ISD::SRA, DL, MVT::v4i32, Bits,
                               DAG.getConstant(31, DL, MVT::v4i32));
    SDValue Magnitude = DAG.getNode(
        ISD::AND, DL, MVT::v4i32, Bits,
        DAG.getConstant(APInt::getSignedMaxValue(32), DL, MVT::v4i32));
    return DAG.getNode(ISD::SUB, DL, MVT::v4i32,
                       DAG.getNode(ISD::XOR, DL, MVT::v4i32, Magnitude, Sign),
                       Sign);
  };
  return DAG.getSetCC(DL, VT, Key(Op.getOperand(0)), Key(Op.getOperand(1)),
                      IntCC);
}

/// R5900: (vselect M, T, F) with a mask M of all ones or all zeros in each
/// element as F ^ ((F ^ T) & M), which needs no inverted mask. An inverted
/// mask, also that of a compare that is legalized to an inverted one, swaps
/// T and F instead.
SDValue MipsSETargetLowering::lowerR5900VSELECT(SDValue Op,
                                                SelectionDAG &DAG) const {
  SDLoc DL(Op);
  EVT VT = Op.getValueType();
  SDValue Mask = Op.getOperand(0);
  SDValue T = Op.getOperand(1), F = Op.getOperand(2);
  // VU0: select the bits in a 128-bit GPR.
  if (VT == MVT::v4f32)
    return DAG.getBitcast(VT, DAG.getNode(ISD::VSELECT, DL, MVT::v4i32, Mask,
                                          DAG.getBitcast(MVT::v4i32, T),
                                          DAG.getBitcast(MVT::v4i32, F)));
  if (isBitwiseNot(Mask)) {
    Mask = Mask.getOperand(0);
    std::swap(T, F);
  } else if (Mask.getOpcode() == ISD::SETCC) {
    ISD::CondCode CC = cast<CondCodeSDNode>(Mask.getOperand(2))->get();
    if (CC == ISD::SETNE || CC == ISD::SETGE || CC == ISD::SETLE ||
        CC == ISD::SETUGE || CC == ISD::SETULE) {
      Mask = DAG.getSetCC(
          DL, Mask.getValueType(), Mask.getOperand(0), Mask.getOperand(1),
          ISD::getSetCCInverse(CC, Mask.getOperand(0).getValueType()));
      std::swap(T, F);
    }
  }
  Mask = DAG.getBitcast(VT, Mask);
  SDValue Diff = DAG.getNode(ISD::XOR, DL, VT, F, T);
  return DAG.getNode(ISD::XOR, DL, VT, F,
                     DAG.getNode(ISD::AND, DL, VT, Diff, Mask));
}

SDValue MipsSETargetLowering::lowerMulDiv(SDValue Op, unsigned NewOpc,
                                          bool HasLo, bool HasHi,
                                          SelectionDAG &DAG) const {
  // MIPS32r6/MIPS64r6 removed accumulator based multiplies.
  assert(!Subtarget.hasMips32r6());

  EVT Ty = Op.getOperand(0).getValueType();
  SDLoc DL(Op);
  SDValue Mult = DAG.getNode(NewOpc, DL, MVT::Untyped,
                             Op.getOperand(0), Op.getOperand(1));
  SDValue Lo, Hi;

  if (HasLo)
    Lo = DAG.getNode(MipsISD::MFLO, DL, Ty, Mult);
  if (HasHi)
    Hi = DAG.getNode(MipsISD::MFHI, DL, Ty, Mult);

  if (!HasLo || !HasHi)
    return HasLo ? Lo : Hi;

  SDValue Vals[] = { Lo, Hi };
  return DAG.getMergeValues(Vals, DL);
}

static SDValue initAccumulator(SDValue In, const SDLoc &DL, SelectionDAG &DAG) {
  SDValue InLo, InHi;
  std::tie(InLo, InHi) = DAG.SplitScalar(In, DL, MVT::i32, MVT::i32);
  return DAG.getNode(MipsISD::MTLOHI, DL, MVT::Untyped, InLo, InHi);
}

static SDValue extractLOHI(SDValue Op, const SDLoc &DL, SelectionDAG &DAG) {
  SDValue Lo = DAG.getNode(MipsISD::MFLO, DL, MVT::i32, Op);
  SDValue Hi = DAG.getNode(MipsISD::MFHI, DL, MVT::i32, Op);
  return DAG.getNode(ISD::BUILD_PAIR, DL, MVT::i64, Lo, Hi);
}

// This function expands mips intrinsic nodes which have 64-bit input operands
// or output values.
//
// out64 = intrinsic-node in64
// =>
// lo = copy (extract-element (in64, 0))
// hi = copy (extract-element (in64, 1))
// mips-specific-node
// v0 = copy lo
// v1 = copy hi
// out64 = merge-values (v0, v1)
//
static SDValue lowerDSPIntr(SDValue Op, SelectionDAG &DAG, unsigned Opc) {
  SDLoc DL(Op);
  bool HasChainIn = Op->getOperand(0).getValueType() == MVT::Other;
  SmallVector<SDValue, 3> Ops;
  unsigned OpNo = 0;

  // See if Op has a chain input.
  if (HasChainIn)
    Ops.push_back(Op->getOperand(OpNo++));

  // The next operand is the intrinsic opcode.
  assert(Op->getOperand(OpNo).getOpcode() == ISD::TargetConstant);

  // See if the next operand has type i64.
  SDValue Opnd = Op->getOperand(++OpNo), In64;

  if (Opnd.getValueType() == MVT::i64)
    In64 = initAccumulator(Opnd, DL, DAG);
  else
    Ops.push_back(Opnd);

  // Push the remaining operands.
  for (++OpNo ; OpNo < Op->getNumOperands(); ++OpNo)
    Ops.push_back(Op->getOperand(OpNo));

  // Add In64 to the end of the list.
  if (In64.getNode())
    Ops.push_back(In64);

  // Scan output.
  SmallVector<EVT, 2> ResTys;

  for (EVT Ty : Op->values())
    ResTys.push_back((Ty == MVT::i64) ? MVT::Untyped : Ty);

  // Create node.
  SDValue Val = DAG.getNode(Opc, DL, ResTys, Ops);
  SDValue Out = (ResTys[0] == MVT::Untyped) ? extractLOHI(Val, DL, DAG) : Val;

  if (!HasChainIn)
    return Out;

  assert(Val->getValueType(1) == MVT::Other);
  SDValue Vals[] = { Out, SDValue(Val.getNode(), 1) };
  return DAG.getMergeValues(Vals, DL);
}

// Lower an MSA copy intrinsic into the specified SelectionDAG node
static SDValue lowerMSACopyIntr(SDValue Op, SelectionDAG &DAG, unsigned Opc) {
  SDLoc DL(Op);
  SDValue Vec = Op->getOperand(1);
  const TargetLowering &TLI = DAG.getTargetLoweringInfo();
  SDValue Idx = DAG.getZExtOrTrunc(Op->getOperand(2), DL,
                                   TLI.getVectorIdxTy(DAG.getDataLayout()));
  EVT ResTy = Op->getValueType(0);
  EVT EltTy = Vec->getValueType(0).getVectorElementType();

  SDValue Result = DAG.getNode(Opc, DL, ResTy, Vec, Idx,
                               DAG.getValueType(EltTy));

  return Result;
}

static SDValue lowerMSASplatZExt(SDValue Op, unsigned OpNr, SelectionDAG &DAG) {
  EVT ResVecTy = Op->getValueType(0);
  EVT ViaVecTy = ResVecTy;
  bool BigEndian = !DAG.getSubtarget().getTargetTriple().isLittleEndian();
  SDLoc DL(Op);

  // When ResVecTy == MVT::v2i64, LaneA is the upper 32 bits of the lane and
  // LaneB is the lower 32-bits. Otherwise LaneA and LaneB are alternating
  // lanes.
  SDValue LaneA = Op->getOperand(OpNr);
  SDValue LaneB;

  if (ResVecTy == MVT::v2i64) {
    // In case of the index being passed as an immediate value, set the upper
    // lane to 0 so that the splati.d instruction can be matched.
    if (isa<ConstantSDNode>(LaneA))
      LaneB = DAG.getConstant(0, DL, MVT::i32);
    // Having the index passed in a register, set the upper lane to the same
    // value as the lower - this results in the BUILD_VECTOR node not being
    // expanded through stack. This way we are able to pattern match the set of
    // nodes created here to splat.d.
    else
      LaneB = LaneA;
    ViaVecTy = MVT::v4i32;
    if(BigEndian)
      std::swap(LaneA, LaneB);
  } else
    LaneB = LaneA;

  SDValue Ops[16] = { LaneA, LaneB, LaneA, LaneB, LaneA, LaneB, LaneA, LaneB,
                      LaneA, LaneB, LaneA, LaneB, LaneA, LaneB, LaneA, LaneB };

  SDValue Result = DAG.getBuildVector(
      ViaVecTy, DL, ArrayRef(Ops, ViaVecTy.getVectorNumElements()));

  if (ViaVecTy != ResVecTy) {
    SDValue One = DAG.getConstant(1, DL, ViaVecTy);
    Result = DAG.getNode(ISD::BITCAST, DL, ResVecTy,
                         DAG.getNode(ISD::AND, DL, ViaVecTy, Result, One));
  }

  return Result;
}

static SDValue lowerMSASplatImm(SDValue Op, unsigned ImmOp, SelectionDAG &DAG,
                                bool IsSigned = false) {
  auto *CImm = cast<ConstantSDNode>(Op->getOperand(ImmOp));
  return DAG.getConstant(
      APInt(Op->getValueType(0).getScalarType().getSizeInBits(),
            IsSigned ? CImm->getSExtValue() : CImm->getZExtValue(), IsSigned),
      SDLoc(Op), Op->getValueType(0));
}

static SDValue getBuildVectorSplat(EVT VecTy, SDValue SplatValue,
                                   bool BigEndian, SelectionDAG &DAG) {
  EVT ViaVecTy = VecTy;
  SDValue SplatValueA = SplatValue;
  SDValue SplatValueB = SplatValue;
  SDLoc DL(SplatValue);

  if (VecTy == MVT::v2i64) {
    // v2i64 BUILD_VECTOR must be performed via v4i32 so split into i32's.
    ViaVecTy = MVT::v4i32;

    SplatValueA = DAG.getNode(ISD::TRUNCATE, DL, MVT::i32, SplatValue);
    SplatValueB = DAG.getNode(ISD::SRL, DL, MVT::i64, SplatValue,
                              DAG.getConstant(32, DL, MVT::i32));
    SplatValueB = DAG.getNode(ISD::TRUNCATE, DL, MVT::i32, SplatValueB);
  }

  // We currently hold the parts in little endian order. Swap them if
  // necessary.
  if (BigEndian)
    std::swap(SplatValueA, SplatValueB);

  SDValue Ops[16] = { SplatValueA, SplatValueB, SplatValueA, SplatValueB,
                      SplatValueA, SplatValueB, SplatValueA, SplatValueB,
                      SplatValueA, SplatValueB, SplatValueA, SplatValueB,
                      SplatValueA, SplatValueB, SplatValueA, SplatValueB };

  SDValue Result = DAG.getBuildVector(
      ViaVecTy, DL, ArrayRef(Ops, ViaVecTy.getVectorNumElements()));

  if (VecTy != ViaVecTy)
    Result = DAG.getNode(ISD::BITCAST, DL, VecTy, Result);

  return Result;
}

static SDValue lowerMSABinaryBitImmIntr(SDValue Op, SelectionDAG &DAG,
                                        unsigned Opc, SDValue Imm,
                                        bool BigEndian) {
  EVT VecTy = Op->getValueType(0);
  SDValue Exp2Imm;
  SDLoc DL(Op);

  // The DAG Combiner can't constant fold bitcasted vectors yet so we must do it
  // here for now.
  if (VecTy == MVT::v2i64) {
    if (ConstantSDNode *CImm = dyn_cast<ConstantSDNode>(Imm)) {
      APInt BitImm = APInt(64, 1) << CImm->getAPIntValue();

      SDValue BitImmHiOp = DAG.getConstant(BitImm.lshr(32).trunc(32), DL,
                                           MVT::i32);
      SDValue BitImmLoOp = DAG.getConstant(BitImm.trunc(32), DL, MVT::i32);

      if (BigEndian)
        std::swap(BitImmLoOp, BitImmHiOp);

      Exp2Imm = DAG.getNode(
          ISD::BITCAST, DL, MVT::v2i64,
          DAG.getBuildVector(MVT::v4i32, DL,
                             {BitImmLoOp, BitImmHiOp, BitImmLoOp, BitImmHiOp}));
    }
  }

  if (!Exp2Imm.getNode()) {
    // We couldnt constant fold, do a vector shift instead

    // Extend i32 to i64 if necessary. Sign or zero extend doesn't matter since
    // only values 0-63 are valid.
    if (VecTy == MVT::v2i64)
      Imm = DAG.getNode(ISD::ZERO_EXTEND, DL, MVT::i64, Imm);

    Exp2Imm = getBuildVectorSplat(VecTy, Imm, BigEndian, DAG);

    Exp2Imm = DAG.getNode(ISD::SHL, DL, VecTy, DAG.getConstant(1, DL, VecTy),
                          Exp2Imm);
  }

  return DAG.getNode(Opc, DL, VecTy, Op->getOperand(1), Exp2Imm);
}

static SDValue truncateVecElts(SDValue Op, SelectionDAG &DAG) {
  SDLoc DL(Op);
  EVT ResTy = Op->getValueType(0);
  SDValue Vec = Op->getOperand(2);
  bool BigEndian = !DAG.getSubtarget().getTargetTriple().isLittleEndian();
  MVT ResEltTy = ResTy == MVT::v2i64 ? MVT::i64 : MVT::i32;
  SDValue ConstValue = DAG.getConstant(Vec.getScalarValueSizeInBits() - 1,
                                       DL, ResEltTy);
  SDValue SplatVec = getBuildVectorSplat(ResTy, ConstValue, BigEndian, DAG);

  return DAG.getNode(ISD::AND, DL, ResTy, Vec, SplatVec);
}

static SDValue lowerMSABitClear(SDValue Op, SelectionDAG &DAG) {
  EVT ResTy = Op->getValueType(0);
  SDLoc DL(Op);
  SDValue One = DAG.getConstant(1, DL, ResTy);
  SDValue Bit = DAG.getNode(ISD::SHL, DL, ResTy, One, truncateVecElts(Op, DAG));

  return DAG.getNode(ISD::AND, DL, ResTy, Op->getOperand(1),
                     DAG.getNOT(DL, Bit, ResTy));
}

static SDValue lowerMSABitClearImm(SDValue Op, SelectionDAG &DAG) {
  SDLoc DL(Op);
  EVT ResTy = Op->getValueType(0);
  APInt BitImm = APInt(ResTy.getScalarSizeInBits(), 1)
                 << Op->getConstantOperandAPInt(2);
  SDValue BitMask = DAG.getConstant(~BitImm, DL, ResTy);

  return DAG.getNode(ISD::AND, DL, ResTy, Op->getOperand(1), BitMask);
}

SDValue MipsSETargetLowering::lowerINTRINSIC_WO_CHAIN(SDValue Op,
                                                      SelectionDAG &DAG) const {
  SDLoc DL(Op);
  unsigned Intrinsic = Op->getConstantOperandVal(0);
  switch (Intrinsic) {
  default:
    return SDValue();
  case Intrinsic::mips_shilo:
    return lowerDSPIntr(Op, DAG, MipsISD::SHILO);
  case Intrinsic::mips_dpau_h_qbl:
    return lowerDSPIntr(Op, DAG, MipsISD::DPAU_H_QBL);
  case Intrinsic::mips_dpau_h_qbr:
    return lowerDSPIntr(Op, DAG, MipsISD::DPAU_H_QBR);
  case Intrinsic::mips_dpsu_h_qbl:
    return lowerDSPIntr(Op, DAG, MipsISD::DPSU_H_QBL);
  case Intrinsic::mips_dpsu_h_qbr:
    return lowerDSPIntr(Op, DAG, MipsISD::DPSU_H_QBR);
  case Intrinsic::mips_dpa_w_ph:
    return lowerDSPIntr(Op, DAG, MipsISD::DPA_W_PH);
  case Intrinsic::mips_dps_w_ph:
    return lowerDSPIntr(Op, DAG, MipsISD::DPS_W_PH);
  case Intrinsic::mips_dpax_w_ph:
    return lowerDSPIntr(Op, DAG, MipsISD::DPAX_W_PH);
  case Intrinsic::mips_dpsx_w_ph:
    return lowerDSPIntr(Op, DAG, MipsISD::DPSX_W_PH);
  case Intrinsic::mips_mulsa_w_ph:
    return lowerDSPIntr(Op, DAG, MipsISD::MULSA_W_PH);
  case Intrinsic::mips_mult:
    return lowerDSPIntr(Op, DAG, MipsISD::Mult);
  case Intrinsic::mips_multu:
    return lowerDSPIntr(Op, DAG, MipsISD::Multu);
  case Intrinsic::mips_madd:
    return lowerDSPIntr(Op, DAG, MipsISD::MAdd);
  case Intrinsic::mips_maddu:
    return lowerDSPIntr(Op, DAG, MipsISD::MAddu);
  case Intrinsic::mips_msub:
    return lowerDSPIntr(Op, DAG, MipsISD::MSub);
  case Intrinsic::mips_msubu:
    return lowerDSPIntr(Op, DAG, MipsISD::MSubu);
  case Intrinsic::mips_addv_b:
  case Intrinsic::mips_addv_h:
  case Intrinsic::mips_addv_w:
  case Intrinsic::mips_addv_d:
    return DAG.getNode(ISD::ADD, DL, Op->getValueType(0), Op->getOperand(1),
                       Op->getOperand(2));
  case Intrinsic::mips_addvi_b:
  case Intrinsic::mips_addvi_h:
  case Intrinsic::mips_addvi_w:
  case Intrinsic::mips_addvi_d:
    return DAG.getNode(ISD::ADD, DL, Op->getValueType(0), Op->getOperand(1),
                       lowerMSASplatImm(Op, 2, DAG));
  case Intrinsic::mips_and_v:
    return DAG.getNode(ISD::AND, DL, Op->getValueType(0), Op->getOperand(1),
                       Op->getOperand(2));
  case Intrinsic::mips_andi_b:
    return DAG.getNode(ISD::AND, DL, Op->getValueType(0), Op->getOperand(1),
                       lowerMSASplatImm(Op, 2, DAG));
  case Intrinsic::mips_bclr_b:
  case Intrinsic::mips_bclr_h:
  case Intrinsic::mips_bclr_w:
  case Intrinsic::mips_bclr_d:
    return lowerMSABitClear(Op, DAG);
  case Intrinsic::mips_bclri_b:
  case Intrinsic::mips_bclri_h:
  case Intrinsic::mips_bclri_w:
  case Intrinsic::mips_bclri_d:
    return lowerMSABitClearImm(Op, DAG);
  case Intrinsic::mips_binsli_b:
  case Intrinsic::mips_binsli_h:
  case Intrinsic::mips_binsli_w:
  case Intrinsic::mips_binsli_d: {
    // binsli_x(IfClear, IfSet, nbits) -> (vselect LBitsMask, IfSet, IfClear)
    EVT VecTy = Op->getValueType(0);
    EVT EltTy = VecTy.getVectorElementType();
    if (Op->getConstantOperandVal(3) >= EltTy.getSizeInBits())
      report_fatal_error("Immediate out of range");
    APInt Mask = APInt::getHighBitsSet(EltTy.getSizeInBits(),
                                       Op->getConstantOperandVal(3) + 1);
    return DAG.getNode(ISD::VSELECT, DL, VecTy,
                       DAG.getConstant(Mask, DL, VecTy, true),
                       Op->getOperand(2), Op->getOperand(1));
  }
  case Intrinsic::mips_binsri_b:
  case Intrinsic::mips_binsri_h:
  case Intrinsic::mips_binsri_w:
  case Intrinsic::mips_binsri_d: {
    // binsri_x(IfClear, IfSet, nbits) -> (vselect RBitsMask, IfSet, IfClear)
    EVT VecTy = Op->getValueType(0);
    EVT EltTy = VecTy.getVectorElementType();
    if (Op->getConstantOperandVal(3) >= EltTy.getSizeInBits())
      report_fatal_error("Immediate out of range");
    APInt Mask = APInt::getLowBitsSet(EltTy.getSizeInBits(),
                                      Op->getConstantOperandVal(3) + 1);
    return DAG.getNode(ISD::VSELECT, DL, VecTy,
                       DAG.getConstant(Mask, DL, VecTy, true),
                       Op->getOperand(2), Op->getOperand(1));
  }
  case Intrinsic::mips_bmnz_v:
    return DAG.getNode(ISD::VSELECT, DL, Op->getValueType(0), Op->getOperand(3),
                       Op->getOperand(2), Op->getOperand(1));
  case Intrinsic::mips_bmnzi_b:
    return DAG.getNode(ISD::VSELECT, DL, Op->getValueType(0),
                       lowerMSASplatImm(Op, 3, DAG), Op->getOperand(2),
                       Op->getOperand(1));
  case Intrinsic::mips_bmz_v:
    return DAG.getNode(ISD::VSELECT, DL, Op->getValueType(0), Op->getOperand(3),
                       Op->getOperand(1), Op->getOperand(2));
  case Intrinsic::mips_bmzi_b:
    return DAG.getNode(ISD::VSELECT, DL, Op->getValueType(0),
                       lowerMSASplatImm(Op, 3, DAG), Op->getOperand(1),
                       Op->getOperand(2));
  case Intrinsic::mips_bneg_b:
  case Intrinsic::mips_bneg_h:
  case Intrinsic::mips_bneg_w:
  case Intrinsic::mips_bneg_d: {
    EVT VecTy = Op->getValueType(0);
    SDValue One = DAG.getConstant(1, DL, VecTy);

    return DAG.getNode(ISD::XOR, DL, VecTy, Op->getOperand(1),
                       DAG.getNode(ISD::SHL, DL, VecTy, One,
                                   truncateVecElts(Op, DAG)));
  }
  case Intrinsic::mips_bnegi_b:
  case Intrinsic::mips_bnegi_h:
  case Intrinsic::mips_bnegi_w:
  case Intrinsic::mips_bnegi_d:
    return lowerMSABinaryBitImmIntr(Op, DAG, ISD::XOR, Op->getOperand(2),
                                    !Subtarget.isLittle());
  case Intrinsic::mips_bnz_b:
  case Intrinsic::mips_bnz_h:
  case Intrinsic::mips_bnz_w:
  case Intrinsic::mips_bnz_d:
    return DAG.getNode(MipsISD::VALL_NONZERO, DL, Op->getValueType(0),
                       Op->getOperand(1));
  case Intrinsic::mips_bnz_v:
    return DAG.getNode(MipsISD::VANY_NONZERO, DL, Op->getValueType(0),
                       Op->getOperand(1));
  case Intrinsic::mips_bsel_v:
    // bsel_v(Mask, IfClear, IfSet) -> (vselect Mask, IfSet, IfClear)
    return DAG.getNode(ISD::VSELECT, DL, Op->getValueType(0),
                       Op->getOperand(1), Op->getOperand(3),
                       Op->getOperand(2));
  case Intrinsic::mips_bseli_b:
    // bseli_v(Mask, IfClear, IfSet) -> (vselect Mask, IfSet, IfClear)
    return DAG.getNode(ISD::VSELECT, DL, Op->getValueType(0),
                       Op->getOperand(1), lowerMSASplatImm(Op, 3, DAG),
                       Op->getOperand(2));
  case Intrinsic::mips_bset_b:
  case Intrinsic::mips_bset_h:
  case Intrinsic::mips_bset_w:
  case Intrinsic::mips_bset_d: {
    EVT VecTy = Op->getValueType(0);
    SDValue One = DAG.getConstant(1, DL, VecTy);

    return DAG.getNode(ISD::OR, DL, VecTy, Op->getOperand(1),
                       DAG.getNode(ISD::SHL, DL, VecTy, One,
                                   truncateVecElts(Op, DAG)));
  }
  case Intrinsic::mips_bseti_b:
  case Intrinsic::mips_bseti_h:
  case Intrinsic::mips_bseti_w:
  case Intrinsic::mips_bseti_d:
    return lowerMSABinaryBitImmIntr(Op, DAG, ISD::OR, Op->getOperand(2),
                                    !Subtarget.isLittle());
  case Intrinsic::mips_bz_b:
  case Intrinsic::mips_bz_h:
  case Intrinsic::mips_bz_w:
  case Intrinsic::mips_bz_d:
    return DAG.getNode(MipsISD::VALL_ZERO, DL, Op->getValueType(0),
                       Op->getOperand(1));
  case Intrinsic::mips_bz_v:
    return DAG.getNode(MipsISD::VANY_ZERO, DL, Op->getValueType(0),
                       Op->getOperand(1));
  case Intrinsic::mips_ceq_b:
  case Intrinsic::mips_ceq_h:
  case Intrinsic::mips_ceq_w:
  case Intrinsic::mips_ceq_d:
    return DAG.getSetCC(DL, Op->getValueType(0), Op->getOperand(1),
                        Op->getOperand(2), ISD::SETEQ);
  case Intrinsic::mips_ceqi_b:
  case Intrinsic::mips_ceqi_h:
  case Intrinsic::mips_ceqi_w:
  case Intrinsic::mips_ceqi_d:
    return DAG.getSetCC(DL, Op->getValueType(0), Op->getOperand(1),
                        lowerMSASplatImm(Op, 2, DAG, true), ISD::SETEQ);
  case Intrinsic::mips_cle_s_b:
  case Intrinsic::mips_cle_s_h:
  case Intrinsic::mips_cle_s_w:
  case Intrinsic::mips_cle_s_d:
    return DAG.getSetCC(DL, Op->getValueType(0), Op->getOperand(1),
                        Op->getOperand(2), ISD::SETLE);
  case Intrinsic::mips_clei_s_b:
  case Intrinsic::mips_clei_s_h:
  case Intrinsic::mips_clei_s_w:
  case Intrinsic::mips_clei_s_d:
    return DAG.getSetCC(DL, Op->getValueType(0), Op->getOperand(1),
                        lowerMSASplatImm(Op, 2, DAG, true), ISD::SETLE);
  case Intrinsic::mips_cle_u_b:
  case Intrinsic::mips_cle_u_h:
  case Intrinsic::mips_cle_u_w:
  case Intrinsic::mips_cle_u_d:
    return DAG.getSetCC(DL, Op->getValueType(0), Op->getOperand(1),
                        Op->getOperand(2), ISD::SETULE);
  case Intrinsic::mips_clei_u_b:
  case Intrinsic::mips_clei_u_h:
  case Intrinsic::mips_clei_u_w:
  case Intrinsic::mips_clei_u_d:
    return DAG.getSetCC(DL, Op->getValueType(0), Op->getOperand(1),
                        lowerMSASplatImm(Op, 2, DAG), ISD::SETULE);
  case Intrinsic::mips_clt_s_b:
  case Intrinsic::mips_clt_s_h:
  case Intrinsic::mips_clt_s_w:
  case Intrinsic::mips_clt_s_d:
    return DAG.getSetCC(DL, Op->getValueType(0), Op->getOperand(1),
                        Op->getOperand(2), ISD::SETLT);
  case Intrinsic::mips_clti_s_b:
  case Intrinsic::mips_clti_s_h:
  case Intrinsic::mips_clti_s_w:
  case Intrinsic::mips_clti_s_d:
    return DAG.getSetCC(DL, Op->getValueType(0), Op->getOperand(1),
                        lowerMSASplatImm(Op, 2, DAG, true), ISD::SETLT);
  case Intrinsic::mips_clt_u_b:
  case Intrinsic::mips_clt_u_h:
  case Intrinsic::mips_clt_u_w:
  case Intrinsic::mips_clt_u_d:
    return DAG.getSetCC(DL, Op->getValueType(0), Op->getOperand(1),
                        Op->getOperand(2), ISD::SETULT);
  case Intrinsic::mips_clti_u_b:
  case Intrinsic::mips_clti_u_h:
  case Intrinsic::mips_clti_u_w:
  case Intrinsic::mips_clti_u_d:
    return DAG.getSetCC(DL, Op->getValueType(0), Op->getOperand(1),
                        lowerMSASplatImm(Op, 2, DAG), ISD::SETULT);
  case Intrinsic::mips_copy_s_b:
  case Intrinsic::mips_copy_s_h:
  case Intrinsic::mips_copy_s_w:
    return lowerMSACopyIntr(Op, DAG, MipsISD::VEXTRACT_SEXT_ELT);
  case Intrinsic::mips_copy_s_d:
    if (Subtarget.hasMips64())
      // Lower directly into VEXTRACT_SEXT_ELT since i64 is legal on Mips64.
      return lowerMSACopyIntr(Op, DAG, MipsISD::VEXTRACT_SEXT_ELT);
    else {
      // Lower into the generic EXTRACT_VECTOR_ELT node and let the type
      // legalizer and EXTRACT_VECTOR_ELT lowering sort it out.
      return DAG.getNode(ISD::EXTRACT_VECTOR_ELT, SDLoc(Op),
                         Op->getValueType(0), Op->getOperand(1),
                         Op->getOperand(2));
    }
  case Intrinsic::mips_copy_u_b:
  case Intrinsic::mips_copy_u_h:
  case Intrinsic::mips_copy_u_w:
    return lowerMSACopyIntr(Op, DAG, MipsISD::VEXTRACT_ZEXT_ELT);
  case Intrinsic::mips_copy_u_d:
    if (Subtarget.hasMips64())
      // Lower directly into VEXTRACT_ZEXT_ELT since i64 is legal on Mips64.
      return lowerMSACopyIntr(Op, DAG, MipsISD::VEXTRACT_ZEXT_ELT);
    else {
      // Lower into the generic EXTRACT_VECTOR_ELT node and let the type
      // legalizer and EXTRACT_VECTOR_ELT lowering sort it out.
      // Note: When i64 is illegal, this results in copy_s.w instructions
      // instead of copy_u.w instructions. This makes no difference to the
      // behaviour since i64 is only illegal when the register file is 32-bit.
      return DAG.getNode(ISD::EXTRACT_VECTOR_ELT, SDLoc(Op),
                         Op->getValueType(0), Op->getOperand(1),
                         Op->getOperand(2));
    }
  case Intrinsic::mips_div_s_b:
  case Intrinsic::mips_div_s_h:
  case Intrinsic::mips_div_s_w:
  case Intrinsic::mips_div_s_d:
    return DAG.getNode(ISD::SDIV, DL, Op->getValueType(0), Op->getOperand(1),
                       Op->getOperand(2));
  case Intrinsic::mips_div_u_b:
  case Intrinsic::mips_div_u_h:
  case Intrinsic::mips_div_u_w:
  case Intrinsic::mips_div_u_d:
    return DAG.getNode(ISD::UDIV, DL, Op->getValueType(0), Op->getOperand(1),
                       Op->getOperand(2));
  case Intrinsic::mips_fadd_w:
  case Intrinsic::mips_fadd_d:
    return DAG.getNode(ISD::FADD, DL, Op->getValueType(0), Op->getOperand(1),
                       Op->getOperand(2), Op->getFlags());
  // Don't lower mips_fcaf_[wd] since LLVM folds SETFALSE condcodes away
  case Intrinsic::mips_fceq_w:
  case Intrinsic::mips_fceq_d:
    return DAG.getSetCC(DL, Op->getValueType(0), Op->getOperand(1),
                        Op->getOperand(2), ISD::SETOEQ);
  case Intrinsic::mips_fcle_w:
  case Intrinsic::mips_fcle_d:
    return DAG.getSetCC(DL, Op->getValueType(0), Op->getOperand(1),
                        Op->getOperand(2), ISD::SETOLE);
  case Intrinsic::mips_fclt_w:
  case Intrinsic::mips_fclt_d:
    return DAG.getSetCC(DL, Op->getValueType(0), Op->getOperand(1),
                        Op->getOperand(2), ISD::SETOLT);
  case Intrinsic::mips_fcne_w:
  case Intrinsic::mips_fcne_d:
    return DAG.getSetCC(DL, Op->getValueType(0), Op->getOperand(1),
                        Op->getOperand(2), ISD::SETONE);
  case Intrinsic::mips_fcor_w:
  case Intrinsic::mips_fcor_d:
    return DAG.getSetCC(DL, Op->getValueType(0), Op->getOperand(1),
                        Op->getOperand(2), ISD::SETO);
  case Intrinsic::mips_fcueq_w:
  case Intrinsic::mips_fcueq_d:
    return DAG.getSetCC(DL, Op->getValueType(0), Op->getOperand(1),
                        Op->getOperand(2), ISD::SETUEQ);
  case Intrinsic::mips_fcule_w:
  case Intrinsic::mips_fcule_d:
    return DAG.getSetCC(DL, Op->getValueType(0), Op->getOperand(1),
                        Op->getOperand(2), ISD::SETULE);
  case Intrinsic::mips_fcult_w:
  case Intrinsic::mips_fcult_d:
    return DAG.getSetCC(DL, Op->getValueType(0), Op->getOperand(1),
                        Op->getOperand(2), ISD::SETULT);
  case Intrinsic::mips_fcun_w:
  case Intrinsic::mips_fcun_d:
    return DAG.getSetCC(DL, Op->getValueType(0), Op->getOperand(1),
                        Op->getOperand(2), ISD::SETUO);
  case Intrinsic::mips_fcune_w:
  case Intrinsic::mips_fcune_d:
    return DAG.getSetCC(DL, Op->getValueType(0), Op->getOperand(1),
                        Op->getOperand(2), ISD::SETUNE);
  case Intrinsic::mips_fdiv_w:
  case Intrinsic::mips_fdiv_d:
    // TODO: If intrinsics have fast-math-flags, propagate them.
    return DAG.getNode(ISD::FDIV, DL, Op->getValueType(0), Op->getOperand(1),
                       Op->getOperand(2));
  case Intrinsic::mips_ffint_u_w:
  case Intrinsic::mips_ffint_u_d:
    return DAG.getNode(ISD::UINT_TO_FP, DL, Op->getValueType(0),
                       Op->getOperand(1));
  case Intrinsic::mips_ffint_s_w:
  case Intrinsic::mips_ffint_s_d:
    return DAG.getNode(ISD::SINT_TO_FP, DL, Op->getValueType(0),
                       Op->getOperand(1));
  case Intrinsic::mips_fill_b:
  case Intrinsic::mips_fill_h:
  case Intrinsic::mips_fill_w:
  case Intrinsic::mips_fill_d: {
    EVT ResTy = Op->getValueType(0);
    SmallVector<SDValue, 16> Ops(ResTy.getVectorNumElements(),
                                 Op->getOperand(1));

    // If ResTy is v2i64 then the type legalizer will break this node down into
    // an equivalent v4i32.
    return DAG.getBuildVector(ResTy, DL, Ops);
  }
  case Intrinsic::mips_fexp2_w:
  case Intrinsic::mips_fexp2_d: {
    // TODO: If intrinsics have fast-math-flags, propagate them.
    EVT ResTy = Op->getValueType(0);
    return DAG.getNode(
        ISD::FMUL, SDLoc(Op), ResTy, Op->getOperand(1),
        DAG.getNode(ISD::FEXP2, SDLoc(Op), ResTy, Op->getOperand(2)));
  }
  case Intrinsic::mips_flog2_w:
  case Intrinsic::mips_flog2_d:
    return DAG.getNode(ISD::FLOG2, DL, Op->getValueType(0), Op->getOperand(1));
  case Intrinsic::mips_fmadd_w:
  case Intrinsic::mips_fmadd_d:
    return DAG.getNode(ISD::FMA, SDLoc(Op), Op->getValueType(0),
                       Op->getOperand(1), Op->getOperand(2), Op->getOperand(3));
  case Intrinsic::mips_fmul_w:
  case Intrinsic::mips_fmul_d:
    return DAG.getNode(ISD::FMUL, DL, Op->getValueType(0), Op->getOperand(1),
                       Op->getOperand(2), Op->getFlags());
  case Intrinsic::mips_fmsub_w:
  case Intrinsic::mips_fmsub_d: {
    // TODO: If intrinsics have fast-math-flags, propagate them.
    return DAG.getNode(MipsISD::FMS, SDLoc(Op), Op->getValueType(0),
                       Op->getOperand(1), Op->getOperand(2), Op->getOperand(3));
  }
  case Intrinsic::mips_frint_w:
  case Intrinsic::mips_frint_d:
    return DAG.getNode(ISD::FRINT, DL, Op->getValueType(0), Op->getOperand(1));
  case Intrinsic::mips_fsqrt_w:
  case Intrinsic::mips_fsqrt_d:
    return DAG.getNode(ISD::FSQRT, DL, Op->getValueType(0), Op->getOperand(1));
  case Intrinsic::mips_fsub_w:
  case Intrinsic::mips_fsub_d:
    return DAG.getNode(ISD::FSUB, DL, Op->getValueType(0), Op->getOperand(1),
                       Op->getOperand(2), Op->getFlags());
  case Intrinsic::mips_ftrunc_u_w:
  case Intrinsic::mips_ftrunc_u_d:
    return DAG.getNode(ISD::FP_TO_UINT, DL, Op->getValueType(0),
                       Op->getOperand(1));
  case Intrinsic::mips_ftrunc_s_w:
  case Intrinsic::mips_ftrunc_s_d:
    return DAG.getNode(ISD::FP_TO_SINT, DL, Op->getValueType(0),
                       Op->getOperand(1));
  case Intrinsic::mips_ilvev_b:
  case Intrinsic::mips_ilvev_h:
  case Intrinsic::mips_ilvev_w:
  case Intrinsic::mips_ilvev_d:
    return DAG.getNode(MipsISD::ILVEV, DL, Op->getValueType(0),
                       Op->getOperand(1), Op->getOperand(2));
  case Intrinsic::mips_ilvl_b:
  case Intrinsic::mips_ilvl_h:
  case Intrinsic::mips_ilvl_w:
  case Intrinsic::mips_ilvl_d:
    return DAG.getNode(MipsISD::ILVL, DL, Op->getValueType(0),
                       Op->getOperand(1), Op->getOperand(2));
  case Intrinsic::mips_ilvod_b:
  case Intrinsic::mips_ilvod_h:
  case Intrinsic::mips_ilvod_w:
  case Intrinsic::mips_ilvod_d:
    return DAG.getNode(MipsISD::ILVOD, DL, Op->getValueType(0),
                       Op->getOperand(1), Op->getOperand(2));
  case Intrinsic::mips_ilvr_b:
  case Intrinsic::mips_ilvr_h:
  case Intrinsic::mips_ilvr_w:
  case Intrinsic::mips_ilvr_d:
    return DAG.getNode(MipsISD::ILVR, DL, Op->getValueType(0),
                       Op->getOperand(1), Op->getOperand(2));
  case Intrinsic::mips_insert_b:
  case Intrinsic::mips_insert_h:
  case Intrinsic::mips_insert_w:
  case Intrinsic::mips_insert_d:
    return DAG.getNode(ISD::INSERT_VECTOR_ELT, SDLoc(Op), Op->getValueType(0),
                       Op->getOperand(1), Op->getOperand(3), Op->getOperand(2));
  case Intrinsic::mips_insve_b:
  case Intrinsic::mips_insve_h:
  case Intrinsic::mips_insve_w:
  case Intrinsic::mips_insve_d: {
    // Report an error for out of range values.
    int64_t Max;
    switch (Intrinsic) {
    case Intrinsic::mips_insve_b: Max = 15; break;
    case Intrinsic::mips_insve_h: Max = 7; break;
    case Intrinsic::mips_insve_w: Max = 3; break;
    case Intrinsic::mips_insve_d: Max = 1; break;
    default: llvm_unreachable("Unmatched intrinsic");
    }
    int64_t Value = cast<ConstantSDNode>(Op->getOperand(2))->getSExtValue();
    if (Value < 0 || Value > Max)
      report_fatal_error("Immediate out of range");
    return DAG.getNode(MipsISD::INSVE, DL, Op->getValueType(0),
                       Op->getOperand(1), Op->getOperand(2), Op->getOperand(3),
                       DAG.getConstant(0, DL, MVT::i32));
    }
  case Intrinsic::mips_ldi_b:
  case Intrinsic::mips_ldi_h:
  case Intrinsic::mips_ldi_w:
  case Intrinsic::mips_ldi_d:
    return lowerMSASplatImm(Op, 1, DAG, true);
  case Intrinsic::mips_lsa:
  case Intrinsic::mips_dlsa: {
    EVT ResTy = Op->getValueType(0);
    return DAG.getNode(ISD::ADD, SDLoc(Op), ResTy, Op->getOperand(1),
                       DAG.getNode(ISD::SHL, SDLoc(Op), ResTy,
                                   Op->getOperand(2), Op->getOperand(3)));
  }
  case Intrinsic::mips_maddv_b:
  case Intrinsic::mips_maddv_h:
  case Intrinsic::mips_maddv_w:
  case Intrinsic::mips_maddv_d: {
    EVT ResTy = Op->getValueType(0);
    return DAG.getNode(ISD::ADD, SDLoc(Op), ResTy, Op->getOperand(1),
                       DAG.getNode(ISD::MUL, SDLoc(Op), ResTy,
                                   Op->getOperand(2), Op->getOperand(3)));
  }
  case Intrinsic::mips_max_s_b:
  case Intrinsic::mips_max_s_h:
  case Intrinsic::mips_max_s_w:
  case Intrinsic::mips_max_s_d:
    return DAG.getNode(ISD::SMAX, DL, Op->getValueType(0),
                       Op->getOperand(1), Op->getOperand(2));
  case Intrinsic::mips_max_u_b:
  case Intrinsic::mips_max_u_h:
  case Intrinsic::mips_max_u_w:
  case Intrinsic::mips_max_u_d:
    return DAG.getNode(ISD::UMAX, DL, Op->getValueType(0),
                       Op->getOperand(1), Op->getOperand(2));
  case Intrinsic::mips_maxi_s_b:
  case Intrinsic::mips_maxi_s_h:
  case Intrinsic::mips_maxi_s_w:
  case Intrinsic::mips_maxi_s_d:
    return DAG.getNode(ISD::SMAX, DL, Op->getValueType(0),
                       Op->getOperand(1), lowerMSASplatImm(Op, 2, DAG, true));
  case Intrinsic::mips_maxi_u_b:
  case Intrinsic::mips_maxi_u_h:
  case Intrinsic::mips_maxi_u_w:
  case Intrinsic::mips_maxi_u_d:
    return DAG.getNode(ISD::UMAX, DL, Op->getValueType(0),
                       Op->getOperand(1), lowerMSASplatImm(Op, 2, DAG));
  case Intrinsic::mips_min_s_b:
  case Intrinsic::mips_min_s_h:
  case Intrinsic::mips_min_s_w:
  case Intrinsic::mips_min_s_d:
    return DAG.getNode(ISD::SMIN, DL, Op->getValueType(0),
                       Op->getOperand(1), Op->getOperand(2));
  case Intrinsic::mips_min_u_b:
  case Intrinsic::mips_min_u_h:
  case Intrinsic::mips_min_u_w:
  case Intrinsic::mips_min_u_d:
    return DAG.getNode(ISD::UMIN, DL, Op->getValueType(0),
                       Op->getOperand(1), Op->getOperand(2));
  case Intrinsic::mips_mini_s_b:
  case Intrinsic::mips_mini_s_h:
  case Intrinsic::mips_mini_s_w:
  case Intrinsic::mips_mini_s_d:
    return DAG.getNode(ISD::SMIN, DL, Op->getValueType(0),
                       Op->getOperand(1), lowerMSASplatImm(Op, 2, DAG, true));
  case Intrinsic::mips_mini_u_b:
  case Intrinsic::mips_mini_u_h:
  case Intrinsic::mips_mini_u_w:
  case Intrinsic::mips_mini_u_d:
    return DAG.getNode(ISD::UMIN, DL, Op->getValueType(0),
                       Op->getOperand(1), lowerMSASplatImm(Op, 2, DAG));
  case Intrinsic::mips_mod_s_b:
  case Intrinsic::mips_mod_s_h:
  case Intrinsic::mips_mod_s_w:
  case Intrinsic::mips_mod_s_d:
    return DAG.getNode(ISD::SREM, DL, Op->getValueType(0), Op->getOperand(1),
                       Op->getOperand(2));
  case Intrinsic::mips_mod_u_b:
  case Intrinsic::mips_mod_u_h:
  case Intrinsic::mips_mod_u_w:
  case Intrinsic::mips_mod_u_d:
    return DAG.getNode(ISD::UREM, DL, Op->getValueType(0), Op->getOperand(1),
                       Op->getOperand(2));
  case Intrinsic::mips_mulv_b:
  case Intrinsic::mips_mulv_h:
  case Intrinsic::mips_mulv_w:
  case Intrinsic::mips_mulv_d:
    return DAG.getNode(ISD::MUL, DL, Op->getValueType(0), Op->getOperand(1),
                       Op->getOperand(2));
  case Intrinsic::mips_msubv_b:
  case Intrinsic::mips_msubv_h:
  case Intrinsic::mips_msubv_w:
  case Intrinsic::mips_msubv_d: {
    EVT ResTy = Op->getValueType(0);
    return DAG.getNode(ISD::SUB, SDLoc(Op), ResTy, Op->getOperand(1),
                       DAG.getNode(ISD::MUL, SDLoc(Op), ResTy,
                                   Op->getOperand(2), Op->getOperand(3)));
  }
  case Intrinsic::mips_nlzc_b:
  case Intrinsic::mips_nlzc_h:
  case Intrinsic::mips_nlzc_w:
  case Intrinsic::mips_nlzc_d:
    return DAG.getNode(ISD::CTLZ, DL, Op->getValueType(0), Op->getOperand(1));
  case Intrinsic::mips_nor_v: {
    SDValue Res = DAG.getNode(ISD::OR, DL, Op->getValueType(0),
                              Op->getOperand(1), Op->getOperand(2));
    return DAG.getNOT(DL, Res, Res->getValueType(0));
  }
  case Intrinsic::mips_nori_b: {
    SDValue Res =  DAG.getNode(ISD::OR, DL, Op->getValueType(0),
                               Op->getOperand(1),
                               lowerMSASplatImm(Op, 2, DAG));
    return DAG.getNOT(DL, Res, Res->getValueType(0));
  }
  case Intrinsic::mips_or_v:
    return DAG.getNode(ISD::OR, DL, Op->getValueType(0), Op->getOperand(1),
                       Op->getOperand(2));
  case Intrinsic::mips_ori_b:
    return DAG.getNode(ISD::OR, DL, Op->getValueType(0),
                       Op->getOperand(1), lowerMSASplatImm(Op, 2, DAG));
  case Intrinsic::mips_pckev_b:
  case Intrinsic::mips_pckev_h:
  case Intrinsic::mips_pckev_w:
  case Intrinsic::mips_pckev_d:
    return DAG.getNode(MipsISD::PCKEV, DL, Op->getValueType(0),
                       Op->getOperand(1), Op->getOperand(2));
  case Intrinsic::mips_pckod_b:
  case Intrinsic::mips_pckod_h:
  case Intrinsic::mips_pckod_w:
  case Intrinsic::mips_pckod_d:
    return DAG.getNode(MipsISD::PCKOD, DL, Op->getValueType(0),
                       Op->getOperand(1), Op->getOperand(2));
  case Intrinsic::mips_pcnt_b:
  case Intrinsic::mips_pcnt_h:
  case Intrinsic::mips_pcnt_w:
  case Intrinsic::mips_pcnt_d:
    return DAG.getNode(ISD::CTPOP, DL, Op->getValueType(0), Op->getOperand(1));
  case Intrinsic::mips_sat_s_b:
  case Intrinsic::mips_sat_s_h:
  case Intrinsic::mips_sat_s_w:
  case Intrinsic::mips_sat_s_d:
  case Intrinsic::mips_sat_u_b:
  case Intrinsic::mips_sat_u_h:
  case Intrinsic::mips_sat_u_w:
  case Intrinsic::mips_sat_u_d: {
    // Report an error for out of range values.
    int64_t Max;
    switch (Intrinsic) {
    case Intrinsic::mips_sat_s_b:
    case Intrinsic::mips_sat_u_b: Max = 7;  break;
    case Intrinsic::mips_sat_s_h:
    case Intrinsic::mips_sat_u_h: Max = 15; break;
    case Intrinsic::mips_sat_s_w:
    case Intrinsic::mips_sat_u_w: Max = 31; break;
    case Intrinsic::mips_sat_s_d:
    case Intrinsic::mips_sat_u_d: Max = 63; break;
    default: llvm_unreachable("Unmatched intrinsic");
    }
    int64_t Value = cast<ConstantSDNode>(Op->getOperand(2))->getSExtValue();
    if (Value < 0 || Value > Max)
      report_fatal_error("Immediate out of range");
    return SDValue();
  }
  case Intrinsic::mips_shf_b:
  case Intrinsic::mips_shf_h:
  case Intrinsic::mips_shf_w: {
    int64_t Value = cast<ConstantSDNode>(Op->getOperand(2))->getSExtValue();
    if (Value < 0 || Value > 255)
      report_fatal_error("Immediate out of range");
    return DAG.getNode(MipsISD::SHF, DL, Op->getValueType(0),
                       Op->getOperand(2), Op->getOperand(1));
  }
  case Intrinsic::mips_sldi_b:
  case Intrinsic::mips_sldi_h:
  case Intrinsic::mips_sldi_w:
  case Intrinsic::mips_sldi_d: {
    // Report an error for out of range values.
    int64_t Max;
    switch (Intrinsic) {
    case Intrinsic::mips_sldi_b: Max = 15; break;
    case Intrinsic::mips_sldi_h: Max = 7; break;
    case Intrinsic::mips_sldi_w: Max = 3; break;
    case Intrinsic::mips_sldi_d: Max = 1; break;
    default: llvm_unreachable("Unmatched intrinsic");
    }
    int64_t Value = cast<ConstantSDNode>(Op->getOperand(3))->getSExtValue();
    if (Value < 0 || Value > Max)
      report_fatal_error("Immediate out of range");
    return SDValue();
  }
  case Intrinsic::mips_sll_b:
  case Intrinsic::mips_sll_h:
  case Intrinsic::mips_sll_w:
  case Intrinsic::mips_sll_d:
    return DAG.getNode(ISD::SHL, DL, Op->getValueType(0), Op->getOperand(1),
                       truncateVecElts(Op, DAG));
  case Intrinsic::mips_slli_b:
  case Intrinsic::mips_slli_h:
  case Intrinsic::mips_slli_w:
  case Intrinsic::mips_slli_d:
    return DAG.getNode(ISD::SHL, DL, Op->getValueType(0),
                       Op->getOperand(1), lowerMSASplatImm(Op, 2, DAG));
  case Intrinsic::mips_splat_b:
  case Intrinsic::mips_splat_h:
  case Intrinsic::mips_splat_w:
  case Intrinsic::mips_splat_d:
    // We can't lower via VECTOR_SHUFFLE because it requires constant shuffle
    // masks, nor can we lower via BUILD_VECTOR & EXTRACT_VECTOR_ELT because
    // EXTRACT_VECTOR_ELT can't extract i64's on MIPS32.
    // Instead we lower to MipsISD::VSHF and match from there.
    return DAG.getNode(MipsISD::VSHF, DL, Op->getValueType(0),
                       lowerMSASplatZExt(Op, 2, DAG), Op->getOperand(1),
                       Op->getOperand(1));
  case Intrinsic::mips_splati_b:
  case Intrinsic::mips_splati_h:
  case Intrinsic::mips_splati_w:
  case Intrinsic::mips_splati_d:
    return DAG.getNode(MipsISD::VSHF, DL, Op->getValueType(0),
                       lowerMSASplatImm(Op, 2, DAG), Op->getOperand(1),
                       Op->getOperand(1));
  case Intrinsic::mips_sra_b:
  case Intrinsic::mips_sra_h:
  case Intrinsic::mips_sra_w:
  case Intrinsic::mips_sra_d:
    return DAG.getNode(ISD::SRA, DL, Op->getValueType(0), Op->getOperand(1),
                       truncateVecElts(Op, DAG));
  case Intrinsic::mips_srai_b:
  case Intrinsic::mips_srai_h:
  case Intrinsic::mips_srai_w:
  case Intrinsic::mips_srai_d:
    return DAG.getNode(ISD::SRA, DL, Op->getValueType(0),
                       Op->getOperand(1), lowerMSASplatImm(Op, 2, DAG));
  case Intrinsic::mips_srari_b:
  case Intrinsic::mips_srari_h:
  case Intrinsic::mips_srari_w:
  case Intrinsic::mips_srari_d: {
    // Report an error for out of range values.
    int64_t Max;
    switch (Intrinsic) {
    case Intrinsic::mips_srari_b: Max = 7; break;
    case Intrinsic::mips_srari_h: Max = 15; break;
    case Intrinsic::mips_srari_w: Max = 31; break;
    case Intrinsic::mips_srari_d: Max = 63; break;
    default: llvm_unreachable("Unmatched intrinsic");
    }
    int64_t Value = cast<ConstantSDNode>(Op->getOperand(2))->getSExtValue();
    if (Value < 0 || Value > Max)
      report_fatal_error("Immediate out of range");
    return SDValue();
  }
  case Intrinsic::mips_srl_b:
  case Intrinsic::mips_srl_h:
  case Intrinsic::mips_srl_w:
  case Intrinsic::mips_srl_d:
    return DAG.getNode(ISD::SRL, DL, Op->getValueType(0), Op->getOperand(1),
                       truncateVecElts(Op, DAG));
  case Intrinsic::mips_srli_b:
  case Intrinsic::mips_srli_h:
  case Intrinsic::mips_srli_w:
  case Intrinsic::mips_srli_d:
    return DAG.getNode(ISD::SRL, DL, Op->getValueType(0),
                       Op->getOperand(1), lowerMSASplatImm(Op, 2, DAG));
  case Intrinsic::mips_srlri_b:
  case Intrinsic::mips_srlri_h:
  case Intrinsic::mips_srlri_w:
  case Intrinsic::mips_srlri_d: {
    // Report an error for out of range values.
    int64_t Max;
    switch (Intrinsic) {
    case Intrinsic::mips_srlri_b: Max = 7; break;
    case Intrinsic::mips_srlri_h: Max = 15; break;
    case Intrinsic::mips_srlri_w: Max = 31; break;
    case Intrinsic::mips_srlri_d: Max = 63; break;
    default: llvm_unreachable("Unmatched intrinsic");
    }
    int64_t Value = cast<ConstantSDNode>(Op->getOperand(2))->getSExtValue();
    if (Value < 0 || Value > Max)
      report_fatal_error("Immediate out of range");
    return SDValue();
  }
  case Intrinsic::mips_subv_b:
  case Intrinsic::mips_subv_h:
  case Intrinsic::mips_subv_w:
  case Intrinsic::mips_subv_d:
    return DAG.getNode(ISD::SUB, DL, Op->getValueType(0), Op->getOperand(1),
                       Op->getOperand(2));
  case Intrinsic::mips_subvi_b:
  case Intrinsic::mips_subvi_h:
  case Intrinsic::mips_subvi_w:
  case Intrinsic::mips_subvi_d:
    return DAG.getNode(ISD::SUB, DL, Op->getValueType(0),
                       Op->getOperand(1), lowerMSASplatImm(Op, 2, DAG));
  case Intrinsic::mips_vshf_b:
  case Intrinsic::mips_vshf_h:
  case Intrinsic::mips_vshf_w:
  case Intrinsic::mips_vshf_d:
    return DAG.getNode(MipsISD::VSHF, DL, Op->getValueType(0),
                       Op->getOperand(1), Op->getOperand(2), Op->getOperand(3));
  case Intrinsic::mips_xor_v:
    return DAG.getNode(ISD::XOR, DL, Op->getValueType(0), Op->getOperand(1),
                       Op->getOperand(2));
  case Intrinsic::mips_xori_b:
    return DAG.getNode(ISD::XOR, DL, Op->getValueType(0),
                       Op->getOperand(1), lowerMSASplatImm(Op, 2, DAG));
  case Intrinsic::thread_pointer: {
    EVT PtrVT = getPointerTy(DAG.getDataLayout());
    return DAG.getNode(MipsISD::ThreadPointer, DL, PtrVT);
  }
  }
}

static SDValue lowerMSALoadIntr(SDValue Op, SelectionDAG &DAG, unsigned Intr,
                                const MipsSubtarget &Subtarget) {
  SDLoc DL(Op);
  SDValue ChainIn = Op->getOperand(0);
  SDValue Address = Op->getOperand(2);
  SDValue Offset  = Op->getOperand(3);
  EVT ResTy = Op->getValueType(0);
  EVT PtrTy = Address->getValueType(0);

  // For N64 addresses have the underlying type MVT::i64. This intrinsic
  // however takes an i32 signed constant offset. The actual type of the
  // intrinsic is a scaled signed i10.
  if (Subtarget.isABI_N64())
    Offset = DAG.getNode(ISD::SIGN_EXTEND, DL, PtrTy, Offset);

  Address = DAG.getNode(ISD::ADD, DL, PtrTy, Address, Offset);
  return DAG.getLoad(ResTy, DL, ChainIn, Address, MachinePointerInfo(),
                     Align(16));
}

SDValue MipsSETargetLowering::lowerINTRINSIC_W_CHAIN(SDValue Op,
                                                     SelectionDAG &DAG) const {
  unsigned Intr = Op->getConstantOperandVal(1);
  switch (Intr) {
  default:
    return SDValue();
  case Intrinsic::mips_extp:
    return lowerDSPIntr(Op, DAG, MipsISD::EXTP);
  case Intrinsic::mips_extpdp:
    return lowerDSPIntr(Op, DAG, MipsISD::EXTPDP);
  case Intrinsic::mips_extr_w:
    return lowerDSPIntr(Op, DAG, MipsISD::EXTR_W);
  case Intrinsic::mips_extr_r_w:
    return lowerDSPIntr(Op, DAG, MipsISD::EXTR_R_W);
  case Intrinsic::mips_extr_rs_w:
    return lowerDSPIntr(Op, DAG, MipsISD::EXTR_RS_W);
  case Intrinsic::mips_extr_s_h:
    return lowerDSPIntr(Op, DAG, MipsISD::EXTR_S_H);
  case Intrinsic::mips_mthlip:
    return lowerDSPIntr(Op, DAG, MipsISD::MTHLIP);
  case Intrinsic::mips_mulsaq_s_w_ph:
    return lowerDSPIntr(Op, DAG, MipsISD::MULSAQ_S_W_PH);
  case Intrinsic::mips_maq_s_w_phl:
    return lowerDSPIntr(Op, DAG, MipsISD::MAQ_S_W_PHL);
  case Intrinsic::mips_maq_s_w_phr:
    return lowerDSPIntr(Op, DAG, MipsISD::MAQ_S_W_PHR);
  case Intrinsic::mips_maq_sa_w_phl:
    return lowerDSPIntr(Op, DAG, MipsISD::MAQ_SA_W_PHL);
  case Intrinsic::mips_maq_sa_w_phr:
    return lowerDSPIntr(Op, DAG, MipsISD::MAQ_SA_W_PHR);
  case Intrinsic::mips_dpaq_s_w_ph:
    return lowerDSPIntr(Op, DAG, MipsISD::DPAQ_S_W_PH);
  case Intrinsic::mips_dpsq_s_w_ph:
    return lowerDSPIntr(Op, DAG, MipsISD::DPSQ_S_W_PH);
  case Intrinsic::mips_dpaq_sa_l_w:
    return lowerDSPIntr(Op, DAG, MipsISD::DPAQ_SA_L_W);
  case Intrinsic::mips_dpsq_sa_l_w:
    return lowerDSPIntr(Op, DAG, MipsISD::DPSQ_SA_L_W);
  case Intrinsic::mips_dpaqx_s_w_ph:
    return lowerDSPIntr(Op, DAG, MipsISD::DPAQX_S_W_PH);
  case Intrinsic::mips_dpaqx_sa_w_ph:
    return lowerDSPIntr(Op, DAG, MipsISD::DPAQX_SA_W_PH);
  case Intrinsic::mips_dpsqx_s_w_ph:
    return lowerDSPIntr(Op, DAG, MipsISD::DPSQX_S_W_PH);
  case Intrinsic::mips_dpsqx_sa_w_ph:
    return lowerDSPIntr(Op, DAG, MipsISD::DPSQX_SA_W_PH);
  case Intrinsic::mips_ld_b:
  case Intrinsic::mips_ld_h:
  case Intrinsic::mips_ld_w:
  case Intrinsic::mips_ld_d:
   return lowerMSALoadIntr(Op, DAG, Intr, Subtarget);
  }
}

static SDValue lowerMSAStoreIntr(SDValue Op, SelectionDAG &DAG, unsigned Intr,
                                 const MipsSubtarget &Subtarget) {
  SDLoc DL(Op);
  SDValue ChainIn = Op->getOperand(0);
  SDValue Value   = Op->getOperand(2);
  SDValue Address = Op->getOperand(3);
  SDValue Offset  = Op->getOperand(4);
  EVT PtrTy = Address->getValueType(0);

  // For N64 addresses have the underlying type MVT::i64. This intrinsic
  // however takes an i32 signed constant offset. The actual type of the
  // intrinsic is a scaled signed i10.
  if (Subtarget.isABI_N64())
    Offset = DAG.getNode(ISD::SIGN_EXTEND, DL, PtrTy, Offset);

  Address = DAG.getNode(ISD::ADD, DL, PtrTy, Address, Offset);

  return DAG.getStore(ChainIn, DL, Value, Address, MachinePointerInfo(),
                      Align(16));
}

SDValue MipsSETargetLowering::lowerINTRINSIC_VOID(SDValue Op,
                                                  SelectionDAG &DAG) const {
  unsigned Intr = Op->getConstantOperandVal(1);
  switch (Intr) {
  default:
    return SDValue();
  case Intrinsic::mips_st_b:
  case Intrinsic::mips_st_h:
  case Intrinsic::mips_st_w:
  case Intrinsic::mips_st_d:
    return lowerMSAStoreIntr(Op, DAG, Intr, Subtarget);
  }
}

// Lower ISD::EXTRACT_VECTOR_ELT into MipsISD::VEXTRACT_SEXT_ELT.
//
// The non-value bits resulting from ISD::EXTRACT_VECTOR_ELT are undefined. We
// choose to sign-extend but we could have equally chosen zero-extend. The
// DAGCombiner will fold any sign/zero extension of the ISD::EXTRACT_VECTOR_ELT
// result into this node later (possibly changing it to a zero-extend in the
// process).
SDValue MipsSETargetLowering::
lowerEXTRACT_VECTOR_ELT(SDValue Op, SelectionDAG &DAG) const {
  SDLoc DL(Op);
  EVT ResTy = Op->getValueType(0);
  SDValue Op0 = Op->getOperand(0);
  SDValue Op1 = Op->getOperand(1);
  EVT VecTy = Op0->getValueType(0);

  if (!VecTy.is128BitVector())
    return SDValue();

  // R5900 v4i32 extraction with a constant index; the others go through a
  // stack temporary.
  if (Subtarget.isR5900() && VecTy == MVT::v4i32) {
    auto *IdxConst = dyn_cast<ConstantSDNode>(Op1);
    if (!IdxConst || IdxConst->getZExtValue() > 3)
      return SDValue();
    unsigned Idx = IdxConst->getZExtValue();
    SDValue Vec = Op0;
    if (Idx >= 2)
      Vec = SDValue(DAG.getMachineNode(Mips::PCPYUD, DL, MVT::v4i32, Vec, Vec),
                    0);
    // A truncate or an arithmetic shift keeps the i32 sign-extended.
    SDValue Dword = DAG.getTargetExtractSubreg(Mips::sub_64, DL, MVT::i64, Vec);
    if (Idx & 1)
      Dword = DAG.getNode(ISD::SRA, DL, MVT::i64, Dword,
                          DAG.getConstant(32, DL, MVT::i32));
    return DAG.getNode(ISD::TRUNCATE, DL, ResTy, Dword);
  }

  // VU0 v4f32 extraction: emit machine nodes directly
  // This avoids the MipsISD::VEXTRACT_SEXT_ELT node which has no patterns for
  // VU0
  if (Subtarget.hasVU0() && VecTy == MVT::v4f32 && ResTy == MVT::f32) {
    // For now, only handle constant indices
    ConstantSDNode *IdxConst = dyn_cast<ConstantSDNode>(Op1);
    if (!IdxConst)
      return SDValue(); // Variable index not supported yet

    unsigned Idx = IdxConst->getZExtValue();
    if (Idx > 3)
      return SDValue();

    // QMFC2: Move VF register to GPR128
    SDValue GPR128Val =
        SDValue(DAG.getMachineNode(Mips::QMFC2, DL, MVT::v4i32, Op0), 0);

    SDValue Result;
    if (Idx == 0) {
      // Element 0: lower 32 bits - just use the register as-is
      // Extract sub_64 from GPR128 to get GPR64
      SDValue GPR64Val =
          DAG.getTargetExtractSubreg(Mips::sub_64, DL, MVT::i64, GPR128Val);
      // Extract sub_32 from GPR64 to get GPR32
      Result = DAG.getTargetExtractSubreg(Mips::sub_32, DL, MVT::i32, GPR64Val);
    } else if (Idx == 1) {
      // Element 1: bits 32-63 - shift right by 32
      SDValue GPR64Val =
          DAG.getTargetExtractSubreg(Mips::sub_64, DL, MVT::i64, GPR128Val);
      // DSRL32 shifts by sa+32, so sa=0 gives 32-bit shift
      SDValue ShiftAmt = DAG.getTargetConstant(0, DL, MVT::i32);
      SDValue Shifted = SDValue(
          DAG.getMachineNode(Mips::DSRL32, DL, MVT::i64, GPR64Val, ShiftAmt),
          0);
      Result = DAG.getTargetExtractSubreg(Mips::sub_32, DL, MVT::i32, Shifted);
    } else if (Idx == 2) {
      // Element 2: bits 64-95 - PCPYUD to move upper to lower
      SDValue Copied = SDValue(DAG.getMachineNode(Mips::PCPYUD, DL, MVT::v4i32,
                                                  GPR128Val, GPR128Val),
                               0);
      SDValue GPR64Val =
          DAG.getTargetExtractSubreg(Mips::sub_64, DL, MVT::i64, Copied);
      Result = DAG.getTargetExtractSubreg(Mips::sub_32, DL, MVT::i32, GPR64Val);
    } else { // Idx == 3
      // Element 3: bits 96-127 - PCPYUD then shift right by 32
      SDValue Copied = SDValue(DAG.getMachineNode(Mips::PCPYUD, DL, MVT::v4i32,
                                                  GPR128Val, GPR128Val),
                               0);
      SDValue GPR64Val =
          DAG.getTargetExtractSubreg(Mips::sub_64, DL, MVT::i64, Copied);
      SDValue ShiftAmt = DAG.getTargetConstant(0, DL, MVT::i32);
      SDValue Shifted = SDValue(
          DAG.getMachineNode(Mips::DSRL32, DL, MVT::i64, GPR64Val, ShiftAmt),
          0);
      Result = DAG.getTargetExtractSubreg(Mips::sub_32, DL, MVT::i32, Shifted);
    }

    // Move i32 to FPU register via MTC1, then bitcast
    SDValue FPRVal =
        SDValue(DAG.getMachineNode(Mips::MTC1, DL, MVT::f32, Result), 0);
    return FPRVal;
  }

  if (ResTy.isInteger()) {
    EVT EltTy = VecTy.getVectorElementType();
    return DAG.getNode(MipsISD::VEXTRACT_SEXT_ELT, DL, ResTy, Op0, Op1,
                       DAG.getValueType(EltTy));
  }

  return Op;
}

static SDValue lowerVU0BUILD_VECTOR(BuildVectorSDNode *Node, SelectionDAG &DAG);

static cl::opt<bool> VU0FDivSplit(
    "mips-vu0-fdiv-split", cl::Hidden, cl::init(true),
    cl::desc("Divide fields z and w of an R5900 VU0 vector through VDIV, in "
             "parallel with DIV.S for x and y"));

/// VU0 v4f32 division. DIV.S is not pipelined (two take 14 cycles), but it
/// runs in parallel with VDIV (DIV.S and VDIV with VWAITQ: 9; two of each
/// with VADDQ: 20, measured). Fields x and y are divided by DIV.S, z and w
/// through VDIV and Q straight into a vector of their own.
SDValue MipsSETargetLowering::lowerVU0FDIV(SDValue Op,
                                           SelectionDAG &DAG) const {
  if (!VU0FDivSplit)
    return SDValue();
  SDLoc DL(Op);
  SDValue A = Op.getOperand(0), B = Op.getOperand(1);
  auto Field = [&](SDValue V, unsigned I) {
    return DAG.getNode(ISD::EXTRACT_VECTOR_ELT, DL, MVT::f32, V,
                       DAG.getVectorIdxConstant(I, DL));
  };
  SDValue Q0 = DAG.getNode(ISD::FDIV, DL, MVT::f32, Field(A, 0), Field(B, 0),
                           Op->getFlags());
  SDValue Q1 = DAG.getNode(ISD::FDIV, DL, MVT::f32, Field(A, 1), Field(B, 1),
                           Op->getFlags());
  // z and w into a vector of their own, so that the divisions through Q do
  // not wait for DIV.S. VF0 has 0.0 in z, and as the base it means none: all
  // fields are written, the others are replaced below. A zero vector (loop
  // invariant) for w.
  SDValue VF0 = DAG.getRegister(Mips::VF0, MVT::v4f32);
  SDValue ZW = DAG.getNode(MipsISD::VU0_DIVQ, DL, MVT::v4f32,
                           {VF0, VF0, A, B, DAG.getConstant(2, DL, MVT::i32),
                            DAG.getConstant(2, DL, MVT::i32)});
  ZW = DAG.getNode(MipsISD::VU0_DIVQ, DL, MVT::v4f32,
                   {ZW, DAG.getConstantFP(0.0, DL, MVT::v4f32), A, B,
                    DAG.getConstant(3, DL, MVT::i32),
                    DAG.getConstant(1, DL, MVT::i32)});
  // x and y as a pair in a GPR (MFC1, PEXTLW, PCPYLD, QMTC2), then VMOVE.xy;
  // the vector repeats them, since one with undefined fields would go
  // through the stack. Measured with the buffers of Vec Divide 1 KB apart
  // modulo 4 KB: 20.9 MFLOPS, 19.7 with DIV.S only.
  auto *XY = cast<BuildVectorSDNode>(
      DAG.getBuildVector(MVT::v4f32, DL, {Q0, Q1, Q0, Q1}).getNode());
  return DAG.getNode(MipsISD::VU0_BLEND, DL, MVT::v4f32, ZW,
                     lowerVU0BUILD_VECTOR(XY, DAG),
                     DAG.getConstant(0b1100, DL, MVT::i32));
}

SDValue MipsSETargetLowering::lowerINSERT_VECTOR_ELT(SDValue Op,
                                                     SelectionDAG &DAG) const {
  SDLoc DL(Op);
  EVT VecTy = Op->getValueType(0);
  SDValue Vec = Op->getOperand(0);
  SDValue Val = Op->getOperand(1);
  SDValue Idx = Op->getOperand(2);
  EVT ValTy = Val.getValueType();

  if (!VecTy.is128BitVector())
    return SDValue();

  // VU0 v4f32 insertion of a constant element: a vector with only that
  // element defined, built in a GPR, then a VMOVE of that field.
  if (Subtarget.hasVU0() && VecTy == MVT::v4f32 && ValTy == MVT::f32 &&
      isa<ConstantSDNode>(Idx)) {
    unsigned I = Idx->getAsZExtVal();
    if (I > 3)
      return SDValue();
    // A field of another vector: one masked VADDbc (or VMULw for w into w)
    // that writes the field directly, 0 + v.<k>. VF0 has 0.0 in x, y and z,
    // a zero vector (loop invariant) is used for w.
    if (VU0InsertField && !Vec.isUndef() &&
        Val.getOpcode() == ISD::EXTRACT_VECTOR_ELT &&
        Val.getOperand(0).getValueType() == MVT::v4f32 &&
        isa<ConstantSDNode>(Val.getOperand(1)) &&
        Val.getConstantOperandVal(1) < 4) {
      unsigned K = Val.getConstantOperandVal(1);
      SDValue Zero = I < 3 || K == 3 ? DAG.getRegister(Mips::VF0, MVT::v4f32)
                                     : DAG.getConstantFP(0.0, DL, MVT::v4f32);
      return DAG.getNode(MipsISD::VU0_INSERT_FIELD, DL, VecTy, Vec, Zero,
                         Val.getOperand(0), DAG.getConstant(K, DL, MVT::i32),
                         DAG.getConstant(1 << (3 - I), DL, MVT::i32));
    }
    SmallVector<SDValue, 4> Elts(4, DAG.getUNDEF(MVT::f32));
    Elts[I] = Val;
    SDValue Lane = DAG.getBuildVector(MVT::v4f32, DL, Elts);
    if (Vec.isUndef())
      return Lane;
    return DAG.getNode(MipsISD::VU0_BLEND, DL, VecTy, Vec, Lane,
                       DAG.getConstant(1 << (3 - I), DL, MVT::i32));
  }

  // Otherwise bitcast to v4i32, insert i32, bitcast back to v4f32
  if (Subtarget.hasVU0() && VecTy == MVT::v4f32 && ValTy == MVT::f32) {
    // Bitcast v4f32 -> v4i32 (VF register -> GPR128 via QMFC2)
    SDValue IntVec = DAG.getNode(ISD::BITCAST, DL, MVT::v4i32, Vec);
    // Bitcast f32 -> i32
    SDValue IntVal = DAG.getNode(ISD::BITCAST, DL, MVT::i32, Val);
    // Insert i32 element using existing VINSERT_VECTOR_ELT support
    SDValue Result = DAG.getNode(ISD::INSERT_VECTOR_ELT, DL, MVT::v4i32, IntVec,
                                 IntVal, Idx);
    // Bitcast v4i32 -> v4f32 (GPR128 -> VF register via QMTC2)
    return DAG.getNode(ISD::BITCAST, DL, MVT::v4f32, Result);
  }

  return SDValue();
}

// Lower ISD::FMA for VU0 v4f32.
// VU0 has VMADD (fd = ACC + fs * ft) but it requires ACC setup, which is
// handled by the ACC chain optimization pass for suitable patterns.
// For standalone FMA, we lower to FMUL + FADD which are both Legal.
SDValue MipsSETargetLowering::lowerFMA(SDValue Op, SelectionDAG &DAG) const {
  SDLoc DL(Op);
  EVT VT = Op.getValueType();

  // Only handle VU0 v4f32
  if (!Subtarget.hasVU0() || VT != MVT::v4f32)
    return SDValue();

  SDValue A = Op.getOperand(0);
  SDValue B = Op.getOperand(1);
  SDValue C = Op.getOperand(2);

  // FMA(a, b, c) = a * b + c
  SDValue Mul = DAG.getNode(ISD::FMUL, DL, VT, A, B);
  return DAG.getNode(ISD::FADD, DL, VT, Mul, C);
}

static bool isConstantOrUndef(const SDValue Op) {
  if (Op->isUndef())
    return true;
  if (isa<ConstantSDNode>(Op))
    return true;
  if (isa<ConstantFPSDNode>(Op))
    return true;
  return false;
}

static bool isConstantOrUndefBUILD_VECTOR(const BuildVectorSDNode *Op) {
  for (unsigned i = 0; i < Op->getNumOperands(); ++i)
    if (isConstantOrUndef(Op->getOperand(i)))
      return true;
  return false;
}

// Lowers ISD::BUILD_VECTOR into appropriate SelectionDAG nodes for the
// backend.
//
// Lowers according to the following rules:
// - Constant splats are legal as-is as long as the SplatBitSize is a power of
//   2 less than or equal to 64 and the value fits into a signed 10-bit
//   immediate
// - Constant splats are lowered to bitconverted BUILD_VECTORs if SplatBitSize
//   is a power of 2 less than or equal to 64 and the value does not fit into a
//   signed 10-bit immediate
// - Non-constant splats are legal as-is.
// - Non-constant non-splats are lowered to sequences of INSERT_VECTOR_ELT.
// - All others are illegal and must be expanded.
/// R5900: a v4i32 of four words in GPRs (i32 or i64, or undef; zero is
/// $zero): a PEXTLW of each pair of elements and a PCPYLD of the pairs.
static SDValue buildR5900Words(ArrayRef<SDValue> Elts, const SDLoc &DL,
                               SelectionDAG &DAG) {
  auto Undef = [&]() {
    return SDValue(
        DAG.getMachineNode(TargetOpcode::IMPLICIT_DEF, DL, MVT::v4i32), 0);
  };
  auto ToGPR128 = [&](SDValue Elt) {
    if (Elt.isUndef())
      return Undef();
    if (isNullConstant(Elt))
      return DAG.getRegister(Mips::R0_128, MVT::v4i32);
    return DAG.getTargetInsertSubreg(Mips::sub_64, DL, MVT::v4i32, Undef(),
                                     DAG.getAnyExtOrTrunc(Elt, DL, MVT::i64));
  };
  // The low doubleword of the result has Lo in its low word.
  auto Pair = [&](SDValue Lo, SDValue Hi) {
    if (Hi.isUndef())
      return ToGPR128(Lo);
    return SDValue(DAG.getMachineNode(Mips::PEXTLW, DL, MVT::v4i32,
                                      ToGPR128(Hi), ToGPR128(Lo)),
                   0);
  };
  SDValue Result = Pair(Elts[0], Elts[1]);
  if (!Elts[2].isUndef() || !Elts[3].isUndef())
    Result = SDValue(DAG.getMachineNode(Mips::PCPYLD, DL, MVT::v4i32,
                                        Pair(Elts[2], Elts[3]), Result),
                     0);
  return Result;
}

/// R5900: a splat of a register to v4i32, v8i16 or v16i8. The scalar is in
/// the low doubleword of a 128-bit register; PEXTLW/PEXTLB doubles a word or
/// byte to the element below it, PCPYLD copies the low doubleword to the
/// high one and PCPYH the low halfword of each doubleword to its others.
/// All zeros are $zero and all ones PNOR of $zero, and a splat of a mask of
/// the low or high bits of a word or halfword shifts all ones. Another v4i32
/// that is not constant is built with buildR5900Words. Other vectors are
/// expanded, constants to a load from the constant pool.
static SDValue lowerR5900BUILD_VECTOR(BuildVectorSDNode *Node,
                                      SelectionDAG &DAG) {
  MVT VT = Node->getSimpleValueType(0);
  SDLoc DL(Node);
  if (ISD::isBuildVectorAllZeros(Node) || ISD::isBuildVectorAllOnes(Node))
    return SDValue(Node, 0);
  SDValue Scalar = Node->getSplatValue();
  // With undefined elements, building the words is not longer than a splat.
  bool HasUndef =
      any_of(Node->op_values(), [](SDValue V) { return V.isUndef(); });
  if (!Scalar || (VT == MVT::v4i32 && HasUndef && !Node->isConstant())) {
    if (VT != MVT::v4i32 || Node->isConstant())
      return SDValue();
    SmallVector<SDValue, 4> Elts(Node->op_values());
    return buildR5900Words(Elts, DL, DAG);
  }
  if (auto *C = dyn_cast<ConstantSDNode>(Scalar)) {
    if (VT == MVT::v16i8)
      return SDValue();
    unsigned EltBits = VT.getScalarSizeInBits();
    APInt Mask = C->getAPIntValue().trunc(EltBits);
    unsigned Opc, Amount;
    if (Mask.isMask()) {
      Opc = VT == MVT::v4i32 ? Mips::PSRLW : Mips::PSRLH;
      Amount = Mask.countl_zero();
    } else if ((~Mask).isMask()) {
      Opc = VT == MVT::v4i32 ? Mips::PSLLW : Mips::PSLLH;
      Amount = Mask.countr_zero();
    } else {
      return SDValue();
    }
    return SDValue(
        DAG.getMachineNode(Opc, DL, VT, DAG.getAllOnesConstant(DL, VT),
                           DAG.getTargetConstant(Amount, DL, MVT::i32)),
        0);
  }
  SDValue X = DAG.getTargetInsertSubreg(
      Mips::sub_64, DL, VT,
      SDValue(DAG.getMachineNode(TargetOpcode::IMPLICIT_DEF, DL, VT), 0),
      DAG.getAnyExtOrTrunc(Scalar, DL, MVT::i64));
  auto MMI = [&](unsigned Opc, SDValue Rs, SDValue Rt) {
    return SDValue(DAG.getMachineNode(Opc, DL, VT, Rs, Rt), 0);
  };
  auto CopyHalf = [&](SDValue V) {
    return SDValue(DAG.getMachineNode(Mips::PCPYH, DL, VT, V), 0);
  };
  if (VT == MVT::v4i32) {
    SDValue W = MMI(Mips::PEXTLW, X, X);
    return MMI(Mips::PCPYLD, W, W);
  }
  if (VT == MVT::v16i8)
    X = MMI(Mips::PEXTLB, X, X);
  return CopyHalf(MMI(Mips::PCPYLD, X, X));
}

/// An f32 element of \p User as i32: a load of it only used by \p User is a
/// load of an i32 instead, which the DAGCombiner does not do once the i32
/// load is custom lowered.
static SDValue bitcastToI32(SDValue Elt, SDNode *User, SelectionDAG &DAG) {
  auto *Ld = dyn_cast<LoadSDNode>(Elt);
  if (!Ld || !ISD::isNormalLoad(Ld) || !Ld->isSimple() ||
      any_of(Ld->uses(), [&](SDUse &U) {
        return U.getResNo() == 0 && U.getUser() != User;
      }))
    return DAG.getBitcast(MVT::i32, Elt);
  SDValue NewLd = DAG.getLoad(MVT::i32, SDLoc(Ld), Ld->getChain(),
                              Ld->getBasePtr(), Ld->getMemOperand());
  DAG.ReplaceAllUsesOfValueWith(SDValue(Ld, 1), NewLd.getValue(1));
  return NewLd;
}

/// VU0: a v4f32 is built in a 128-bit GPR from its elements as i32, then
/// moved to VU0 with QMTC2. A splat without undefined elements is built as in
/// lowerR5900BUILD_VECTOR, other vectors with buildR5900Words. Constants are
/// loaded from the constant pool.
static SDValue lowerVU0BUILD_VECTOR(BuildVectorSDNode *Node,
                                    SelectionDAG &DAG) {
  SDLoc DL(Node);
  if (Node->isConstant())
    return SDValue();
  SmallVector<SDValue, 4> Elts;
  SmallDenseMap<SDValue, SDValue, 4> AsI32;
  for (SDValue Elt : Node->op_values()) {
    SDValue &I32 = AsI32[Elt];
    if (!I32)
      I32 =
          Elt.isUndef() ? DAG.getUNDEF(MVT::i32) : bitcastToI32(Elt, Node, DAG);
    Elts.push_back(I32);
  }
  SDValue Result;
  if (Node->getSplatValue() &&
      none_of(Elts, [](SDValue V) { return V.isUndef(); })) {
    SDValue BV = DAG.getBuildVector(MVT::v4i32, DL, Elts);
    Result = lowerR5900BUILD_VECTOR(cast<BuildVectorSDNode>(BV), DAG);
  } else {
    Result = buildR5900Words(Elts, DL, DAG);
  }
  return DAG.getBitcast(MVT::v4f32, Result);
}

/// VU0: a scalar in field x (the other fields are undefined) is moved there
/// from a GPR with QMTC2: MFC1 for a value in an FPR, LW for a load.
static SDValue lowerVU0SCALAR_TO_VECTOR(SDValue Op, SelectionDAG &DAG) {
  SDLoc DL(Op);
  SDValue Word = bitcastToI32(Op.getOperand(0), Op.getNode(), DAG);
  SDValue Undef(DAG.getMachineNode(TargetOpcode::IMPLICIT_DEF, DL, MVT::v4i32),
                0);
  SDValue V =
      DAG.getTargetInsertSubreg(Mips::sub_64, DL, MVT::v4i32, Undef,
                                DAG.getAnyExtOrTrunc(Word, DL, MVT::i64));
  return DAG.getBitcast(MVT::v4f32, V);
}

/// VU0: an unsigned conversion is the signed one of X, or for X >= 2^31,
/// the signed one of X - 2^31 with the sign bit set. That one is non-negative
/// exactly for X >= 2^31.
SDValue MipsSETargetLowering::lowerVU0FP_TO_UINT(SDValue Op,
                                                 SelectionDAG &DAG) const {
  SDLoc DL(Op);
  SDValue X = Op.getOperand(0);
  SDValue Lo = DAG.getNode(ISD::FP_TO_SINT, DL, MVT::v4i32, X);
  SDValue Hi =
      DAG.getNode(ISD::FP_TO_SINT, DL, MVT::v4i32,
                  DAG.getNode(ISD::FSUB, DL, MVT::v4f32, X,
                              DAG.getConstantFP(0x1p31, DL, MVT::v4f32)));
  SDValue IsHi = DAG.getSetCC(
      DL, MVT::v4i32, Hi, DAG.getAllOnesConstant(DL, MVT::v4i32), ISD::SETGT);
  SDValue HiResult =
      DAG.getNode(ISD::XOR, DL, MVT::v4i32, Hi,
                  DAG.getConstant(APInt::getSignMask(32), DL, MVT::v4i32));
  return DAG.getSelect(DL, MVT::v4i32, IsHi, HiResult, Lo);
}

SDValue MipsSETargetLowering::lowerBUILD_VECTOR(SDValue Op,
                                                SelectionDAG &DAG) const {
  BuildVectorSDNode *Node = cast<BuildVectorSDNode>(Op);
  EVT ResTy = Op->getValueType(0);
  SDLoc DL(Op);
  APInt SplatValue, SplatUndef;
  unsigned SplatBitSize;
  bool HasAnyUndefs;

  if (Subtarget.isR5900() &&
      (ResTy == MVT::v4i32 || ResTy == MVT::v8i16 || ResTy == MVT::v16i8))
    return lowerR5900BUILD_VECTOR(Node, DAG);

  if (Subtarget.hasVU0() && ResTy == MVT::v4f32)
    return lowerVU0BUILD_VECTOR(Node, DAG);

  if (!Subtarget.hasMSA() || !ResTy.is128BitVector())
    return SDValue();

  if (Node->isConstantSplat(SplatValue, SplatUndef, SplatBitSize,
                            HasAnyUndefs, 8,
                            !Subtarget.isLittle()) && SplatBitSize <= 64) {
    // We can only cope with 8, 16, 32, or 64-bit elements
    if (SplatBitSize != 8 && SplatBitSize != 16 && SplatBitSize != 32 &&
        SplatBitSize != 64)
      return SDValue();

    // If the value isn't an integer type we will have to bitcast
    // from an integer type first. Also, if there are any undefs, we must
    // lower them to defined values first.
    if (ResTy.isInteger() && !HasAnyUndefs)
      return Op;

    EVT ViaVecTy;

    switch (SplatBitSize) {
    default:
      return SDValue();
    case 8:
      ViaVecTy = MVT::v16i8;
      break;
    case 16:
      ViaVecTy = MVT::v8i16;
      break;
    case 32:
      ViaVecTy = MVT::v4i32;
      break;
    case 64:
      // There's no fill.d to fall back on for 64-bit values
      return SDValue();
    }

    // SelectionDAG::getConstant will promote SplatValue appropriately.
    SDValue Result = DAG.getConstant(SplatValue, DL, ViaVecTy);

    // Bitcast to the type we originally wanted
    if (ViaVecTy != ResTy)
      Result = DAG.getNode(ISD::BITCAST, SDLoc(Node), ResTy, Result);

    return Result;
  } else if (DAG.isSplatValue(Op, /* AllowUndefs */ false))
    return Op;
  else if (!isConstantOrUndefBUILD_VECTOR(Node)) {
    // Use INSERT_VECTOR_ELT operations rather than expand to stores.
    // The resulting code is the same length as the expansion, but it doesn't
    // use memory operations
    EVT ResTy = Node->getValueType(0);

    assert(ResTy.isVector());

    unsigned NumElts = ResTy.getVectorNumElements();
    SDValue Vector = DAG.getUNDEF(ResTy);
    for (unsigned i = 0; i < NumElts; ++i) {
      Vector = DAG.getNode(ISD::INSERT_VECTOR_ELT, DL, ResTy, Vector,
                           Node->getOperand(i),
                           DAG.getConstant(i, DL, MVT::i32));
    }
    return Vector;
  }

  return SDValue();
}

// Lower VECTOR_SHUFFLE into SHF (if possible).
//
// SHF splits the vector into blocks of four elements, then shuffles these
// elements according to a <4 x i2> constant (encoded as an integer immediate).
//
// It is therefore possible to lower into SHF when the mask takes the form:
//   <a, b, c, d, a+4, b+4, c+4, d+4, a+8, b+8, c+8, d+8, ...>
// When undef's appear they are treated as if they were whatever value is
// necessary in order to fit the above forms.
//
// For example:
//   %2 = shufflevector <8 x i16> %0, <8 x i16> undef,
//                      <8 x i32> <i32 3, i32 2, i32 1, i32 0,
//                                 i32 7, i32 6, i32 5, i32 4>
// is lowered to:
//   (SHF_H $w0, $w1, 27)
// where the 27 comes from:
//   3 + (2 << 2) + (1 << 4) + (0 << 6)
static SDValue lowerVECTOR_SHUFFLE_SHF(SDValue Op, EVT ResTy,
                                       SmallVector<int, 16> Indices,
                                       SelectionDAG &DAG) {
  int SHFIndices[4] = { -1, -1, -1, -1 };

  if (Indices.size() < 4)
    return SDValue();

  for (unsigned i = 0; i < 4; ++i) {
    for (unsigned j = i; j < Indices.size(); j += 4) {
      int Idx = Indices[j];

      // Convert from vector index to 4-element subvector index
      // If an index refers to an element outside of the subvector then give up
      if (Idx != -1) {
        Idx -= 4 * (j / 4);
        if (Idx < 0 || Idx >= 4)
          return SDValue();
      }

      // If the mask has an undef, replace it with the current index.
      // Note that it might still be undef if the current index is also undef
      if (SHFIndices[i] == -1)
        SHFIndices[i] = Idx;

      // Check that non-undef values are the same as in the mask. If they
      // aren't then give up
      if (!(Idx == -1 || Idx == SHFIndices[i]))
        return SDValue();
    }
  }

  // Calculate the immediate. Replace any remaining undefs with zero
  APInt Imm(32, 0);
  for (int i = 3; i >= 0; --i) {
    int Idx = SHFIndices[i];

    if (Idx == -1)
      Idx = 0;

    Imm <<= 2;
    Imm |= Idx & 0x3;
  }

  SDLoc DL(Op);
  return DAG.getNode(MipsISD::SHF, DL, ResTy,
                     DAG.getTargetConstant(Imm, DL, MVT::i32),
                     Op->getOperand(0));
}

/// Determine whether a range fits a regular pattern of values.
/// This function accounts for the possibility of jumping over the End iterator.
template <typename ValType>
static bool
fitsRegularPattern(typename SmallVectorImpl<ValType>::const_iterator Begin,
                   unsigned CheckStride,
                   typename SmallVectorImpl<ValType>::const_iterator End,
                   ValType ExpectedIndex, unsigned ExpectedIndexStride) {
  auto &I = Begin;

  while (I != End) {
    if (*I != -1 && *I != ExpectedIndex)
      return false;
    ExpectedIndex += ExpectedIndexStride;

    // Incrementing past End is undefined behaviour so we must increment one
    // step at a time and check for End at each step.
    for (unsigned n = 0; n < CheckStride && I != End; ++n, ++I)
      ; // Empty loop body.
  }
  return true;
}

// Determine whether VECTOR_SHUFFLE is a SPLATI.
//
// It is a SPLATI when the mask is:
//   <x, x, x, ...>
// where x is any valid index.
//
// When undef's appear in the mask they are treated as if they were whatever
// value is necessary in order to fit the above form.
static bool isVECTOR_SHUFFLE_SPLATI(SDValue Op, EVT ResTy,
                                    SmallVector<int, 16> Indices,
                                    SelectionDAG &DAG) {
  assert((Indices.size() % 2) == 0);

  int SplatIndex = -1;
  for (const auto &V : Indices) {
    if (V != -1) {
      SplatIndex = V;
      break;
    }
  }

  return fitsRegularPattern<int>(Indices.begin(), 1, Indices.end(), SplatIndex,
                                 0);
}

// Lower VECTOR_SHUFFLE into ILVEV (if possible).
//
// ILVEV interleaves the even elements from each vector.
//
// It is possible to lower into ILVEV when the mask consists of two of the
// following forms interleaved:
//   <0, 2, 4, ...>
//   <n, n+2, n+4, ...>
// where n is the number of elements in the vector.
// For example:
//   <0, 0, 2, 2, 4, 4, ...>
//   <0, n, 2, n+2, 4, n+4, ...>
//
// When undef's appear in the mask they are treated as if they were whatever
// value is necessary in order to fit the above forms.
static SDValue lowerVECTOR_SHUFFLE_ILVEV(SDValue Op, EVT ResTy,
                                         SmallVector<int, 16> Indices,
                                         SelectionDAG &DAG) {
  assert((Indices.size() % 2) == 0);

  SDValue Wt;
  SDValue Ws;
  const auto &Begin = Indices.begin();
  const auto &End = Indices.end();

  // Check even elements are taken from the even elements of one half or the
  // other and pick an operand accordingly.
  if (fitsRegularPattern<int>(Begin, 2, End, 0, 2))
    Wt = Op->getOperand(0);
  else if (fitsRegularPattern<int>(Begin, 2, End, Indices.size(), 2))
    Wt = Op->getOperand(1);
  else
    return SDValue();

  // Check odd elements are taken from the even elements of one half or the
  // other and pick an operand accordingly.
  if (fitsRegularPattern<int>(Begin + 1, 2, End, 0, 2))
    Ws = Op->getOperand(0);
  else if (fitsRegularPattern<int>(Begin + 1, 2, End, Indices.size(), 2))
    Ws = Op->getOperand(1);
  else
    return SDValue();

  return DAG.getNode(MipsISD::ILVEV, SDLoc(Op), ResTy, Ws, Wt);
}

// Lower VECTOR_SHUFFLE into ILVOD (if possible).
//
// ILVOD interleaves the odd elements from each vector.
//
// It is possible to lower into ILVOD when the mask consists of two of the
// following forms interleaved:
//   <1, 3, 5, ...>
//   <n+1, n+3, n+5, ...>
// where n is the number of elements in the vector.
// For example:
//   <1, 1, 3, 3, 5, 5, ...>
//   <1, n+1, 3, n+3, 5, n+5, ...>
//
// When undef's appear in the mask they are treated as if they were whatever
// value is necessary in order to fit the above forms.
static SDValue lowerVECTOR_SHUFFLE_ILVOD(SDValue Op, EVT ResTy,
                                         SmallVector<int, 16> Indices,
                                         SelectionDAG &DAG) {
  assert((Indices.size() % 2) == 0);

  SDValue Wt;
  SDValue Ws;
  const auto &Begin = Indices.begin();
  const auto &End = Indices.end();

  // Check even elements are taken from the odd elements of one half or the
  // other and pick an operand accordingly.
  if (fitsRegularPattern<int>(Begin, 2, End, 1, 2))
    Wt = Op->getOperand(0);
  else if (fitsRegularPattern<int>(Begin, 2, End, Indices.size() + 1, 2))
    Wt = Op->getOperand(1);
  else
    return SDValue();

  // Check odd elements are taken from the odd elements of one half or the
  // other and pick an operand accordingly.
  if (fitsRegularPattern<int>(Begin + 1, 2, End, 1, 2))
    Ws = Op->getOperand(0);
  else if (fitsRegularPattern<int>(Begin + 1, 2, End, Indices.size() + 1, 2))
    Ws = Op->getOperand(1);
  else
    return SDValue();

  return DAG.getNode(MipsISD::ILVOD, SDLoc(Op), ResTy, Ws, Wt);
}

// Lower VECTOR_SHUFFLE into ILVR (if possible).
//
// ILVR interleaves consecutive elements from the right (lowest-indexed) half of
// each vector.
//
// It is possible to lower into ILVR when the mask consists of two of the
// following forms interleaved:
//   <0, 1, 2, ...>
//   <n, n+1, n+2, ...>
// where n is the number of elements in the vector.
// For example:
//   <0, 0, 1, 1, 2, 2, ...>
//   <0, n, 1, n+1, 2, n+2, ...>
//
// When undef's appear in the mask they are treated as if they were whatever
// value is necessary in order to fit the above forms.
static SDValue lowerVECTOR_SHUFFLE_ILVR(SDValue Op, EVT ResTy,
                                        SmallVector<int, 16> Indices,
                                        SelectionDAG &DAG) {
  assert((Indices.size() % 2) == 0);

  SDValue Wt;
  SDValue Ws;
  const auto &Begin = Indices.begin();
  const auto &End = Indices.end();

  // Check even elements are taken from the right (lowest-indexed) elements of
  // one half or the other and pick an operand accordingly.
  if (fitsRegularPattern<int>(Begin, 2, End, 0, 1))
    Wt = Op->getOperand(0);
  else if (fitsRegularPattern<int>(Begin, 2, End, Indices.size(), 1))
    Wt = Op->getOperand(1);
  else
    return SDValue();

  // Check odd elements are taken from the right (lowest-indexed) elements of
  // one half or the other and pick an operand accordingly.
  if (fitsRegularPattern<int>(Begin + 1, 2, End, 0, 1))
    Ws = Op->getOperand(0);
  else if (fitsRegularPattern<int>(Begin + 1, 2, End, Indices.size(), 1))
    Ws = Op->getOperand(1);
  else
    return SDValue();

  return DAG.getNode(MipsISD::ILVR, SDLoc(Op), ResTy, Ws, Wt);
}

// Lower VECTOR_SHUFFLE into ILVL (if possible).
//
// ILVL interleaves consecutive elements from the left (highest-indexed) half
// of each vector.
//
// It is possible to lower into ILVL when the mask consists of two of the
// following forms interleaved:
//   <x, x+1, x+2, ...>
//   <n+x, n+x+1, n+x+2, ...>
// where n is the number of elements in the vector and x is half n.
// For example:
//   <x, x, x+1, x+1, x+2, x+2, ...>
//   <x, n+x, x+1, n+x+1, x+2, n+x+2, ...>
//
// When undef's appear in the mask they are treated as if they were whatever
// value is necessary in order to fit the above forms.
static SDValue lowerVECTOR_SHUFFLE_ILVL(SDValue Op, EVT ResTy,
                                        SmallVector<int, 16> Indices,
                                        SelectionDAG &DAG) {
  assert((Indices.size() % 2) == 0);

  unsigned HalfSize = Indices.size() / 2;
  SDValue Wt;
  SDValue Ws;
  const auto &Begin = Indices.begin();
  const auto &End = Indices.end();

  // Check even elements are taken from the left (highest-indexed) elements of
  // one half or the other and pick an operand accordingly.
  if (fitsRegularPattern<int>(Begin, 2, End, HalfSize, 1))
    Wt = Op->getOperand(0);
  else if (fitsRegularPattern<int>(Begin, 2, End, Indices.size() + HalfSize, 1))
    Wt = Op->getOperand(1);
  else
    return SDValue();

  // Check odd elements are taken from the left (highest-indexed) elements of
  // one half or the other and pick an operand accordingly.
  if (fitsRegularPattern<int>(Begin + 1, 2, End, HalfSize, 1))
    Ws = Op->getOperand(0);
  else if (fitsRegularPattern<int>(Begin + 1, 2, End, Indices.size() + HalfSize,
                                   1))
    Ws = Op->getOperand(1);
  else
    return SDValue();

  return DAG.getNode(MipsISD::ILVL, SDLoc(Op), ResTy, Ws, Wt);
}

// Lower VECTOR_SHUFFLE into PCKEV (if possible).
//
// PCKEV copies the even elements of each vector into the result vector.
//
// It is possible to lower into PCKEV when the mask consists of two of the
// following forms concatenated:
//   <0, 2, 4, ...>
//   <n, n+2, n+4, ...>
// where n is the number of elements in the vector.
// For example:
//   <0, 2, 4, ..., 0, 2, 4, ...>
//   <0, 2, 4, ..., n, n+2, n+4, ...>
//
// When undef's appear in the mask they are treated as if they were whatever
// value is necessary in order to fit the above forms.
static SDValue lowerVECTOR_SHUFFLE_PCKEV(SDValue Op, EVT ResTy,
                                         SmallVector<int, 16> Indices,
                                         SelectionDAG &DAG) {
  assert((Indices.size() % 2) == 0);

  SDValue Wt;
  SDValue Ws;
  const auto &Begin = Indices.begin();
  const auto &Mid = Indices.begin() + Indices.size() / 2;
  const auto &End = Indices.end();

  if (fitsRegularPattern<int>(Begin, 1, Mid, 0, 2))
    Wt = Op->getOperand(0);
  else if (fitsRegularPattern<int>(Begin, 1, Mid, Indices.size(), 2))
    Wt = Op->getOperand(1);
  else
    return SDValue();

  if (fitsRegularPattern<int>(Mid, 1, End, 0, 2))
    Ws = Op->getOperand(0);
  else if (fitsRegularPattern<int>(Mid, 1, End, Indices.size(), 2))
    Ws = Op->getOperand(1);
  else
    return SDValue();

  return DAG.getNode(MipsISD::PCKEV, SDLoc(Op), ResTy, Ws, Wt);
}

// Lower VECTOR_SHUFFLE into PCKOD (if possible).
//
// PCKOD copies the odd elements of each vector into the result vector.
//
// It is possible to lower into PCKOD when the mask consists of two of the
// following forms concatenated:
//   <1, 3, 5, ...>
//   <n+1, n+3, n+5, ...>
// where n is the number of elements in the vector.
// For example:
//   <1, 3, 5, ..., 1, 3, 5, ...>
//   <1, 3, 5, ..., n+1, n+3, n+5, ...>
//
// When undef's appear in the mask they are treated as if they were whatever
// value is necessary in order to fit the above forms.
static SDValue lowerVECTOR_SHUFFLE_PCKOD(SDValue Op, EVT ResTy,
                                         SmallVector<int, 16> Indices,
                                         SelectionDAG &DAG) {
  assert((Indices.size() % 2) == 0);

  SDValue Wt;
  SDValue Ws;
  const auto &Begin = Indices.begin();
  const auto &Mid = Indices.begin() + Indices.size() / 2;
  const auto &End = Indices.end();

  if (fitsRegularPattern<int>(Begin, 1, Mid, 1, 2))
    Wt = Op->getOperand(0);
  else if (fitsRegularPattern<int>(Begin, 1, Mid, Indices.size() + 1, 2))
    Wt = Op->getOperand(1);
  else
    return SDValue();

  if (fitsRegularPattern<int>(Mid, 1, End, 1, 2))
    Ws = Op->getOperand(0);
  else if (fitsRegularPattern<int>(Mid, 1, End, Indices.size() + 1, 2))
    Ws = Op->getOperand(1);
  else
    return SDValue();

  return DAG.getNode(MipsISD::PCKOD, SDLoc(Op), ResTy, Ws, Wt);
}

// Lower VECTOR_SHUFFLE into VSHF.
//
// This mostly consists of converting the shuffle indices in Indices into a
// BUILD_VECTOR and adding it as an operand to the resulting VSHF. There is
// also code to eliminate unused operands of the VECTOR_SHUFFLE. For example,
// if the type is v8i16 and all the indices are less than 8 then the second
// operand is unused and can be replaced with anything. We choose to replace it
// with the used operand since this reduces the number of instructions overall.
//
// NOTE: SPLATI shuffle masks may contain UNDEFs, since isSPLATI() treats
//       UNDEFs as same as SPLATI index.
//       For other instances we use the last valid index if UNDEF is
//       encountered.
static SDValue lowerVECTOR_SHUFFLE_VSHF(SDValue Op, EVT ResTy,
                                        const SmallVector<int, 16> &Indices,
                                        const bool isSPLATI,
                                        SelectionDAG &DAG) {
  SmallVector<SDValue, 16> Ops;
  SDValue Op0;
  SDValue Op1;
  EVT MaskVecTy = ResTy.changeVectorElementTypeToInteger();
  EVT MaskEltTy = MaskVecTy.getVectorElementType();
  bool Using1stVec = false;
  bool Using2ndVec = false;
  SDLoc DL(Op);
  int ResTyNumElts = ResTy.getVectorNumElements();

  for (int i = 0; i < ResTyNumElts; ++i) {
    // Idx == -1 means UNDEF/poison
    int Idx = Indices[i];

    if (0 <= Idx && Idx < ResTyNumElts)
      Using1stVec = true;
    if (ResTyNumElts <= Idx && Idx < ResTyNumElts * 2)
      Using2ndVec = true;
  }

  // Find the first non-undef index. This index is used as a default when there
  // is a leading UNDEF/poison.
  int SplatIndex = 0;
  for (int Idx : Indices)
    if (Idx >= 0) {
      SplatIndex = Idx;
      break;
    }

  int LastValidIndex = SplatIndex;
  for (size_t i = 0; i < Indices.size(); i++) {
    int Idx = Indices[i];
    if (Idx < 0) {
      // Continue using splati index or use the last valid index.
      Idx = isSPLATI ? SplatIndex : LastValidIndex;
    } else {
      LastValidIndex = Idx;
    }
    Ops.push_back(DAG.getTargetConstant(Idx, DL, MaskEltTy));
  }

  SDValue MaskVec = DAG.getBuildVector(MaskVecTy, DL, Ops);

  if (Using1stVec && Using2ndVec) {
    Op0 = Op->getOperand(0);
    Op1 = Op->getOperand(1);
  } else if (Using1stVec)
    Op0 = Op1 = Op->getOperand(0);
  else if (Using2ndVec)
    Op0 = Op1 = Op->getOperand(1);
  else
    llvm_unreachable("shuffle vector mask references neither vector operand?");

  // VECTOR_SHUFFLE concatenates the vectors in an vectorwise fashion.
  // <0b00, 0b01> + <0b10, 0b11> -> <0b00, 0b01, 0b10, 0b11>
  // VSHF concatenates the vectors in a bitwise fashion:
  // <0b00, 0b01> + <0b10, 0b11> ->
  // 0b0100       + 0b1110       -> 0b01001110
  //                                <0b10, 0b11, 0b00, 0b01>
  // We must therefore swap the operands to get the correct result.
  return DAG.getNode(MipsISD::VSHF, DL, ResTy, MaskVec, Op1, Op0);
}

// Lower VECTOR_SHUFFLE into one of a number of instructions depending on the
// indices in the shuffle.
/// The R5900 MMI instructions that a shuffle of two 128-bit integer vectors A
/// and B can be lowered to, by its mask.
enum class R5900Shuffle {
  None,
  UnzipEven, // PPACW/H/B B, A
  UnzipOdd,  // PPACH/B of A and B shifted right by an element, PCPYUD of
             // PEXCW A and B for words
  ZipLo,     // PEXTLW/H/B B, A
  ZipHi,     // PEXTUW/H/B B, A
  CatLo,     // PCPYLD B, A: the low halves of A and B
  CatHi,     // PCPYUD A, B: the high halves of A and B
  // Permutations of the words of a single source:
  ExchEven,   // PEXEW A: <2, 1, 0, 3>
  ExchCenter, // PEXCW A: <0, 2, 1, 3>
  Rotate3,    // PROT3W A: <1, 2, 0, 3>
  SwapHalves, // PCPYLD A, (PCPYUD A, A): <2, 3, 0, 1>
};

/// Classify a shuffle mask of \p NumElts elements. With \p SingleSrc both
/// operands are the same vector, so an index into the second one is the same
/// element of the first.
static R5900Shuffle classifyR5900Shuffle(ArrayRef<int> Mask, unsigned NumElts,
                                         bool SingleSrc) {
  auto Matches = [&](function_ref<unsigned(unsigned)> Expected) {
    for (unsigned I = 0; I < NumElts; ++I) {
      int M = Mask[I];
      unsigned E = Expected(I);
      if (M >= 0 && (unsigned)M != E &&
          !(SingleSrc && (unsigned)M % NumElts == E % NumElts))
        return false;
    }
    return true;
  };
  unsigned Half = NumElts / 2;
  if (Matches([](unsigned I) { return 2 * I; }))
    return R5900Shuffle::UnzipEven;
  if (Matches([](unsigned I) { return 2 * I + 1; }))
    return R5900Shuffle::UnzipOdd;
  if (Matches([&](unsigned I) { return (I % 2) * NumElts + I / 2; }))
    return R5900Shuffle::ZipLo;
  if (Matches([&](unsigned I) { return (I % 2) * NumElts + Half + I / 2; }))
    return R5900Shuffle::ZipHi;
  if (Matches([&](unsigned I) { return I < Half ? I : NumElts + I - Half; }))
    return R5900Shuffle::CatLo;
  if (Matches([&](unsigned I) { return I < Half ? Half + I : NumElts + I; }))
    return R5900Shuffle::CatHi;
  if (NumElts != 4 || !SingleSrc)
    return R5900Shuffle::None;
  auto MatchesWords = [&](std::array<unsigned, 4> Words) {
    return Matches([&](unsigned I) { return Words[I]; });
  };
  if (MatchesWords({2, 1, 0, 3}))
    return R5900Shuffle::ExchEven;
  if (MatchesWords({0, 2, 1, 3}))
    return R5900Shuffle::ExchCenter;
  if (MatchesWords({1, 2, 0, 3}))
    return R5900Shuffle::Rotate3;
  if (MatchesWords({2, 3, 0, 1}))
    return R5900Shuffle::SwapHalves;
  return R5900Shuffle::None;
}

/// The number of MMI instructions of a shuffle kind for \p NumElts elements.
unsigned llvm::getR5900ShuffleOps(ArrayRef<int> Mask, unsigned NumElts,
                                  bool SingleSrc) {
  switch (classifyR5900Shuffle(Mask, NumElts, SingleSrc)) {
  case R5900Shuffle::None:
    return 0;
  case R5900Shuffle::UnzipOdd:
    // Two shifts or exchanges and a pack, one less from a single source.
    return SingleSrc ? 2 : 3;
  case R5900Shuffle::SwapHalves:
    return 2;
  default:
    return 1;
  }
}

static SDValue lowerR5900VECTOR_SHUFFLE(ShuffleVectorSDNode *Node,
                                        SelectionDAG &DAG) {
  EVT VT = Node->getValueType(0);
  if (VT != MVT::v4i32 && VT != MVT::v8i16 && VT != MVT::v16i8)
    return SDValue();
  SDLoc DL(Node);
  SDValue A = Node->getOperand(0), B = Node->getOperand(1);
  unsigned NumElts = VT.getVectorNumElements();
  bool SingleSrc = B.isUndef() || A == B;
  if (SingleSrc)
    B = A;

  auto Op = [&](unsigned W, unsigned H, unsigned Bt) {
    return VT == MVT::v4i32 ? W : VT == MVT::v8i16 ? H : Bt;
  };
  auto MMI = [&](unsigned Opc, SDValue Rs, SDValue Rt) {
    return SDValue(DAG.getMachineNode(Opc, DL, VT, Rs, Rt), 0);
  };
  switch (classifyR5900Shuffle(Node->getMask(), NumElts, SingleSrc)) {
  case R5900Shuffle::None:
    return SDValue();
  case R5900Shuffle::UnzipEven:
    return MMI(Op(Mips::PPACW, Mips::PPACH, Mips::PPACB), B, A);
  case R5900Shuffle::UnzipOdd: {
    if (VT == MVT::v4i32) {
      auto Exchange = [&](SDValue V) {
        return SDValue(DAG.getMachineNode(Mips::PEXCW, DL, VT, V), 0);
      };
      SDValue XA = Exchange(A);
      return MMI(Mips::PCPYUD, XA, SingleSrc ? XA : Exchange(B));
    }
    // Move the odd elements to the even ones of the elements twice as wide.
    MVT WideVT = VT == MVT::v8i16 ? MVT::v4i32 : MVT::v8i16;
    unsigned Bits = VT.getScalarSizeInBits();
    auto Shift = [&](SDValue V) {
      return DAG.getBitcast(VT, DAG.getNode(ISD::SRL, DL, WideVT,
                                            DAG.getBitcast(WideVT, V),
                                            DAG.getConstant(Bits, DL, WideVT)));
    };
    SDValue SA = Shift(A);
    return MMI(Op(0, Mips::PPACH, Mips::PPACB), SingleSrc ? SA : Shift(B), SA);
  }
  case R5900Shuffle::ZipLo:
    return MMI(Op(Mips::PEXTLW, Mips::PEXTLH, Mips::PEXTLB), B, A);
  case R5900Shuffle::ZipHi:
    return MMI(Op(Mips::PEXTUW, Mips::PEXTUH, Mips::PEXTUB), B, A);
  case R5900Shuffle::CatLo:
    return MMI(Mips::PCPYLD, B, A);
  case R5900Shuffle::CatHi:
    return MMI(Mips::PCPYUD, A, B);
  case R5900Shuffle::ExchEven:
    return SDValue(DAG.getMachineNode(Mips::PEXEW, DL, VT, A), 0);
  case R5900Shuffle::ExchCenter:
    return SDValue(DAG.getMachineNode(Mips::PEXCW, DL, VT, A), 0);
  case R5900Shuffle::Rotate3:
    return SDValue(DAG.getMachineNode(Mips::PROT3W, DL, VT, A), 0);
  case R5900Shuffle::SwapHalves:
    return MMI(Mips::PCPYLD, A, MMI(Mips::PCPYUD, A, A));
  }
  llvm_unreachable("unknown R5900 shuffle");
}

SDValue MipsSETargetLowering::lowerVECTOR_SHUFFLE(SDValue Op,
                                                  SelectionDAG &DAG) const {
  ShuffleVectorSDNode *Node = cast<ShuffleVectorSDNode>(Op);
  EVT ResTy = Op->getValueType(0);

  if (!ResTy.is128BitVector())
    return SDValue();

  // The other R5900 integer shuffles are expanded, there is no MSA.
  if (Subtarget.isR5900() && ResTy.isInteger())
    return lowerR5900VECTOR_SHUFFLE(Node, DAG);

  // VU0 v4f32 shuffles: detect blend patterns for destination masking
  if (Subtarget.hasVU0() && ResTy == MVT::v4f32) {
    SDLoc DL(Op);
    SDValue V1 = Op.getOperand(0);
    SDValue V2 = Op.getOperand(1);

    ArrayRef<int> Mask = Node->getMask();

    // Check for blend pattern: elements come from V1 or V2 at same positions
    // A blend is when each element either comes from V1[i] or V2[i] (not
    // shuffled) Example: <0, 1, 6, 7> means x,y from V1, z,w from V2 -> blend
    // mask 0b1100
    //          <4, 5, 2, 3> means x,y from V2, z,w from V1 -> blend mask 0b0011
    unsigned BlendMask = 0; // Bits set = take from V1, clear = take from V2
    bool IsBlend = true;
    for (unsigned i = 0; i < 4; ++i) {
      int Idx = Mask[i];
      if (Idx < 0) {
        // Undef - we can take from either, default to V2 (base)
        continue;
      } else if (Idx == (int)i) {
        // From V1 at same position - this component will be written
        BlendMask |= (1 << (3 - i)); // xyzw = bits 3,2,1,0
      } else if (Idx == (int)(i + 4)) {
        // From V2 at same position - this component preserved from base
      } else {
        // General permutation, not a simple blend
        IsBlend = false;
        break;
      }
    }

    if (IsBlend && BlendMask != 0 && BlendMask != 0xF) {
      // Create VU0_BLEND(base=V2, src=V1, mask)
      // Result: for each bit set in mask, take from V1; otherwise take from V2
      SDValue MaskVal = DAG.getConstant(BlendMask, DL, MVT::i32);
      return DAG.getNode(MipsISD::VU0_BLEND, DL, ResTy, V2, V1, MaskVal);
    }

    // Check for identity shuffle <0,1,2,3> - just return V1
    bool IsIdentity = true;
    for (unsigned i = 0; i < 4; ++i) {
      if (Mask[i] >= 0 && Mask[i] != (int)i) {
        IsIdentity = false;
        break;
      }
    }
    if (IsIdentity)
      return V1;

    // Check for all-V2 shuffle <4,5,6,7> - just return V2
    bool IsV2Identity = true;
    for (unsigned i = 0; i < 4; ++i) {
      if (Mask[i] >= 0 && Mask[i] != (int)(i + 4)) {
        IsV2Identity = false;
        break;
      }
    }
    if (IsV2Identity)
      return V2;

    // Check if all indices come from V1 (indices < 4) or are undef
    // This indicates a single-source shuffle where V2 isn't used
    bool IsSingleSource = true;
    for (unsigned i = 0; i < 4; ++i) {
      if (Mask[i] >= 4) {
        IsSingleSource = false;
        break;
      }
    }

    // Check for splat pattern (all non-undef indices are the same)
    // Splat can be implemented as: VSUB zero, v, v; VADDbc result, zero, v
    if (IsSingleSource) {
      int SplatIdx = -1;
      bool IsSplat = true;
      for (unsigned i = 0; i < 4; ++i) {
        if (Mask[i] >= 0) {
          if (SplatIdx < 0) {
            SplatIdx = Mask[i];
          } else if (Mask[i] != SplatIdx) {
            IsSplat = false;
            break;
          }
        }
      }
      if (IsSplat && SplatIdx >= 0 && SplatIdx < 4) {
        // A broadcast is the field added to zero: VADDbc(V1 - V1, V1).
        return getVU0Broadcast(DAG, DL, V1, SplatIdx);
      }
    }

    // Check for VMR32 rotation pattern <1,2,3,0> (rotate left by 1)
    // VMR32.xyzw ft, fs: ft.x=fs.y, ft.y=fs.z, ft.z=fs.w, ft.w=fs.x
    if (IsSingleSource || V1 == V2 || V2.isUndef()) {
      bool IsVMR32 =
          (Mask[0] == 1 || Mask[0] < 0) && (Mask[1] == 2 || Mask[1] < 0) &&
          (Mask[2] == 3 || Mask[2] < 0) && (Mask[3] == 0 || Mask[3] < 0);
      if (IsVMR32) {
        // Use VMR32 instruction
        return SDValue(
            DAG.getMachineNode(Mips::VMR32, DL, MVT::v4f32,
                               DAG.getTargetConstant(0xF, DL, MVT::i32), V1),
            0);
      }

      // Check for reverse rotation <3,0,1,2> (rotate right by 1 = VMR32 x 3)
      bool IsRevRot =
          (Mask[0] == 3 || Mask[0] < 0) && (Mask[1] == 0 || Mask[1] < 0) &&
          (Mask[2] == 1 || Mask[2] < 0) && (Mask[3] == 2 || Mask[3] < 0);
      if (IsRevRot) {
        // Three VMR32 operations = rotate right by 1
        SDValue Rot1 = SDValue(
            DAG.getMachineNode(Mips::VMR32, DL, MVT::v4f32,
                               DAG.getTargetConstant(0xF, DL, MVT::i32), V1),
            0);
        SDValue Rot2 = SDValue(
            DAG.getMachineNode(Mips::VMR32, DL, MVT::v4f32,
                               DAG.getTargetConstant(0xF, DL, MVT::i32), Rot1),
            0);
        return SDValue(
            DAG.getMachineNode(Mips::VMR32, DL, MVT::v4f32,
                               DAG.getTargetConstant(0xF, DL, MVT::i32), Rot2),
            0);
      }
    }

    // Otherwise an MMI shuffle of the bits in a 128-bit GPR, if there is one.
    // The others are expanded.
    if (getR5900ShuffleOps(Mask, 4,
                           IsSingleSource || V2.isUndef() || V1 == V2)) {
      SDValue Shuffle =
          DAG.getVectorShuffle(MVT::v4i32, DL, DAG.getBitcast(MVT::v4i32, V1),
                               DAG.getBitcast(MVT::v4i32, V2), Mask);
      return DAG.getBitcast(MVT::v4f32, Shuffle);
    }
    return SDValue();
  }

  int ResTyNumElts = ResTy.getVectorNumElements();
  SmallVector<int, 16> Indices;

  for (int i = 0; i < ResTyNumElts; ++i)
    Indices.push_back(Node->getMaskElt(i));

  // splati.[bhwd] is preferable to the others but is matched from
  // MipsISD::VSHF.
  if (isVECTOR_SHUFFLE_SPLATI(Op, ResTy, Indices, DAG))
    return lowerVECTOR_SHUFFLE_VSHF(Op, ResTy, Indices, true, DAG);
  SDValue Result;
  if ((Result = lowerVECTOR_SHUFFLE_ILVEV(Op, ResTy, Indices, DAG)))
    return Result;
  if ((Result = lowerVECTOR_SHUFFLE_ILVOD(Op, ResTy, Indices, DAG)))
    return Result;
  if ((Result = lowerVECTOR_SHUFFLE_ILVL(Op, ResTy, Indices, DAG)))
    return Result;
  if ((Result = lowerVECTOR_SHUFFLE_ILVR(Op, ResTy, Indices, DAG)))
    return Result;
  if ((Result = lowerVECTOR_SHUFFLE_PCKEV(Op, ResTy, Indices, DAG)))
    return Result;
  if ((Result = lowerVECTOR_SHUFFLE_PCKOD(Op, ResTy, Indices, DAG)))
    return Result;
  if ((Result = lowerVECTOR_SHUFFLE_SHF(Op, ResTy, Indices, DAG)))
    return Result;
  return lowerVECTOR_SHUFFLE_VSHF(Op, ResTy, Indices, false, DAG);
}

MachineBasicBlock *
MipsSETargetLowering::emitBPOSGE32(MachineInstr &MI,
                                   MachineBasicBlock *BB) const {
  // $bb:
  //  bposge32_pseudo $vr0
  //  =>
  // $bb:
  //  bposge32 $tbb
  // $fbb:
  //  li $vr2, 0
  //  b $sink
  // $tbb:
  //  li $vr1, 1
  // $sink:
  //  $vr0 = phi($vr2, $fbb, $vr1, $tbb)

  MachineRegisterInfo &RegInfo = BB->getParent()->getRegInfo();
  const TargetInstrInfo *TII = Subtarget.getInstrInfo();
  const TargetRegisterClass *RC = &Mips::GPR32RegClass;
  DebugLoc DL = MI.getDebugLoc();
  const BasicBlock *LLVM_BB = BB->getBasicBlock();
  MachineFunction::iterator It = std::next(MachineFunction::iterator(BB));
  MachineFunction *F = BB->getParent();
  MachineBasicBlock *FBB = F->CreateMachineBasicBlock(LLVM_BB);
  MachineBasicBlock *TBB = F->CreateMachineBasicBlock(LLVM_BB);
  MachineBasicBlock *Sink  = F->CreateMachineBasicBlock(LLVM_BB);
  F->insert(It, FBB);
  F->insert(It, TBB);
  F->insert(It, Sink);

  // Transfer the remainder of BB and its successor edges to Sink.
  Sink->splice(Sink->begin(), BB, std::next(MachineBasicBlock::iterator(MI)),
               BB->end());
  Sink->transferSuccessorsAndUpdatePHIs(BB);

  // Add successors.
  BB->addSuccessor(FBB);
  BB->addSuccessor(TBB);
  FBB->addSuccessor(Sink);
  TBB->addSuccessor(Sink);

  // Insert the real bposge32 instruction to $BB.
  BuildMI(BB, DL, TII->get(Mips::BPOSGE32)).addMBB(TBB);
  // Insert the real bposge32c instruction to $BB.
  BuildMI(BB, DL, TII->get(Mips::BPOSGE32C_MMR3)).addMBB(TBB);

  // Fill $FBB.
  Register VR2 = RegInfo.createVirtualRegister(RC);
  BuildMI(*FBB, FBB->end(), DL, TII->get(Mips::ADDiu), VR2)
      .addReg(Mips::R0)
      .addImm(0);
  BuildMI(*FBB, FBB->end(), DL, TII->get(Mips::B)).addMBB(Sink);

  // Fill $TBB.
  Register VR1 = RegInfo.createVirtualRegister(RC);
  BuildMI(*TBB, TBB->end(), DL, TII->get(Mips::ADDiu), VR1)
      .addReg(Mips::R0)
      .addImm(1);

  // Insert phi function to $Sink.
  BuildMI(*Sink, Sink->begin(), DL, TII->get(Mips::PHI),
          MI.getOperand(0).getReg())
      .addReg(VR2)
      .addMBB(FBB)
      .addReg(VR1)
      .addMBB(TBB);

  MI.eraseFromParent(); // The pseudo instruction is gone now.
  return Sink;
}

MachineBasicBlock *MipsSETargetLowering::emitMSACBranchPseudo(
    MachineInstr &MI, MachineBasicBlock *BB, unsigned BranchOp) const {
  // $bb:
  //  vany_nonzero $rd, $ws
  //  =>
  // $bb:
  //  bnz.b $ws, $tbb
  //  b $fbb
  // $fbb:
  //  li $rd1, 0
  //  b $sink
  // $tbb:
  //  li $rd2, 1
  // $sink:
  //  $rd = phi($rd1, $fbb, $rd2, $tbb)

  MachineRegisterInfo &RegInfo = BB->getParent()->getRegInfo();
  const TargetInstrInfo *TII = Subtarget.getInstrInfo();
  const TargetRegisterClass *RC = &Mips::GPR32RegClass;
  DebugLoc DL = MI.getDebugLoc();
  const BasicBlock *LLVM_BB = BB->getBasicBlock();
  MachineFunction::iterator It = std::next(MachineFunction::iterator(BB));
  MachineFunction *F = BB->getParent();
  MachineBasicBlock *FBB = F->CreateMachineBasicBlock(LLVM_BB);
  MachineBasicBlock *TBB = F->CreateMachineBasicBlock(LLVM_BB);
  MachineBasicBlock *Sink  = F->CreateMachineBasicBlock(LLVM_BB);
  F->insert(It, FBB);
  F->insert(It, TBB);
  F->insert(It, Sink);

  // Transfer the remainder of BB and its successor edges to Sink.
  Sink->splice(Sink->begin(), BB, std::next(MachineBasicBlock::iterator(MI)),
               BB->end());
  Sink->transferSuccessorsAndUpdatePHIs(BB);

  // Add successors.
  BB->addSuccessor(FBB);
  BB->addSuccessor(TBB);
  FBB->addSuccessor(Sink);
  TBB->addSuccessor(Sink);

  // Insert the real bnz.b instruction to $BB.
  BuildMI(BB, DL, TII->get(BranchOp))
      .addReg(MI.getOperand(1).getReg())
      .addMBB(TBB);

  // Fill $FBB.
  Register RD1 = RegInfo.createVirtualRegister(RC);
  BuildMI(*FBB, FBB->end(), DL, TII->get(Mips::ADDiu), RD1)
      .addReg(Mips::R0)
      .addImm(0);
  BuildMI(*FBB, FBB->end(), DL, TII->get(Mips::B)).addMBB(Sink);

  // Fill $TBB.
  Register RD2 = RegInfo.createVirtualRegister(RC);
  BuildMI(*TBB, TBB->end(), DL, TII->get(Mips::ADDiu), RD2)
      .addReg(Mips::R0)
      .addImm(1);

  // Insert phi function to $Sink.
  BuildMI(*Sink, Sink->begin(), DL, TII->get(Mips::PHI),
          MI.getOperand(0).getReg())
      .addReg(RD1)
      .addMBB(FBB)
      .addReg(RD2)
      .addMBB(TBB);

  MI.eraseFromParent(); // The pseudo instruction is gone now.
  return Sink;
}

// Emit the R5900 multiply-add chain pseudo instruction.
//

// Emit the COPY_FW pseudo instruction.
//
// copy_fw_pseudo $fd, $ws, n
// =>
// copy_u_w $rt, $ws, $n
// mtc1     $rt, $fd
//
// When n is zero, the equivalent operation can be performed with (potentially)
// zero instructions due to register overlaps. This optimization is never valid
// for lane 1 because it would require FR=0 mode which isn't supported by MSA.
MachineBasicBlock *
MipsSETargetLowering::emitCOPY_FW(MachineInstr &MI,
                                  MachineBasicBlock *BB) const {
  const TargetInstrInfo *TII = Subtarget.getInstrInfo();
  MachineRegisterInfo &RegInfo = BB->getParent()->getRegInfo();
  DebugLoc DL = MI.getDebugLoc();
  Register Fd = MI.getOperand(0).getReg();
  Register Ws = MI.getOperand(1).getReg();
  unsigned Lane = MI.getOperand(2).getImm();

  if (Lane == 0) {
    unsigned Wt = Ws;
    if (!Subtarget.useOddSPReg()) {
      // We must copy to an even-numbered MSA register so that the
      // single-precision sub-register is also guaranteed to be even-numbered.
      Wt = RegInfo.createVirtualRegister(&Mips::MSA128WEvensRegClass);

      BuildMI(*BB, MI, DL, TII->get(Mips::COPY), Wt).addReg(Ws);
    }

    BuildMI(*BB, MI, DL, TII->get(Mips::COPY), Fd).addReg(Wt, {}, Mips::sub_lo);
  } else {
    Register Wt = RegInfo.createVirtualRegister(
        Subtarget.useOddSPReg() ? &Mips::MSA128WRegClass
                                : &Mips::MSA128WEvensRegClass);

    BuildMI(*BB, MI, DL, TII->get(Mips::SPLATI_W), Wt).addReg(Ws).addImm(Lane);
    BuildMI(*BB, MI, DL, TII->get(Mips::COPY), Fd).addReg(Wt, {}, Mips::sub_lo);
  }

  MI.eraseFromParent(); // The pseudo instruction is gone now.
  return BB;
}

// Emit the COPY_FD pseudo instruction.
//
// copy_fd_pseudo $fd, $ws, n
// =>
// splati.d $wt, $ws, $n
// copy $fd, $wt:sub_64
//
// When n is zero, the equivalent operation can be performed with (potentially)
// zero instructions due to register overlaps. This optimization is always
// valid because FR=1 mode which is the only supported mode in MSA.
MachineBasicBlock *
MipsSETargetLowering::emitCOPY_FD(MachineInstr &MI,
                                  MachineBasicBlock *BB) const {
  assert(Subtarget.isFP64bit());

  const TargetInstrInfo *TII = Subtarget.getInstrInfo();
  MachineRegisterInfo &RegInfo = BB->getParent()->getRegInfo();
  Register Fd = MI.getOperand(0).getReg();
  Register Ws = MI.getOperand(1).getReg();
  unsigned Lane = MI.getOperand(2).getImm() * 2;
  DebugLoc DL = MI.getDebugLoc();

  if (Lane == 0)
    BuildMI(*BB, MI, DL, TII->get(Mips::COPY), Fd).addReg(Ws, {}, Mips::sub_64);
  else {
    Register Wt = RegInfo.createVirtualRegister(&Mips::MSA128DRegClass);

    BuildMI(*BB, MI, DL, TII->get(Mips::SPLATI_D), Wt).addReg(Ws).addImm(1);
    BuildMI(*BB, MI, DL, TII->get(Mips::COPY), Fd).addReg(Wt, {}, Mips::sub_64);
  }

  MI.eraseFromParent(); // The pseudo instruction is gone now.
  return BB;
}

// Emit the INSERT_FW pseudo instruction.
//
// insert_fw_pseudo $wd, $wd_in, $n, $fs
// =>
// subreg_to_reg $wt:sub_lo, $fs
// insve_w $wd[$n], $wd_in, $wt[0]
MachineBasicBlock *
MipsSETargetLowering::emitINSERT_FW(MachineInstr &MI,
                                    MachineBasicBlock *BB) const {
  const TargetInstrInfo *TII = Subtarget.getInstrInfo();
  MachineRegisterInfo &RegInfo = BB->getParent()->getRegInfo();
  DebugLoc DL = MI.getDebugLoc();
  Register Wd = MI.getOperand(0).getReg();
  Register Wd_in = MI.getOperand(1).getReg();
  unsigned Lane = MI.getOperand(2).getImm();
  Register Fs = MI.getOperand(3).getReg();
  Register Wt = RegInfo.createVirtualRegister(
      Subtarget.useOddSPReg() ? &Mips::MSA128WRegClass
                              : &Mips::MSA128WEvensRegClass);

  BuildMI(*BB, MI, DL, TII->get(Mips::SUBREG_TO_REG), Wt)
      .addReg(Fs)
      .addImm(Mips::sub_lo);
  BuildMI(*BB, MI, DL, TII->get(Mips::INSVE_W), Wd)
      .addReg(Wd_in)
      .addImm(Lane)
      .addReg(Wt)
      .addImm(0);

  MI.eraseFromParent(); // The pseudo instruction is gone now.
  return BB;
}

// Emit the INSERT_FD pseudo instruction.
//
// insert_fd_pseudo $wd, $fs, n
// =>
// subreg_to_reg $wt:sub_64, $fs
// insve_d $wd[$n], $wd_in, $wt[0]
MachineBasicBlock *
MipsSETargetLowering::emitINSERT_FD(MachineInstr &MI,
                                    MachineBasicBlock *BB) const {
  assert(Subtarget.isFP64bit());

  const TargetInstrInfo *TII = Subtarget.getInstrInfo();
  MachineRegisterInfo &RegInfo = BB->getParent()->getRegInfo();
  DebugLoc DL = MI.getDebugLoc();
  Register Wd = MI.getOperand(0).getReg();
  Register Wd_in = MI.getOperand(1).getReg();
  unsigned Lane = MI.getOperand(2).getImm();
  Register Fs = MI.getOperand(3).getReg();
  Register Wt = RegInfo.createVirtualRegister(&Mips::MSA128DRegClass);

  BuildMI(*BB, MI, DL, TII->get(Mips::SUBREG_TO_REG), Wt)
      .addReg(Fs)
      .addImm(Mips::sub_64);
  BuildMI(*BB, MI, DL, TII->get(Mips::INSVE_D), Wd)
      .addReg(Wd_in)
      .addImm(Lane)
      .addReg(Wt)
      .addImm(0);

  MI.eraseFromParent(); // The pseudo instruction is gone now.
  return BB;
}

// Emit the INSERT_([BHWD]|F[WD])_VIDX pseudo instruction.
//
// For integer:
// (INSERT_([BHWD]|F[WD])_PSEUDO $wd, $wd_in, $n, $rs)
// =>
// (SLL $lanetmp1, $lane, <log2size)
// (SLD_B $wdtmp1, $wd_in, $wd_in, $lanetmp1)
// (INSERT_[BHWD], $wdtmp2, $wdtmp1, 0, $rs)
// (NEG $lanetmp2, $lanetmp1)
// (SLD_B $wd, $wdtmp2, $wdtmp2,  $lanetmp2)
//
// For floating point:
// (INSERT_([BHWD]|F[WD])_PSEUDO $wd, $wd_in, $n, $fs)
// =>
// (SUBREG_TO_REG $wt, $fs, <subreg>)
// (SLL $lanetmp1, $lane, <log2size)
// (SLD_B $wdtmp1, $wd_in, $wd_in, $lanetmp1)
// (INSVE_[WD], $wdtmp2, 0, $wdtmp1, 0)
// (NEG $lanetmp2, $lanetmp1)
// (SLD_B $wd, $wdtmp2, $wdtmp2,  $lanetmp2)
MachineBasicBlock *MipsSETargetLowering::emitINSERT_DF_VIDX(
    MachineInstr &MI, MachineBasicBlock *BB, unsigned EltSizeInBytes,
    bool IsFP) const {
  const TargetInstrInfo *TII = Subtarget.getInstrInfo();
  MachineRegisterInfo &RegInfo = BB->getParent()->getRegInfo();
  DebugLoc DL = MI.getDebugLoc();
  Register Wd = MI.getOperand(0).getReg();
  Register SrcVecReg = MI.getOperand(1).getReg();
  Register LaneReg = MI.getOperand(2).getReg();
  Register SrcValReg = MI.getOperand(3).getReg();

  const TargetRegisterClass *VecRC = nullptr;
  // FIXME: This should be true for N32 too.
  const TargetRegisterClass *GPRRC =
      Subtarget.isABI_N64() ? &Mips::GPR64RegClass : &Mips::GPR32RegClass;
  unsigned SubRegIdx = Subtarget.isABI_N64() ? Mips::sub_32 : 0;
  unsigned ShiftOp = Subtarget.isABI_N64() ? Mips::DSLL : Mips::SLL;
  unsigned EltLog2Size;
  unsigned InsertOp = 0;
  unsigned InsveOp = 0;
  switch (EltSizeInBytes) {
  default:
    llvm_unreachable("Unexpected size");
  case 1:
    EltLog2Size = 0;
    InsertOp = Mips::INSERT_B;
    InsveOp = Mips::INSVE_B;
    VecRC = &Mips::MSA128BRegClass;
    break;
  case 2:
    EltLog2Size = 1;
    InsertOp = Mips::INSERT_H;
    InsveOp = Mips::INSVE_H;
    VecRC = &Mips::MSA128HRegClass;
    break;
  case 4:
    EltLog2Size = 2;
    InsertOp = Mips::INSERT_W;
    InsveOp = Mips::INSVE_W;
    VecRC = &Mips::MSA128WRegClass;
    break;
  case 8:
    EltLog2Size = 3;
    InsertOp = Mips::INSERT_D;
    InsveOp = Mips::INSVE_D;
    VecRC = &Mips::MSA128DRegClass;
    break;
  }

  if (IsFP) {
    Register Wt = RegInfo.createVirtualRegister(VecRC);
    BuildMI(*BB, MI, DL, TII->get(Mips::SUBREG_TO_REG), Wt)
        .addReg(SrcValReg)
        .addImm(EltSizeInBytes == 8 ? Mips::sub_64 : Mips::sub_lo);
    SrcValReg = Wt;
  }

  // Convert the lane index into a byte index
  if (EltSizeInBytes != 1) {
    Register LaneTmp1 = RegInfo.createVirtualRegister(GPRRC);
    BuildMI(*BB, MI, DL, TII->get(ShiftOp), LaneTmp1)
        .addReg(LaneReg)
        .addImm(EltLog2Size);
    LaneReg = LaneTmp1;
  }

  // Rotate bytes around so that the desired lane is element zero
  Register WdTmp1 = RegInfo.createVirtualRegister(VecRC);
  BuildMI(*BB, MI, DL, TII->get(Mips::SLD_B), WdTmp1)
      .addReg(SrcVecReg)
      .addReg(SrcVecReg)
      .addReg(LaneReg, {}, SubRegIdx);

  Register WdTmp2 = RegInfo.createVirtualRegister(VecRC);
  if (IsFP) {
    // Use insve.df to insert to element zero
    BuildMI(*BB, MI, DL, TII->get(InsveOp), WdTmp2)
        .addReg(WdTmp1)
        .addImm(0)
        .addReg(SrcValReg)
        .addImm(0);
  } else {
    // Use insert.df to insert to element zero
    BuildMI(*BB, MI, DL, TII->get(InsertOp), WdTmp2)
        .addReg(WdTmp1)
        .addReg(SrcValReg)
        .addImm(0);
  }

  // Rotate elements the rest of the way for a full rotation.
  // sld.df inteprets $rt modulo the number of columns so we only need to negate
  // the lane index to do this.
  Register LaneTmp2 = RegInfo.createVirtualRegister(GPRRC);
  BuildMI(*BB, MI, DL, TII->get(Subtarget.isABI_N64() ? Mips::DSUB : Mips::SUB),
          LaneTmp2)
      .addReg(Subtarget.isABI_N64() ? Mips::R0_64 : Mips::R0)
      .addReg(LaneReg);
  BuildMI(*BB, MI, DL, TII->get(Mips::SLD_B), Wd)
      .addReg(WdTmp2)
      .addReg(WdTmp2)
      .addReg(LaneTmp2, {}, SubRegIdx);

  MI.eraseFromParent(); // The pseudo instruction is gone now.
  return BB;
}

// Emit the FILL_FW pseudo instruction.
//
// fill_fw_pseudo $wd, $fs
// =>
// implicit_def $wt1
// insert_subreg $wt2:subreg_lo, $wt1, $fs
// splati.w $wd, $wt2[0]
MachineBasicBlock *
MipsSETargetLowering::emitFILL_FW(MachineInstr &MI,
                                  MachineBasicBlock *BB) const {
  const TargetInstrInfo *TII = Subtarget.getInstrInfo();
  MachineRegisterInfo &RegInfo = BB->getParent()->getRegInfo();
  DebugLoc DL = MI.getDebugLoc();
  Register Wd = MI.getOperand(0).getReg();
  Register Fs = MI.getOperand(1).getReg();
  Register Wt1 = RegInfo.createVirtualRegister(
      Subtarget.useOddSPReg() ? &Mips::MSA128WRegClass
                              : &Mips::MSA128WEvensRegClass);
  Register Wt2 = RegInfo.createVirtualRegister(
      Subtarget.useOddSPReg() ? &Mips::MSA128WRegClass
                              : &Mips::MSA128WEvensRegClass);

  BuildMI(*BB, MI, DL, TII->get(Mips::IMPLICIT_DEF), Wt1);
  BuildMI(*BB, MI, DL, TII->get(Mips::INSERT_SUBREG), Wt2)
      .addReg(Wt1)
      .addReg(Fs)
      .addImm(Mips::sub_lo);
  BuildMI(*BB, MI, DL, TII->get(Mips::SPLATI_W), Wd).addReg(Wt2).addImm(0);

  MI.eraseFromParent(); // The pseudo instruction is gone now.
  return BB;
}

// Emit the FILL_FD pseudo instruction.
//
// fill_fd_pseudo $wd, $fs
// =>
// implicit_def $wt1
// insert_subreg $wt2:subreg_64, $wt1, $fs
// splati.d $wd, $wt2[0]
MachineBasicBlock *
MipsSETargetLowering::emitFILL_FD(MachineInstr &MI,
                                  MachineBasicBlock *BB) const {
  assert(Subtarget.isFP64bit());

  const TargetInstrInfo *TII = Subtarget.getInstrInfo();
  MachineRegisterInfo &RegInfo = BB->getParent()->getRegInfo();
  DebugLoc DL = MI.getDebugLoc();
  Register Wd = MI.getOperand(0).getReg();
  Register Fs = MI.getOperand(1).getReg();
  Register Wt1 = RegInfo.createVirtualRegister(&Mips::MSA128DRegClass);
  Register Wt2 = RegInfo.createVirtualRegister(&Mips::MSA128DRegClass);

  BuildMI(*BB, MI, DL, TII->get(Mips::IMPLICIT_DEF), Wt1);
  BuildMI(*BB, MI, DL, TII->get(Mips::INSERT_SUBREG), Wt2)
      .addReg(Wt1)
      .addReg(Fs)
      .addImm(Mips::sub_64);
  BuildMI(*BB, MI, DL, TII->get(Mips::SPLATI_D), Wd).addReg(Wt2).addImm(0);

  MI.eraseFromParent(); // The pseudo instruction is gone now.
  return BB;
}

// Emit the FEXP2_W_1 pseudo instructions.
//
// fexp2_w_1_pseudo $wd, $wt
// =>
// ldi.w $ws, 1
// fexp2.w $wd, $ws, $wt
MachineBasicBlock *
MipsSETargetLowering::emitFEXP2_W_1(MachineInstr &MI,
                                    MachineBasicBlock *BB) const {
  const TargetInstrInfo *TII = Subtarget.getInstrInfo();
  MachineRegisterInfo &RegInfo = BB->getParent()->getRegInfo();
  const TargetRegisterClass *RC = &Mips::MSA128WRegClass;
  Register Ws1 = RegInfo.createVirtualRegister(RC);
  Register Ws2 = RegInfo.createVirtualRegister(RC);
  DebugLoc DL = MI.getDebugLoc();

  // Splat 1.0 into a vector
  BuildMI(*BB, MI, DL, TII->get(Mips::LDI_W), Ws1).addImm(1);
  BuildMI(*BB, MI, DL, TII->get(Mips::FFINT_U_W), Ws2).addReg(Ws1);

  // Emit 1.0 * fexp2(Wt)
  BuildMI(*BB, MI, DL, TII->get(Mips::FEXP2_W), MI.getOperand(0).getReg())
      .addReg(Ws2)
      .addReg(MI.getOperand(1).getReg());

  MI.eraseFromParent(); // The pseudo instruction is gone now.
  return BB;
}

// Emit the FEXP2_D_1 pseudo instructions.
//
// fexp2_d_1_pseudo $wd, $wt
// =>
// ldi.d $ws, 1
// fexp2.d $wd, $ws, $wt
MachineBasicBlock *
MipsSETargetLowering::emitFEXP2_D_1(MachineInstr &MI,
                                    MachineBasicBlock *BB) const {
  const TargetInstrInfo *TII = Subtarget.getInstrInfo();
  MachineRegisterInfo &RegInfo = BB->getParent()->getRegInfo();
  const TargetRegisterClass *RC = &Mips::MSA128DRegClass;
  Register Ws1 = RegInfo.createVirtualRegister(RC);
  Register Ws2 = RegInfo.createVirtualRegister(RC);
  DebugLoc DL = MI.getDebugLoc();

  // Splat 1.0 into a vector
  BuildMI(*BB, MI, DL, TII->get(Mips::LDI_D), Ws1).addImm(1);
  BuildMI(*BB, MI, DL, TII->get(Mips::FFINT_U_D), Ws2).addReg(Ws1);

  // Emit 1.0 * fexp2(Wt)
  BuildMI(*BB, MI, DL, TII->get(Mips::FEXP2_D), MI.getOperand(0).getReg())
      .addReg(Ws2)
      .addReg(MI.getOperand(1).getReg());

  MI.eraseFromParent(); // The pseudo instruction is gone now.
  return BB;
}

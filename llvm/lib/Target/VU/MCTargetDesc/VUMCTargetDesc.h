//===-- VUMCTargetDesc.h - VU target descriptions ---------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_VU_MCTARGETDESC_VUMCTARGETDESC_H
#define LLVM_LIB_TARGET_VU_MCTARGETDESC_VUMCTARGETDESC_H

#include "llvm/MC/MCFixup.h"
#include "llvm/MC/MCInstrDesc.h"
#include <cstdint>
#include <memory>

namespace llvm {
class MCAsmBackend;
class MCCodeEmitter;
class MCContext;
class MCInstrInfo;
class MCObjectTargetWriter;
class MCRegisterInfo;
class MCSubtargetInfo;
class MCTargetOptions;
class Target;

MCCodeEmitter *createVUMCCodeEmitter(const MCInstrInfo &MCII, MCContext &Ctx);
MCAsmBackend *createVUAsmBackend(const Target &T, const MCSubtargetInfo &STI,
                                 const MCRegisterInfo &MRI,
                                 const MCTargetOptions &Options);
std::unique_ptr<MCObjectTargetWriter> createVUELFObjectWriter(uint8_t OSABI);

namespace VU {

/// TSFlags: set for instructions that go in the lower half of a micro
/// instruction word.
enum { TSF_IsLower = 1 << 0 };

inline bool isLowerInstr(const MCInstrDesc &Desc) {
  return Desc.TSFlags & TSF_IsLower;
}

/// Operands of a BUNDLE (one 64-bit micro instruction word).
enum BundleOperand {
  BundleFlagsOp = 0, ///< Immediate: BundleFlags.
  BundleUpperOp = 1, ///< MCInst: the upper instruction.
  BundleLowerOp = 2, ///< MCInst: the lower instruction.
};

/// Flags stored in bits 31-27 of the upper instruction word. The I flag is
/// not stored in the bundle: it is set when the lower instruction is LOI.
enum BundleFlags : unsigned {
  FlagT = 1 << 0, ///< Halt and interrupt the host (debug).
  FlagD = 1 << 1, ///< Break and interrupt the host (debug).
  FlagM = 1 << 2, ///< VU0 only: COP2 interlock.
  FlagE = 1 << 3, ///< End the micro program after the next instruction.
  FlagI = 1 << 4, ///< The lower word is an immediate for the I register.
};
constexpr unsigned BundleFlagsShift = 27;

/// The lower half of an empty micro instruction: "move vf0, vf0" with an
/// empty destination mask, as emitted by dvp-as for "nop".
constexpr uint32_t LowerNopEncoding = 0x8000033c;

enum Fixups {
  /// 11-bit branch offset in units of 8 bytes, relative to the next micro
  /// instruction word.
  fixup_vu_pcrel11 = FirstTargetFixupKind,
  LastTargetFixupKind,
  NumTargetFixupKinds = LastTargetFixupKind - FirstTargetFixupKind
};

} // namespace VU
} // namespace llvm

#define GET_REGINFO_ENUM
#include "VUGenRegisterInfo.inc"

#define GET_INSTRINFO_ENUM
#define GET_INSTRINFO_MC_HELPER_DECLS
#include "VUGenInstrInfo.inc"

#define GET_SUBTARGETINFO_ENUM
#include "VUGenSubtargetInfo.inc"

#endif // LLVM_LIB_TARGET_VU_MCTARGETDESC_VUMCTARGETDESC_H

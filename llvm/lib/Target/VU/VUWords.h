//===-- VUWords.h - Micro instruction words of a VU loop --------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A model of the micro instruction words of a block, used to compare layouts
// of single block loops before the block is rebuilt from one of them.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_VU_VUWORDS_H
#define LLVM_LIB_TARGET_VU_VUWORDS_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"

namespace llvm {
class DebugLoc;
class InstrItineraryData;
class MachineBasicBlock;
class MachineInstr;
class TargetInstrInfo;
class TargetRegisterInfo;

namespace VU {
/// A micro instruction word: an upper and a lower instruction, both optional.
struct Word {
  MachineInstr *Upper = nullptr, *Lower = nullptr;
  /// The delay slot of the branch in the word before.
  bool DelaySlot = false;
};

/// Returns the instructions of the word \p W, which may be a bundle, without
/// meta instructions.
SmallVector<MachineInstr *, 2> getWordInstrs(MachineInstr &W);

/// Returns the cycles of an iteration of the single block loop \p Words,
/// stalls included.
int getLoopCycles(ArrayRef<Word> Words, const TargetInstrInfo &TII,
                  const InstrItineraryData *Itin,
                  const TargetRegisterInfo &TRI);

/// Replaces the words of \p MBB with \p Words, which hold all its
/// instructions apart from NOPs and delay slot markers.
void rebuildWords(MachineBasicBlock &MBB, ArrayRef<Word> Words,
                  const TargetInstrInfo &TII, const DebugLoc &DL);
} // namespace VU
} // namespace llvm

#endif // LLVM_LIB_TARGET_VU_VUWORDS_H

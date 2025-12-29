//===- DVPVUCallingConv.td - Calling Conventions for DVPVU -*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This describes the calling conventions for the PS2 VU architecture.
//
// Note: The VU does not have a traditional stack. It operates as a
// coprocessor/shader processor with fixed data memory. This convention
// is inspired by MIPS n32 ABI (8 int args, 8 float args) but expanded
// for VU's larger register file.
//
// Convention (similar to MIPS n32 but with more registers):
// - VF4-VF15: 12 vector arguments (enough for 4x4 matrix + 8 vectors)
// - VI4-VI11: 8 integer arguments
// - VF1-VF3: Vector return values
// - VI1-VI3: Integer return values
// - VF16-VF30: Callee-saved vectors (15 registers)
// - VI12-VI14: Callee-saved integers (3 registers)
// - VF0: Constant (0, 0, 0, 1) - reserved
// - VI0: Constant 0 - reserved
// - VI15: Link register (return address)
//
//===----------------------------------------------------------------------===//

//===----------------------------------------------------------------------===//
// DVPVU Argument Calling Convention
//===----------------------------------------------------------------------===//

def CC_DVPVU : CallingConv<[
  // Promote i8/i16 to i16 (native width)
  CCIfType<[i8], CCPromoteToType<i16>>,

  // Vector arguments in VF4-VF15 (12 vectors = 48 floats)
  // This allows passing a full 4x4 matrix plus 8 additional vectors
  CCIfType<[v4f32], CCAssignToReg<[VF4, VF5, VF6, VF7, VF8, VF9, VF10, VF11,
                                   VF12, VF13, VF14, VF15]>>,

  // Scalar float arguments - use vector registers
  CCIfType<[f32], CCAssignToReg<[VF4, VF5, VF6, VF7, VF8, VF9, VF10, VF11,
                                 VF12, VF13, VF14, VF15]>>,

  // Integer arguments in VI4-VI11 (8 integers, matching MIPS n32)
  CCIfType<[i16], CCAssignToReg<[VI4, VI5, VI6, VI7, VI8, VI9, VI10, VI11]>>
]>;

//===----------------------------------------------------------------------===//
// DVPVU Return Value Calling Convention
//===----------------------------------------------------------------------===//

def RetCC_DVPVU : CallingConv<[
  // Vector return values in VF1-VF3
  CCIfType<[v4f32], CCAssignToReg<[VF1, VF2, VF3]>>,

  // Scalar float return values
  CCIfType<[f32], CCAssignToReg<[VF1, VF2, VF3]>>,

  // Integer return values in VI1-VI3
  CCIfType<[i16], CCAssignToReg<[VI1, VI2, VI3]>>
]>;

//===----------------------------------------------------------------------===//
// Callee-saved Registers
//===----------------------------------------------------------------------===//

// VF16-VF30: 15 callee-saved vector registers
// VI12-VI14: 3 callee-saved integer registers
// Note: VF31 is available but reserved for potential future use
//       VI15 is the link register (not callee-saved, used by BAL/JR)
def CSR_DVPVU : CalleeSavedRegs<(add VF16, VF17, VF18, VF19, VF20, VF21, VF22, VF23,
                                     VF24, VF25, VF26, VF27, VF28, VF29, VF30,
                                     VI12, VI13, VI14)>;

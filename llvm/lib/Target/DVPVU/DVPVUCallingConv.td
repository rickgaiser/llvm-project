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
// Note: The VU does not have a traditional calling convention. It operates
// as a coprocessor/shader processor. This is a minimal convention for
// LLVM compatibility.
//
// Convention:
// - VF4-VF7: First 4 vector arguments
// - VI4-VI7: First 4 integer arguments
// - VF1-VF3: Vector return values
// - VI1-VI3: Integer return values
// - VI14: Recommended stack pointer (for data memory)
// - VI15: Recommended link register
//
//===----------------------------------------------------------------------===//

//===----------------------------------------------------------------------===//
// DVPVU Argument Calling Convention
//===----------------------------------------------------------------------===//

def CC_DVPVU : CallingConv<[
  // Promote i8/i16 to i16 (native width)
  CCIfType<[i8], CCPromoteToType<i16>>,

  // Vector arguments in VF4-VF7
  CCIfType<[v4f32], CCAssignToReg<[VF4, VF5, VF6, VF7]>>,

  // Scalar float arguments - use vector registers
  CCIfType<[f32], CCAssignToReg<[VF4, VF5, VF6, VF7]>>,

  // Integer arguments in VI4-VI7
  CCIfType<[i16], CCAssignToReg<[VI4, VI5, VI6, VI7]>>
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

def CSR_DVPVU : CalleeSavedRegs<(add VF8, VF9, VF10, VF11, VF12, VF13, VF14, VF15,
                                     VF16, VF17, VF18, VF19, VF20, VF21, VF22, VF23,
                                     VF24, VF25, VF26, VF27, VF28, VF29, VF30, VF31,
                                     VI8, VI9, VI10, VI11, VI12, VI13)>;

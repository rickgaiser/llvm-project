//===- IntrinsicsVU.td - PS2 Vector Unit intrinsics --------*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// VU data memory is addressed in quadwords (16 bytes) through 16-bit integer
// registers. The memory intrinsics below take quadword addresses.
//
//===----------------------------------------------------------------------===//

let TargetPrefix = "vu" in {

// The memory intrinsics carry a memory operand (see
// VUTargetLowering::getTgtMemIntrinsic).

// Load the quadword at a quadword address.
def int_vu_lq : DefaultAttrsIntrinsic<[llvm_v4f32_ty], [llvm_i16_ty],
                                      [IntrReadMem], "", [SDNPMemOperand]>;

// Store a quadword. The mask selects the fields that are written
// (x = 8, y = 4, z = 2, w = 1).
def int_vu_sq : DefaultAttrsIntrinsic<[], [llvm_v4f32_ty, llvm_i16_ty],
                                      [IntrWriteMem], "", [SDNPMemOperand]>;
def int_vu_sq_mask
    : DefaultAttrsIntrinsic<[], [llvm_v4f32_ty, llvm_i16_ty, llvm_i32_ty],
                            [IntrWriteMem, ImmArg<ArgIndex<2>>], "",
                            [SDNPMemOperand]>;

// Load or store the low 16 bits of one field (x = 0, y = 1, z = 2, w = 3) of
// a quadword.
def int_vu_ilw : DefaultAttrsIntrinsic<[llvm_i16_ty], [llvm_i16_ty, llvm_i32_ty],
                                       [IntrReadMem, ImmArg<ArgIndex<1>>], "",
                                       [SDNPMemOperand]>;
def int_vu_isw
    : DefaultAttrsIntrinsic<[], [llvm_i16_ty, llvm_i16_ty, llvm_i32_ty],
                            [IntrWriteMem, ImmArg<ArgIndex<2>>], "",
                            [SDNPMemOperand]>;

// VIF double buffering: the quadword address of the current (TOP) buffer and
// the value of ITOP.
def int_vu_xtop : DefaultAttrsIntrinsic<[llvm_i16_ty], [],
                                        [IntrInaccessibleMemOnly]>;
def int_vu_xitop : DefaultAttrsIntrinsic<[llvm_i16_ty], [],
                                         [IntrInaccessibleMemOnly]>;

// Start a GIF (PATH1) transfer of the GS packet at a quadword address. The
// GIF reads VU data memory.
def int_vu_xgkick : DefaultAttrsIntrinsic<[], [llvm_i16_ty], []>;

} // TargetPrefix = "vu"

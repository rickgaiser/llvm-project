//===-- VURegisterInfo.td - VU register definitions --------*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

class VUReg<bits<16> Enc, string n> : Register<n> {
  let HWEncoding = Enc;
  let Namespace = "VU";
}

// 128-bit floating-point registers. VF0 is hardwired to (0, 0, 0, 1), a
// constant: reading it does not keep an instruction in a loop.
let isConstant = true in
def VF0 : VUReg<0, "vf0">, DwarfRegNum<[0]>;
foreach i = 1...31 in
  def VF#i : VUReg<i, "vf"#i>, DwarfRegNum<[i]>;

// 16-bit integer registers. VI0 is hardwired to 0.
let isConstant = true in
def VI0 : VUReg<0, "vi0">, DwarfRegNum<[32]>;
foreach i = 1...15 in
  def VI#i : VUReg<i, "vi"#i>, DwarfRegNum<[!add(i, 32)]>;

// Special registers. They only appear as fixed operands in the assembly
// syntax ("acc", "i", "q", "p", "r") and are never encoded.
def ACC : VUReg<0, "acc">, DwarfRegNum<[48]>; // Accumulator
def I   : VUReg<0, "i">,   DwarfRegNum<[49]>; // Immediate (LOI)
def Q   : VUReg<0, "q">,   DwarfRegNum<[50]>; // FDIV result
def P   : VUReg<0, "p">,   DwarfRegNum<[51]>; // EFU result (VU1)
def R   : VUReg<0, "r">,   DwarfRegNum<[52]>; // Random number
def CF  : VUReg<0, "cf">,  DwarfRegNum<[53]>; // Clipping flags

// VF registers hold vectors, and scalar floats in the x field.
def VF : RegisterClass<"VU", [v4f32, v4i32, f32], 128,
                       (sequence "VF%u", 0, 31)>;
def VI : RegisterClass<"VU", [i16], 16, (sequence "VI%u", 0, 15)>;
def ACCR : RegisterClass<"VU", [v4f32], 128, (add ACC)>;
// The result of the clipping flag tests (FCAND, FCOR, FCEQ).
def VI1R : RegisterClass<"VU", [i16], 16, (add VI1)>;
// An ACC value that is not read or written by an FMAC instruction for a while
// can be kept in a VF register; see getLargestLegalSuperClass.
def ACCVF : RegisterClass<"VU", [v4f32], 128, (add VF, ACC)>;
// Q holds a scalar, like the x field of a VF register.
def QR : RegisterClass<"VU", [f32], 32, (add Q)> {
  let Size = 128;
}
def QVF : RegisterClass<"VU", [f32], 128, (add VF, Q)>;
// i32 is listed so that TableGen can type generic patterns; it is not a legal
// type for instruction selection.
def SPR : RegisterClass<"VU", [f32, i32], 32, (add I, Q, P, R)>;

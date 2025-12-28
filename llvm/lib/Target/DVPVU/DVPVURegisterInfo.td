//===- DVPVURegisterInfo.td - DVPVU Register defs ----------*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Declarations that describe the PS2 VU register file.
//
// VU Register File:
// - VF0-VF31: 32 x 128-bit vector float registers (4 x 32-bit float)
//   - VF0 is constant: (0.0, 0.0, 0.0, 1.0) for x, y, z, w
// - VI0-VI15: 16 x 16-bit integer registers
//   - VI0 is constant 0
// - Special registers:
//   - ACC: 128-bit accumulator (4 x 32-bit float)
//   - I: 32-bit immediate register
//   - Q: 32-bit division/sqrt result
//   - P: 32-bit EFU result (VU1 only)
//   - R: 32-bit random number register
//
//===----------------------------------------------------------------------===//

let Namespace = "DVPVU" in {
  // Subregister indices for vector components
  def sub_x : SubRegIndex<32, 0>;
  def sub_y : SubRegIndex<32, 32>;
  def sub_z : SubRegIndex<32, 64>;
  def sub_w : SubRegIndex<32, 96>;
}

//===----------------------------------------------------------------------===//
// Vector Float Registers (VF0-VF31) - 128-bit
//===----------------------------------------------------------------------===//

class VFReg<bits<5> num, string n> : Register<n> {
  field bits<5> Num = num;
  let Namespace = "DVPVU";
  let HWEncoding{4-0} = num;
}

// Define all 32 vector float registers
foreach i = 0-31 in {
  def VF#i : VFReg<i, "vf"#i>, DwarfRegNum<[i]>;
}

//===----------------------------------------------------------------------===//
// Vector Integer Registers (VI0-VI15) - 16-bit
//===----------------------------------------------------------------------===//

class VIReg<bits<4> num, string n> : Register<n> {
  field bits<4> Num = num;
  let Namespace = "DVPVU";
  let HWEncoding{3-0} = num;
}

// Define all 16 integer registers
foreach i = 0-15 in {
  def VI#i : VIReg<i, "vi"#i>, DwarfRegNum<[!add(i, 32)]>;
}

//===----------------------------------------------------------------------===//
// Special Registers
//===----------------------------------------------------------------------===//

// Accumulator register (128-bit, same as VF)
def ACC : Register<"acc"> {
  let Namespace = "DVPVU";
  let HWEncoding = 0;
}

// Immediate register (32-bit float)
def I_REG : Register<"i"> {
  let Namespace = "DVPVU";
  let HWEncoding = 0;
}

// Division/sqrt result register (32-bit float)
def Q : Register<"q"> {
  let Namespace = "DVPVU";
  let HWEncoding = 0;
}

// EFU result register (32-bit float, VU1 only)
def P : Register<"p"> {
  let Namespace = "DVPVU";
  let HWEncoding = 0;
}

// Random number register (32-bit)
def R : Register<"r"> {
  let Namespace = "DVPVU";
  let HWEncoding = 0;
}

// Status flags register (16-bit)
def SF : Register<"sf"> {
  let Namespace = "DVPVU";
  let HWEncoding = 0;
}

// MAC flags register (16-bit)
def MAC : Register<"mac"> {
  let Namespace = "DVPVU";
  let HWEncoding = 0;
}

// Clipping flags register (24-bit)
def CF : Register<"cf"> {
  let Namespace = "DVPVU";
  let HWEncoding = 0;
}

//===----------------------------------------------------------------------===//
// Register Classes
//===----------------------------------------------------------------------===//

// Vector float register class (VF0-VF31)
// VF0 is constant but still in the class; reserved in RegisterInfo.cpp
def VFRegs : RegisterClass<"DVPVU", [v4f32], 128,
    (add (sequence "VF%u", 1, 31), VF0)> {
  let Size = 128;
}

// Allocatable vector float registers (excludes VF0)
def VFRegsNoVF0 : RegisterClass<"DVPVU", [v4f32], 128,
    (sequence "VF%u", 1, 31)> {
  let Size = 128;
}

// Vector integer register class (VI0-VI15)
// VI0 is constant but still in the class; reserved in RegisterInfo.cpp
def VIRegs : RegisterClass<"DVPVU", [i16], 16,
    (add (sequence "VI%u", 1, 15), VI0)> {
  let Size = 16;
}

// Allocatable integer registers (excludes VI0)
def VIRegsNoVI0 : RegisterClass<"DVPVU", [i16], 16,
    (sequence "VI%u", 1, 15)> {
  let Size = 16;
}

// Accumulator register class
def ACCRegs : RegisterClass<"DVPVU", [v4f32], 128, (add ACC)> {
  let Size = 128;
  let isAllocatable = 0;
}

// I register class (immediate)
def IRegs : RegisterClass<"DVPVU", [f32], 32, (add I_REG)> {
  let Size = 32;
  let isAllocatable = 0;
}

// Q register class (division result)
def QRegs : RegisterClass<"DVPVU", [f32], 32, (add Q)> {
  let Size = 32;
  let isAllocatable = 0;
}

// P register class (EFU result, VU1 only)
def PRegs : RegisterClass<"DVPVU", [f32], 32, (add P)> {
  let Size = 32;
  let isAllocatable = 0;
}

// R register class (random)
def RRegs : RegisterClass<"DVPVU", [i32], 32, (add R)> {
  let Size = 32;
  let isAllocatable = 0;
}

// Status flags register class
def SFRegs : RegisterClass<"DVPVU", [i16], 16, (add SF)> {
  let Size = 16;
  let CopyCost = -1;
  let isAllocatable = 0;
}

// MAC flags register class
def MACRegs : RegisterClass<"DVPVU", [i16], 16, (add MAC)> {
  let Size = 16;
  let CopyCost = -1;
  let isAllocatable = 0;
}

// Clipping flags register class
def CFRegs : RegisterClass<"DVPVU", [i32], 32, (add CF)> {
  let Size = 32;
  let CopyCost = -1;
  let isAllocatable = 0;
}

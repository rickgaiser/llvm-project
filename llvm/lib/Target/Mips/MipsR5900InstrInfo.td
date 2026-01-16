//===- MipsR5900InstrInfo.td - R5900 EE Core/MMI Instructions -*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file describes R5900 EE core and MMI (Multimedia Instructions).
// Requires: MipsR5900InstrFormats.td
//
//===----------------------------------------------------------------------===//

//===----------------------------------------------------------------------===//
// R5900 SDNodes
//===----------------------------------------------------------------------===//

// R5900 Parallel Mult/Add nodes - return i64 directly to GPR
// These operate on 64-bit registers but only use lower 32 bits for multiply.
// PMult/PMultu: rd[63:0] = rs[31:0] * rt[31:0] (widening 32x32->64)
// PMAdd/PMAddu: rd[63:0] = HI:LO + rs[31:0] * rt[31:0] (widening multiply-add)
// Ordering ensured by glue: PMult outputs glue, PMAdd inputs/outputs glue.
def SDT_PMult : SDTypeProfile<1, 2, [SDTCisVT<0, i64>, SDTCisVT<1, i64>,
                                     SDTCisSameAs<1, 2>]>;
def MipsPMult  : SDNode<"MipsISD::PMult", SDT_PMult, [SDNPOutGlue]>;
def MipsPMultu : SDNode<"MipsISD::PMultu", SDT_PMult, [SDNPOutGlue]>;
def MipsPMAdd  : SDNode<"MipsISD::PMAdd", SDT_PMult, [SDNPInGlue, SDNPOutGlue]>;
def MipsPMAddu : SDNode<"MipsISD::PMAddu", SDT_PMult, [SDNPInGlue, SDNPOutGlue]>;
def MipsPMSub  : SDNode<"MipsISD::PMSub", SDT_PMult, [SDNPInGlue, SDNPOutGlue]>;

// R5900 PLZCW - count leading bits matching MSB, returns (count - 1)
def MipsPLZCW : SDNode<"MipsISD::PLZCW", SDTIntUnaryOp>;

//===----------------------------------------------------------------------===//
// R5900 Enable/Disable Interrupt Instructions
//===----------------------------------------------------------------------===//

let Predicates = [HasR5900] in {
  let DecoderNamespace = "R5900" in {
    def EI_R5900 : ER_FT<"ei", II_EI>, EI_FM_R5900<0x38>;
    def DI_R5900 : ER_FT<"di", II_DI>, EI_FM_R5900<0x39>;
  }
}

//===----------------------------------------------------------------------===//
// R5900 Breakpoint Register Instructions
//===----------------------------------------------------------------------===//
// MFBPC/MFIAB/MFIABM/MFDAB/MFDABM/MFDVB/MFDVBM - move from breakpoint registers
// MTBPC/MTIAB/MTIABM/MTDAB/MTDABM/MTDVB/MTDVBM - move to breakpoint registers

let Predicates = [HasR5900] in {
  let DecoderNamespace = "R5900" in {
    // Move from breakpoint registers (read breakpoint reg to GPR)
    def MFBPC : InstSE<(outs GPR32Opnd:$rt), (ins), "mfbpc\t$rt", [], II_MFC0,
                       FrmFR, "mfbpc">, MFBP_FM_R5900<0>;
    def MFIAB : InstSE<(outs GPR32Opnd:$rt), (ins), "mfiab\t$rt", [], II_MFC0,
                       FrmFR, "mfiab">, MFBP_FM_R5900<2>;
    def MFIABM : InstSE<(outs GPR32Opnd:$rt), (ins), "mfiabm\t$rt", [], II_MFC0,
                        FrmFR, "mfiabm">, MFBP_FM_R5900<3>;
    def MFDAB : InstSE<(outs GPR32Opnd:$rt), (ins), "mfdab\t$rt", [], II_MFC0,
                       FrmFR, "mfdab">, MFBP_FM_R5900<4>;
    def MFDABM : InstSE<(outs GPR32Opnd:$rt), (ins), "mfdabm\t$rt", [], II_MFC0,
                        FrmFR, "mfdabm">, MFBP_FM_R5900<5>;
    def MFDVB : InstSE<(outs GPR32Opnd:$rt), (ins), "mfdvb\t$rt", [], II_MFC0,
                       FrmFR, "mfdvb">, MFBP_FM_R5900<6>;
    def MFDVBM : InstSE<(outs GPR32Opnd:$rt), (ins), "mfdvbm\t$rt", [], II_MFC0,
                        FrmFR, "mfdvbm">, MFBP_FM_R5900<7>;
    // Move to breakpoint registers (write GPR to breakpoint reg)
    def MTBPC : InstSE<(outs), (ins GPR32Opnd:$rt), "mtbpc\t$rt", [], II_MTC0,
                       FrmFR, "mtbpc">, MTBP_FM_R5900<0>;
    def MTIAB : InstSE<(outs), (ins GPR32Opnd:$rt), "mtiab\t$rt", [], II_MTC0,
                       FrmFR, "mtiab">, MTBP_FM_R5900<2>;
    def MTIABM : InstSE<(outs), (ins GPR32Opnd:$rt), "mtiabm\t$rt", [], II_MTC0,
                        FrmFR, "mtiabm">, MTBP_FM_R5900<3>;
    def MTDAB : InstSE<(outs), (ins GPR32Opnd:$rt), "mtdab\t$rt", [], II_MTC0,
                       FrmFR, "mtdab">, MTBP_FM_R5900<4>;
    def MTDABM : InstSE<(outs), (ins GPR32Opnd:$rt), "mtdabm\t$rt", [], II_MTC0,
                        FrmFR, "mtdabm">, MTBP_FM_R5900<5>;
    def MTDVB : InstSE<(outs), (ins GPR32Opnd:$rt), "mtdvb\t$rt", [], II_MTC0,
                       FrmFR, "mtdvb">, MTBP_FM_R5900<6>;
    def MTDVBM : InstSE<(outs), (ins GPR32Opnd:$rt), "mtdvbm\t$rt", [], II_MTC0,
                        FrmFR, "mtdvbm">, MTBP_FM_R5900<7>;
    // COP0 conditional branches (branch on COP0 condition)
    let isBranch = 1, isTerminator = 1, hasDelaySlot = 1 in {
      def BC0F : InstSE<(outs), (ins brtarget:$offset), "bc0f\t$offset", [],
                        II_BC, FrmI, "bc0f">, BC0F_FM_R5900<0>;
      def BC0T : InstSE<(outs), (ins brtarget:$offset), "bc0t\t$offset", [],
                        II_BC, FrmI, "bc0t">, BC0F_FM_R5900<1>;
    }
  }
}

//===----------------------------------------------------------------------===//
// R5900 Sync Aliases
//===----------------------------------------------------------------------===//
// sync.l (stype=0) and sync.p (stype=0x10)

let Predicates = [HasR5900] in {
  def : MipsInstAlias<"sync.l", (SYNC 0), 1>;
  def : MipsInstAlias<"sync.p", (SYNC 0x10), 1>;
}

//===----------------------------------------------------------------------===//
// R5900 EE Core Integer Instructions
//===----------------------------------------------------------------------===//

let Predicates = [HasR5900] in {

//===----------------------------------------------------------------------===//
// R5900 3-Operand Multiply (Pipeline 0) - writes to rd, HI, LO
//===----------------------------------------------------------------------===//

// 3-operand MULT: (rd, HI, LO) <- rs * rt (signed)
// Uses mul pattern - R5900 uses this instead of MIPS32's MUL instruction
let Defs = [HI0, LO0], isCommutable = 1 in
def R5900_MULT : InstSE<(outs GPR32Opnd:$rd),
                        (ins GPR32Opnd:$rs, GPR32Opnd:$rt),
                        "mult\t$rd, $rs, $rt",
                        [(set GPR32Opnd:$rd, (mul GPR32Opnd:$rs, GPR32Opnd:$rt))],
                        II_MULT, FrmR>,
                 R5900_MULT3_FM<0x18>;

// 3-operand MULTU: (rd, HI, LO) <- rs * rt (unsigned)
// Note: low 32 bits of signed/unsigned multiply are identical, so both use mul
let Defs = [HI0, LO0], isCommutable = 1 in
def R5900_MULTU : InstSE<(outs GPR32Opnd:$rd),
                         (ins GPR32Opnd:$rs, GPR32Opnd:$rt),
                         "multu\t$rd, $rs, $rt", [], II_MULT, FrmR>,
                  R5900_MULT3_FM<0x19>;

//===----------------------------------------------------------------------===//
// R5900 MADD/MADDU/MSUB/MSUBU (Pipeline 0, MMI opcode)
//===----------------------------------------------------------------------===//

// 3-operand MADD: (rd, HI, LO) <- (HI,LO) + rs * rt (signed)
let Defs = [HI0, LO0], Uses = [HI0, LO0] in
def R5900_MADD : InstSE<(outs GPR32Opnd:$rd),
                        (ins GPR32Opnd:$rs, GPR32Opnd:$rt),
                        "madd\t$rd, $rs, $rt", [], II_MADD, FrmR>,
                 R5900_MMI_3R_FM<0x00>;

// 3-operand MADDU: (rd, HI, LO) <- (HI,LO) + rs * rt (unsigned)
let Defs = [HI0, LO0], Uses = [HI0, LO0] in
def R5900_MADDU : InstSE<(outs GPR32Opnd:$rd),
                         (ins GPR32Opnd:$rs, GPR32Opnd:$rt),
                         "maddu\t$rd, $rs, $rt", [], II_MADD, FrmR>,
                  R5900_MMI_3R_FM<0x01>;

// Pseudo instruction for multiply-add chain: (add (mul a, b), (mul c, d))
// Expands to: R5900_MULT + R5900_MADD
// This saves one instruction compared to MULT + MULT + ADDU
let usesCustomInserter = 1, Defs = [HI0, LO0], Predicates = [HasR5900] in
def PseudoR5900MulMulAdd : PseudoSE<
    (outs GPR32Opnd:$rd),
    (ins GPR32Opnd:$a, GPR32Opnd:$b, GPR32Opnd:$c, GPR32Opnd:$d),
    [(set GPR32Opnd:$rd, (add (mul GPR32Opnd:$a, GPR32Opnd:$b),
                              (mul GPR32Opnd:$c, GPR32Opnd:$d)))],
    II_MADD>;

//===----------------------------------------------------------------------===//
// R5900 Pipeline 1 Move Instructions
//===----------------------------------------------------------------------===//

let Uses = [HI1] in
def MFHI1 : InstSE<(outs GPR32Opnd:$rd), (ins),
                   "mfhi1\t$rd", [], II_MFHI_MFLO, FrmR>,
            R5900_MFHILO1_FM<0x10>;

let Uses = [LO1] in
def MFLO1 : InstSE<(outs GPR32Opnd:$rd), (ins),
                   "mflo1\t$rd", [], II_MFHI_MFLO, FrmR>,
            R5900_MFHILO1_FM<0x12>;

let Defs = [HI1] in
def MTHI1 : InstSE<(outs), (ins GPR32Opnd:$rs),
                   "mthi1\t$rs", [], II_MTHI_MTLO, FrmR>,
            R5900_MTHILO1_FM<0x11>;

let Defs = [LO1] in
def MTLO1 : InstSE<(outs), (ins GPR32Opnd:$rs),
                   "mtlo1\t$rs", [], II_MTHI_MTLO, FrmR>,
            R5900_MTHILO1_FM<0x13>;

//===----------------------------------------------------------------------===//
// R5900 Pipeline 1 Multiply/Divide
//===----------------------------------------------------------------------===//

// 3-operand MULT1: (rd, HI1, LO1) <- rs * rt (signed)
let Defs = [HI1, LO1], isCommutable = 1 in
def MULT1 : InstSE<(outs GPR32Opnd:$rd),
                   (ins GPR32Opnd:$rs, GPR32Opnd:$rt),
                   "mult1\t$rd, $rs, $rt", [], II_MULT, FrmR>,
            R5900_MMI_3R_FM<0x18>;

// 3-operand MULTU1: (rd, HI1, LO1) <- rs * rt (unsigned)
let Defs = [HI1, LO1], isCommutable = 1 in
def MULTU1 : InstSE<(outs GPR32Opnd:$rd),
                    (ins GPR32Opnd:$rs, GPR32Opnd:$rt),
                    "multu1\t$rd, $rs, $rt", [], II_MULT, FrmR>,
             R5900_MMI_3R_FM<0x19>;

// DIV1: (HI1, LO1) <- rs / rt (signed)
let Defs = [HI1, LO1] in
def DIV1 : InstSE<(outs), (ins GPR32Opnd:$rs, GPR32Opnd:$rt),
                  "div1\t$$zero, $rs, $rt", [], II_DIV, FrmR>,
           R5900_MMI_2R_FM<0x1a>;

// DIVU1: (HI1, LO1) <- rs / rt (unsigned)
let Defs = [HI1, LO1] in
def DIVU1 : InstSE<(outs), (ins GPR32Opnd:$rs, GPR32Opnd:$rt),
                   "divu1\t$$zero, $rs, $rt", [], II_DIV, FrmR>,
            R5900_MMI_2R_FM<0x1b>;

//===----------------------------------------------------------------------===//
// PLZCW - Parallel Leading Zero or one Count Word
// Counts leading zeros/ones in two 32-bit words in parallel
//===----------------------------------------------------------------------===//

// PLZCW rd, rs - Count leading zeros/ones in rs[31:0] and rs[63:32]
// Result is (count - 1) stored in rd[31:0] and rd[63:32] respectively
let DecoderNamespace = "R5900" in
def PLZCW : InstSE<(outs GPR64Opnd:$rd), (ins GPR64Opnd:$rs),
                   "plzcw\t$rd, $rs", [], II_CLZ, FrmR>,
            R5900_MMI_PLZCW_FM;

//===----------------------------------------------------------------------===//
// R5900 Pipeline 1 Multiply-Add
// Note: MADD1/MADDU1 use DecoderNamespace to avoid conflict with CLZ/CLO
//===----------------------------------------------------------------------===//

// 3-operand MADD1: (rd, HI1, LO1) <- (HI1,LO1) + rs * rt (signed)
let Defs = [HI1, LO1], Uses = [HI1, LO1], DecoderNamespace = "R5900" in
def MADD1 : InstSE<(outs GPR32Opnd:$rd),
                   (ins GPR32Opnd:$rs, GPR32Opnd:$rt),
                   "madd1\t$rd, $rs, $rt", [], II_MADD, FrmR>,
            R5900_MMI_3R_FM<0x20>;

// 3-operand MADDU1: (rd, HI1, LO1) <- (HI1,LO1) + rs * rt (unsigned)
let Defs = [HI1, LO1], Uses = [HI1, LO1], DecoderNamespace = "R5900" in
def MADDU1 : InstSE<(outs GPR32Opnd:$rd),
                    (ins GPR32Opnd:$rs, GPR32Opnd:$rt),
                    "maddu1\t$rd, $rs, $rt", [], II_MADD, FrmR>,
             R5900_MMI_3R_FM<0x21>;

//===----------------------------------------------------------------------===//
// PMFHL - Parallel Move From HI/LO Register
// Combines HI and LO into a single register in one instruction.
//===----------------------------------------------------------------------===//

// PMFHL.LW - Move HI:LO to GPR64 in word units
// rd[31:0] = LO[31:0], rd[63:32] = HI[31:0]
// This is perfect for extracting 64-bit result from 32x32 multiply.
let Uses = [HI0, LO0], DecoderNamespace = "R5900" in
def PMFHL_LW : InstSE<(outs GPR64Opnd:$rd), (ins),
                      "pmfhl.lw\t$rd", [], II_MFHI_MFLO, FrmR>,
               R5900_MMI_PMFHL_FM<0b00000>;

// PMFHL.UW - Move HI:LO to GPR64 (upper word units)
// rd[31:0] = LO[63:32], rd[63:32] = HI[63:32]
let Uses = [HI0, LO0], DecoderNamespace = "R5900" in
def PMFHL_UW : InstSE<(outs GPR64Opnd:$rd), (ins),
                      "pmfhl.uw\t$rd", [], II_MFHI_MFLO, FrmR>,
               R5900_MMI_PMFHL_FM<0b00001>;

// PMFHL.SLW - Move HI:LO to GPR64 (saturating lower word)
// rd = saturate(HI:LO[63:0]) to 32-bit signed, sign-extended to 64-bit
let Uses = [HI0, LO0], DecoderNamespace = "R5900" in
def PMFHL_SLW : InstSE<(outs GPR64Opnd:$rd), (ins),
                       "pmfhl.slw\t$rd", [], II_MFHI_MFLO, FrmR>,
                R5900_MMI_PMFHL_FM<0b00010>;

// PMFHL.LH - Move HI:LO to GPR64 (lower halfwords packed)
// rd = {LO[47:32], LO[15:0], HI[47:32], HI[15:0]}
let Uses = [HI0, LO0], DecoderNamespace = "R5900" in
def PMFHL_LH : InstSE<(outs GPR64Opnd:$rd), (ins),
                      "pmfhl.lh\t$rd", [], II_MFHI_MFLO, FrmR>,
               R5900_MMI_PMFHL_FM<0b00011>;

// PMFHL.SH - Move HI:LO to GPR64 (saturating halfwords)
// rd = saturating pack of HI:LO halfwords
let Uses = [HI0, LO0], DecoderNamespace = "R5900" in
def PMFHL_SH : InstSE<(outs GPR64Opnd:$rd), (ins),
                      "pmfhl.sh\t$rd", [], II_MFHI_MFLO, FrmR>,
               R5900_MMI_PMFHL_FM<0b00100>;

// PMTHL.LW - Move GPR64 to HI:LO in word units
// LO[31:0] = rs[31:0], HI[31:0] = rs[63:32]
// This is the inverse of PMFHL.LW, used for restoring HI:LO from a GPR64.
let Defs = [HI0, LO0], DecoderNamespace = "R5900" in
def PMTHL_LW : InstSE<(outs), (ins GPR64Opnd:$rs),
                      "pmthl.lw\t$rs", [], II_MTHI_MTLO, FrmR>,
               R5900_MMI_PMTHL_FM<0b00000>;

//===----------------------------------------------------------------------===//
// PMFHI/PMFLO/PMTHI/PMTLO - Parallel Move From/To HI/LO
// These operate on the 128-bit HI:LO register pair.
//===----------------------------------------------------------------------===//

// PMFHI - Parallel Move From HI Register
// rd = HI (128-bit)
let Uses = [HI0], DecoderNamespace = "R5900" in
def PMFHI : InstSE<(outs GPR64Opnd:$rd), (ins),
                   "pmfhi\t$rd", [], II_MFHI_MFLO, FrmR>,
            R5900_MMI2_RD_FM<0b01000>;

// PMFLO - Parallel Move From LO Register
// rd = LO (128-bit)
let Uses = [LO0], DecoderNamespace = "R5900" in
def PMFLO : InstSE<(outs GPR64Opnd:$rd), (ins),
                   "pmflo\t$rd", [], II_MFHI_MFLO, FrmR>,
            R5900_MMI2_RD_FM<0b01001>;

// PMTHI - Parallel Move To HI Register
// HI = rs (128-bit)
let Defs = [HI0], DecoderNamespace = "R5900" in
def PMTHI : InstSE<(outs), (ins GPR64Opnd:$rs),
                   "pmthi\t$rs", [], II_MTHI_MTLO, FrmR>,
            R5900_MMI3_RS_FM<0b01000>;

// PMTLO - Parallel Move To LO Register
// LO = rs (128-bit)
let Defs = [LO0], DecoderNamespace = "R5900" in
def PMTLO : InstSE<(outs), (ins GPR64Opnd:$rs),
                   "pmtlo\t$rs", [], II_MTHI_MTLO, FrmR>,
            R5900_MMI3_RS_FM<0b01001>;

//===----------------------------------------------------------------------===//
// PMULTW/PMADDW - Parallel Multiply/Multiply-Add Word
// These instructions perform 32x32->64 widening multiply with result directly
// in GPR64, eliminating the need for mfhi/mflo or pmfhl.lw extraction.
//
// PMULTW rd, rs, rt:
//   - Computes: rd[63:0] = sign_extend(rs[31:0]) * sign_extend(rt[31:0])
//   - Also writes to LO[63:0] (sign-extended) and HI (for upper lane, unused)
//   - Key advantage: 64-bit result directly in rd, no extraction needed
//
// PMADDW rd, rs, rt:
//   - Computes: rd[63:0] = LO[63:0] + sign_extend(rs[31:0]) * sign_extend(rt[31:0])
//   - Accumulates in LO[63:0], writes accumulated result to rd
//   - Perfect for widening MADD chains: PMULTW + N×PMADDW
//===----------------------------------------------------------------------===//

// PMULTW - Parallel Multiply Word (signed)
// rd[63:0] = rs[31:0] * rt[31:0], also writes to HI:LO
let Defs = [HI0, LO0], DecoderNamespace = "R5900" in
def PMULTW : InstSE<(outs GPR64Opnd:$rd), (ins GPR64Opnd:$rs, GPR64Opnd:$rt),
                    "pmultw\t$rd, $rs, $rt",
                    [(set GPR64Opnd:$rd, (MipsPMult GPR64Opnd:$rs, GPR64Opnd:$rt))],
                    II_MULT, FrmR>,
             R5900_MMI2_FM<0b01100>;

// PMADDW - Parallel Multiply-Add Word (signed)
// rd[63:0] = LO[63:0] + rs[31:0] * rt[31:0], accumulates in HI:LO
// Ordering with previous PMULTW/PMADDW ensured by Uses = [HI0, LO0].
let Defs = [HI0, LO0], Uses = [HI0, LO0], DecoderNamespace = "R5900" in
def PMADDW : InstSE<(outs GPR64Opnd:$rd), (ins GPR64Opnd:$rs, GPR64Opnd:$rt),
                    "pmaddw\t$rd, $rs, $rt",
                    [(set GPR64Opnd:$rd, (MipsPMAdd GPR64Opnd:$rs, GPR64Opnd:$rt))],
                    II_MADD, FrmR>,
             R5900_MMI2_FM<0b00000>;

// PMSUBW - Parallel Multiply-Subtract Word (signed)
// rd[63:0] = LO[63:0] - rs[31:0] * rt[31:0], subtracts from HI:LO
let Defs = [HI0, LO0], Uses = [HI0, LO0], DecoderNamespace = "R5900" in
def PMSUBW : InstSE<(outs GPR64Opnd:$rd), (ins GPR64Opnd:$rs, GPR64Opnd:$rt),
                    "pmsubw\t$rd, $rs, $rt",
                    [(set GPR64Opnd:$rd, (MipsPMSub GPR64Opnd:$rs, GPR64Opnd:$rt))],
                    II_MADD, FrmR>,
             R5900_MMI2_FM<0b00100>;

// PMULTUW - Parallel Multiply Word (unsigned)
// rd[63:0] = rs[31:0] * rt[31:0] (unsigned), also writes to HI:LO
let Defs = [HI0, LO0], DecoderNamespace = "R5900" in
def PMULTUW : InstSE<(outs GPR64Opnd:$rd), (ins GPR64Opnd:$rs, GPR64Opnd:$rt),
                     "pmultuw\t$rd, $rs, $rt",
                     [(set GPR64Opnd:$rd, (MipsPMultu GPR64Opnd:$rs, GPR64Opnd:$rt))],
                     II_MULT, FrmR>,
              R5900_MMI3_FM<0b01100>;

// PMADDUW - Parallel Multiply-Add Word (unsigned)
// rd[63:0] = LO[63:0] + rs[31:0] * rt[31:0] (unsigned), accumulates in HI:LO
let Defs = [HI0, LO0], Uses = [HI0, LO0], DecoderNamespace = "R5900" in
def PMADDUW : InstSE<(outs GPR64Opnd:$rd), (ins GPR64Opnd:$rs, GPR64Opnd:$rt),
                     "pmadduw\t$rd, $rs, $rt",
                     [(set GPR64Opnd:$rd, (MipsPMAddu GPR64Opnd:$rs, GPR64Opnd:$rt))],
                     II_MADD, FrmR>,
              R5900_MMI3_FM<0b00000>;

//===----------------------------------------------------------------------===//
// PADD/PSUB - Parallel Add/Subtract
// These perform SIMD add/subtract on packed words, halfwords, or bytes.
//===----------------------------------------------------------------------===//

// PADDW - Parallel Add Word
// rd = rs + rt for each of 4 packed 32-bit words
let DecoderNamespace = "R5900" in
def PADDW : InstSE<(outs GPR128Opnd:$rd), (ins GPR128Opnd:$rs, GPR128Opnd:$rt),
                   "paddw\t$rd, $rs, $rt",
                   [(set v4i32:$rd, (add v4i32:$rs, v4i32:$rt))],
                   II_ADD, FrmR>,
            R5900_MMI0_FM<0b00000>;

// PSUBW - Parallel Subtract Word
// rd = rs - rt for each of 4 packed 32-bit words
let DecoderNamespace = "R5900" in
def PSUBW : InstSE<(outs GPR128Opnd:$rd), (ins GPR128Opnd:$rs, GPR128Opnd:$rt),
                   "psubw\t$rd, $rs, $rt",
                   [(set v4i32:$rd, (sub v4i32:$rs, v4i32:$rt))],
                   II_ADD, FrmR>,
            R5900_MMI0_FM<0b00001>;

// PADDH - Parallel Add Halfword
// rd = rs + rt for each of 8 packed 16-bit halfwords
let DecoderNamespace = "R5900" in
def PADDH : InstSE<(outs GPR128Opnd:$rd), (ins GPR128Opnd:$rs, GPR128Opnd:$rt),
                   "paddh\t$rd, $rs, $rt",
                   [(set v8i16:$rd, (add v8i16:$rs, v8i16:$rt))],
                   II_ADD, FrmR>,
            R5900_MMI0_FM<0b00100>;

// PSUBH - Parallel Subtract Halfword
// rd = rs - rt for each of 8 packed 16-bit halfwords
let DecoderNamespace = "R5900" in
def PSUBH : InstSE<(outs GPR128Opnd:$rd), (ins GPR128Opnd:$rs, GPR128Opnd:$rt),
                   "psubh\t$rd, $rs, $rt",
                   [(set v8i16:$rd, (sub v8i16:$rs, v8i16:$rt))],
                   II_ADD, FrmR>,
            R5900_MMI0_FM<0b00101>;

// PADDB - Parallel Add Byte
// rd = rs + rt for each of 16 packed 8-bit bytes
let DecoderNamespace = "R5900" in
def PADDB : InstSE<(outs GPR128Opnd:$rd), (ins GPR128Opnd:$rs, GPR128Opnd:$rt),
                   "paddb\t$rd, $rs, $rt",
                   [(set v16i8:$rd, (add v16i8:$rs, v16i8:$rt))],
                   II_ADD, FrmR>,
            R5900_MMI0_FM<0b01000>;

// PSUBB - Parallel Subtract Byte
// rd = rs - rt for each of 16 packed 8-bit bytes
let DecoderNamespace = "R5900" in
def PSUBB : InstSE<(outs GPR128Opnd:$rd), (ins GPR128Opnd:$rs, GPR128Opnd:$rt),
                   "psubb\t$rd, $rs, $rt",
                   [(set v16i8:$rd, (sub v16i8:$rs, v16i8:$rt))],
                   II_ADD, FrmR>,
            R5900_MMI0_FM<0b01001>;

//===----------------------------------------------------------------------===//
// PMAX/PMIN - Parallel Maximum/Minimum
// These perform SIMD max/min operations on packed words or halfwords.
//===----------------------------------------------------------------------===//

// PMAXW - Parallel Maximum Word
// rd = max(rs, rt) for each of 4 packed 32-bit signed words
let DecoderNamespace = "R5900" in
def PMAXW : InstSE<(outs GPR128Opnd:$rd), (ins GPR128Opnd:$rs, GPR128Opnd:$rt),
                   "pmaxw\t$rd, $rs, $rt",
                   [(set v4i32:$rd, (smax v4i32:$rs, v4i32:$rt))],
                   II_AND, FrmR>,
            R5900_MMI0_FM<0b00011>;

// PMAXH - Parallel Maximum Halfword
// rd = max(rs, rt) for each of 8 packed 16-bit signed halfwords
let DecoderNamespace = "R5900" in
def PMAXH : InstSE<(outs GPR128Opnd:$rd), (ins GPR128Opnd:$rs, GPR128Opnd:$rt),
                   "pmaxh\t$rd, $rs, $rt",
                   [(set v8i16:$rd, (smax v8i16:$rs, v8i16:$rt))],
                   II_AND, FrmR>,
            R5900_MMI0_FM<0b00111>;

// PMINW - Parallel Minimum Word
// rd = min(rs, rt) for each of 4 packed 32-bit signed words
let DecoderNamespace = "R5900" in
def PMINW : InstSE<(outs GPR128Opnd:$rd), (ins GPR128Opnd:$rs, GPR128Opnd:$rt),
                   "pminw\t$rd, $rs, $rt",
                   [(set v4i32:$rd, (smin v4i32:$rs, v4i32:$rt))],
                   II_AND, FrmR>,
            R5900_MMI1_FM<0b00011>;

// PMINH - Parallel Minimum Halfword
// rd = min(rs, rt) for each of 8 packed 16-bit signed halfwords
let DecoderNamespace = "R5900" in
def PMINH : InstSE<(outs GPR128Opnd:$rd), (ins GPR128Opnd:$rs, GPR128Opnd:$rt),
                   "pminh\t$rd, $rs, $rt",
                   [(set v8i16:$rd, (smin v8i16:$rs, v8i16:$rt))],
                   II_AND, FrmR>,
            R5900_MMI1_FM<0b00111>;

//===----------------------------------------------------------------------===//
// PABS - Parallel Absolute Value
// Both use MMI1 format (func=0x28) with different sa values
//===----------------------------------------------------------------------===//

// PABSW - Parallel Absolute Word
// rd = abs(rt) for each of 4 packed 32-bit signed words
let DecoderNamespace = "R5900" in
def PABSW : InstSE<(outs GPR128Opnd:$rd), (ins GPR128Opnd:$rt),
                   "pabsw\t$rd, $rt",
                   [(set v4i32:$rd, (abs v4i32:$rt))],
                   II_AND, FrmR>,
            R5900_MMI1_2R_FM<0b00001>;

// PABSH - Parallel Absolute Halfword
// rd = abs(rt) for each of 8 packed 16-bit signed halfwords
let DecoderNamespace = "R5900" in
def PABSH : InstSE<(outs GPR128Opnd:$rd), (ins GPR128Opnd:$rt),
                   "pabsh\t$rd, $rt",
                   [(set v8i16:$rd, (abs v8i16:$rt))],
                   II_AND, FrmR>,
            R5900_MMI1_2R_FM<0b00101>;

//===----------------------------------------------------------------------===//
// Parallel Logical Operations
//===----------------------------------------------------------------------===//

// PAND - Parallel AND
// rd = rs & rt (128-bit)
let DecoderNamespace = "R5900" in
def PAND : InstSE<(outs GPR128Opnd:$rd), (ins GPR128Opnd:$rs, GPR128Opnd:$rt),
                  "pand\t$rd, $rs, $rt",
                  [(set v4i32:$rd, (and v4i32:$rs, v4i32:$rt))],
                  II_AND, FrmR>,
           R5900_MMI2_FM<0b10010>;

// POR - Parallel OR
// rd = rs | rt (128-bit)
let DecoderNamespace = "R5900" in
def POR : InstSE<(outs GPR128Opnd:$rd), (ins GPR128Opnd:$rs, GPR128Opnd:$rt),
                 "por\t$rd, $rs, $rt",
                 [(set v4i32:$rd, (or v4i32:$rs, v4i32:$rt))],
                 II_AND, FrmR>,
          R5900_MMI3_FM<0b10010>;

// PXOR - Parallel XOR
// rd = rs ^ rt (128-bit)
let DecoderNamespace = "R5900" in
def PXOR : InstSE<(outs GPR128Opnd:$rd), (ins GPR128Opnd:$rs, GPR128Opnd:$rt),
                  "pxor\t$rd, $rs, $rt",
                  [(set v4i32:$rd, (xor v4i32:$rs, v4i32:$rt))],
                  II_AND, FrmR>,
           R5900_MMI2_FM<0b10011>;

// PNOR - Parallel NOR
// rd = ~(rs | rt) (128-bit)
let DecoderNamespace = "R5900" in
def PNOR : InstSE<(outs GPR128Opnd:$rd), (ins GPR128Opnd:$rs, GPR128Opnd:$rt),
                  "pnor\t$rd, $rs, $rt", [], II_AND, FrmR>,
           R5900_MMI3_FM<0b10011>;

// Patterns for other vector types (v8i16, v16i8) - bitcast through v4i32
def : Pat<(v8i16 (and v8i16:$rs, v8i16:$rt)),
          (PAND (v8i16 $rs), (v8i16 $rt))>;
def : Pat<(v16i8 (and v16i8:$rs, v16i8:$rt)),
          (PAND (v16i8 $rs), (v16i8 $rt))>;
def : Pat<(v8i16 (or v8i16:$rs, v8i16:$rt)),
          (POR (v8i16 $rs), (v8i16 $rt))>;
def : Pat<(v16i8 (or v16i8:$rs, v16i8:$rt)),
          (POR (v16i8 $rs), (v16i8 $rt))>;
def : Pat<(v8i16 (xor v8i16:$rs, v8i16:$rt)),
          (PXOR (v8i16 $rs), (v8i16 $rt))>;
def : Pat<(v16i8 (xor v16i8:$rs, v16i8:$rt)),
          (PXOR (v16i8 $rs), (v16i8 $rt))>;

//===----------------------------------------------------------------------===//
// Parallel Compare Operations
// These return masks: 0xFFFFFFFF (word), 0xFFFF (halfword), 0xFF (byte) for true
//===----------------------------------------------------------------------===//

// PCGTW - Parallel Compare for Greater Than Word
// rd = (rs > rt) ? 0xFFFFFFFF : 0 for each of 4 packed 32-bit signed words
let DecoderNamespace = "R5900" in
def PCGTW : InstSE<(outs GPR128Opnd:$rd), (ins GPR128Opnd:$rs, GPR128Opnd:$rt),
                   "pcgtw\t$rd, $rs, $rt", [], II_AND, FrmR>,
            R5900_MMI0_FM<0b00010>;

// PCGTH - Parallel Compare for Greater Than Halfword
// rd = (rs > rt) ? 0xFFFF : 0 for each of 8 packed 16-bit signed halfwords
let DecoderNamespace = "R5900" in
def PCGTH : InstSE<(outs GPR128Opnd:$rd), (ins GPR128Opnd:$rs, GPR128Opnd:$rt),
                   "pcgth\t$rd, $rs, $rt", [], II_AND, FrmR>,
            R5900_MMI0_FM<0b00110>;

// PCGTB - Parallel Compare for Greater Than Byte
// rd = (rs > rt) ? 0xFF : 0 for each of 16 packed 8-bit signed bytes
let DecoderNamespace = "R5900" in
def PCGTB : InstSE<(outs GPR128Opnd:$rd), (ins GPR128Opnd:$rs, GPR128Opnd:$rt),
                   "pcgtb\t$rd, $rs, $rt", [], II_AND, FrmR>,
            R5900_MMI0_FM<0b01010>;

// PCEQW - Parallel Compare for Equal Word
// rd = (rs == rt) ? 0xFFFFFFFF : 0 for each of 4 packed 32-bit words
let DecoderNamespace = "R5900" in
def PCEQW : InstSE<(outs GPR128Opnd:$rd), (ins GPR128Opnd:$rs, GPR128Opnd:$rt),
                   "pceqw\t$rd, $rs, $rt", [], II_AND, FrmR>,
            R5900_MMI1_FM<0b00010>;

// PCEQH - Parallel Compare for Equal Halfword
// rd = (rs == rt) ? 0xFFFF : 0 for each of 8 packed 16-bit halfwords
let DecoderNamespace = "R5900" in
def PCEQH : InstSE<(outs GPR128Opnd:$rd), (ins GPR128Opnd:$rs, GPR128Opnd:$rt),
                   "pceqh\t$rd, $rs, $rt", [], II_AND, FrmR>,
            R5900_MMI1_FM<0b00110>;

// PCEQB - Parallel Compare for Equal Byte
// rd = (rs == rt) ? 0xFF : 0 for each of 16 packed 8-bit bytes
let DecoderNamespace = "R5900" in
def PCEQB : InstSE<(outs GPR128Opnd:$rd), (ins GPR128Opnd:$rs, GPR128Opnd:$rt),
                   "pceqb\t$rd, $rs, $rt", [], II_AND, FrmR>,
            R5900_MMI1_FM<0b01010>;

//===----------------------------------------------------------------------===//
// Parallel Shift Operations (Immediate)
// These perform SIMD shifts on packed words or halfwords with immediate amount.
//===----------------------------------------------------------------------===//

// PSLLH - Parallel Shift Left Logical Halfword
// rd = rt << sa for each of 8 packed 16-bit halfwords
let DecoderNamespace = "R5900" in
def PSLLH : InstSE<(outs GPR128Opnd:$rd), (ins GPR128Opnd:$rt, uimm4:$sa),
                   "psllh\t$rd, $rt, $sa", [], II_SLL, FrmR>,
            R5900_MMI_SHIFT_FM<0b110100>;

// PSRLH - Parallel Shift Right Logical Halfword
// rd = rt >> sa (logical) for each of 8 packed 16-bit halfwords
let DecoderNamespace = "R5900" in
def PSRLH : InstSE<(outs GPR128Opnd:$rd), (ins GPR128Opnd:$rt, uimm4:$sa),
                   "psrlh\t$rd, $rt, $sa", [], II_SRL, FrmR>,
            R5900_MMI_SHIFT_FM<0b110110>;

// PSRAH - Parallel Shift Right Arithmetic Halfword
// rd = rt >> sa (arithmetic) for each of 8 packed 16-bit halfwords
let DecoderNamespace = "R5900" in
def PSRAH : InstSE<(outs GPR128Opnd:$rd), (ins GPR128Opnd:$rt, uimm4:$sa),
                   "psrah\t$rd, $rt, $sa", [], II_SRA, FrmR>,
            R5900_MMI_SHIFT_FM<0b110111>;

// PSLLW - Parallel Shift Left Logical Word
// rd = rt << sa for each of 4 packed 32-bit words
let DecoderNamespace = "R5900" in
def PSLLW : InstSE<(outs GPR128Opnd:$rd), (ins GPR128Opnd:$rt, uimm5:$sa),
                   "psllw\t$rd, $rt, $sa", [], II_SLL, FrmR>,
            R5900_MMI_SHIFT_FM<0b111100>;

// PSRLW - Parallel Shift Right Logical Word
// rd = rt >> sa (logical) for each of 4 packed 32-bit words
let DecoderNamespace = "R5900" in
def PSRLW : InstSE<(outs GPR128Opnd:$rd), (ins GPR128Opnd:$rt, uimm5:$sa),
                   "psrlw\t$rd, $rt, $sa", [], II_SRL, FrmR>,
            R5900_MMI_SHIFT_FM<0b111110>;

// PSRAW - Parallel Shift Right Arithmetic Word
// rd = rt >> sa (arithmetic) for each of 4 packed 32-bit words
let DecoderNamespace = "R5900" in
def PSRAW : InstSE<(outs GPR128Opnd:$rd), (ins GPR128Opnd:$rt, uimm5:$sa),
                   "psraw\t$rd, $rt, $sa", [], II_SRA, FrmR>,
            R5900_MMI_SHIFT_FM<0b111111>;

//===----------------------------------------------------------------------===//
// Parallel Shift Operations (Variable) - WARNING: 2-word operations only!
// These instructions only operate on 2 of 4 words (elements 0 and 2).
// Elements 1 and 3 are DESTROYED (overwritten with sign-extended results).
// Use PSLLW/PSRLW/PSRAW for 4-word immediate shifts instead.
// These are asm-only - no autovectorization patterns due to partial operation.
//===----------------------------------------------------------------------===//

// PSLLVW - Parallel Shift Left Logical Variable Word
// WARNING: Only shifts 2 words (elements 0 and 2), destroys elements 1 and 3!
// rd[63:0] = sign_extend(rt[31:0] << rs[4:0])
// rd[127:64] = sign_extend(rt[95:64] << rs[68:64])
let DecoderNamespace = "R5900" in
def PSLLVW : InstSE<(outs GPR128Opnd:$rd), (ins GPR128Opnd:$rt, GPR128Opnd:$rs),
                    "psllvw\t$rd, $rt, $rs", [], II_SLL, FrmR>,
             R5900_MMI2_FM<0b00010>;

// PSRLVW - Parallel Shift Right Logical Variable Word
// WARNING: Only shifts 2 words (elements 0 and 2), destroys elements 1 and 3!
// rd[63:0] = sign_extend(rt[31:0] >> rs[4:0])
// rd[127:64] = sign_extend(rt[95:64] >> rs[68:64])
let DecoderNamespace = "R5900" in
def PSRLVW : InstSE<(outs GPR128Opnd:$rd), (ins GPR128Opnd:$rt, GPR128Opnd:$rs),
                    "psrlvw\t$rd, $rt, $rs", [], II_SRL, FrmR>,
             R5900_MMI2_FM<0b00011>;

// PSRAVW - Parallel Shift Right Arithmetic Variable Word
// WARNING: Only shifts 2 words (elements 0 and 2), destroys elements 1 and 3!
// rd[63:0] = sign_extend(rt[31:0] >> rs[4:0]) (arithmetic)
// rd[127:64] = sign_extend(rt[95:64] >> rs[68:64]) (arithmetic)
let DecoderNamespace = "R5900" in
def PSRAVW : InstSE<(outs GPR128Opnd:$rd), (ins GPR128Opnd:$rt, GPR128Opnd:$rs),
                    "psravw\t$rd, $rt, $rs", [], II_SRA, FrmR>,
             R5900_MMI3_FM<0b00011>;

// NOTE: No v4i32 shift patterns - PSLLVW/PSRLVW/PSRAVW only work on 2 words.
// v4i32 shifts are set to Expand in MipsSEISelLowering.cpp (scalarized).

//===----------------------------------------------------------------------===//
// Parallel Saturating Add/Subtract Operations
// These perform SIMD add/subtract with saturation on packed elements.
//===----------------------------------------------------------------------===//

// Signed Saturation - MMI0 class

// PADDSW - Parallel Add with Signed Saturation Word
// rd = saturate_signed(rs + rt) for each of 4 packed 32-bit words
let DecoderNamespace = "R5900" in
def PADDSW : InstSE<(outs GPR128Opnd:$rd), (ins GPR128Opnd:$rs, GPR128Opnd:$rt),
                    "paddsw\t$rd, $rs, $rt",
                    [(set v4i32:$rd, (saddsat v4i32:$rs, v4i32:$rt))],
                    II_ADD, FrmR>,
             R5900_MMI0_FM<0b10000>;

// PSUBSW - Parallel Subtract with Signed Saturation Word
// rd = saturate_signed(rs - rt) for each of 4 packed 32-bit words
let DecoderNamespace = "R5900" in
def PSUBSW : InstSE<(outs GPR128Opnd:$rd), (ins GPR128Opnd:$rs, GPR128Opnd:$rt),
                    "psubsw\t$rd, $rs, $rt",
                    [(set v4i32:$rd, (ssubsat v4i32:$rs, v4i32:$rt))],
                    II_ADD, FrmR>,
             R5900_MMI0_FM<0b10001>;

// PADDSH - Parallel Add with Signed Saturation Halfword
// rd = saturate_signed(rs + rt) for each of 8 packed 16-bit halfwords
let DecoderNamespace = "R5900" in
def PADDSH : InstSE<(outs GPR128Opnd:$rd), (ins GPR128Opnd:$rs, GPR128Opnd:$rt),
                    "paddsh\t$rd, $rs, $rt",
                    [(set v8i16:$rd, (saddsat v8i16:$rs, v8i16:$rt))],
                    II_ADD, FrmR>,
             R5900_MMI0_FM<0b10100>;

// PSUBSH - Parallel Subtract with Signed Saturation Halfword
// rd = saturate_signed(rs - rt) for each of 8 packed 16-bit halfwords
let DecoderNamespace = "R5900" in
def PSUBSH : InstSE<(outs GPR128Opnd:$rd), (ins GPR128Opnd:$rs, GPR128Opnd:$rt),
                    "psubsh\t$rd, $rs, $rt",
                    [(set v8i16:$rd, (ssubsat v8i16:$rs, v8i16:$rt))],
                    II_ADD, FrmR>,
             R5900_MMI0_FM<0b10101>;

// PADDSB - Parallel Add with Signed Saturation Byte
// rd = saturate_signed(rs + rt) for each of 16 packed 8-bit bytes
let DecoderNamespace = "R5900" in
def PADDSB : InstSE<(outs GPR128Opnd:$rd), (ins GPR128Opnd:$rs, GPR128Opnd:$rt),
                    "paddsb\t$rd, $rs, $rt",
                    [(set v16i8:$rd, (saddsat v16i8:$rs, v16i8:$rt))],
                    II_ADD, FrmR>,
             R5900_MMI0_FM<0b11000>;

// PSUBSB - Parallel Subtract with Signed Saturation Byte
// rd = saturate_signed(rs - rt) for each of 16 packed 8-bit bytes
let DecoderNamespace = "R5900" in
def PSUBSB : InstSE<(outs GPR128Opnd:$rd), (ins GPR128Opnd:$rs, GPR128Opnd:$rt),
                    "psubsb\t$rd, $rs, $rt",
                    [(set v16i8:$rd, (ssubsat v16i8:$rs, v16i8:$rt))],
                    II_ADD, FrmR>,
             R5900_MMI0_FM<0b11001>;

// Unsigned Saturation - MMI1 class

// PADDUW - Parallel Add with Unsigned Saturation Word
// rd = saturate_unsigned(rs + rt) for each of 4 packed 32-bit words
let DecoderNamespace = "R5900" in
def PADDUW : InstSE<(outs GPR128Opnd:$rd), (ins GPR128Opnd:$rs, GPR128Opnd:$rt),
                    "padduw\t$rd, $rs, $rt",
                    [(set v4i32:$rd, (uaddsat v4i32:$rs, v4i32:$rt))],
                    II_ADD, FrmR>,
             R5900_MMI1_FM<0b10000>;

// PSUBUW - Parallel Subtract with Unsigned Saturation Word
// rd = saturate_unsigned(rs - rt) for each of 4 packed 32-bit words
let DecoderNamespace = "R5900" in
def PSUBUW : InstSE<(outs GPR128Opnd:$rd), (ins GPR128Opnd:$rs, GPR128Opnd:$rt),
                    "psubuw\t$rd, $rs, $rt",
                    [(set v4i32:$rd, (usubsat v4i32:$rs, v4i32:$rt))],
                    II_ADD, FrmR>,
             R5900_MMI1_FM<0b10001>;

// PADDUH - Parallel Add with Unsigned Saturation Halfword
// rd = saturate_unsigned(rs + rt) for each of 8 packed 16-bit halfwords
let DecoderNamespace = "R5900" in
def PADDUH : InstSE<(outs GPR128Opnd:$rd), (ins GPR128Opnd:$rs, GPR128Opnd:$rt),
                    "padduh\t$rd, $rs, $rt",
                    [(set v8i16:$rd, (uaddsat v8i16:$rs, v8i16:$rt))],
                    II_ADD, FrmR>,
             R5900_MMI1_FM<0b10100>;

// PSUBUH - Parallel Subtract with Unsigned Saturation Halfword
// rd = saturate_unsigned(rs - rt) for each of 8 packed 16-bit halfwords
let DecoderNamespace = "R5900" in
def PSUBUH : InstSE<(outs GPR128Opnd:$rd), (ins GPR128Opnd:$rs, GPR128Opnd:$rt),
                    "psubuh\t$rd, $rs, $rt",
                    [(set v8i16:$rd, (usubsat v8i16:$rs, v8i16:$rt))],
                    II_ADD, FrmR>,
             R5900_MMI1_FM<0b10101>;

// PADDUB - Parallel Add with Unsigned Saturation Byte
// rd = saturate_unsigned(rs + rt) for each of 16 packed 8-bit bytes
let DecoderNamespace = "R5900" in
def PADDUB : InstSE<(outs GPR128Opnd:$rd), (ins GPR128Opnd:$rs, GPR128Opnd:$rt),
                    "paddub\t$rd, $rs, $rt",
                    [(set v16i8:$rd, (uaddsat v16i8:$rs, v16i8:$rt))],
                    II_ADD, FrmR>,
             R5900_MMI1_FM<0b11000>;

// PSUBUB - Parallel Subtract with Unsigned Saturation Byte
// rd = saturate_unsigned(rs - rt) for each of 16 packed 8-bit bytes
let DecoderNamespace = "R5900" in
def PSUBUB : InstSE<(outs GPR128Opnd:$rd), (ins GPR128Opnd:$rs, GPR128Opnd:$rt),
                    "psubub\t$rd, $rs, $rt",
                    [(set v16i8:$rd, (usubsat v16i8:$rs, v16i8:$rt))],
                    II_ADD, FrmR>,
             R5900_MMI1_FM<0b11001>;

//===----------------------------------------------------------------------===//
// R5900 SA Register Instructions
//===----------------------------------------------------------------------===//

// MFSA - Move From Shift Amount register
// rd = SA (used for context save)
let DecoderNamespace = "R5900" in
def MFSA : InstSE<(outs GPR64Opnd:$rd), (ins),
                  "mfsa\t$rd", [], II_MFHI_MFLO, FrmR>,
           R5900_MFSA_FM;

// MTSA - Move To Shift Amount register
// SA = rs (used for context restore)
let DecoderNamespace = "R5900" in
def MTSA : InstSE<(outs), (ins GPR64Opnd:$rs),
                  "mtsa\t$rs", [], II_MTHI_MTLO, FrmR>,
           R5900_MTSA_FM;

// MTSAB - Move Byte count To Shift Amount register
// SA = (rs XOR imm) * 8 (for byte alignment)
let DecoderNamespace = "R5900" in
def MTSAB : InstSE<(outs), (ins GPR64Opnd:$rs, simm16:$imm),
                   "mtsab\t$rs, $imm", [], II_MTHI_MTLO, FrmI>,
            R5900_MTSAB_FM;

// MTSAH - Move Halfword count To Shift Amount register
// SA = (rs XOR imm) * 16 (for halfword alignment)
let DecoderNamespace = "R5900" in
def MTSAH : InstSE<(outs), (ins GPR64Opnd:$rs, simm16:$imm),
                   "mtsah\t$rs, $imm", [], II_MTHI_MTLO, FrmI>,
            R5900_MTSAH_FM;

// QFSRV - Quadword Funnel Shift Right Variable
// rd = (rs || rt) >> SA (concatenate rs and rt, shift right by SA bits)
let DecoderNamespace = "R5900" in
def QFSRV : InstSE<(outs GPR128Opnd:$rd), (ins GPR128Opnd:$rs, GPR128Opnd:$rt),
                   "qfsrv\t$rd, $rs, $rt", [], II_SRL, FrmR>,
            R5900_MMI1_FM<0b11011>;

//===----------------------------------------------------------------------===//
// R5900 Data Rearrangement Instructions
//===----------------------------------------------------------------------===//

// --- Parallel Extend Lower (interleave with zeros) ---
// PEXTLW - Parallel Extend Lower from Word
// Interleaves lower words from rs and rt
let DecoderNamespace = "R5900" in
def PEXTLW : InstSE<(outs GPR128Opnd:$rd), (ins GPR128Opnd:$rs, GPR128Opnd:$rt),
                    "pextlw\t$rd, $rs, $rt", [], II_ADD, FrmR>,
             R5900_MMI0_FM<0b10010>;

// PEXTLH - Parallel Extend Lower from Halfword
let DecoderNamespace = "R5900" in
def PEXTLH : InstSE<(outs GPR128Opnd:$rd), (ins GPR128Opnd:$rs, GPR128Opnd:$rt),
                    "pextlh\t$rd, $rs, $rt", [], II_ADD, FrmR>,
             R5900_MMI0_FM<0b10110>;

// PEXTLB - Parallel Extend Lower from Byte
let DecoderNamespace = "R5900" in
def PEXTLB : InstSE<(outs GPR128Opnd:$rd), (ins GPR128Opnd:$rs, GPR128Opnd:$rt),
                    "pextlb\t$rd, $rs, $rt", [], II_ADD, FrmR>,
             R5900_MMI0_FM<0b11010>;

// --- Parallel Extend Upper ---
// PEXTUW - Parallel Extend Upper from Word
let DecoderNamespace = "R5900" in
def PEXTUW : InstSE<(outs GPR128Opnd:$rd), (ins GPR128Opnd:$rs, GPR128Opnd:$rt),
                    "pextuw\t$rd, $rs, $rt", [], II_ADD, FrmR>,
             R5900_MMI1_FM<0b10010>;

// PEXTUH - Parallel Extend Upper from Halfword
let DecoderNamespace = "R5900" in
def PEXTUH : InstSE<(outs GPR128Opnd:$rd), (ins GPR128Opnd:$rs, GPR128Opnd:$rt),
                    "pextuh\t$rd, $rs, $rt", [], II_ADD, FrmR>,
             R5900_MMI1_FM<0b10110>;

// PEXTUB - Parallel Extend Upper from Byte
let DecoderNamespace = "R5900" in
def PEXTUB : InstSE<(outs GPR128Opnd:$rd), (ins GPR128Opnd:$rs, GPR128Opnd:$rt),
                    "pextub\t$rd, $rs, $rt", [], II_ADD, FrmR>,
             R5900_MMI1_FM<0b11010>;

// --- Parallel Pack ---
// PPACW - Parallel Pack to Word
let DecoderNamespace = "R5900" in
def PPACW : InstSE<(outs GPR128Opnd:$rd), (ins GPR128Opnd:$rs, GPR128Opnd:$rt),
                   "ppacw\t$rd, $rs, $rt", [], II_ADD, FrmR>,
            R5900_MMI0_FM<0b10011>;

// PPACH - Parallel Pack to Halfword
let DecoderNamespace = "R5900" in
def PPACH : InstSE<(outs GPR128Opnd:$rd), (ins GPR128Opnd:$rs, GPR128Opnd:$rt),
                   "ppach\t$rd, $rs, $rt", [], II_ADD, FrmR>,
            R5900_MMI0_FM<0b10111>;

// PPACB - Parallel Pack to Byte
let DecoderNamespace = "R5900" in
def PPACB : InstSE<(outs GPR128Opnd:$rd), (ins GPR128Opnd:$rs, GPR128Opnd:$rt),
                   "ppacb\t$rd, $rs, $rt", [], II_ADD, FrmR>,
            R5900_MMI0_FM<0b11011>;

// --- Parallel Copy ---
// PCPYLD - Parallel Copy Lower Doubleword
let DecoderNamespace = "R5900" in
def PCPYLD : InstSE<(outs GPR128Opnd:$rd), (ins GPR128Opnd:$rs, GPR128Opnd:$rt),
                    "pcpyld\t$rd, $rs, $rt", [], II_ADD, FrmR>,
             R5900_MMI2_FM<0b01110>;

// PCPYUD - Parallel Copy Upper Doubleword
let DecoderNamespace = "R5900" in
def PCPYUD : InstSE<(outs GPR128Opnd:$rd), (ins GPR128Opnd:$rs, GPR128Opnd:$rt),
                    "pcpyud\t$rd, $rs, $rt", [], II_ADD, FrmR>,
             R5900_MMI3_FM<0b01110>;

// PCPYH - Parallel Copy Halfword
let DecoderNamespace = "R5900" in
def PCPYH : InstSE<(outs GPR128Opnd:$rd), (ins GPR128Opnd:$rt),
                   "pcpyh\t$rd, $rt", [], II_ADD, FrmR>,
            R5900_MMI3_2R_FM<0b11011>;

// --- Parallel Interleave ---
// PINTH - Parallel Interleave Halfword
let DecoderNamespace = "R5900" in
def PINTH : InstSE<(outs GPR128Opnd:$rd), (ins GPR128Opnd:$rs, GPR128Opnd:$rt),
                   "pinth\t$rd, $rs, $rt", [], II_ADD, FrmR>,
            R5900_MMI2_FM<0b01010>;

// PINTEH - Parallel Interleave Even Halfword
let DecoderNamespace = "R5900" in
def PINTEH : InstSE<(outs GPR128Opnd:$rd), (ins GPR128Opnd:$rs, GPR128Opnd:$rt),
                    "pinteh\t$rd, $rs, $rt", [], II_ADD, FrmR>,
             R5900_MMI3_FM<0b01010>;

// --- Parallel Exchange ---
// PEXEH - Parallel Exchange Even Halfword
let DecoderNamespace = "R5900" in
def PEXEH : InstSE<(outs GPR128Opnd:$rd), (ins GPR128Opnd:$rt),
                   "pexeh\t$rd, $rt", [], II_ADD, FrmR>,
            R5900_MMI2_2R_FM<0b11010>;

// PEXEW - Parallel Exchange Even Word
let DecoderNamespace = "R5900" in
def PEXEW : InstSE<(outs GPR128Opnd:$rd), (ins GPR128Opnd:$rt),
                   "pexew\t$rd, $rt", [], II_ADD, FrmR>,
            R5900_MMI2_2R_FM<0b11110>;

// PEXCH - Parallel Exchange Center Halfword
let DecoderNamespace = "R5900" in
def PEXCH : InstSE<(outs GPR128Opnd:$rd), (ins GPR128Opnd:$rt),
                   "pexch\t$rd, $rt", [], II_ADD, FrmR>,
            R5900_MMI3_2R_FM<0b11010>;

// PEXCW - Parallel Exchange Center Word
let DecoderNamespace = "R5900" in
def PEXCW : InstSE<(outs GPR128Opnd:$rd), (ins GPR128Opnd:$rt),
                   "pexcw\t$rd, $rt", [], II_ADD, FrmR>,
            R5900_MMI3_2R_FM<0b11110>;

// --- Other Rearrangement ---
// PREVH - Parallel Reverse Halfword
let DecoderNamespace = "R5900" in
def PREVH : InstSE<(outs GPR128Opnd:$rd), (ins GPR128Opnd:$rt),
                   "prevh\t$rd, $rt", [], II_ADD, FrmR>,
            R5900_MMI2_2R_FM<0b11011>;

// PROT3W - Parallel Rotate 3 Words Left
let DecoderNamespace = "R5900" in
def PROT3W : InstSE<(outs GPR128Opnd:$rd), (ins GPR128Opnd:$rt),
                    "prot3w\t$rd, $rt", [], II_ADD, FrmR>,
             R5900_MMI2_2R_FM<0b11111>;

// --- RGB Packing/Unpacking ---
// PEXT5 - Parallel Extend from 5 bits (RGB555 to RGBA32)
let DecoderNamespace = "R5900" in
def PEXT5 : InstSE<(outs GPR128Opnd:$rd), (ins GPR128Opnd:$rt),
                   "pext5\t$rd, $rt", [], II_ADD, FrmR>,
            R5900_MMI0_2R_FM<0b11110>;

// PPAC5 - Parallel Pack to 5 bits (RGBA32 to RGB555)
let DecoderNamespace = "R5900" in
def PPAC5 : InstSE<(outs GPR128Opnd:$rd), (ins GPR128Opnd:$rt),
                   "ppac5\t$rd, $rt", [], II_ADD, FrmR>,
            R5900_MMI0_2R_FM<0b11111>;

//===----------------------------------------------------------------------===//
// R5900 Halfword Multiply Instructions
//===----------------------------------------------------------------------===//
// These instructions produce results in rd, HI, and LO registers.
// Assembly-only for now (no autovectorization patterns).

// PMULTH - Parallel Multiply Halfword
// Multiplies 8 pairs of 16-bit signed integers, stores 32-bit results in rd/HI/LO
let DecoderNamespace = "R5900",
    Defs = [HI0, LO0, HI0_64, LO0_64] in
def PMULTH : InstSE<(outs GPR128Opnd:$rd), (ins GPR128Opnd:$rs, GPR128Opnd:$rt),
                    "pmulth\t$rd, $rs, $rt", [], II_MULT, FrmR>,
             R5900_MMI2_FM<0b11100>;

// PMADDH - Parallel Multiply-Add Halfword
// Multiplies and accumulates 8 pairs of halfwords into HI/LO/rd
let DecoderNamespace = "R5900",
    Uses = [HI0, LO0, HI0_64, LO0_64],
    Defs = [HI0, LO0, HI0_64, LO0_64] in
def PMADDH : InstSE<(outs GPR128Opnd:$rd), (ins GPR128Opnd:$rs, GPR128Opnd:$rt),
                    "pmaddh\t$rd, $rs, $rt", [], II_MADD, FrmR>,
             R5900_MMI2_FM<0b10000>;

// PMSUBH - Parallel Multiply-Subtract Halfword
// Multiplies and subtracts 8 pairs of halfwords from HI/LO
let DecoderNamespace = "R5900",
    Uses = [HI0, LO0, HI0_64, LO0_64],
    Defs = [HI0, LO0, HI0_64, LO0_64] in
def PMSUBH : InstSE<(outs GPR128Opnd:$rd), (ins GPR128Opnd:$rs, GPR128Opnd:$rt),
                    "pmsubh\t$rd, $rs, $rt", [], II_MADD, FrmR>,
             R5900_MMI2_FM<0b10100>;

// PHMADH - Parallel Horizontal Multiply-Add Halfword
// Horizontal multiply-add pairs of halfwords
let DecoderNamespace = "R5900",
    Defs = [HI0, LO0, HI0_64, LO0_64] in
def PHMADH : InstSE<(outs GPR128Opnd:$rd), (ins GPR128Opnd:$rs, GPR128Opnd:$rt),
                    "phmadh\t$rd, $rs, $rt", [], II_MADD, FrmR>,
             R5900_MMI2_FM<0b10001>;

// PHMSBH - Parallel Horizontal Multiply-Subtract Halfword
// Horizontal multiply-subtract pairs of halfwords
let DecoderNamespace = "R5900",
    Defs = [HI0, LO0, HI0_64, LO0_64] in
def PHMSBH : InstSE<(outs GPR128Opnd:$rd), (ins GPR128Opnd:$rs, GPR128Opnd:$rt),
                    "phmsbh\t$rd, $rs, $rt", [], II_MADD, FrmR>,
             R5900_MMI2_FM<0b10101>;

//===----------------------------------------------------------------------===//
// R5900 Parallel Divide Instructions
//===----------------------------------------------------------------------===//
// These instructions write results to HI and LO only (no rd).

// PDIVW - Parallel Divide Word (signed)
// Divides 2 pairs of 32-bit signed integers, results in HI/LO
let DecoderNamespace = "R5900",
    Defs = [HI0, LO0, HI0_64, LO0_64] in
def PDIVW : InstSE<(outs), (ins GPR128Opnd:$rs, GPR128Opnd:$rt),
                   "pdivw\t$rs, $rt", [], II_DIV, FrmR>,
            R5900_MMI2_DIV_FM<0b01101>;

// PDIVUW - Parallel Divide Unsigned Word
// Divides 2 pairs of 32-bit unsigned integers, results in HI/LO
let DecoderNamespace = "R5900",
    Defs = [HI0, LO0, HI0_64, LO0_64] in
def PDIVUW : InstSE<(outs), (ins GPR128Opnd:$rs, GPR128Opnd:$rt),
                    "pdivuw\t$rs, $rt", [], II_DIV, FrmR>,
             R5900_MMI3_DIV_FM<0b01101>;

// PDIVBW - Parallel Divide Broadcast Word
// Divides 4 words by lowest halfword of rt, results in HI/LO
let DecoderNamespace = "R5900",
    Defs = [HI0, LO0, HI0_64, LO0_64] in
def PDIVBW : InstSE<(outs), (ins GPR128Opnd:$rs, GPR128Opnd:$rt),
                    "pdivbw\t$rs, $rt", [], II_DIV, FrmR>,
             R5900_MMI2_DIV_FM<0b11101>;

//===----------------------------------------------------------------------===//
// R5900 Special Parallel Instructions
//===----------------------------------------------------------------------===//

// PADSBH - Parallel Add/Subtract Halfword
// Lower 4 halfwords: subtract, Upper 4 halfwords: add
// No direct LLVM pattern - this is a unique hybrid operation
let DecoderNamespace = "R5900" in
def PADSBH : InstSE<(outs GPR128Opnd:$rd), (ins GPR128Opnd:$rs, GPR128Opnd:$rt),
                    "padsbh\t$rd, $rs, $rt", [], II_ADD, FrmR>,
             R5900_MMI1_FM<0b00100>;

// R5900 GPR128 vector bitcast patterns
// Bitcasts between v4i32, v8i16, v16i8 are free (same register class)
def : Pat<(v4i32 (bitconvert v16i8:$src)), (COPY_TO_REGCLASS v16i8:$src, GPR128)>;
def : Pat<(v4i32 (bitconvert v8i16:$src)), (COPY_TO_REGCLASS v8i16:$src, GPR128)>;
def : Pat<(v8i16 (bitconvert v4i32:$src)), (COPY_TO_REGCLASS v4i32:$src, GPR128)>;
def : Pat<(v8i16 (bitconvert v16i8:$src)), (COPY_TO_REGCLASS v16i8:$src, GPR128)>;
def : Pat<(v16i8 (bitconvert v4i32:$src)), (COPY_TO_REGCLASS v4i32:$src, GPR128)>;
def : Pat<(v16i8 (bitconvert v8i16:$src)), (COPY_TO_REGCLASS v8i16:$src, GPR128)>;

//===----------------------------------------------------------------------===//
// R5900 Scalar MMI Operations
// These use MMI instructions for scalar operations on i32/i64 values.
// On R5900, GPR128/GPR64/GPR32 are the same physical registers, so these
// instructions operate on the lower bits and ignore/produce garbage in upper bits.
//===----------------------------------------------------------------------===//

// Scalar versions of MMI instructions - same encoding, different register class
// These are codegen-only; the assembler/disassembler uses the vector versions.
// We define GPR32 versions since MIPS uses GPR32 for i32 operations.
let isCodeGenOnly = 1, hasSideEffects = 0 in {
  // PABSW scalar version - absolute value i32
  def PABSW_S32 : InstSE<(outs GPR32Opnd:$rd), (ins GPR32Opnd:$rt),
                         "pabsw\t$rd, $rt", [], II_AND, FrmR>,
                  R5900_MMI1_2R_FM<0b00001>;

  // PMAXW/PMINW scalar versions - signed max/min i32
  let isCommutable = 1 in {
    def PMAXW_S32 : InstSE<(outs GPR32Opnd:$rd), (ins GPR32Opnd:$rs, GPR32Opnd:$rt),
                           "pmaxw\t$rd, $rs, $rt", [], II_AND, FrmR>,
                    R5900_MMI0_FM<0b00011>;

    def PMINW_S32 : InstSE<(outs GPR32Opnd:$rd), (ins GPR32Opnd:$rs, GPR32Opnd:$rt),
                           "pminw\t$rd, $rs, $rt", [], II_AND, FrmR>,
                    R5900_MMI1_FM<0b00011>;
  }
}

// Scalar MMI patterns - use separate Pat definitions to avoid pattern issues
def : Pat<(i32 (abs GPR32:$rt)), (PABSW_S32 GPR32:$rt)>;
def : Pat<(i32 (smax GPR32:$rs, GPR32:$rt)), (PMAXW_S32 GPR32:$rs, GPR32:$rt)>;
def : Pat<(i32 (smin GPR32:$rs, GPR32:$rt)), (PMINW_S32 GPR32:$rs, GPR32:$rt)>;

// Scalar saturating arithmetic instructions
let isCodeGenOnly = 1, hasSideEffects = 0, isCommutable = 1 in {
  def PADDSW_S32 : InstSE<(outs GPR32Opnd:$rd), (ins GPR32Opnd:$rs, GPR32Opnd:$rt),
                          "paddsw\t$rd, $rs, $rt", [], II_ADD, FrmR>,
                   R5900_MMI0_FM<0b10000>;

  def PADDUW_S32 : InstSE<(outs GPR32Opnd:$rd), (ins GPR32Opnd:$rs, GPR32Opnd:$rt),
                          "padduw\t$rd, $rs, $rt", [], II_ADD, FrmR>,
                   R5900_MMI1_FM<0b10000>;
}

let isCodeGenOnly = 1, hasSideEffects = 0 in {
  def PSUBSW_S32 : InstSE<(outs GPR32Opnd:$rd), (ins GPR32Opnd:$rs, GPR32Opnd:$rt),
                          "psubsw\t$rd, $rs, $rt", [], II_ADD, FrmR>,
                   R5900_MMI0_FM<0b10001>;

  def PSUBUW_S32 : InstSE<(outs GPR32Opnd:$rd), (ins GPR32Opnd:$rs, GPR32Opnd:$rt),
                          "psubuw\t$rd, $rs, $rt", [], II_ADD, FrmR>,
                   R5900_MMI1_FM<0b10001>;
}

// Scalar saturating arithmetic patterns
def : Pat<(i32 (saddsat GPR32:$rs, GPR32:$rt)), (PADDSW_S32 GPR32:$rs, GPR32:$rt)>;
def : Pat<(i32 (ssubsat GPR32:$rs, GPR32:$rt)), (PSUBSW_S32 GPR32:$rs, GPR32:$rt)>;
def : Pat<(i32 (uaddsat GPR32:$rs, GPR32:$rt)), (PADDUW_S32 GPR32:$rs, GPR32:$rt)>;
def : Pat<(i32 (usubsat GPR32:$rs, GPR32:$rt)), (PSUBUW_S32 GPR32:$rs, GPR32:$rt)>;

// PLZCW scalar version for CTLZ lowering
// PLZCW counts leading bits matching MSB (sign bit), returns (count - 1)
// CTLZ is lowered to: (src < 0) ? 0 : (PLZCW(src) + 1)
let isCodeGenOnly = 1, hasSideEffects = 0 in {
  def PLZCW_S32 : InstSE<(outs GPR32Opnd:$rd), (ins GPR32Opnd:$rs),
                         "plzcw\t$rd, $rs", [], II_CLZ, FrmR>,
                  R5900_MMI_PLZCW_FM;
}

// Pattern to match MipsISD::PLZCW to PLZCW_S32 instruction
def : Pat<(i32 (MipsPLZCW GPR32:$rs)), (PLZCW_S32 GPR32:$rs)>;

} // Predicates = [HasR5900]

//===----------------------------------------------------------------------===//
// R5900 128-bit GPR Load/Store Instructions
//===----------------------------------------------------------------------===//
// LQ/SQ load/store 128-bit values to/from the extended 128-bit GPRs.
// These are R5900-specific instructions (not MIPS III/IV).

class LQ_GPR<string opstr, RegisterOperand RC, InstrItinClass Itin,
             SDPatternOperator OpNode = null_frag> :
  InstSE<(outs RC:$rt), (ins mem_simm16:$addr),
         !strconcat(opstr, "\t$rt, $addr"),
         [(set RC:$rt, (OpNode addrDefault:$addr))], Itin, FrmI, opstr> {
  let DecoderMethod = "DecodeGPR128Mem";
  let DecoderNamespace = "R5900";
  let mayLoad = 1;
}

class SQ_GPR<string opstr, RegisterOperand RC, InstrItinClass Itin,
             SDPatternOperator OpNode = null_frag> :
  InstSE<(outs), (ins RC:$rt, mem_simm16:$addr),
         !strconcat(opstr, "\t$rt, $addr"),
         [(OpNode RC:$rt, addrDefault:$addr)], Itin, FrmI, opstr> {
  let DecoderMethod = "DecodeGPR128Mem";
  let DecoderNamespace = "R5900";
  let mayStore = 1;
}

// Aligned load/store pattern fragments for R5900 LQ/SQ (require 16-byte alignment)
// These are required because LQ/SQ cause TLB misses on unaligned accesses
def aligned_load_128 : PatFrag<(ops node:$ptr), (load node:$ptr)> {
  let IsLoad = true;
  let IsNonExtLoad = true;
  let MinAlignment = 16;
}

def aligned_store_128 : PatFrag<(ops node:$val, node:$ptr), (store node:$val, node:$ptr)> {
  let IsStore = true;
  let IsTruncStore = false;
  let MinAlignment = 16;
}

let Predicates = [HasR5900] in {
  // LQ - Load Quadword (128-bit memory -> GPR128)
  // Opcode: 0x1e (bits 31-26 = 011110)
  // Note: Requires 16-byte alignment to avoid TLB misses
  def LQ : LQ_GPR<"lq", GPR128Opnd, II_LQ>, LW_FM<0x1e>;

  // SQ - Store Quadword (GPR128 -> 128-bit memory)
  // Opcode: 0x1f (bits 31-26 = 011111)
  // Note: Requires 16-byte alignment to avoid TLB misses
  def SQ : SQ_GPR<"sq", GPR128Opnd, II_SQ>, LW_FM<0x1f>;

  // LQ patterns for each vector type (only match 16-byte aligned loads)
  def : Pat<(v4i32 (aligned_load_128 addrDefault:$addr)), (LQ addrDefault:$addr)>;
  def : Pat<(v8i16 (aligned_load_128 addrDefault:$addr)), (LQ addrDefault:$addr)>;
  def : Pat<(v16i8 (aligned_load_128 addrDefault:$addr)), (LQ addrDefault:$addr)>;

  // SQ patterns for each vector type (only match 16-byte aligned stores)
  def : Pat<(aligned_store_128 v4i32:$rt, addrDefault:$addr), (SQ $rt, addrDefault:$addr)>;
  def : Pat<(aligned_store_128 v8i16:$rt, addrDefault:$addr), (SQ $rt, addrDefault:$addr)>;
  def : Pat<(aligned_store_128 v16i8:$rt, addrDefault:$addr), (SQ $rt, addrDefault:$addr)>;
} // Predicates = [HasR5900]

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

} // Predicates = [HasR5900]

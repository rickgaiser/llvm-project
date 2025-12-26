//==- MipsScheduleR5900.td - R5900 Scheduling Definitions --*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file describes the scheduling model for the R5900 (PS2 Emotion Engine)
// processor. The R5900 is a MIPS III processor with a 6-stage pipeline and
// 2-way superscalar execution. It has dual integer pipelines (I0, I1), a
// single load/store unit, and a single-precision FPU.
//
//===----------------------------------------------------------------------===//

def MipsR5900Model : SchedMachineModel {
  int IssueWidth = 2;        // 2-way superscalar
  int MicroOpBufferSize = 0; // In-order processor
  int LoadLatency = 1;       // Integer loads: 1 cycle
  int MispredictPenalty = 4; // 6-stage pipeline, estimated penalty
  int HighLatency = 37;      // DIV is 37 cycles

  // CompleteModel = 0 while developing the model. Set to 1 when complete.
  let CompleteModel = 0;
  let PostRAScheduler = 1;
  let FullInstRWOverlapCheck = 1;

  // R5900 is MIPS III based, exclude features it doesn't support
  list<Predicate> UnsupportedFeatures = [HasMips32, HasMips32r2, HasMips32r5,
                                         HasMips32r6,
                                         HasMips64, HasMips64r2, HasMips64r5,
                                         HasMips64r6,
                                         InMicroMips, InMips16Mode,
                                         HasCnMips, HasCnMipsP,
                                         HasDSP, HasDSPR2, HasMips3D, HasMT,
                                         HasCRC, HasMSA, HasVirt, HasGINV,
                                         HasEVA];
}

let SchedModel = MipsR5900Model in {

//===----------------------------------------------------------------------===//
// Dual Integer Pipelines (I0, I1)
//===----------------------------------------------------------------------===//
// The R5900 has two integer pipelines, each with a 64-bit ALU.
// Most integer instructions can execute on either pipeline.

def R5900IntPipe : ProcResource<2> { let BufferSize = 0; }
def R5900IssueALU : ProcResource<1> { let Super = R5900IntPipe; }

// Basic ALU operations: 1 cycle latency
def R5900WriteALU : SchedWriteRes<[R5900IssueALU]>;

// add, addi, addiu, addu, and, andi, lui, nor, or, ori,
// sll, sllv, slt, slti, sltiu, sltu, sra, srav, srl, srlv,
// sub, subu, xor, xori, nop, ssnop
def : InstRW<[R5900WriteALU], (instrs ADD, ADDi, ADDiu, ADDu, AND, ANDi,
                               LEA_ADDiu, LUi, NOP, NOR, OR, ORi, SLL, SLLV,
                               SLT, SLTi, SLTiu, SLTu, SRA, SRAV, SRL, SRLV,
                               SSNOP, SUB, SUBu, XOR, XORi, COPY)>;

//===----------------------------------------------------------------------===//
// MIPS III 64-bit Instructions (R5900 supports these)
//===----------------------------------------------------------------------===//

def : InstRW<[R5900WriteALU], (instrs DADD, DADDi, DADDiu, DADDu, DSLL, DSLL32,
                               DSLLV, DSRA, DSRA32, DSRAV, DSRL, DSRL32, DSRLV,
                               DSUB, DSUBu, AND64, ANDi64, OR64, ORi64, NOR64,
                               XOR64, XORi64, SLL64_32, SLL64_64, SLT64, SLTi64,
                               SLTiu64, SLTu64, LUi64, LEA_ADDiu64)>;

//===----------------------------------------------------------------------===//
// Multiply/Divide Unit (MDU)
//===----------------------------------------------------------------------===//
// The R5900 has a multiply-accumulate unit in each integer pipeline.
// For simplicity, we model them as a single shared resource.

def R5900MDU : ProcResource<1> { let BufferSize = 0; }

// Multiply: 4 cycles latency
def R5900WriteMul : SchedWriteRes<[R5900IssueALU, R5900MDU]> {
  let Latency = 4;
  let ReleaseAtCycles = [1, 4];
}

// Divide: 37 cycles latency
def R5900WriteDiv : SchedWriteRes<[R5900IssueALU, R5900MDU]> {
  let Latency = 37;
  let ReleaseAtCycles = [1, 37];
}

// MFHI/MFLO: result available in 1 cycle
def R5900WriteHILO : SchedWriteRes<[R5900IssueALU]>;

// MTHI/MTLO: 1 cycle
def R5900WriteMTHILO : SchedWriteRes<[R5900IssueALU]>;

// mult, multu
def : InstRW<[R5900WriteMul], (instrs MULT, MULTu, PseudoMULT, PseudoMULTu)>;

// div, divu
def : InstRW<[R5900WriteDiv], (instrs PseudoSDIV, SDIV, PseudoUDIV, UDIV)>;

// mfhi, mflo
def : InstRW<[R5900WriteHILO], (instrs MFHI, MFLO, PseudoMFHI, PseudoMFLO,
                                MFHI64, MFLO64, PseudoMFHI64, PseudoMFLO64)>;

// mthi, mtlo
def : InstRW<[R5900WriteMTHILO], (instrs MTHI, MTLO, PseudoMTLOHI,
                                  MTHI64, MTLO64, PseudoMTLOHI64)>;

// MIPS III 64-bit multiply/divide
def : InstRW<[R5900WriteMul], (instrs DMULT, DMULTu, PseudoDMULT, PseudoDMULTu)>;
def : InstRW<[R5900WriteDiv], (instrs DSDIV, DUDIV, PseudoDSDIV, PseudoDUDIV)>;

//===----------------------------------------------------------------------===//
// Load/Store Pipeline
//===----------------------------------------------------------------------===//

def R5900LDST : ProcResource<1> { let BufferSize = 0; }
def R5900IssueLDST : ProcResource<1> { let Super = R5900LDST; }

// Integer loads: 1 cycle latency
def R5900WriteLoad : SchedWriteRes<[R5900IssueLDST]>;

// Integer stores: 1 cycle
def R5900WriteStore : SchedWriteRes<[R5900IssueLDST]>;

// lb, lbu, lh, lhu, lw
def : InstRW<[R5900WriteLoad], (instrs LB, LBu, LH, LHu, LW, LWL, LWR)>;

// MIPS III 64-bit loads
def : InstRW<[R5900WriteLoad], (instrs LD, LDL, LDR, LWu)>;

// sb, sh, sw, sw[lr]
def : InstRW<[R5900WriteStore], (instrs SB, SH, SW, SWL, SWR)>;

// MIPS III 64-bit stores
def : InstRW<[R5900WriteStore], (instrs SD, SDL, SDR)>;

// sync
def R5900WriteSync : SchedWriteRes<[R5900IssueLDST]>;
def : InstRW<[R5900WriteSync], (instrs SYNC)>;

//===----------------------------------------------------------------------===//
// Branch Pipeline
//===----------------------------------------------------------------------===//

def R5900Branch : ProcResource<1> { let BufferSize = 0; }
def R5900IssueBranch : ProcResource<1> { let Super = R5900Branch; }

// Branches and jumps: 1 cycle
def R5900WriteBranch : SchedWriteRes<[R5900IssueBranch]>;

// Jump-and-link: 2 cycles (return address available)
def R5900WriteJAL : SchedWriteRes<[R5900IssueBranch]> {
  let Latency = 2;
}

// Trap instructions
def R5900WriteTrap : SchedWriteRes<[R5900IssueBranch]>;

// b, beq, beql, bgez, bgezl, bgtz, bgtzl, blez, blezl, bltz, bltzl, bne, bnel
// j, jr, jr.hb
def : InstRW<[R5900WriteBranch], (instrs B, BEQ, BEQL, BGEZ, BGEZL, BGTZ, BGTZL,
                                  BLEZ, BLEZL, BLTZ, BLTZL, BNE, BNEL, J, JR,
                                  JR_HB, PseudoIndirectBranch,
                                  PseudoIndirectHazardBranch, PseudoReturn,
                                  RetRA, TAILCALL, TAILCALLREG,
                                  TAILCALLREGHB)>;

// MIPS III 64-bit branches
def : InstRW<[R5900WriteBranch], (instrs BEQ64, BGEZ64, BGTZ64, BLEZ64, BLTZ64,
                                  BNE64, JR64, JR_HB64, TAILCALLREG64,
                                  TAILCALLREGHB64, PseudoReturn64)>;

// bal, bgezal, bgezall, bltzal, bltzall, jal, jalr, jalr.hb
def : InstRW<[R5900WriteJAL], (instrs BAL, BAL_BR, BGEZAL, BGEZALL, BLTZAL,
                               BLTZALL, JAL, JALR, JALR_HB, JALRPseudo,
                               JALRHBPseudo)>;

// MIPS III 64-bit JAL
def : InstRW<[R5900WriteJAL], (instrs JALR64, JALR64Pseudo, JALRHB64Pseudo,
                               JALR_HB64)>;

// break, syscall, teq, teqi, tge, tgei, tgeiu, tgeu, tlt, tlti, tltiu, tltu,
// tne, tnei, trap
def : InstRW<[R5900WriteTrap], (instrs BREAK, SYSCALL, TEQ, TEQI, TGE, TGEI,
                                TGEIU, TGEU, TNE, TNEI, TLT, TLTI, TLTU,
                                TTLTIU, TRAP, SDBBP)>;

//===----------------------------------------------------------------------===//
// Floating Point Unit (FPU) - Single Precision Only
//===----------------------------------------------------------------------===//
// The R5900 FPU has an 8-stage pipeline and only supports single-precision.
// Double-precision instructions are NOT supported.

def R5900FPU : ProcResource<1> { let BufferSize = 0; }
def R5900IssueFPU : ProcResource<1> { let Super = R5900FPU; }

// FPU Div/Sqrt unit (serialized)
def R5900FPUDivSqrt : ProcResource<1>;

// FPU Arithmetic (add, sub, mul): 4 cycles
def R5900WriteFPUArith : SchedWriteRes<[R5900IssueFPU]> {
  let Latency = 4;
}

// FPU Division: 8 cycles
def R5900WriteFPUDiv : SchedWriteRes<[R5900IssueFPU, R5900FPUDivSqrt]> {
  let Latency = 8;
  let ReleaseAtCycles = [1, 8];
}

// FPU Square Root: 8 cycles
def R5900WriteFPUSqrt : SchedWriteRes<[R5900IssueFPU, R5900FPUDivSqrt]> {
  let Latency = 8;
  let ReleaseAtCycles = [1, 8];
}

// FPU Move/Abs/Neg: 1 cycle
def R5900WriteFPUMove : SchedWriteRes<[R5900IssueFPU]>;

// FPU Compare: 2 cycles
def R5900WriteFPUCmp : SchedWriteRes<[R5900IssueFPU]> {
  let Latency = 2;
}

// MFC1/MTC1: 2 cycles
def R5900WriteFPUTransfer : SchedWriteRes<[R5900IssueFPU]> {
  let Latency = 2;
}

// FPU Load: 2 cycles
def R5900WriteFPULoad : SchedWriteRes<[R5900IssueLDST]> {
  let Latency = 2;
}

// FPU Store: 1 cycle
def R5900WriteFPUStore : SchedWriteRes<[R5900IssueLDST]>;

// add.s, sub.s
def : InstRW<[R5900WriteFPUArith], (instrs FADD_S, FSUB_S)>;

// mul.s
def : InstRW<[R5900WriteFPUArith], (instrs FMUL_S)>;

// div.s
def : InstRW<[R5900WriteFPUDiv], (instrs FDIV_S)>;

// sqrt.s
def : InstRW<[R5900WriteFPUSqrt], (instrs FSQRT_S)>;

// abs.s, neg.s, mov.s
def : InstRW<[R5900WriteFPUMove], (instrs FABS_S, FNEG_S, FMOV_S)>;

// c.<cc>.s (floating point compare)
def : InstRW<[R5900WriteFPUCmp], (instrs FCMP_S32, BC1F, BC1T, BC1FL, BC1TL)>;
def : InstRW<[R5900WriteFPUCmp], (instregex "C_[A-Z]+_S$")>;

// cvt.s.w, cvt.w.s
def : InstRW<[R5900WriteFPUArith], (instrs CVT_S_W, CVT_W_S)>;

// ceil, floor, round, trunc
def : InstRW<[R5900WriteFPUArith], (instrs CEIL_W_S, FLOOR_W_S, ROUND_W_S,
                                    TRUNC_W_S, PseudoTRUNC_W_S)>;

// mfc1, mtc1, cfc1, ctc1
def : InstRW<[R5900WriteFPUTransfer], (instrs MFC1, MTC1, CFC1, CTC1)>;

// lwc1 (FPU load)
def : InstRW<[R5900WriteFPULoad], (instrs LWC1)>;

// swc1 (FPU store)
def : InstRW<[R5900WriteFPUStore], (instrs SWC1)>;

// movn.s, movz.s (conditional moves)
def : InstRW<[R5900WriteFPUMove], (instrs MOVN_I_S, MOVZ_I_S, MOVF_S, MOVT_S,
                                   MOVF_I, MOVT_I)>;

//===----------------------------------------------------------------------===//
// R5900 FPU Accumulator Instructions
//===----------------------------------------------------------------------===//
// The R5900 has a dedicated FPU accumulator register (ACC) for multiply-add
// chains. These instructions have the same 4-cycle latency as regular FPU
// arithmetic operations.

// FPU Accumulator operations: 4 cycles (same as mul.s)
def R5900WriteFPUAcc : SchedWriteRes<[R5900IssueFPU]> {
  let Latency = 4;
}

// adda.s, suba.s, mula.s (write ACC only)
// madda.s, msuba.s (read and write ACC)
// madd.s, msub.s (read ACC, write FPR)
def : InstRW<[R5900WriteFPUAcc], (instrs ADDA_S, SUBA_S, MULA_S,
                                         MADDA_S, MSUBA_S,
                                         R5900_MADD_S, R5900_MSUB_S)>;

//===----------------------------------------------------------------------===//
// COP0 Instructions
//===----------------------------------------------------------------------===//

def R5900COP0 : ProcResource<1> { let BufferSize = 0; }
def R5900IssueCOP0 : ProcResource<1> { let Super = R5900COP0; }

def R5900WriteCOP0 : SchedWriteRes<[R5900IssueCOP0]> {
  let Latency = 3;
}

def R5900WriteTLB : SchedWriteRes<[R5900IssueCOP0]> {
  let Latency = 4;
}

def : InstRW<[R5900WriteCOP0], (instrs MFC0, MTC0, DMFC0, DMTC0)>;
def : InstRW<[R5900WriteTLB], (instrs TLBP, TLBR, TLBWI, TLBWR)>;
def : InstRW<[R5900WriteCOP0], (instrs DERET, ERET, WAIT)>;

} // SchedModel = MipsR5900Model

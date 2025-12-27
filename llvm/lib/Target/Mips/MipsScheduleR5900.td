//==- MipsScheduleR5900.td - R5900 Scheduling Definitions --*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file describes the scheduling model for the R5900 (PS2 Emotion Engine)
// processor. The R5900 is a MIPS III-based 2-way superscalar processor with:
//   - 6-stage integer pipeline
//   - 8-stage FPU pipeline
//   - 6 physical pipelines: I0, I1, LS, BR, C1 (FPU), C2 (VU0)
//
// Dual-Issue Rules:
//   Pipe0: SA ops, COP1 Operate, COP2 Operate, ALU, MAC0, Branch
//   Pipe1: Load/Store, SYNC, LZC, ERET, COP1 Move, COP2 Move, ALU, MAC1, BR, COP0
//
//===----------------------------------------------------------------------===//

def MipsR5900Model : SchedMachineModel {
  int IssueWidth = 2;        // 2-way superscalar
  int MicroOpBufferSize = 0; // In-order processor
  int LoadLatency = 1;       // Integer loads: 1 cycle
  int MispredictPenalty = 4; // 6-stage pipeline
  int HighLatency = 37;      // DIV is 37 cycles

  // Not all MIPS instructions are modeled; R5900 only supports a subset
  // (no double-precision FPU, no LL/SC, no atomics). Unsupported instructions
  // use default timing.
  let CompleteModel = 0;
  let PostRAScheduler = 1;
  let FullInstRWOverlapCheck = 1;

  // R5900 is MIPS III based, exclude unsupported features
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
// Physical Pipeline Resources
//===----------------------------------------------------------------------===//
// The R5900 has 6 physical pipelines that execute different instruction types.

// Integer Pipeline 0 - ALU, Shift, SA ops, MAC0 (mult/div to HI0/LO0)
def R5900PipeI0 : ProcResource<1> { let BufferSize = 0; }

// Integer Pipeline 1 - ALU, SYNC, LZC, ERET, MAC1 (mult1/div1 to HI1/LO1)
def R5900PipeI1 : ProcResource<1> { let BufferSize = 0; }

// Load/Store Pipeline - Load, Store, COP0, COP1 Move, COP2 Move
def R5900PipeLS : ProcResource<1> { let BufferSize = 0; }

// Branch Pipeline - Branch, Jump
def R5900PipeBR : ProcResource<1> { let BufferSize = 0; }

// COP1 Pipeline - FPU operations (single precision only)
def R5900PipeC1 : ProcResource<1> { let BufferSize = 0; }

// COP2 Pipeline - VU0 macro operations (not modeled in detail yet)
def R5900PipeC2 : ProcResource<1> { let BufferSize = 0; }

//===----------------------------------------------------------------------===//
// Logical Issue Slots
//===----------------------------------------------------------------------===//
// The R5900 can issue up to 2 instructions per cycle. Each instruction is
// assigned to either Pipe0 or Pipe1 based on its type.

// Issue slot 0: SA ops, COP1 Operate, COP2 Operate, ALU, MAC0, Branch
def R5900Slot0 : ProcResource<1> { let BufferSize = 0; }

// Issue slot 1: Load/Store, SYNC, LZC, ERET, COP1 Move, COP2 Move, ALU, MAC1, BR, COP0
def R5900Slot1 : ProcResource<1> { let BufferSize = 0; }

//===----------------------------------------------------------------------===//
// Multiply/Divide Units
//===----------------------------------------------------------------------===//
// R5900 has two MAC units - MAC0 in I0 and MAC1 in I1

def R5900MAC0 : ProcResource<1> { let BufferSize = 0; }  // HI0/LO0
def R5900MAC1 : ProcResource<1> { let BufferSize = 0; }  // HI1/LO1

//===----------------------------------------------------------------------===//
// FPU Division/Square Root Unit
//===----------------------------------------------------------------------===//

def R5900FPUDivSqrt : ProcResource<1> { let BufferSize = 0; }

//===----------------------------------------------------------------------===//
// Integer ALU Instructions
//===----------------------------------------------------------------------===//
// ALU ops can execute on either I0 or I1, and issue from either slot.
// We model them as using just one slot (flexible scheduling).

def R5900WriteALU : SchedWriteRes<[R5900Slot0]> {
  let Latency = 1;
}

// ALU ops that prefer slot 1 (can dual-issue with slot 0 ops)
def R5900WriteALU1 : SchedWriteRes<[R5900Slot1]> {
  let Latency = 1;
}

// add, addi, addiu, addu, and, andi, lui, nor, or, ori,
// sll, sllv, slt, slti, sltiu, sltu, sra, srav, srl, srlv,
// sub, subu, xor, xori, nop, ssnop
def : InstRW<[R5900WriteALU], (instrs ADD, ADDi, ADDiu, ADDu, AND, ANDi,
                               LEA_ADDiu, LUi, NOP, NOR, OR, ORi, SLL, SLLV,
                               SLT, SLTi, SLTiu, SLTu, SRA, SRAV, SRL, SRLV,
                               SSNOP, SUB, SUBu, XOR, XORi, COPY)>;

// MIPS III 64-bit ALU instructions
def : InstRW<[R5900WriteALU], (instrs DADD, DADDi, DADDiu, DADDu, DSLL, DSLL32,
                               DSLLV, DSRA, DSRA32, DSRAV, DSRL, DSRL32, DSRLV,
                               DSUB, DSUBu, AND64, ANDi64, OR64, ORi64, NOR64,
                               XOR64, XORi64, SLL64_32, SLL64_64, SLT64, SLTi64,
                               SLTiu64, SLTu64, LUi64, LEA_ADDiu64)>;

//===----------------------------------------------------------------------===//
// Multiply/Divide Instructions (MAC0 - in I0 pipeline)
//===----------------------------------------------------------------------===//
// MULT/DIV use I0 pipeline with MAC0 unit

def R5900WriteMul : SchedWriteRes<[R5900Slot0, R5900PipeI0, R5900MAC0]> {
  let Latency = 4;
  let ReleaseAtCycles = [1, 1, 4];
}

def R5900WriteDiv : SchedWriteRes<[R5900Slot0, R5900PipeI0, R5900MAC0]> {
  let Latency = 37;
  let ReleaseAtCycles = [1, 1, 37];
}

def R5900WriteHILO : SchedWriteRes<[R5900Slot0]> {
  let Latency = 1;
}

def R5900WriteMTHILO : SchedWriteRes<[R5900Slot0]> {
  let Latency = 1;
}

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
// Load/Store Pipeline (LS)
//===----------------------------------------------------------------------===//
// Load/Store instructions use the LS pipeline and issue from Slot1

def R5900WriteLoad : SchedWriteRes<[R5900Slot1, R5900PipeLS]> {
  let Latency = 1;
}

def R5900WriteStore : SchedWriteRes<[R5900Slot1, R5900PipeLS]> {
  let Latency = 1;
}

// lb, lbu, lh, lhu, lw
def : InstRW<[R5900WriteLoad], (instrs LB, LBu, LH, LHu, LW, LWL, LWR)>;

// MIPS III 64-bit loads
def : InstRW<[R5900WriteLoad], (instrs LD, LDL, LDR, LWu)>;

// sb, sh, sw
def : InstRW<[R5900WriteStore], (instrs SB, SH, SW, SWL, SWR)>;

// MIPS III 64-bit stores
def : InstRW<[R5900WriteStore], (instrs SD, SDL, SDR)>;

// SYNC - uses I1 pipeline (Slot1 only)
def R5900WriteSync : SchedWriteRes<[R5900Slot1, R5900PipeI1]> {
  let Latency = 1;
}
def : InstRW<[R5900WriteSync], (instrs SYNC)>;

//===----------------------------------------------------------------------===//
// Branch Pipeline (BR)
//===----------------------------------------------------------------------===//
// Branch instructions use the BR pipeline. Can issue from either slot.

def R5900WriteBranch : SchedWriteRes<[R5900PipeBR]> {
  let Latency = 1;
}

def R5900WriteJAL : SchedWriteRes<[R5900PipeBR]> {
  let Latency = 2;
}

def R5900WriteTrap : SchedWriteRes<[R5900PipeBR]> {
  let Latency = 1;
}

// b, beq, beql, bgez, bgezl, bgtz, bgtzl, blez, blezl, bltz, bltzl, bne, bnel
// j, jr
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

// bal, bgezal, bltzal, jal, jalr
def : InstRW<[R5900WriteJAL], (instrs BAL, BAL_BR, BGEZAL, BGEZALL, BLTZAL,
                               BLTZALL, JAL, JALR, JALR_HB, JALRPseudo,
                               JALRHBPseudo)>;

// MIPS III 64-bit JAL
def : InstRW<[R5900WriteJAL], (instrs JALR64, JALR64Pseudo, JALRHB64Pseudo,
                               JALR_HB64)>;

// Trap instructions
def : InstRW<[R5900WriteTrap], (instrs BREAK, SYSCALL, TEQ, TEQI, TGE, TGEI,
                                TGEIU, TGEU, TNE, TNEI, TLT, TLTI, TLTU,
                                TTLTIU, TRAP, SDBBP)>;

//===----------------------------------------------------------------------===//
// COP1 Pipeline (FPU - Single Precision Only)
//===----------------------------------------------------------------------===//
// FPU operations execute on the C1 pipeline and issue from Slot0.
// The R5900 FPU has an 8-stage pipeline.

// FPU Arithmetic (add.s, sub.s, mul.s): 4 cycles
def R5900WriteFPUArith : SchedWriteRes<[R5900Slot0, R5900PipeC1]> {
  let Latency = 4;
}

// FPU Division: 8 cycles, blocks div/sqrt unit
def R5900WriteFPUDiv : SchedWriteRes<[R5900Slot0, R5900PipeC1, R5900FPUDivSqrt]> {
  let Latency = 8;
  let ReleaseAtCycles = [1, 1, 8];
}

// FPU Square Root: 8 cycles, blocks div/sqrt unit
def R5900WriteFPUSqrt : SchedWriteRes<[R5900Slot0, R5900PipeC1, R5900FPUDivSqrt]> {
  let Latency = 8;
  let ReleaseAtCycles = [1, 1, 8];
}

// FPU Move/Abs/Neg: 4 cycles (per EE docs, FPU move is 4 cycles)
def R5900WriteFPUMove : SchedWriteRes<[R5900Slot0, R5900PipeC1]> {
  let Latency = 4;
}

// FPU Compare: 2 cycles
def R5900WriteFPUCmp : SchedWriteRes<[R5900Slot0, R5900PipeC1]> {
  let Latency = 2;
}

// FPU Load (LWC1): Uses LS pipe only, issues from Slot1
// Data is available after 2 cycles (load latency)
// This can dual-issue with C1 pipe operations (FPU arithmetic)
def R5900WriteFPULoad : SchedWriteRes<[R5900Slot1, R5900PipeLS]> {
  let Latency = 2;
}

// FPU Store (SWC1): Uses LS pipe only
def R5900WriteFPUStore : SchedWriteRes<[R5900Slot1, R5900PipeLS]> {
  let Latency = 1;
}

// MFC1/MTC1: Use LS pipe (COP1 Move), issues from Slot1
// Note: COP1 Move + COP1 Operate in same cycle causes 1-cycle stall
def R5900WriteFPUTransfer : SchedWriteRes<[R5900Slot1, R5900PipeLS]> {
  let Latency = 2;
}

// FPU Accumulator operations
// The R5900 FPU has internal ACC forwarding - ACC-to-ACC has 1-cycle latency.
// Only the final madd.s/msub.s (which reads ACC and writes to FGR) has 4-cycle latency.

// ACC-init (mula.s, adda.s, suba.s): writes to ACC only, 1-cycle forwarding latency
def R5900WriteFPUAccInit : SchedWriteRes<[R5900Slot0, R5900PipeC1]> {
  let Latency = 1;
}

// ACC-accumulate (madda.s, msuba.s): reads/writes ACC, 1-cycle forwarding latency
def R5900WriteFPUAccChain : SchedWriteRes<[R5900Slot0, R5900PipeC1]> {
  let Latency = 1;
}

// ACC-read (madd.s, msub.s): reads ACC, writes to FGR32, 4-cycle latency to result
def R5900WriteFPUAccResult : SchedWriteRes<[R5900Slot0, R5900PipeC1]> {
  let Latency = 4;
}

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

// movn.s, movz.s, movf.s, movt.s (conditional moves)
def : InstRW<[R5900WriteFPUMove], (instrs MOVN_I_S, MOVZ_I_S, MOVF_S, MOVT_S,
                                   MOVF_I, MOVT_I)>;

// R5900 FPU Accumulator Instructions
// ACC-init: mula.s, adda.s, suba.s (1-cycle to ACC)
def : InstRW<[R5900WriteFPUAccInit], (instrs ADDA_S, SUBA_S, MULA_S)>;

// ACC-chain: madda.s, msuba.s (1-cycle ACC forwarding)
def : InstRW<[R5900WriteFPUAccChain], (instrs MADDA_S, MSUBA_S)>;

// ACC-result: madd.s, msub.s (4-cycle to FGR32)
def : InstRW<[R5900WriteFPUAccResult], (instrs R5900_MADD_S, R5900_MSUB_S)>;

//===----------------------------------------------------------------------===//
// COP0 Instructions
//===----------------------------------------------------------------------===//
// COP0 instructions use the LS pipeline and issue from Slot1

def R5900WriteCOP0 : SchedWriteRes<[R5900Slot1, R5900PipeLS]> {
  let Latency = 3;
}

def R5900WriteTLB : SchedWriteRes<[R5900Slot1, R5900PipeLS]> {
  let Latency = 4;
}

def R5900WriteERET : SchedWriteRes<[R5900Slot1, R5900PipeI1]> {
  let Latency = 1;
}

def : InstRW<[R5900WriteCOP0], (instrs MFC0, MTC0, DMFC0, DMTC0)>;
def : InstRW<[R5900WriteTLB], (instrs TLBP, TLBR, TLBWI, TLBWR)>;
def : InstRW<[R5900WriteERET], (instrs DERET, ERET, ERet, WAIT, EHB)>;

//===----------------------------------------------------------------------===//
// 64-bit Load/Store Variants
//===----------------------------------------------------------------------===//
def : InstRW<[R5900WriteLoad], (instrs LB64, LBu64, LH64, LHu64, LW64,
                                 LWL64, LWR64)>;
def : InstRW<[R5900WriteStore], (instrs SB64, SH64, SW64, SWL64, SWR64)>;

//===----------------------------------------------------------------------===//
// Cache/Prefetch Instructions
//===----------------------------------------------------------------------===//
def R5900WriteCache : SchedWriteRes<[R5900Slot1, R5900PipeLS]> {
  let Latency = 1;
}
def : InstRW<[R5900WriteCache], (instrs CACHE, PREF)>;

//===----------------------------------------------------------------------===//
// RDHWR Instruction
//===----------------------------------------------------------------------===//
def : InstRW<[R5900WriteCOP0], (instrs RDHWR, RDHWR64)>;

//===----------------------------------------------------------------------===//
// COP2/COP3 Instructions (VU0 macro mode and placeholder)
//===----------------------------------------------------------------------===//
// VU0 macro mode uses C2 pipeline. COP3 is placeholder.
def R5900WriteCOP2 : SchedWriteRes<[R5900Slot0, R5900PipeC2]> {
  let Latency = 4;
}

def R5900WriteCOP2Move : SchedWriteRes<[R5900Slot1, R5900PipeLS]> {
  let Latency = 2;
}

def : InstRW<[R5900WriteCOP2Move], (instrs MFC2, MTC2, DMFC2, DMTC2)>;
def : InstRW<[R5900WriteCOP2], (instrs LWC2, LWC3, SWC2, SWC3,
                                 LDC2, LDC3, SDC2, SDC3)>;

//===----------------------------------------------------------------------===//
// R5900-Specific FPU Instructions
//===----------------------------------------------------------------------===//
// R5900 MIN/MAX/RSQRT instructions

def : InstRW<[R5900WriteFPUArith], (instrs R5900_MAX_S, R5900_MIN_S)>;
def : InstRW<[R5900WriteFPUSqrt], (instrs R5900_RSQRT_S)>;

} // SchedModel = MipsR5900Model

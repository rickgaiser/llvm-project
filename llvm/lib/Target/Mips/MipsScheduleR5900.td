//==- MipsScheduleR5900.td - R5900 Scheduling Definitions --*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file describes the scheduling model for the R5900 (PS2 Emotion Engine)
// processor. The R5900 is a MIPS III-based 2-way superscalar in-order processor.
//
// Pipeline Structure:
//   - 6-stage integer pipeline
//   - 8-stage FPU pipeline
//   - Two integer pipelines (I0, I1) that can each execute ALU ops
//   - Separate functional units for multiply/divide, load/store, branch, FPU
//
// Dual-Issue Rules:
//   - ALU ops can issue to either pipeline (dual-issue capable)
//   - Load/Store uses dedicated LS unit (one per cycle)
//   - MAC0 (mult/div) is in I0, MAC1 (mult1/div1) is in I1
//   - FPU operations use C1 pipeline
//   - VU0 macro mode uses C2 pipeline
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
// Processor Resources
//===----------------------------------------------------------------------===//
// Modeled after Cortex-A53/Spacemit-X60 style: functional units with counts
// that naturally enforce issue constraints.

let BufferSize = 0 in {
  // Integer ALU - can dual-issue (2 ALU ops per cycle)
  def R5900UnitALU : ProcResource<2>;

  // Multiply/Divide units - one in each integer pipeline
  def R5900UnitMAC0 : ProcResource<1>;  // Pipeline 0: HI0/LO0
  def R5900UnitMAC1 : ProcResource<1>;  // Pipeline 1: HI1/LO1

  // Load/Store unit - single issue per cycle
  def R5900UnitLdSt : ProcResource<1>;

  // Branch unit
  def R5900UnitBranch : ProcResource<1>;

  // FPU (COP1) - single precision only
  def R5900UnitFPU : ProcResource<1>;

  // FPU Division/Square Root - shared, blocks during operation
  def R5900UnitFPUDivSqrt : ProcResource<1>;

  // VU0 macro mode (COP2)
  def R5900UnitVU0 : ProcResource<1>;
}

//===----------------------------------------------------------------------===//
// Integer ALU Instructions
//===----------------------------------------------------------------------===//
// ALU ops can execute on either pipeline, so they can dual-issue with each
// other. Using ProcResource<2> allows 2 ALU ops per cycle.

def R5900WriteALU : SchedWriteRes<[R5900UnitALU]> {
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
// Multiply/Divide Instructions (MAC0 - Pipeline 0)
//===----------------------------------------------------------------------===//
// MULT/DIV use MAC0 unit. They consume one ALU slot and the MAC0 resource.

def R5900WriteMul : SchedWriteRes<[R5900UnitALU, R5900UnitMAC0]> {
  let Latency = 4;
  let ReleaseAtCycles = [1, 2];  // MAC0 has 2-cycle throughput
}

def R5900WriteDiv : SchedWriteRes<[R5900UnitALU, R5900UnitMAC0]> {
  let Latency = 37;
  let ReleaseAtCycles = [1, 37];
}

def R5900WriteHILO : SchedWriteRes<[R5900UnitALU]> {
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
def : InstRW<[R5900WriteHILO], (instrs MTHI, MTLO, PseudoMTLOHI,
                                MTHI64, MTLO64, PseudoMTLOHI64)>;

// MIPS III 64-bit multiply/divide
def : InstRW<[R5900WriteMul], (instrs DMULT, DMULTu, PseudoDMULT, PseudoDMULTu)>;
def : InstRW<[R5900WriteDiv], (instrs DSDIV, DUDIV, PseudoDSDIV, PseudoDUDIV)>;

//===----------------------------------------------------------------------===//
// R5900 3-Operand Multiply (Pipeline 0)
//===----------------------------------------------------------------------===//

// 3-operand MULT/MULTU (writes to rd, HI, LO)
def : InstRW<[R5900WriteMul], (instrs R5900_MULT, R5900_MULTU)>;

// 3-operand MADD/MADDU (Pipeline 0)
def : InstRW<[R5900WriteMul], (instrs R5900_MADD, R5900_MADDU)>;

//===----------------------------------------------------------------------===//
// R5900 Pipeline 1 Instructions (MAC1)
//===----------------------------------------------------------------------===//
// MULT1/DIV1 use MAC1 unit in pipeline 1.

def R5900WriteMul1 : SchedWriteRes<[R5900UnitALU, R5900UnitMAC1]> {
  let Latency = 4;
  let ReleaseAtCycles = [1, 2];  // MAC1 has 2-cycle throughput
}

def R5900WriteDiv1 : SchedWriteRes<[R5900UnitALU, R5900UnitMAC1]> {
  let Latency = 37;
  let ReleaseAtCycles = [1, 37];
}

def R5900WriteHILO1 : SchedWriteRes<[R5900UnitALU]> {
  let Latency = 1;
}

// mult1, multu1
def : InstRW<[R5900WriteMul1], (instrs MULT1, MULTU1)>;

// div1, divu1
def : InstRW<[R5900WriteDiv1], (instrs DIV1, DIVU1)>;

// mfhi1, mflo1
def : InstRW<[R5900WriteHILO1], (instrs MFHI1, MFLO1)>;

// mthi1, mtlo1
def : InstRW<[R5900WriteHILO1], (instrs MTHI1, MTLO1)>;

// madd1, maddu1
def : InstRW<[R5900WriteMul1], (instrs MADD1, MADDU1)>;

//===----------------------------------------------------------------------===//
// Load/Store Instructions
//===----------------------------------------------------------------------===//
// Load/Store use the dedicated LS unit (one operation per cycle).

def R5900WriteLoad : SchedWriteRes<[R5900UnitLdSt]> {
  let Latency = 1;
}

def R5900WriteStore : SchedWriteRes<[R5900UnitLdSt]> {
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

// 64-bit load/store variants
def : InstRW<[R5900WriteLoad], (instrs LB64, LBu64, LH64, LHu64, LW64,
                                 LWL64, LWR64)>;
def : InstRW<[R5900WriteStore], (instrs SB64, SH64, SW64, SWL64, SWR64)>;

// SYNC - uses ALU (cannot dual-issue with load/store)
def R5900WriteSync : SchedWriteRes<[R5900UnitALU]> {
  let Latency = 1;
}
def : InstRW<[R5900WriteSync], (instrs SYNC)>;

//===----------------------------------------------------------------------===//
// Branch Instructions
//===----------------------------------------------------------------------===//
// Branch instructions use the branch unit.

def R5900WriteBranch : SchedWriteRes<[R5900UnitBranch]> {
  let Latency = 1;
}

def R5900WriteJAL : SchedWriteRes<[R5900UnitBranch]> {
  let Latency = 2;
}

def R5900WriteTrap : SchedWriteRes<[R5900UnitBranch]> {
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
// COP1 (FPU) Instructions - Single Precision Only
//===----------------------------------------------------------------------===//
// FPU operations use the dedicated FPU unit. The R5900 FPU has an 8-stage
// pipeline but only supports single precision.

// FPU Arithmetic (add.s, sub.s, mul.s): 4 cycles
def R5900WriteFPUArith : SchedWriteRes<[R5900UnitFPU]> {
  let Latency = 4;
}

// FPU Division: 8 cycles latency, 7 cycle throughput, blocks div/sqrt unit
def R5900WriteFPUDiv : SchedWriteRes<[R5900UnitFPU, R5900UnitFPUDivSqrt]> {
  let Latency = 8;
  let ReleaseAtCycles = [1, 7];
}

// FPU Square Root: 8 cycles latency, 7 cycle throughput, blocks div/sqrt unit
def R5900WriteFPUSqrt : SchedWriteRes<[R5900UnitFPU, R5900UnitFPUDivSqrt]> {
  let Latency = 8;
  let ReleaseAtCycles = [1, 7];
}

// FPU Reciprocal Square Root: 14 cycles latency, 13 cycle throughput
def R5900WriteFPURsqrt : SchedWriteRes<[R5900UnitFPU, R5900UnitFPUDivSqrt]> {
  let Latency = 14;
  let ReleaseAtCycles = [1, 13];
}

// FPU Move/Abs/Neg: 4 cycles
def R5900WriteFPUMove : SchedWriteRes<[R5900UnitFPU]> {
  let Latency = 4;
}

// FPU Compare: 4 cycles (per official EE Core Users Manual section 2.12)
def R5900WriteFPUCmp : SchedWriteRes<[R5900UnitFPU]> {
  let Latency = 4;
}

// FPU Load (LWC1): Uses load/store unit, 2-cycle latency for FP use
def R5900WriteFPULoad : SchedWriteRes<[R5900UnitLdSt]> {
  let Latency = 2;
}

// FPU Store (SWC1): Uses load/store unit
def R5900WriteFPUStore : SchedWriteRes<[R5900UnitLdSt]> {
  let Latency = 1;
}

// MFC1/MTC1: Uses load/store unit (COP1 Move path)
def R5900WriteFPUTransfer : SchedWriteRes<[R5900UnitLdSt]> {
  let Latency = 2;
}

// FPU Accumulator operations
// The R5900 FPU has internal ACC forwarding - ACC-to-ACC has 1-cycle latency.

// ACC-init (mula.s, adda.s, suba.s): writes to ACC only
def R5900WriteFPUAccInit : SchedWriteRes<[R5900UnitFPU]> {
  let Latency = 1;
}

// ACC-accumulate (madda.s, msuba.s): reads/writes ACC
def R5900WriteFPUAccChain : SchedWriteRes<[R5900UnitFPU]> {
  let Latency = 1;
}

// ACC-read (madd.s, msub.s): reads ACC, writes to FGR32
def R5900WriteFPUAccResult : SchedWriteRes<[R5900UnitFPU]> {
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
def : InstRW<[R5900WriteFPUAccInit], (instrs ADDA_S, SUBA_S, MULA_S)>;
def : InstRW<[R5900WriteFPUAccChain], (instrs MADDA_S, MSUBA_S)>;
def : InstRW<[R5900WriteFPUAccResult], (instrs R5900_MADD_S, R5900_MSUB_S)>;

// R5900-specific FPU: MIN/MAX/RSQRT
def : InstRW<[R5900WriteFPUArith], (instrs R5900_MAX_S, R5900_MIN_S)>;
def : InstRW<[R5900WriteFPURsqrt], (instrs R5900_RSQRT_S)>;

//===----------------------------------------------------------------------===//
// COP0 Instructions
//===----------------------------------------------------------------------===//
// COP0 instructions use the load/store path.

def R5900WriteCOP0 : SchedWriteRes<[R5900UnitLdSt]> {
  let Latency = 3;
}

def R5900WriteTLB : SchedWriteRes<[R5900UnitLdSt]> {
  let Latency = 4;
}

def R5900WriteERET : SchedWriteRes<[R5900UnitALU]> {
  let Latency = 1;
}

def : InstRW<[R5900WriteCOP0], (instrs MFC0, MTC0, DMFC0, DMTC0)>;
def : InstRW<[R5900WriteTLB], (instrs TLBP, TLBR, TLBWI, TLBWR)>;
def : InstRW<[R5900WriteERET], (instrs DERET, ERET, ERet, WAIT, EHB)>;

//===----------------------------------------------------------------------===//
// Cache/Prefetch Instructions
//===----------------------------------------------------------------------===//

def R5900WriteCache : SchedWriteRes<[R5900UnitLdSt]> {
  let Latency = 1;
}
def : InstRW<[R5900WriteCache], (instrs CACHE, PREF)>;

//===----------------------------------------------------------------------===//
// RDHWR Instruction
//===----------------------------------------------------------------------===//

def : InstRW<[R5900WriteCOP0], (instrs RDHWR, RDHWR64)>;

//===----------------------------------------------------------------------===//
// COP2 Instructions (VU0 macro mode)
//===----------------------------------------------------------------------===//
// VU0 macro mode uses the VU0 unit.

def R5900WriteCOP2 : SchedWriteRes<[R5900UnitVU0]> {
  let Latency = 4;
}

def R5900WriteCOP2Move : SchedWriteRes<[R5900UnitLdSt]> {
  let Latency = 2;
}

def : InstRW<[R5900WriteCOP2Move], (instrs MFC2, MTC2, DMFC2, DMTC2)>;
def : InstRW<[R5900WriteCOP2], (instrs LWC2, LWC3, SWC2, SWC3,
                                 LDC2, LDC3, SDC2, SDC3)>;

//===----------------------------------------------------------------------===//
// VU0 Quadword Load/Store (128-bit)
//===----------------------------------------------------------------------===//
// LQC2/SQC2 use the load/store unit with VU0.

def R5900WriteVFLoad : SchedWriteRes<[R5900UnitLdSt]> {
  let Latency = 2;
}

def R5900WriteVFStore : SchedWriteRes<[R5900UnitLdSt]> {
  let Latency = 1;
}

def : InstRW<[R5900WriteVFLoad], (instrs LQC2)>;
def : InstRW<[R5900WriteVFStore], (instrs SQC2)>;

//===----------------------------------------------------------------------===//
// VU0 Arithmetic Instructions
//===----------------------------------------------------------------------===//
// VU0 macro mode vector arithmetic uses the VU0 unit.
// Latency is 4 cycles (FMAC pipeline).

def R5900WriteVU0Arith : SchedWriteRes<[R5900UnitVU0]> {
  let Latency = 4;
}

// VU0 ACC-writing instructions have 1-cycle latency for ACC chaining
def R5900WriteVU0AccInit : SchedWriteRes<[R5900UnitVU0]> {
  let Latency = 1;
}

// VU0 ACC-reading instructions have 4-cycle latency
def R5900WriteVU0AccResult : SchedWriteRes<[R5900UnitVU0]> {
  let Latency = 4;
}

// VADD, VSUB, VMUL (3-register, VF destination)
def : InstRW<[R5900WriteVU0Arith], (instrs VADD, VSUB, VMUL)>;

// VADDA, VSUBA, VMULA (ACC-writing, no VF destination)
def : InstRW<[R5900WriteVU0AccInit], (instrs VADDA, VSUBA, VMULA)>;

// VMADD, VMSUB (ACC-reading, VF destination)
def : InstRW<[R5900WriteVU0AccResult], (instrs VMADD, VMSUB)>;

// VMADDA, VMSUBA (ACC-reading and ACC-writing)
def : InstRW<[R5900WriteVU0AccResult], (instrs VMADDA, VMSUBA)>;

} // SchedModel = MipsR5900Model

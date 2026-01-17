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
//===----------------------------------------------------------------------===//

def MipsR5900Model : SchedMachineModel {
  int IssueWidth = 2;        // 2-way superscalar
  int MicroOpBufferSize = 0; // In-order processor
  int LoadLatency = 1;       // Integer loads: 1 cycle
  int MispredictPenalty = 4; // 6-stage pipeline
  int HighLatency = 37;      // DIV is 37 cycles

  let CompleteModel = 1;
  let PostRAScheduler = 1;
  let FullInstRWOverlapCheck = 1;

  // R5900 is MIPS III based with limited MIPS IV extensions.
  // Exclude unsupported instruction categories:
  // - MIPS32/64 ISA revisions (R5900 is MIPS III/IV based)
  // - microMIPS and MIPS16 encodings
  // - Cavium extensions (CnMips)
  // - DSP, 3D, MT, CRC, MSA, virtualization, GINV, EVA extensions
  // - NotR5900: instructions explicitly excluded from R5900
  // - IsFP64bit: 64-bit FPU (R5900 has single-precision only)
  // - IsNotSingleFloat: double-precision FPU operations
  // - HasMips4_32: MIPS IV instructions not in R5900's subset (MOVF/MOVT)
  // - HasMips4_32r2: MIPS IV/32R2 instructions (RECIP, RSQRT, MADD.S, etc.)
  // - HasMips3_32r2: MIPS III/32R2 subset (CVT.L.S, etc.)
  list<Predicate> UnsupportedFeatures = [HasMips32, HasMips32r2, HasMips32r5,
                                         HasMips32r6,
                                         HasMips64, HasMips64r2, HasMips64r5,
                                         HasMips64r6,
                                         InMicroMips, InMips16Mode,
                                         HasCnMips, HasCnMipsP,
                                         HasDSP, HasDSPR2, HasMips3D, HasMT,
                                         HasCRC, HasMSA, HasVirt, HasGINV,
                                         HasEVA,
                                         // R5900-specific exclusions
                                         NotR5900,
                                         IsFP64bit, IsNotSingleFloat,
                                         HasMips4_32, HasMips4_32r2,
                                         HasMips3_32r2];
}

let SchedModel = MipsR5900Model in {

//===----------------------------------------------------------------------===//
// Processor Resources
//===----------------------------------------------------------------------===//
// The R5900 is a 2-way superscalar with two logical pipelines (Pipe 0, Pipe 1).
// Each pipeline can issue one instruction per cycle. Some functional units are
// tied to a specific pipe, while ALU/Branch can use either.
//
// Pipe 0: COP1(FPU) Operate, MAC0, COP2(VU0) Operate, SA Operate
// Pipe 1: Load/Store, MAC1, COP0, COP1(FPU) Move, COP2(VU0) Move, SYNC, LZC, ERET
// Either: ALU, Branch
// Both:   Wide Operate (MMI)

let BufferSize = 0 in {
  // Physical pipelines - enforce dual-issue constraints
  def R5900Pipe0 : ProcResource<1>;  // I0 pipeline
  def R5900Pipe1 : ProcResource<1>;  // I1 pipeline

  // Integer ALU - can use either pipeline
  def R5900UnitALU : ProcResGroup<[R5900Pipe0, R5900Pipe1]>;

  // Branch unit - can use either pipeline
  def R5900UnitBranch : ProcResGroup<[R5900Pipe0, R5900Pipe1]>;

  // Multiply/Divide unit (Pipeline 0 only)
  def R5900UnitMAC0 : ProcResource<1>;  // HI0/LO0

  // Load/Store unit (Pipeline 1 only)
  def R5900UnitLdSt : ProcResource<1>;

  // FPU (COP1) Operate - Pipeline 0 only, single precision
  def R5900UnitFPU : ProcResource<1>;
}

//===----------------------------------------------------------------------===//
// Integer ALU Instructions
//===----------------------------------------------------------------------===//
// ALU ops can execute on either pipeline, so they can dual-issue with each
// other. Using ProcResource<2> allows 2 ALU ops per cycle.

// EE Core Users Manual 2.12: latency=1, throughput=1
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
// MULT/DIV use MAC0 unit in Pipeline 0. They can dual-issue with Pipe 1 ops.

// EE Core Users Manual 2.12: latency=4, throughput=2 (Pipe 0 only)
def R5900WriteMul : SchedWriteRes<[R5900Pipe0, R5900UnitMAC0]> {
  let Latency = 4;
  let ReleaseAtCycles = [1, 2];
}

// EE Core Users Manual 2.12: latency=37, throughput=37 (Pipe 0 only)
def R5900WriteDiv : SchedWriteRes<[R5900Pipe0, R5900UnitMAC0]> {
  let Latency = 37;
  let ReleaseAtCycles = [1, 37];
}

// EE Core Users Manual 2.11.1: MFHI/MFLO/MTHI/MTLO use I0's MAC unit
// EE Core Users Manual 2.12: latency=1, throughput=1 (Pipe 0 only)
def R5900WriteHILO : SchedWriteRes<[R5900Pipe0, R5900UnitMAC0]> {
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
// Load/Store Instructions (Pipeline 1)
//===----------------------------------------------------------------------===//
// Load/Store use the dedicated LS unit in Pipeline 1.

// EE Core Users Manual 2.12: latency=1, throughput=1 (cache hit, Pipe 1 only)
def R5900WriteLoad : SchedWriteRes<[R5900Pipe1, R5900UnitLdSt]> {
  let Latency = 1;
}

// EE Core Users Manual 2.12: throughput=1 (store has no result latency, Pipe 1 only)
def R5900WriteStore : SchedWriteRes<[R5900Pipe1, R5900UnitLdSt]> {
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

// SYNC - Pipeline 1 only (cannot dual-issue with load/store)
// Estimated: latency=1, throughput=1 (Pipe 1 only)
def R5900WriteSync : SchedWriteRes<[R5900Pipe1]> {
  let Latency = 1;
}
def : InstRW<[R5900WriteSync], (instrs SYNC)>;

//===----------------------------------------------------------------------===//
// Branch Instructions
//===----------------------------------------------------------------------===//
// Branch instructions use the branch unit.

// EE Core Users Manual 2.12: latency=1, throughput=1 (Branch / Jump)
def R5900WriteBranch : SchedWriteRes<[R5900UnitBranch]> {
  let Latency = 1;
}

// Estimated: latency=1, throughput=1 (similar to branch)
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
def : InstRW<[R5900WriteBranch], (instrs BAL, BAL_BR, BGEZAL, BGEZALL, BLTZAL,
                                  BLTZALL, JAL, JALR, JALR_HB, JALRPseudo,
                                  JALRHBPseudo)>;

// MIPS III 64-bit JAL
def : InstRW<[R5900WriteBranch], (instrs JALR64, JALR64Pseudo, JALRHB64Pseudo,
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

// EE Core Users Manual 2.12: latency=4, throughput=1
// (Add/Sub/Abs/Neg/C.cond, CVT, Mul, Move, MADD - Pipe 0 only)
def R5900WriteFPUOps : SchedWriteRes<[R5900Pipe0, R5900UnitFPU]> {
  let Latency = 4;
}

// EE Core Users Manual 2.12: latency=8, throughput=7 (DIV.S, SQRT.S - Pipe 0 only, blocks FPU)
def R5900WriteFPUDivSqrt : SchedWriteRes<[R5900Pipe0, R5900UnitFPU]> {
  let Latency = 8;
  let ReleaseAtCycles = [1, 7];
}

// EE Core Users Manual 2.12: latency=14, throughput=13 (RSQRT.S - Pipe 0 only, blocks FPU)
def R5900WriteFPURsqrt : SchedWriteRes<[R5900Pipe0, R5900UnitFPU]> {
  let Latency = 14;
  let ReleaseAtCycles = [1, 13];
}

// EE Core Users Manual 2.12: latency=2, throughput=1 (LWC1, cache hit, Pipe 1 only)
def R5900WriteFPULoad : SchedWriteRes<[R5900Pipe1, R5900UnitLdSt]> {
  let Latency = 2;
}

// Estimated: latency=2, throughput=1 (SWC1 not in Section 2.12, using LWC1 timing)
def R5900WriteFPUStore : SchedWriteRes<[R5900Pipe1, R5900UnitLdSt]> {
  let Latency = 2;
}

// EE Core Users Manual 2.12: latency=2, throughput=1 (MFC1/MTC1, Pipe 1 only)
def R5900WriteFPUTransfer : SchedWriteRes<[R5900Pipe1, R5900UnitLdSt]> {
  let Latency = 2;
}

// add.s, sub.s
def : InstRW<[R5900WriteFPUOps], (instrs FADD_S, FSUB_S)>;

// mul.s
def : InstRW<[R5900WriteFPUOps], (instrs FMUL_S)>;

// div.s, sqrt.s
def : InstRW<[R5900WriteFPUDivSqrt], (instrs FDIV_S, FSQRT_S)>;

// abs.s, neg.s, mov.s
def : InstRW<[R5900WriteFPUOps], (instrs FABS_S, FNEG_S, FMOV_S)>;

// c.<cc>.s (floating point compare)
def : InstRW<[R5900WriteFPUOps], (instrs FCMP_S32, BC1F, BC1T, BC1FL, BC1TL)>;
def : InstRW<[R5900WriteFPUOps], (instregex "C_[A-Z]+_S$")>;

// cvt.s.w, cvt.w.s
def : InstRW<[R5900WriteFPUOps], (instrs CVT_S_W, CVT_W_S)>;

// mfc1, mtc1, cfc1, ctc1
def : InstRW<[R5900WriteFPUTransfer], (instrs MFC1, MTC1, CFC1, CTC1)>;

// lwc1 (FPU load)
def : InstRW<[R5900WriteFPULoad], (instrs LWC1)>;

// swc1 (FPU store)
def : InstRW<[R5900WriteFPUStore], (instrs SWC1)>;

// movn.s, movz.s (conditional moves based on GPR)
// Note: R5900 does NOT support MOVT/MOVF (FCC-based conditional moves)
def : InstRW<[R5900WriteFPUOps], (instrs MOVN_I_S, MOVZ_I_S)>;

// R5900-specific FPU: MIN/MAX/RSQRT
def : InstRW<[R5900WriteFPUOps], (instrs R5900_MAX_S, R5900_MIN_S)>;
def : InstRW<[R5900WriteFPURsqrt], (instrs R5900_RSQRT_S)>;

//===----------------------------------------------------------------------===//
// COP0 Instructions (Pipeline 1)
//===----------------------------------------------------------------------===//
// COP0 instructions use Pipeline 1.

// Estimated: latency=3, throughput=1 (not in Section 2.12 table, Pipe 1 only)
def R5900WriteCOP0 : SchedWriteRes<[R5900Pipe1]> {
  let Latency = 3;
}

// Estimated: latency=4, throughput=1 (not in Section 2.12 table, Pipe 1 only)
def R5900WriteTLB : SchedWriteRes<[R5900Pipe1]> {
  let Latency = 4;
}

// Estimated: latency=1, throughput=1 (not in Section 2.12 table, Pipe 1 only)
def R5900WriteERET : SchedWriteRes<[R5900Pipe1]> {
  let Latency = 1;
}

def : InstRW<[R5900WriteCOP0], (instrs MFC0, MTC0, DMFC0, DMTC0)>;
def : InstRW<[R5900WriteTLB], (instrs TLBP, TLBR, TLBWI, TLBWR)>;
def : InstRW<[R5900WriteERET], (instrs DERET, ERET, ERet, WAIT, EHB)>;

//===----------------------------------------------------------------------===//
// Cache/Prefetch Instructions
//===----------------------------------------------------------------------===//

// Estimated: latency=1, throughput=1 (not in Section 2.12 table, Pipe 1 only)
def R5900WriteCache : SchedWriteRes<[R5900Pipe1, R5900UnitLdSt]> {
  let Latency = 1;
}
def : InstRW<[R5900WriteCache], (instrs CACHE, PREF)>;

//===----------------------------------------------------------------------===//
// RDHWR Instruction
//===----------------------------------------------------------------------===//

def : InstRW<[R5900WriteCOP0], (instrs RDHWR, RDHWR64)>;

//===----------------------------------------------------------------------===//
// R5900-Specific Instructions
//===----------------------------------------------------------------------===//

// Enable/Disable Interrupts
def : InstRW<[R5900WriteCOP0], (instrs EI_R5900, DI_R5900)>;

// Breakpoint Register Instructions (read from breakpoint regs)
def : InstRW<[R5900WriteCOP0], (instrs MFBPC, MFIAB, MFIABM, MFDAB, MFDABM,
                                MFDVB, MFDVBM)>;

// Breakpoint Register Instructions (write to breakpoint regs)
def : InstRW<[R5900WriteCOP0], (instrs MTBPC, MTIAB, MTIABM, MTDAB, MTDABM,
                                MTDVB, MTDVBM)>;

// COP0 Conditional Branches
def : InstRW<[R5900WriteBranch], (instrs BC0F, BC0T)>;

// R5900 Multiply-Multiply-Add pseudo
def : InstRW<[R5900WriteMul], (instrs PseudoR5900MulMulAdd)>;

//===----------------------------------------------------------------------===//
// MIPS IV Conditional Moves
//===----------------------------------------------------------------------===//

// Integer conditional moves (32-bit)
def : InstRW<[R5900WriteALU], (instrs MOVN_I_I, MOVZ_I_I)>;

// Integer conditional moves (64-bit variants)
def : InstRW<[R5900WriteALU], (instrs MOVN_I_I64, MOVZ_I_I64, MOVN_I64_I,
                                MOVZ_I64_I, MOVN_I64_I64, MOVZ_I64_I64)>;

// FPU conditional moves with 64-bit GPR condition (single precision)
def : InstRW<[R5900WriteFPUOps], (instrs MOVN_I64_S, MOVZ_I64_S)>;

//===----------------------------------------------------------------------===//
// COP2 (VU0 Macro Mode) Instructions
//===----------------------------------------------------------------------===//

// COP2 Load/Store - uses load/store unit (Pipe 1)
def : InstRW<[R5900WriteLoad], (instrs LWC2, LDC2)>;
def : InstRW<[R5900WriteStore], (instrs SWC2, SDC2)>;

// COP2 Register Transfers (Pipe 1 - COP2 Move)
def : InstRW<[R5900WriteCOP0], (instrs MFC2, MTC2, DMFC2, DMTC2)>;

//===----------------------------------------------------------------------===//
// 64-bit GPR Instructions
//===----------------------------------------------------------------------===//

// 64-bit shift pseudo
def : InstRW<[R5900WriteALU], (instrs DSLL64_32)>;

// 64-bit indirect branch
def : InstRW<[R5900WriteBranch], (instrs PseudoIndirectBranch64)>;

//===----------------------------------------------------------------------===//
// FPU Conversion Pseudo
//===----------------------------------------------------------------------===//

// Single-precision conversion from integer
def : InstRW<[R5900WriteFPUOps], (instrs PseudoCVT_S_W)>;

} // SchedModel = MipsR5900Model

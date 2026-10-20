//=- MipsScheduleR5900.td - R5900 Scheduling Definitions --*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The R5900 is an in-order MIPS III core that issues up to two instructions
// per cycle into two logical pipes:
//   - pipe 0: FPU operations, multiply/divide (MAC0) and HI/LO moves,
//   - pipe 1: loads, stores, prefetch, FPU loads/stores/moves, SYNC and COP0,
//   - either pipe: ALU operations and branches.
// All FPU instructions, including the FPU loads, stores and moves, also use
// the single COP1 (C1) slot, so at most one of them issues per cycle.
//
// The latencies are from the EE Core User's Manual, section 2.12, as measured
// on hardware. Measured differences from the manual:
//   - a load result is available to the instruction issued 2 cycles later,
//   - DIV.S and SQRT.S have a latency of 7 and hold C1 for 2 cycles,
//   - MFC1 and SWC1 read their FPR one cycle later than FPU operations.
//
//===----------------------------------------------------------------------===//

def MipsR5900Model : SchedMachineModel {
  int IssueWidth = 2;
  int MicroOpBufferSize = 0; // In-order.
  int LoadLatency = 2;
  int MispredictPenalty = 4; // Estimated.
  int HighLatency = 37;

  let CompleteModel = 1;
  let PostRAScheduler = 1;
  let FullInstRWOverlapCheck = 1;

  list<Predicate> UnsupportedFeatures = [HasMips32, HasMips32r2, HasMips32r5,
                                         HasMips32r6, HasMips64, HasMips64r2,
                                         HasMips64r5, HasMips64r6, HasMips4_32,
                                         HasMips4_32r2, HasMips5_32r2,
                                         HasMadd4, IsNotSingleFloat, NotR5900,
                                         InMicroMips, InMips16Mode, HasCnMips,
                                         HasCnMipsP, HasDSP, HasDSPR2,
                                         HasDSPR3, HasMips3D, HasMT, HasCRC,
                                         HasMSA, HasVirt, HasGINV, HasEVA];
}

let SchedModel = MipsR5900Model in {

// In-order: an instruction waits in issue until its resources are free.
let BufferSize = 0 in {
def R5900Pipe0 : ProcResource<1>;
def R5900Pipe1 : ProcResource<1>;
def R5900Pipe01 : ProcResGroup<[R5900Pipe0, R5900Pipe1]>;

// The COP1 slot, shared by all FPU instructions.
def R5900C1 : ProcResource<1>;
// Multiply/divide unit for HI/LO.
def R5900MAC0 : ProcResource<1>;
// FPU divide/square root.
def R5900FDiv : ProcResource<1>;
} // BufferSize = 0

// Instructions complete out of order: a short instruction does not wait for
// an earlier long one.
class R5900Write<list<ProcResourceKind> Res, int Lat = 1,
                 list<int> Release = []> : SchedWriteRes<Res> {
  let Latency = Lat;
  let ReleaseAtCycles = Release;
  let RetireOOO = 1;
}

// ALU
// ===

def R5900WriteALU : R5900Write<[R5900Pipe01]>;

def : InstRW<[R5900WriteALU], (instrs ADD, ADDi, ADDiu, ADDu, AND, ANDi,
                               LEA_ADDiu, LUi, NOP, NOR, OR, ORi, SLL, SLLV,
                               SLT, SLTi, SLTiu, SLTu, SRA, SRAV, SRL, SRLV,
                               SSNOP, SUB, SUBu, XOR, XORi, COPY, EHB)>;

def : InstRW<[R5900WriteALU], (instrs DADD, DADDi, DADDiu, DADDu, DSLL,
                               DSLL32, DSLLV, DSRA, DSRA32, DSRAV, DSRL,
                               DSRL32, DSRLV, DSUB, DSUBu, AND64, ANDi64,
                               OR64, ORi64, NOR64, XOR64, XORi64, SLL64_32,
                               SLL64_64, SLT64, SLTi64, SLTiu64, SLTu64,
                               LUi64, LEA_ADDiu64, DSLL64_32)>;

// movn, movz
def : InstRW<[R5900WriteALU], (instrs MOVN_I_I, MOVN_I_I64, MOVN_I64_I,
                               MOVN_I64_I64, MOVZ_I_I, MOVZ_I_I64, MOVZ_I64_I,
                               MOVZ_I64_I64)>;

// Multiply and divide (MAC0)
// ==========================

def R5900WriteMul : R5900Write<[R5900Pipe0, R5900MAC0], 4, [1, 2]>;
def R5900WriteDiv : R5900Write<[R5900Pipe0, R5900MAC0], 37, [1, 37]>;
def R5900WriteMoveHILO : R5900Write<[R5900Pipe0]>;

def : InstRW<[R5900WriteMul], (instrs MULT, MULTu, PseudoMULT, PseudoMULTu)>;
def : InstRW<[R5900WriteDiv], (instrs SDIV, UDIV, PseudoSDIV, PseudoUDIV)>;
def : InstRW<[R5900WriteMoveHILO], (instrs MFHI, MFLO, MTHI, MTLO, PseudoMFHI,
                                    PseudoMFLO, PseudoMTLOHI, MFHI64, MFLO64,
                                    MTHI64, MTLO64, PseudoMFHI64,
                                    PseudoMFLO64, PseudoMTLOHI64)>;

// Loads, stores and other pipe 1 instructions
// ===========================================

def R5900WriteLoad : R5900Write<[R5900Pipe1], 2>;
def R5900WritePipe1 : R5900Write<[R5900Pipe1]>;

def : InstRW<[R5900WriteLoad], (instrs LB, LBu, LH, LHu, LW, LWL, LWR, LD, LDL,
                                LDR, LWu, LB64, LBu64, LH64, LHu64, LW64,
                                LWL64, LWR64)>;
def : InstRW<[R5900WritePipe1], (instrs SB, SH, SW, SWL, SWR, SD, SDL, SDR,
                                 SB64, SH64, SW64, SWL64, SWR64)>;
def : InstRW<[R5900WritePipe1], (instrs PREF, CACHE, SYNC, MFC0, MTC0, DMFC0,
                                 DMTC0, TLBP, TLBR, TLBWI, TLBWR, WAIT, RDHWR,
                                 RDHWR64)>;

// COP2 (VU0 macro mode) loads, stores and moves
def : InstRW<[R5900WriteLoad], (instrs LWC2, LDC2, MFC2, MTC2, DMFC2, DMTC2)>;
def : InstRW<[R5900WritePipe1], (instrs SWC2, SDC2)>;

// Branches and jumps
// ==================

def R5900WriteBranch : R5900Write<[R5900Pipe01]>;

def : InstRW<[R5900WriteBranch], (instrs B, BAL, BAL_BR, BEQ, BNE, BGTZ, BGEZ,
                                  BLEZ, BLTZ, BLTZAL, BGEZAL, BEQL, BNEL,
                                  BGEZL, BGTZL, BLEZL, BLTZL, BGEZALL, BLTZALL,
                                  J, JAL, JALR, JR, JR_HB, JALR_HB,
                                  JALRPseudo, JALRHBPseudo, TAILCALL,
                                  TAILCALLREG, TAILCALLREGHB,
                                  PseudoIndirectBranch,
                                  PseudoIndirectHazardBranch, PseudoReturn,
                                  RetRA, ERET, ERet)>;

def : InstRW<[R5900WriteBranch], (instrs BEQ64, BNE64, BGEZ64, BGTZ64, BLEZ64,
                                  BLTZ64, JALR64, JALR64Pseudo,
                                  JALRHB64Pseudo, JR64, JALR_HB64, JR_HB64,
                                  TAILCALLREG64, TAILCALLREGHB64,
                                  PseudoIndirectBranch64,
                                  PseudoIndirectHazardBranch64,
                                  PseudoReturn64)>;

def : InstRW<[R5900WriteBranch], (instrs BREAK, SYSCALL, TEQ, TEQI, TGE, TGEI,
                                  TGEIU, TGEU, TNE, TNEI, TLT, TLTI, TLTU,
                                  TTLTIU, TRAP, SDBBP)>;

def : InstRW<[R5900WriteBranch], (instrs BC1F, BC1T, BC1FL, BC1TL)>;

// FPU
// ===

def R5900WriteFPU : R5900Write<[R5900Pipe0, R5900C1], 4>;
def R5900WriteFPUDivSqrt : R5900Write<[R5900Pipe0, R5900C1, R5900FDiv], 7,
                                      [1, 2, 7]>;
// lwc1, mtc1, mfc1
def R5900WriteFPUMove : R5900Write<[R5900Pipe1, R5900C1], 2>;
// swc1
def R5900WriteFPUStore : R5900Write<[R5900Pipe1, R5900C1]>;

// MFC1 and SWC1 read their FPR one cycle later than FPU operations.
def R5900ReadFPRLate : SchedReadAdvance<-1>;

def : InstRW<[R5900WriteFPU], (instrs FADD_S, FSUB_S, FMUL_S, FABS_S, FNEG_S,
                               FMOV_S, CVT_S_W, CVT_W_S, TRUNC_W_S, CVT_L_S,
                               CVT_L_D64, PseudoCVT_S_W, FCMP_S32, C_F_S,
                               C_EQ_S, C_OLT_S, C_OLE_S)>;
def : InstRW<[R5900WriteFPUDivSqrt], (instrs FDIV_S, FSQRT_S)>;

def : InstRW<[R5900WriteFPUMove], (instrs LWC1, MTC1, CTC1, CFC1)>;
def : InstRW<[R5900WriteFPUMove, R5900ReadFPRLate], (instrs MFC1)>;
def : InstRW<[R5900WriteFPUStore, R5900ReadFPRLate], (instrs SWC1)>;

} // SchedModel = MipsR5900Model

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
//   - pipe 0: FPU and VU0 macro operations, multiply/divide (MAC0), HI/LO
//     moves and 128-bit multimedia (MMI) operations,
//   - pipe 1: loads, stores, prefetch, FPU and VU0 loads/stores/moves, SYNC,
//     COP0, LZC and the pipe 1 multiply/divide (MAC1),
//   - either pipe: ALU operations and branches.
// All FPU instructions, including the FPU loads, stores and moves, also use
// the single COP1 (C1) slot, so at most one of them issues per cycle. The
// same holds for the VU0 macro instructions and the COP2 (C2) slot.
//
// The latencies are from the EE Core User's Manual, section 2.12, as measured
// on hardware. Measured differences from the manual:
//   - a load result is available to the instruction issued 2 cycles later,
//   - DIV.S and SQRT.S have a latency of 7 and hold C1 for 2 cycles,
//     RSQRT.S a latency of 13,
//   - MFC1 and SWC1 read their FPR one cycle later than FPU operations,
//     QMFC2 and SQC2 their VF register one cycle later than VU0 operations,
//   - a C.cond.S result is available to BC1F/BC1T after 6 cycles,
//   - a multiply-add reads the accumulator 2 cycles late,
//   - LWL/LWR/LDL/LDR read the register they merge into 1 cycle late,
//   - the VU0 Q register result of VDIV and VSQRT is available after 9
//     cycles, of VRSQRT after 15,
//   - a 128-bit MMI operation uses both integer pipes, I0 and I1.
//
//===----------------------------------------------------------------------===//

def MipsR5900Model : SchedMachineModel {
  int IssueWidth = 2;
  int MicroOpBufferSize = 0; // In-order.
  int LoadLatency = 2;
  int MispredictPenalty = 3;
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

// The integer pipes I0 and I1. A 128-bit MMI operation uses both, so it does
// not pair with an ALU, LZC or MAC1 instruction, but it does pair with a load
// or store (measured).
def R5900I0 : ProcResource<1>;
def R5900I1 : ProcResource<1>;
def R5900I01 : ProcResGroup<[R5900I0, R5900I1]>;

// The COP1 slot, shared by all FPU instructions.
def R5900C1 : ProcResource<1>;
// The COP2 (VU0 macro mode) slot.
def R5900C2 : ProcResource<1>;
// Multiply/divide unit for HI/LO.
def R5900MAC0 : ProcResource<1>;
// Multiply/divide unit for HI1/LO1.
def R5900MAC1 : ProcResource<1>;
// FPU divide/square root.
def R5900FDiv : ProcResource<1>;
// VU0 divide/square root (Q register).
def R5900VU0FDiv : ProcResource<1>;
} // BufferSize = 0

// Instructions complete out of order: a short instruction does not wait for
// an earlier long one.
class R5900Write<list<ProcResourceKind> Res, int Lat = 1,
                 list<int> Release = []> : SchedWriteRes<Res> {
  let Latency = Lat;
  let ReleaseAtCycles = Release;
  let RetireOOO = 1;
}

// The latency of an additional result, such as the implicit HI and LO of a
// multiply; the resources are used by the instruction's first write.
class R5900WriteLat<int Lat> : SchedWriteRes<[]> {
  let Latency = Lat;
  let NumMicroOps = 0;
}

// MFC1, SWC1, QMFC2 and SQC2 read their FPR or VF register one cycle later
// than FPU and VU0 operations.
def R5900ReadFPRLate : SchedReadAdvance<-1>;

// ALU
// ===

def R5900WriteALU : R5900Write<[R5900Pipe01, R5900I01]>;

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
def R5900WriteMulHILO : R5900WriteLat<4>;
def R5900WriteDivHILO : R5900WriteLat<37>;

// A multiply-add reads the accumulator of a previous multiply 2 cycles after
// it issued, its GPR result is available after 4 cycles.
def R5900ReadAcc : SchedReadAdvance<2, [R5900WriteMul, R5900WriteMulHILO]>;

// HI and LO are implicit defs, so they each need their own write.
def : InstRW<[R5900WriteMul, R5900WriteMulHILO],
             (instrs MULT, MULTu, PseudoMULT, PseudoMULTu,
              PseudoR5900MulMulAdd)>;
def : InstRW<[R5900WriteMul, R5900WriteMulHILO, R5900WriteMulHILO],
             (instrs R5900_MULT, R5900_MULTU)>;
def : InstRW<[R5900WriteMul, R5900WriteMulHILO, R5900WriteMulHILO,
              ReadDefault, ReadDefault, R5900ReadAcc, R5900ReadAcc],
             (instrs R5900_MADD, R5900_MADDU)>;
def : InstRW<[R5900WriteDiv, R5900WriteDivHILO],
             (instrs SDIV, UDIV, PseudoSDIV, PseudoUDIV)>;
def : InstRW<[R5900WriteMoveHILO], (instrs MFHI, MFLO, MTHI, MTLO, PseudoMFHI,
                                    PseudoMFLO, PseudoMTLOHI, MFHI64, MFLO64,
                                    MTHI64, MTLO64, PseudoMFHI64,
                                    PseudoMFLO64, PseudoMTLOHI64)>;

// Multiply and divide (MAC1)
// ==========================

def R5900WriteMul1 : R5900Write<[R5900Pipe1, R5900I1, R5900MAC1], 4,
                                [1, 1, 2]>;
def R5900WriteDiv1 : R5900Write<[R5900Pipe1, R5900I1, R5900MAC1], 37,
                                [1, 1, 37]>;
def R5900WriteMoveHILO1 : R5900Write<[R5900Pipe1]>;
def R5900WriteMul1HILO : R5900WriteLat<4>;
def R5900WriteDiv1HILO : R5900WriteLat<37>;

def R5900ReadAcc1 : SchedReadAdvance<2, [R5900WriteMul1,
                                         R5900WriteMul1HILO]>;

def : InstRW<[R5900WriteMul1, R5900WriteMul1HILO, R5900WriteMul1HILO],
             (instrs MULT1, MULTU1)>;
def : InstRW<[R5900WriteMul1, R5900WriteMul1HILO, R5900WriteMul1HILO,
              ReadDefault, ReadDefault, R5900ReadAcc1, R5900ReadAcc1],
             (instrs MADD1, MADDU1)>;
def : InstRW<[R5900WriteDiv1, R5900WriteDiv1HILO],
             (instrs DIV1, DIVU1, PseudoSDIV1, PseudoUDIV1)>;
def : InstRW<[R5900WriteMoveHILO1], (instrs MFHI1, MFLO1, MTHI1, MTLO1,
                                     PseudoMFHI1, PseudoMFLO1)>;

// Loads, stores and other pipe 1 instructions
// ===========================================

def R5900WriteLoad : R5900Write<[R5900Pipe1], 2>;
def R5900WritePipe1 : R5900Write<[R5900Pipe1]>;

def : InstRW<[R5900WriteLoad], (instrs LB, LBu, LH, LHu, LW, LD, LWu, LB64,
                                LBu64, LH64, LHu64, LW64, LQ)>;
// The register an unaligned load merges into is read 1 cycle late, so ldl
// followed by ldr of the same register takes 2 cycles. The merged register is
// read 1 of the MachineScheduler, which only counts register operands, and
// read 2 of llvm-mca, which also counts the offset.
def R5900ReadMerge : SchedReadAdvance<1, [R5900WriteLoad]>;
def : InstRW<[R5900WriteLoad, ReadDefault, R5900ReadMerge, R5900ReadMerge],
             (instrs LWL, LWR, LDL, LDR, LWL64, LWR64)>;
def : InstRW<[R5900WritePipe1], (instrs SB, SH, SW, SWL, SWR, SD, SDL, SDR,
                                 SB64, SH64, SW64, SWL64, SWR64, SQ)>;
def : InstRW<[R5900WritePipe1], (instrs PREF, CACHE, SYNC, MFC0, MTC0, DMFC0,
                                 DMTC0, TLBP, TLBR, TLBWI, TLBWR, WAIT, RDHWR,
                                 RDHWR64, EI_R5900, DI_R5900, MFBPC, MFIAB,
                                 MFIABM, MFDAB, MFDABM, MFDVB, MFDVBM, MTBPC,
                                 MTIAB, MTIABM, MTDAB, MTDABM, MTDVB,
                                 MTDVBM)>;

// COP2 (VU0 macro mode) loads, stores and moves
def : InstRW<[R5900WriteLoad], (instrs LWC2, LDC2, MFC2, MTC2, DMFC2, DMTC2)>;
def : InstRW<[R5900WritePipe1], (instrs SWC2, SDC2)>;

// A transfer to VU0 holds the C2 slot for one cycle: an independent VU0
// instruction or transfer issues in the next cycle (measured).
def R5900WriteVU0MoveIn : R5900Write<[R5900Pipe1, R5900C2], 2>;
def R5900WriteVU0MoveOut : R5900Write<[R5900Pipe1, R5900C2], 2>;
def R5900WriteVU0Store : R5900Write<[R5900Pipe1, R5900C2]>;

def : InstRW<[R5900WriteVU0MoveIn], (instrs LQC2, CTC2, QMTC2, CTC2_I, QMTC2_I)>;
def : InstRW<[R5900WriteVU0MoveOut], (instrs CFC2, CFC2_I)>;
def : InstRW<[R5900WriteVU0MoveOut, R5900ReadFPRLate], (instrs QMFC2, QMFC2_I)>;
def : InstRW<[R5900WriteVU0Store, R5900ReadFPRLate], (instrs SQC2)>;

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

def : InstRW<[R5900WriteBranch], (instrs BC1F, BC1T, BC1FL, BC1TL, BC0F,
                                  BC0T)>;

// FPU
// ===

def R5900WriteFPU : R5900Write<[R5900Pipe0, R5900C1], 4>;
def R5900WriteFPUDivSqrt : R5900Write<[R5900Pipe0, R5900C1, R5900FDiv], 7,
                                      [1, 2, 7]>;
// lwc1, mtc1, mfc1
def R5900WriteFPUMove : R5900Write<[R5900Pipe1, R5900C1], 2>;
// swc1
def R5900WriteFPUStore : R5900Write<[R5900Pipe1, R5900C1]>;

def : InstRW<[R5900WriteFPU], (instrs FADD_S, FSUB_S, FMUL_S, FABS_S, FNEG_S,
                               FMOV_S, CVT_S_W, CVT_W_S, TRUNC_W_S, CVT_L_S,
                               CVT_L_D64, PseudoCVT_S_W)>;
// A compare result is available to BC1F/BC1T after 6 cycles.
def R5900WriteFPUCmp : R5900Write<[R5900Pipe0, R5900C1], 6>;
def : InstRW<[R5900WriteFPUCmp], (instrs FCMP_S32, C_F_S, C_EQ_S, C_OLT_S,
                                  C_OLE_S)>;
def : InstRW<[R5900WriteFPUDivSqrt], (instrs FDIV_S, FSQRT_S, R5900_SQRT_S)>;
def : InstRW<[R5900WriteFPU], (instrs R5900_MAX_S, R5900_MIN_S)>;

def R5900WriteFPURsqrt : R5900Write<[R5900Pipe0, R5900C1, R5900FDiv], 13,
                                    [1, 2, 13]>;
def : InstRW<[R5900WriteFPURsqrt], (instrs R5900_RSQRT_S)>;

// FPU accumulator: an ACC result is available to the next ACC operation
// after 1 cycle, an FPR result after 4 cycles.
def R5900WriteFPUAcc : R5900Write<[R5900Pipe0, R5900C1]>;

def : InstRW<[R5900WriteFPUAcc], (instrs ADDA_S, SUBA_S, MULA_S, MADDA_S,
                                  MSUBA_S)>;
def : InstRW<[R5900WriteFPU], (instrs R5900_MADD_S, R5900_MSUB_S)>;

def : InstRW<[R5900WriteFPUMove], (instrs LWC1, MTC1, CTC1, CFC1)>;
def : InstRW<[R5900WriteFPUMove, R5900ReadFPRLate], (instrs MFC1)>;
def : InstRW<[R5900WriteFPUStore, R5900ReadFPRLate], (instrs SWC1)>;

// VU0 macro mode
// ==============

// As for the FPU accumulator, an ACC result is available to the next ACC
// operation after 1 cycle (VU Users Manual, section 5.4.2).
def R5900WriteVU0 : R5900Write<[R5900Pipe0, R5900C2], 4>;
def R5900WriteVU0Acc : R5900Write<[R5900Pipe0, R5900C2]>;

// VDIV, VSQRT and VRSQRT issue in either pipe, also next to an FPU or VU0
// operation, and hold only the divider: VDIV and VSQRT for 7 cycles. The Q
// result is available to VWAITQ after 9 cycles, or 15 for VRSQRT, and the
// instruction after VWAITQ issues in the next cycle (measured).
def R5900WriteVU0Div : R5900Write<[R5900Pipe01, R5900VU0FDiv], 9, [1, 7]>;
def R5900WriteVU0Rsqrt : R5900Write<[R5900Pipe01, R5900VU0FDiv], 15, [1, 15]>;
// VWAITQ stalls the whole core until Q is available: nothing issues with it.
def R5900WriteVU0WaitQ : R5900Write<[R5900Pipe0, R5900Pipe1, R5900C2]>;
// Reads of Q are not interlocked: an instruction that reads Q before VWAITQ
// gets the previous Q (measured), so it does not wait for the division.
def R5900ReadVU0Q : SchedReadAdvance<15, [R5900WriteVU0Div,
                                          R5900WriteVU0Rsqrt]>;

def : InstRW<[R5900WriteVU0], (instrs VADD, VSUB, VMUL, VMADD, VMSUB, VADDm,
                               VSUBm, VMULm, VMOVE, VMOVEm, VMR32, VABS, VMAX,
                               VMINI, VCLIPW, VOPMSUB, VFTOI0, VFTOI4,
                               VFTOI12, VFTOI15, VITOF0, VITOF4, VITOF12,
                               VITOF15)>;
// The advance applies only to the divider results, so it is given for every
// use: llvm-mca counts the dest mask as a use, the MachineScheduler does not.
def : InstRW<[R5900WriteVU0, R5900ReadVU0Q, R5900ReadVU0Q, R5900ReadVU0Q],
             (instrs VADDQ, VSUBQ, VMULQ, VMADDQ, VMSUBQ)>;
def : InstRW<[R5900WriteVU0],
             (instregex "^V(ADD|SUB|MUL|MADD|MSUB|MAX|MINI)bc[xyzw]$")>;
def : InstRW<[R5900WriteVU0Acc], (instrs VADDA, VSUBA, VMULA, VMADDA, VMSUBA,
                                  VOPMULA, VNOP)>;
def : InstRW<[R5900WriteVU0WaitQ], (instrs VWAITQ)>;
def : InstRW<[R5900WriteVU0Div], (instrs VDIV, VSQRT)>;
def : InstRW<[R5900WriteVU0Rsqrt], (instrs VRSQRT)>;
def : InstRW<[R5900WriteVU0Acc],
             (instregex "^V(ADD|SUB|MUL|MADD|MSUB)Abc[xyzw]$")>;
// The code generation forms with ACC as a value (MipsVU0InstrInfo.td).
def : InstRW<[R5900WriteVU0Acc], (instrs VMULA_V, VMADDA_V, VMSUBA_V,
                                  VOPMULA_V)>;
def : InstRW<[R5900WriteVU0Acc], (instregex "^V(MUL|MADD|MSUB)Abc[xyzw]_V$")>;
def : InstRW<[R5900WriteVU0], (instrs VMADD_V, VMSUB_V, VOPMSUB_V)>;
def : InstRW<[R5900WriteVU0], (instregex "^V(MADD|MSUB)bc[xyzw]_V$")>;
def : InstRW<[R5900WriteVU0], (instregex "^V(ADD|MUL)bc[xyzw]m$")>;
def : InstRW<[R5900WriteVU0Div], (instrs VDIV_V)>;
def : InstRW<[R5900WriteVU0WaitQ], (instrs VWAITQ_V)>;
def : InstRW<[R5900WriteVU0], (instrs VADDQ_V, VSUBQ_V, VMULQ_V, VADDQm)>;

// Multimedia (MMI)
// ================

// The 128-bit multimedia instructions use both I0 and I1, but issue in pipe 0.
def R5900WriteMMI : R5900Write<[R5900Pipe0, R5900I0, R5900I1]>;
def R5900WriteMMIMul : R5900Write<[R5900Pipe0, R5900I0, R5900I1, R5900MAC0,
                                   R5900MAC1], 4, [1, 1, 1, 2, 2]>;
def R5900WriteMMIDiv : R5900Write<[R5900Pipe0, R5900I0, R5900I1, R5900MAC0,
                                   R5900MAC1], 37, [1, 1, 1, 37, 37]>;
def R5900WriteMMIMulHILO : R5900WriteLat<4>;
def R5900WriteMMIDivHILO : R5900WriteLat<37>;
def R5900WriteLZC : R5900Write<[R5900Pipe1, R5900I1]>;
// Move to/from SA
def R5900WriteSA : R5900Write<[R5900Pipe0]>;

def : InstRW<[R5900WriteSA], (instrs MFSA, MTSA, MTSAB, MTSAH)>;

def : InstRW<[R5900WriteMMI], (instrs PADDW, PSUBW, PADDH, PSUBH, PADDB, PSUBB,
                               PAND, POR, PXOR, PNOR, PMAXW, PMINW, PMAXH,
                               PMINH, PABSW, PABSH, PSLLW, PSRLW, PSRAW, PSLLH,
                               PSRLH, PSRAH, PSLLVW, PSRLVW, PSRAVW, PCGTW,
                               PCEQW, PCGTH, PCEQH, PCGTB, PCEQB, PADDSW,
                               PSUBSW, PADDSH, PSUBSH, PADDSB, PSUBSB, PADDUW,
                               PSUBUW, PADDUH, PSUBUH, PADDUB, PSUBUB, PADSBH,
                               PEXTLW, PEXTLH, PEXTLB, PEXTUW, PEXTUH, PEXTUB,
                               PPACW, PPACH, PPACB, PCPYLD, PCPYUD, PCPYH,
                               PINTH, PINTEH, PEXEH, PEXEW, PEXCH, PEXCW,
                               PREVH, PEXT5, PPAC5, PROT3W, QFSRV, PMFHI,
                               PMFLO, PMTHI, PMTLO, PMFHL_LW, PMFHL_UW,
                               PMFHL_SLW, PMFHL_LH, PMFHL_SH, PMTHL_LW,
                               PABSW_S32,
                               PMAXW_S32, PMINW_S32, PADDSW_S32, PSUBSW_S32,
                               PADDUW_S32, PSUBUW_S32)>;
def R5900ReadMMIAcc : SchedReadAdvance<2, [R5900WriteMMIMul,
                                             R5900WriteMMIMulHILO]>;

// Writes for rd and the implicit HI and LO (and their upper halves).
def : InstRW<[R5900WriteMMIMul, R5900WriteMMIMulHILO,
              R5900WriteMMIMulHILO],
             (instrs PMULTW, PMULTUW)>;
def : InstRW<[R5900WriteMMIMul, R5900WriteMMIMulHILO, R5900WriteMMIMulHILO,
              ReadDefault, ReadDefault, R5900ReadMMIAcc, R5900ReadMMIAcc],
             (instrs PMADDW, PMSUBW, PMADDUW)>;
def : InstRW<[R5900WriteMMIMul, R5900WriteMMIMulHILO, R5900WriteMMIMulHILO,
              R5900WriteMMIMulHILO, R5900WriteMMIMulHILO],
             (instrs PMULTH, PMADDH, PMSUBH, PHMADH, PHMSBH)>;
// HI and LO as one value (MipsR5900LoopAcc): a PMADDH reads the sum of the
// previous one 2 cycles late (measured: PMADDH to PMADDH 2 cycles).
def : InstRW<[R5900WriteMMIMul, R5900WriteMMIMulHILO, ReadDefault,
              ReadDefault, R5900ReadMMIAcc],
             (instrs PseudoPMADDH)>;
def : InstRW<[R5900WriteMMI], (instrs PseudoPMTHILO, PseudoPMFLO,
                                      PseudoPMFHI)>;
def : InstRW<[R5900WriteMMIDiv, R5900WriteMMIDivHILO, R5900WriteMMIDivHILO,
              R5900WriteMMIDivHILO], (instrs PDIVW, PDIVUW, PDIVBW)>;
def : InstRW<[R5900WriteLZC], (instrs PLZCW, PLZCW_S32)>;

} // SchedModel = MipsR5900Model

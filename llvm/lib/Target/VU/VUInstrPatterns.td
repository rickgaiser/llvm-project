//===-- VUInstrPatterns.td - VU selection patterns ---------*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Scalar floats live in the x field of a VF register; the other fields are
// undefined. VF0 is (0, 0, 0, 1), so its x, y and z fields are a free zero.
//
// Instructions are selected with a full ".xyzw" destination mask unless only
// some fields are written on purpose.
//
//===----------------------------------------------------------------------===//

//===----------------------------------------------------------------------===//
// Target nodes
//===----------------------------------------------------------------------===//

def VUret : SDNode<"VUISD::RET", SDTNone,
                   [SDNPHasChain, SDNPOptInGlue, SDNPVariadic]>;

// Broadcast the x field of a VF register to all four fields. Combined into
// VUISD::BCAST before selection.
def SDT_VUSplat : SDTypeProfile<1, 1, [SDTCisVT<0, v4f32>, SDTCisVT<1, f32>]>;
def VUsplat : SDNode<"VUISD::SPLAT", SDT_VUSplat>;

// Broadcast field $f (x = 0 ... w = 3) of a VF register (v4f32 or f32).
def SDT_VUBcast : SDTypeProfile<1, 2, [SDTCisVT<0, v4f32>, SDTCisFP<1>,
                                       SDTCisVT<2, i32>]>;
def VUbcast : SDNode<"VUISD::BCAST", SDT_VUBcast>;

// A sum of products computed through ACC: MULA, MADDA/MSUBA..., MADD/MSUB.
// The operands are (a, b, field, negate) for each term; see
// VUTargetLowering::combineAccSum. Selected in VUDAGToDAGISel.
def VUaccsum : SDNode<"VUISD::ACCSUM", SDTypeProfile<1, -1, [SDTCisVT<0, v4f32>]>>;

// Fixed-point conversions with n fraction bits (4, 12 or 15):
// FTOI = fp_to_sint (v * 2^n), ITOF = sint_to_fp (v) * 2^-n.
def VUftoi : SDNode<"VUISD::FTOI", SDTypeProfile<1, 2, [SDTCisVT<0, v4i32>,
                                   SDTCisVT<1, v4f32>, SDTCisVT<2, i32>]>>;
def VUitof : SDNode<"VUISD::ITOF", SDTypeProfile<1, 2, [SDTCisVT<0, v4f32>,
                                   SDTCisVT<1, v4i32>, SDTCisVT<2, i32>]>>;

// Insert a VI register, sign-extended to 32 bits, into field $f of an
// integer vector.
def VUinsertvi : SDNode<"VUISD::INSERT_VI", SDTypeProfile<1, 3, [
                        SDTCisVT<0, v4i32>, SDTCisVT<1, v4i32>,
                        SDTCisVT<2, i16>, SDTCisVT<3, i32>]>>;

//===----------------------------------------------------------------------===//
// Pseudo instructions
//===----------------------------------------------------------------------===//

class VUPseudo<dag outs, dag ins, list<dag> pattern = [],
               InstrItinClass itin = NoItinerary>
    : VUInst<outs, ins, ""> {
  let isPseudo = 1;
  let isCodeGenOnly = 1;
  let Pattern = pattern;
  // The issue slot of the expansion, for the pipeliner.
  let Itinerary = itin;
}

// End of the micro program: a word with the E bit and its delay slot.
// Expanded by the asm printer.
let isReturn = 1, isTerminator = 1, isBarrier = 1, hasSideEffects = 1 in
def PROGRAM_END : VUPseudo<(outs), (ins), [(VUret)]>;

// A NOP in the delay slot of a branch.
let isTerminator = 1, isCodeGenOnly = 1 in
def DSNOP : VUUpperExt<0x2f, (outs), (ins), "nop"> {
  let Inst{24-11} = 0;
}

// Marks a word with instructions in the delay slot of a branch, which makes
// the word a terminator. It has no encoding.
let isTerminator = 1 in
def DELAY_SLOT : VUPseudo<(outs), (ins)>;

// The pseudos below are expanded after register allocation
// (VUInstrInfo::expandPostRAPseudo).

// f32 constant: LOI c; ADDi.x d, vf0, i
let Defs = [I], isReMaterializable = 1, isAsCheapAsAMove = 1 in
def LOADF32 : VUPseudo<(outs VF:$d), (ins f32imm:$c),
                       [(set (f32 VF:$d), (f32 fpimm:$c))], IIC_VULoi>;

// Splat field f of s: ADD<f>.xyz d, vf0, s; MUL<f>.w d, vf0, s
def SPLAT : VUPseudo<(outs VF:$d), (ins VF:$s, i32imm:$f), [], IIC_VUUpper2>;
def : Pat<(VUbcast (v4f32 VF:$s), timm:$f), (SPLAT VF:$s, timm:$f)>;
def : Pat<(VUbcast (f32 VF:$s), timm:$f), (SPLAT VF:$s, timm:$f)>;

// Insert field sf of s into field f of src: ADD<sf>.<f> d, vf0, s for x, y
// and z, MUL<sf>.w d, vf0, s for w. A scalar is field x; a field of another
// vector is inserted from its place, without moving it to x first.
let Constraints = "$d = $src" in
def INSERT_FIELD : VUPseudo<(outs VF:$d),
                            (ins VF:$src, VF:$s, i16imm:$f, i16imm:$sf), [],
                            IIC_VUUpper>;
def : Pat<(vector_insert (v4f32 VF:$src), (f32 VF:$s), imm:$f),
          (INSERT_FIELD VF:$src, VF:$s, imm:$f, 0)>;
let AddedComplexity = 1 in
def : Pat<(vector_insert (v4f32 VF:$src),
                         (f32 (vector_extract (v4f32 VF:$v), imm:$sf)), imm:$f),
          (INSERT_FIELD VF:$src, VF:$v, imm:$f, imm:$sf)>;

// Insert a VI register into one field: MFIR.<field> d, s
let Constraints = "$d = $src" in
def INSERT_VI : VUPseudo<(outs VF:$d), (ins VF:$src, VI:$s, i32imm:$f),
    [(set (v4i32 VF:$d), (VUinsertvi (v4i32 VF:$src), (i16 VI:$s), timm:$f))],
    IIC_VULower>;

// Negation: LOI -1.0; MULi.xyzw d, s, i
let Defs = [I] in
def FNEG : VUPseudo<(outs VF:$d), (ins VF:$s), [], IIC_VULoi>;
def : Pat<(fneg (v4f32 VF:$s)), (FNEG VF:$s)>;
def : Pat<(fneg (f32 VF:$s)), (FNEG VF:$s)>;

// Division and square root through Q are selected in VUDAGToDAGISel:
//   DIV q, a.f, b.f / SQRT q, a.f; ADDq.x d, vf0, q
// VUFoldQ folds the ADDq into the instructions that use the result, and
// VUHazards inserts WAITQ where Q is read too early.

//===----------------------------------------------------------------------===//
// Floating-point arithmetic
//===----------------------------------------------------------------------===//

foreach VT = [v4f32, f32] in {
  def : Pat<(fadd (VT VF:$a), (VT VF:$b)), (ADD 15, VF:$a, VF:$b)>;
  def : Pat<(fsub (VT VF:$a), (VT VF:$b)), (SUB 15, VF:$a, VF:$b)>;
  def : Pat<(fmul (VT VF:$a), (VT VF:$b)), (MUL 15, VF:$a, VF:$b)>;
  def : Pat<(fmaxnum (VT VF:$a), (VT VF:$b)), (MAX 15, VF:$a, VF:$b)>;
  def : Pat<(fminnum (VT VF:$a), (VT VF:$b)), (MINI 15, VF:$a, VF:$b)>;
  def : Pat<(fabs (VT VF:$a)), (ABS 15, VF:$a)>;
  def : Pat<(fma (VT VF:$a), (VT VF:$b), (VT VF:$c)),
            (ADD 15, (MUL 15, VF:$a, VF:$b), VF:$c)>;
}

// Broadcast forms: fs op ft.<field>
foreach F = Fields in
  def FieldImm # F.Value : TImmLeaf<i32, "return Imm == " # F.Value # ";">;

foreach F = Fields in {
  defvar Imm = !cast<TImmLeaf>("FieldImm" # F.Value);
  foreach VT = [v4f32, f32] in {
    def : Pat<(fmul (v4f32 VF:$a), (VUbcast (VT VF:$b), Imm)),
              (!cast<Instruction>("MUL" # F.Name) 15, VF:$a, VF:$b)>;
    def : Pat<(fadd (v4f32 VF:$a), (VUbcast (VT VF:$b), Imm)),
              (!cast<Instruction>("ADD" # F.Name) 15, VF:$a, VF:$b)>;
    def : Pat<(fsub (v4f32 VF:$a), (VUbcast (VT VF:$b), Imm)),
              (!cast<Instruction>("SUB" # F.Name) 15, VF:$a, VF:$b)>;
    def : Pat<(fmaxnum (v4f32 VF:$a), (VUbcast (VT VF:$b), Imm)),
              (!cast<Instruction>("MAX" # F.Name) 15, VF:$a, VF:$b)>;
    def : Pat<(fminnum (v4f32 VF:$a), (VUbcast (VT VF:$b), Imm)),
              (!cast<Instruction>("MINI" # F.Name) 15, VF:$a, VF:$b)>;
  }
}

// All-zero vector: SUB.xyzw d, vf0, vf0
def : Pat<(v4f32 immAllZerosV), (SUB 15, (v4f32 VF0), (v4f32 VF0))>;
def fpimm0 : FPImmLeaf<f32, [{ return Imm.isExactlyValue(+0.0); }]>;
def : Pat<(f32 fpimm0), (SUB 15, (f32 VF0), (f32 VF0))>;

//===----------------------------------------------------------------------===//
// Vector fields
//===----------------------------------------------------------------------===//

// The x field needs no instruction; the others are moved to x:
// ADD<f>.x d, vf0, v
def : Pat<(f32 (extractelt (v4f32 VF:$v), 0)), (COPY_TO_REGCLASS VF:$v, VF)>;
def : Pat<(f32 (extractelt (v4f32 VF:$v), 1)), (ADDy 8, (f32 VF0), VF:$v)>;
def : Pat<(f32 (extractelt (v4f32 VF:$v), 2)), (ADDz 8, (f32 VF0), VF:$v)>;
def : Pat<(f32 (extractelt (v4f32 VF:$v), 3)), (ADDw 8, (f32 VF0), VF:$v)>;

// Integer and float vectors share the VF registers.
def : Pat<(v4i32 (bitconvert (v4f32 VF:$v))), (v4i32 VF:$v)>;
def : Pat<(v4f32 (bitconvert (v4i32 VF:$v))), (v4f32 VF:$v)>;

//===----------------------------------------------------------------------===//
// Conversions
//===----------------------------------------------------------------------===//

def : Pat<(v4f32 (sint_to_fp (v4i32 VF:$v))), (ITOF0 15, VF:$v)>;
def : Pat<(v4i32 (fp_to_sint (v4f32 VF:$v))), (FTOI0 15, VF:$v)>;

foreach N = [4, 12, 15] in {
  defvar Bits = TImmLeaf<i32, "return Imm == " # N # ";">;
  def : Pat<(VUftoi (v4f32 VF:$v), Bits),
            (!cast<Instruction>("FTOI" # N) 15, VF:$v)>;
  def : Pat<(VUitof (v4i32 VF:$v), Bits),
            (!cast<Instruction>("ITOF" # N) 15, VF:$v)>;
}

// i16 <-> f32: through the x field. MFIR sign-extends to 32 bits.
def : Pat<(f32 (sint_to_fp (i16 VI:$v))), (ITOF0 8, (MFIR 8, VI:$v))>;
def : Pat<(i16 (fp_to_sint (f32 VF:$v))), (MTIR (FTOI0 8, VF:$v), 0)>;

//===----------------------------------------------------------------------===//
// Integer arithmetic (16-bit VI registers)
//===----------------------------------------------------------------------===//

def simm5_imm : ImmLeaf<i16, [{ return isInt<5>(Imm); }]>;
def uimm15_imm : ImmLeaf<i16, [{ return isUInt<15>(Imm); }]>;
def nuimm15_imm : ImmLeaf<i16, [{ return Imm < 0 && isUInt<15>(-Imm); }]>;
def NegImm : SDNodeXForm<imm, [{
  return CurDAG->getTargetConstant(-N->getSExtValue(), SDLoc(N), MVT::i16);
}]>;

def : Pat<(i16 uimm15_imm:$c), (IADDIU (i16 VI0), uimm15_imm:$c)>;
def : Pat<(i16 nuimm15_imm:$c), (ISUBIU (i16 VI0), (NegImm $c))>;
def : Pat<(i16 -32768), (ISUBIU (ISUBIU (i16 VI0), 32767), 1)>;

def : Pat<(add VI:$a, simm5_imm:$c), (IADDI VI:$a, simm5_imm:$c)>;
def : Pat<(add VI:$a, uimm15_imm:$c), (IADDIU VI:$a, uimm15_imm:$c)>;
def : Pat<(add VI:$a, nuimm15_imm:$c), (ISUBIU VI:$a, (NegImm $c))>;
def : Pat<(sub VI:$a, uimm15_imm:$c), (ISUBIU VI:$a, uimm15_imm:$c)>;

def : Pat<(add VI:$a, VI:$b), (IADD VI:$a, VI:$b)>;
def : Pat<(sub VI:$a, VI:$b), (ISUB VI:$a, VI:$b)>;
def : Pat<(sub 0, VI:$b), (ISUB (i16 VI0), VI:$b)>;
def : Pat<(and VI:$a, VI:$b), (IAND VI:$a, VI:$b)>;
def : Pat<(or VI:$a, VI:$b), (IOR VI:$a, VI:$b)>;

// There is no multiplier or shifter: a multiplication or left shift by a
// constant is lowered to additions in VUTargetLowering::lowerMUL.

//===----------------------------------------------------------------------===//
// Memory (llvm.vu.* intrinsics, quadword addresses)
//===----------------------------------------------------------------------===//

def simm11_imm : ImmLeaf<i16, [{ return isInt<11>(Imm); }]>;

// Field index (x = 0 ... w = 3) to destination mask.
def FieldToMask : SDNodeXForm<timm, [{
  return CurDAG->getTargetConstant(8 >> N->getZExtValue(), SDLoc(N),
                                   MVT::i32);
}]>;

// Addresses are imm11(VI): base + offset, offset only (with VI0), or base
// only (with offset 0).
multiclass LoadQPats<Instruction I> {
  def : Pat<(v4f32 (int_vu_lq (add VI:$b, simm11_imm:$o))),
            (I 15, simm11_imm:$o, VI:$b)>;
  def : Pat<(v4f32 (int_vu_lq simm11_imm:$o)), (I 15, simm11_imm:$o, (i16 VI0))>;
  def : Pat<(v4f32 (int_vu_lq VI:$b)), (I 15, 0, VI:$b)>;
}
defm : LoadQPats<LQ>;

def : Pat<(int_vu_sq (v4f32 VF:$v), (add VI:$b, simm11_imm:$o)),
          (SQ 15, VF:$v, simm11_imm:$o, VI:$b)>;
def : Pat<(int_vu_sq (v4f32 VF:$v), simm11_imm:$o),
          (SQ 15, VF:$v, simm11_imm:$o, (i16 VI0))>;
def : Pat<(int_vu_sq (v4f32 VF:$v), VI:$b), (SQ 15, VF:$v, 0, VI:$b)>;

def : Pat<(int_vu_sq_mask (v4f32 VF:$v), (add VI:$b, simm11_imm:$o), (i32 timm:$m)),
          (SQ (i32 timm:$m), VF:$v, simm11_imm:$o, VI:$b)>;
def : Pat<(int_vu_sq_mask (v4f32 VF:$v), simm11_imm:$o, (i32 timm:$m)),
          (SQ (i32 timm:$m), VF:$v, simm11_imm:$o, (i16 VI0))>;
def : Pat<(int_vu_sq_mask (v4f32 VF:$v), VI:$b, (i32 timm:$m)),
          (SQ (i32 timm:$m), VF:$v, 0, VI:$b)>;

def : Pat<(i16 (int_vu_ilw (add VI:$b, simm11_imm:$o), (i32 timm:$f))),
          (ILW (FieldToMask (i32 timm:$f)), simm11_imm:$o, VI:$b)>;
def : Pat<(i16 (int_vu_ilw simm11_imm:$o, (i32 timm:$f))),
          (ILW (FieldToMask (i32 timm:$f)), simm11_imm:$o, (i16 VI0))>;
def : Pat<(i16 (int_vu_ilw VI:$b, (i32 timm:$f))),
          (ILW (FieldToMask (i32 timm:$f)), 0, VI:$b)>;

def : Pat<(int_vu_isw (i16 VI:$v), (add VI:$b, simm11_imm:$o), (i32 timm:$f)),
          (ISW (FieldToMask (i32 timm:$f)), VI:$v, simm11_imm:$o, VI:$b)>;
def : Pat<(int_vu_isw (i16 VI:$v), simm11_imm:$o, (i32 timm:$f)),
          (ISW (FieldToMask (i32 timm:$f)), VI:$v, simm11_imm:$o, (i16 VI0))>;
def : Pat<(int_vu_isw (i16 VI:$v), VI:$b, (i32 timm:$f)),
          (ISW (FieldToMask (i32 timm:$f)), VI:$v, 0, VI:$b)>;

def : Pat<(i16 (int_vu_xitop)), (XITOP)>;
let Predicates = [HasVU1] in {
def : Pat<(i16 (int_vu_xtop)), (XTOP)>;
def : Pat<(int_vu_xgkick (i16 VI:$a)), (XGKICK VI:$a)>;
}

//===----------------------------------------------------------------------===//
// Branches
//===----------------------------------------------------------------------===//

def : Pat<(br bb:$t), (B bb:$t)>;

// Comparisons with zero use VI0.
def : Pat<(brcond (i16 (setcc VI:$a, (i16 0), SETEQ)), bb:$t),
          (IBEQ VI:$a, (i16 VI0), bb:$t)>;
def : Pat<(brcond (i16 (setcc VI:$a, (i16 0), SETNE)), bb:$t),
          (IBNE VI:$a, (i16 VI0), bb:$t)>;
def : Pat<(brcond (i16 (setcc VI:$a, VI:$b, SETEQ)), bb:$t),
          (IBEQ VI:$a, VI:$b, bb:$t)>;
def : Pat<(brcond (i16 (setcc VI:$a, VI:$b, SETNE)), bb:$t),
          (IBNE VI:$a, VI:$b, bb:$t)>;

// Signed comparisons against zero, and of a difference otherwise.
multiclass BranchCmpZero<CondCode CC, Instruction I> {
  def : Pat<(brcond (i16 (setcc VI:$a, (i16 0), CC)), bb:$t), (I VI:$a, bb:$t)>;
  def : Pat<(brcond (i16 (setcc VI:$a, VI:$b, CC)), bb:$t),
            (I (ISUB VI:$a, VI:$b), bb:$t)>;
}
defm : BranchCmpZero<SETLT, IBLTZ>;
defm : BranchCmpZero<SETGT, IBGTZ>;
defm : BranchCmpZero<SETLE, IBLEZ>;
defm : BranchCmpZero<SETGE, IBGEZ>;

def : Pat<(brcond (i16 VI:$c), bb:$t), (IBNE VI:$c, (i16 VI0), bb:$t)>;

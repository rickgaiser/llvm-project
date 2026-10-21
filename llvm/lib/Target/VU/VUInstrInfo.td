//===-- VUInstrInfo.td - VU micro instructions -------------*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Encodings follow the VU User's Manual, chapter 4 (micro mode instruction
// reference).
//
//===----------------------------------------------------------------------===//

include "VUInstrFormats.td"

//===----------------------------------------------------------------------===//
// Operands
//===----------------------------------------------------------------------===//

// Destination field mask written as a mnemonic suffix: ".xyzw" etc.
// x is bit 3, y bit 2, z bit 1 and w bit 0.
def DestAsmOperand : AsmOperandClass {
  let Name = "Dest";
  let RenderMethod = "addDestOperands";
}
def dest : Operand<i32> {
  let ParserMatchClass = DestAsmOperand;
  let PrintMethod = "printDest";
}

// A destination with exactly one field (ILW, ISW, ILWR, ISWR).
def DestSingleAsmOperand : AsmOperandClass {
  let Name = "DestSingle";
  let RenderMethod = "addDestOperands";
  let DiagnosticString = "destination must be a single field (x, y, z or w)";
}
def dest1 : Operand<i32> {
  let ParserMatchClass = DestSingleAsmOperand;
  let PrintMethod = "printDest";
}

// A single field written as a register suffix: "vf1x". x=0, y=1, z=2, w=3.
def FieldAsmOperand : AsmOperandClass {
  let Name = "Field";
  let RenderMethod = "addFieldOperands";
  let DiagnosticString = "expected a register with a field suffix (x, y, z or w)";
}
def vufield : Operand<i32> {
  let ParserMatchClass = FieldAsmOperand;
  let PrintMethod = "printField";
}

class VUImmAsmOperand<string name, string diag> : AsmOperandClass {
  let Name = name;
  let RenderMethod = "addImmOperands";
  let DiagnosticString = diag;
}

def SImm5AsmOperand : VUImmAsmOperand<"SImm5",
    "immediate must be an integer in the range [-16, 15]">;
def simm5 : Operand<i16> {
  let ParserMatchClass = SImm5AsmOperand;
  let DecoderMethod = "decodeSImmOperand<5>";
}

def SImm11AsmOperand : VUImmAsmOperand<"SImm11",
    "immediate must be an integer in the range [-1024, 1023]">;
def simm11 : Operand<i16> {
  let ParserMatchClass = SImm11AsmOperand;
  let DecoderMethod = "decodeSImmOperand<11>";
}

def UImm12AsmOperand : VUImmAsmOperand<"UImm12",
    "immediate must be an integer in the range [0, 4095]">;
def uimm12 : Operand<i32> {
  let ParserMatchClass = UImm12AsmOperand;
}

def UImm15AsmOperand : VUImmAsmOperand<"UImm15",
    "immediate must be an integer in the range [0, 32767]">;
def uimm15 : Operand<i16> {
  let ParserMatchClass = UImm15AsmOperand;
}

def UImm24AsmOperand : VUImmAsmOperand<"UImm24",
    "immediate must be an integer in the range [0, 16777215]">;
def uimm24 : Operand<i32> {
  let ParserMatchClass = UImm24AsmOperand;
}

// LOI: a 32-bit integer or a floating-point literal.
def LOIImmAsmOperand : VUImmAsmOperand<"LOIImm",
    "immediate must be a 32-bit integer or a floating-point number">;
def loiimm : Operand<i32> {
  let ParserMatchClass = LOIImmAsmOperand;
  let PrintMethod = "printHex32";
}

// Branch target: an expression resolved PC-relative in units of 8 bytes,
// relative to the next micro instruction. A plain integer is used as the raw
// 11-bit field, like dvp-as does.
def BrTargetAsmOperand : VUImmAsmOperand<"BrTarget",
    "branch target must be a label or an integer in the range [-1024, 1023]">;
def brtarget : Operand<OtherVT> {
  let ParserMatchClass = BrTargetAsmOperand;
  let EncoderMethod = "getBranchTargetOpValue";
  let DecoderMethod = "decodeSImmOperand<11>";
  let PrintMethod = "printBranchTarget";
  let OperandType = "OPERAND_PCREL";
}

// The four vector fields, used to generate the broadcast (bc) variants.
class VUField<string name, bits<2> value> {
  string Name = name;
  bits<2> Value = value;
}
def FieldX : VUField<"x", 0>;
def FieldY : VUField<"y", 1>;
def FieldZ : VUField<"z", 2>;
def FieldW : VUField<"w", 3>;
defvar Fields = [FieldX, FieldY, FieldZ, FieldW];

//===----------------------------------------------------------------------===//
// Upper instructions: FMAC operations
//===----------------------------------------------------------------------===//

// fd = fs op ft
class U3<bits<6> op, string mn>
    : VUUpperOp<op, (outs VF:$fd), (ins dest:$dest, VF:$fs, VF:$ft),
                mn # "$dest\t$fd, $fs, $ft"> {
  bits<4> dest;
  bits<5> fd;
  bits<5> fs;
  bits<5> ft;
  let Inst{24-21} = dest;
  let Inst{20-16} = ft;
  let Inst{15-11} = fs;
  let Inst{10-6} = fd;
}

// fd = fs op ft.bc
class U3BC<bits<4> op, bits<2> bc, string mn, string f>
    : VUUpperOp<{op, bc}, (outs VF:$fd), (ins dest:$dest, VF:$fs, VF:$ft),
                mn # "$dest\t$fd, $fs, ${ft}" # f> {
  bits<4> dest;
  bits<5> fd;
  bits<5> fs;
  bits<5> ft;
  let Inst{24-21} = dest;
  let Inst{20-16} = ft;
  let Inst{15-11} = fs;
  let Inst{10-6} = fd;
}

// fd = fs op q/i
class U3S<bits<6> op, string mn, string sreg>
    : VUUpperOp<op, (outs VF:$fd), (ins dest:$dest, VF:$fs),
                mn # "$dest\t$fd, $fs, " # sreg> {
  bits<4> dest;
  bits<5> fd;
  bits<5> fs;
  let Inst{24-21} = dest;
  let Inst{20-16} = 0;
  let Inst{15-11} = fs;
  let Inst{10-6} = fd;
}

// acc = fs op ft
class UA<bits<7> idx, string mn>
    : VUUpperExt<idx, (outs), (ins dest:$dest, VF:$fs, VF:$ft),
                 mn # "$dest\tacc, $fs, $ft"> {
  bits<4> dest;
  bits<5> fs;
  bits<5> ft;
  let Inst{24-21} = dest;
  let Inst{20-16} = ft;
  let Inst{15-11} = fs;
  let Defs = [ACC];
}

// acc = fs op ft.bc
class UABC<bits<5> idx, bits<2> bc, string mn, string f>
    : VUUpperExt<{idx, bc}, (outs), (ins dest:$dest, VF:$fs, VF:$ft),
                 mn # "$dest\tacc, $fs, ${ft}" # f> {
  bits<4> dest;
  bits<5> fs;
  bits<5> ft;
  let Inst{24-21} = dest;
  let Inst{20-16} = ft;
  let Inst{15-11} = fs;
  let Defs = [ACC];
}

// acc = fs op q/i
class UAS<bits<7> idx, string mn, string sreg>
    : VUUpperExt<idx, (outs), (ins dest:$dest, VF:$fs),
                 mn # "$dest\tacc, $fs, " # sreg> {
  bits<4> dest;
  bits<5> fs;
  let Inst{24-21} = dest;
  let Inst{20-16} = 0;
  let Inst{15-11} = fs;
  let Defs = [ACC];
}

// ft = op fs
class U2<bits<7> idx, string mn>
    : VUUpperExt<idx, (outs VF:$ft), (ins dest:$dest, VF:$fs),
                 mn # "$dest\t$ft, $fs"> {
  bits<4> dest;
  bits<5> fs;
  bits<5> ft;
  let Inst{24-21} = dest;
  let Inst{20-16} = ft;
  let Inst{15-11} = fs;
}

multiclass U3BCs<bits<4> op, string mn> {
  foreach F = Fields in
    def NAME # F.Name : U3BC<op, F.Value, mn # F.Name, F.Name>;
}

multiclass UABCs<bits<5> idx, string mn> {
  foreach F = Fields in
    def NAME # F.Name : UABC<idx, F.Value, mn # F.Name, F.Name>;
}

// Operations that support every operand form.
multiclass FmacOp<string mn, bits<6> op, bits<4> opbc, bits<6> opq,
                  bits<6> opi, bits<7> idxa, bits<5> idxabc, bits<7> idxaq,
                  bits<7> idxai> {
  def "" : U3<op, mn>;
  defm "" : U3BCs<opbc, mn>;
  let Uses = [Q] in
  def q : U3S<opq, mn # "q", "q">;
  let Uses = [I] in
  def i : U3S<opi, mn # "i", "i">;
  def A : UA<idxa, mn # "a">;
  defm A : UABCs<idxabc, mn # "a">;
  let Uses = [Q] in
  def Aq : UAS<idxaq, mn # "aq", "q">;
  let Uses = [I] in
  def Ai : UAS<idxai, mn # "ai", "i">;
}

defm ADD  : FmacOp<"add",  0x28, 0b0000, 0x20, 0x22, 0x28, 0b00000, 0x20, 0x22>;
defm SUB  : FmacOp<"sub",  0x2c, 0b0001, 0x24, 0x26, 0x2c, 0b00001, 0x24, 0x26>;
defm MUL  : FmacOp<"mul",  0x2a, 0b0110, 0x1c, 0x1e, 0x2a, 0b00110, 0x1c, 0x1e>;

let Uses = [ACC] in {
defm MADD : FmacOp<"madd", 0x29, 0b0010, 0x21, 0x23, 0x29, 0b00010, 0x21, 0x23>;
defm MSUB : FmacOp<"msub", 0x2d, 0b0011, 0x25, 0x27, 0x2d, 0b00011, 0x25, 0x27>;
}

// MAX and MINI have no q and no ACC forms.
multiclass MinMaxOp<string mn, bits<6> op, bits<4> opbc, bits<6> opi> {
  def "" : U3<op, mn>;
  defm "" : U3BCs<opbc, mn>;
  let Uses = [I] in
  def i : U3S<opi, mn # "i", "i">;
}

defm MAX  : MinMaxOp<"max",  0x2b, 0b0100, 0x1d>;
defm MINI : MinMaxOp<"mini", 0x2f, 0b0101, 0x1f>;

// Outer product: OPMULA.xyz ACC, fs, ft and OPMSUB.xyz fd, fs, ft.
let Defs = [ACC] in
def OPMULA : VUUpperExt<0x2e, (outs), (ins VF:$fs, VF:$ft),
                        "opmula.xyz\tacc, $fs, $ft"> {
  bits<5> fs;
  bits<5> ft;
  let Inst{24-21} = 0b1110;
  let Inst{20-16} = ft;
  let Inst{15-11} = fs;
}

let Uses = [ACC] in
def OPMSUB : VUUpperOp<0x2e, (outs VF:$fd), (ins VF:$fs, VF:$ft),
                       "opmsub.xyz\t$fd, $fs, $ft"> {
  bits<5> fd;
  bits<5> fs;
  bits<5> ft;
  let Inst{24-21} = 0b1110;
  let Inst{20-16} = ft;
  let Inst{15-11} = fs;
  let Inst{10-6} = fd;
}

def ABS : U2<0x1d, "abs">;

def ITOF0  : U2<0x10, "itof0">;
def ITOF4  : U2<0x11, "itof4">;
def ITOF12 : U2<0x12, "itof12">;
def ITOF15 : U2<0x13, "itof15">;
def FTOI0  : U2<0x14, "ftoi0">;
def FTOI4  : U2<0x15, "ftoi4">;
def FTOI12 : U2<0x16, "ftoi12">;
def FTOI15 : U2<0x17, "ftoi15">;

// CLIPw.xyz fs, ft.w: judge fs.xyz against +/-|ft.w| and update the clipping
// flags.
def CLIP : VUUpperExt<0x1f, (outs), (ins VF:$fs, VF:$ft),
                      "clipw.xyz\t$fs, ${ft}w"> {
  bits<5> fs;
  bits<5> ft;
  let Inst{24-21} = 0b1110;
  let Inst{20-16} = ft;
  let Inst{15-11} = fs;
}

def NOP : VUUpperExt<0x2f, (outs), (ins), "nop"> {
  let Inst{24-11} = 0;
}

//===----------------------------------------------------------------------===//
// Lower instructions
//===----------------------------------------------------------------------===//

// Lower NOP. There is no real lower NOP: assemblers emit "move vf0, vf0" with
// an empty destination mask. The parser and disassembler handle it, so it is
// not part of the generated matcher and decoder.
let isCodeGenOnly = 1 in
def LNOP : VULowerExt<0x30, (outs), (ins), "nop"> {
  let Inst{24-11} = 0;
}

// LOI: the lower word holds a 32-bit immediate that is loaded into I. The
// code emitter sets the I bit of the upper word.
let Defs = [I], DecoderNamespace = "LOI" in
def LOI : VULower<(outs), (ins loiimm:$imm), "loi\t$imm"> {
  bits<32> imm;
  let Inst = imm;
}

//--- Load / store -----------------------------------------------------------//

let mayLoad = 1 in {
def LQ : VULowerOp<0x00, (outs VF:$ft), (ins dest:$dest, simm11:$imm, VI:$is),
                   "lq$dest\t$ft, ${imm}(${is})"> {
  bits<4> dest;
  bits<5> ft;
  bits<5> is;
  bits<11> imm;
  let Inst{24-21} = dest;
  let Inst{20-16} = ft;
  let Inst{15-11} = is;
  let Inst{10-0} = imm;
}

def ILW : VULowerOp<0x04, (outs VI:$it), (ins dest1:$dest, simm11:$imm, VI:$is),
                    "ilw$dest\t$it, ${imm}(${is})"> {
  bits<4> dest;
  bits<5> it;
  bits<5> is;
  bits<11> imm;
  let Inst{24-21} = dest;
  let Inst{20-16} = it;
  let Inst{15-11} = is;
  let Inst{10-0} = imm;
}

class LoadIncDec<bits<7> idx, string mn, string addr>
    : VULowerExt<idx, (outs VF:$ft, VI:$is_wb), (ins dest:$dest, VI:$is),
                 mn # "$dest\t$ft, " # addr> {
  bits<4> dest;
  bits<5> ft;
  bits<5> is;
  let Inst{24-21} = dest;
  let Inst{20-16} = ft;
  let Inst{15-11} = is;
  let Constraints = "$is = $is_wb";
}

def LQI : LoadIncDec<0x34, "lqi", "(${is}++)">;
def LQD : LoadIncDec<0x36, "lqd", "(--${is})">;

def ILWR : VULowerExt<0x3e, (outs VI:$it), (ins dest1:$dest, VI:$is),
                      "ilwr$dest\t$it, (${is})"> {
  bits<4> dest;
  bits<5> it;
  bits<5> is;
  let Inst{24-21} = dest;
  let Inst{20-16} = it;
  let Inst{15-11} = is;
}
} // mayLoad = 1

let mayStore = 1 in {
def SQ : VULowerOp<0x01, (outs), (ins dest:$dest, VF:$fs, simm11:$imm, VI:$it),
                   "sq$dest\t$fs, ${imm}(${it})"> {
  bits<4> dest;
  bits<5> fs;
  bits<5> it;
  bits<11> imm;
  let Inst{24-21} = dest;
  let Inst{20-16} = it;
  let Inst{15-11} = fs;
  let Inst{10-0} = imm;
}

def ISW : VULowerOp<0x05, (outs), (ins dest1:$dest, VI:$it, simm11:$imm, VI:$is),
                    "isw$dest\t$it, ${imm}(${is})"> {
  bits<4> dest;
  bits<5> it;
  bits<5> is;
  bits<11> imm;
  let Inst{24-21} = dest;
  let Inst{20-16} = it;
  let Inst{15-11} = is;
  let Inst{10-0} = imm;
}

class StoreIncDec<bits<7> idx, string mn, string addr>
    : VULowerExt<idx, (outs VI:$it_wb), (ins dest:$dest, VF:$fs, VI:$it),
                 mn # "$dest\t$fs, " # addr> {
  bits<4> dest;
  bits<5> fs;
  bits<5> it;
  let Inst{24-21} = dest;
  let Inst{20-16} = it;
  let Inst{15-11} = fs;
  let Constraints = "$it = $it_wb";
}

def SQI : StoreIncDec<0x35, "sqi", "(${it}++)">;
def SQD : StoreIncDec<0x37, "sqd", "(--${it})">;

def ISWR : VULowerExt<0x3f, (outs), (ins dest1:$dest, VI:$it, VI:$is),
                      "iswr$dest\t$it, (${is})"> {
  bits<4> dest;
  bits<5> it;
  bits<5> is;
  let Inst{24-21} = dest;
  let Inst{20-16} = it;
  let Inst{15-11} = is;
}
} // mayStore = 1

//--- Integer arithmetic -----------------------------------------------------//

class IntRRR<bits<6> fn, string mn>
    : VULowerFn<fn, (outs VI:$id), (ins VI:$is, VI:$it),
                mn # "\t$id, $is, $it"> {
  bits<5> id;
  bits<5> is;
  bits<5> it;
  let Inst{24-21} = 0;
  let Inst{20-16} = it;
  let Inst{15-11} = is;
  let Inst{10-6} = id;
}

def IADD : IntRRR<0x30, "iadd">;
def ISUB : IntRRR<0x31, "isub">;
def IAND : IntRRR<0x34, "iand">;
def IOR  : IntRRR<0x35, "ior">;

def IADDI : VULowerFn<0x32, (outs VI:$it), (ins VI:$is, simm5:$imm),
                      "iaddi\t$it, $is, $imm"> {
  bits<5> it;
  bits<5> is;
  bits<5> imm;
  let Inst{24-21} = 0;
  let Inst{20-16} = it;
  let Inst{15-11} = is;
  let Inst{10-6} = imm;
}

class IntRRImm15<bits<7> op, string mn>
    : VULowerOp<op, (outs VI:$it), (ins VI:$is, uimm15:$imm),
                mn # "\t$it, $is, $imm"> {
  bits<5> it;
  bits<5> is;
  bits<15> imm;
  let Inst{24-21} = imm{14-11};
  let Inst{20-16} = it;
  let Inst{15-11} = is;
  let Inst{10-0} = imm{10-0};
}

def IADDIU : IntRRImm15<0x08, "iaddiu">;
def ISUBIU : IntRRImm15<0x09, "isubiu">;

//--- Register transfer ------------------------------------------------------//

class MoveFF<bits<7> idx, string mn>
    : VULowerExt<idx, (outs VF:$ft), (ins dest:$dest, VF:$fs),
                 mn # "$dest\t$ft, $fs"> {
  bits<4> dest;
  bits<5> ft;
  bits<5> fs;
  let Inst{24-21} = dest;
  let Inst{20-16} = ft;
  let Inst{15-11} = fs;
}

def MOVE : MoveFF<0x30, "move">;
def MR32 : MoveFF<0x31, "mr32">;

def MFIR : VULowerExt<0x3d, (outs VF:$ft), (ins dest:$dest, VI:$is),
                      "mfir$dest\t$ft, $is"> {
  bits<4> dest;
  bits<5> ft;
  bits<5> is;
  let Inst{24-21} = dest;
  let Inst{20-16} = ft;
  let Inst{15-11} = is;
}

def MTIR : VULowerExt<0x3c, (outs VI:$it), (ins VF:$fs, vufield:$fsf),
                      "mtir\t$it, $fs$fsf"> {
  bits<5> it;
  bits<5> fs;
  bits<2> fsf;
  let Inst{24-23} = 0;
  let Inst{22-21} = fsf;
  let Inst{20-16} = it;
  let Inst{15-11} = fs;
}

//--- FDIV -------------------------------------------------------------------//

let Defs = [Q] in {
def DIV : VULowerExt<0x38, (outs), (ins VF:$fs, vufield:$fsf, VF:$ft, vufield:$ftf),
                     "div\tq, $fs$fsf, $ft$ftf"> {
  bits<5> fs;
  bits<2> fsf;
  bits<5> ft;
  bits<2> ftf;
  let Inst{24-23} = ftf;
  let Inst{22-21} = fsf;
  let Inst{20-16} = ft;
  let Inst{15-11} = fs;
}

def RSQRT : VULowerExt<0x3a, (outs),
                       (ins VF:$fs, vufield:$fsf, VF:$ft, vufield:$ftf),
                       "rsqrt\tq, $fs$fsf, $ft$ftf"> {
  bits<5> fs;
  bits<2> fsf;
  bits<5> ft;
  bits<2> ftf;
  let Inst{24-23} = ftf;
  let Inst{22-21} = fsf;
  let Inst{20-16} = ft;
  let Inst{15-11} = fs;
}

def SQRT : VULowerExt<0x39, (outs), (ins VF:$ft, vufield:$ftf),
                      "sqrt\tq, $ft$ftf"> {
  bits<5> ft;
  bits<2> ftf;
  let Inst{24-23} = ftf;
  let Inst{22-21} = 0;
  let Inst{20-16} = ft;
  let Inst{15-11} = 0;
}
} // Defs = [Q]

// WAITQ and WAITP also define Q and P: reads of the result are ordered after
// the wait, and may share its micro instruction word.
let Uses = [Q], Defs = [Q] in
def WAITQ : VULowerExt<0x3b, (outs), (ins), "waitq"> {
  let Inst{24-11} = 0;
}

//--- Random numbers ---------------------------------------------------------//

class RandGet<bits<7> idx, string mn>
    : VULowerExt<idx, (outs VF:$ft), (ins dest:$dest), mn # "$dest\t$ft, r"> {
  bits<4> dest;
  bits<5> ft;
  let Inst{24-21} = dest;
  let Inst{20-16} = ft;
  let Inst{15-11} = 0;
}

class RandSet<bits<7> idx, string mn>
    : VULowerExt<idx, (outs), (ins VF:$fs, vufield:$fsf), mn # "\tr, $fs$fsf"> {
  bits<5> fs;
  bits<2> fsf;
  let Inst{24-23} = 0;
  let Inst{22-21} = fsf;
  let Inst{20-16} = 0;
  let Inst{15-11} = fs;
}

let Uses = [R], Defs = [R] in
def RNEXT : RandGet<0x40, "rnext">;
let Uses = [R] in
def RGET : RandGet<0x41, "rget">;
let Defs = [R] in {
def RINIT : RandSet<0x42, "rinit">;
def RXOR : RandSet<0x43, "rxor">;
}

//--- Flags ------------------------------------------------------------------//

// Status flag operations with a 12-bit immediate split over bit 21 and
// bits 10-0.
class StatusFlagOp<bits<7> op, string mn>
    : VULowerOp<op, (outs VI:$it), (ins uimm12:$imm), mn # "\t$it, $imm"> {
  bits<5> it;
  bits<12> imm;
  let Inst{24-22} = 0;
  let Inst{21} = imm{11};
  let Inst{20-16} = it;
  let Inst{15-11} = 0;
  let Inst{10-0} = imm{10-0};
}

def FSEQ  : StatusFlagOp<0x14, "fseq">;
def FSAND : StatusFlagOp<0x16, "fsand">;
def FSOR  : StatusFlagOp<0x17, "fsor">;

def FSSET : VULowerOp<0x15, (outs), (ins uimm12:$imm), "fsset\t$imm"> {
  bits<12> imm;
  let Inst{24-22} = 0;
  let Inst{21} = imm{11};
  let Inst{20-11} = 0;
  let Inst{10-0} = imm{10-0};
}

class MACFlagOp<bits<7> op, string mn>
    : VULowerOp<op, (outs VI:$it), (ins VI:$is), mn # "\t$it, $is"> {
  bits<5> it;
  bits<5> is;
  let Inst{24-21} = 0;
  let Inst{20-16} = it;
  let Inst{15-11} = is;
  let Inst{10-0} = 0;
}

def FMEQ  : MACFlagOp<0x18, "fmeq">;
def FMAND : MACFlagOp<0x1a, "fmand">;
def FMOR  : MACFlagOp<0x1b, "fmor">;

// Clipping flag tests always write their result to VI1.
class ClipFlagTest<bits<7> op, string mn>
    : VULowerOp<op, (outs), (ins uimm24:$imm), mn # "\tvi1, $imm"> {
  bits<24> imm;
  let Inst{24} = 0;
  let Inst{23-0} = imm;
  let Defs = [VI1];
}

def FCEQ  : ClipFlagTest<0x10, "fceq">;
def FCAND : ClipFlagTest<0x12, "fcand">;
def FCOR  : ClipFlagTest<0x13, "fcor">;

def FCSET : VULowerOp<0x11, (outs), (ins uimm24:$imm), "fcset\t$imm"> {
  bits<24> imm;
  let Inst{24} = 0;
  let Inst{23-0} = imm;
}

def FCGET : VULowerOp<0x1c, (outs VI:$it), (ins), "fcget\t$it"> {
  bits<5> it;
  let Inst{24-21} = 0;
  let Inst{20-16} = it;
  let Inst{15-0} = 0;
}

//--- Branches ---------------------------------------------------------------//

let isBranch = 1, isTerminator = 1 in {
class CondBranch2<bits<7> op, string mn>
    : VULowerOp<op, (outs), (ins VI:$it, VI:$is, brtarget:$imm),
                mn # "\t$it, $is, $imm"> {
  bits<5> it;
  bits<5> is;
  bits<11> imm;
  let Inst{24-21} = 0;
  let Inst{20-16} = it;
  let Inst{15-11} = is;
  let Inst{10-0} = imm;
}

class CondBranch1<bits<7> op, string mn>
    : VULowerOp<op, (outs), (ins VI:$is, brtarget:$imm), mn # "\t$is, $imm"> {
  bits<5> is;
  bits<11> imm;
  let Inst{24-16} = 0;
  let Inst{15-11} = is;
  let Inst{10-0} = imm;
}
}

def IBEQ  : CondBranch2<0x28, "ibeq">;
def IBNE  : CondBranch2<0x29, "ibne">;
def IBLTZ : CondBranch1<0x2c, "ibltz">;
def IBGTZ : CondBranch1<0x2d, "ibgtz">;
def IBLEZ : CondBranch1<0x2e, "iblez">;
def IBGEZ : CondBranch1<0x2f, "ibgez">;

let isBranch = 1, isTerminator = 1, isBarrier = 1 in
def B : VULowerOp<0x20, (outs), (ins brtarget:$imm), "b\t$imm"> {
  bits<11> imm;
  let Inst{24-11} = 0;
  let Inst{10-0} = imm;
}

let isCall = 1 in
def BAL : VULowerOp<0x21, (outs VI:$it), (ins brtarget:$imm), "bal\t$it, $imm"> {
  bits<5> it;
  bits<11> imm;
  let Inst{24-21} = 0;
  let Inst{20-16} = it;
  let Inst{15-11} = 0;
  let Inst{10-0} = imm;
}

let isBranch = 1, isIndirectBranch = 1, isTerminator = 1, isBarrier = 1 in
def JR : VULowerOp<0x24, (outs), (ins VI:$is), "jr\t$is"> {
  bits<5> is;
  let Inst{24-16} = 0;
  let Inst{15-11} = is;
  let Inst{10-0} = 0;
}

let isCall = 1 in
def JALR : VULowerOp<0x25, (outs VI:$it), (ins VI:$is), "jalr\t$it, $is"> {
  bits<5> it;
  bits<5> is;
  let Inst{24-21} = 0;
  let Inst{20-16} = it;
  let Inst{15-11} = is;
  let Inst{10-0} = 0;
}

//--- VU1 only: EFU, external units ------------------------------------------//

let Predicates = [HasVU1] in {

let Uses = [P] in
def MFP : VULowerExt<0x64, (outs VF:$ft), (ins dest:$dest), "mfp$dest\t$ft, p"> {
  bits<4> dest;
  bits<5> ft;
  let Inst{24-21} = dest;
  let Inst{20-16} = ft;
  let Inst{15-11} = 0;
}

let Uses = [P], Defs = [P] in
def WAITP : VULowerExt<0x7b, (outs), (ins), "waitp"> {
  let Inst{24-11} = 0;
}

// Vector EFU operations use a fixed destination mask.
class EFUVector<bits<7> idx, bits<4> mask, string mn>
    : VULowerExt<idx, (outs), (ins VF:$fs), mn # "\tp, $fs"> {
  bits<5> fs;
  let Inst{24-21} = mask;
  let Inst{20-16} = 0;
  let Inst{15-11} = fs;
  let Defs = [P];
}

def ESADD   : EFUVector<0x70, 0b1110, "esadd">;
def ERSADD  : EFUVector<0x71, 0b1110, "ersadd">;
def ELENG   : EFUVector<0x72, 0b1110, "eleng">;
def ERLENG  : EFUVector<0x73, 0b1110, "erleng">;
def EATANxy : EFUVector<0x74, 0b1100, "eatanxy">;
def EATANxz : EFUVector<0x75, 0b1010, "eatanxz">;
def ESUM    : EFUVector<0x76, 0b1111, "esum">;

class EFUScalar<bits<7> idx, string mn>
    : VULowerExt<idx, (outs), (ins VF:$fs, vufield:$fsf), mn # "\tp, $fs$fsf"> {
  bits<5> fs;
  bits<2> fsf;
  let Inst{24-23} = 0;
  let Inst{22-21} = fsf;
  let Inst{20-16} = 0;
  let Inst{15-11} = fs;
  let Defs = [P];
}

def ESQRT  : EFUScalar<0x78, "esqrt">;
def ERSQRT : EFUScalar<0x79, "ersqrt">;
def ERCPR  : EFUScalar<0x7a, "ercpr">;
def ESIN   : EFUScalar<0x7c, "esin">;
def EATAN  : EFUScalar<0x7d, "eatan">;
def EEXP   : EFUScalar<0x7e, "eexp">;

let hasSideEffects = 1 in
def XTOP : VULowerExt<0x68, (outs VI:$it), (ins), "xtop\t$it"> {
  bits<5> it;
  let Inst{24-21} = 0;
  let Inst{20-16} = it;
  let Inst{15-11} = 0;
}

let hasSideEffects = 1 in
def XGKICK : VULowerExt<0x6c, (outs), (ins VI:$is), "xgkick\t$is"> {
  bits<5> is;
  let Inst{24-16} = 0;
  let Inst{15-11} = is;
}

} // Predicates = [HasVU1]

let hasSideEffects = 1 in
def XITOP : VULowerExt<0x69, (outs VI:$it), (ins), "xitop\t$it"> {
  bits<5> it;
  let Inst{24-21} = 0;
  let Inst{20-16} = it;
  let Inst{15-11} = 0;
}

//===----------------------------------------------------------------------===//
// Code generation
//===----------------------------------------------------------------------===//

include "VUInstrPatterns.td"

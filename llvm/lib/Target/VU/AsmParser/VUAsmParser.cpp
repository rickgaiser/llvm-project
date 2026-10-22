//===-- VUAsmParser.cpp - Parse VU micro mode assembly --------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A statement holds one micro instruction word: an upper instruction followed
// by a lower instruction on the same line, as written by VCL and accepted by
// dvp-as:
//
//   mulax.xyz ACC, VF14, VF06x    lq.xyz VF13, 60(VI00)
//   nop[e]                        nop
//
// A line with a single instruction gets a NOP in the other half.
//
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/VUMCTargetDesc.h"
#include "TargetInfo/VUTargetInfo.h"
#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCParser/AsmLexer.h"
#include "llvm/MC/MCParser/MCParsedAsmOperand.h"
#include "llvm/MC/MCParser/MCTargetAsmParser.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCSectionELF.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/StringSaver.h"

using namespace llvm;

namespace {

class VUOperand;

class VUAsmParser : public MCTargetAsmParser {
  BumpPtrAllocator Alloc;
  StringSaver Saver{Alloc};

  // The two halves of the statement being parsed.
  SmallVector<std::unique_ptr<MCParsedAsmOperand>, 8> UpperOps;
  SmallVector<std::unique_ptr<MCParsedAsmOperand>, 8> LowerOps;
  SMLoc UpperLoc;
  SMLoc LowerLoc;
  unsigned BundleFlags = 0;

  SMLoc getLoc() const { return getParser().getTok().getLoc(); }

  bool parseHalf(OperandVector &Ops, StringRef Name, SMLoc NameLoc,
                 bool IsFirst);
  bool parseFlags();
  bool parseOperand(OperandVector &Ops);
  bool parseRegisterOperand(OperandVector &Ops);
  bool parseImmediate(OperandVector &Ops);
  bool parseBaseRegister(OperandVector &Ops);
  bool matchHalf(OperandVector &Ops, SMLoc Loc, MCInst &Inst);

  bool parseRegister(MCRegister &Reg, SMLoc &StartLoc, SMLoc &EndLoc) override;
  ParseStatus tryParseRegister(MCRegister &Reg, SMLoc &StartLoc,
                               SMLoc &EndLoc) override;
  bool parseInstruction(ParseInstructionInfo &Info, StringRef Name,
                        SMLoc NameLoc, OperandVector &Operands) override;
  bool matchAndEmitInstruction(SMLoc IDLoc, unsigned &Opcode,
                               OperandVector &Operands, MCStreamer &Out,
                               uint64_t &ErrorInfo,
                               bool MatchingInlineAsm) override;
  ParseStatus parseDirective(AsmToken DirectiveID) override;

#define GET_ASSEMBLER_HEADER
#include "VUGenAsmMatcher.inc"

public:
  enum VUMatchResultTy {
    Match_Dummy = FIRST_TARGET_MATCH_RESULT_TY,
#define GET_OPERAND_DIAGNOSTIC_TYPES
#include "VUGenAsmMatcher.inc"
#undef GET_OPERAND_DIAGNOSTIC_TYPES
  };

  VUAsmParser(const MCSubtargetInfo &STI, MCAsmParser &Parser,
              const MCInstrInfo &MII)
      : MCTargetAsmParser(STI, MII) {
    MCAsmParserExtension::Initialize(Parser);
    setAvailableFeatures(ComputeAvailableFeatures(STI.getFeatureBits()));
  }
};

class VUOperand : public MCParsedAsmOperand {
  enum KindTy { Token, Register, Immediate, Dest } Kind;
  SMLoc StartLoc, EndLoc;
  StringRef Tok;
  MCRegister Reg;
  const MCExpr *Imm = nullptr;
  unsigned Mask = 0;

  bool getConstant(int64_t &Value) const {
    if (Kind != Immediate)
      return false;
    if (const auto *CE = dyn_cast<MCConstantExpr>(Imm)) {
      Value = CE->getValue();
      return true;
    }
    return false;
  }

public:
  VUOperand(KindTy K, SMLoc S, SMLoc E) : Kind(K), StartLoc(S), EndLoc(E) {}

  static std::unique_ptr<VUOperand> createToken(StringRef Str, SMLoc S) {
    auto Op = std::make_unique<VUOperand>(Token, S, S);
    Op->Tok = Str;
    return Op;
  }
  static std::unique_ptr<VUOperand> createReg(MCRegister R, SMLoc S, SMLoc E) {
    auto Op = std::make_unique<VUOperand>(Register, S, E);
    Op->Reg = R;
    return Op;
  }
  static std::unique_ptr<VUOperand> createImm(const MCExpr *Val, SMLoc S,
                                              SMLoc E) {
    auto Op = std::make_unique<VUOperand>(Immediate, S, E);
    Op->Imm = Val;
    return Op;
  }
  static std::unique_ptr<VUOperand> createDest(unsigned Mask, SMLoc S) {
    auto Op = std::make_unique<VUOperand>(Dest, S, S);
    Op->Mask = Mask;
    return Op;
  }

  bool isToken() const override { return Kind == Token; }
  bool isReg() const override { return Kind == Register; }
  bool isImm() const override { return Kind == Immediate; }
  bool isMem() const override { return false; }

  bool isDest() const { return Kind == Dest; }
  bool isDestSingle() const {
    return Kind == Dest && llvm::popcount(Mask) == 1;
  }
  bool isField() const {
    return Kind == Token && Tok.size() == 1 && StringRef("xyzw").contains(Tok);
  }

  template <unsigned N> bool isSImm() const {
    int64_t V;
    return getConstant(V) && isInt<N>(V);
  }
  template <unsigned N> bool isUImm() const {
    int64_t V;
    return getConstant(V) && isUInt<N>(V);
  }
  bool isSImm5() const { return isSImm<5>(); }
  bool isSImm11() const { return isSImm<11>(); }
  bool isUImm12() const { return isUImm<12>(); }
  bool isUImm15() const { return isUImm<15>(); }
  bool isUImm24() const { return isUImm<24>(); }
  bool isLOIImm() const {
    int64_t V;
    return getConstant(V) && (isUInt<32>(V) || isInt<32>(V));
  }
  bool isBrTarget() const {
    int64_t V;
    if (getConstant(V))
      return isInt<11>(V);
    return Kind == Immediate;
  }

  void setToken(StringRef Str) {
    assert(Kind == Token && "Invalid access!");
    Tok = Str;
  }
  StringRef getToken() const {
    assert(Kind == Token && "Invalid access!");
    return Tok;
  }
  MCRegister getReg() const override {
    assert(Kind == Register && "Invalid access!");
    return Reg;
  }
  SMLoc getStartLoc() const override { return StartLoc; }
  SMLoc getEndLoc() const override { return EndLoc; }

  void addRegOperands(MCInst &Inst, unsigned N) const {
    assert(N == 1 && "Invalid number of operands!");
    Inst.addOperand(MCOperand::createReg(getReg()));
  }
  void addImmOperands(MCInst &Inst, unsigned N) const {
    assert(N == 1 && "Invalid number of operands!");
    int64_t V;
    if (getConstant(V))
      Inst.addOperand(MCOperand::createImm(V));
    else
      Inst.addOperand(MCOperand::createExpr(Imm));
  }
  void addDestOperands(MCInst &Inst, unsigned N) const {
    assert(N == 1 && "Invalid number of operands!");
    Inst.addOperand(MCOperand::createImm(Mask));
  }
  void addFieldOperands(MCInst &Inst, unsigned N) const {
    assert(N == 1 && "Invalid number of operands!");
    Inst.addOperand(MCOperand::createImm(StringRef("xyzw").find(Tok)));
  }

  void print(raw_ostream &OS, const MCAsmInfo &MAI) const override {
    switch (Kind) {
    case Token:
      OS << "Token: " << Tok;
      break;
    case Register:
      OS << "Reg: " << Reg.id();
      break;
    case Immediate:
      OS << "Imm: ";
      MAI.printExpr(OS, *Imm);
      break;
    case Dest:
      OS << "Dest: " << Mask;
      break;
    }
  }
};

} // end anonymous namespace

#define GET_REGISTER_MATCHER
#define GET_SUBTARGET_FEATURE_NAME
#define GET_MATCHER_IMPLEMENTATION
#define GET_MNEMONIC_SPELL_CHECKER
#include "VUGenAsmMatcher.inc"

/// Parse a field mask such as "xyw". Fields must be in xyzw order.
static std::optional<unsigned> parseFieldMask(StringRef S) {
  unsigned Mask = 0;
  size_t Pos = 0;
  for (char C : S) {
    size_t Idx = StringRef("xyzw").find(C, Pos);
    if (Idx == StringRef::npos)
      return std::nullopt;
    Mask |= 8 >> Idx;
    Pos = Idx + 1;
  }
  return Mask;
}

/// Mnemonics (without destination suffix) that take a destination mask.
static bool hasDestField(StringRef Base) {
  auto IsFmac = [](StringRef S) {
    return StringSwitch<bool>(S)
        .Cases({"add", "adda", "sub", "suba", "mul", "mula", "madd", "madda",
                "msub", "msuba", "max", "mini"},
               true)
        .Default(false);
  };
  if (IsFmac(Base))
    return true;
  // Broadcast, I and Q forms: "addx", "mulq", "minii", ...
  if (Base.size() > 1 && StringRef("xyzwiq").contains(Base.back()) &&
      IsFmac(Base.drop_back()))
    return true;
  return StringSwitch<bool>(Base)
      .Cases({"abs", "ftoi0", "ftoi4", "ftoi12", "ftoi15", "itof0", "itof4",
              "itof12", "itof15"},
             true)
      .Cases({"lq", "lqi", "lqd", "sq", "sqi", "sqd", "ilw", "isw", "ilwr",
              "iswr", "move", "mr32", "mfir", "mfp", "rnext", "rget"},
             true)
      .Default(false);
}

/// Mnemonics whose destination is always ".xyz".
static bool hasFixedXYZDest(StringRef Base) {
  return Base == "clipw" || Base == "opmula" || Base == "opmsub";
}

/// Mnemonics without operands. A following identifier starts the second
/// instruction of the micro instruction word.
static bool hasNoOperands(StringRef Base) {
  return Base == "nop" || Base == "waitq" || Base == "waitp";
}

/// Match a register name such as "vf01", "VI2", "ACCxyz" or "Q". On success
/// returns the register and the field suffix of a VF register or ACC.
static MCRegister matchVURegister(StringRef Name, StringRef &Suffix) {
  std::string Lower = Name.lower();
  StringRef S = Lower;
  Suffix = StringRef();

  auto ParseNumbered = [&](StringRef Prefix, unsigned Count,
                           MCRegister First) -> MCRegister {
    if (!S.consume_front(Prefix))
      return MCRegister();
    size_t NumLen = S.find_first_not_of("0123456789");
    StringRef Num = S.substr(0, NumLen);
    unsigned N;
    if (Num.empty() || Num.getAsInteger(10, N) || N >= Count)
      return MCRegister();
    Suffix = Name.substr(Name.size() - (S.size() - Num.size()));
    // The generated register enums are contiguous for VF0-31 and VI0-15.
    return MCRegister(First.id() + N);
  };

  if (S.starts_with("vf"))
    return ParseNumbered("vf", 32, VU::VF0);
  if (S.starts_with("vi")) {
    MCRegister R = ParseNumbered("vi", 16, VU::VI0);
    return Suffix.empty() ? R : MCRegister();
  }
  if (S.consume_front("acc")) {
    Suffix = Name.substr(3);
    return VU::ACC;
  }
  return StringSwitch<MCRegister>(S)
      .Case("i", VU::I)
      .Case("q", VU::Q)
      .Case("p", VU::P)
      .Case("r", VU::R)
      .Default(MCRegister());
}

bool VUAsmParser::parseRegister(MCRegister &Reg, SMLoc &StartLoc,
                                SMLoc &EndLoc) {
  if (!tryParseRegister(Reg, StartLoc, EndLoc).isSuccess())
    return Error(StartLoc, "invalid register name");
  return false;
}

ParseStatus VUAsmParser::tryParseRegister(MCRegister &Reg, SMLoc &StartLoc,
                                          SMLoc &EndLoc) {
  const AsmToken &Tok = getParser().getTok();
  StartLoc = Tok.getLoc();
  EndLoc = Tok.getEndLoc();
  if (Tok.isNot(AsmToken::Identifier))
    return ParseStatus::NoMatch;
  StringRef Suffix;
  Reg = matchVURegister(Tok.getIdentifier(), Suffix);
  if (!Reg || !Suffix.empty())
    return ParseStatus::NoMatch;
  getParser().Lex();
  return ParseStatus::Success;
}

bool VUAsmParser::parseRegisterOperand(OperandVector &Ops) {
  const AsmToken &Tok = getParser().getTok();
  SMLoc S = Tok.getLoc();
  SMLoc E = Tok.getEndLoc();
  StringRef Suffix;
  MCRegister Reg = matchVURegister(Tok.getIdentifier(), Suffix);
  assert(Reg && "Expected a register");

  std::optional<unsigned> Mask = parseFieldMask(Suffix.lower());
  if (!Mask)
    return Error(S, "invalid field suffix '" + Suffix + "'");
  getParser().Lex();

  Ops.push_back(VUOperand::createReg(Reg, S, E));
  // A single field selects a scalar operand ("vf2x", "vf7w"). A longer suffix
  // only repeats the destination mask ("VF07xyz") and is not checked.
  if (Suffix.size() == 1 && Reg != VU::ACC)
    Ops.push_back(VUOperand::createToken(Saver.save(Suffix.lower()), E));
  return false;
}

bool VUAsmParser::parseImmediate(OperandVector &Ops) {
  SMLoc S = getLoc();
  AsmLexer &Lexer = getLexer();

  // A floating-point literal (for LOI) is converted to its bit pattern.
  bool Negative =
      Lexer.is(AsmToken::Minus) && Lexer.peekTok().is(AsmToken::Real);
  if (Lexer.is(AsmToken::Real) || Negative) {
    if (Negative)
      getParser().Lex();
    APFloat F(APFloat::IEEEsingle());
    auto Status = F.convertFromString(getParser().getTok().getString(),
                                      APFloat::rmNearestTiesToEven);
    if (!Status)
      return Error(S, "invalid floating-point literal");
    if (Negative)
      F.changeSign();
    SMLoc E = getParser().getTok().getEndLoc();
    getParser().Lex();
    uint64_t Bits = F.bitcastToAPInt().getZExtValue();
    Ops.push_back(
        VUOperand::createImm(MCConstantExpr::create(Bits, getContext()), S, E));
    return false;
  }

  const MCExpr *Expr;
  SMLoc E;
  if (getParser().parseExpression(Expr, E))
    return true;
  Ops.push_back(VUOperand::createImm(Expr, S, E));
  return false;
}

/// Parse "(vi2)", "(vi2++)" or "(--vi2)" into separate tokens and a register.
bool VUAsmParser::parseBaseRegister(OperandVector &Ops) {
  AsmLexer &Lexer = getLexer();
  assert(Lexer.is(AsmToken::LParen) && "Expected '('");
  Ops.push_back(VUOperand::createToken("(", getLoc()));
  getParser().Lex();

  if (Lexer.is(AsmToken::Minus)) {
    Ops.push_back(VUOperand::createToken("-", getLoc()));
    getParser().Lex();
    if (Lexer.isNot(AsmToken::Minus))
      return TokError("expected '--'");
    Ops.push_back(VUOperand::createToken("-", getLoc()));
    getParser().Lex();
  }

  StringRef Suffix;
  if (Lexer.isNot(AsmToken::Identifier) ||
      !matchVURegister(Lexer.getTok().getIdentifier(), Suffix))
    return TokError("expected a register");
  if (parseRegisterOperand(Ops))
    return true;

  if (Lexer.is(AsmToken::Plus)) {
    Ops.push_back(VUOperand::createToken("+", getLoc()));
    getParser().Lex();
    if (Lexer.isNot(AsmToken::Plus))
      return TokError("expected '++'");
    Ops.push_back(VUOperand::createToken("+", getLoc()));
    getParser().Lex();
  }

  if (Lexer.isNot(AsmToken::RParen))
    return TokError("expected ')'");
  Ops.push_back(VUOperand::createToken(")", getLoc()));
  getParser().Lex();
  return false;
}

bool VUAsmParser::parseOperand(OperandVector &Ops) {
  AsmLexer &Lexer = getLexer();

  if (Lexer.is(AsmToken::Identifier)) {
    StringRef Suffix;
    if (matchVURegister(Lexer.getTok().getIdentifier(), Suffix))
      return parseRegisterOperand(Ops);
  }

  if (Lexer.is(AsmToken::LParen)) {
    // "(vi2)", "(vi2++)" or "(--vi2)"; otherwise a parenthesized expression.
    AsmToken Next[2];
    Lexer.peekTokens(Next);
    StringRef Suffix;
    if ((Next[0].is(AsmToken::Identifier) &&
         matchVURegister(Next[0].getIdentifier(), Suffix)) ||
        (Next[0].is(AsmToken::Minus) && Next[1].is(AsmToken::Minus)))
      return parseBaseRegister(Ops);
  }

  if (parseImmediate(Ops))
    return true;

  // "imm(vi2)"
  if (Lexer.is(AsmToken::LParen))
    return parseBaseRegister(Ops);
  return false;
}

bool VUAsmParser::parseFlags() {
  AsmLexer &Lexer = getLexer();
  while (Lexer.is(AsmToken::LBrac)) {
    getParser().Lex();
    if (Lexer.isNot(AsmToken::Identifier))
      return TokError("expected a flag (e, m, d or t)");
    SMLoc FlagLoc = getLoc();
    for (char C : Lexer.getTok().getIdentifier().lower()) {
      unsigned Flag = StringSwitch<unsigned>(StringRef(&C, 1))
                          .Case("e", VU::FlagE)
                          .Case("m", VU::FlagM)
                          .Case("d", VU::FlagD)
                          .Case("t", VU::FlagT)
                          .Default(0);
      if (!Flag)
        return Error(FlagLoc, "unknown flag '" + Twine(C) + "'");
      BundleFlags |= Flag;
    }
    getParser().Lex();
    if (Lexer.isNot(AsmToken::RBrac))
      return TokError("expected ']'");
    getParser().Lex();
  }
  return false;
}

bool VUAsmParser::parseHalf(OperandVector &Ops, StringRef Name, SMLoc NameLoc,
                            bool IsFirst) {
  AsmLexer &Lexer = getLexer();
  std::string LowerName = Name.lower();

  // VCL also writes the flags before the field mask: "maxi[E].w".
  if (Lexer.is(AsmToken::LBrac)) {
    if (!IsFirst)
      return TokError("flags are only allowed on the first instruction");
    if (parseFlags())
      return true;
    if (Lexer.is(AsmToken::Identifier) &&
        Lexer.getTok().getIdentifier().starts_with(".")) {
      LowerName += Lexer.getTok().getIdentifier().lower();
      getParser().Lex();
    }
  }

  auto [Base, Suffix] = StringRef(LowerName).split('.');

  if (hasFixedXYZDest(Base)) {
    if (!Suffix.empty() && Suffix != "xyz")
      return Error(NameLoc, "'" + Base + "' only supports the .xyz field mask");
    Ops.push_back(VUOperand::createToken(Saver.save(Base + ".xyz"), NameLoc));
  } else {
    Ops.push_back(VUOperand::createToken(Saver.save(Base), NameLoc));
    if (hasDestField(Base)) {
      // No suffix means all fields; a lone '.' means no fields.
      bool HasDot = Base.size() != LowerName.size();
      std::optional<unsigned> Mask = parseFieldMask(Suffix);
      if (!Mask)
        return Error(NameLoc, "invalid field mask '." + Suffix + "'");
      Ops.push_back(VUOperand::createDest(HasDot ? *Mask : 0xf, NameLoc));
    } else if (!Suffix.empty()) {
      // Let the matcher report the unknown mnemonic, e.g. "iadd.x".
      static_cast<VUOperand &>(*Ops.back()).setToken(Saver.save(LowerName));
    }
  }

  if (Lexer.is(AsmToken::LBrac)) {
    if (!IsFirst)
      return TokError("flags are only allowed on the first instruction");
    if (parseFlags())
      return true;
  }

  if (hasNoOperands(Base) || Lexer.is(AsmToken::EndOfStatement))
    return false;

  if (parseOperand(Ops))
    return true;
  while (Lexer.is(AsmToken::Comma)) {
    getParser().Lex();
    if (parseOperand(Ops))
      return true;
  }
  return false;
}

bool VUAsmParser::parseInstruction(ParseInstructionInfo &Info, StringRef Name,
                                   SMLoc NameLoc, OperandVector &Operands) {
  UpperOps.clear();
  LowerOps.clear();
  BundleFlags = 0;

  UpperLoc = NameLoc;
  if (parseHalf(UpperOps, Name, NameLoc, /*IsFirst=*/true))
    return true;

  AsmLexer &Lexer = getLexer();
  if (Lexer.isNot(AsmToken::EndOfStatement)) {
    if (Lexer.isNot(AsmToken::Identifier))
      return TokError("unexpected token in operand list");
    LowerLoc = getLoc();
    StringRef LowerName = Lexer.getTok().getIdentifier();
    getParser().Lex();
    if (parseHalf(LowerOps, LowerName, LowerLoc, /*IsFirst=*/false))
      return true;
    if (Lexer.isNot(AsmToken::EndOfStatement))
      return TokError("unexpected token after the lower instruction");
  }
  getParser().Lex(); // Consume the EndOfStatement.

  // The generic parser only needs a non-empty operand list; the halves are
  // matched in matchAndEmitInstruction.
  Operands.push_back(VUOperand::createToken(Saver.save(Name), NameLoc));
  return false;
}

bool VUAsmParser::matchHalf(OperandVector &Ops, SMLoc Loc, MCInst &Inst) {
  uint64_t ErrorInfo = 0;
  FeatureBitset MissingFeatures;
  unsigned Result = MatchInstructionImpl(Ops, Inst, ErrorInfo, MissingFeatures,
                                         /*matchingInlineAsm=*/false);
  auto ErrorLoc = [&]() {
    if (ErrorInfo < Ops.size())
      return static_cast<VUOperand &>(*Ops[ErrorInfo]).getStartLoc();
    return Loc;
  };

  switch (Result) {
  case Match_Success: {
    // ACC and Q operands are not matched from the assembly syntax; the
    // matcher leaves a placeholder for them.
    const MCInstrDesc &Desc = MII.get(Inst.getOpcode());
    for (unsigned I = 0, E = Inst.getNumOperands(); I != E; ++I) {
      if (I >= Desc.getNumOperands())
        break;
      int16_t RC = Desc.operands()[I].RegClass;
      if (RC == VU::ACCRRegClassID)
        Inst.getOperand(I) = MCOperand::createReg(VU::ACC);
      else if (RC == VU::QRRegClassID)
        Inst.getOperand(I) = MCOperand::createReg(VU::Q);
    }
    Inst.setLoc(Loc);
    return false;
  }
  case Match_MissingFeature: {
    std::string Msg = "instruction requires:";
    for (unsigned I = 0, E = MissingFeatures.size(); I != E; ++I)
      if (MissingFeatures[I])
        Msg += std::string(" ") + getSubtargetFeatureName(I);
    return Error(Loc, Msg);
  }
  case Match_MnemonicFail: {
    StringRef Mnemonic = static_cast<VUOperand &>(*Ops[0]).getToken();
    FeatureBitset FBS = ComputeAvailableFeatures(getSTI().getFeatureBits());
    std::string Suggestion = VUMnemonicSpellCheck(Mnemonic, FBS, 0);
    return Error(Loc, "unrecognized instruction mnemonic" + Suggestion);
  }
  case Match_InvalidOperand:
    if (ErrorInfo != ~0ULL && ErrorInfo >= Ops.size())
      return Error(Loc, "too few operands for instruction");
    return Error(ErrorLoc(), "invalid operand for instruction");
  default:
    break;
  }

  if (const char *Diag = getMatchKindDiag((VUMatchResultTy)Result))
    return Error(ErrorLoc(), Diag);
  return Error(Loc, "invalid instruction");
}

bool VUAsmParser::matchAndEmitInstruction(SMLoc IDLoc, unsigned &Opcode,
                                          OperandVector &Operands,
                                          MCStreamer &Out, uint64_t &ErrorInfo,
                                          bool MatchingInlineAsm) {
  MCInst Upper, Lower;
  if (matchHalf(UpperOps, UpperLoc, Upper))
    return true;
  bool FirstIsLower = VU::isLowerInstr(MII.get(Upper.getOpcode()));

  if (LowerOps.empty()) {
    // A single instruction: the other half of the word is a NOP.
    if (FirstIsLower) {
      Lower = Upper;
      Upper = MCInst();
      Upper.setOpcode(VU::NOP);
      Upper.setLoc(IDLoc);
    } else {
      Lower.setOpcode(VU::LNOP);
      Lower.setLoc(IDLoc);
    }
  } else {
    if (FirstIsLower)
      return Error(UpperLoc, "expected an upper instruction");
    if (matchHalf(LowerOps, LowerLoc, Lower))
      return true;
    if (Lower.getOpcode() == VU::NOP) {
      Lower = MCInst();
      Lower.setOpcode(VU::LNOP);
      Lower.setLoc(LowerLoc);
    } else if (!VU::isLowerInstr(MII.get(Lower.getOpcode()))) {
      return Error(LowerLoc, "expected a lower instruction");
    }
  }

  MCContext &Ctx = getContext();
  MCInst *UpperInst = Ctx.createMCInst();
  *UpperInst = Upper;
  MCInst *LowerInst = Ctx.createMCInst();
  *LowerInst = Lower;

  MCInst Bundle;
  Bundle.setOpcode(VU::BUNDLE);
  Bundle.setLoc(IDLoc);
  Bundle.addOperand(MCOperand::createImm(BundleFlags));
  Bundle.addOperand(MCOperand::createInst(UpperInst));
  Bundle.addOperand(MCOperand::createInst(LowerInst));

  // MCStreamer::emitInstruction only visits the expressions of the bundle
  // itself, so register the symbols used by its instructions here.
  for (const MCInst *I : {UpperInst, LowerInst})
    for (const MCOperand &Op : *I)
      if (Op.isExpr())
        Out.visitUsedExpr(*Op.getExpr());

  Out.getCurrentSectionOnly()->ensureMinAlignment(Align(8));
  Out.emitInstruction(Bundle, getSTI());
  Opcode = VU::BUNDLE;
  return false;
}

ParseStatus VUAsmParser::parseDirective(AsmToken DirectiveID) {
  StringRef IDVal = DirectiveID.getIdentifier();

  // ".vu" starts VU code, which dvp-as places in the ".vutext" section.
  if (IDVal == ".vu") {
    if (parseEOL())
      return ParseStatus::Failure;
    MCSection *Sec = getContext().getELFSection(
        ".vutext", ELF::SHT_PROGBITS, ELF::SHF_ALLOC | ELF::SHF_EXECINSTR);
    Sec->ensureMinAlignment(Align(8));
    getStreamer().switchSection(Sec);
    return ParseStatus::Success;
  }

  return ParseStatus::NoMatch;
}

extern "C" LLVM_EXTERNAL_VISIBILITY void LLVMInitializeVUAsmParser() {
  RegisterMCAsmParser<VUAsmParser> X(getTheVUTarget());
}

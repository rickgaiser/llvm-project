//===-- DVPVUAsmParser.cpp - Parse DVPVU assembly to MCInst ---------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/DVPVUMCTargetDesc.h"
#include "TargetInfo/DVPVUTargetInfo.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCParser/AsmLexer.h"
#include "llvm/MC/MCParser/MCParsedAsmOperand.h"
#include "llvm/MC/MCParser/MCTargetAsmParser.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/MCValue.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/MathExtras.h"

using namespace llvm;

#define DEBUG_TYPE "dvpvu-asm-parser"

namespace {

class DVPVUOperand : public MCParsedAsmOperand {
public:
  enum KindTy {
    Token,
    Register,
    Immediate,
    Memory,
    DestMask,
    Broadcast,
    FieldSel
  };

private:
  KindTy Kind;
  SMLoc StartLoc, EndLoc;

  union {
    StringRef Tok;
    struct {
      MCRegister RegNum;
    } Reg;
    struct {
      const MCExpr *Val;
    } Imm;
    struct {
      MCRegister BaseReg;
      const MCExpr *Offset;
    } Mem;
    struct {
      unsigned Mask; // 4-bit xyzw mask
    } Dest;
    struct {
      unsigned Field; // 0=x, 1=y, 2=z, 3=w
    } BC;
  };

public:
  DVPVUOperand(KindTy K) : Kind(K) {}

  bool isToken() const override { return Kind == Token; }
  bool isReg() const override { return Kind == Register; }
  bool isImm() const override { return Kind == Immediate; }
  bool isMem() const override { return Kind == Memory; }
  bool isMemVI() const { return Kind == Memory; }
  bool isDestMask() const { return Kind == DestMask; }
  bool isBroadcast() const { return Kind == Broadcast || Kind == FieldSel; }
  bool isField() const { return Kind == FieldSel || Kind == Broadcast; }

  SMLoc getStartLoc() const override { return StartLoc; }
  SMLoc getEndLoc() const override { return EndLoc; }

  StringRef getToken() const {
    assert(Kind == Token && "Not a token");
    return Tok;
  }

  MCRegister getReg() const override {
    assert(Kind == Register && "Not a register");
    return Reg.RegNum;
  }

  const MCExpr *getImm() const {
    assert(Kind == Immediate && "Not an immediate");
    return Imm.Val;
  }

  unsigned getDestMaskVal() const {
    assert(Kind == DestMask && "Not a dest mask");
    return Dest.Mask;
  }

  unsigned getBroadcastField() const {
    assert((Kind == Broadcast || Kind == FieldSel) && "Not a broadcast/field");
    return BC.Field;
  }

  unsigned getFieldVal() const {
    assert((Kind == FieldSel || Kind == Broadcast) && "Not a field/broadcast");
    return BC.Field;
  }

  MCRegister getMemBase() const {
    assert(Kind == Memory && "Not a memory operand");
    return Mem.BaseReg;
  }

  const MCExpr *getMemOffset() const {
    assert(Kind == Memory && "Not a memory operand");
    return Mem.Offset;
  }

  void addRegOperands(MCInst &Inst, unsigned N) const {
    assert(N == 1 && "Invalid number of operands");
    Inst.addOperand(MCOperand::createReg(getReg()));
  }

  void addImmOperands(MCInst &Inst, unsigned N) const {
    assert(N == 1 && "Invalid number of operands");
    const MCExpr *Expr = getImm();
    if (auto *CE = dyn_cast<MCConstantExpr>(Expr))
      Inst.addOperand(MCOperand::createImm(CE->getValue()));
    else
      Inst.addOperand(MCOperand::createExpr(Expr));
  }

  void addDestMaskOperands(MCInst &Inst, unsigned N) const {
    assert(N == 1 && "Invalid number of operands");
    Inst.addOperand(MCOperand::createImm(getDestMaskVal()));
  }

  void addBroadcastOperands(MCInst &Inst, unsigned N) const {
    assert(N == 1 && "Invalid number of operands");
    Inst.addOperand(MCOperand::createImm(getBroadcastField()));
  }

  void addFieldOperands(MCInst &Inst, unsigned N) const {
    assert(N == 1 && "Invalid number of operands");
    Inst.addOperand(MCOperand::createImm(getFieldVal()));
  }

  void addMemVIOperands(MCInst &Inst, unsigned N) const {
    assert(N == 2 && "Invalid number of operands for memory");
    Inst.addOperand(MCOperand::createReg(getMemBase()));
    const MCExpr *Expr = getMemOffset();
    if (auto *CE = dyn_cast<MCConstantExpr>(Expr))
      Inst.addOperand(MCOperand::createImm(CE->getValue()));
    else
      Inst.addOperand(MCOperand::createExpr(Expr));
  }

  void print(raw_ostream &OS, const MCAsmInfo &MAI) const override {
    switch (Kind) {
    case Token:
      OS << "Tok: " << Tok;
      break;
    case Register:
      OS << "Reg: " << Reg.RegNum;
      break;
    case Immediate:
      OS << "Imm: ";
      MAI.printExpr(OS, *Imm.Val);
      break;
    case Memory:
      OS << "Mem: " << Mem.BaseReg << " + ";
      MAI.printExpr(OS, *Mem.Offset);
      break;
    case DestMask:
      OS << "Dest: " << Dest.Mask;
      break;
    case Broadcast:
      OS << "BC: " << BC.Field;
      break;
    case FieldSel:
      OS << "Field: " << BC.Field;
      break;
    }
  }

  static std::unique_ptr<DVPVUOperand> createToken(StringRef Str, SMLoc S) {
    auto Op = std::make_unique<DVPVUOperand>(Token);
    Op->Tok = Str;
    Op->StartLoc = S;
    Op->EndLoc = S;
    return Op;
  }

  static std::unique_ptr<DVPVUOperand> createReg(MCRegister RegNo, SMLoc S,
                                                  SMLoc E) {
    auto Op = std::make_unique<DVPVUOperand>(Register);
    Op->Reg.RegNum = RegNo;
    Op->StartLoc = S;
    Op->EndLoc = E;
    return Op;
  }

  static std::unique_ptr<DVPVUOperand> createImm(const MCExpr *Val, SMLoc S,
                                                  SMLoc E) {
    auto Op = std::make_unique<DVPVUOperand>(Immediate);
    Op->Imm.Val = Val;
    Op->StartLoc = S;
    Op->EndLoc = E;
    return Op;
  }

  static std::unique_ptr<DVPVUOperand> createMem(MCRegister BaseReg,
                                                  const MCExpr *Offset,
                                                  SMLoc S, SMLoc E) {
    auto Op = std::make_unique<DVPVUOperand>(Memory);
    Op->Mem.BaseReg = BaseReg;
    Op->Mem.Offset = Offset;
    Op->StartLoc = S;
    Op->EndLoc = E;
    return Op;
  }

  static std::unique_ptr<DVPVUOperand> createDestMask(unsigned Mask, SMLoc S,
                                                       SMLoc E) {
    auto Op = std::make_unique<DVPVUOperand>(DestMask);
    Op->Dest.Mask = Mask;
    Op->StartLoc = S;
    Op->EndLoc = E;
    return Op;
  }

  static std::unique_ptr<DVPVUOperand> createBroadcast(unsigned Field, SMLoc S,
                                                        SMLoc E) {
    auto Op = std::make_unique<DVPVUOperand>(Broadcast);
    Op->BC.Field = Field;
    Op->StartLoc = S;
    Op->EndLoc = E;
    return Op;
  }

  static std::unique_ptr<DVPVUOperand> createFieldSel(unsigned Field, SMLoc S,
                                                       SMLoc E) {
    auto Op = std::make_unique<DVPVUOperand>(FieldSel);
    Op->BC.Field = Field;
    Op->StartLoc = S;
    Op->EndLoc = E;
    return Op;
  }
};

class DVPVUAsmParser : public MCTargetAsmParser {
  MCAsmParser &Parser;
  const MCRegisterInfo *MRI;

#define GET_ASSEMBLER_HEADER
#include "DVPVUGenAsmMatcher.inc"

  bool matchAndEmitInstruction(SMLoc IDLoc, unsigned &Opcode,
                               OperandVector &Operands, MCStreamer &Out,
                               uint64_t &ErrorInfo,
                               bool MatchingInlineAsm) override;

  bool parseRegister(MCRegister &Reg, SMLoc &StartLoc, SMLoc &EndLoc) override;
  ParseStatus tryParseRegister(MCRegister &Reg, SMLoc &StartLoc,
                               SMLoc &EndLoc) override;

  bool parseInstruction(ParseInstructionInfo &Info, StringRef Name,
                        SMLoc NameLoc, OperandVector &Operands) override;

  ParseStatus parseDirective(AsmToken DirectiveID) override;

  ParseStatus parseOperand(OperandVector &Operands);
  ParseStatus parseVFRegister(OperandVector &Operands);
  ParseStatus parseVIRegister(OperandVector &Operands);
  ParseStatus parseDestMask(OperandVector &Operands);
  ParseStatus parseBroadcast(OperandVector &Operands);
  ParseStatus parseMemoryOperand(OperandVector &Operands);
  ParseStatus parseImmediate(OperandVector &Operands);
  ParseStatus parseImmediateOrMemory(OperandVector &Operands);
  ParseStatus parseMemVI(OperandVector &Operands);
  ParseStatus parseField(OperandVector &Operands);

  MCRegister matchRegisterName(StringRef Name);
  unsigned parseDestMaskString(StringRef Mask);

public:
  DVPVUAsmParser(const MCSubtargetInfo &STI, MCAsmParser &Parser,
                  const MCInstrInfo &MII, const MCTargetOptions &Options)
      : MCTargetAsmParser(Options, STI, MII), Parser(Parser) {
    MCAsmParserExtension::Initialize(Parser);
    MRI = getContext().getRegisterInfo();
    setAvailableFeatures(ComputeAvailableFeatures(STI.getFeatureBits()));
  }
};

} // end anonymous namespace

#define GET_REGISTER_MATCHER
#define GET_MATCHER_IMPLEMENTATION
#include "DVPVUGenAsmMatcher.inc"

bool DVPVUAsmParser::matchAndEmitInstruction(SMLoc IDLoc, unsigned &Opcode,
                                             OperandVector &Operands,
                                             MCStreamer &Out,
                                             uint64_t &ErrorInfo,
                                             bool MatchingInlineAsm) {
  MCInst Inst;
  unsigned MatchResult =
      MatchInstructionImpl(Operands, Inst, ErrorInfo, MatchingInlineAsm);

  switch (MatchResult) {
  case Match_Success:
    Out.emitInstruction(Inst, getSTI());
    return false;
  case Match_MissingFeature:
    return Error(IDLoc, "instruction requires a CPU feature not currently enabled");
  case Match_InvalidOperand: {
    SMLoc ErrorLoc = IDLoc;
    if (ErrorInfo != ~0ULL && ErrorInfo < Operands.size()) {
      ErrorLoc = Operands[ErrorInfo]->getStartLoc();
      if (ErrorLoc == SMLoc())
        ErrorLoc = IDLoc;
    }
    return Error(ErrorLoc, "invalid operand for instruction");
  }
  case Match_MnemonicFail:
    return Error(IDLoc, "unrecognized instruction mnemonic");
  default:
    return Error(IDLoc, "unknown match error");
  }
}

bool DVPVUAsmParser::parseRegister(MCRegister &Reg, SMLoc &StartLoc,
                                    SMLoc &EndLoc) {
  if (!tryParseRegister(Reg, StartLoc, EndLoc).isSuccess())
    return Error(StartLoc, "expected register");
  return false;
}

ParseStatus DVPVUAsmParser::tryParseRegister(MCRegister &Reg, SMLoc &StartLoc,
                                              SMLoc &EndLoc) {
  const AsmToken &Tok = Parser.getTok();
  StartLoc = Tok.getLoc();
  EndLoc = Tok.getEndLoc();

  if (Tok.isNot(AsmToken::Identifier))
    return ParseStatus::NoMatch;

  Reg = matchRegisterName(Tok.getString());
  if (!Reg)
    return ParseStatus::NoMatch;

  Parser.Lex();
  return ParseStatus::Success;
}

MCRegister DVPVUAsmParser::matchRegisterName(StringRef Name) {
  // Convert to lowercase for matching
  std::string LowerStr = Name.lower();
  StringRef LowerName(LowerStr);

  // Match VF registers (vf0-vf31)
  if (LowerName.starts_with("vf")) {
    StringRef NumStr = LowerName.substr(2);
    unsigned RegNum;
    if (!NumStr.getAsInteger(10, RegNum) && RegNum < 32) {
      return DVPVU::VF0 + RegNum;
    }
  }

  // Match VI registers (vi0-vi15)
  if (LowerName.starts_with("vi")) {
    StringRef NumStr = LowerName.substr(2);
    unsigned RegNum;
    if (!NumStr.getAsInteger(10, RegNum) && RegNum < 16) {
      return DVPVU::VI0 + RegNum;
    }
  }

  // Match special registers
  return StringSwitch<MCRegister>(LowerName)
      .Case("acc", DVPVU::ACC)
      .Case("i", DVPVU::I_REG)
      .Case("q", DVPVU::Q)
      .Case("p", DVPVU::P)
      .Case("r", DVPVU::R)
      .Default(MCRegister());
}

unsigned DVPVUAsmParser::parseDestMaskString(StringRef Mask) {
  unsigned Result = 0;
  for (char C : Mask) {
    switch (C) {
    case 'x': case 'X': Result |= 0x8; break;
    case 'y': case 'Y': Result |= 0x4; break;
    case 'z': case 'Z': Result |= 0x2; break;
    case 'w': case 'W': Result |= 0x1; break;
    default: return 0;
    }
  }
  return Result;
}

bool DVPVUAsmParser::parseInstruction(ParseInstructionInfo &Info,
                                       StringRef Name, SMLoc NameLoc,
                                       OperandVector &Operands) {
  // Check for dest mask suffix (e.g., add.xyzw)
  StringRef Mnemonic = Name;
  StringRef DestSuffix;

  size_t DotPos = Name.find('.');
  if (DotPos != StringRef::npos) {
    Mnemonic = Name.substr(0, DotPos);
    DestSuffix = Name.substr(DotPos + 1);
  }

  // Add the mnemonic as a token
  Operands.push_back(DVPVUOperand::createToken(Mnemonic, NameLoc));

  // If there's a dest mask, add it as an operand
  if (!DestSuffix.empty()) {
    unsigned Mask = parseDestMaskString(DestSuffix);
    if (Mask == 0) {
      return Error(NameLoc, "invalid destination mask");
    }
    Operands.push_back(DVPVUOperand::createDestMask(Mask, NameLoc, NameLoc));
  }

  // Parse operands
  while (getLexer().isNot(AsmToken::EndOfStatement)) {
    // Skip comma
    if (getLexer().is(AsmToken::Comma))
      Parser.Lex();

    auto Result = parseOperand(Operands);
    if (!Result.isSuccess()) {
      return true;
    }
  }

  // Consume the end of statement
  Parser.Lex();
  return false;
}

ParseStatus DVPVUAsmParser::parseOperand(OperandVector &Operands) {
  // Try register first
  ParseStatus Result = parseVFRegister(Operands);
  if (Result.isSuccess() || Result.isFailure())
    return Result;

  Result = parseVIRegister(Operands);
  if (Result.isSuccess() || Result.isFailure())
    return Result;

  // Check if this looks like a memory operand: either (reg) or expr(reg)
  // We need to be careful not to consume tokens we can't handle
  if (getLexer().is(AsmToken::LParen)) {
    // This is (reg) form
    Result = parseMemoryOperand(Operands);
    if (Result.isSuccess() || Result.isFailure())
      return Result;
  }

  // Try immediate (this may also be a memory operand if followed by '(')
  Result = parseImmediateOrMemory(Operands);
  if (Result.isSuccess() || Result.isFailure())
    return Result;

  return ParseStatus::NoMatch;
}

ParseStatus DVPVUAsmParser::parseVFRegister(OperandVector &Operands) {
  const AsmToken &Tok = Parser.getTok();
  SMLoc StartLoc = Tok.getLoc();

  if (Tok.isNot(AsmToken::Identifier))
    return ParseStatus::NoMatch;

  StringRef Name = Tok.getString();
  std::string LowerStr = Name.lower();
  StringRef LowerName(LowerStr);

  if (!LowerName.starts_with("vf"))
    return ParseStatus::NoMatch;

  // Parse register number, possibly with a trailing field selector
  StringRef NumPart = LowerName.substr(2);

  // Check for trailing field selector (x, y, z, w)
  unsigned Field = 4; // 4 = no field
  if (NumPart.size() > 0) {
    char LastChar = NumPart.back();
    if (LastChar == 'x') { Field = 0; NumPart = NumPart.drop_back(); }
    else if (LastChar == 'y') { Field = 1; NumPart = NumPart.drop_back(); }
    else if (LastChar == 'z') { Field = 2; NumPart = NumPart.drop_back(); }
    else if (LastChar == 'w') { Field = 3; NumPart = NumPart.drop_back(); }
  }

  unsigned RegNum;
  if (NumPart.getAsInteger(10, RegNum) || RegNum >= 32)
    return ParseStatus::Failure;

  MCRegister Reg = DVPVU::VF0 + RegNum;
  SMLoc EndLoc = Tok.getEndLoc();
  Parser.Lex();

  // Check for separate field selector token (e.g., "vf1 x")
  if (Field == 4 && getLexer().is(AsmToken::Identifier)) {
    StringRef Suffix = getLexer().getTok().getString();
    if (Suffix.size() == 1) {
      char C = Suffix[0];
      if (C == 'x' || C == 'X') Field = 0;
      else if (C == 'y' || C == 'Y') Field = 1;
      else if (C == 'z' || C == 'Z') Field = 2;
      else if (C == 'w' || C == 'W') Field = 3;
      if (Field < 4) {
        EndLoc = getLexer().getTok().getEndLoc();
        Parser.Lex();
      }
    }
  }

  Operands.push_back(DVPVUOperand::createReg(Reg, StartLoc, EndLoc));
  if (Field < 4) {
    Operands.push_back(DVPVUOperand::createBroadcast(Field, StartLoc, EndLoc));
  }
  return ParseStatus::Success;
}

ParseStatus DVPVUAsmParser::parseVIRegister(OperandVector &Operands) {
  const AsmToken &Tok = Parser.getTok();
  SMLoc StartLoc = Tok.getLoc();

  if (Tok.isNot(AsmToken::Identifier))
    return ParseStatus::NoMatch;

  StringRef Name = Tok.getString();
  std::string LowerStr = Name.lower();
  StringRef LowerName(LowerStr);

  if (!LowerName.starts_with("vi") && LowerName != "acc" &&
      LowerName != "i" && LowerName != "q" && LowerName != "p" &&
      LowerName != "r")
    return ParseStatus::NoMatch;

  MCRegister Reg = matchRegisterName(Name);
  if (!Reg)
    return ParseStatus::Failure;

  SMLoc EndLoc = Tok.getEndLoc();
  Parser.Lex();

  Operands.push_back(DVPVUOperand::createReg(Reg, StartLoc, EndLoc));
  return ParseStatus::Success;
}

ParseStatus DVPVUAsmParser::parseMemoryOperand(OperandVector &Operands) {
  // Parse: offset(base_reg)
  SMLoc StartLoc = Parser.getTok().getLoc();

  const MCExpr *Offset = nullptr;
  if (getLexer().isNot(AsmToken::LParen)) {
    // Parse offset
    if (Parser.parseExpression(Offset))
      return ParseStatus::Failure;
  } else {
    // No offset, default to 0
    Offset = MCConstantExpr::create(0, getContext());
  }

  if (getLexer().isNot(AsmToken::LParen))
    return ParseStatus::NoMatch;

  Parser.Lex(); // Consume '('

  MCRegister BaseReg;
  SMLoc RegStart, RegEnd;
  if (!tryParseRegister(BaseReg, RegStart, RegEnd).isSuccess())
    return ParseStatus::Failure;

  if (getLexer().isNot(AsmToken::RParen))
    return Error(Parser.getTok().getLoc(), "expected ')'");

  SMLoc EndLoc = Parser.getTok().getEndLoc();
  Parser.Lex(); // Consume ')'

  Operands.push_back(DVPVUOperand::createMem(BaseReg, Offset, StartLoc, EndLoc));
  return ParseStatus::Success;
}

ParseStatus DVPVUAsmParser::parseImmediate(OperandVector &Operands) {
  SMLoc StartLoc = Parser.getTok().getLoc();

  const MCExpr *Expr;
  if (Parser.parseExpression(Expr))
    return ParseStatus::Failure;

  SMLoc EndLoc = Parser.getTok().getLoc();
  Operands.push_back(DVPVUOperand::createImm(Expr, StartLoc, EndLoc));
  return ParseStatus::Success;
}

ParseStatus DVPVUAsmParser::parseImmediateOrMemory(OperandVector &Operands) {
  // This function parses an expression and checks if it's followed by '('
  // If so, it's a memory operand: expr(base_reg)
  // Otherwise, it's just an immediate
  SMLoc StartLoc = Parser.getTok().getLoc();

  const MCExpr *Expr;
  if (Parser.parseExpression(Expr))
    return ParseStatus::Failure;

  // Check if this is a memory operand (followed by '(')
  if (getLexer().is(AsmToken::LParen)) {
    Parser.Lex(); // Consume '('

    MCRegister BaseReg;
    SMLoc RegStart, RegEnd;
    if (!tryParseRegister(BaseReg, RegStart, RegEnd).isSuccess())
      return ParseStatus::Failure;

    if (getLexer().isNot(AsmToken::RParen))
      return Error(Parser.getTok().getLoc(), "expected ')'");

    SMLoc EndLoc = Parser.getTok().getEndLoc();
    Parser.Lex(); // Consume ')'

    Operands.push_back(DVPVUOperand::createMem(BaseReg, Expr, StartLoc, EndLoc));
    return ParseStatus::Success;
  }

  // It's just an immediate
  SMLoc EndLoc = Parser.getTok().getLoc();
  Operands.push_back(DVPVUOperand::createImm(Expr, StartLoc, EndLoc));
  return ParseStatus::Success;
}

ParseStatus DVPVUAsmParser::parseMemVI(OperandVector &Operands) {
  return parseMemoryOperand(Operands);
}

ParseStatus DVPVUAsmParser::parseField(OperandVector &Operands) {
  const AsmToken &Tok = Parser.getTok();
  SMLoc StartLoc = Tok.getLoc();

  if (Tok.isNot(AsmToken::Identifier))
    return ParseStatus::NoMatch;

  StringRef Name = Tok.getString();
  if (Name.size() != 1)
    return ParseStatus::NoMatch;

  unsigned Field = StringSwitch<unsigned>(Name.lower())
      .Case("x", 0)
      .Case("y", 1)
      .Case("z", 2)
      .Case("w", 3)
      .Default(4);

  if (Field >= 4)
    return ParseStatus::NoMatch;

  SMLoc EndLoc = Tok.getEndLoc();
  Parser.Lex();

  Operands.push_back(DVPVUOperand::createFieldSel(Field, StartLoc, EndLoc));
  return ParseStatus::Success;
}

ParseStatus DVPVUAsmParser::parseDestMask(OperandVector &Operands) {
  // Already parsed in parseInstruction from mnemonic suffix
  return ParseStatus::NoMatch;
}

ParseStatus DVPVUAsmParser::parseBroadcast(OperandVector &Operands) {
  // Already parsed as part of register in parseVFRegister
  return ParseStatus::NoMatch;
}

ParseStatus DVPVUAsmParser::parseDirective(AsmToken DirectiveID) {
  // No target-specific directives yet
  return ParseStatus::NoMatch;
}

extern "C" LLVM_EXTERNAL_VISIBILITY void LLVMInitializeDVPVUAsmParser() {
  RegisterMCAsmParser<DVPVUAsmParser> X(getTheDVPVUTarget());
}

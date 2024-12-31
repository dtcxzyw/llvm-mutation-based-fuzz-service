// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024 Yingwei Zheng
// This file is licensed under the Apache-2.0 License.
// See the LICENSE file for more information.

#include <llvm/ADT/APFloat.h>
#include <llvm/ADT/APInt.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/STLExtras.h>
#include <llvm/ADT/STLFunctionalExtras.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Analysis/InstructionSimplify.h>
#include <llvm/IR/Argument.h>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GEPNoWrapFlags.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstVisitor.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Operator.h>
#include <llvm/IR/PassInstrumentation.h>
#include <llvm/IR/PatternMatch.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Value.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IRPrinter/IRPrintingPasses.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/FormattedStream.h>
#include <llvm/Support/InitLLVM.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/ToolOutputFile.h>
#include <llvm/Support/raw_ostream.h>
#include <cstdlib>
#include <random>
#include <string>

using namespace llvm;
using namespace PatternMatch;

static cl::opt<std::string> SeedFile(cl::Positional, cl::desc("<seed>"),
                                     cl::Required, cl::value_desc("seed file"));
static cl::opt<std::string> OutputFile(cl::Positional, cl::desc("<output>"),
                                       cl::Required,
                                       cl::value_desc("output file"));
static cl::opt<std::string> Recipe(cl::Positional, cl::desc("<recipe>"),
                                   cl::Required, cl::value_desc("recipe"));

std::mt19937_64 Gen(std::random_device{}());
bool randomBool() { return std::uniform_int_distribution<>{0, 1}(Gen); }
uint32_t randomUInt(uint32_t Max) {
  return std::uniform_int_distribution<uint32_t>{0, Max}(Gen);
}
int32_t randomInt(int32_t Min, int32_t Max) {
  return std::uniform_int_distribution<int32_t>{Min, Max}(Gen);
}
int32_t randomIntNotEqual(int32_t Min, int32_t Max, int32_t NotEqual) {
  while (true) {
    int32_t Value = randomInt(Min, Max);
    if (Value != NotEqual)
      return Value;
  }
}
// Mutators

bool mutateConstant(Instruction &I) {
  for (auto &Op : I.operands()) {
    if (!isa<Constant>(Op.get()))
      continue;
    if (randomBool())
      continue;
    const APInt *C;
    if (match(Op.get(), m_APInt(C))) {
      switch (randomUInt(3)) {
      case 0: {
        // Special values
        switch (randomUInt(4)) {
        case 0:
          Op.set(ConstantInt::get(Op->getType(), 0));
          break;
        case 1:
          Op.set(ConstantInt::get(Op->getType(), 1));
          break;
        case 2:
          Op.set(ConstantInt::get(Op->getType(), -1, /*IsSigned=*/true));
          break;
        case 3:
          Op.set(ConstantInt::get(Op->getType(),
                                  APInt::getSignedMaxValue(C->getBitWidth())));
          break;
        case 4:
          Op.set(ConstantInt::get(Op->getType(),
                                  APInt::getSignedMinValue(C->getBitWidth())));
          break;
        }
        break;
      }
      case 1: {
        // Negate
        Op.set(ConstantInt::get(Op->getType(), -(*C)));
        break;
      }
      case 2: {
        // Inversion
        Op.set(ConstantInt::get(Op->getType(), ~(*C)));
        break;
      }
      case 3: {
        // Random value
        if (C->getBitWidth() < 64)
          return false;
        Op.set(ConstantInt::get(
            Op->getType(),
            APInt(C->getBitWidth(),
                  std::uniform_int_distribution<uint64_t>{0}(Gen))));
        break;
      }
      }
      return true;
    }
  }
  return false;
}
bool mutateFlags(Instruction &I, bool Add) {
  if (auto *OBO = dyn_cast<OverflowingBinaryOperator>(&I)) {
    if (Add) {
      if (randomBool()) {
        if (!OBO->hasNoUnsignedWrap()) {
          I.setHasNoUnsignedWrap();
          return true;
        }
      } else {
        if (!OBO->hasNoSignedWrap()) {
          I.setHasNoSignedWrap();
          return true;
        }
      }
    } else {
      if (randomBool()) {
        if (OBO->hasNoUnsignedWrap()) {
          I.setHasNoUnsignedWrap(false);
          return true;
        }
      } else {
        if (OBO->hasNoSignedWrap()) {
          I.setHasNoSignedWrap(false);
          return true;
        }
      }
    }
  }
  if (auto *Exact = dyn_cast<PossiblyExactOperator>(&I)) {
    if (Add) {
      if (!Exact->isExact()) {
        I.setIsExact();
        return true;
      }
    } else {
      if (Exact->isExact()) {
        I.setIsExact(false);
        return true;
      }
    }
  }
  if (auto *GEP = dyn_cast<GetElementPtrInst>(&I)) {
    if (Add) {
      switch (randomUInt(2)) {
      case 0:
        if (!GEP->isInBounds()) {
          GEP->setIsInBounds(true);
          return true;
        }
        break;
      case 1:
        if (!GEP->getNoWrapFlags().hasNoUnsignedWrap()) {
          GEP->setNoWrapFlags(GEP->getNoWrapFlags() |
                              GEPNoWrapFlags::noUnsignedWrap());
          return true;
        }
        break;
      case 2:
        if (!GEP->getNoWrapFlags().hasNoUnsignedSignedWrap()) {
          GEP->setNoWrapFlags(GEP->getNoWrapFlags() |
                              GEPNoWrapFlags::noUnsignedSignedWrap());
          return true;
        }
        break;
      }
    } else {
      switch (randomUInt(2)) {
      case 0:
        if (GEP->isInBounds()) {
          GEP->setIsInBounds(false);
          return true;
        }
        break;
      case 1:
        if (GEP->getNoWrapFlags().hasNoUnsignedWrap()) {
          GEP->setNoWrapFlags(GEP->getNoWrapFlags().withoutNoUnsignedWrap());
          return true;
        }
        break;
      case 2:
        if (GEP->getNoWrapFlags().hasNoUnsignedSignedWrap()) {
          GEP->setNoWrapFlags(
              GEP->getNoWrapFlags().withoutNoUnsignedSignedWrap());
          return true;
        }
        break;
      }
    }
  }
  if (auto *Trunc = dyn_cast<TruncInst>(&I)) {
    if (Add) {
      if (randomBool()) {
        if (!Trunc->hasNoUnsignedWrap()) {
          I.setHasNoUnsignedWrap();
          return true;
        }
      } else {
        if (!Trunc->hasNoSignedWrap()) {
          I.setHasNoSignedWrap();
          return true;
        }
      }
    } else {
      if (randomBool()) {
        if (Trunc->hasNoUnsignedWrap()) {
          I.setHasNoUnsignedWrap(false);
          return true;
        }
      } else {
        if (Trunc->hasNoSignedWrap()) {
          I.setHasNoSignedWrap(false);
          return true;
        }
      }
    }
  }
  if (auto *Disjoint = dyn_cast<PossiblyDisjointInst>(&I)) {
    if (Add) {
      if (!Disjoint->isDisjoint()) {
        Disjoint->setIsDisjoint(true);
        return true;
      }
    } else {
      if (Disjoint->isDisjoint()) {
        Disjoint->setIsDisjoint(false);
        return true;
      }
    }
  }
  if (auto *NNeg = dyn_cast<PossiblyNonNegInst>(&I)) {
    if (Add) {
      if (!NNeg->hasNonNeg()) {
        NNeg->setNonNeg();
        return true;
      }
    } else {
      if (NNeg->hasNonNeg()) {
        NNeg->setNonNeg(false);
        return true;
      }
    }
  }
  if (auto *ICmp = dyn_cast<ICmpInst>(&I)) {
    if (Add) {
      if (!ICmp->hasSameSign()) {
        ICmp->setSameSign();
        return true;
      }
    } else {
      if (ICmp->hasSameSign()) {
        ICmp->setSameSign(false);
        return true;
      }
    }
  }
  if (auto *FPOp = dyn_cast<FPMathOperator>(&I)) {
    if (Add) {
      switch (randomUInt(2)) {
      case 0:
        if (!FPOp->hasNoInfs()) {
          I.setHasNoInfs(true);
          return true;
        }
        break;
      case 1:
        if (!FPOp->hasNoNaNs()) {
          I.setHasNoNaNs(true);
          return true;
        }
        break;
      case 2:
        if (!FPOp->hasNoSignedZeros()) {
          I.setHasNoSignedZeros(true);
          return true;
        }
        break;
      }
    } else {
      switch (randomUInt(2)) {
      case 0:
        if (FPOp->hasNoInfs()) {
          I.setHasNoInfs(false);
          return true;
        }
        break;
      case 1:
        if (FPOp->hasNoNaNs()) {
          I.setHasNoNaNs(false);
          return true;
        }
        break;
      case 2:
        if (FPOp->hasNoSignedZeros()) {
          I.setHasNoSignedZeros(false);
          return true;
        }
        break;
      }
    }
  }
  if (auto *II = dyn_cast<IntrinsicInst>(&I)) {
    if (II->getType()->isIntOrIntVectorTy() && randomBool()) {
      // ret attr
      if (Add) {
        if (!II->hasRetAttr(Attribute::NoUndef)) {
          II->addRetAttr(Attribute::NoUndef);
          return true;
        }
      } else {
        if (II->hasRetAttr(Attribute::NoUndef)) {
          II->removeRetAttr(Attribute::NoUndef);
          return true;
        }
      }
    } else {
      switch (II->getIntrinsicID()) {
      case Intrinsic::abs:
      case Intrinsic::ctlz:
      case Intrinsic::cttz:
        if (Add == cast<Constant>(II->getArgOperand(1))->isNullValue()) {
          II->setArgOperand(
              1, ConstantInt::getBool(II->getArgOperand(1)->getType(), Add));
          return true;
        }
      default:
        break;
      }
    }
  }
  return false;
}
bool addFlags(Instruction &I) { return mutateFlags(I, /*Add=*/true); }
bool dropFlags(Instruction &I) { return mutateFlags(I, /*Add=*/false); }
bool createNewInst(Instruction &Old, function_ref<Value *(IRBuilder<> &)> New) {
  IRBuilder<> Builder(&Old);
  Old.replaceAllUsesWith(New(Builder));
  Old.eraseFromParent();
  return true;
}
bool mutateOpcode(Instruction &I) {
  if (auto *ICmp = dyn_cast<ICmpInst>(&I)) {
    ICmp->setPredicate(static_cast<ICmpInst::Predicate>(randomIntNotEqual(
        ICmpInst::FIRST_ICMP_PREDICATE, ICmpInst::LAST_ICMP_PREDICATE,
        ICmp->getPredicate())));
    return true;
  }
  if (auto *FCmp = dyn_cast<FCmpInst>(&I)) {
    FCmp->setPredicate(static_cast<FCmpInst::Predicate>(randomIntNotEqual(
        FCmpInst::FIRST_FCMP_PREDICATE, FCmpInst::LAST_FCMP_PREDICATE,
        FCmp->getPredicate())));
    return true;
  }
  // logical and/or <-> bitwise and/or
  if (auto *SI = dyn_cast<SelectInst>(&I)) {
    if (SI->getType()->isIntOrIntVectorTy(1) &&
        SI->getType() == SI->getTrueValue()->getType()) {
      if (match(SI->getTrueValue(), m_One()))
        return createNewInst(I, [&](IRBuilder<> &Builder) {
          return Builder.CreateOr(SI->getCondition(), SI->getFalseValue());
        });

      if (match(SI->getFalseValue(), m_Zero()))
        return createNewInst(I, [&](IRBuilder<> &Builder) {
          return Builder.CreateAnd(SI->getCondition(), SI->getTrueValue());
        });
    }
  }
  if (I.getType()->isIntOrIntVectorTy(1)) {
    if (I.getOpcode() == Instruction::And)
      return createNewInst(I, [&](IRBuilder<> &Builder) {
        return Builder.CreateLogicalAnd(I.getOperand(0), I.getOperand(1));
      });
    if (I.getOpcode() == Instruction::Or)
      return createNewInst(I, [&](IRBuilder<> &Builder) {
        return Builder.CreateLogicalOr(I.getOperand(0), I.getOperand(1));
      });
  }
  // lshr <-> ashr
  if (I.getOpcode() == Instruction::LShr)
    return createNewInst(I, [&](IRBuilder<> &Builder) {
      return Builder.CreateAShr(I.getOperand(0), I.getOperand(1), I.getName(),
                                I.isExact());
    });
  if (I.getOpcode() == Instruction::AShr)
    return createNewInst(I, [&](IRBuilder<> &Builder) {
      return Builder.CreateLShr(I.getOperand(0), I.getOperand(1), I.getName(),
                                I.isExact());
    });
  // sext <-> zext
  if (I.getOpcode() == Instruction::SExt)
    return createNewInst(I, [&](IRBuilder<> &Builder) {
      return Builder.CreateZExt(I.getOperand(0), I.getType(), I.getName());
    });
  if (I.getOpcode() == Instruction::ZExt)
    return createNewInst(I, [&](IRBuilder<> &Builder) {
      return Builder.CreateSExt(I.getOperand(0), I.getType(), I.getName());
    });
  // and/or/xor
  if (I.isBitwiseLogicOp())
    return createNewInst(I, [&](IRBuilder<> &Builder) {
      return Builder.CreateBinOp(
          static_cast<Instruction::BinaryOps>(randomIntNotEqual(
              Instruction::And, Instruction::Xor, I.getOpcode())),
          I.getOperand(0), I.getOperand(1), I.getName());
    });
  // [s|u]max/min
  // [s|u]cmp
  return false;
}
bool canonicalizeOp(Instruction &I) {
  switch (I.getOpcode()) {
  // sext -> zext nneg
  case Instruction::SExt:
    return createNewInst(I, [&](IRBuilder<> &Builder) {
      return Builder.CreateZExt(I.getOperand(0), I.getType(), I.getName(),
                                /*IsNonNeg=*/true);
    });
  // sitofp -> uitofp nneg
  case Instruction::SIToFP:
    return createNewInst(I, [&](IRBuilder<> &Builder) {
      return Builder.CreateUIToFP(I.getOperand(0), I.getType(), I.getName(),
                                  /*IsNonNeg=*/true);
    });
  // xor/add -> or disjoint
  case Instruction::Xor:
  case Instruction::Add:
    return createNewInst(I, [&](IRBuilder<> &Builder) {
      auto *Val =
          Builder.CreateOr(I.getOperand(0), I.getOperand(1), I.getName());
      if (auto *Or = dyn_cast<PossiblyDisjointInst>(Val))
        Or->setIsDisjoint(true);
      return Val;
    });
  // icmp spred -> icmp samesign upred
  case Instruction::ICmp: {
    auto *Cmp = cast<ICmpInst>(&I);
    if (Cmp->isUnsigned()) {
      Cmp->setSameSign(true);
      Cmp->setPredicate(Cmp->getUnsignedPredicate());
      return true;
    }
    break;
  }
  // fcmp unordered -> fcmp nnan ordered
  case Instruction::FCmp: {
    auto *Cmp = cast<FCmpInst>(&I);
    if (FCmpInst::isUnordered(Cmp->getPredicate())) {
      Cmp->setHasNoNaNs(true);
      Cmp->setPredicate(Cmp->getOrderedPredicate());
      return true;
    }
    break;
  }
  // logical -> bitwise
  case Instruction::Select: {
    Value *X, *Y;
    if (match(&I, m_LogicalAnd(m_Value(X), m_Value(Y))))
      return createNewInst(I, [&](IRBuilder<> &Builder) {
        return Builder.CreateAnd(X, Y, I.getName());
      });
    if (match(&I, m_LogicalOr(m_Value(X), m_Value(Y))))
      return createNewInst(I, [&](IRBuilder<> &Builder) {
        return Builder.CreateOr(X, Y, I.getName());
      });
    break;
  }
  default:
    break;
  }
  return false;
}
bool commuteOperands(Instruction &I) {
  if (auto *BI = dyn_cast<BranchInst>(&I)) {
    if (BI->isConditional()) {
      BI->swapSuccessors();
      return true;
    }
    return false;
  }
  if (auto *SI = dyn_cast<SelectInst>(&I)) {
    if (match(SI, m_LogicalOp(m_Value(), m_Value())))
      return false;
    SI->swapValues();
    return true;
  }
  if (I.getNumOperands() < 2)
    return false;
  if (isa<PHINode>(&I))
    return false;
  if (I.getOperand(0)->getType() != I.getOperand(1)->getType())
    return false;
  I.getOperandUse(0).swap(I.getOperandUse(1));
  return true;
}
bool commuteOperandsOfCommutativeInst(Instruction &I) {
  if (I.getNumOperands() < 2)
    return false;
  if (auto *SI = dyn_cast<SelectInst>(&I)) {
    if (match(SI, m_LogicalOp(m_Value(), m_Value())))
      return false;
    Value *X;
    if (match(SI->getCondition(), m_Not(m_Value(X))))
      SI->setCondition(X);
    else if (auto *Cmp = dyn_cast<CmpInst>(SI->getCondition()))
      Cmp->setPredicate(Cmp->getInversePredicate());
    else
      return false;
    SI->swapValues();
    return true;
  }
  if (isa<Constant>(I.getOperand(1)))
    return false;
  if (auto *Cmp = dyn_cast<CmpInst>(&I)) {
    Cmp->swapOperands();
    return true;
  }
  if (!I.isCommutative())
    return false;
  I.getOperandUse(0).swap(I.getOperandUse(1));
  return true;
}
std::string getTypeName(Type *Ty) {
  if (Ty->isIntegerTy())
    return "i" + std::to_string(Ty->getScalarSizeInBits());
  if (Ty->isFloatTy())
    return "f32";
  if (Ty->isDoubleTy())
    return "f64";
  if (Ty->isHalfTy())
    return "f16";
  if (Ty->isBFloatTy())
    return "bf16";
  if (Ty->isPointerTy())
    return "ptr";
  if (auto *Vec = dyn_cast<FixedVectorType>(Ty)) {
    auto Sub = getTypeName(Vec->getElementType());
    if (Sub.empty())
      return "";
    return std::to_string(Vec->getNumElements()) + "x" + Sub;
  }
  return "";
}
bool breakOneUse(Instruction &I) {
  if (!I.hasOneUse())
    return false;
  if (!I.getType()->isSingleValueType())
    return false;
  if (I.isTerminator())
    return false;
  if (isa<PHINode>(&I))
    return false;

  auto *Ty = I.getType();
  auto TyName = getTypeName(Ty);
  auto *M = I.getModule();
  auto Callee = M->getOrInsertFunction(
      "fuzz_use_" + TyName,
      FunctionType::get(Type::getVoidTy(M->getContext()), {Ty}, false));
  IRBuilder<> Builder(I.getNextNode());
  Builder.CreateCall(Callee, &I);
  return true;
}
bool mutateArgAttr(Argument &Arg) {
  switch (randomUInt(1)) {
  case 0:
    if (Arg.getType()->isPointerTy()) {
      if (Arg.hasNonNullAttr())
        Arg.removeAttr(Attribute::NonNull);
      else
        Arg.addAttr(Attribute::NonNull);
      return true;
    }
    break;
  case 1:
    if (Arg.hasAttribute(Attribute::NoUndef))
      Arg.removeAttr(Attribute::NoUndef);
    else
      Arg.addAttr(Attribute::NoUndef);
    return true;
  }
  return false;
}
bool replaceArgUse(Instruction &I) {
  SmallVector<Use *> Uses;
  for (auto &Op : I.operands())
    if (isa<Argument>(Op) && !Op->hasOneUse())
      Uses.push_back(&Op);
  if (Uses.empty())
    return false;
  auto &Op = *Uses[randomUInt(Uses.size() - 1)];
  SmallVector<Argument *> Replacements;
  for (auto &Arg : I.getFunction()->args())
    if (Arg.getType() == Op->getType() && &Arg != Op.get())
      Replacements.push_back(&Arg);
  if (Replacements.empty())
    return false;
  Op->replaceAllUsesWith(Replacements[randomUInt(Replacements.size() - 1)]);
  return true;
}

// Recipes
bool mutateInst(Instruction &I) {
  switch (randomUInt(5)) {
  case 0:
    return mutateConstant(I);
  case 1:
    return addFlags(I);
  case 2:
    return dropFlags(I);
  case 3:
    return mutateOpcode(I);
  case 4:
    return commuteOperands(I);
  case 5:
    return replaceArgUse(I);
  }
  llvm_unreachable("Unreachable code");
}
constexpr uint32_t MaxIterFactor = 100;

bool correctnessCheck(Function &F) {
  uint32_t MutationCount = randomInt(1, 5);
  uint32_t MutationIter = 0;
  uint32_t MaxIter = MutationCount * MaxIterFactor;

  for (uint32_t I = 0; I < MaxIter; ++I) {
    uint32_t Size = F.arg_size();
    for (auto &BB : F)
      Size += BB.size();
    uint32_t Pos = randomUInt(Size - 1);
    uint32_t Idx = 0;

    for (auto &Arg : F.args())
      if ((Idx++ == Pos) && mutateArgAttr(Arg)) {
        if (++MutationIter == MutationCount)
          return true;
      }
    for (auto &BB : F) {
      for (auto &I : BB) {
        if ((Idx++ == Pos) && mutateInst(I)) {
          if (++MutationIter == MutationCount)
            return true;
        }
      }
    }
  }
  return MutationIter != 0;
}

bool mutateOnce(Function &F, bool (*Mutator)(Instruction &)) {
  for (uint32_t I = 0; I < MaxIterFactor; ++I) {
    uint32_t Size = F.arg_size();
    for (auto &BB : F)
      Size += BB.size();
    uint32_t Pos = randomUInt(Size - 1);
    uint32_t Idx = 0;

    for (auto &BB : F) {
      for (auto &I : BB) {
        if ((Idx++ == Pos) && Mutator(I)) {
          return true;
        }
      }
    }
  }
  return false;
}

bool commutativeCheck(Function &F) {
  return mutateOnce(F, commuteOperandsOfCommutativeInst);
}
bool multiUseCheck(Function &F) { return mutateOnce(F, breakOneUse); }
bool flagPreservingCheck(Function &F) { return mutateOnce(F, addFlags); }
// TODO: remove noundef/nonnull on args
bool flagDroppingCheck(Function &F) { return mutateOnce(F, dropFlags); }
bool canonicalFormCheck(Function &F) { return mutateOnce(F, canonicalizeOp); }

int main(int argc, char **argv) {
  InitLLVM Init{argc, argv};
  cl::ParseCommandLineOptions(argc, argv, "mutate\n");

  LLVMContext Ctx;
  SMDiagnostic Err;
  auto M = parseIRFile(SeedFile, Err, Ctx);
  if (!M) {
    Err.print(argv[0], errs());
    return EXIT_FAILURE;
  }

  if (M->empty())
    return EXIT_FAILURE;

  SmallVector<Function *> Funcs;
  for (auto &F : *M)
    if (!F.isDeclaration())
      Funcs.push_back(&F);

  if (Funcs.empty())
    return EXIT_FAILURE;

  bool (*mutateFunc)(Function &F) = nullptr;
  if (Recipe == "correctness")
    mutateFunc = correctnessCheck;
  else if (Recipe == "commutative")
    mutateFunc = commutativeCheck;
  else if (Recipe == "multi-use")
    mutateFunc = multiUseCheck;
  else if (Recipe == "flag-preserving")
    mutateFunc = flagPreservingCheck;
  else if (Recipe == "flag-dropping")
    mutateFunc = flagDroppingCheck;
  else if (Recipe == "canonical-form")
    mutateFunc = canonicalFormCheck;
  else {
    errs() << "Unknown recipe " << Recipe << "\n";
    return EXIT_FAILURE;
  }

  SmallVector<Function *> ErasedFuncs;
  for (auto &Func : Funcs) {
    if (!mutateFunc(*Func)) {
      ErasedFuncs.push_back(Func);
    }
  }
  for (auto *Func : ErasedFuncs)
    Func->eraseFromParent();

  // if (verifyModule(*M, &errs()))
  //   return EXIT_FAILURE;

  std::error_code EC;
  raw_fd_ostream OS(OutputFile, EC, sys::fs::OF_Text);
  if (EC) {
    errs() << "Error opening file: " << EC.message() << '\n';
    return EXIT_FAILURE;
  }
  M->print(OS, nullptr);

  return EXIT_SUCCESS;
}

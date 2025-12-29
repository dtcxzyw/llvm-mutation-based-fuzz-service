// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024 Yingwei Zheng
// This file is licensed under the Apache-2.0 License.
// See the LICENSE file for more information.

#include <llvm/ADT/APFloat.h>
#include <llvm/ADT/APInt.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/STLExtras.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Analysis/InstructionSimplify.h>
#include <llvm/IR/Argument.h>
#include <llvm/IR/AttributeMask.h>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constant.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalValue.h>
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
#include <llvm/Linker/Linker.h>
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
#include <filesystem>
#include <string>

using namespace llvm;
using namespace PatternMatch;
namespace fs = std::filesystem;

static cl::opt<std::string> SeedsDir(cl::Positional, cl::desc("<seeds dir>"),
                                     cl::Required,
                                     cl::value_desc("path to seeds"));
static cl::opt<std::string> OutputFile(cl::Positional,
                                       cl::desc("<output file>"), cl::Required,
                                       cl::value_desc("path to seed file"));
static cl::opt<bool> IgnoreFP("ignore-fp", cl::desc("Ignore FP ops"),
                              cl::init(false));
static bool isValidType(Type *Ty) {
  if (Ty->isScalableTy())
    return false;
  if (Ty->isVoidTy() || Ty->isIntOrIntVectorTy() || Ty->isLabelTy())
    return true;
  if (Ty->isPtrOrPtrVectorTy())
    return cast<PointerType>(Ty->getScalarType())->getAddressSpace() == 0;
  if (Ty->isFPOrFPVectorTy()) {
    auto *Scalar = Ty->getScalarType();
    return Scalar->isFloatTy() || Scalar->isHalfTy() || Scalar->isDoubleTy();
  }
  if (Ty->isArrayTy())
    return isValidType(Ty->getArrayElementType());
  if (auto *StructTy = dyn_cast<StructType>(Ty)) {
    if (StructTy->isOpaque() || StructTy->isPacked())
      return false;
    for (uint32_t I = 0, E = StructTy->getNumElements(); I != E; ++I)
      if (!isValidType(StructTy->getElementType(I)))
        return false;
    return true;
  }
  return false;
}
static bool hasUnsupportedType(Instruction &I) {
  if (!isValidType(I.getType()))
    return true;
  for (auto &Operand : I.operands())
    if (!isValidType(Operand->getType()))
      return true;
  return false;
}

int main(int argc, char **argv) {
  InitLLVM Init{argc, argv};
  cl::ParseCommandLineOptions(argc, argv, "merge\n");

  LLVMContext Ctx;
  SMDiagnostic Err;
  Module OutM("", Ctx);
  uint64_t BatchSize = 128;
  uint64_t IterCount = 0;

  while (OutM.size() < BatchSize) {
    for (auto &Seed : fs::directory_iterator(SeedsDir.c_str())) {
      DenseSet<StringRef> Symbols;
      for (auto &GV : OutM.globals())
        Symbols.insert(GV.getName());
      for (auto &F : OutM.functions())
        Symbols.insert(F.getName());

      std::unique_ptr<Module> M = parseIRFile(Seed.path().c_str(), Err, Ctx);
      if (!M) {
        Err.print(argv[0], errs());
        return EXIT_FAILURE;
      }

      SmallPtrSet<GlobalValue *, 16> ErasedGlobals;
      for (auto &Alias : M->aliases()) {
        ErasedGlobals.insert(&Alias);
      }

      for (auto &GV : M->globals()) {
        StringRef Name = GV.getName();
        uint32_t Id = 0;
        if (Symbols.count(Name)) {
          do {
            GV.setName(Name + std::to_string(++Id));
          } while (Symbols.count(GV.getName()));
        }

        if (GV.getAddressSpace() != 0 || !isValidType(GV.getValueType())) {
          ErasedGlobals.insert(&GV);
        }
      }

      for (auto &F : *M) {
        if (F.empty())
          continue;

        StringRef Name = F.getName();
        uint32_t Id = 0;
        if (Symbols.count(Name)) {
          do {
            F.setName(Name + std::to_string(++Id));
          } while (Symbols.count(F.getName()));
        }

        DominatorTree DT(F);
        if (!isValidType(F.getReturnType()) ||
            (!F.arg_empty() && !all_of(F.args(), [](Argument &Arg) {
              return isValidType(Arg.getType());
            }))) {
          ErasedGlobals.insert(&F);
          continue;
        }

        for (auto &Arg : F.args()) {
          Arg.removeAttr(Attribute::NoAlias);
          Arg.removeAttr(Attribute::StructRet);
          Arg.removeAttr(Attribute::SwiftError);
        }

        for (auto &BB : F) {
          for (auto &I : make_early_inc_range(BB)) {
            I.dropUnknownNonDebugMetadata({Attribute::NoUndef,
                                           Attribute::Dereferenceable,
                                           Attribute::Range});
            if (hasUnsupportedType(I) ||
                I.getOpcode() == Instruction::IntToPtr ||
                isa<AtomicRMWInst>(I) || isa<AtomicCmpXchgInst>(I) ||
                isa<AllocaInst>(I)) {
              ErasedGlobals.insert(&F);
              break;
            }
            if (auto *Load = dyn_cast<LoadInst>(&I)) {
              if (!Load->isSimple()) {
                ErasedGlobals.insert(&F);
                break;
              }
            }
            if (auto *Store = dyn_cast<StoreInst>(&I)) {
              if (!Store->isSimple()) {
                ErasedGlobals.insert(&F);
                break;
              }
            }
            if (I.isDebugOrPseudoInst()) {
              I.eraseFromParent();
              continue;
            }
            if (auto *GEP = dyn_cast<GetElementPtrInst>(&I)) {
              if (!isValidType(GEP->getSourceElementType())) {
                ErasedGlobals.insert(&F);
                break;
              }
            }
            if (auto *Call = dyn_cast<CallBase>(&I)) {
              // FIXME: Alive2 do not respect call-site attrs.
              AttributeMask AttrsToRemove;
              AttrsToRemove.addAttribute(Attribute::NoUndef);
              AttrsToRemove.addAttribute(Attribute::NonNull);
              AttrsToRemove.addAttribute(Attribute::Range);
              AttrsToRemove.addAttribute(Attribute::Alignment);
              AttrsToRemove.addAttribute(Attribute::Dereferenceable);
              AttrsToRemove.addAttribute(Attribute::DereferenceableOrNull);
              AttrsToRemove.addAttribute(Attribute::NoFPClass);
              for (auto &Arg : Call->args())
                Call->removeParamAttrs(Call->getArgOperandNo(&Arg),
                                       AttrsToRemove);

              bool Known = false;
              if (auto *II = dyn_cast<IntrinsicInst>(Call)) {
                switch (II->getIntrinsicID()) {
                case Intrinsic::umax:
                case Intrinsic::umin:
                case Intrinsic::smax:
                case Intrinsic::smin:
                case Intrinsic::abs:
                case Intrinsic::ctlz:
                case Intrinsic::cttz:
                case Intrinsic::ctpop:
                case Intrinsic::sadd_sat:
                case Intrinsic::ssub_sat:
                case Intrinsic::sshl_sat:
                case Intrinsic::uadd_sat:
                case Intrinsic::usub_sat:
                case Intrinsic::ushl_sat:
                case Intrinsic::sadd_with_overflow:
                case Intrinsic::ssub_with_overflow:
                case Intrinsic::smul_with_overflow:
                case Intrinsic::uadd_with_overflow:
                case Intrinsic::usub_with_overflow:
                case Intrinsic::umul_with_overflow:
                case Intrinsic::fshl:
                case Intrinsic::fshr:
                case Intrinsic::bitreverse:
                case Intrinsic::bswap:
                case Intrinsic::fabs:
                case Intrinsic::copysign:
                case Intrinsic::is_fpclass:
                case Intrinsic::fma:
                case Intrinsic::fmuladd:
                case Intrinsic::maximum:
                case Intrinsic::maximumnum:
                case Intrinsic::maxnum:
                case Intrinsic::minimum:
                case Intrinsic::minimumnum:
                case Intrinsic::minnum:
                case Intrinsic::canonicalize:
                  Known = true;
                case Intrinsic::assume:
                  Known = !II->hasOperandBundles();
                default:
                  break;
                }
              }
              if (!Known) {
                ErasedGlobals.insert(&F);
                break;
              }
            }
            if (auto *Sel = dyn_cast<SelectInst>(&I)) {
              if (Sel->getTrueValue()->getType()->isAggregateType()) {
                ErasedGlobals.insert(&F);
                break;
              }
            }
            if (auto *FPOp = dyn_cast<FPMathOperator>(&I)) {
              if (IgnoreFP) {
                ErasedGlobals.insert(&F);
                break;
              }
              I.setHasAllowContract(false);
              I.setHasAllowReassoc(false);
              I.setHasAllowReciprocal(false);
              I.setHasApproxFunc(false);
              // FIXME
              I.setHasNoSignedZeros(false);
            }
            for (auto &U : I.operands()) {
              Constant *C;
              if (isa<ConstantExpr>(U) ||
                  (isa<UndefValue>(U) && !isa<PoisonValue>(U))) {
                U.set(Constant::getNullValue(U->getType()));
              } else if (match(U.get(), m_Constant(C)) &&
                         !isa<PoisonValue>(C) &&
                         C->containsUndefOrPoisonElement()) {
                Constant *ReplaceC =
                    Constant::getNullValue(C->getType()->getScalarType());
                U.set(Constant::replaceUndefsWith(C, ReplaceC));
              }
            }
          }

          for (auto Succ : successors(&BB)) {
            if (DT.dominates(Succ, &BB)) {
              ErasedGlobals.insert(&F);
              break;
            }
          }
          if (ErasedGlobals.contains(&F))
            break;
        }
      }
      for (auto *F : ErasedGlobals) {
        F->replaceAllUsesWith(PoisonValue::get(F->getType()));
        F->eraseFromParent();
      }

      Linker::linkModules(OutM, std::move(M));
    }

    if (OutM.empty() || ++IterCount > BatchSize) {
      errs() << "No valid functions found in " << SeedsDir << '\n';
      return EXIT_FAILURE;
    }
  }

  // TODO: set datalayout for pointer width
  if (verifyModule(OutM, &errs()))
    return EXIT_FAILURE;

  std::error_code EC;
  raw_fd_ostream OS(OutputFile.c_str(), EC, sys::fs::OF_Text);
  if (EC) {
    errs() << "Error opening file: " << EC.message() << '\n';
    return EXIT_FAILURE;
  }

  OutM.print(OS, nullptr);
  return EXIT_SUCCESS;
}

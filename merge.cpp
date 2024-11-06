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
#include <llvm/IR/Attributes.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
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
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>

using namespace llvm;
using namespace PatternMatch;
namespace fs = std::filesystem;

static cl::opt<std::string> SeedsDir(cl::Positional, cl::desc("<seeds dir>"),
                                     cl::Required,
                                     cl::value_desc("path to seeds"));
static cl::opt<std::string> OutputFile(cl::Positional,
                                       cl::desc("<output file>"), cl::Required,
                                       cl::value_desc("path to seed file"));

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
      for (auto &F : OutM.functions())
        Symbols.insert(F.getName());

      std::unique_ptr<Module> M = parseIRFile(Seed.path().c_str(), Err, Ctx);
      if (!M) {
        Err.print(argv[0], errs());
        return EXIT_FAILURE;
      }

      SmallPtrSet<Function *, 16> ErasedFuncs;
      for (auto &GV : M->globals()) {
        StringRef Name = GV.getName();
        uint32_t Id = 0;
        if (Symbols.count(Name)) {
          do {
            GV.setName(Name + std::to_string(++Id));
          } while (Symbols.count(GV.getName()));
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

        for (auto &BB : F) {
          for (auto &I : make_early_inc_range(BB)) {
            I.dropUnknownNonDebugMetadata({Attribute::NoUndef,
                                           Attribute::Dereferenceable,
                                           Attribute::Range});
            if (I.getOpcode() == Instruction::IntToPtr) {
              ErasedFuncs.insert(&F);
              continue;
            }
            if (I.isDebugOrPseudoInst()) {
              I.eraseFromParent();
              continue;
            }
            if (auto *Call = dyn_cast<CallBase>(&I)) {
              if (!isa<IntrinsicInst>(Call)) {
                ErasedFuncs.insert(&F);
                continue;
              }
            }
          }

          for (auto Succ : successors(&BB)) {
            if (DT.dominates(Succ, &BB)) {
              ErasedFuncs.insert(&F);
              break;
            }
          }
        }
      }
      for (auto *F : ErasedFuncs) {
        F->replaceAllUsesWith(PoisonValue::get(F->getType()));
        F->eraseFromParent();
      }

      Linker::linkModules(OutM, std::move(M));
    }

    if (OutM.empty() || ++IterCount > BatchSize)
      return EXIT_FAILURE;
  }

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

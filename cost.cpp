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

static cl::opt<std::string> InputFile(cl::Positional, cl::desc("<input>"),
                                      cl::Required,
                                      cl::value_desc("path to input IR"));

int main(int argc, char **argv) {
  InitLLVM Init{argc, argv};
  cl::ParseCommandLineOptions(argc, argv, "merge\n");

  LLVMContext Ctx;
  SMDiagnostic Err;
  std::unique_ptr<Module> M = parseIRFile(InputFile, Err, Ctx);
  if (!M) {
    Err.print(argv[0], errs());
    return EXIT_FAILURE;
  }

  for (auto &F : *M) {
    if (F.empty())
      continue;
    uint32_t Cost = 0;
    for (auto &BB : F) {
      for (auto &I : BB) {
        if (I.isIntDivRem())
          Cost += 10;
        else if (I.getOpcode() == Instruction::Load ||
                 I.getOpcode() == Instruction::Store)
          Cost += 4;
        else if (auto *II = dyn_cast<IntrinsicInst>(&I)) {
          switch (II->getIntrinsicID()) {
          case Intrinsic::assume:
          case Intrinsic::lifetime_start:
          case Intrinsic::lifetime_end:
          case Intrinsic::is_constant:
            break;
          case Intrinsic::sadd_sat:
          case Intrinsic::uadd_sat:
          case Intrinsic::ssub_sat:
          case Intrinsic::usub_sat:
          case Intrinsic::sshl_sat:
          case Intrinsic::ushl_sat:
          case Intrinsic::sadd_with_overflow:
          case Intrinsic::uadd_with_overflow:
          case Intrinsic::ssub_with_overflow:
          case Intrinsic::usub_with_overflow:
          case Intrinsic::smul_with_overflow:
          case Intrinsic::umul_with_overflow:
            Cost += 3;
            break;
          case Intrinsic::is_fpclass:
          case Intrinsic::fabs:
          case Intrinsic::copysign:
          case Intrinsic::maximum:
          case Intrinsic::minimum:
          case Intrinsic::maximumnum:
          case Intrinsic::minimumnum:
          case Intrinsic::maxnum:
          case Intrinsic::minnum:
          case Intrinsic::smax:
          case Intrinsic::smin:
          case Intrinsic::umax:
          case Intrinsic::umin:
            Cost += 1;
            break;
          default:
            Cost += 2;
            break;
          }
        } else if (isa<CallInst>(I))
          ;
        else
          ++Cost;
      }
    }
    outs() << F.getName() << ": " << Cost << '\n';
  }
  return EXIT_SUCCESS;
}

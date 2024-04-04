//===-- SPIRVSplitRegionExitBlocks.cpp ----------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This pass trims convergence intrinsics as those were only useful when
// modifying the CFG during IR passes.
//
//===----------------------------------------------------------------------===//

#include "Analysis/SPIRVConvergenceRegionAnalysis.h"
#include "SPIRV.h"
#include "SPIRVSubtarget.h"
#include "SPIRVTargetMachine.h"
#include "SPIRVUtils.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/CodeGen/IntrinsicLowering.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/IntrinsicsSPIRV.h"
#include "llvm/InitializePasses.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/LoopSimplify.h"
#include "llvm/Transforms/Utils/LowerMemIntrinsics.h"

using namespace llvm;

namespace llvm {
void initializeSPIRVSplitRegionExitBlocksPass(PassRegistry &);
} // namespace llvm

namespace llvm {

class SPIRVSplitRegionExitBlocks : public FunctionPass {
public:
  static char ID;

  SPIRVSplitRegionExitBlocks() : FunctionPass(ID) {
    initializeSPIRVSplitRegionExitBlocksPass(*PassRegistry::getPassRegistry());
  };

  BasicBlock *getExitForRegion(const SPIRV::ConvergenceRegion *CR) {
    assert(CR->Exits.size() == 1);
    auto Exit = *CR->Exits.begin();
    auto Children = successors(Exit);

    // TODO: need to see how to handle switches in the exit BB.
    assert(std::distance(Children.begin(), Children.end()) <= 2);

    if (Children.empty())
      return nullptr;

    if (CR->Blocks.count(*(Children.begin() + 0)) == 0)
      return *(Children.begin() + 0);
    if (CR->Blocks.count(*(Children.begin() + 1)) == 0)
      return *(Children.begin() + 1);
    assert(0 && "The BB is an exit, but no successor is outside of the CR. "
                "Something is wrong.");
    return nullptr;
  }

  bool runOnConvergenceRegionNoRecurse(LoopInfo &LI,
                                       const SPIRV::ConvergenceRegion *CR) {
    assert(CR->Exits.size() == 1);

    auto *Exiting = *CR->Exits.begin();
    auto *Exit = getExitForRegion(CR);
    if (Exit == nullptr)
      return false;

    auto Preds = predecessors(Exit);
    if (std::distance(Preds.begin(), Preds.end()) == 1)
      return false;

    auto F = Exit->getParent();
    auto NewExit = BasicBlock::Create(F->getContext(), "new.exit", F);
    IRBuilder<> Builder(NewExit);
    Builder.CreateBr(Exit);

    if (BranchInst *BI = dyn_cast<BranchInst>(Exiting->getTerminator())) {
      for (unsigned i = 0; i < BI->getNumSuccessors(); i++) {
        if (BI->getSuccessor(i) != Exit)
          continue;
        BI->setSuccessor(i, NewExit);
      }
    } else {
      assert(0);
    }
    return true;
  }

  bool runOnConvergenceRegion(LoopInfo &LI,
                              const SPIRV::ConvergenceRegion *CR) {
#if 0
    BranchInst::Create(F->getContext(), BranchFalse, Condition, BranchTrue);

    auto NewExit = Exit->splitBasicBlockBefore(Exit->getTerminator(), "new.exit");
    DenseSet<Instruction *> ToRemove;
    for (auto *P : Preds) {
      if (CR->Blocks.count(P) != 0)
        continue;

      auto Br = P->getTerminator();
      IRBuilder<> Builder(P->getTerminator());
      Builder.CreateBr(NewExit);
      ToRemove.insert(Br);
    }

      for (auto *I : ToRemove)
        I->eraseFromParent();
      return true;

    for (auto *Exiting : CR->Exits) {
      auto Exits = successors(Exiting);
      size_t ExitsCount = std::distance(Exits.begin(), Exits.end());
      // No successor, probably a return, nothing to do.
      if (Exits.empty())
        continue;

      //bool isLoopHeader = LI.isLoopHeader(Exit);
      if (ExitsCount == 1)
        continue;
      auto Exit = *Exits.begin(); // FIXME
      auto Preds = predecessors(Exit);

      auto NewExit = Exit->splitBasicBlockBefore(Exit->getTerminator(), "new.exit");

      DenseSet<Instruction *> ToRemove;
      for (auto *P : Preds) {
        if (CR->Blocks.count(P) != 0)
          continue;

        auto Br = P->getTerminator();
        IRBuilder<> Builder(P->getTerminator());
        Builder.CreateBr(NewExit);
        ToRemove.insert(Br);
      }

      for (auto *I : ToRemove)
        I->eraseFromParent();
      return true;
    }
#endif
    if (runOnConvergenceRegionNoRecurse(LI, CR))
      return true;

    for (auto *Child : CR->Children)
      if (runOnConvergenceRegion(LI, Child))
        return true;

    return false;
  }

  virtual bool runOnFunction(Function &F) override {
    LoopInfo &LI = getAnalysis<LoopInfoWrapperPass>().getLoopInfo();
    const auto *TopLevelRegion =
        getAnalysis<SPIRVConvergenceRegionAnalysisWrapperPass>()
            .getRegionInfo()
            .getTopLevelRegion();
    TopLevelRegion->dump();

    bool modified = false;
    while (runOnConvergenceRegion(LI, TopLevelRegion)) {
      TopLevelRegion = getAnalysis<SPIRVConvergenceRegionAnalysisWrapperPass>()
                           .getRegionInfo()
                           .getTopLevelRegion();
      modified = true;
    }

    return modified;
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<DominatorTreeWrapperPass>();
    AU.addRequired<LoopInfoWrapperPass>();
    AU.addRequired<SPIRVConvergenceRegionAnalysisWrapperPass>();
    FunctionPass::getAnalysisUsage(AU);
  }
};
} // namespace llvm

char SPIRVSplitRegionExitBlocks::ID = 0;

INITIALIZE_PASS_BEGIN(SPIRVSplitRegionExitBlocks, "split-region-exit-blocks",
                      "SPIRV split region exit blocks", false, false)
INITIALIZE_PASS_DEPENDENCY(LoopSimplify)
INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)
INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(SPIRVConvergenceRegionAnalysisWrapperPass)

INITIALIZE_PASS_END(SPIRVSplitRegionExitBlocks, "split-region-exit-blocks",
                    "SPIRV split region exit blocks", false, false)

FunctionPass *llvm::createSPIRVSplitRegionExitBlocksPass() {
  return new SPIRVSplitRegionExitBlocks();
}

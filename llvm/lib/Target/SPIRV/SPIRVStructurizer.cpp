//===-- SPIRVStructurizer.cpp -----------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This pass adds the required OpSelection/OpLoop merge instructions to
// generate valid SPIR-V.
// This pass trims convergence intrinsics as those were only useful when
// modifying the CFG during IR passes.
//
//===----------------------------------------------------------------------===//

#include "Analysis/SPIRVConvergenceRegionAnalysis.h"
#include "SPIRV.h"
#include "SPIRVSubtarget.h"
#include "SPIRVTargetMachine.h"
#include "SPIRVUtils.h"
#include "llvm/CodeGen/IntrinsicLowering.h"
#include "llvm/CodeGen/MachineLoopInfo.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/IntrinsicsSPIRV.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/LowerMemIntrinsics.h"

using namespace llvm;
using namespace SPIRV;

namespace llvm {
void initializeSPIRVStructurizerPass(PassRegistry &);
}

namespace {

// Returns the exact convergence region in the tree defined by `Node` for which
// `MBB` is the header, nullptr otherwise.
const ConvergenceRegion *getRegionForHeader(const ConvergenceRegion *Node,
                                            MachineBasicBlock *MBB) {
  if (Node->Entry == MBB->getBasicBlock())
    return Node;

  for (auto *Child : Node->Children) {
    const auto *CR = getRegionForHeader(Child, MBB);
    if (CR != nullptr)
      return CR;
  }

  return nullptr;
}

// Returns the MachineBasicBlock in `MF` matching `BB`, nullptr otherwise.
MachineBasicBlock *getMachineBlockFor(MachineFunction &MF, BasicBlock *BB) {
  for (auto &MBB : MF)
    if (MBB.getBasicBlock() == BB)
      return &MBB;
  return nullptr;
}

// Returns the single MachineBasicBlock exiting the convergence region `CR`,
// nullptr if no such exit exists. MF must be the function CR belongs to.
MachineBasicBlock *getExitFor(MachineFunction &MF,
                              const ConvergenceRegion *CR) {
  // FIXME: we need a pass to correctly handle multi-exits regions.
  // In such cases, we should merge the exits nodes into a single exit.
  // For now, let's pick the first.
  assert(CR->Exits.size() >= 1);
  auto *Exit = *CR->Exits.begin();

  auto *Terminator = Exit->getTerminator();
  for (unsigned i = 0; i < Terminator->getNumSuccessors(); i++) {
    auto *Child = Terminator->getSuccessor(i);
    if (CR->Blocks.count(Child) == 0)
      return getMachineBlockFor(MF, Child);
  }
  return nullptr;
}

bool hasPredecessorsOutsideRegion(MachineBasicBlock *MBB,
                                  const ConvergenceRegion *CR) {
  for (auto *P : MBB->predecessors()) {
    if (!CR->Blocks.count(P->getBasicBlock()))
      return true;
  }
  return false;
}

} // anonymous namespace

class SPIRVStructurizer : public MachineFunctionPass {
public:
  static char ID;

  SPIRVStructurizer() : MachineFunctionPass(ID) {
    initializeSPIRVStructurizerPass(*PassRegistry::getPassRegistry());
  };

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    MachineFunctionPass::getAnalysisUsage(AU);
    AU.addRequired<MachineLoopInfo>();
    AU.addRequired<SPIRVConvergenceRegionAnalysisWrapperPass>();
  }

  virtual bool runOnMachineFunction(MachineFunction &MF) override {
    const auto &MLI = getAnalysis<MachineLoopInfo>();
    const auto *TopLevelRegion =
        getAnalysis<SPIRVConvergenceRegionAnalysisWrapperPass>()
            .getRegionInfo()
            .getTopLevelRegion();
    TopLevelRegion->dump();

    auto &TII = *MF.getSubtarget<SPIRVSubtarget>().getInstrInfo();
    auto &TRI = *MF.getSubtarget<SPIRVSubtarget>().getRegisterInfo();
    auto &RBI = *MF.getSubtarget<SPIRVSubtarget>().getRegBankInfo();

    for (auto &MBB : MF) {
      auto *CR = getRegionForHeader(TopLevelRegion, &MBB);
      // This MBB is not a region header. No merge instruction to add.
      if (CR == nullptr)
        continue;

      // This convergence region is not a loop (function-wide region for ex).
      if (!MLI.isLoopHeader(&MBB))
        continue;

      assert(MLI.isLoopHeader(&MBB));
      auto *L = MLI.getLoopFor(&MBB);

      auto *Merge = getExitFor(MF, CR);
      auto *Continue = L->getLoopLatch();

      assert(!hasPredecessorsOutsideRegion(Merge, CR));

      // Conditional branch are built using a fallthrough if false + BR.
      // So the last instruction is not always the first branch.
      auto *I = &*MBB.getFirstTerminator();
      BuildMI(MBB, I, I->getDebugLoc(), TII.get(SPIRV::OpLoopMerge))
          .addMBB(Merge)
          .addMBB(Continue)
          .addImm(SPIRV::SelectionControl::None)
          .constrainAllUses(TII, TRI, RBI);
    }
    return true;
  }
};

char SPIRVStructurizer::ID = 0;
INITIALIZE_PASS(SPIRVStructurizer, "structurizer", "SPIRV structurizer", false,
                false)

FunctionPass *llvm::createSPIRVStructurizerPass() {
  return new SPIRVStructurizer();
}

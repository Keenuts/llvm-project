//===-- SPIRVStructurizer.cpp ----------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//===----------------------------------------------------------------------===//

#include "Analysis/SPIRVConvergenceRegionAnalysis.h"
#include "SPIRV.h"
#include "SPIRVSubtarget.h"
#include "SPIRVTargetMachine.h"
#include "SPIRVUtils.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
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
#include <stack>
#include <queue>

#if 0
#define PRINT(...) \
  fprintf(stderr, __VA_ARGS__)
#else
#define PRINT(...) \
  ;
#endif

using namespace llvm;
using namespace SPIRV;

namespace llvm {
void initializeSPIRVStructurizerPass(PassRegistry &);

namespace {

using BlockSet = std::unordered_set<BasicBlock*>;
using Edge = std::pair<BasicBlock*, BasicBlock*>;

// Returns the exact convergence region in the tree defined by `Node` for which
// `BB` is the header, nullptr otherwise.
const ConvergenceRegion *getRegionForHeader(const ConvergenceRegion *Node,
                                            BasicBlock *BB) {
  if (Node->Entry == BB)
    return Node;

  for (auto *Child : Node->Children) {
    const auto *CR = getRegionForHeader(Child, BB);
    if (CR != nullptr)
      return CR;
  }
  return nullptr;
}

// Returns the single BasicBlock exiting the convergence region `CR`,
// nullptr if no such exit exists. F must be the function CR belongs to.
BasicBlock *getExitFor(const ConvergenceRegion *CR) {
  std::unordered_set<BasicBlock *> ExitTargets;
  for (BasicBlock *Exit : CR->Exits) {
    for (BasicBlock *Successor : successors(Exit)) {
      if (CR->Blocks.count(Successor) == 0)
        ExitTargets.insert(Successor);
    }
  }

  assert(ExitTargets.size() <= 1);
  if (ExitTargets.size() == 0)
    return nullptr;

  return *ExitTargets.begin();
}

BasicBlock* getDesignatedMergeBlock(Instruction *I) {
  IntrinsicInst *II = dyn_cast<IntrinsicInst>(I);
  if (II == nullptr)
    return nullptr;

  if (II->getIntrinsicID() != Intrinsic::spv_loop_merge
      && II->getIntrinsicID() != Intrinsic::spv_selection_merge)
    return nullptr;

  BlockAddress *BA = cast<BlockAddress>(II->getOperand(0));
  return BA->getBasicBlock();
}

BasicBlock* getDesignatedContinueBlock(Instruction *I) {
  IntrinsicInst *II = dyn_cast<IntrinsicInst>(I);
  if (II == nullptr)
    return nullptr;

  if (II->getIntrinsicID() != Intrinsic::spv_loop_merge)
    return nullptr;

  BlockAddress *BA = cast<BlockAddress>(II->getOperand(1));
  return BA->getBasicBlock();
}

bool isDefinedAsSelectionMergeBy(BasicBlock &Header, BasicBlock &Merge) {
  for (auto &I : Header) {
    BasicBlock *MB = getDesignatedMergeBlock(&I);
    if (MB == &Merge)
      return true;
  }
  return false;
}

bool hasLoopMergeInstruction(BasicBlock &BB) {
  for (auto &I : BB)
    if (getDesignatedContinueBlock(&I))
      return true;
  return false;
}

bool isMergeInstruction(Instruction *I) {
  return getDesignatedMergeBlock(I) != nullptr;
}

SmallPtrSet<BasicBlock*, 2> getHeaderBlocks(Function &F) {
  SmallPtrSet<BasicBlock *, 2> Output;
  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      if (getDesignatedMergeBlock(&I) != nullptr)
        Output.insert(&BB);
    }
  }
  return Output;
}

// Returns all basic blocks in |F| referenced by at least 1
// OpSelectionMerge/OpLoopMerge instruction.
SmallPtrSet<BasicBlock*, 2> getMergeBlocks(Function &F) {
  SmallPtrSet<BasicBlock *, 2> Output;
  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      BasicBlock *MB = getDesignatedMergeBlock(&I);
      if (MB != nullptr)
        Output.insert(MB);
    }
  }
  return Output;
}

std::vector<Instruction*> getMergeInstructions(BasicBlock &BB) {
  std::vector<Instruction*> Output;
  for (Instruction &I : BB)
    if (isMergeInstruction(&I))
      Output.push_back(&I);
  return Output;
}

// Returns all basic blocks in |F| referenced as continue target by at least 1 OpLoopMerge instruction.
SmallPtrSet<BasicBlock*, 2> getContinueBlocks(Function &F) {
  SmallPtrSet<BasicBlock *, 2> Output;
  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      BasicBlock *MB = getDesignatedContinueBlock(&I);
      if (MB != nullptr)
        Output.insert(MB);
    }
  }
  return Output;
}

// Do a preorder traversal of the CFG starting from the BB |Start|.
// point. Calls |op| on each basic block encountered during the traversal.
void visit(BasicBlock &Start, std::function<bool(BasicBlock *)> op) {
  std::stack<BasicBlock *> ToVisit;
  SmallPtrSet<BasicBlock *, 8> Seen;

  ToVisit.push(&Start);
  Seen.insert(ToVisit.top());
  while (ToVisit.size() != 0) {
    BasicBlock *BB = ToVisit.top();
    ToVisit.pop();

    if (!op(BB))
      continue;

    for (auto Succ : successors(BB)) {
      if (Seen.contains(Succ))
        continue;
      ToVisit.push(Succ);
      Seen.insert(Succ);
    }
  }
}

std::string fixName(const char* name) {
  std::string output = "";
  size_t len = strlen(name);
  for (size_t i = 0; i < len; i++) {
    if (name[i] >= 'a' && name[i] <= 'z')
      output += name[i];
    else if (name[i] >= 'A' && name[i] <= 'Z')
      output += name[i];
    else if (name[i] >= '0' && name[i] <= '9')
      output += name[i];
    else
    output += "_";
  }
  return output;
}

void digraph(const char* title, Function &F) {
  PRINT("digraph title: %s\n", title);
  PRINT("digraph {\n");
  for (BasicBlock &BB : F) {
    for (BasicBlock *Successor : successors(&BB)) {
      PRINT("  %s -> %s;\n", fixName(BB.getName().data()).c_str(), fixName(Successor->getName().data()).c_str());
    }

    auto MergeInstructions = getMergeInstructions(BB);
    for (Instruction *I : MergeInstructions) {
      BasicBlock *Merge = getDesignatedMergeBlock(I);
      if (Merge)
        PRINT("  %s -> %s [style=dashed];\n", fixName(BB.getName().data()).c_str(), fixName(Merge->getName().data()).c_str());
      BasicBlock *Continue = getDesignatedContinueBlock(I);
      if (Continue)
        PRINT("  %s -> %s [style=dotted];\n", fixName(BB.getName().data()).c_str(), fixName(Continue->getName().data()).c_str());
    }
  }
  PRINT("}\n");
}

class PartialOrderingVisitor {
public:
  PartialOrderingVisitor(Function &F) {
    DT.recalculate(F);
    LI = LoopInfo(DT);

    visit(&*F.begin(), 0);

    for (auto& [BB, Rank] : B2R) {
      Order.emplace_back(BB, Rank);
    };

    std::sort(Order.begin(), Order.end(), [](const auto& LHS, const auto& RHS) {
        return LHS.second < RHS.second;
    });

    for (size_t i = 0; i < Order.size(); i++) {
      B2R[Order[i].first] = i;
    }
  }

  BlockSet getReachableFrom(BasicBlock *Start) {
    std::queue<BasicBlock*> ToVisit;
    ToVisit.push(Start);

    BlockSet Output;
    while (ToVisit.size() != 0) {
      BasicBlock *BB = ToVisit.front();
      ToVisit.pop();

      if (Output.count(BB) != 0)
        continue;
      Output.insert(BB);

      for (BasicBlock *Successor : successors(BB)) {
        if (DT.dominates(Successor, BB))
          continue;
        ToVisit.push(Successor);
      }
    }

    return Output;
  }

  void partialOrderVisit(BasicBlock &Start, std::function<bool(BasicBlock*)> op) {
    BlockSet Reachable = getReachableFrom(&Start);
    assert(B2R.count(&Start) != 0);
    size_t Rank = Order[B2R[&Start]].second;
    //PRINT("Starting rank: %zu\n", Rank);

    auto It = Order.begin();
    while (It != Order.end() && It->second < Rank)
      ++It;

    if (It == Order.end())
      return;
    //PRINT("Starting Node: %zu (%s)\n", It->second, It->first->getName().data());

    //PRINT("Requesting visit from %s\n", Start.getName().data());
    size_t EndRank = Order.rbegin()->second + 1;
    //PRINT("EndRank=%zu\n", EndRank);
    for (; It != Order.end() && It->second <= EndRank; ++It) {
      if (Reachable.count(It->first) == 0) {
        //PRINT(" ignoring %s (not reachable)\n", It->first->getName().data());
        continue;
      }

      //PRINT(" visit %s\n", It->first->getName().data());
      if (!op(It->first)) {
        EndRank = It->second;
        //PRINT("op() returned false. EndRank=%zu\n", EndRank);
      }
    }
  }

  void dump() {
    PRINT("Order:\n");
    for (auto& [BB, Rank] : Order) {
      PRINT("%zu - %s\n", Rank, BB->getName().data());
    }
    digraph("partial ordering visit", *Order[0].first->getParent());
  }

private:
  size_t visit(BasicBlock *BB, size_t Rank) {
    if (Visited.count(BB) != 0)
      return Rank;

    //PRINT("Visiting %s (%zu)\n", fixName(BB->getName().data()).c_str(), Rank);

    Loop *L = LI.getLoopFor(BB);
    const bool isLoopHeader = LI.isLoopHeader(BB);

    if (B2R.count(BB) == 0) {
      B2R.emplace(BB, Rank);
    } else {
      B2R[BB] = std::max(B2R[BB], Rank);
    }

    for (BasicBlock *Predecessor : predecessors(BB)) {
      if (isLoopHeader && L->contains(Predecessor)) {
        //PRINT(" - pred %s ignored (loop header, in its loop)\n", fixName(Predecessor->getName().data()).c_str());
        continue;
      }

      if (B2R.count(Predecessor) == 0) {
        //PRINT(" - pred %s not finished. Not visiting\n", fixName(Predecessor->getName().data()).c_str());
        return Rank;
      }
    }

    Visited.insert(BB);
    //PRINT("Visiting %s.\n", fixName(BB->getName().data()).c_str());

    SmallVector<BasicBlock*, 2> OtherSuccessors;
    BasicBlock *LoopSuccessor = nullptr;

    for (BasicBlock *Successor : successors(BB)) {
      // Ignoring back-edges.
      if (DT.dominates(Successor, BB))
        continue;

      if (isLoopHeader && L->contains(Successor)) {
        assert(LoopSuccessor == nullptr);
        LoopSuccessor = Successor;
      }
      else
        OtherSuccessors.push_back(Successor);
    }

    if (LoopSuccessor)
      Rank = visit(LoopSuccessor, Rank + 1);

    size_t OutputRank = Rank;
    for (BasicBlock *Item : OtherSuccessors)
      OutputRank = std::max(OutputRank, visit(Item, Rank + 1));
    return OutputRank;
  };

  DomTreeBuilder::BBDomTree DT;
  LoopInfo LI;

  BlockSet Visited;
  std::unordered_map<BasicBlock*, size_t> B2R;
  std::vector<std::pair<BasicBlock*, size_t>> Order;
};

void partialOrderVisit(BasicBlock &Start, std::function<bool(BasicBlock*)> op) {
  PartialOrderingVisitor V(*Start.getParent());
  //V.dump();
  V.partialOrderVisit(Start, op);
}

void replaceIfBranchTargets(BasicBlock *BB, BasicBlock *OldTarget, BasicBlock *NewTarget) {
  auto *BI = cast<BranchInst>(BB->getTerminator());

  // 1. Replace all matching successors.
  for (size_t i = 0; i < BI->getNumSuccessors(); i++) {
    if (BI->getSuccessor(i) == OldTarget)
      BI->setSuccessor(i, NewTarget);
  }

  // Branch was unconditional, no fixup required.
  if (BI->isUnconditional())
    return;

  // Branch had 2 successors, maybe now both are the same?
  if (BI->getSuccessor(0) != BI->getSuccessor(1))
    return;


  // Note: we may end up here because the original IR had such branches.
  // This means Target is not necessarily equal to NewTarget.
  IRBuilder<> Builder(BB);
  Builder.SetInsertPoint(BI);
  Builder.CreateBr(BI->getSuccessor(0));
  BI->eraseFromParent();

  // The branch was the only instruction, nothing else to do.
  if (BB->size() == 1)
    return;

  // Otherwise, we need to check: was there an OpSelectionMerge before this branch?
  // If we removed the OpBranchConditional, we must also remove the OpSelectionMerge.
  // This is not valid for OpLoopMerge:
  IntrinsicInst *II = dyn_cast<IntrinsicInst>(BB->getTerminator()->getPrevNode());
  if (!II || II->getIntrinsicID() != Intrinsic::spv_selection_merge)
    return;

  Constant *C = cast<Constant>(II->getOperand(0));
  II->eraseFromParent();
  if (!C->isConstantUsed())
    C->destroyConstant();
}

void replaceBranchTargets(BasicBlock *BB, BasicBlock *OldTarget, BasicBlock *NewTarget) {
  auto *T = BB->getTerminator();
  if (isa<ReturnInst>(T))
    return;

  if (isa<BranchInst>(T))
    return replaceIfBranchTargets(BB, OldTarget, NewTarget);

  if (auto *SI = dyn_cast<SwitchInst>(T)) {
    for (size_t i = 0; i < SI->getNumSuccessors(); i++) {
      if (SI->getSuccessor(i) == OldTarget)
        SI->setSuccessor(i, NewTarget);
    }
    return;
  }

  assert(false && "Unhandled terminator type.");
}

void replacePhiTargets(BasicBlock *BB, BasicBlock *OldSrc, BasicBlock *NewSrc) {
  for (PHINode &Phi : BB->phis()) {
    int index = Phi.getBasicBlockIndex(OldSrc);
    if (index == -1)
      continue;
    Phi.setIncomingBlock(index, NewSrc);
  }
}

} // anonymous namespace

class SPIRVStructurizer : public FunctionPass {
public:
  static char ID;

  SPIRVStructurizer() : FunctionPass(ID) {
    initializeSPIRVStructurizerPass(*PassRegistry::getPassRegistry());
  };

  /// Create a value in BB set to the value associated with the branch the block
  /// terminator will take.
  Value *createExitVariable(
      BasicBlock *BB,
      const DenseMap<BasicBlock *, ConstantInt *> &TargetToValue) {
    auto *T = BB->getTerminator();
    if (isa<ReturnInst>(T))
      return nullptr;

    IRBuilder<> Builder(BB);
    Builder.SetInsertPoint(T);

    if (auto *BI = dyn_cast<BranchInst>(T)) {

      BasicBlock *LHSTarget = BI->getSuccessor(0);
      BasicBlock *RHSTarget =
          BI->isConditional() ? BI->getSuccessor(1) : nullptr;

      Value *LHS = TargetToValue.count(LHSTarget) != 0
                       ? TargetToValue.at(LHSTarget)
                       : nullptr;
      Value *RHS = TargetToValue.count(RHSTarget) != 0
                       ? TargetToValue.at(RHSTarget)
                       : nullptr;

      if (LHS == nullptr || RHS == nullptr)
        return LHS == nullptr ? RHS : LHS;
      return Builder.CreateSelect(BI->getCondition(), LHS, RHS);
    }

    // TODO: add support for switch cases.
    llvm_unreachable("Unhandled terminator type.");
  }

  // TODO: this assumes we return void. Need to figure out how to handle OpReturn with values.
  // This is only used for infinite loops, to create a valid merge block.
  // I should generate an OpUnreachable.
  BasicBlock *createReturnBlock(Function &F) {
    BasicBlock *BB = BasicBlock::Create(F.getContext(), "new.exit", &F);
    IRBuilder<> Builder(BB);
    Builder.CreateUnreachable();
    //Builder.CreateRetVoid();
    return BB;
  }

  bool addMergeForLoops(Function &F) {
    LoopInfo &LI = getAnalysis<LoopInfoWrapperPass>().getLoopInfo();
    auto *TopLevelRegion =
        getAnalysis<SPIRVConvergenceRegionAnalysisWrapperPass>()
            .getRegionInfo()
            .getTopLevelRegion();

    bool Modified = false;
    for (auto &BB : F) {
      // Not a loop header. Ignoring for now.
      if (!LI.isLoopHeader(&BB))
        continue;
      auto *L = LI.getLoopFor(&BB);

      // This loop header is not the entrance of a convergence region. Ignoring
      // this block.
      auto *CR = getRegionForHeader(TopLevelRegion, &BB);
      if (CR == nullptr)
        continue;

      IRBuilder<> Builder(&BB);

      auto *Merge = getExitFor(CR);
      // We are indeed in a loop, but there are no exits (infinite loop).
      // TODO: I see no value in having real infinite loops in vulkan shaders.
      // For now, I need to create a Merge block, and a structurally reachable block for it, but maybe we'd
      // want to raise an error, as locking up the system is probably not wanted.
      if (Merge == nullptr) {
        BranchInst *Br = cast<BranchInst>(BB.getTerminator());
        assert(cast<BranchInst>(BB.getTerminator())->isUnconditional());

        Merge = createReturnBlock(F);
        Builder.SetInsertPoint(Br);
        Builder.CreateCondBr(Builder.getFalse(), Merge, Br->getSuccessor(0));
        Br->eraseFromParent();
      }

      auto *Continue = L->getLoopLatch();

      Builder.SetInsertPoint(BB.getTerminator());
      auto MergeAddress = BlockAddress::get(Merge->getParent(), Merge);
      auto ContinueAddress = BlockAddress::get(Continue->getParent(), Continue);
      SmallVector<Value *, 2> Args = { MergeAddress, ContinueAddress };

      Builder.CreateIntrinsic(Intrinsic::spv_loop_merge, { }, { Args });
      Modified = true;
    }

    return Modified;
  }

  // Adds an OpSelectionMerge to the immediate dominator or each node with an in-degree of 2 or more which is not
  // already the merge target of an OpLoopMerge/OpSelectionMerge.
  bool addMergeForNodesWithMultiplePredecessors(Function &F) {
    //DominatorTree &DT = getAnalysis<DominatorTreeWrapperPass>().getDomTree();
    DomTreeBuilder::BBDomTree DT;
    DT.recalculate(F);

    //auto MergeBlocks = getMergeBlocks(F);
    //auto ContinueBlocks = getContinueBlocks(F);

    bool Modified = false;
    for (auto &BB : F) {
      if (pred_size(&BB) <= 1)
        continue;

      if (hasLoopMergeInstruction(BB) && pred_size(&BB) <= 2)
        continue;

      //if (MergeBlocks.contains(&BB) || ContinueBlocks.contains(&BB))
      //  continue;

      assert(DT.getNode(&BB)->getIDom());
      BasicBlock *Header = DT.getNode(&BB)->getIDom()->getBlock();

      if (isDefinedAsSelectionMergeBy(*Header, BB))
        continue;

      IRBuilder<> Builder(Header);
      Builder.SetInsertPoint(Header->getTerminator());

      auto MergeAddress = BlockAddress::get(BB.getParent(), &BB);
      SmallVector<Value *, 1> Args = { MergeAddress };
      Builder.CreateIntrinsic(Intrinsic::spv_selection_merge, { }, { Args });

      Modified = true;
    }

    return Modified;
  }

  // When a block has multiple OpSelectionMerge/OpLoopMerge, sorts those instructions
  // to but the "largest" first.
  // A merge instruction is defined as larger than another when its target merge block post-dominates the other
  // target's merge block.
  // (This ordering should match the nesting ordering of the source HLSL).
  bool sortSelectionMerge(Function &F, BasicBlock &Block) {
    std::vector<Instruction*> MergeInstructions;
    for (Instruction &I : Block)
      if (isMergeInstruction(&I))
        MergeInstructions.push_back(&I);

    if (MergeInstructions.size() <= 1)
      return false;

    Instruction *InsertionPoint = *MergeInstructions.begin();

    DomTreeBuilder::BBPostDomTree PDT;
    PDT.recalculate(F);
    std::sort(MergeInstructions.begin(), MergeInstructions.end(), [&PDT](Instruction* Left, Instruction *Right) {
        return PDT.dominates(getDesignatedMergeBlock(Right), getDesignatedMergeBlock(Left));
    });

    for (Instruction *I : MergeInstructions) {
      I->moveBefore(InsertionPoint);
      InsertionPoint = I;
    }

    return true;
  }

  bool sortSelectionMergeHeaders(Function &F) {
    bool Modified = false;
    for (BasicBlock &BB : F) {
      Modified |= sortSelectionMerge(F, BB);
    }
    return Modified;
  }

  // Split basic blocks containing multiple OpLoopMerge/OpSelectionMerge
  // instructions so each basic block contains only a single merge instruction.
  bool splitBlocksWithMultipleHeaders(Function &F) {
    std::stack<BasicBlock*> Work;
    for (auto &BB : F) {
      std::vector<Instruction*> MergeInstructions = getMergeInstructions(BB);
      if (MergeInstructions.size() <= 1)
        continue;
      Work.push(&BB);
    }

    const bool Modified = Work.size() > 0;

    while (Work.size() > 0) {
      BasicBlock *Header = Work.top();
      Work.pop();

      std::vector<Instruction*> MergeInstructions = getMergeInstructions(*Header);
      for (unsigned i = 1; i < MergeInstructions.size(); i++) {
        BasicBlock *NewBlock = Header->splitBasicBlock(MergeInstructions[i], "new.header");

        if (getDesignatedContinueBlock(MergeInstructions[0]) == nullptr) {
          BasicBlock *Unreachable = createReturnBlock(F);

          BranchInst *BI = cast<BranchInst>(Header->getTerminator());
          IRBuilder<> Builder(Header);
          Builder.SetInsertPoint(BI);
          Builder.CreateCondBr(Builder.getTrue(), NewBlock, Unreachable);
          BI->eraseFromParent();
        }

        Header = NewBlock;
      }
    }

    return Modified;
  }

  // Adds an OpSelectionMerge to each block with an out-degree >= 2 which doesn't already have an OpSelectionMerge.
  bool addMergeForDivergentBlocks(Function &F) {
    DomTreeBuilder::BBPostDomTree PDT;
    PDT.recalculate(F);
    bool Modified = false;

    auto MergeBlocks = getMergeBlocks(F);
    auto ContinueBlocks = getContinueBlocks(F);

    for (auto &BB : F) {
      if (getMergeInstructions(BB).size() != 0)
        continue;

      std::vector<BasicBlock*> Candidates;
      for (BasicBlock *Successor : successors(&BB)) {
        if (MergeBlocks.contains(Successor))
          continue;
        if (ContinueBlocks.contains(Successor))
          continue;
        Candidates.push_back(Successor);
      }

      if (Candidates.size() <= 1)
        continue;

      Modified = true;
      BasicBlock *Merge = Candidates[0];

      auto MergeAddress = BlockAddress::get(Merge->getParent(), Merge);
      SmallVector<Value *, 1> Args = { MergeAddress };
      IRBuilder<> Builder(&BB);
      Builder.SetInsertPoint(BB.getTerminator());
      Builder.CreateIntrinsic(Intrinsic::spv_selection_merge, { }, { Args });
    }

    return Modified;
  }

  struct DivergentConstruct;
  using ConstructList = std::vector<std::unique_ptr<DivergentConstruct>>;

  struct DivergentConstruct {
    BasicBlock *Header = nullptr;
    BasicBlock *Merge = nullptr;
    BasicBlock *Continue = nullptr;

    DivergentConstruct *Parent = nullptr;
    ConstructList Children;

    void dump(size_t indent = 0) {
      std::string spacing(indent, ' ');
      PRINT("%sthis:     %p\n", spacing.c_str(), (void*)this);
      PRINT("%sHeader:   %s\n", spacing.c_str(), Header   ? Header->getName().data() : "nullptr");
      PRINT("%sMerge:    %s\n", spacing.c_str(), Merge    ? Merge->getName().data() : "nullptr");
      PRINT("%sContinue: %s\n", spacing.c_str(), Continue ? Continue->getName().data() : "nullptr");
      PRINT("%sParent:   %p\n", spacing.c_str(), (void*)Parent);
      PRINT("%sChildren: [ \n", spacing.c_str());
      for (auto& Child : Children)
        Child->dump(indent + 2);
      PRINT("%s]           \n", spacing.c_str());
    }
  };

  struct Splitter {
    Function &F;
    LoopInfo &LI;
    DomTreeBuilder::BBDomTree DT;
    DomTreeBuilder::BBPostDomTree PDT;

    Splitter(Function &F, LoopInfo &LI)
      : F(F), LI(LI)
    {
      invalidate();
    }

    void invalidate() {
      PDT.recalculate(F);
      DT.recalculate(F);
    }

    void dump_construct(const char* title, const std::vector<BasicBlock*>& construct) {
      PRINT("Construct %s\n", title);
      for (BasicBlock *B : construct)
        PRINT(" - %s\n", B->getName().data());
    }

    std::vector<BasicBlock*> getContinueConstructBlocks(BasicBlock *Header, BasicBlock *Continue) {
      std::vector<BasicBlock*> Output;
      Loop *L = LI.getLoopFor(Continue);
      BasicBlock *BackEdgeBlock = L->getLoopLatch();
      assert(BackEdgeBlock);

      //PRINT("Requesting continue construct: H=%s, C=%s\n", Header->getName().data(), Continue->getName().data());
      partialOrderVisit(*Continue, [&](BasicBlock *BB) {
          if (BB == Header)
            return false;
          Output.push_back(BB);
          return true;
      });
      dump_construct("continue", Output);
      return Output;
    }

    std::vector<BasicBlock*> getLoopConstructBlocks(BasicBlock *Header, BasicBlock *Merge, BasicBlock *Continue) {
      assert(DT.dominates(Header, Merge));

      //std::vector<BasicBlock*> CV = getContinueConstructBlocks(Header, Continue);
      //std::unordered_set<BasicBlock*> ContinueConstruct(CV.begin(), CV.end());

      //PRINT("Requesting loop construct: H=%s, M=%s, C=%s\n", Header->getName().data(), Merge->getName().data(), Continue->getName().data());
      std::vector<BasicBlock*> Output;
      partialOrderVisit(*Header, [&](BasicBlock *BB) {
          if (BB == Merge)
            return false;
          if (DT.dominates(Merge, BB) || !DT.dominates(Header, BB))
            return false;
          Output.push_back(BB);
          return true;
      });
      dump_construct("loop", Output);
      return Output;
    }

    std::vector<BasicBlock*> getSelectionConstructBlocks(DivergentConstruct *Node) {
      assert(DT.dominates(Node->Header, Node->Merge));
      PRINT("Requesting selection construct: H=%s, M=%s\n",
            Node->Header->getName().data(),
            Node->Merge->getName().data());

      BlockSet OutsideBlocks;
      OutsideBlocks.insert(Node->Merge);

      for (DivergentConstruct *It = Node->Parent; It != nullptr; It = It->Parent) {
        OutsideBlocks.insert(It->Merge);
        if (It->Continue)
          OutsideBlocks.insert(It->Continue);
      }

      std::vector<BasicBlock*> Output;
      partialOrderVisit(*Node->Header, [&](BasicBlock *BB) {
          if (OutsideBlocks.count(BB) != 0)
            return false;
          if (DT.dominates(Node->Merge, BB) || !DT.dominates(Node->Header, BB))
            return false;
          Output.push_back(BB);
          return true;
      });
      dump_construct("selection", Output);
      return Output;
    }

    std::vector<BasicBlock*> getSwitchConstructBlocks(BasicBlock *Header, BasicBlock *Merge) {
      assert(DT.dominates(Header, Merge));

      std::vector<BasicBlock*> Output;
      partialOrderVisit(*Header, [&](BasicBlock *BB) {
          // the blocks structurally dominated by a switch header,
          if (!DT.dominates(Header, BB))
            return false;
          // excluding blocks structurally dominated by the switch header’s merge block.
          if (DT.dominates(Merge, BB) || BB == Merge)
            return false;
          Output.push_back(BB);
          return true;
      });
      return Output;
    }

    std::vector<BasicBlock*> getCaseConstructBlocks(BasicBlock *Target, BasicBlock *Merge) {
      assert(DT.dominates(Target, Merge));

      std::vector<BasicBlock*> Output;
      partialOrderVisit(*Target, [&](BasicBlock *BB) {
          // the blocks structurally dominated by an OpSwitch Target or Default block
          if (!DT.dominates(Target, BB))
            return false;
          // excluding the blocks structurally dominated by the OpSwitch construct’s corresponding merge block.
          if (DT.dominates(Merge, BB) || BB == Merge)
            return false;
          Output.push_back(BB);
          return true;
      });
      return Output;
    }

    std::vector<Edge> createAliasBlocksForComplexEdges(std::vector<Edge> Edges) {
      std::unordered_map<BasicBlock*, BasicBlock*> Seen;
      std::vector<Edge> Output;

      for (auto& [Src, Dst] : Edges) {
        if (Seen.count(Src) == 0) {
          Seen.emplace(Src, Dst);
          Output.emplace_back(Src, Dst);
          continue;
        }

        // The exact same edge was already seen. Ignoring.
        if (Seen[Src] == Dst)
          continue;

        // The same Src block branches to 2 distinct blocks. This will be an issue
        // for the generated OpPhi. Creating alias block.
        BasicBlock *NewSrc = BasicBlock::Create(F.getContext(), "new.exit.src", &F);
        replaceBranchTargets(Src, Dst, NewSrc);
        replacePhiTargets(Dst, Src, NewSrc);

        IRBuilder<> Builder(NewSrc);
        Builder.CreateBr(Dst);

        Seen.emplace(NewSrc, Dst);
        Output.emplace_back(NewSrc, Dst);
      }

      return Output;
    }

    BasicBlock* createSingleExitNode(BasicBlock *Header, std::vector<Edge> &Edges) {
      auto NewExit = BasicBlock::Create(F.getContext(), "new.exit", &F);
      IRBuilder<> ExitBuilder(NewExit);

      PRINT("Edges\n");
      for (auto& [Src, Dst] : Edges) {
        PRINT(" - %s -> %s\n", fixName(Src->getName().data()).c_str(), fixName(Dst->getName().data()).c_str());
      }

      BlockSet SeenDst;
      std::vector<BasicBlock*> Dsts;
      std::unordered_map<BasicBlock*, ConstantInt*> DstToIndex;

      // Given 2 edges: Src1 -> Dst, Src2 -> Dst:
      // If Dst has an PHI node, and Src1 and Src2 are both operands, both Src1 and Src2 cannot
      // be hidden by NewExit.
      // Create 2 new nodes: Alias1, Alias2 to which NewExit will branch before going to Dst.
      // Then, patchup Dst PHI node to look for Alias1 and Alias2.
      std::vector<Edge> FixedEdges = createAliasBlocksForComplexEdges(Edges);

      PRINT("Fixed edges\n");
      for (auto& [Src, Dst] : FixedEdges) {
        PRINT(" - %s -> %s\n", fixName(Src->getName().data()).c_str(), fixName(Dst->getName().data()).c_str());
      }

      for (auto& [Src, Dst] : FixedEdges) {
        if (DstToIndex.count(Dst) != 0)
          continue;
        DstToIndex.emplace(Dst, ExitBuilder.getInt32(DstToIndex.size()));
        Dsts.push_back(Dst);
      }

      if (Dsts.size() == 1) {
        for (auto& [Src, Dst] : FixedEdges) {
          replaceBranchTargets(Src, Dst, NewExit);
          replacePhiTargets(Dst, Src, NewExit);
        }
        ExitBuilder.CreateBr(Dsts[0]);
        return NewExit;
      }

      PHINode *PhiNode = ExitBuilder.CreatePHI(ExitBuilder.getInt32Ty(), FixedEdges.size());

      for (auto& [Src, Dst] : FixedEdges) {
        PhiNode->addIncoming(DstToIndex[Dst], Src);
        replaceBranchTargets(Src, Dst, NewExit);
        replacePhiTargets(Dst, Src, NewExit);
      }

      // If we can avoid an OpSwitch, generate an OpBranch. Reason is some OpBranch are allowed to exist
      // without a new OpSelectionMerge if one of the branch is the parent's merge node, while OpSwitches
      // are not.
      if (Dsts.size() == 2) {
        Value *Condition = ExitBuilder.CreateCmp(CmpInst::ICMP_EQ, DstToIndex[Dsts[0]], PhiNode);
        ExitBuilder.CreateCondBr(Condition, Dsts[0], Dsts[1]);
        return NewExit;
      }


      SwitchInst *Sw = ExitBuilder.CreateSwitch(PhiNode, Dsts[0], Dsts.size() - 1);
      for (auto It = Dsts.begin() + 1; It != Dsts.end(); ++It) {
        Sw->addCase(DstToIndex[*It], *It);
      }
      return NewExit;
    }
  };

  std::vector<Edge> getExitsFrom(const BlockSet &Construct, BasicBlock &Header) {
    std::vector<Edge> Output;
    visit(Header, [&](BasicBlock *Item) {
        if (Construct.count(Item) == 0)
          return false;

        for (BasicBlock *Successor : successors(Item)) {
          if (Construct.count(Successor) == 0)
            Output.emplace_back(Item, Successor);
        }
        return true;
    });

    return Output;
  }

  void construct(BlockSet& Visited, Splitter &S, BasicBlock *BB, DivergentConstruct *Parent) {
    if (Visited.count(BB) != 0)
      return;
    Visited.insert(BB);

    auto MIS = getMergeInstructions(*BB);
    if (MIS.size() == 0) {
      for (BasicBlock *Successor : successors(BB))
        construct(Visited, S, Successor, Parent);
      return;
    }

    assert(MIS.size() == 1);
    Instruction *MI = MIS[0];

    BasicBlock *Merge = getDesignatedMergeBlock(MI);
    BasicBlock *Continue = getDesignatedContinueBlock(MI);

    auto Output = std::make_unique<DivergentConstruct>();
    Output->Header = BB;
    Output->Merge = Merge;
    Output->Continue = Continue;
    Output->Parent = Parent;

    construct(Visited, S, Merge, Parent);
    if (Continue)
      construct(Visited, S, Continue, Output.get());

    for (BasicBlock *Successor : successors(BB))
      construct(Visited, S, Successor, Output.get());

    if (Parent)
      Parent->Children.emplace_back(std::move(Output));
  }

  BlockSet getConstructBlocks(Splitter &S, DivergentConstruct *Node) {
    assert(Node->Header && Node->Merge);

    if (Node->Continue) {
      auto LoopBlocks = S.getLoopConstructBlocks(Node->Header, Node->Merge, Node->Continue);
      return BlockSet(LoopBlocks.begin(), LoopBlocks.end());
    }

    auto SelectionBlocks = S.getSelectionConstructBlocks(Node);
    return BlockSet(SelectionBlocks.begin(), SelectionBlocks.end());
  }

  void dump_blockset(const char* title, const BlockSet &Set) {
    PRINT("BlockSet %s\n", title);
    for (BasicBlock *B : Set)
      PRINT("%s\n", B->getName().data());
    PRINT("--\n");
  }

  void dump_edges(const char* title, const std::vector<Edge> &Edges) {
    PRINT("Edges %s:\n", title);
    for (auto& [Src, Dst] : Edges)
      PRINT("%s -> %s\n", Src->getName().data(), Dst->getName().data());
    PRINT("--\n");
  }

  void fixupConstruct(Splitter &S, DivergentConstruct *Node) {
    for (auto& Child : Node->Children)
      fixupConstruct(S, Child.get());

    // This construct is the root construct. Does not represent any real construct, just
    // a way to access the first level of the forest.
    if (Node->Parent == nullptr)
      return;

    // This node's parent is the root. Meaning this is a top-level construct.
    // There can be multiple exists, but all are guaranteed to exit at most 1 construct since
    // we are at first level.
    if (Node->Parent->Header == nullptr)
      return;

    // Health check for the structure.
    assert(Node->Header && Node->Merge);
    assert(Node->Parent->Header && Node->Parent->Merge);

    //PRINT("Checking construct header by %s\n", Node->Header->getName().data());
    BlockSet ConstructBlocks = getConstructBlocks(S, Node);
    BlockSet ParentBlocks = getConstructBlocks(S, Node->Parent);

    //dump_blockset("construct", ConstructBlocks);
    //dump_blockset(ParentBlocks);

    auto Edges = getExitsFrom(ConstructBlocks, *Node->Header);

    //dump_edges("exits", Edges);
    // No edges exiting the construct.
    if (Edges.size() < 1)
      return;

    bool HasBadEdge = Node->Merge == Node->Parent->Merge || Node->Merge == Node->Parent->Continue;
    //BasicBlock *Target = Edges[0].second;
    for (auto& [Src, Dst] : Edges) {
      // - Breaking from a selection construct: S is a selection construct, S is the innermost structured
      //   control-flow construct containing A, and B is the merge block for S
      // - Breaking from the innermost loop: S is the innermost loop construct containing A,
      //   and B is the merge block for S
      if (Node->Merge == Dst)
        continue;

      // Entering the innermost loop’s continue construct: S is the innermost loop construct containing A,
      // and B is the continue target for S
      if (Node->Continue == Dst)
        continue;

      // TODO: what about cases branching to another case in the switch? Seems to work, but
      // need to double check.
      HasBadEdge = true;
    }

    if (!HasBadEdge)
      return;

    PRINT("Node %s has bad edges. Creating a new exit.\n", fixName(Node->Header->getName().data()).c_str());
    digraph("State before fixing", *Node->Header->getParent());
    // Create a single exit node gathering all exit edges.
    BasicBlock *NewExit = S.createSingleExitNode(Node->Header, Edges);
    digraph("State after fixing", *Node->Header->getParent());

    // Fixup this construct's merge node to point to the new exit.
    // Note: this algorithm fixes inner-most divergence construct first. So recursive
    // structures sharing a single merge node are fixed from the inside toward the outside.
    auto MergeInstructions = getMergeInstructions(*Node->Header);
    assert(MergeInstructions.size() == 1);
    Instruction *I = MergeInstructions[0];
    BlockAddress *BA = cast<BlockAddress>(I->getOperand(0));
    if (BA->getBasicBlock() == Node->Merge) {
      auto MergeAddress = BlockAddress::get(NewExit->getParent(), NewExit);
      I->setOperand(0, MergeAddress);
    }

    // Clean up of the possible dangling BockAddr operands to prevent MIR
    // comments about "address of removed block taken".
    if (!BA->isConstantUsed())
      BA->destroyConstant();

    Node->Merge = NewExit;
    // Regenerate the dom trees.
    S.invalidate();
  }

  bool splitCriticalEdges(Function &F) {
    LoopInfo &LI = getAnalysis<LoopInfoWrapperPass>().getLoopInfo();
    Splitter S(F, LI);

    DivergentConstruct Root;
    BlockSet Visited;
    construct(Visited, S, &*F.begin(), &Root);
    Root.dump();
    fixupConstruct(S, &Root);

    return true;
  }

  bool simplifyBranches(Function &F) {
    bool Modified = false;

    for (BasicBlock &BB : F) {
      SwitchInst *SI = dyn_cast<SwitchInst>(BB.getTerminator());
      if (!SI)
        continue;
      if (SI->getNumCases() > 1)
        continue;

      Modified = true;
      IRBuilder<> Builder(&BB);
      Builder.SetInsertPoint(SI);

      if (SI->getNumCases() == 0) {
        Builder.CreateBr(SI->getDefaultDest());
      } else {
        Value *Condition = Builder.CreateCmp(CmpInst::ICMP_EQ, SI->getCondition(), SI->case_begin()->getCaseValue());
        Builder.CreateCondBr(Condition, SI->case_begin()->getCaseSuccessor(), SI->getDefaultDest());
      }
      SI->eraseFromParent();
    }

    return Modified;
  }

  bool splitSwitchCases(Function &F) {
    bool Modified = false;

    for (BasicBlock &BB : F) {
      SwitchInst *SI = dyn_cast<SwitchInst>(BB.getTerminator());
      if (!SI)
        continue;

      BlockSet Seen;
      Seen.insert(SI->getDefaultDest());

      auto It = SI->case_begin();
      while (It != SI->case_end()) {
        BasicBlock *Target = It->getCaseSuccessor();
        if (Seen.count(Target) == 0) {
          Seen.insert(Target);
          ++It;
          continue;
        }

        Modified = true;
        BasicBlock *NewTarget = BasicBlock::Create(F.getContext(), "new.sw.case", &F);
        IRBuilder<> Builder(NewTarget);
        Builder.CreateBr(Target);
        SI->addCase(It->getCaseValue(), NewTarget);
        It = SI->removeCase(It);
      }
    }

    return Modified;
  }

  bool IsRequiredForPhiNode(BasicBlock *BB) {
    for (BasicBlock *Successor : successors(BB)) {
      for (PHINode &Phi : Successor->phis()) {
        if (Phi.getBasicBlockIndex(BB) != -1)
          return true;
      }
    }

    return false;
  }

  bool removeUselessBlocks(Function &F) {
    std::vector<BasicBlock*> ToRemove;

    auto MergeBlocks = getMergeBlocks(F);
    auto ContinueBlocks = getContinueBlocks(F);

    for (BasicBlock &BB : F) {
      if (BB.size() != 1)
        continue;

      if (isa<ReturnInst>(BB.getTerminator()))
        continue;

      if (MergeBlocks.count(&BB) != 0 || ContinueBlocks.count(&BB) != 0)
        continue;

      if (IsRequiredForPhiNode(&BB))
        continue;

      if (BB.getUniqueSuccessor() == nullptr)
        continue;

      BasicBlock *Successor = BB.getUniqueSuccessor();
      std::vector<BasicBlock*> Predecessors(predecessors(&BB).begin(), predecessors(&BB).end());
      for (BasicBlock *Predecessor : Predecessors)
        replaceBranchTargets(Predecessor, &BB, Successor);
      ToRemove.push_back(&BB);
    }

    for (BasicBlock *BB : ToRemove)
      BB->eraseFromParent();

    return ToRemove.size() != 0;
  }

  bool addHeaderToRemainingDivergentDAG(Function &F) {
    bool Modified = false;

    auto MergeBlocks = getMergeBlocks(F);
    auto ContinueBlocks = getContinueBlocks(F);
    auto HeaderBlocks = getHeaderBlocks(F);

    DomTreeBuilder::BBDomTree DT;
    DomTreeBuilder::BBPostDomTree PDT;
    PDT.recalculate(F);
    DT.recalculate(F);

    for (BasicBlock &BB : F) {
      if (HeaderBlocks.count(&BB) != 0)
        continue;
      if (succ_size(&BB) < 2)
        continue;

      size_t CandidateEdges = 0;
      for (BasicBlock *Successor : successors(&BB)) {
        if (MergeBlocks.count(Successor) != 0 || ContinueBlocks.count(Successor) != 0)
          continue;
        if (HeaderBlocks.count(Successor) != 0)
          continue;
        CandidateEdges += 1;
      }

      if (CandidateEdges <= 1)
        continue;

      BasicBlock *Header = &BB;
      BasicBlock *Merge = PDT.getNode(&BB)->getIDom()->getBlock();

      bool HasBadBlock = false;
      visit(*Header, [&](const BasicBlock* Node) {
          if (DT.dominates(Header, Node))
            return false;
          if (PDT.dominates(Merge, Node))
            return false;
          if (Node == Header || Node == Merge)
            return true;

          HasBadBlock |= MergeBlocks.count(Node) != 0
                      || ContinueBlocks.count(Node) != 0
                      || HeaderBlocks.count(Node) != 0;
          return !HasBadBlock;
      });

      if (HasBadBlock)
        continue;

      Modified = true;
      Instruction *SplitInstruction = Merge->getTerminator();
      if (isMergeInstruction(SplitInstruction->getPrevNode()))
        SplitInstruction = SplitInstruction->getPrevNode();
      BasicBlock *NewMerge = Merge->splitBasicBlockBefore(SplitInstruction, "new.merge");

      IRBuilder<> Builder(Header);
      Builder.SetInsertPoint(Header->getTerminator());

      auto MergeAddress = BlockAddress::get(NewMerge->getParent(), NewMerge);
      SmallVector<Value *, 1> Args = { MergeAddress };
      Builder.CreateIntrinsic(Intrinsic::spv_selection_merge, { }, { Args });
    }

    return Modified;
  }

  virtual bool runOnFunction(Function &F) override {
    bool Modified = false;
    digraph("start", F);

#if 1
    Modified |= splitSwitchCases(F);
    digraph("after splitSwitchCases", F);

    Modified |= simplifyBranches(F);
    digraph("after simplifyBranches", F);

    // STEP 6
    Modified |= addMergeForLoops(F);
    digraph("after addMergeForLoops", F);

    // STEP 7
    Modified |= addMergeForNodesWithMultiplePredecessors(F);
    digraph("after addMergeForNodesWithMultiplePredecessors", F);

    // STEP 8
    Modified |= sortSelectionMergeHeaders(F);
    Modified |= splitBlocksWithMultipleHeaders(F);
    digraph("after splitBlocksWithMultipleHeaders", F);

    // STEP 9
    Modified |= addMergeForDivergentBlocks(F);
    digraph("after addMergeForDivergentBlocks", F);

    // STEP 10
    Modified |= splitCriticalEdges(F);
    digraph("after splitCriticalEdges", F);

    // STEP 11
    Modified |= removeUselessBlocks(F);
    digraph("after removeUselessBlocks", F);

#if 1
    // STEP 12
    Modified |= addHeaderToRemainingDivergentDAG(F);
    digraph("after addHeaderToRemainingDivergentDAG", F);
#endif

#if 0
    // STEP 9
    Modified |= addMergeForDivergentBlocks(F);
#endif
#endif

    return Modified;
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<DominatorTreeWrapperPass>();
    AU.addRequired<LoopInfoWrapperPass>();
    AU.addRequired<SPIRVConvergenceRegionAnalysisWrapperPass>();

    AU.addPreserved<SPIRVConvergenceRegionAnalysisWrapperPass>();
    FunctionPass::getAnalysisUsage(AU);
  }
};
} // namespace llvm

char SPIRVStructurizer::ID = 0;

INITIALIZE_PASS_BEGIN(SPIRVStructurizer, "structurizer", "structurize SPIRV", false, false)
INITIALIZE_PASS_DEPENDENCY(LoopSimplify)
INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)
INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(SPIRVConvergenceRegionAnalysisWrapperPass)

INITIALIZE_PASS_END(SPIRVStructurizer, "structurize", "structurize SPIRV", false, false)

FunctionPass *llvm::createSPIRVStructurizerPass() {
  return new SPIRVStructurizer();
}


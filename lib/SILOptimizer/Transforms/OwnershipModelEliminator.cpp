//===--- OwnershipModelEliminator.cpp - Eliminate SILOwnership Instr. -----===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2016 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
///
///  \file
///
///  This file contains a small pass that lowers SIL ownership instructions to
///  their constituent operations. This will enable us to separate
///  implementation
///  of Semantic ARC in SIL and SILGen from ensuring that all of the optimizer
///  passes respect Semantic ARC. This is done by running this pass right after
///  SILGen and as the pass pipeline is updated, moving this pass further and
///  further back in the pipeline.
///
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "sil-ownership-model-eliminator"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILVisitor.h"

using namespace swift;

//===----------------------------------------------------------------------===//
//                               Implementation
//===----------------------------------------------------------------------===//

namespace {

struct OwnershipModelEliminatorVisitor
    : SILInstructionVisitor<OwnershipModelEliminatorVisitor, bool> {
  SILBuilder &B;

  OwnershipModelEliminatorVisitor(SILBuilder &B) : B(B) {}
  void beforeVisit(ValueBase *V) {
    auto *I = cast<SILInstruction>(V);
    B.setInsertionPoint(I);
    B.setCurrentDebugScope(I->getDebugScope());
  }

  bool visitValueBase(ValueBase *V) { return false; }
  bool visitLoadInst(LoadInst *LI);
  bool visitStoreInst(StoreInst *SI);
  bool visitCopyValueInst(CopyValueInst *CVI);
  bool visitDestroyValueInst(DestroyValueInst *DVI);
  bool visitLoadBorrowInst(LoadBorrowInst *LBI);
  bool visitEndBorrowInst(EndBorrowInst *EBI) {
    EBI->eraseFromParent();
    return true;
  }
};

} // end anonymous namespace

bool OwnershipModelEliminatorVisitor::visitLoadInst(LoadInst *LI) {
  auto Qualifier = LI->getOwnershipQualifier();

  // If the qualifier is unqualified, there is nothing further to do
  // here. Just return.
  if (Qualifier == LoadOwnershipQualifier::Unqualified)
    return false;

  // Otherwise, we need to break down the load inst into its unqualified
  // components.
  auto *UnqualifiedLoad = B.createLoad(LI->getLoc(), LI->getOperand(),
                                       LoadOwnershipQualifier::Unqualified);

  // If we have a copy, insert a retain_value. All other copies do not require
  // more work.
  if (Qualifier == LoadOwnershipQualifier::Copy) {
    B.emitCopyValueOperation(UnqualifiedLoad->getLoc(), UnqualifiedLoad);
  }

  // Then remove the qualified load and use the unqualified load as the def of
  // all of LI's uses.
  LI->replaceAllUsesWith(UnqualifiedLoad);
  LI->eraseFromParent();
  return true;
}

bool OwnershipModelEliminatorVisitor::visitStoreInst(StoreInst *SI) {
  auto Qualifier = SI->getOwnershipQualifier();

  // If the qualifier is unqualified, there is nothing further to do
  // here. Just return.
  if (Qualifier == StoreOwnershipQualifier::Unqualified)
    return false;

  // Otherwise, we need to break down the store.
  if (Qualifier != StoreOwnershipQualifier::Assign) {
    // If the ownership qualifier is not an assign, we can just emit an
    // unqualified store.
    B.createStore(SI->getLoc(), SI->getSrc(), SI->getDest(),
                  StoreOwnershipQualifier::Unqualified);
  } else {
    // If the ownership qualifier is [assign], then we need to eliminate the
    // old value.
    //
    // 1. Load old value.
    // 2. Store new value.
    // 3. Release old value.
    auto *Old = B.createLoad(SI->getLoc(), SI->getDest(),
                             LoadOwnershipQualifier::Unqualified);
    B.createStore(SI->getLoc(), SI->getSrc(), SI->getDest(),
                  StoreOwnershipQualifier::Unqualified);
    B.emitDestroyValueOperation(SI->getLoc(), Old);
  }

  // Then remove the qualified store.
  SI->eraseFromParent();
  return true;
}

bool
OwnershipModelEliminatorVisitor::visitLoadBorrowInst(LoadBorrowInst *LBI) {
  // Break down the load borrow into an unqualified load.
  auto *UnqualifiedLoad = B.createLoad(LBI->getLoc(), LBI->getOperand(),
                                       LoadOwnershipQualifier::Unqualified);

  // Then remove the qualified load and use the unqualified load as the def of
  // all of LI's uses.
  LBI->replaceAllUsesWith(UnqualifiedLoad);
  LBI->eraseFromParent();
  return true;
}

bool OwnershipModelEliminatorVisitor::visitCopyValueInst(CopyValueInst *CVI) {
  // Now that we have set the unqualified ownership flag, destroy value
  // operation will delegate to the appropriate strong_release, etc.
  B.emitCopyValueOperation(CVI->getLoc(), CVI->getOperand());
  CVI->replaceAllUsesWith(CVI->getOperand());
  CVI->eraseFromParent();
  return true;
}

bool OwnershipModelEliminatorVisitor::visitDestroyValueInst(DestroyValueInst *DVI) {
  // Now that we have set the unqualified ownership flag, destroy value
  // operation will delegate to the appropriate strong_release, etc.
  B.emitDestroyValueOperation(DVI->getLoc(), DVI->getOperand());
  DVI->eraseFromParent();
  return true;
}

//===----------------------------------------------------------------------===//
//                           Top Level Entry Point
//===----------------------------------------------------------------------===//

namespace {

struct OwnershipModelEliminator : SILFunctionTransform {
  void run() override {
    SILFunction *F = getFunction();

    // Set F to have unqualified ownership.
    F->setUnqualifiedOwnership();

    bool MadeChange = false;
    SILBuilder B(*F);
    OwnershipModelEliminatorVisitor Visitor(B);

    for (auto &BB : *F) {
      for (auto II = BB.begin(), IE = BB.end(); II != IE;) {
        // Since we are going to be potentially removing instructions, we need
        // to make sure to grab out instruction and increment first.
        SILInstruction *I = &*II;
        ++II;

        MadeChange |= Visitor.visit(I);
      }
    }

    if (MadeChange) {
      // If we made any changes, we just changed instructions, so invalidate
      // that analysis.
      invalidateAnalysis(SILAnalysis::InvalidationKind::Instructions);
    }
  }

  StringRef getName() override { return "Ownership Model Eliminator"; }
};

} // end anonymous namespace

SILTransform *swift::createOwnershipModelEliminator() {
  return new OwnershipModelEliminator();
}

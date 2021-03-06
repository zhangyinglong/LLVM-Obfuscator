//=== flatten.cpp - Flatten Control Flow Pass  ----------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
// http://ac.inf.elte.hu/Vol_030_2009/003.pdf
#define DEBUG_TYPE "flatten"
#include "Transform/flatten.h"
#include "Transform/copy.h"
#include "Transform/obf_utilities.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/Dominators.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/User.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/CFG.h"
#include <algorithm>
#include <vector>
#include <chrono>
#include <random>

using namespace llvm;

static cl::list<std::string>
flattenFunc("flattenFunc", cl::CommaSeparated,
            cl::desc("Flatten only some functions: "
                     "flattenFunc=\"func1,func2\""));

static cl::opt<std::string> flattenSeed(
    "flattenSeed", cl::init(""),
    cl::desc("Seed for random number generator. Defaults to system time"));

static cl::opt<double>
flattenProbability("flattenProbability", cl::init(0.5),
                   cl::desc("Probability that a function will be split"));

static cl::opt<bool> disableFlatten(
    "disableFlatten", cl::init(false),
    cl::desc("Disable Flatten pass regardless. Useful when used in -OX mode."));

Value *Flatten::findBlock(LLVMContext &context,
                          std::vector<BasicBlock *> &blocks,
                          BasicBlock *block) {
  auto iterator = std::find(blocks.begin(), blocks.end(), block);
  assert(iterator != blocks.end() && "Block does not exist in vector!");
  unsigned index = iterator - blocks.begin();
  return ConstantInt::get(Type::getInt32Ty(context), index, false);
}

// Initialise and check options
bool Flatten::doInitialization(Module &M) {
  if (disableFlatten)
    return false;

  if (flattenProbability < 0.f || flattenProbability > 1.f) {
    LLVMContext &ctx = getGlobalContext();
    ctx.emitError("Flatten: Probability must be between 0 and 1");
  }
  // Seed engine and create distribution
  if (!flattenSeed.empty()) {
    std::seed_seq seed(flattenSeed.begin(), flattenSeed.end());
    engine.seed(seed);
  } else {
    unsigned seed = std::chrono::system_clock::now().time_since_epoch().count();
    engine.seed(seed);
  }
  trial.param(
      std::bernoulli_distribution::param_type((double)flattenProbability));

  return false;
}

bool Flatten::runOnFunction(Function &F) {
  // If the function is declared elsewhere in other translation unit
  // we should not modify it here
  if (F.isDeclaration()) {
    return false;
  }
  bool mustObfuscate = Copy::isFunctionTagged(F, ObfUtils::FlattenObf);
  DEBUG(errs() << "flatten: Function '" << F.getName() << "'\n");

  // Check if function is requested
  auto funcListStart = flattenFunc.begin(), funcListEnd = flattenFunc.end();
  if (!mustObfuscate && flattenFunc.size() != 0 &&
      std::find(funcListStart, funcListEnd, F.getName()) == funcListEnd) {
    DEBUG(errs() << "\tFunction not requested -- skipping\n");
    return false;
  }

  LLVMContext &context = F.getContext();

  // Use a vector to store the list of blocks
  std::vector<BasicBlock *> blocks;
  blocks.reserve(F.size());

  DEBUG(errs() << "\t" << F.size() << " basic blocks found\n");
  Twine blockPrefix = "block_";
  unsigned i = 0;
  DEBUG(errs() << "\tListing and filtering blocks\n");
  // Get original list of blocks
  for (auto &block : F) {
    DEBUG(if (!block.hasName()) { block.setName(blockPrefix + Twine(i++)); });

    DEBUG(errs() << "\tBlock " << block.getName() << "\n");
    BasicBlock::iterator inst1 = block.begin();
    if (block.getFirstNonPHIOrDbgOrLifetime()) {
      inst1 = block.getFirstNonPHIOrDbgOrLifetime();
    }

    if (isa<IndirectBrInst>(block.getTerminator())) {
      // TODO Maybe handle this
      DEBUG(errs() << "\tSkipping function -- IndirectBrInst encountered\n");
      return false;
    }

    if (isa<SwitchInst>(block.getTerminator())) {
      // TODO Maybe handle this
      DEBUG(errs() << "\tSkipping function -- SwitchInst encountered\n");
      return false;
    }

    // LLVM does not support PHINodes for Invoke Edges
    if (isa<InvokeInst>(block.getTerminator())) {
      // TODO Maybe handle this
      DEBUG(errs() << "\tSkipping function -- InvokeInst encountered\n");
      return false;
    }

    if (block.isLandingPad()) {
      DEBUG(errs() << "\t\tSkipping: Landing pad block\n");
      continue;
    }
    if (&block == &F.getEntryBlock()) {
      DEBUG(errs() << "\t\tSkipping: Entry block\n");
      continue;
    }

    DEBUG(errs() << "\t\tAdding block\n");
    blocks.push_back(&block);
  }

  DEBUG(errs() << "\t" << blocks.size() << " basic blocks remaining\n");
  if (blocks.size() < 2) {
    DEBUG(errs() << "\tNothing left to flatten\n");
    return false;
  }
  // Setup other variables
  BasicBlock &entryBlock = F.getEntryBlock();

  if (entryBlock.getTerminator()->getNumSuccessors() == blocks.size() ||
      entryBlock.getTerminator()->getNumSuccessors() == 0) {
    DEBUG(errs() << "\tFunction is trivial -- already flat control flow\n");
    return false;
  }

  if (!trial(engine)) {
    DEBUG(errs() << "\tSkipping: Bernoulli trial failed\n");
    return false;
  }

  // DEBUG_WITH_TYPE("cfg", F.viewCFG());

  // Demote all the PHI Nodes to stack
  DEBUG(errs() << "\tDemoting PHI Nodes to stack\n");
  for (auto block : blocks) {
    std::vector<PHINode *> phis;
    for (auto &inst : *block) {
      if (PHINode *phiInst = dyn_cast<PHINode>(&inst)) {
        phis.push_back(phiInst);
      }
    }
    for (auto phiInst : phis) {
      DemotePHIToStack(phiInst);
    }
  }

  BasicBlock *initialBlock;
  // Going to have to split the entry block into 2 blocks
  if (entryBlock.getTerminator()->getNumSuccessors() > 1) {
    DEBUG(errs() << "\tSplitting entry block\n");
    initialBlock = SplitBlock(&entryBlock, entryBlock.getTerminator(), this);
    blocks.push_back(initialBlock);
  } else {
    initialBlock = entryBlock.getTerminator()->getSuccessor(0);
  }
  DEBUG(entryBlock.setName("entry_block"));
  DEBUG(initialBlock->setName("initial_block"));

  entryBlock.getTerminator()->eraseFromParent();

  // Entry Block builder
  IRBuilder<> entryBuilder(&entryBlock);

  BasicBlock *jumpBlock = BasicBlock::Create(context, "", &F);
  jumpBlock->moveBefore(initialBlock);
  DEBUG(jumpBlock->setName("jump_block"));

  // Jump Block builder
  IRBuilder<> jumpBuilder(jumpBlock);

  Twine jumpIndexName("");
  DEBUG(jumpIndexName = jumpIndexName.concat("jump_index"));
  PHINode *jumpIndex = jumpBuilder.CreatePHI(Type::getInt32Ty(context),
                                             blocks.size() + 1, jumpIndexName);

  DEBUG(errs() << "\tCreating jump table:\n");
  std::vector<Constant *> blockAddresses(blocks.size());

  for (unsigned i = 0, iEnd = blocks.size(); i < iEnd; ++i) {
    BasicBlock *block = blocks[i];
    blockAddresses[i] = (Constant *)BlockAddress::get(block);
  }
  // Create JumpTables
  DEBUG(errs() << "\tCreating jump table:\n");
  ArrayType *jumpType =
      ArrayType::get(Type::getInt8PtrTy(context), blocks.size());
  Constant *jumpValues = ConstantArray::get(jumpType, blockAddresses);
  Twine jumpTableName("");
  DEBUG(jumpTableName = jumpTableName.concat(F.getName()).concat("_jumpTable"));
  GlobalVariable *jumpTable = new GlobalVariable(
      *(F.getParent()), jumpType, false, GlobalValue::PrivateLinkage,
      jumpValues, jumpTableName);

  Value *indices[2];
  indices[0] = ConstantInt::get(Type::getInt32Ty(F.getContext()), 0, true);
  indices[1] = jumpIndex;

  // Create indirect branch
  Twine jumpAddrPtrName("");
  DEBUG(jumpAddrPtrName = jumpAddrPtrName.concat("jump_addr_ptr"));
  Value *jumpAddressPtr =
      jumpBuilder.CreateInBoundsGEP(jumpTable, indices, jumpAddrPtrName);
  Twine jumpAddrName("");
  DEBUG(jumpAddrName = jumpAddrName.concat("jump_addr"));
  LoadInst *jumpAddr = jumpBuilder.CreateLoad(jumpAddressPtr, jumpAddrName);
  IndirectBrInst *indirectBranch =
      jumpBuilder.CreateIndirectBr(jumpAddr, blocks.size());
  assert(indirectBranch && "IndirectBranchInst cannot be null!");

  for (unsigned i = 0, iEnd = blocks.size(); i < iEnd; ++i) {
    BasicBlock *block = blocks[i];
    assert(block != &entryBlock && "Entry block should not be processed!");
    DEBUG(errs() << "\t" << block->getName() << ":\n");
    ConstantInt *index = ConstantInt::get(Type::getInt32Ty(context), i, false);

    // Using switch for now
    // switchInst->addCase(index, block);
    indirectBranch->addDestination(block);

    // Create jump index
    if (block == initialBlock) {
      jumpIndex->addIncoming(index, &entryBlock);
    }

    TerminatorInst *terminator = block->getTerminator();
    bool hasSuccessor = terminator->getNumSuccessors() > 0;
    if (terminator->getNumSuccessors() == 0) {
      // No need to do anything
      DEBUG(errs() << "\t\t0 Successor\n");
      // ReturnInst, ResumeInst, UnreachableInst
    } else if (terminator->getNumSuccessors() == 1) {
      // Trivial
      DEBUG(errs() << "\t\t1 Successor\n");
      BasicBlock *destination = terminator->getSuccessor(0);
      Value *destinationIndexValue = findBlock(context, blocks, destination);
      jumpIndex->addIncoming(destinationIndexValue, block);

      terminator->eraseFromParent();
      BranchInst::Create(jumpBlock, block);
    } else { // > 1 succesors
      DEBUG(errs() << "\t\t" << terminator->getNumSuccessors()
                   << " Successors\n");
      // Conditional branch
      if (BranchInst *branch = dyn_cast<BranchInst>(terminator)) {
        DEBUG(errs() << "\t\tConditional branch\n");
        BasicBlock *trueBlock = branch->getSuccessor(0);
        BasicBlock *falseBlock = branch->getSuccessor(1);
        Value *trueIndex = findBlock(context, blocks, trueBlock);
        Value *falseIndex = findBlock(context, blocks, falseBlock);
        SelectInst *select = SelectInst::Create(
            branch->getCondition(), trueIndex, falseIndex, "", terminator);

        jumpIndex->addIncoming(select, block);

        terminator->eraseFromParent();
        BranchInst::Create(jumpBlock, block);

// Disabled because Invoke edges are not supported in promoting PHI
#if 0
        } else if (InvokeInst *invoke = dyn_cast<InvokeInst>(terminator)) {
          // InvokeInst
          DEBUG(errs() << "\t\tInvoke Terminator\n");
          Value *destination =
              findBlock(context, blocks, invoke->getNormalDest());
          BasicBlock *newDestination = BasicBlock::Create(context, "", &F);
          invoke->setNormalDest(newDestination);
          jumpIndex->addIncoming(destination, newDestination);
          BranchInst::Create(jumpBlock, newDestination);
#endif
      } else {
        DEBUG(errs() << terminator->getOpcodeName()
                     << " type of TerminatorInst encountered \n");
        llvm_unreachable("Unexpected TerminatorInst encountered!");
      }
    }

#if 0
      // Move all PHI Nodes to jumpBlock
      std::vector<PHINode *> movePHIs;
      for (auto &inst : *block) {
        if (PHINode *phi = dyn_cast<PHINode>(&inst)) {
          movePHIs.push_back(phi);
        }
      }

      for (auto phi : movePHIs) {
        phi->moveBefore(jumpBlock->begin());
      }
#endif

    if (hasSuccessor) {
      DEBUG(errs() << "\t\tHandling successor use\n");
      for (auto &inst : *block) {
        DEBUG(errs() << "\t\t\t" << inst << "\n");
        std::vector<User *> users;
        PHINode *phi = nullptr;
        bool isUsed = false;
        // Find the phi node in jumpBlock if it's there
        for (auto user = inst.use_begin(), useEnd = inst.use_end();
             user != useEnd; ++user) {
          Instruction *userInst = dyn_cast<Instruction>(*user);
          assert(userInst && "User is not an instruction");
          BasicBlock *userBlock = userInst->getParent();
          if (userBlock == jumpBlock) {
            phi = dyn_cast<PHINode>(userInst);
            if (phi && phi != jumpIndex) {
              isUsed = true;
            }
          } else if (userBlock != block) {
            isUsed = true;
            users.push_back(*user);
            DEBUG(errs() << "\t\t\t\tUsed in " << userBlock->getName() << "\n");
          }
        }
        if (isUsed && !phi) {
          DEBUG(errs() << "\t\t\t\tCreating PHI Node\n");
          phi = jumpBuilder.CreatePHI(inst.getType(), users.size(), "");
          phi->moveBefore(jumpBlock->begin());
        }
        if (isUsed) {
          if (phi->getBasicBlockIndex(block) == -1)
            phi->addIncoming(&inst, block);
          for (User *user : users) {
            user->replaceUsesOfWith(&inst, phi);
          }
          DemotePHIToStack(phi);
        }
      }
    }
  }

  entryBuilder.CreateBr(jumpBlock);

#if 0
  // Iterate through PHINodes of jumpBlock and assign NULL values or other
  // necessary incoming
  for (auto &inst : *jumpBlock) {
    if (PHINode *phi = dyn_cast<PHINode>(&inst)) {
      // Handle entryBlock
      if (phi->getBasicBlockIndex(&entryBlock) == -1) {
        // Set to null
        Value *nullValue = Constant::getNullValue(phi->getType());
        phi->addIncoming(nullValue, &entryBlock);
      }

      // All other blocks
      for (auto block : blocks) {
        if (phi->getBasicBlockIndex(block) == -1 &&
            block->getTerminator()->getNumSuccessors() > 0) {
          phi->addIncoming(phi, block);
        }
      }
    }
  }
#endif

  // DEBUG_WITH_TYPE("cfg", F.viewCFG());
  // DEBUG_WITH_TYPE("cfg", F.viewCFG());

  ObfUtils::tagFunction(F, ObfUtils::FlattenObf);
  return true;
}

bool Flatten::isEligible(Function &F) {
  DEBUG(errs() << "Flatten: Checking " << F.getName() << " eligibility:\n");
  if (F.isDeclaration()) {
    DEBUG(errs() << "\tIneligible -- declaration\n");
    return false;
  }
  unsigned eligibleBB = 0;
  DEBUG(errs() << "\tInspecting basic blocks\n");
  BasicBlock &entryBlock = F.getEntryBlock();
  for (auto &block : F) {
    BasicBlock::iterator inst1 = block.begin();
    if (block.getFirstNonPHIOrDbgOrLifetime()) {
      inst1 = block.getFirstNonPHIOrDbgOrLifetime();
    }

    if (isa<IndirectBrInst>(block.getTerminator())) {
      // TODO Maybe handle this
      DEBUG(errs() << "\tIneligible function -- IndirectBrInst encountered\n");
      return false;
    }

    if (isa<SwitchInst>(block.getTerminator())) {
      // TODO Maybe handle this
      DEBUG(errs() << "\tIneligible function -- SwitchInst encountered\n");
      return false;
    }

    // LLVM does not support PHINodes for Invoke Edges
    if (isa<InvokeInst>(block.getTerminator())) {
      DEBUG(errs() << "\tIneligible function -- InvokeInst encountered\n");
      return false;
    }

    if (block.isLandingPad()) {
      continue;
    }
    if (&block == &entryBlock) {
      continue;
    }
    ++eligibleBB;
  }

  if (!eligibleBB) {
    DEBUG(errs() << "\tIneligible function -- No eligible BB\n");
    return false;
  }

  if (entryBlock.getTerminator()->getNumSuccessors() == eligibleBB ||
      entryBlock.getTerminator()->getNumSuccessors() == 0) {
    DEBUG(errs() << "\ttIneligible function -- already flat control flow\n");
    return false;
  }
  return true;
}

char Flatten::ID = 0;
static RegisterPass<Flatten> X("flatten", "Flatten function control flow",
                               false, false);

/*
 *	This pass creates new definitions for variables used as operands
 *	of specifific instructions: add, sub, mul, trunc.
 *
 *	These new definitions are inserted right after the use site, and
 *	all remaining uses dominated by this new definition are renamed
 *	properly.
 *
 *	Copyright (C) 2012  Victor Hugo Sperle Campos
*/

#include "llvm/IR/Metadata.h"
#include "llvm/Pass.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/Dominators.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/CFG.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Analysis/DominanceFrontier.h"
#include "llvm/IR/Constants.h"
#include <deque>
#include <algorithm>

namespace llvm {

class uSSA : public FunctionPass {
public:
	static char ID; // Pass identification, replacement for typeid.
	uSSA() : FunctionPass(ID) {}
	void getAnalysisUsage(AnalysisUsage &AU) const;
	bool runOnFunction(Function&);
	
	void createNewDefs(BasicBlock *BB);
	void renameNewDefs(Instruction *newdef);

private:
	// Variables always live
	DominatorTree *DT_;
	DominanceFrontier *DF_;
};

}

bool getTruncInstrumentation();

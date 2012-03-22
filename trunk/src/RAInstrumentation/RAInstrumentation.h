/*
 * RAInstrumentation.h
 *
 *  Created on: 22/03/2012
 *      Author: raphael
 */

#ifndef RAINSTRUMENTATION_H_
#define RAINSTRUMENTATION_H_

#define DEBUG_TYPE "range-analysis-printer"

#include "llvm/Constants.h"
#include "llvm/DerivedTypes.h"
#include "llvm/Function.h"
#include "llvm/InstrTypes.h"
#include "llvm/Instructions.h"
#include "llvm/LLVMContext.h"
#include "llvm/Module.h"
#include "llvm/Pass.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/raw_ostream.h"
#include <vector>
#include <list>
#include <stdio.h>
#include "RAInstrumentation.h"

#define BIT_WIDTH 32

using namespace llvm;


namespace {

	struct RAInstrumentation : public ModulePass {
		static char ID;
		RAInstrumentation() : ModulePass(ID) {};

        void printValueInfo(const Value *V);
		void MarkAsNotOriginal(Instruction& inst);
        void PrintInstructionIdentifier(std::string M, std::string F, const Value *V);

        bool IsNotOriginal(Instruction& inst);
        bool isValidInst(Instruction *I);
        virtual bool runOnModule(Module &M);

        Function& GetSetCurrentMinMaxFunction();
        Function& GetPrintHashFunction();
        Instruction* GetNextInstruction(Instruction& i);

        void InstrumentMainFunction(Function* F);

        Module* module;
        LLVMContext* context;
		FILE* file;

	};
}

char RAInstrumentation::ID = 0;





#endif /* RAINSTRUMENTATION_H_ */

//===-------------------------- RangeAnalysis.cpp -------------------------===//
//===----- Performs a range analysis of the variables of the function -----===//
//
//					 The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "range-analysis"

#include "RangeAnalysis.h"
#include "llvm/Support/InstIterator.h"

using namespace llvm;

// These macros are used to get stats regarging the precision of our analysis.
STATISTIC(usedBits, "Initial number of bits.");
STATISTIC(needBits, "Needed bits.");
STATISTIC(percentReduction, "Percentage of reduction of the number of bits.");
STATISTIC(numSCCs, "Number of strongly connected components.");
STATISTIC(numAloneSCCs, "Number of SCCs containing only one node.");
STATISTIC(sizeMaxSCC, "Size of largest SCC.");
STATISTIC(numVars, "Number of variables");
STATISTIC(numUnknown, "Number of unknown variables");
STATISTIC(numEmpty, "Number of empty-set variables");
STATISTIC(numCPlusInf, "Number of variables [c, +inf].");
STATISTIC(numCC, "Number of variables [c, c].");
STATISTIC(numMinInfC, "Number of variables [-inf, c].");
STATISTIC(numMaxRange, "Number of variables [-inf, +inf].");
STATISTIC(numConstants, "Number of constants.");
STATISTIC(numZeroUses, "Number of variables without any use.");
STATISTIC(numNotInt, "Number of variables that are not Integer.");
STATISTIC(numOps, "Number of operations");

// The number of bits needed to store the largest variable of the function (APInt).
unsigned MAX_BIT_INT = 1;

// This map is used to store the number of times that the widen_meet 
// operator is called on a variable. It was a Fernando's suggestion.
DenseMap<const Value*, unsigned> FerMap;

// ========================================================================== //
// Static global functions and definitions
// ========================================================================== //

// The min and max integer values for a given bit width.
APInt Min = APInt::getSignedMinValue(MAX_BIT_INT);
APInt Max = APInt::getSignedMaxValue(MAX_BIT_INT);
APInt Zero(MAX_BIT_INT, 0, true);

// String used to identify sigmas
const std::string sigmaString = "vSSA_sigma";

// Used to print pseudo-edges in the Constraint Graph dot
std::string pestring;
raw_string_ostream pseudoEdgesString(pestring);

// Used to profile
Profile prof;

// Print name of variable according to its type
static void printVarName(const Value *V, raw_ostream& OS) {
	const Argument *A = NULL;
	const Instruction *I = NULL;

	if ((A = dyn_cast<Argument>(V))) {
		OS << A->getParent()->getName() << "." << A->getName();
	} else if ((I = dyn_cast<Instruction>(V))) {
		OS << I->getParent()->getParent()->getName() << "."
				<< I->getParent()->getName() << "." << I->getName();
	} else {
		OS << V->getName();
	}
}

/// Selects the instructions that we are going to evaluate.
static bool isValidInstruction(const Instruction* I) {

	switch (I->getOpcode()) {
	case Instruction::PHI:
	case Instruction::Add:
	case Instruction::Sub:
	case Instruction::Mul:
	case Instruction::UDiv:
	case Instruction::SDiv:
	case Instruction::URem:
	case Instruction::SRem:
	case Instruction::Shl:
	case Instruction::LShr:
	case Instruction::AShr:
	case Instruction::And:
	case Instruction::Or:
	case Instruction::Xor:
	case Instruction::Trunc:
	case Instruction::ZExt:
	case Instruction::SExt:
	case Instruction::Load:
	case Instruction::Store:
		return true;
	default:
		return false;
	}

	assert(false && "It should not be here.");
	return false;
}

// ========================================================================== //
// RangeAnalysis
// ========================================================================== //
unsigned RangeAnalysis::getMaxBitWidth(const Function& F) {
	unsigned int InstBitSize = 0, opBitSize = 0, max = 0;

	// Obtains the maximum bit width of the instructions of the function.
	for (const_inst_iterator I = inst_begin(F), E = inst_end(F); I != E; ++I) {
		InstBitSize = I->getType()->getPrimitiveSizeInBits();
		if (I->getType()->isIntegerTy() && InstBitSize > max) {
			max = InstBitSize;
		}

		// Obtains the maximum bit width of the operands of the instruction.
		User::const_op_iterator bgn = I->op_begin(), end = I->op_end();
		for (; bgn != end; ++bgn) {
			opBitSize = (*bgn)->getType()->getPrimitiveSizeInBits();
			if ((*bgn)->getType()->isIntegerTy() && opBitSize > max) {
				max = opBitSize;
			}
		}
	}

	// Bitwidth equal to 0 is not valid, so we increment to 1
	if (max == 0) ++max;
	
	return max;
}

void RangeAnalysis::updateMinMax(unsigned maxBitWidth) {
	// Updates the Min and Max values.
	Min = APInt::getSignedMinValue(maxBitWidth);
	Max = APInt::getSignedMaxValue(maxBitWidth);
	Zero = APInt(MAX_BIT_INT, 0, true);
}

// ========================================================================== //
// IntraProceduralRangeAnalysis
// ========================================================================== //
template <class CGT>
Range IntraProceduralRA<CGT>::getRange(const Value *v){
	return CG->getRange(v);
}

template<class CGT>
bool IntraProceduralRA<CGT>::runOnFunction(Function &F) {
//	if(CG) delete CG;
	CG = new CGT();

	MAX_BIT_INT = getMaxBitWidth(F);
	updateMinMax(MAX_BIT_INT);

	// Build the graph and find the intervals of the variables.
	Profile::TimeValue before = prof.timenow();
	CG->buildGraph(F);
	CG->buildVarNodes();
	Profile::TimeValue elapsed = prof.timenow() - before;
	prof.updateTime("BuildGraph", elapsed);
#ifdef PRINT_DEBUG
	CG->printToFile(F, "/tmp/" + F.getName() + "cgpre.dot");
	errs() << "Analyzing function " << F.getName() << ":\n";
#endif
	CG->findIntervals();
#ifdef PRINT_DEBUG
	CG->printToFile(F, "/tmp/" + F.getName() + "cgpos.dot");
#endif

	return false;
}

template<class CGT>
void IntraProceduralRA<CGT>::getAnalysisUsage(AnalysisUsage &AU) const {
	AU.setPreservesAll();
}

template <class CGT>
IntraProceduralRA<CGT>::~IntraProceduralRA(){
	prof.printTime("BuildGraph");
	prof.printTime("Nuutila");
	prof.printTime("SCCs resolution");
	prof.printTime("ComputeStats");
	
	std::ostringstream formated;
	formated << 100 * (1.0 - ((double)(needBits) / usedBits));
	errs() << formated.str() << "\t - " << " Percentage of reduction\n";

//	delete CG;
}

// ========================================================================== //
// InterProceduralRangeAnalysis
// ========================================================================== //
template <class CGT>
Range InterProceduralRA<CGT>::getRange(const Value *v){
	return CG->getRange(v);
}

template<class CGT>
unsigned InterProceduralRA<CGT>::getMaxBitWidth(Module &M) {
	unsigned max = 0;
	// Search through the functions for the max int bitwidth
	for (Module::iterator I = M.begin(), E = M.end(); I != E; ++I) {
		if (!I->isDeclaration()) {
			unsigned bitwidth = RangeAnalysis::getMaxBitWidth(*I);

			if (bitwidth > max)
				max = bitwidth;
		}
	}
	return max;
}

template<class CGT>
bool InterProceduralRA<CGT>::runOnModule(Module &M) {
	// Constraint Graph
//	if(CG) delete CG;
	CG = new CGT();

	MAX_BIT_INT = getMaxBitWidth(M);
	updateMinMax(MAX_BIT_INT);

	// Build the Constraint Graph by running on each function
	Profile::TimeValue before = prof.timenow();
	
	for (Module::iterator I = M.begin(), E = M.end(); I != E; ++I) {
		// If the function is only a declaration, or if it has variable number of arguments, do not match
		if (I->isDeclaration() || I->isVarArg())
			continue;
			
		CG->buildGraph(*I);
		MatchParametersAndReturnValues(*I, *CG);
	}
	CG->buildVarNodes();
	
	Profile::TimeValue elapsed = prof.timenow() - before;
	prof.updateTime("BuildGraph", elapsed);

#ifdef PRINT_DEBUG
	CG->printToFile(*(M.begin()), "/tmp/" + M.getModuleIdentifier() + ".cgpre.dot");
#endif
	CG->findIntervals();
#ifdef PRINT_DEBUG
	CG->printToFile(*(M.begin()), "/tmp/" + M.getModuleIdentifier() + ".cgpos.dot");
#endif
	// FIXME: Não sei se tem que retornar true ou false
	return true;
}

template<class CGT>
void InterProceduralRA<CGT>::MatchParametersAndReturnValues(Function &F,
		ConstraintGraph &G) {
	// Only do the matching if F has any use
	if (!F.hasNUsesOrMore(1)) {
		return;
	}

	// Data structure which contains the matches between formal and real parameters
	// First: formal parameter
	// Second: real parameter
	SmallVector<std::pair<Value*, Value*>, 4> Parameters(F.arg_size());

	// Fetch the function arguments (formal parameters) into the data structure
	Function::arg_iterator argptr;
	Function::arg_iterator e;
	unsigned i;

	for (i = 0, argptr = F.arg_begin(), e = F.arg_end(); argptr != e;
			++i, ++argptr)
		Parameters[i].first = argptr;

	// Check if the function returns a supported value type. If not, no return value matching is done
	bool noReturn = F.getReturnType()->isVoidTy();

	// Creates the data structure which receives the return values of the function, if there is any
	SmallPtrSet<Value*, 8> ReturnValues;

	if (!noReturn) {
		// Iterate over the basic blocks to fetch all possible return values
		for (Function::iterator bb = F.begin(), bbend = F.end(); bb != bbend;
				++bb) {
			// Get the terminator instruction of the basic block and check if it's
			// a return instruction: if it's not, continue to next basic block
			Instruction *terminator = bb->getTerminator();

			ReturnInst *RI = dyn_cast<ReturnInst>(terminator);

			if (!RI)
				continue;

			// Get the return value and insert in the data structure
			ReturnValues.insert(RI->getReturnValue());
		}
	}

	// For each use of F, get the real parameters and the caller instruction to do the matching
	std::vector<PhiOp*> matchers(F.arg_size(), NULL);

	for (unsigned i = 0, e = Parameters.size(); i < e; ++i) {
		VarNode *sink = G.addVarNode(Parameters[i].first);

		matchers[i] = new PhiOp(new BasicInterval(), sink, NULL,
				Instruction::PHI);

		// Insert the operation in the graph.
		G.getOprs()->insert(matchers[i]);

		// Insert this definition in defmap
		(*G.getDefMap())[sink->getValue()] = matchers[i];
	}

	// For each return value, create a node
	std::vector<VarNode*> returnvars;

	for (SmallPtrSetIterator<Value*> ri = ReturnValues.begin(), re =
			ReturnValues.end(); ri != re; ++ri) {
		// Add VarNode to the CG
		VarNode *from = G.addVarNode(*ri);

		returnvars.push_back(from);
	}

	for (Value::use_iterator UI = F.use_begin(), E = F.use_end(); UI != E;
			++UI) {
		User *U = *UI;

		// Ignore blockaddress uses
		if (isa<BlockAddress>(U))
			continue;

		// Used by a non-instruction, or not the callee of a function, do not
		// match.
		if (!isa<CallInst>(U) && !isa<InvokeInst>(U))
			continue;

		Instruction *caller = cast<Instruction>(U);

		CallSite CS(caller);
		if (!CS.isCallee(UI))
			continue;

		// Iterate over the real parameters and put them in the data structure
		CallSite::arg_iterator AI;
		CallSite::arg_iterator EI;

		for (i = 0, AI = CS.arg_begin(), EI = CS.arg_end(); AI != EI; ++i, ++AI)
			Parameters[i].second = *AI;

		// // Do the interprocedural construction of CG
		VarNode* to = NULL;
		VarNode* from = NULL;

		// Match formal and real parameters
		for (i = 0; i < Parameters.size(); ++i) {
			// Add real parameter to the CG
			from = G.addVarNode(Parameters[i].second);

			// Connect nodes
			matchers[i]->addSource(from);

			// Inserts the sources of the operation in the use map list.
			G.getUseMap()->find(from->getValue())->second.insert(matchers[i]);
		}

		// Match return values
		if (!noReturn) {
			// Add caller instruction to the CG (it receives the return value)
			to = G.addVarNode(caller);

			PhiOp *phiOp = new PhiOp(new BasicInterval(), to, NULL,
					Instruction::PHI);

			// Insert the operation in the graph.
			G.getOprs()->insert(phiOp);

			// Insert this definition in defmap
			(*G.getDefMap())[to->getValue()] = phiOp;

			for (std::vector<VarNode*>::iterator vit = returnvars.begin(),
					vend = returnvars.end(); vit != vend; ++vit) {
				VarNode *var = *vit;
				phiOp->addSource(var);

				// Inserts the sources of the operation in the use map list.
				G.getUseMap()->find(var->getValue())->second.insert(phiOp);
			}
		}

		// Real parameters are cleaned before moving to the next use (for safety's sake)
		for (i = 0; i < Parameters.size(); ++i)
			Parameters[i].second = NULL;
	}
}

template <class CGT>
InterProceduralRA<CGT>::~InterProceduralRA(){
	prof.printTime("BuildGraph");
	prof.printTime("Nuutila");
	prof.printTime("SCCs resolution");
	prof.printTime("ComputeStats");
	
	std::ostringstream formated;
	formated << 100 * (1.0 - ((double)(needBits) / usedBits));
	errs() << formated.str() << "\t - " << " Percentage of reduction\n";
}

template<class CGT>
char IntraProceduralRA<CGT>::ID = 0;
static RegisterPass<IntraProceduralRA<Cousot> > Y("ra-intra-cousot",
		"Range Analysis (Cousot - intra)");
static RegisterPass<IntraProceduralRA<CropDFS> > Z("ra-intra-crop",
		"Range Analysis (Crop - intra)");
template<class CGT>
char InterProceduralRA<CGT>::ID = 2;
static RegisterPass<InterProceduralRA<Cousot> > W("ra-inter-cousot",
		"Range Analysis (Cousot - inter)");
static RegisterPass<InterProceduralRA<CropDFS> > X("ra-inter-crop",
		"Range Analysis (Crop - inter)");

// ========================================================================== //
// Range
// ========================================================================== //
Range::Range() :
		l(Min), u(Max), type(Regular) {
}

Range::Range(APInt lb, APInt ub, RangeType rType = Regular) :
		l(lb), u(ub), type(rType) {
}

Range::~Range() {
}

bool Range::isMaxRange() const {
	return this->getLower().eq(Min) && this->getUpper().eq(Max);
}

/// Add and Mul are commutatives. So, they are a little different 
/// of the other operations.
Range Range::add(const Range& other) {
	APInt l = Min, u = Max;
	if (this->getLower().ne(Min) && other.getLower().ne(Min)) {
		l = getLower() + other.getLower();
	}

	if (this->getUpper().ne(Max) && other.getUpper().ne(Max)) {
		u = getUpper() + other.getUpper();
	}

	return Range(l, u);
}

/// [a, b] − [c, d] = 
/// [min (a − c, a − d, b − c, b − d), 
/// max (a − c, a − d, b − c, b − d)] = [a − d, b − c]
/// The other operations are just like this operation.
Range Range::sub(const Range& other) {
	const APInt &a = this->getLower();
	const APInt &b = this->getUpper();
	const APInt &c = other.getLower();
	const APInt &d = other.getUpper();
	APInt l, u;

	//a-d
	if (a.eq(Min) || d.eq(Max))
		l = Min;
	else
		//FIXME: handle overflow when a-d < Min
		l = a - d;

	//b-c
	if (b.eq(Max) || c.eq(Min))
		u = Max;
	else
		//FIXME: handle overflow when b-c > Max
		u = b - c;

	return Range(l, u);
}

#define MUL_HELPER(x,y) x.eq(Max) ? \
							(y.slt(Zero) ? Min : (y.eq(Zero) ? Zero : Max)) \
							: (y.eq(Max) ? \
								(x.slt(Zero) ? Min :(x.eq(Zero) ? Zero : Max)) \
								:(x.eq(Min) ? \
									(y.slt(Zero) ? Max : (y.eq(Zero) ? Zero : Min)) \
									: (y.eq(Min) ? \
										(x.slt(Zero) ? Max : (x.eq(Zero) ? Zero : Min)) \
										:(x*y))))

/// Add and Mul are commutatives. So, they are a little different 
/// of the other operations.
// [a, b] * [c, d] = [Min(a*c, a*d, b*c, b*d), Max(a*c, a*d, b*c, b*d)]
Range Range::mul(const Range& other) {
	if (this->isMaxRange() || other.isMaxRange()) {
		return Range(Min, Max);
	}

	const APInt &a = this->getLower();
	const APInt &b = this->getUpper();
	const APInt &c = other.getLower();
	const APInt &d = other.getUpper();

	APInt candidates[4];
	candidates[0] = MUL_HELPER(a,c);
	candidates[1] = MUL_HELPER(a,d);
	candidates[2] = MUL_HELPER(b,c);
	candidates[3] = MUL_HELPER(b,d);

	//Lower bound is the min value from the vector, while upper bound is the max value
	APInt *min = &candidates[0];
	APInt *max = &candidates[0];

	for (unsigned i = 1; i < 4; ++i) {
		if (candidates[i].sgt(*max))
			max = &candidates[i];
		else if (candidates[i].slt(*min))
			min = &candidates[i];
	}

	return Range(*min, *max);
}

Range Range::udiv(const Range& other) {
	APInt ll = Min, lu = Min, ul = Max, uu = Max;
	if (this->getLower().ne(Min) && other.getLower().ne(Min)) {
		if (other.getLower().ne(APInt::getNullValue(MAX_BIT_INT))) {
			ll = this->getLower().udiv(other.getLower()); // lower lower
		}
	}

	if (this->getLower().ne(Min) && other.getUpper().ne(Max)) {
		if (other.getUpper().ne(APInt::getNullValue(MAX_BIT_INT))) {
			lu = this->getLower().udiv(other.getUpper()); // lower upper
		}
	}

	if (this->getUpper().ne(Max) && other.getLower().ne(Min)) {
		if (other.getLower().ne(APInt::getNullValue(MAX_BIT_INT))) {
			ul = this->getUpper().udiv(other.getLower()); // upper lower
		}
	}

	if (this->getUpper().ne(Max) && other.getUpper().ne(Max)) {
		if (other.getUpper().ne(APInt::getNullValue(MAX_BIT_INT))) {
			uu = this->getUpper().udiv(other.getUpper()); // upper upper
		}
	}

	APInt l = ll.slt(lu) ? ll : lu;
	APInt u = uu.sgt(ul) ? uu : ul;

	return Range(l, u);
}

Range Range::sdiv(const Range& other) {
	APInt ll = Min, lu = Min, ul = Max, uu = Max;
	if (this->getLower().ne(Min) && other.getLower().ne(Min)) {
		if (other.getLower().ne(APInt::getNullValue(MAX_BIT_INT))) {
			ll = this->getLower().sdiv(other.getLower()); // lower lower
		}
	}

	if (this->getLower().ne(Min) && other.getUpper().ne(Max)) {
		if (other.getUpper().ne(APInt::getNullValue(MAX_BIT_INT))) {
			lu = this->getLower().sdiv(other.getUpper()); // lower upper
		}
	}

	if (this->getUpper().ne(Max) && other.getLower().ne(Min)) {
		if (other.getLower().ne(APInt::getNullValue(MAX_BIT_INT))) {
			ul = this->getUpper().sdiv(other.getLower()); // upper lower
		}
	}

	if (this->getUpper().ne(Max) && other.getUpper().ne(Max)) {
		if (other.getUpper().ne(APInt::getNullValue(MAX_BIT_INT))) {
			uu = this->getUpper().sdiv(other.getUpper()); // upper upper
		}
	}

	APInt l = ll.slt(lu) ? ll : lu;
	APInt u = uu.sgt(ul) ? uu : ul;

	return Range(l, u);
}

Range Range::urem(const Range& other) {
	// Deal with mod 0 exception
	if (other.getLower().eq(Zero) || other.getUpper().eq(Zero)) {
		return Range(Min, Max);
	}
	
	APInt ll = Min, lu = Min, ul = Max, uu = Max;
	if (this->getLower().ne(Min) && other.getLower().ne(Min)) {
		ll = this->getLower().urem(other.getLower()); // lower lower
	}

	if (this->getLower().ne(Min) && other.getUpper().ne(Max)) {
		lu = this->getLower().urem(other.getUpper()); // lower upper
	}

	if (this->getUpper().ne(Max) && other.getLower().ne(Min)) {
		ul = this->getUpper().urem(other.getLower()); // upper lower
	}

	if (this->getUpper().ne(Max) && other.getUpper().ne(Max)) {
		uu = this->getUpper().urem(other.getUpper()); // upper upper
	}

	APInt l = ll.slt(lu) ? ll : lu;
	APInt u = uu.sgt(ul) ? uu : ul;

	return Range(l, u);
}

Range Range::srem(const Range& other) {
	// Deal with mod 0 exception
	if (other.getLower().eq(Zero) || other.getUpper().eq(Zero)) {
		return Range(Min, Max);
	}
	
	APInt ll = Min, lu = Min, ul = Max, uu = Max;

	if(other == Range(Zero,Zero) || other == Range(Min, Max, Empty))
		return Range(Min, Max, Empty);

	if((other.getLower().slt(Zero) && other.getUpper().sgt(Zero))
		|| other.getLower().eq(Zero)
		|| other.getUpper().eq(Zero))
		return Range(Min, Max);

	if (this->getLower().ne(Min) && other.getLower().ne(Min)) {
		ll = this->getLower().srem(other.getLower()); // lower lower
	}

	if (this->getLower().ne(Min) && other.getUpper().ne(Max)) {
		lu = this->getLower().srem(other.getUpper()); // lower upper
	}

	if (this->getUpper().ne(Max) && other.getLower().ne(Min)) {
		ul = this->getUpper().srem(other.getLower()); // upper lower
	}

	if (this->getUpper().ne(Max) && other.getUpper().ne(Max)) {
		uu = this->getUpper().srem(other.getUpper()); // upper upper
	}

	APInt l = ll.slt(lu) ? ll : lu;
	APInt u = uu.sgt(ul) ? uu : ul;

	return Range(l, u);
}

Range Range::shl(const Range& other) {
	APInt ll = Min, lu = Min, ul = Max, uu = Max;
	if (this->getLower().ne(Min) && other.getLower().ne(Min)) {
		ll = this->getLower().shl(other.getLower()); // lower lower
	}

	if (this->getLower().ne(Min) && other.getUpper().ne(Max)) {
		lu = this->getLower().shl(other.getUpper()); // lower upper
	}

	if (this->getUpper().ne(Max) && other.getLower().ne(Min)) {
		ul = this->getUpper().shl(other.getLower()); // upper lower
	}

	if (this->getUpper().ne(Max) && other.getUpper().ne(Max)) {
		uu = this->getUpper().shl(other.getUpper()); // upper upper
	}

	APInt l = ll.slt(lu) ? ll : lu;
	APInt u = uu.sgt(ul) ? uu : ul;

	return Range(l, u);
}

Range Range::lshr(const Range& other) {
	
	// If any of the bounds is negative, result is [0, +inf] automatically
	if (this->getLower().isNegative() || this->getUpper().isNegative()) {
		return Range(Zero, Max);
	}
	
	APInt ll = Min, lu = Min, ul = Max, uu = Max;	
	
	if (this->getLower().ne(Min) && other.getLower().ne(Min)) {
		ll = this->getLower().lshr(other.getLower()); // lower lower
	}

	if (this->getLower().ne(Min) && other.getUpper().ne(Max)) {
		lu = this->getLower().lshr(other.getUpper()); // lower upper
	}

	if (this->getUpper().ne(Max) && other.getLower().ne(Min)) {
		ul = this->getUpper().lshr(other.getLower()); // upper lower
	}

	if (this->getUpper().ne(Max) && other.getUpper().ne(Max)) {
		uu = this->getUpper().lshr(other.getUpper()); // upper upper
	}

	APInt l = ll.slt(lu) ? ll : lu;
	APInt u = uu.sgt(ul) ? uu : ul;

	return Range(l, u);
}

Range Range::ashr(const Range& other) {
	APInt ll = Min, lu = Min, ul = Max, uu = Max;
	if (this->getLower().ne(Min) && other.getLower().ne(Min)) {
		ll = this->getLower().ashr(other.getLower()); // lower lower
	}

	if (this->getLower().ne(Min) && other.getUpper().ne(Max)) {
		lu = this->getLower().ashr(other.getUpper()); // lower upper
	}

	if (this->getUpper().ne(Max) && other.getLower().ne(Min)) {
		ul = this->getUpper().ashr(other.getLower()); // upper lower
	}

	if (this->getUpper().ne(Max) && other.getUpper().ne(Max)) {
		uu = this->getUpper().ashr(other.getUpper()); // upper upper
	}

	APInt l = ll.slt(lu) ? ll : lu;
	APInt u = uu.sgt(ul) ? uu : ul;

	return Range(l, u);
}

Range Range::And(const Range& other) {
	APInt ll = Min, lu = Min, ul = Max, uu = Max;
	if (this->getLower().ne(Min) && other.getLower().ne(Min)) {
		ll = this->getLower().And(other.getLower()); // lower lower
	}

	if (this->getLower().ne(Min) && other.getUpper().ne(Max)) {
		lu = this->getLower().And(other.getUpper()); // lower upper
	}

	if (this->getUpper().ne(Max) && other.getLower().ne(Min)) {
		ul = this->getUpper().And(other.getLower()); // upper lower
	}

	if (this->getUpper().ne(Max) && other.getUpper().ne(Max)) {
		uu = this->getUpper().And(other.getUpper()); // upper upper
	}

	APInt l = ll.slt(lu) ? ll : lu;
	APInt u = uu.sgt(ul) ? uu : ul;

	return Range(l, u);
}

Range Range::Or(const Range& other) {
	if (this->isUnknown() || other.isUnknown()) {
		return Range(Min, Max, Unknown);
	}

	APInt ll = Min, lu = Min, ul = Max, uu = Max;
	if (this->getLower().ne(Min) && other.getLower().ne(Min)) {
		ll = this->getLower().Or(other.getLower()); // lower lower
	}

	if (this->getLower().ne(Min) && other.getUpper().ne(Max)) {
		lu = this->getLower().Or(other.getUpper()); // lower upper
	}

	if (this->getUpper().ne(Max) && other.getLower().ne(Min)) {
		ul = this->getUpper().Or(other.getLower()); // upper lower
	}

	if (this->getUpper().ne(Max) && other.getUpper().ne(Max)) {
		uu = this->getUpper().Or(other.getUpper()); // upper upper
	}

	APInt l = ll.slt(lu) ? ll : lu;
	APInt u = uu.sgt(ul) ? uu : ul;

	return Range(l, u);
}

Range Range::Xor(const Range& other) {
	APInt ll = Min, lu = Min, ul = Max, uu = Max;
	if (this->getLower().ne(Min) && other.getLower().ne(Min)) {
		ll = this->getLower().Xor(other.getLower()); // lower lower
	}

	if (this->getLower().ne(Min) && other.getUpper().ne(Max)) {
		lu = this->getLower().Xor(other.getUpper()); // lower upper
	}

	if (this->getUpper().ne(Max) && other.getLower().ne(Min)) {
		ul = this->getUpper().Xor(other.getLower()); // upper lower
	}

	if (this->getUpper().ne(Max) && other.getUpper().ne(Max)) {
		uu = this->getUpper().Xor(other.getUpper()); // upper upper
	}

	APInt l = ll.slt(lu) ? ll : lu;
	APInt u = uu.sgt(ul) ? uu : ul;

	return Range(l, u);
}

// Truncate
//		- if the source range is entirely inside max bit range, he is the result
//      - else, the result is the max bit range
Range Range::truncate(unsigned bitwidth) const {
	APInt maxupper = APInt::getSignedMaxValue(bitwidth);
	APInt maxlower = APInt::getSignedMinValue(bitwidth);

	if (bitwidth < MAX_BIT_INT) {
		maxupper = maxupper.sext(MAX_BIT_INT);
		maxlower = maxlower.sext(MAX_BIT_INT);
	}

	// Check if source range is contained by max bit range
	if (this->getLower().sge(maxlower) && this->getUpper().sle(maxupper)) {
		return *this;
	} else {
		return Range(maxlower, maxupper);
	}
}

Range Range::sextOrTrunc(unsigned bitwidth) const {
	return truncate(bitwidth);
}

Range Range::zextOrTrunc(unsigned bitwidth) const {
	APInt maxupper = APInt::getSignedMaxValue(bitwidth);
	APInt maxlower = APInt::getSignedMinValue(bitwidth);

	if (bitwidth < MAX_BIT_INT) {
		maxupper = maxupper.sext(MAX_BIT_INT);
		maxlower = maxlower.sext(MAX_BIT_INT);
	}

	return Range(maxlower, maxupper);
}

Range Range::intersectWith(const Range& other) const {
	if (this->isEmpty() || other.isEmpty())
		return Range(Min, Max, Empty);

	if (this->isUnknown()) {
		return other;
	}

	if (other.isUnknown()) {
		return *this;
	}

	APInt l = getLower().sgt(other.getLower()) ? getLower() : other.getLower();
	APInt u = getUpper().slt(other.getUpper()) ? getUpper() : other.getUpper();
	return Range(l, u);
}

Range Range::unionWith(const Range& other) const {
	if (this->isEmpty()) {
		return other;
	}

	if (other.isEmpty()) {
		return *this;
	}

	if (this->isUnknown()) {
		return other;
	}

	if (other.isUnknown()) {
		return *this;
	}

	APInt l = getLower().slt(other.getLower()) ? getLower() : other.getLower();
	APInt u = getUpper().sgt(other.getUpper()) ? getUpper() : other.getUpper();
	return Range(l, u);
}

bool Range::operator==(const Range& other) const {
	return this->type == other.type && getLower().eq(other.getLower())
			&& getUpper().eq(other.getUpper());
}

bool Range::operator!=(const Range& other) const {
	return this->type != other.type || getLower().ne(other.getLower())
			|| getUpper().ne(other.getUpper());
}

void Range::print(raw_ostream& OS) const {
	if (this->isUnknown()) {
		OS << "Unknown";
		return;
	}

	if (this->isEmpty()) {
		OS << "Empty";
		return;
	}

	if (getLower().eq(Min)) {
		OS << "[-inf, ";
	} else {
		OS << "[" << getLower() << ", ";
	}

	if (getUpper().eq(Max)) {
		OS << "+inf]";
	} else {
		OS << getUpper() << "]";
	}
}

raw_ostream& operator<<(raw_ostream& OS, const Range& R) {
	R.print(OS);
	return OS;
}

// ========================================================================== //
// BasicInterval
// ========================================================================== //

BasicInterval::BasicInterval(const Range& range) :
		range(range) {
}

BasicInterval::BasicInterval() :
		range(Range(Min, Max)) {
}

BasicInterval::BasicInterval(const APInt& l, const APInt& u) :
		range(Range(l, u)) {
}

// This is a base class, its dtor must be virtual.
BasicInterval::~BasicInterval() {
}

/// Pretty print.
void BasicInterval::print(raw_ostream& OS) const {
	this->getRange().print(OS);
}

// ========================================================================== //
// SymbInterval
// ========================================================================== //

SymbInterval::SymbInterval(const Range& range, const Value* bound,
		CmpInst::Predicate pred) :
		BasicInterval(range), bound(bound), pred(pred) {
}

SymbInterval::~SymbInterval() {
}

Range SymbInterval::fixIntersects(VarNode* bound, VarNode* sink) {
	// Get the lower and the upper bound of the
	// node which bounds this intersection.
	APInt l = bound->getRange().getLower();
	APInt u = bound->getRange().getUpper();

	// Get the lower and upper bound of the interval of this operation
	APInt lower = sink->getRange().getLower();
	APInt upper = sink->getRange().getUpper();

	switch (this->getOperation()) {
	case ICmpInst::ICMP_EQ: // equal
		return Range(l, u);
		break;
	case ICmpInst::ICMP_SLE: // signed less or equal
		return Range(lower, u);
		break;
	case ICmpInst::ICMP_SLT: // signed less than
		if (u != Max) {
			return Range(lower, u - 1);
		} else {
			return Range(lower, u);
		}
		break;
	case ICmpInst::ICMP_SGE: // signed greater or equal
		return Range(l, upper);
		break;
	case ICmpInst::ICMP_SGT: // signed greater than
		if (l != Min) {
			return Range(l + 1, upper);
		} else {
			return Range(l, upper);
		}
		break;
	default:
		return Range(Min, Max);
	}

	return Range(Min, Max);
}

/// Pretty print.
void SymbInterval::print(raw_ostream& OS) const {
	switch (this->getOperation()) {
	case ICmpInst::ICMP_EQ: // equal
		OS << "[lb(";
		printVarName(getBound(), OS);
		OS << "), ub(";
		printVarName(getBound(), OS);
		OS << ")]";
		break;
	case ICmpInst::ICMP_SLE: // sign less or equal
		OS << "[-inf, ub(";
		printVarName(getBound(), OS);
		OS << ")]";
		break;
	case ICmpInst::ICMP_SLT: // sign less than
		OS << "[-inf, ub(";
		printVarName(getBound(), OS);
		OS << ") - 1]";
		break;
	case ICmpInst::ICMP_SGE: // sign greater or equal
		OS << "[lb(";
		printVarName(getBound(), OS);
		OS << "), +inf]";
		break;
	case ICmpInst::ICMP_SGT: // sign greater than
		OS << "[lb(";
		printVarName(getBound(), OS);
		OS << " - 1), +inf]";
		break;
	default:
		OS << "Unknown Instruction.\n";
	}
}

// ========================================================================== //
// VarNode
// ========================================================================== //

/// The ctor.
VarNode::VarNode(const Value* V) :
		V(V), interval(Range(Min, Max, Unknown)) {
}

/// The dtor.
VarNode::~VarNode() {
}

/// Initializes the value of the node.
void VarNode::init(bool outside) {
	const Value* V = this->getValue();
	if (const ConstantInt *CI = dyn_cast<ConstantInt>(V)) {
		APInt tmp = CI->getValue();
		if (tmp.getBitWidth() < MAX_BIT_INT) {
			tmp = tmp.sext(MAX_BIT_INT);
		}
		this->setRange(Range(tmp, tmp));
	} else {
		if (!outside) {
			// Initialize with a basic, unknown, interval.
			this->setRange(Range(Min, Max, Unknown));
		} else {
			this->setRange(Range(Min, Max));
		}
	}
}

/// Pretty print.
void VarNode::print(raw_ostream& OS) const {
	if (const ConstantInt* C = dyn_cast<ConstantInt>(this->getValue())) {
		OS << C->getValue();
	} else {
		printVarName(this->getValue(), OS);
	}
	OS << " ";
	this->getRange().print(OS);
}

void VarNode::storeAbstractState() {
	ASSERT(!this->interval.isUnknown(), "storeAbstractState doesn't handle empty set")

	if (this->interval.getLower().eq(Min))
		if (this->interval.getUpper().eq(Max))
			this->abstractState = '?';
		else
			this->abstractState = '-';
	else if (this->interval.getUpper().eq(Max))
		this->abstractState = '+';
	else
		this->abstractState = '0';
}

raw_ostream& operator<<(raw_ostream& OS, const VarNode* VN) {
	VN->print(OS);
	return OS;
}

// ========================================================================== //
// BasicOp
// ========================================================================== //

/// We can not want people creating objects of this class,
/// but we want to inherit of it.
BasicOp::BasicOp(BasicInterval* intersect, VarNode* sink,
		const Instruction *inst) :
		intersect(intersect), sink(sink), inst(inst) {
}

/// We can not want people creating objects of this class,
/// but we want to inherit of it.
BasicOp::~BasicOp() {
	delete intersect;
}

/// Replace symbolic intervals with hard-wired constants.
void BasicOp::fixIntersects(VarNode* V) {
	if (SymbInterval * SI = dyn_cast<SymbInterval>(getIntersect())) {
		Range r = SI->fixIntersects(V, getSink());
		this->setIntersect(SI->fixIntersects(V, getSink()));
	}
}

// ========================================================================== //
// ControlDep
// ========================================================================== //

ControlDep::ControlDep(VarNode* sink, VarNode* source) :
		BasicOp(new BasicInterval(), sink, NULL), source(source) {
}

ControlDep::~ControlDep() {
}

Range ControlDep::eval() const {
	return Range(Min, Max);
}

void ControlDep::print(raw_ostream& OS) const {
}

// ========================================================================== //
// UnaryOp
// ========================================================================== //

UnaryOp::UnaryOp(BasicInterval* intersect, VarNode* sink,
		const Instruction *inst, VarNode* source, unsigned int opcode) :
		BasicOp(intersect, sink, inst), source(source), opcode(opcode) {
}

// The dtor.
UnaryOp::~UnaryOp() {
}

/// Computes the interval of the sink based on the interval of the sources,
/// the operation and the interval associated to the operation.
Range UnaryOp::eval() const {

	unsigned bw = getSink()->getValue()->getType()->getPrimitiveSizeInBits();
	Range oprnd = source->getRange();
	Range result(Min, Max, Unknown);

	if (oprnd.isRegular()) {
		switch (this->getOpcode()) {
		case Instruction::Trunc:
			result = oprnd.truncate(bw);
			break;
		case Instruction::ZExt:
			result = oprnd.zextOrTrunc(bw);
			break;
		case Instruction::SExt:
			result = oprnd.sextOrTrunc(bw);
			break;
		default:
			// Loads and Stores are handled here.
			result = oprnd;
			break;
		}
	} else if (oprnd.isEmpty())
		result = Range(Min, Max, Empty);

	if (!getIntersect()->getRange().isMaxRange()) {
		Range aux(getIntersect()->getRange());
		result = result.intersectWith(aux);
	}

	// To ensure that we always are dealing with the correct bit width.
	return result;
}

/// Prints the content of the operation. I didn't it an operator overload
/// because I had problems to access the members of the class outside it.
void UnaryOp::print(raw_ostream& OS) const {
	const char* quot = "\"";
	OS << " " << quot << this << quot << " [label=\"";

	// Instruction bitwidth
	unsigned bw = getSink()->getValue()->getType()->getPrimitiveSizeInBits();

	switch (this->opcode) {
	case Instruction::Trunc:
		OS << "trunc i" << bw;
		break;
	case Instruction::ZExt:
		OS << "zext i" << bw;
		break;
	case Instruction::SExt:
		OS << "sext i" << bw;
		break;
	default:
		// Phi functions, Loads and Stores are handled here.
		this->getIntersect()->print(OS);
		break;
	}

	OS << "\"]\n";

	const Value* V = this->getSource()->getValue();
	if (const ConstantInt* C = dyn_cast<ConstantInt>(V)) {
		OS << " " << C->getValue() << " -> " << quot << this << quot << "\n";
	} else {
		OS << " " << quot;
		printVarName(V, OS);
		OS << quot << " -> " << quot << this << quot << "\n";
	}

	const Value* VS = this->getSink()->getValue();
	OS << " " << quot << this << quot << " -> " << quot;
	printVarName(VS, OS);
	OS << quot << "\n";
}

// ========================================================================== //
// SigmaOp
// ========================================================================== //

SigmaOp::SigmaOp(BasicInterval* intersect, VarNode* sink,
		const Instruction *inst, VarNode* source, unsigned int opcode) :
		UnaryOp(intersect, sink, inst, source, opcode), unresolved(false) {
}

// The dtor.
SigmaOp::~SigmaOp() {
}

/// Computes the interval of the sink based on the interval of the sources,
/// the operation and the interval associated to the operation.
Range SigmaOp::eval() const {
	Range result = this->getSource()->getRange();

	result = result.intersectWith(getIntersect()->getRange());

	return result;
}

/// Prints the content of the operation. I didn't it an operator overload
/// because I had problems to access the members of the class outside it.
void SigmaOp::print(raw_ostream& OS) const {
	const char* quot = "\"";
	OS << " " << quot << this << quot << " [label=\"";
	this->getIntersect()->print(OS);
	OS << "\"]\n";
	const Value* V = this->getSource()->getValue();
	if (const ConstantInt* C = dyn_cast<ConstantInt>(V)) {
		OS << " " << C->getValue() << " -> " << quot << this << quot << "\n";
	} else {
		OS << " " << quot;
		printVarName(V, OS);
		OS << quot << " -> " << quot << this << quot << "\n";
	}

	const Value* VS = this->getSink()->getValue();
	OS << " " << quot << this << quot << " -> " << quot;
	printVarName(VS, OS);
	OS << quot << "\n";
}

// ========================================================================== //
// BinaryOp
// ========================================================================== //

// The ctor.
BinaryOp::BinaryOp(BasicInterval* intersect, VarNode* sink,
		const Instruction* inst, VarNode* source1, VarNode* source2,
		unsigned int opcode) :
		BasicOp(intersect, sink, inst), source1(source1), source2(source2), opcode(
				opcode) {
}

/// The dtor.
BinaryOp::~BinaryOp() {
}

/// Computes the interval of the sink based on the interval of the sources,
/// the operation and the interval associated to the operation.
/// Basically, this function performs the operation indicated in its opcode
/// taking as its operands the source1 and the source2.
Range BinaryOp::eval() const {

	Range op1 = this->getSource1()->getRange();
	Range op2 = this->getSource2()->getRange();
	Range result(Min, Max, Unknown);

	//only evaluate if all operands are Regular
	if (op1.isRegular() && op2.isRegular()) {
		switch (this->getOpcode()) {
		case Instruction::Add:
			result = op1.add(op2);
			break;
		case Instruction::Sub:
			result = op1.sub(op2);
			break;
		case Instruction::Mul:
			result = op1.mul(op2);
			break;
		case Instruction::UDiv:
			result = op1.udiv(op2);
			break;
		case Instruction::SDiv:
			result = op1.sdiv(op2);
			break;
		case Instruction::URem:
			result = op1.urem(op2);
			break;
		case Instruction::SRem:
			result = op1.srem(op2);
			break;
		case Instruction::Shl:
			result = op1.shl(op2);
			break;
		case Instruction::LShr:
			result = op1.lshr(op2);
			break;
		case Instruction::AShr:
			result = op1.ashr(op2);
			break;
		case Instruction::And:
			result = op1.And(op2);
			break;
		case Instruction::Or:
			result = op1.Or(op2);
			break;
		case Instruction::Xor:
			result = op1.Xor(op2);
			break;
		default:
			break;
		}
		
		// If resulting interval has become inconsistent, set it to max range for safety
		if (result.getLower().sgt(result.getUpper())) {
			result = Range(Min, Max);
		}

		//FIXME: check if this intersection happens
		bool test = this->getIntersect()->getRange().isMaxRange();

		if (!test) {
			Range aux = this->getIntersect()->getRange();
			result = result.intersectWith(aux);
		}
	} else {
		if (op1.isEmpty() || op2.isEmpty())
			result = Range(Min, Max, Empty);
	}

	return result;
}

/// Pretty print.
void BinaryOp::print(raw_ostream& OS) const {
	const char* quot = "\"";
	const char* opcodeName = Instruction::getOpcodeName(this->getOpcode());
	OS << " " << quot << this << quot << " [label=\"" << opcodeName << "\"]\n";

	const Value* V1 = this->getSource1()->getValue();
	if (const ConstantInt* C = dyn_cast<ConstantInt>(V1)) {
		OS << " " << C->getValue() << " -> " << quot << this << quot << "\n";
	} else {
		OS << " " << quot;
		printVarName(V1, OS);
		OS << quot << " -> " << quot << this << quot << "\n";
	}

	const Value* V2 = this->getSource2()->getValue();
	if (const ConstantInt* C = dyn_cast<ConstantInt>(V2)) {
		OS << " " << C->getValue() << " -> " << quot << this << quot << "\n";
	} else {
		OS << " " << quot;
		printVarName(V2, OS);
		OS << quot << " -> " << quot << this << quot << "\n";
	}

	const Value* VS = this->getSink()->getValue();
	OS << " " << quot << this << quot << " -> " << quot;
	printVarName(VS, OS);
	OS << quot << "\n";
}

// ========================================================================== //
// PhiOp
// ========================================================================== //

// The ctor.
PhiOp::PhiOp(BasicInterval* intersect, VarNode* sink, const Instruction* inst,
		unsigned int opcode) :
		BasicOp(intersect, sink, inst), opcode(opcode) {
}

/// The dtor.
PhiOp::~PhiOp() {
}

// Add source to the vector of sources
void PhiOp::addSource(const VarNode* newsrc) {
	this->sources.push_back(newsrc);
}

/// Computes the interval of the sink based on the interval of the sources.
/// The result of evaluating a phi-function is the union of the ranges of
/// every variable used in the phi.
Range PhiOp::eval() const {

	Range result = this->getSource(0)->getRange();

	// Iterate over the sources of the phiop
	for (SmallVectorImpl<const VarNode*>::const_iterator sit = sources.begin()
			+ 1, send = sources.end(); sit != send; ++sit) {
		result = result.unionWith((*sit)->getRange());
	}

	return result;
}

/// Prints the content of the operation. I didn't it an operator overload
/// because I had problems to access the members of the class outside it.
void PhiOp::print(raw_ostream& OS) const {
	const char* quot = "\"";
	OS << " " << quot << this << quot << " [label=\"";
	OS << "phi";
	OS << "\"]\n";

	for (SmallVectorImpl<const VarNode*>::const_iterator sit = sources.begin(),
			send = sources.end(); sit != send; ++sit) {
		const Value* V = (*sit)->getValue();
		if (const ConstantInt* C = dyn_cast<ConstantInt>(V)) {
			OS << " " << C->getValue() << " -> " << quot << this << quot
					<< "\n";
		} else {
			OS << " " << quot;
			printVarName(V, OS);
			OS << quot << " -> " << quot << this << quot << "\n";
		}
	}

	const Value* VS = this->getSink()->getValue();
	OS << " " << quot << this << quot << " -> " << quot;
	printVarName(VS, OS);
	OS << quot << "\n";
}

// ========================================================================== //
// ValueBranchMap
// ========================================================================== //

ValueBranchMap::ValueBranchMap(const Value* V, const BasicBlock* BBTrue,
		const BasicBlock* BBFalse, BasicInterval* ItvT, BasicInterval* ItvF) :
		V(V), BBTrue(BBTrue), BBFalse(BBFalse), ItvT(ItvT), ItvF(ItvF) {
}

ValueBranchMap::~ValueBranchMap() {
}

void ValueBranchMap::clear() {
//	if (ItvT) {
//		delete ItvT;
//		ItvT = NULL;
//	}
//
//	if (ItvF) {
//		delete ItvF;
//		ItvF = NULL;
//	}
}

// ========================================================================== //
// ValueSwitchMap
// ========================================================================== //

ValueSwitchMap::ValueSwitchMap(const Value* V,
		SmallVector<std::pair<BasicInterval*, const BasicBlock*>, 4> &BBsuccs) :
		V(V), BBsuccs(BBsuccs) {
}

ValueSwitchMap::~ValueSwitchMap() {
}

void ValueSwitchMap::clear() {
//	for (unsigned i = 0, e = BBsuccs.size(); i < e; ++i) {
//		if (BBsuccs[i].first) {
//			delete BBsuccs[i].first;
//			BBsuccs[i].first = NULL;
//		}
//	}
}

// ========================================================================== //
// ConstraintGraph
// ========================================================================== //

ConstraintGraph::ConstraintGraph() {
	this->func = NULL;
}

/// The dtor.
ConstraintGraph::~ConstraintGraph() {
//	errs() << "\nConstraintGraph::~ConstraintGraph : "<< this->vars.size();
	//delete symbMap;

	for (VarNodes::iterator vit = vars.begin(), vend = vars.end();
			vit != vend; ++vit) {
		delete vit->second;
	}

	for (GenOprs::iterator oit = oprs.begin(), oend = oprs.end(); oit != oend;
			++oit) {
		delete *oit;
	}

	for (ValuesBranchMap::iterator vit = valuesBranchMap.begin(), vend =
			valuesBranchMap.end(); vit != vend; ++vit) {
		vit->second.clear();
	}

	for (ValuesSwitchMap::iterator vit = valuesSwitchMap.begin(), vend =
			valuesSwitchMap.end(); vit != vend; ++vit) {
		vit->second.clear();
	}
}

Range ConstraintGraph::getRange(const Value *v) {
//	errs() << "\nInconsistency " << this->vars.size();
//	VarNodes::iterator vit = this->vars.find(v);
//	if (vit == this->vars.end())
		return Range(Min, Max, Unknown);
//	return vit->second->getRange();
}

/// Adds a VarNode to the graph.
VarNode* ConstraintGraph::addVarNode(const Value* V) {
	VarNodes::iterator vit = this->vars.find(V);
	
	if (vit != this->vars.end()) {
		return vit->second;
	}

	VarNode* node = new VarNode(V);
	this->vars.insert(std::make_pair(V, node));

	// Inserts the node in the use map list.
	SmallPtrSet<BasicOp*, 8> useList;
	this->useMap.insert(std::make_pair(V, useList));
	return node;
}

/// Adds an UnaryOp in the graph.
void ConstraintGraph::addUnaryOp(const Instruction* I) {
	// Create the sink.
	VarNode* sink = addVarNode(I);
	// Create the source.
	VarNode* source = NULL;

	switch (I->getOpcode()) {
	case Instruction::Store:
		source = addVarNode(I->getOperand(1));
		break;
	case Instruction::Load:
	case Instruction::Trunc:
	case Instruction::ZExt:
	case Instruction::SExt:
		source = addVarNode(I->getOperand(0));
		break;
	default:
		return;
	}

	// Create the operation using the intersect to constrain sink's interval.
	UnaryOp* UOp = new UnaryOp(new BasicInterval(), sink, I, source,
			I->getOpcode());
	this->oprs.insert(UOp);

	// Insert this definition in defmap
	this->defMap[sink->getValue()] = UOp;

	// Inserts the sources of the operation in the use map list.
	this->useMap.find(source->getValue())->second.insert(UOp);
}

/// XXX: I'm assuming that we are always analyzing bytecodes in e-SSA form.
/// So, we don't have intersections associated with binary oprs. 
/// To have an intersect, we must have a Sigma instruction.
/// Adds a BinaryOp in the graph.
void ConstraintGraph::addBinaryOp(const Instruction* I) {
	// Create the sink.
	VarNode* sink = addVarNode(I);

	// Create the sources.
	VarNode *source1 = addVarNode(I->getOperand(0));
	VarNode *source2 = addVarNode(I->getOperand(1));

	// Create the operation using the intersect to constrain sink's interval.
	BasicInterval* BI = new BasicInterval();
	BinaryOp* BOp = new BinaryOp(BI, sink, I, source1, source2, I->getOpcode());

	// Insert the operation in the graph.
	this->oprs.insert(BOp);

	// Insert this definition in defmap
	this->defMap[sink->getValue()] = BOp;

	// Inserts the sources of the operation in the use map list.
	this->useMap.find(source1->getValue())->second.insert(BOp);
	this->useMap.find(source2->getValue())->second.insert(BOp);
}

/// Add a phi node (actual phi, does not include sigmas)
void ConstraintGraph::addPhiOp(const PHINode* Phi) {
	// Create the sink.
	VarNode* sink = addVarNode(Phi);
	PhiOp* phiOp = new PhiOp(new BasicInterval(), sink, Phi, Phi->getOpcode());

	// Insert the operation in the graph.
	this->oprs.insert(phiOp);

	// Insert this definition in defmap
	this->defMap[sink->getValue()] = phiOp;

	// Create the sources.
	for (User::const_op_iterator it = Phi->op_begin(), e = Phi->op_end();
			it != e; ++it) {
		VarNode* source = addVarNode(*it);
		phiOp->addSource(source);

		// Inserts the sources of the operation in the use map list.
		this->useMap.find(source->getValue())->second.insert(phiOp);
	}
}

void ConstraintGraph::addSigmaOp(const PHINode* Sigma) {
	// Create the sink.
	VarNode* sink = addVarNode(Sigma);
	BasicInterval* BItv = NULL;
	SigmaOp* sigmaOp = NULL;

	const BasicBlock *thisbb = Sigma->getParent();

	// Create the sources (FIXME: sigma has only 1 source. This 'for' may not be needed)
	for (User::const_op_iterator it = Sigma->op_begin(), e = Sigma->op_end();
			it != e; ++it) {
		Value *operand = *it;
		VarNode* source = addVarNode(operand);

		// Create the operation (two cases from: branch or switch)
		ValuesBranchMap::iterator vbmit = this->valuesBranchMap.find(operand);

		// Branch case
		if (vbmit != this->valuesBranchMap.end()) {
			const ValueBranchMap &VBM = vbmit->second;
			if (thisbb == VBM.getBBTrue()) {
				BItv = VBM.getItvT();
			} else {
				if (thisbb == VBM.getBBFalse()) {
					BItv = VBM.getItvF();
				}
			}
		} else {
			// Switch case
			ValuesSwitchMap::iterator vsmit = this->valuesSwitchMap.find(
					operand);

			if (vsmit == this->valuesSwitchMap.end()) {
				continue;
			}

			const ValueSwitchMap &VSM = vsmit->second;

			// Find out which case are we dealing with
			for (unsigned idx = 0, e = VSM.getNumOfCases(); idx < e; ++idx) {
				const BasicBlock *bb = VSM.getBB(idx);

				if (bb == thisbb) {
					BItv = VSM.getItv(idx);
					break;
				}
			}
		}

		if (BItv == NULL) {
			sigmaOp = new SigmaOp(new BasicInterval(), sink, Sigma, source,
					Sigma->getOpcode());
		} else {
			sigmaOp = new SigmaOp(BItv, sink, Sigma, source,
					Sigma->getOpcode());
		}

		// Insert the operation in the graph.
		this->oprs.insert(sigmaOp);

		// Insert this definition in defmap
		this->defMap[sink->getValue()] = sigmaOp;

		// Inserts the sources of the operation in the use map list.
		this->useMap.find(source->getValue())->second.insert(sigmaOp);
	}
}

void ConstraintGraph::buildOperations(const Instruction* I) {

	// Handle binary instructions.
	if (I->isBinaryOp()) {
		addBinaryOp(I);
	} else {
		// Handle Phi functions.
		if (const PHINode* Phi = dyn_cast<PHINode>(I)) {
			if (Phi->getName().startswith(sigmaString)) {
				addSigmaOp(Phi);
			} else {
				addPhiOp(Phi);
			}
		} else {
			// We have an unary instruction to handle.
			addUnaryOp(I);
		}
	}
}

void ConstraintGraph::buildValueSwitchMap(const SwitchInst *sw) {
	const Value *condition = sw->getCondition();

	// Verify conditions
	const Type* opType = sw->getCondition()->getType();
	if (!opType->isIntegerTy()) {
		return;
	}

	// Create VarNode for switch condition explicitly (need to do this when inlining is used!)
	addVarNode(condition);

	SmallVector<std::pair<BasicInterval*, const BasicBlock*>, 4> BBsuccs;

	// Treat when condition of switch is a cast of the real condition (same thing as in buildValueBranchMap)
	const CastInst *castinst = NULL;
	const Value *Op0_0 = NULL;
	if ((castinst = dyn_cast<CastInst>(condition))) {
		Op0_0 = castinst->getOperand(0);
	}

	// Handle 'default', if there is any
	BasicBlock *succ = sw->getDefaultDest();

	if (succ) {
		APInt sigMin = Min;
		APInt sigMax = Max;

		Range Values = Range(sigMin, sigMax);

		// Create the interval using the intersection in the branch.
		BasicInterval* BI = new BasicInterval(Values);

		BBsuccs.push_back(std::make_pair(BI, succ));
	}

	// Handle the rest of cases
	for (unsigned i = 1, e = sw->getNumCases(); i < e; ++i) {
		BasicBlock *succ = sw->getSuccessor(i);
		const ConstantInt *constant = sw->getCaseValue(i);

		APInt sigMin = constant->getValue();
		APInt sigMax = sigMin;

		if (sigMin.getBitWidth() < MAX_BIT_INT) {
			sigMin = sigMin.sext(MAX_BIT_INT);
		}
		if (sigMax.getBitWidth() < MAX_BIT_INT) {
			sigMax = sigMax.sext(MAX_BIT_INT);
		}

//		if (sigMax.slt(sigMin)) {
//			sigMax = APInt::getSignedMaxValue(MAX_BIT_INT);
//		}

		Range Values = Range(sigMin, sigMax);

		// Create the interval using the intersection in the branch.
		BasicInterval* BI = new BasicInterval(Values);

		BBsuccs.push_back(std::make_pair(BI, succ));
	}

	ValueSwitchMap VSM(condition, BBsuccs);
	valuesSwitchMap.insert(std::make_pair(condition, VSM));

	if (Op0_0) {
		ValueSwitchMap VSM_0(Op0_0, BBsuccs);
		valuesSwitchMap.insert(std::make_pair(Op0_0, VSM_0));
	}
}

void ConstraintGraph::buildValueBranchMap(const BranchInst *br) {
	// Verify conditions
	if (!br->isConditional())
		return;

	ICmpInst *ici = dyn_cast<ICmpInst>(br->getCondition());
	if (!ici)
		return;

	const Type* op0Type = ici->getOperand(0)->getType();
	const Type* op1Type = ici->getOperand(1)->getType();
	if (!op0Type->isIntegerTy() || !op1Type->isIntegerTy()) {
		return;
	}

	// Create VarNodes for comparison operands explicitly (need to do this when inlining is used!)
	addVarNode(ici->getOperand(0));
	addVarNode(ici->getOperand(1));

	// Gets the successors of the current basic block.
	const BasicBlock *TBlock = br->getSuccessor(0);
	const BasicBlock *FBlock = br->getSuccessor(1);

	// We have a Variable-Constant comparison.
	if (ConstantInt * CI = dyn_cast<ConstantInt>(ici->getOperand(1))) {
		// Calculate the range of values that would satisfy the comparison.
		ConstantRange CR(CI->getValue(), CI->getValue() + 1);
		unsigned int pred = ici->getPredicate();

		ConstantRange tmpT = ConstantRange::makeICmpRegion(pred, CR);
		APInt sigMin = tmpT.getSignedMin();
		APInt sigMax = tmpT.getSignedMax();

		if (sigMin.getBitWidth() < MAX_BIT_INT) {
			sigMin = sigMin.sext(MAX_BIT_INT);
		}
		if (sigMax.getBitWidth() < MAX_BIT_INT) {
			sigMax = sigMax.sext(MAX_BIT_INT);
		}

		if (sigMax.slt(sigMin)) {
			sigMax = Max;
		}

		Range TValues = Range(sigMin, sigMax);

		// If we're interested in the false dest, invert the condition.
		ConstantRange tmpF = tmpT.inverse();
		sigMin = tmpF.getSignedMin();
		sigMax = tmpF.getSignedMax();

		if (sigMin.getBitWidth() < MAX_BIT_INT) {
			sigMin = sigMin.sext(MAX_BIT_INT);
		}
		if (sigMax.getBitWidth() < MAX_BIT_INT) {
			sigMax = sigMax.sext(MAX_BIT_INT);
		}

		if (sigMax.slt(sigMin)) {
			sigMax = Max;
		}

		Range FValues = Range(sigMin, sigMax);

		// Create the interval using the intersection in the branch.
		BasicInterval* BT = new BasicInterval(TValues);
		BasicInterval* BF = new BasicInterval(FValues);

		const Value *Op0 = ici->getOperand(0);
		ValueBranchMap VBM(Op0, TBlock, FBlock, BT, BF);
		valuesBranchMap.insert(std::make_pair(Op0, VBM));

		// Do the same for the operand of Op0 (if Op0 is a cast instruction)
		const CastInst *castinst = NULL;
		if ((castinst = dyn_cast<CastInst>(Op0))) {
			const Value *Op0_0 = castinst->getOperand(0);

			BasicInterval* BT = new BasicInterval(TValues);
			BasicInterval* BF = new BasicInterval(FValues);

			ValueBranchMap VBM(Op0_0, TBlock, FBlock, BT, BF);
			valuesBranchMap.insert(std::make_pair(Op0_0, VBM));
		}
	} else {
		// Create the interval using the intersection in the branch.
		CmpInst::Predicate pred = ici->getPredicate();
		CmpInst::Predicate invPred = ici->getInversePredicate();

		Range CR(Min, Max, Unknown);

		const Value* Op1 = ici->getOperand(1);

		// Symbolic intervals for op0
		const Value* Op0 = ici->getOperand(0);
		SymbInterval* STOp0 = new SymbInterval(CR, Op1, pred);
		SymbInterval* SFOp0 = new SymbInterval(CR, Op1, invPred);

		ValueBranchMap VBMOp0(Op0, TBlock, FBlock, STOp0, SFOp0);
		valuesBranchMap.insert(std::make_pair(Op0, VBMOp0));

		// Symbolic intervals for operand of op0 (if op0 is a cast instruction)
		const CastInst *castinst = NULL;
		if ((castinst = dyn_cast<CastInst>(Op0))) {
			const Value* Op0_0 = castinst->getOperand(0);

			SymbInterval* STOp1_1 = new SymbInterval(CR, Op1, pred);
			SymbInterval* SFOp1_1 = new SymbInterval(CR, Op1, invPred);

			ValueBranchMap VBMOp1_1(Op0_0, TBlock, FBlock, STOp1_1, SFOp1_1);
			valuesBranchMap.insert(std::make_pair(Op0_0, VBMOp1_1));
		}

		// Symbolic intervals for op1
		SymbInterval* STOp1 = new SymbInterval(CR, Op0, invPred);
		SymbInterval* SFOp1 = new SymbInterval(CR, Op0, pred);
		ValueBranchMap VBMOp1(Op1, TBlock, FBlock, STOp1, SFOp1);
		valuesBranchMap.insert(std::make_pair(Op1, VBMOp1));

		// Symbolic intervals for operand of op1 (if op1 is a cast instruction)
		castinst = NULL;
		if ((castinst = dyn_cast<CastInst>(Op1))) {
			const Value* Op0_0 = castinst->getOperand(0);

			SymbInterval* STOp1_1 = new SymbInterval(CR, Op1, pred);
			SymbInterval* SFOp1_1 = new SymbInterval(CR, Op1, invPred);

			ValueBranchMap VBMOp1_1(Op0_0, TBlock, FBlock, STOp1_1, SFOp1_1);
			valuesBranchMap.insert(std::make_pair(Op0_0, VBMOp1_1));
		}
	}
}

void ConstraintGraph::buildValueMaps(const Function& F) {
	for (Function::const_iterator iBB = F.begin(), eBB = F.end(); iBB != eBB;
			++iBB) {
		const TerminatorInst* ti = iBB->getTerminator();
		const BranchInst* br = dyn_cast<BranchInst>(ti);
		const SwitchInst* sw = dyn_cast<SwitchInst>(ti);

		if (br) {
			buildValueBranchMap(br);
		} else if (sw) {
			buildValueSwitchMap(sw);
		}
	}
}

/// Iterates through all instructions in the function and builds the graph.
void ConstraintGraph::buildGraph(const Function& F) {
	this->func = &F;
	buildValueMaps(F);

//	for (Function::const_arg_iterator ait = F.arg_begin(), aend = F.arg_end(); ait != aend; ++ait) {
//		const Value *argument = &*ait;
//		const Type* ty = argument->getType();
//
//		if (!ty->isIntegerTy()) {
//			continue;
//		}
//
//		addVarNode(argument);
//	}

	for (const_inst_iterator I = inst_begin(F), E = inst_end(F); I != E; ++I) {
		const Type* ty = (&*I)->getType();
		if (!(ty->isIntegerTy() || ty->isPointerTy() || ty->isVoidTy())) {
			continue;
		}

		if (!isValidInstruction(&*I)) {
			continue;
		}

		buildOperations(&*I);
	}
}

void ConstraintGraph::buildVarNodes() {
	// Initializes the nodes and the use map structure.
	VarNodes::iterator bgn = this->vars.begin(), end = this->vars.end();
	for (; bgn != end; ++bgn) {
		bgn->second->init(!this->defMap.count(bgn->first));
	}
}

//FIXME: do it just for component
void CropDFS::storeAbstractStates(const SmallPtrSet<VarNode*, 32> *component) {
	for (SmallPtrSetIterator<VarNode*> cit = component->begin(), cend =
			component->end(); cit != cend; ++cit) {
		(*cit)->storeAbstractState();
	}
}

bool Meet::fixed(BasicOp* op){
	Range oldInterval = op->getSink()->getRange();
	Range newInterval = op->eval();
	
	op->getSink()->setRange(newInterval);
	
	return oldInterval != newInterval;
}

/// This is the meet operator of the growth analysis. The growth analysis
/// will change the bounds of each variable, if necessary. Initially, each
/// variable is bound to either the undefined interval, e.g. [., .], or to
/// a constant interval, e.g., [3, 15]. After this analysis runs, there will
/// be no undefined interval. Each variable will be either bound to a
/// constant interval, or to [-, c], or to [c, +], or to [-, +].
bool Meet::widen(BasicOp* op) {
	Range oldInterval = op->getSink()->getRange();
	Range newInterval = op->eval();

	APInt oldLower = oldInterval.getLower();
	APInt oldUpper = oldInterval.getUpper();
	APInt newLower = newInterval.getLower();
	APInt newUpper = newInterval.getUpper();

	if (oldInterval.isUnknown()) {
		op->getSink()->setRange(newInterval);
	} else {
		if (newLower.slt(oldLower) && newUpper.sgt(oldUpper)) {
			op->getSink()->setRange(Range(Min, Max));
		} else {
			if (newLower.slt(oldLower)) {
				op->getSink()->setRange(Range(Min, oldUpper));
			} else {
				if (newUpper.sgt(oldUpper)) {
					op->getSink()->setRange(Range(oldLower, Max));
				}
			}
		}
	}

	Range sinkInterval = op->getSink()->getRange();
	return oldInterval != sinkInterval;
}

bool Meet::growth(BasicOp* op) {
	Range oldInterval = op->getSink()->getRange();
	Range newInterval = op->eval();

	if (oldInterval.isUnknown())
		op->getSink()->setRange(newInterval);
	else {
		APInt oldLower = oldInterval.getLower();
		APInt oldUpper = oldInterval.getUpper();
		APInt newLower = newInterval.getLower();
		APInt newUpper = newInterval.getUpper();
		if (newLower.slt(oldLower))
			if (newUpper.sgt(oldUpper))
				op->getSink()->setRange(Range());
			else
				op->getSink()->setRange(Range(Min, oldUpper));
		else if (newUpper.sgt(oldUpper))
			op->getSink()->setRange(Range(oldLower, Max));
	}
	Range sinkInterval = op->getSink()->getRange();
	return oldInterval != sinkInterval;
}

/// This is the meet operator of the cropping analysis. Whereas the growth
/// analysis expands the bounds of each variable, regardless of intersections
/// in the constraint graph, the cropping analysis shyrinks these bounds back
/// to ranges that respect the intersections.
bool Meet::narrow(BasicOp* op) {
	APInt oLower = op->getSink()->getRange().getLower();
	APInt oUpper = op->getSink()->getRange().getUpper();
	//errs() << "old> " << op->getSink()->getValue()->getName() << " [" << oLower << ", " << oUpper << "]\n";
	Range newInterval = op->eval();

	APInt nLower = newInterval.getLower();
	APInt nUpper = newInterval.getUpper();
	
	//errs() << "new> " << op->getSink()->getValue()->getName() << " [" << oLower << ", " << oUpper << "]\n";
	
	bool hasChanged = false;

	if (oLower.eq(Min) && nLower.ne(Min)) {
		op->getSink()->setRange(Range(nLower, oUpper));
		hasChanged = true;
	} else {
		APInt smin = APIntOps::smin(oLower, nLower);
		if (oLower.ne(smin)) {
			op->getSink()->setRange(Range(smin, oUpper));
			hasChanged = true;
		}
	}

	if (oUpper.eq(Max) && nUpper.ne(Max)) {
		op->getSink()->setRange(Range(op->getSink()->getRange().getLower(), nUpper));
		hasChanged = true;
	} else {
		APInt smax = APIntOps::smax(oUpper, nUpper);
		if (oUpper.ne(smax)) {
			op->getSink()->setRange(Range(op->getSink()->getRange().getLower(), smax));
			hasChanged = true;
		}
	}
	
	//errs() << "final> " << op->getSink()->getValue()->getName() << " [" << op->getSink()->getRange().getLower() << ", " << op->getSink()->getRange().getUpper() << "]\n";

	return hasChanged;
}

bool Meet::crop(BasicOp* op) {
	Range oldInterval = op->getSink()->getRange();
	Range newInterval = op->eval();

	bool hasChanged = false;
	char abstractState = op->getSink()->getAbstractState();

	if ((abstractState == '-' || abstractState == '?')
			&& newInterval.getLower().sgt(oldInterval.getLower())) {
		op->getSink()->setRange(
				Range(newInterval.getLower(), oldInterval.getUpper()));
		hasChanged = true;
	}

	if ((abstractState == '+' || abstractState == '?')
			&& newInterval.getUpper().slt(oldInterval.getUpper())) {
		op->getSink()->setRange(
				Range(op->getSink()->getRange().getLower(),
						newInterval.getUpper()));
		hasChanged = true;
	}

	return hasChanged;
}

void Cousot::preUpdate(const UseMap &compUseMap,
		SmallPtrSet<const Value*, 6>& entryPoints) {
	update(compUseMap, entryPoints, Meet::widen);
}

void Cousot::posUpdate(const UseMap &compUseMap,
		SmallPtrSet<const Value*, 6>& entryPoints,
		const SmallPtrSet<VarNode*, 32> *component) {
	update(compUseMap, entryPoints, Meet::narrow);
}

void CropDFS::preUpdate(const UseMap &compUseMap,
		SmallPtrSet<const Value*, 6>& entryPoints) {
	update(compUseMap, entryPoints, Meet::growth);
}

void CropDFS::posUpdate(const UseMap &compUseMap,
		SmallPtrSet<const Value*, 6>& entryPoints,
		const SmallPtrSet<VarNode*, 32> *component) {
	storeAbstractStates(component);
	GenOprs::iterator obgn = oprs.begin(), oend = oprs.end();
	for (; obgn != oend; ++obgn) {
		BasicOp *op = *obgn;

		if (component->count(op->getSink()))
			//int_op
			if (isa<UnaryOp>(op)
					&& (op->getSink()->getRange().getLower().ne(Min)
							|| op->getSink()->getRange().getUpper().ne(Max)))
				crop(compUseMap, op);
	}
}

void CropDFS::crop(const UseMap &compUseMap, BasicOp *op) {
	SmallPtrSet<BasicOp*, 8> activeOps;
	SmallPtrSet<const VarNode*, 8> visitedOps;

	//init the activeOps only with the op received
	activeOps.insert(op);

	while (!activeOps.empty()) {
		BasicOp* V = *activeOps.begin();
		activeOps.erase(V);
		const VarNode* sink = V->getSink();

		//if the sink has been visited go to the next activeOps
		if (visitedOps.count(sink))
			continue;

		Meet::crop(V);
		visitedOps.insert(sink);

		// The use list.of sink
		const SmallPtrSet<BasicOp*, 8> &L =
				compUseMap.find(sink->getValue())->second;
		SmallPtrSetIterator<BasicOp*> bgn = L.begin(), end = L.end();

		for (; bgn != end; ++bgn) {
			activeOps.insert(*bgn);
		}
	}
}

void ConstraintGraph::update(const UseMap &compUseMap,
		SmallPtrSet<const Value*, 6>& actv, bool(*meet)(BasicOp* op)) {
	while (!actv.empty()) {
		const Value* V = *actv.begin();
		actv.erase(V);
		
		//errs() << "Update: " << V->getName() << "\n";
		// The use list.
		const SmallPtrSet<BasicOp*, 8> &L = compUseMap.find(V)->second;
		SmallPtrSetIterator<BasicOp*> bgn = L.begin(), end = L.end();

		for (; bgn != end; ++bgn) {
			if (meet(*bgn)) {
				// I want to use it as a set, but I also want
				// keep an order or insertions and removals.
				actv.insert((*bgn)->getSink()->getValue());
			}
		}
	}
}

void ConstraintGraph::update(unsigned nIterations, const UseMap &compUseMap,
		SmallPtrSet<const Value*, 6>& actv){
	while (!actv.empty()) {
		const Value* V = *actv.begin();
		actv.erase(V);
		// The use list.
		const SmallPtrSet<BasicOp*, 8> &L = compUseMap.find(V)->second;
		SmallPtrSetIterator<BasicOp*> bgn = L.begin(), end = L.end();
		for (; bgn != end; ++bgn) {
			if(nIterations == 0) {actv.clear(); return;}
			else --nIterations;

			if(Meet::fixed(*bgn))
				actv.insert((*bgn)->getSink()->getValue());
		}
	}
}

/// Finds the intervals of the variables in the graph.
void ConstraintGraph::findIntervals() {
	// Builds symbMap
	Profile::TimeValue before = prof.timenow();
	buildSymbolicIntersectMap();

	// List of SCCs
	Nuutila sccList(&vars, &useMap, &symbMap);
	Profile::TimeValue after = prof.timenow();
	Profile::TimeValue elapsed = after - before;
	prof.updateTime("Nuutila", elapsed);

	// STATS
	numSCCs += sccList.worklist.size();
#ifdef SCC_DEBUG
	unsigned numberOfSCCs = numSCCs;
#endif

	// For each SCC in graph, do the following
	before = prof.timenow();
	
	for (Nuutila::iterator nit = sccList.begin(), nend = sccList.end();
			nit != nend; ++nit) {
		SmallPtrSet<VarNode*, 32> &component = sccList.components[*nit];
#ifdef SCC_DEBUG
		--numberOfSCCs;
#endif
		// STATS
		if (component.size() == 1) {
			++numAloneSCCs;
			fixIntersects(component);
			
			VarNode *var = *component.begin();
			if (var->getRange().isUnknown()) {
				var->setRange(Range(Min, Max));
			}
		}else{
			if (component.size() > sizeMaxSCC) {
				sizeMaxSCC = component.size();
			}

			//PRINTCOMPONENT(component)

			UseMap compUseMap = buildUseMap(component);

			// Get the entry points of the SCC
			SmallPtrSet<const Value*, 6> entryPoints;

			generateEntryPoints(component, entryPoints);
			//iterate a fixed number of time before widening
			update(component.size()*2 | NUMBER_FIXED_ITERATIONS, compUseMap, entryPoints);

#ifdef PRINT_DEBUG
			if (func)
				printToFile(*func, "/tmp/" + func->getName() + "cgfixed.dot");
#endif
			
			// Primeiro iterate till fix point
			generateEntryPoints(component, entryPoints);
			// Primeiro iterate till fix point
			preUpdate(compUseMap, entryPoints);
			fixIntersects(component);
			
			// FIXME: Ensure that this code is not needed
			for (SmallPtrSetIterator<VarNode*> cit = component.begin(), cend = component.end(); cit != cend; ++cit) {
				VarNode* var = *cit;
				
				if (var->getRange().isUnknown()) {
					var->setRange(Range(Min, Max));
				}
			}

			//printResultIntervals();
#ifdef PRINT_DEBUG
			if (func)
				printToFile(*func, "/tmp/" + func->getName() + "cgint.dot");
#endif

			// Segundo iterate till fix point
			SmallPtrSet<const Value*, 6> activeVars;
			generateActivesVars(component, activeVars);
			posUpdate(compUseMap, activeVars, &component);
		}
		propagateToNextSCC(component);
	}
	
	elapsed = prof.timenow() - before;
	prof.updateTime("SCCs resolution", elapsed);
	
#ifdef SCC_DEBUG
	ASSERT(numberOfSCCs==0, "Not all SCCs have been visited")
#endif

#ifdef STATS
	before = prof.timenow();
	computeStats();
	elapsed = prof.timenow() - before;
	prof.updateTime("ComputeStats", elapsed);
#endif
}

void ConstraintGraph::generateEntryPoints(SmallPtrSet<VarNode*, 32> &component
		, SmallPtrSet<const Value*, 6> &entryPoints) {
	if (!entryPoints.empty()) {
		errs() << "Set não vazio\n";
	}

	// Iterate over the varnodes in the component
	for (SmallPtrSetIterator<VarNode*> cit = component.begin(), cend =
			component.end(); cit != cend; ++cit) {
		VarNode *var = *cit;
		const Value *V = var->getValue();

		// FIXME: trocar strcmp por enumeraçao de varnodes
		if (V->getName().startswith(sigmaString)) {
			DefMap::iterator dit = this->defMap.find(V);

			if (dit != this->defMap.end()) {
				BasicOp *bop = dit->second;
				SigmaOp *defop = dyn_cast<SigmaOp>(bop);

				if (defop && defop->isUnresolved()) {
					//printVarName(defop->getSink()->getValue(), errs());

					//errs() << defop->getSink()->getRange();
					defop->getSink()->setRange(bop->eval());
					//errs() << defop->getSink()->getRange();
					defop->markResolved();
				}
			}
		}

		// TODO: Verificar a condição para ser um entry point
		if (!var->getRange().isUnknown()) {
			entryPoints.insert(V);
		}
	}
}

void ConstraintGraph::fixIntersects(SmallPtrSet<VarNode*, 32> &component) {
	// Iterate again over the varnodes in the component
	for (SmallPtrSetIterator<VarNode*> cit = component.begin(), cend =
			component.end(); cit != cend; ++cit) {
		VarNode *var = *cit;
		const Value *V = var->getValue();

		SymbMap::iterator sit = symbMap.find(V);

		if (sit != symbMap.end()) {
			for (SmallPtrSetIterator<BasicOp*> opit = sit->second.begin(),
					opend = sit->second.end(); opit != opend; ++opit) {
				BasicOp *op = *opit;

				op->fixIntersects(var);
			}
		}
	}
}

void ConstraintGraph::generateActivesVars(SmallPtrSet<VarNode*, 32> &component
		, SmallPtrSet<const Value*, 6> &activeVars) {
	if (!activeVars.empty()) {
		errs() << "Set não vazio\n";
	}

	for (SmallPtrSetIterator<VarNode*> cit = component.begin(), cend =
			component.end(); cit != cend; ++cit) {
		VarNode *var = *cit;
		const Value *V = var->getValue();

		const ConstantInt* CI = dyn_cast<ConstantInt>(V);
		if (CI) {
			continue;
		}

		activeVars.insert(V);
	}
}

// TODO: To implement it.
/// Releases the memory used by the graph.
void ConstraintGraph::clear() {

}

/// Prints the content of the graph in dot format. For more informations
/// about the dot format, see: http://www.graphviz.org/pdf/dotguide.pdf
void ConstraintGraph::print(const Function& F, raw_ostream& OS) const {
	const char* quot = "\"";
	// Print the header of the .dot file.
	OS << "digraph dotgraph {\n";
	OS << "label=\"Constraint Graph for \'";
	OS << F.getNameStr() << "\' function\";\n";
	OS << "node [shape=record,fontname=\"Times-Roman\",fontsize=14];\n";

	// Print the body of the .dot file.
	VarNodes::const_iterator bgn, end;
	for (bgn = vars.begin(), end = vars.end(); bgn != end; ++bgn) {
		if (const ConstantInt* C = dyn_cast<ConstantInt>(bgn->first)) {
			OS << " " << C->getValue();
		} else {
			OS << quot;
			printVarName(bgn->first, OS);
			OS << quot;
		}

		OS << " [label=\"" << bgn->second << "\"]\n";
	}

	GenOprs::const_iterator B = oprs.begin(), E = oprs.end();
	for (; B != E; ++B) {
		(*B)->print(OS);
		OS << "\n";
	}

	OS << pseudoEdgesString.str();

	// Print the footer of the .dot file.
	OS << "}\n";
}

void ConstraintGraph::printToFile(const Function& F, Twine FileName) {
	std::string ErrorInfo;
	raw_fd_ostream file(FileName.str().c_str(), ErrorInfo);
	print(F, file);
	file.close();
}

void ConstraintGraph::printResultIntervals() {
	for (VarNodes::iterator vbgn = vars.begin(), vend = vars.end();
			vbgn != vend; ++vbgn) {
		if (const ConstantInt* C = dyn_cast<ConstantInt>(vbgn->first)) {
			errs() << C->getValue() << " ";
		} else {
			printVarName(vbgn->first, errs());
		}

		vbgn->second->getRange().print(errs());
		errs() << "\n";
	}

	errs() << "\n";
}

void ConstraintGraph::computeStats() {
	for (VarNodes::const_iterator vbgn = vars.begin(), vend = vars.end();
			vbgn != vend; ++vbgn) {
		// We only count the instructions that have uses.
		if (vbgn->first->getNumUses() == 0) {
			++numZeroUses;
			//continue;
		}

		// ConstantInts must NOT be counted!!
		if (isa<ConstantInt>(vbgn->first)) {
			++numConstants;
			continue;
		}
		
		// Variables that are not IntegerTy are ignored
		if (!vbgn->first->getType()->isIntegerTy()) {
			++numNotInt;
			continue;
		}


		// Count original (used) bits
		unsigned total = vbgn->first->getType()->getPrimitiveSizeInBits();
		usedBits += total;
		Range CR = vbgn->second->getRange();

		// If range is unknown, we have total needed bits
		if (CR.isUnknown()) {
			++numUnknown;
			needBits += total;
			continue;
		}

		// If range is empty, we have 0 needed bits
		if (CR.isEmpty()) {
			++numEmpty;
			continue;
		}
		
		if (CR.getLower().eq(Min)) {
			if (CR.getUpper().eq(Max)) {
				++numMaxRange;
			}
			else {
				++numMinInfC;
			}
		}
		else if (CR.getUpper().eq(Max)) {
			++numCPlusInf;
		}
		else {
			++numCC;
		}

		unsigned ub, lb;

		if (CR.getLower().isNegative()) {
			APInt abs = CR.getLower().abs();
			lb = abs.getActiveBits() + 1;
		} else {
			lb = CR.getLower().getActiveBits() + 1;
		}

		if (CR.getUpper().isNegative()) {
			APInt abs = CR.getUpper().abs();
			ub = abs.getActiveBits() + 1;
		} else {
			ub = CR.getUpper().getActiveBits() + 1;
		}

		unsigned nBits = lb > ub ? lb : ub;
		
		// If both bounds are positive, decrement needed bits by 1
		if (!CR.getLower().isNegative() && !CR.getUpper().isNegative()) {
			--nBits;
		}

		if (nBits < total) {
			needBits += nBits;
		} else {
			needBits += total;
		}
//		errs() << "\nVar [" << vbgn->first->getNameStr() <<"] Range"<< CR <<" Total ["<<total<<"] Needed ["<<nBits <<"]";
	}

	double totalB = usedBits;
	double needB = needBits;
	double reduction = (double) (totalB - needB) * 100 / totalB;
	percentReduction = (unsigned int) reduction;


	numVars += this->vars.size();
	numOps += this->oprs.size();
}

/*
 *	This method builds a map that binds each variable to the operation in
 *  which this variable is defined.
 */

//DefMap ConstraintGraph::buildDefMap(const SmallPtrSet<VarNode*, 32> &component)
//{
//	std::deque<BasicOp*> list;
//	for (GenOprs::iterator opit = oprs.begin(), opend = oprs.end(); opit != opend; ++opit) {
//		BasicOp *op = *opit;
//
//		if (std::find(component.begin(), component.end(), op->getSink()) != component.end()) {
//			list.push_back(op);
//		}
//	}
//
//	DefMap defMap;
//
//	for (std::deque<BasicOp*>::iterator opit = list.begin(), opend = list.end(); opit != opend; ++opit) {
//		BasicOp *op = *opit;
//		defMap[op->getSink()] = op;
//	}
//
//	return defMap;
//}

/*
 *	This method builds a map that binds each variable label to the operations
 *  where this variable is used.
 */
UseMap ConstraintGraph::buildUseMap(
		const SmallPtrSet<VarNode*, 32> &component) {
	UseMap compUseMap;

	for (SmallPtrSetIterator<VarNode*> vit = component.begin(), vend =
			component.end(); vit != vend; ++vit) {
		const VarNode *var = *vit;
		const Value *V = var->getValue();

		// Get the component's use list for V (it does not exist until we try to get it)
		SmallPtrSet<BasicOp*, 8> &list = compUseMap[V];

		// Get the use list of the variable in component
		UseMap::iterator p = this->useMap.find(V);

		// For each operation in the list, verify if its sink is in the component
		for (SmallPtrSetIterator<BasicOp*> opit = p->second.begin(), opend =
				p->second.end(); opit != opend; ++opit) {
			VarNode *sink = (*opit)->getSink();

			// If it is, add op to the component's use map
			if (component.count(sink)) {
				list.insert(*opit);
			}
		}
	}

	return compUseMap;
}

/*
 *	This method builds a map of variables to the lists of operations where
 *  these variables are used as futures. Its C++ type should be something like
 *  map<VarNode, List<Operation>>.
 */
void ConstraintGraph::buildSymbolicIntersectMap() {
	// Creates the symbolic intervals map
	//FIXME: Why this map is beeing recreated?
	symbMap = SymbMap();

	// Iterate over the operations set
	for (GenOprs::iterator oit = oprs.begin(), oend = oprs.end(); oit != oend;
			++oit) {
		BasicOp *op = *oit;

		// If the operation is unary and its interval is symbolic
		UnaryOp *uop = dyn_cast<UnaryOp>(op);

		if (uop && isa<SymbInterval>(uop->getIntersect())) {
			SymbInterval* symbi = cast<SymbInterval>(uop->getIntersect());

			const Value *V = symbi->getBound();
			SymbMap::iterator p = symbMap.find(V);

			if (p != symbMap.end()) {
				p->second.insert(uop);
			} else {
				SmallPtrSet<BasicOp*, 8> l;

				if (!l.empty()) {
					errs() << "Erro no set\n";
				}

				l.insert(uop);
				symbMap.insert(std::make_pair(V, l));
			}
		}

	}
}

/*
 *	This method evaluates once each operation that uses a variable in
 *  component, so that the next SCCs after component will have entry
 *  points to kick start the range analysis algorithm.
 */
void ConstraintGraph::propagateToNextSCC(
		const SmallPtrSet<VarNode*, 32> &component) {
	for (SmallPtrSetIterator<VarNode*> cit = component.begin(), cend =
			component.end(); cit != cend; ++cit) {
		VarNode *var = *cit;
		const Value *V = var->getValue();

		UseMap::iterator p = this->useMap.find(V);

		for (SmallPtrSetIterator<BasicOp*> sit = p->second.begin(), send =
				p->second.end(); sit != send; ++sit) {
			BasicOp *op = *sit;
			SigmaOp *sigmaop = dyn_cast<SigmaOp>(op);

			op->getSink()->setRange(op->eval());

			if (sigmaop && sigmaop->getIntersect()->getRange().isUnknown()) {
				sigmaop->markUnresolved();
			}
		}
	}
}

/*
 *	Adds the edges that ensure that we solve a future before fixing its
 *  interval. I have created a new class: ControlDep edges, to represent
 *  the control dependences. In this way, in order to delete these edges,
 *  one just need to go over the map of uses removing every instance of the
 *  ControlDep class.
 */
void Nuutila::addControlDependenceEdges(SymbMap *symbMap, UseMap *useMap,
		VarNodes* vars) {
	for (SymbMap::iterator sit = symbMap->begin(), send = symbMap->end();
			sit != send; ++sit) {
		for (SmallPtrSetIterator<BasicOp*> opit = sit->second.begin(), opend =
				sit->second.end(); opit != opend; ++opit) {
			// Cria uma operação pseudo-aresta
			VarNodes::iterator source_value = vars->find(sit->first);
			VarNode* source = source_value->second;

//			if (source_value != vars.end()) {
//				source = vars.find(sit->first)->second;
//			}

//			if (source == NULL) {
//				continue;
//			}

			BasicOp *cdedge = new ControlDep((*opit)->getSink(), source);
//			BasicOp *cdedge = new ControlDep((cast<UnaryOp>(*opit))->getSource(), source);

			//(*useMap)[(*opit)->getSink()->getValue()].insert(cdedge);
			(*useMap)[sit->first].insert(cdedge);
		}
	}
}

/*
 *	Removes the control dependence edges from the constraint graph.
 */
void Nuutila::delControlDependenceEdges(UseMap *useMap) {
	for (UseMap::iterator it = useMap->begin(), end = useMap->end(); it != end;
			++it) {

		std::deque<ControlDep*> ops;

		if (!ops.empty()) {
			errs() << "Erro na lista\n";
		}

		for (SmallPtrSetIterator<BasicOp*> sit = it->second.begin(), send =
				it->second.end(); sit != send; ++sit) {
			ControlDep *op = NULL;

			if ((op = dyn_cast<ControlDep>(*sit))) {
				ops.push_back(op);
			}
		}

		for (std::deque<ControlDep*>::iterator dit = ops.begin(), dend =
				ops.end(); dit != dend; ++dit) {
			ControlDep *op = *dit;

			// Add pseudo edge to the string
			const Value* V = op->getSource()->getValue();
			if (const ConstantInt* C = dyn_cast<ConstantInt>(V)) {
				pseudoEdgesString << " " << C->getValue() << " -> ";
			} else {
				pseudoEdgesString << " " << '"';
				printVarName(V, pseudoEdgesString);
				pseudoEdgesString << '"' << " -> ";
			}

			const Value* VS = op->getSink()->getValue();
			pseudoEdgesString << '"';
			printVarName(VS, pseudoEdgesString);
			pseudoEdgesString << '"';

			pseudoEdgesString << " [style=dashed]\n";

			// Remove pseudo edge from the map
			it->second.erase(op);
		}
	}
}

/*
 *	Finds SCCs using Nuutila's algorithm. This algorithm is divided in
 *  two parts. The first calls the recursive visit procedure on every node
 *  in the constraint graph. The second phase revisits these nodes,
 *  grouping them in components.
 */
void Nuutila::visit(Value *V, std::stack<Value*> &stack, UseMap *useMap) {
	dfs[V] = index;
	++index;
	root[V] = V;

	// Visit every node defined in an instruction that uses V
	for (SmallPtrSetIterator<BasicOp*> sit = (*useMap)[V].begin(), send =
			(*useMap)[V].end(); sit != send; ++sit) {
		BasicOp *op = *sit;
		Value *name = const_cast<Value*>(op->getSink()->getValue());

		if (dfs[name] < 0) {
			visit(name, stack, useMap);
		}

		if ((inComponent.count(name) == false)
				&& (dfs[root[V]] >= dfs[root[name]])) {
			root[V] = root[name];
		}
	}

	// The second phase of the algorithm assigns components to stacked nodes
	if (root[V] == V) {
		// Neither the worklist nor the map of components is part of Nuutila's
		// original algorithm. We are using these data structures to get a
		// topological ordering of the SCCs without having to go over the root
		// list once more.
		worklist.push_back(V);

		SmallPtrSet<VarNode*, 32> SCC;

		if (!SCC.empty()) {
			errs() << "Erro na lista\n";
		}

		SCC.insert((*variables)[V]);

		inComponent.insert(V);

		while (!stack.empty() && (dfs[stack.top()] > dfs[V])) {
			Value *node = stack.top();
			stack.pop();

			inComponent.insert(node);

			SCC.insert((*variables)[node]);
		}

		components[V] = SCC;
	} else {
		stack.push(V);
	}
}

/*
 *	Finds the strongly connected components in the constraint graph formed by
 *	Variables and UseMap. The class receives the map of futures to insert the
 *  control dependence edges in the contraint graph. These edges are removed
 *  after the class is done computing the SCCs.
 */
Nuutila::Nuutila(VarNodes *varNodes, UseMap *useMap, SymbMap *symbMap,
		bool single) {
	if (single) {
		/* FERNANDO */
		SmallPtrSet<VarNode*, 32> SCC;
		for (VarNodes::iterator vit = varNodes->begin(), vend = varNodes->end();
				vit != vend; ++vit) {
			SCC.insert(vit->second);
		}

		for (VarNodes::iterator vit = varNodes->begin(), vend = varNodes->end();
				vit != vend; ++vit) {
			Value *V = const_cast<Value*>(vit->first);
			components[V] = SCC;
		}

		if (!varNodes->empty()) {
			this->worklist.push_back(const_cast<Value*>(varNodes->begin()->first));
		}
	} else {
		// Copy structures
		this->variables = varNodes;
		this->index = 0;

		// Iterate over all varnodes of the constraint graph
		for (VarNodes::iterator vit = varNodes->begin(), vend = varNodes->end();
				vit != vend; ++vit) {
			// Initialize DFS control variable for each Value in the graph
			Value *V = const_cast<Value*>(vit->first);
			dfs[V] = -1;
		}

		addControlDependenceEdges(symbMap, useMap, varNodes);

		// Iterate again over all varnodes of the constraint graph
		for (VarNodes::iterator vit = varNodes->begin(), vend = varNodes->end();
				vit != vend; ++vit) {
			Value *V = const_cast<Value*>(vit->first);

			// If the Value has not been visited yet, call visit for him
			if (dfs[V] < 0) {
				std::stack<Value*> pilha;
				if (!pilha.empty()) {
					errs() << "Erro na pilha\n";
				}

				visit(V, pilha, useMap);
			}
		}

		delControlDependenceEdges(useMap);
	}

#ifdef SCC_DEBUG
	ASSERT(checkWorklist(),"an inconsistency in SCC worklist have been found")
	ASSERT(checkComponents(), "a component has been used more than once")
	ASSERT(checkTopologicalSort(useMap), "topological sort is incorrect")
#endif
}

#ifdef SCC_DEBUG
bool Nuutila::checkWorklist() {
	bool consistent = true;
	for (Nuutila::iterator nit = this->begin(), nend = this->end();
			nit != nend; ++nit) {
		Value *v = *nit;
		for (Nuutila::iterator nit2 = this->begin(), nend2 = this->end();
				nit2 != nend2; ++nit2) {
			Value *v2 = *nit2;
			if (v == v2 && nit != nit2) {
				errs() << "[Nuutila::checkWorklist] Duplicated entry in worklist\n";
				v->dump();
				consistent = false;
			}
		}
	}
	return consistent;
}

bool Nuutila::checkComponents() {
	bool isConsistent = true;
	for (Nuutila::iterator nit = this->begin(), nend = this->end();
			nit != nend; ++nit) {
		SmallPtrSet<VarNode*, 32> *component = &this->components[*nit];
		for (Nuutila::iterator nit2 = this->begin(), nend2 = this->end();
				nit2 != nend2; ++nit2) {
			SmallPtrSet<VarNode*, 32> *component2 = &this->components[*nit2];
			if (component == component2 && nit != nit2) {
				errs() << "[Nuutila::checkComponent] Component ["<< component
				<< ", " << component->size() << "]\n";
				isConsistent = false;
			}
		}
	}
	return isConsistent;
}

/**
 * Check if a component has an edge to another component
 */
bool Nuutila::hasEdge(SmallPtrSet<VarNode*, 32> *componentFrom,
		SmallPtrSet<VarNode*, 32> *componentTo, UseMap *useMap)
{
	for (SmallPtrSetIterator<VarNode*> vit = componentFrom->begin(),
			vend = componentFrom->end(); vit != vend; ++vit)
	{
		const Value *source = (*vit)->getValue();
		for (SmallPtrSetIterator<BasicOp*> sit = (*useMap)[source].begin(),
				send = (*useMap)[source].end(); sit != send; ++sit)
		{
			BasicOp *op = *sit;
			if(componentTo->count(op->getSink())) {
				return true;
			}
		}
	}
	return false;
}

bool Nuutila::checkTopologicalSort(UseMap *useMap) {
	bool isConsistent = true;
	DenseMap<SmallPtrSet<VarNode*, 32>*,bool> visited;
	for (Nuutila::iterator nit = this->begin(), nend = this->end();
			nit != nend; ++nit) {
		SmallPtrSet<VarNode*, 32> *component = &this->components[*nit];
		visited[component] = false;
	}

	for (Nuutila::iterator nit = this->begin(), nend = this->end();
			nit != nend; ++nit) {
		SmallPtrSet<VarNode*, 32> *component = &this->components[*nit];

		if(!visited[component]) {
			visited[component] = true;
			//check if this component points to another component that has already been visited
			for (Nuutila::iterator nit2 = this->begin(), nend2 = this->end();
					nit2 != nend2; ++nit2) {
				SmallPtrSet<VarNode*, 32> *component2 = &this->components[*nit2];
				if(nit != nit2 && visited[component2] &&
						hasEdge(component, component2, useMap)) {
					isConsistent = false;
				}
			}
		} else {
			errs() << "[Nuutila::checkTopologicalSort] Component visited more than once time\n";
		}

	}

	return isConsistent;
}
#endif

#define ASSERT_TRUE(print_op,op,op1,op2,res) total++; if(op1.op(op2) != res){ \
			failed++; \
			errs() << "\t[" << total << "] " << print_op << ": "; \
			op1.print(errs()); \
			errs() << " "; \
			op2.print(errs()); \
			errs() << " RESULT: "; \
			(op1.op(op2)).print(errs()); \
			errs() << " EXPECTED: "; \
			res.print(errs()); \
			errs() << "\n";}

void RangeUnitTest::printStats() {
	errs() << "\n//********************** STATS *******************************//\n";
	errs() << "\tFailed: " << failed << " (" << failed / total;
	if (failed > 0)
		errs() << "." << 100 / (total / failed);
	errs() << "%)\n";
	errs() << "\tTotal: " << total << "\n";
	errs()
			<< "//************************************************************//\n";
}

bool RangeUnitTest::runOnModule(Module & M) {
	MAX_BIT_INT = InterProceduralRA<Cousot>::getMaxBitWidth(M);
	RangeAnalysis::updateMinMax(MAX_BIT_INT);
	errs() << "Running unit tests for Range class!\n";
	// --------------------------- Shared Objects -------------------------//
	Range unknown(Min, Max, Unknown);
	Range empty(Min, Max, Empty);
	Range zero(Zero, Zero);
	Range infy(Min, Max);
	Range pos(Zero, Max);
	Range neg(Min, Zero);
	// -------------------------------- ADD --------------------------------//
	// [a, b] - [c, d] = [a + c, b + d]
	ASSERT_TRUE("ADD", add, infy, infy, infy);
	ASSERT_TRUE("ADD", add, zero, infy, infy);
	ASSERT_TRUE("ADD", add, zero, zero, zero);
	ASSERT_TRUE("ADD", add, neg, zero, neg);
	ASSERT_TRUE("ADD", add, neg, infy, infy);
	ASSERT_TRUE("ADD", add, neg, neg, neg);
	ASSERT_TRUE("ADD", add, pos, zero, pos);
	ASSERT_TRUE("ADD", add, pos, infy, infy);
	ASSERT_TRUE("ADD", add, pos, neg, infy);
	ASSERT_TRUE("ADD", add, pos, pos, pos);
	ASSERT_TRUE("ADD", add, Range(Zero, Min-50), Range(Zero, Min-50), unknown);

	// -------------------------------- SUB --------------------------------//
	// [a, b] - [c, d] = [a - d, b - c]
	ASSERT_TRUE("SUB", sub, infy, infy, infy);
	ASSERT_TRUE("SUB", sub, infy, zero, infy);
	ASSERT_TRUE("SUB", sub, infy, pos, infy);
	ASSERT_TRUE("SUB", sub, infy, neg, infy);
	ASSERT_TRUE("SUB", sub, zero, zero, zero);
	ASSERT_TRUE("SUB", sub, zero, infy, infy);
	ASSERT_TRUE("SUB", sub, zero, pos, neg);
	ASSERT_TRUE("SUB", sub, zero, neg, pos);
	ASSERT_TRUE("SUB", sub, pos, zero, pos);
	ASSERT_TRUE("SUB", sub, pos, infy, infy);
	ASSERT_TRUE("SUB", sub, pos, neg, pos);
	ASSERT_TRUE("SUB", sub, pos, pos, infy);
	ASSERT_TRUE("SUB", sub, neg, zero, neg);
	ASSERT_TRUE("SUB", sub, neg, infy, infy);
	ASSERT_TRUE("SUB", sub, neg, neg, infy);
	ASSERT_TRUE("SUB", sub, neg, pos, neg);

	// -------------------------------- MUL --------------------------------//
	//  [a, b] * [c, d] = [Min(a*c, a*d, b*c, b*d), Max(a*c, a*d, b*c, b*d)]
	ASSERT_TRUE("MUL", mul, infy, infy, infy);
	ASSERT_TRUE("MUL", mul, zero, infy, infy);
	ASSERT_TRUE("MUL", mul, zero, zero, zero);
	ASSERT_TRUE("MUL", mul, neg, zero, zero);
	ASSERT_TRUE("MUL", mul, neg, infy, infy);
	ASSERT_TRUE("MUL", mul, neg, neg, pos);
	ASSERT_TRUE("MUL", mul, pos, zero, zero);
	ASSERT_TRUE("MUL", mul, pos, infy, infy);
	ASSERT_TRUE("MUL", mul, pos, neg, neg);
	ASSERT_TRUE("MUL", mul, pos, pos, pos);

	printStats();
	return true;
}

char RangeUnitTest::ID = 3;
static RegisterPass<RangeUnitTest> T("ra-test-range",
		"Run unit test for class Range");

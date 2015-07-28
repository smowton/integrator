//===- TentativeLoads.cpp -------------------------------------------------===//
//
// The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

// A mini-analysis that spots tentative loads and memcpy instructions.
// These are loads whose incoming dataflow (a) crosses a /yield point/, a point where we must assume
// that another thread got a chance to run and messed with our state, (b) is not dominated
// by other loads or stores that will check the incoming state / overwrite it with known state,
// and (c) is not known to be thread-local regardless.

// The main phase has already taken care of part (c) for us by setting ShadowInstruction::u.load.isThreadLocal
// when the load was known to be from a thread-private object. We will set the same flag wherever
// it's clear that checking this load would be redundant.

#include "llvm/Analysis/LLPE.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/IR/DataLayout.h"

using namespace llvm;

static uint32_t TLProgressN = 0;
const uint32_t TLProgressLimit = 1000;

static void TLProgress() {

  TLProgressN++;
  if(TLProgressN == TLProgressLimit) {

    errs() << ".";
    TLProgressN = 0;

  }

}

static TLMapTy::Allocator TLMapAllocator;
static TLMapTy TLEmptyMap(TLMapAllocator);
TLMapPointer llvm::TLEmptyMapPtr(&TLEmptyMap);

TLLocalStore* TLMapPointer::getMapForBlock(ShadowBB* BB) {

  return BB->tlStore;

}

TLMapPointer TLMapPointer::getReadableCopy() {

  TLMapTy* newMap = new TLMapTy(TLMapAllocator);
  for(TLMapTy::iterator it = M->begin(), itend = M->end(); it != itend; ++it)
    newMap->insert(it.start(), it.stop(), *it);

  return TLMapPointer(newMap);

}

bool TLMapPointer::dropReference() {

  delete M;
  M = 0;

  return true;

}

void TLMapPointer::mergeStores(TLMapPointer* mergeFrom, TLMapPointer* mergeTo, uint64_t ASize, TLMerger* Visitor) {

  // Intersect the sets per byte. The values are just booleans, so overwriting without erasing is fine.

  SmallVector<std::pair<uint64_t, uint64_t>, 4> keepRanges;

  for(TLMapTy::iterator it = mergeFrom->M->begin(), itend = mergeFrom->M->end();
      it != itend; ++it) {

    for(TLMapTy::iterator toit = mergeTo->M->find(it.start()), toitend = mergeTo->M->end();
	toit != toitend && toit.start() < it.stop(); ++toit) {

      uint64_t keepStart = std::max(toit.start(), it.start());
      uint64_t keepStop = std::min(toit.stop(), it.stop());
      keepRanges.push_back(std::make_pair(keepStart, keepStop));

    }

  }

  mergeTo->M->clear();
  for(SmallVector<std::pair<uint64_t, uint64_t>, 4>::iterator it = keepRanges.begin(),
	itend = keepRanges.end(); it != itend; ++it) {

    mergeTo->M->insert(it->first, it->second, true);

  }

}

TLMapPointer* ShadowBB::getWritableTLStore(ShadowValue O) {

  tlStore = tlStore->getWritableFrameList();
  bool isNewStore;
  TLMapPointer* ret = tlStore->getOrCreateStoreFor(O, &isNewStore);

  if(isNewStore)
    ret->M = new TLMapTy(TLMapAllocator);

  return ret;

}

static void markAllObjectsTentative(ShadowInstruction* SI, ShadowBB* BB) {

  BB->tlStore = BB->tlStore->getEmptyMap();
  BB->tlStore->allOthersClobbered = true;
  BB->IA->yieldState = BARRIER_HERE;

  if(inst_is<LoadInst>(SI) || inst_is<AtomicRMWInst>(SI))
    errs() << "Clobber all at " << SI->parent->IA->F.getName() << "," << SI->parent->invar->BB->getName() << "," << std::distance(SI->parent->invar->BB->begin(), BasicBlock::iterator(SI->invar->I)) << "\n";

}

static void markGoodBytes(ShadowValue GoodPtr, uint64_t Len, bool contextEnabled, ShadowBB* BB, uint64_t Offset = 0) {

  // ignoreUntil indicates we're within a disabled context. The loads and stores here will
  // be committed unmodified, in particular without checks that their results are as expected,
  // and so they do not make any subsequent check redundant.
  // Stores in disabled contexts can't count either, because of the situation:
  // disabled {
  //   call void thread_yield();
  //   %0 = load %x;
  //   store %0, %y;
  // }
  // %1 = load %y
  
  // Here the load %y must be checked, because the load %x cannot be checked.

  if(!contextEnabled)
    return;

  // If allOthersClobbered is false then no object is tentative.
  if(!BB->tlStore->allOthersClobbered)
    return;

  std::pair<ValSetType, ImprovedVal> PtrTarget;
  if(!tryGetUniqueIV(GoodPtr, PtrTarget))
    return;

  if(PtrTarget.first != ValSetTypePB)
    return;

  if(PtrTarget.second.V.isGV() &&  PtrTarget.second.V.u.GV->G->isConstant())
    return;

  SmallVector<std::pair<uint64_t, uint64_t>, 1> addRanges;

  TLMapPointer* store = BB->tlStore->getReadableStoreFor(PtrTarget.second.V);
  uint64_t start = PtrTarget.second.Offset + Offset;
  uint64_t stop = PtrTarget.second.Offset + Offset + Len;

  if(!store) {
   
    addRanges.push_back(std::make_pair(start, stop));

  }
  else {

    TLMapTy::iterator it = store->M->find(start), itend = store->M->end();

    if(it == itend || it.start() >= stop) {

      addRanges.push_back(std::make_pair(start, stop));

    }
    else {

      // Gap at left?

      if(it.start() > start)
	addRanges.push_back(std::make_pair(start, it.start()));

      for(; it != itend && it.start() < stop; ++it) {
    
	// Gap to the right of this extent?
	if(it.stop() < stop) {

	  TLMapTy::iterator nextit = it;
	  ++nextit;

	  uint64_t gapend;
	  if(nextit == itend)
	    gapend = stop;
	  else
	    gapend = std::min(stop, nextit.start());

	  if(it.stop() != gapend)
	    addRanges.push_back(std::make_pair(it.stop(), gapend));

	}

      }

    }

  }
  
  if(!addRanges.empty()) {

    TLMapPointer* writeStore = BB->getWritableTLStore(PtrTarget.second.V);
    for(SmallVector<std::pair<uint64_t, uint64_t>, 1>::iterator it = addRanges.begin(),
	  itend = addRanges.end(); it != itend; ++it) {

      writeStore->M->insert(it->first, it->second, true);

    }

  }

}

static void walkPathCondition(PathConditionTypes Ty, PathCondition& Cond, bool contextEnabled, ShadowBB* BB) {

  ShadowValue CondSV = BB->IA->getFunctionRoot()->getPathConditionSV(Cond);
  uint64_t Len = 0;
  switch(Ty) {
  case PathConditionTypeIntmem:
    Len = GlobalAA->getTypeStoreSize(Cond.u.val->getType());
    break;
  case PathConditionTypeString:
    Len = cast<ConstantDataArray>(Cond.u.val)->getNumElements();
    break;
  default:
    release_assert(0 && "Bad path condition type");
    llvm_unreachable("Bad path condition type");
  }

  markGoodBytes(CondSV, Len, contextEnabled, BB, Cond.offset);

}

static void walkPathConditions(PathConditionTypes Ty, std::vector<PathCondition>& Conds, bool contextEnabled, ShadowBB* BB, uint32_t stackDepth) {

  for(std::vector<PathCondition>::iterator it = Conds.begin(), itend = Conds.end(); it != itend; ++it) {

    if(stackDepth != it->fromStackIdx || BB->invar->BB != it->fromBB)
      continue;

    walkPathCondition(Ty, *it, contextEnabled, BB);

  }

}

void llvm::doTLCallMerge(ShadowBB* BB, InlineAttempt* IA) {

  TLMerger V(BB->IA, false);
  IA->visitLiveReturnBlocks(V);
  V.doMerge();
  
  BB->tlStore = V.newMap;

}

static void walkPathConditionsIn(PathConditions& PC, uint32_t stackIdx, ShadowBB* BB, bool contextEnabled, bool secondPass) {

  walkPathConditions(PathConditionTypeIntmem, PC.IntmemPathConditions, 
		     contextEnabled, BB, stackIdx);
  walkPathConditions(PathConditionTypeString, PC.StringPathConditions, 
		     contextEnabled, BB, stackIdx);

  for(std::vector<PathFunc>::iterator it = PC.FuncPathConditions.begin(),
	itend = PC.FuncPathConditions.end(); it != itend; ++it) {

    if(it->stackIdx != stackIdx)
      continue;
    
    it->IA->BBs[0]->tlStore = BB->tlStore;
    // Path conditions can be treated like committed code, as the user is responsible for checking
    // their applicability.
    it->IA->findTentativeLoads(/* commitDisabledHere = */false, secondPass);
    doTLCallMerge(BB, it->IA);
    
  }

}

void llvm::TLWalkPathConditions(ShadowBB* BB, bool contextEnabled, bool secondPass) {

  InlineAttempt* IA = BB->IA->getFunctionRoot();
  
  if(IA->targetCallInfo)
    walkPathConditionsIn(GlobalIHP->pathConditions, IA->targetCallInfo->targetStackDepth, BB, contextEnabled, secondPass);

  if(BB->IA->invarInfo->pathConditions)
    walkPathConditionsIn(*BB->IA->invarInfo->pathConditions, UINT_MAX, BB, contextEnabled, secondPass);

}

static void walkCopyInst(ShadowValue CopyFrom, ShadowValue CopyTo, ShadowValue LenSV, bool contextEnabled, ShadowBB* BB) {

  uint64_t Len;
  if(!tryGetConstantInt(LenSV, Len))
    return;

  markGoodBytes(CopyTo, Len, contextEnabled, BB);
  markGoodBytes(CopyFrom, Len, contextEnabled, BB);

}


static void updateTLStore(ShadowInstruction* SI, bool contextEnabled) {

  if(inst_is<AllocaInst>(SI)) {

    ShadowValue SV(SI);
    ShadowValue Base;
    getBaseObject(SV, Base);
    markGoodBytes(ShadowValue(SI), SI->parent->IA->getFunctionRoot()->localAllocas[Base.u.PtrOrFd.idx].storeSize, contextEnabled, SI->parent);

  }
  else if(LoadInst* LI = dyn_cast_inst<LoadInst>(SI)) {

    if((LI->isVolatile() || SI->hasOrderingConstraint()) && !SI->parent->IA->pass->atomicOpIsSimple(LI))
      markAllObjectsTentative(SI, SI->parent);
    else
      markGoodBytes(SI->getOperand(0), GlobalAA->getTypeStoreSize(LI->getType()), contextEnabled, SI->parent);

  }
  else if(StoreInst* StoreI = dyn_cast_inst<StoreInst>(SI)) {

    // I don't think there's a need to regard a volatile /store/ as a yield point, as this is *outgoing* interthread communication
    // if it communication at all. Compare pthread_unlock which is not a yield point to pthread_lock, which is.
    //if(StoreI->isVolatile())
    //markAllObjectsTentative(SI, SI->parent);
    //else
    markGoodBytes(SI->getOperand(1), GlobalAA->getTypeStoreSize(StoreI->getValueOperand()->getType()), contextEnabled, SI->parent);

  }
  else if(SI->readsMemoryDirectly() && SI->hasOrderingConstraint()) {

    // Might create a synchronisation edge:
    if(SI->isThreadLocal == TLS_MUSTCHECK && !SI->parent->IA->pass->atomicOpIsSimple(SI->invar->I))
      markAllObjectsTentative(SI, SI->parent);
    else
      markGoodBytes(SI->getOperand(0), GlobalAA->getTypeStoreSize(SI->getType()), contextEnabled, SI->parent);

  }
  else if(inst_is<FenceInst>(SI)) {

    markAllObjectsTentative(SI, SI->parent);

  }
  else if(inst_is<CallInst>(SI) || inst_is<InvokeInst>(SI)) {

    if(inst_is<MemSetInst>(SI)) {

      uint64_t MemSize;
      if(!tryGetConstantInt(SI->getCallArgOperand(2), MemSize))
	return;

      markGoodBytes(SI->getCallArgOperand(0), MemSize, contextEnabled, SI->parent);

    }
    else if(inst_is<MemTransferInst>(SI)) {

      walkCopyInst(SI->getCallArgOperand(0), SI->getCallArgOperand(1), SI->getCallArgOperand(2), contextEnabled, SI->parent);

    }
    else {

      CallInst* CallI = dyn_cast_inst<CallInst>(SI);

      Function* F = getCalledFunction(SI);
      DenseMap<Function*, specialfunctions>::iterator findit;
      if(ReadFile* RF = SI->parent->IA->tryGetReadFile(SI)) {

	markGoodBytes(SI->getCallArgOperand(1), RF->readSize, contextEnabled, SI->parent);

      }
      else if((findit = SpecialFunctionMap.find(F)) != SpecialFunctionMap.end()) {

	switch(findit->second) {

	case SF_REALLOC:

	  walkCopyInst(SI, SI->getCallArgOperand(0), SI->getCallArgOperand(1), contextEnabled, SI->parent);
	  // Fall through to:

	case SF_MALLOC:
	  
	  {
	  
	    ShadowValue SV(SI);
	    ShadowValue Base;
	    getBaseObject(SV, Base);

	    markGoodBytes(SV, GlobalIHP->heap[Base.u.PtrOrFd.idx].storeSize, contextEnabled, SI->parent);

	  }

	default:
	  break;

	}

      }
      else if(CallI && (((!F) && !GlobalIHP->programSingleThreaded) || GlobalIHP->yieldFunctions.count(F))) {

	if(GlobalIHP->pessimisticLocks.count(CallI)) {

	  // Pessimistic locks clobber at specialisation time;
	  // no runtime checking required.
	  return;

	}
	
	SmallDenseMap<CallInst*, std::vector<GlobalVariable*>, 4>::iterator findit =
	  GlobalIHP->lockDomains.find(CallI);

	if(findit != GlobalIHP->lockDomains.end()) {

	  for(std::vector<GlobalVariable*>::iterator it = findit->second.begin(),
		itend = findit->second.end(); it != itend; ++it) {

	    ShadowGV* SGV = &GlobalIHP->shadowGlobals[GlobalIHP->getShadowGlobalIndex(*it)];
	    ShadowValue SV(SGV);
	    TLMapPointer* TLObj = SI->parent->getWritableTLStore(SV);
	    // Mark whole object tentative:
	    TLObj->M->clear();

	  }

	}
	else {

	  // No explicit domain given; clobbers everything.
	  markAllObjectsTentative(SI, SI->parent);

	}

      }

    }

  }

}

static bool shouldCheckRead(ImprovedVal& Ptr, uint64_t Size, ShadowBB* BB) {

  // Read from null?
  if(Ptr.V.isNullPointer())
    return false;

  // Read from constant global?
  if(Ptr.V.isGV() && Ptr.V.u.GV->G->isConstant())
    return false;

  bool verbose = false;

  if(verbose)
    errs() << "Read from " << itcache(Ptr.V) << ":\n";

  TLMapPointer* Map = BB->tlStore->getReadableStoreFor(Ptr.V);
  if(!Map) {
    if(verbose)
      errs() << "Whole map: " << BB->tlStore->allOthersClobbered << "\n";
    return BB->tlStore->allOthersClobbered;
  }

  if(verbose) {

    for(TLMapTy::iterator it = Map->M->begin(), itend = Map->M->end(); it != itend; ++it) {

      errs() << it.start() << "-" << it.stop() << "\n";

    }

  }

  TLMapTy::iterator it = Map->M->find(Ptr.Offset);
  bool coveredByMap = (it != Map->M->end() && ((int64_t)it.start()) <= Ptr.Offset && ((int64_t)it.stop()) >= Ptr.Offset + ((int64_t)Size));

  return !coveredByMap;
    
}

ThreadLocalState IntegrationAttempt::shouldCheckCopy(ShadowInstruction& SI, ShadowValue PtrOp, ShadowValue LenSV) {

  uint64_t Len;
  bool LenValid = tryGetConstantInt(LenSV, Len);
  std::pair<ValSetType, ImprovedVal> Ptr;

  if((!LenValid) || (!tryGetUniqueIV(PtrOp, Ptr)) || Ptr.first != ValSetTypePB)
    return TLS_NEVERCHECK;

  if(Len == 0)
    return TLS_NEVERCHECK;

  // memcpyValues is unpopulated if the copy didn't "work" during specialisation,
  // so there is nothing to check.
  DenseMap<ShadowInstruction*, SmallVector<IVSRange, 4> >::iterator findit = GlobalIHP->memcpyValues.find(&SI);
  if(findit == GlobalIHP->memcpyValues.end() || !findit->second.size())
    return TLS_NEVERCHECK;

  // Check each concrete value that was successfully read during information prop
  for(SmallVector<IVSRange, 4>::iterator it = findit->second.begin(),
	itend = findit->second.end(); it != itend; ++it) {

    if(it->second.isWhollyUnknown())
      continue;

    ImprovedVal ReadPtr = Ptr.second;
    ReadPtr.Offset += it->first.first;
    if(shouldCheckRead(ReadPtr, it->first.second - it->first.first, SI.parent))
      return TLS_MUSTCHECK;

  }

  // No value requires a runtime check
  return TLS_NOCHECK;
    
}

ThreadLocalState IntegrationAttempt::shouldCheckLoadFrom(ShadowInstruction& SI, ImprovedVal& Ptr, uint64_t LoadSize) {

  if(Ptr.V.isNullOrConst())
    return TLS_NEVERCHECK;

  ImprovedValSetMulti* IV = dyn_cast<ImprovedValSetMulti>(SI.i.PB);
  if(IV) {

    SmallVector<IVSRange, 4> vals;
    for(ImprovedValSetMulti::MapIt it = IV->Map.begin(), itend = IV->Map.end(); it != itend; ++it) {

      if(it.value().isWhollyUnknown())
	continue;      

      ImprovedVal ReadPtr = Ptr;
      ReadPtr.Offset += it.start();
      if(shouldCheckRead(ReadPtr, it.stop() - it.start(), SI.parent))
	return TLS_MUSTCHECK;

    }

    return TLS_NOCHECK;

  }

  return shouldCheckRead(Ptr, LoadSize, SI.parent) ? TLS_MUSTCHECK : TLS_NOCHECK;
  
}

ThreadLocalState IntegrationAttempt::shouldCheckLoad(ShadowInstruction& SI) {

  if(GlobalIHP->programSingleThreaded)
    return TLS_NEVERCHECK;

  if(SI.readsMemoryDirectly() && !SI.isCopyInst()) {

    // Load doesn't extract any useful information?
    ImprovedValSetSingle* IVS = dyn_cast<ImprovedValSetSingle>(SI.i.PB);
    if(IVS && IVS->isWhollyUnknown())
      return TLS_NEVERCHECK;

  }

  if(inst_is<LoadInst>(&SI)) {

    if(SI.hasOrderingConstraint())
      return TLS_MUSTCHECK;

    // Read from known-good memory?

    ShadowValue PtrOp = SI.getOperand(0);
    std::pair<ValSetType, ImprovedVal> Single;
    ImprovedValSet* IV;

    uint64_t LoadSize = GlobalAA->getTypeStoreSize(SI.getType());

    getIVOrSingleVal(PtrOp, IV, Single);
    if(IV) {

      ImprovedValSetSingle* IVS = cast<ImprovedValSetSingle>(IV);

      if(IVS->isWhollyUnknown() || IVS->SetType != ValSetTypePB)
	return TLS_NEVERCHECK;

      ThreadLocalState result = TLS_NEVERCHECK;

      for(uint32_t i = 0, ilim = IVS->Values.size(); i != ilim && result != TLS_MUSTCHECK; ++i)
	result = std::min(shouldCheckLoadFrom(SI, IVS->Values[i], LoadSize), result);

      return result;

    }
    else {

      if(Single.first != ValSetTypePB)
	return TLS_NEVERCHECK;
      return shouldCheckLoadFrom(SI, Single.second, LoadSize);

    }

  }
  else if(inst_is<MemTransferInst>(&SI)) {

    ShadowValue PtrOp = SI.getCallArgOperand(1);
    ShadowValue Len = SI.getCallArgOperand(2);

    return shouldCheckCopy(SI, PtrOp, Len);

  }
  else if(inst_is<AtomicRMWInst>(&SI) || inst_is<AtomicCmpXchgInst>(&SI)) {

    // Always volatile if anything useful was loaded.
    return TLS_MUSTCHECK;

  }
  else {

    // Realloc instruction
    return shouldCheckCopy(SI, SI.getCallArgOperand(0), SI.getCallArgOperand(1));

  }

}

bool ShadowInstruction::isCopyInst() {

  if(inst_is<MemTransferInst>(this))
    return true;

  if(inst_is<CallInst>(this)) {

    Function* F = getCalledFunction(this);
    DenseMap<Function*, specialfunctions>::iterator findit = SpecialFunctionMap.find(F);
    if(findit == SpecialFunctionMap.end())
      return false;

    switch(findit->second) {
      
    case SF_VACOPY:
    case SF_REALLOC:
      return true;
    default:
      return false;

    }


  }

  return false;

}

ShadowValue ShadowInstruction::getCopySource() {

  if(inst_is<MemTransferInst>(this)) {

    return getCallArgOperand(1);

  }
  else if(inst_is<CallInst>(this)) {

    Function* F = getCalledFunction(this);
    if(!F)
      return ShadowValue();

    DenseMap<Function*, specialfunctions>::iterator findit = SpecialFunctionMap.find(F);
    if(findit == SpecialFunctionMap.end())
      return ShadowValue();

    switch(findit->second) {
      
    case SF_VACOPY:
      return getCallArgOperand(1);
    case SF_REALLOC:
      return getCallArgOperand(0);
    default:
      return ShadowValue();

    }
    
  }
  else {

    return ShadowValue();

  }

}

ShadowValue ShadowInstruction::getCopyDest() {

  if(inst_is<MemTransferInst>(this)) {

    return getCallArgOperand(0);

  }
  else if(inst_is<CallInst>(this)) {

    Function* F = getCalledFunction(this);
    if(!F)
      return ShadowValue();

    DenseMap<Function*, specialfunctions>::iterator findit = SpecialFunctionMap.find(F);
    if(findit == SpecialFunctionMap.end())
      return ShadowValue();

    switch(findit->second) {
      
    case SF_VACOPY:
      return getCallArgOperand(0);
    case SF_REALLOC:
      return ShadowValue(this);
    default:
      return ShadowValue();

    }
    
  }
  else {

    return ShadowValue();

  }

}

void llvm::doTLStoreMerge(ShadowBB* BB) {

  TLMerger V(BB->IA, false);
  BB->IA->visitNormalPredecessorsBW(BB, &V, /* ctx = */0);
  V.doMerge();

  BB->tlStore = V.newMap;

}

void InlineAttempt::findTentativeLoads(bool commitDisabledHere, bool secondPass) {

  if(isRootMainCall()) {
    BBs[0]->tlStore = new TLLocalStore(0);
    BBs[0]->tlStore->allOthersClobbered = false;
  }

  if(invarInfo->frameSize != -1 || !Callers.size()) {
    BBs[0]->tlStore = BBs[0]->tlStore->getWritableFrameList();
    BBs[0]->tlStore->pushStackFrame(this);
  }

  findTentativeLoadsInLoop(0, commitDisabledHere, secondPass);

}

bool IntegrationAttempt::squashUnavailableObject(ShadowInstruction& SI, const ImprovedValSetSingle& IVS, bool inLoopAnalyser, ShadowValue ReadPtr, int64_t ReadOffset, uint64_t ReadSize) {

  bool squash = false;

  for(uint32_t i = 0, ilim = IVS.Values.size(); i != ilim && !squash; ++i) {

    const ImprovedVal& IV = IVS.Values[i];

    if(IVS.SetType == ValSetTypePB) {

      // Stack objects are always available, so no need to check them.
      if(IV.V.isPtrIdx() && IV.V.getFrameNo() == -1) {

	// Globals too:
	AllocData* AD = getAllocData(IV.V);
	if(AD->allocValue.isInst()) {

	  if(AD->isCommitted && !AD->committedVal)
	    squash = true;

	}

      }

    }
    else if(IVS.SetType == ValSetTypeFD) {

      if(IV.V.isFdIdx()) {

	FDGlobalState& FDGS = pass->fds[IV.V.getFd()];
	if(FDGS.isCommitted && !FDGS.CommittedVal)
	  squash = true;

      }

    }

  }

  if(squash) {

    release_assert((!inLoopAnalyser) && "TODO: squashUnavailableObject implementation for loops");

    errs() << "Squash ";
    IVS.print(errs(), false);
    errs() << " read by " << itcache(&SI) << "\n";

    // Instruction no longer checkable:
    SI.isThreadLocal = TLS_NEVERCHECK;

    // Overwrite the pointer in the store to prevent future readers from encountering it again.
    ImprovedValSetSingle OD(ValSetTypeUnknown, true);
    ImprovedValSetSingle ReadP;
    getImprovedValSetSingle(ReadPtr, ReadP);
    
    release_assert(ReadP.SetType == ValSetTypePB && ReadP.Values.size());

    for(uint32_t i = 0, ilim = ReadP.Values.size(); i != ilim; ++i)
      ReadP.Values[i].Offset += ReadOffset;

    executeWriteInst(&ReadPtr, ReadP, OD, ReadSize, &SI);

  }

  return squash;

}

void IntegrationAttempt::squashUnavailableObjects(ShadowInstruction& SI, ImprovedValSet* PB, bool inLoopAnalyser) {

  if(ImprovedValSetSingle* IVS = dyn_cast_or_null<ImprovedValSetSingle>(PB)) {
    if(squashUnavailableObject(SI, *IVS, inLoopAnalyser, SI.getOperand(0), 0, GlobalTD->getTypeStoreSize(SI.getType())))
      IVS->setOverdef();
  }
  else {

    ImprovedValSetMulti* IVM = cast<ImprovedValSetMulti>(PB);
    for(ImprovedValSetMulti::MapIt it = IVM->Map.begin(), itend = IVM->Map.end();
	it != itend; ++it) {

      if(squashUnavailableObject(SI, it.value(), inLoopAnalyser, SI.getOperand(0), it.start(), it.stop() - it.start())) {
	ImprovedValSetSingle OD(it.value().SetType, true);
	uint64_t oldStart = it.start(), oldStop = it.stop();
	it.erase();
	it.insert(oldStart, oldStop, OD);
      }

    }

  }

}

void IntegrationAttempt::squashUnavailableObjects(ShadowInstruction& SI, bool inLoopAnalyser) {

  // The result of this load (or data read by this copy instruction) may contain pointers or
  // FDs which are not available, but it requires a check and the check cannot be synthesised.
  // Therefore replace them with Unknown.

  if(inst_is<LoadInst>(&SI) || inst_is<AtomicCmpXchgInst>(&SI)) {

    if(SI.i.PB)
      squashUnavailableObjects(SI, SI.i.PB, inLoopAnalyser);

  }
  else {

    // Copy instruction.
    DenseMap<ShadowInstruction*, SmallVector<IVSRange, 4> >::iterator findit = pass->memcpyValues.find(&SI);
    if(findit != pass->memcpyValues.end()) {

      for(SmallVector<IVSRange, 4>::iterator it = findit->second.begin(), itend = findit->second.end();
	  it != itend; ++it) {

	if(squashUnavailableObject(SI, it->second, inLoopAnalyser, SI.getCopySource(), it->first.first, it->first.second - it->first.first)) {

	  it->second.setOverdef();

	  // Undo storing the pointer or FD.

	  ImprovedValSetSingle OD(ValSetTypeUnknown, true);

	  ShadowValue WritePtr = SI.getCopyDest();
	  ImprovedValSetSingle WriteP;
	  getImprovedValSetSingle(WritePtr, WriteP);

	  int64_t WriteOffset = it->first.first;
	  uint64_t WriteSize = it->first.second - it->first.first;
    
	  release_assert(WriteP.SetType == ValSetTypePB && WriteP.Values.size());

	  for(uint32_t i = 0, ilim = WriteP.Values.size(); i != ilim; ++i)
	    WriteP.Values[i].Offset += WriteOffset;

	  executeWriteInst(&WritePtr, WriteP, OD, WriteSize, &SI);

	}

      }

    }

  }

}

void IntegrationAttempt::replaceUnavailableObjects(ShadowInstruction& SI, bool inLoopAnalyser) {

  // If this load read a pointer or FD that is currently unrealisable (i.e. has been previously committed
  // but currently has no committed value), volunteer to replace it, becoming the new definitive version,
  // if the block is certain and thus this version must be reachable from all (future) users.

  if(inLoopAnalyser)
    return;

  if(inst_is<LoadInst>(&SI) || inst_is<CallInst>(&SI) || inst_is<InvokeInst>(&SI)) {

    if(SI.parent->status != BBSTATUS_CERTAIN)
      return;

    ShadowValue Base;
    int64_t Offset;
    if(getBaseAndConstantOffset(ShadowValue(&SI), Base, Offset, false)) {
      
      if(Base.isPtrIdx() && Base.getFrameNo() == -1) {

	AllocData* AD = getAllocData(Base);
	if(AD->isCommitted && !AD->committedVal) {

	  // This means that the save phase will record the new reference and patch refs will be accrued
	  // in the meantime.
	  errs() << itcache(&SI) << " stepping up as new canonical reference for " << itcache(Base) << "\n";
	  AD->isCommitted = false;
	  AD->allocValue = ShadowValue(&SI);
	  // Should be safe to change the allocType, as until now the allocation had no examplar pointer
	  // and thus could not be referenced.
	  AD->allocType = SI.getType();
	  release_assert(isa<PointerType>(AD->allocType));

	}

      }

    }
    else {

      ImprovedValSetSingle* IVS = dyn_cast_or_null<ImprovedValSetSingle>(SI.i.PB);
      if(IVS && IVS->Values.size() == 1 && IVS->SetType == ValSetTypeFD && IVS->Values[0].V.isFdIdx()) {

	int32_t FD = IVS->Values[0].V.getFd();
	FDGlobalState& FDGS = pass->fds[FD];

	if(FDGS.isCommitted && !FDGS.CommittedVal) {

	  errs() << itcache(&SI) << " stepping up as new canonical reference for " << itcache(IVS->Values[0].V) << "\n";
	  FDGS.isCommitted = false;
	  FDGS.SI = &SI;

	}

      }

    }

  }

}

void IntegrationAttempt::TLAnalyseInstruction(ShadowInstruction& SI, bool commitDisabledHere, bool secondPass, bool inLoopAnalyser) {

  // Note that TLS_NEVERCHECK may have been assigned already during the main analysis phase,
  // signifying a load from a known thread-local object.

  if(SI.readsMemoryDirectly()) {

    // Ordinary load or memcpy, without memory ordering constraints.
    // Check this value if a previous memory op has rendered it uncertain.

    // Known that we must check when this block is reached from a loop preheader?
    // If so whether it is tentative from the latch is irrelevant.
    if(secondPass && SI.isThreadLocal == TLS_MUSTCHECK)
      return;
    
    if(SI.isThreadLocal != TLS_NEVERCHECK)
      SI.isThreadLocal = shouldCheckLoad(SI);
    
    if(SI.isThreadLocal == TLS_MUSTCHECK) {

      readsTentativeData = true;
      squashUnavailableObjects(SI, inLoopAnalyser);
      
    }
    else {

      replaceUnavailableObjects(SI, inLoopAnalyser);

    }

  }
  else if(inst_is<CallInst>(&SI) || inst_is<InvokeInst>(&SI)) {
      
    // This is a little awkwardly placed since expanded calls are not tentative loads,
    // but this way it's together with load instructions replacing an unavailable object.
    replaceUnavailableObjects(SI, inLoopAnalyser);

  }
  else {

    if(SI.isThreadLocal == TLS_NEVERCHECK)
      return;

  }
  
  updateTLStore(&SI, !commitDisabledHere);

}

void IntegrationAttempt::findTentativeLoadsInUnboundedLoop(const ShadowLoopInvar* UL, bool commitDisabledHere, bool secondPass) {

  ShadowBB* BB = getBB(UL->headerIdx);

  // Give header its store:
  BB->tlStore = getBB(UL->preheaderIdx)->tlStore;
  
  if(!edgeIsDead(getBBInvar(UL->latchIdx), getBBInvar(UL->headerIdx))) {

    if(!secondPass) {
      // Passing true for the last parameter causes the store to be given to the header from the latch
      // and not to any exit blocks. 
      findTentativeLoadsInLoop(UL, commitDisabledHere, false, true);
      BB->tlStore = getBB(UL->latchIdx)->tlStore;
    }
    findTentativeLoadsInLoop(UL, commitDisabledHere, true);

  }
  else {

    findTentativeLoadsInLoop(UL, commitDisabledHere, secondPass);

  }

}

void IntegrationAttempt::findTentativeLoadsInLoop(const ShadowLoopInvar* L, bool commitDisabledHere, bool secondPass, bool latchToHeader) {

  // Don't repeat search due to sharing:
  if(tentativeLoadsRun)
    return;

  TLProgress();

  uint32_t startIdx;
  if(L)
    startIdx = L->headerIdx;
  else
    startIdx = 0;

  for(uint32_t i = startIdx, ilim = nBBs + BBsOffset; i != ilim && ((!L) || L->contains(getBBInvar(i)->naturalScope)); ++i) {

    ShadowBB* BB = getBB(i);
    if(!BB)
      continue;
    
    if(BB->invar->naturalScope != L) {

      const ShadowLoopInvar* NewLInfo = BB->invar->naturalScope;

      PeelAttempt* LPA;
      if((LPA = getPeelAttempt(BB->invar->naturalScope)) && LPA->isTerminated()) {

	LPA->Iterations[0]->BBs[0]->tlStore = getBB(NewLInfo->preheaderIdx)->tlStore;
	bool commitDisabled = commitDisabledHere || !LPA->isEnabled();
	uint32_t latchIdx = NewLInfo->latchIdx;

	for(uint32_t j = 0, jlim = LPA->Iterations.size(); j != jlim; ++j) {

	  LPA->Iterations[j]->findTentativeLoadsInLoop(BB->invar->naturalScope, commitDisabled, secondPass);
	  if(j + 1 != jlim)
	    LPA->Iterations[j + 1]->BBs[0]->tlStore = LPA->Iterations[j]->getBB(latchIdx)->tlStore;

	}
	
      }
      else {

	findTentativeLoadsInUnboundedLoop(BB->invar->naturalScope, 
					  commitDisabledHere || (LPA && !LPA->isEnabled()),
					  secondPass);

      }

      while(i != ilim && BB->invar->naturalScope->contains(getBBInvar(i)->naturalScope))
	++i;
      --i;
      continue;

    }

    if(i != startIdx) {

      doTLStoreMerge(BB);

    }

    TLWalkPathConditions(BB, !commitDisabledHere, secondPass);

    bool brokeOnUnreachableCall = false;

    for(uint32_t j = 0, jlim = BB->invar->insts.size(); j != jlim; ++j) {

      ShadowInstruction& SI = BB->insts[j];
      TLAnalyseInstruction(SI, commitDisabledHere, secondPass, false);
      
      if(InlineAttempt* IA = getInlineAttempt(&SI)) {

	IA->BBs[0]->tlStore = BB->tlStore;
	IA->findTentativeLoads(commitDisabledHere || !IA->isEnabled(), secondPass);
	doTLCallMerge(BB, IA);

	if(!BB->tlStore) {

	  // Call exit unreachable
	  brokeOnUnreachableCall = true;
	  break;

	}	    

      }

    }

    if(!BB->tlStore) {

      // Block doesn't have a store due to a never-returns call.
      // Can't have any successors either in this case.

      release_assert(brokeOnUnreachableCall);
      continue;

    }

    // Give a store copy to each successor block that needs it. If latchToHeader is true,
    // ignore branches to outside the current loop; otherwise ignore any latch->header edge.

    for(uint32_t i = 0; i < BB->invar->succIdxs.size(); ++i) {

      if(!BB->succsAlive[i])
	continue;
      
      ShadowBBInvar* SuccBBI = getBBInvar(BB->invar->succIdxs[i]);
      if(L) {

	if(L != this->L && latchToHeader && !L->contains(SuccBBI->naturalScope))
	  continue;
	else if(L != this->L && (!latchToHeader) && SuccBBI->idx == L->headerIdx) {
	  release_assert(BB->invar->idx == L->latchIdx);
	  continue;
	}

      }

      // Create a store reference for each live successor
      ++BB->tlStore->refCount;

    }

    // Drop stack allocations here.

    if(BB->invar->succIdxs.size() == 0) {

      if(invarInfo->frameSize != -1) {
	BB->tlStore = BB->tlStore->getWritableFrameList();
	BB->tlStore->popStackFrame();
      }

    }

    // Drop the reference belonging to this block.

    if(!isa<ReturnInst>(BB->invar->BB->getTerminator()))
      SAFE_DROP_REF(BB->tlStore);
    
  }

}

void IntegrationAttempt::resetTentativeLoads() {

  tentativeLoadsRun = false;

  for(IAIterator it = child_calls_begin(this), itend = child_calls_end(this); it != itend; ++it) {

    it->second->resetTentativeLoads();

  }

  for(DenseMap<const ShadowLoopInvar*, PeelAttempt*>::iterator it = peelChildren.begin(),
	itend = peelChildren.end(); it != itend; ++it) {
    
    if(!it->second->isTerminated())
      continue;
    
    for(uint32_t i = 0; i < it->second->Iterations.size(); ++i)
      it->second->Iterations[i]->resetTentativeLoads();
    
  }

}

// Our main interface to other passes:

bool llvm::requiresRuntimeCheck(ShadowValue V, bool includeSpecialChecks) {

  if(GlobalIHP->omitChecks)
    return false;

  if(!V.isInst())
    return false;

  return V.u.I->parent->IA->requiresRuntimeCheck2(V, includeSpecialChecks);

}

void IntegrationAttempt::countTentativeInstructions() {

  if(isCommitted())
    return;

  for(uint32_t i = BBsOffset, ilim = BBsOffset + nBBs; i != ilim; ++i) {

    ShadowBBInvar* BBI = getBBInvar(i);
    ShadowBB* BB = getBB(*BBI);
    if(!BB)
      continue;

    if(BBI->naturalScope != L) {

      const ShadowLoopInvar* subL = immediateChildLoop(L, BBI->naturalScope);
      PeelAttempt* LPA;
      if((LPA = getPeelAttempt(subL)) && LPA->isTerminated()) {

	while(i != ilim && subL->contains(getBBInvar(i)->naturalScope))
	  ++i;
	--i;
	continue;

      }

    }

    for(uint32_t j = 0, jlim = BBI->insts.size(); j != jlim; ++j) {

      ShadowInstruction* SI = &BB->insts[j];

      // This should count only instructions that are checked because their result might be
      // invalidated by the concurrent action of other threads in the same address space.
      // Instructions with SI->needsRuntimeCheck set are checked to implement a path condition
      // or other check and so should not be included in the count.
      
      if(requiresRuntimeCheck2(ShadowValue(SI), false) && SI->needsRuntimeCheck == RUNTIME_CHECK_NONE)
	++checkedInstructionsHere;

    }

  }

  checkedInstructionsChildren = checkedInstructionsHere;

  for(IAIterator it = child_calls_begin(this), itend = child_calls_end(this); it != itend; ++it) {

    it->second->countTentativeInstructions();
    checkedInstructionsChildren += it->second->checkedInstructionsChildren;

  }

  for(DenseMap<const ShadowLoopInvar*, PeelAttempt*>::iterator it = peelChildren.begin(),
	itend = peelChildren.end(); it != itend; ++it) {

    if(!it->second->isTerminated())
      continue;

    for(uint32_t i = 0, ilim = it->second->Iterations.size(); i != ilim; ++i) {
     
      it->second->Iterations[i]->countTentativeInstructions();
      checkedInstructionsChildren += it->second->Iterations[i]->checkedInstructionsChildren;

    }

  }

}

bool PeelAttempt::containsTentativeLoads() {

  for(uint32_t i = 0, ilim = Iterations.size(); i != ilim; ++i)
    if(Iterations[i]->containsTentativeLoads())
      return true;

  return false;

}

bool IntegrationAttempt::containsTentativeLoads() {

  return readsTentativeData;

}

bool IntegrationAttempt::requiresRuntimeCheck2(ShadowValue V, bool includeSpecialChecks) {

  release_assert(V.isInst());
  ShadowInstruction* SI = V.u.I;

  if(SI->getType()->isVoidTy())
    return false;

  // This indicates a member of a disabled loop that hasn't been analysed.
  if(!SI->i.PB)
    return false;

  if(SI->needsRuntimeCheck == RUNTIME_CHECK_AS_EXPECTED)
    return true;
  if(includeSpecialChecks && (SI->needsRuntimeCheck == RUNTIME_CHECK_READ_LLIOWD || SI->needsRuntimeCheck == RUNTIME_CHECK_READ_MEMCMP))
    return true;

  if(inst_is<MemTransferInst>(SI) || ((!inst_is<CallInst>(SI)) && SI->readsMemoryDirectly())) {
    
    if(SI->isThreadLocal == TLS_MUSTCHECK)
      return true;
    
  }
  else if (InlineAttempt* IA = getInlineAttempt(SI)) {

    if((!IA->isEnabled()) && IA->containsTentativeLoads())
      return !SI->i.PB->isWhollyUnknown();

  }
  else if(inst_is<PHINode>(SI)) {

    ShadowBB* BB = SI->parent;
    for(uint32_t i = 0, ilim = BB->invar->predIdxs.size(); i != ilim; ++i) {

      ShadowBBInvar* predBBI = getBBInvar(BB->invar->predIdxs[i]);
      if(predBBI->naturalScope != L && ((!L) || L->contains(predBBI->naturalScope))) {

	PeelAttempt* LPA = getPeelAttempt(immediateChildLoop(L, predBBI->naturalScope));
	if(LPA && LPA->isTerminated() && (!LPA->isEnabled()) && LPA->containsTentativeLoads())
	  return !SI->i.PB->isWhollyUnknown();

      }

    }

  }

  return false;

}

void IntegrationAttempt::addCheckpointFailedBlocks() {

  if(isCommitted())
    return;

  for(uint32_t i = BBsOffset, ilim = BBsOffset + nBBs; i != ilim; ++i) {

    ShadowBBInvar* BBI = getBBInvar(i);
    ShadowBB* BB = getBB(*BBI);

    if(!BB)
      continue;

    if(BBI->naturalScope != L) {

      const ShadowLoopInvar* subL = immediateChildLoop(L, BBI->naturalScope);
      PeelAttempt* LPA;

      if((LPA = getPeelAttempt(subL)) && LPA->isTerminated() && LPA->isEnabled()) {

	for(uint32_t k = 0, klim = LPA->Iterations.size(); k != klim; ++k)
	  LPA->Iterations[k]->addCheckpointFailedBlocks();	

	while(i != ilim && subL->contains(getBBInvar(i)->naturalScope))
	  ++i;
	--i;
	continue;
	
      }

    }

    for(uint32_t j = 0, jlim = BBI->insts.size(); j != jlim; ++j) {

      ShadowInstruction* SI = &BB->insts[j];
      InlineAttempt* IA;

      if(requiresRuntimeCheck2(ShadowValue(SI), false) || SI->needsRuntimeCheck == RUNTIME_CHECK_READ_MEMCMP) {

	// Treat tested exit PHIs as a block.
	if(inst_is<PHINode>(SI) && (j + 1) != jlim && inst_is<PHINode>(&BB->insts[j+1]))
	  continue;
	
	// Invoke instruction?
	if(j == jlim - 1)
	  getFunctionRoot()->markBlockAndSuccsReachableUnspecialised(BB->invar->succIdxs[0], 0);
	else
	  getFunctionRoot()->markBlockAndSuccsReachableUnspecialised(i, j + 1);

      }
      else if(SI->needsRuntimeCheck == RUNTIME_CHECK_READ_LLIOWD) {

	// Special checks *precede* the instruction
	getFunctionRoot()->markBlockAndSuccsReachableUnspecialised(i, j);

      }
      else if((IA = getInlineAttempt(SI)) && IA->isEnabled()) {

	IA->addCheckpointFailedBlocks();
	if(IA->hasFailedReturnPath()) {

	  // If this is the block terminator then it must be an invoke instruction,
	  // the only kind of terminator that produces a checkable value.
	  // If it is an invoke, mark the normal continuation reachable on failure.
	  if(j == jlim - 1)
	    getFunctionRoot()->markBlockAndSuccsReachableUnspecialised(BB->invar->succIdxs[0], 0);
	  else
	    getFunctionRoot()->markBlockAndSuccsReachableUnspecialised(i, j + 1);

	}

      }

    }

  }

}

void llvm::rerunTentativeLoads(ShadowInstruction* SI, InlineAttempt* IA, bool inLoopAnalyser) {

  // This indicates the call never returns, and so there will be no further exploration along these lines.
  if(!SI->parent->tlStore)
    return;

  if(IA->readsTentativeData) {

    // There may have been thread interference during the function, and/or it may have read data
    // that needed checking from prior interference and may have used it, unchecked, to calculate
    // its return value or store values to memory. Everything needs checking at this point.
    errs() << "Warning: disabled context " << IA->SeqNumber << " reads tentative information\n";
    SI->parent->tlStore = SI->parent->tlStore->getEmptyMap();
    SI->parent->tlStore->allOthersClobbered = true;
    IA->backupTlStore->dropReference();

    if(IA->returnValue)
      SI->parent->IA->squashUnavailableObjects(*SI, IA->returnValue, inLoopAnalyser);

  }
  else {

    // It does not corrupt state, but it does not itself perform checks.
    // Undo any check elimination performed within the function.
    release_assert(IA->backupTlStore);
    SI->parent->tlStore->dropReference();
    SI->parent->tlStore = IA->backupTlStore;

  }

}

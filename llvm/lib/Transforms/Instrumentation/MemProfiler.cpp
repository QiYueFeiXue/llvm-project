//===- MemProfiler.cpp - memory allocation and access profiler ------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file is a part of MemProfiler. Memory accesses are instrumented
// to increment the access count held in a shadow memory location, or
// alternatively to call into the runtime. Memory intrinsic calls (memmove,
// memcpy, memset) are changed to call the memory profiling runtime version
// instead.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Instrumentation/MemProfiler.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Analysis/MemoryBuiltins.h"
#include "llvm/Analysis/MemoryProfileInfo.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Constant.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DiagnosticInfo.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Value.h"
#include "llvm/ProfileData/InstrProf.h"
#include "llvm/ProfileData/InstrProfReader.h"
#include "llvm/Support/BLAKE3.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/HashBuilder.h"
#include "llvm/Support/VirtualFileSystem.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/LongestCommonSequence.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"
#include <map>
#include <set>

using namespace llvm;
using namespace llvm::memprof;

#define DEBUG_TYPE "memprof"

namespace llvm {
extern cl::opt<bool> PGOWarnMissing;
extern cl::opt<bool> NoPGOWarnMismatch;
extern cl::opt<bool> NoPGOWarnMismatchComdatWeak;
} // namespace llvm

constexpr int LLVM_MEM_PROFILER_VERSION = 1;

// Size of memory mapped to a single shadow location.
constexpr uint64_t DefaultMemGranularity = 64;

// Size of memory mapped to a single histogram bucket.
constexpr uint64_t HistogramGranularity = 8;

// Scale from granularity down to shadow size.
constexpr uint64_t DefaultShadowScale = 3;

constexpr char MemProfModuleCtorName[] = "memprof.module_ctor";
constexpr uint64_t MemProfCtorAndDtorPriority = 1;
// On Emscripten, the system needs more than one priorities for constructors.
constexpr uint64_t MemProfEmscriptenCtorAndDtorPriority = 50;
constexpr char MemProfInitName[] = "__memprof_init";
constexpr char MemProfVersionCheckNamePrefix[] =
    "__memprof_version_mismatch_check_v";

constexpr char MemProfShadowMemoryDynamicAddress[] =
    "__memprof_shadow_memory_dynamic_address";

constexpr char MemProfFilenameVar[] = "__memprof_profile_filename";

constexpr char MemProfHistogramFlagVar[] = "__memprof_histogram";

// Command-line flags.

static cl::opt<bool> ClInsertVersionCheck(
    "memprof-guard-against-version-mismatch",
    cl::desc("Guard against compiler/runtime version mismatch."), cl::Hidden,
    cl::init(true));

// This flag may need to be replaced with -f[no-]memprof-reads.
static cl::opt<bool> ClInstrumentReads("memprof-instrument-reads",
                                       cl::desc("instrument read instructions"),
                                       cl::Hidden, cl::init(true));

static cl::opt<bool>
    ClInstrumentWrites("memprof-instrument-writes",
                       cl::desc("instrument write instructions"), cl::Hidden,
                       cl::init(true));

static cl::opt<bool> ClInstrumentAtomics(
    "memprof-instrument-atomics",
    cl::desc("instrument atomic instructions (rmw, cmpxchg)"), cl::Hidden,
    cl::init(true));

static cl::opt<bool> ClUseCalls(
    "memprof-use-callbacks",
    cl::desc("Use callbacks instead of inline instrumentation sequences."),
    cl::Hidden, cl::init(false));

static cl::opt<std::string>
    ClMemoryAccessCallbackPrefix("memprof-memory-access-callback-prefix",
                                 cl::desc("Prefix for memory access callbacks"),
                                 cl::Hidden, cl::init("__memprof_"));

// These flags allow to change the shadow mapping.
// The shadow mapping looks like
//    Shadow = ((Mem & mask) >> scale) + offset

static cl::opt<int> ClMappingScale("memprof-mapping-scale",
                                   cl::desc("scale of memprof shadow mapping"),
                                   cl::Hidden, cl::init(DefaultShadowScale));

static cl::opt<int>
    ClMappingGranularity("memprof-mapping-granularity",
                         cl::desc("granularity of memprof shadow mapping"),
                         cl::Hidden, cl::init(DefaultMemGranularity));

static cl::opt<bool> ClStack("memprof-instrument-stack",
                             cl::desc("Instrument scalar stack variables"),
                             cl::Hidden, cl::init(false));

// Debug flags.

static cl::opt<int> ClDebug("memprof-debug", cl::desc("debug"), cl::Hidden,
                            cl::init(0));

static cl::opt<std::string> ClDebugFunc("memprof-debug-func", cl::Hidden,
                                        cl::desc("Debug func"));

static cl::opt<int> ClDebugMin("memprof-debug-min", cl::desc("Debug min inst"),
                               cl::Hidden, cl::init(-1));

static cl::opt<int> ClDebugMax("memprof-debug-max", cl::desc("Debug max inst"),
                               cl::Hidden, cl::init(-1));

// By default disable matching of allocation profiles onto operator new that
// already explicitly pass a hot/cold hint, since we don't currently
// override these hints anyway.
static cl::opt<bool> ClMemProfMatchHotColdNew(
    "memprof-match-hot-cold-new",
    cl::desc(
        "Match allocation profiles onto existing hot/cold operator new calls"),
    cl::Hidden, cl::init(false));

static cl::opt<bool> ClHistogram("memprof-histogram",
                                 cl::desc("Collect access count histograms"),
                                 cl::Hidden, cl::init(false));

static cl::opt<bool>
    ClPrintMemProfMatchInfo("memprof-print-match-info",
                            cl::desc("Print matching stats for each allocation "
                                     "context in this module's profiles"),
                            cl::Hidden, cl::init(false));

static cl::opt<std::string>
    MemprofRuntimeDefaultOptions("memprof-runtime-default-options",
                                 cl::desc("The default memprof options"),
                                 cl::Hidden, cl::init(""));

static cl::opt<bool>
    SalvageStaleProfile("memprof-salvage-stale-profile",
                        cl::desc("Salvage stale MemProf profile"),
                        cl::init(false), cl::Hidden);

extern cl::opt<bool> MemProfReportHintedSizes;
extern cl::opt<unsigned> MinClonedColdBytePercent;
extern cl::opt<unsigned> MinCallsiteColdBytePercent;

static cl::opt<unsigned> MinMatchedColdBytePercent(
    "memprof-matching-cold-threshold", cl::init(100), cl::Hidden,
    cl::desc("Min percent of cold bytes matched to hint allocation cold"));

// Instrumentation statistics
STATISTIC(NumInstrumentedReads, "Number of instrumented reads");
STATISTIC(NumInstrumentedWrites, "Number of instrumented writes");
STATISTIC(NumSkippedStackReads, "Number of non-instrumented stack reads");
STATISTIC(NumSkippedStackWrites, "Number of non-instrumented stack writes");

// Matching statistics
STATISTIC(NumOfMemProfMissing, "Number of functions without memory profile.");
STATISTIC(NumOfMemProfMismatch,
          "Number of functions having mismatched memory profile hash.");
STATISTIC(NumOfMemProfFunc, "Number of functions having valid memory profile.");
STATISTIC(NumOfMemProfAllocContextProfiles,
          "Number of alloc contexts in memory profile.");
STATISTIC(NumOfMemProfCallSiteProfiles,
          "Number of callsites in memory profile.");
STATISTIC(NumOfMemProfMatchedAllocContexts,
          "Number of matched memory profile alloc contexts.");
STATISTIC(NumOfMemProfMatchedAllocs,
          "Number of matched memory profile allocs.");
STATISTIC(NumOfMemProfMatchedCallSites,
          "Number of matched memory profile callsites.");

namespace {

/// This struct defines the shadow mapping using the rule:
///   shadow = ((mem & mask) >> Scale) ADD DynamicShadowOffset.
struct ShadowMapping {
  ShadowMapping() {
    Scale = ClMappingScale;
    Granularity = ClHistogram ? HistogramGranularity : ClMappingGranularity;
    Mask = ~(Granularity - 1);
  }

  int Scale;
  int Granularity;
  uint64_t Mask; // Computed as ~(Granularity-1)
};

static uint64_t getCtorAndDtorPriority(Triple &TargetTriple) {
  return TargetTriple.isOSEmscripten() ? MemProfEmscriptenCtorAndDtorPriority
                                       : MemProfCtorAndDtorPriority;
}

struct InterestingMemoryAccess {
  Value *Addr = nullptr;
  bool IsWrite;
  Type *AccessTy;
  Value *MaybeMask = nullptr;
};

/// Instrument the code in module to profile memory accesses.
class MemProfiler {
public:
  MemProfiler(Module &M) {
    C = &(M.getContext());
    LongSize = M.getDataLayout().getPointerSizeInBits();
    IntptrTy = Type::getIntNTy(*C, LongSize);
    PtrTy = PointerType::getUnqual(*C);
  }

  /// If it is an interesting memory access, populate information
  /// about the access and return a InterestingMemoryAccess struct.
  /// Otherwise return std::nullopt.
  std::optional<InterestingMemoryAccess>
  isInterestingMemoryAccess(Instruction *I) const;

  void instrumentMop(Instruction *I, const DataLayout &DL,
                     InterestingMemoryAccess &Access);
  void instrumentAddress(Instruction *OrigIns, Instruction *InsertBefore,
                         Value *Addr, bool IsWrite);
  void instrumentMaskedLoadOrStore(const DataLayout &DL, Value *Mask,
                                   Instruction *I, Value *Addr, Type *AccessTy,
                                   bool IsWrite);
  void instrumentMemIntrinsic(MemIntrinsic *MI);
  Value *memToShadow(Value *Shadow, IRBuilder<> &IRB);
  bool instrumentFunction(Function &F);
  bool maybeInsertMemProfInitAtFunctionEntry(Function &F);
  bool insertDynamicShadowAtFunctionEntry(Function &F);

private:
  void initializeCallbacks(Module &M);

  LLVMContext *C;
  int LongSize;
  Type *IntptrTy;
  PointerType *PtrTy;
  ShadowMapping Mapping;

  // These arrays is indexed by AccessIsWrite
  FunctionCallee MemProfMemoryAccessCallback[2];

  FunctionCallee MemProfMemmove, MemProfMemcpy, MemProfMemset;
  Value *DynamicShadowOffset = nullptr;
};

class ModuleMemProfiler {
public:
  ModuleMemProfiler(Module &M) { TargetTriple = M.getTargetTriple(); }

  bool instrumentModule(Module &);

private:
  Triple TargetTriple;
  ShadowMapping Mapping;
  Function *MemProfCtorFunction = nullptr;
};

// Options under which we need to record the context size info in the alloc trie
// used to build metadata.
bool recordContextSizeInfo() {
  return MemProfReportHintedSizes || MinClonedColdBytePercent < 100 ||
         MinCallsiteColdBytePercent < 100;
}

} // end anonymous namespace

MemProfilerPass::MemProfilerPass() = default;

PreservedAnalyses MemProfilerPass::run(Function &F,
                                       AnalysisManager<Function> &AM) {
  assert((!ClHistogram || ClMappingGranularity == DefaultMemGranularity) &&
         "Memprof with histogram only supports default mapping granularity");
  Module &M = *F.getParent();
  MemProfiler Profiler(M);
  if (Profiler.instrumentFunction(F))
    return PreservedAnalyses::none();
  return PreservedAnalyses::all();
}

ModuleMemProfilerPass::ModuleMemProfilerPass() = default;

PreservedAnalyses ModuleMemProfilerPass::run(Module &M,
                                             AnalysisManager<Module> &AM) {

  ModuleMemProfiler Profiler(M);
  if (Profiler.instrumentModule(M))
    return PreservedAnalyses::none();
  return PreservedAnalyses::all();
}

Value *MemProfiler::memToShadow(Value *Shadow, IRBuilder<> &IRB) {
  // (Shadow & mask) >> scale
  Shadow = IRB.CreateAnd(Shadow, Mapping.Mask);
  Shadow = IRB.CreateLShr(Shadow, Mapping.Scale);
  // (Shadow >> scale) | offset
  assert(DynamicShadowOffset);
  return IRB.CreateAdd(Shadow, DynamicShadowOffset);
}

// Instrument memset/memmove/memcpy
void MemProfiler::instrumentMemIntrinsic(MemIntrinsic *MI) {
  IRBuilder<> IRB(MI);
  if (isa<MemTransferInst>(MI)) {
    IRB.CreateCall(isa<MemMoveInst>(MI) ? MemProfMemmove : MemProfMemcpy,
                   {MI->getOperand(0), MI->getOperand(1),
                    IRB.CreateIntCast(MI->getOperand(2), IntptrTy, false)});
  } else if (isa<MemSetInst>(MI)) {
    IRB.CreateCall(
        MemProfMemset,
        {MI->getOperand(0),
         IRB.CreateIntCast(MI->getOperand(1), IRB.getInt32Ty(), false),
         IRB.CreateIntCast(MI->getOperand(2), IntptrTy, false)});
  }
  MI->eraseFromParent();
}

std::optional<InterestingMemoryAccess>
MemProfiler::isInterestingMemoryAccess(Instruction *I) const {
  // Do not instrument the load fetching the dynamic shadow address.
  if (DynamicShadowOffset == I)
    return std::nullopt;

  InterestingMemoryAccess Access;

  if (LoadInst *LI = dyn_cast<LoadInst>(I)) {
    if (!ClInstrumentReads)
      return std::nullopt;
    Access.IsWrite = false;
    Access.AccessTy = LI->getType();
    Access.Addr = LI->getPointerOperand();
  } else if (StoreInst *SI = dyn_cast<StoreInst>(I)) {
    if (!ClInstrumentWrites)
      return std::nullopt;
    Access.IsWrite = true;
    Access.AccessTy = SI->getValueOperand()->getType();
    Access.Addr = SI->getPointerOperand();
  } else if (AtomicRMWInst *RMW = dyn_cast<AtomicRMWInst>(I)) {
    if (!ClInstrumentAtomics)
      return std::nullopt;
    Access.IsWrite = true;
    Access.AccessTy = RMW->getValOperand()->getType();
    Access.Addr = RMW->getPointerOperand();
  } else if (AtomicCmpXchgInst *XCHG = dyn_cast<AtomicCmpXchgInst>(I)) {
    if (!ClInstrumentAtomics)
      return std::nullopt;
    Access.IsWrite = true;
    Access.AccessTy = XCHG->getCompareOperand()->getType();
    Access.Addr = XCHG->getPointerOperand();
  } else if (auto *CI = dyn_cast<CallInst>(I)) {
    auto *F = CI->getCalledFunction();
    if (F && (F->getIntrinsicID() == Intrinsic::masked_load ||
              F->getIntrinsicID() == Intrinsic::masked_store)) {
      unsigned OpOffset = 0;
      if (F->getIntrinsicID() == Intrinsic::masked_store) {
        if (!ClInstrumentWrites)
          return std::nullopt;
        // Masked store has an initial operand for the value.
        OpOffset = 1;
        Access.AccessTy = CI->getArgOperand(0)->getType();
        Access.IsWrite = true;
      } else {
        if (!ClInstrumentReads)
          return std::nullopt;
        Access.AccessTy = CI->getType();
        Access.IsWrite = false;
      }

      auto *BasePtr = CI->getOperand(0 + OpOffset);
      Access.MaybeMask = CI->getOperand(2 + OpOffset);
      Access.Addr = BasePtr;
    }
  }

  if (!Access.Addr)
    return std::nullopt;

  // Do not instrument accesses from different address spaces; we cannot deal
  // with them.
  Type *PtrTy = cast<PointerType>(Access.Addr->getType()->getScalarType());
  if (PtrTy->getPointerAddressSpace() != 0)
    return std::nullopt;

  // Ignore swifterror addresses.
  // swifterror memory addresses are mem2reg promoted by instruction
  // selection. As such they cannot have regular uses like an instrumentation
  // function and it makes no sense to track them as memory.
  if (Access.Addr->isSwiftError())
    return std::nullopt;

  // Peel off GEPs and BitCasts.
  auto *Addr = Access.Addr->stripInBoundsOffsets();

  if (GlobalVariable *GV = dyn_cast<GlobalVariable>(Addr)) {
    // Do not instrument PGO counter updates.
    if (GV->hasSection()) {
      StringRef SectionName = GV->getSection();
      // Check if the global is in the PGO counters section.
      auto OF = I->getModule()->getTargetTriple().getObjectFormat();
      if (SectionName.ends_with(
              getInstrProfSectionName(IPSK_cnts, OF, /*AddSegmentInfo=*/false)))
        return std::nullopt;
    }

    // Do not instrument accesses to LLVM internal variables.
    if (GV->getName().starts_with("__llvm"))
      return std::nullopt;
  }

  return Access;
}

void MemProfiler::instrumentMaskedLoadOrStore(const DataLayout &DL, Value *Mask,
                                              Instruction *I, Value *Addr,
                                              Type *AccessTy, bool IsWrite) {
  auto *VTy = cast<FixedVectorType>(AccessTy);
  unsigned Num = VTy->getNumElements();
  auto *Zero = ConstantInt::get(IntptrTy, 0);
  for (unsigned Idx = 0; Idx < Num; ++Idx) {
    Value *InstrumentedAddress = nullptr;
    Instruction *InsertBefore = I;
    if (auto *Vector = dyn_cast<ConstantVector>(Mask)) {
      // dyn_cast as we might get UndefValue
      if (auto *Masked = dyn_cast<ConstantInt>(Vector->getOperand(Idx))) {
        if (Masked->isZero())
          // Mask is constant false, so no instrumentation needed.
          continue;
        // If we have a true or undef value, fall through to instrumentAddress.
        // with InsertBefore == I
      }
    } else {
      IRBuilder<> IRB(I);
      Value *MaskElem = IRB.CreateExtractElement(Mask, Idx);
      Instruction *ThenTerm = SplitBlockAndInsertIfThen(MaskElem, I, false);
      InsertBefore = ThenTerm;
    }

    IRBuilder<> IRB(InsertBefore);
    InstrumentedAddress =
        IRB.CreateGEP(VTy, Addr, {Zero, ConstantInt::get(IntptrTy, Idx)});
    instrumentAddress(I, InsertBefore, InstrumentedAddress, IsWrite);
  }
}

void MemProfiler::instrumentMop(Instruction *I, const DataLayout &DL,
                                InterestingMemoryAccess &Access) {
  // Skip instrumentation of stack accesses unless requested.
  if (!ClStack && isa<AllocaInst>(getUnderlyingObject(Access.Addr))) {
    if (Access.IsWrite)
      ++NumSkippedStackWrites;
    else
      ++NumSkippedStackReads;
    return;
  }

  if (Access.IsWrite)
    NumInstrumentedWrites++;
  else
    NumInstrumentedReads++;

  if (Access.MaybeMask) {
    instrumentMaskedLoadOrStore(DL, Access.MaybeMask, I, Access.Addr,
                                Access.AccessTy, Access.IsWrite);
  } else {
    // Since the access counts will be accumulated across the entire allocation,
    // we only update the shadow access count for the first location and thus
    // don't need to worry about alignment and type size.
    instrumentAddress(I, I, Access.Addr, Access.IsWrite);
  }
}

void MemProfiler::instrumentAddress(Instruction *OrigIns,
                                    Instruction *InsertBefore, Value *Addr,
                                    bool IsWrite) {
  IRBuilder<> IRB(InsertBefore);
  Value *AddrLong = IRB.CreatePointerCast(Addr, IntptrTy);

  if (ClUseCalls) {
    IRB.CreateCall(MemProfMemoryAccessCallback[IsWrite], AddrLong);
    return;
  }

  Type *ShadowTy = ClHistogram ? Type::getInt8Ty(*C) : Type::getInt64Ty(*C);
  Type *ShadowPtrTy = PointerType::get(*C, 0);

  Value *ShadowPtr = memToShadow(AddrLong, IRB);
  Value *ShadowAddr = IRB.CreateIntToPtr(ShadowPtr, ShadowPtrTy);
  Value *ShadowValue = IRB.CreateLoad(ShadowTy, ShadowAddr);
  // If we are profiling with histograms, add overflow protection at 255.
  if (ClHistogram) {
    Value *MaxCount = ConstantInt::get(Type::getInt8Ty(*C), 255);
    Value *Cmp = IRB.CreateICmpULT(ShadowValue, MaxCount);
    Instruction *IncBlock =
        SplitBlockAndInsertIfThen(Cmp, InsertBefore, /*Unreachable=*/false);
    IRB.SetInsertPoint(IncBlock);
  }
  Value *Inc = ConstantInt::get(ShadowTy, 1);
  ShadowValue = IRB.CreateAdd(ShadowValue, Inc);
  IRB.CreateStore(ShadowValue, ShadowAddr);
}

// Create the variable for the profile file name.
void createProfileFileNameVar(Module &M) {
  const MDString *MemProfFilename =
      dyn_cast_or_null<MDString>(M.getModuleFlag("MemProfProfileFilename"));
  if (!MemProfFilename)
    return;
  assert(!MemProfFilename->getString().empty() &&
         "Unexpected MemProfProfileFilename metadata with empty string");
  Constant *ProfileNameConst = ConstantDataArray::getString(
      M.getContext(), MemProfFilename->getString(), true);
  GlobalVariable *ProfileNameVar = new GlobalVariable(
      M, ProfileNameConst->getType(), /*isConstant=*/true,
      GlobalValue::WeakAnyLinkage, ProfileNameConst, MemProfFilenameVar);
  const Triple &TT = M.getTargetTriple();
  if (TT.supportsCOMDAT()) {
    ProfileNameVar->setLinkage(GlobalValue::ExternalLinkage);
    ProfileNameVar->setComdat(M.getOrInsertComdat(MemProfFilenameVar));
  }
}

// Set MemprofHistogramFlag as a Global veriable in IR. This makes it accessible
// to the runtime, changing shadow count behavior.
void createMemprofHistogramFlagVar(Module &M) {
  const StringRef VarName(MemProfHistogramFlagVar);
  Type *IntTy1 = Type::getInt1Ty(M.getContext());
  auto MemprofHistogramFlag = new GlobalVariable(
      M, IntTy1, true, GlobalValue::WeakAnyLinkage,
      Constant::getIntegerValue(IntTy1, APInt(1, ClHistogram)), VarName);
  const Triple &TT = M.getTargetTriple();
  if (TT.supportsCOMDAT()) {
    MemprofHistogramFlag->setLinkage(GlobalValue::ExternalLinkage);
    MemprofHistogramFlag->setComdat(M.getOrInsertComdat(VarName));
  }
  appendToCompilerUsed(M, MemprofHistogramFlag);
}

void createMemprofDefaultOptionsVar(Module &M) {
  Constant *OptionsConst = ConstantDataArray::getString(
      M.getContext(), MemprofRuntimeDefaultOptions, /*AddNull=*/true);
  GlobalVariable *OptionsVar = new GlobalVariable(
      M, OptionsConst->getType(), /*isConstant=*/true,
      GlobalValue::WeakAnyLinkage, OptionsConst, getMemprofOptionsSymbolName());
  const Triple &TT = M.getTargetTriple();
  if (TT.supportsCOMDAT()) {
    OptionsVar->setLinkage(GlobalValue::ExternalLinkage);
    OptionsVar->setComdat(M.getOrInsertComdat(OptionsVar->getName()));
  }
}

bool ModuleMemProfiler::instrumentModule(Module &M) {

  // Create a module constructor.
  std::string MemProfVersion = std::to_string(LLVM_MEM_PROFILER_VERSION);
  std::string VersionCheckName =
      ClInsertVersionCheck ? (MemProfVersionCheckNamePrefix + MemProfVersion)
                           : "";
  std::tie(MemProfCtorFunction, std::ignore) =
      createSanitizerCtorAndInitFunctions(M, MemProfModuleCtorName,
                                          MemProfInitName, /*InitArgTypes=*/{},
                                          /*InitArgs=*/{}, VersionCheckName);

  const uint64_t Priority = getCtorAndDtorPriority(TargetTriple);
  appendToGlobalCtors(M, MemProfCtorFunction, Priority);

  createProfileFileNameVar(M);

  createMemprofHistogramFlagVar(M);

  createMemprofDefaultOptionsVar(M);

  return true;
}

void MemProfiler::initializeCallbacks(Module &M) {
  IRBuilder<> IRB(*C);

  for (size_t AccessIsWrite = 0; AccessIsWrite <= 1; AccessIsWrite++) {
    const std::string TypeStr = AccessIsWrite ? "store" : "load";
    const std::string HistPrefix = ClHistogram ? "hist_" : "";

    SmallVector<Type *, 2> Args1{1, IntptrTy};
    MemProfMemoryAccessCallback[AccessIsWrite] = M.getOrInsertFunction(
        ClMemoryAccessCallbackPrefix + HistPrefix + TypeStr,
        FunctionType::get(IRB.getVoidTy(), Args1, false));
  }
  MemProfMemmove = M.getOrInsertFunction(
      ClMemoryAccessCallbackPrefix + "memmove", PtrTy, PtrTy, PtrTy, IntptrTy);
  MemProfMemcpy = M.getOrInsertFunction(ClMemoryAccessCallbackPrefix + "memcpy",
                                        PtrTy, PtrTy, PtrTy, IntptrTy);
  MemProfMemset =
      M.getOrInsertFunction(ClMemoryAccessCallbackPrefix + "memset", PtrTy,
                            PtrTy, IRB.getInt32Ty(), IntptrTy);
}

bool MemProfiler::maybeInsertMemProfInitAtFunctionEntry(Function &F) {
  // For each NSObject descendant having a +load method, this method is invoked
  // by the ObjC runtime before any of the static constructors is called.
  // Therefore we need to instrument such methods with a call to __memprof_init
  // at the beginning in order to initialize our runtime before any access to
  // the shadow memory.
  // We cannot just ignore these methods, because they may call other
  // instrumented functions.
  if (F.getName().contains(" load]")) {
    FunctionCallee MemProfInitFunction =
        declareSanitizerInitFunction(*F.getParent(), MemProfInitName, {});
    IRBuilder<> IRB(&F.front(), F.front().begin());
    IRB.CreateCall(MemProfInitFunction, {});
    return true;
  }
  return false;
}

bool MemProfiler::insertDynamicShadowAtFunctionEntry(Function &F) {
  IRBuilder<> IRB(&F.front().front());
  Value *GlobalDynamicAddress = F.getParent()->getOrInsertGlobal(
      MemProfShadowMemoryDynamicAddress, IntptrTy);
  if (F.getParent()->getPICLevel() == PICLevel::NotPIC)
    cast<GlobalVariable>(GlobalDynamicAddress)->setDSOLocal(true);
  DynamicShadowOffset = IRB.CreateLoad(IntptrTy, GlobalDynamicAddress);
  return true;
}

bool MemProfiler::instrumentFunction(Function &F) {
  if (F.getLinkage() == GlobalValue::AvailableExternallyLinkage)
    return false;
  if (ClDebugFunc == F.getName())
    return false;
  if (F.getName().starts_with("__memprof_"))
    return false;

  bool FunctionModified = false;

  // If needed, insert __memprof_init.
  // This function needs to be called even if the function body is not
  // instrumented.
  if (maybeInsertMemProfInitAtFunctionEntry(F))
    FunctionModified = true;

  LLVM_DEBUG(dbgs() << "MEMPROF instrumenting:\n" << F << "\n");

  initializeCallbacks(*F.getParent());

  SmallVector<Instruction *, 16> ToInstrument;

  // Fill the set of memory operations to instrument.
  for (auto &BB : F) {
    for (auto &Inst : BB) {
      if (isInterestingMemoryAccess(&Inst) || isa<MemIntrinsic>(Inst))
        ToInstrument.push_back(&Inst);
    }
  }

  if (ToInstrument.empty()) {
    LLVM_DEBUG(dbgs() << "MEMPROF done instrumenting: " << FunctionModified
                      << " " << F << "\n");

    return FunctionModified;
  }

  FunctionModified |= insertDynamicShadowAtFunctionEntry(F);

  int NumInstrumented = 0;
  for (auto *Inst : ToInstrument) {
    if (ClDebugMin < 0 || ClDebugMax < 0 ||
        (NumInstrumented >= ClDebugMin && NumInstrumented <= ClDebugMax)) {
      std::optional<InterestingMemoryAccess> Access =
          isInterestingMemoryAccess(Inst);
      if (Access)
        instrumentMop(Inst, F.getDataLayout(), *Access);
      else
        instrumentMemIntrinsic(cast<MemIntrinsic>(Inst));
    }
    NumInstrumented++;
  }

  if (NumInstrumented > 0)
    FunctionModified = true;

  LLVM_DEBUG(dbgs() << "MEMPROF done instrumenting: " << FunctionModified << " "
                    << F << "\n");

  return FunctionModified;
}

static void addCallsiteMetadata(Instruction &I,
                                ArrayRef<uint64_t> InlinedCallStack,
                                LLVMContext &Ctx) {
  I.setMetadata(LLVMContext::MD_callsite,
                buildCallstackMetadata(InlinedCallStack, Ctx));
}

static uint64_t computeStackId(GlobalValue::GUID Function, uint32_t LineOffset,
                               uint32_t Column) {
  llvm::HashBuilder<llvm::TruncatedBLAKE3<8>, llvm::endianness::little>
      HashBuilder;
  HashBuilder.add(Function, LineOffset, Column);
  llvm::BLAKE3Result<8> Hash = HashBuilder.final();
  uint64_t Id;
  std::memcpy(&Id, Hash.data(), sizeof(Hash));
  return Id;
}

static uint64_t computeStackId(const memprof::Frame &Frame) {
  return computeStackId(Frame.Function, Frame.LineOffset, Frame.Column);
}

// Helper to generate a single hash id for a given callstack, used for emitting
// matching statistics and useful for uniquing such statistics across modules.
static uint64_t computeFullStackId(ArrayRef<Frame> CallStack) {
  llvm::HashBuilder<llvm::TruncatedBLAKE3<8>, llvm::endianness::little>
      HashBuilder;
  for (auto &F : CallStack)
    HashBuilder.add(F.Function, F.LineOffset, F.Column);
  llvm::BLAKE3Result<8> Hash = HashBuilder.final();
  uint64_t Id;
  std::memcpy(&Id, Hash.data(), sizeof(Hash));
  return Id;
}

static AllocationType addCallStack(CallStackTrie &AllocTrie,
                                   const AllocationInfo *AllocInfo,
                                   uint64_t FullStackId) {
  SmallVector<uint64_t> StackIds;
  for (const auto &StackFrame : AllocInfo->CallStack)
    StackIds.push_back(computeStackId(StackFrame));
  auto AllocType = getAllocType(AllocInfo->Info.getTotalLifetimeAccessDensity(),
                                AllocInfo->Info.getAllocCount(),
                                AllocInfo->Info.getTotalLifetime());
  std::vector<ContextTotalSize> ContextSizeInfo;
  if (recordContextSizeInfo()) {
    auto TotalSize = AllocInfo->Info.getTotalSize();
    assert(TotalSize);
    assert(FullStackId != 0);
    ContextSizeInfo.push_back({FullStackId, TotalSize});
  }
  AllocTrie.addCallStack(AllocType, StackIds, std::move(ContextSizeInfo));
  return AllocType;
}

// Helper to compare the InlinedCallStack computed from an instruction's debug
// info to a list of Frames from profile data (either the allocation data or a
// callsite).
static bool
stackFrameIncludesInlinedCallStack(ArrayRef<Frame> ProfileCallStack,
                                   ArrayRef<uint64_t> InlinedCallStack) {
  return ProfileCallStack.size() >= InlinedCallStack.size() &&
         llvm::equal(ProfileCallStack.take_front(InlinedCallStack.size()),
                     InlinedCallStack, [](const Frame &F, uint64_t StackId) {
                       return computeStackId(F) == StackId;
                     });
}

static bool isAllocationWithHotColdVariant(const Function *Callee,
                                           const TargetLibraryInfo &TLI) {
  if (!Callee)
    return false;
  LibFunc Func;
  if (!TLI.getLibFunc(*Callee, Func))
    return false;
  switch (Func) {
  case LibFunc_Znwm:
  case LibFunc_ZnwmRKSt9nothrow_t:
  case LibFunc_ZnwmSt11align_val_t:
  case LibFunc_ZnwmSt11align_val_tRKSt9nothrow_t:
  case LibFunc_Znam:
  case LibFunc_ZnamRKSt9nothrow_t:
  case LibFunc_ZnamSt11align_val_t:
  case LibFunc_ZnamSt11align_val_tRKSt9nothrow_t:
  case LibFunc_size_returning_new:
  case LibFunc_size_returning_new_aligned:
    return true;
  case LibFunc_Znwm12__hot_cold_t:
  case LibFunc_ZnwmRKSt9nothrow_t12__hot_cold_t:
  case LibFunc_ZnwmSt11align_val_t12__hot_cold_t:
  case LibFunc_ZnwmSt11align_val_tRKSt9nothrow_t12__hot_cold_t:
  case LibFunc_Znam12__hot_cold_t:
  case LibFunc_ZnamRKSt9nothrow_t12__hot_cold_t:
  case LibFunc_ZnamSt11align_val_t12__hot_cold_t:
  case LibFunc_ZnamSt11align_val_tRKSt9nothrow_t12__hot_cold_t:
  case LibFunc_size_returning_new_hot_cold:
  case LibFunc_size_returning_new_aligned_hot_cold:
    return ClMemProfMatchHotColdNew;
  default:
    return false;
  }
}

struct AllocMatchInfo {
  uint64_t TotalSize = 0;
  size_t NumFramesMatched = 0;
  AllocationType AllocType = AllocationType::None;
  bool Matched = false;
};

DenseMap<uint64_t, SmallVector<CallEdgeTy, 0>>
memprof::extractCallsFromIR(Module &M, const TargetLibraryInfo &TLI,
                            function_ref<bool(uint64_t)> IsPresentInProfile) {
  DenseMap<uint64_t, SmallVector<CallEdgeTy, 0>> Calls;

  auto GetOffset = [](const DILocation *DIL) {
    return (DIL->getLine() - DIL->getScope()->getSubprogram()->getLine()) &
           0xffff;
  };

  for (Function &F : M) {
    if (F.isDeclaration())
      continue;

    for (auto &BB : F) {
      for (auto &I : BB) {
        if (!isa<CallBase>(&I) || isa<IntrinsicInst>(&I))
          continue;

        auto *CB = dyn_cast<CallBase>(&I);
        auto *CalledFunction = CB->getCalledFunction();
        // Disregard indirect calls and intrinsics.
        if (!CalledFunction || CalledFunction->isIntrinsic())
          continue;

        StringRef CalleeName = CalledFunction->getName();
        // True if we are calling a heap allocation function that supports
        // hot/cold variants.
        bool IsAlloc = isAllocationWithHotColdVariant(CalledFunction, TLI);
        // True for the first iteration below, indicating that we are looking at
        // a leaf node.
        bool IsLeaf = true;
        for (const DILocation *DIL = I.getDebugLoc(); DIL;
             DIL = DIL->getInlinedAt()) {
          StringRef CallerName = DIL->getSubprogramLinkageName();
          assert(!CallerName.empty() &&
                 "Be sure to enable -fdebug-info-for-profiling");
          uint64_t CallerGUID = memprof::getGUID(CallerName);
          uint64_t CalleeGUID = memprof::getGUID(CalleeName);
          // Pretend that we are calling a function with GUID == 0 if we are
          // in the inline stack leading to a heap allocation function.
          if (IsAlloc) {
            if (IsLeaf) {
              // For leaf nodes, set CalleeGUID to 0 without consulting
              // IsPresentInProfile.
              CalleeGUID = 0;
            } else if (!IsPresentInProfile(CalleeGUID)) {
              // In addition to the leaf case above, continue to set CalleeGUID
              // to 0 as long as we don't see CalleeGUID in the profile.
              CalleeGUID = 0;
            } else {
              // Once we encounter a callee that exists in the profile, stop
              // setting CalleeGUID to 0.
              IsAlloc = false;
            }
          }

          LineLocation Loc = {GetOffset(DIL), DIL->getColumn()};
          Calls[CallerGUID].emplace_back(Loc, CalleeGUID);
          CalleeName = CallerName;
          IsLeaf = false;
        }
      }
    }
  }

  // Sort each call list by the source location.
  for (auto &[CallerGUID, CallList] : Calls) {
    llvm::sort(CallList);
    CallList.erase(llvm::unique(CallList), CallList.end());
  }

  return Calls;
}

DenseMap<uint64_t, LocToLocMap>
memprof::computeUndriftMap(Module &M, IndexedInstrProfReader *MemProfReader,
                           const TargetLibraryInfo &TLI) {
  DenseMap<uint64_t, LocToLocMap> UndriftMaps;

  DenseMap<uint64_t, SmallVector<memprof::CallEdgeTy, 0>> CallsFromProfile =
      MemProfReader->getMemProfCallerCalleePairs();
  DenseMap<uint64_t, SmallVector<memprof::CallEdgeTy, 0>> CallsFromIR =
      extractCallsFromIR(M, TLI, [&](uint64_t GUID) {
        return CallsFromProfile.contains(GUID);
      });

  // Compute an undrift map for each CallerGUID.
  for (const auto &[CallerGUID, IRAnchors] : CallsFromIR) {
    auto It = CallsFromProfile.find(CallerGUID);
    if (It == CallsFromProfile.end())
      continue;
    const auto &ProfileAnchors = It->second;

    LocToLocMap Matchings;
    longestCommonSequence<LineLocation, GlobalValue::GUID>(
        ProfileAnchors, IRAnchors, std::equal_to<GlobalValue::GUID>(),
        [&](LineLocation A, LineLocation B) { Matchings.try_emplace(A, B); });
    [[maybe_unused]] bool Inserted =
        UndriftMaps.try_emplace(CallerGUID, std::move(Matchings)).second;

    // The insertion must succeed because we visit each GUID exactly once.
    assert(Inserted);
  }

  return UndriftMaps;
}

// Given a MemProfRecord, undrift all the source locations present in the
// record in place.
static void
undriftMemProfRecord(const DenseMap<uint64_t, LocToLocMap> &UndriftMaps,
                     memprof::MemProfRecord &MemProfRec) {
  // Undrift a call stack in place.
  auto UndriftCallStack = [&](std::vector<Frame> &CallStack) {
    for (auto &F : CallStack) {
      auto I = UndriftMaps.find(F.Function);
      if (I == UndriftMaps.end())
        continue;
      auto J = I->second.find(LineLocation(F.LineOffset, F.Column));
      if (J == I->second.end())
        continue;
      auto &NewLoc = J->second;
      F.LineOffset = NewLoc.LineOffset;
      F.Column = NewLoc.Column;
    }
  };

  for (auto &AS : MemProfRec.AllocSites)
    UndriftCallStack(AS.CallStack);

  for (auto &CS : MemProfRec.CallSites)
    UndriftCallStack(CS.Frames);
}

static void
readMemprof(Module &M, Function &F, IndexedInstrProfReader *MemProfReader,
            const TargetLibraryInfo &TLI,
            std::map<uint64_t, AllocMatchInfo> &FullStackIdToAllocMatchInfo,
            std::set<std::vector<uint64_t>> &MatchedCallSites,
            DenseMap<uint64_t, LocToLocMap> &UndriftMaps) {
  auto &Ctx = M.getContext();
  // Previously we used getIRPGOFuncName() here. If F is local linkage,
  // getIRPGOFuncName() returns FuncName with prefix 'FileName;'. But
  // llvm-profdata uses FuncName in dwarf to create GUID which doesn't
  // contain FileName's prefix. It caused local linkage function can't
  // find MemProfRecord. So we use getName() now.
  // 'unique-internal-linkage-names' can make MemProf work better for local
  // linkage function.
  auto FuncName = F.getName();
  auto FuncGUID = Function::getGUIDAssumingExternalLinkage(FuncName);
  std::optional<memprof::MemProfRecord> MemProfRec;
  auto Err = MemProfReader->getMemProfRecord(FuncGUID).moveInto(MemProfRec);
  if (Err) {
    handleAllErrors(std::move(Err), [&](const InstrProfError &IPE) {
      auto Err = IPE.get();
      bool SkipWarning = false;
      LLVM_DEBUG(dbgs() << "Error in reading profile for Func " << FuncName
                        << ": ");
      if (Err == instrprof_error::unknown_function) {
        NumOfMemProfMissing++;
        SkipWarning = !PGOWarnMissing;
        LLVM_DEBUG(dbgs() << "unknown function");
      } else if (Err == instrprof_error::hash_mismatch) {
        NumOfMemProfMismatch++;
        SkipWarning =
            NoPGOWarnMismatch ||
            (NoPGOWarnMismatchComdatWeak &&
             (F.hasComdat() ||
              F.getLinkage() == GlobalValue::AvailableExternallyLinkage));
        LLVM_DEBUG(dbgs() << "hash mismatch (skip=" << SkipWarning << ")");
      }

      if (SkipWarning)
        return;

      std::string Msg = (IPE.message() + Twine(" ") + F.getName().str() +
                         Twine(" Hash = ") + std::to_string(FuncGUID))
                            .str();

      Ctx.diagnose(
          DiagnosticInfoPGOProfile(M.getName().data(), Msg, DS_Warning));
    });
    return;
  }

  NumOfMemProfFunc++;

  // If requested, undrfit MemProfRecord so that the source locations in it
  // match those in the IR.
  if (SalvageStaleProfile)
    undriftMemProfRecord(UndriftMaps, *MemProfRec);

  // Detect if there are non-zero column numbers in the profile. If not,
  // treat all column numbers as 0 when matching (i.e. ignore any non-zero
  // columns in the IR). The profiled binary might have been built with
  // column numbers disabled, for example.
  bool ProfileHasColumns = false;

  // Build maps of the location hash to all profile data with that leaf location
  // (allocation info and the callsites).
  std::map<uint64_t, std::set<const AllocationInfo *>> LocHashToAllocInfo;
  // A hash function for std::unordered_set<ArrayRef<Frame>> to work.
  struct CallStackHash {
    size_t operator()(ArrayRef<Frame> CS) const {
      return computeFullStackId(CS);
    }
  };
  // For the callsites we need to record slices of the frame array (see comments
  // below where the map entries are added).
  std::map<uint64_t, std::unordered_set<ArrayRef<Frame>, CallStackHash>>
      LocHashToCallSites;
  for (auto &AI : MemProfRec->AllocSites) {
    NumOfMemProfAllocContextProfiles++;
    // Associate the allocation info with the leaf frame. The later matching
    // code will match any inlined call sequences in the IR with a longer prefix
    // of call stack frames.
    uint64_t StackId = computeStackId(AI.CallStack[0]);
    LocHashToAllocInfo[StackId].insert(&AI);
    ProfileHasColumns |= AI.CallStack[0].Column;
  }
  for (auto &CS : MemProfRec->CallSites) {
    NumOfMemProfCallSiteProfiles++;
    // Need to record all frames from leaf up to and including this function,
    // as any of these may or may not have been inlined at this point.
    unsigned Idx = 0;
    for (auto &StackFrame : CS.Frames) {
      uint64_t StackId = computeStackId(StackFrame);
      LocHashToCallSites[StackId].insert(
          ArrayRef<Frame>(CS.Frames).drop_front(Idx++));
      ProfileHasColumns |= StackFrame.Column;
      // Once we find this function, we can stop recording.
      if (StackFrame.Function == FuncGUID)
        break;
    }
    assert(Idx <= CS.Frames.size() && CS.Frames[Idx - 1].Function == FuncGUID);
  }

  auto GetOffset = [](const DILocation *DIL) {
    return (DIL->getLine() - DIL->getScope()->getSubprogram()->getLine()) &
           0xffff;
  };

  // Now walk the instructions, looking up the associated profile data using
  // debug locations.
  for (auto &BB : F) {
    for (auto &I : BB) {
      if (I.isDebugOrPseudoInst())
        continue;
      // We are only interested in calls (allocation or interior call stack
      // context calls).
      auto *CI = dyn_cast<CallBase>(&I);
      if (!CI)
        continue;
      auto *CalledFunction = CI->getCalledFunction();
      if (CalledFunction && CalledFunction->isIntrinsic())
        continue;
      // List of call stack ids computed from the location hashes on debug
      // locations (leaf to inlined at root).
      SmallVector<uint64_t, 8> InlinedCallStack;
      // Was the leaf location found in one of the profile maps?
      bool LeafFound = false;
      // If leaf was found in a map, iterators pointing to its location in both
      // of the maps. It might exist in neither, one, or both (the latter case
      // can happen because we don't currently have discriminators to
      // distinguish the case when a single line/col maps to both an allocation
      // and another callsite).
      auto AllocInfoIter = LocHashToAllocInfo.end();
      auto CallSitesIter = LocHashToCallSites.end();
      for (const DILocation *DIL = I.getDebugLoc(); DIL != nullptr;
           DIL = DIL->getInlinedAt()) {
        // Use C++ linkage name if possible. Need to compile with
        // -fdebug-info-for-profiling to get linkage name.
        StringRef Name = DIL->getScope()->getSubprogram()->getLinkageName();
        if (Name.empty())
          Name = DIL->getScope()->getSubprogram()->getName();
        auto CalleeGUID = Function::getGUIDAssumingExternalLinkage(Name);
        auto StackId = computeStackId(CalleeGUID, GetOffset(DIL),
                                      ProfileHasColumns ? DIL->getColumn() : 0);
        // Check if we have found the profile's leaf frame. If yes, collect
        // the rest of the call's inlined context starting here. If not, see if
        // we find a match further up the inlined context (in case the profile
        // was missing debug frames at the leaf).
        if (!LeafFound) {
          AllocInfoIter = LocHashToAllocInfo.find(StackId);
          CallSitesIter = LocHashToCallSites.find(StackId);
          if (AllocInfoIter != LocHashToAllocInfo.end() ||
              CallSitesIter != LocHashToCallSites.end())
            LeafFound = true;
        }
        if (LeafFound)
          InlinedCallStack.push_back(StackId);
      }
      // If leaf not in either of the maps, skip inst.
      if (!LeafFound)
        continue;

      // First add !memprof metadata from allocation info, if we found the
      // instruction's leaf location in that map, and if the rest of the
      // instruction's locations match the prefix Frame locations on an
      // allocation context with the same leaf.
      if (AllocInfoIter != LocHashToAllocInfo.end() &&
          // Only consider allocations which support hinting.
          isAllocationWithHotColdVariant(CI->getCalledFunction(), TLI)) {
        // We may match this instruction's location list to multiple MIB
        // contexts. Add them to a Trie specialized for trimming the contexts to
        // the minimal needed to disambiguate contexts with unique behavior.
        CallStackTrie AllocTrie;
        uint64_t TotalSize = 0;
        uint64_t TotalColdSize = 0;
        for (auto *AllocInfo : AllocInfoIter->second) {
          // Check the full inlined call stack against this one.
          // If we found and thus matched all frames on the call, include
          // this MIB.
          if (stackFrameIncludesInlinedCallStack(AllocInfo->CallStack,
                                                 InlinedCallStack)) {
            NumOfMemProfMatchedAllocContexts++;
            uint64_t FullStackId = 0;
            if (ClPrintMemProfMatchInfo || recordContextSizeInfo())
              FullStackId = computeFullStackId(AllocInfo->CallStack);
            auto AllocType = addCallStack(AllocTrie, AllocInfo, FullStackId);
            TotalSize += AllocInfo->Info.getTotalSize();
            if (AllocType == AllocationType::Cold)
              TotalColdSize += AllocInfo->Info.getTotalSize();
            // Record information about the allocation if match info printing
            // was requested.
            if (ClPrintMemProfMatchInfo) {
              assert(FullStackId != 0);
              FullStackIdToAllocMatchInfo[FullStackId] = {
                  AllocInfo->Info.getTotalSize(), InlinedCallStack.size(),
                  AllocType, /*Matched=*/true};
            }
          }
        }
        // If the threshold for the percent of cold bytes is less than 100%,
        // and not all bytes are cold, see if we should still hint this
        // allocation as cold without context sensitivity.
        if (TotalColdSize < TotalSize && MinMatchedColdBytePercent < 100 &&
            TotalColdSize * 100 >= MinMatchedColdBytePercent * TotalSize) {
          AllocTrie.addSingleAllocTypeAttribute(CI, AllocationType::Cold,
                                                "dominant");
          continue;
        }

        // We might not have matched any to the full inlined call stack.
        // But if we did, create and attach metadata, or a function attribute if
        // all contexts have identical profiled behavior.
        if (!AllocTrie.empty()) {
          NumOfMemProfMatchedAllocs++;
          // MemprofMDAttached will be false if a function attribute was
          // attached.
          bool MemprofMDAttached = AllocTrie.buildAndAttachMIBMetadata(CI);
          assert(MemprofMDAttached == I.hasMetadata(LLVMContext::MD_memprof));
          if (MemprofMDAttached) {
            // Add callsite metadata for the instruction's location list so that
            // it simpler later on to identify which part of the MIB contexts
            // are from this particular instruction (including during inlining,
            // when the callsite metadata will be updated appropriately).
            // FIXME: can this be changed to strip out the matching stack
            // context ids from the MIB contexts and not add any callsite
            // metadata here to save space?
            addCallsiteMetadata(I, InlinedCallStack, Ctx);
          }
        }
        continue;
      }

      if (CallSitesIter == LocHashToCallSites.end())
        continue;

      // Otherwise, add callsite metadata. If we reach here then we found the
      // instruction's leaf location in the callsites map and not the allocation
      // map.
      for (auto CallStackIdx : CallSitesIter->second) {
        // If we found and thus matched all frames on the call, create and
        // attach call stack metadata.
        if (stackFrameIncludesInlinedCallStack(CallStackIdx,
                                               InlinedCallStack)) {
          NumOfMemProfMatchedCallSites++;
          addCallsiteMetadata(I, InlinedCallStack, Ctx);
          // Only need to find one with a matching call stack and add a single
          // callsite metadata.

          // Accumulate call site matching information upon request.
          if (ClPrintMemProfMatchInfo) {
            std::vector<uint64_t> CallStack;
            append_range(CallStack, InlinedCallStack);
            MatchedCallSites.insert(std::move(CallStack));
          }
          break;
        }
      }
    }
  }
}

MemProfUsePass::MemProfUsePass(std::string MemoryProfileFile,
                               IntrusiveRefCntPtr<vfs::FileSystem> FS)
    : MemoryProfileFileName(MemoryProfileFile), FS(FS) {
  if (!FS)
    this->FS = vfs::getRealFileSystem();
}

PreservedAnalyses MemProfUsePass::run(Module &M, ModuleAnalysisManager &AM) {
  // Return immediately if the module doesn't contain any function.
  if (M.empty())
    return PreservedAnalyses::all();

  LLVM_DEBUG(dbgs() << "Read in memory profile:");
  auto &Ctx = M.getContext();
  auto ReaderOrErr = IndexedInstrProfReader::create(MemoryProfileFileName, *FS);
  if (Error E = ReaderOrErr.takeError()) {
    handleAllErrors(std::move(E), [&](const ErrorInfoBase &EI) {
      Ctx.diagnose(
          DiagnosticInfoPGOProfile(MemoryProfileFileName.data(), EI.message()));
    });
    return PreservedAnalyses::all();
  }

  std::unique_ptr<IndexedInstrProfReader> MemProfReader =
      std::move(ReaderOrErr.get());
  if (!MemProfReader) {
    Ctx.diagnose(DiagnosticInfoPGOProfile(
        MemoryProfileFileName.data(), StringRef("Cannot get MemProfReader")));
    return PreservedAnalyses::all();
  }

  if (!MemProfReader->hasMemoryProfile()) {
    Ctx.diagnose(DiagnosticInfoPGOProfile(MemoryProfileFileName.data(),
                                          "Not a memory profile"));
    return PreservedAnalyses::all();
  }

  auto &FAM = AM.getResult<FunctionAnalysisManagerModuleProxy>(M).getManager();

  TargetLibraryInfo &TLI = FAM.getResult<TargetLibraryAnalysis>(*M.begin());
  DenseMap<uint64_t, LocToLocMap> UndriftMaps;
  if (SalvageStaleProfile)
    UndriftMaps = computeUndriftMap(M, MemProfReader.get(), TLI);

  // Map from the stack has of each allocation context in the function profiles
  // to the total profiled size (bytes), allocation type, and whether we matched
  // it to an allocation in the IR.
  std::map<uint64_t, AllocMatchInfo> FullStackIdToAllocMatchInfo;

  // Set of the matched call sites, each expressed as a sequence of an inline
  // call stack.
  std::set<std::vector<uint64_t>> MatchedCallSites;

  for (auto &F : M) {
    if (F.isDeclaration())
      continue;

    const TargetLibraryInfo &TLI = FAM.getResult<TargetLibraryAnalysis>(F);
    readMemprof(M, F, MemProfReader.get(), TLI, FullStackIdToAllocMatchInfo,
                MatchedCallSites, UndriftMaps);
  }

  if (ClPrintMemProfMatchInfo) {
    for (const auto &[Id, Info] : FullStackIdToAllocMatchInfo)
      errs() << "MemProf " << getAllocTypeAttributeString(Info.AllocType)
             << " context with id " << Id << " has total profiled size "
             << Info.TotalSize << (Info.Matched ? " is" : " not")
             << " matched with " << Info.NumFramesMatched << " frames\n";

    for (const auto &CallStack : MatchedCallSites) {
      errs() << "MemProf callsite match for inline call stack";
      for (uint64_t StackId : CallStack)
        errs() << " " << StackId;
      errs() << "\n";
    }
  }

  return PreservedAnalyses::none();
}

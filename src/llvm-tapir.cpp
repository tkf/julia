// This file is a part of Julia. License is MIT: https://julialang.org/license
#define DEBUG_TYPE "julia_tapir"

#include "llvm-version.h"

#include <llvm-c/Core.h>
#include <llvm-c/Types.h>

#include <llvm/IR/Value.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>
#ifdef JL_DEBUG_BUILD
#include <llvm/IR/Verifier.h>
#endif
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Tapir/LoweringUtils.h"
#include "llvm/Analysis/TapirTaskInfo.h"
#include <llvm/Support/Debug.h>

#include "codegen_shared.h"
#include "julia.h"
#include "julia_internal.h"
#include "jitlayers.h"
#include "llvm-pass-helpers.h"

/**
 * JuliaTapir lowers Tapir constructs through outlining to Julia Task's.
 * After lowering the code should be equivalent to the Julia code below:
 *
 * ```julia
 *   llvmf = ... # outlined function
 *   tasklist = Task[]
 *   t = Task(llvmf)
 *   push!(tasklist, t)
 *   schedule(t)
 *   sync_end(tasklist)
 * ```
 **/

namespace llvm {

class JuliaTapir : public TapirTarget, private JuliaPassContext {
    ValueToValueMapTy DetachBlockToTaskGroup;
    ValueToValueMapTy SyncRegionToTaskGroup;
    Type *SpawnFTy = nullptr;

    // Opaque Julia runtime functions
    FunctionCallee JlTapirTaskGroup;
    FunctionCallee JlTapirSpawn;
    FunctionCallee JlTapirSync;

    // Accessors for opaque Julia runtime functions
    FunctionCallee get_jl_tapir_taskgroup();
    FunctionCallee get_jl_tapir_spawn();
    FunctionCallee get_jl_tapir_sync();

    void replaceDecayedPointerInArgStruct(TaskOutlineInfo &);
    void replaceDecayedPointerInOutline(TaskOutlineInfo &);

    void insertGCPreserve(Function &);
    void insertPtls(Function &);

    public:
        JuliaTapir(Module &M);
        ~JuliaTapir() {}

        ArgStructMode getArgStructMode() const override final {
            return ArgStructMode::Static;
            // return ArgStructMode::Dynamic;
        }

        Value *lowerGrainsizeCall(CallInst *GrainsizeCall) override final;
        void lowerSync(SyncInst &inst) override final;

        void preProcessFunction(Function &F, TaskInfo &TI,
                                bool OutliningTapirLoops) override final;
        void postProcessFunction(Function &F, bool OutliningTapirLoops) override final;
        void postProcessHelper(Function &F) override final;

        void preProcessOutlinedTask(Function &F, Instruction *DetachPt,
                                    Instruction *TaskFrameCreate,
                                    bool IsSpawner) override final;
        void postProcessOutlinedTask(Function &F, Instruction *DetachPt,
                                     Instruction *TaskFrameCreate,
                                     bool IsSpawner) override final;
        void preProcessRootSpawner(Function &F) override final;
        void postProcessRootSpawner(Function &F) override final;
        void processSubTaskCall(TaskOutlineInfo &TOI, DominatorTree &DT)
            override final;
};

JuliaTapir::JuliaTapir(Module &M) : TapirTarget(M) {
    LLVMContext &C = M.getContext();
    // Initialize any types we need for lowering.
    SpawnFTy = PointerType::getUnqual(
        FunctionType::get(Type::getVoidTy(C), { Type::getInt8PtrTy(C) }, false));

    initAll(M);

    // Ideally, we can rely on `initAll(M)` to set up all runtime functions.
    // However, it does not set up the functions; i.e., it relies on that they
    // are created in `emit_function`. Since we are adding *new* GC roots, we
    // need to manually make sure that these functions are set up, even though
    // these functions are not inserted in the emission phase. The function
    // signatures have to be matched with the ones in `codegen.cpp` (see
    // `gc_preserve_begin_func = new JuliaFunction{...}` etc.).
    gc_preserve_begin_func = cast<Function>(M.getOrInsertFunction(
        "llvm.julia.gc_preserve_begin",
        FunctionType::get(Type::getTokenTy(C), true)).getCallee());
    gc_preserve_end_func = cast<Function>(M.getOrInsertFunction(
        "llvm.julia.gc_preserve_end",
        FunctionType::get(Type::getVoidTy(C), {Type::getTokenTy(C)}, false)).getCallee());
}

FunctionCallee JuliaTapir::get_jl_tapir_taskgroup() {
    if (JlTapirTaskGroup)
        return JlTapirTaskGroup;

    AttributeList AL;
    FunctionType *FTy = FunctionType::get(T_prjlvalue, {}, false);

    JlTapirTaskGroup = M.getOrInsertFunction("jl_tapir_taskgroup", FTy, AL);
    return JlTapirTaskGroup;
}

FunctionCallee JuliaTapir::get_jl_tapir_spawn() {
    if (JlTapirSpawn)
        return JlTapirSpawn;

    LLVMContext &C = M.getContext();
    const DataLayout &DL = M.getDataLayout();
    AttributeList AL;
    FunctionType *FTy = FunctionType::get(
        Type::getVoidTy(C),
        {
            T_prjlvalue,           // jl_value_t *tasks
            SpawnFTy,              // void *f
            Type::getInt8PtrTy(C), // void *arg
            DL.getIntPtrType(C),   // size_t arg_size
        },
        false);

    JlTapirSpawn = M.getOrInsertFunction("jl_tapir_spawn", FTy, AL);
    return JlTapirSpawn;
}

FunctionCallee JuliaTapir::get_jl_tapir_sync() {
    if (JlTapirSync)
        return JlTapirSync;

    LLVMContext &C = M.getContext();
    AttributeList AL;
    FunctionType *FTy = FunctionType::get(Type::getVoidTy(C), {T_prjlvalue}, false);

    JlTapirSync = M.getOrInsertFunction("jl_tapir_sync", FTy, AL);
    return JlTapirSync;
}

Value *JuliaTapir::lowerGrainsizeCall(CallInst *GrainsizeCall) {
    Value *Limit = GrainsizeCall->getArgOperand(0);
    Module *M = GrainsizeCall->getModule();
    IRBuilder<> Builder(GrainsizeCall);

    // get jl_n_threads (extern global variable)
    Constant *proto = M->getOrInsertGlobal("jl_n_threads", Type::getInt32Ty(M->getContext()));

    Value *Workers = Builder.CreateLoad(proto);

    // Choose 8xWorkers as grainsize
    Value *WorkersX8 = Builder.CreateIntCast(
        Builder.CreateMul(Workers, ConstantInt::get(Workers->getType(), 8)),
        Limit->getType(), false);

    // Compute ceil(limit / 8 * workers) =
    //           (limit + 8 * workers - 1) / (8 * workers)
    Value *SmallLoopVal =
      Builder.CreateUDiv(Builder.CreateSub(Builder.CreateAdd(Limit, WorkersX8),
                                           ConstantInt::get(Limit->getType(), 1)),
                         WorkersX8);
    // Compute min
    Value *LargeLoopVal = ConstantInt::get(Limit->getType(), 2048);
    Value *Cmp = Builder.CreateICmpULT(LargeLoopVal, SmallLoopVal);
    Value *Grainsize = Builder.CreateSelect(Cmp, LargeLoopVal, SmallLoopVal);

    // Replace uses of grainsize intrinsic call with this grainsize value.
    GrainsizeCall->replaceAllUsesWith(Grainsize);
    return Grainsize;
}

void JuliaTapir::lowerSync(SyncInst &SI) {
    IRBuilder<> builder(&SI);
    Value* SR = SI.getSyncRegion();
    auto TG = SyncRegionToTaskGroup[SR];
    builder.CreateCall(get_jl_tapir_sync(), {TG});
    BranchInst *PostSync = BranchInst::Create(SI.getSuccessor(0));
    ReplaceInstWithInst(&SI, PostSync);
}

void JuliaTapir::preProcessFunction(Function &F, TaskInfo &TI, bool OutliningTapirLoops) {
    if (OutliningTapirLoops) // TODO: figure out if we need to do something
        return;

    for (Task *T : post_order(TI.getRootTask())) {
        if (T->isRootTask())
            continue;
        DetachInst *Detach = T->getDetach();
        BasicBlock *detB = Detach->getParent();
        Value *SR = Detach->getSyncRegion();

        // Sync regions and task groups are one-to-one. However, since multiple
        // detach instructions can be invoked in a single sync region, we check
        // if a corresponding task group is created.
        Value *TG = SyncRegionToTaskGroup[SR];
        if (!TG) {
            // Create a taskgroup for each SyncRegion by calling
            // `jl_tapir_taskgroup` at the beggining of the function.:
            TG = CallInst::Create(
                get_jl_tapir_taskgroup(),
                {},
                "",
                F.getEntryBlock().getTerminator());
            SyncRegionToTaskGroup[SR] = TG;
        }
        // TODO: don't look up the map twice
        if (!DetachBlockToTaskGroup[detB]) {
            DetachBlockToTaskGroup[detB] = TG;
        }
    }
}

void JuliaTapir::postProcessFunction(Function &F, bool OutliningTapirLoops) {
    // nothing
}

void JuliaTapir::postProcessHelper(Function &F) {
    // nothing
}

void JuliaTapir::preProcessOutlinedTask(Function &F, Instruction *DetachPt,
                                        Instruction *TaskFrameCreate,
                                        bool IsSpawner) {
    // nothing
}

void JuliaTapir::insertGCPreserve(Function &F) {
    for (BasicBlock &BB : F) {
        for (Instruction &I : BB) {
            if (CallInst *TG = dyn_cast<CallInst>(&I)) {
                if (TG->getCalledFunction() == get_jl_tapir_taskgroup().getCallee()) {
                    // "Put `TG` in `GC.@preserve`":
                    Value *gctoken = CallInst::Create(
                        gc_preserve_begin_func,
                        {TG},
                        "",
                        BB.getTerminator());
                    // Make sure we "close `GC.@preserve`":
                    for (BasicBlock &BB2 : F) {
                        if (isa<ReturnInst>(BB2.getTerminator())) {
                            CallInst::Create(gc_preserve_end_func, {gctoken}, "", BB2.getTerminator());
                        }
                    }
                }
            }
        }
    }
}

void JuliaTapir::insertPtls(Function &F) {
    if (getPtls(F))
        return;
    // Do what `allocate_gc_frame` (`codegen.cpp`) does:
    CallInst::Create(
        ptls_getter,
        {},
        "",
        F.getEntryBlock().getFirstNonPHI());
    assert(getPtls(F));
    // TODO: other things in `allocate_gc_frame`
}

void JuliaTapir::postProcessOutlinedTask(Function &F, Instruction *DetachPt,
                                         Instruction *TaskFrameCreate,
                                         bool IsSpawner) {
    insertGCPreserve(F);
    insertPtls(F);
}

void JuliaTapir::preProcessRootSpawner(Function &F) {
    // nothing
}

void JuliaTapir::postProcessRootSpawner(Function &F) {
    insertGCPreserve(F);
    insertPtls(F);
}

// Extract out the index value used by GEP created in LLVM/Tapir's
// `createTaskArgsStruct`:
uint64_t structFieldIndex(GetElementPtrInst *GEP) {
    auto LastIdx = cast<ConstantInt>(*(GEP->op_end()-1));
    return LastIdx->getValue().getLimitedValue();
}

// Replace decayed pointers in the argument struct with `AddressSpace::Generic`.
// See: `GCInvariantVerifier::visitStoreInst`.
void JuliaTapir::replaceDecayedPointerInArgStruct(TaskOutlineInfo &TOI) {
    LLVMContext &C = M.getContext();
    CallBase *ReplCall = cast<CallBase>(TOI.ReplCall);
    AllocaInst *CallerArgStruct = cast<AllocaInst>(ReplCall->getArgOperand(0));
    BasicBlock *CallBlock = TOI.ReplStart->getParent();
    for (Instruction &V: *CallBlock) {
        auto Store = dyn_cast<StoreInst>(&V);
        if (!Store)
            continue;
        auto GEP = dyn_cast<GetElementPtrInst>(Store->getOperand(1));
        if (!(GEP && GEP->getPointerOperand() == CallerArgStruct))
            continue;
        auto T_int1 = Type::getInt1Ty(C);
        auto MD = MDNode::get(C, ConstantAsMetadata::get(ConstantInt::get(T_int1, true)));
        V.setMetadata("julia.tapir.store", MD);
    }

    replaceDecayedPointerInOutline(TOI);
}

static PointerType *needAddrSpaceCast(Type *T) {
    auto PT = dyn_cast<PointerType>(T);
    if (PT && (PT->getAddressSpace() == AddressSpace::CalleeRooted ||
               PT->getAddressSpace() == AddressSpace::Derived)) {
        return PT;
    } else {
        return nullptr;
    }
}

static PointerType *castTypeForGC(PointerType *T) {
    return PointerType::get(T->getElementType(), AddressSpace::Tracked);
}

// Replace the address space of fields of the argument struct.
void JuliaTapir::replaceDecayedPointerInOutline(TaskOutlineInfo &TOI) {
    LLVMContext &C = M.getContext();
    Function *F = TOI.Outline;
    FunctionType *FTy = F->getFunctionType();
    assert(FTy->getNumParams() == 1);
    auto STy = cast<StructType>(cast<PointerType>(FTy->getParamType(0))->getElementType());

    bool need_change = false;
    SmallVector<Type*, 8> FieldTypes;
    auto NumFields = STy->getStructNumElements();
    FieldTypes.resize(NumFields);
    for (size_t i = 0; i < NumFields; i++) {
        auto T0 = STy->getStructElementType(i);
        if (auto PT0 = needAddrSpaceCast(T0)) {
            FieldTypes[i] = castTypeForGC(PT0);
            need_change = true;
        } else {
            FieldTypes[i] = T0;
        }
    }
    StructType *NSTy = StructType::create(C, FieldTypes);
    PointerType *NPTy = PointerType::getUnqual(NSTy);
    FunctionType *NFTy = FunctionType::get(FTy->getReturnType(), {NPTy}, FTy->isVarArg());

    // TODO: enable this
    // if (!need_change)
    //     return;

    Function *NF = Function::Create(NFTy, F->getLinkage(), F->getAddressSpace(),
                                    // F->getName() + ".jltapir",
                                    F->getName(), // maybe add suffix?
                                    F->getParent());
    NF->copyAttributesFrom(F);
    NF->setComdat(F->getComdat());
    // Note: For an example of code changing function, see:
    // llvm/lib/Transforms/IPO/DeadArgumentElimination.cpp

    // Copy function body
    NF->getBasicBlockList().splice(NF->begin(), F->getBasicBlockList());
    Argument *NArg = NF->args().begin();
    Argument *Arg = F->args().begin();
    Arg->replaceAllUsesWith(NArg);
    NArg->setName(Arg->getName());

    SmallVector<GetElementPtrInst*, 8> GEPs;
    for (Value *I: NArg->users()) {
        auto GEP = dyn_cast<GetElementPtrInst>(I);
        if (GEP && needAddrSpaceCast(GEP->getResultElementType()))
            GEPs.push_back(GEP);
    }
    for (auto GEP: GEPs) {
        // Extract IdxList from GEP:
        SmallVector<Value*, 8> IdxList;
        IdxList.reserve(GEP->getNumIndices());
        for (size_t i = 0; i < GEP->getNumIndices(); i++) {
            IdxList.push_back(GEP->getOperand(i + 1));
        }

        auto NGEP = GetElementPtrInst::Create(
            NSTy,
            GEP->getPointerOperand(),
            IdxList,
            GEP->getName(),
            GEP);
        assert(NSTy->getTypeAtIndex(structFieldIndex(GEP)) == NGEP->getResultElementType());
        assert(!needAddrSpaceCast(NGEP->getResultElementType()));
        NGEP->copyMetadata(*GEP);
        GEP->replaceAllUsesWith(NGEP);
        GEP->eraseFromParent();

        // Load as generic and then decay to the actually used address space.
        // First creating a copy `GEPUsers` as we are going to mutate the instructions.
        // ASK: Is this required?
        auto GEPUsers = SmallVector<Value*, 8>(NGEP->users());
        for (Value *I: GEPUsers) {
            auto LI = dyn_cast<LoadInst>(I);
            if (!LI)
                continue;
            auto NLI = new LoadInst(NGEP->getResultElementType(), NGEP, "redecay.tmp",
                                    LI->isVolatile(), LI->getAlignment(), LI->getOrdering(),
                                    LI->getSyncScopeID(), LI);
            auto Decay = BitCastInst::Create(Instruction::AddrSpaceCast, NLI, LI->getType(),
                                             "redecay", LI);
            NLI->copyMetadata(*LI);
            LI->replaceAllUsesWith(Decay);
            LI->eraseFromParent();
            assert(!needAddrSpaceCast(NLI->getType()));
        }
    }
#ifdef JL_DEBUG_BUILD
    for (BasicBlock &BB: *NF) {
        for (Instruction &I: BB) {
            auto Load = dyn_cast<LoadInst>(&I);
            if (!Load)
                continue;
            assert(!needAddrSpaceCast(Load->getType()));
        }
    }
#endif

    F->eraseFromParent();
    TOI.Outline = NF;
#ifdef JL_DEBUG_BUILD
    assert(!verifyFunction(*NF, &dbgs()));
#endif
}

// Based on QthreadsABI
void JuliaTapir::processSubTaskCall(TaskOutlineInfo &TOI, DominatorTree &DT) {
    replaceDecayedPointerInArgStruct(TOI);

    Function *Outlined = TOI.Outline;
    Instruction *ReplStart = TOI.ReplStart;
    CallBase *ReplCall = cast<CallBase>(TOI.ReplCall);
    BasicBlock *CallBlock = ReplStart->getParent();

    LLVMContext &C = M.getContext();
    const DataLayout &DL = M.getDataLayout();

    // At this point, we have a call in the parent to a function containing the
    // task body.  That function takes as its argument a pointer to a structure
    // containing the inputs to the task body.  This structure is initialized in
    // the parent immediately before the call.

    // Construct a call to jl_tapir_spawn:
    IRBuilder<> CallerIRBuilder(ReplCall);
    Value *OutlinedFnPtr = CallerIRBuilder.CreatePointerBitCastOrAddrSpaceCast(
        Outlined, SpawnFTy);
    AllocaInst *CallerArgStruct = cast<AllocaInst>(ReplCall->getArgOperand(0));
    Type *ArgsTy = CallerArgStruct->getAllocatedType();
    Value *ArgStructPtr = CallerIRBuilder.CreateBitCast(CallerArgStruct,
                                                        Type::getInt8PtrTy(C));
    ConstantInt *ArgSize = ConstantInt::get(DL.getIntPtrType(C),
                                            DL.getTypeAllocSize(ArgsTy));

    // Get the task group handle associatated with this detach instruction.
    // (NOTE: Since detach instruction is a terminator, we can use the basic
    // block containing it to identify the detach.)
    // TODO: Do I have access to task group inside nested tasks?
    // TODO: (If so, when are unneeded task groups cleaned up?)
    Value *TaskGroupPtr = DetachBlockToTaskGroup[TOI.ReplCall->getParent()];

    CallInst *Call = CallerIRBuilder.CreateCall(
        get_jl_tapir_spawn(),
        {
            TaskGroupPtr,  // jl_value_t *tasks
            OutlinedFnPtr, // void *f
            ArgStructPtr,  // void *arg
            ArgSize,       // size_t arg_size
        });
    Call->setDebugLoc(ReplCall->getDebugLoc());
    TOI.replaceReplCall(Call);
    ReplCall->eraseFromParent();

    CallerIRBuilder.SetInsertPoint(Call);
    CallerIRBuilder.CreateLifetimeStart(CallerArgStruct, ArgSize);
    CallerIRBuilder.SetInsertPoint(CallBlock, ++Call->getIterator());
    CallerIRBuilder.CreateLifetimeEnd(CallerArgStruct, ArgSize);

    if (TOI.ReplUnwind)
        // TODO: Copied from Qthread; do we still need this?
        BranchInst::Create(TOI.ReplRet, CallBlock);
}

} // namespace LLVM

llvm::TapirTarget *jl_tapir_target_factory(llvm::Module &M) {
    return new llvm::JuliaTapir(M);
}

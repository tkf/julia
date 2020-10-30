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

void JuliaTapir::postProcessOutlinedTask(Function &F, Instruction *DetachPt,
                                         Instruction *TaskFrameCreate,
                                         bool IsSpawner) {
    // nothing
}

void JuliaTapir::preProcessRootSpawner(Function &F) {
    // nothing
}

void JuliaTapir::postProcessRootSpawner(Function &F) {
    bool need_gc_frame = false;
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
                    need_gc_frame = true;
                }
            }
        }
    }
    if (need_gc_frame && !getPtls(F)) {
        // Do what `allocate_gc_frame` (`codegen.cpp`) does:
        CallInst::Create(
            ptls_getter,
            {},
            "",
            F.getEntryBlock().getFirstNonPHI());
        assert(getPtls(F));
        // TODO: other things in `allocate_gc_frame`
    }
}

// Based on QthreadsABI
void JuliaTapir::processSubTaskCall(TaskOutlineInfo &TOI, DominatorTree &DT) {
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

    CallerIRBuilder.SetInsertPoint(ReplStart);
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

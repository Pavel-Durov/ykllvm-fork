#include "llvm/Transforms/Yk/BasicBlockTracer.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/Transforms/Yk/ModuleClone.h"

#define DEBUG_TYPE "yk-basicblock-tracer-pass"

using namespace llvm;

namespace llvm {
void initializeYkBasicBlockTracerPass(PassRegistry &);
} // namespace llvm

namespace {
struct YkBasicBlockTracer : public ModulePass {
  static char ID;

  YkBasicBlockTracer() : ModulePass(ID) {
    initializeYkBasicBlockTracerPass(*PassRegistry::getPassRegistry());
  }

  bool runOnModule(Module &M) override {
    LLVMContext &Context = M.getContext();
    // Create externally linked function declaration:
    //   void __yk_trace_basicblock(int functionIndex, int blockIndex)
    Type *ReturnType = Type::getVoidTy(Context);
    Type *FunctionIndexArgType = Type::getInt32Ty(Context);
    Type *BlockIndexArgType = Type::getInt32Ty(Context);

    FunctionType *FType = FunctionType::get(
        ReturnType, {FunctionIndexArgType, BlockIndexArgType}, false);

    Function *TraceFunc = Function::Create(
        FType, GlobalVariable::ExternalLinkage, YK_TRACE_FUNCTION, M);

    Function *DummyTraceFunc = Function::Create(
        FType, GlobalVariable::ExternalLinkage, YK_TRACE_FUNCTION_DUMMY, M);

    IRBuilder<> builder(Context);
    uint32_t FunctionIndex = 0;
    for (auto &F : M) {
      uint32_t BlockIndex = 0;
      for (auto &BB : F) {
        builder.SetInsertPoint(&*BB.getFirstInsertionPt());

        // Check if function is address-taken by looking at its metadata
        bool isUnopt = F.getName().startswith(YK_UNOPT_PREFIX);
        bool isAddrTaken = F.getMetadata(YK_FUNC_ADDR_TAKEN_MD_NAME) != nullptr;

        if (isAddrTaken || isUnopt) {
          // For address-taken functions or unoptimised functions, we need
          // tracing always (because they're not duplicated but can be called
          // while tracing)
          builder.CreateCall(TraceFunc, {builder.getInt32(FunctionIndex),
                                         builder.getInt32(BlockIndex)});
        } else {
          // Add dummy tracing calls to optimised functions
          builder.CreateCall(DummyTraceFunc, {builder.getInt32(FunctionIndex),
                                              builder.getInt32(BlockIndex)});
        }

        assert(BlockIndex != UINT32_MAX &&
               "Expected BlockIndex to not overflow");
        BlockIndex++;
      }
      assert(FunctionIndex != UINT32_MAX &&
             "Expected FunctionIndex to not overflow");
      FunctionIndex++;
    }
    return true;
  }
  void getAnalysisUsage(AnalysisUsage &AU) const override {
    // AU.setPreservesCFG();
    AU.setPreservesAll(); // if appropriate
  }
};
} // namespace

char YkBasicBlockTracer::ID = 0;

INITIALIZE_PASS(YkBasicBlockTracer, DEBUG_TYPE, "yk basicblock tracer", false,
                false)

ModulePass *llvm::createYkBasicBlockTracerPass() {
  return new YkBasicBlockTracer();
}

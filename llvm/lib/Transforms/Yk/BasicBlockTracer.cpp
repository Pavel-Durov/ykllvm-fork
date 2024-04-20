#include "llvm/Transforms/Yk/BasicBlockTracer.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/InitializePasses.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/Pass.h"

#define DEBUG_TYPE "yk-basicblock-tracer-pass"

using namespace llvm;

namespace llvm {
void initializeYkBasicBlockTracerPass(PassRegistry &);
} // namespace llvm

namespace {
struct YkBasicBlockTracer : public ModulePass {
  static char ID;

  enum TraceType : int {
    BASIC_BLOCK = 0,
    EXTERNAL_CALL = 1,
    INDIRECT_CALL = 2
  };

  YkBasicBlockTracer() : ModulePass(ID) {
    initializeYkBasicBlockTracerPass(*PassRegistry::getPassRegistry());
  }

  bool runOnModule(Module &M) override {
    LLVMContext &Context = M.getContext();
    // Create externally linked function declaration:
    //   void yk_trace_basicblock(int functionIndex, int blockIndex)
    Type *ReturnType = Type::getVoidTy(Context);
    Type *FunctionIndexArgType = Type::getInt32Ty(Context);
    Type *BlockIndexArgType = Type::getInt32Ty(Context);
    Type *TracingType = Type::getInt32Ty(Context);

    FunctionType *FType = FunctionType::get(
        ReturnType, {FunctionIndexArgType, BlockIndexArgType, TracingType},
        false);
    Function *TraceFunc = Function::Create(
        FType, GlobalVariable::ExternalLinkage, YK_TRACE_FUNCTION, M);

    IRBuilder<> builder(Context);
    uint32_t FunctionIndex = 0;
    for (auto &F : M) {
      uint32_t BlockIndex = 0;
      for (auto &BB : F) {
        for (auto &I : BB) {
          if (auto *call = dyn_cast<CallInst>(&I)) {
            Function *calledFunction = call->getCalledFunction();

            if (isa<DbgInfoIntrinsic>(I)) {
              continue;
            }
            if (calledFunction && calledFunction->isIntrinsic()) {
              continue;
            }

            if (calledFunction &&
                calledFunction->getName() != YK_TRACE_FUNCTION) {
              if (calledFunction->isDeclaration()) {
                // Insert EXTERNAL_CALL tracing call before the call
                builder.SetInsertPoint(call);
                builder.CreateCall(TraceFunc,
                                   {builder.getInt32(FunctionIndex),
                                    builder.getInt32(BlockIndex),
                                    builder.getInt32(EXTERNAL_CALL)});
                // errs() << "[LLVM] External call detected: " << calledFunction->getName() << ", " << FunctionIndex  << ", " << BlockIndex << "\n";
              }
            }
            if (!call->getCalledFunction()) {
              // Insert INDIRECT_CALL tracing call before the call
              // builder.SetInsertPoint(call);
              // TODO: set function ptr as argument
              // Value *funcPtr = call->getCalledOperand();  // Correct method
              // to get the callee builder.CreateCall(TraceFunc,
              // {builder.getInt32(FunctionIndex),
              //                                funcPtr,
              //                                builder.getInt32(INDIRECT_CALL)});
            }
          }
        }
        // Insert BASIC_BLOCK tracing call before the call instruction.
        builder.SetInsertPoint(&*BB.getFirstInsertionPt());
        builder.CreateCall(TraceFunc, {builder.getInt32(FunctionIndex),
                                       builder.getInt32(BlockIndex),
                                       builder.getInt32(BASIC_BLOCK)});
        // errs() << "[LLVM] Start of BB. " << F.getName() << " " << FunctionIndex  << ", " << BlockIndex << "\n";
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
};
} // namespace

char YkBasicBlockTracer::ID = 0;

INITIALIZE_PASS(YkBasicBlockTracer, DEBUG_TYPE, "yk basicblock tracer", false,
                false)

ModulePass *llvm::createYkBasicBlockTracerPass() {
  return new YkBasicBlockTracer();
}

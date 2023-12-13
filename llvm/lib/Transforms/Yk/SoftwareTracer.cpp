#include "llvm/Transforms/Yk/SoftwareTracer.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Pass.h"

#define DEBUG_TYPE "yk-software-tracer-pass"

using namespace llvm;

bool SoftwareTracerPass::doInitialization(Module &M) {
  LLVMContext &Context = M.getContext();
  if (externalFunc == NULL) {
    Type *ReturnType = Type::getVoidTy(Context);
    Type *functionIndexArgType = Type::getInt32Ty(Context);
    Type *blockIndexArgType = Type::getInt32Ty(Context);

    FunctionType *FType = FunctionType::get(
        ReturnType, {functionIndexArgType, blockIndexArgType}, false);
    externalFunc = Function::Create(FType, GlobalVariable::ExternalLinkage,
                                    YK_TRACE_FUNCTION, M);
    return true;
  }
  return false;
}
bool SoftwareTracerPass::runOnModule(Module &M) {
  int functionIndex = 0;
  for (Module::iterator F = M.begin(), E = M.end(); F != E; ++F) {
    LLVMContext &Context = M.getContext();
    IRBuilder<> builder(Context);
    int blockIndex = 0;
    for (Function::iterator BB = F->begin(), E = F->end(); BB != E; ++BB) {
      builder.SetInsertPoint(&*BB->getFirstInsertionPt());
      builder.CreateCall(externalFunc, {builder.getInt32(functionIndex),
                                        builder.getInt32(blockIndex)});
      ++blockIndex;
    }
    ++functionIndex;
  }
  return true;
}

// char SoftwareTracerPass::ID = 0;

ModulePass *llvm::createSoftwareTracerPass() {
  return new SoftwareTracerPass();
}

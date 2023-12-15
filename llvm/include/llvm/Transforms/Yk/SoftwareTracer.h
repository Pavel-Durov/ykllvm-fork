#ifndef LLVM_TRANSFORMS_YK_HELLOWORLD_H
#define LLVM_TRANSFORMS_YK_HELLOWORLD_H

#include "llvm/Pass.h"
#include "llvm/Passes/PassBuilder.h"
// The name of the trace function
#define YK_TRACE_FUNCTION "yk_trace_basic_block"

namespace llvm {
    ModulePass *createSoftwareTracerPass();

    class SoftwareTracerPass : public ModulePass {
        Function *externalFunc = NULL;
        public:
            static char ID;
            SoftwareTracerPass() : ModulePass(ID) {}
            SoftwareTracerPass(const SoftwareTracerPass &other) : SoftwareTracerPass() {
                externalFunc = other.externalFunc;
            }

            // Required by PassRegistry
            static StringRef name() {
                return "SoftwareTracerPass";
            }
            // Required by PassRegistry
            bool run(Module &M) {
                return true;
            }

            virtual bool runOnModule(Module &M) override;
            
            virtual bool doInitialization(Module &M) override ;
            
            // Required by PassRegistry
            void printPipeline(raw_ostream &OS, function_ref<StringRef(StringRef)> MapClassName2PassName) {
                /* DO NOTHING */
            }
            // Required by PassRegistry
            PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM){
                return PreservedAnalyses::none();
            }
    };
} // namespace llvm
char llvm::SoftwareTracerPass::ID = 0;
#endif

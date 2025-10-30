#include "llvm/CodeGen/IRAnalysisPass.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/InitializePasses.h"
#include "llvm/PassRegistry.h"
#include "llvm/Support/raw_ostream.h"
#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace llvm;

namespace {
const bool PrintMIR = false;
const bool CountAddressTakenFunctions = true;

struct FunctionStats {
  std::string functionName;
  size_t numIRBasicBlocks;
  size_t numIRInstructions;
  size_t numMIRBasicBlocks;
  size_t numMIRInstructions;
};
} // anonymous namespace

namespace llvm {

class IRAnalysisPass : public MachineFunctionPass {
public:
  static char ID;
  IRAnalysisPass();

  bool runOnMachineFunction(MachineFunction &MF) override;

  ~IRAnalysisPass() override;

private:
  // Map from module name to (IRBBs, IRInsts, MIRBBs, MIRInsts)
  struct ModuleStats {
    size_t numIRBasicBlocks = 0;
    size_t numIRInstructions = 0;
    size_t numMIRBasicBlocks = 0;
    size_t numMIRInstructions = 0;
  };
  std::map<std::string, ModuleStats> moduleStats;
  // Vector to store per-function statistics
  std::vector<FunctionStats> functionStats;
  // Track address-taken functions
  std::set<std::string> addressTakenFunctions;
};

} // namespace llvm

IRAnalysisPass::IRAnalysisPass() : MachineFunctionPass(ID) {
  initializeIRAnalysisPassPass(*PassRegistry::getPassRegistry());
}

bool IRAnalysisPass::runOnMachineFunction(MachineFunction &MF) {
  // Get the module name and function
  const Module *M = MF.getFunction().getParent();
  std::string moduleName = M->getName().str();
  const Function &F = MF.getFunction();
  std::string functionName = MF.getName().str();

  // Check if function address is taken
  if (CountAddressTakenFunctions && F.hasAddressTaken()) {
    addressTakenFunctions.insert(moduleName + "::" + functionName);
  }

  // Count IR basic blocks and instructions
  size_t numIRBBs = 0;
  size_t numIRInsts = 0;
  for (const auto &BB : F) {
    ++numIRBBs;
    for (const auto &I : BB) {
      // Exclude debug intrinsics from IR instruction count
      if (!isa<DbgInfoIntrinsic>(&I)) {
        ++numIRInsts;
      }
    }
  }

  // Count MIR basic blocks and instructions
  size_t numMIRBBs = 0;
  size_t numMIRInsts = 0;
  for (auto &MBB : MF) {
    ++numMIRBBs;
    if (PrintMIR) {
      errs() << "Function: " << MF.getName() << "\n";
      errs() << "Basic Block: " << MBB.getName() << "\n";
    }
    // Count only non-debug instructions
    for (auto &MI : MBB) {
      if (!MI.isDebugInstr()) {
        ++numMIRInsts;
      }
    }
    if (PrintMIR) {
      MBB.print(errs());
    }
  }

  // Store per-function statistics
  FunctionStats funcStats;
  funcStats.functionName = moduleName + "::" + functionName;
  funcStats.numIRBasicBlocks = numIRBBs;
  funcStats.numIRInstructions = numIRInsts;
  funcStats.numMIRBasicBlocks = numMIRBBs;
  funcStats.numMIRInstructions = numMIRInsts;
  functionStats.push_back(funcStats);

  // Update the map for this module
  auto &stats = moduleStats[moduleName];
  stats.numIRBasicBlocks += numIRBBs;
  stats.numIRInstructions += numIRInsts;
  stats.numMIRBasicBlocks += numMIRBBs;
  stats.numMIRInstructions += numMIRInsts;

  return false;
}

IRAnalysisPass::~IRAnalysisPass() {
  errs() << "=== IR Analysis Pass Statistics ===\n\n";
  
  for (const auto &entry : moduleStats) {
    const std::string &moduleName = entry.first;
    const ModuleStats &stats = entry.second;
    
    double avgIRInstsPerBlock =
        stats.numIRBasicBlocks > 0
            ? static_cast<double>(stats.numIRInstructions) / stats.numIRBasicBlocks
            : 0.0;
    
    double avgMIRInstsPerBlock =
        stats.numMIRBasicBlocks > 0
            ? static_cast<double>(stats.numMIRInstructions) / stats.numMIRBasicBlocks
            : 0.0;
    
    errs() << "Module: " << moduleName << "\n"
           << "  AOT IR Statistics:\n"
           << "    Total Basic Blocks: " << stats.numIRBasicBlocks << "\n"
           << "    Total Instructions: " << stats.numIRInstructions << "\n"
           << "    Average Instructions per Block: " << avgIRInstsPerBlock << "\n"
           << "  MIR Statistics:\n"
           << "    Total Basic Blocks: " << stats.numMIRBasicBlocks << "\n"
           << "    Total Instructions: " << stats.numMIRInstructions << "\n"
           << "    Average Instructions per Block: " << avgMIRInstsPerBlock << "\n\n";
  }
  errs() << "===========================\n";

  // Print address-taken functions if enabled
  if (CountAddressTakenFunctions && !addressTakenFunctions.empty()) {
    errs() << "\n=== Address-Taken Functions ===\n";
    errs() << "Total: " << addressTakenFunctions.size() << "\n";
    for (const auto &funcName : addressTakenFunctions) {
      errs() << "  " << funcName << "\n";
    }
    errs() << "==============================\n";
  }
}

char IRAnalysisPass::ID = 0;
INITIALIZE_PASS(IRAnalysisPass, "ir-analysis-pass", "IR Analysis Pass", false, false)

namespace llvm {
MachineFunctionPass *createIRAnalysisPass() { return new IRAnalysisPass(); }
} // namespace llvm

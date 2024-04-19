//===- YkIR/YkIRWRiter.cpp -- Yk JIT IR Serialiaser---------------------===//
//
// Converts an LLVM module into Yk's on-disk AOT IR.
//
//===-------------------------------------------------------------------===//

#include "llvm/BinaryFormat/ELF.h"
#include "llvm/IR/Constant.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCSectionELF.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;
using namespace std;

namespace {

class SerialiseInstructionException {
private:
  string S;

public:
  SerialiseInstructionException(string S) : S(S) {}
  string &what() { return S; }
};

const char *SectionName = ".yk_ir";
const uint32_t Magic = 0xedd5f00d;
const uint32_t Version = 0;

enum OpCode {
  OpCodeNop = 0,
  OpCodeLoad,
  OpCodeStore,
  OpCodeAlloca,
  OpCodeCall,
  OpCodeBr,
  OpCodeCondBr,
  OpCodeICmp,
  OpCodeRet,
  OpCodeInsertValue,
  OpCodePtrAdd,
  OpCodeBinOp,
  OpCodeUnimplemented = 255, // YKFIXME: Will eventually be deleted.
};

enum OperandKind {
  OperandKindConstant = 0,
  OperandKindLocal,
  OperandKindGlobal,
  OperandKindFunction,
  OperandKindArg,
};

enum TypeKind {
  Void = 0,
  Integer,
  Ptr,
  FunctionTy,
  Struct,
  UnimplementedType = 255, // YKFIXME: Will eventually be deleted.
};

// A predicate used in a numeric comparison.
enum CmpPredicate {
  PredEqual = 0,
  PredNotEqual,
  PredUnsignedGreater,
  PredUnsignedGreaterEqual,
  PredUnsignedLess,
  PredUnsignedLessEqual,
  PredSignedGreater,
  PredSignedGreaterEqual,
  PredSignedLess,
  PredSignedLessEqual,
};

// A binary operator.
enum BinOp {
  BinOpAdd,
  BinOpSub,
  BinOpMul,
  BinOpOr,
  BinOpAnd,
  BinOpXor,
  BinOpShl,
  BinOpAShr,
  BinOpFAdd,
  BinOpFDiv,
  BinOpFMul,
  BinOpFRem,
  BinOpFSub,
  BinOpLShr,
  BinOpSDiv,
  BinOpSRem,
  BinOpUDiv,
  BinOpURem,
};

template <class T> string toString(T *X) {
  string S;
  raw_string_ostream SS(S);
  X->print(SS);
  return S;
}

// Get the index of an element in its parent container.
template <class C, class E> size_t getIndex(C *Container, E *FindElement) {
  bool Found = false;
  size_t Idx = 0;
  for (E &AnElement : *Container) {
    if (&AnElement == FindElement) {
      Found = true;
      break;
    }
    Idx++;
  }
  assert(Found);
  return Idx;
}

// A <BBIdx, InstrIdx> pair that Uniquely identifies an Yk IR instruction within
// a function.
using InstrLoc = std::tuple<size_t, size_t>;

// Maps an LLVM instruction that generates a value to the corresponding Yk IR
// instruction.
using ValueLoweringMap = map<Instruction *, InstrLoc>;

// The class responsible for serialising our IR into the interpreter binary.
//
// It walks over the LLVM IR, lowering each function, block, instruction, etc.
// into a Yk IR equivalent.
//
// As it does this there are some invariants that must be maintained:
//
//  - The current basic block index (BBIdx) is passed down the lowering process.
//    This must be incremented each time we finish a Yk IR basic block.
//
//  - Similarly for instructions. Each time we finish a Yk IR instruction,
//    we must increment the current instruction index (InstIdx).
//
//  - When we are done lowering an LLVM instruction that generates a value, we
//    must update the `VLMap` with an entry that maps the LLVM instruction to
//    the final Yk IR instruction in the lowering. If the LLVM instruction
//    doesn't generate a value, or the LLVM instruction lowered to exactly zero
//    Yk IR instructions, then there is no need to update the `VLMap`.
//
// These invariants are required so that when we encounter a local variable as
// an operand to an LLVM instruction, we can quickly find the corresponding Yk
// IR local variable.
class YkIRWriter {
private:
  Module &M;
  MCStreamer &OutStreamer;
  DataLayout DL;

  vector<llvm::Type *> Types;
  vector<llvm::Constant *> Constants;
  vector<llvm::GlobalVariable *> Globals;

  // Return the index of the LLVM type `Ty`, inserting a new entry if
  // necessary.
  size_t typeIndex(llvm::Type *Ty) {
    vector<llvm::Type *>::iterator Found =
        std::find(Types.begin(), Types.end(), Ty);
    if (Found != Types.end()) {
      return std::distance(Types.begin(), Found);
    }

    // Not found. Assign it a type index.
    size_t Idx = Types.size();
    Types.push_back(Ty);

    // If the newly-registered type is an aggregate type that contains other
    // types, then assign them type indices now too.
    for (llvm::Type *STy : Ty->subtypes()) {
      typeIndex(STy);
    }

    return Idx;
  }

  // Return the index of the LLVM constant `C`, inserting a new entry if
  // necessary.
  size_t constantIndex(Constant *C) {
    vector<Constant *>::iterator Found =
        std::find(Constants.begin(), Constants.end(), C);
    if (Found != Constants.end()) {
      return std::distance(Constants.begin(), Found);
    }
    size_t Idx = Constants.size();
    Constants.push_back(C);
    return Idx;
  }

  // Return the index of the LLVM global `G`, inserting a new entry if
  // necessary.
  size_t globalIndex(class GlobalVariable *G) {
    vector<class GlobalVariable *>::iterator Found =
        std::find(Globals.begin(), Globals.end(), G);
    if (Found != Globals.end()) {
      return std::distance(Globals.begin(), Found);
    }
    size_t Idx = Globals.size();
    Globals.push_back(G);
    return Idx;
  }

  size_t functionIndex(llvm::Function *F) {
    // FIXME: For now we assume that function indicies in LLVM IR and our IR
    // are the same.
    return getIndex(&M, F);
  }

  // Serialises a null-terminated string.
  void serialiseString(StringRef S) {
    OutStreamer.emitBinaryData(S);
    OutStreamer.emitInt8(0); // null terminator.
  }

  void serialiseOpcode(OpCode Code) { OutStreamer.emitInt8(Code); }

  void serialiseOperandKind(OperandKind Kind) { OutStreamer.emitInt8(Kind); }

  void serialiseConstantOperand(Instruction *Parent, llvm::Constant *C) {
    serialiseOperandKind(OperandKindConstant);
    OutStreamer.emitSizeT(constantIndex(C));
  }

  void serialiseLocalVariableOperand(Instruction *I, ValueLoweringMap &VLMap) {
    auto [BBIdx, InstIdx] = VLMap.at(I);
    serialiseOperandKind(OperandKindLocal);
    OutStreamer.emitSizeT(BBIdx);
    OutStreamer.emitSizeT(InstIdx);
  }

  void serialiseFunctionOperand(llvm::Function *F) {
    serialiseOperandKind(OperandKindFunction);
    OutStreamer.emitSizeT(functionIndex(F));
  }

  void serialiseBlockLabel(BasicBlock *BB) {
    // Basic block indices are the same in both LLVM IR and our IR.
    OutStreamer.emitSizeT(getIndex(BB->getParent(), BB));
  }

  void serialiseArgOperand(ValueLoweringMap &VLMap, Argument *A) {
    // This assumes that the argument indices match in both IRs.

    // opcode:
    serialiseOperandKind(OperandKindArg);
    // parent function index:
    OutStreamer.emitSizeT(getIndex(&M, A->getParent()));
    // arg index
    OutStreamer.emitSizeT(A->getArgNo());
  }

  void serialiseGlobalOperand(GlobalVariable *G) {
    serialiseOperandKind(OperandKindGlobal);
    OutStreamer.emitSizeT(globalIndex(G));
  }

  void serialiseOperand(Instruction *Parent, ValueLoweringMap &VLMap,
                        Value *V) {
    if (llvm::GlobalVariable *G = dyn_cast<llvm::GlobalVariable>(V)) {
      serialiseGlobalOperand(G);
    } else if (llvm::Function *F = dyn_cast<llvm::Function>(V)) {
      serialiseFunctionOperand(F);
    } else if (llvm::Constant *C = dyn_cast<llvm::Constant>(V)) {
      serialiseConstantOperand(Parent, C);
    } else if (llvm::Argument *A = dyn_cast<llvm::Argument>(V)) {
      serialiseArgOperand(VLMap, A);
    } else if (Instruction *I = dyn_cast<Instruction>(V)) {
      serialiseLocalVariableOperand(I, VLMap);
    } else {
      llvm::report_fatal_error(
          StringRef("attempt to serialise non-yk-operand: " + toString(V)));
    }
  }

  void serialiseBinaryOperatorInst(llvm::BinaryOperator *I,
                                   ValueLoweringMap &VLMap, unsigned BBIdx,
                                   unsigned &InstIdx) {
    assert(I->getNumOperands() == 2);

    // opcode:
    serialiseOpcode(OpCodeBinOp);
    // left-hand side:
    serialiseOperand(I, VLMap, I->getOperand(0));
    // binary operator:
    serialiseBinOperator(I->getOpcode());
    // right-hand side:
    serialiseOperand(I, VLMap, I->getOperand(1));

    VLMap[I] = {BBIdx, InstIdx};
    InstIdx++;
  }

  // Serialise a binary operator.
  void serialiseBinOperator(Instruction::BinaryOps BO) {
    // operand kind:
    // OutStreamer.emitInt8(OperandKind::OpKindBinOp);
    // the operator:
    switch (BO) {
    case Instruction::BinaryOps::Add:
      OutStreamer.emitInt8(BinOp::BinOpAdd);
      break;
    case Instruction::BinaryOps::Sub:
      OutStreamer.emitInt8(BinOp::BinOpSub);
      break;
    case Instruction::BinaryOps::Mul:
      OutStreamer.emitInt8(BinOp::BinOpMul);
      break;
    case Instruction::BinaryOps::Or:
      OutStreamer.emitInt8(BinOp::BinOpOr);
      break;
    case Instruction::BinaryOps::And:
      OutStreamer.emitInt8(BinOp::BinOpAnd);
      break;
    case Instruction::BinaryOps::Xor:
      OutStreamer.emitInt8(BinOp::BinOpXor);
      break;
    case Instruction::BinaryOps::Shl:
      OutStreamer.emitInt8(BinOp::BinOpShl);
      break;
    case Instruction::BinaryOps::AShr:
      OutStreamer.emitInt8(BinOp::BinOpAShr);
      break;
    case Instruction::BinaryOps::FAdd:
      OutStreamer.emitInt8(BinOp::BinOpFAdd);
      break;
    case Instruction::BinaryOps::FDiv:
      OutStreamer.emitInt8(BinOp::BinOpFDiv);
      break;
    case Instruction::BinaryOps::FMul:
      OutStreamer.emitInt8(BinOp::BinOpFMul);
      break;
    case Instruction::BinaryOps::FRem:
      OutStreamer.emitInt8(BinOp::BinOpFRem);
      break;
    case Instruction::BinaryOps::FSub:
      OutStreamer.emitInt8(BinOp::BinOpFSub);
      break;
    case Instruction::BinaryOps::LShr:
      OutStreamer.emitInt8(BinOp::BinOpLShr);
      break;
    case Instruction::BinaryOps::SDiv:
      OutStreamer.emitInt8(BinOp::BinOpSDiv);
      break;
    case Instruction::BinaryOps::SRem:
      OutStreamer.emitInt8(BinOp::BinOpSRem);
      break;
    case Instruction::BinaryOps::UDiv:
      OutStreamer.emitInt8(BinOp::BinOpUDiv);
      break;
    case Instruction::BinaryOps::URem:
      OutStreamer.emitInt8(BinOp::BinOpURem);
      break;
    default:
      llvm::report_fatal_error("unknown binary operator");
    }
  }

  void serialiseAllocaInst(AllocaInst *I, ValueLoweringMap &VLMap,
                           unsigned BBIdx, unsigned &InstIdx) {
    // opcode:
    serialiseOpcode(OpCodeAlloca);

    // type to be allocated:
    OutStreamer.emitSizeT(typeIndex(I->getAllocatedType()));

    // number of objects to allocate
    ConstantInt *CI = cast<ConstantInt>(I->getArraySize());
    // XXX guard cast
    OutStreamer.emitSizeT(CI->getZExtValue());

    VLMap[I] = {BBIdx, InstIdx};
    InstIdx++;
  }

  void serialiseCallInst(CallInst *I, ValueLoweringMap &VLMap, unsigned BBIdx,
                         unsigned &InstIdx) {
    if (I->isInlineAsm()) {
      // For now we omit calls to empty inline asm blocks.
      //
      // These are pretty much always present in yk unit tests to block
      // optimisations.
      // if (!(cast<InlineAsm>(Callee)->getAsmString().empty())) {
      if (!(cast<InlineAsm>(I->getCalledOperand())->getAsmString().empty())) {
        // Non-empty asm block. We can't ignore it.
        serialiseUnimplementedInstruction(I, VLMap, BBIdx, InstIdx);
      }
      return;
    }

    // FIXME: indirect calls.
    //
    // Note that this assertion can also fail if you do a direct call without
    // the correct type annotation at the call site.
    //
    // e.g. for a functiion:
    //
    //   define i32 @f(i32, ...)
    //
    // if you do:
    //
    //   call i32 @f(1i32, 2i32);
    //
    // instead of:
    //
    //   call i32 (i32, ...) @f(1i32, 2i32);
    assert(I->getCalledFunction());

    // opcode:
    serialiseOpcode(OpCodeCall);
    // callee:
    OutStreamer.emitSizeT(functionIndex(I->getCalledFunction()));
    // num_args:
    // (this includes static and varargs arguments)
    OutStreamer.emitInt32(I->arg_size());
    // args:
    for (unsigned OI = 0; OI < I->arg_size(); OI++) {
      serialiseOperand(I, VLMap, I->getOperand(OI));
    }

    // If the return type is non-void, then this defines a local.
    if (!I->getType()->isVoidTy()) {
      VLMap[I] = {BBIdx, InstIdx};
    }
    InstIdx++;
  }

  void serialiseBranchInst(BranchInst *I, ValueLoweringMap &VLMap,
                           unsigned BBIdx, unsigned &InstIdx) {
    // We split LLVM's `br` into two Yk IR instructions: one for unconditional
    // branching, another for conidtional branching.
    if (!I->isConditional()) {
      // We don't serialise the branch target for unconditional branches because
      // traces will guide us.
      //
      // opcode:
      serialiseOpcode(OpCodeBr);
    } else {
      // opcode:
      serialiseOpcode(OpCodeCondBr);
      // We DO need operands for conditional branches, so that we can build
      // guards.
      //
      // cond:
      serialiseOperand(I, VLMap, I->getCondition());
      // true_bb:
      serialiseBlockLabel(I->getSuccessor(0));
      // false_bb:
      serialiseBlockLabel(I->getSuccessor(1));
    }
    InstIdx++;
  }

  void serialiseLoadInst(LoadInst *I, ValueLoweringMap &VLMap, unsigned BBIdx,
                         unsigned &InstIdx) {
    // opcode:
    serialiseOpcode(OpCodeLoad);
    // ptr:
    serialiseOperand(I, VLMap, I->getPointerOperand());
    // type_idx:
    OutStreamer.emitSizeT(typeIndex(I->getType()));

    VLMap[I] = {BBIdx, InstIdx};
    InstIdx++;
  }

  void serialiseStoreInst(StoreInst *I, ValueLoweringMap &VLMap, unsigned BBIdx,
                          unsigned &InstIdx) {
    // opcode:
    serialiseOpcode(OpCodeStore);
    // value:
    serialiseOperand(I, VLMap, I->getValueOperand());
    // ptr:
    serialiseOperand(I, VLMap, I->getPointerOperand());

    InstIdx++;
  }

  void serialiseGetElementPtrInst(GetElementPtrInst *I, ValueLoweringMap &VLMap,
                                  unsigned BBIdx, unsigned &InstIdx) {
    unsigned BitWidth = 64;
    MapVector<Value *, APInt> Offsets;
    APInt Offset(BitWidth, 0);

    bool Res = I->collectOffset(DL, BitWidth, Offsets, Offset);
    assert(Res);

    // opcode:
    serialiseOpcode(OpCodePtrAdd);
    // type_idx:
    OutStreamer.emitSizeT(typeIndex(I->getType()));
    // pointer:
    serialiseOperand(I, VLMap, I->getPointerOperand());
    // offset:
    serialiseOperand(I, VLMap, ConstantInt::get(I->getContext(), Offset));

    VLMap[I] = {BBIdx, InstIdx};
    InstIdx++;
  }

  // Serialise an LLVM predicate.
  void serialisePredicate(llvm::CmpInst::Predicate P) {
    std::optional<CmpPredicate> LP = std::nullopt;
    switch (P) {
    case llvm::CmpInst::ICMP_EQ:
      LP = PredEqual;
      break;
    case llvm::CmpInst::ICMP_NE:
      LP = PredNotEqual;
      break;
    case llvm::CmpInst::ICMP_UGT:
      LP = PredUnsignedGreater;
      break;
    case llvm::CmpInst::ICMP_UGE:
      LP = PredUnsignedGreaterEqual;
      break;
    case llvm::CmpInst::ICMP_ULT:
      LP = PredUnsignedLess;
      break;
    case llvm::CmpInst::ICMP_ULE:
      LP = PredUnsignedLessEqual;
      break;
    case llvm::CmpInst::ICMP_SGT:
      LP = PredSignedGreater;
      break;
    case llvm::CmpInst::ICMP_SGE:
      LP = PredSignedGreaterEqual;
      break;
    case llvm::CmpInst::ICMP_SLT:
      LP = PredSignedLess;
      break;
    case llvm::CmpInst::ICMP_SLE:
      LP = PredSignedLessEqual;
      break;
    default:
      abort(); // TODO: floating point predicates.
    }
    OutStreamer.emitInt8(LP.value());
  }

  void serialiseICmpInst(ICmpInst *I, ValueLoweringMap &VLMap, unsigned BBIdx,
                         unsigned &InstIdx) {
    // opcode:
    serialiseOpcode(OpCodeICmp);
    // type_idx:
    OutStreamer.emitSizeT(typeIndex(I->getType()));
    // lhs:
    serialiseOperand(I, VLMap, I->getOperand(0));
    // predicate:
    serialisePredicate(I->getPredicate());
    // rhs:
    serialiseOperand(I, VLMap, I->getOperand(1));

    VLMap[I] = {BBIdx, InstIdx};
    InstIdx++;
  }

  void serialiseReturnInst(ReturnInst *I, ValueLoweringMap &VLMap,
                           unsigned BBIdx, unsigned &InstIdx) {
    // opcode:
    serialiseOpcode(OpCodeRet);

    Value *RV = I->getReturnValue();
    if (RV == nullptr) {
      // has_val = 0:
      OutStreamer.emitInt8(0);
    } else {
      // has_val = 1:
      OutStreamer.emitInt8(1);
      // value:
      serialiseOperand(I, VLMap, RV);
    }

    InstIdx++;
  }

  void serialiseInsertValueInst(InsertValueInst *I, ValueLoweringMap &VLMap,
                                unsigned BBIdx, unsigned &InstIdx) {
    // opcode:
    serialiseOpcode(OpCodeInsertValue);
    // agg:
    serialiseOperand(I, VLMap, I->getAggregateOperand());
    // elem:
    serialiseOperand(I, VLMap, I->getInsertedValueOperand());

    InstIdx++;
  }

  void serialiseInst(Instruction *I, ValueLoweringMap &VLMap, unsigned BBIdx,
                     unsigned &InstIdx) {
    // Macro to make the dispatch below easier to read/sort.
#define INST_SERIALISE(LLVM_INST, LLVM_INST_TYPE, SERIALISER)                  \
  if (LLVM_INST_TYPE *II = dyn_cast<LLVM_INST_TYPE>(LLVM_INST)) {              \
    SERIALISER(II, VLMap, BBIdx, InstIdx);                                     \
    return;                                                                    \
  }

    INST_SERIALISE(I, AllocaInst, serialiseAllocaInst);
    INST_SERIALISE(I, BinaryOperator, serialiseBinaryOperatorInst);
    INST_SERIALISE(I, BranchInst, serialiseBranchInst);
    INST_SERIALISE(I, CallInst, serialiseCallInst);
    INST_SERIALISE(I, GetElementPtrInst, serialiseGetElementPtrInst);
    INST_SERIALISE(I, ICmpInst, serialiseICmpInst);
    INST_SERIALISE(I, InsertValueInst, serialiseInsertValueInst);
    INST_SERIALISE(I, LoadInst, serialiseLoadInst);
    INST_SERIALISE(I, ReturnInst, serialiseReturnInst);
    INST_SERIALISE(I, StoreInst, serialiseStoreInst);

    // INST_SERIALISE does an early return upon a match, so if we get here then
    // the instruction wasn't handled.
    serialiseUnimplementedInstruction(I, VLMap, BBIdx, InstIdx);
  }

  void serialiseUnimplementedInstruction(Instruction *I,
                                         ValueLoweringMap &VLMap,
                                         unsigned BBIdx, unsigned &InstIdx) {
    // opcode:
    serialiseOpcode(OpCodeUnimplemented);
    // stringified problem instruction
    serialiseString(toString(I));

    if (!I->getType()->isVoidTy()) {
      VLMap[I] = {BBIdx, InstIdx};
    }
    InstIdx++;
  }

  void serialiseBlock(BasicBlock &BB, ValueLoweringMap &VLMap,
                      unsigned &BBIdx) {
    // Keep the instruction skipping logic in one place.
    auto ShouldSkipInstr = [](Instruction *I) {
      // Skip non-semantic instrucitons for now.
      //
      // We may come back to them later if we need better debugging
      // facilities, but for now they just clutter up our AOT module.
      if (I->isDebugOrPseudoInst()) {
        return true;
      }

      // See serialiseCallInst() for details.
      if (CallInst *CI = dyn_cast<CallInst>(I)) {
        if (InlineAsm *IA = dyn_cast<InlineAsm>(CI->getCalledOperand())) {
          return IA->getAsmString().empty();
        }
      }

      return false;
    };

    // Count instructions.
    //
    // FIXME: I don't like this much:
    //
    //  - Assumes one LLVM instruction becomes exactly one Yk IR instruction.
    //  - Requires a second loop to count ahead of time.
    //
    // Can we emit the instrucitons into a temp buffer and keep a running count
    // of how many instructions we generated instead?
    size_t NumInstrs = 0;
    for (Instruction &I : BB) {
      if (ShouldSkipInstr(&I)) {
        continue;
      }
      NumInstrs++;
    }

    // num_instrs:
    OutStreamer.emitSizeT(NumInstrs);
    // instrs:
    unsigned InstIdx = 0;
    for (Instruction &I : BB) {
      if (ShouldSkipInstr(&I)) {
        continue;
      }
      serialiseInst(&I, VLMap, BBIdx, InstIdx);
    }

    // Check we emitted the number of instructions that we promised.
    assert(InstIdx == NumInstrs);

    BBIdx++;
  }

  void serialiseArg(Argument *A) {
    // type_index:
    OutStreamer.emitSizeT(typeIndex(A->getType()));
  }

  void serialiseFunc(llvm::Function &F) {
    // name:
    serialiseString(F.getName());
    // type_idx:
    OutStreamer.emitSizeT(typeIndex(F.getFunctionType()));
    // num_blocks:
    OutStreamer.emitSizeT(F.size());
    // blocks:
    unsigned BBIdx = 0;
    ValueLoweringMap VLMap;
    for (BasicBlock &BB : F) {
      serialiseBlock(BB, VLMap, BBIdx);
    }
  }

  void serialiseFunctionType(FunctionType *Ty) {
    OutStreamer.emitInt8(TypeKind::FunctionTy);
    // num_args:
    OutStreamer.emitSizeT(Ty->getNumParams());
    // arg_tys:
    for (llvm::Type *SubTy : Ty->params()) {
      OutStreamer.emitSizeT(typeIndex(SubTy));
    }
    // ret_ty:
    OutStreamer.emitSizeT(typeIndex(Ty->getReturnType()));
    // is_vararg:
    OutStreamer.emitInt8(Ty->isVarArg());
  }

  void serialiseStructType(StructType *STy) {
    OutStreamer.emitInt8(TypeKind::Struct);
    unsigned NumFields = STy->getNumElements();
    DataLayout DL(&M);
    const StructLayout *SL = DL.getStructLayout(STy);
    // num_fields:
    OutStreamer.emitSizeT(NumFields);
    // field_tys:
    for (unsigned I = 0; I < NumFields; I++) {
      OutStreamer.emitSizeT(typeIndex(STy->getElementType(I)));
    }
    // field_bit_offs:
    for (unsigned I = 0; I < NumFields; I++) {
      OutStreamer.emitSizeT(SL->getElementOffsetInBits(I));
    }
  }

  void serialiseType(llvm::Type *Ty) {
    if (Ty->isVoidTy()) {
      OutStreamer.emitInt8(TypeKind::Void);
    } else if (PointerType *PT = dyn_cast<PointerType>(Ty)) {
      // FIXME: The Yk runtime assumes all pointers are void-ptr-sized.
      assert(DL.getPointerSize(PT->getAddressSpace()) == sizeof(void *));
      OutStreamer.emitInt8(TypeKind::Ptr);
    } else if (IntegerType *ITy = dyn_cast<IntegerType>(Ty)) {
      OutStreamer.emitInt8(TypeKind::Integer);
      OutStreamer.emitInt32(ITy->getBitWidth());
    } else if (FunctionType *FTy = dyn_cast<FunctionType>(Ty)) {
      serialiseFunctionType(FTy);
    } else if (StructType *STy = dyn_cast<StructType>(Ty)) {
      serialiseStructType(STy);
    } else {
      OutStreamer.emitInt8(TypeKind::UnimplementedType);
      serialiseString(toString(Ty));
    }
  }

  void serialiseConstantInt(ConstantInt *CI) {
    OutStreamer.emitSizeT(typeIndex(CI->getType()));
    OutStreamer.emitSizeT(CI->getBitWidth() / 8);
    for (size_t I = 0; I < CI->getBitWidth(); I += 8) {
      uint64_t Byte = CI->getValue().extractBitsAsZExtValue(8, I);
      OutStreamer.emitInt8(Byte);
    }
  }

  void serialiseUnimplementedConstant(Constant *C) {
    // type_index:
    OutStreamer.emitSizeT(typeIndex(C->getType()));
    // num_bytes:
    // Just report zero for now.
    OutStreamer.emitSizeT(0);
  }

  void serialiseConstant(Constant *C) {
    if (ConstantInt *CI = dyn_cast<ConstantInt>(C)) {
      serialiseConstantInt(CI);
    } else {
      serialiseUnimplementedConstant(C);
    }
  }

  void serialiseGlobal(class GlobalVariable *G) {
    OutStreamer.emitInt8(G->isThreadLocal());
    serialiseString(G->getName());
  }

public:
  YkIRWriter(Module &M, MCStreamer &OutStreamer)
      : M(M), OutStreamer(OutStreamer), DL(&M) {}

  // Entry point for IR serialisation.
  //
  // The order of serialisation matters.
  //
  // - Serialising functions can introduce new types and constants.
  // - Serialising constants can introduce new types.
  //
  // So we must serialise functions, then constants, then types.
  void serialise() {
    // header:
    OutStreamer.emitInt32(Magic);
    OutStreamer.emitInt32(Version);

    // num_funcs:
    OutStreamer.emitSizeT(M.size());
    // funcs:
    for (llvm::Function &F : M) {
      serialiseFunc(F);
    }

    // num_constants:
    OutStreamer.emitSizeT(Constants.size());
    // constants:
    for (Constant *&C : Constants) {
      serialiseConstant(C);
    }

    // num_globals:
    OutStreamer.emitSizeT(Globals.size());
    // globals:
    for (class GlobalVariable *&G : Globals) {
      serialiseGlobal(G);
    }

    // Now that we've finished serialising globals, add a global (immutable, for
    // now) array to the LLVM module containing pointers to all the global
    // variables. We will use this to find the addresses of globals at runtime.
    // The indices of the array correspond with `GlobalDeclIdx`s in the AOT IR.
    vector<llvm::Constant *> GlobalsAsConsts;
    for (llvm::GlobalVariable *G : Globals) {
      GlobalsAsConsts.push_back(cast<llvm::Constant>(G));
    }
    ArrayType *GlobalsArrayTy =
        ArrayType::get(PointerType::get(M.getContext(), 0), Globals.size());
    GlobalVariable *GlobalsArray = new GlobalVariable(
        M, GlobalsArrayTy, true, GlobalValue::LinkageTypes::ExternalLinkage,
        ConstantArray::get(GlobalsArrayTy, GlobalsAsConsts));
    GlobalsArray->setName("__yk_globalvar_ptrs");

    // num_types:
    OutStreamer.emitSizeT(Types.size());
    // types:
    for (llvm::Type *&Ty : Types) {
      serialiseType(Ty);
    }
  }
};
} // anonymous namespace

// Create an ELF section for storing Yk IR into.
MCSection *createYkIRSection(MCContext &Ctx, const MCSection *TextSec) {
  if (Ctx.getObjectFileType() != MCContext::IsELF)
    return nullptr;

  const MCSectionELF *ElfSec = static_cast<const MCSectionELF *>(TextSec);
  unsigned Flags = ELF::SHF_LINK_ORDER;
  StringRef GroupName;

  // Ensure the loader loads it.
  Flags |= ELF::SHF_ALLOC;

  return Ctx.getELFSection(SectionName, ELF::SHT_LLVM_BB_ADDR_MAP, Flags, 0,
                           GroupName, true, ElfSec->getUniqueID(),
                           cast<MCSymbolELF>(TextSec->getBeginSymbol()));
}

// Emit a start/end IR marker.
//
// The JIT uses a start and end marker to make a Rust slice of the IR.
void emitStartOrEndSymbol(MCContext &MCtxt, MCStreamer &OutStreamer,
                          bool Start) {
  std::string SymName("ykllvm.yk_ir.");
  if (Start)
    SymName.append("start");
  else
    SymName.append("stop");

  MCSymbol *Sym = MCtxt.getOrCreateSymbol(SymName);
  OutStreamer.emitSymbolAttribute(Sym, llvm::MCSA_Global);
  OutStreamer.emitLabel(Sym);
}

namespace llvm {

// Emit Yk IR into the resulting ELF binary.
void embedYkIR(MCContext &Ctx, MCStreamer &OutStreamer, Module &M) {
  MCSection *YkIRSec =
      createYkIRSection(Ctx, std::get<0>(OutStreamer.getCurrentSection()));

  OutStreamer.pushSection();
  OutStreamer.switchSection(YkIRSec);
  emitStartOrEndSymbol(Ctx, OutStreamer, true);
  YkIRWriter(M, OutStreamer).serialise();
  emitStartOrEndSymbol(Ctx, OutStreamer, false);
  OutStreamer.popSection();
}
} // namespace llvm

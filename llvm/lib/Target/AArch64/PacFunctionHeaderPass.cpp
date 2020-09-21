
#include "AArch64.h"
#include "AArch64InstrInfo.h"
#include "AArch64MachineFunctionInfo.h"
#include "AArch64RegisterInfo.h"
#include "MCTargetDesc/AArch64AddressingModes.h"

#include "PacFunctionHeaderPass.h"

#include "llvm/ADT/SmallSet.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineJumpTableInfo.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/Support/GraphWriter.h"
#include "llvm/Support/raw_ostream.h"

#include <set>

using namespace llvm;

PacFunctionHeaderPass::PacFunctionHeaderPass() : MachineFunctionPass(ID) {}

PacFunctionHeaderPass::~PacFunctionHeaderPass() {}

StringRef PacFunctionHeaderPass::getPassName() const {
  return "---### PAC Function Header Pass ###---";
}

bool PacFunctionHeaderPass::doInitialization(Module &M) { return false; }

bool PacFunctionHeaderPass::doFinalization(Module &M) { return false; }

bool PacFunctionHeaderPass::runOnMachineFunction(MachineFunction &F) {
  const TargetInstrInfo &TII = *F.getSubtarget().getInstrInfo();
  // Add a function header signature update used for indirect calls
  MachineFunction::iterator BI = F.begin();
  MachineBasicBlock *BB = &*BI;
  MachineBasicBlock::instr_iterator instIt = BB->instr_begin();
  DebugLoc DL;

  // Load Patch for indirect function call header
  BuildMI(*BB, instIt, DL, TII.get(AArch64::MOVZXi))
      .addReg(AArch64::X27, RegState::Define)
      .addImm(0)
      .addImm(48);
  // Apply Patch
  BuildMI(*BB, instIt, DL, TII.get(AArch64::EORXrs))
      .addReg(AArch64::X28)
      .addReg(AArch64::X28)
      .addReg(AArch64::X27)
      .addImm(0);
  // Load patch for return from indirect call
  BuildMI(*BB, instIt, DL, TII.get(AArch64::MOVZXi))
      .addReg(AArch64::X27)
      .addImm(0)
      .addImm(48);
  // This instruction is a dummy, which gets patch to a branch jumping over
  // the last MOVZXi instruction to skip overwriting the patch value
  // This will patched to a "B 8" instruction
  MachineInstr *BranchInst = BuildMI(*BB, instIt, DL, TII.get(AArch64::MOVZXi))
                                 .addReg(AArch64::X27)
                                 .addImm(0)
                                 .addImm(0);
  // Save the instruction to the meta data section to ease call binary rewriting
  BranchInst->storeBranchOverPatch();

  // Load zero patch for return from direct call
  BuildMI(*BB, instIt, DL, TII.get(AArch64::MOVZXi))
      .addReg(AArch64::X27)
      .addImm(0)
      .addImm(0);

  // Now iterate to the return, where we use the patch
  for (MachineBasicBlock &bb : F) {
    for (MachineInstr &inst : bb) {
      if (inst.isReturn()) {
        instIt = (MachineBasicBlock::instr_iterator)inst;
        // Apply return patch
        BuildMI(bb, instIt, DebugLoc(), TII.get(AArch64::EORXrs))
            .addReg(AArch64::X28)
            .addReg(AArch64::X28)
            .addReg(AArch64::X27)
            .addImm(0);
      }
    }
  }
  return true;
}

char PacFunctionHeaderPass::ID = 0;

FunctionPass *llvm::createPacFunctionHeaderPass() {
  return new PacFunctionHeaderPass();
}

INITIALIZE_PASS(PacFunctionHeaderPass, "PacFunctionHeaderPass",
                "PAC Function Header Pass", false, false)

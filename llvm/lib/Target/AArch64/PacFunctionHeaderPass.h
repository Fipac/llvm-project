#ifndef PACCFUNCTIONHEADER_PASS_H
#define PACCFUNCTIONHEADER_PASS_H

#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineFunctionPass.h"

namespace llvm {

void initializePacFunctionHeaderPassPass(PassRegistry &);

class PacFunctionHeaderPass : public MachineFunctionPass {
public:
  static char ID;

  PacFunctionHeaderPass();
  virtual ~PacFunctionHeaderPass();

  virtual StringRef getPassName() const override;
  virtual bool doInitialization(Module &M) override;
  virtual bool doFinalization(Module &M) override;

  bool runOnMachineFunction(MachineFunction &F) override;
};

} /* namespace llvm */

#endif

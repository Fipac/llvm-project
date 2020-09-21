#ifndef PACCFI_PASS_H
#define PACCFI_PASS_H

#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineFunctionPass.h"

#include "../../Transforms/Instrumentation/MaximumSpanningTree.h"

#include <map>

namespace llvm {

typedef MaximumSpanningTree<MachineBasicBlock> MaxSpanTree;
typedef MaxSpanTree::Edge MSTEdge;

struct EdgeValue {
  enum UpdateType { NO_UPDATE = 0, INVALID, UPDATE };
  double weight;
  UpdateType update;

  EdgeValue() : weight(1.0), update(NO_UPDATE) {}
};

void initializePacCFIPassPass(PassRegistry &);

class PacCFIPass : public MachineFunctionPass {
public:
  static char ID;

  enum PatchType {
    STATE_PATCH = 0,
    CALL_PATCH,
    ICALL_PATCH
  };

  PacCFIPass();
  virtual ~PacCFIPass();

  virtual StringRef getPassName() const override;
  virtual bool doInitialization(Module &M) override;
  virtual bool doFinalization(Module &M) override;

  bool runOnMachineFunction(MachineFunction &F) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;

private:
  void insertPacUpdates(MachineFunction &F);
  std::map<const MachineBasicBlock *, bool> instrumentCalls(MachineFunction &F);
  std::map<MSTEdge, EdgeValue>
  determineUpdateEdges(MachineFunction &F,
                       std::map<const MachineBasicBlock *, bool> callBlocks);
  void insertUpdates(std::map<MSTEdge, EdgeValue> &edges, MachineFunction &F);
  void insertCfiChecks(MachineFunction &F);
  void insertCfiCheck(MachineFunction &F, MachineBasicBlock &bb,
                      MachineBasicBlock::instr_iterator &it);

  MachineBasicBlock *splitBlock(MachineBasicBlock &mbb,
                                MachineBasicBlock::instr_iterator &it);
  MachineBasicBlock *splitEdge(MachineBasicBlock *bb, MachineBasicBlock *succ);
  void mergeBasicBlocks(MachineFunction &F);
  MachineBasicBlock *getBranchDestBlock(const MachineInstr &MI) const;

  MachineBasicBlock *addSignatureUpdate(MachineBasicBlock *bb,
                                        MachineBasicBlock *succ);
  MachineBasicBlock *addSignatureUpdate(MachineBasicBlock *sigUpdateBB,
                                        MachineBasicBlock::instr_iterator &it,
                                        PatchType patch_type,
                                        StringRef CallTarget);
  void addBranchMetadata(MachineFunction &F);
  void addBBMetadata(MachineFunction &F);
};

template <class T> class Translator {
public:
  Translator() = default;
  virtual ~Translator() = default;

  /// @brief Resets the translator and discards all mappings
  void reset();

  /// @brief Translates the input parameter using all internal mappings
  ///
  /// When no mapping could be found than the input itself is returned.
  ///
  /// @param input The parameter which should be translated
  /// @return The translated parameter
  const T &operator[](const T &input);

  /// @brief Introduces a new mapping rule
  /// @param from The mappings source parameter
  /// @param to The mappings destination parameter
  void map(const T &from, const T &to);

private:
  std::map<T, T> mapping_;
};

template <class T> inline void Translator<T>::reset() { mapping_.clear(); }

template <class T> inline const T &Translator<T>::operator[](const T &input) {
  auto MIE = mapping_.end();
  auto MI = mapping_.find(input);

  if (MI != MIE) {
    return MI->second;
  }
  return input;
}

template <class T> inline void Translator<T>::map(const T &from, const T &to) {
  if (from != to) {
    mapping_[from] = to;
  }
}

} /* namespace llvm */

#endif


#include "AArch64.h"
#include "AArch64InstrInfo.h"
#include "AArch64MachineFunctionInfo.h"
#include "AArch64RegisterInfo.h"
#include "MCTargetDesc/AArch64AddressingModes.h"

#include "PacCFIPass.h"

#include "llvm/ADT/SmallSet.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineJumpTableInfo.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/Support/GraphWriter.h"
#include "llvm/Support/raw_ostream.h"

#include <set>

using namespace llvm;

namespace {
// Write a graphviz file for the CFG inside an MCFunction.
void emitDOTFile(const std::string &FileName, const MachineFunction &f) {
  std::error_code Error;
  raw_fd_ostream Out(FileName.c_str(), Error, sys::fs::OpenFlags::F_None);
  WriteGraph(Out, &f, false);
}

void emitDOTFile(std::string baseName, uint32_t nr, std::string comment,
                 const MachineFunction &f) {
  std::string name;
  raw_string_ostream strm(name);
  strm << format("CFG_%s_%d_%s.dot", baseName.c_str(), nr, comment.c_str());
  strm.flush();
  emitDOTFile(name, f);
}
} // namespace

// TODO Dirty hack ...
std::map<MSTEdge, EdgeValue> globalEdges;

namespace llvm {

cl::opt<bool> insertEorPac(
    "insert-eor-pac",
    cl::desc("Enables EOR based PAC emulation for performance testing."),
    cl::init(false));

cl::opt<std::string>
    insertCfiCheckOpt("insert-cfi-check",
                      cl::desc("Insert a CFI check. Valid options: bb (basic "
                               "block), fend (function end)."),
                      cl::init(""));

template <>
struct DOTGraphTraits<const MachineFunction *> : public DefaultDOTGraphTraits {

  DOTGraphTraits(bool isSimple = false) : DefaultDOTGraphTraits(isSimple) {}

  static std::string getGraphName(const MachineFunction *F) {
    return "CFG for '" + F->getName().str() + "' function";
  }

  std::string getNodeLabel(const MachineBasicBlock *Node,
                           const MachineFunction *Graph) {
    std::string OutStr;
    {
      raw_string_ostream OSS(OutStr);

      OSS << "BB#" << Node->getNumber();
      if (const BasicBlock *BB = Node->getBasicBlock())
        OSS << ": " << BB->getName();
      OSS << '\n';
      if (!isSimple()) {
        for (MachineBasicBlock::const_iterator II = Node->begin(),
                                               IE = Node->end();
             II != IE; ++II) {
          OSS << ": " << *II;
        }
      }
    }

    if (OutStr[0] == '\n')
      OutStr.erase(OutStr.begin());

    // Process string output to make it nicer...
    for (unsigned i = 0; i != OutStr.length(); ++i)
      if (OutStr[i] == '\n') { // Left justify
        OutStr[i] = '\\';
        OutStr.insert(OutStr.begin() + i + 1, 'l');
      }
    return OutStr;
  }

  static std::string
  getEdgeAttributes(const MachineBasicBlock *BB,
                    MachineBasicBlock::const_succ_iterator iter,
                    const MachineFunction *F) {
    auto edgeIt = globalEdges.find(std::make_pair(BB, *iter));
    if (edgeIt == globalEdges.end())
      return "";
    std::string weightStr;
    raw_string_ostream strm(weightStr);
    strm << "label=\"" << edgeIt->second.weight << "\"";
    strm.flush();
    switch (edgeIt->second.update) {
    case EdgeValue::UPDATE:
      return weightStr + ",color=red";
    default:
      return weightStr;
    }
  }
};

} // namespace llvm

PacCFIPass::PacCFIPass() : MachineFunctionPass(ID) {}

PacCFIPass::~PacCFIPass() {}

StringRef PacCFIPass::getPassName() const {
  return "---### PAC CFI Pass ###---";
}

bool PacCFIPass::doInitialization(Module &M) { return false; }

bool PacCFIPass::doFinalization(Module &M) { return false; }

bool PacCFIPass::runOnMachineFunction(MachineFunction &F) {
  uint32_t dotNr = 0;
  emitDOTFile(F.getName().str(), dotNr++, "in", F);

  // Split up basic blocks with 3 terminator instructions
  SmallVector<std::pair<MachineBasicBlock *, MachineBasicBlock::instr_iterator>,
              4>
      work_list;
  for (MachineBasicBlock &bb : F) {
    auto S = std::distance(bb.terminators().begin(), bb.terminators().end());
    if (S == 3) {
      MachineBasicBlock::instr_iterator terminator =
          std::next(bb.getFirstInstrTerminator());

      work_list.push_back(std::make_pair(&bb, terminator));
    }
  }
  for (auto E : work_list) {
    splitBlock(*E.first, E.second);
  }

  insertPacUpdates(F);
  emitDOTFile(F.getName().str(), dotNr++, "afterPAC", F);

  std::map<const MachineBasicBlock *, bool> callBlocks = instrumentCalls(F);
  emitDOTFile(F.getName().str(), dotNr++, "instrumentCalls", F);

  std::map<MSTEdge, EdgeValue> edges = determineUpdateEdges(F, callBlocks);
  globalEdges = edges;
  emitDOTFile(F.getName().str(), dotNr++, "updateEdges", F);

  insertUpdates(edges, F);
  edges.clear();
  globalEdges.clear();
  F.RenumberBlocks(nullptr);
  emitDOTFile(F.getName().str(), dotNr++, "insertUpdates", F);

  mergeBasicBlocks(F);
  F.RenumberBlocks(nullptr);
  emitDOTFile(F.getName().str(), dotNr++, "mergeBB", F);

  insertCfiChecks(F);
  emitDOTFile(F.getName().str(), dotNr++, "insertCfiChecks", F);

  addBranchMetadata(F);
  addBBMetadata(F);

  {
    std::string Filename = "CFG." + F.getName().str() + ".mi";
    std::error_code ErrorInfo;
    raw_fd_ostream File(Filename.c_str(), ErrorInfo,
                        sys::fs::OpenFlags::F_None);
    F.print(File);
  }

  // F.verify(this);
  return true;
}

void PacCFIPass::addBranchMetadata(MachineFunction &F) {
  for (MachineBasicBlock &MBB : F) {
    for(MachineInstr &MI : MBB) {
      if(MI.isConditionalBranch()) {
        MI.storeConditionalBranch();
      } else if(MI.isBranch()) {
        MI.storeBranch();
      }
    }
  }
}

void PacCFIPass::addBBMetadata(MachineFunction &F) {
  for (MachineBasicBlock &MBB : F) {
    // Append BB Predecessors at the beginning of the first instruction
    MachineInstr &MI = *MBB.instr_begin();

    for(auto Pred : MBB.predecessors()) {
      MI.predecessors.push_back(Pred);
    }
  }
}

void PacCFIPass::insertCfiCheck(MachineFunction &F, MachineBasicBlock &bb,
                                MachineBasicBlock::instr_iterator &it) {
  MachineRegisterInfo &MRI = F.getRegInfo();
  const TargetInstrInfo &TII = *F.getSubtarget().getInstrInfo();
  Register PatchReg = MRI.createVirtualRegister(&AArch64::GPR64RegClass);
  auto DL = DebugLoc();

  MachineInstr *PatchInst = BuildMI(bb, it, DL, TII.get(AArch64::MOVZXi))
                                .addReg(PatchReg, RegState::Define)
                                .addImm(0)
                                .addImm(48);
  PatchInst->storeCfiCheck();

  BuildMI(bb, it, DebugLoc(), TII.get(AArch64::EORXrs))
      .addReg(PatchReg)
      .addReg(PatchReg)
      .addReg(AArch64::X28)
      .addImm(0);

  if (insertEorPac) {
    // Performance evaluation sequence
    BuildMI(bb, it, DL, TII.get(AArch64::EORXri))
        .addReg(AArch64::X28)
        .addReg(AArch64::X28)
        .addImm(2);
    BuildMI(bb, it, DL, TII.get(AArch64::EORXri))
        .addReg(AArch64::X28)
        .addReg(AArch64::X28)
        .addImm(3);
    BuildMI(bb, it, DL, TII.get(AArch64::EORXri))
        .addReg(AArch64::X28)
        .addReg(AArch64::X28)
        .addImm(5);
    BuildMI(bb, it, DL, TII.get(AArch64::EORXrs))
        .addReg(AArch64::X28)
        .addReg(AArch64::X28)
        .addReg(PatchReg, RegState::Kill)
        .addImm(0);
  } else {
    MRI.constrainRegClass(PatchReg, &AArch64::GPR64spRegClass);
    BuildMI(bb, it, DL, TII.get(AArch64::AUTIA))
        .addReg(AArch64::X28)
        .addReg(PatchReg, RegState::Kill);
    // Alternative Eor based approach without PAC
    // BuildMI(MBB, it, DebugLoc(), TII.get(AArch64::EORXrs))
    //   .addReg(AArch64::X28)
    //   .addReg(AArch64::X28)
    //   .addReg(PcReg, RegState::Kill)
    //   .addImm(0);
  }
}

void PacCFIPass::insertCfiChecks(MachineFunction &F) {
  if (insertCfiCheckOpt == "bb") {
    for (MachineBasicBlock &bb : F) {
      MachineBasicBlock::instr_iterator it = bb.getFirstInstrTerminator();
      insertCfiCheck(F, bb, it);
    }
  } else if (insertCfiCheckOpt == "fend") {
    for (MachineBasicBlock &bb : F) {
      for (MachineInstr &inst : bb) {
        if (inst.isReturn()) {
          MachineBasicBlock::instr_iterator it(inst);
          insertCfiCheck(F, bb, it);
        }
      }
    }
  }
}

std::map<const MachineBasicBlock *, bool>
PacCFIPass::instrumentCalls(MachineFunction &F) {
  std::map<const MachineBasicBlock *, bool> callBlocks;
  std::vector<MachineInstr *> calls;

  for (MachineBasicBlock &bb : F) {
    for (MachineInstr &II : bb) {
      if (!II.isCall())
        continue;
      calls.push_back(&II);
    }
  }

  for (MachineInstr *inst : calls) {
    MachineBasicBlock *bb = inst->getParent();
    MachineBasicBlock::instr_iterator instIt = bb->instr_end();

    // split in front of the ADJCALLSTACKDOWN instruction
    for (MachineBasicBlock::reverse_instr_iterator it(inst);
         it != bb->instr_rend(); ++it) {
      if (it->getOpcode() == AArch64::ADJCALLSTACKDOWN) {
        instIt = (MachineBasicBlock::instr_iterator) & *it;
        break;
      }
    }
    if (instIt == bb->instr_end()) {
      dbgs() << "splitting directly in front of call\n";
      instIt = (MachineBasicBlock::instr_iterator)inst;
    }

    const MachineOperand &op = inst->getOperand(0);
    if (!op.isGlobal() && !op.isSymbol()) {
      addSignatureUpdate(bb, instIt, ICALL_PATCH , StringRef(""));

      // If we first have an indirect call, we need to patch after
      instIt = (MachineBasicBlock::instr_iterator)inst;
      for (; instIt != bb->instr_end(); ++instIt) {
        if (instIt->getOpcode() == AArch64::ADJCALLSTACKUP)
          break;
      }
      ++instIt;

      addSignatureUpdate(bb, instIt, ICALL_PATCH, StringRef(""));
    } else {
      // We had a direct call, store it to the metadata
      StringRef Target;
      if (op.isSymbol())
        Target = op.getSymbolName();
      else
        Target = op.getGlobal()->getName();

      addSignatureUpdate(bb, instIt, CALL_PATCH, Target);
      inst->storeDefiniteCall(Target);
    }
  }
  return callBlocks;
}

MachineBasicBlock *
PacCFIPass::getBranchDestBlock(const MachineInstr &MI) const {
  switch (MI.getOpcode()) {
  default:
    return nullptr;
  case AArch64::B:
    return MI.getOperand(0).getMBB();
  case AArch64::TBZW:
  case AArch64::TBNZW:
  case AArch64::TBZX:
  case AArch64::TBNZX:
    return MI.getOperand(2).getMBB();
  case AArch64::CBZW:
  case AArch64::CBNZW:
  case AArch64::CBZX:
  case AArch64::CBNZX:
  case AArch64::Bcc:
    return MI.getOperand(1).getMBB();
  }
}

// NOTE a similar function can be found in
// BranchRelaxation::splitBlockBeforeInstr
MachineBasicBlock *
PacCFIPass::splitBlock(MachineBasicBlock &mbb,
                       MachineBasicBlock::instr_iterator &it) {
  MachineFunction &F = *mbb.getParent();

  // remember if a fallthrough to the next MBB possible
  bool couldFallthrough = mbb.canFallThrough();

  // create new MBB after the current MBB
  MachineBasicBlock *newBB = F.CreateMachineBasicBlock(mbb.getBasicBlock());
  F.insert(std::next(MachineFunction::iterator(&mbb)), newBB);
  assert(mbb.isLayoutSuccessor(newBB));

  // move the machine instructions, starting with it, into the new MBB
  newBB->splice(newBB->end(), &mbb, it, mbb.end());

  // setup the basic successors
  while (mbb.succ_size())
    mbb.removeSuccessor(mbb.succ_begin());
  mbb.addSuccessor(newBB);
  if (couldFallthrough)
    newBB->addSuccessor(&*std::next(MachineFunction::iterator(newBB)));

  // fixup the successor lists of the MBB using the remaining terminators
  MachineBasicBlock::instr_iterator MI = mbb.getFirstInstrTerminator();
  for (auto ME = mbb.instr_end(); MI != ME; ++MI) {
    MachineBasicBlock *target = getBranchDestBlock(*MI);
    if (target)
      mbb.addSuccessor(target);
  }

  // fixup successor list of the new MBB using the moved terminators
  MI = newBB->getFirstInstrTerminator();
  for (auto ME = newBB->instr_end(); MI != ME; ++MI) {
    MachineBasicBlock *target = getBranchDestBlock(*MI);
    if (target && !newBB->isSuccessor(target)) {
      newBB->addSuccessor(target);
    }
  }

  // fixup live ins in the new MBB
  SmallSet<unsigned, 16> definedPhysRegs;
  for (auto MI = newBB->instr_begin(); MI != newBB->instr_end(); ++MI) {
    ConstMIBundleOperands opIt(*MI);
    for (; opIt.isValid(); ++opIt) {
      const MachineOperand &MO = *opIt;
      if (!MO.isReg())
        continue;

      unsigned MOReg = MO.getReg();
      if (!Register::isPhysicalRegister(MOReg))
        continue;

      if (MO.isDef()) {
        definedPhysRegs.insert(MOReg);
        continue;
      }

      if (!MO.readsReg() || definedPhysRegs.count(MOReg))
        continue;
      newBB->addLiveIn(MOReg);
    }
  }
  newBB->sortUniqueLiveIns();

  // fixup PHI nodes in the successors
  for (auto SI = newBB->succ_begin(), SE = newBB->succ_end(); SI != SE; ++SI) {
    for (MachineBasicBlock::instr_iterator MI = (*SI)->instr_begin(),
                                           ME = (*SI)->instr_end();
         MI != ME && MI->isPHI(); ++MI) {
      if (mbb.isSuccessor(*SI))
        assert(false && "Currently not implemented!");

      for (unsigned i = 2, e = MI->getNumOperands() + 1; i < e; i += 2) {
        MachineOperand &MO = MI->getOperand(i);
        if (MO.getMBB() == &mbb)
          MO.setMBB(newBB);
      }
    }
  }

  return newBB;
}

void PacCFIPass::mergeBasicBlocks(MachineFunction &F) {
  MachineFunction::iterator BI = F.begin();
  while (BI != F.end()) {
    // skip blocks which can not be merged
    auto succIt = BI->succ_begin();
    if (BI->succ_size() != 1 || (*succIt)->pred_size() != 1) {
      ++BI;
      continue;
    }

    MachineBasicBlock *succ = *succIt;
    MachineBasicBlock *bb = &*BI;

    // make sure that the branch of the successor is analyzable
    // (requirement for updateTerminator)
    const TargetInstrInfo &TII = *F.getSubtarget().getInstrInfo();
    MachineBasicBlock *TBB = nullptr, *FBB = nullptr;
    SmallVector<MachineOperand, 4> Cond;
    if (!succ->succ_empty() && TII.analyzeBranch(*succ, TBB, FBB, Cond)) {
      bb->updateTerminator();
      ++BI;
      continue;
    }

    // move the MBBs next to each other and fix the terminators
    succ->moveAfter(bb);
    succ->updateTerminator();
    bb->updateTerminator();

    // move instructions from the successor
    bb->splice(bb->end(), succ, succ->begin(), succ->end());

    bb->removeSuccessor(succ);
    bb->transferSuccessorsAndUpdatePHIs(succ);
    F.remove(succ);
    F.DeleteMachineBasicBlock(succ);

    // restart from the beginning
    BI = F.begin(); // not the fastest solution but it definitely works
  }
}

MachineBasicBlock *PacCFIPass::splitEdge(MachineBasicBlock *bb,
                                         MachineBasicBlock *succ) {

  MachineBasicBlock *newBB = bb->SplitCriticalEdge(succ, *this);
  MachineBasicBlock::iterator terminator = bb->getFirstTerminator();

  // Determine if we have a table branch
  bool tableBranch = false;
  if (!newBB) {
    MachineFunction &F = *bb->getParent();
    MachineRegisterInfo &MRI = F.getRegInfo();
    MachineInstr *JTDest = MRI.getVRegDef(terminator->getOperand(0).getReg());

    if (terminator->getOpcode() == AArch64::BR &&
        (JTDest->getOpcode() == AArch64::JumpTableDest8 ||
         JTDest->getOpcode() == AArch64::JumpTableDest16 ||
         JTDest->getOpcode() == AArch64::JumpTableDest32)) {
      // try to split a jump table
      tableBranch = true;
      unsigned JTIdx = JTDest->getOperand(4).getIndex();
      MachineJumpTableInfo *JTI = bb->getParent()->getJumpTableInfo();

      newBB = F.CreateMachineBasicBlock(succ->getBasicBlock());
      F.insert(F.end(), newBB);
      newBB->addSuccessor(succ);
      bb->replaceSuccessor(succ, newBB);
      JTI->ReplaceMBBInJumpTable(JTIdx, succ, newBB);
      newBB->updateTerminator();

      // Fix up any PHI nodes in the successor.
      for (MachineBasicBlock::instr_iterator MI = succ->instr_begin(),
                                             ME = succ->instr_end();
           MI != ME && MI->isPHI(); ++MI) {
        for (unsigned i = 2, e = MI->getNumOperands() + 1; i != e; i += 2) {
          MachineOperand &MO = MI->getOperand(i);
          if (MO.getMBB() == bb)
            MO.setMBB(newBB);
        }
      }
    }
  }

  if (!newBB) {
    errs() << "splitting of edge from " << bb->getFullName() << " to "
           << succ->getFullName() << " failed!";
    llvm_unreachable("Could not split edge");
  }

  MachineBasicBlock *newPred = *newBB->pred_begin();
  MachineBasicBlock *newSucc = *newBB->succ_begin();

  if (!tableBranch && newSucc->pred_size() == 1) {
    newBB->moveBefore(newSucc);
    newBB->updateTerminator();
    newPred->updateTerminator();
  }
  return newBB;
}

std::map<MSTEdge, EdgeValue> PacCFIPass::determineUpdateEdges(
    MachineFunction &F, std::map<const MachineBasicBlock *, bool> callBlocks) {
  std::map<MSTEdge, EdgeValue> edges;
  MachineBasicBlock *exitBlock = nullptr;

  // search all graph edges and give them default weights and status
  for (MachineBasicBlock &bb : F) {
    MachineBasicBlock::succ_iterator SI = bb.succ_begin(), SIE = bb.succ_end();
    if (SI == SIE) {
      if (!exitBlock) {
        exitBlock = &bb;
        continue;
      }
      // insert invalid edges between the exit blocks to make sure that
      // multiple exit points are properly patched
      MSTEdge edge = std::make_pair(&bb, exitBlock);
      edges[edge].update = EdgeValue::INVALID;
      edges[edge].weight = 100.0;
      continue;
    }
    for (; SI != SIE; ++SI) {
      MSTEdge edge = std::make_pair(&bb, *SI);

      // Add penalty for conditional branches, which have more than one
      // successor
      edges[edge].update = EdgeValue::UPDATE;
      if (bb.succ_size() > 1) {
        edges[edge].weight = 30;
      } else {
        edges[edge].weight = 1;
      }
    }
  }

  // build the edge vector for the spanning tree calculation
  MaxSpanTree::EdgeWeights edgeVector;
  for (const auto &pair : edges) {
    const MSTEdge &edge = pair.first;
    edgeVector.push_back(std::make_pair(edge, edges[edge].weight));
  }

  MaxSpanTree MST(edgeVector);

  // Invert MST
  for (MaxSpanTree::Edge &mstEdge : MST) {
    EdgeValue::UpdateType &type = edges[mstEdge].update;

    switch (type) {
    case EdgeValue::UPDATE:
      type = EdgeValue::NO_UPDATE;
      break;
    case EdgeValue::INVALID:
      edges.erase(mstEdge);
      break;
    default:
      llvm_unreachable("This should not happen...\n");
    }
  }

  return edges;
}

void PacCFIPass::insertUpdates(std::map<MSTEdge, EdgeValue> &edges,
                               MachineFunction &F) {
  // apply the update
  std::vector<MSTEdge> postCall;
  std::map<MachineBasicBlock *, std::pair<unsigned, unsigned>>
      signatureStackingValues;
  Translator<MachineBasicBlock *> map;

  for (const auto &edge : edges) {
    if (edge.second.update == EdgeValue::NO_UPDATE)
      continue;

    MachineBasicBlock *origSrc =
        const_cast<MachineBasicBlock *>(edge.first.first);
    MachineBasicBlock *origDst =
        const_cast<MachineBasicBlock *>(edge.first.second);
    MachineBasicBlock *updateBB = nullptr;

    switch (edge.second.update) {
    case EdgeValue::UPDATE:
      updateBB = addSignatureUpdate(map[origSrc], map[origDst]);
      map.map(origSrc, *updateBB->pred_begin());
      map.map(origDst, *updateBB->succ_begin());
      break;

    default:
      llvm_unreachable("This should not happen...\n");
    }
  }
}

void PacCFIPass::insertPacUpdates(MachineFunction &F) {
  const TargetInstrInfo &TII = *F.getSubtarget().getInstrInfo();
  MachineRegisterInfo &MRI = F.getRegInfo();

  for (MachineBasicBlock &MBB : F) {
    Register PcReg = MRI.createVirtualRegister(&AArch64::GPR64RegClass);
    auto it = MBB.instr_begin();

    // Skip possible phi nodes
    while (it != MBB.instr_end() && (it->getOpcode() == TargetOpcode::PHI ||
                                     it->getOpcode() == TargetOpcode::G_PHI))
      it++;

    MachineInstr *ConstGenInst = BuildMI(MBB, it, DebugLoc(), TII.get(AArch64::ADR))
                                .addReg(PcReg, RegState::Define)
                                .addImm(0);
    ConstGenInst->storeStateUpdate();

    if (insertEorPac) {
      // Performance evaluation sequence
      BuildMI(MBB, it, DebugLoc(), TII.get(AArch64::EORXri))
          .addReg(AArch64::X28)
          .addReg(AArch64::X28)
          .addImm(2);
      BuildMI(MBB, it, DebugLoc(), TII.get(AArch64::EORXri))
          .addReg(AArch64::X28)
          .addReg(AArch64::X28)
          .addImm(3);
      BuildMI(MBB, it, DebugLoc(), TII.get(AArch64::EORXri))
          .addReg(AArch64::X28)
          .addReg(AArch64::X28)
          .addImm(5);
      BuildMI(MBB, it, DebugLoc(), TII.get(AArch64::EORXrs))
          .addReg(AArch64::X28)
          .addReg(AArch64::X28)
          .addReg(PcReg, RegState::Kill)
          .addImm(0);
    } else {
      MRI.constrainRegClass(PcReg, &AArch64::GPR64spRegClass);
      MachineInstr* UpdateInst = BuildMI(MBB, it, DebugLoc(), TII.get(AArch64::PACIA))
                                          .addReg(AArch64::X28)
                                          .addReg(PcReg, RegState::Kill);
      // Alternative Eor based approach without PAC
      // BuildMI(MBB, it, DebugLoc(), TII.get(AArch64::EORXrs))
      //   .addReg(AArch64::X28)
      //   .addReg(AArch64::X28)
      //   .addReg(PcReg, RegState::Kill)
      //   .addImm(0);
    }
  }
}

MachineBasicBlock *PacCFIPass::addSignatureUpdate(MachineBasicBlock *bb,
                                                  MachineBasicBlock *succ) {
  MachineBasicBlock *sigUpdateBB = splitEdge(bb, succ);
  MachineBasicBlock::instr_iterator it = sigUpdateBB->instr_begin();

  return addSignatureUpdate(sigUpdateBB, it, STATE_PATCH, StringRef(""));
}

MachineBasicBlock *
PacCFIPass::addSignatureUpdate(MachineBasicBlock *sigUpdateBB,
                               MachineBasicBlock::instr_iterator &it,
                               PatchType patch_type,
                               StringRef CallTarget) {
  MachineFunction &F = *sigUpdateBB->getParent();
  MachineRegisterInfo &MRI = F.getRegInfo();
  const TargetInstrInfo &TII = *F.getSubtarget().getInstrInfo();

  Register dataReg = MRI.createVirtualRegister(&AArch64::GPR64RegClass);
  MachineInstr *PatchInst =
      BuildMI(*sigUpdateBB, it, DebugLoc(), TII.get(AArch64::MOVZXi))
          .addReg(dataReg, RegState::Define)
          .addImm(0)
          .addImm(48);
  BuildMI(*sigUpdateBB, it, DebugLoc(), TII.get(AArch64::EORXrs))
      .addReg(AArch64::X28)
      .addReg(AArch64::X28)
      .addReg(dataReg, RegState::Kill)
      .addImm(0);

  if(patch_type == STATE_PATCH) {
    PatchInst->storeStatePatch();
  } else if(patch_type == CALL_PATCH) {
    PatchInst->storeCallPatch(CallTarget);
  } else if(patch_type == ICALL_PATCH) {
    PatchInst->storeIcallPatch();
  }

  return sigUpdateBB;
}

void PacCFIPass::getAnalysisUsage(AnalysisUsage &AU) const {
  MachineFunctionPass::getAnalysisUsage(AU);
}

char PacCFIPass::ID = 0;

FunctionPass *llvm::createPacCFIPass() { return new PacCFIPass(); }

INITIALIZE_PASS(PacCFIPass, "PacCFIPass", "PAC CFI Pass", false, false)

#ifndef CFI_METADATA_HANDLER_H
#define CFI_METADATA_HANDLER_H

#include "llvm/BinaryFormat/ELF.h"
#include "llvm/CodeGen/AsmPrinter.h"
#include "llvm/CodeGen/AsmPrinterHandler.h"
#include "llvm/CodeGen/CFIMetadataDefinitions.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/IR/Instruction.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCSectionELF.h"
#include "llvm/MC/MCStreamer.h"

namespace llvm {

struct CFISymbolPairCondBrProt {
  MCSymbol *Sym;
  MCSymbol *Target;
  uint32_t TrueValue;
  uint32_t FalseValue;
};

struct CFISymbolPairBr {
   MCSymbol *Sym;
   MCSymbol *Successor;
};

struct CFIDefiniteCall {
  MCSymbol *Sym;
  StringRef CallTarget;
};

struct CFIPredecesssor {
  const MCSymbol *Sym;
  const MCSymbol *Predecessor;
};

struct CFIConfig {
  StringRef Section;
  MCSymbol *Start;
  MCSymbol *End;
  const MCSymbolELF *SectionSym;
  StringRef FnName;
  // Symbols of each class
  SmallVector<CFISymbolPairBr, 5> CondBranches;
  SmallVector<CFISymbolPairBr, 5> Branches;
  SmallVector<CFIDefiniteCall, 5> DefiniteCalls;
  SmallVector<MCSymbol *, 5> Returns;
  SmallVector<CFIPredecesssor, 5> Predecessors;
  SmallVector<CFIDefiniteCall, 5> CallPatches;
  SmallVector<MCSymbol *, 5> ICallPatches;
  SmallVector<MCSymbol *, 5> StateUpdates;
  SmallVector<MCSymbol *, 5> StatePatches;
  SmallVector<MCSymbol *, 5> BranchOverPatches;
  SmallVector<MCSymbol *, 5> CfiChecks;
  SmallVector<uint32_t, 1> NrMBBs;
};

class AsmPrinter;


template <class TargetFunctionInfo>
class CFIMetadataHandler : public AsmPrinterHandler {
private:
  AsmPrinter *Asm;
  const TargetFunctionInfo *MFI;
  SmallVector<CFIConfig *, 5> Configs;
  CFIConfig *CurrentConfig;
  std::map<const MachineBasicBlock*, const MCSymbol*> MbbSyms;
  std::map<const MachineInstr*, const MCSymbol*> MbbTargets;

  void populateSectionsToEmit(
      SmallVector<std::pair<StringRef, const MCSymbolELF *>, 5>
          &SectionsToEmit) {
    for (auto C : Configs) {
      bool found = false;
      for (auto S : SectionsToEmit) {
        if (S.first == C->Section) {
          found = true;
        }
      }
      if (!found) {
        SectionsToEmit.emplace_back(C->Section, C->SectionSym);
      }
    }
  }

public:
  CFIMetadataHandler(AsmPrinter *A, Module *M) : Asm(A), MFI() {}

  void setSymbolSize(const MCSymbol *Sym, uint64_t Size) override {}

  void beginFunction(const MachineFunction *MF) override {
    // Get the section and its name
    const MCSection *Section = Asm->getCurrentSection();
    const MCSectionELF *ELFSection = cast<MCSectionELF>(Section);

    MFI = MF->getInfo<TargetFunctionInfo>();
    CurrentConfig = new CFIConfig();
    MCSymbol *Sym = Asm->createTempSymbol("cfi_begin");
    Asm->OutStreamer->emitLabel(Sym);
    CurrentConfig->Section = ELFSection->getName();
    CurrentConfig->SectionSym = cast<MCSymbolELF>(Section->getBeginSymbol());
    CurrentConfig->Start = Sym;
    CurrentConfig->FnName = MF->getName();

    Configs.push_back(CurrentConfig);
  }

  void endFunction(const MachineFunction *MF) override {
    for (const MachineBasicBlock &MBB : *MF) {
      for(const MachineInstr &MI : MBB) {
        if(MI.predecessors.size()) {
          const MCSymbol* SrcSymbol = MbbTargets[&MI];
          for(MachineBasicBlock* Pred : MI.predecessors) {
            auto PredSymbol = MbbSyms.find(Pred);
            if(PredSymbol == MbbSyms.end()) {
              llvm_unreachable("Did not find Pred Symbol");
            }
            CurrentConfig->Predecessors.push_back(
              {SrcSymbol, PredSymbol->second});
          }
        }
      }
    }

    MCSymbol *Sym = Asm->createTempSymbol("cfi_end");
    Asm->OutStreamer->emitLabel(Sym);
    CurrentConfig->End = Sym;
  }

  void emitBasicBlockEnd(const MachineBasicBlock &MBB, const MCSymbol *sym) {
    MbbSyms[&MBB] = sym;
  }

  void beginInstruction(const MachineInstr *MI) override {
    if(MI->predecessors.size()) {
      MCSymbol *MbbTargetLabel =
          Asm->OutContext.createTempSymbol("mbb_target", true);
      Asm->OutStreamer->emitLabel(MbbTargetLabel);
      MbbTargets[MI] = MbbTargetLabel;
    }

    switch (MI->MD.getType()) {
    default:
      // Do nothing
      break;
    case CfiMIMetadata::DEFINITE_CALL: {
      MCSymbol *CallInstLabel =
          Asm->OutContext.createTempSymbol("cfi_definite_call", true);
      Asm->OutStreamer->emitLabel(CallInstLabel);
      CurrentConfig->DefiniteCalls.push_back(
          {CallInstLabel, MI->MD.CallTarget});
    } break;
    case CfiMIMetadata::FUNC_RETURN: {
      MCSymbol *ReturnLabel =
          Asm->OutContext.createTempSymbol("cfi_func_return", true);
      Asm->OutStreamer->emitLabel(ReturnLabel);
      CurrentConfig->Returns.push_back(ReturnLabel);
    } break;
    case CfiMIMetadata::CALL_PATCH: {
      MCSymbol *CallUpdateLabel =
          Asm->OutContext.createTempSymbol("cfi_call_patch", true);
      Asm->OutStreamer->emitLabel(CallUpdateLabel);
      CurrentConfig->CallPatches.push_back(
          {CallUpdateLabel, MI->MD.CallTarget});
    } break;
    case CfiMIMetadata::ICALL_PATCH: {
      MCSymbol *ICallUpdateLabel =
          Asm->OutContext.createTempSymbol("cfi_icall_patch", true);
      Asm->OutStreamer->emitLabel(ICallUpdateLabel);
      CurrentConfig->ICallPatches.push_back(ICallUpdateLabel);
    } break;
    case CfiMIMetadata::STATE_UPDATE: {
      MCSymbol *StateUpdateLabel =
          Asm->OutContext.createTempSymbol("cfi_state_update", true);
      Asm->OutStreamer->emitLabel(StateUpdateLabel);
      CurrentConfig->StateUpdates.push_back(StateUpdateLabel);
    } break;
    case CfiMIMetadata::STATE_PATCH: {
      MCSymbol *StatePatchLabel =
          Asm->OutContext.createTempSymbol("cfi_state_patch", true);
      Asm->OutStreamer->emitLabel(StatePatchLabel);
      CurrentConfig->StatePatches.push_back(StatePatchLabel);
    } break;
    case CfiMIMetadata::BRANCH_OVER_PATCH: {
      MCSymbol *BranchOverPatchLabel =
          Asm->OutContext.createTempSymbol("cfi_branch_over_patch", true);
      Asm->OutStreamer->emitLabel(BranchOverPatchLabel);
      CurrentConfig->BranchOverPatches.push_back(BranchOverPatchLabel);
    } break;
    case CfiMIMetadata::CFI_CHECK: {
      MCSymbol *CfiCheckLabel =
          Asm->OutContext.createTempSymbol("cfi_check", true);
      Asm->OutStreamer->emitLabel(CfiCheckLabel);
      CurrentConfig->CfiChecks.push_back(CfiCheckLabel);
    } break;
    case CfiMIMetadata::BRANCH: {
      MCSymbol *CfiBranchLabel =
          Asm->OutContext.createTempSymbol("cfi_branch", true);
      Asm->OutStreamer->emitLabel(CfiBranchLabel);
      int idx = 0;
      do {
        const MachineOperand &MO = MI->getOperand(idx);
        if(MO.isMBB()) {
          CurrentConfig->Branches.push_back({CfiBranchLabel, MO.getMBB()->getSymbol()});
          break;
        }
      } while(idx++ < MI->getNumOperands());
    } break;
    case CfiMIMetadata::COND_BRANCH: {
      MCSymbol *CfiCondBranchLabel =
          Asm->OutContext.createTempSymbol("cfi_cond_branch", true);
      Asm->OutStreamer->emitLabel(CfiCondBranchLabel);
      int idx = 0;
      do {
        const MachineOperand &MO = MI->getOperand(idx);
        if(MO.isMBB()) {
          CurrentConfig->CondBranches.push_back({CfiCondBranchLabel, MO.getMBB()->getSymbol()});
          break;
        }
      } while(idx++ < MI->getNumOperands());
    } break;
    case CfiMIMetadata::MBB_STATISTIC: {
      MCSymbol *MbbStatLabel =
          Asm->OutContext.createTempSymbol("mbb_stat", true);
      Asm->OutStreamer->emitLabel(MbbStatLabel);
      CurrentConfig->NrMBBs.push_back(MI->MD.nr_mbbs);
    }
    break;
    }
  }

  void endInstruction() override {}

  void emitConfig(CFIConfig &C) {
    const MCExpr *expr;

    // Function begin
    Asm->OutStreamer->AddComment("Start Function " + C.FnName);
    Asm->OutStreamer->emitIntValue(CFI_MD_TYPE::FUNC_START, 2);
    expr = MCSymbolRefExpr::create(C.Start, Asm->OutContext);
    Asm->OutStreamer->emitValue(expr, 4);

    for (auto P : C.Predecessors) {
      Asm->OutStreamer->AddComment("  Predecessor");
      Asm->OutStreamer->emitIntValue(CFI_MD_TYPE::PREDECESSOR, 2);
      expr = MCSymbolRefExpr::create(P.Sym, Asm->OutContext);
      Asm->OutStreamer->emitValue(expr, 4);
      expr = MCSymbolRefExpr::create(P.Predecessor, Asm->OutContext);
      Asm->OutStreamer->emitValue(expr, 4);
    }

    for (auto S : C.DefiniteCalls) {
      Asm->OutStreamer->AddComment("  DefiniteCall");
      Asm->OutStreamer->emitIntValue(CFI_MD_TYPE::DEFINITE_CALL, 2);
      expr = MCSymbolRefExpr::create(S.Sym, Asm->OutContext);
      Asm->OutStreamer->emitValue(expr, 4);
      expr = MCSymbolRefExpr::create(
          S.CallTarget, MCSymbolRefExpr::VariantKind::VK_None, Asm->OutContext);
      Asm->OutStreamer->emitValue(expr, 4);
    }

    for (auto S : C.Returns) {
      Asm->OutStreamer->AddComment("  Returns");
      Asm->OutStreamer->emitIntValue(CFI_MD_TYPE::FUNC_RETURN, 2);
      expr = MCSymbolRefExpr::create(S, Asm->OutContext);
      Asm->OutStreamer->emitValue(expr, 4);
    }

    for (auto S : C.CallPatches) {
      Asm->OutStreamer->AddComment("  CallPatches");
      Asm->OutStreamer->emitIntValue(CFI_MD_TYPE::CALL_PATCH, 2);
      expr = MCSymbolRefExpr::create(S.Sym, Asm->OutContext);
      Asm->OutStreamer->emitValue(expr, 4);
      expr = MCSymbolRefExpr::create(
          S.CallTarget, MCSymbolRefExpr::VariantKind::VK_None, Asm->OutContext);
      Asm->OutStreamer->emitValue(expr, 4);
    }

    for (auto S : C.ICallPatches) {
      Asm->OutStreamer->AddComment("  ICallPatches");
      Asm->OutStreamer->emitIntValue(CFI_MD_TYPE::ICALL_PATCH, 2);
      expr = MCSymbolRefExpr::create(S, Asm->OutContext);
      Asm->OutStreamer->emitValue(expr, 4);
    }

    for (auto S : C.StateUpdates) {
      Asm->OutStreamer->AddComment("  StateUpdates");
      Asm->OutStreamer->emitIntValue(CFI_MD_TYPE::STATE_UPDATE, 2);
      expr = MCSymbolRefExpr::create(S, Asm->OutContext);
      Asm->OutStreamer->emitValue(expr, 4);
    }

    for (auto S : C.StatePatches) {
      Asm->OutStreamer->AddComment("  StatePatches");
      Asm->OutStreamer->emitIntValue(CFI_MD_TYPE::STATE_PATCH, 2);
      expr = MCSymbolRefExpr::create(S, Asm->OutContext);
      Asm->OutStreamer->emitValue(expr, 4);
    }

    for (auto S : C.BranchOverPatches) {
      Asm->OutStreamer->AddComment("  BranchOverPatches");
      Asm->OutStreamer->emitIntValue(CFI_MD_TYPE::BRANCH_OVER_PATCH, 2);
      expr = MCSymbolRefExpr::create(S, Asm->OutContext);
      Asm->OutStreamer->emitValue(expr, 4);
    }

    for (auto S : C.CfiChecks) {
      Asm->OutStreamer->AddComment("  CfiChecks");
      Asm->OutStreamer->emitIntValue(CFI_MD_TYPE::CFI_CHECK, 2);
      expr = MCSymbolRefExpr::create(S, Asm->OutContext);
      Asm->OutStreamer->emitValue(expr, 4);
    }

    for (auto S : C.CondBranches) {
      Asm->OutStreamer->AddComment("  CondBranches");
      Asm->OutStreamer->emitIntValue(CFI_MD_TYPE::COND_BRANCH, 2);
      expr = MCSymbolRefExpr::create(S.Sym, Asm->OutContext);
      Asm->OutStreamer->emitValue(expr, 4);
      expr = MCSymbolRefExpr::create(S.Successor, Asm->OutContext);
      Asm->OutStreamer->emitValue(expr, 4);
    }

    for (auto S : C.Branches) {
      Asm->OutStreamer->AddComment("  Branches");
      Asm->OutStreamer->emitIntValue(CFI_MD_TYPE::BRANCH, 2);
      expr = MCSymbolRefExpr::create(S.Sym, Asm->OutContext);
      Asm->OutStreamer->emitValue(expr, 4);
      expr = MCSymbolRefExpr::create(S.Successor, Asm->OutContext);
      Asm->OutStreamer->emitValue(expr, 4);
    }

    for (auto S : C.NrMBBs) {
      Asm->OutStreamer->AddComment("  MBBStat");
      Asm->OutStreamer->emitIntValue(CFI_MD_TYPE::MBB_STATISTIC, 2);
      Asm->OutStreamer->emitIntValue(S, 4);
    }

    Asm->OutStreamer->AddComment("  Function End");
    Asm->OutStreamer->emitIntValue(CFI_MD_TYPE::FUNC_END, 2);
    expr = MCSymbolRefExpr::create(C.End, Asm->OutContext);
    Asm->OutStreamer->emitValue(expr, 4);
  }

  void endModule() override {
    SmallVector<std::pair<StringRef, const MCSymbolELF *>, 5> SectionsToEmit;
    populateSectionsToEmit(SectionsToEmit);

    for (auto S : SectionsToEmit) {
      auto section_name = CFI_MD_SECTION_PREFIX + S.first.str();

      MCSectionELF *OutputSection = Asm->OutContext.getELFSection(
          section_name, ELF::SHT_PROGBITS,
          // SHF_LINK_ORDER ensures that we have a dependency on the
          // associated section
          ELF::SHF_LINK_ORDER,
          0,       // No fixed size
          "",      // No group name (gets deduplicated by the linker)
          0x12345, // Magic number
          S.second);

      Asm->OutStreamer->SwitchSection(OutputSection);
      Asm->OutStreamer->AddComment("SCE Debug section");
      Asm->OutStreamer->emitIntValue(CFI_MD_MAGIC_START, 4);

      for (auto C : Configs) {
        if (S.first == C->Section) {
          emitConfig(*C);
        }
      }
    }
  }
};

} // namespace llvm

#endif

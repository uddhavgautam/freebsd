//===-- MipsDelaySlotFiller.cpp - Mips Delay Slot Filler ------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Simple pass to fill delay slots with useful instructions.
//
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/MipsMCNaCl.h"
#include "Mips.h"
#include "MipsInstrInfo.h"
#include "MipsTargetMachine.h"
#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/CodeGen/MachineBranchProbabilityInfo.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/PseudoSourceValue.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Target/TargetInstrInfo.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetRegisterInfo.h"

using namespace llvm;

#define DEBUG_TYPE "delay-slot-filler"

STATISTIC(FilledSlots, "Number of delay slots filled");
STATISTIC(UsefulSlots, "Number of delay slots filled with instructions that"
                       " are not NOP.");

static cl::opt<bool> DisableDelaySlotFiller(
  "disable-mips-delay-filler",
  cl::init(false),
  cl::desc("Fill all delay slots with NOPs."),
  cl::Hidden);

static cl::opt<bool> DisableForwardSearch(
  "disable-mips-df-forward-search",
  cl::init(true),
  cl::desc("Disallow MIPS delay filler to search forward."),
  cl::Hidden);

static cl::opt<bool> DisableSuccBBSearch(
  "disable-mips-df-succbb-search",
  cl::init(true),
  cl::desc("Disallow MIPS delay filler to search successor basic blocks."),
  cl::Hidden);

static cl::opt<bool> DisableBackwardSearch(
  "disable-mips-df-backward-search",
  cl::init(false),
  cl::desc("Disallow MIPS delay filler to search backward."),
  cl::Hidden);

namespace {
  typedef MachineBasicBlock::iterator Iter;
  typedef MachineBasicBlock::reverse_iterator ReverseIter;
  typedef SmallDenseMap<MachineBasicBlock*, MachineInstr*, 2> BB2BrMap;

  class RegDefsUses {
  public:
    RegDefsUses(const TargetRegisterInfo &TRI);
    void init(const MachineInstr &MI);

    /// This function sets all caller-saved registers in Defs.
    void setCallerSaved(const MachineInstr &MI);

    /// This function sets all unallocatable registers in Defs.
    void setUnallocatableRegs(const MachineFunction &MF);

    /// Set bits in Uses corresponding to MBB's live-out registers except for
    /// the registers that are live-in to SuccBB.
    void addLiveOut(const MachineBasicBlock &MBB,
                    const MachineBasicBlock &SuccBB);

    bool update(const MachineInstr &MI, unsigned Begin, unsigned End);

  private:
    bool checkRegDefsUses(BitVector &NewDefs, BitVector &NewUses, unsigned Reg,
                          bool IsDef) const;

    /// Returns true if Reg or its alias is in RegSet.
    bool isRegInSet(const BitVector &RegSet, unsigned Reg) const;

    const TargetRegisterInfo &TRI;
    BitVector Defs, Uses;
  };

  /// Base class for inspecting loads and stores.
  class InspectMemInstr {
  public:
    InspectMemInstr(bool ForbidMemInstr_)
      : OrigSeenLoad(false), OrigSeenStore(false), SeenLoad(false),
        SeenStore(false), ForbidMemInstr(ForbidMemInstr_) {}

    /// Return true if MI cannot be moved to delay slot.
    bool hasHazard(const MachineInstr &MI);

    virtual ~InspectMemInstr() {}

  protected:
    /// Flags indicating whether loads or stores have been seen.
    bool OrigSeenLoad, OrigSeenStore, SeenLoad, SeenStore;

    /// Memory instructions are not allowed to move to delay slot if this flag
    /// is true.
    bool ForbidMemInstr;

  private:
    virtual bool hasHazard_(const MachineInstr &MI) = 0;
  };

  /// This subclass rejects any memory instructions.
  class NoMemInstr : public InspectMemInstr {
  public:
    NoMemInstr() : InspectMemInstr(true) {}
  private:
    bool hasHazard_(const MachineInstr &MI) override { return true; }
  };

  /// This subclass accepts loads from stacks and constant loads.
  class LoadFromStackOrConst : public InspectMemInstr {
  public:
    LoadFromStackOrConst() : InspectMemInstr(false) {}
  private:
    bool hasHazard_(const MachineInstr &MI) override;
  };

  /// This subclass uses memory dependence information to determine whether a
  /// memory instruction can be moved to a delay slot.
  class MemDefsUses : public InspectMemInstr {
  public:
    MemDefsUses(const DataLayout &DL, const MachineFrameInfo *MFI);

  private:
    typedef PointerUnion<const Value *, const PseudoSourceValue *> ValueType;

    bool hasHazard_(const MachineInstr &MI) override;

    /// Update Defs and Uses. Return true if there exist dependences that
    /// disqualify the delay slot candidate between V and values in Uses and
    /// Defs.
    bool updateDefsUses(ValueType V, bool MayStore);

    /// Get the list of underlying objects of MI's memory operand.
    bool getUnderlyingObjects(const MachineInstr &MI,
                              SmallVectorImpl<ValueType> &Objects) const;

    const MachineFrameInfo *MFI;
    SmallPtrSet<ValueType, 4> Uses, Defs;
    const DataLayout &DL;

    /// Flags indicating whether loads or stores with no underlying objects have
    /// been seen.
    bool SeenNoObjLoad, SeenNoObjStore;
  };

  class Filler : public MachineFunctionPass {
  public:
    Filler(TargetMachine &tm)
      : MachineFunctionPass(ID), TM(tm) { }

    const char *getPassName() const override {
      return "Mips Delay Slot Filler";
    }

    bool runOnMachineFunction(MachineFunction &F) override {
      bool Changed = false;
      for (MachineFunction::iterator FI = F.begin(), FE = F.end();
           FI != FE; ++FI)
        Changed |= runOnMachineBasicBlock(*FI);

      // This pass invalidates liveness information when it reorders
      // instructions to fill delay slot. Without this, -verify-machineinstrs
      // will fail.
      if (Changed)
        F.getRegInfo().invalidateLiveness();

      return Changed;
    }

    void getAnalysisUsage(AnalysisUsage &AU) const override {
      AU.addRequired<MachineBranchProbabilityInfo>();
      MachineFunctionPass::getAnalysisUsage(AU);
    }

  private:
    bool runOnMachineBasicBlock(MachineBasicBlock &MBB);

    Iter replaceWithCompactBranch(MachineBasicBlock &MBB,
                                  Iter Branch, DebugLoc DL);

    Iter replaceWithCompactJump(MachineBasicBlock &MBB,
                                Iter Jump, DebugLoc DL);

    /// This function checks if it is valid to move Candidate to the delay slot
    /// and returns true if it isn't. It also updates memory and register
    /// dependence information.
    bool delayHasHazard(const MachineInstr &Candidate, RegDefsUses &RegDU,
                        InspectMemInstr &IM) const;

    /// This function searches range [Begin, End) for an instruction that can be
    /// moved to the delay slot. Returns true on success.
    template<typename IterTy>
    bool searchRange(MachineBasicBlock &MBB, IterTy Begin, IterTy End,
                     RegDefsUses &RegDU, InspectMemInstr &IM, Iter Slot,
                     IterTy &Filler) const;

    /// This function searches in the backward direction for an instruction that
    /// can be moved to the delay slot. Returns true on success.
    bool searchBackward(MachineBasicBlock &MBB, Iter Slot) const;

    /// This function searches MBB in the forward direction for an instruction
    /// that can be moved to the delay slot. Returns true on success.
    bool searchForward(MachineBasicBlock &MBB, Iter Slot) const;

    /// This function searches one of MBB's successor blocks for an instruction
    /// that can be moved to the delay slot and inserts clones of the
    /// instruction into the successor's predecessor blocks.
    bool searchSuccBBs(MachineBasicBlock &MBB, Iter Slot) const;

    /// Pick a successor block of MBB. Return NULL if MBB doesn't have a
    /// successor block that is not a landing pad.
    MachineBasicBlock *selectSuccBB(MachineBasicBlock &B) const;

    /// This function analyzes MBB and returns an instruction with an unoccupied
    /// slot that branches to Dst.
    std::pair<MipsInstrInfo::BranchType, MachineInstr *>
    getBranch(MachineBasicBlock &MBB, const MachineBasicBlock &Dst) const;

    /// Examine Pred and see if it is possible to insert an instruction into
    /// one of its branches delay slot or its end.
    bool examinePred(MachineBasicBlock &Pred, const MachineBasicBlock &Succ,
                     RegDefsUses &RegDU, bool &HasMultipleSuccs,
                     BB2BrMap &BrMap) const;

    bool terminateSearch(const MachineInstr &Candidate) const;

    TargetMachine &TM;

    static char ID;
  };
  char Filler::ID = 0;
} // end of anonymous namespace

static bool hasUnoccupiedSlot(const MachineInstr *MI) {
  return MI->hasDelaySlot() && !MI->isBundledWithSucc();
}

/// This function inserts clones of Filler into predecessor blocks.
static void insertDelayFiller(Iter Filler, const BB2BrMap &BrMap) {
  MachineFunction *MF = Filler->getParent()->getParent();

  for (BB2BrMap::const_iterator I = BrMap.begin(); I != BrMap.end(); ++I) {
    if (I->second) {
      MIBundleBuilder(I->second).append(MF->CloneMachineInstr(&*Filler));
      ++UsefulSlots;
    } else {
      I->first->insert(I->first->end(), MF->CloneMachineInstr(&*Filler));
    }
  }
}

/// This function adds registers Filler defines to MBB's live-in register list.
static void addLiveInRegs(Iter Filler, MachineBasicBlock &MBB) {
  for (unsigned I = 0, E = Filler->getNumOperands(); I != E; ++I) {
    const MachineOperand &MO = Filler->getOperand(I);
    unsigned R;

    if (!MO.isReg() || !MO.isDef() || !(R = MO.getReg()))
      continue;

#ifndef NDEBUG
    const MachineFunction &MF = *MBB.getParent();
    assert(MF.getSubtarget().getRegisterInfo()->getAllocatableSet(MF).test(R) &&
           "Shouldn't move an instruction with unallocatable registers across "
           "basic block boundaries.");
#endif

    if (!MBB.isLiveIn(R))
      MBB.addLiveIn(R);
  }
}

RegDefsUses::RegDefsUses(const TargetRegisterInfo &TRI)
    : TRI(TRI), Defs(TRI.getNumRegs(), false), Uses(TRI.getNumRegs(), false) {}

void RegDefsUses::init(const MachineInstr &MI) {
  // Add all register operands which are explicit and non-variadic.
  update(MI, 0, MI.getDesc().getNumOperands());

  // If MI is a call, add RA to Defs to prevent users of RA from going into
  // delay slot.
  if (MI.isCall())
    Defs.set(Mips::RA);

  // Add all implicit register operands of branch instructions except
  // register AT.
  if (MI.isBranch()) {
    update(MI, MI.getDesc().getNumOperands(), MI.getNumOperands());
    Defs.reset(Mips::AT);
  }
}

void RegDefsUses::setCallerSaved(const MachineInstr &MI) {
  assert(MI.isCall());

  // Add RA/RA_64 to Defs to prevent users of RA/RA_64 from going into
  // the delay slot. The reason is that RA/RA_64 must not be changed
  // in the delay slot so that the callee can return to the caller.
  if (MI.definesRegister(Mips::RA) || MI.definesRegister(Mips::RA_64)) {
    Defs.set(Mips::RA);
    Defs.set(Mips::RA_64);
  }

  // If MI is a call, add all caller-saved registers to Defs.
  BitVector CallerSavedRegs(TRI.getNumRegs(), true);

  CallerSavedRegs.reset(Mips::ZERO);
  CallerSavedRegs.reset(Mips::ZERO_64);

  for (const MCPhysReg *R = TRI.getCalleeSavedRegs(MI.getParent()->getParent());
       *R; ++R)
    for (MCRegAliasIterator AI(*R, &TRI, true); AI.isValid(); ++AI)
      CallerSavedRegs.reset(*AI);

  Defs |= CallerSavedRegs;
}

void RegDefsUses::setUnallocatableRegs(const MachineFunction &MF) {
  BitVector AllocSet = TRI.getAllocatableSet(MF);

  for (int R = AllocSet.find_first(); R != -1; R = AllocSet.find_next(R))
    for (MCRegAliasIterator AI(R, &TRI, false); AI.isValid(); ++AI)
      AllocSet.set(*AI);

  AllocSet.set(Mips::ZERO);
  AllocSet.set(Mips::ZERO_64);

  Defs |= AllocSet.flip();
}

void RegDefsUses::addLiveOut(const MachineBasicBlock &MBB,
                             const MachineBasicBlock &SuccBB) {
  for (MachineBasicBlock::const_succ_iterator SI = MBB.succ_begin(),
       SE = MBB.succ_end(); SI != SE; ++SI)
    if (*SI != &SuccBB)
      for (MachineBasicBlock::livein_iterator LI = (*SI)->livein_begin(),
           LE = (*SI)->livein_end(); LI != LE; ++LI)
        Uses.set(*LI);
}

bool RegDefsUses::update(const MachineInstr &MI, unsigned Begin, unsigned End) {
  BitVector NewDefs(TRI.getNumRegs()), NewUses(TRI.getNumRegs());
  bool HasHazard = false;

  for (unsigned I = Begin; I != End; ++I) {
    const MachineOperand &MO = MI.getOperand(I);

    if (MO.isReg() && MO.getReg())
      HasHazard |= checkRegDefsUses(NewDefs, NewUses, MO.getReg(), MO.isDef());
  }

  Defs |= NewDefs;
  Uses |= NewUses;

  return HasHazard;
}

bool RegDefsUses::checkRegDefsUses(BitVector &NewDefs, BitVector &NewUses,
                                   unsigned Reg, bool IsDef) const {
  if (IsDef) {
    NewDefs.set(Reg);
    // check whether Reg has already been defined or used.
    return (isRegInSet(Defs, Reg) || isRegInSet(Uses, Reg));
  }

  NewUses.set(Reg);
  // check whether Reg has already been defined.
  return isRegInSet(Defs, Reg);
}

bool RegDefsUses::isRegInSet(const BitVector &RegSet, unsigned Reg) const {
  // Check Reg and all aliased Registers.
  for (MCRegAliasIterator AI(Reg, &TRI, true); AI.isValid(); ++AI)
    if (RegSet.test(*AI))
      return true;
  return false;
}

bool InspectMemInstr::hasHazard(const MachineInstr &MI) {
  if (!MI.mayStore() && !MI.mayLoad())
    return false;

  if (ForbidMemInstr)
    return true;

  OrigSeenLoad = SeenLoad;
  OrigSeenStore = SeenStore;
  SeenLoad |= MI.mayLoad();
  SeenStore |= MI.mayStore();

  // If MI is an ordered or volatile memory reference, disallow moving
  // subsequent loads and stores to delay slot.
  if (MI.hasOrderedMemoryRef() && (OrigSeenLoad || OrigSeenStore)) {
    ForbidMemInstr = true;
    return true;
  }

  return hasHazard_(MI);
}

bool LoadFromStackOrConst::hasHazard_(const MachineInstr &MI) {
  if (MI.mayStore())
    return true;

  if (!MI.hasOneMemOperand() || !(*MI.memoperands_begin())->getPseudoValue())
    return true;

  if (const PseudoSourceValue *PSV =
      (*MI.memoperands_begin())->getPseudoValue()) {
    if (isa<FixedStackPseudoSourceValue>(PSV))
      return false;
    return !PSV->isConstant(nullptr) && PSV != PseudoSourceValue::getStack();
  }

  return true;
}

MemDefsUses::MemDefsUses(const DataLayout &DL, const MachineFrameInfo *MFI_)
    : InspectMemInstr(false), MFI(MFI_), DL(DL), SeenNoObjLoad(false),
      SeenNoObjStore(false) {}

bool MemDefsUses::hasHazard_(const MachineInstr &MI) {
  bool HasHazard = false;
  SmallVector<ValueType, 4> Objs;

  // Check underlying object list.
  if (getUnderlyingObjects(MI, Objs)) {
    for (SmallVectorImpl<ValueType>::const_iterator I = Objs.begin();
         I != Objs.end(); ++I)
      HasHazard |= updateDefsUses(*I, MI.mayStore());

    return HasHazard;
  }

  // No underlying objects found.
  HasHazard = MI.mayStore() && (OrigSeenLoad || OrigSeenStore);
  HasHazard |= MI.mayLoad() || OrigSeenStore;

  SeenNoObjLoad |= MI.mayLoad();
  SeenNoObjStore |= MI.mayStore();

  return HasHazard;
}

bool MemDefsUses::updateDefsUses(ValueType V, bool MayStore) {
  if (MayStore)
    return !Defs.insert(V).second || Uses.count(V) || SeenNoObjStore ||
           SeenNoObjLoad;

  Uses.insert(V);
  return Defs.count(V) || SeenNoObjStore;
}

bool MemDefsUses::
getUnderlyingObjects(const MachineInstr &MI,
                     SmallVectorImpl<ValueType> &Objects) const {
  if (!MI.hasOneMemOperand() ||
      (!(*MI.memoperands_begin())->getValue() &&
       !(*MI.memoperands_begin())->getPseudoValue()))
    return false;

  if (const PseudoSourceValue *PSV =
      (*MI.memoperands_begin())->getPseudoValue()) {
    if (!PSV->isAliased(MFI))
      return false;
    Objects.push_back(PSV);
    return true;
  }

  const Value *V = (*MI.memoperands_begin())->getValue();

  SmallVector<Value *, 4> Objs;
  GetUnderlyingObjects(const_cast<Value *>(V), Objs, DL);

  for (SmallVectorImpl<Value *>::iterator I = Objs.begin(), E = Objs.end();
       I != E; ++I) {
    if (!isIdentifiedObject(V))
      return false;

    Objects.push_back(*I);
  }

  return true;
}

// Replace Branch with the compact branch instruction.
Iter Filler::replaceWithCompactBranch(MachineBasicBlock &MBB,
                                      Iter Branch, DebugLoc DL) {
  const MipsInstrInfo *TII =
      MBB.getParent()->getSubtarget<MipsSubtarget>().getInstrInfo();

  unsigned NewOpcode =
    (((unsigned) Branch->getOpcode()) == Mips::BEQ) ? Mips::BEQZC_MM
                                                    : Mips::BNEZC_MM;

  const MCInstrDesc &NewDesc = TII->get(NewOpcode);
  MachineInstrBuilder MIB = BuildMI(MBB, Branch, DL, NewDesc);

  MIB.addReg(Branch->getOperand(0).getReg());
  MIB.addMBB(Branch->getOperand(2).getMBB());

  Iter tmpIter = Branch;
  Branch = std::prev(Branch);
  MBB.erase(tmpIter);

  return Branch;
}

// Replace Jumps with the compact jump instruction.
Iter Filler::replaceWithCompactJump(MachineBasicBlock &MBB,
                                    Iter Jump, DebugLoc DL) {
  const MipsInstrInfo *TII =
      MBB.getParent()->getSubtarget<MipsSubtarget>().getInstrInfo();

  const MCInstrDesc &NewDesc = TII->get(Mips::JRC16_MM);
  MachineInstrBuilder MIB = BuildMI(MBB, Jump, DL, NewDesc);

  MIB.addReg(Jump->getOperand(0).getReg());

  Iter tmpIter = Jump;
  Jump = std::prev(Jump);
  MBB.erase(tmpIter);

  return Jump;
}

// For given opcode returns opcode of corresponding instruction with short
// delay slot.
static int getEquivalentCallShort(int Opcode) {
  switch (Opcode) {
  case Mips::BGEZAL:
    return Mips::BGEZALS_MM;
  case Mips::BLTZAL:
    return Mips::BLTZALS_MM;
  case Mips::JAL:
    return Mips::JALS_MM;
  case Mips::JALR:
    return Mips::JALRS_MM;
  case Mips::JALR16_MM:
    return Mips::JALRS16_MM;
  default:
    llvm_unreachable("Unexpected call instruction for microMIPS.");
  }
}

/// runOnMachineBasicBlock - Fill in delay slots for the given basic block.
/// We assume there is only one delay slot per delayed instruction.
bool Filler::runOnMachineBasicBlock(MachineBasicBlock &MBB) {
  bool Changed = false;
  const MipsSubtarget &STI = MBB.getParent()->getSubtarget<MipsSubtarget>();
  bool InMicroMipsMode = STI.inMicroMipsMode();
  const MipsInstrInfo *TII = STI.getInstrInfo();

  for (Iter I = MBB.begin(); I != MBB.end(); ++I) {
    if (!hasUnoccupiedSlot(&*I))
      continue;

    ++FilledSlots;
    Changed = true;

    // Delay slot filling is disabled at -O0.
    if (!DisableDelaySlotFiller && (TM.getOptLevel() != CodeGenOpt::None)) {
      bool Filled = false;

      if (searchBackward(MBB, I)) {
        Filled = true;
      } else if (I->isTerminator()) {
        if (searchSuccBBs(MBB, I)) {
          Filled = true;
        }
      } else if (searchForward(MBB, I)) {
        Filled = true;
      }

      if (Filled) {
        // Get instruction with delay slot.
        MachineBasicBlock::instr_iterator DSI(I);

        if (InMicroMipsMode && TII->GetInstSizeInBytes(std::next(DSI)) == 2 &&
            DSI->isCall()) {
          // If instruction in delay slot is 16b change opcode to
          // corresponding instruction with short delay slot.
          DSI->setDesc(TII->get(getEquivalentCallShort(DSI->getOpcode())));
        }

        continue;
      }
    }

    // If instruction is BEQ or BNE with one ZERO register, then instead of
    // adding NOP replace this instruction with the corresponding compact
    // branch instruction, i.e. BEQZC or BNEZC.
    unsigned Opcode = I->getOpcode();
    if (InMicroMipsMode) {
      switch (Opcode) {
        case Mips::BEQ:
        case Mips::BNE:
          if (((unsigned) I->getOperand(1).getReg()) == Mips::ZERO) {
            I = replaceWithCompactBranch(MBB, I, I->getDebugLoc());
            continue;
          }
          break;
        case Mips::JR:
        case Mips::PseudoReturn:
        case Mips::PseudoIndirectBranch:
          // For microMIPS the PseudoReturn and PseudoIndirectBranch are allways
          // expanded to JR_MM, so they can be replaced with JRC16_MM.
          I = replaceWithCompactJump(MBB, I, I->getDebugLoc());
          continue;
        default:
          break;
      }
    }
    // Bundle the NOP to the instruction with the delay slot.
    BuildMI(MBB, std::next(I), I->getDebugLoc(), TII->get(Mips::NOP));
    MIBundleBuilder(MBB, I, std::next(I, 2));
  }

  return Changed;
}

/// createMipsDelaySlotFillerPass - Returns a pass that fills in delay
/// slots in Mips MachineFunctions
FunctionPass *llvm::createMipsDelaySlotFillerPass(MipsTargetMachine &tm) {
  return new Filler(tm);
}

template<typename IterTy>
bool Filler::searchRange(MachineBasicBlock &MBB, IterTy Begin, IterTy End,
                         RegDefsUses &RegDU, InspectMemInstr& IM, Iter Slot,
                         IterTy &Filler) const {
  bool IsReverseIter = std::is_convertible<IterTy, ReverseIter>::value;

  for (IterTy I = Begin; I != End;) {
    IterTy CurrI = I;
    ++I;

    // skip debug value
    if (CurrI->isDebugValue())
      continue;

    if (terminateSearch(*CurrI))
      break;

    assert((!CurrI->isCall() && !CurrI->isReturn() && !CurrI->isBranch()) &&
           "Cannot put calls, returns or branches in delay slot.");

    if (CurrI->isKill()) {
      CurrI->eraseFromParent();

      // This special case is needed for reverse iterators, because when we
      // erase an instruction, the iterators are updated to point to the next
      // instruction.
      if (IsReverseIter && I != End)
        I = CurrI;
      continue;
    }

    if (delayHasHazard(*CurrI, RegDU, IM))
      continue;

    const MipsSubtarget &STI = MBB.getParent()->getSubtarget<MipsSubtarget>();
    if (STI.isTargetNaCl()) {
      // In NaCl, instructions that must be masked are forbidden in delay slots.
      // We only check for loads, stores and SP changes.  Calls, returns and
      // branches are not checked because non-NaCl targets never put them in
      // delay slots.
      unsigned AddrIdx;
      if ((isBasePlusOffsetMemoryAccess(CurrI->getOpcode(), &AddrIdx) &&
           baseRegNeedsLoadStoreMask(CurrI->getOperand(AddrIdx).getReg())) ||
          CurrI->modifiesRegister(Mips::SP, STI.getRegisterInfo()))
        continue;
    }

    bool InMicroMipsMode = STI.inMicroMipsMode();
    const MipsInstrInfo *TII = STI.getInstrInfo();
    unsigned Opcode = (*Slot).getOpcode();
    if (InMicroMipsMode && TII->GetInstSizeInBytes(&(*CurrI)) == 2 &&
        (Opcode == Mips::JR || Opcode == Mips::PseudoIndirectBranch ||
         Opcode == Mips::PseudoReturn))
      continue;

    Filler = CurrI;
    return true;
  }

  return false;
}

bool Filler::searchBackward(MachineBasicBlock &MBB, Iter Slot) const {
  if (DisableBackwardSearch)
    return false;

  RegDefsUses RegDU(*MBB.getParent()->getSubtarget().getRegisterInfo());
  MemDefsUses MemDU(*TM.getDataLayout(), MBB.getParent()->getFrameInfo());
  ReverseIter Filler;

  RegDU.init(*Slot);

  if (!searchRange(MBB, ReverseIter(Slot), MBB.rend(), RegDU, MemDU, Slot,
                   Filler))
    return false;

  MBB.splice(std::next(Slot), &MBB, std::next(Filler).base());
  MIBundleBuilder(MBB, Slot, std::next(Slot, 2));
  ++UsefulSlots;
  return true;
}

bool Filler::searchForward(MachineBasicBlock &MBB, Iter Slot) const {
  // Can handle only calls.
  if (DisableForwardSearch || !Slot->isCall())
    return false;

  RegDefsUses RegDU(*MBB.getParent()->getSubtarget().getRegisterInfo());
  NoMemInstr NM;
  Iter Filler;

  RegDU.setCallerSaved(*Slot);

  if (!searchRange(MBB, std::next(Slot), MBB.end(), RegDU, NM, Slot, Filler))
    return false;

  MBB.splice(std::next(Slot), &MBB, Filler);
  MIBundleBuilder(MBB, Slot, std::next(Slot, 2));
  ++UsefulSlots;
  return true;
}

bool Filler::searchSuccBBs(MachineBasicBlock &MBB, Iter Slot) const {
  if (DisableSuccBBSearch)
    return false;

  MachineBasicBlock *SuccBB = selectSuccBB(MBB);

  if (!SuccBB)
    return false;

  RegDefsUses RegDU(*MBB.getParent()->getSubtarget().getRegisterInfo());
  bool HasMultipleSuccs = false;
  BB2BrMap BrMap;
  std::unique_ptr<InspectMemInstr> IM;
  Iter Filler;

  // Iterate over SuccBB's predecessor list.
  for (MachineBasicBlock::pred_iterator PI = SuccBB->pred_begin(),
       PE = SuccBB->pred_end(); PI != PE; ++PI)
    if (!examinePred(**PI, *SuccBB, RegDU, HasMultipleSuccs, BrMap))
      return false;

  // Do not allow moving instructions which have unallocatable register operands
  // across basic block boundaries.
  RegDU.setUnallocatableRegs(*MBB.getParent());

  // Only allow moving loads from stack or constants if any of the SuccBB's
  // predecessors have multiple successors.
  if (HasMultipleSuccs) {
    IM.reset(new LoadFromStackOrConst());
  } else {
    const MachineFrameInfo *MFI = MBB.getParent()->getFrameInfo();
    IM.reset(new MemDefsUses(*TM.getDataLayout(), MFI));
  }

  if (!searchRange(MBB, SuccBB->begin(), SuccBB->end(), RegDU, *IM, Slot,
                   Filler))
    return false;

  insertDelayFiller(Filler, BrMap);
  addLiveInRegs(Filler, *SuccBB);
  Filler->eraseFromParent();

  return true;
}

MachineBasicBlock *Filler::selectSuccBB(MachineBasicBlock &B) const {
  if (B.succ_empty())
    return nullptr;

  // Select the successor with the larget edge weight.
  auto &Prob = getAnalysis<MachineBranchProbabilityInfo>();
  MachineBasicBlock *S = *std::max_element(B.succ_begin(), B.succ_end(),
                                           [&](const MachineBasicBlock *Dst0,
                                               const MachineBasicBlock *Dst1) {
    return Prob.getEdgeWeight(&B, Dst0) < Prob.getEdgeWeight(&B, Dst1);
  });
  return S->isLandingPad() ? nullptr : S;
}

std::pair<MipsInstrInfo::BranchType, MachineInstr *>
Filler::getBranch(MachineBasicBlock &MBB, const MachineBasicBlock &Dst) const {
  const MipsInstrInfo *TII =
      MBB.getParent()->getSubtarget<MipsSubtarget>().getInstrInfo();
  MachineBasicBlock *TrueBB = nullptr, *FalseBB = nullptr;
  SmallVector<MachineInstr*, 2> BranchInstrs;
  SmallVector<MachineOperand, 2> Cond;

  MipsInstrInfo::BranchType R =
    TII->AnalyzeBranch(MBB, TrueBB, FalseBB, Cond, false, BranchInstrs);

  if ((R == MipsInstrInfo::BT_None) || (R == MipsInstrInfo::BT_NoBranch))
    return std::make_pair(R, nullptr);

  if (R != MipsInstrInfo::BT_CondUncond) {
    if (!hasUnoccupiedSlot(BranchInstrs[0]))
      return std::make_pair(MipsInstrInfo::BT_None, nullptr);

    assert(((R != MipsInstrInfo::BT_Uncond) || (TrueBB == &Dst)));

    return std::make_pair(R, BranchInstrs[0]);
  }

  assert((TrueBB == &Dst) || (FalseBB == &Dst));

  // Examine the conditional branch. See if its slot is occupied.
  if (hasUnoccupiedSlot(BranchInstrs[0]))
    return std::make_pair(MipsInstrInfo::BT_Cond, BranchInstrs[0]);

  // If that fails, try the unconditional branch.
  if (hasUnoccupiedSlot(BranchInstrs[1]) && (FalseBB == &Dst))
    return std::make_pair(MipsInstrInfo::BT_Uncond, BranchInstrs[1]);

  return std::make_pair(MipsInstrInfo::BT_None, nullptr);
}

bool Filler::examinePred(MachineBasicBlock &Pred, const MachineBasicBlock &Succ,
                         RegDefsUses &RegDU, bool &HasMultipleSuccs,
                         BB2BrMap &BrMap) const {
  std::pair<MipsInstrInfo::BranchType, MachineInstr *> P =
    getBranch(Pred, Succ);

  // Return if either getBranch wasn't able to analyze the branches or there
  // were no branches with unoccupied slots.
  if (P.first == MipsInstrInfo::BT_None)
    return false;

  if ((P.first != MipsInstrInfo::BT_Uncond) &&
      (P.first != MipsInstrInfo::BT_NoBranch)) {
    HasMultipleSuccs = true;
    RegDU.addLiveOut(Pred, Succ);
  }

  BrMap[&Pred] = P.second;
  return true;
}

bool Filler::delayHasHazard(const MachineInstr &Candidate, RegDefsUses &RegDU,
                            InspectMemInstr &IM) const {
  assert(!Candidate.isKill() &&
         "KILL instructions should have been eliminated at this point.");

  bool HasHazard = Candidate.isImplicitDef();

  HasHazard |= IM.hasHazard(Candidate);
  HasHazard |= RegDU.update(Candidate, 0, Candidate.getNumOperands());

  return HasHazard;
}

bool Filler::terminateSearch(const MachineInstr &Candidate) const {
  return (Candidate.isTerminator() || Candidate.isCall() ||
          Candidate.isPosition() || Candidate.isInlineAsm() ||
          Candidate.hasUnmodeledSideEffects());
}

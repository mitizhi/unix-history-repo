//===- Thumb2InstrInfo.cpp - Thumb-2 Instruction Information ----*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file contains the Thumb-2 implementation of the TargetInstrInfo class.
//
//===----------------------------------------------------------------------===//

#include "Thumb2InstrInfo.h"
#include "ARM.h"
#include "ARMConstantPoolValue.h"
#include "ARMAddressingModes.h"
#include "ARMGenInstrInfo.inc"
#include "ARMMachineFunctionInfo.h"
#include "Thumb2InstrInfo.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineMemOperand.h"
#include "llvm/CodeGen/PseudoSourceValue.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/CommandLine.h"

using namespace llvm;

static cl::opt<bool>
OldT2IfCvt("old-thumb2-ifcvt", cl::Hidden,
           cl::desc("Use old-style Thumb2 if-conversion heuristics"),
           cl::init(false));

Thumb2InstrInfo::Thumb2InstrInfo(const ARMSubtarget &STI)
  : ARMBaseInstrInfo(STI), RI(*this, STI) {
}

unsigned Thumb2InstrInfo::getUnindexedOpcode(unsigned Opc) const {
  // FIXME
  return 0;
}

void
Thumb2InstrInfo::ReplaceTailWithBranchTo(MachineBasicBlock::iterator Tail,
                                         MachineBasicBlock *NewDest) const {
  MachineBasicBlock *MBB = Tail->getParent();
  ARMFunctionInfo *AFI = MBB->getParent()->getInfo<ARMFunctionInfo>();
  if (!AFI->hasITBlocks()) {
    TargetInstrInfoImpl::ReplaceTailWithBranchTo(Tail, NewDest);
    return;
  }

  // If the first instruction of Tail is predicated, we may have to update
  // the IT instruction.
  unsigned PredReg = 0;
  ARMCC::CondCodes CC = llvm::getInstrPredicate(Tail, PredReg);
  MachineBasicBlock::iterator MBBI = Tail;
  if (CC != ARMCC::AL)
    // Expecting at least the t2IT instruction before it.
    --MBBI;

  // Actually replace the tail.
  TargetInstrInfoImpl::ReplaceTailWithBranchTo(Tail, NewDest);

  // Fix up IT.
  if (CC != ARMCC::AL) {
    MachineBasicBlock::iterator E = MBB->begin();
    unsigned Count = 4; // At most 4 instructions in an IT block.
    while (Count && MBBI != E) {
      if (MBBI->isDebugValue()) {
        --MBBI;
        continue;
      }
      if (MBBI->getOpcode() == ARM::t2IT) {
        unsigned Mask = MBBI->getOperand(1).getImm();
        if (Count == 4)
          MBBI->eraseFromParent();
        else {
          unsigned MaskOn = 1 << Count;
          unsigned MaskOff = ~(MaskOn - 1);
          MBBI->getOperand(1).setImm((Mask & MaskOff) | MaskOn);
        }
        return;
      }
      --MBBI;
      --Count;
    }

    // Ctrl flow can reach here if branch folding is run before IT block
    // formation pass.
  }
}

bool
Thumb2InstrInfo::isLegalToSplitMBBAt(MachineBasicBlock &MBB,
                                     MachineBasicBlock::iterator MBBI) const {
  unsigned PredReg = 0;
  return llvm::getITInstrPredicate(MBBI, PredReg) == ARMCC::AL;
}

void Thumb2InstrInfo::copyPhysReg(MachineBasicBlock &MBB,
                                  MachineBasicBlock::iterator I, DebugLoc DL,
                                  unsigned DestReg, unsigned SrcReg,
                                  bool KillSrc) const {
  // Handle SPR, DPR, and QPR copies.
  if (!ARM::GPRRegClass.contains(DestReg, SrcReg))
    return ARMBaseInstrInfo::copyPhysReg(MBB, I, DL, DestReg, SrcReg, KillSrc);

  bool tDest = ARM::tGPRRegClass.contains(DestReg);
  bool tSrc  = ARM::tGPRRegClass.contains(SrcReg);
  unsigned Opc = ARM::tMOVgpr2gpr;
  if (tDest && tSrc)
    Opc = ARM::tMOVr;
  else if (tSrc)
    Opc = ARM::tMOVtgpr2gpr;
  else if (tDest)
    Opc = ARM::tMOVgpr2tgpr;

  BuildMI(MBB, I, DL, get(Opc), DestReg)
    .addReg(SrcReg, getKillRegState(KillSrc));
}

void Thumb2InstrInfo::
storeRegToStackSlot(MachineBasicBlock &MBB, MachineBasicBlock::iterator I,
                    unsigned SrcReg, bool isKill, int FI,
                    const TargetRegisterClass *RC,
                    const TargetRegisterInfo *TRI) const {
  if (RC == ARM::GPRRegisterClass   || RC == ARM::tGPRRegisterClass ||
      RC == ARM::tcGPRRegisterClass || RC == ARM::rGPRRegisterClass) {
    DebugLoc DL;
    if (I != MBB.end()) DL = I->getDebugLoc();

    MachineFunction &MF = *MBB.getParent();
    MachineFrameInfo &MFI = *MF.getFrameInfo();
    MachineMemOperand *MMO =
      MF.getMachineMemOperand(
                      MachinePointerInfo(PseudoSourceValue::getFixedStack(FI)),
                              MachineMemOperand::MOStore,
                              MFI.getObjectSize(FI),
                              MFI.getObjectAlignment(FI));
    AddDefaultPred(BuildMI(MBB, I, DL, get(ARM::t2STRi12))
                   .addReg(SrcReg, getKillRegState(isKill))
                   .addFrameIndex(FI).addImm(0).addMemOperand(MMO));
    return;
  }

  ARMBaseInstrInfo::storeRegToStackSlot(MBB, I, SrcReg, isKill, FI, RC, TRI);
}

void Thumb2InstrInfo::
loadRegFromStackSlot(MachineBasicBlock &MBB, MachineBasicBlock::iterator I,
                     unsigned DestReg, int FI,
                     const TargetRegisterClass *RC,
                     const TargetRegisterInfo *TRI) const {
  if (RC == ARM::GPRRegisterClass   || RC == ARM::tGPRRegisterClass ||
      RC == ARM::tcGPRRegisterClass || RC == ARM::rGPRRegisterClass) {
    DebugLoc DL;
    if (I != MBB.end()) DL = I->getDebugLoc();

    MachineFunction &MF = *MBB.getParent();
    MachineFrameInfo &MFI = *MF.getFrameInfo();
    MachineMemOperand *MMO =
      MF.getMachineMemOperand(
                      MachinePointerInfo(PseudoSourceValue::getFixedStack(FI)),
                              MachineMemOperand::MOLoad,
                              MFI.getObjectSize(FI),
                              MFI.getObjectAlignment(FI));
    AddDefaultPred(BuildMI(MBB, I, DL, get(ARM::t2LDRi12), DestReg)
                   .addFrameIndex(FI).addImm(0).addMemOperand(MMO));
    return;
  }

  ARMBaseInstrInfo::loadRegFromStackSlot(MBB, I, DestReg, FI, RC, TRI);
}

void llvm::emitT2RegPlusImmediate(MachineBasicBlock &MBB,
                               MachineBasicBlock::iterator &MBBI, DebugLoc dl,
                               unsigned DestReg, unsigned BaseReg, int NumBytes,
                               ARMCC::CondCodes Pred, unsigned PredReg,
                               const ARMBaseInstrInfo &TII) {
  bool isSub = NumBytes < 0;
  if (isSub) NumBytes = -NumBytes;

  // If profitable, use a movw or movt to materialize the offset.
  // FIXME: Use the scavenger to grab a scratch register.
  if (DestReg != ARM::SP && DestReg != BaseReg &&
      NumBytes >= 4096 &&
      ARM_AM::getT2SOImmVal(NumBytes) == -1) {
    bool Fits = false;
    if (NumBytes < 65536) {
      // Use a movw to materialize the 16-bit constant.
      BuildMI(MBB, MBBI, dl, TII.get(ARM::t2MOVi16), DestReg)
        .addImm(NumBytes)
        .addImm((unsigned)Pred).addReg(PredReg);
      Fits = true;
    } else if ((NumBytes & 0xffff) == 0) {
      // Use a movt to materialize the 32-bit constant.
      BuildMI(MBB, MBBI, dl, TII.get(ARM::t2MOVTi16), DestReg)
        .addReg(DestReg)
        .addImm(NumBytes >> 16)
        .addImm((unsigned)Pred).addReg(PredReg);
      Fits = true;
    }

    if (Fits) {
      if (isSub) {
        BuildMI(MBB, MBBI, dl, TII.get(ARM::t2SUBrr), DestReg)
          .addReg(BaseReg, RegState::Kill)
          .addReg(DestReg, RegState::Kill)
          .addImm((unsigned)Pred).addReg(PredReg).addReg(0);
      } else {
        BuildMI(MBB, MBBI, dl, TII.get(ARM::t2ADDrr), DestReg)
          .addReg(DestReg, RegState::Kill)
          .addReg(BaseReg, RegState::Kill)
        .addImm((unsigned)Pred).addReg(PredReg).addReg(0);
      }
      return;
    }
  }

  while (NumBytes) {
    unsigned ThisVal = NumBytes;
    unsigned Opc = 0;
    if (DestReg == ARM::SP && BaseReg != ARM::SP) {
      // mov sp, rn. Note t2MOVr cannot be used.
      BuildMI(MBB, MBBI, dl, TII.get(ARM::tMOVgpr2gpr),DestReg).addReg(BaseReg);
      BaseReg = ARM::SP;
      continue;
    }

    bool HasCCOut = true;
    if (BaseReg == ARM::SP) {
      // sub sp, sp, #imm7
      if (DestReg == ARM::SP && (ThisVal < ((1 << 7)-1) * 4)) {
        assert((ThisVal & 3) == 0 && "Stack update is not multiple of 4?");
        Opc = isSub ? ARM::tSUBspi : ARM::tADDspi;
        // FIXME: Fix Thumb1 immediate encoding.
        BuildMI(MBB, MBBI, dl, TII.get(Opc), DestReg)
          .addReg(BaseReg).addImm(ThisVal/4);
        NumBytes = 0;
        continue;
      }

      // sub rd, sp, so_imm
      Opc = isSub ? ARM::t2SUBrSPi : ARM::t2ADDrSPi;
      if (ARM_AM::getT2SOImmVal(NumBytes) != -1) {
        NumBytes = 0;
      } else {
        // FIXME: Move this to ARMAddressingModes.h?
        unsigned RotAmt = CountLeadingZeros_32(ThisVal);
        ThisVal = ThisVal & ARM_AM::rotr32(0xff000000U, RotAmt);
        NumBytes &= ~ThisVal;
        assert(ARM_AM::getT2SOImmVal(ThisVal) != -1 &&
               "Bit extraction didn't work?");
      }
    } else {
      assert(DestReg != ARM::SP && BaseReg != ARM::SP);
      Opc = isSub ? ARM::t2SUBri : ARM::t2ADDri;
      if (ARM_AM::getT2SOImmVal(NumBytes) != -1) {
        NumBytes = 0;
      } else if (ThisVal < 4096) {
        Opc = isSub ? ARM::t2SUBri12 : ARM::t2ADDri12;
        HasCCOut = false;
        NumBytes = 0;
      } else {
        // FIXME: Move this to ARMAddressingModes.h?
        unsigned RotAmt = CountLeadingZeros_32(ThisVal);
        ThisVal = ThisVal & ARM_AM::rotr32(0xff000000U, RotAmt);
        NumBytes &= ~ThisVal;
        assert(ARM_AM::getT2SOImmVal(ThisVal) != -1 &&
               "Bit extraction didn't work?");
      }
    }

    // Build the new ADD / SUB.
    MachineInstrBuilder MIB =
      AddDefaultPred(BuildMI(MBB, MBBI, dl, TII.get(Opc), DestReg)
                     .addReg(BaseReg, RegState::Kill)
                     .addImm(ThisVal));
    if (HasCCOut)
      AddDefaultCC(MIB);

    BaseReg = DestReg;
  }
}

static unsigned
negativeOffsetOpcode(unsigned opcode)
{
  switch (opcode) {
  case ARM::t2LDRi12:   return ARM::t2LDRi8;
  case ARM::t2LDRHi12:  return ARM::t2LDRHi8;
  case ARM::t2LDRBi12:  return ARM::t2LDRBi8;
  case ARM::t2LDRSHi12: return ARM::t2LDRSHi8;
  case ARM::t2LDRSBi12: return ARM::t2LDRSBi8;
  case ARM::t2STRi12:   return ARM::t2STRi8;
  case ARM::t2STRBi12:  return ARM::t2STRBi8;
  case ARM::t2STRHi12:  return ARM::t2STRHi8;

  case ARM::t2LDRi8:
  case ARM::t2LDRHi8:
  case ARM::t2LDRBi8:
  case ARM::t2LDRSHi8:
  case ARM::t2LDRSBi8:
  case ARM::t2STRi8:
  case ARM::t2STRBi8:
  case ARM::t2STRHi8:
    return opcode;

  default:
    break;
  }

  return 0;
}

static unsigned
positiveOffsetOpcode(unsigned opcode)
{
  switch (opcode) {
  case ARM::t2LDRi8:   return ARM::t2LDRi12;
  case ARM::t2LDRHi8:  return ARM::t2LDRHi12;
  case ARM::t2LDRBi8:  return ARM::t2LDRBi12;
  case ARM::t2LDRSHi8: return ARM::t2LDRSHi12;
  case ARM::t2LDRSBi8: return ARM::t2LDRSBi12;
  case ARM::t2STRi8:   return ARM::t2STRi12;
  case ARM::t2STRBi8:  return ARM::t2STRBi12;
  case ARM::t2STRHi8:  return ARM::t2STRHi12;

  case ARM::t2LDRi12:
  case ARM::t2LDRHi12:
  case ARM::t2LDRBi12:
  case ARM::t2LDRSHi12:
  case ARM::t2LDRSBi12:
  case ARM::t2STRi12:
  case ARM::t2STRBi12:
  case ARM::t2STRHi12:
    return opcode;

  default:
    break;
  }

  return 0;
}

static unsigned
immediateOffsetOpcode(unsigned opcode)
{
  switch (opcode) {
  case ARM::t2LDRs:   return ARM::t2LDRi12;
  case ARM::t2LDRHs:  return ARM::t2LDRHi12;
  case ARM::t2LDRBs:  return ARM::t2LDRBi12;
  case ARM::t2LDRSHs: return ARM::t2LDRSHi12;
  case ARM::t2LDRSBs: return ARM::t2LDRSBi12;
  case ARM::t2STRs:   return ARM::t2STRi12;
  case ARM::t2STRBs:  return ARM::t2STRBi12;
  case ARM::t2STRHs:  return ARM::t2STRHi12;

  case ARM::t2LDRi12:
  case ARM::t2LDRHi12:
  case ARM::t2LDRBi12:
  case ARM::t2LDRSHi12:
  case ARM::t2LDRSBi12:
  case ARM::t2STRi12:
  case ARM::t2STRBi12:
  case ARM::t2STRHi12:
  case ARM::t2LDRi8:
  case ARM::t2LDRHi8:
  case ARM::t2LDRBi8:
  case ARM::t2LDRSHi8:
  case ARM::t2LDRSBi8:
  case ARM::t2STRi8:
  case ARM::t2STRBi8:
  case ARM::t2STRHi8:
    return opcode;

  default:
    break;
  }

  return 0;
}

bool llvm::rewriteT2FrameIndex(MachineInstr &MI, unsigned FrameRegIdx,
                               unsigned FrameReg, int &Offset,
                               const ARMBaseInstrInfo &TII) {
  unsigned Opcode = MI.getOpcode();
  const TargetInstrDesc &Desc = MI.getDesc();
  unsigned AddrMode = (Desc.TSFlags & ARMII::AddrModeMask);
  bool isSub = false;

  // Memory operands in inline assembly always use AddrModeT2_i12.
  if (Opcode == ARM::INLINEASM)
    AddrMode = ARMII::AddrModeT2_i12; // FIXME. mode for thumb2?

  if (Opcode == ARM::t2ADDri || Opcode == ARM::t2ADDri12) {
    Offset += MI.getOperand(FrameRegIdx+1).getImm();

    unsigned PredReg;
    if (Offset == 0 && getInstrPredicate(&MI, PredReg) == ARMCC::AL) {
      // Turn it into a move.
      MI.setDesc(TII.get(ARM::tMOVgpr2gpr));
      MI.getOperand(FrameRegIdx).ChangeToRegister(FrameReg, false);
      // Remove offset and remaining explicit predicate operands.
      do MI.RemoveOperand(FrameRegIdx+1);
      while (MI.getNumOperands() > FrameRegIdx+1 &&
             (!MI.getOperand(FrameRegIdx+1).isReg() ||
              !MI.getOperand(FrameRegIdx+1).isImm()));
      return true;
    }

    bool isSP = FrameReg == ARM::SP;
    bool HasCCOut = Opcode != ARM::t2ADDri12;

    if (Offset < 0) {
      Offset = -Offset;
      isSub = true;
      MI.setDesc(TII.get(isSP ? ARM::t2SUBrSPi : ARM::t2SUBri));
    } else {
      MI.setDesc(TII.get(isSP ? ARM::t2ADDrSPi : ARM::t2ADDri));
    }

    // Common case: small offset, fits into instruction.
    if (ARM_AM::getT2SOImmVal(Offset) != -1) {
      MI.getOperand(FrameRegIdx).ChangeToRegister(FrameReg, false);
      MI.getOperand(FrameRegIdx+1).ChangeToImmediate(Offset);
      // Add cc_out operand if the original instruction did not have one.
      if (!HasCCOut)
        MI.addOperand(MachineOperand::CreateReg(0, false));
      Offset = 0;
      return true;
    }
    // Another common case: imm12.
    if (Offset < 4096 &&
        (!HasCCOut || MI.getOperand(MI.getNumOperands()-1).getReg() == 0)) {
      unsigned NewOpc = isSP
        ? (isSub ? ARM::t2SUBrSPi12 : ARM::t2ADDrSPi12)
        : (isSub ? ARM::t2SUBri12   : ARM::t2ADDri12);
      MI.setDesc(TII.get(NewOpc));
      MI.getOperand(FrameRegIdx).ChangeToRegister(FrameReg, false);
      MI.getOperand(FrameRegIdx+1).ChangeToImmediate(Offset);
      // Remove the cc_out operand.
      if (HasCCOut)
        MI.RemoveOperand(MI.getNumOperands()-1);
      Offset = 0;
      return true;
    }

    // Otherwise, extract 8 adjacent bits from the immediate into this
    // t2ADDri/t2SUBri.
    unsigned RotAmt = CountLeadingZeros_32(Offset);
    unsigned ThisImmVal = Offset & ARM_AM::rotr32(0xff000000U, RotAmt);

    // We will handle these bits from offset, clear them.
    Offset &= ~ThisImmVal;

    assert(ARM_AM::getT2SOImmVal(ThisImmVal) != -1 &&
           "Bit extraction didn't work?");
    MI.getOperand(FrameRegIdx+1).ChangeToImmediate(ThisImmVal);
    // Add cc_out operand if the original instruction did not have one.
    if (!HasCCOut)
      MI.addOperand(MachineOperand::CreateReg(0, false));

  } else {

    // AddrMode4 and AddrMode6 cannot handle any offset.
    if (AddrMode == ARMII::AddrMode4 || AddrMode == ARMII::AddrMode6)
      return false;

    // AddrModeT2_so cannot handle any offset. If there is no offset
    // register then we change to an immediate version.
    unsigned NewOpc = Opcode;
    if (AddrMode == ARMII::AddrModeT2_so) {
      unsigned OffsetReg = MI.getOperand(FrameRegIdx+1).getReg();
      if (OffsetReg != 0) {
        MI.getOperand(FrameRegIdx).ChangeToRegister(FrameReg, false);
        return Offset == 0;
      }

      MI.RemoveOperand(FrameRegIdx+1);
      MI.getOperand(FrameRegIdx+1).ChangeToImmediate(0);
      NewOpc = immediateOffsetOpcode(Opcode);
      AddrMode = ARMII::AddrModeT2_i12;
    }

    unsigned NumBits = 0;
    unsigned Scale = 1;
    if (AddrMode == ARMII::AddrModeT2_i8 || AddrMode == ARMII::AddrModeT2_i12) {
      // i8 supports only negative, and i12 supports only positive, so
      // based on Offset sign convert Opcode to the appropriate
      // instruction
      Offset += MI.getOperand(FrameRegIdx+1).getImm();
      if (Offset < 0) {
        NewOpc = negativeOffsetOpcode(Opcode);
        NumBits = 8;
        isSub = true;
        Offset = -Offset;
      } else {
        NewOpc = positiveOffsetOpcode(Opcode);
        NumBits = 12;
      }
    } else if (AddrMode == ARMII::AddrMode5) {
      // VFP address mode.
      const MachineOperand &OffOp = MI.getOperand(FrameRegIdx+1);
      int InstrOffs = ARM_AM::getAM5Offset(OffOp.getImm());
      if (ARM_AM::getAM5Op(OffOp.getImm()) == ARM_AM::sub)
        InstrOffs *= -1;
      NumBits = 8;
      Scale = 4;
      Offset += InstrOffs * 4;
      assert((Offset & (Scale-1)) == 0 && "Can't encode this offset!");
      if (Offset < 0) {
        Offset = -Offset;
        isSub = true;
      }
    } else {
      llvm_unreachable("Unsupported addressing mode!");
    }

    if (NewOpc != Opcode)
      MI.setDesc(TII.get(NewOpc));

    MachineOperand &ImmOp = MI.getOperand(FrameRegIdx+1);

    // Attempt to fold address computation
    // Common case: small offset, fits into instruction.
    int ImmedOffset = Offset / Scale;
    unsigned Mask = (1 << NumBits) - 1;
    if ((unsigned)Offset <= Mask * Scale) {
      // Replace the FrameIndex with fp/sp
      MI.getOperand(FrameRegIdx).ChangeToRegister(FrameReg, false);
      if (isSub) {
        if (AddrMode == ARMII::AddrMode5)
          // FIXME: Not consistent.
          ImmedOffset |= 1 << NumBits;
        else
          ImmedOffset = -ImmedOffset;
      }
      ImmOp.ChangeToImmediate(ImmedOffset);
      Offset = 0;
      return true;
    }

    // Otherwise, offset doesn't fit. Pull in what we can to simplify
    ImmedOffset = ImmedOffset & Mask;
    if (isSub) {
      if (AddrMode == ARMII::AddrMode5)
        // FIXME: Not consistent.
        ImmedOffset |= 1 << NumBits;
      else {
        ImmedOffset = -ImmedOffset;
        if (ImmedOffset == 0)
          // Change the opcode back if the encoded offset is zero.
          MI.setDesc(TII.get(positiveOffsetOpcode(NewOpc)));
      }
    }
    ImmOp.ChangeToImmediate(ImmedOffset);
    Offset &= ~(Mask*Scale);
  }

  Offset = (isSub) ? -Offset : Offset;
  return Offset == 0;
}

/// scheduleTwoAddrSource - Schedule the copy / re-mat of the source of the
/// two-addrss instruction inserted by two-address pass.
void
Thumb2InstrInfo::scheduleTwoAddrSource(MachineInstr *SrcMI,
                                       MachineInstr *UseMI,
                                       const TargetRegisterInfo &TRI) const {
  if (SrcMI->getOpcode() != ARM::tMOVgpr2gpr ||
      SrcMI->getOperand(1).isKill())
    return;

  unsigned PredReg = 0;
  ARMCC::CondCodes CC = llvm::getInstrPredicate(UseMI, PredReg);
  if (CC == ARMCC::AL || PredReg != ARM::CPSR)
    return;

  // Schedule the copy so it doesn't come between previous instructions
  // and UseMI which can form an IT block.
  unsigned SrcReg = SrcMI->getOperand(1).getReg();
  ARMCC::CondCodes OCC = ARMCC::getOppositeCondition(CC);
  MachineBasicBlock *MBB = UseMI->getParent();
  MachineBasicBlock::iterator MBBI = SrcMI;
  unsigned NumInsts = 0;
  while (--MBBI != MBB->begin()) {
    if (MBBI->isDebugValue())
      continue;

    MachineInstr *NMI = &*MBBI;
    ARMCC::CondCodes NCC = llvm::getInstrPredicate(NMI, PredReg);
    if (!(NCC == CC || NCC == OCC) ||
        NMI->modifiesRegister(SrcReg, &TRI) ||
        NMI->definesRegister(ARM::CPSR))
      break;
    if (++NumInsts == 4)
      // Too many in a row!
      return;
  }

  if (NumInsts) {
    MBB->remove(SrcMI);
    MBB->insert(++MBBI, SrcMI);
  }
}

ARMCC::CondCodes
llvm::getITInstrPredicate(const MachineInstr *MI, unsigned &PredReg) {
  unsigned Opc = MI->getOpcode();
  if (Opc == ARM::tBcc || Opc == ARM::t2Bcc)
    return ARMCC::AL;
  return llvm::getInstrPredicate(MI, PredReg);
}

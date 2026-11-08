// XNU adapter extraction; source hashes in transport-sources.json.
/** @file
  gts7xlwifi copy of UniXNU-umi-latest 3eb7257 package/touch/UmiRpmh.inc: the touch rails are PM8150 L13 (AVDD 3.3 V) and
  L14 (DVDD 1.8 V), "ldoa13"/"ldoa14" in the command DB (live FDT rpmh-regulator-ldoa13/14). The lookup takes the
  resource name, and a vote writes any value to any of the VRM words (0 voltage mV, 4 enable, 8 mode), one ACTIVE
  request on APPS RSC DRV2 at a time, so several votes can follow one another.
  Original: one bounded ldoc1 ACTIVE enable request on APPS RSC DRV2.
  Independent implementation; hardware references and limits in SOURCES.txt.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#define DB_POINTER       0x0C3F000CU
#define DB_BASE          0x80860000U
#define DB_SIZE          0x20000U
#define DB_DATA          0x90U
#define RSC_BASE         0x18220000U
#define TCS_BASE         (RSC_BASE + 0xD00U)
#define TCS_STRIDE       0x2A0U
#define IRQ_ENABLE       0x00U
#define IRQ_STATUS       0x04U
#define IRQ_CLEAR        0x08U
#define CMD_WAIT         0x10U
#define CONTROL          0x14U
#define STATUS           0x18U
#define CMD_ENABLE       0x1CU
#define CMD_MSGID        0x30U
#define CMD_ADDRESS      0x34U
#define CMD_DATA         0x38U
#define CMD_STATUS       0x3CU
#define AMC_ENABLE       BIT16
#define AMC_TRIGGER      BIT24
#define CMD_COMPLETE     (BIT8 | BIT16)
#define WRITE_RESPONSE   (8U | BIT8 | BIT16)
#define TRANSACTION_NS   20000000ULL
#define IRQ_MASK_NS      1000000ULL

typedef struct {
  UINT32 Control;
  UINT32 Wait;
  UINT32 Enable;
  UINT32 MsgId;
  UINT32 Address;
  UINT32 Data;
  UINT32 IrqBit;
} UMI_TCS_SNAPSHOT;

STATIC BOOLEAN mInCall;
STATIC BOOLEAN mSlotOwned;
STATIC BOOLEAN mInFlight;
STATIC BOOLEAN mInFlightEnable;
STATIC BOOLEAN mVoteOutstanding;
STATIC UINTN mTcs;
STATIC UINT32 mAddress;
STATIC UMI_TCS_SNAPSHOT mSaved;

STATIC BOOLEAN
Expired (UINT64 Start, UINT64 Limit)
{
  return GetTimeInNanoSecond (GetPerformanceCounter () - Start) >= Limit;
}

STATIC UINTN
Register (UINTN Tcs, UINTN Offset)
{
  return TCS_BASE + TCS_STRIDE * Tcs + Offset;
}

STATIC EFI_STATUS
WaitChecked (UINTN Address, UINT32 Value, UINT32 Mask, UINT64 Start, UINT64 Limit)
{
  while ((MmioRead32 (Address) & Mask) != (Value & Mask)) {
    if (Expired (Start, Limit)) {
      return EFI_TIMEOUT;
    }
    MicroSecondDelay (1);
  }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
WriteChecked (UINTN Address, UINT32 Value, UINT32 Mask, UINT64 Start, UINT64 Limit)
{
  if (Expired (Start, Limit)) {
    return EFI_TIMEOUT;
  }
  MmioWrite32 (Address, Value);
  MemoryFence ();
  return WaitChecked (Address, Value, Mask, Start, Limit);
}

STATIC EFI_STATUS
SetOwnIrq (BOOLEAN Enable, UINT64 Start, UINT64 Limit)
{
  UINT32 Bit = 1U << mTcs;
  UINT32 Value = MmioRead32 (TCS_BASE + IRQ_ENABLE);
  Value = (Value & ~Bit) | (Enable ? Bit : 0);
  return WriteChecked (TCS_BASE + IRQ_ENABLE, Value, Bit, Start, Limit);
}

STATIC UINT32 mKey0, mKey1, mWord, mValue;

STATIC EFI_STATUS
FindTouchResource (UINT32 *Address, UINT64 Start)
{
  UINTN Group;
  UINTN Entry;
  UINTN Header;
  UINTN Offset;
  UINTN Count;
  UINTN Matches = 0;
  UINT32 Found = 0;
  UINT32 Pointer = MmioRead32 (DB_POINTER);
  UINT32 Version;
  UINT32 Magic;
  UINT16 Slave;

  DEBUG ((DEBUG_WARN, "UMI rpmh cmd-db pointer=%08x\n", Pointer));
  if (Pointer != DB_BASE) {
    return EFI_UNSUPPORTED;
  }
  Version = MmioRead32 (DB_BASE);
  Magic = MmioRead32 (DB_BASE + 4);
  DEBUG ((DEBUG_WARN, "UMI rpmh cmd-db version=%08x magic=%08x\n", Version, Magic));
  if ((Version != 1) || (Magic != 0x0C0330DBU) ||
      ((MmioRead32 (DB_BASE + 0x8C) & BIT0) != 0)) {
    return EFI_UNSUPPORTED;
  }
  for (Group = 0; Group < 8; Group++) {
    Header = DB_BASE + 8 + Group * 16;
    Slave = MmioRead16 (Header);
    if (Slave == 0) {
      break;
    }
    Offset = MmioRead16 (Header + 2);
    Count = MmioRead16 (Header + 6);
    if (((Offset & 3) != 0) || (Offset > DB_SIZE - DB_DATA) ||
        (Count > (DB_SIZE - DB_DATA - Offset) / 24)) {
      return EFI_COMPROMISED_DATA;
    }
    for (Entry = 0; Entry < Count; Entry++) {
      if (Expired (Start, TRANSACTION_NS)) {
        return EFI_TIMEOUT;
      }
      Header = DB_BASE + DB_DATA + Offset + Entry * 24;
      // Exact padded 8-byte key (mKey0/mKey1), read as little-endian words.
      // gts7xlwifi 2026-11-05: the T970 command DB lists smpa4/5/6 and smpc8 twice (periph2 [t970 cmddb] rows). Linux's
      // cmd_db_get_header() returns the first match, so the first one is kept and later duplicates are only counted.
      if ((MmioRead32 (Header) == mKey0) &&
          (MmioRead32 (Header + 4) == mKey1) && (Matches == 0)) {
        Found = MmioRead32 (Header + 16);
        // VRM (slave 4) and, for the audio DSP's lcx.lvl/lmx.lvl level votes (package/T970Adsp.cpp), ARC (slave 3): word 0 only.
        if (((Slave != 4) && (Slave != 3)) || (Found > 0x7FFFFU) ||
            ((Found & 3) != 0) || (((Found >> 16) & 7) != Slave) ||
            ((Slave == 3) && (mWord != 0)) ||
            ((((Found + 4) >> 16) & 7) != Slave)) {
          return EFI_COMPROMISED_DATA;
        }
        Matches++;
      }
    }
  }
  if (Matches == 0) {
    return EFI_NOT_FOUND;
  }
  *Address = Found + mWord;
  DEBUG ((DEBUG_WARN, "T970 rpmh %08x%08x base=%08x word=%08x value=%u\n", mKey1, mKey0, Found, *Address, mValue));
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
SelectSlot (VOID)
{
  UINT32 Config = MmioRead32 (RSC_BASE + 0x0C);
  UINT32 Irq = MmioRead32 (TCS_BASE + IRQ_STATUS);
  UINT32 State[2];
  UINT32 Ctl[2];
  UINT32 En[2];
  UINT32 Wait[2];
  UINTN Index;
  UINTN Selected = 2;

  DEBUG ((DEBUG_WARN, "UMI rpmh DRV2 cfg=%08x irq-en=%08x irq=%08x\n",
          Config, MmioRead32 (TCS_BASE + IRQ_ENABLE), Irq));
  if ((((Config >> 12) & 0x3F) != 8) || (((Config >> 27) & 0x1F) != 16)) {
    return EFI_UNSUPPORTED;
  }
  for (Index = 0; Index < 2; Index++) {
    State[Index] = MmioRead32 (Register (Index, STATUS));
    Ctl[Index] = MmioRead32 (Register (Index, CONTROL));
    En[Index] = MmioRead32 (Register (Index, CMD_ENABLE));
    Wait[Index] = MmioRead32 (Register (Index, CMD_WAIT));
    DEBUG ((DEBUG_WARN, "UMI rpmh TCS%u sts=%08x ctrl=%08x en=%08x wait=%08x\n",
            (UINT32)Index, State[Index], Ctl[Index], En[Index], Wait[Index]));
    // Refuse concurrent ACTIVE traffic, including a different idle candidate.
    if (((State[Index] & BIT0) == 0) || ((Irq & (1U << Index)) != 0)) {
      return EFI_NOT_READY;
    }
    // Attempt13 inherited idle, empty slots with CONTROL=0. Only accept the
    // documented AMC bits; initialize their sequence before issuing a request.
    if ((Selected == 2) && (En[Index] == 0) && (Wait[Index] == 0) &&
        ((Ctl[Index] & ~(AMC_ENABLE | AMC_TRIGGER)) == 0)) {
      Selected = Index;
    }
  }
  if (Selected == 2) {
    return EFI_NOT_READY;
  }
  mTcs = Selected;
  mSaved.Control = Ctl[Selected];
  mSaved.Wait = Wait[Selected];
  mSaved.Enable = En[Selected];
  mSaved.MsgId = MmioRead32 (Register (mTcs, CMD_MSGID));
  mSaved.Address = MmioRead32 (Register (mTcs, CMD_ADDRESS));
  mSaved.Data = MmioRead32 (Register (mTcs, CMD_DATA));
  mSaved.IrqBit = MmioRead32 (TCS_BASE + IRQ_ENABLE) & (1U << mTcs);
  mSlotOwned = TRUE; // Before any write, including IRQ or trigger clearing.
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
FinishInFlight (UINT64 Start)
{
  UINT32 State;
  UINT32 Irq;
  UINT32 CommandStatus;
  if (!mInFlight) {
    return EFI_SUCCESS;
  }
  for (;;) {
    State = MmioRead32 (Register (mTcs, STATUS));
    Irq = MmioRead32 (TCS_BASE + IRQ_STATUS);
    CommandStatus = MmioRead32 (Register (mTcs, CMD_STATUS));
    if (((State & BIT0) != 0) && ((Irq & (1U << mTcs)) != 0)) {
      mInFlight = FALSE;
      if ((CommandStatus & CMD_COMPLETE) != CMD_COMPLETE) {
        // Hardware ended the request with an error. It is safe to reclaim
        // the idle slot, but the request may have changed the resource.
        // In particular, a failed disable must retain the enable marker.
        DEBUG ((DEBUG_WARN, "UMI rpmh error completion TCS%u enable=%u cmd-sts=%08x; vote uncertain\n",
                (UINT32)mTcs, mInFlightEnable, CommandStatus));
        return EFI_DEVICE_ERROR;
      }
      mVoteOutstanding = mInFlightEnable;
      DEBUG ((DEBUG_WARN, "UMI rpmh completed TCS%u enable=%u cmd-sts=%08x\n",
              (UINT32)mTcs, mInFlightEnable, CommandStatus));
      return EFI_SUCCESS;
    }
    if (Expired (Start, TRANSACTION_NS)) {
      DEBUG ((DEBUG_WARN, "UMI rpmh inflight TCS%u sts=%08x ctrl=%08x irq=%08x cmd-sts=%08x; retained\n",
              (UINT32)mTcs, State, MmioRead32 (Register (mTcs, CONTROL)), Irq, CommandStatus));
      return EFI_TIMEOUT;
    }
    MicroSecondDelay (1);
  }
}

STATIC EFI_STATUS
ClearAmcControl (UINT64 Start)
{
  UINT32 Control = MmioRead32 (Register (mTcs, CONTROL)) & ~AMC_TRIGGER;
  EFI_STATUS Result;

  // Hardware requires trigger clear/readback BEFORE mode clear/readback.
  Result = WriteChecked (Register (mTcs, CONTROL), Control,
                          MAX_UINT32, Start, TRANSACTION_NS);
  if (!EFI_ERROR (Result)) {
    Result = WriteChecked (Register (mTcs, CONTROL), Control & ~AMC_ENABLE,
                            MAX_UINT32, Start, TRANSACTION_NS);
  }
  return Result;
}

STATIC EFI_STATUS
RestoreSlot (UINT64 Start)
{
  EFI_STATUS Result;
  UINT32 IrqValue;
  UINT32 Bit = 1U << mTcs;
  if (!mSlotOwned) {
    return EFI_SUCCESS;
  }
  if (mInFlight || ((MmioRead32 (Register (mTcs, STATUS)) & BIT0) == 0)) {
    return EFI_NOT_READY;
  }
  Result = ClearAmcControl (Start);
  if (!EFI_ERROR (Result)) {
    Result = WriteChecked (Register (mTcs, CMD_ENABLE), 0, MAX_UINT32, Start, TRANSACTION_NS);
  }
  if (!EFI_ERROR (Result)) {
    Result = WriteChecked (Register (mTcs, CMD_WAIT), mSaved.Wait, MAX_UINT32, Start, TRANSACTION_NS);
  }
  if (!EFI_ERROR (Result)) {
    Result = WriteChecked (Register (mTcs, CMD_MSGID), mSaved.MsgId, MAX_UINT32, Start, TRANSACTION_NS);
  }
  if (!EFI_ERROR (Result)) {
    Result = WriteChecked (Register (mTcs, CMD_ADDRESS), mSaved.Address, MAX_UINT32, Start, TRANSACTION_NS);
  }
  if (!EFI_ERROR (Result)) {
    Result = WriteChecked (Register (mTcs, CMD_DATA), mSaved.Data, MAX_UINT32, Start, TRANSACTION_NS);
  }
  if (!EFI_ERROR (Result)) {
    // Never replay an inherited trigger=1, even if its old command completed.
    Result = WriteChecked (Register (mTcs, CONTROL), mSaved.Control & ~AMC_TRIGGER,
                            MAX_UINT32, Start, TRANSACTION_NS);
  }
  if (EFI_ERROR (Result)) {
    return Result;
  }
  if (Expired (Start, TRANSACTION_NS)) {
    return EFI_TIMEOUT;
  }
  MmioWrite32 (TCS_BASE + IRQ_CLEAR, Bit); // W1C this slot only, not a replay.
  MemoryFence ();
  while ((MmioRead32 (TCS_BASE + IRQ_STATUS) & Bit) != 0) {
    if (Expired (Start, TRANSACTION_NS)) {
      return EFI_TIMEOUT;
    }
    MicroSecondDelay (1);
  }
  IrqValue = MmioRead32 (TCS_BASE + IRQ_ENABLE);
  IrqValue = (IrqValue & ~Bit) | mSaved.IrqBit;
  Result = WriteChecked (TCS_BASE + IRQ_ENABLE, IrqValue, Bit, Start, TRANSACTION_NS);
  if (!EFI_ERROR (Result)) {
    mSlotOwned = FALSE;
  }
  return Result;
}

STATIC EFI_STATUS
SendVote (BOOLEAN Enable, UINT64 Start)
{
  EFI_STATUS Result = SelectSlot ();
  if (EFI_ERROR (Result)) {
    return Result;
  }
  Result = ClearAmcControl (Start);
  if (!EFI_ERROR (Result)) {
    Result = SetOwnIrq (TRUE, Start, TRANSACTION_NS);
  }
  if (!EFI_ERROR (Result)) {
    Result = WriteChecked (Register (mTcs, CMD_MSGID), WRITE_RESPONSE, MAX_UINT32, Start, TRANSACTION_NS);
  }
  if (!EFI_ERROR (Result)) {
    Result = WriteChecked (Register (mTcs, CMD_ADDRESS), mAddress, MAX_UINT32, Start, TRANSACTION_NS);
  }
  if (!EFI_ERROR (Result)) {
    Result = WriteChecked (Register (mTcs, CMD_DATA), Enable ? mValue : 0, MAX_UINT32, Start, TRANSACTION_NS);
  }
  if (!EFI_ERROR (Result)) {
    Result = WriteChecked (Register (mTcs, CMD_WAIT), BIT0, MAX_UINT32, Start, TRANSACTION_NS);
  }
  if (!EFI_ERROR (Result)) {
    Result = WriteChecked (Register (mTcs, CMD_ENABLE), BIT0, MAX_UINT32, Start, TRANSACTION_NS);
  }
  if (!EFI_ERROR (Result)) {
    Result = WriteChecked (Register (mTcs, CONTROL), AMC_ENABLE,
                            MAX_UINT32, Start, TRANSACTION_NS);
  }
  if (EFI_ERROR (Result)) {
    return Result; // No trigger issued; caller can restore only while idle.
  }
  DEBUG ((DEBUG_WARN, "UMI rpmh request DRV2 TCS%u addr=%08x enable=%u; inherited DRV vote unknown\n",
          (UINT32)mTcs, mAddress, Enable));
  if (Expired (Start, TRANSACTION_NS)) {
    return EFI_TIMEOUT; // Still untriggered; cleanup can restore an idle slot.
  }
  mInFlight = TRUE; // A trigger write can take effect even if readback fails.
  mInFlightEnable = Enable;
  if (Enable) {
    mVoteOutstanding = TRUE;
  }
  // No deadline-rejecting helper between marking inflight and actual issue.
  MmioWrite32 (Register (mTcs, CONTROL), AMC_ENABLE | AMC_TRIGGER);
  MemoryFence ();
  Result = WaitChecked (Register (mTcs, CONTROL), AMC_ENABLE | AMC_TRIGGER,
                         MAX_UINT32, Start, TRANSACTION_NS);
  if (!EFI_ERROR (Result)) {
    Result = FinishInFlight (Start);
  }
  if (!EFI_ERROR (Result)) {
    Result = RestoreSlot (Start);
  }
  return Result;
}

STATIC EFI_STATUS
EndCall (EFI_STATUS Result, BOOLEAN Interrupts)
{
  if (EFI_ERROR (Result) && mSlotOwned) {
    // Quarantine a late completion before restoring CPU IRQ state. Do not
    // overwrite command/control registers of a busy or unconfirmed TCS.
    EFI_STATUS MaskStatus = SetOwnIrq (FALSE, GetPerformanceCounter (), IRQ_MASK_NS);
    DEBUG ((DEBUG_WARN, "UMI rpmh retained slot=%u inflight=%u vote=%u irq-mask=%d\n",
            (UINT32)mTcs, mInFlight, mVoteOutstanding, MaskStatus));
    (VOID)MaskStatus;
  }
  SetInterruptState (Interrupts);
  mInCall = FALSE;
  return Result;
}

BOOLEAN
UmiRpmhTouchOutstanding (VOID)
{
  return mSlotOwned || mInFlight || mVoteOutstanding;
}

BOOLEAN
UmiRpmhTouchVoteSettled (VOID)
{
  return mVoteOutstanding && !mInFlight && !mSlotOwned && !mInCall;
}

EFI_STATUS
UmiRpmhTouchEnable (VOID)
{
  BOOLEAN Interrupts;
  EFI_STATUS Result;
  UINT64 Start;
  if (mInCall || UmiRpmhTouchOutstanding ()) {
    return EFI_ALREADY_STARTED;
  }
  mInCall = TRUE;
  Interrupts = SaveAndDisableInterrupts ();
  Start = GetPerformanceCounter ();
  Result = FindTouchResource (&mAddress, Start);
  if (!EFI_ERROR (Result)) {
    Result = SendVote (TRUE, Start);
  }
  if (EFI_ERROR (Result) && mSlotOwned && !mInFlight) {
    // A failure before triggering may be safely unwound while still idle.
    (VOID)RestoreSlot (Start);
  }
  return EndCall (Result, Interrupts);
}

EFI_STATUS
UmiRpmhTouchDisable (VOID)
{
  BOOLEAN Interrupts;
  EFI_STATUS Result;
  UINT64 Start;
  if (mInCall) {
    return EFI_ALREADY_STARTED;
  }
  if (!UmiRpmhTouchOutstanding ()) {
    return EFI_SUCCESS;
  }
  mInCall = TRUE;
  Interrupts = SaveAndDisableInterrupts ();
  Start = GetPerformanceCounter ();
  Result = EFI_SUCCESS;
  if (mInFlight) {
    Result = SetOwnIrq (TRUE, Start, TRANSACTION_NS);
    if (!EFI_ERROR (Result)) {
      Result = FinishInFlight (Start);
    }
  }
  if (!EFI_ERROR (Result)) {
    Result = RestoreSlot (Start);
  }
  if (!EFI_ERROR (Result) && mVoteOutstanding) {
    DEBUG ((DEBUG_WARN, "UMI rpmh withdraw current DRV2 enable request; no claim to reconstruct inherited vote\n"));
    Result = SendVote (FALSE, Start);
  }
  return EndCall (Result, Interrupts);
}

// gts7xlwifi: one ACTIVE write of Value to VRM word Word (0 voltage in mV, 4 enable, 8 mode) of the resource whose
// command-DB name is Name (at most 8 bytes). A completed vote is left in place, and the next call may follow it.
EFI_STATUS
T970RpmhWrite (CONST char *Name, UINT32 Word, UINT32 Value)
{
  UINT8 Key[8] = {0};
  for (UINTN i = 0; i < 8 && Name[i]; i++) {
    Key[i] = (UINT8)Name[i];
  }
  mKey0 = (UINT32)Key[0] | ((UINT32)Key[1] << 8) | ((UINT32)Key[2] << 16) | ((UINT32)Key[3] << 24);
  mKey1 = (UINT32)Key[4] | ((UINT32)Key[5] << 8) | ((UINT32)Key[6] << 16) | ((UINT32)Key[7] << 24);
  mWord = Word;
  mValue = Value;
  if (UmiRpmhTouchVoteSettled ()) {
    mVoteOutstanding = FALSE;   /* the previous vote completed; it stays applied */
  }
  return UmiRpmhTouchEnable ();
}

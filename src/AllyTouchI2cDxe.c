/** @file
  AllyTouchI2cDxe -- EFI_ABSOLUTE_POINTER_PROTOCOL producer for the ROG Xbox
  Ally X Novatek NVTK0603 I2C-HID touchscreen, so rEFInd can be driven by touch.

    Layer 0  FCH AOAC power un-gating                 -- FchAoac.h + here
    Layer 1  DesignWare I2C master (MMIO, polled)     -- DwI2c.c
    Layer 2  HID-over-I2C transport                   -- I2cHid.c
    Layer 3  report parse -> AbsolutePointer          -- HidParse.c + here

  Bring-up model (updated after on-hardware testing, 2026-07-17):

  In a normal boot the firmware leaves the FCH I2C controller tile power-gated
  (AOAC device 5, see FchAoac.h), so the controller MMIO window reads garbage
  until the tile is powered on -- and a driver load failure from that state
  kept rEFInd from launching. Two consequences drive the shape of this driver:

    1. The entry point never returns an error. If bring-up fails, the driver
       stays resident and retries on a 1 s timer; the boot manager is never
       exposed to a load failure.

    2. The driver un-gates the I2C tile itself via the AOAC registers --
       exactly what \_SB.I2CA._PS0 does -- before touching controller MMIO.
       If the tile was freshly powered (firmware never programmed it), the
       full 400 kHz timing is programmed rather than inherited.

  Detection targets the DSDT-confirmed panel first (controller 0xFEDC2000,
  slave 0x01, HID descriptor register 0x0000) and falls back to a sweep of
  the candidate controller bases x slave addresses x descriptor registers.

  The panel side may still need firmware initialization that only the
  platform can do. The firmware provides that path: holding VOLUME UP during
  the boot-animation splash powers and initializes the touchscreen before the
  boot loader runs, after which this driver finds a live panel immediately.

  Load it from the UEFI Shell first ("load AllyTouchI2cDxe.efi") to see the
  detection log; in rEFInd's drivers_x64/ the same output flashes by at boot.

  Copyright (c) 2026, jlobue10 and contributors. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Protocol/AbsolutePointer.h>

#include "DwI2c.h"
#include "FchAoac.h"
#include "I2cHid.h"
#include "HidParse.h"

//
// Device-specific constants, confirmed from the Ally X (RC73XA) DSDT dump
// (tools/allyx-hwinfo/acpi_disasm/DSDT.dsl):
//   \_SB.I2CA (AMDI0010, _UID 0): Memory32Fixed 0xFEDC2000, len 0x1000
//   \_SB.I2CA.TPL0 (_HID NVTK0603, _CID PNP0C50): I2cSerialBusV2 slave 0x01,
//     400 kHz; _DSM(HID-I2C, func 1) returns HID descriptor register 0x0000.
//
#define ALLY_I2C_BASE        DW_I2C_FCH_BASE_0   // 0xFEDC2000
#define ALLY_I2C_SLAVE_ADDR  NVTK_I2C_ADDR       // 0x01
#define ALLY_HID_DESC_REG    0x0000

//
// Poll every 10 ms (EFI timer units are 100 ns). At 400 kHz a full
// wMaxInputLength read takes well under 2 ms; 100 Hz polling keeps menu
// tracking snappy.
//
#define ALLY_POLL_PERIOD      100000

#define ALLY_RETRY_PERIOD     (1000 * 1000 * 10)  // 1 s in 100 ns units
#define ALLY_RETRY_MAX        30                  // give up after ~30 s

//
// After RESET the device posts a 2-byte zero "reset acknowledge" on the input
// path; wait up to this many 1 ms tries for it, then proceed regardless
// (if the firmware already initialized the panel, a missed ack is not fatal).
//
#define ALLY_RESET_ACK_TRIES  200

typedef struct {
  UINT64                          Signature;
  EFI_ABSOLUTE_POINTER_PROTOCOL   AbsolutePointer;
  EFI_ABSOLUTE_POINTER_MODE       Mode;
  EFI_ABSOLUTE_POINTER_STATE      State;
  BOOLEAN                         StateChanged;
  BOOLEAN                         LastTip;
  BOOLEAN                         NeedReinit;
  BOOLEAN                         Verbose;        // Print() allowed (load time)
  UINT32                          I2cBase;
  UINT8                           SlaveAddr;
  I2C_HID_DESCRIPTOR              HidDesc;
  HID_TOUCH_LAYOUT                Layout;
  UINT8                           *InputBuf;      // wMaxInputLength bytes
  EFI_EVENT                       PollEvent;
  EFI_EVENT                       ExitBootEvent;
  EFI_EVENT                       RetryEvent;
  UINTN                           RetryCount;
  EFI_HANDLE                      Handle;
} ALLY_TOUCH_DEV;

#define ALLY_TOUCH_SIG  SIGNATURE_64 ('A','l','l','y','T','c','h','1')
#define ALLY_TOUCH_FROM_ABS(a) BASE_CR (a, ALLY_TOUCH_DEV, AbsolutePointer)

STATIC CONST UINT32  mCandidateBases[] = {
  DW_I2C_FCH_BASE_0, DW_I2C_FCH_BASE_1, DW_I2C_FCH_BASE_2,
  DW_I2C_FCH_BASE_3, DW_I2C_FCH_BASE_4
};
STATIC CONST UINT8   mCandidateAddrs[]    = {
  NVTK_I2C_ADDR, GOODIX_I2C_ADDR_A, GOODIX_I2C_ADDR_B
};
STATIC CONST UINT16  mCandidateDescRegs[] = { 0x0000, 0x0001, 0x0020 };

// ---------------------------------------------------------------------------
// Layer 0: FCH AOAC power gating
// ---------------------------------------------------------------------------

/**
  Make sure the I2C controller tile is out of D3 before its MMIO window is
  touched. Mirrors \_SB.I2CA._PS0 -> \_SB.DSAD (5, 0): request D0, set
  PwrOnDev, wait for the state ladder to report fully-on.

  @param[out] FreshPowerOn  TRUE if the tile was gated and this call powered
                            it on (i.e. firmware never programmed it and the
                            bus timing must not be inherited).

  @retval EFI_SUCCESS   Tile reports D0.
  @retval EFI_TIMEOUT   Tile never reached D0.
**/
STATIC
EFI_STATUS
FchAoacPowerOnI2c (
  OUT BOOLEAN  *FreshPowerOn
  )
{
  UINT8  Ctl;
  UINTN  WaitedUs;

  *FreshPowerOn = FALSE;

  if ((MmioRead8 (FCH_AOAC_DEV_STATUS (FCH_AOAC_DEV_I2C0)) & FCH_AOAC_STATE_MASK)
      == FCH_AOAC_STATE_D0) {
    return EFI_SUCCESS;
  }

  Ctl  = MmioRead8 (FCH_AOAC_DEV_CTL (FCH_AOAC_DEV_I2C0));
  Ctl &= ~FCH_AOAC_TARGET_STATE_MASK;            // target D0
  Ctl |= FCH_AOAC_PWR_ON_DEV;
  MmioWrite8 (FCH_AOAC_DEV_CTL (FCH_AOAC_DEV_I2C0), Ctl);

  for (WaitedUs = 0; WaitedUs < 100000; WaitedUs += 100) {
    if ((MmioRead8 (FCH_AOAC_DEV_STATUS (FCH_AOAC_DEV_I2C0)) & FCH_AOAC_STATE_MASK)
        == FCH_AOAC_STATE_D0) {
      *FreshPowerOn = TRUE;
      DEBUG ((DEBUG_INFO, "AllyTouch: AOAC powered on I2C0 tile\n"));
      return EFI_SUCCESS;
    }
    gBS->Stall (100);
  }

  DEBUG ((DEBUG_ERROR, "AllyTouch: AOAC power-on of I2C0 timed out\n"));
  return EFI_TIMEOUT;
}

// ---------------------------------------------------------------------------
// Layer 3: EFI_ABSOLUTE_POINTER_PROTOCOL
// ---------------------------------------------------------------------------

EFI_STATUS
EFIAPI
AllyTouchReset (
  IN EFI_ABSOLUTE_POINTER_PROTOCOL  *This,
  IN BOOLEAN                        ExtendedVerification
  )
{
  ALLY_TOUCH_DEV  *Dev = ALLY_TOUCH_FROM_ABS (This);
  EFI_TPL         OldTpl;

  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  ZeroMem (&Dev->State, sizeof (Dev->State));
  Dev->StateChanged = FALSE;
  Dev->LastTip      = FALSE;
  if (ExtendedVerification) {
    Dev->NeedReinit = TRUE;   // next poll re-inits the master and re-powers
  }
  gBS->RestoreTPL (OldTpl);
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
AllyTouchGetState (
  IN OUT EFI_ABSOLUTE_POINTER_PROTOCOL  *This,
  OUT    EFI_ABSOLUTE_POINTER_STATE     *State
  )
{
  ALLY_TOUCH_DEV  *Dev = ALLY_TOUCH_FROM_ABS (This);
  EFI_TPL         OldTpl;

  if (State == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  if (!Dev->StateChanged) {
    gBS->RestoreTPL (OldTpl);
    return EFI_NOT_READY;
  }
  CopyMem (State, &Dev->State, sizeof (Dev->State));
  Dev->StateChanged = FALSE;
  gBS->RestoreTPL (OldTpl);
  return EFI_SUCCESS;
}

STATIC
VOID
EFIAPI
AllyTouchWaitForInput (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  if (((ALLY_TOUCH_DEV *)Context)->StateChanged) {
    gBS->SignalEvent (Event);
  }
}

//
// Poll timer: read one input report, extract tip/X/Y, update State.
//
STATIC
VOID
EFIAPI
AllyTouchPoll (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  ALLY_TOUCH_DEV  *Dev = (ALLY_TOUCH_DEV *)Context;
  EFI_STATUS      Status;
  UINTN           Len;
  UINT8           *Payload;
  UINTN           PayloadLen;
  BOOLEAN         Tip;
  UINT32          X;
  UINT32          Y;

  if (Dev->NeedReinit) {
    if (EFI_ERROR (DwI2cInit (Dev->I2cBase, Dev->SlaveAddr, FALSE))) {
      return;
    }
    (VOID)I2cHidSetPower (Dev->I2cBase, Dev->HidDesc.wCommandRegister,
                          I2C_HID_POWER_ON);
    Dev->NeedReinit = FALSE;
  }

  Status = I2cHidRawRead (Dev->I2cBase, Dev->InputBuf,
                          Dev->HidDesc.wMaxInputLength);
  if (Status == EFI_NO_RESPONSE) {
    return;                       // device NAKed: nothing pending
  }
  if (EFI_ERROR (Status)) {
    Dev->NeedReinit = TRUE;       // stuck bus -- recover on the next tick
    return;
  }

  Len = Dev->InputBuf[0] | ((UINTN)Dev->InputBuf[1] << 8);
  if ((Len < 3) || (Len > Dev->HidDesc.wMaxInputLength)) {
    return;                       // zero-length sentinel / garbage: no report
  }

  Payload    = Dev->InputBuf + 2;
  PayloadLen = Len - 2;
  if (Dev->Layout.HasReportId) {
    if (Payload[0] != Dev->Layout.ReportId) {
      return;                     // some other report (pen, vendor, ...)
    }
    Payload++;
    PayloadLen--;
  }

  Tip = (BOOLEAN)(HidExtractBits (Payload, PayloadLen,
                                  Dev->Layout.TipBitOffset, 1) != 0);
  X   = HidExtractBits (Payload, PayloadLen,
                        Dev->Layout.XBitOffset, Dev->Layout.XBitSize);
  Y   = HidExtractBits (Payload, PayloadLen,
                        Dev->Layout.YBitOffset, Dev->Layout.YBitSize);

  if (Tip || Dev->LastTip) {
    //
    // Report position while touching plus one final lift-off event. On
    // lift-off keep the last coordinates so the release lands on the icon.
    //
    if (Tip) {
      Dev->State.CurrentX = X;
      Dev->State.CurrentY = Y;
    }
    Dev->State.CurrentZ      = 0;
    Dev->State.ActiveButtons = Tip ? EFI_ABSP_TouchActive : 0;
    Dev->StateChanged        = TRUE;
    Dev->LastTip             = Tip;
  }
}

STATIC
VOID
EFIAPI
AllyTouchExitBootServices (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  ALLY_TOUCH_DEV  *Dev = (ALLY_TOUCH_DEV *)Context;

  gBS->SetTimer (Dev->PollEvent, TimerCancel, 0);
  DwI2cDisable (Dev->I2cBase);    // leave the bus idle for the OS driver
}

// ---------------------------------------------------------------------------
// Detection + bring-up
// ---------------------------------------------------------------------------

/**
  Find the panel: un-gate the I2C tile, try the DSDT-confirmed target first,
  then sweep controllers x addresses x descriptor registers. On success
  Dev->I2cBase / Dev->SlaveAddr / Dev->HidDesc are filled and the controller
  is initialized and targeting the panel.
**/
STATIC
EFI_STATUS
AllyTouchDetect (
  IN OUT ALLY_TOUCH_DEV  *Dev
  )
{
  EFI_STATUS  Status;
  BOOLEAN     Fresh;
  UINTN       b, a, r;

  Status = FchAoacPowerOnI2c (&Fresh);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // Confirmed target: I2CA @ 0xFEDC2000, Novatek @ 0x01, descriptor @ 0x0000.
  //
  if (DwI2cControllerPresent (ALLY_I2C_BASE) &&
      !EFI_ERROR (DwI2cInit (ALLY_I2C_BASE, ALLY_I2C_SLAVE_ADDR, Fresh)) &&
      !EFI_ERROR (I2cHidReadDescriptor (ALLY_I2C_BASE, ALLY_HID_DESC_REG,
                                        &Dev->HidDesc))) {
    Dev->I2cBase   = ALLY_I2C_BASE;
    Dev->SlaveAddr = ALLY_I2C_SLAVE_ADDR;
    return EFI_SUCCESS;
  }

  //
  // Fallback sweep for panel/board variants. Only controllers whose tile is
  // already powered respond to the COMP_TYPE probe.
  //
  for (b = 0; b < ARRAY_SIZE (mCandidateBases); b++) {
    if (!DwI2cControllerPresent (mCandidateBases[b])) {
      continue;
    }
    for (a = 0; a < ARRAY_SIZE (mCandidateAddrs); a++) {
      if (EFI_ERROR (DwI2cInit (mCandidateBases[b], mCandidateAddrs[a],
                                (mCandidateBases[b] == ALLY_I2C_BASE) && Fresh))) {
        continue;
      }
      for (r = 0; r < ARRAY_SIZE (mCandidateDescRegs); r++) {
        if (!EFI_ERROR (I2cHidReadDescriptor (mCandidateBases[b],
                                              mCandidateDescRegs[r],
                                              &Dev->HidDesc))) {
          Dev->I2cBase   = mCandidateBases[b];
          Dev->SlaveAddr = mCandidateAddrs[a];
          return EFI_SUCCESS;
        }
      }
      DwI2cDisable (mCandidateBases[b]);
    }
  }
  return EFI_NOT_FOUND;
}

/**
  SET_POWER(ON) + RESET, then drain the reset acknowledge (best effort).
  This is the sequence the generic Linux i2c-hid driver uses to bring the
  panel from D3 to reporting.
**/
STATIC
VOID
AllyTouchBringUp (
  IN ALLY_TOUCH_DEV  *Dev
  )
{
  UINTN  Try;
  UINT8  Ack[2];

  (VOID)I2cHidSetPower (Dev->I2cBase, Dev->HidDesc.wCommandRegister,
                        I2C_HID_POWER_ON);
  gBS->Stall (2000);

  if (EFI_ERROR (I2cHidReset (Dev->I2cBase, Dev->HidDesc.wCommandRegister))) {
    return;
  }
  for (Try = 0; Try < ALLY_RESET_ACK_TRIES; Try++) {
    if (!EFI_ERROR (I2cHidRawRead (Dev->I2cBase, Ack, sizeof (Ack))) &&
        (Ack[0] == 0) && (Ack[1] == 0)) {
      return;
    }
    gBS->Stall (1000);
  }
  // No ack seen -- proceed anyway; the panel may already have been live.
}

/**
  One complete bring-up attempt: detect the panel, read + parse the report
  descriptor, power/reset the device, install the protocol. Safe to call
  repeatedly on the same Dev; a failed attempt cleans up after itself (Dev
  stays allocated for the next try).
**/
STATIC
EFI_STATUS
AllyTouchTryInit (
  IN OUT ALLY_TOUCH_DEV  *Dev
  )
{
  EFI_STATUS  Status;
  UINT8       *ReportDesc;

  ReportDesc = NULL;

  //
  // Clear state a previous failed attempt may have left behind.
  //
  ZeroMem (&Dev->HidDesc, sizeof (Dev->HidDesc));
  ZeroMem (&Dev->Layout, sizeof (Dev->Layout));
  ZeroMem (&Dev->State, sizeof (Dev->State));
  Dev->StateChanged = FALSE;
  Dev->LastTip      = FALSE;
  Dev->NeedReinit   = FALSE;

  Status = AllyTouchDetect (Dev);
  if (EFI_ERROR (Status)) {
    if (Dev->Verbose) {
      Print (L"AllyTouchI2cDxe: no HID-over-I2C touch panel answered "
             L"(panel not powered at this stage?) -- will keep retrying.\n"
             L"  Tip: holding VOLUME UP at the boot splash makes the firmware "
             L"initialize the touchscreen.\n");
    }
    return Status;
  }

  if (Dev->Verbose) {
    Print (L"AllyTouchI2cDxe: panel at I2C base 0x%08x addr 0x%02x, "
           L"VID 0x%04x PID 0x%04x\n",
           Dev->I2cBase, Dev->SlaveAddr,
           Dev->HidDesc.wVendorID, Dev->HidDesc.wProductID);
  }

  //
  // Read the report descriptor and locate the first contact's tip/X/Y.
  //
  ReportDesc = AllocateZeroPool (Dev->HidDesc.wReportDescLength);
  if (ReportDesc == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    goto Fail;
  }
  Status = I2cHidReadRegister (Dev->I2cBase, Dev->HidDesc.wReportDescRegister,
                               ReportDesc, Dev->HidDesc.wReportDescLength);
  if (EFI_ERROR (Status)) {
    if (Dev->Verbose) {
      Print (L"AllyTouchI2cDxe: report descriptor read failed: %r\n", Status);
    }
    goto Fail;
  }

  //
  // GT7868Q descriptor fixup carried over from ty2/goodix-gt7868q-linux-driver:
  // byte 607 is a Logical Minimum tag that should be Logical Maximum. Only
  // relevant if the sweep found a Goodix panel (other Ally-family boards).
  //
  if ((Dev->HidDesc.wVendorID == 0x27C6) &&
      (Dev->HidDesc.wReportDescLength > 608) && (ReportDesc[607] == 0x15)) {
    ReportDesc[607] = 0x25;
  }

  Status = HidParseTouchLayout (ReportDesc, Dev->HidDesc.wReportDescLength,
                                &Dev->Layout);
  if (EFI_ERROR (Status)) {
    if (Dev->Verbose) {
      Print (L"AllyTouchI2cDxe: no touch (tip+X+Y) input report found in the "
             L"report descriptor.\n");
    }
    goto Fail;
  }
  FreePool (ReportDesc);
  ReportDesc = NULL;

  //
  // A zero logical max would make consumers divide by zero when scaling.
  //
  if (Dev->Layout.XLogicalMax == 0) {
    Dev->Layout.XLogicalMax = 0xFFFF;
  }
  if (Dev->Layout.YLogicalMax == 0) {
    Dev->Layout.YLogicalMax = 0xFFFF;
  }

  Dev->InputBuf = AllocateZeroPool (Dev->HidDesc.wMaxInputLength);
  if (Dev->InputBuf == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    goto Fail;
  }

  AllyTouchBringUp (Dev);

  Dev->Mode.AbsoluteMinX = 0;
  Dev->Mode.AbsoluteMinY = 0;
  Dev->Mode.AbsoluteMinZ = 0;
  Dev->Mode.AbsoluteMaxX = Dev->Layout.XLogicalMax;
  Dev->Mode.AbsoluteMaxY = Dev->Layout.YLogicalMax;
  Dev->Mode.AbsoluteMaxZ = 0;
  Dev->Mode.Attributes   = 0;

  Dev->AbsolutePointer.Reset    = AllyTouchReset;
  Dev->AbsolutePointer.GetState = AllyTouchGetState;
  Dev->AbsolutePointer.Mode     = &Dev->Mode;

  Status = gBS->CreateEvent (EVT_NOTIFY_WAIT, TPL_NOTIFY,
                             AllyTouchWaitForInput, Dev,
                             &Dev->AbsolutePointer.WaitForInput);
  if (EFI_ERROR (Status)) {
    goto Fail;
  }

  Status = gBS->CreateEvent (EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_CALLBACK,
                             AllyTouchPoll, Dev, &Dev->PollEvent);
  if (EFI_ERROR (Status)) {
    goto Fail;
  }
  Status = gBS->SetTimer (Dev->PollEvent, TimerPeriodic, ALLY_POLL_PERIOD);
  if (EFI_ERROR (Status)) {
    goto Fail;
  }

  Status = gBS->CreateEvent (EVT_SIGNAL_EXIT_BOOT_SERVICES, TPL_NOTIFY,
                             AllyTouchExitBootServices, Dev,
                             &Dev->ExitBootEvent);
  if (EFI_ERROR (Status)) {
    goto Fail;
  }

  Status = gBS->InstallMultipleProtocolInterfaces (
                  &Dev->Handle,
                  &gEfiAbsolutePointerProtocolGuid, &Dev->AbsolutePointer,
                  NULL);
  if (EFI_ERROR (Status)) {
    goto Fail;
  }

  if (Dev->Verbose) {
    Print (L"AllyTouchI2cDxe: EFI_ABSOLUTE_POINTER_PROTOCOL installed "
           L"(%ux%u).\n",
           (UINT32)Dev->Mode.AbsoluteMaxX, (UINT32)Dev->Mode.AbsoluteMaxY);
  }
  DEBUG ((DEBUG_INFO, "AllyTouch: protocol installed (%dx%d)\n",
          (UINT32)Dev->Mode.AbsoluteMaxX, (UINT32)Dev->Mode.AbsoluteMaxY));
  return EFI_SUCCESS;

Fail:
  if (ReportDesc != NULL) {
    FreePool (ReportDesc);
  }
  if (Dev->ExitBootEvent != NULL) {
    gBS->CloseEvent (Dev->ExitBootEvent);
    Dev->ExitBootEvent = NULL;
  }
  if (Dev->PollEvent != NULL) {
    gBS->SetTimer (Dev->PollEvent, TimerCancel, 0);
    gBS->CloseEvent (Dev->PollEvent);
    Dev->PollEvent = NULL;
  }
  if (Dev->AbsolutePointer.WaitForInput != NULL) {
    gBS->CloseEvent (Dev->AbsolutePointer.WaitForInput);
    Dev->AbsolutePointer.WaitForInput = NULL;
  }
  if (Dev->InputBuf != NULL) {
    FreePool (Dev->InputBuf);
    Dev->InputBuf = NULL;
  }
  return Status;
}

// ---------------------------------------------------------------------------
// Retry loop and entry point
// ---------------------------------------------------------------------------

/**
  Periodic retry after a failed bring-up at load time. Covers the case where
  the panel becomes ready only after the boot manager (and this driver) have
  already started. Silent: rEFInd's menu is on screen by now, so no Print().
**/
STATIC
VOID
EFIAPI
AllyTouchRetry (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  ALLY_TOUCH_DEV  *Dev = (ALLY_TOUCH_DEV *)Context;

  Dev->RetryCount++;
  if (!EFI_ERROR (AllyTouchTryInit (Dev))) {
    DEBUG ((DEBUG_INFO, "AllyTouch: bring-up succeeded on retry %d\n",
            (UINT32)Dev->RetryCount));
  } else if (Dev->RetryCount < ALLY_RETRY_MAX) {
    return;                                // periodic timer fires again
  }

  gBS->SetTimer (Event, TimerCancel, 0);
  gBS->CloseEvent (Event);
  Dev->RetryEvent = NULL;
}

EFI_STATUS
EFIAPI
AllyTouchI2cDxeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS      Status;
  ALLY_TOUCH_DEV  *Dev;

  //
  // This driver must never fail its entry point: rEFInd treats a driver
  // load error as fatal enough to keep it from launching cleanly (observed
  // on hardware). If anything goes wrong the driver stays resident and
  // inert, or retries in the background.
  //
  Dev = AllocateZeroPool (sizeof (ALLY_TOUCH_DEV));
  if (Dev == NULL) {
    return EFI_SUCCESS;
  }
  Dev->Signature = ALLY_TOUCH_SIG;
  Dev->Verbose   = TRUE;

  Status = AllyTouchTryInit (Dev);
  Dev->Verbose = FALSE;
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "AllyTouch: bring-up failed at load: %r -- "
            "retrying every 1 s (max %d)\n", Status, ALLY_RETRY_MAX));
    Status = gBS->CreateEvent (EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_CALLBACK,
                               AllyTouchRetry, Dev, &Dev->RetryEvent);
    if (!EFI_ERROR (Status)) {
      gBS->SetTimer (Dev->RetryEvent, TimerPeriodic, ALLY_RETRY_PERIOD);
    }
  }

  return EFI_SUCCESS;
}

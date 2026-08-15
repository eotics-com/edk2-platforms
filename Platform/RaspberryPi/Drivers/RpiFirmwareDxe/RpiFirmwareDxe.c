/** @file
 *
 *  Copyright (c) 2023, Mario Bălănică <mariobalanica02@gmail.com>
 *  Copyright (c) 2020, Pete Batard <pete@akeo.ie>
 *  Copyright (c) 2019, ARM Limited. All rights reserved.
 *  Copyright (c) 2017-2020, Andrei Warkentin <andrey.warkentin@gmail.com>
 *  Copyright (c) 2016, Linaro, Ltd. All rights reserved.
 *
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 **/

#include <PiDxe.h>

#include <Library/ArmLib.h>
#include <Library/DmaLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/IoLib.h>
#include <Library/SynchronizationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeLib.h>

#include <IndustryStandard/Bcm2836Mbox.h>
#include <IndustryStandard/RpiMbox.h>

#include <Protocol/RpiFirmware.h>

//
// The number of statically allocated buffer pages
//
#define NUM_PAGES   1

//
// A complete Pi 5 pmic_read_adc response is about 1 KiB. Keep enough room for
// future channels while leaving the shared-page lock words outside the command.
//
#define RPI_FIRMWARE_GENCMD_TAG_SIZE   0x800
#define RPI_FIRMWARE_GENCMD_DATA_SIZE  (RPI_FIRMWARE_GENCMD_TAG_SIZE - sizeof (UINT32))

#pragma pack(1)
typedef struct {
  UINT32    BufferSize;
  UINT32    Response;
} RPI_FW_BUFFER_HEAD;

typedef struct {
  UINT32    TagId;
  UINT32    TagSize;
  UINT32    TagValueSize;
} RPI_FW_TAG_HEAD;

typedef struct {
  UINT32                    DeviceId;
  UINT32                    PowerState;
} RPI_FW_POWER_STATE_TAG;

typedef struct {
  RPI_FW_BUFFER_HEAD        BufferHead;
  RPI_FW_TAG_HEAD           TagHead;
  RPI_FW_POWER_STATE_TAG    TagBody;
  UINT32                    EndTag;
} RPI_FW_SET_POWER_STATE_CMD;

typedef struct {
  UINT32                    Base;
  UINT32                    Size;
} RPI_FW_ARM_MEMORY_TAG;

typedef struct {
  RPI_FW_BUFFER_HEAD        BufferHead;
  RPI_FW_TAG_HEAD           TagHead;
  RPI_FW_ARM_MEMORY_TAG     TagBody;
  UINT32                    EndTag;
} RPI_FW_GET_ARM_MEMORY_CMD;

typedef struct {
  UINT8                     MacAddress[6];
  UINT32                    Padding;
} RPI_FW_MAC_ADDR_TAG;

typedef struct {
  RPI_FW_BUFFER_HEAD        BufferHead;
  RPI_FW_TAG_HEAD           TagHead;
  RPI_FW_MAC_ADDR_TAG       TagBody;
  UINT32                    EndTag;
} RPI_FW_GET_MAC_ADDR_CMD;

typedef struct {
  UINT64                    Serial;
} RPI_FW_SERIAL_TAG;

typedef struct {
  RPI_FW_BUFFER_HEAD        BufferHead;
  RPI_FW_TAG_HEAD           TagHead;
  RPI_FW_SERIAL_TAG         TagBody;
  UINT32                    EndTag;
} RPI_FW_GET_SERIAL_CMD;

typedef struct {
  UINT32                    Model;
} RPI_FW_MODEL_TAG;

typedef struct {
  RPI_FW_BUFFER_HEAD        BufferHead;
  RPI_FW_TAG_HEAD           TagHead;
  RPI_FW_MODEL_TAG          TagBody;
  UINT32                    EndTag;
} RPI_FW_GET_MODEL_CMD;

typedef struct {
  UINT32                    Revision;
} RPI_FW_MODEL_REVISION_TAG;

typedef struct {
  RPI_FW_BUFFER_HEAD        BufferHead;
  RPI_FW_TAG_HEAD           TagHead;
  RPI_FW_MODEL_REVISION_TAG TagBody;
  UINT32                    EndTag;
} RPI_FW_GET_REVISION_CMD;

typedef struct {
  UINT32 Width;
  UINT32 Height;
} RPI_FW_FB_SIZE_TAG;

typedef struct {
  RPI_FW_BUFFER_HEAD        BufferHead;
  RPI_FW_TAG_HEAD           TagHead;
  RPI_FW_FB_SIZE_TAG        TagBody;
  UINT32                    EndTag;
} RPI_FW_GET_FB_SIZE_CMD;

typedef struct {
  UINT32 Depth;
} RPI_FW_FB_DEPTH_TAG;

typedef struct {
  UINT32 Pitch;
} RPI_FW_FB_PITCH_TAG;

typedef struct {
  UINT32 AlignmentBase;
  UINT32 Size;
} RPI_FW_FB_ALLOC_TAG;

typedef struct {
  RPI_FW_BUFFER_HEAD        BufferHead;
  RPI_FW_TAG_HEAD           FreeFbTag;
  UINT32                    EndTag;
} RPI_FW_FREE_FB_CMD;

typedef struct {
  RPI_FW_BUFFER_HEAD        BufferHead;
  RPI_FW_TAG_HEAD           PhysSizeTag;
  RPI_FW_FB_SIZE_TAG        PhysSize;
  RPI_FW_TAG_HEAD           VirtSizeTag;
  RPI_FW_FB_SIZE_TAG        VirtSize;
  RPI_FW_TAG_HEAD           DepthTag;
  RPI_FW_FB_DEPTH_TAG       Depth;
  RPI_FW_TAG_HEAD           AllocFbTag;
  RPI_FW_FB_ALLOC_TAG       AllocFb;
  RPI_FW_TAG_HEAD           PitchTag;
  RPI_FW_FB_PITCH_TAG       Pitch;
  UINT32                    EndTag;
} RPI_FW_INIT_FB_CMD;

typedef struct {
  RPI_FW_BUFFER_HEAD        BufferHead;
  RPI_FW_TAG_HEAD           TagHead;
  UINT8                     CommandLine[0];
} RPI_FW_GET_COMMAND_LINE_CMD;

typedef struct {
  UINT32                    ClockId;
  UINT32                    ClockRate;
  UINT32                    SkipTurbo;
} RPI_FW_SET_CLOCK_RATE_TAG;

typedef struct {
  RPI_FW_BUFFER_HEAD        BufferHead;
  RPI_FW_TAG_HEAD           TagHead;
  RPI_FW_SET_CLOCK_RATE_TAG TagBody;
  UINT32                    EndTag;
} RPI_FW_SET_CLOCK_RATE_CMD;

typedef struct {
  UINT32                    ClockId;
  UINT32                    ClockRate;
} RPI_FW_CLOCK_RATE_TAG;

typedef struct {
  RPI_FW_BUFFER_HEAD        BufferHead;
  RPI_FW_TAG_HEAD           TagHead;
  RPI_FW_CLOCK_RATE_TAG     TagBody;
  UINT32                    EndTag;
} RPI_FW_GET_CLOCK_RATE_CMD;

typedef struct {
  UINT32                    ClockId;
  UINT32                    ClockState;
} RPI_FW_GET_CLOCK_STATE_TAG;

typedef struct {
  RPI_FW_BUFFER_HEAD         BufferHead;
  RPI_FW_TAG_HEAD            TagHead;
  RPI_FW_GET_CLOCK_STATE_TAG TagBody;
  UINT32                     EndTag;
} RPI_FW_SET_CLOCK_STATE_CMD;

typedef struct {
  UINT32 Pin;
  UINT32 State;
} RPI_FW_SET_GPIO_TAG;

typedef struct {
  RPI_FW_BUFFER_HEAD        BufferHead;
  RPI_FW_TAG_HEAD           TagHead;
  RPI_FW_SET_GPIO_TAG       TagBody;
  UINT32                    EndTag;
} RPI_FW_SET_GPIO_CMD;

typedef struct {
  UINT32                       DeviceAddress;
} RPI_FW_NOTIFY_XHCI_RESET_TAG;

typedef struct {
  RPI_FW_BUFFER_HEAD           BufferHead;
  RPI_FW_TAG_HEAD              TagHead;
  RPI_FW_NOTIFY_XHCI_RESET_TAG TagBody;
  UINT32                       EndTag;
} RPI_FW_NOTIFY_XHCI_RESET_CMD;

typedef struct {
  UINT32                       Gpio;
  UINT32                       Direction;
  UINT32                       Polarity;
  UINT32                       TermEn;
  UINT32                       TermPullUp;
} RPI_FW_GPIO_GET_CFG_TAG;

typedef struct {
  RPI_FW_BUFFER_HEAD           BufferHead;
  RPI_FW_TAG_HEAD              TagHead;
  RPI_FW_GPIO_GET_CFG_TAG      TagBody;
  UINT32                       EndTag;
} RPI_FW_NOTIFY_GPIO_GET_CFG_CMD;

typedef struct {
  UINT32                       Gpio;
  UINT32                       Direction;
  UINT32                       Polarity;
  UINT32                       TermEn;
  UINT32                       TermPullUp;
  UINT32                       State;
} RPI_FW_GPIO_SET_CFG_TAG;

typedef struct {
  RPI_FW_BUFFER_HEAD           BufferHead;
  RPI_FW_TAG_HEAD              TagHead;
  RPI_FW_GPIO_SET_CFG_TAG      TagBody;
  UINT32                       EndTag;
} RPI_FW_NOTIFY_GPIO_SET_CFG_CMD;
#pragma pack()

STATIC UINTN mMboxBaseAddress;
STATIC EFI_PHYSICAL_ADDRESS  mMboxPhysicalAddress;

STATIC VOID  *mDmaBuffer;
STATIC UINTN mDmaBufferSize;
STATIC VOID  *mDmaBufferMapping;
STATIC UINTN mDmaBufferBusAddress;
STATIC EFI_PHYSICAL_ADDRESS  mDmaBufferPhysicalAddress;

STATIC SPIN_LOCK mMailboxLock;

STATIC
VOID
SharedMailboxWrite (
  IN UINTN   Offset,
  IN UINT32  Value
  )
{
  volatile UINT32  *Address;

  Address = (volatile UINT32 *)((UINT8 *)mDmaBuffer + Offset);
  *Address = Value;

  if (EfiAtRuntime ()) {
    WriteBackDataCacheRange ((VOID *)Address, sizeof (*Address));
  }

  ArmDataSynchronizationBarrier ();
}

STATIC
UINT32
SharedMailboxRead (
  IN UINTN  Offset
  )
{
  volatile UINT32  *Address;

  Address = (volatile UINT32 *)((UINT8 *)mDmaBuffer + Offset);
  if (EfiAtRuntime ()) {
    InvalidateDataCacheRange ((VOID *)Address, sizeof (*Address));
  }

  ArmDataSynchronizationBarrier ();
  return *Address;
}

STATIC
BOOLEAN
AcquireMailboxLock (
  VOID
  )
{
  if (!AcquireSpinLockOrFail (&mMailboxLock)) {
    return FALSE;
  }

  //
  // AML and the runtime driver share the mailbox after ExitBootServices.
  // Use a non-blocking two-party Peterson lock: a runtime service must never
  // wait for AML because it may have preempted the AML interpreter itself.
  //
  SharedMailboxWrite (RPI_FIRMWARE_MAILBOX_FW_ACTIVE_OFFSET, 1);
  SharedMailboxWrite (
    RPI_FIRMWARE_MAILBOX_TURN_OFFSET,
    RPI_FIRMWARE_MAILBOX_OWNER_ACPI
    );

  if ((SharedMailboxRead (RPI_FIRMWARE_MAILBOX_ACPI_ACTIVE_OFFSET) != 0) &&
      (SharedMailboxRead (RPI_FIRMWARE_MAILBOX_TURN_OFFSET) ==
       RPI_FIRMWARE_MAILBOX_OWNER_ACPI))
  {
    SharedMailboxWrite (RPI_FIRMWARE_MAILBOX_FW_ACTIVE_OFFSET, 0);
    ReleaseSpinLock (&mMailboxLock);
    return FALSE;
  }

  return TRUE;
}

STATIC
VOID
ReleaseMailboxLock (
  VOID
  )
{
  SharedMailboxWrite (RPI_FIRMWARE_MAILBOX_FW_ACTIVE_OFFSET, 0);
  ReleaseSpinLock (&mMailboxLock);
}

STATIC
BOOLEAN
DrainMailbox (
  VOID
  )
{
  INTN    Tries;
  UINT32  Val;

  //
  // Get rid of stale response data in the mailbox
  //
  Tries = 0;
  do {
    Val = MmioRead32 (mMboxBaseAddress + BCM2836_MBOX_STATUS_OFFSET);
    if (Val & (1U << BCM2836_MBOX_STATUS_EMPTY)) {
      return TRUE;
    }
    ArmDataSynchronizationBarrier ();
    MmioRead32 (mMboxBaseAddress + BCM2836_MBOX_READ_OFFSET);
  } while (++Tries < RPI_MBOX_MAX_TRIES);

  return FALSE;
}

STATIC
BOOLEAN
MailboxWaitForStatusCleared (
  IN  UINTN   StatusMask
  )
{
  INTN    Tries;
  UINT32  Val;

  //
  // Get rid of stale response data in the mailbox
  //
  Tries = 0;
  do {
    Val = MmioRead32 (mMboxBaseAddress + BCM2836_MBOX_STATUS_OFFSET);
    if ((Val & StatusMask) == 0) {
      return TRUE;
    }
    ArmDataSynchronizationBarrier ();
  } while (++Tries < RPI_MBOX_MAX_TRIES);

  return FALSE;
}

STATIC
EFI_STATUS
MailboxTransaction (
  IN    UINTN   Length,
  IN    UINTN   Channel,
  OUT   UINT32  *Result
  )
{
  if ((Channel >= BCM2836_MBOX_NUM_CHANNELS) ||
      (Length > RPI_FIRMWARE_MAILBOX_COMMAND_SIZE))
  {
    return EFI_INVALID_PARAMETER;
  }

  //
  // Get rid of stale response data in the mailbox
  //
  if (!DrainMailbox ()) {
    DEBUG ((DEBUG_ERROR, "%a: timeout waiting for mailbox to drain\n",
      __func__));
    return EFI_TIMEOUT;
  }

  //
  // Wait for the 'output register full' bit to become clear
  //
  if (!MailboxWaitForStatusCleared (1U << BCM2836_MBOX_STATUS_FULL)) {
    DEBUG ((DEBUG_ERROR, "%a: timeout waiting for outbox to become empty\n",
      __func__));
    return EFI_TIMEOUT;
  }

  //
  // The DMA buffer is initially mapped as WC/Normal-NC, but it
  // somehow ends up being cached at runtime.
  //
  if (EfiAtRuntime ()) {
    WriteBackDataCacheRange (mDmaBuffer, Length);
  }

  ArmDataSynchronizationBarrier ();

  //
  // Start the mailbox transaction
  //
  MmioWrite32 (mMboxBaseAddress + BCM2836_MBOX_WRITE_OFFSET,
    (UINT32)((UINTN)mDmaBufferBusAddress | Channel));

  ArmDataSynchronizationBarrier ();

  //
  // Wait for the 'input register empty' bit to clear
  //
  if (!MailboxWaitForStatusCleared (1U << BCM2836_MBOX_STATUS_EMPTY)) {
    DEBUG ((DEBUG_ERROR, "%a: timeout waiting for inbox to become full\n",
      __func__));
    return EFI_TIMEOUT;
  }

  if (EfiAtRuntime ()) {
    InvalidateDataCacheRange (mDmaBuffer, Length);
  }

  //
  // Read back the result
  //
  ArmDataSynchronizationBarrier ();
  *Result = MmioRead32 (mMboxBaseAddress + BCM2836_MBOX_READ_OFFSET);
  ArmDataSynchronizationBarrier ();

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
RpiFirmwareSetPowerState (
  IN  UINT32    DeviceId,
  IN  BOOLEAN   PowerState,
  IN  BOOLEAN   Wait
  )
{
  RPI_FW_SET_POWER_STATE_CMD  *Cmd;
  EFI_STATUS                  Status;
  UINT32                      Result;

  if (!AcquireMailboxLock ()) {
    DEBUG ((DEBUG_ERROR, "%a: failed to acquire spinlock\n", __func__));
    return EFI_DEVICE_ERROR;
  }

  Cmd = mDmaBuffer;
  ZeroMem (Cmd, sizeof (*Cmd));

  Cmd->BufferHead.BufferSize  = sizeof (*Cmd);
  Cmd->BufferHead.Response    = 0;
  Cmd->TagHead.TagId          = RPI_MBOX_SET_POWER_STATE;
  Cmd->TagHead.TagSize        = sizeof (Cmd->TagBody);
  Cmd->TagHead.TagValueSize   = 0;
  Cmd->TagBody.DeviceId       = DeviceId;
  Cmd->TagBody.PowerState     = (PowerState ? RPI_MBOX_POWER_STATE_ENABLE : 0) |
                                (Wait ? RPI_MBOX_POWER_STATE_WAIT : 0);
  Cmd->EndTag                 = 0;

  Status = MailboxTransaction (Cmd->BufferHead.BufferSize, RPI_MBOX_VC_CHANNEL, &Result);


  if (EFI_ERROR (Status) ||
      Cmd->BufferHead.Response != RPI_MBOX_RESP_SUCCESS) {
    DEBUG ((DEBUG_ERROR,
      "%a: mailbox transaction error: Status == %r, Response == 0x%x\n",
      __func__, Status, Cmd->BufferHead.Response));
    Status = EFI_DEVICE_ERROR;
  }

  if (!EFI_ERROR (Status) &&
      PowerState ^ (Cmd->TagBody.PowerState & RPI_MBOX_POWER_STATE_ENABLE)) {
    DEBUG ((DEBUG_ERROR, "%a: failed to %sable power for device %d\n",
      __func__, PowerState ? "en" : "dis", DeviceId));
    Status = EFI_DEVICE_ERROR;
  }
  ReleaseMailboxLock ();

  return Status;
}

STATIC
EFI_STATUS
EFIAPI
RpiFirmwareGetArmMemory (
  OUT   UINT32 *Base,
  OUT   UINT32 *Size
  )
{
  RPI_FW_GET_ARM_MEMORY_CMD   *Cmd;
  EFI_STATUS                  Status;
  UINT32                      Result;

  if (!AcquireMailboxLock ()) {
    DEBUG ((DEBUG_ERROR, "%a: failed to acquire spinlock\n", __func__));
    return EFI_DEVICE_ERROR;
  }

  Cmd = mDmaBuffer;
  ZeroMem (Cmd, sizeof (*Cmd));

  Cmd->BufferHead.BufferSize  = sizeof (*Cmd);
  Cmd->BufferHead.Response    = 0;
  Cmd->TagHead.TagId          = RPI_MBOX_GET_ARM_MEMSIZE;
  Cmd->TagHead.TagSize        = sizeof (Cmd->TagBody);
  Cmd->TagHead.TagValueSize   = 0;
  Cmd->EndTag                 = 0;

  Status = MailboxTransaction (Cmd->BufferHead.BufferSize, RPI_MBOX_VC_CHANNEL, &Result);


  if (EFI_ERROR (Status) ||
      Cmd->BufferHead.Response != RPI_MBOX_RESP_SUCCESS) {
    DEBUG ((DEBUG_ERROR,
      "%a: mailbox transaction error: Status == %r, Response == 0x%x\n",
      __func__, Status, Cmd->BufferHead.Response));
    ReleaseMailboxLock ();
    return EFI_DEVICE_ERROR;
  }

  *Base = Cmd->TagBody.Base;
  *Size = Cmd->TagBody.Size;
  ReleaseMailboxLock ();

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
RpiFirmwareGetMacAddress (
  OUT   UINT8   MacAddress[6]
  )
{
  RPI_FW_GET_MAC_ADDR_CMD     *Cmd;
  EFI_STATUS                  Status;
  UINT32                      Result;

  if (!AcquireMailboxLock ()) {
    DEBUG ((DEBUG_ERROR, "%a: failed to acquire spinlock\n", __func__));
    return EFI_DEVICE_ERROR;
  }

  Cmd = mDmaBuffer;
  ZeroMem (Cmd, sizeof (*Cmd));

  Cmd->BufferHead.BufferSize  = sizeof (*Cmd);
  Cmd->BufferHead.Response    = 0;
  Cmd->TagHead.TagId          = RPI_MBOX_GET_MAC_ADDRESS;
  Cmd->TagHead.TagSize        = sizeof (Cmd->TagBody);
  Cmd->TagHead.TagValueSize   = 0;
  Cmd->EndTag                 = 0;

  Status = MailboxTransaction (Cmd->BufferHead.BufferSize, RPI_MBOX_VC_CHANNEL, &Result);

  if (EFI_ERROR (Status) ||
      Cmd->BufferHead.Response != RPI_MBOX_RESP_SUCCESS) {
    DEBUG ((DEBUG_ERROR,
      "%a: mailbox transaction error: Status == %r, Response == 0x%x\n",
      __func__, Status, Cmd->BufferHead.Response));
    ReleaseMailboxLock ();
    return EFI_DEVICE_ERROR;
  }

  CopyMem (MacAddress, Cmd->TagBody.MacAddress, sizeof (Cmd->TagBody.MacAddress));
  ReleaseMailboxLock ();

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
RpiFirmwareGetSerial (
  OUT   UINT64 *Serial
  )
{
  RPI_FW_GET_SERIAL_CMD       *Cmd;
  EFI_STATUS                  Status;
  UINT32                      Result;

  if (!AcquireMailboxLock ()) {
    DEBUG ((DEBUG_ERROR, "%a: failed to acquire spinlock\n", __func__));
    return EFI_DEVICE_ERROR;
  }

  Cmd = mDmaBuffer;
  ZeroMem (Cmd, sizeof (*Cmd));

  Cmd->BufferHead.BufferSize  = sizeof (*Cmd);
  Cmd->BufferHead.Response    = 0;
  Cmd->TagHead.TagId          = RPI_MBOX_GET_BOARD_SERIAL;
  Cmd->TagHead.TagSize        = sizeof (Cmd->TagBody);
  Cmd->TagHead.TagValueSize   = 0;
  Cmd->EndTag                 = 0;

  Status = MailboxTransaction (Cmd->BufferHead.BufferSize, RPI_MBOX_VC_CHANNEL, &Result);

  if (EFI_ERROR (Status) ||
      Cmd->BufferHead.Response != RPI_MBOX_RESP_SUCCESS) {
    DEBUG ((DEBUG_ERROR,
      "%a: mailbox transaction error: Status == %r, Response == 0x%x\n",
      __func__, Status, Cmd->BufferHead.Response));
    ReleaseMailboxLock ();
    return EFI_DEVICE_ERROR;
  }

  *Serial = Cmd->TagBody.Serial;
  ReleaseMailboxLock ();
  // Some platforms return 0 or 0x0000000010000000 for serial.
  // For those, try to use the MAC address.
  if ((*Serial == 0) || ((*Serial & 0xFFFFFFFF0FFFFFFFULL) == 0)) {
    Status = RpiFirmwareGetMacAddress ((UINT8*) Serial);
    // Convert to a more user-friendly value
    *Serial = SwapBytes64 (*Serial << 16);
  }

  return Status;
}

STATIC
EFI_STATUS
EFIAPI
RpiFirmwareGetModel (
  OUT   UINT32 *Model
  )
{
  RPI_FW_GET_MODEL_CMD       *Cmd;
  EFI_STATUS                  Status;
  UINT32                      Result;

  if (!AcquireMailboxLock ()) {
    DEBUG ((DEBUG_ERROR, "%a: failed to acquire spinlock\n", __func__));
    return EFI_DEVICE_ERROR;
  }

  Cmd = mDmaBuffer;
  ZeroMem (Cmd, sizeof (*Cmd));

  Cmd->BufferHead.BufferSize  = sizeof (*Cmd);
  Cmd->BufferHead.Response    = 0;
  Cmd->TagHead.TagId          = RPI_MBOX_GET_BOARD_MODEL;
  Cmd->TagHead.TagSize        = sizeof (Cmd->TagBody);
  Cmd->TagHead.TagValueSize   = 0;
  Cmd->EndTag                 = 0;

  Status = MailboxTransaction (Cmd->BufferHead.BufferSize, RPI_MBOX_VC_CHANNEL, &Result);

  if (EFI_ERROR (Status) ||
      Cmd->BufferHead.Response != RPI_MBOX_RESP_SUCCESS) {
    DEBUG ((DEBUG_ERROR,
      "%a: mailbox transaction error: Status == %r, Response == 0x%x\n",
      __func__, Status, Cmd->BufferHead.Response));
    ReleaseMailboxLock ();
    return EFI_DEVICE_ERROR;
  }

  *Model = Cmd->TagBody.Model;
  ReleaseMailboxLock ();

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
RpiFirmwareGetModelRevision (
  OUT   UINT32 *Revision
  )
{
  RPI_FW_GET_REVISION_CMD       *Cmd;
  EFI_STATUS                    Status;
  UINT32                        Result;

  if (!AcquireMailboxLock ()) {
    DEBUG ((DEBUG_ERROR, "%a: failed to acquire spinlock\n", __func__));
    return EFI_DEVICE_ERROR;
  }

  Cmd = mDmaBuffer;
  ZeroMem (Cmd, sizeof (*Cmd));

  Cmd->BufferHead.BufferSize  = sizeof (*Cmd);
  Cmd->BufferHead.Response    = 0;
  Cmd->TagHead.TagId          = RPI_MBOX_GET_BOARD_REVISION;
  Cmd->TagHead.TagSize        = sizeof (Cmd->TagBody);
  Cmd->TagHead.TagValueSize   = 0;
  Cmd->EndTag                 = 0;

  Status = MailboxTransaction (Cmd->BufferHead.BufferSize, RPI_MBOX_VC_CHANNEL, &Result);

  if (EFI_ERROR (Status) ||
      Cmd->BufferHead.Response != RPI_MBOX_RESP_SUCCESS) {
    DEBUG ((DEBUG_ERROR,
      "%a: mailbox transaction error: Status == %r, Response == 0x%x\n",
      __func__, Status, Cmd->BufferHead.Response));
    ReleaseMailboxLock ();
    return EFI_DEVICE_ERROR;
  }

  *Revision = Cmd->TagBody.Revision;
  ReleaseMailboxLock ();

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
RpiFirmwareGetFirmwareRevision (
  OUT   UINT32 *Revision
  )
{
  RPI_FW_GET_REVISION_CMD       *Cmd;
  EFI_STATUS                    Status;
  UINT32                        Result;

  if (!AcquireMailboxLock ()) {
    DEBUG ((DEBUG_ERROR, "%a: failed to acquire spinlock\n", __func__));
    return EFI_DEVICE_ERROR;
  }

  Cmd = mDmaBuffer;
  ZeroMem (Cmd, sizeof (*Cmd));

  Cmd->BufferHead.BufferSize  = sizeof (*Cmd);
  Cmd->BufferHead.Response    = 0;
  Cmd->TagHead.TagId          = RPI_MBOX_GET_REVISION;
  Cmd->TagHead.TagSize        = sizeof (Cmd->TagBody);
  Cmd->TagHead.TagValueSize   = 0;
  Cmd->EndTag                 = 0;

  Status = MailboxTransaction (Cmd->BufferHead.BufferSize, RPI_MBOX_VC_CHANNEL, &Result);

  if (EFI_ERROR (Status) ||
      Cmd->BufferHead.Response != RPI_MBOX_RESP_SUCCESS) {
    DEBUG ((DEBUG_ERROR,
      "%a: mailbox transaction error: Status == %r, Response == 0x%x\n",
      __func__, Status, Cmd->BufferHead.Response));
    ReleaseMailboxLock ();
    return EFI_DEVICE_ERROR;
  }

  *Revision = Cmd->TagBody.Revision;
  ReleaseMailboxLock ();

  return EFI_SUCCESS;
}

STATIC
CHAR8*
EFIAPI
RpiFirmwareGetModelName (
  IN INTN ModelId
  )
{
  UINT32  Revision;

  // If a negative ModelId is passed, detect it.
  if ((ModelId < 0) && (RpiFirmwareGetModelRevision (&Revision) == EFI_SUCCESS)) {
    ModelId = (Revision >> 4) & 0xFF;
  }

  switch (ModelId) {
  // www.raspberrypi.org/documentation/hardware/raspberrypi/revision-codes/README.md
  case 0x00:
    return "Raspberry Pi Model A";
  case 0x01:
    return "Raspberry Pi Model B";
  case 0x02:
    return "Raspberry Pi Model A+";
  case 0x03:
    return "Raspberry Pi Model B+";
  case 0x04:
    return "Raspberry Pi 2 Model B";
  case 0x06:
    return "Raspberry Pi Compute Module 1";
  case 0x08:
    return "Raspberry Pi 3 Model B";
  case 0x09:
    return "Raspberry Pi Zero";
  case 0x0A:
    return "Raspberry Pi Compute Module 3";
  case 0x0C:
    return "Raspberry Pi Zero W";
  case 0x0D:
    return "Raspberry Pi 3 Model B+";
  case 0x0E:
    return "Raspberry Pi 3 Model A+";
  case 0x10:
    return "Raspberry Pi Compute Module 3+";
  case 0x11:
    return "Raspberry Pi 4 Model B";
  case 0x12:
    return "Raspberry Pi Zero 2 W";
  case 0x13:
    return "Raspberry Pi 400";
  case 0x14:
    return "Raspberry Pi Compute Module 4";
  default:
    return "Unknown Raspberry Pi Model";
  }
}

STATIC
EFI_STATUS
EFIAPI
RPiFirmwareGetModelInstalledMB (
  OUT   UINT32 *InstalledMB
  )
{
  EFI_STATUS Status;
  UINT32     Revision;

  Status = RpiFirmwareGetModelRevision(&Revision);
  if (EFI_ERROR(Status)) {
    DEBUG ((DEBUG_ERROR, "%a: Could not get the board revision: Status == %r\n",
      __func__, Status));
    return EFI_DEVICE_ERROR;
  }

  //
  // www.raspberrypi.org/documentation/hardware/raspberrypi/revision-codes/README.md
  // Bits [20-22] indicate the amount of memory starting with 256MB (000b)
  // and doubling in size for each value (001b = 512 MB, 010b = 1GB, etc.)
  //
  *InstalledMB = 256 << ((Revision >> 20) & 0x07);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
RPiFirmwareGetModelFamily (
  OUT   UINT32 *ModelFamily
  )
{
  EFI_STATUS                  Status;
  UINT32                      Revision;
  UINT32                      ModelId;

  Status = RpiFirmwareGetModelRevision(&Revision);
  if (EFI_ERROR(Status)) {
    DEBUG ((DEBUG_ERROR,
      "%a: Could not get the board revision: Status == %r\n",
      __func__, Status));
    return EFI_DEVICE_ERROR;
  } else {
    ModelId = (Revision >> 4) & 0xFF;
  }

  switch (ModelId) {
  // www.raspberrypi.org/documentation/hardware/raspberrypi/revision-codes/README.md
  case 0x00:          // Raspberry Pi Model A
  case 0x01:          // Raspberry Pi Model B
  case 0x02:          // Raspberry Pi Model A+
  case 0x03:          // Raspberry Pi Model B+
  case 0x06:          // Raspberry Pi Compute Module 1
  case 0x09:          // Raspberry Pi Zero
  case 0x0C:          // Raspberry Pi Zero W
      *ModelFamily = 1;
      break;
  case 0x04:          // Raspberry Pi 2 Model B
      *ModelFamily = 2;
      break;
  case 0x08:          // Raspberry Pi 3 Model B
  case 0x0A:          // Raspberry Pi Compute Module 3
  case 0x0D:          // Raspberry Pi 3 Model B+
  case 0x0E:          // Raspberry Pi 3 Model A+
  case 0x10:          // Raspberry Pi Compute Module 3+
  case 0x12:          // Raspberry Pi Zero 2 W
      *ModelFamily = 3;
      break;
  case 0x11:          // Raspberry Pi 4 Model B
  case 0x13:          // Raspberry Pi 400
  case 0x14:          // Raspberry Pi Computer Module 4
      *ModelFamily = 4;
      break;
  default:
      *ModelFamily = 0;
      break;
  }

  if (*ModelFamily == 0) {
    DEBUG ((DEBUG_ERROR,
      "%a: Unknown Raspberry Pi model family : ModelId == 0x%x\n",
      __func__, ModelId));
    return EFI_UNSUPPORTED;
    }

  return EFI_SUCCESS;
}

STATIC
CHAR8*
EFIAPI
RpiFirmwareGetManufacturerName (
  IN INTN ManufacturerId
  )
{
  UINT32  Revision;

  // If a negative ModelId is passed, detect it.
  if ((ManufacturerId < 0) && (RpiFirmwareGetModelRevision (&Revision) == EFI_SUCCESS)) {
    ManufacturerId = (Revision >> 16) & 0x0F;
  }

  switch (ManufacturerId) {
  // www.raspberrypi.org/documentation/hardware/raspberrypi/revision-codes/README.md
  case 0x00:
    return "Sony UK";
  case 0x01:
    return "Egoman";
  case 0x02:
  case 0x04:
    return "Embest";
  case 0x03:
    return "Sony Japan";
  case 0x05:
    return "Stadium";
  default:
    return "Unknown Manufacturer";
  }
}

STATIC
CHAR8*
EFIAPI
RpiFirmwareGetCpuName (
  IN INTN CpuId
  )
{
  UINT32  Revision;

  // If a negative CpuId is passed, detect it.
  if ((CpuId < 0) && (RpiFirmwareGetModelRevision (&Revision) == EFI_SUCCESS)) {
    CpuId = (Revision >> 12) & 0x0F;
  }

  switch (CpuId) {
  // www.raspberrypi.org/documentation/hardware/raspberrypi/revision-codes/README.md
  case 0x00:
    return "BCM2835 (ARM11)";
  case 0x01:
    return "BCM2836 (ARM Cortex-A7)";
  case 0x02:
    return "BCM2837 (ARM Cortex-A53)";
  case 0x03:
    return "BCM2711 (ARM Cortex-A72)";
  default:
    return "Unknown CPU Model";
  }
}

STATIC
EFI_STATUS
EFIAPI
RpiFirmwareGetFbSize (
  OUT   UINT32 *Width,
  OUT   UINT32 *Height
  )
{
  RPI_FW_GET_FB_SIZE_CMD     *Cmd;
  EFI_STATUS                  Status;
  UINT32                      Result;

  if (!AcquireMailboxLock ()) {
    DEBUG ((DEBUG_ERROR, "%a: failed to acquire spinlock\n", __func__));
    return EFI_DEVICE_ERROR;
  }

  Cmd = mDmaBuffer;
  ZeroMem (Cmd, sizeof (*Cmd));

  Cmd->BufferHead.BufferSize  = sizeof (*Cmd);
  Cmd->BufferHead.Response    = 0;
  Cmd->TagHead.TagId          = RPI_MBOX_GET_FB_GEOMETRY;
  Cmd->TagHead.TagSize        = sizeof (Cmd->TagBody);
  Cmd->TagHead.TagValueSize   = 0;
  Cmd->EndTag                 = 0;

  Status = MailboxTransaction (Cmd->BufferHead.BufferSize, RPI_MBOX_VC_CHANNEL, &Result);

  if (EFI_ERROR (Status) ||
      Cmd->BufferHead.Response != RPI_MBOX_RESP_SUCCESS) {
    DEBUG ((DEBUG_ERROR,
      "%a: mailbox  transaction error: Status == %r, Response == 0x%x\n",
      __func__, Status, Cmd->BufferHead.Response));
    ReleaseMailboxLock ();
    return EFI_DEVICE_ERROR;
  }

  *Width = Cmd->TagBody.Width;
  *Height = Cmd->TagBody.Height;
  ReleaseMailboxLock ();

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
RpiFirmwareFreeFb (VOID)
{
  RPI_FW_FREE_FB_CMD *Cmd;
  EFI_STATUS         Status;
  UINT32             Result;

  if (!AcquireMailboxLock ()) {
    DEBUG ((DEBUG_ERROR, "%a: failed to acquire spinlock\n", __func__));
    return EFI_DEVICE_ERROR;
  }

  Cmd = mDmaBuffer;
  ZeroMem (Cmd, sizeof (*Cmd));

  Cmd->BufferHead.BufferSize   = sizeof (*Cmd);
  Cmd->BufferHead.Response     = 0;

  Cmd->FreeFbTag.TagId         = RPI_MBOX_FREE_FB;
  Cmd->FreeFbTag.TagSize       = 0;
  Cmd->FreeFbTag.TagValueSize  = 0;
  Cmd->EndTag                  = 0;

  Status = MailboxTransaction (Cmd->BufferHead.BufferSize, RPI_MBOX_VC_CHANNEL, &Result);

  if (EFI_ERROR (Status) ||
      Cmd->BufferHead.Response != RPI_MBOX_RESP_SUCCESS) {
    DEBUG ((DEBUG_ERROR,
      "%a: mailbox transaction error: Status == %r, Response == 0x%x\n",
      __func__, Status, Cmd->BufferHead.Response));
    ReleaseMailboxLock ();
    return EFI_DEVICE_ERROR;
  }

  ReleaseMailboxLock ();

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
RpiFirmwareAllocFb (
  IN  UINT32 Width,
  IN  UINT32 Height,
  IN  UINT32 Depth,
  OUT EFI_PHYSICAL_ADDRESS *FbBase,
  OUT UINTN *FbSize,
  OUT UINTN *Pitch
  )
{
  RPI_FW_INIT_FB_CMD *Cmd;
  EFI_STATUS         Status;
  UINT32             Result;

  ASSERT (FbSize != NULL);
  ASSERT (FbBase != NULL);

  if (!AcquireMailboxLock ()) {
    DEBUG ((DEBUG_ERROR, "%a: failed to acquire spinlock\n", __func__));
    return EFI_DEVICE_ERROR;
  }

  Cmd = mDmaBuffer;
  ZeroMem (Cmd, sizeof (*Cmd));

  Cmd->BufferHead.BufferSize  = sizeof (*Cmd);
  Cmd->BufferHead.Response    = 0;

  Cmd->PhysSizeTag.TagId      = RPI_MBOX_SET_FB_PGEOM;
  Cmd->PhysSizeTag.TagSize    = sizeof (Cmd->PhysSize);
  Cmd->PhysSize.Width         = Width;
  Cmd->PhysSize.Height        = Height;
  Cmd->VirtSizeTag.TagId      = RPI_MBOX_SET_FB_VGEOM;
  Cmd->VirtSizeTag.TagSize    = sizeof (Cmd->VirtSize);
  Cmd->VirtSize.Width         = Width;
  Cmd->VirtSize.Height        = Height;
  Cmd->DepthTag.TagId         = RPI_MBOX_SET_FB_DEPTH;
  Cmd->DepthTag.TagSize       = sizeof (Cmd->Depth);
  Cmd->Depth.Depth            = Depth;
  Cmd->AllocFbTag.TagId       = RPI_MBOX_ALLOC_FB;
  Cmd->AllocFbTag.TagSize     = sizeof (Cmd->AllocFb);
  Cmd->AllocFb.AlignmentBase  = 32;
  Cmd->PitchTag.TagId         = RPI_MBOX_GET_FB_LINELENGTH;
  Cmd->PitchTag.TagSize       = sizeof (Cmd->Pitch);
  Cmd->EndTag                 = 0;

  Status = MailboxTransaction (Cmd->BufferHead.BufferSize, RPI_MBOX_VC_CHANNEL, &Result);

  if (EFI_ERROR (Status) ||
      Cmd->BufferHead.Response != RPI_MBOX_RESP_SUCCESS) {
    DEBUG ((DEBUG_ERROR,
      "%a: mailbox transaction error: Status == %r, Response == 0x%x\n",
      __func__, Status, Cmd->BufferHead.Response));
    ReleaseMailboxLock ();
    return EFI_DEVICE_ERROR;
  }

  *Pitch = Cmd->Pitch.Pitch;
  *FbBase = Cmd->AllocFb.AlignmentBase & ~PcdGet64 (PcdDmaDeviceOffset);
  *FbSize = Cmd->AllocFb.Size;
  ReleaseMailboxLock ();

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
RpiFirmwareGetCommmandLine (
  IN  UINTN               BufferSize,
  OUT CHAR8               CommandLine[]
  )
{
  RPI_FW_GET_COMMAND_LINE_CMD  *Cmd;
  EFI_STATUS                    Status;
  UINT32                        Result;

  if ((BufferSize % sizeof (UINT32)) != 0) {
    DEBUG ((DEBUG_ERROR, "%a: BufferSize must be a multiple of 4\n",
      __func__));
    return EFI_INVALID_PARAMETER;
  }

  if (sizeof (*Cmd) + BufferSize + sizeof (UINT32) >
      RPI_FIRMWARE_MAILBOX_COMMAND_SIZE)
  {
    DEBUG ((DEBUG_ERROR, "%a: BufferSize exceeds size of DMA buffer\n",
      __func__));
    return EFI_OUT_OF_RESOURCES;
  }

  if (!AcquireMailboxLock ()) {
    DEBUG ((DEBUG_ERROR, "%a: failed to acquire spinlock\n", __func__));
    return EFI_DEVICE_ERROR;
  }

  Cmd = mDmaBuffer;
  ZeroMem (Cmd, sizeof (*Cmd) + BufferSize + sizeof (UINT32));

  Cmd->BufferHead.BufferSize  = sizeof (*Cmd) + BufferSize + sizeof (UINT32);
  Cmd->BufferHead.Response    = 0;
  Cmd->TagHead.TagId          = RPI_MBOX_GET_COMMAND_LINE;
  Cmd->TagHead.TagSize        = BufferSize;
  Cmd->TagHead.TagValueSize   = 0;

  Status = MailboxTransaction (Cmd->BufferHead.BufferSize, RPI_MBOX_VC_CHANNEL, &Result);

  if (EFI_ERROR (Status) ||
      Cmd->BufferHead.Response != RPI_MBOX_RESP_SUCCESS) {
    DEBUG ((DEBUG_ERROR,
      "%a: mailbox transaction error: Status == %r, Response == 0x%x\n",
      __func__, Status, Cmd->BufferHead.Response));
    ReleaseMailboxLock ();
    return EFI_DEVICE_ERROR;
  }

  Cmd->TagHead.TagValueSize &= ~RPI_MBOX_VALUE_SIZE_RESPONSE_MASK;
  if (Cmd->TagHead.TagValueSize >= BufferSize &&
      Cmd->CommandLine[Cmd->TagHead.TagValueSize - 1] != '\0') {
    DEBUG ((DEBUG_ERROR, "%a: insufficient buffer size\n", __func__));
    ReleaseMailboxLock ();
    return EFI_OUT_OF_RESOURCES;
  }

  CopyMem (CommandLine, Cmd->CommandLine, Cmd->TagHead.TagValueSize);

  if (Cmd->TagHead.TagValueSize == 0 ||
      CommandLine[Cmd->TagHead.TagValueSize - 1] != '\0') {
    //
    // Add a NUL terminator if required.
    //
    CommandLine[Cmd->TagHead.TagValueSize] = '\0';
  }

  ReleaseMailboxLock ();
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
RpiFirmwareSetClockRate (
  IN  UINT32  ClockId,
  IN  UINT32  ClockRate,
  IN  BOOLEAN SkipTurbo
  )
{
  RPI_FW_SET_CLOCK_RATE_CMD   *Cmd;
  EFI_STATUS                  Status;
  UINT32                      Result;

  if (!AcquireMailboxLock ()) {
    DEBUG ((DEBUG_ERROR, "%a: failed to acquire spinlock\n", __func__));
    return EFI_DEVICE_ERROR;
  }

  Cmd = mDmaBuffer;
  ZeroMem (Cmd, sizeof (*Cmd));

  Cmd->BufferHead.BufferSize  = sizeof (*Cmd);
  Cmd->BufferHead.Response    = 0;
  Cmd->TagHead.TagId          = RPI_MBOX_SET_CLOCK_RATE;
  Cmd->TagHead.TagSize        = sizeof (Cmd->TagBody);
  Cmd->TagHead.TagValueSize   = 0;
  Cmd->TagBody.ClockId        = ClockId;
  Cmd->TagBody.ClockRate      = ClockRate;
  Cmd->TagBody.SkipTurbo      = SkipTurbo ? 1 : 0;
  Cmd->EndTag                 = 0;

  DEBUG ((DEBUG_INFO, "%a: Request clock rate %X = %d\n", __func__, ClockId, ClockRate));
  Status = MailboxTransaction (Cmd->BufferHead.BufferSize, RPI_MBOX_VC_CHANNEL, &Result);

  if (EFI_ERROR (Status) ||
      Cmd->BufferHead.Response != RPI_MBOX_RESP_SUCCESS) {
    DEBUG ((DEBUG_ERROR,
      "%a: mailbox transaction error: Status == %r, Response == 0x%x\n",
      __func__, Status, Cmd->BufferHead.Response));
    ReleaseMailboxLock ();
    return EFI_DEVICE_ERROR;
  }

  ReleaseMailboxLock ();

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
RpiFirmwareGetClockRate (
  IN  UINT32 ClockId,
  IN  UINT32 ClockKind,
  OUT UINT32 *ClockRate
  )
{
  RPI_FW_GET_CLOCK_RATE_CMD   *Cmd;
  EFI_STATUS                  Status;
  UINT32                      Result;

  if (!AcquireMailboxLock ()) {
    DEBUG ((DEBUG_ERROR, "%a: failed to acquire spinlock\n", __func__));
    return EFI_DEVICE_ERROR;
  }

  Cmd = mDmaBuffer;
  ZeroMem (Cmd, sizeof (*Cmd));

  Cmd->BufferHead.BufferSize  = sizeof (*Cmd);
  Cmd->BufferHead.Response    = 0;
  Cmd->TagHead.TagId          = ClockKind;
  Cmd->TagHead.TagSize        = sizeof (Cmd->TagBody);
  Cmd->TagHead.TagValueSize   = 0;
  Cmd->TagBody.ClockId        = ClockId;
  Cmd->EndTag                 = 0;

  Status = MailboxTransaction (Cmd->BufferHead.BufferSize, RPI_MBOX_VC_CHANNEL, &Result);

  if (EFI_ERROR (Status) ||
      Cmd->BufferHead.Response != RPI_MBOX_RESP_SUCCESS) {
    DEBUG ((DEBUG_ERROR,
      "%a: mailbox transaction error: Status == %r, Response == 0x%x\n",
      __func__, Status, Cmd->BufferHead.Response));
    ReleaseMailboxLock ();
    return EFI_DEVICE_ERROR;
  }

  *ClockRate = Cmd->TagBody.ClockRate;
  ReleaseMailboxLock ();

  DEBUG ((DEBUG_INFO, "%a: Get Clock Rate return: ClockRate=%d ClockId=%X\n", __func__, *ClockRate, ClockId));

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
RpiFirmwareGetCurrentClockState (
  IN  UINT32    ClockId,
  OUT UINT32    *ClockState
  )
{
  return RpiFirmwareGetClockRate (ClockId, RPI_MBOX_GET_CLOCK_STATE, ClockState);
}

STATIC
EFI_STATUS
EFIAPI
RpiFirmwareGetCurrentClockRate (
  IN  UINT32    ClockId,
  OUT UINT32    *ClockRate
  )
{
  return RpiFirmwareGetClockRate (ClockId, RPI_MBOX_GET_CLOCK_RATE, ClockRate);
}

STATIC
EFI_STATUS
EFIAPI
RpiFirmwareGetMaxClockRate (
  IN  UINT32    ClockId,
  OUT UINT32    *ClockRate
  )
{
  return RpiFirmwareGetClockRate (ClockId, RPI_MBOX_GET_MAX_CLOCK_RATE, ClockRate);
}

STATIC
EFI_STATUS
EFIAPI
RpiFirmwareGetMinClockRate (
  IN  UINT32    ClockId,
  OUT UINT32    *ClockRate
  )
{
  return RpiFirmwareGetClockRate (ClockId, RPI_MBOX_GET_MIN_CLOCK_RATE, ClockRate);
}

STATIC
EFI_STATUS
RpiFirmwareSetClockState (
  IN  UINT32 ClockId,
  IN  UINT32 ClockState
  )
{
  RPI_FW_SET_CLOCK_STATE_CMD  *Cmd;
  EFI_STATUS                  Status;
  UINT32                      Result;

  if (!AcquireMailboxLock ()) {
    DEBUG ((DEBUG_ERROR, "%a: failed to acquire spinlock\n", __func__));
    return EFI_DEVICE_ERROR;
  }

  Cmd = mDmaBuffer;
  ZeroMem (Cmd, sizeof (*Cmd));

  Cmd->BufferHead.BufferSize  = sizeof (*Cmd);
  Cmd->BufferHead.Response    = 0;
  Cmd->TagHead.TagId          = RPI_MBOX_SET_CLOCK_STATE;
  Cmd->TagHead.TagSize        = sizeof (Cmd->TagBody);
  Cmd->TagHead.TagValueSize   = 0;
  Cmd->TagBody.ClockId        = ClockId;
  Cmd->TagBody.ClockState     = ClockState;
  Cmd->EndTag                 = 0;

  Status = MailboxTransaction (Cmd->BufferHead.BufferSize, RPI_MBOX_VC_CHANNEL, &Result);

  if (EFI_ERROR (Status) ||
      Cmd->BufferHead.Response != RPI_MBOX_RESP_SUCCESS) {
    DEBUG ((DEBUG_ERROR,
      "%a: mailbox transaction error: Status == %r, Response == 0x%x\n",
      __func__, Status, Cmd->BufferHead.Response));
    ReleaseMailboxLock ();
    return EFI_DEVICE_ERROR;
  }

  ReleaseMailboxLock ();

  return EFI_SUCCESS;
}

STATIC
VOID
EFIAPI
RpiFirmwareSetGpio (
  IN  UINT32  Gpio,
  IN  BOOLEAN State
  )
{
  RPI_FW_SET_GPIO_CMD *Cmd;
  EFI_STATUS          Status;
  UINT32              Result;

  if (!AcquireMailboxLock ()) {
    DEBUG ((DEBUG_ERROR, "%a: failed to acquire spinlock\n", __func__));
    return;
  }

  Cmd = mDmaBuffer;
  ZeroMem (Cmd, sizeof (*Cmd));

  Cmd->BufferHead.BufferSize  = sizeof (*Cmd);
  Cmd->BufferHead.Response    = 0;
  Cmd->TagHead.TagId          = RPI_MBOX_SET_GPIO;
  Cmd->TagHead.TagSize        = sizeof (Cmd->TagBody);
  /*
   * There's also a 128 pin offset.
   */
  Cmd->TagBody.Pin = 128 + Gpio;
  Cmd->TagBody.State = State;
  Cmd->TagHead.TagValueSize   = 0;
  Cmd->EndTag                 = 0;

  Status = MailboxTransaction (Cmd->BufferHead.BufferSize, RPI_MBOX_VC_CHANNEL, &Result);

  if (EFI_ERROR (Status) ||
      Cmd->BufferHead.Response != RPI_MBOX_RESP_SUCCESS) {
    DEBUG ((DEBUG_ERROR,
      "%a: mailbox  transaction error: Status == %r, Response == 0x%x\n",
      __func__, Status, Cmd->BufferHead.Response));
  }
  ReleaseMailboxLock ();
}

STATIC
VOID
EFIAPI
RpiFirmwareSetLed (
  IN  BOOLEAN On
  )
{
  RpiFirmwareSetGpio (RPI_EXP_GPIO_LED, On);
}

STATIC
EFI_STATUS
EFIAPI
RpiFirmwareNotifyXhciReset (
  IN UINTN BusNumber,
  IN UINTN DeviceNumber,
  IN UINTN FunctionNumber
  )
{
  RPI_FW_NOTIFY_XHCI_RESET_CMD *Cmd;
  EFI_STATUS                   Status;
  UINT32                       Result;

  if (!AcquireMailboxLock ()) {
    DEBUG ((DEBUG_ERROR, "%a: failed to acquire spinlock\n", __func__));
    return EFI_DEVICE_ERROR;
  }

  Cmd = mDmaBuffer;
  ZeroMem (Cmd, sizeof (*Cmd));

  Cmd->BufferHead.BufferSize  = sizeof (*Cmd);
  Cmd->BufferHead.Response    = 0;
  Cmd->TagHead.TagId          = RPI_MBOX_NOTIFY_XHCI_RESET;
  Cmd->TagHead.TagSize        = sizeof (Cmd->TagBody);
  Cmd->TagHead.TagValueSize   = 0;
  Cmd->TagBody.DeviceAddress  = BusNumber << 20 | DeviceNumber << 15 | FunctionNumber << 12;
  Cmd->EndTag                 = 0;

  Status = MailboxTransaction (Cmd->BufferHead.BufferSize, RPI_MBOX_VC_CHANNEL, &Result);

  if (EFI_ERROR (Status) ||
      Cmd->BufferHead.Response != RPI_MBOX_RESP_SUCCESS) {
    DEBUG ((DEBUG_ERROR,
      "%a: mailbox  transaction error: Status == %r, Response == 0x%x\n",
      __func__, Status, Cmd->BufferHead.Response));
  }

  ReleaseMailboxLock ();

  return Status;
}

STATIC
EFI_STATUS
EFIAPI
RpiFirmwareNotifyGpioGetCfg (
  IN UINTN  Gpio,
  IN UINT32 *Polarity
  )
{
  RPI_FW_NOTIFY_GPIO_GET_CFG_CMD *Cmd;
  EFI_STATUS                   Status;
  UINT32                       Result;

  if (!AcquireMailboxLock ()) {
    DEBUG ((DEBUG_ERROR, "%a: failed to acquire spinlock\n", __func__));
    return EFI_DEVICE_ERROR;
  }

  Cmd = mDmaBuffer;
  ZeroMem (Cmd, sizeof (*Cmd));

  Cmd->BufferHead.BufferSize  = sizeof (*Cmd);
  Cmd->BufferHead.Response    = 0;
  Cmd->TagHead.TagId          = RPI_MBOX_GET_GPIO_CONFIG;
  Cmd->TagHead.TagSize        = sizeof (Cmd->TagBody);
  Cmd->TagBody.Gpio = 128 + Gpio;

  Cmd->TagHead.TagValueSize   = 0;
  Cmd->EndTag                 = 0;

  Status = MailboxTransaction (Cmd->BufferHead.BufferSize, RPI_MBOX_VC_CHANNEL, &Result);

  *Polarity = Cmd->TagBody.Polarity;

  if (EFI_ERROR (Status) ||
      Cmd->BufferHead.Response != RPI_MBOX_RESP_SUCCESS) {
    DEBUG ((DEBUG_ERROR,
      "%a: mailbox  transaction error: Status == %r, Response == 0x%x\n",
      __func__, Status, Cmd->BufferHead.Response));
  }

  ReleaseMailboxLock ();

  return Status;
}

STATIC
EFI_STATUS
EFIAPI
RpiFirmwareNotifyGpioSetCfg (
  IN UINTN Gpio,
  IN UINTN Direction,
  IN UINTN State
  )
{
  RPI_FW_NOTIFY_GPIO_SET_CFG_CMD *Cmd;
  EFI_STATUS                   Status;
  UINT32                       Result;

  Status = RpiFirmwareNotifyGpioGetCfg (Gpio, &Result);
  if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "%a: Failed to get GPIO polarity\n", __func__));
      Result = 0; //default polarity
  }


  if (!AcquireMailboxLock ()) {
    DEBUG ((DEBUG_ERROR, "%a: failed to acquire spinlock\n", __func__));
    return EFI_DEVICE_ERROR;
  }

  Cmd = mDmaBuffer;
  ZeroMem (Cmd, sizeof (*Cmd));

  Cmd->BufferHead.BufferSize  = sizeof (*Cmd);
  Cmd->BufferHead.Response    = 0;
  Cmd->TagHead.TagId          = RPI_MBOX_SET_GPIO_CONFIG;
  Cmd->TagHead.TagSize        = sizeof (Cmd->TagBody);

  Cmd->TagBody.Gpio = 128 + Gpio;
  Cmd->TagBody.Direction = Direction;
  Cmd->TagBody.Polarity = Result;
  Cmd->TagBody.TermEn = 0;
  Cmd->TagBody.TermPullUp = 0;
  Cmd->TagBody.State = State;

  Cmd->TagHead.TagValueSize   = 0;
  Cmd->EndTag                 = 0;

  Status = MailboxTransaction (Cmd->BufferHead.BufferSize, RPI_MBOX_VC_CHANNEL, &Result);

  if (EFI_ERROR (Status) ||
      Cmd->BufferHead.Response != RPI_MBOX_RESP_SUCCESS) {
    DEBUG ((DEBUG_ERROR,
      "%a: mailbox  transaction error: Status == %r, Response == 0x%x\n",
      __func__, Status, Cmd->BufferHead.Response));
  }

  ReleaseMailboxLock ();

  RpiFirmwareSetGpio (Gpio,!State);


  return Status;
}


#pragma pack()
typedef struct {
  UINT32                    Register;
  UINT32                    Value;
} RPI_FW_RTC_TAG;

typedef struct {
  RPI_FW_BUFFER_HEAD        BufferHead;
  RPI_FW_TAG_HEAD           TagHead;
  RPI_FW_RTC_TAG            TagBody;
  UINT32                    EndTag;
} RPI_FW_RTC_CMD;

typedef struct {
  UINT32                    Id;
  UINT32                    Value;
} RPI_FW_ID_VALUE_TAG;

typedef struct {
  RPI_FW_BUFFER_HEAD        BufferHead;
  RPI_FW_TAG_HEAD           TagHead;
  RPI_FW_ID_VALUE_TAG       TagBody;
  UINT32                    EndTag;
} RPI_FW_ID_VALUE_CMD;

typedef struct {
  UINT32                    Status;
  CHAR8                     Data[RPI_FIRMWARE_GENCMD_DATA_SIZE];
} RPI_FW_GENCMD_TAG;

typedef struct {
  RPI_FW_BUFFER_HEAD        BufferHead;
  RPI_FW_TAG_HEAD           TagHead;
  RPI_FW_GENCMD_TAG         TagBody;
  UINT32                    EndTag;
} RPI_FW_GENCMD_CMD;
#pragma pack()

STATIC
EFI_STATUS
RpiFirmwareGetIdValue (
  IN  UINT32  TagId,
  IN  UINT32  Id,
  OUT UINT32  *Value
  )
{
  RPI_FW_ID_VALUE_CMD  *Cmd;
  EFI_STATUS           Status;
  UINT32               Result;

  if (Value == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (!AcquireMailboxLock ()) {
    DEBUG ((DEBUG_ERROR, "%a: failed to acquire mailbox lock\n", __func__));
    return EFI_NOT_READY;
  }

  Cmd = mDmaBuffer;
  ZeroMem (Cmd, sizeof (*Cmd));

  Cmd->BufferHead.BufferSize = sizeof (*Cmd);
  Cmd->TagHead.TagId = TagId;
  Cmd->TagHead.TagSize = sizeof (Cmd->TagBody);
  Cmd->TagBody.Id = Id;

  Status = MailboxTransaction (
             Cmd->BufferHead.BufferSize,
             RPI_MBOX_VC_CHANNEL,
             &Result
             );
  if (EFI_ERROR (Status) ||
      (Result != (UINT32)(mDmaBufferBusAddress | RPI_MBOX_VC_CHANNEL)) ||
      (Cmd->BufferHead.Response != RPI_MBOX_RESP_SUCCESS) ||
      ((Cmd->TagHead.TagValueSize & RPI_MBOX_VALUE_SIZE_RESPONSE_MASK) == 0) ||
      ((Cmd->TagHead.TagValueSize & ~RPI_MBOX_VALUE_SIZE_RESPONSE_MASK) <
       sizeof (Cmd->TagBody)) ||
      (Cmd->TagBody.Id != Id))
  {
    DEBUG ((
      DEBUG_VERBOSE,
      "%a: mailbox transaction failed: Tag=0x%x Id=%u Status=%r Response=0x%x\n",
      __func__,
      TagId,
      Id,
      Status,
      Cmd->BufferHead.Response
      ));
    Status = EFI_DEVICE_ERROR;
  } else {
    *Value = Cmd->TagBody.Value;
  }

  ReleaseMailboxLock ();
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
RpiFirmwareGetTemperature (
  IN  UINT32  TemperatureId,
  OUT UINT32  *Temperature
  )
{
  if (TemperatureId != 0) {
    return EFI_UNSUPPORTED;
  }

  return RpiFirmwareGetIdValue (
           RPI_MBOX_GET_TEMPERATURE,
           TemperatureId,
           Temperature
           );
}

STATIC
EFI_STATUS
EFIAPI
RpiFirmwareGetVoltage (
  IN  UINT32  VoltageId,
  OUT UINT32  *Voltage
  )
{
  if ((VoltageId < 1) || (VoltageId > 4)) {
    return EFI_UNSUPPORTED;
  }

  return RpiFirmwareGetIdValue (
           RPI_MBOX_GET_VOLTAGE,
           VoltageId,
           Voltage
           );
}

STATIC
EFI_STATUS
EFIAPI
RpiFirmwareGetGencmd (
  IN  CONST CHAR8  *Command,
  OUT CHAR8        *Response,
  IN  UINTN        ResponseSize
  )
{
  RPI_FW_GENCMD_CMD  *Cmd;
  UINTN              CommandSize;
  UINTN              ResponseLength;
  EFI_STATUS         Status;
  UINT32             Result;

  if ((Command == NULL) || (Response == NULL) || (ResponseSize == 0)) {
    return EFI_INVALID_PARAMETER;
  }

  CommandSize = AsciiStrSize (Command);
  if (CommandSize > RPI_FIRMWARE_GENCMD_DATA_SIZE) {
    return EFI_BAD_BUFFER_SIZE;
  }

  if (!AcquireMailboxLock ()) {
    DEBUG ((DEBUG_ERROR, "%a: failed to acquire mailbox lock\n", __func__));
    return EFI_NOT_READY;
  }

  Cmd = mDmaBuffer;
  ZeroMem (Cmd, sizeof (*Cmd));

  Cmd->BufferHead.BufferSize = sizeof (*Cmd);
  Cmd->TagHead.TagId         = RPI_MBOX_GET_GENCMD_RESULT;
  Cmd->TagHead.TagSize       = sizeof (Cmd->TagBody);
  CopyMem (Cmd->TagBody.Data, Command, CommandSize);

  Status = MailboxTransaction (
             Cmd->BufferHead.BufferSize,
             RPI_MBOX_VC_CHANNEL,
             &Result
             );
  if (EFI_ERROR (Status) ||
      (Result != (UINT32)(mDmaBufferBusAddress | RPI_MBOX_VC_CHANNEL)) ||
      (Cmd->BufferHead.Response != RPI_MBOX_RESP_SUCCESS) ||
      ((Cmd->TagHead.TagValueSize & RPI_MBOX_VALUE_SIZE_RESPONSE_MASK) == 0) ||
      ((Cmd->TagHead.TagValueSize & ~RPI_MBOX_VALUE_SIZE_RESPONSE_MASK) <
       sizeof (Cmd->TagBody.Status)) ||
      (Cmd->TagBody.Status != 0))
  {
    DEBUG ((
      DEBUG_VERBOSE,
      "%a: command failed: Command='%a' Status=%r Response=0x%x Result=%u\n",
      __func__,
      Command,
      Status,
      Cmd->BufferHead.Response,
      Cmd->TagBody.Status
      ));
    Status = EFI_DEVICE_ERROR;
    goto Exit;
  }

  ResponseLength = AsciiStrnLenS (
                     Cmd->TagBody.Data,
                     sizeof (Cmd->TagBody.Data)
                     );
  if (ResponseLength == sizeof (Cmd->TagBody.Data)) {
    Status = EFI_DEVICE_ERROR;
    goto Exit;
  }

  if (ResponseLength >= ResponseSize) {
    Status = EFI_BUFFER_TOO_SMALL;
    goto Exit;
  }

  CopyMem (Response, Cmd->TagBody.Data, ResponseLength + 1);
  Status = EFI_SUCCESS;

Exit:
  ReleaseMailboxLock ();
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
RpiFirmwareGetMailboxBuffer (
  OUT EFI_PHYSICAL_ADDRESS  *CpuAddress,
  OUT UINTN                 *BusAddress,
  OUT UINTN                 *BufferSize,
  OUT EFI_PHYSICAL_ADDRESS  *MailboxAddress
  )
{
  if ((CpuAddress == NULL) || (BusAddress == NULL) ||
      (BufferSize == NULL) || (MailboxAddress == NULL))
  {
    return EFI_INVALID_PARAMETER;
  }

  *CpuAddress = mDmaBufferPhysicalAddress;
  *BusAddress = mDmaBufferBusAddress;
  *BufferSize = mDmaBufferSize;
  *MailboxAddress = mMboxPhysicalAddress;

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
RpiFirmwareGetRtc (
  IN   RASPBERRY_PI_RTC_REGISTER  Register,
  OUT  UINT32                     *Value
  )
{
  RPI_FW_RTC_CMD               *Cmd;
  EFI_STATUS                   Status;
  UINT32                       Result;

  if (!AcquireMailboxLock ()) {
    DEBUG ((DEBUG_ERROR, "%a: failed to acquire spinlock\n", __FUNCTION__));
    return EFI_DEVICE_ERROR;
  }

  Cmd = mDmaBuffer;
  ZeroMem (Cmd, sizeof (*Cmd));

  Cmd->BufferHead.BufferSize  = sizeof (*Cmd);
  Cmd->BufferHead.Response    = 0;
  Cmd->TagHead.TagId          = RPI_MBOX_GET_RTC_REG;
  Cmd->TagHead.TagSize        = sizeof (Cmd->TagBody);
  Cmd->TagHead.TagValueSize   = 0;
  Cmd->TagBody.Register       = Register;
  Cmd->TagBody.Value          = 0;
  Cmd->EndTag                 = 0;

  Status = MailboxTransaction (Cmd->BufferHead.BufferSize, RPI_MBOX_VC_CHANNEL, &Result);

  if (EFI_ERROR (Status) ||
      Cmd->BufferHead.Response != RPI_MBOX_RESP_SUCCESS) {
    DEBUG ((DEBUG_ERROR,
      "%a: mailbox  transaction error: Status == %r, Response == 0x%x\n",
      __FUNCTION__, Status, Cmd->BufferHead.Response));
    Status = EFI_DEVICE_ERROR;
  } else {
    *Value = Cmd->TagBody.Value;
  }

  ReleaseMailboxLock ();

  return Status;
}

STATIC
EFI_STATUS
EFIAPI
RpiFirmwareSetRtc (
  IN   RASPBERRY_PI_RTC_REGISTER  Register,
  IN   UINT32                     Value
  )
{
  RPI_FW_RTC_CMD               *Cmd;
  EFI_STATUS                   Status;
  UINT32                       Result;

  if (!AcquireMailboxLock ()) {
    DEBUG ((DEBUG_ERROR, "%a: failed to acquire spinlock\n", __FUNCTION__));
    return EFI_DEVICE_ERROR;
  }

  Cmd = mDmaBuffer;
  ZeroMem (Cmd, sizeof (*Cmd));

  Cmd->BufferHead.BufferSize  = sizeof (*Cmd);
  Cmd->BufferHead.Response    = 0;
  Cmd->TagHead.TagId          = RPI_MBOX_SET_RTC_REG;
  Cmd->TagHead.TagSize        = sizeof (Cmd->TagBody);
  Cmd->TagHead.TagValueSize   = 0;
  Cmd->TagBody.Register       = Register;
  Cmd->TagBody.Value          = Value;
  Cmd->EndTag                 = 0;

  Status = MailboxTransaction (Cmd->BufferHead.BufferSize, RPI_MBOX_VC_CHANNEL, &Result);

  if (EFI_ERROR (Status) ||
      Cmd->BufferHead.Response != RPI_MBOX_RESP_SUCCESS) {
    DEBUG ((DEBUG_ERROR,
      "%a: mailbox  transaction error: Status == %r, Response == 0x%x\n",
      __FUNCTION__, Status, Cmd->BufferHead.Response));
    Status = EFI_DEVICE_ERROR;
  }

  ReleaseMailboxLock ();

  return Status;
}

STATIC RASPBERRY_PI_FIRMWARE_PROTOCOL mRpiFirmwareProtocol = {
  RpiFirmwareSetPowerState,
  RpiFirmwareGetMacAddress,
  RpiFirmwareGetCommmandLine,
  RpiFirmwareGetCurrentClockRate,
  RpiFirmwareGetMaxClockRate,
  RpiFirmwareGetMinClockRate,
  RpiFirmwareSetClockRate,
  RpiFirmwareAllocFb,
  RpiFirmwareFreeFb,
  RpiFirmwareGetFbSize,
  RpiFirmwareSetLed,
  RpiFirmwareGetSerial,
  RpiFirmwareGetModel,
  RpiFirmwareGetModelRevision,
  RpiFirmwareGetModelName,
  RPiFirmwareGetModelFamily,
  RpiFirmwareGetFirmwareRevision,
  RpiFirmwareGetManufacturerName,
  RpiFirmwareGetCpuName,
  RpiFirmwareGetArmMemory,
  RPiFirmwareGetModelInstalledMB,
  RpiFirmwareNotifyXhciReset,
  RpiFirmwareGetCurrentClockState,
  RpiFirmwareSetClockState,
  RpiFirmwareNotifyGpioSetCfg,
  RpiFirmwareGetRtc,
  RpiFirmwareSetRtc,
  RpiFirmwareGetTemperature,
  RpiFirmwareGetVoltage,
  RpiFirmwareGetGencmd,
  RpiFirmwareGetMailboxBuffer,
};

STATIC
VOID
EFIAPI
RpiFirmwareVirtualAddressChangeNotify (
  IN EFI_EVENT        Event,
  IN VOID             *Context
  )
{
  EfiConvertPointer (0x0, (VOID **)&mMboxBaseAddress);
  EfiConvertPointer (0x0, (VOID **)&mDmaBuffer);
  EfiConvertPointer (0x0, (VOID **)&mRpiFirmwareProtocol.GetRtc);
  EfiConvertPointer (0x0, (VOID **)&mRpiFirmwareProtocol.SetRtc);
  EfiConvertPointer (0x0, (VOID **)&mRpiFirmwareProtocol.GetTemperature);
  EfiConvertPointer (0x0, (VOID **)&mRpiFirmwareProtocol.GetVoltage);
  EfiConvertPointer (0x0, (VOID **)&mRpiFirmwareProtocol.GetGencmd);
}

/**
  Initialize the state information for the CPU Architectural Protocol

  @param  ImageHandle   of the loaded driver
  @param  SystemTable   Pointer to the System Table

  @retval EFI_SUCCESS           Protocol registered
  @retval EFI_OUT_OF_RESOURCES  Cannot allocate protocol data structure
  @retval EFI_DEVICE_ERROR      Hardware problems

**/
EFI_STATUS
RpiFirmwareDxeInitialize (
  IN EFI_HANDLE         ImageHandle,
  IN EFI_SYSTEM_TABLE   *SystemTable
  )
{
  EFI_STATUS      Status;
  UINTN           AlignedMboxAddress;
  EFI_EVENT       VirtualAddressChangeEvent = NULL;

  mMboxPhysicalAddress = PcdGet64 (PcdFwMailboxBaseAddress);
  mMboxBaseAddress = (UINTN)mMboxPhysicalAddress;

  //
  // We only need one of these
  //
  ASSERT_PROTOCOL_ALREADY_INSTALLED (NULL, &gRaspberryPiFirmwareProtocolGuid);

  InitializeSpinLock (&mMailboxLock);

  Status = DmaAllocateBuffer (EfiRuntimeServicesData, NUM_PAGES, &mDmaBuffer);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: failed to allocate DMA buffer (Status == %r)\n", __func__));
    return Status;
  }

  mDmaBufferSize = EFI_PAGES_TO_SIZE (NUM_PAGES);
  mDmaBufferPhysicalAddress = (EFI_PHYSICAL_ADDRESS)(UINTN)mDmaBuffer;
  ZeroMem (mDmaBuffer, mDmaBufferSize);
  Status = DmaMap (MapOperationBusMasterCommonBuffer, mDmaBuffer, &mDmaBufferSize,
             &mDmaBufferBusAddress, &mDmaBufferMapping);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: failed to map DMA buffer (Status == %r)\n", __func__));
    goto FreeBuffer;
  }

  //
  // The channel index is encoded in the low bits of the bus address,
  // so make sure these are cleared.
  //
  ASSERT (!(mDmaBufferBusAddress & (BCM2836_MBOX_NUM_CHANNELS - 1)));

  Status = gBS->InstallProtocolInterface (&ImageHandle,
                  &gRaspberryPiFirmwareProtocolGuid, EFI_NATIVE_INTERFACE,
                  &mRpiFirmwareProtocol);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR,
      "%a: failed to install RPI firmware protocol (Status == %r)\n",
      __func__, Status));
    goto UnmapBuffer;
  }

  AlignedMboxAddress = mMboxBaseAddress & ~(EFI_PAGE_SIZE - 1);

  Status = gDS->AddMemorySpace (
                  EfiGcdMemoryTypeMemoryMappedIo,
                  AlignedMboxAddress,
                  EFI_PAGE_SIZE,
                  EFI_MEMORY_UC | EFI_MEMORY_RUNTIME);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: AddMemorySpace failed. Status=%r\n",
            __FUNCTION__, Status));
    goto UnmapBuffer;
  }

  Status = gDS->SetMemorySpaceAttributes (
                  AlignedMboxAddress,
                  EFI_PAGE_SIZE,
                  EFI_MEMORY_UC | EFI_MEMORY_RUNTIME);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: SetMemorySpaceAttributes failed. Status=%r\n",
            __FUNCTION__, Status));
    goto UnmapBuffer;
  }

  Status = gBS->CreateEventEx (
                  EVT_NOTIFY_SIGNAL,
                  TPL_NOTIFY,
                  RpiFirmwareVirtualAddressChangeNotify,
                  NULL,
                  &gEfiEventVirtualAddressChangeGuid,
                  &VirtualAddressChangeEvent);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: failed to register for virtual address change. Status=%r\n",
            __func__, Status));
    goto UnmapBuffer;
  }

  return EFI_SUCCESS;

UnmapBuffer:
  DmaUnmap (mDmaBufferMapping);
FreeBuffer:
  DmaFreeBuffer (NUM_PAGES, mDmaBuffer);

  return Status;
}

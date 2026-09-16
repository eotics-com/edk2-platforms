/** @file
  UEFI Simple Network driver for SMSC/Microchip LAN95xx USB Ethernet.

  Copyright (c) 2026 Ahmed ARIF <arif.ing@outlook.com>

  The LAN9512/LAN9514 used by Raspberry Pi 3 exposes its Ethernet function as
  a vendor-specific USB device (0424:ec00).  This driver binds that function,
  programs the public LAN95xx register interface, and publishes
  EFI_SIMPLE_NETWORK_PROTOCOL for the EDK2 network stack.

  SPDX-License-Identifier: GPL-2.0-or-later
**/

#include <Uefi.h>

#include <IndustryStandard/Usb.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/NetLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

#include <Protocol/DevicePath.h>
#include <Protocol/RpiFirmware.h>
#include <Protocol/SimpleNetwork.h>
#include <Protocol/UsbIo.h>

#define SMSC95XX_SIGNATURE              SIGNATURE_32 ('S', '9', '5', 'X')
#define SMSC95XX_VENDOR_ID              0x0424
#define SMSC95XX_PRODUCT_LAN951X        0xEC00
#define SMSC95XX_PRODUCT_LAN9500        0x9500
#define SMSC95XX_PRODUCT_LAN9500A       0x9E00
#define SMSC95XX_PRODUCT_LAN9530        0x9530
#define SMSC95XX_PRODUCT_LAN9730        0x9730
#define SMSC95XX_PRODUCT_SMSC9500       0x9900

#define SMSC95XX_WRITE_REGISTER         0xA0
#define SMSC95XX_READ_REGISTER          0xA1

#define SMSC_ID_REV                     0x0000
#define SMSC_INT_STS                    0x0008
#define SMSC_INT_STS_RXDF               0x00000800
#define SMSC_TX_CFG                     0x0010
#define SMSC_HW_CFG                     0x0014
#define SMSC_LED_GPIO_CFG               0x0024
#define SMSC_AFC_CFG                    0x002C
#define SMSC_E2P_CMD                    0x0030
#define SMSC_E2P_DATA                   0x0034
#define SMSC_BURST_CAP                  0x0038
#define SMSC_INT_EP_CTL                 0x0068
#define SMSC_BULK_IN_DLY                0x006C
#define SMSC_MAC_CR                     0x0100
#define SMSC_ADDRH                      0x0104
#define SMSC_ADDRL                      0x0108
#define SMSC_HASHH                      0x010C
#define SMSC_HASHL                      0x0110
#define SMSC_MII_ADDR                   0x0114
#define SMSC_MII_DATA                   0x0118
#define SMSC_FLOW                       0x011C
#define SMSC_VLAN1                      0x0120
#define SMSC_COE_CR                     0x0130

#define SMSC_HW_CFG_BIR                 0x00001000
#define SMSC_HW_CFG_RXDOFF              0x00000600
#define SMSC_HW_CFG_MEF                 0x00000020
#define SMSC_HW_CFG_BCE                 0x00000002
#define SMSC_HW_CFG_LRST                0x00000008

#define SMSC_TX_CFG_ON                  0x00000004
#define SMSC_INT_EP_CTL_PHY_INT         0x00008000
#define SMSC_LED_GPIO_SPEED             0x01000000
#define SMSC_LED_GPIO_LINK              0x00100000
#define SMSC_LED_GPIO_DUPLEX            0x00010000
#define SMSC_AFC_CFG_DEFAULT            0x00F830A1
#define SMSC_AFC_CFG_FLOW_CONTROL       0x0000000F
#define SMSC_FLOW_CONTROL_FULL          0xFFFF0002

#define SMSC_MAC_CR_RXALL               0x80000000
#define SMSC_MAC_CR_RCVOWN              0x00800000
#define SMSC_MAC_CR_FDPX                0x00100000
#define SMSC_MAC_CR_MCPAS               0x00080000
#define SMSC_MAC_CR_PRMS                0x00040000
#define SMSC_MAC_CR_HPFILT              0x00002000
#define SMSC_MAC_CR_BCAST               0x00000800
#define SMSC_MAC_CR_TXEN                0x00000008
#define SMSC_MAC_CR_RXEN                0x00000004

#define SMSC_MII_WRITE                  0x00000002
#define SMSC_MII_BUSY                   0x00000001
#define SMSC_MII_BMCR                   0
#define SMSC_MII_BMSR                   1
#define SMSC_MII_ADVERTISE              4
#define SMSC_MII_PHY_INT_SOURCE         29
#define SMSC_MII_PHY_INT_MASK           30
#define SMSC_MII_PHY_SPECIAL            31
#define SMSC_MII_BMCR_RESET             0x8000
#define SMSC_MII_BMCR_AN_ENABLE         0x1000
#define SMSC_MII_BMCR_POWER_DOWN        0x0800
#define SMSC_MII_BMCR_ISOLATE           0x0400
#define SMSC_MII_BMCR_AN_RESTART        0x0200
#define SMSC_MII_BMSR_LINK              0x0004
#define SMSC_MII_ADVERTISE_DEFAULT      0x01E1
#define SMSC_MII_ADVERTISE_PAUSE_CAP    0x0400
#define SMSC_MII_ADVERTISE_PAUSE_ASYM   0x0800
#define SMSC_MII_PHY_INT_DEFAULT        0x0050
#define SMSC_MII_SPECIAL_SPEED          0x001C
#define SMSC_MII_SPECIAL_10_HALF        0x0004
#define SMSC_MII_SPECIAL_100_HALF       0x0008
#define SMSC_MII_SPECIAL_10_FULL        0x0014
#define SMSC_MII_SPECIAL_100_FULL       0x0018

#define SMSC_E2P_BUSY                   0x80000000
#define SMSC_E2P_TIMEOUT                0x00000400
#define SMSC_E2P_ADDRESS_MASK           0x000001FF
#define SMSC_EEPROM_MAC_OFFSET          1

#define SMSC_TX_CMD_A_FIRST             0x00002000
#define SMSC_TX_CMD_A_LAST              0x00001000
#define SMSC_TX_LENGTH_MASK             0x000007FF
#define SMSC_RX_STATUS_LENGTH           0x3FFF0000
#define SMSC_RX_STATUS_ERROR            0x00008000

#define SMSC_ETHERNET_HEADER_SIZE       14
#define SMSC_ETHERNET_MTU               1500
#define SMSC_MAX_FRAME_SIZE             1518
#define SMSC_MIN_FRAME_SIZE             60
#define SMSC_HIGH_SPEED_PACKET_SIZE     512
#define SMSC_FULL_SPEED_PACKET_SIZE     64
#define SMSC_HS_BURST_SIZE              (16 * 1024 + 5 * SMSC_HIGH_SPEED_PACKET_SIZE)
#define SMSC_FS_BURST_SIZE              (6 * 1024 + 33 * SMSC_FULL_SPEED_PACKET_SIZE)
#define SMSC_BULK_IN_DELAY_DEFAULT      0x00002000
#define SMSC_RX_BUFFER_SIZE             SMSC_HS_BURST_SIZE
#define SMSC_TX_BUFFER_SIZE             (8 + SMSC_MAX_FRAME_SIZE)
#define SMSC_CONTROL_TIMEOUT_MS         1000
#define SMSC_BULK_IN_TIMEOUT_MS         20
#define SMSC_BULK_OUT_TIMEOUT_MS        3000
#define SMSC_PHY_ID                     1
#define SMSC_MCAST_FILTER_COUNT         16

#define SMSC95XX_FROM_SNP(This) \
  BASE_CR ((This), SMSC95XX_DEVICE, Snp)

typedef struct {
  UINT32                      Signature;
  EFI_HANDLE                  ParentHandle;
  EFI_HANDLE                  ChildHandle;
  EFI_USB_IO_PROTOCOL         *UsbIo;
  EFI_DEVICE_PATH_PROTOCOL    *DevicePath;
  EFI_SIMPLE_NETWORK_PROTOCOL Snp;
  EFI_SIMPLE_NETWORK_MODE     Mode;
  UINT8                       BulkInEndpoint;
  UINT8                       BulkOutEndpoint;
  UINT16                      BulkInMaxPacket;
  UINT16                      BulkOutMaxPacket;
  UINTN                       RxTransferSize;
  UINT32                      DeviceIdRevision;
  UINT32                      MacControl;
  UINT8                       RxBuffer[SMSC_RX_BUFFER_SIZE];
  UINTN                       RxLength;
  UINTN                       RxOffset;
  UINTN                       RxPendingLength;
  UINT8                       TxBuffer[SMSC_TX_BUFFER_SIZE];
  VOID                        *RecycledTxBuffer;
} SMSC95XX_DEVICE;

STATIC EFI_STATUS EFIAPI Smsc95xxDriverSupported (
  IN EFI_DRIVER_BINDING_PROTOCOL *This,
  IN EFI_HANDLE                  Controller,
  IN EFI_DEVICE_PATH_PROTOCOL    *RemainingDevicePath OPTIONAL
  );
STATIC EFI_STATUS EFIAPI Smsc95xxDriverStart (
  IN EFI_DRIVER_BINDING_PROTOCOL *This,
  IN EFI_HANDLE                  Controller,
  IN EFI_DEVICE_PATH_PROTOCOL    *RemainingDevicePath OPTIONAL
  );
STATIC EFI_STATUS EFIAPI Smsc95xxDriverStop (
  IN EFI_DRIVER_BINDING_PROTOCOL *This,
  IN EFI_HANDLE                  Controller,
  IN UINTN                       NumberOfChildren,
  IN EFI_HANDLE                  *ChildHandleBuffer OPTIONAL
  );

STATIC EFI_DRIVER_BINDING_PROTOCOL mSmsc95xxDriverBinding = {
  Smsc95xxDriverSupported,
  Smsc95xxDriverStart,
  Smsc95xxDriverStop,
  0x10,
  NULL,
  NULL
};

STATIC
UINT32
SmscReadLe32 (
  IN CONST UINT8 *Buffer
  )
{
  return (UINT32)Buffer[0] |
         ((UINT32)Buffer[1] << 8) |
         ((UINT32)Buffer[2] << 16) |
         ((UINT32)Buffer[3] << 24);
}

STATIC
VOID
SmscWriteLe32 (
  OUT UINT8  *Buffer,
  IN  UINT32 Value
  )
{
  Buffer[0] = (UINT8)Value;
  Buffer[1] = (UINT8)(Value >> 8);
  Buffer[2] = (UINT8)(Value >> 16);
  Buffer[3] = (UINT8)(Value >> 24);
}

STATIC
BOOLEAN
SmscIsSupportedProduct (
  IN UINT16 ProductId
  )
{
  return ProductId == SMSC95XX_PRODUCT_LAN951X ||
         ProductId == SMSC95XX_PRODUCT_LAN9500 ||
         ProductId == SMSC95XX_PRODUCT_LAN9500A ||
         ProductId == SMSC95XX_PRODUCT_LAN9530 ||
         ProductId == SMSC95XX_PRODUCT_LAN9730 ||
         ProductId == SMSC95XX_PRODUCT_SMSC9500;
}

STATIC
EFI_STATUS
SmscVendorTransfer (
  IN     SMSC95XX_DEVICE *Device,
  IN     UINT8           RequestCode,
  IN     UINT16          Register,
  IN OUT VOID            *Buffer,
  IN     UINTN           BufferSize,
  IN     BOOLEAN         Read
  )
{
  EFI_USB_DEVICE_REQUEST Request;
  EFI_USB_DATA_DIRECTION Direction;
  EFI_STATUS             Status;
  UINT32                 UsbStatus;

  ZeroMem (&Request, sizeof (Request));
  Request.RequestType = (UINT8)(USB_REQ_TYPE_VENDOR | USB_TARGET_DEVICE |
                                (Read ? USB_ENDPOINT_DIR_IN : 0));
  Request.Request = RequestCode;
  Request.Index = Register;
  Request.Length = (UINT16)BufferSize;
  Direction = Read ? EfiUsbDataIn : EfiUsbDataOut;

  UsbStatus = 0;
  Status = Device->UsbIo->UsbControlTransfer (
                            Device->UsbIo,
                            &Request,
                            Direction,
                            SMSC_CONTROL_TIMEOUT_MS,
                            Buffer,
                            BufferSize,
                            &UsbStatus
                            );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return UsbStatus == 0 ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}

STATIC
EFI_STATUS
SmscReadRegister (
  IN  SMSC95XX_DEVICE *Device,
  IN  UINT16          Register,
  OUT UINT32          *Value
  )
{
  EFI_STATUS Status;
  UINT8      Buffer[4];

  Status = SmscVendorTransfer (
             Device,
             SMSC95XX_READ_REGISTER,
             Register,
             Buffer,
             sizeof (Buffer),
             TRUE
             );
  if (!EFI_ERROR (Status)) {
    *Value = SmscReadLe32 (Buffer);
  }

  return Status;
}

STATIC
EFI_STATUS
SmscWriteRegister (
  IN SMSC95XX_DEVICE *Device,
  IN UINT16          Register,
  IN UINT32          Value
  )
{
  UINT8 Buffer[4];

  SmscWriteLe32 (Buffer, Value);
  return SmscVendorTransfer (
           Device,
           SMSC95XX_WRITE_REGISTER,
           Register,
           Buffer,
           sizeof (Buffer),
           FALSE
           );
}

STATIC
EFI_STATUS
SmscWaitForMii (
  IN SMSC95XX_DEVICE *Device
  )
{
  EFI_STATUS Status;
  UINT32     Value;
  UINTN      Retry;

  for (Retry = 0; Retry < 100; Retry++) {
    Status = SmscReadRegister (Device, SMSC_MII_ADDR, &Value);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    if ((Value & SMSC_MII_BUSY) == 0) {
      return EFI_SUCCESS;
    }

    gBS->Stall (10000);
  }

  return EFI_TIMEOUT;
}

STATIC
EFI_STATUS
SmscReadPhy (
  IN  SMSC95XX_DEVICE *Device,
  IN  UINT8           Register,
  OUT UINT16          *Value
  )
{
  EFI_STATUS Status;
  UINT32     Command;
  UINT32     Data;

  Status = SmscWaitForMii (Device);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Command = ((UINT32)SMSC_PHY_ID << 11) |
            ((UINT32)(Register & 0x1F) << 6) |
            SMSC_MII_BUSY;
  Status = SmscWriteRegister (Device, SMSC_MII_ADDR, Command);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = SmscWaitForMii (Device);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = SmscReadRegister (Device, SMSC_MII_DATA, &Data);
  if (!EFI_ERROR (Status)) {
    *Value = (UINT16)Data;
  }

  return Status;
}

STATIC
EFI_STATUS
SmscWritePhy (
  IN SMSC95XX_DEVICE *Device,
  IN UINT8           Register,
  IN UINT16          Value
  )
{
  EFI_STATUS Status;
  UINT32     Command;

  Status = SmscWaitForMii (Device);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = SmscWriteRegister (Device, SMSC_MII_DATA, Value);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Command = ((UINT32)SMSC_PHY_ID << 11) |
            ((UINT32)(Register & 0x1F) << 6) |
            SMSC_MII_WRITE |
            SMSC_MII_BUSY;
  Status = SmscWriteRegister (Device, SMSC_MII_ADDR, Command);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return SmscWaitForMii (Device);
}

STATIC
BOOLEAN
SmscIsValidMac (
  IN CONST UINT8 *Address
  )
{
  UINTN Index;
  UINT8 OrValue;
  UINT8 AndValue;

  if ((Address[0] & 1) != 0) {
    return FALSE;
  }

  OrValue = 0;
  AndValue = 0xFF;
  for (Index = 0; Index < NET_ETHER_ADDR_LEN; Index++) {
    OrValue |= Address[Index];
    AndValue &= Address[Index];
  }

  return OrValue != 0 && AndValue != 0xFF;
}

STATIC
EFI_STATUS
SmscReadEepromByte (
  IN  SMSC95XX_DEVICE *Device,
  IN  UINT16          Address,
  OUT UINT8           *Value
  )
{
  EFI_STATUS Status;
  UINT32     Command;
  UINT32     Data;
  UINTN      Retry;

  for (Retry = 0; Retry < 100; Retry++) {
    Status = SmscReadRegister (Device, SMSC_E2P_CMD, &Command);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    if ((Command & SMSC_E2P_BUSY) == 0) {
      break;
    }

    gBS->Stall (10000);
  }

  if (Retry == 100) {
    return EFI_TIMEOUT;
  }

  Command = SMSC_E2P_BUSY | (Address & SMSC_E2P_ADDRESS_MASK);
  Status = SmscWriteRegister (Device, SMSC_E2P_CMD, Command);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  for (Retry = 0; Retry < 100; Retry++) {
    gBS->Stall (10000);
    Status = SmscReadRegister (Device, SMSC_E2P_CMD, &Command);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    if ((Command & SMSC_E2P_BUSY) == 0) {
      if ((Command & SMSC_E2P_TIMEOUT) != 0) {
        return EFI_TIMEOUT;
      }

      Status = SmscReadRegister (Device, SMSC_E2P_DATA, &Data);
      if (!EFI_ERROR (Status)) {
        *Value = (UINT8)Data;
      }

      return Status;
    }
  }

  return EFI_TIMEOUT;
}

STATIC
EFI_STATUS
SmscGetMacAddress (
  IN  SMSC95XX_DEVICE *Device,
  OUT UINT8           *Address
  )
{
  RASPBERRY_PI_FIRMWARE_PROTOCOL *Firmware;
  EFI_STATUS                     Status;
  UINT32                         AddressLow;
  UINT32                         AddressHigh;
  UINTN                          Index;

  Firmware = NULL;
  Status = gBS->LocateProtocol (
                  &gRaspberryPiFirmwareProtocolGuid,
                  NULL,
                  (VOID **)&Firmware
                  );
  if (!EFI_ERROR (Status) && Firmware != NULL) {
    Status = Firmware->GetMacAddress (Address);
    if (!EFI_ERROR (Status) && SmscIsValidMac (Address)) {
      return EFI_SUCCESS;
    }
  }

  Status = SmscReadRegister (Device, SMSC_ADDRL, &AddressLow);
  if (!EFI_ERROR (Status)) {
    Status = SmscReadRegister (Device, SMSC_ADDRH, &AddressHigh);
  }

  if (!EFI_ERROR (Status)) {
    Address[0] = (UINT8)AddressLow;
    Address[1] = (UINT8)(AddressLow >> 8);
    Address[2] = (UINT8)(AddressLow >> 16);
    Address[3] = (UINT8)(AddressLow >> 24);
    Address[4] = (UINT8)AddressHigh;
    Address[5] = (UINT8)(AddressHigh >> 8);
    if (SmscIsValidMac (Address)) {
      return EFI_SUCCESS;
    }
  }

  for (Index = 0; Index < NET_ETHER_ADDR_LEN; Index++) {
    Status = SmscReadEepromByte (
               Device,
               SMSC_EEPROM_MAC_OFFSET + (UINT16)Index,
               &Address[Index]
               );
    if (EFI_ERROR (Status)) {
      break;
    }
  }

  if (!EFI_ERROR (Status) && SmscIsValidMac (Address)) {
    return EFI_SUCCESS;
  }

  Address[0] = 0x02;
  Address[1] = 0x42;
  Address[2] = 0x4C;
  Address[3] = (UINT8)(Device->DeviceIdRevision >> 16);
  Address[4] = (UINT8)(Device->DeviceIdRevision >> 8);
  Address[5] = (UINT8)Device->DeviceIdRevision;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
SmscProgramMacAddress (
  IN SMSC95XX_DEVICE *Device,
  IN CONST UINT8     *Address
  )
{
  EFI_STATUS Status;
  UINT32     AddressLow;
  UINT32     AddressHigh;

  AddressLow = (UINT32)Address[0] |
               ((UINT32)Address[1] << 8) |
               ((UINT32)Address[2] << 16) |
               ((UINT32)Address[3] << 24);
  AddressHigh = (UINT32)Address[4] | ((UINT32)Address[5] << 8);

  Status = SmscWriteRegister (Device, SMSC_ADDRL, AddressLow);
  if (!EFI_ERROR (Status)) {
    Status = SmscWriteRegister (Device, SMSC_ADDRH, AddressHigh);
  }

  return Status;
}

STATIC
UINT32
SmscEthernetCrc (
  IN CONST UINT8 *Address
  )
{
  UINT32 Crc;
  UINTN  ByteIndex;
  UINTN  BitIndex;

  Crc = 0xFFFFFFFF;
  for (ByteIndex = 0; ByteIndex < NET_ETHER_ADDR_LEN; ByteIndex++) {
    UINT8 CurrentByte;

    CurrentByte = Address[ByteIndex];
    for (BitIndex = 0; BitIndex < 8; BitIndex++) {
      UINT32 Carry;

      Carry = (Crc ^ CurrentByte) & 1;
      Crc >>= 1;
      if (Carry != 0) {
        Crc ^= 0xEDB88320;
      }

      CurrentByte >>= 1;
    }
  }

  return Crc;
}

STATIC
EFI_STATUS
SmscProgramReceiveFilters (
  IN SMSC95XX_DEVICE *Device
  )
{
  EFI_SIMPLE_NETWORK_MODE *Mode;
  EFI_STATUS              Status;
  UINT32                  HashHigh;
  UINT32                  HashLow;
  UINT32                  MacControl;
  UINTN                   Index;

  Mode = &Device->Mode;
  HashHigh = 0;
  HashLow = 0;
  MacControl = Device->MacControl &
               ~(SMSC_MAC_CR_RXALL | SMSC_MAC_CR_MCPAS |
                 SMSC_MAC_CR_PRMS | SMSC_MAC_CR_HPFILT |
                 SMSC_MAC_CR_BCAST);

  if ((Mode->ReceiveFilterSetting & EFI_SIMPLE_NETWORK_RECEIVE_PROMISCUOUS) != 0) {
    MacControl |= SMSC_MAC_CR_PRMS;
  } else if ((Mode->ReceiveFilterSetting &
              EFI_SIMPLE_NETWORK_RECEIVE_PROMISCUOUS_MULTICAST) != 0) {
    MacControl |= SMSC_MAC_CR_MCPAS;
  } else if ((Mode->ReceiveFilterSetting & EFI_SIMPLE_NETWORK_RECEIVE_MULTICAST) != 0 &&
             Mode->MCastFilterCount != 0) {
    MacControl |= SMSC_MAC_CR_HPFILT;
    for (Index = 0; Index < Mode->MCastFilterCount; Index++) {
      UINT32 HashBit;

      HashBit = (SmscEthernetCrc (Mode->MCastFilter[Index].Addr) >> 26) & 0x3F;
      if ((HashBit & 0x20) != 0) {
        HashHigh |= 1U << (HashBit & 0x1F);
      } else {
        HashLow |= 1U << HashBit;
      }
    }
  }

  if ((Mode->ReceiveFilterSetting &
       (EFI_SIMPLE_NETWORK_RECEIVE_BROADCAST |
        EFI_SIMPLE_NETWORK_RECEIVE_PROMISCUOUS)) == 0) {
    MacControl |= SMSC_MAC_CR_BCAST;
  }

  Status = SmscWriteRegister (Device, SMSC_HASHH, HashHigh);
  if (!EFI_ERROR (Status)) {
    Status = SmscWriteRegister (Device, SMSC_HASHL, HashLow);
  }
  if (!EFI_ERROR (Status)) {
    Status = SmscWriteRegister (Device, SMSC_MAC_CR, MacControl);
  }
  if (!EFI_ERROR (Status)) {
    Device->MacControl = MacControl;
  }

  return Status;
}

STATIC
EFI_STATUS
SmscUpdateLink (
  IN SMSC95XX_DEVICE *Device
  )
{
  EFI_STATUS Status;
  UINT16     Bmsr;
  UINT16     PhySpecial;
  UINT32     MacControl;
  BOOLEAN    FullDuplex;

  Status = SmscReadPhy (Device, SMSC_MII_BMSR, &Bmsr);
  if (!EFI_ERROR (Status)) {
    Status = SmscReadPhy (Device, SMSC_MII_BMSR, &Bmsr);
  }
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Device->Mode.MediaPresent = (Bmsr & SMSC_MII_BMSR_LINK) != 0;
  if (!Device->Mode.MediaPresent) {
    return EFI_SUCCESS;
  }

  Status = SmscReadPhy (Device, SMSC_MII_PHY_SPECIAL, &PhySpecial);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  FullDuplex = (PhySpecial & SMSC_MII_SPECIAL_SPEED) == SMSC_MII_SPECIAL_10_FULL ||
               (PhySpecial & SMSC_MII_SPECIAL_SPEED) == SMSC_MII_SPECIAL_100_FULL;
  MacControl = Device->MacControl;
  if (FullDuplex) {
    MacControl |= SMSC_MAC_CR_FDPX;
    MacControl &= ~SMSC_MAC_CR_RCVOWN;
  } else {
    MacControl &= ~SMSC_MAC_CR_FDPX;
    MacControl |= SMSC_MAC_CR_RCVOWN;
  }

  if (MacControl != Device->MacControl) {
    Status = SmscWriteRegister (Device, SMSC_MAC_CR, MacControl);
    if (!EFI_ERROR (Status)) {
      Device->MacControl = MacControl;
    }
  }

  return Status;
}

STATIC
EFI_STATUS
SmscInitializeHardware (
  IN SMSC95XX_DEVICE *Device
  )
{
  EFI_STATUS Status;
  UINT32     Value;
  UINT32     BurstCapacity;
  UINT16     PhyControl;
  UINT16     PhyInterruptSource;
  UINTN      Retry;

  Status = SmscWriteRegister (Device, SMSC_HW_CFG, SMSC_HW_CFG_LRST);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  for (Retry = 0; Retry < 100; Retry++) {
    gBS->Stall (10000);
    Status = SmscReadRegister (Device, SMSC_HW_CFG, &Value);
    if (EFI_ERROR (Status) || (Value & SMSC_HW_CFG_LRST) == 0) {
      break;
    }
  }
  if (EFI_ERROR (Status) || Retry == 100) {
    return EFI_ERROR (Status) ? Status : EFI_TIMEOUT;
  }

  Status = SmscReadRegister (Device, SMSC_ID_REV, &Device->DeviceIdRevision);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = SmscProgramMacAddress (Device, Device->Mode.CurrentAddress.Addr);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = SmscReadRegister (Device, SMSC_HW_CFG, &Value);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Value &= ~SMSC_HW_CFG_BIR;
  Value &= ~SMSC_HW_CFG_RXDOFF;
  Status = SmscWriteRegister (Device, SMSC_HW_CFG, Value);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (Device->BulkInMaxPacket >= SMSC_HIGH_SPEED_PACKET_SIZE) {
    Device->RxTransferSize = SMSC_HS_BURST_SIZE;
  } else {
    Device->RxTransferSize = SMSC_FS_BURST_SIZE;
  }
  BurstCapacity = (UINT32)(Device->RxTransferSize / Device->BulkInMaxPacket);
  Status = SmscWriteRegister (Device, SMSC_BURST_CAP, BurstCapacity);
  if (!EFI_ERROR (Status)) {
    Status = SmscWriteRegister (
               Device,
               SMSC_BULK_IN_DLY,
               SMSC_BULK_IN_DELAY_DEFAULT
               );
  }
  if (!EFI_ERROR (Status)) {
    Status = SmscReadRegister (Device, SMSC_HW_CFG, &Value);
  }
  if (!EFI_ERROR (Status)) {
    Value |= SMSC_HW_CFG_MEF | SMSC_HW_CFG_BCE;
    Value &= ~SMSC_HW_CFG_RXDOFF;
    Status = SmscWriteRegister (Device, SMSC_HW_CFG, Value);
  }
  if (!EFI_ERROR (Status)) {
    Status = SmscWriteRegister (Device, SMSC_INT_STS, MAX_UINT32);
  }
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = SmscReadRegister (Device, SMSC_LED_GPIO_CFG, &Value);
  if (!EFI_ERROR (Status)) {
    Value |= SMSC_LED_GPIO_SPEED | SMSC_LED_GPIO_LINK | SMSC_LED_GPIO_DUPLEX;
    Status = SmscWriteRegister (Device, SMSC_LED_GPIO_CFG, Value);
  }
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = SmscWriteRegister (Device, SMSC_FLOW, SMSC_FLOW_CONTROL_FULL);
  if (!EFI_ERROR (Status)) {
    Status = SmscWriteRegister (
               Device,
               SMSC_AFC_CFG,
               SMSC_AFC_CFG_DEFAULT | SMSC_AFC_CFG_FLOW_CONTROL
               );
  }
  if (!EFI_ERROR (Status)) {
    Status = SmscWriteRegister (Device, SMSC_VLAN1, 0x8100);
  }
  if (!EFI_ERROR (Status)) {
    Status = SmscWriteRegister (Device, SMSC_COE_CR, 0);
  }
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = SmscReadRegister (Device, SMSC_MAC_CR, &Value);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Device->MacControl = Value | SMSC_MAC_CR_TXEN | SMSC_MAC_CR_RXEN;
  Status = SmscProgramReceiveFilters (Device);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = SmscReadRegister (Device, SMSC_INT_EP_CTL, &Value);
  if (!EFI_ERROR (Status)) {
    Status = SmscWriteRegister (
               Device,
               SMSC_INT_EP_CTL,
               Value | SMSC_INT_EP_CTL_PHY_INT
               );
  }
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = SmscWritePhy (Device, SMSC_MII_BMCR, SMSC_MII_BMCR_RESET);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  for (Retry = 0; Retry < 100; Retry++) {
    gBS->Stall (10000);
    Status = SmscReadPhy (Device, SMSC_MII_BMCR, &PhyControl);
    if (EFI_ERROR (Status) || (PhyControl & SMSC_MII_BMCR_RESET) == 0) {
      break;
    }
  }
  if (EFI_ERROR (Status) || Retry == 100) {
    return EFI_ERROR (Status) ? Status : EFI_TIMEOUT;
  }

  Status = SmscWritePhy (
             Device,
             SMSC_MII_ADVERTISE,
             SMSC_MII_ADVERTISE_DEFAULT |
             SMSC_MII_ADVERTISE_PAUSE_CAP |
             SMSC_MII_ADVERTISE_PAUSE_ASYM
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Status = SmscReadPhy (Device, SMSC_MII_PHY_INT_SOURCE, &PhyInterruptSource);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Status = SmscWritePhy (Device, SMSC_MII_PHY_INT_MASK, SMSC_MII_PHY_INT_DEFAULT);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Status = SmscReadPhy (Device, SMSC_MII_BMCR, &PhyControl);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  PhyControl &= ~(SMSC_MII_BMCR_POWER_DOWN | SMSC_MII_BMCR_ISOLATE);
  PhyControl |= SMSC_MII_BMCR_AN_ENABLE | SMSC_MII_BMCR_AN_RESTART;
  Status = SmscWritePhy (Device, SMSC_MII_BMCR, PhyControl);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = SmscWriteRegister (Device, SMSC_TX_CFG, SMSC_TX_CFG_ON);
  if (!EFI_ERROR (Status)) {
    Device->RxLength = 0;
    Device->RxOffset = 0;
    Device->RxPendingLength = 0;
    Device->RecycledTxBuffer = NULL;
    SmscUpdateLink (Device);
  }

  return Status;
}

STATIC
EFI_STATUS
SmscConfigureEndpoints (
  IN SMSC95XX_DEVICE *Device
  )
{
  EFI_USB_INTERFACE_DESCRIPTOR Interface;
  EFI_USB_ENDPOINT_DESCRIPTOR  Endpoint;
  EFI_STATUS                   Status;
  UINT8                        Index;

  Status = Device->UsbIo->UsbGetInterfaceDescriptor (Device->UsbIo, &Interface);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  for (Index = 0; Index < Interface.NumEndpoints; Index++) {
    Status = Device->UsbIo->UsbGetEndpointDescriptor (Device->UsbIo, Index, &Endpoint);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    if ((Endpoint.Attributes & USB_ENDPOINT_TYPE_MASK) != USB_ENDPOINT_BULK) {
      continue;
    }

    if ((Endpoint.EndpointAddress & USB_ENDPOINT_DIR_IN) != 0) {
      Device->BulkInEndpoint = Endpoint.EndpointAddress;
      Device->BulkInMaxPacket = Endpoint.MaxPacketSize;
    } else {
      Device->BulkOutEndpoint = Endpoint.EndpointAddress;
      Device->BulkOutMaxPacket = Endpoint.MaxPacketSize;
    }
  }

  if (Device->BulkInEndpoint == 0 || Device->BulkInMaxPacket == 0 ||
      Device->BulkOutEndpoint == 0 || Device->BulkOutMaxPacket == 0) {
    return EFI_UNSUPPORTED;
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
SmscBulkReceive (
  IN SMSC95XX_DEVICE *Device
  )
{
  EFI_STATUS Status;
  UINT32     UsbStatus;
  UINTN      TransferLength;

  TransferLength = Device->RxTransferSize - Device->RxPendingLength;
  UsbStatus = 0;
  Status = Device->UsbIo->UsbBulkTransfer (
                            Device->UsbIo,
                            Device->BulkInEndpoint,
                            &Device->RxBuffer[Device->RxPendingLength],
                            &TransferLength,
                            SMSC_BULK_IN_TIMEOUT_MS,
                            &UsbStatus
                            );

  Device->RxPendingLength += TransferLength;
  if (Status == EFI_TIMEOUT) {
    return EFI_NOT_READY;
  }
  if (EFI_ERROR (Status) || UsbStatus != 0) {
    Device->RxPendingLength = 0;
    return EFI_DEVICE_ERROR;
  }
  if (Device->RxPendingLength == 0) {
    return EFI_NOT_READY;
  }

  Device->RxLength = Device->RxPendingLength;
  Device->RxOffset = 0;
  Device->RxPendingLength = 0;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
SmscValidateSnp (
  IN EFI_SIMPLE_NETWORK_PROTOCOL *Snp,
  OUT SMSC95XX_DEVICE            **Device
  )
{
  if (Snp == NULL || Snp->Mode == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  *Device = SMSC95XX_FROM_SNP (Snp);
  if ((*Device)->Signature != SMSC95XX_SIGNATURE) {
    return EFI_INVALID_PARAMETER;
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
SmscSnpStart (
  IN EFI_SIMPLE_NETWORK_PROTOCOL *Snp
  )
{
  SMSC95XX_DEVICE *Device;
  EFI_STATUS      Status;

  Status = SmscValidateSnp (Snp, &Device);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  if (Device->Mode.State != EfiSimpleNetworkStopped) {
    return EFI_ALREADY_STARTED;
  }

  Device->Mode.State = EfiSimpleNetworkStarted;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
SmscSnpStop (
  IN EFI_SIMPLE_NETWORK_PROTOCOL *Snp
  )
{
  SMSC95XX_DEVICE *Device;
  EFI_STATUS      Status;

  Status = SmscValidateSnp (Snp, &Device);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  if (Device->Mode.State == EfiSimpleNetworkStopped) {
    return EFI_NOT_STARTED;
  }
  if (Device->Mode.State == EfiSimpleNetworkInitialized) {
    return EFI_DEVICE_ERROR;
  }

  Device->Mode.State = EfiSimpleNetworkStopped;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
SmscSnpInitialize (
  IN EFI_SIMPLE_NETWORK_PROTOCOL *Snp,
  IN UINTN                       ExtraRxBufferSize,
  IN UINTN                       ExtraTxBufferSize
  )
{
  SMSC95XX_DEVICE *Device;
  EFI_STATUS      Status;

  (VOID)ExtraRxBufferSize;
  (VOID)ExtraTxBufferSize;
  Status = SmscValidateSnp (Snp, &Device);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  if (Device->Mode.State == EfiSimpleNetworkStopped) {
    return EFI_NOT_STARTED;
  }
  if (Device->Mode.State == EfiSimpleNetworkInitialized) {
    return EFI_SUCCESS;
  }

  Status = SmscInitializeHardware (Device);
  if (!EFI_ERROR (Status)) {
    Device->Mode.State = EfiSimpleNetworkInitialized;
  }

  return Status;
}

STATIC
EFI_STATUS
EFIAPI
SmscSnpReset (
  IN EFI_SIMPLE_NETWORK_PROTOCOL *Snp,
  IN BOOLEAN                     ExtendedVerification
  )
{
  SMSC95XX_DEVICE *Device;
  EFI_STATUS      Status;

  (VOID)ExtendedVerification;
  Status = SmscValidateSnp (Snp, &Device);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  if (Device->Mode.State != EfiSimpleNetworkInitialized) {
    return EFI_NOT_STARTED;
  }

  return SmscInitializeHardware (Device);
}

STATIC
EFI_STATUS
EFIAPI
SmscSnpShutdown (
  IN EFI_SIMPLE_NETWORK_PROTOCOL *Snp
  )
{
  SMSC95XX_DEVICE *Device;
  EFI_STATUS      Status;

  Status = SmscValidateSnp (Snp, &Device);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  if (Device->Mode.State != EfiSimpleNetworkInitialized) {
    return EFI_NOT_STARTED;
  }

  SmscWriteRegister (Device, SMSC_TX_CFG, 0);
  Device->MacControl &= ~(SMSC_MAC_CR_TXEN | SMSC_MAC_CR_RXEN);
  SmscWriteRegister (Device, SMSC_MAC_CR, Device->MacControl);
  Device->Mode.MediaPresent = FALSE;
  Device->Mode.State = EfiSimpleNetworkStarted;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
SmscSnpReceiveFilters (
  IN EFI_SIMPLE_NETWORK_PROTOCOL *Snp,
  IN UINT32                      Enable,
  IN UINT32                      Disable,
  IN BOOLEAN                     ResetMCastFilter,
  IN UINTN                       MCastFilterCount,
  IN EFI_MAC_ADDRESS             *MCastFilter OPTIONAL
  )
{
  SMSC95XX_DEVICE        *Device;
  EFI_SIMPLE_NETWORK_MODE *Mode;
  EFI_STATUS             Status;
  UINT32                 NewSetting;

  Status = SmscValidateSnp (Snp, &Device);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Mode = &Device->Mode;
  if (Mode->State != EfiSimpleNetworkInitialized) {
    return EFI_NOT_STARTED;
  }
  if (((Enable | Disable) & ~Mode->ReceiveFilterMask) != 0 ||
      MCastFilterCount > Mode->MaxMCastFilterCount ||
      (MCastFilterCount != 0 && MCastFilter == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  NewSetting = (Mode->ReceiveFilterSetting | Enable) & ~Disable;
  if (ResetMCastFilter) {
    Mode->MCastFilterCount = 0;
    ZeroMem (Mode->MCastFilter, sizeof (Mode->MCastFilter));
  } else if (MCastFilterCount != 0) {
    CopyMem (
      Mode->MCastFilter,
      MCastFilter,
      MCastFilterCount * sizeof (EFI_MAC_ADDRESS)
      );
    Mode->MCastFilterCount = (UINT32)MCastFilterCount;
  }

  Mode->ReceiveFilterSetting = NewSetting;
  return SmscProgramReceiveFilters (Device);
}

STATIC
EFI_STATUS
EFIAPI
SmscSnpStationAddress (
  IN EFI_SIMPLE_NETWORK_PROTOCOL *Snp,
  IN BOOLEAN                     Reset,
  IN EFI_MAC_ADDRESS             *New OPTIONAL
  )
{
  SMSC95XX_DEVICE *Device;
  EFI_MAC_ADDRESS *Address;
  EFI_STATUS      Status;

  Status = SmscValidateSnp (Snp, &Device);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  if (Device->Mode.State != EfiSimpleNetworkInitialized) {
    return EFI_NOT_STARTED;
  }
  if (!Reset && (New == NULL || !SmscIsValidMac (New->Addr))) {
    return EFI_INVALID_PARAMETER;
  }

  Address = Reset ? &Device->Mode.PermanentAddress : New;
  Status = SmscProgramMacAddress (Device, Address->Addr);
  if (!EFI_ERROR (Status)) {
    CopyMem (&Device->Mode.CurrentAddress, Address, sizeof (*Address));
  }

  return Status;
}

STATIC
EFI_STATUS
EFIAPI
SmscSnpStatistics (
  IN     EFI_SIMPLE_NETWORK_PROTOCOL *Snp,
  IN     BOOLEAN                     Reset,
  IN OUT UINTN                       *StatisticsSize OPTIONAL,
  OUT    EFI_NETWORK_STATISTICS      *StatisticsTable OPTIONAL
  )
{
  SMSC95XX_DEVICE *Device;
  EFI_STATUS      Status;

  (VOID)Reset;
  Status = SmscValidateSnp (Snp, &Device);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  if (Device->Mode.State != EfiSimpleNetworkInitialized) {
    return EFI_NOT_STARTED;
  }
  if (StatisticsSize == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  if (StatisticsTable == NULL || *StatisticsSize < sizeof (*StatisticsTable)) {
    *StatisticsSize = sizeof (*StatisticsTable);
    return EFI_BUFFER_TOO_SMALL;
  }

  ZeroMem (StatisticsTable, sizeof (*StatisticsTable));
  *StatisticsSize = sizeof (*StatisticsTable);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
SmscSnpMCastIpToMac (
  IN  EFI_SIMPLE_NETWORK_PROTOCOL *Snp,
  IN  BOOLEAN                     IsIpv6,
  IN  EFI_IP_ADDRESS              *Ip,
  OUT EFI_MAC_ADDRESS             *Mac
  )
{
  SMSC95XX_DEVICE *Device;
  EFI_STATUS      Status;

  Status = SmscValidateSnp (Snp, &Device);
  if (EFI_ERROR (Status) || Ip == NULL || Mac == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  if (Device->Mode.State == EfiSimpleNetworkStopped) {
    return EFI_NOT_STARTED;
  }

  ZeroMem (Mac, sizeof (*Mac));
  if (IsIpv6) {
    Mac->Addr[0] = 0x33;
    Mac->Addr[1] = 0x33;
    CopyMem (&Mac->Addr[2], &Ip->v6.Addr[12], 4);
  } else {
    Mac->Addr[0] = 0x01;
    Mac->Addr[1] = 0x00;
    Mac->Addr[2] = 0x5E;
    Mac->Addr[3] = Ip->v4.Addr[1] & 0x7F;
    Mac->Addr[4] = Ip->v4.Addr[2];
    Mac->Addr[5] = Ip->v4.Addr[3];
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
SmscSnpNvData (
  IN     EFI_SIMPLE_NETWORK_PROTOCOL *Snp,
  IN     BOOLEAN                     ReadWrite,
  IN     UINTN                       Offset,
  IN     UINTN                       BufferSize,
  IN OUT VOID                        *Buffer
  )
{
  SMSC95XX_DEVICE *Device;
  EFI_STATUS      Status;

  (VOID)ReadWrite;
  (VOID)Offset;
  (VOID)BufferSize;
  (VOID)Buffer;
  Status = SmscValidateSnp (Snp, &Device);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  if (Device->Mode.State != EfiSimpleNetworkInitialized) {
    return EFI_NOT_STARTED;
  }

  return EFI_UNSUPPORTED;
}

STATIC
EFI_STATUS
EFIAPI
SmscSnpGetStatus (
  IN  EFI_SIMPLE_NETWORK_PROTOCOL *Snp,
  OUT UINT32                      *InterruptStatus OPTIONAL,
  OUT VOID                        **TxBuffer OPTIONAL
  )
{
  SMSC95XX_DEVICE *Device;
  EFI_STATUS      Status;
  UINT32          DeviceInterruptStatus;

  Status = SmscValidateSnp (Snp, &Device);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  if (Device->Mode.State != EfiSimpleNetworkInitialized) {
    return EFI_NOT_STARTED;
  }
  if (InterruptStatus == NULL && TxBuffer == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (InterruptStatus != NULL) {
    SmscUpdateLink (Device);
    *InterruptStatus = 0;

    Status = SmscReadRegister (Device, SMSC_INT_STS, &DeviceInterruptStatus);
    if (!EFI_ERROR (Status) &&
        (DeviceInterruptStatus & SMSC_INT_STS_RXDF) != 0) {
      SmscWriteRegister (Device, SMSC_INT_STS, SMSC_INT_STS_RXDF);
    }
  }
  if (TxBuffer != NULL) {
    *TxBuffer = Device->RecycledTxBuffer;
    Device->RecycledTxBuffer = NULL;
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
SmscSnpTransmit (
  IN EFI_SIMPLE_NETWORK_PROTOCOL *Snp,
  IN UINTN                       HeaderSize,
  IN UINTN                       BufferSize,
  IN VOID                        *Buffer,
  IN EFI_MAC_ADDRESS             *SrcAddr OPTIONAL,
  IN EFI_MAC_ADDRESS             *DestAddr OPTIONAL,
  IN UINT16                      *Protocol OPTIONAL
  )
{
  SMSC95XX_DEVICE *Device;
  EFI_STATUS      Status;
  UINT8           *Frame;
  UINTN           FrameLength;
  UINTN           TransferLength;
  UINT32          UsbStatus;

  Status = SmscValidateSnp (Snp, &Device);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  if (Device->Mode.State != EfiSimpleNetworkInitialized) {
    return EFI_NOT_STARTED;
  }
  if (Buffer == NULL || BufferSize < SMSC_ETHERNET_HEADER_SIZE ||
      BufferSize > SMSC_MAX_FRAME_SIZE ||
      (HeaderSize != 0 && HeaderSize != SMSC_ETHERNET_HEADER_SIZE) ||
      (HeaderSize != 0 && (DestAddr == NULL || Protocol == NULL))) {
    return EFI_INVALID_PARAMETER;
  }
  if (Device->RecycledTxBuffer != NULL) {
    return EFI_NOT_READY;
  }

  if (!Device->Mode.MediaPresent) {
    return EFI_NOT_READY;
  }

  Frame = &Device->TxBuffer[8];
  CopyMem (Frame, Buffer, BufferSize);
  if (HeaderSize != 0) {
    CopyMem (&Frame[0], DestAddr->Addr, NET_ETHER_ADDR_LEN);
    if (SrcAddr != NULL) {
      CopyMem (&Frame[6], SrcAddr->Addr, NET_ETHER_ADDR_LEN);
    } else {
      CopyMem (&Frame[6], Device->Mode.CurrentAddress.Addr, NET_ETHER_ADDR_LEN);
    }
    Frame[12] = (UINT8)(*Protocol >> 8);
    Frame[13] = (UINT8)*Protocol;
  }

  FrameLength = BufferSize;
  if (FrameLength < SMSC_MIN_FRAME_SIZE) {
    ZeroMem (&Frame[FrameLength], SMSC_MIN_FRAME_SIZE - FrameLength);
    FrameLength = SMSC_MIN_FRAME_SIZE;
  }

  SmscWriteLe32 (
    &Device->TxBuffer[0],
    SMSC_TX_CMD_A_FIRST | SMSC_TX_CMD_A_LAST |
    ((UINT32)FrameLength & SMSC_TX_LENGTH_MASK)
    );
  SmscWriteLe32 (
    &Device->TxBuffer[4],
    (UINT32)FrameLength & SMSC_TX_LENGTH_MASK
    );
  TransferLength = 8 + FrameLength;
  UsbStatus = 0;
  Status = Device->UsbIo->UsbBulkTransfer (
                            Device->UsbIo,
                            Device->BulkOutEndpoint,
                            Device->TxBuffer,
                            &TransferLength,
                            SMSC_BULK_OUT_TIMEOUT_MS,
                            &UsbStatus
                            );
  if (EFI_ERROR (Status) || UsbStatus != 0) {
    return EFI_DEVICE_ERROR;
  }

  Device->RecycledTxBuffer = Buffer;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
SmscSnpReceive (
  IN     EFI_SIMPLE_NETWORK_PROTOCOL *Snp,
  OUT    UINTN                       *HeaderSize OPTIONAL,
  IN OUT UINTN                       *BufferSize,
  OUT    VOID                        *Buffer,
  OUT    EFI_MAC_ADDRESS             *SrcAddr OPTIONAL,
  OUT    EFI_MAC_ADDRESS             *DestAddr OPTIONAL,
  OUT    UINT16                      *Protocol OPTIONAL
  )
{
  SMSC95XX_DEVICE *Device;
  EFI_STATUS      Status;

  Status = SmscValidateSnp (Snp, &Device);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  if (Device->Mode.State != EfiSimpleNetworkInitialized) {
    return EFI_NOT_STARTED;
  }
  if (BufferSize == NULL || Buffer == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  for (;;) {
    UINT32 ReceiveStatus;
    UINTN  FrameWithFcsLength;
    UINTN  FrameLength;
    UINTN  RecordLength;
    UINT8  *Frame;

    if (Device->RxOffset + 4 > Device->RxLength) {
      Status = SmscBulkReceive (Device);
      if (EFI_ERROR (Status)) {
        return Status;
      }
    }

    ReceiveStatus = SmscReadLe32 (&Device->RxBuffer[Device->RxOffset]);
    FrameWithFcsLength = (ReceiveStatus & SMSC_RX_STATUS_LENGTH) >> 16;
    RecordLength = 4 + FrameWithFcsLength;
    if (Device->RxLength - Device->RxOffset > RecordLength) {
      RecordLength = 4 + ALIGN_VALUE (FrameWithFcsLength, 4);
    }
    if (FrameWithFcsLength < SMSC_ETHERNET_HEADER_SIZE + 4 ||
        FrameWithFcsLength > SMSC_MAX_FRAME_SIZE + 4 ||
        Device->RxOffset + RecordLength > Device->RxLength) {
      Device->RxLength = 0;
      Device->RxOffset = 0;
      return EFI_DEVICE_ERROR;
    }

    if ((ReceiveStatus & SMSC_RX_STATUS_ERROR) != 0) {
      Device->RxOffset += RecordLength;
      continue;
    }

    Frame = &Device->RxBuffer[Device->RxOffset + 4];
    FrameLength = FrameWithFcsLength - 4;
    if (*BufferSize < FrameLength) {
      *BufferSize = FrameLength;
      return EFI_BUFFER_TOO_SMALL;
    }

    CopyMem (Buffer, Frame, FrameLength);
    *BufferSize = FrameLength;
    if (HeaderSize != NULL) {
      *HeaderSize = SMSC_ETHERNET_HEADER_SIZE;
    }
    if (DestAddr != NULL) {
      CopyMem (DestAddr->Addr, &Frame[0], NET_ETHER_ADDR_LEN);
    }
    if (SrcAddr != NULL) {
      CopyMem (SrcAddr->Addr, &Frame[6], NET_ETHER_ADDR_LEN);
    }
    if (Protocol != NULL) {
      *Protocol = (UINT16)(((UINT16)Frame[12] << 8) | Frame[13]);
    }
    Device->RxOffset += RecordLength;
    return EFI_SUCCESS;
  }
}

STATIC
VOID
SmscInitializeSnp (
  IN SMSC95XX_DEVICE *Device
  )
{
  EFI_SIMPLE_NETWORK_MODE *Mode;

  Device->Snp.Revision = EFI_SIMPLE_NETWORK_PROTOCOL_REVISION;
  Device->Snp.Start = SmscSnpStart;
  Device->Snp.Stop = SmscSnpStop;
  Device->Snp.Initialize = SmscSnpInitialize;
  Device->Snp.Reset = SmscSnpReset;
  Device->Snp.Shutdown = SmscSnpShutdown;
  Device->Snp.ReceiveFilters = SmscSnpReceiveFilters;
  Device->Snp.StationAddress = SmscSnpStationAddress;
  Device->Snp.Statistics = SmscSnpStatistics;
  Device->Snp.MCastIpToMac = SmscSnpMCastIpToMac;
  Device->Snp.NvData = SmscSnpNvData;
  Device->Snp.GetStatus = SmscSnpGetStatus;
  Device->Snp.Transmit = SmscSnpTransmit;
  Device->Snp.Receive = SmscSnpReceive;
  Device->Snp.WaitForPacket = NULL;
  Device->Snp.Mode = &Device->Mode;

  Mode = &Device->Mode;
  Mode->State = EfiSimpleNetworkStopped;
  Mode->HwAddressSize = NET_ETHER_ADDR_LEN;
  Mode->MediaHeaderSize = SMSC_ETHERNET_HEADER_SIZE;
  Mode->MaxPacketSize = SMSC_ETHERNET_MTU;
  Mode->ReceiveFilterMask = EFI_SIMPLE_NETWORK_RECEIVE_UNICAST |
                            EFI_SIMPLE_NETWORK_RECEIVE_MULTICAST |
                            EFI_SIMPLE_NETWORK_RECEIVE_BROADCAST |
                            EFI_SIMPLE_NETWORK_RECEIVE_PROMISCUOUS |
                            EFI_SIMPLE_NETWORK_RECEIVE_PROMISCUOUS_MULTICAST;
  Mode->ReceiveFilterSetting = EFI_SIMPLE_NETWORK_RECEIVE_UNICAST |
                               EFI_SIMPLE_NETWORK_RECEIVE_BROADCAST;
  Mode->MaxMCastFilterCount = SMSC_MCAST_FILTER_COUNT;
  Mode->IfType = NET_IFTYPE_ETHERNET;
  Mode->MacAddressChangeable = TRUE;
  Mode->MultipleTxSupported = FALSE;
  Mode->MediaPresentSupported = TRUE;
  SetMem (&Mode->BroadcastAddress, NET_ETHER_ADDR_LEN, 0xFF);
}

STATIC
EFI_STATUS
EFIAPI
Smsc95xxDriverSupported (
  IN EFI_DRIVER_BINDING_PROTOCOL *This,
  IN EFI_HANDLE                  Controller,
  IN EFI_DEVICE_PATH_PROTOCOL    *RemainingDevicePath OPTIONAL
  )
{
  EFI_USB_DEVICE_DESCRIPTOR Descriptor;
  EFI_USB_IO_PROTOCOL       *UsbIo;
  EFI_STATUS                Status;

  (VOID)RemainingDevicePath;
  Status = gBS->OpenProtocol (
                  Controller,
                  &gEfiUsbIoProtocolGuid,
                  (VOID **)&UsbIo,
                  This->DriverBindingHandle,
                  Controller,
                  EFI_OPEN_PROTOCOL_BY_DRIVER
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = UsbIo->UsbGetDeviceDescriptor (UsbIo, &Descriptor);
  gBS->CloseProtocol (
         Controller,
         &gEfiUsbIoProtocolGuid,
         This->DriverBindingHandle,
         Controller
         );
  if (EFI_ERROR (Status) || Descriptor.IdVendor != SMSC95XX_VENDOR_ID ||
      !SmscIsSupportedProduct (Descriptor.IdProduct)) {
    return EFI_UNSUPPORTED;
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
Smsc95xxDriverStart (
  IN EFI_DRIVER_BINDING_PROTOCOL *This,
  IN EFI_HANDLE                  Controller,
  IN EFI_DEVICE_PATH_PROTOCOL    *RemainingDevicePath OPTIONAL
  )
{
  SMSC95XX_DEVICE          *Device;
  EFI_DEVICE_PATH_PROTOCOL *ParentDevicePath;
  MAC_ADDR_DEVICE_PATH     MacNode;
  EFI_STATUS               Status;

  (VOID)RemainingDevicePath;
  Device = AllocateZeroPool (sizeof (*Device));
  if (Device == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Device->Signature = SMSC95XX_SIGNATURE;
  Device->ParentHandle = Controller;
  Status = gBS->OpenProtocol (
                  Controller,
                  &gEfiUsbIoProtocolGuid,
                  (VOID **)&Device->UsbIo,
                  This->DriverBindingHandle,
                  Controller,
                  EFI_OPEN_PROTOCOL_BY_DRIVER
                  );
  if (EFI_ERROR (Status)) {
    goto Error;
  }

  Status = SmscConfigureEndpoints (Device);
  if (EFI_ERROR (Status)) {
    goto Error;
  }
  Status = SmscReadRegister (Device, SMSC_ID_REV, &Device->DeviceIdRevision);
  if (EFI_ERROR (Status)) {
    goto Error;
  }

  SmscInitializeSnp (Device);
  Status = SmscGetMacAddress (Device, Device->Mode.PermanentAddress.Addr);
  if (EFI_ERROR (Status)) {
    goto Error;
  }
  CopyMem (
    &Device->Mode.CurrentAddress,
    &Device->Mode.PermanentAddress,
    sizeof (EFI_MAC_ADDRESS)
    );

  Status = gBS->HandleProtocol (
                  Controller,
                  &gEfiDevicePathProtocolGuid,
                  (VOID **)&ParentDevicePath
                  );
  if (EFI_ERROR (Status)) {
    goto Error;
  }

  ZeroMem (&MacNode, sizeof (MacNode));
  MacNode.Header.Type = MESSAGING_DEVICE_PATH;
  MacNode.Header.SubType = MSG_MAC_ADDR_DP;
  SetDevicePathNodeLength (&MacNode.Header, sizeof (MacNode));
  CopyMem (&MacNode.MacAddress, &Device->Mode.CurrentAddress, NET_ETHER_ADDR_LEN);
  MacNode.IfType = Device->Mode.IfType;
  Device->DevicePath = AppendDevicePathNode (
                         ParentDevicePath,
                         (EFI_DEVICE_PATH_PROTOCOL *)&MacNode
                         );
  if (Device->DevicePath == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    goto Error;
  }

  Status = gBS->InstallMultipleProtocolInterfaces (
                  &Device->ChildHandle,
                  &gEfiSimpleNetworkProtocolGuid,
                  &Device->Snp,
                  &gEfiDevicePathProtocolGuid,
                  Device->DevicePath,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    goto Error;
  }
  Status = gBS->OpenProtocol (
                  Controller,
                  &gEfiUsbIoProtocolGuid,
                  (VOID **)&Device->UsbIo,
                  This->DriverBindingHandle,
                  Device->ChildHandle,
                  EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER
                  );
  if (EFI_ERROR (Status)) {
    gBS->UninstallMultipleProtocolInterfaces (
           Device->ChildHandle,
           &gEfiSimpleNetworkProtocolGuid,
           &Device->Snp,
           &gEfiDevicePathProtocolGuid,
           Device->DevicePath,
           NULL
           );
    Device->ChildHandle = NULL;
    goto Error;
  }

  return EFI_SUCCESS;

Error:
  if (Device->DevicePath != NULL) {
    FreePool (Device->DevicePath);
  }
  if (Device->UsbIo != NULL) {
    gBS->CloseProtocol (
           Controller,
           &gEfiUsbIoProtocolGuid,
           This->DriverBindingHandle,
           Controller
           );
  }
  FreePool (Device);
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
Smsc95xxDriverStop (
  IN EFI_DRIVER_BINDING_PROTOCOL *This,
  IN EFI_HANDLE                  Controller,
  IN UINTN                       NumberOfChildren,
  IN EFI_HANDLE                  *ChildHandleBuffer OPTIONAL
  )
{
  EFI_SIMPLE_NETWORK_PROTOCOL *Snp;
  SMSC95XX_DEVICE            *Device;
  EFI_STATUS                 Status;
  BOOLEAN                    AllChildrenStopped;
  UINTN                      Index;

  if (NumberOfChildren == 0) {
    gBS->CloseProtocol (
           Controller,
           &gEfiUsbIoProtocolGuid,
           This->DriverBindingHandle,
           Controller
           );
    return EFI_SUCCESS;
  }

  AllChildrenStopped = TRUE;
  for (Index = 0; Index < NumberOfChildren; Index++) {
    Status = gBS->OpenProtocol (
                    ChildHandleBuffer[Index],
                    &gEfiSimpleNetworkProtocolGuid,
                    (VOID **)&Snp,
                    This->DriverBindingHandle,
                    Controller,
                    EFI_OPEN_PROTOCOL_GET_PROTOCOL
                    );
    if (EFI_ERROR (Status)) {
      AllChildrenStopped = FALSE;
      continue;
    }

    Device = SMSC95XX_FROM_SNP (Snp);
    gBS->CloseProtocol (
           Controller,
           &gEfiUsbIoProtocolGuid,
           This->DriverBindingHandle,
           ChildHandleBuffer[Index]
           );
    Status = gBS->UninstallMultipleProtocolInterfaces (
                    ChildHandleBuffer[Index],
                    &gEfiSimpleNetworkProtocolGuid,
                    &Device->Snp,
                    &gEfiDevicePathProtocolGuid,
                    Device->DevicePath,
                    NULL
                    );
    if (EFI_ERROR (Status)) {
      AllChildrenStopped = FALSE;
      gBS->OpenProtocol (
             Controller,
             &gEfiUsbIoProtocolGuid,
             (VOID **)&Device->UsbIo,
             This->DriverBindingHandle,
             ChildHandleBuffer[Index],
             EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER
             );
      continue;
    }

    FreePool (Device->DevicePath);
    FreePool (Device);
  }

  return AllChildrenStopped ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}

EFI_STATUS
EFIAPI
Smsc95xxEntryPoint (
  IN EFI_HANDLE       ImageHandle,
  IN EFI_SYSTEM_TABLE *SystemTable
  )
{
  return EfiLibInstallDriverBinding (
           ImageHandle,
           SystemTable,
           &mSmsc95xxDriverBinding,
           ImageHandle
           );
}

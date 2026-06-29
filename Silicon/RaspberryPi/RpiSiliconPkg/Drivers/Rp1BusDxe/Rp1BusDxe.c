/** @file
 *
 *  Copyright (c) 2023-2024, Mario Bălănică <mariobalanica02@gmail.com>
 *
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 **/

#include <Uefi.h>
#include <IndustryStandard/Pci.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/NonDiscoverableDeviceRegistrationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Rp1.h>

#include "Rp1BusDxe.h"

STATIC
VOID
EFIAPI
Rp1BusRegisterDwc3Controllers (
  IN RP1_BUS_DATA  *Rp1Data
  )
{
  EFI_STATUS            Status;
  UINTN                 Index;
  EFI_PHYSICAL_ADDRESS  FullBase;
  EFI_HANDLE            DeviceHandle;
  RP1_BUS_PROTOCOL      *Rp1Bus;

  EFI_PHYSICAL_ADDRESS  Dwc3Addresses[] = {
    RP1_USBHOST0_BASE, RP1_USBHOST1_BASE
  };

  for (Index = 0; Index < ARRAY_SIZE (Dwc3Addresses); Index++) {
    DeviceHandle = NULL;
    FullBase     = Rp1Data->PeripheralBase + Dwc3Addresses[Index];

    Status = RegisterNonDiscoverableMmioDevice (
               NonDiscoverableDeviceTypeXhci,
               NonDiscoverableDeviceDmaTypeNonCoherent,
               NULL,
               &DeviceHandle,
               1,
               FullBase,
               RP1_USBHOST_SIZE
               );
    if (EFI_ERROR (Status)) {
      DEBUG ((
        DEBUG_ERROR,
        "RP1: Failed to register DWC3 controller at 0x%lx. Status=%r\n",
        FullBase,
        Status
        ));
      continue;
    }

    Status = gBS->OpenProtocol (
                    Rp1Data->ControllerHandle,
                    &gRp1BusProtocolGuid,
                    (VOID **)&Rp1Bus,
                    Rp1Data->DriverBinding->DriverBindingHandle,
                    DeviceHandle,
                    EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER
                    );
    if (EFI_ERROR (Status)) {
      DEBUG ((
        DEBUG_ERROR,
        "RP1: Failed to open DWC3 by controller. Status=%r\n",
        Status
        ));
      continue;
    }
  }
}

STATIC
VOID
EFIAPI
Rp1BusRegisterDevices (
  IN RP1_BUS_DATA  *Rp1Data
  )
{
  Rp1BusRegisterDwc3Controllers (Rp1Data);
}

STATIC
VOID
EFIAPI
Rp1BusEnableInterrupts (
  IN RP1_BUS_DATA  *Rp1Data
  )
{
  MmioWrite32 (
    Rp1Data->PeripheralBase + RP1_PCIE_REG_SET + RP1_PCIE_MSIX_CFG (RP1_INT_USBHOST0_0),
    RP1_PCIE_MSIX_CFG_ENABLE
    );
  MmioWrite32 (
    Rp1Data->PeripheralBase + RP1_PCIE_REG_SET + RP1_PCIE_MSIX_CFG (RP1_INT_USBHOST1_0),
    RP1_PCIE_MSIX_CFG_ENABLE
    );
  MmioWrite32 (
    Rp1Data->PeripheralBase + RP1_PCIE_REG_SET + RP1_PCIE_MSIX_CFG (RP1_INT_ETH),
    RP1_PCIE_MSIX_CFG_ENABLE
    );
}

//
// RP1 Ethernet clock and PHY setup.
//
#define RP1_CLK_ETH_CTRL          (RP1_CLOCKS_MAIN_BASE + 0x064)
#define RP1_CLK_ETH_TSU_CTRL      (RP1_CLOCKS_MAIN_BASE + 0x134)
#define RP1_CLK_CTRL_ENABLE       BIT11

// The active-low PHY reset is RP1 GPIO32 (bank 1, pin 4).
#define RP1_ETH_PHY_RESET_PIN     4
#define RP1_ETH_PHY_RESET_BIT     (1U << RP1_ETH_PHY_RESET_PIN)

#define RP1_RIO_OUT               0x0000
#define RP1_RIO_OE                0x0004
#define RP1_RIO_SET               0x2000
#define RP1_RIO_CLR               0x3000

#define RP1_GPIO_CTRL_REG         (0x04 + (RP1_ETH_PHY_RESET_PIN * 8))
#define RP1_PADS_CTRL_REG         (0x04 + (RP1_ETH_PHY_RESET_PIN * 4))

#define RP1_GPIO_FUNCSEL_MASK     0x0000001f
#define RP1_GPIO_OUTOVER_MASK     0x00003000
#define RP1_GPIO_OEOVER_MASK      0x0000c000
#define RP1_GPIO_INOVER_MASK      0x00030000
#define RP1_GPIO_FSEL_GPIO        0x05

#define RP1_PAD_PULL_MASK         0x0000000c
#define RP1_PAD_IN_ENABLE         0x00000040
#define RP1_PAD_OUT_DISABLE       0x00000080

STATIC
VOID
EFIAPI
Rp1BusEthernetBringUp (
  IN RP1_BUS_DATA  *Rp1Data
  )
{
  EFI_PHYSICAL_ADDRESS  Base;
  UINT32                Val;
  UINT32                Ctrl;
  UINT32                Pad;

  Base = Rp1Data->PeripheralBase;

  Val = MmioRead32 (Base + RP1_CLK_ETH_CTRL);
  if ((Val & RP1_CLK_CTRL_ENABLE) == 0) {
    MmioWrite32 (Base + RP1_CLK_ETH_CTRL, Val | RP1_CLK_CTRL_ENABLE);
  }
  Val = MmioRead32 (Base + RP1_CLK_ETH_TSU_CTRL);
  if ((Val & RP1_CLK_CTRL_ENABLE) == 0) {
    MmioWrite32 (Base + RP1_CLK_ETH_TSU_CTRL, Val | RP1_CLK_CTRL_ENABLE);
  }
  gBS->Stall (100);

  // Assert reset before enabling the output driver.
  MmioWrite32 (
    Base + RP1_SYS_RIO1_BASE + RP1_RIO_OUT + RP1_RIO_CLR,
    RP1_ETH_PHY_RESET_BIT
    );
  MmioWrite32 (
    Base + RP1_SYS_RIO1_BASE + RP1_RIO_OE + RP1_RIO_SET,
    RP1_ETH_PHY_RESET_BIT
    );

  Pad  = MmioRead32 (Base + RP1_PADS_BANK1_BASE + RP1_PADS_CTRL_REG);
  Pad &= ~(RP1_PAD_PULL_MASK | RP1_PAD_OUT_DISABLE);
  Pad |= RP1_PAD_IN_ENABLE;
  MmioWrite32 (Base + RP1_PADS_BANK1_BASE + RP1_PADS_CTRL_REG, Pad);

  Ctrl  = MmioRead32 (Base + RP1_IO_BANK1_BASE + RP1_GPIO_CTRL_REG);
  Ctrl &= ~(RP1_GPIO_FUNCSEL_MASK | RP1_GPIO_OUTOVER_MASK |
            RP1_GPIO_OEOVER_MASK | RP1_GPIO_INOVER_MASK);
  Ctrl |= RP1_GPIO_FSEL_GPIO;
  MmioWrite32 (Base + RP1_IO_BANK1_BASE + RP1_GPIO_CTRL_REG, Ctrl);

  gBS->Stall (5000);
  MmioWrite32 (
    Base + RP1_SYS_RIO1_BASE + RP1_RIO_OUT + RP1_RIO_SET,
    RP1_ETH_PHY_RESET_BIT
    );
  gBS->Stall (20000);
}

//
// Enable the RP1 USB host clocks and configure both DWC3 cores.
//
#define RP1_CLK_USBH0_MICROFRAME_CTRL  (RP1_CLOCKS_MAIN_BASE + 0x0F4)
#define RP1_CLK_USBH1_MICROFRAME_CTRL  (RP1_CLOCKS_MAIN_BASE + 0x104)
#define RP1_CLK_USBH0_SUSPEND_CTRL     (RP1_CLOCKS_MAIN_BASE + 0x114)
#define RP1_CLK_USBH1_SUSPEND_CTRL     (RP1_CLOCKS_MAIN_BASE + 0x124)

#define DWC3_GSNPSID                   0xC120
#define DWC3_GCTL                      0xC110
#define DWC3_GCTL_PRTCAPDIR_MASK       (BIT13 | BIT12)
#define DWC3_GCTL_PRTCAP_HOST          BIT12
#define DWC3_GUSB2PHYCFG0              0xC200
#define DWC3_GUSB2PHYCFG_SUSPHY        BIT6
#define DWC3_GUSB2PHYCFG_ENBLSLPM      BIT8
#define DWC3_GUSB3PIPECTL0             0xC2C0
#define DWC3_GUSB3PIPECTL_SUSPHY       BIT17
#define DWC3_GUSB3PIPECTL_UX_EXIT_PX   BIT27
#define DWC3_GFLADJ                    0xC630
#define DWC3_GFLADJ_30MHZ_SDBND_SEL    BIT7
#define DWC3_GFLADJ_30MHZ_MASK         0x3F
#define DWC3_GFLADJ_30MHZ_DEFAULT      0x20

STATIC
VOID
EFIAPI
Rp1BusUsbHostBringUp (
  IN RP1_BUS_DATA  *Rp1Data
  )
{
  STATIC CONST EFI_PHYSICAL_ADDRESS Dwc3Offsets[] = {
    RP1_USBHOST0_BASE,
    RP1_USBHOST1_BASE
  };
  EFI_PHYSICAL_ADDRESS  Base;
  EFI_PHYSICAL_ADDRESS  DwcBase;
  UINT32                Reg;
  UINTN                 Index;

  Base = Rp1Data->PeripheralBase;

  Reg = MmioRead32 (Base + RP1_CLK_USBH0_MICROFRAME_CTRL);
  if ((Reg & RP1_CLK_CTRL_ENABLE) == 0) {
    MmioWrite32 (Base + RP1_CLK_USBH0_MICROFRAME_CTRL, Reg | RP1_CLK_CTRL_ENABLE);
  }
  Reg = MmioRead32 (Base + RP1_CLK_USBH0_SUSPEND_CTRL);
  if ((Reg & RP1_CLK_CTRL_ENABLE) == 0) {
    MmioWrite32 (Base + RP1_CLK_USBH0_SUSPEND_CTRL, Reg | RP1_CLK_CTRL_ENABLE);
  }
  Reg = MmioRead32 (Base + RP1_CLK_USBH1_MICROFRAME_CTRL);
  if ((Reg & RP1_CLK_CTRL_ENABLE) == 0) {
    MmioWrite32 (Base + RP1_CLK_USBH1_MICROFRAME_CTRL, Reg | RP1_CLK_CTRL_ENABLE);
  }
  Reg = MmioRead32 (Base + RP1_CLK_USBH1_SUSPEND_CTRL);
  if ((Reg & RP1_CLK_CTRL_ENABLE) == 0) {
    MmioWrite32 (Base + RP1_CLK_USBH1_SUSPEND_CTRL, Reg | RP1_CLK_CTRL_ENABLE);
  }
  gBS->Stall (100);

  for (Index = 0; Index < ARRAY_SIZE (Dwc3Offsets); Index++) {
    DwcBase = Base + Dwc3Offsets[Index];
    Reg     = MmioRead32 (DwcBase + DWC3_GSNPSID);
    if ((Reg == 0) || (Reg == MAX_UINT32)) {
      continue;
    }

    Reg = MmioRead32 (DwcBase + DWC3_GCTL);
    if ((Reg & DWC3_GCTL_PRTCAPDIR_MASK) != DWC3_GCTL_PRTCAP_HOST) {
      Reg &= ~DWC3_GCTL_PRTCAPDIR_MASK;
      Reg |= DWC3_GCTL_PRTCAP_HOST;
      MmioWrite32 (DwcBase + DWC3_GCTL, Reg);
    }

    Reg  = MmioRead32 (DwcBase + DWC3_GUSB2PHYCFG0);
    Reg &= ~(DWC3_GUSB2PHYCFG_SUSPHY | DWC3_GUSB2PHYCFG_ENBLSLPM);
    MmioWrite32 (DwcBase + DWC3_GUSB2PHYCFG0, Reg);

    Reg  = MmioRead32 (DwcBase + DWC3_GUSB3PIPECTL0);
    Reg &= ~(DWC3_GUSB3PIPECTL_SUSPHY | DWC3_GUSB3PIPECTL_UX_EXIT_PX);
    MmioWrite32 (DwcBase + DWC3_GUSB3PIPECTL0, Reg);

    Reg  = MmioRead32 (DwcBase + DWC3_GFLADJ);
    Reg &= ~(UINT32)DWC3_GFLADJ_30MHZ_MASK;
    Reg |= DWC3_GFLADJ_30MHZ_SDBND_SEL | DWC3_GFLADJ_30MHZ_DEFAULT;
    MmioWrite32 (DwcBase + DWC3_GFLADJ, Reg);
  }
}

STATIC
EFI_PHYSICAL_ADDRESS
EFIAPI
Rp1BusGetPeripheralBase (
  IN RP1_BUS_PROTOCOL  *This
  )
{
  ASSERT (This != NULL);

  return (RP1_BUS_DATA_FROM_THIS (This))->PeripheralBase;
}

EFI_STATUS
EFIAPI
Rp1BusDriverBindingSupported (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                   ControllerHandle,
  IN EFI_DEVICE_PATH_PROTOCOL     *RemainingDevicePath
  )
{
  EFI_STATUS           Status;
  EFI_PCI_IO_PROTOCOL  *PciIo;
  UINT32               PciId;

  Status = gBS->OpenProtocol (
                  ControllerHandle,
                  &gEfiPciIoProtocolGuid,
                  (VOID **)&PciIo,
                  This->DriverBindingHandle,
                  ControllerHandle,
                  EFI_OPEN_PROTOCOL_BY_DRIVER
                  );

  if (EFI_ERROR (Status)) {
    return EFI_UNSUPPORTED;
  }

  Status = PciIo->Pci.Read (
                        PciIo,
                        EfiPciIoWidthUint32,
                        PCI_VENDOR_ID_OFFSET,
                        1,
                        &PciId
                        );

  if (EFI_ERROR (Status)) {
    Status = EFI_UNSUPPORTED;
    goto Exit;
  }

  if (((PciId & 0xffff) != PCI_VENDOR_ID_RPILTD) ||
      ((PciId >> 16) != PCI_DEVICE_ID_RP1))
  {
    Status = EFI_UNSUPPORTED;
  }

Exit:
  gBS->CloseProtocol (
         ControllerHandle,
         &gEfiPciIoProtocolGuid,
         This->DriverBindingHandle,
         ControllerHandle
         );

  return Status;
}

EFI_STATUS
EFIAPI
Rp1BusDriverBindingStart (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                   ControllerHandle,
  IN EFI_DEVICE_PATH_PROTOCOL     *RemainingDevicePath
  )
{
  EFI_STATUS                         Status;
  EFI_PCI_IO_PROTOCOL                *PciIo;
  UINT64                             Supports;
  RP1_BUS_DATA                       *Rp1Data;
  EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR  *PeripheralDesc;

  Rp1Data = NULL;

  Status = gBS->OpenProtocol (
                  ControllerHandle,
                  &gEfiPciIoProtocolGuid,
                  (VOID **)&PciIo,
                  This->DriverBindingHandle,
                  ControllerHandle,
                  EFI_OPEN_PROTOCOL_BY_DRIVER
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = PciIo->Attributes (
                    PciIo,
                    EfiPciIoAttributeOperationSupported,
                    0,
                    &Supports
                    );
  if (!EFI_ERROR (Status)) {
    Supports &= (UINT64)EFI_PCI_DEVICE_ENABLE;
    Status    = PciIo->Attributes (
                         PciIo,
                         EfiPciIoAttributeOperationEnable,
                         Supports,
                         NULL
                         );
  }

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "RP1: Failed to enable PCI device. Status=%r\n", Status));
    goto Fail;
  }

  Rp1Data = AllocateZeroPool (sizeof (RP1_BUS_DATA));
  if (Rp1Data == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    DEBUG ((DEBUG_ERROR, "RP1: Failed to allocate device context. Status=%r\n", Status));
    goto Fail;
  }

  Rp1Data->Signature                = RP1_BUS_DATA_SIGNATURE;
  Rp1Data->ControllerHandle         = ControllerHandle;
  Rp1Data->DriverBinding            = This;
  Rp1Data->PciIo                    = PciIo;
  Rp1Data->Rp1Bus.GetPeripheralBase = Rp1BusGetPeripheralBase;

  Status = PciIo->GetBarAttributes (
                    PciIo,
                    RP1_PERIPHERAL_BAR_INDEX,
                    NULL,
                    (VOID **)&PeripheralDesc
                    );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "RP1: Failed to get BAR attributes. Status=%r\n", Status));
    goto Fail;
  }

  Rp1Data->PeripheralBase = PeripheralDesc->AddrRangeMin;
  FreePool (PeripheralDesc);

  Rp1Data->ChipId = MmioRead32 (Rp1Data->PeripheralBase + RP1_SYSINFO_BASE);

  Status = gBS->InstallMultipleProtocolInterfaces (
                  &ControllerHandle,
                  &gRp1BusProtocolGuid,
                  &Rp1Data->Rp1Bus,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "RP1: Failed to install bus protocol. Status=%r\n", Status));
    goto Fail;
  }

  DEBUG ((
    DEBUG_INFO,
    "RP1: chip id %x, peripheral base at CPU address 0x%lx\n",
    Rp1Data->ChipId,
    Rp1Data->PeripheralBase
    ));

  Rp1BusRegisterDevices (Rp1Data);
  Rp1BusEthernetBringUp (Rp1Data);
  Rp1BusUsbHostBringUp (Rp1Data);
  Rp1BusEnableInterrupts (Rp1Data);

  return EFI_SUCCESS;

Fail:
  gBS->CloseProtocol (
         ControllerHandle,
         &gEfiPciIoProtocolGuid,
         This->DriverBindingHandle,
         ControllerHandle
         );

  gBS->UninstallMultipleProtocolInterfaces (
         ControllerHandle,
         &gRp1BusProtocolGuid,
         This->DriverBindingHandle,
         ControllerHandle
         );

  if (Rp1Data != NULL) {
    FreePool (Rp1Data);
  }

  return Status;
}

STATIC
EFI_STATUS
EFIAPI
Rp1BusUnregisterNonDiscoverableDevice (
  IN  EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN  EFI_HANDLE                   ControllerHandle,
  IN  EFI_HANDLE                   DeviceHandle
  )
{
  EFI_STATUS                Status;
  NON_DISCOVERABLE_DEVICE   *NonDiscoverableDevice;
  EFI_DEVICE_PATH_PROTOCOL  *NonDiscoverableDevicePath;
  RP1_BUS_PROTOCOL          *Rp1Bus;

  Status = gBS->OpenProtocol (
                  DeviceHandle,
                  &gEdkiiNonDiscoverableDeviceProtocolGuid,
                  (VOID **)&NonDiscoverableDevice,
                  This->DriverBindingHandle,
                  ControllerHandle,
                  EFI_OPEN_PROTOCOL_GET_PROTOCOL
                  );
  if (EFI_ERROR (Status)) {
    ASSERT_EFI_ERROR (Status);
    return Status;
  }

  Status = gBS->OpenProtocol (
                  DeviceHandle,
                  &gEfiDevicePathProtocolGuid,
                  (VOID **)&NonDiscoverableDevicePath,
                  This->DriverBindingHandle,
                  ControllerHandle,
                  EFI_OPEN_PROTOCOL_GET_PROTOCOL
                  );
  if (EFI_ERROR (Status)) {
    ASSERT_EFI_ERROR (Status);
    return Status;
  }

  Status = gBS->CloseProtocol (
                  ControllerHandle,
                  &gRp1BusProtocolGuid,
                  This->DriverBindingHandle,
                  DeviceHandle
                  );
  ASSERT_EFI_ERROR (Status);

  Status = gBS->UninstallMultipleProtocolInterfaces (
                  DeviceHandle,
                  &gEdkiiNonDiscoverableDeviceProtocolGuid,
                  NonDiscoverableDevice,
                  &gEfiDevicePathProtocolGuid,
                  NonDiscoverableDevicePath,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    gBS->OpenProtocol (
           ControllerHandle,
           &gRp1BusProtocolGuid,
           (VOID **)&Rp1Bus,
           This->DriverBindingHandle,
           DeviceHandle,
           EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER
           );
    return Status;
  }

  FreePool (NonDiscoverableDevice);
  FreePool (NonDiscoverableDevicePath);

  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
Rp1BusDriverBindingStop (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                   ControllerHandle,
  IN UINTN                        NumberOfChildren,
  IN EFI_HANDLE                   *DeviceHandleBuffer
  )
{
  EFI_STATUS        Status;
  UINTN             Index;
  RP1_BUS_PROTOCOL  *Rp1Bus;
  BOOLEAN           AllChildrenStopped;

  if (NumberOfChildren == 0) {
    DEBUG ((DEBUG_INFO, "RP1: Stop bus at %p\n", ControllerHandle));

    Status = gBS->OpenProtocol (
                    ControllerHandle,
                    &gRp1BusProtocolGuid,
                    (VOID **)&Rp1Bus,
                    This->DriverBindingHandle,
                    ControllerHandle,
                    EFI_OPEN_PROTOCOL_GET_PROTOCOL
                    );
    if (EFI_ERROR (Status)) {
      return Status;
    }

    Status = gBS->UninstallMultipleProtocolInterfaces (
                    ControllerHandle,
                    &gRp1BusProtocolGuid,
                    Rp1Bus,
                    NULL
                    );
    ASSERT_EFI_ERROR (Status);

    FreePool (RP1_BUS_DATA_FROM_THIS (Rp1Bus));

    Status = gBS->CloseProtocol (
                    ControllerHandle,
                    &gEfiPciIoProtocolGuid,
                    This->DriverBindingHandle,
                    ControllerHandle
                    );
    ASSERT_EFI_ERROR (Status);

    return EFI_SUCCESS;
  }

  AllChildrenStopped = TRUE;

  for (Index = 0; Index < NumberOfChildren; Index++) {
    //
    // We only register non-discoverable PCI devices so far.
    //
    Status = Rp1BusUnregisterNonDiscoverableDevice (
               This,
               ControllerHandle,
               DeviceHandleBuffer[Index]
               );
    if (EFI_ERROR (Status)) {
      AllChildrenStopped = FALSE;
      continue;
    }
  }

  if (!AllChildrenStopped) {
    return EFI_DEVICE_ERROR;
  }

  return EFI_SUCCESS;
}

EFI_DRIVER_BINDING_PROTOCOL  mRp1BusDriverBinding = {
  Rp1BusDriverBindingSupported,
  Rp1BusDriverBindingStart,
  Rp1BusDriverBindingStop,
  0x10,
  NULL,
  NULL
};

EFI_STATUS
EFIAPI
Rp1BusDxeEntryPoint (
  IN  EFI_HANDLE        ImageHandle,
  IN  EFI_SYSTEM_TABLE  *SystemTable
  )
{
  return EfiLibInstallDriverBindingComponentName2 (
           ImageHandle,
           SystemTable,
           &mRp1BusDriverBinding,
           ImageHandle,
           &mRp1BusComponentName,
           &mRp1BusComponentName2
           );
}

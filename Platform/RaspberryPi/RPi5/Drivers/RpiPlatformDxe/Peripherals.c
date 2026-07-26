/** @file
 *
 *  Copyright (c) 2023-2024, Mario Bălănică <mariobalanica02@gmail.com>
 *
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 **/

#include <Uefi.h>
#include <IndustryStandard/Bcm2712.h>
#include <IndustryStandard/Bcm2712Pinctrl.h>
#include <Library/Bcm2712GpioLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Protocol/BrcmStbSdhciDevice.h>

#include "Peripherals.h"
#include "ConfigTable.h"
#include "RpiPlatformDxe.h"

STATIC
EFI_STATUS
EFIAPI
SdControllerSetSignalingVoltage (
  IN BRCMSTB_SDHCI_DEVICE_PROTOCOL      *This,
  IN SD_MMC_SIGNALING_VOLTAGE           Voltage
  )
{
  // sd_io_1v8_reg
  GpioWrite (BCM2712_GIO_AON, 3, Voltage == SdMmcSignalingVoltage18);

  return EFI_SUCCESS;
}

STATIC BRCMSTB_SDHCI_DEVICE_PROTOCOL mSdController = {
  .HostAddress            = BCM2712_BRCMSTB_SDIO1_HOST_BASE,
  .CfgAddress             = BCM2712_BRCMSTB_SDIO1_CFG_BASE,
  .DmaType                = NonDiscoverableDeviceDmaTypeNonCoherent,
  .IsSlotRemovable        = TRUE,
  .SetSignalingVoltage    = SdControllerSetSignalingVoltage
};

STATIC
EFI_STATUS
EFIAPI
RegisterSdControllers (
  VOID
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  Handle = NULL;

  Status = gBS->InstallMultipleProtocolInterfaces (
                  &Handle,
                  &gBrcmStbSdhciDeviceProtocolGuid,
                  &mSdController,
                  NULL);
  ASSERT_EFI_ERROR (Status);

  return Status;
}

STATIC
EFI_STATUS
EFIAPI
InitGpioPinctrls (
  VOID
  )
{
  // SD card detect
  GpioSetFunction (BCM2712_GIO_AON, 5, GIO_AON_PIN5_ALT_SD_CARD_G);
  GpioSetPull (BCM2712_GIO_AON, 5, BCM2712_GPIO_PIN_PULL_UP);

  // ACPI owns the microSD power rail and starts with it off.
  GpioWrite (BCM2712_GIO_AON, 4, FALSE);
  GpioSetDirection (BCM2712_GIO_AON, 4, BCM2712_GPIO_PIN_OUTPUT);

  // Default the microSD signaling voltage to 3.3 V.
  GpioWrite (BCM2712_GIO_AON, 3, FALSE);
  GpioSetDirection (BCM2712_GIO_AON, 3, BCM2712_GPIO_PIN_OUTPUT);

  // Route SDIO to Wi-Fi
  GpioSetFunction (BCM2712_GIO, 30, GIO_PIN30_ALT_SD2);
  GpioSetPull (BCM2712_GIO, 30, BCM2712_GPIO_PIN_PULL_NONE);
  GpioSetFunction (BCM2712_GIO, 31, GIO_PIN31_ALT_SD2);
  GpioSetPull (BCM2712_GIO, 31, BCM2712_GPIO_PIN_PULL_UP);
  GpioSetFunction (BCM2712_GIO, 32, GIO_PIN32_ALT_SD2);
  GpioSetPull (BCM2712_GIO, 32, BCM2712_GPIO_PIN_PULL_UP);
  GpioSetFunction (BCM2712_GIO, 33, GIO_PIN33_ALT_SD2);
  GpioSetPull (BCM2712_GIO, 33, BCM2712_GPIO_PIN_PULL_UP);
  GpioSetFunction (BCM2712_GIO, 34, GIO_PIN34_ALT_SD2);
  GpioSetPull (BCM2712_GIO, 34, BCM2712_GPIO_PIN_PULL_UP);
  GpioSetFunction (BCM2712_GIO, 35, GIO_PIN35_ALT_SD2);
  GpioSetPull (BCM2712_GIO, 35, BCM2712_GPIO_PIN_PULL_UP);

  // CYW43455 WL_ON
  GpioWrite (BCM2712_GIO, 28, TRUE);
  GpioSetDirection (BCM2712_GIO, 28, BCM2712_GPIO_PIN_OUTPUT);

  return EFI_SUCCESS;
}

//
// Configure the SDIO2 host for the on-board Wi-Fi device.
//
#define SDIO2_CFG_CTRL                          0x0
#define SDIO2_CFG_CTRL_SDCD_N_TEST_EN           BIT31
#define SDIO2_CFG_CTRL_SDCD_N_TEST_LEV          BIT30
#define SDIO2_CFG_SD_PIN_SEL                    0x44
#define SDIO2_CFG_SD_PIN_SEL_MASK               (BIT1 | BIT0)
#define SDIO2_CFG_SD_PIN_SEL_SD                 BIT1
#define SDIO2_CFG_CQ_CAPABILITY                 0x4C
#define SDIO2_CFG_CQ_CAPABILITY_FMUL_SHIFT      12
#define SDIO2_CFG_MAX_50MHZ_MODE                0x1AC
#define SDIO2_CFG_MAX_50MHZ_MODE_STRAP_OVERRIDE BIT31
#define SDIO2_CFG_MAX_50MHZ_MODE_ENABLE         BIT0
#define SDIO2_BASE_CLOCK_MHZ                    200

STATIC
EFI_STATUS
EFIAPI
InitWifiSdioHost (
  VOID
  )
{
  STATIC CONST UINT32 Sdio2D0Pins[6][6] = {
    { 0x08, 24, 1, 0x14, 24, 0 },
    { 0x08, 28, 1, 0x14, 26, 2 },
    { 0x0C, 0,  1, 0x14, 28, 2 },
    { 0x0C, 4,  1, 0x18, 0,  2 },
    { 0x0C, 8,  1, 0x18, 2,  2 },
    { 0x0C, 12, 1, 0x18, 4,  2 }
  };
  UINTN   Index;
  UINT32  Reg;

  // SDIO2 D0 pinctrl
  for (Index = 0; Index < 6; Index++) {
    Reg = MmioRead32 (BCM2712_PINCTRL_BASE + Sdio2D0Pins[Index][0]);
    Reg &= ~(0xFU << Sdio2D0Pins[Index][1]);
    Reg |= (Sdio2D0Pins[Index][2] & 0xF) << Sdio2D0Pins[Index][1];
    MmioWrite32 (BCM2712_PINCTRL_BASE + Sdio2D0Pins[Index][0], Reg);

    Reg = MmioRead32 (BCM2712_PINCTRL_BASE + Sdio2D0Pins[Index][3]);
    Reg &= ~(0x3U << Sdio2D0Pins[Index][4]);
    Reg |= (Sdio2D0Pins[Index][5] & 0x3) << Sdio2D0Pins[Index][4];
    MmioWrite32 (BCM2712_PINCTRL_BASE + Sdio2D0Pins[Index][3], Reg);
  }

  // SD/MMC pin-select -> SD
  Reg = MmioRead32 (BCM2712_BRCMSTB_SDIO2_CFG_BASE + SDIO2_CFG_SD_PIN_SEL);
  Reg &= ~SDIO2_CFG_SD_PIN_SEL_MASK;
  Reg |= SDIO2_CFG_SD_PIN_SEL_SD;
  MmioWrite32 (BCM2712_BRCMSTB_SDIO2_CFG_BASE + SDIO2_CFG_SD_PIN_SEL, Reg);

  // Wait for WL_ON to settle.
  gBS->Stall (150000);

  // 50 MHz strap override
  Reg = MmioRead32 (BCM2712_BRCMSTB_SDIO2_CFG_BASE + SDIO2_CFG_MAX_50MHZ_MODE);
  Reg &= ~SDIO2_CFG_MAX_50MHZ_MODE_ENABLE;
  Reg |= SDIO2_CFG_MAX_50MHZ_MODE_STRAP_OVERRIDE;
  MmioWrite32 (BCM2712_BRCMSTB_SDIO2_CFG_BASE + SDIO2_CFG_MAX_50MHZ_MODE, Reg);

  // Base clock (SDC1 Caps): FMUL = 3, 200 MHz
  Reg = (3U << SDIO2_CFG_CQ_CAPABILITY_FMUL_SHIFT) | SDIO2_BASE_CLOCK_MHZ;
  MmioWrite32 (BCM2712_BRCMSTB_SDIO2_CFG_BASE + SDIO2_CFG_CQ_CAPABILITY, Reg);

  // Force card present
  Reg = MmioRead32 (BCM2712_BRCMSTB_SDIO2_CFG_BASE + SDIO2_CFG_CTRL);
  Reg &= ~SDIO2_CFG_CTRL_SDCD_N_TEST_LEV;
  Reg |= SDIO2_CFG_CTRL_SDCD_N_TEST_EN;
  MmioWrite32 (BCM2712_BRCMSTB_SDIO2_CFG_BASE + SDIO2_CFG_CTRL, Reg);

  return EFI_SUCCESS;
}

BCM2712_PCIE_PLATFORM_PROTOCOL  mPciePlatform = {
  .Mem32BusBase = PCI_RESERVED_MEM32_BASE,
  .Mem32Size    = PCI_RESERVED_MEM32_SIZE,

  .Settings         = {
    [1] = { // Connector (configurable)
      .Enabled      = PCIE1_SETTINGS_ENABLED_DEFAULT,
      .MaxLinkSpeed = PCIE1_SETTINGS_MAX_LINK_SPEED_DEFAULT
    },
    [2] = { // RP1 (fixed)
      .Enabled      = TRUE,
      .MaxLinkSpeed = 2,
      .RcbMatchMps  = TRUE,
      .VdmToQosMap  = 0xbbaa9888
    }
  }
};

STATIC
EFI_STATUS
EFIAPI
RegisterPciePlatform (
  VOID
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  Handle = NULL;

  ASSERT_PROTOCOL_ALREADY_INSTALLED (NULL, &gBcm2712PciePlatformProtocolGuid);
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &Handle,
                  &gBcm2712PciePlatformProtocolGuid,
                  &mPciePlatform,
                  NULL
                  );
  ASSERT_EFI_ERROR (Status);

  return Status;
}

EFI_STATUS
EFIAPI
SetupPeripherals (
  VOID
  )
{
  InitGpioPinctrls ();
  InitWifiSdioHost ();

  RegisterSdControllers ();
  RegisterPciePlatform ();

  return EFI_SUCCESS;
}

VOID
EFIAPI
ApplyPeripheralVariables (
  VOID
  )
{
}

VOID
EFIAPI
SetupPeripheralVariables (
  VOID
  )
{
  EFI_STATUS    Status;
  UINTN         Size;

  Size = sizeof (BCM2712_PCIE_CONTROLLER_SETTINGS);
  Status = gRT->GetVariable (L"Pcie1Settings",
                  &gRpiPlatformFormSetGuid,
                  NULL, &Size, &mPciePlatform.Settings[1]);
  if (EFI_ERROR (Status)) {
    Status = gRT->SetVariable (
                    L"Pcie1Settings",
                    &gRpiPlatformFormSetGuid,
                    EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS,
                    Size,
                    &mPciePlatform.Settings[1]);
    ASSERT_EFI_ERROR (Status);
  }
}

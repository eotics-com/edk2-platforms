/** @file
 *
 *  Copyright (c) 2023-2024, Mario Bălănică <mariobalanica02@gmail.com>
 *
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 **/

#include <Guid/RpiPlatformFormSetGuid.h>
#include <IndustryStandard/Acpi.h>
#include <IndustryStandard/Pci.h>
#include <IndustryStandard/PeImage.h>
#include <Library/AcpiLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/FdtLib.h>
#include <Library/FdtPlatformLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PeCoffGetEntryPointLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Protocol/AcpiSystemDescriptionTable.h>
#include <Protocol/EdidActive.h>
#include <Protocol/PciIo.h>
#include <Protocol/Rp1Bus.h>
#include <Protocol/RpiFirmware.h>
#include <Rp1.h>
#include <Rp1DsiPanel.h>
#include <RpiPlatformVarStoreData.h>
#include <Rpi5McfgTable.h>
#include <ConfigVars.h>

#include "ConfigTable.h"
#include "RpiPlatformDxe.h"
#include "Peripherals.h"

//
// AcpiTables.inf
//
STATIC CONST EFI_GUID mAcpiTableFile = {
  0x7E374E25, 0x8E01, 0x4FEE, { 0x87, 0xf2, 0x39, 0x0C, 0x23, 0xC6, 0x06, 0xCD }
};

STATIC ACPI_SD_COMPAT_MODE_VARSTORE_DATA    AcpiSdCompatMode;
STATIC ACPI_SD_LIMIT_UHS_VARSTORE_DATA      AcpiSdLimitUhs;

STATIC ACPI_PCIE_ECAM_COMPAT_MODE_VARSTORE_DATA          AcpiPcieEcamCompatMode;
STATIC ACPI_PCIE_32_BIT_BAR_SPACE_SIZE_MB_VARSTORE_DATA  AcpiPcie32BitBarSpaceSizeMB;

STATIC BOOLEAN                      mIsAcpiEnabled;
STATIC EFI_ACPI_SDT_PROTOCOL        *mAcpiSdtProtocol;
STATIC EFI_ACPI_DESCRIPTION_HEADER  *mDsdtTable;

STATIC UINT64  mAcpiPciMem32Base;
STATIC UINT64  mAcpiPciMem32Size;

STATIC CONST UINT8  mRp1DsiPanelEdid[RP1_DSI_PANEL_EDID_SIZE] = {
  RP1_DSI_PANEL_EDID_BYTES
};

STATIC EFI_EXIT_BOOT_SERVICES  mOriginalExitBootServices;

typedef enum {
  AcpiOsUnknown = 0,
  AcpiOsWindows,
} ACPI_OS_BOOT_TYPE;

#define SDT_PATTERN_LEN  (AML_NAME_SEG_SIZE + 1)

#define RP1_CLK_PWM1_CTRL              (RP1_CLOCKS_MAIN_BASE + 0x084)
#define RP1_CLK_PWM1_DIV_INT           (RP1_CLOCKS_MAIN_BASE + 0x088)
#define RP1_CLK_PWM1_DIV_FRAC          (RP1_CLOCKS_MAIN_BASE + 0x08C)
#define RP1_CLK_ADC_CTRL               (RP1_CLOCKS_MAIN_BASE + 0x144)
#define RP1_CLK_ADC_DIV_INT            (RP1_CLOCKS_MAIN_BASE + 0x148)
#define RP1_CLK_CTRL_AUXSRC_MASK       0x000003E0
#define RP1_CLK_CTRL_AUXSRC_XOSC       (2U << 5)
#define RP1_CLK_CTRL_SRC_MASK          BIT0
#define RP1_CLK_CTRL_SRC_AUX           BIT0
#define RP1_CLK_CTRL_ENABLE            BIT11

#define RP1_FAN_GPIO_CTRL              (RP1_IO_BANK2_BASE + 0x5C)
#define RP1_FAN_PAD_CTRL               (RP1_PADS_BANK2_BASE + 0x30)
#define RP1_GPIO_FUNCSEL_MASK          0x0000001F
#define RP1_GPIO_OVERRIDE_MASK         0x0003F000
#define RP1_PAD_PULL_MASK              0x0000000C
#define RP1_PAD_PULL_DOWN              0x00000004
#define RP1_PAD_OUT_DISABLE            BIT7

#define RP1_PWM_GLOBAL_CTRL            (RP1_PWM1_BASE + 0x000)
#define RP1_PWM_CHANNEL3_CTRL          (RP1_PWM1_BASE + 0x044)
#define RP1_PWM_CHANNEL3_RANGE         (RP1_PWM1_BASE + 0x048)
#define RP1_PWM_CHANNEL3_DUTY          (RP1_PWM1_BASE + 0x050)
#define RP1_PWM_CHANNEL3_ENABLE        BIT3
#define RP1_PWM_CHANNEL_DEFAULT        (BIT8 | BIT0)
#define RP1_PWM_POLARITY_INVERTED      BIT3
#define RP1_PWM_SET_UPDATE             BIT31
#define RP1_FAN_PWM_RANGE              2078U
#define RP1_FAN_PWM_LOW                75U

#define RP1_ADC_CS                     (RP1_ADC_BASE + 0x000)
#define RP1_ADC_RESULT                 (RP1_ADC_BASE + 0x004)
#define RP1_ADC_INTE                   (RP1_ADC_BASE + 0x018)
#define RP1_ADC_RWTYPE_SET             0x2000
#define RP1_ADC_RWTYPE_CLR             0x3000
#define RP1_ADC_CS_AINSEL_MASK         (0x7U << 12)
#define RP1_ADC_CS_ERR_STICKY          BIT10
#define RP1_ADC_CS_ERR                 BIT9
#define RP1_ADC_CS_READY               BIT8
#define RP1_ADC_CS_START_ONCE          BIT2
#define RP1_ADC_CS_TS_EN               BIT1
#define RP1_ADC_CS_EN                  BIT0
#define RP1_ADC_TEMP_CHANNEL           4U
#define RP1_ADC_RESULT_MASK            0xFFFU

#define TELEMETRY_TEMPERATURE_ID       0U
#define TELEMETRY_MIN_VOLTAGE_ID       1U
#define TELEMETRY_MAX_VOLTAGE_ID       4U
#define TELEMETRY_MIN_VOLTAGE_UV       100000U
#define TELEMETRY_MAX_VOLTAGE_UV       6000000U
#define PMIC_ADC_RESPONSE_SIZE          2048U
#define PMIC_ADC_MAX_VOLTAGE_UV         6000000U
#define PMIC_ADC_MAX_CURRENT_UA         20000000U

typedef struct {
  UINT32       Id;
  BOOLEAN      IsVoltage;
  CONST CHAR8  *Name;
} PMIC_ADC_CHANNEL;

STATIC CONST PMIC_ADC_CHANNEL  mPmicAdcChannels[] = {
  {  0, FALSE, "3V7_WL_SW_A" },
  {  1, FALSE, "3V3_SYS_A" },
  {  2, FALSE, "1V8_SYS_A" },
  {  3, FALSE, "DDR_VDD2_A" },
  {  4, FALSE, "DDR_VDDQ_A" },
  {  5, FALSE, "1V1_SYS_A" },
  {  6, FALSE, "0V8_SW_A" },
  {  7, FALSE, "VDD_CORE_A" },
  {  8, TRUE,  "3V7_WL_SW_V" },
  {  9, TRUE,  "3V3_SYS_V" },
  { 10, TRUE,  "1V8_SYS_V" },
  { 11, TRUE,  "DDR_VDD2_V" },
  { 12, TRUE,  "DDR_VDDQ_V" },
  { 13, TRUE,  "1V1_SYS_V" },
  { 14, TRUE,  "0V8_SW_V" },
  { 15, TRUE,  "VDD_CORE_V" },
  { 16, FALSE, "0V8_AON_A" },
  { 17, FALSE, "3V3_DAC_A" },
  { 18, FALSE, "3V3_ADC_A" },
  { 19, TRUE,  "0V8_AON_V" },
  { 20, TRUE,  "3V3_DAC_V" },
  { 21, TRUE,  "3V3_ADC_V" },
  { 22, FALSE, "HDMI_A" },
  { 23, TRUE,  "HDMI_V" },
  { 24, TRUE,  "EXT5V_V" },
  { 25, TRUE,  "BATT_V" },
};

STATIC CONST UINT8  mPmicAdcPowerPairs[][2] = {
  {  0,  8 },
  {  1,  9 },
  {  2, 10 },
  {  3, 11 },
  {  4, 12 },
  {  5, 13 },
  {  6, 14 },
  {  7, 15 },
  { 16, 19 },
  { 17, 20 },
  { 18, 21 },
  { 22, 23 },
};

//
// Simple NameOp integer patcher.
// Does not allocate memory and can be safely used at ExitBootServices.
//
STATIC
EFI_STATUS
EFIAPI
AcpiUpdateSdtNameInteger (
  IN  EFI_ACPI_DESCRIPTION_HEADER  *AcpiTable,
  IN  CHAR8                        Name[AML_NAME_SEG_SIZE],
  IN  UINTN                        Value
  )
{
  UINTN   Index;
  CHAR8   Pattern[SDT_PATTERN_LEN];
  UINT8   *SdtPtr;
  UINT32  DataSize;
  UINT32  ValueOffset;

  if (AcpiTable->Length <= SDT_PATTERN_LEN) {
    return EFI_INVALID_PARAMETER;
  }

  SdtPtr = (UINT8 *)AcpiTable;
  //
  // Do a single NameOp variable replacement. These are of the
  // form "08 XXXX SIZE VAL", where SIZE is: 0A=byte, 0B=word, 0C=dword,
  // XXXX is the name and VAL is the value.
  //
  Pattern[0] = AML_NAME_OP;
  CopyMem (Pattern + 1, Name, AML_NAME_SEG_SIZE);

  ValueOffset = SDT_PATTERN_LEN + 1;

  for (Index = 0; Index < (AcpiTable->Length - SDT_PATTERN_LEN); Index++) {
    if (CompareMem (SdtPtr + Index, Pattern, SDT_PATTERN_LEN) == 0) {
      switch (SdtPtr[Index + SDT_PATTERN_LEN]) {
        case AML_QWORD_PREFIX:
          DataSize = sizeof (UINT64);
          break;
        case AML_DWORD_PREFIX:
          DataSize = sizeof (UINT32);
          break;
        case AML_WORD_PREFIX:
          DataSize = sizeof (UINT16);
          break;
        case AML_ONE_OP:
        case AML_ZERO_OP:
          ValueOffset--;
        // Fallthrough
        case AML_BYTE_PREFIX:
          DataSize = sizeof (UINT8);
          break;
        default:
          return EFI_UNSUPPORTED;
      }

      CopyMem (SdtPtr + Index + ValueOffset, &Value, DataSize);
      return EFI_SUCCESS;
    }
  }

  return EFI_NOT_FOUND;
}

STATIC
VOID
EFIAPI
DsdtFixupStatus (
  IN EFI_ACPI_SDT_PROTOCOL    *AcpiSdtProtocol,
  IN EFI_ACPI_HANDLE          TableHandle
  )
{
  EFI_STATUS  Status;
  UINTN       Index;

  struct {
    CHAR8    *ObjectPath;
    BOOLEAN  Enabled;
  } DevStatus[] = {
    { "\\_SB.PCI0._STA", FALSE },                             // Not exposed
    { "\\_SB.PCI1._STA", mPciePlatform.Settings[1].Enabled }, // Configurable
    { "\\_SB.PCI2._STA", FALSE },                             // Reserved by RP1
  };

  for (Index = 0; Index < ARRAY_SIZE (DevStatus); Index++) {
    if (DevStatus[Index].Enabled == FALSE) {
      Status = AcpiAmlObjectUpdateInteger (AcpiSdtProtocol, TableHandle,
                  DevStatus[Index].ObjectPath, 0x0);
      if (EFI_ERROR (Status)) {
        DEBUG ((DEBUG_ERROR, "%a: Failed to patch %a. Status=%r\n",
                __func__, DevStatus[Index].ObjectPath, Status));
      }
    }
  }
}

STATIC
BOOLEAN
IsRp1DsiPanelPresent (
  VOID
  )
{
  EFI_EDID_ACTIVE_PROTOCOL  *EdidActive;
  EFI_HANDLE                *Handles;
  EFI_STATUS                Status;
  UINTN                     HandleCount;
  UINTN                     Index;
  BOOLEAN                   Present;

  Handles     = NULL;
  HandleCount = 0;
  Present     = FALSE;
  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiEdidActiveProtocolGuid,
                  NULL,
                  &HandleCount,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    return FALSE;
  }

  for (Index = 0; Index < HandleCount; Index++) {
    Status = gBS->HandleProtocol (
                    Handles[Index],
                    &gEfiEdidActiveProtocolGuid,
                    (VOID **)&EdidActive
                    );
    if (!EFI_ERROR (Status) &&
        (EdidActive->Edid != NULL) &&
        (EdidActive->SizeOfEdid >= sizeof (mRp1DsiPanelEdid)) &&
        (CompareMem (
           EdidActive->Edid,
           mRp1DsiPanelEdid,
           sizeof (mRp1DsiPanelEdid)
           ) == 0))
    {
      Present = TRUE;
      break;
    }
  }

  FreePool (Handles);
  return Present;
}

STATIC
VOID
EFIAPI
DsdtFixupDisplay (
  IN EFI_ACPI_SDT_PROTOCOL  *AcpiSdtProtocol,
  IN EFI_ACPI_HANDLE        TableHandle
  )
{
  BOOLEAN     DsiPresent;
  EFI_STATUS  Status;

  DsiPresent = IsRp1DsiPanelPresent ();
  Status = AcpiAmlObjectUpdateInteger (
             AcpiSdtProtocol,
             TableHandle,
             "\\_SB.GPU0.DSTA",
             DsiPresent ? 0xF : 0x0
             );
  if (EFI_ERROR (Status)) {
    return;
  }
}

STATIC
VOID
EFIAPI
DsdtFixupSd (
  IN EFI_ACPI_SDT_PROTOCOL    *AcpiSdtProtocol,
  IN EFI_ACPI_HANDLE          TableHandle
  )
{
  EFI_STATUS Status;

  Status = AcpiAmlObjectUpdateInteger (AcpiSdtProtocol, TableHandle,
                "\\_SB.SDCM", AcpiSdCompatMode.Value);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: Failed to patch AcpiSdCompatMode.\n", __func__));
  }

  Status = AcpiAmlObjectUpdateInteger (AcpiSdtProtocol, TableHandle,
                "\\_SB.SDLU", AcpiSdLimitUhs.Value);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: Failed to patch AcpiSdLimitUhs.\n", __func__));
  }
}

STATIC
VOID
Rp1ProgramGemMac (
  IN RP1_BUS_PROTOCOL  *Rp1Bus
  )
{
  EFI_STATUS                      Status;
  RASPBERRY_PI_FIRMWARE_PROTOCOL  *Firmware;
  UINT8                           Mac[6];
  EFI_PHYSICAL_ADDRESS            GemBase;

  Status = gBS->LocateProtocol (
                  &gRaspberryPiFirmwareProtocolGuid,
                  NULL,
                  (VOID **)&Firmware
                  );
  if (EFI_ERROR (Status) || EFI_ERROR (Firmware->GetMacAddress (Mac))) {
    return;
  }

  // SA1T latches the station address, so write SA1B first.
  GemBase = Rp1Bus->GetPeripheralBase (Rp1Bus) + RP1_ETH_BASE;
  MmioWrite32 (
    (UINTN)(GemBase + 0x88),
    (UINT32)Mac[0] | ((UINT32)Mac[1] << 8) |
    ((UINT32)Mac[2] << 16) | ((UINT32)Mac[3] << 24)
    );
  MmioWrite32 (
    (UINTN)(GemBase + 0x8C),
    (UINT32)Mac[4] | ((UINT32)Mac[5] << 8)
    );
}

STATIC
BOOLEAN
Rp1FanIsPresent (
  VOID
  )
{
  CONST CHAR8  *Compatible;
  CONST CHAR8  *NodeStatus;
  VOID         *Fdt;
  INT32        Length;
  INT32        Node;

  Fdt = FdtPlatformGetBase ();
  if (Fdt == NULL) {
    return FALSE;
  }

  Node = FdtPathOffset (Fdt, "/cooling_fan");
  if (Node < 0) {
    return FALSE;
  }

  Compatible = FdtGetProp (Fdt, Node, "compatible", &Length);
  if ((Compatible == NULL) ||
      !FdtStringListContains (Compatible, Length, "pwm-fan"))
  {
    return FALSE;
  }

  NodeStatus = FdtGetProp (Fdt, Node, "status", &Length);
  if ((NodeStatus == NULL) || (Length <= 0) ||
      (NodeStatus[Length - 1] != '\0'))
  {
    return FALSE;
  }

  return (AsciiStrCmp (NodeStatus, "okay") == 0) ||
         (AsciiStrCmp (NodeStatus, "ok") == 0);
}

STATIC
VOID
Rp1ProgramFan (
  IN RP1_BUS_PROTOCOL  *Rp1Bus
  )
{
  EFI_PHYSICAL_ADDRESS  Base;
  UINT32                Register;

  Base = Rp1Bus->GetPeripheralBase (Rp1Bus);

  // Drive PWM1 directly from the 50 MHz crystal clock.
  MmioWrite32 (Base + RP1_CLK_PWM1_DIV_INT, 1);
  MmioWrite32 (Base + RP1_CLK_PWM1_DIV_FRAC, 0);
  Register  = MmioRead32 (Base + RP1_CLK_PWM1_CTRL);
  Register &= ~(RP1_CLK_CTRL_AUXSRC_MASK | RP1_CLK_CTRL_SRC_MASK);
  Register |= RP1_CLK_CTRL_AUXSRC_XOSC | RP1_CLK_CTRL_SRC_AUX |
              RP1_CLK_CTRL_ENABLE;
  MmioWrite32 (Base + RP1_CLK_PWM1_CTRL, Register);

  // GPIO45 is PWM1 channel 3 and uses a pull-down on the Pi 5 fan header.
  Register  = MmioRead32 (Base + RP1_FAN_PAD_CTRL);
  Register &= ~(RP1_PAD_PULL_MASK | RP1_PAD_OUT_DISABLE);
  Register |= RP1_PAD_PULL_DOWN;
  MmioWrite32 (Base + RP1_FAN_PAD_CTRL, Register);

  Register  = MmioRead32 (Base + RP1_FAN_GPIO_CTRL);
  Register &= ~(RP1_GPIO_FUNCSEL_MASK | RP1_GPIO_OVERRIDE_MASK);
  MmioWrite32 (Base + RP1_FAN_GPIO_CTRL, Register);

  // Start at the lowest cooling level; ACPI takes ownership from here.
  MmioWrite32 (Base + RP1_PWM_CHANNEL3_RANGE, RP1_FAN_PWM_RANGE);
  MmioWrite32 (
    Base + RP1_PWM_CHANNEL3_DUTY,
    (RP1_FAN_PWM_RANGE * RP1_FAN_PWM_LOW) / 255
    );
  MmioWrite32 (
    Base + RP1_PWM_CHANNEL3_CTRL,
    RP1_PWM_CHANNEL_DEFAULT | RP1_PWM_POLARITY_INVERTED
    );

  Register  = MmioRead32 (Base + RP1_PWM_GLOBAL_CTRL);
  Register |= RP1_PWM_CHANNEL3_ENABLE | RP1_PWM_SET_UPDATE;
  MmioWrite32 (Base + RP1_PWM_GLOBAL_CTRL, Register);
}

STATIC
BOOLEAN
Rp1ProgramAdc (
  IN RP1_BUS_PROTOCOL  *Rp1Bus
  )
{
  EFI_PHYSICAL_ADDRESS  Base;
  UINT32                Register;
  UINTN                 Retry;

  Base = Rp1Bus->GetPeripheralBase (Rp1Bus);

  // The RP1 ADC has a single 50 MHz parent and an integer-only divider.
  MmioWrite32 (Base + RP1_CLK_ADC_DIV_INT, 1);
  Register = MmioRead32 (Base + RP1_CLK_ADC_CTRL);
  Register |= RP1_CLK_CTRL_ENABLE;
  MmioWrite32 (Base + RP1_CLK_ADC_CTRL, Register);

  MmioWrite32 (Base + RP1_ADC_INTE, 0);
  MmioWrite32 (
    Base + RP1_ADC_CS,
    RP1_ADC_CS_EN | RP1_ADC_CS_ERR_STICKY
    );

  // Validate the same internal temperature channel that AML will sample.
  MmioWrite32 (
    Base + RP1_ADC_RWTYPE_CLR + RP1_ADC_CS,
    RP1_ADC_CS_AINSEL_MASK
    );
  MmioWrite32 (
    Base + RP1_ADC_RWTYPE_SET + RP1_ADC_CS,
    (RP1_ADC_TEMP_CHANNEL << 12) |
    RP1_ADC_CS_TS_EN |
    RP1_ADC_CS_START_ONCE
    );

  for (Retry = 0; Retry < 1000; Retry++) {
    Register = MmioRead32 (Base + RP1_ADC_CS);
    if ((Register & RP1_ADC_CS_READY) != 0) {
      break;
    }

    CpuPause ();
  }

  if ((Retry == 1000) || ((Register & RP1_ADC_CS_ERR) != 0) ||
      ((MmioRead32 (Base + RP1_ADC_RESULT) & ~RP1_ADC_RESULT_MASK) != 0))
  {
    DEBUG ((DEBUG_WARN, "%a: RP1 temperature ADC validation failed\n", __func__));
    return FALSE;
  }

  return TRUE;
}

STATIC
VOID
EFIAPI
DsdtFixupRp1 (
  IN EFI_ACPI_SDT_PROTOCOL    *AcpiSdtProtocol,
  IN EFI_ACPI_HANDLE          TableHandle
  )
{
  BOOLEAN           AdcReady;
  BOOLEAN           FanPresent;
  EFI_STATUS        Status;
  RP1_BUS_PROTOCOL  *Rp1Bus;
  UINTN             HandleCount;
  EFI_HANDLE        *Handles;

  HandleCount = 0;
  Handles = NULL;
  Rp1Bus = NULL;

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gRp1BusProtocolGuid,
                  NULL,
                  &HandleCount,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_WARN,
      "%a: Failed to locate RP1 instance! Status=%r\n",
      __func__,
      Status
      ));
    return;
  }

  if (HandleCount > 1) {
    DEBUG ((DEBUG_WARN, "%a: Only one RP1 instance is supported!\n", __func__));
  }

  Status = gBS->HandleProtocol (
                  Handles[0],
                  &gRp1BusProtocolGuid,
                  (VOID **)&Rp1Bus
                  );
  FreePool (Handles);

  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_WARN,
      "%a: Failed to get RP1 bus protocol! Status=%r\n",
      __func__,
      Status
      ));
    return;
  }

  Status = AcpiAmlObjectUpdateInteger (
             AcpiSdtProtocol,
             TableHandle,
             "\\_SB.RP1B.PBAR",
             Rp1Bus->GetPeripheralBase (Rp1Bus)
             );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: Failed to patch PBAR. Status=%r\n", __func__, Status));
  }

  Rp1ProgramGemMac (Rp1Bus);

  AdcReady = Rp1ProgramAdc (Rp1Bus);
  Status = AcpiAmlObjectUpdateInteger (
             AcpiSdtProtocol,
             TableHandle,
             "\\_SB.RP1B.ASTA",
             AdcReady ? 0xF : 0x0
             );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: Failed to patch ASTA. Status=%r\n", __func__, Status));
  }

  FanPresent = Rp1FanIsPresent ();

  Status = AcpiAmlObjectUpdateInteger (
             AcpiSdtProtocol,
             TableHandle,
             "\\_SB.RP1B.FSTA",
             FanPresent ? 0xF : 0x0
             );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: Failed to patch FSTA. Status=%r\n", __func__, Status));
    return;
  }

  if (!FanPresent) {
    DEBUG ((DEBUG_INFO, "%a: No bootloader-detected Pi 5 fan\n", __func__));
    return;
  }

  Status = AcpiAmlObjectUpdateInteger (
             AcpiSdtProtocol,
             TableHandle,
             "\\_SB.RP1B.FRQS",
             0x1
             );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: Failed to patch FRQS. Status=%r\n", __func__, Status));
    return;
  }

  Rp1ProgramFan (Rp1Bus);
  DEBUG ((DEBUG_INFO, "%a: Pi 5 fan exposed through ACPI\n", __func__));
}

STATIC
EFI_STATUS
PatchDsdtInteger (
  IN EFI_ACPI_SDT_PROTOCOL  *AcpiSdtProtocol,
  IN EFI_ACPI_HANDLE        TableHandle,
  IN CHAR8                  *ObjectPath,
  IN UINTN                  Value
  )
{
  EFI_STATUS  Status;

  Status = AcpiAmlObjectUpdateInteger (
             AcpiSdtProtocol,
             TableHandle,
             ObjectPath,
             Value
             );
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "%a: Failed to patch %a. Status=%r\n",
      __func__,
      ObjectPath,
      Status
      ));
  }

  return Status;
}

STATIC
BOOLEAN
ParsePmicAdcValue (
  IN  CONST CHAR8             *Response,
  IN  CONST PMIC_ADC_CHANNEL  *Channel,
  OUT UINT32                  *Value
  )
{
  CONST CHAR8  *Cursor;
  CONST CHAR8  *Line;
  UINTN        FractionDigits;
  UINTN        NameLength;
  UINT32       ParsedId;
  UINT64       Fraction;
  UINT64       Integer;
  UINT64       Result;
  BOOLEAN      HaveDigit;
  BOOLEAN      RoundUp;

  if ((Response == NULL) || (Channel == NULL) || (Value == NULL)) {
    return FALSE;
  }

  NameLength = AsciiStrLen (Channel->Name);
  Line = Response;
  while ((Line = AsciiStrStr (Line, Channel->Name)) != NULL) {
    if (((Line == Response) || (Line[-1] == '\n') || (Line[-1] == '\r') ||
         (Line[-1] == ' ')) &&
        (Line[NameLength] == ' '))
    {
      break;
    }

    Line++;
  }

  if (Line == NULL) {
    return FALSE;
  }

  Cursor = Line + NameLength;
  while ((*Cursor != '\0') && (*Cursor != '\n') && (*Cursor != '(')) {
    Cursor++;
  }

  if (*Cursor++ != '(') {
    return FALSE;
  }

  ParsedId = 0;
  HaveDigit = FALSE;
  while ((*Cursor >= '0') && (*Cursor <= '9')) {
    HaveDigit = TRUE;
    ParsedId = (ParsedId * 10) + (*Cursor++ - '0');
  }

  if (!HaveDigit || (*Cursor++ != ')') || (ParsedId != Channel->Id)) {
    return FALSE;
  }

  while ((*Cursor != '\0') && (*Cursor != '\n') && (*Cursor != '=')) {
    Cursor++;
  }

  if (*Cursor++ != '=') {
    return FALSE;
  }

  Integer = 0;
  HaveDigit = FALSE;
  while ((*Cursor >= '0') && (*Cursor <= '9')) {
    HaveDigit = TRUE;
    Integer = (Integer * 10) + (*Cursor++ - '0');
  }

  if (!HaveDigit) {
    return FALSE;
  }

  Fraction = 0;
  FractionDigits = 0;
  RoundUp = FALSE;
  if (*Cursor == '.') {
    Cursor++;
    while ((*Cursor >= '0') && (*Cursor <= '9')) {
      if (FractionDigits < 6) {
        Fraction = (Fraction * 10) + (*Cursor - '0');
      } else if ((FractionDigits == 6) && (*Cursor >= '5')) {
        RoundUp = TRUE;
      }

      FractionDigits++;
      Cursor++;
    }
  }

  while (FractionDigits < 6) {
    Fraction *= 10;
    FractionDigits++;
  }

  if (*Cursor != (Channel->IsVoltage ? 'V' : 'A')) {
    return FALSE;
  }

  Result = (Integer * 1000000ULL) + Fraction + (RoundUp ? 1 : 0);
  if (Result > MAX_UINT32) {
    return FALSE;
  }

  *Value = (UINT32)Result;
  return TRUE;
}

STATIC
BOOLEAN
GetFdtPowerProperty (
  IN  CONST CHAR8  *PropertyName,
  OUT UINT32       *Value
  )
{
  CONST UINT32  *Property;
  VOID          *Fdt;
  INT32         Length;
  INT32         Node;

  Fdt = FdtPlatformGetBase ();
  if (Fdt == NULL) {
    return FALSE;
  }

  Node = FdtPathOffset (Fdt, "/chosen/power");
  if (Node < 0) {
    return FALSE;
  }

  Property = FdtGetProp (Fdt, Node, PropertyName, &Length);
  if ((Property == NULL) || (Length < sizeof (*Property))) {
    return FALSE;
  }

  *Value = Fdt32ToCpu (*Property);
  return TRUE;
}

STATIC
VOID
DsdtFixupFirmwareTelemetry (
  IN EFI_ACPI_SDT_PROTOCOL  *AcpiSdtProtocol,
  IN EFI_ACPI_HANDLE        TableHandle
  )
{
  CHAR8                           PmicResponse[PMIC_ADC_RESPONSE_SIZE];
  BOOLEAN                         HasPowerPair;
  EFI_PHYSICAL_ADDRESS            BufferAddress;
  EFI_PHYSICAL_ADDRESS            MailboxAddress;
  EFI_STATUS                      Status;
  RASPBERRY_PI_FIRMWARE_PROTOCOL  *Firmware;
  UINT32                          Id;
  UINT32                          PmicCurrentMask;
  UINT32                          PmicVoltageMask;
  UINT32                          PowerReset;
  UINT32                          PsuMaxCurrent;
  UINT32                          PsuMaxPower;
  UINT32                          RtcMask;
  UINT32                          Temperature;
  UINT32                          TemperatureMask;
  UINT32                          UsbHighCurrent;
  UINT32                          UsbOverCurrent;
  UINT32                          Value;
  UINT32                          VoltageMask;
  UINTN                           BufferSize;
  UINTN                           Index;
  UINTN                           MailboxBusAddress;

  Status = gBS->LocateProtocol (
                  &gRaspberryPiFirmwareProtocolGuid,
                  NULL,
                  (VOID **)&Firmware
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "%a: Firmware protocol unavailable. Status=%r\n", __func__, Status));
    return;
  }

  Status = Firmware->GetMailboxBuffer (
                       &BufferAddress,
                       &MailboxBusAddress,
                       &BufferSize,
                       &MailboxAddress
                       );
  if (EFI_ERROR (Status) ||
      (BufferSize < EFI_PAGE_SIZE) ||
      ((BufferAddress & (sizeof (UINT32) - 1)) != 0) ||
      ((MailboxBusAddress & 0xF) != 0) ||
      (MailboxBusAddress > MAX_UINT32))
  {
    DEBUG ((DEBUG_WARN, "%a: Firmware mailbox buffer is not ACPI-safe\n", __func__));
    return;
  }

  // The public property interface defines exactly one temperature sensor,
  // ID 0. Some firmware versions ignore the ID and echo a SoC reading for any
  // value, so probing arbitrary IDs creates phantom sensors.
  TemperatureMask = 0;
  Status = Firmware->GetTemperature (TELEMETRY_TEMPERATURE_ID, &Temperature);
  if (!EFI_ERROR (Status) && (Temperature >= 1000) &&
      (Temperature <= 150000))
  {
    TemperatureMask = 1U << TELEMETRY_TEMPERATURE_ID;
    DEBUG ((DEBUG_INFO, "%a: Firmware SoC temperature is %u mC\n", __func__, Temperature));
  }

  VoltageMask = 0;
  for (Id = TELEMETRY_MIN_VOLTAGE_ID;
       Id <= TELEMETRY_MAX_VOLTAGE_ID;
       Id++)
  {
    Status = Firmware->GetVoltage (Id, &Value);
    if (!EFI_ERROR (Status) && (Value >= TELEMETRY_MIN_VOLTAGE_UV) &&
        (Value <= TELEMETRY_MAX_VOLTAGE_UV))
    {
      VoltageMask |= 1U << Id;
      DEBUG ((DEBUG_INFO, "%a: Firmware voltage %u is %u uV\n", __func__, Id, Value));
    }
  }

  RtcMask = 0;
  for (Id = RpiRtcBatteryChargeVoltage;
       Id <= RpiRtcBatteryVoltage;
       Id++)
  {
    Status = Firmware->GetRtc ((RASPBERRY_PI_RTC_REGISTER)Id, &Value);
    if (!EFI_ERROR (Status)) {
      RtcMask |= 1U << Id;
      DEBUG ((DEBUG_INFO, "%a: RTC telemetry register %u is %u\n", __func__, Id, Value));
    }
  }

  PmicCurrentMask = 0;
  PmicVoltageMask = 0;
  ZeroMem (PmicResponse, sizeof (PmicResponse));
  Status = Firmware->GetGencmd (
                       "pmic_read_adc",
                       PmicResponse,
                       sizeof (PmicResponse)
                       );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "%a: PMIC ADC telemetry unavailable. Status=%r\n", __func__, Status));
  } else {
    for (Index = 0; Index < ARRAY_SIZE (mPmicAdcChannels); Index++) {
      if (!ParsePmicAdcValue (PmicResponse, &mPmicAdcChannels[Index], &Value)) {
        continue;
      }

      if (mPmicAdcChannels[Index].IsVoltage) {
        if (Value > PMIC_ADC_MAX_VOLTAGE_UV) {
          continue;
        }

        PmicVoltageMask |= 1U << mPmicAdcChannels[Index].Id;
      } else {
        if (Value > PMIC_ADC_MAX_CURRENT_UA) {
          continue;
        }

        PmicCurrentMask |= 1U << mPmicAdcChannels[Index].Id;
      }

      DEBUG ((
        DEBUG_INFO,
        "%a: PMIC %-12a ID %u is %u u%c\n",
        __func__,
        mPmicAdcChannels[Index].Name,
        mPmicAdcChannels[Index].Id,
        Value,
        mPmicAdcChannels[Index].IsVoltage ? 'V' : 'A'
        ));
    }
  }

  HasPowerPair = FALSE;
  for (Index = 0; Index < ARRAY_SIZE (mPmicAdcPowerPairs); Index++) {
    if (((PmicCurrentMask & (1U << mPmicAdcPowerPairs[Index][0])) != 0) &&
        ((PmicVoltageMask & (1U << mPmicAdcPowerPairs[Index][1])) != 0))
    {
      HasPowerPair = TRUE;
      break;
    }
  }

  PsuMaxCurrent = MAX_UINT32;
  PsuMaxPower = MAX_UINT32;
  if (GetFdtPowerProperty ("max_current", &Value) &&
      (Value <= (MAX_UINT32 / 5)))
  {
    PsuMaxCurrent = Value;
    // Raspberry Pi 5 consumes the fixed 5 V PDO; mA * 5 gives mW.
    PsuMaxPower = Value * 5;
  }

  PowerReset = MAX_UINT32;
  if (!GetFdtPowerProperty ("power_reset", &PowerReset)) {
    GetFdtPowerProperty ("reset_event", &PowerReset);
  }

  UsbHighCurrent = MAX_UINT32;
  GetFdtPowerProperty ("usb_max_current_enable", &UsbHighCurrent);
  UsbOverCurrent = MAX_UINT32;
  GetFdtPowerProperty ("usb_over_current_detected", &UsbOverCurrent);

  if (EFI_ERROR (PatchDsdtInteger (AcpiSdtProtocol, TableHandle, "\\_SB.MBPA", MailboxAddress)) ||
      EFI_ERROR (PatchDsdtInteger (AcpiSdtProtocol, TableHandle, "\\_SB.MBCA", BufferAddress)) ||
      EFI_ERROR (PatchDsdtInteger (AcpiSdtProtocol, TableHandle, "\\_SB.MBBA", MailboxBusAddress)) ||
      EFI_ERROR (PatchDsdtInteger (AcpiSdtProtocol, TableHandle, "\\_SB.TMLO", TemperatureMask)) ||
      EFI_ERROR (PatchDsdtInteger (AcpiSdtProtocol, TableHandle, "\\_SB.VMLO", VoltageMask)) ||
      EFI_ERROR (PatchDsdtInteger (AcpiSdtProtocol, TableHandle, "\\_SB.RTCP", RtcMask)) ||
      EFI_ERROR (PatchDsdtInteger (AcpiSdtProtocol, TableHandle, "\\_SB.PVLO", PmicVoltageMask)) ||
      EFI_ERROR (PatchDsdtInteger (AcpiSdtProtocol, TableHandle, "\\_SB.PILO", PmicCurrentMask)) ||
      EFI_ERROR (PatchDsdtInteger (AcpiSdtProtocol, TableHandle, "\\_SB.PMEN", HasPowerPair ? 1 : 0)) ||
      EFI_ERROR (PatchDsdtInteger (AcpiSdtProtocol, TableHandle, "\\_SB.PSMW", PsuMaxPower)) ||
      EFI_ERROR (PatchDsdtInteger (AcpiSdtProtocol, TableHandle, "\\_SB.PSMC", PsuMaxCurrent)) ||
      EFI_ERROR (PatchDsdtInteger (AcpiSdtProtocol, TableHandle, "\\_SB.PSRR", PowerReset)) ||
      EFI_ERROR (PatchDsdtInteger (AcpiSdtProtocol, TableHandle, "\\_SB.PSUH", UsbHighCurrent)) ||
      EFI_ERROR (PatchDsdtInteger (AcpiSdtProtocol, TableHandle, "\\_SB.PSOC", UsbOverCurrent)))
  {
    return;
  }

  Status = PatchDsdtInteger (AcpiSdtProtocol, TableHandle, "\\_SB.MBEN", 1);
  if (!EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_INFO,
      "%a: Firmware telemetry exposed: temperature=0x%x voltage=0x%x rtc=0x%x pmic-v=0x%x pmic-a=0x%x\n",
      __func__,
      TemperatureMask,
      VoltageMask,
      RtcMask,
      PmicVoltageMask,
      PmicCurrentMask
      ));
    DEBUG ((
      DEBUG_INFO,
      "%a: Power source: max-current=%u mA max-power=%u mW reset=0x%x usb-high=%u usb-overcurrent=%u\n",
      __func__,
      PsuMaxCurrent,
      PsuMaxPower,
      PowerReset,
      UsbHighCurrent,
      UsbOverCurrent
      ));
  }
}

STATIC
VOID
EFIAPI
DsdtFixupPcie (
  IN EFI_ACPI_SDT_PROTOCOL    *AcpiSdtProtocol,
  IN EFI_ACPI_HANDLE          TableHandle
  )
{
  EFI_STATUS Status;

  Status = AcpiAmlObjectUpdateInteger (AcpiSdtProtocol, TableHandle,
                "\\_SB.BB32", mAcpiPciMem32Base);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: Failed to patch BB32.\n", __func__));
  }

  Status = AcpiAmlObjectUpdateInteger (AcpiSdtProtocol, TableHandle,
                "\\_SB.MS32", mAcpiPciMem32Size);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: Failed to patch MS32.\n", __func__));
  }
}

STATIC
EFI_STATUS
EFIAPI
AcpiFixupPcieEcam (
  IN ACPI_OS_BOOT_TYPE  OsType
  )
{
  EFI_STATUS                    Status;
  UINTN                         Index;
  RPI5_MCFG_TABLE               *McfgTable;
  EFI_ACPI_DESCRIPTION_HEADER   *FadtTable;
  UINTN                         TableKey;
  UINT32                        PcieEcamMode;
  UINT8                         PcieBusMax;

  Index = 0;
  Status = AcpiLocateTableBySignature (
             mAcpiSdtProtocol,
             EFI_ACPI_6_4_PCI_EXPRESS_MEMORY_MAPPED_CONFIGURATION_SPACE_BASE_ADDRESS_DESCRIPTION_TABLE_SIGNATURE,
             &Index,
             (EFI_ACPI_DESCRIPTION_HEADER **)&McfgTable,
             &TableKey);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: Couldn't locate ACPI MCFG table! Status=%r\n",
            __func__, Status));
    return Status;
  }

  PcieEcamMode = AcpiPcieEcamCompatMode.Value;

  if (PcieEcamMode == ACPI_PCIE_ECAM_COMPAT_MODE_NXPMX6_DEN0115 ||
      PcieEcamMode == ACPI_PCIE_ECAM_COMPAT_MODE_NXPMX6_GRAVITON) {
    if (OsType == AcpiOsWindows) {
      PcieEcamMode = ACPI_PCIE_ECAM_COMPAT_MODE_NXPMX6;
    } else {
      PcieEcamMode &= ~ACPI_PCIE_ECAM_COMPAT_MODE_NXPMX6;
    }
  }

  switch (PcieEcamMode) {
    case ACPI_PCIE_ECAM_COMPAT_MODE_NXPMX6:
      PcieBusMax = 0;

      Index = 0;
      Status = AcpiLocateTableBySignature (
                mAcpiSdtProtocol,
                EFI_ACPI_6_3_FIXED_ACPI_DESCRIPTION_TABLE_SIGNATURE,
                &Index,
                &FadtTable,
                &TableKey);
      if (EFI_ERROR (Status)) {
        DEBUG ((DEBUG_ERROR, "%a: Couldn't locate ACPI FADT table! Status=%r\n",
                __func__, Status));
        return Status;
      }

      CopyMem (FadtTable->OemId, "NXPMX6", sizeof (FadtTable->OemId));
      AcpiUpdateChecksum ((UINT8 *)FadtTable, FadtTable->Length);
      break;

    case ACPI_PCIE_ECAM_COMPAT_MODE_GRAVITON:
      PcieBusMax = 0;

      CopyMem (McfgTable->Header.Header.OemId, "AMAZON", sizeof (McfgTable->Header.Header.OemId));
      McfgTable->Header.Header.OemTableId = SIGNATURE_64 ('G','R','A','V','I','T','O','N');
      McfgTable->Header.Header.OemRevision = 0;

      //
      // The ECAM window of the single function exposed on bus 0 is obtained
      // from the "AMZN0001" device in DSDT.
      // This causes a conflict with the region described in MCFG, but since
      // the latter is superfluous, we can simply point it to a bogus region
      // way above the register space.
      //
      for (Index = 0; Index < ARRAY_SIZE (McfgTable->Entries); Index++) {
        McfgTable->Entries[Index].BaseAddress = BASE_1TB + (Index * SIZE_1MB);
      }
      break;

    default: // ACPI_PCIE_ECAM_COMPAT_MODE_DEN0115
      PcieBusMax = PCI_MAX_BUS;

      // MCFG must be hidden.
      McfgTable->Header.Header.Signature = 0;
      break;
  }

  AcpiUpdateChecksum ((UINT8 *)McfgTable, McfgTable->Header.Header.Length);

  AcpiUpdateSdtNameInteger (mDsdtTable, "PBMA", PcieBusMax);

  return EFI_SUCCESS;
}

STATIC
VOID
EFIAPI
AcpiOsBootHandler (
  IN ACPI_OS_BOOT_TYPE  OsType
  )
{
  if ((mAcpiSdtProtocol == NULL) || (mDsdtTable == NULL)) {
    ASSERT (FALSE);
    return;
  }

  AcpiFixupPcieEcam (OsType);

  AcpiUpdateChecksum ((UINT8 *)mDsdtTable, mDsdtTable->Length);
}

STATIC
UINTN
EFIAPI
FindPeImageBase (
  EFI_PHYSICAL_ADDRESS  Base
  )
{
  EFI_IMAGE_DOS_HEADER                 *DosHdr;
  EFI_IMAGE_OPTIONAL_HEADER_PTR_UNION  Hdr;

  Base &= ~(EFI_PAGE_SIZE - 1);

  while (Base != 0) {
    DosHdr = (EFI_IMAGE_DOS_HEADER *)Base;
    if (DosHdr->e_magic == EFI_IMAGE_DOS_SIGNATURE) {
      Hdr.Pe32 = (EFI_IMAGE_NT_HEADERS32 *)(Base + DosHdr->e_lfanew);
      if (Hdr.Pe32->Signature == EFI_IMAGE_NT_SIGNATURE) {
        break;
      }
    }

    Base -= EFI_PAGE_SIZE;
  }

  return Base;
}

STATIC CHAR8 mWinLoadNameStr[] = "winload";
STATIC CHAR8 mFreeLdrNameStr[] = "FreeLoader";
#define PDB_NAME_MAX_LENGTH   256

STATIC
BOOLEAN
EFIAPI
IsPeImageWinLoader (
  IN VOID *PeImage
 )
{
  CHAR8  *PdbStr;
  UINTN  WinLoadNameStrLen;
  UINTN  Index;

  PdbStr = (CHAR8 *)PeCoffLoaderGetPdbPointer (PeImage);
  if (PdbStr == NULL) {
    return FALSE;
  }

  WinLoadNameStrLen = sizeof (mWinLoadNameStr) - sizeof (CHAR8);

  for (Index = 0; Index < PDB_NAME_MAX_LENGTH && PdbStr[Index] != '\0'; Index++) {
    if (AsciiStrnCmp (PdbStr + Index, mWinLoadNameStr, WinLoadNameStrLen) == 0) {
      return TRUE;
    }
  }

  return FALSE;
}

//
// FreeLoader leaves the CodeView PDB name empty. Match its product name in the
// loaded image instead.
//
STATIC
BOOLEAN
EFIAPI
IsPeImageFreeLoader (
  IN VOID *PeImage
  )
{
  EFI_IMAGE_DOS_HEADER                 *DosHdr;
  EFI_IMAGE_OPTIONAL_HEADER_PTR_UNION  Hdr;
  CONST UINT8                          *Image;
  UINT32                               SizeOfImage;
  UINTN                                NameLen;
  UINTN                                Index;

  DosHdr = (EFI_IMAGE_DOS_HEADER *)PeImage;
  if (DosHdr->e_magic != EFI_IMAGE_DOS_SIGNATURE) {
    return FALSE;
  }

  Hdr.Pe32 = (EFI_IMAGE_NT_HEADERS32 *)((UINT8 *)PeImage + DosHdr->e_lfanew);
  if (Hdr.Pe32->Signature != EFI_IMAGE_NT_SIGNATURE) {
    return FALSE;
  }

  if (Hdr.Pe32->OptionalHeader.Magic == EFI_IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
    SizeOfImage = Hdr.Pe32Plus->OptionalHeader.SizeOfImage;
  } else {
    SizeOfImage = Hdr.Pe32->OptionalHeader.SizeOfImage;
  }

  NameLen = sizeof (mFreeLdrNameStr) - sizeof (CHAR8);
  if (SizeOfImage < NameLen) {
    return FALSE;
  }

  Image = (CONST UINT8 *)PeImage;
  for (Index = 0; Index <= SizeOfImage - NameLen; Index++) {
    if (CompareMem (Image + Index, mFreeLdrNameStr, NameLen) == 0) {
      return TRUE;
    }
  }

  return FALSE;
}

STATIC
EFI_STATUS
EFIAPI
AcpiExitBootServicesHook (
  IN EFI_HANDLE  ImageHandle,
  IN UINTN       MapKey
  )
{
  UINTN               ReturnAddress;
  UINTN               OsLoaderAddress;
  ACPI_OS_BOOT_TYPE   OsType;

  ReturnAddress = (UINTN)RETURN_ADDRESS (0);

  gBS->ExitBootServices = mOriginalExitBootServices;

  OsType = AcpiOsUnknown;

  OsLoaderAddress = FindPeImageBase (ReturnAddress);
  if (OsLoaderAddress > 0) {
    if (IsPeImageWinLoader ((VOID *)OsLoaderAddress) ||
        IsPeImageFreeLoader ((VOID *)OsLoaderAddress)) {
      OsType = AcpiOsWindows;
    }
  }

  AcpiOsBootHandler (OsType);

  return gBS->ExitBootServices (ImageHandle, MapKey);
}

STATIC
EFI_STATUS
EFIAPI
GetPciMem32TotalRange (
  OUT UINT64  *Base,
  OUT UINT64  *Size
  )
{
  EFI_STATUS           Status;
  UINTN                Index;
  EFI_HANDLE           *Handles;
  UINTN                HandleCount;
  EFI_PCI_IO_PROTOCOL  *PciIo;
  PCI_TYPE01           PciConfigHeader;
  UINT32               MemoryBase;
  UINT32               MinimumMemoryBase;
  UINT32               MemoryLimit;
  UINT32               MaximumMemoryLimit;

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiPciIoProtocolGuid,
                  NULL,
                  &HandleCount,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  MinimumMemoryBase  = MAX_UINT32;
  MaximumMemoryLimit = 0;

  for (Index = 0; Index < HandleCount; Index++) {
    Status = gBS->HandleProtocol (
                    Handles[Index],
                    &gEfiPciIoProtocolGuid,
                    (VOID **)&PciIo
                    );
    ASSERT_EFI_ERROR (Status);
    if (EFI_ERROR (Status)) {
      continue;
    }

    Status = PciIo->Pci.Read (
                          PciIo,
                          EfiPciIoWidthUint32,
                          0,
                          sizeof (PciConfigHeader) / sizeof (UINT32),
                          &PciConfigHeader
                          );
    if (EFI_ERROR (Status) ||
        (!IS_PCI_P2P (&PciConfigHeader) &&
         !IS_PCI_P2P_SUB (&PciConfigHeader)))
    {
      continue;
    }

    MemoryBase = 0;
    PciIo->Pci.Read (
                 PciIo,
                 EfiPciIoWidthUint16,
                 OFFSET_OF (PCI_TYPE01, Bridge.MemoryBase),
                 1,
                 &MemoryBase
                 );
    MemoryBase <<= 16;

    if (MinimumMemoryBase > MemoryBase) {
      MinimumMemoryBase = MemoryBase;
    }

    MemoryLimit = 0;
    PciIo->Pci.Read (
                 PciIo,
                 EfiPciIoWidthUint16,
                 OFFSET_OF (PCI_TYPE01, Bridge.MemoryLimit),
                 1,
                 &MemoryLimit
                 );
    MemoryLimit <<= 16;

    if (MaximumMemoryLimit < MemoryLimit) {
      MaximumMemoryLimit = MemoryLimit;
    }
  }

  FreePool (Handles);

  if (MaximumMemoryLimit == 0) {
    return EFI_NOT_FOUND;
  }

  *Base = MinimumMemoryBase;
  *Size = (MaximumMemoryLimit + SIZE_1MB) - MinimumMemoryBase;

  return EFI_SUCCESS;
}

//
// See Bcm2712PciHostBridgeLib.c for more details.
//
STATIC
VOID
EFIAPI
AdjustPciReservedMemory (
  VOID
  )
{
  EFI_STATUS  Status;
  UINT64      MemoryToReclaimBase;
  UINT64      MemoryToReclaimSize;
  UINT64      PciMem32Base;
  UINT64      PciMem32Size;
  UINT64      PciMem32PreferredSize;

  //
  // Initially, we wish to reclaim all system RAM and disable
  // PCI 32-bit memory if possible.
  //
  MemoryToReclaimBase = PCI_RESERVED_MEM32_BASE;
  MemoryToReclaimSize = PCI_RESERVED_MEM32_SIZE;

  mAcpiPciMem32Base = MemoryToReclaimBase;
  mAcpiPciMem32Size = 0;

  //
  // Compute the reserved memory size for ACPI boot.
  // FDT uses DMA translation and does not need any reserved RAM.
  //
  if (mIsAcpiEnabled) {
    Status = GetPciMem32TotalRange (&PciMem32Base, &PciMem32Size);
    if (EFI_ERROR (Status)) {
      DEBUG ((
        DEBUG_ERROR,
        "%a: Failed to get Mem32 region. Status=%r\n",
        __func__,
        Status
        ));

      PciMem32Base = mAcpiPciMem32Base;
      PciMem32Size = mAcpiPciMem32Size;
    }

    DEBUG ((
      DEBUG_INFO,
      "%a: Mem32 Base: 0x%lx, Size: 0x%lx\n",
      __func__,
      PciMem32Base,
      PciMem32Size
      ));

    if ((PciMem32Base < MemoryToReclaimBase) || (PciMem32Size > MemoryToReclaimSize)) {
      ASSERT (FALSE);
      DEBUG ((DEBUG_ERROR, "%a: Mem32 region out of reserved bounds!", __func__));
      goto ReclaimMemoryExit;
    }

    PciMem32PreferredSize = AcpiPcie32BitBarSpaceSizeMB.Value * 1024 * 1024;
    if (PciMem32PreferredSize <= MemoryToReclaimSize) {
      if (PciMem32Size < PciMem32PreferredSize) {
        DEBUG ((
          DEBUG_INFO,
          "%a: Mem32 Preferred Size: 0x%lx\n",
          __func__,
          PciMem32PreferredSize
          ));

        PciMem32Size = PciMem32PreferredSize;
      }
    } else {
      DEBUG ((
        DEBUG_ERROR,
        "%a: Mem32 Preferred Size too large: 0x%lx\n",
        __func__,
        PciMem32PreferredSize
        ));
    }

    mAcpiPciMem32Base = PciMem32Base;
    mAcpiPciMem32Size = PciMem32Size;

    if (PciMem32Size > 0) {
      MemoryToReclaimBase = PciMem32Base + PciMem32Size;
      MemoryToReclaimSize = SIZE_4GB - MemoryToReclaimBase;
    }
  }

ReclaimMemoryExit:
  if ((mSystemMemorySize > PCI_RESERVED_MEM32_BASE) && (MemoryToReclaimSize > 0)) {
    DEBUG ((
      DEBUG_INFO,
      "%a: Reclaiming system RAM - Base: 0x%lx, Size: 0x%lx\n",
      __func__,
      MemoryToReclaimBase,
      MemoryToReclaimSize
      ));

    Status = gDS->AddMemorySpace (
                    EfiGcdMemoryTypeSystemMemory,
                    MemoryToReclaimBase,
                    MemoryToReclaimSize,
                    EFI_MEMORY_WC | EFI_MEMORY_WT | EFI_MEMORY_WB
                    );
    ASSERT_EFI_ERROR (Status);
    if (EFI_ERROR (Status)) {
      return;
    }

    Status = gDS->SetMemorySpaceAttributes (
                    MemoryToReclaimBase,
                    MemoryToReclaimSize,
                    EFI_MEMORY_WB
                    );
    ASSERT_EFI_ERROR (Status);
  }
}

STATIC
EFI_STATUS
EFIAPI
InstallAcpiTables (
  VOID
  )
{
  EFI_STATUS       Status;
  UINTN            TableKey;
  UINTN            TableIndex;
  EFI_ACPI_HANDLE  TableHandle;

  Status = gBS->LocateProtocol (
                  &gEfiAcpiSdtProtocolGuid,
                  NULL,
                  (VOID **)&mAcpiSdtProtocol
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: Couldn't locate gEfiAcpiSdtProtocolGuid!\n", __func__));
    return Status;
  }

  Status = LocateAndInstallAcpiFromFvConditional (&mAcpiTableFile, NULL);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: Failed to install ACPI tables!\n"));
    return Status;
  }

  TableIndex = 0;
  Status = AcpiLocateTableBySignature (
             mAcpiSdtProtocol,
             EFI_ACPI_6_3_DIFFERENTIATED_SYSTEM_DESCRIPTION_TABLE_SIGNATURE,
             &TableIndex,
             &mDsdtTable,
             &TableKey
             );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: Couldn't locate ACPI DSDT table!\n", __func__));
    return Status;
  }

  Status = mAcpiSdtProtocol->OpenSdt (TableKey, &TableHandle);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: Couldn't open ACPI DSDT table!\n", __func__));
    mAcpiSdtProtocol->Close (TableHandle);
    return Status;
  }

  DsdtFixupStatus (mAcpiSdtProtocol, TableHandle);
  DsdtFixupDisplay (mAcpiSdtProtocol, TableHandle);
  DsdtFixupSd (mAcpiSdtProtocol, TableHandle);
  DsdtFixupRp1 (mAcpiSdtProtocol, TableHandle);
  DsdtFixupFirmwareTelemetry (mAcpiSdtProtocol, TableHandle);
  DsdtFixupPcie (mAcpiSdtProtocol, TableHandle);

  mAcpiSdtProtocol->Close (TableHandle);

  return EFI_SUCCESS;
}

STATIC
VOID
EFIAPI
OnReadyToBoot (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  gBS->CloseEvent (Event);

  AdjustPciReservedMemory ();

  if (mIsAcpiEnabled) {
    InstallAcpiTables ();
  } else {
    // FDT installation is done by FdtDxe.
  }
}

VOID
EFIAPI
ApplyConfigTableVariables (
  VOID
  )
{
  EFI_STATUS  Status;
  EFI_EVENT   Event;

  mIsAcpiEnabled = PcdGet32 (PcdSystemTableMode) == SYSTEM_TABLE_MODE_ACPI ||
                   PcdGet32 (PcdSystemTableMode) == SYSTEM_TABLE_MODE_BOTH;

  Status = gBS->CreateEventEx (
                  EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  OnReadyToBoot,
                  NULL,
                  &gEfiEventReadyToBootGuid,
                  &Event
                  );
  ASSERT_EFI_ERROR (Status);

  if (mIsAcpiEnabled) {
    mOriginalExitBootServices = gBS->ExitBootServices;
    gBS->ExitBootServices = AcpiExitBootServicesHook;
  }
}

VOID
EFIAPI
SetupConfigTableVariables (
  VOID
  )
{
  EFI_STATUS    Status;
  UINTN         Size;
  UINT32        Var32;

  AcpiSdCompatMode.Value = ACPI_SD_COMPAT_MODE_DEFAULT;
  AcpiSdLimitUhs.Value = ACPI_SD_LIMIT_UHS_DEFAULT;
  AcpiPcieEcamCompatMode.Value = ACPI_PCIE_ECAM_COMPAT_MODE_DEFAULT;
  AcpiPcie32BitBarSpaceSizeMB.Value = ACPI_PCIE_32_BIT_BAR_SPACE_SIZE_MB_DEFAULT;

  Size = sizeof (ACPI_SD_COMPAT_MODE_VARSTORE_DATA);
  Status = gRT->GetVariable (L"AcpiSdCompatMode",
                  &gRpiPlatformFormSetGuid,
                  NULL, &Size, &AcpiSdCompatMode);
  if (EFI_ERROR (Status)) {
    Status = gRT->SetVariable (
                    L"AcpiSdCompatMode",
                    &gRpiPlatformFormSetGuid,
                    EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS,
                    Size,
                    &AcpiSdCompatMode);
    ASSERT_EFI_ERROR (Status);
  }

  Size = sizeof (ACPI_SD_LIMIT_UHS_VARSTORE_DATA);
  Status = gRT->GetVariable (L"AcpiSdLimitUhs",
                  &gRpiPlatformFormSetGuid,
                  NULL, &Size, &AcpiSdLimitUhs);
  if (EFI_ERROR (Status)) {
    Status = gRT->SetVariable (
                    L"AcpiSdLimitUhs",
                    &gRpiPlatformFormSetGuid,
                    EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS,
                    Size,
                    &AcpiSdLimitUhs);
    ASSERT_EFI_ERROR (Status);
  }

  Size = sizeof (ACPI_PCIE_ECAM_COMPAT_MODE_VARSTORE_DATA);
  Status = gRT->GetVariable (L"AcpiPcieEcamCompatMode",
                  &gRpiPlatformFormSetGuid,
                  NULL, &Size, &AcpiPcieEcamCompatMode);
  if (EFI_ERROR (Status)) {
    Status = gRT->SetVariable (
                    L"AcpiPcieEcamCompatMode",
                    &gRpiPlatformFormSetGuid,
                    EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS,
                    Size,
                    &AcpiPcieEcamCompatMode);
    ASSERT_EFI_ERROR (Status);
  }

  Size = sizeof (ACPI_PCIE_32_BIT_BAR_SPACE_SIZE_MB_VARSTORE_DATA);
  Status = gRT->GetVariable (L"AcpiPcie32BitBarSpaceSizeMB",
                  &gRpiPlatformFormSetGuid,
                  NULL, &Size, &AcpiPcie32BitBarSpaceSizeMB);
  if (EFI_ERROR (Status)) {
    Status = gRT->SetVariable (
                    L"AcpiPcie32BitBarSpaceSizeMB",
                    &gRpiPlatformFormSetGuid,
                    EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS,
                    Size,
                    &AcpiPcie32BitBarSpaceSizeMB);
    ASSERT_EFI_ERROR (Status);
  }

  Size = sizeof (UINT32);
  Status = gRT->GetVariable (L"SystemTableMode",
                  &gRpiPlatformFormSetGuid,
                  NULL, &Size, &Var32);
  if (EFI_ERROR (Status)) {
    Status = PcdSet32S (PcdSystemTableMode, PcdGet32 (PcdSystemTableMode));
    ASSERT_EFI_ERROR (Status);
  }
}

/** @file
  Low-level RP1 DISP1, DSI host, D-PHY and display DMA support.

  The register programming follows the publicly available RP1 Linux driver,
  but is implemented here specifically for the fixed 720x1280 firmware mode.

  Copyright (c) 2026, Ahmed Ghanem. All rights reserved.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Library/BaseLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Rp1.h>

#include "Rp1DsiGopDxe.h"

#define RP1_PANEL_MCU_ADDRESS              0x45
#define RP1_PANEL_MCU_ID_REGISTER          0x01
#define RP1_PANEL_MCU_POWER_REGISTER       0x02

// RP1 bank 2 starts at GPIO34; GPIO40/41 are local pins 6/7 (I2C4 fsel 2).
#define RP1_DISP1_SDA_BANK_PIN             6U
#define RP1_DISP1_SCL_BANK_PIN             7U
#define RP1_GPIO_CTRL(Pin)                 (0x04U + ((Pin) * 8U))
#define RP1_PAD_CTRL(Pin)                  (0x04U + ((Pin) * 4U))
#define RP1_GPIO_FUNCSEL_MASK              0x0000001fU
#define RP1_GPIO_OVERRIDE_MASK             0x0003f000U
#define RP1_GPIO_FSEL_I2C4                 2U
#define RP1_PAD_PULL_MASK                  0x0000000cU
#define RP1_PAD_PULL_UP                    (2U << 2)
#define RP1_PAD_DRIVE_MASK                 0x00000030U
#define RP1_PAD_DRIVE_12MA                 (3U << 4)
#define RP1_PAD_INPUT_ENABLE               BIT6
#define RP1_PAD_OUTPUT_DISABLE             BIT7

// RP1 RIO atomic aliases. OE=1 drives OUT; OE=0 releases the pin.
#define RP1_RIO_OUT                        0x0000
#define RP1_RIO_OE                         0x0004
#define RP1_RIO_IN                         0x0008
#define RP1_RIO_SET                        0x2000
#define RP1_RIO_CLR                        0x3000
#define RP1_GPIO_FSEL_GPIO                 5U
#define RP1_DISP1_SDA_BIT                  BIT6
#define RP1_DISP1_SCL_BIT                  BIT7
#define RP1_DISP1_I2C_BITS                 (RP1_DISP1_SDA_BIT | RP1_DISP1_SCL_BIT)

// Synopsys DesignWare I2C registers.
#define DW_I2C_CON                         0x000
#define DW_I2C_TAR                         0x004
#define DW_I2C_DATA_CMD                    0x010
#define DW_I2C_SS_SCL_HCNT                 0x014
#define DW_I2C_SS_SCL_LCNT                 0x018
#define DW_I2C_INTR_MASK                   0x030
#define DW_I2C_RAW_INTR_STAT               0x034
#define DW_I2C_RX_TL                       0x038
#define DW_I2C_TX_TL                       0x03c
#define DW_I2C_CLR_INTR                    0x040
#define DW_I2C_CLR_TX_ABRT                 0x054
#define DW_I2C_CLR_STOP_DET                0x060
#define DW_I2C_ENABLE                      0x06c
#define DW_I2C_STATUS                      0x070
#define DW_I2C_TXFLR                       0x074
#define DW_I2C_RXFLR                       0x078
#define DW_I2C_SDA_HOLD                    0x07c
#define DW_I2C_TX_ABRT_SOURCE              0x080
#define DW_I2C_ENABLE_STATUS               0x09c
#define DW_I2C_COMP_TYPE                   0x0fc
#define DW_I2C_SMBUS_INTR_MASK             0x0cc

#define DW_I2C_COMP_TYPE_VALUE             0x44570140U
#define DW_I2C_CON_MASTER                  BIT0
#define DW_I2C_CON_SPEED_STANDARD          BIT1
#define DW_I2C_CON_RESTART_ENABLE          BIT5
#define DW_I2C_CON_SLAVE_DISABLE           BIT6
#define DW_I2C_DATA_READ                   BIT8
#define DW_I2C_DATA_STOP                   BIT9
#define DW_I2C_DATA_RESTART                BIT10
#define DW_I2C_RAW_TX_ABRT                 BIT6
#define DW_I2C_RAW_STOP_DET                BIT9
#define DW_I2C_RAW_MST_ON_HOLD             BIT13
#define DW_I2C_ENABLE_CONTROLLER           BIT0
#define DW_I2C_ENABLE_ABORT                BIT1
#define DW_I2C_STATUS_ACTIVITY             BIT0
#define DW_I2C_STATUS_TFNF                 BIT1
#define DW_I2C_STATUS_MASTER_ACTIVITY      BIT5
#define DW_I2C_STATUS_MASTER_HOLD          BIT7

// RP1 clock manager registers and fields.
#define RP1_CLK_MIPI1_CFG_CTRL             (RP1_CLOCKS_MAIN_BASE + 0x0d4)
#define RP1_CLK_MIPI1_CFG_DIV_INT          (RP1_CLOCKS_MAIN_BASE + 0x0d8)
#define RP1_VIDEO_CLK_MIPI1_DPI_CTRL       (RP1_CLOCKS_VIDEO_BASE + 0x030)
#define RP1_VIDEO_CLK_MIPI1_DPI_DIV_INT    (RP1_CLOCKS_VIDEO_BASE + 0x034)
#define RP1_VIDEO_CLK_MIPI1_DPI_DIV_FRAC   (RP1_CLOCKS_VIDEO_BASE + 0x038)
#define RP1_CLK_CTRL_AUXSRC_MASK           (0x1fU << 5)
#define RP1_CLK_CTRL_ENABLE                BIT11
#define RP1_DPI_AUXSRC_DSI_BYTE_CLOCK      3U

// RP1 MIPI configuration registers.
#define RP1_MIPICFG_CFG                    0x004
#define RP1_MIPICFG_INTE                   0x02c

// Synopsys DSI host registers.
#define DSI_PWR_UP                         0x004
#define DSI_CLKMGR_CFG                     0x008
#define DSI_DPI_VCID                       0x00c
#define DSI_DPI_COLOR_CODING               0x010
#define DSI_DPI_CFG_POL                    0x014
#define DSI_DPI_LP_CMD_TIM                 0x018
#define DSI_PCKHDL_CFG                     0x02c
#define DSI_GEN_VCID                       0x030
#define DSI_MODE_CFG                       0x034
#define DSI_VID_MODE_CFG                   0x038
#define DSI_VID_PKT_SIZE                   0x03c
#define DSI_VID_NUM_CHUNKS                 0x040
#define DSI_VID_NULL_SIZE                  0x044
#define DSI_VID_HSA_TIME                   0x048
#define DSI_VID_HBP_TIME                   0x04c
#define DSI_VID_HLINE_TIME                 0x050
#define DSI_VID_VSA_LINES                  0x054
#define DSI_VID_VBP_LINES                  0x058
#define DSI_VID_VFP_LINES                  0x05c
#define DSI_VID_VACTIVE_LINES              0x060
#define DSI_CMD_MODE_CFG                   0x068
#define DSI_GEN_HDR                        0x06c
#define DSI_GEN_PLD_DATA                   0x070
#define DSI_CMD_PKT_STATUS                 0x074
#define DSI_TO_CNT_CFG                     0x078
#define DSI_BTA_TO_CNT                     0x08c
#define DSI_LPCLK_CTRL                     0x094
#define DSI_PHY_TMR_LPCLK_CFG              0x098
#define DSI_PHY_TMR_CFG                    0x09c
#define DSI_PHYRSTZ                        0x0a0
#define DSI_PHY_IF_CFG                     0x0a4
#define DSI_PHY_STATUS                     0x0b0
#define DSI_PHY_TST_CTRL0                  0x0b4
#define DSI_PHY_TST_CTRL1                  0x0b8

#define DSI_PCKHDL_EOTP_TX_ENABLE          BIT0
#define DSI_PCKHDL_BTA_ENABLE              BIT2
#define DSI_VID_MODE_LP_VSA_ENABLE         BIT8
#define DSI_VID_MODE_LP_VBP_ENABLE         BIT9
#define DSI_VID_MODE_LP_VFP_ENABLE         BIT10
#define DSI_VID_MODE_LP_VACT_ENABLE        BIT11
#define DSI_VID_MODE_LP_HBP_ENABLE         BIT12
#define DSI_VID_MODE_LP_HFP_ENABLE         BIT13
#define DSI_VID_MODE_LP_CMD_ENABLE         BIT15
#define DSI_VID_MODE_SYNC_EVENTS           1U
#define DSI_CMD_MODE_ALL_LP                0x010f7f00U
#define DSI_PHY_SHUTDOWN                   BIT0
#define DSI_PHY_RESET                      BIT1
#define DSI_PHY_TEST_CLEAR                 BIT0
#define DSI_PHY_TEST_CLOCK                 BIT1
#define DSI_PHY_TEST_ENABLE                BIT16

// D-PHY test interface register addresses and fixed 1 GHz configuration.
#define DPHY_PLL_INPUT_DIV                 0x17
#define DPHY_PLL_LOOP_DIV                  0x18
#define DPHY_PLL_DIV_CTRL                  0x19
#define DPHY_CLOCK_POLARITY                0x35
#define DPHY_LANE0_HS_RX_CTRL              0x44
#define DPHY_DATA0_POLARITY                0x45
#define DPHY_DATA1_POLARITY                0x55
#define DPHY_DATA2_POLARITY                0x85
#define DPHY_DATA3_POLARITY                0x95

// RP1 DPI DMA registers.
#define DPI_DMA_CONTROL                    0x000
#define DPI_DMA_IRQ_ENABLE                 0x004
#define DPI_DMA_IRQ_FLAGS                  0x008
#define DPI_DMA_QOS                        0x00c
#define DPI_DMA_ADDRESS_LOW                0x010
#define DPI_DMA_STRIDE                     0x014
#define DPI_DMA_VISIBLE_AREA               0x018
#define DPI_DMA_SYNC_WIDTH                 0x01c
#define DPI_DMA_BACK_PORCH                 0x020
#define DPI_DMA_FRONT_PORCH                0x024
#define DPI_DMA_SHIFT                      0x028
#define DPI_DMA_INPUT_MASK                 0x02c
#define DPI_DMA_OUTPUT_MASK                0x030
#define DPI_DMA_RGB_SIZE                   0x034
#define DPI_DMA_STATUS                     0x03c
#define DPI_DMA_ADDRESS_HIGH               0x040

#define DPI_DMA_CONTROL_ARM                BIT0
#define DPI_DMA_CONTROL_AUTO_REPEAT        BIT1
#define DPI_DMA_CONTROL_HIGH_WATER(Value)  ((Value) << 3)
#define DPI_DMA_CONTROL_TIMING_ENABLES     \
  (BIT17 | BIT18 | BIT19 | BIT20 | BIT21 | BIT22)

#define DSI_DCS_SHORT_WRITE                0x05U
#define DSI_DCS_SHORT_WRITE_PARAMETER      0x15U
#define DSI_DCS_LONG_WRITE                 0x39U

STATIC
VOID
Rp1DsiConfigurePin (
  IN RP1_DSI_HW  *Hardware,
  IN UINT32      Pin,
  IN UINT32      Function
  )
{
  EFI_PHYSICAL_ADDRESS  Address;
  UINT32                Value;

  Address  = Hardware->PeripheralBase + RP1_PADS_BANK2_BASE + RP1_PAD_CTRL (Pin);
  Value    = MmioRead32 (Address);
  Value   &= ~(RP1_PAD_PULL_MASK | RP1_PAD_DRIVE_MASK | RP1_PAD_OUTPUT_DISABLE);
  Value   |= RP1_PAD_PULL_UP | RP1_PAD_DRIVE_12MA | RP1_PAD_INPUT_ENABLE;
  MmioWrite32 (Address, Value);
  (VOID)MmioRead32 (Address);

  Address  = Hardware->PeripheralBase + RP1_IO_BANK2_BASE + RP1_GPIO_CTRL (Pin);
  Value    = MmioRead32 (Address);
  Value   &= ~(RP1_GPIO_FUNCSEL_MASK | RP1_GPIO_OVERRIDE_MASK);
  Value   |= Function;
  MmioWrite32 (Address, Value);
  (VOID)MmioRead32 (Address);
  MemoryFence ();
}

STATIC
VOID
Rp1DsiConfigureI2cPins (
  IN RP1_DSI_HW  *Hardware
  )
{
  Rp1DsiConfigurePin (Hardware, RP1_DISP1_SDA_BANK_PIN, RP1_GPIO_FSEL_I2C4);
  Rp1DsiConfigurePin (Hardware, RP1_DISP1_SCL_BANK_PIN, RP1_GPIO_FSEL_I2C4);
}

/**
  Recover a bus that an earlier boot stage left mid-transaction.

  The pins are released high and only actively driven low, retaining I2C's
  open-drain electrical behaviour. Nine clocks release a slave waiting for the
  remainder of a byte, and the final sequence generates a STOP condition.
**/
STATIC
VOID
Rp1DsiI2cRecoverBus (
  IN RP1_DSI_HW  *Hardware
  )
{
  EFI_PHYSICAL_ADDRESS  RioBase;
  UINT32                Index;

  RioBase = Hardware->PeripheralBase + RP1_SYS_RIO2_BASE;

  // Preload low, but keep both output enables released.
  MmioWrite32 (RioBase + RP1_RIO_OUT + RP1_RIO_CLR, RP1_DISP1_I2C_BITS);
  MmioWrite32 (RioBase + RP1_RIO_OE + RP1_RIO_CLR, RP1_DISP1_I2C_BITS);
  Rp1DsiConfigurePin (Hardware, RP1_DISP1_SDA_BANK_PIN, RP1_GPIO_FSEL_GPIO);
  Rp1DsiConfigurePin (Hardware, RP1_DISP1_SCL_BANK_PIN, RP1_GPIO_FSEL_GPIO);
  MicroSecondDelay (10);

  for (Index = 0; Index < 9; Index++) {
    MmioWrite32 (RioBase + RP1_RIO_OE + RP1_RIO_SET, RP1_DISP1_SCL_BIT);
    MicroSecondDelay (5);
    MmioWrite32 (RioBase + RP1_RIO_OE + RP1_RIO_CLR, RP1_DISP1_SCL_BIT);
    MicroSecondDelay (5);
  }

  // STOP: hold SDA low, release SCL, then release SDA.
  MmioWrite32 (RioBase + RP1_RIO_OE + RP1_RIO_SET, RP1_DISP1_SDA_BIT);
  MicroSecondDelay (5);
  MmioWrite32 (RioBase + RP1_RIO_OE + RP1_RIO_CLR, RP1_DISP1_SCL_BIT);
  MicroSecondDelay (5);
  MmioWrite32 (RioBase + RP1_RIO_OE + RP1_RIO_CLR, RP1_DISP1_SDA_BIT);
  MicroSecondDelay (5);

  Rp1DsiConfigureI2cPins (Hardware);
}

STATIC
EFI_STATUS
Rp1DsiI2cSetEnabled (
  IN RP1_DSI_HW  *Hardware,
  IN BOOLEAN     Enable
  )
{
  EFI_PHYSICAL_ADDRESS  Base;
  UINT32                EnableValue;
  UINT32                Index;
  UINT32                Expected;
  UINT32                Interrupts;
  UINT32                Status;

  Base     = Hardware->PeripheralBase + RP1_I2C4_BASE;
  Expected = Enable ? DW_I2C_ENABLE_CONTROLLER : 0;

  if (!Enable) {
    // Do not strand SCL low by disabling a master that has not gone idle yet.
    for (Index = 0; Index < 2000; Index++) {
      Status     = MmioRead32 (Base + DW_I2C_STATUS);
      Interrupts = MmioRead32 (Base + DW_I2C_RAW_INTR_STAT);
      if (((Status & (DW_I2C_STATUS_ACTIVITY | DW_I2C_STATUS_MASTER_ACTIVITY |
                      DW_I2C_STATUS_MASTER_HOLD)) == 0) &&
          ((Interrupts & DW_I2C_RAW_MST_ON_HOLD) == 0)) {
        break;
      }

      MicroSecondDelay (10);
    }

    if (Index == 2000) {
      EnableValue = MmioRead32 (Base + DW_I2C_ENABLE);
      if ((EnableValue & DW_I2C_ENABLE_CONTROLLER) == 0) {
        MmioWrite32 (Base + DW_I2C_ENABLE, DW_I2C_ENABLE_CONTROLLER);
        MicroSecondDelay (100);
        EnableValue = DW_I2C_ENABLE_CONTROLLER;
      }

      MmioWrite32 (Base + DW_I2C_ENABLE, EnableValue | DW_I2C_ENABLE_ABORT);
      for (Index = 0; Index < 100; Index++) {
        if ((MmioRead32 (Base + DW_I2C_ENABLE) & DW_I2C_ENABLE_ABORT) == 0) {
          break;
        }

        MicroSecondDelay (10);
      }
    }
  }

  MmioWrite32 (
    Base + DW_I2C_ENABLE,
    Enable ? DW_I2C_ENABLE_CONTROLLER : 0
    );

  for (Index = 0; Index < 1000; Index++) {
    if ((MmioRead32 (Base + DW_I2C_ENABLE_STATUS) &
         DW_I2C_ENABLE_CONTROLLER) == Expected) {
      return EFI_SUCCESS;
    }

    MicroSecondDelay (10);
  }

  return EFI_TIMEOUT;
}

STATIC
EFI_STATUS
Rp1DsiI2cBegin (
  IN RP1_DSI_HW  *Hardware,
  IN UINT8       SlaveAddress
  )
{
  EFI_PHYSICAL_ADDRESS  Base;
  EFI_STATUS            Status;

  Base   = Hardware->PeripheralBase + RP1_I2C4_BASE;
  Status = Rp1DsiI2cSetEnabled (Hardware, FALSE);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  MmioWrite32 (Base + DW_I2C_TAR, SlaveAddress);
  (VOID)MmioRead32 (Base + DW_I2C_CLR_INTR);
  return Rp1DsiI2cSetEnabled (Hardware, TRUE);
}

STATIC
EFI_STATUS
Rp1DsiI2cWaitForCompletion (
  IN  RP1_DSI_HW  *Hardware,
  OUT UINT8       *ReadValue OPTIONAL
  )
{
  EFI_PHYSICAL_ADDRESS  Base;
  UINT32                Index;
  UINT32                Interrupts;
  BOOLEAN               ReadComplete;
  BOOLEAN               StopDetected;

  Base          = Hardware->PeripheralBase + RP1_I2C4_BASE;
  ReadComplete  = (ReadValue == NULL);
  StopDetected  = FALSE;
  for (Index = 0; Index < 10000; Index++) {
    Interrupts = MmioRead32 (Base + DW_I2C_RAW_INTR_STAT);
    if ((Interrupts & DW_I2C_RAW_TX_ABRT) != 0) {
      (VOID)MmioRead32 (Base + DW_I2C_CLR_TX_ABRT);
      return EFI_NO_RESPONSE;
    }

    // Drain RX before acknowledging STOP, matching the DesignWare master ISR.
    if (!ReadComplete && (MmioRead32 (Base + DW_I2C_RXFLR) != 0)) {
      *ReadValue   = (UINT8)MmioRead32 (Base + DW_I2C_DATA_CMD);
      ReadComplete = TRUE;
    }

    if ((Interrupts & DW_I2C_RAW_STOP_DET) != 0) {
      (VOID)MmioRead32 (Base + DW_I2C_CLR_STOP_DET);
      StopDetected = TRUE;
    }

    if (StopDetected && ReadComplete) {
      return EFI_SUCCESS;
    }

    MicroSecondDelay (10);
  }

  return EFI_TIMEOUT;
}

STATIC
EFI_STATUS
Rp1DsiI2cWaitForTxSpace (
  IN RP1_DSI_HW  *Hardware
  )
{
  EFI_PHYSICAL_ADDRESS  Base;
  UINT32                Index;

  Base = Hardware->PeripheralBase + RP1_I2C4_BASE;
  for (Index = 0; Index < 1000; Index++) {
    if ((MmioRead32 (Base + DW_I2C_STATUS) & DW_I2C_STATUS_TFNF) != 0) {
      return EFI_SUCCESS;
    }

    MicroSecondDelay (10);
  }

  return EFI_TIMEOUT;
}

EFI_STATUS
Rp1DsiI2cWriteRegister (
  IN RP1_DSI_HW  *Hardware,
  IN UINT8       SlaveAddress,
  IN UINT8       Register,
  IN UINT8       Value
  )
{
  EFI_PHYSICAL_ADDRESS  Base;
  EFI_STATUS            Status;

  Base   = Hardware->PeripheralBase + RP1_I2C4_BASE;
  Status = Rp1DsiI2cBegin (Hardware, SlaveAddress);
  if (!EFI_ERROR (Status)) {
    Status = Rp1DsiI2cWaitForTxSpace (Hardware);
  }

  if (!EFI_ERROR (Status)) {
    MmioWrite32 (Base + DW_I2C_DATA_CMD, Register);
    Status = Rp1DsiI2cWaitForTxSpace (Hardware);
  }

  if (!EFI_ERROR (Status)) {
    MmioWrite32 (Base + DW_I2C_DATA_CMD, (UINT32)Value | DW_I2C_DATA_STOP);
    Status = Rp1DsiI2cWaitForCompletion (Hardware, NULL);
  }

  (VOID)Rp1DsiI2cSetEnabled (Hardware, FALSE);
  return Status;
}

EFI_STATUS
Rp1DsiI2cReadRegister (
  IN  RP1_DSI_HW  *Hardware,
  IN  UINT8       SlaveAddress,
  IN  UINT8       Register,
  OUT UINT8       *Value
  )
{
  EFI_PHYSICAL_ADDRESS  Base;
  EFI_STATUS            Status;

  if (Value == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Base   = Hardware->PeripheralBase + RP1_I2C4_BASE;
  Status = Rp1DsiI2cBegin (Hardware, SlaveAddress);
  if (!EFI_ERROR (Status)) {
    Status = Rp1DsiI2cWaitForTxSpace (Hardware);
  }

  if (!EFI_ERROR (Status)) {
    MmioWrite32 (Base + DW_I2C_DATA_CMD, Register);
    Status = Rp1DsiI2cWaitForTxSpace (Hardware);
  }

  if (!EFI_ERROR (Status)) {
    MmioWrite32 (
      Base + DW_I2C_DATA_CMD,
      DW_I2C_DATA_READ | DW_I2C_DATA_RESTART | DW_I2C_DATA_STOP
      );
    Status = Rp1DsiI2cWaitForCompletion (Hardware, Value);
  }

  (VOID)Rp1DsiI2cSetEnabled (Hardware, FALSE);
  return Status;
}

STATIC
EFI_STATUS
Rp1DsiI2cInitialize (
  IN RP1_DSI_HW  *Hardware
  )
{
  EFI_PHYSICAL_ADDRESS  Base;
  EFI_STATUS            Status;
  UINT32                ComponentType;

  Rp1DsiConfigureI2cPins (Hardware);

  Base          = Hardware->PeripheralBase + RP1_I2C4_BASE;
  ComponentType = MmioRead32 (Base + DW_I2C_COMP_TYPE);
  if (ComponentType != DW_I2C_COMP_TYPE_VALUE) {
    return EFI_DEVICE_ERROR;
  }

  Status = Rp1DsiI2cSetEnabled (Hardware, FALSE);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  MmioWrite32 (
    Base + DW_I2C_CON,
    DW_I2C_CON_MASTER | DW_I2C_CON_SPEED_STANDARD |
    DW_I2C_CON_RESTART_ENABLE | DW_I2C_CON_SLAVE_DISABLE
    );
  // Linux DesignWare timing formula for RP1's 200 MHz clk_sys input:
  // 4.0 us high + 65 ns SDA fall, 4.7 us low + 100 ns SCL fall.
  MmioWrite32 (Base + DW_I2C_SS_SCL_HCNT, 810);
  MmioWrite32 (Base + DW_I2C_SS_SCL_LCNT, 959);
  MmioWrite32 (Base + DW_I2C_RX_TL, 0);
  MmioWrite32 (Base + DW_I2C_TX_TL, 0);
  // Keep the reset TX hold count and enable the DesignWare RX hold workaround.
  MmioWrite32 (Base + DW_I2C_SDA_HOLD, MmioRead32 (Base + DW_I2C_SDA_HOLD) | BIT16);
  MmioWrite32 (Base + DW_I2C_INTR_MASK, 0);
  MmioWrite32 (Base + DW_I2C_SMBUS_INTR_MASK, 0);
  (VOID)MmioRead32 (Base + DW_I2C_CLR_INTR);
  Rp1DsiI2cRecoverBus (Hardware);
  return EFI_SUCCESS;
}

STATIC
VOID
Rp1DsiConfigureClocks (
  IN RP1_DSI_HW  *Hardware
  )
{
  EFI_PHYSICAL_ADDRESS  Address;
  UINT32                Control;

  // clk_mipi1_cfg: 50 MHz XOSC / 2 = 25 MHz.
  Address  = Hardware->PeripheralBase + RP1_CLK_MIPI1_CFG_CTRL;
  Control  = MmioRead32 (Address);
  Control &= ~(RP1_CLK_CTRL_AUXSRC_MASK | RP1_CLK_CTRL_ENABLE);
  MmioWrite32 (Address, Control);
  MmioWrite32 (Hardware->PeripheralBase + RP1_CLK_MIPI1_CFG_DIV_INT, 2);
  MmioWrite32 (Address, Control | RP1_CLK_CTRL_ENABLE);
  (VOID)MmioRead32 (Address);
  MicroSecondDelay (10);
}

STATIC
VOID
Rp1DsiStartDpiClock (
  IN RP1_DSI_HW  *Hardware
  )
{
  EFI_PHYSICAL_ADDRESS  Address;
  UINT32                Control;

  // 125 MHz DSI byte clock / 1.5 = the panel's 83.33 MHz pixel clock.
  Address  = Hardware->PeripheralBase + RP1_VIDEO_CLK_MIPI1_DPI_CTRL;
  Control  = MmioRead32 (Address);
  Control &= ~(RP1_CLK_CTRL_AUXSRC_MASK | RP1_CLK_CTRL_ENABLE);
  Control |= RP1_DPI_AUXSRC_DSI_BYTE_CLOCK << 5;
  MmioWrite32 (Address, Control);
  MmioWrite32 (Hardware->PeripheralBase + RP1_VIDEO_CLK_MIPI1_DPI_DIV_INT, 1);
  MmioWrite32 (Hardware->PeripheralBase + RP1_VIDEO_CLK_MIPI1_DPI_DIV_FRAC, 0x80000000U);
  MmioWrite32 (Address, Control | RP1_CLK_CTRL_ENABLE);
  (VOID)MmioRead32 (Address);
}

STATIC
VOID
Rp1DsiPhyTransaction (
  IN RP1_DSI_HW  *Hardware,
  IN UINT8       TestCode,
  IN UINT8       TestData
  )
{
  MmioWrite32 (Hardware->DsiHostBase + DSI_PHY_TST_CTRL1, TestCode | DSI_PHY_TEST_ENABLE);
  MmioWrite32 (Hardware->DsiHostBase + DSI_PHY_TST_CTRL0, 0);
  (VOID)MmioRead32 (Hardware->DsiHostBase + DSI_PHY_TST_CTRL1);
  MmioWrite32 (Hardware->DsiHostBase + DSI_PHY_TST_CTRL1, TestData);
  MmioWrite32 (Hardware->DsiHostBase + DSI_PHY_TST_CTRL0, DSI_PHY_TEST_CLOCK);
}

STATIC
VOID
Rp1DsiPhyInitialize (
  IN RP1_DSI_HW  *Hardware
  )
{
  MmioWrite32 (Hardware->DsiHostBase + DSI_PHYRSTZ, 0);
  MmioWrite32 (Hardware->DsiHostBase + DSI_PHY_TST_CTRL0, DSI_PHY_TEST_CLOCK);
  MmioWrite32 (Hardware->DsiHostBase + DSI_PHY_TST_CTRL1, 0);
  MmioWrite32 (
    Hardware->DsiHostBase + DSI_PHY_TST_CTRL0,
    DSI_PHY_TEST_CLOCK | DSI_PHY_TEST_CLEAR
    );
  MicroSecondDelay (1);
  MmioWrite32 (Hardware->DsiHostBase + DSI_PHY_TST_CTRL0, DSI_PHY_TEST_CLOCK);
  MicroSecondDelay (1);

  // 50 MHz reference, N=1, M=20: 1 GHz lane rate / 125 MHz byte clock.
  Rp1DsiPhyTransaction (Hardware, DPHY_LANE0_HS_RX_CTRL, 0x2aU << 1);
  Rp1DsiPhyTransaction (Hardware, DPHY_PLL_DIV_CTRL, 0x30);
  Rp1DsiPhyTransaction (Hardware, DPHY_PLL_INPUT_DIV, 0);
  Rp1DsiPhyTransaction (Hardware, DPHY_PLL_LOOP_DIV, 0x80);
  Rp1DsiPhyTransaction (Hardware, DPHY_PLL_LOOP_DIV, 19);

  Rp1DsiPhyTransaction (Hardware, DPHY_CLOCK_POLARITY, 0);
  Rp1DsiPhyTransaction (Hardware, DPHY_DATA0_POLARITY, 0);
  Rp1DsiPhyTransaction (Hardware, DPHY_DATA1_POLARITY, 0);
  Rp1DsiPhyTransaction (Hardware, DPHY_DATA2_POLARITY, 0);
  Rp1DsiPhyTransaction (Hardware, DPHY_DATA3_POLARITY, 0);

  MicroSecondDelay (1);
  MmioWrite32 (Hardware->DsiHostBase + DSI_PHYRSTZ, DSI_PHY_SHUTDOWN);
  MicroSecondDelay (1);
  MmioWrite32 (
    Hardware->DsiHostBase + DSI_PHYRSTZ,
    DSI_PHY_SHUTDOWN | DSI_PHY_RESET
    );
  MicroSecondDelay (1);
}

STATIC
EFI_STATUS
Rp1DsiWaitForPhy (
  IN RP1_DSI_HW  *Hardware
  )
{
  UINT32  Index;
  UINT32  Status;

  for (Index = 0; Index < 16384; Index++) {
    Status = MmioRead32 (Hardware->DsiHostBase + DSI_PHY_STATUS);
    if ((Status & BIT0) != 0) {
      break;
    }

    MicroSecondDelay (10);
  }

  if (Index == 16384) {
    return EFI_TIMEOUT;
  }

  MmioWrite32 (Hardware->DsiHostBase + DSI_LPCLK_CTRL, 1);
  MmioWrite32 (Hardware->DsiHostBase + DSI_PHY_TST_CTRL0, DSI_PHY_TEST_CLOCK);
  MmioWrite32 (Hardware->DsiHostBase + DSI_PWR_UP, 1);
  Rp1DsiStartDpiClock (Hardware);

  // Clock lane plus the two active data lanes must reach LP-11 stop state.
  for (Index = 0; Index < 1024; Index++) {
    Status = MmioRead32 (Hardware->DsiHostBase + DSI_PHY_STATUS);
    if ((Status & (BIT4 | BIT7)) == (BIT4 | BIT7)) {
      return EFI_SUCCESS;
    }

    MicroSecondDelay (10);
  }

  return EFI_TIMEOUT;
}

STATIC
EFI_STATUS
Rp1DsiHostInitialize (
  IN RP1_DSI_HW  *Hardware
  )
{
  UINT32  VideoMode;

  MmioWrite32 (Hardware->MipiCfgBase + RP1_MIPICFG_CFG, 0);  // DSI, not CSI-2.
  MmioWrite32 (Hardware->MipiCfgBase + RP1_MIPICFG_INTE, 0); // Firmware uses polling.

  MmioWrite32 (Hardware->DsiHostBase + DSI_PWR_UP, 0);
  MmioWrite32 (Hardware->DsiHostBase + DSI_PHY_IF_CFG, 1);   // Two lanes.
  MmioWrite32 (Hardware->DsiHostBase + DSI_DPI_CFG_POL, 0);
  MmioWrite32 (Hardware->DsiHostBase + DSI_DPI_VCID, 0);
  MmioWrite32 (Hardware->DsiHostBase + DSI_GEN_VCID, 0);
  MmioWrite32 (Hardware->DsiHostBase + DSI_DPI_COLOR_CODING, 0x005); // RGB888.

  VideoMode = DSI_VID_MODE_LP_HFP_ENABLE | DSI_VID_MODE_LP_HBP_ENABLE |
              DSI_VID_MODE_LP_VACT_ENABLE | DSI_VID_MODE_LP_VFP_ENABLE |
              DSI_VID_MODE_LP_VBP_ENABLE | DSI_VID_MODE_LP_VSA_ENABLE |
              DSI_VID_MODE_LP_CMD_ENABLE | DSI_VID_MODE_SYNC_EVENTS;
  MmioWrite32 (Hardware->DsiHostBase + DSI_VID_MODE_CFG, VideoMode);
  MmioWrite32 (Hardware->DsiHostBase + DSI_CMD_MODE_CFG, DSI_CMD_MODE_ALL_LP);
  MmioWrite32 (
    Hardware->DsiHostBase + DSI_PCKHDL_CFG,
    DSI_PCKHDL_BTA_ENABLE | DSI_PCKHDL_EOTP_TX_ENABLE
    );
  MmioWrite32 (Hardware->DsiHostBase + DSI_MODE_CFG, 1); // Command mode.

  MmioWrite32 (Hardware->DsiHostBase + DSI_TO_CNT_CFG, (28580U << 16) | 0x40U);
  MmioWrite32 (Hardware->DsiHostBase + DSI_BTA_TO_CNT, 0x0d00);
  MmioWrite32 (Hardware->DsiHostBase + DSI_CLKMGR_CFG, (0x50U << 8) | 7U);

  MmioWrite32 (
    Hardware->DsiHostBase + DSI_VID_PKT_SIZE,
    RP1_DSI_HORIZONTAL_RESOLUTION
    );
  MmioWrite32 (Hardware->DsiHostBase + DSI_VID_NUM_CHUNKS, 0);
  MmioWrite32 (Hardware->DsiHostBase + DSI_VID_NULL_SIZE, 0);
  MmioWrite32 (
    Hardware->DsiHostBase + DSI_VID_HSA_TIME,
    (RP1_DSI_HORIZONTAL_SYNC_WIDTH * 3U) / 2U
    );
  MmioWrite32 (
    Hardware->DsiHostBase + DSI_VID_HBP_TIME,
    (RP1_DSI_HORIZONTAL_BACK_PORCH * 3U) / 2U
    );
  MmioWrite32 (
    Hardware->DsiHostBase + DSI_VID_HLINE_TIME,
    ((RP1_DSI_HORIZONTAL_RESOLUTION + RP1_DSI_HORIZONTAL_FRONT_PORCH +
      RP1_DSI_HORIZONTAL_SYNC_WIDTH + RP1_DSI_HORIZONTAL_BACK_PORCH) * 3U) / 2U
    );
  MmioWrite32 (
    Hardware->DsiHostBase + DSI_VID_VSA_LINES,
    RP1_DSI_VERTICAL_SYNC_WIDTH
    );
  MmioWrite32 (
    Hardware->DsiHostBase + DSI_VID_VBP_LINES,
    RP1_DSI_VERTICAL_BACK_PORCH
    );
  MmioWrite32 (
    Hardware->DsiHostBase + DSI_VID_VFP_LINES,
    RP1_DSI_VERTICAL_FRONT_PORCH
    );
  MmioWrite32 (
    Hardware->DsiHostBase + DSI_VID_VACTIVE_LINES,
    RP1_DSI_VERTICAL_RESOLUTION
    );

  Rp1DsiPhyInitialize (Hardware);
  MmioWrite32 (Hardware->DsiHostBase + DSI_PHY_TMR_LPCLK_CFG, (51U << 16) | 135U);
  MmioWrite32 (Hardware->DsiHostBase + DSI_PHY_TMR_CFG, (37U << 16) | 111U);
  MmioWrite32 (Hardware->DsiHostBase + DSI_DPI_LP_CMD_TIM, 11U << 16);

  return Rp1DsiWaitForPhy (Hardware);
}

STATIC
EFI_STATUS
Rp1DsiWaitForCommandFifos (
  IN RP1_DSI_HW  *Hardware
  )
{
  UINT32  Index;
  UINT32  Status;

  for (Index = 0; Index < 256; Index++) {
    Status = MmioRead32 (Hardware->DsiHostBase + DSI_CMD_PKT_STATUS);
    if ((Status & 0x0fU) == 0x05U) {
      return EFI_SUCCESS;
    }

    MicroSecondDelay (100);
  }

  return EFI_TIMEOUT;
}

EFI_STATUS
Rp1DsiDcsWrite (
  IN RP1_DSI_HW  *Hardware,
  IN CONST UINT8 *Payload,
  IN UINTN       PayloadSize
  )
{
  EFI_STATUS  Status;
  UINT32      Header;
  UINT32      PayloadWord;
  UINTN       Index;
  UINTN       Remaining;

  if ((Hardware == NULL) || (Payload == NULL) || (PayloadSize == 0) ||
      (PayloadSize > MAX_UINT16)) {
    return EFI_INVALID_PARAMETER;
  }

  Status = Rp1DsiWaitForCommandFifos (Hardware);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  MmioWrite32 (Hardware->DsiHostBase + DSI_CMD_MODE_CFG, DSI_CMD_MODE_ALL_LP);
  (VOID)MmioRead32 (Hardware->DsiHostBase + DSI_CMD_MODE_CFG);

  if (PayloadSize == 1) {
    Header = DSI_DCS_SHORT_WRITE | ((UINT32)Payload[0] << 8);
  } else if (PayloadSize == 2) {
    Header = DSI_DCS_SHORT_WRITE_PARAMETER |
             ((UINT32)Payload[0] << 8) | ((UINT32)Payload[1] << 16);
  } else {
    Header    = DSI_DCS_LONG_WRITE | ((UINT32)PayloadSize << 8);
    Remaining = PayloadSize;
    Index     = 0;
    while (Remaining != 0) {
      PayloadWord = Payload[Index++];
      Remaining--;
      if (Remaining != 0) {
        PayloadWord |= (UINT32)Payload[Index++] << 8;
        Remaining--;
      }

      if (Remaining != 0) {
        PayloadWord |= (UINT32)Payload[Index++] << 16;
        Remaining--;
      }

      if (Remaining != 0) {
        PayloadWord |= (UINT32)Payload[Index++] << 24;
        Remaining--;
      }

      MmioWrite32 (Hardware->DsiHostBase + DSI_GEN_PLD_DATA, PayloadWord);
    }
  }

  MmioWrite32 (Hardware->DsiHostBase + DSI_GEN_HDR, Header);
  return Rp1DsiWaitForCommandFifos (Hardware);
}

EFI_STATUS
Rp1DsiHardwareInitialize (
  IN OUT RP1_DSI_HW  *Hardware
  )
{
  EFI_STATUS  Status;
  UINT32      Index;

  if ((Hardware == NULL) || (Hardware->PeripheralBase == 0)) {
    return EFI_INVALID_PARAMETER;
  }

  Hardware->DsiDmaBase  = Hardware->PeripheralBase + RP1_MIPI1_DSIDMA_BASE;
  Hardware->DsiHostBase = Hardware->PeripheralBase + RP1_MIPI1_DSIHOST_BASE;
  Hardware->MipiCfgBase = Hardware->PeripheralBase + RP1_MIPI1_CFG_BASE;

  Status = Rp1DsiI2cInitialize (Hardware);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // Match the upstream Display 2 regulator probe: place the panel in reset.
  // The acknowledged write also establishes that the MCU is present. It may
  // still be leaving reset when DXE dispatches, so allow one bounded second.
  Status = EFI_NO_RESPONSE;
  for (Index = 0; Index < 20; Index++) {
    Status = Rp1DsiI2cWriteRegister (
               Hardware,
               RP1_PANEL_MCU_ADDRESS,
               RP1_PANEL_MCU_POWER_REGISTER,
               0
               );
    if (!EFI_ERROR (Status)) {
      break;
    }

    MicroSecondDelay (50000);
  }

  if (EFI_ERROR (Status)) {
    return EFI_NOT_FOUND;
  }

  Rp1DsiConfigureClocks (Hardware);
  return Rp1DsiHostInitialize (Hardware);
}

EFI_STATUS
Rp1DsiStartScanout (
  IN RP1_DSI_HW           *Hardware,
  IN EFI_PHYSICAL_ADDRESS DmaAddress
  )
{
  UINT32  Control;
  UINT32  InputMask;
  UINT32  OutputMask;
  UINT32  Shift;

  if ((Hardware == NULL) || (DmaAddress == 0)) {
    return EFI_INVALID_PARAMETER;
  }

  if ((MmioRead32 (Hardware->DsiDmaBase + DPI_DMA_STATUS) & 0x0f8fU) != 0) {
    MmioWrite32 (Hardware->DsiDmaBase + DPI_DMA_CONTROL, 0);
    MicroSecondDelay (20000);
  }

  MmioWrite32 (
    Hardware->DsiDmaBase + DPI_DMA_VISIBLE_AREA,
    (RP1_DSI_VERTICAL_RESOLUTION - 1U) |
    ((RP1_DSI_HORIZONTAL_RESOLUTION - 1U) << 16)
    );
  MmioWrite32 (
    Hardware->DsiDmaBase + DPI_DMA_SYNC_WIDTH,
    (RP1_DSI_VERTICAL_SYNC_WIDTH - 1U) |
    ((RP1_DSI_HORIZONTAL_SYNC_WIDTH - 1U) << 16)
    );
  MmioWrite32 (
    Hardware->DsiDmaBase + DPI_DMA_BACK_PORCH,
    (RP1_DSI_VERTICAL_SYNC_WIDTH + RP1_DSI_VERTICAL_BACK_PORCH - 1U) |
    ((RP1_DSI_HORIZONTAL_SYNC_WIDTH + RP1_DSI_HORIZONTAL_BACK_PORCH - 1U) << 16)
    );
  MmioWrite32 (
    Hardware->DsiDmaBase + DPI_DMA_FRONT_PORCH,
    (RP1_DSI_VERTICAL_FRONT_PORCH - 1U) |
    ((RP1_DSI_HORIZONTAL_FRONT_PORCH - 1U) << 16)
    );

  InputMask  = 0x3fcU | (0x3fcU << 10) | (0x3fcU << 20);
  OutputMask = InputMask;
  Shift      = 23U | (15U << 5) | (7U << 10) |
               (23U << 15) | (15U << 20) | (7U << 25);
  MmioWrite32 (Hardware->DsiDmaBase + DPI_DMA_INPUT_MASK, InputMask);
  MmioWrite32 (Hardware->DsiDmaBase + DPI_DMA_OUTPUT_MASK, OutputMask);
  MmioWrite32 (Hardware->DsiDmaBase + DPI_DMA_SHIFT, Shift);
  MmioWrite32 (Hardware->DsiDmaBase + DPI_DMA_RGB_SIZE, 3U << 16);
  MmioWrite32 (
    Hardware->DsiDmaBase + DPI_DMA_QOS,
    (0xbU << 4) | (0x2U << 8) | (0x8U << 12) | (0x7U << 16)
    );
  MmioWrite32 (Hardware->DsiDmaBase + DPI_DMA_IRQ_FLAGS, MAX_UINT32);
  MmioWrite32 (Hardware->DsiDmaBase + DPI_DMA_IRQ_ENABLE, 0);

  Control = DPI_DMA_CONTROL_ARM | DPI_DMA_CONTROL_AUTO_REPEAT |
            DPI_DMA_CONTROL_HIGH_WATER (448) | DPI_DMA_CONTROL_TIMING_ENABLES;
  MmioWrite32 (Hardware->DsiDmaBase + DPI_DMA_CONTROL, Control);
  MmioWrite32 (
    Hardware->DsiDmaBase + DPI_DMA_STRIDE,
    RP1_DSI_HORIZONTAL_RESOLUTION * RP1_DSI_BYTES_PER_PIXEL
    );
  MmioWrite32 (Hardware->DsiDmaBase + DPI_DMA_ADDRESS_HIGH, (UINT32)(DmaAddress >> 32));

  // Enter video mode before writing the low address, which arms DMA immediately.
  MmioWrite32 (Hardware->DsiHostBase + DSI_MODE_CFG, 0);
  MemoryFence ();
  MmioWrite32 (Hardware->DsiDmaBase + DPI_DMA_ADDRESS_LOW, (UINT32)DmaAddress);
  (VOID)MmioRead32 (Hardware->DsiDmaBase + DPI_DMA_ADDRESS_LOW);

  return EFI_SUCCESS;
}

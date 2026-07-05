/** @file
 *
 *  Copyright (c) 2023-2024, Mario Bălănică <mariobalanica02@gmail.com>
 *
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 **/

#ifndef __BCM2712_H__
#define __BCM2712_H__

#define BCM2712_IO_BASE                                   0x1000000000
#define BCM2712_IO_LENGTH                                 0x1000000000

#define BCM2712_LEGACY_BUS_BASE                           0x107c000000
#define BCM2712_LEGACY_BUS_LENGTH                         0x4000000

#define BCM2712_PL011_UART0_BASE                          0x107d001000
#define BCM2712_PL011_LENGTH                              0x200

#define BCM2712_BRCMSTB_GIO_BASE                          0x107d508500
#define BCM2712_BRCMSTB_GIO_LENGTH                        0x40
#define BCM2712_BRCMSTB_GIO_AON_BASE                      0x107d517c00
#define BCM2712_BRCMSTB_GIO_AON_LENGTH                    0x40

#define BCM2712_PINCTRL_BASE                              0x107d504100
#define BCM2712_PINCTRL_LENGTH                            0x20
#define BCM2712_PINCTRL_AON_BASE                          0x107d510700
#define BCM2712_PINCTRL_AON_LENGTH                        0x1C

#define BCM2712_BRCMSTB_SDIO1_HOST_BASE                   0x1000fff000
#define BCM2712_BRCMSTB_SDIO1_CFG_BASE                    0x1000fff400
#define BCM2712_BRCMSTB_SDIO2_HOST_BASE                   0x1001100000
#define BCM2712_BRCMSTB_SDIO2_CFG_BASE                    0x1001100400
#define BCM2712_BRCMSTB_SDIO_HOST_LENGTH                  0x260
#define BCM2712_BRCMSTB_SDIO_CFG_LENGTH                   0x200

#define BCM2712_BRCMSTB_PCIE0_BASE                        0x1000100000
#define BCM2712_BRCMSTB_PCIE0_CPU_MEM_BASE                0x1700000000
#define BCM2712_BRCMSTB_PCIE0_CPU_MEM64_BASE              0x1400000000
#define BCM2712_BRCMSTB_PCIE1_BASE                        0x1000110000
#define BCM2712_BRCMSTB_PCIE1_CPU_MEM_BASE                0x1b00000000
#define BCM2712_BRCMSTB_PCIE1_CPU_MEM64_BASE              0x1800000000
#define BCM2712_BRCMSTB_PCIE2_BASE                        0x1000120000
#define BCM2712_BRCMSTB_PCIE2_CPU_MEM_BASE                0x1f00000000
#define BCM2712_BRCMSTB_PCIE2_CPU_MEM64_BASE              0x1c00000000
#define BCM2712_BRCMSTB_PCIE_LENGTH                       0x9310
#define BCM2712_BRCMSTB_PCIE_MEM_SIZE                     0xfffffffc
#define BCM2712_BRCMSTB_PCIE_MEM64_SIZE                   0x300000000
#define BCM2712_BRCMSTB_PCIE_COUNT                        3

//
// VideoCore VII (V3D 7.1)
//
#define BCM2712_V3D_HUB_BASE                              0x1002000000
#define BCM2712_V3D_HUB_LENGTH                            0x4000
#define BCM2712_V3D_CORE0_BASE                            0x1002008000
#define BCM2712_V3D_CORE0_LENGTH                          0x6000
#define BCM2712_V3D_SMS_BASE                              0x1002030800
#define BCM2712_V3D_SMS_LENGTH                            0x700
#define BCM2712_V3D_CORE_INTERRUPT                        282   // GIC_SPI 250 + 32
#define BCM2712_V3D_HUB_INTERRUPT                         281   // GIC_SPI 249 + 32

//
// Display pipeline
//
#define BCM2712_HVS_BASE                                  0x107c580000
#define BCM2712_HVS_LENGTH                                0x1a000
#define BCM2712_HVS_IOMMU_BASE                            0x1000005200
#define BCM2712_HVS_IOMMU_LENGTH                          0x80
#define BCM2712_PIXELVALVE0_BASE                          0x107c410000
#define BCM2712_PIXELVALVE0_LENGTH                        0x100
#define BCM2712_PIXELVALVE0_INTERRUPT                     133   // GIC_SPI 101 + 32
#define BCM2712_PIXELVALVE1_BASE                          0x107c411000
#define BCM2712_PIXELVALVE1_LENGTH                        0x100
#define BCM2712_PIXELVALVE1_INTERRUPT                     142   // GIC_SPI 110 + 32
#define BCM2712_MOP_BASE                                  0x107c500000
#define BCM2712_MOP_LENGTH                                0x28
#define BCM2712_MOPLET_BASE                               0x107c501000
#define BCM2712_MOPLET_LENGTH                             0x20
#define BCM2712_DISP_INTR_BASE                            0x107c502000
#define BCM2712_DISP_INTR_LENGTH                          0x30

#endif // __BCM2712_H__

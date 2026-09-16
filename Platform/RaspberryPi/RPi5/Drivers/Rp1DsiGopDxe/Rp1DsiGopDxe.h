/** @file
  Raspberry Pi 5 RP1 DSI graphics output support.

  Copyright (c) 2026, Ahmed Ghanem. All rights reserved.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef RP1_DSI_GOP_DXE_H_
#define RP1_DSI_GOP_DXE_H_

#include <Uefi.h>

#include <Rp1DsiPanel.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/PciIo.h>
#include <Protocol/Rp1Bus.h>

//
// Versioned handoff contract for a logical GOP whose pixels are transformed
// into a separate linear GOP.  Loaders that understand this protocol can use
// the public GOP for their console and pass the real linear scanout to the OS.
//
#define EFI_GRAPHICS_OUTPUT_TRANSFORM_PROTOCOL_GUID \
  { 0xac775d0f, 0x1199, 0x42e2, { 0x92, 0x00, 0x71, 0xdc, 0x78, 0xaa, 0x65, 0xbb } }

#define EFI_GRAPHICS_OUTPUT_TRANSFORM_VERSION             1U
#define EFI_GRAPHICS_OUTPUT_TRANSFORM_FIXED_SCANOUT        BIT0
#define EFI_GRAPHICS_OUTPUT_TRANSFORM_ROTATION_IDENTITY    0U
#define EFI_GRAPHICS_OUTPUT_TRANSFORM_ROTATION_90          1U
#define EFI_GRAPHICS_OUTPUT_TRANSFORM_ROTATION_180         2U
#define EFI_GRAPHICS_OUTPUT_TRANSFORM_ROTATION_270         3U

typedef struct {
  UINT32                        Size;
  UINT32                        Version;
  UINT32                        Flags;
  UINT32                        Rotation;
  UINT32                        LogicalWidth;
  UINT32                        LogicalHeight;
  EFI_GRAPHICS_OUTPUT_PROTOCOL  *LinearGop;
} EFI_GRAPHICS_OUTPUT_TRANSFORM_PROTOCOL;

typedef struct {
  EFI_PHYSICAL_ADDRESS    PeripheralBase;
  EFI_PHYSICAL_ADDRESS    DsiHostBase;
  EFI_PHYSICAL_ADDRESS    DsiDmaBase;
  EFI_PHYSICAL_ADDRESS    MipiCfgBase;
} RP1_DSI_HW;

EFI_STATUS
Rp1DsiHardwareInitialize (
  IN OUT RP1_DSI_HW  *Hardware
  );

EFI_STATUS
Rp1DsiPanelInitialize (
  IN RP1_DSI_HW  *Hardware
  );

EFI_STATUS
Rp1DsiPanelEnableBacklight (
  IN RP1_DSI_HW  *Hardware
  );

EFI_STATUS
Rp1DsiStartScanout (
  IN RP1_DSI_HW          *Hardware,
  IN EFI_PHYSICAL_ADDRESS DmaAddress
  );

EFI_STATUS
Rp1DsiDcsWrite (
  IN RP1_DSI_HW  *Hardware,
  IN CONST UINT8 *Payload,
  IN UINTN       PayloadSize
  );

EFI_STATUS
Rp1DsiI2cReadRegister (
  IN  RP1_DSI_HW  *Hardware,
  IN  UINT8       SlaveAddress,
  IN  UINT8       Register,
  OUT UINT8       *Value
  );

EFI_STATUS
Rp1DsiI2cWriteRegister (
  IN RP1_DSI_HW  *Hardware,
  IN UINT8       SlaveAddress,
  IN UINT8       Register,
  IN UINT8       Value
  );

#endif // RP1_DSI_GOP_DXE_H_

/** @file
  Raspberry Pi 5 RP1 DSI Graphics Output Protocol driver.

  Copyright (c) 2026, Ahmed Ghanem. All rights reserved.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/Cpu.h>
#include <Protocol/DevicePath.h>
#include <Protocol/EdidActive.h>
#include <Protocol/EdidDiscovered.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/PciIo.h>
#include <Protocol/Rp1Bus.h>

#include "Rp1DsiGopDxe.h"

#define RP1_DSI_GOP_SIGNATURE  SIGNATURE_32 ('R', 'D', 'G', 'P')

typedef struct {
  VENDOR_DEVICE_PATH         Vendor;
  EFI_DEVICE_PATH_PROTOCOL  End;
} RP1_DSI_DEVICE_PATH;

typedef struct {
  UINT32                                Signature;
  EFI_HANDLE                            Handle;
  EFI_HANDLE                            NativeHandle;
  EFI_GRAPHICS_OUTPUT_PROTOCOL          Gop;
  EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE     Mode;
  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION  ModeInfo;
  EFI_GRAPHICS_OUTPUT_PROTOCOL          LandscapeGop;
  EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE     LandscapeMode;
  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION  LandscapeModeInfo;
  EFI_GRAPHICS_OUTPUT_TRANSFORM_PROTOCOL Transform;
  EFI_EDID_DISCOVERED_PROTOCOL           EdidDiscovered;
  EFI_EDID_ACTIVE_PROTOCOL               EdidActive;
  UINT8                                  Edid[128];
  RP1_DSI_DEVICE_PATH                   DevicePath;
  RP1_DSI_HW                            Hardware;
  EFI_PCI_IO_PROTOCOL                   *PciIo;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL         *LandscapeBuffer;
  EFI_PHYSICAL_ADDRESS                  FrameBufferBase;
  EFI_PHYSICAL_ADDRESS                  DmaAddress;
  VOID                                  *DmaMapping;
  UINTN                                 FrameBufferPages;
} RP1_DSI_GOP_CONTEXT;

#define RP1_DSI_CONTEXT_FROM_GOP(This) \
  BASE_CR ((This), RP1_DSI_GOP_CONTEXT, Gop)

#define RP1_DSI_CONTEXT_FROM_LANDSCAPE_GOP(This) \
  BASE_CR ((This), RP1_DSI_GOP_CONTEXT, LandscapeGop)

STATIC CONST EFI_GUID mRp1DsiDisplayDevicePathGuid = {
  0x94e709f1, 0x8b2e, 0x4ed1, { 0xa9, 0x2d, 0x5b, 0xb1, 0x44, 0x5d, 0x55, 0xc4 }
};

STATIC CONST EFI_GUID mGraphicsOutputTransformProtocolGuid =
  EFI_GRAPHICS_OUTPUT_TRANSFORM_PROTOCOL_GUID;

STATIC
VOID
Rp1DsiInitializeEdid (
  IN OUT RP1_DSI_GOP_CONTEXT  *Context
  )
{
  STATIC CONST UINT8  PanelEdid[RP1_DSI_PANEL_EDID_SIZE] = {
    RP1_DSI_PANEL_EDID_BYTES
  };

  CopyMem (Context->Edid, PanelEdid, sizeof (PanelEdid));
  Context->EdidDiscovered.SizeOfEdid = sizeof (Context->Edid);
  Context->EdidDiscovered.Edid       = Context->Edid;
  Context->EdidActive.SizeOfEdid     = sizeof (Context->Edid);
  Context->EdidActive.Edid           = Context->Edid;
}

STATIC
EFI_STATUS
EFIAPI
Rp1DsiGopQueryMode (
  IN  EFI_GRAPHICS_OUTPUT_PROTOCOL          *This,
  IN  UINT32                                ModeNumber,
  OUT UINTN                                 *SizeOfInfo,
  OUT EFI_GRAPHICS_OUTPUT_MODE_INFORMATION  **Info
  )
{
  RP1_DSI_GOP_CONTEXT  *Context;

  if ((This == NULL) || (SizeOfInfo == NULL) || (Info == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Context = RP1_DSI_CONTEXT_FROM_GOP (This);
  if (ModeNumber >= Context->Mode.MaxMode) {
    return EFI_UNSUPPORTED;
  }

  *Info = AllocateCopyPool (sizeof (Context->ModeInfo), &Context->ModeInfo);
  if (*Info == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  *SizeOfInfo = sizeof (Context->ModeInfo);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
Rp1DsiLandscapeGopQueryMode (
  IN  EFI_GRAPHICS_OUTPUT_PROTOCOL          *This,
  IN  UINT32                                ModeNumber,
  OUT UINTN                                 *SizeOfInfo,
  OUT EFI_GRAPHICS_OUTPUT_MODE_INFORMATION  **Info
  )
{
  RP1_DSI_GOP_CONTEXT  *Context;

  if ((This == NULL) || (SizeOfInfo == NULL) || (Info == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Context = RP1_DSI_CONTEXT_FROM_LANDSCAPE_GOP (This);
  if (ModeNumber >= Context->LandscapeMode.MaxMode) {
    return EFI_UNSUPPORTED;
  }

  *Info = AllocateCopyPool (
            sizeof (Context->LandscapeModeInfo),
            &Context->LandscapeModeInfo
            );
  if (*Info == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  *SizeOfInfo = sizeof (Context->LandscapeModeInfo);
  return EFI_SUCCESS;
}

STATIC
VOID
Rp1DsiFlushFramebuffer (
  IN RP1_DSI_GOP_CONTEXT  *Context
  )
{
  (VOID)Context->PciIo->Flush (Context->PciIo);
}

STATIC
EFI_STATUS
EFIAPI
Rp1DsiGopSetMode (
  IN EFI_GRAPHICS_OUTPUT_PROTOCOL  *This,
  IN UINT32                       ModeNumber
  )
{
  RP1_DSI_GOP_CONTEXT  *Context;

  if (This == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Context = RP1_DSI_CONTEXT_FROM_GOP (This);
  if (ModeNumber >= Context->Mode.MaxMode) {
    return EFI_UNSUPPORTED;
  }

  SetMem32 (
    (VOID *)(UINTN)Context->FrameBufferBase,
    RP1_DSI_FRAMEBUFFER_SIZE,
    0xff000000U
    );
  Context->Mode.Mode = ModeNumber;
  Rp1DsiFlushFramebuffer (Context);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
Rp1DsiLandscapeGopSetMode (
  IN EFI_GRAPHICS_OUTPUT_PROTOCOL  *This,
  IN UINT32                        ModeNumber
  )
{
  RP1_DSI_GOP_CONTEXT  *Context;

  if (This == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Context = RP1_DSI_CONTEXT_FROM_LANDSCAPE_GOP (This);
  if (ModeNumber >= Context->LandscapeMode.MaxMode) {
    return EFI_UNSUPPORTED;
  }

  SetMem32 (Context->LandscapeBuffer, RP1_DSI_FRAMEBUFFER_SIZE, 0xff000000U);
  SetMem32 (
    (VOID *)(UINTN)Context->FrameBufferBase,
    RP1_DSI_FRAMEBUFFER_SIZE,
    0xff000000U
    );
  Context->LandscapeMode.Mode = ModeNumber;
  Rp1DsiFlushFramebuffer (Context);
  return EFI_SUCCESS;
}

STATIC
BOOLEAN
Rp1DsiVideoRectangleValid (
  IN UINTN  X,
  IN UINTN  Y,
  IN UINTN  Width,
  IN UINTN  Height,
  IN UINTN  ScreenWidth,
  IN UINTN  ScreenHeight
  )
{
  return (Width != 0) && (Height != 0) &&
         (X < ScreenWidth) && (Y < ScreenHeight) &&
         (Width <= ScreenWidth - X) &&
         (Height <= ScreenHeight - Y);
}

STATIC
EFI_STATUS
Rp1DsiValidateBufferStride (
  IN OUT UINTN  *Delta,
  IN     UINTN  X,
  IN     UINTN  Width
  )
{
  UINTN  Required;

  if ((Delta == NULL) || (Width > (MAX_UINTN / sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL))) ||
      (X > (MAX_UINTN / sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL)) - Width)) {
    return EFI_INVALID_PARAMETER;
  }

  Required = (X + Width) * sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL);
  if (*Delta == 0) {
    if (X != 0) {
      return EFI_INVALID_PARAMETER;
    }

    *Delta = Required;
  }

  if ((*Delta < Required) || ((*Delta % sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL)) != 0)) {
    return EFI_INVALID_PARAMETER;
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
Rp1DsiGopBlt (
  IN EFI_GRAPHICS_OUTPUT_PROTOCOL      *This,
  IN EFI_GRAPHICS_OUTPUT_BLT_PIXEL     *BltBuffer OPTIONAL,
  IN EFI_GRAPHICS_OUTPUT_BLT_OPERATION BltOperation,
  IN UINTN                             SourceX,
  IN UINTN                             SourceY,
  IN UINTN                             DestinationX,
  IN UINTN                             DestinationY,
  IN UINTN                             Width,
  IN UINTN                             Height,
  IN UINTN                             Delta OPTIONAL
  )
{
  RP1_DSI_GOP_CONTEXT             *Context;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL   *FrameBuffer;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL   *BufferRow;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL   *VideoRow;
  EFI_STATUS                      Status;
  UINT32                          Fill;
  UINTN                           Row;
  UINTN                           RowBytes;

  if ((This == NULL) || (Width == 0) || (Height == 0) ||
      (BltOperation >= EfiGraphicsOutputBltOperationMax)) {
    return EFI_INVALID_PARAMETER;
  }

  Context     = RP1_DSI_CONTEXT_FROM_GOP (This);
  FrameBuffer = (EFI_GRAPHICS_OUTPUT_BLT_PIXEL *)(UINTN)Context->FrameBufferBase;
  RowBytes    = Width * sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL);

  switch (BltOperation) {
    case EfiBltVideoFill:
      if ((BltBuffer == NULL) ||
          !Rp1DsiVideoRectangleValid (
             DestinationX,
             DestinationY,
             Width,
             Height,
             RP1_DSI_HORIZONTAL_RESOLUTION,
             RP1_DSI_VERTICAL_RESOLUTION
             )) {
        return EFI_INVALID_PARAMETER;
      }

      CopyMem (&Fill, BltBuffer, sizeof (Fill));
      Fill |= 0xff000000U;
      for (Row = 0; Row < Height; Row++) {
        VideoRow = FrameBuffer + ((DestinationY + Row) * RP1_DSI_HORIZONTAL_RESOLUTION) +
                   DestinationX;
        SetMem32 (VideoRow, RowBytes, Fill);
      }

      Rp1DsiFlushFramebuffer (Context);
      return EFI_SUCCESS;

    case EfiBltVideoToBltBuffer:
      if ((BltBuffer == NULL) ||
          !Rp1DsiVideoRectangleValid (
             SourceX,
             SourceY,
             Width,
             Height,
             RP1_DSI_HORIZONTAL_RESOLUTION,
             RP1_DSI_VERTICAL_RESOLUTION
             )) {
        return EFI_INVALID_PARAMETER;
      }

      Status = Rp1DsiValidateBufferStride (&Delta, DestinationX, Width);
      if (EFI_ERROR (Status)) {
        return Status;
      }

      for (Row = 0; Row < Height; Row++) {
        VideoRow  = FrameBuffer + ((SourceY + Row) * RP1_DSI_HORIZONTAL_RESOLUTION) + SourceX;
        BufferRow = (EFI_GRAPHICS_OUTPUT_BLT_PIXEL *)((UINT8 *)BltBuffer +
                      ((DestinationY + Row) * Delta)) + DestinationX;
        CopyMem (BufferRow, VideoRow, RowBytes);
      }

      return EFI_SUCCESS;

    case EfiBltBufferToVideo:
      if ((BltBuffer == NULL) ||
          !Rp1DsiVideoRectangleValid (
             DestinationX,
             DestinationY,
             Width,
             Height,
             RP1_DSI_HORIZONTAL_RESOLUTION,
             RP1_DSI_VERTICAL_RESOLUTION
             )) {
        return EFI_INVALID_PARAMETER;
      }

      Status = Rp1DsiValidateBufferStride (&Delta, SourceX, Width);
      if (EFI_ERROR (Status)) {
        return Status;
      }

      for (Row = 0; Row < Height; Row++) {
        BufferRow = (EFI_GRAPHICS_OUTPUT_BLT_PIXEL *)((UINT8 *)BltBuffer +
                      ((SourceY + Row) * Delta)) + SourceX;
        VideoRow  = FrameBuffer + ((DestinationY + Row) * RP1_DSI_HORIZONTAL_RESOLUTION) +
                    DestinationX;
        CopyMem (VideoRow, BufferRow, RowBytes);
      }

      Rp1DsiFlushFramebuffer (Context);
      return EFI_SUCCESS;

    case EfiBltVideoToVideo:
      if (!Rp1DsiVideoRectangleValid (
             SourceX,
             SourceY,
             Width,
             Height,
             RP1_DSI_HORIZONTAL_RESOLUTION,
             RP1_DSI_VERTICAL_RESOLUTION
             ) ||
          !Rp1DsiVideoRectangleValid (
             DestinationX,
             DestinationY,
             Width,
             Height,
             RP1_DSI_HORIZONTAL_RESOLUTION,
             RP1_DSI_VERTICAL_RESOLUTION
             )) {
        return EFI_INVALID_PARAMETER;
      }

      if (DestinationY > SourceY) {
        for (Row = Height; Row-- > 0;) {
          VideoRow  = FrameBuffer + ((DestinationY + Row) * RP1_DSI_HORIZONTAL_RESOLUTION) +
                      DestinationX;
          BufferRow = FrameBuffer + ((SourceY + Row) * RP1_DSI_HORIZONTAL_RESOLUTION) + SourceX;
          CopyMem (VideoRow, BufferRow, RowBytes);
        }
      } else {
        for (Row = 0; Row < Height; Row++) {
          VideoRow  = FrameBuffer + ((DestinationY + Row) * RP1_DSI_HORIZONTAL_RESOLUTION) +
                      DestinationX;
          BufferRow = FrameBuffer + ((SourceY + Row) * RP1_DSI_HORIZONTAL_RESOLUTION) + SourceX;
          CopyMem (VideoRow, BufferRow, RowBytes);
        }
      }

      Rp1DsiFlushFramebuffer (Context);
      return EFI_SUCCESS;

    default:
      return EFI_INVALID_PARAMETER;
  }
}

/**
  Copy a logical landscape rectangle into the native portrait scanout.

  A clockwise rotation maps logical (X, Y) to physical (719 - Y, X).
  The RP1 DSI DMA remains a simple native 720x1280 linear scanout; only the
  firmware BLT surface is presented as 1280x720.
**/
STATIC
VOID
Rp1DsiRotateLandscapeRectangle (
  IN RP1_DSI_GOP_CONTEXT  *Context,
  IN UINTN                X,
  IN UINTN                Y,
  IN UINTN                Width,
  IN UINTN                Height
  )
{
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *LogicalPixel;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *PhysicalPixel;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *PhysicalFrameBuffer;
  UINTN                          Column;
  UINTN                          Row;

  PhysicalFrameBuffer =
    (EFI_GRAPHICS_OUTPUT_BLT_PIXEL *)(UINTN)Context->FrameBufferBase;

  for (Row = 0; Row < Height; Row++) {
    LogicalPixel = Context->LandscapeBuffer +
                   ((Y + Row) * RP1_DSI_LANDSCAPE_WIDTH) + X;
    PhysicalPixel = PhysicalFrameBuffer +
                    (X * RP1_DSI_HORIZONTAL_RESOLUTION) +
                    (RP1_DSI_HORIZONTAL_RESOLUTION - 1U - (Y + Row));

    for (Column = 0; Column < Width; Column++) {
      *PhysicalPixel = *LogicalPixel++;
      PhysicalPixel += RP1_DSI_HORIZONTAL_RESOLUTION;
    }
  }

  Rp1DsiFlushFramebuffer (Context);
}

STATIC
EFI_STATUS
EFIAPI
Rp1DsiLandscapeGopBlt (
  IN EFI_GRAPHICS_OUTPUT_PROTOCOL       *This,
  IN EFI_GRAPHICS_OUTPUT_BLT_PIXEL      *BltBuffer OPTIONAL,
  IN EFI_GRAPHICS_OUTPUT_BLT_OPERATION  BltOperation,
  IN UINTN                              SourceX,
  IN UINTN                              SourceY,
  IN UINTN                              DestinationX,
  IN UINTN                              DestinationY,
  IN UINTN                              Width,
  IN UINTN                              Height,
  IN UINTN                              Delta OPTIONAL
  )
{
  RP1_DSI_GOP_CONTEXT            *Context;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *BufferRow;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *LogicalRow;
  EFI_STATUS                     Status;
  UINT32                         Fill;
  UINTN                          Row;
  UINTN                          RowBytes;

  if ((This == NULL) || (Width == 0) || (Height == 0) ||
      (BltOperation >= EfiGraphicsOutputBltOperationMax)) {
    return EFI_INVALID_PARAMETER;
  }

  Context  = RP1_DSI_CONTEXT_FROM_LANDSCAPE_GOP (This);
  RowBytes = Width * sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL);

  switch (BltOperation) {
    case EfiBltVideoFill:
      if ((BltBuffer == NULL) ||
          !Rp1DsiVideoRectangleValid (
             DestinationX,
             DestinationY,
             Width,
             Height,
             RP1_DSI_LANDSCAPE_WIDTH,
             RP1_DSI_LANDSCAPE_HEIGHT
             )) {
        return EFI_INVALID_PARAMETER;
      }

      CopyMem (&Fill, BltBuffer, sizeof (Fill));
      Fill |= 0xff000000U;
      for (Row = 0; Row < Height; Row++) {
        LogicalRow = Context->LandscapeBuffer +
                     ((DestinationY + Row) * RP1_DSI_LANDSCAPE_WIDTH) +
                     DestinationX;
        SetMem32 (LogicalRow, RowBytes, Fill);
      }

      Rp1DsiRotateLandscapeRectangle (
        Context,
        DestinationX,
        DestinationY,
        Width,
        Height
        );
      return EFI_SUCCESS;

    case EfiBltVideoToBltBuffer:
      if ((BltBuffer == NULL) ||
          !Rp1DsiVideoRectangleValid (
             SourceX,
             SourceY,
             Width,
             Height,
             RP1_DSI_LANDSCAPE_WIDTH,
             RP1_DSI_LANDSCAPE_HEIGHT
             )) {
        return EFI_INVALID_PARAMETER;
      }

      Status = Rp1DsiValidateBufferStride (&Delta, DestinationX, Width);
      if (EFI_ERROR (Status)) {
        return Status;
      }

      for (Row = 0; Row < Height; Row++) {
        LogicalRow = Context->LandscapeBuffer +
                     ((SourceY + Row) * RP1_DSI_LANDSCAPE_WIDTH) + SourceX;
        BufferRow = (EFI_GRAPHICS_OUTPUT_BLT_PIXEL *)((UINT8 *)BltBuffer +
                      ((DestinationY + Row) * Delta)) + DestinationX;
        CopyMem (BufferRow, LogicalRow, RowBytes);
      }

      return EFI_SUCCESS;

    case EfiBltBufferToVideo:
      if ((BltBuffer == NULL) ||
          !Rp1DsiVideoRectangleValid (
             DestinationX,
             DestinationY,
             Width,
             Height,
             RP1_DSI_LANDSCAPE_WIDTH,
             RP1_DSI_LANDSCAPE_HEIGHT
             )) {
        return EFI_INVALID_PARAMETER;
      }

      Status = Rp1DsiValidateBufferStride (&Delta, SourceX, Width);
      if (EFI_ERROR (Status)) {
        return Status;
      }

      for (Row = 0; Row < Height; Row++) {
        BufferRow = (EFI_GRAPHICS_OUTPUT_BLT_PIXEL *)((UINT8 *)BltBuffer +
                      ((SourceY + Row) * Delta)) + SourceX;
        LogicalRow = Context->LandscapeBuffer +
                     ((DestinationY + Row) * RP1_DSI_LANDSCAPE_WIDTH) +
                     DestinationX;
        CopyMem (LogicalRow, BufferRow, RowBytes);
      }

      Rp1DsiRotateLandscapeRectangle (
        Context,
        DestinationX,
        DestinationY,
        Width,
        Height
        );
      return EFI_SUCCESS;

    case EfiBltVideoToVideo:
      if (!Rp1DsiVideoRectangleValid (
             SourceX,
             SourceY,
             Width,
             Height,
             RP1_DSI_LANDSCAPE_WIDTH,
             RP1_DSI_LANDSCAPE_HEIGHT
             ) ||
          !Rp1DsiVideoRectangleValid (
             DestinationX,
             DestinationY,
             Width,
             Height,
             RP1_DSI_LANDSCAPE_WIDTH,
             RP1_DSI_LANDSCAPE_HEIGHT
             )) {
        return EFI_INVALID_PARAMETER;
      }

      if (DestinationY > SourceY) {
        for (Row = Height; Row-- > 0;) {
          LogicalRow = Context->LandscapeBuffer +
                       ((DestinationY + Row) * RP1_DSI_LANDSCAPE_WIDTH) +
                       DestinationX;
          BufferRow = Context->LandscapeBuffer +
                      ((SourceY + Row) * RP1_DSI_LANDSCAPE_WIDTH) + SourceX;
          CopyMem (LogicalRow, BufferRow, RowBytes);
        }
      } else {
        for (Row = 0; Row < Height; Row++) {
          LogicalRow = Context->LandscapeBuffer +
                       ((DestinationY + Row) * RP1_DSI_LANDSCAPE_WIDTH) +
                       DestinationX;
          BufferRow = Context->LandscapeBuffer +
                      ((SourceY + Row) * RP1_DSI_LANDSCAPE_WIDTH) + SourceX;
          CopyMem (LogicalRow, BufferRow, RowBytes);
        }
      }

      Rp1DsiRotateLandscapeRectangle (
        Context,
        DestinationX,
        DestinationY,
        Width,
        Height
        );
      return EFI_SUCCESS;

    default:
      return EFI_INVALID_PARAMETER;
  }
}

STATIC
VOID
Rp1DsiInitializeDevicePath (
  IN OUT RP1_DSI_GOP_CONTEXT  *Context
  )
{
  Context->DevicePath.Vendor.Header.Type      = HARDWARE_DEVICE_PATH;
  Context->DevicePath.Vendor.Header.SubType   = HW_VENDOR_DP;
  Context->DevicePath.Vendor.Header.Length[0] = (UINT8)sizeof (VENDOR_DEVICE_PATH);
  Context->DevicePath.Vendor.Header.Length[1] = (UINT8)(sizeof (VENDOR_DEVICE_PATH) >> 8);
  CopyGuid (&Context->DevicePath.Vendor.Guid, &mRp1DsiDisplayDevicePathGuid);

  Context->DevicePath.End.Type      = END_DEVICE_PATH_TYPE;
  Context->DevicePath.End.SubType   = END_ENTIRE_DEVICE_PATH_SUBTYPE;
  Context->DevicePath.End.Length[0] = sizeof (EFI_DEVICE_PATH_PROTOCOL);
  Context->DevicePath.End.Length[1] = 0;
}

STATIC
VOID
Rp1DsiInitializeGop (
  IN OUT RP1_DSI_GOP_CONTEXT  *Context
  )
{
  //
  // Keep a native linear GOP for loaders that hand the framebuffer directly
  // to the OS.  It has no device path, so console discovery selects only the
  // public landscape GOP below.
  //
  Context->Gop.QueryMode = Rp1DsiGopQueryMode;
  Context->Gop.SetMode   = Rp1DsiGopSetMode;
  Context->Gop.Blt       = Rp1DsiGopBlt;
  Context->Gop.Mode      = &Context->Mode;

  Context->Mode.MaxMode         = 1;
  Context->Mode.Mode            = 0;
  Context->Mode.Info            = &Context->ModeInfo;
  Context->Mode.SizeOfInfo      = sizeof (Context->ModeInfo);
  Context->Mode.FrameBufferBase = Context->FrameBufferBase;
  Context->Mode.FrameBufferSize = RP1_DSI_FRAMEBUFFER_SIZE;

  Context->ModeInfo.Version              = 0;
  Context->ModeInfo.HorizontalResolution = RP1_DSI_HORIZONTAL_RESOLUTION;
  Context->ModeInfo.VerticalResolution   = RP1_DSI_VERTICAL_RESOLUTION;
  Context->ModeInfo.PixelFormat          = PixelBlueGreenRedReserved8BitPerColor;
  ZeroMem (&Context->ModeInfo.PixelInformation, sizeof (Context->ModeInfo.PixelInformation));
  Context->ModeInfo.PixelsPerScanLine = RP1_DSI_HORIZONTAL_RESOLUTION;

  //
  // Touch Display 2 is physically 720x1280.  Present the standard landscape
  // mounting as a BLT-only 1280x720 GOP and rotate updates into native memory.
  //
  Context->LandscapeGop.QueryMode = Rp1DsiLandscapeGopQueryMode;
  Context->LandscapeGop.SetMode   = Rp1DsiLandscapeGopSetMode;
  Context->LandscapeGop.Blt       = Rp1DsiLandscapeGopBlt;
  Context->LandscapeGop.Mode      = &Context->LandscapeMode;

  Context->LandscapeMode.MaxMode         = 1;
  Context->LandscapeMode.Mode            = 0;
  Context->LandscapeMode.Info            = &Context->LandscapeModeInfo;
  Context->LandscapeMode.SizeOfInfo      = sizeof (Context->LandscapeModeInfo);
  Context->LandscapeMode.FrameBufferBase = 0;
  Context->LandscapeMode.FrameBufferSize = 0;

  Context->LandscapeModeInfo.Version              = 0;
  Context->LandscapeModeInfo.HorizontalResolution = RP1_DSI_LANDSCAPE_WIDTH;
  Context->LandscapeModeInfo.VerticalResolution   = RP1_DSI_LANDSCAPE_HEIGHT;
  Context->LandscapeModeInfo.PixelFormat          = PixelBltOnly;
  ZeroMem (
    &Context->LandscapeModeInfo.PixelInformation,
    sizeof (Context->LandscapeModeInfo.PixelInformation)
    );
  Context->LandscapeModeInfo.PixelsPerScanLine = RP1_DSI_LANDSCAPE_WIDTH;

  Context->Transform.Size          = sizeof (Context->Transform);
  Context->Transform.Version       = EFI_GRAPHICS_OUTPUT_TRANSFORM_VERSION;
  Context->Transform.Flags         = EFI_GRAPHICS_OUTPUT_TRANSFORM_FIXED_SCANOUT;
  Context->Transform.Rotation      = EFI_GRAPHICS_OUTPUT_TRANSFORM_ROTATION_90;
  Context->Transform.LogicalWidth  = RP1_DSI_LANDSCAPE_WIDTH;
  Context->Transform.LogicalHeight = RP1_DSI_LANDSCAPE_HEIGHT;
  Context->Transform.LinearGop     = &Context->Gop;

  Rp1DsiInitializeEdid (Context);
  Rp1DsiInitializeDevicePath (Context);
}

STATIC
EFI_STATUS
Rp1DsiAllocateFramebuffer (
  IN OUT RP1_DSI_GOP_CONTEXT  *Context
  )
{
  EFI_CPU_ARCH_PROTOCOL  *Cpu;
  EFI_STATUS             Status;
  EFI_STATUS             AttributeStatus;
  UINTN                  NumberOfBytes;

  Context->FrameBufferPages = EFI_SIZE_TO_PAGES (RP1_DSI_FRAMEBUFFER_SIZE);
  Context->FrameBufferBase  = 0xbfffffffULL;
  Status = gBS->AllocatePages (
                  AllocateMaxAddress,
                  EfiReservedMemoryType,
                  Context->FrameBufferPages,
                  &Context->FrameBufferBase
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  SetMem32 (
    (VOID *)(UINTN)Context->FrameBufferBase,
    RP1_DSI_FRAMEBUFFER_SIZE,
    0xff000000U
    );

  Status = gBS->LocateProtocol (&gEfiCpuArchProtocolGuid, NULL, (VOID **)&Cpu);
  if (EFI_ERROR (Status)) {
    goto ErrorFreePages;
  }

  AttributeStatus = Cpu->SetMemoryAttributes (
                           Cpu,
                           Context->FrameBufferBase,
                           EFI_PAGES_TO_SIZE (Context->FrameBufferPages),
                           EFI_MEMORY_WC
                           );
  if (EFI_ERROR (AttributeStatus)) {
    AttributeStatus = Cpu->SetMemoryAttributes (
                             Cpu,
                             Context->FrameBufferBase,
                             EFI_PAGES_TO_SIZE (Context->FrameBufferPages),
                             EFI_MEMORY_WT
                             );
  }

  if (EFI_ERROR (AttributeStatus)) {
    AttributeStatus = Cpu->SetMemoryAttributes (
                             Cpu,
                             Context->FrameBufferBase,
                             EFI_PAGES_TO_SIZE (Context->FrameBufferPages),
                             EFI_MEMORY_UC
                             );
  }

  if (EFI_ERROR (AttributeStatus)) {
    Status = AttributeStatus;
    goto ErrorFreePages;
  }

  NumberOfBytes = RP1_DSI_FRAMEBUFFER_SIZE;
  Status = Context->PciIo->Map (
                             Context->PciIo,
                             EfiPciIoOperationBusMasterRead,
                             (VOID *)(UINTN)Context->FrameBufferBase,
                             &NumberOfBytes,
                             &Context->DmaAddress,
                             &Context->DmaMapping
                             );
  if (EFI_ERROR (Status) || (NumberOfBytes != RP1_DSI_FRAMEBUFFER_SIZE)) {
    if (!EFI_ERROR (Status)) {
      (VOID)Context->PciIo->Unmap (Context->PciIo, Context->DmaMapping);
      Context->DmaMapping = NULL;
      Status = EFI_BAD_BUFFER_SIZE;
    }

    goto ErrorFreePages;
  }

  Rp1DsiFlushFramebuffer (Context);
  return EFI_SUCCESS;

ErrorFreePages:
  gBS->FreePages (Context->FrameBufferBase, Context->FrameBufferPages);
  Context->FrameBufferBase = 0;
  return Status;
}

STATIC
EFI_STATUS
Rp1DsiLocateRp1 (
  OUT RP1_BUS_PROTOCOL     **Rp1Bus,
  OUT EFI_PCI_IO_PROTOCOL **PciIo
  )
{
  EFI_HANDLE  *Handles;
  EFI_STATUS  Status;
  UINTN       HandleCount;
  UINTN       Index;

  if ((Rp1Bus == NULL) || (PciIo == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  *Rp1Bus = NULL;
  *PciIo  = NULL;
  Status  = gBS->LocateHandleBuffer (
                   ByProtocol,
                   &gRp1BusProtocolGuid,
                   NULL,
                   &HandleCount,
                   &Handles
                   );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = EFI_NOT_FOUND;
  for (Index = 0; Index < HandleCount; Index++) {
    Status = gBS->HandleProtocol (
                    Handles[Index],
                    &gRp1BusProtocolGuid,
                    (VOID **)Rp1Bus
                    );
    if (EFI_ERROR (Status)) {
      continue;
    }

    Status = gBS->HandleProtocol (
                    Handles[Index],
                    &gEfiPciIoProtocolGuid,
                    (VOID **)PciIo
                    );
    if (!EFI_ERROR (Status)) {
      break;
    }

    *Rp1Bus = NULL;
  }

  FreePool (Handles);
  return Status;
}

EFI_STATUS
EFIAPI
Rp1DsiGopDxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  RP1_DSI_GOP_CONTEXT  *Context;
  RP1_BUS_PROTOCOL     *Rp1Bus;
  EFI_STATUS           Status;

  Context = AllocateZeroPool (sizeof (*Context));
  if (Context == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Context->Signature = RP1_DSI_GOP_SIGNATURE;
  Status = Rp1DsiLocateRp1 (&Rp1Bus, &Context->PciIo);
  if (EFI_ERROR (Status)) {
    goto ErrorFreeContext;
  }

  Context->Hardware.PeripheralBase = Rp1Bus->GetPeripheralBase (Rp1Bus);
  Status = Rp1DsiHardwareInitialize (&Context->Hardware);
  if (EFI_ERROR (Status)) {
    goto ErrorFreeContext;
  }

  Status = Rp1DsiPanelInitialize (&Context->Hardware);
  if (EFI_ERROR (Status)) {
    goto ErrorFreeContext;
  }

  Status = Rp1DsiAllocateFramebuffer (Context);
  if (EFI_ERROR (Status)) {
    goto ErrorFreeContext;
  }

  Context->LandscapeBuffer = AllocatePool (RP1_DSI_FRAMEBUFFER_SIZE);
  if (Context->LandscapeBuffer == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    goto ErrorUnmapFramebuffer;
  }

  SetMem32 (Context->LandscapeBuffer, RP1_DSI_FRAMEBUFFER_SIZE, 0xff000000U);

  Rp1DsiInitializeGop (Context);
  Status = Rp1DsiStartScanout (&Context->Hardware, Context->DmaAddress);
  if (EFI_ERROR (Status)) {
    goto ErrorUnmapFramebuffer;
  }

  (VOID)Rp1DsiPanelEnableBacklight (&Context->Hardware);

  //
  // Publish the landscape console first.  LocateProtocol() returns the first
  // matching GOP, and several UEFI applications use it instead of the GOP on
  // ConsoleOutHandle.  Installing the native portrait handoff GOP first made
  // those applications bypass the rotated console even though GraphicsConsole
  // itself correctly selected 1280x720.
  //
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &Context->Handle,
                  &gEfiGraphicsOutputProtocolGuid,
                  &Context->LandscapeGop,
                  &gEfiDevicePathProtocolGuid,
                  &Context->DevicePath,
                  &gEfiEdidDiscoveredProtocolGuid,
                  &Context->EdidDiscovered,
                  &gEfiEdidActiveProtocolGuid,
                  &Context->EdidActive,
                  &mGraphicsOutputTransformProtocolGuid,
                  &Context->Transform,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    // DMA is live; retaining its reserved buffer is safer than freeing it.
    return Status;
  }

  //
  // Keep the direct native framebuffer available for loaders that must pass a
  // linear scanout to the OS, but publish it only after the public console GOP
  // and without a device path so it can never become a ConOut device.
  //
  Status = gBS->InstallProtocolInterface (
                  &Context->NativeHandle,
                  &gEfiGraphicsOutputProtocolGuid,
                  EFI_NATIVE_INTERFACE,
                  &Context->Gop
                  );
  if (EFI_ERROR (Status)) {
    // DMA is live; retaining its reserved buffer is safer than freeing it.
    return Status;
  }

  return EFI_SUCCESS;

ErrorUnmapFramebuffer:
  if (Context->LandscapeBuffer != NULL) {
    FreePool (Context->LandscapeBuffer);
  }

  (VOID)Context->PciIo->Unmap (Context->PciIo, Context->DmaMapping);
  gBS->FreePages (Context->FrameBufferBase, Context->FrameBufferPages);
ErrorFreeContext:
  FreePool (Context);
  return Status;
}

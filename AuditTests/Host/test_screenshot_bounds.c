/* Credits: ©DNMTechLabs / dnmtechlabs@gmail.com */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef NULL
#undef NULL
#endif
#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/DevicePath.h>
#include <Protocol/GraphicsOutput.h>

#include "../../BootLoaderSelectorPkg/BootLoaderSelector/Utils.h"

EFI_HANDLE gImageHandle = NULL;
EFI_SYSTEM_TABLE *gST = NULL;
EFI_BOOT_SERVICES *gBS = NULL;
EFI_GUID gEfiGraphicsOutputProtocolGuid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
EFI_GUID gEfiDevicePathProtocolGuid = EFI_DEVICE_PATH_PROTOCOL_GUID;

static EFI_GRAPHICS_OUTPUT_PROTOCOL gGop;
static EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE gGopMode;
static EFI_GRAPHICS_OUTPUT_MODE_INFORMATION gGopInfo;
static EFI_GRAPHICS_OUTPUT_PROTOCOL gWrongGop;
static EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE gWrongGopMode;
static EFI_GRAPHICS_OUTPUT_MODE_INFORMATION gWrongGopInfo;
static EFI_DEVICE_PATH_PROTOCOL gConsoleDevicePath;
static BOOLEAN gDirectGopAvailable;
static BOOLEAN gDevicePathAvailable;
static BOOLEAN gLocateDevicePathSucceeds;
static UINTN gFallbackGopCount;
static UINTN gLocateDevicePathCalls;
static UINTN gLocateHandleCalls;
static EFI_HANDLE gConsoleHandle = (EFI_HANDLE)(UINTN)1;
static EFI_HANDLE gActiveGopHandle = (EFI_HANDLE)(UINTN)2;
static EFI_HANDLE gWrongGopHandle = (EFI_HANDLE)(UINTN)3;
static UINTN gAllocationCalls;
static UINTN gFreeCalls;
static BOOLEAN gAllowAllocations;
static UINTN gFileOpenCalls;
static BOOLEAN gPretendPathsExist;
static EFI_FILE_PROTOCOL gExistingFile;
static BOOLEAN gAllowVolumeOpen;
static UINTN gScreenshotCreateCalls;

VOID *EFIAPI AllocateZeroPool(IN UINTN AllocationSize) {
  gAllocationCalls++;
  if (!gAllowAllocations) return NULL;
  return calloc(1U, (size_t)AllocationSize);
}

VOID EFIAPI FreePool(IN JCONST VOID *Buffer) {
  if (Buffer != NULL) {
    gFreeCalls++;
    free((void *)Buffer);
  }
}

VOID *EFIAPI ZeroMem(OUT VOID *Buffer, IN UINTN Length) {
  return memset(Buffer, 0, (size_t)Length);
}

UINTN EFIAPI UnicodeSPrint(
  OUT CHAR16 *StartOfBuffer,
  IN UINTN BufferSize,
  IN CONST CHAR16 *FormatString,
  ...
  ) {
  (void)FormatString;
  /* Deterministic valid path for the pathname-buffer regression. */
  BlsSafeStrCpy(StartOfBuffer, BufferSize / sizeof(CHAR16),
                L"\\EFI\\BOOT\\Screenshots\\BLS-123456789ABCDEF0-001.bmp");
  return BlsStrLen16Bounded(StartOfBuffer, BufferSize / sizeof(CHAR16));
}

UINTN BlsStrLen16Bounded(IN CONST CHAR16 *Str, IN UINTN MaxChars) {
  UINTN length;
  length = 0;
  if (Str == NULL) return 0;
  while (length < MaxChars && Str[length] != L'\0') length++;
  return length;
}

VOID BlsSafeStrCpy(OUT CHAR16 *Dst, IN UINTN DstChars, IN CONST CHAR16 *Src) {
  UINTN index;
  if (Dst == NULL || DstChars == 0) return;
  index = 0;
  while (Src != NULL && index + 1U < DstChars && Src[index] != L'\0') {
    Dst[index] = Src[index];
    index++;
  }
  Dst[index] = L'\0';
}

EFI_STATUS BlsOpenVolumeRoot(IN EFI_HANDLE VolumeHandle, OUT EFI_FILE_PROTOCOL **Root) {
  (void)VolumeHandle;
  if (Root != NULL) *Root = gAllowVolumeOpen ? &gExistingFile : NULL;
  return gAllowVolumeOpen ? EFI_SUCCESS : EFI_UNSUPPORTED;
}

EFI_STATUS BlsFileOpen(
  IN EFI_FILE_PROTOCOL *Parent,
  OUT EFI_FILE_PROTOCOL **File,
  IN CONST CHAR16 *Path,
  IN UINT64 OpenMode,
  IN UINT64 Attributes
  ) {
  (void)Parent;
  (void)Path;
  (void)OpenMode;
  (void)Attributes;
  gFileOpenCalls++;
  if (File == NULL) return EFI_INVALID_PARAMETER;
  if (gAllowVolumeOpen) {
    if ((Attributes & EFI_FILE_DIRECTORY) != 0U) {
      *File = &gExistingFile;
      return EFI_SUCCESS;
    }
    if ((OpenMode & EFI_FILE_MODE_CREATE) != 0U) {
      gScreenshotCreateCalls++;
      *File = &gExistingFile;
      return EFI_SUCCESS;
    }
    *File = NULL;
    return EFI_NOT_FOUND;
  }
  if (gPretendPathsExist) {
    *File = &gExistingFile;
    return EFI_SUCCESS;
  }
  *File = NULL;
  return EFI_UNSUPPORTED;
}

EFI_STATUS BlsFileWriteAll(IN EFI_FILE_PROTOCOL *File, IN CONST VOID *Buffer, IN UINTN BufferSize) {
  (void)File;
  (void)Buffer;
  (void)BufferSize;
  return EFI_UNSUPPORTED;
}

EFI_STATUS BlsFileFlush(IN EFI_FILE_PROTOCOL *File) {
  (void)File;
  return EFI_UNSUPPORTED;
}

EFI_STATUS BlsFileDelete(IN OUT EFI_FILE_PROTOCOL **File) {
  if (File != NULL) *File = NULL;
  return EFI_UNSUPPORTED;
}

EFI_STATUS BlsFileClose(IN OUT EFI_FILE_PROTOCOL **File) {
  if (File != NULL) *File = NULL;
  return EFI_SUCCESS;
}

VOID BlsFileCloseBestEffort(IN OUT EFI_FILE_PROTOCOL **File) {
  if (File != NULL) *File = NULL;
}

static EFI_STATUS EFIAPI TestGetNextMonotonicCount(OUT UINT64 *Count) {
  if (Count == NULL) return EFI_INVALID_PARAMETER;
  *Count = 0x123456789ABCDEF0ULL;
  return EFI_SUCCESS;
}

BOOLEAN EFIAPI IsDevicePathValid(
  IN CONST EFI_DEVICE_PATH_PROTOCOL *DevicePath,
  IN UINTN MaxSize
  ) {
  (void)MaxSize;
  return (BOOLEAN)(DevicePath == &gConsoleDevicePath);
}

EFI_STATUS BlsGetProtocolInterface(
  IN EFI_HANDLE Handle, IN EFI_GUID *Protocol, OUT VOID **Interface
  ) {
  if (Interface == NULL) return EFI_INVALID_PARAMETER;
  *Interface = NULL;
  if (Protocol == &gEfiGraphicsOutputProtocolGuid) {
    if (Handle == gConsoleHandle) {
      if (!gDirectGopAvailable) return EFI_UNSUPPORTED;
      *Interface = &gGop;
      return EFI_SUCCESS;
    }
    if (Handle == gActiveGopHandle) {
      *Interface = &gGop;
      return EFI_SUCCESS;
    }
    if (Handle == gWrongGopHandle) {
      *Interface = &gWrongGop;
      return EFI_SUCCESS;
    }
    return EFI_NOT_FOUND;
  }
  if (Protocol == &gEfiDevicePathProtocolGuid && Handle == gConsoleHandle) {
    if (!gDevicePathAvailable) return EFI_UNSUPPORTED;
    *Interface = &gConsoleDevicePath;
    return EFI_SUCCESS;
  }
  return EFI_UNSUPPORTED;
}

static EFI_STATUS EFIAPI TestLocateProtocol(
  IN EFI_GUID *Protocol,
  IN VOID *Registration OPTIONAL,
  OUT VOID **Interface
  ) {
  (void)Protocol;
  (void)Registration;
  if (Interface == NULL) return EFI_INVALID_PARAMETER;
  /* This deliberately models the Round37 baseline failure: LocateProtocol()
     returns the first GOP, which is not the active console framebuffer. */
  *Interface = &gWrongGop;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI TestLocateDevicePath(
  IN EFI_GUID *Protocol,
  IN OUT EFI_DEVICE_PATH_PROTOCOL **DevicePath,
  OUT EFI_HANDLE *Device
  ) {
  (void)Protocol;
  gLocateDevicePathCalls++;
  if (DevicePath == NULL || Device == NULL) return EFI_INVALID_PARAMETER;
  if (!gLocateDevicePathSucceeds) return EFI_NOT_FOUND;
  *Device = gActiveGopHandle;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI TestLocateHandlePresence(
  IN EFI_LOCATE_SEARCH_TYPE SearchType,
  IN EFI_GUID *Protocol OPTIONAL,
  IN VOID *SearchKey OPTIONAL,
  IN OUT UINTN *BufferSize,
  OUT EFI_HANDLE *Buffer
  ) {
  (void)SearchType;
  (void)Protocol;
  (void)SearchKey;
  (void)BufferSize;
  (void)Buffer;
  return EFI_UNSUPPORTED;
}

EFI_STATUS BlsLocateProtocolHandlesBounded(
  IN EFI_GUID *Protocol,
  IN UINTN MaxHandles,
  OUT EFI_HANDLE **Handles,
  OUT UINTN *Count
  ) {
  gLocateHandleCalls++;
  if (Protocol == NULL || Handles == NULL || Count == NULL || MaxHandles == 0U) {
    return EFI_INVALID_PARAMETER;
  }
  *Handles = NULL;
  *Count = 0U;
  if (Protocol != &gEfiGraphicsOutputProtocolGuid || gFallbackGopCount == 0U) {
    return EFI_NOT_FOUND;
  }
  if (gFallbackGopCount > MaxHandles) return EFI_COMPROMISED_DATA;
  *Handles = AllocateZeroPool(gFallbackGopCount * sizeof(**Handles));
  if (*Handles == NULL) return EFI_OUT_OF_RESOURCES;
  (*Handles)[0] = gActiveGopHandle;
  if (gFallbackGopCount > 1U) (*Handles)[1] = gWrongGopHandle;
  *Count = gFallbackGopCount;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI TestBlt(
  IN EFI_GRAPHICS_OUTPUT_PROTOCOL *This,
  IN OUT EFI_GRAPHICS_OUTPUT_BLT_PIXEL *BltBuffer OPTIONAL,
  IN EFI_GRAPHICS_OUTPUT_BLT_OPERATION BltOperation,
  IN UINTN SourceX,
  IN UINTN SourceY,
  IN UINTN DestinationX,
  IN UINTN DestinationY,
  IN UINTN Width,
  IN UINTN Height,
  IN UINTN Delta OPTIONAL
  ) {
  (void)This;
  (void)BltBuffer;
  (void)BltOperation;
  (void)SourceX;
  (void)SourceY;
  (void)DestinationX;
  (void)DestinationY;
  (void)Width;
  (void)Height;
  (void)Delta;
  return EFI_SUCCESS;
}

#include "../../BootLoaderSelectorPkg/BootLoaderSelector/Screenshot.c"

int main(void) {
  EFI_BOOT_SERVICES bootServices;
  EFI_SYSTEM_TABLE systemTable;
  EFI_STATUS status;

  memset(&bootServices, 0, sizeof(bootServices));
  memset(&systemTable, 0, sizeof(systemTable));
  memset(&gGop, 0, sizeof(gGop));
  memset(&gGopMode, 0, sizeof(gGopMode));
  memset(&gGopInfo, 0, sizeof(gGopInfo));
  memset(&gWrongGop, 0, sizeof(gWrongGop));
  memset(&gWrongGopMode, 0, sizeof(gWrongGopMode));
  memset(&gWrongGopInfo, 0, sizeof(gWrongGopInfo));
  memset(&gConsoleDevicePath, 0, sizeof(gConsoleDevicePath));

  bootServices.LocateProtocol = TestLocateProtocol;
  bootServices.LocateDevicePath = TestLocateDevicePath;
  bootServices.LocateHandle = TestLocateHandlePresence;
  bootServices.GetNextMonotonicCount = TestGetNextMonotonicCount;
  gBS = &bootServices;
  systemTable.ConsoleOutHandle = gConsoleHandle;
  gST = &systemTable;
  gGop.Blt = TestBlt;
  gGop.Mode = &gGopMode;
  gGopMode.Info = &gGopInfo;
  gGopMode.SizeOfInfo = sizeof(gGopInfo);
  gGopMode.MaxMode = 1U;
  gGopMode.Mode = 0U;
  gWrongGop.Blt = TestBlt;
  gWrongGop.Mode = &gWrongGopMode;
  gWrongGopMode.Info = &gWrongGopInfo;
  gWrongGopMode.SizeOfInfo = sizeof(gWrongGopInfo);
  gWrongGopMode.MaxMode = 1U;
  gWrongGopMode.Mode = 0U;
  gWrongGopInfo.HorizontalResolution = BLS_SCREENSHOT_DIMENSION_MAX + 1U;
  gWrongGopInfo.VerticalResolution = 1U;
  gDirectGopAvailable = TRUE;
  gDevicePathAvailable = TRUE;
  gLocateDevicePathSucceeds = TRUE;
  gFallbackGopCount = 1U;

  /* When ConsoleOut has no GOP directly, resolve the GOP on its device path
     rather than taking the first globally located framebuffer. */
  gDirectGopAvailable = FALSE;
  gDevicePathAvailable = TRUE;
  gLocateDevicePathSucceeds = TRUE;
  gFallbackGopCount = 2U;
  gGopInfo.HorizontalResolution = 1920U;
  gGopInfo.VerticalResolution = 1080U;
  gAllocationCalls = 0U;
  gFreeCalls = 0U;
  gLocateDevicePathCalls = 0U;
  gLocateHandleCalls = 0U;
  gAllowAllocations = TRUE;
  status = BlsSaveScreenshot((EFI_HANDLE)(UINTN)1, NULL, 0);
  assert(status == EFI_UNSUPPORTED);
  assert(gLocateDevicePathCalls == 1U);
  assert(gLocateHandleCalls == 0U);
  assert(gAllocationCalls == 1U);
  assert(gFreeCalls == 1U);
  gAllowAllocations = FALSE;

  /* If the active console cannot be mapped and multiple GOPs exist, fail
     closed instead of capturing an arbitrary framebuffer. */
  gDevicePathAvailable = FALSE;
  gLocateDevicePathSucceeds = FALSE;
  gFallbackGopCount = 2U;
  gAllocationCalls = 0U;
  gFreeCalls = 0U;
  gLocateHandleCalls = 0U;
  gAllowAllocations = TRUE;
  status = BlsSaveScreenshot((EFI_HANDLE)(UINTN)1, NULL, 0);
  assert(status == EFI_UNSUPPORTED);
  assert(gLocateHandleCalls == 1U);
  assert(gAllocationCalls == 1U);
  assert(gFreeCalls == 1U);

  /* A single global GOP remains an unambiguous compatibility fallback. */
  gFallbackGopCount = 1U;
  gAllocationCalls = 0U;
  gFreeCalls = 0U;
  gAllowAllocations = TRUE;
  status = BlsSaveScreenshot((EFI_HANDLE)(UINTN)1, NULL, 0);
  assert(status == EFI_UNSUPPORTED);
  assert(gAllocationCalls == 2U); /* bounded handle list + screenshot row */
  assert(gFreeCalls == 2U);
  gAllowAllocations = FALSE;

  /* UEFI requires Mode to be in 0..MaxMode-1. Reject inconsistent firmware
     metadata before allocating or touching the framebuffer. */
  gDirectGopAvailable = TRUE;
  gGopMode.Mode = gGopMode.MaxMode;
  gAllocationCalls = 0U;
  status = BlsSaveScreenshot((EFI_HANDLE)(UINTN)1, NULL, 0);
  assert(status == EFI_UNSUPPORTED);
  assert(gAllocationCalls == 0U);
  gGopMode.Mode = 0U;
  gGopMode.MaxMode = 0U;
  status = BlsSaveScreenshot((EFI_HANDLE)(UINTN)1, NULL, 0);
  assert(status == EFI_UNSUPPORTED);
  gGopMode.MaxMode = 1U;

  {
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL pixel;
    UINT8 *bytes;
    memset(&pixel, 0, sizeof(pixel));
    pixel.Blue = 0x11U;
    pixel.Green = 0x22U;
    pixel.Red = 0x33U;
    pixel.Reserved = 0x44U;
    bytes = (UINT8 *)&pixel;
    assert(sizeof(pixel) == 4U);
    assert(bytes[0] == 0x11U);
    assert(bytes[1] == 0x22U);
    assert(bytes[2] == 0x33U);
    assert(bytes[3] == 0x44U);
  }

  gGopInfo.HorizontalResolution = BLS_SCREENSHOT_DIMENSION_MAX + 1U;
  gGopInfo.VerticalResolution = 1U;
  gAllocationCalls = 0;
  status = BlsSaveScreenshot((EFI_HANDLE)(UINTN)1, NULL, 0);
  assert(status == EFI_UNSUPPORTED);
  assert(gAllocationCalls == 0U);

  gGopInfo.HorizontalResolution = BLS_SCREENSHOT_DIMENSION_MAX;
  gGopInfo.VerticalResolution = 4097U;
  gAllocationCalls = 0;
  status = BlsSaveScreenshot((EFI_HANDLE)(UINTN)1, NULL, 0);
  assert(status == EFI_UNSUPPORTED);
  assert(gAllocationCalls == 0U);

  gGopInfo.HorizontalResolution = 15360U;
  gGopInfo.VerticalResolution = 8640U;
  gAllocationCalls = 0;
  status = BlsSaveScreenshot((EFI_HANDLE)(UINTN)1, NULL, 0);
  assert(status == EFI_OUT_OF_RESOURCES);
  assert(gAllocationCalls == 1U);

  /* A valid-size capture needs only one row allocation.  The mocked volume
     open fails immediately afterwards, which also proves deterministic
     release of that row on the early-error path. */
  gGopInfo.HorizontalResolution = 1920U;
  gGopInfo.VerticalResolution = 1080U;
  gAllocationCalls = 0U;
  gFreeCalls = 0U;
  gAllowAllocations = TRUE;
  status = BlsSaveScreenshot((EFI_HANDLE)(UINTN)1, NULL, 0);
  assert(status == EFI_UNSUPPORTED);
  assert(gAllocationCalls == 1U);
  assert(gFreeCalls == 1U);
  gAllowAllocations = FALSE;

  {
    EFI_FILE_PROTOCOL root;
    CHAR16 path[BLS_MAX_PATH];

    memset(&root, 0, sizeof(root));
    memset(&gExistingFile, 0, sizeof(gExistingFile));
    gPretendPathsExist = TRUE;
    gFileOpenCalls = 0U;
    status = BlsFindScreenshotPath(&root, path, ARRAY_SIZE(path));
    assert(status == EFI_VOLUME_FULL);
    assert(gFileOpenCalls == BLS_SCREENSHOT_PATH_SCAN_MAX);

    /* Firmware without GetNextMonotonicCount must remain bounded too; a
       saturated screenshot directory must never trigger 65K synchronous
       filesystem probes on the UI thread. */
    bootServices.GetNextMonotonicCount = NULL;
    gFileOpenCalls = 0U;
    status = BlsFindScreenshotPath(&root, path, ARRAY_SIZE(path));
    assert(status == EFI_VOLUME_FULL);
    assert(gFileOpenCalls == BLS_SCREENSHOT_FALLBACK_SCAN_MAX);
    bootServices.GetNextMonotonicCount = TestGetNextMonotonicCount;
    gPretendPathsExist = FALSE;
  }

  /* REGRESSION: report-path buffers must be checked before opening/creating
     the BMP. Returning success with a truncated SavedPath loses the only path
     the caller received for an otherwise successful capture. */
  {
    CHAR16 tinyPath[8];
    gAllowVolumeOpen = TRUE;
    gAllowAllocations = TRUE;
    gDirectGopAvailable = TRUE;
    gGopInfo.HorizontalResolution = 64U;
    gGopInfo.VerticalResolution = 64U;
    gScreenshotCreateCalls = 0U;
    status = BlsSaveScreenshot((EFI_HANDLE)(UINTN)1, tinyPath, ARRAY_SIZE(tinyPath));
    assert(status == EFI_BUFFER_TOO_SMALL);
    assert(gScreenshotCreateCalls == 0U);
    assert(tinyPath[0] == L'\0');
    gAllowVolumeOpen = FALSE;
    gAllowAllocations = FALSE;
  }

  puts("screenshot_bounds: PASS");
  return 0;
}

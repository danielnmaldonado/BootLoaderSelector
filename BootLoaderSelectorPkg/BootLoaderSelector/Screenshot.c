/** @file
  BootLoaderSelector screenshot support.
  Saves current GOP framebuffer as a 32-bit BMP when F10 is pressed.
  Credits: ©DNMTechLabs / dnmtechlabs@gmail.com
**/
#include "Screenshot.h"
#include "Utils.h"
#include <Library/BaseMemoryLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/DevicePath.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/SimpleFileSystem.h>

#define BLS_BMP_HEADER_SIZE 54U
#define BLS_SCREENSHOT_DIR  L"\\EFI\\BOOT\\Screenshots"
#define BLS_SCREENSHOT_MONO_BASE L"\\EFI\\BOOT\\Screenshots\\BLS-%08x%08x-%03u.bmp"
#define BLS_SCREENSHOT_FALLBACK_BASE L"\\EFI\\BOOT\\Screenshots\\BLS-%08x.bmp"
/* Preserve practical resolutions through 16K while bounding row work and
   streamed BMP output derived from firmware-reported GOP dimensions. */
#define BLS_SCREENSHOT_DIMENSION_MAX   32768U
#define BLS_SCREENSHOT_PIXEL_BYTES_MAX (512U * 1024U * 1024U)
#define BLS_SCREENSHOT_PATH_SCAN_MAX     256U
#define BLS_SCREENSHOT_FALLBACK_SCAN_MAX 1024U
#define BLS_SCREENSHOT_GOP_HANDLE_SCAN_MAX 64U

static VOID BlsPutLe16(OUT UINT8 *Ptr, IN UINT16 Value) {
  Ptr[0] = (UINT8)(Value & 0xff);
  Ptr[1] = (UINT8)((Value >> 8) & 0xff);
}

static VOID BlsPutLe32(OUT UINT8 *Ptr, IN UINT32 Value) {
  Ptr[0] = (UINT8)(Value & 0xff);
  Ptr[1] = (UINT8)((Value >> 8) & 0xff);
  Ptr[2] = (UINT8)((Value >> 16) & 0xff);
  Ptr[3] = (UINT8)((Value >> 24) & 0xff);
}

static EFI_STATUS BlsEnsureDirectory(IN EFI_FILE_PROTOCOL *Root, IN CONST CHAR16 *Path) {
  EFI_FILE_PROTOCOL *Dir;
  EFI_STATUS Status;
  if (Root == NULL || Path == NULL) return EFI_INVALID_PARAMETER;
  Dir = NULL;
  Status = BlsFileOpen(Root, &Dir, Path, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, EFI_FILE_DIRECTORY);
  if (Status != EFI_SUCCESS) {
    BlsFileCloseBestEffort(&Dir);
    return Status;
  }
  if (Dir == NULL) return EFI_DEVICE_ERROR;
  return BlsFileClose(&Dir);
}


static EFI_STATUS BlsFindScreenshotPath(IN EFI_FILE_PROTOCOL *Root, OUT CHAR16 *Path, IN UINTN PathChars) {
  UINT64 Monotonic;
  UINT32 Attempt;
  UINT32 AttemptLimit;
  BOOLEAN HaveMonotonic;

  if (Root == NULL || Path == NULL || PathChars == 0) return EFI_INVALID_PARAMETER;
  Path[0] = L'\0';

  Monotonic = 0;
  HaveMonotonic = FALSE;
  if (gBS != NULL && gBS->GetNextMonotonicCount != NULL &&
      gBS->GetNextMonotonicCount(&Monotonic) == EFI_SUCCESS) {
    HaveMonotonic = TRUE;
  }
  AttemptLimit = HaveMonotonic ? BLS_SCREENSHOT_PATH_SCAN_MAX : BLS_SCREENSHOT_FALLBACK_SCAN_MAX;

  for (Attempt = 1U; Attempt <= AttemptLimit; Attempt++) {
    EFI_STATUS Status;
    EFI_FILE_PROTOCOL *Existing;
    CHAR16 Candidate[BLS_MAX_PATH];
    UINTN CandidateLength;

    Existing = NULL;
    Candidate[0] = L'\0';
    if (HaveMonotonic) {
      UnicodeSPrint(Candidate, sizeof(Candidate), BLS_SCREENSHOT_MONO_BASE,
                    (UINT32)(Monotonic >> 32), (UINT32)Monotonic, Attempt);
    } else {
      UnicodeSPrint(Candidate, sizeof(Candidate), BLS_SCREENSHOT_FALLBACK_BASE, Attempt);
    }
    CandidateLength = BlsStrLen16Bounded(Candidate, ARRAY_SIZE(Candidate));
    if (CandidateLength >= ARRAY_SIZE(Candidate) || CandidateLength + 1U > PathChars) {
      return EFI_BUFFER_TOO_SMALL;
    }
    BlsSafeStrCpy(Path, PathChars, Candidate);
    Status = BlsFileOpen(Root, &Existing, Path, EFI_FILE_MODE_READ, 0);
    if (Status == EFI_NOT_FOUND) {
      BlsFileCloseBestEffort(&Existing);
      return EFI_SUCCESS;
    }
    if (Status != EFI_SUCCESS) {
      BlsFileCloseBestEffort(&Existing);
      return Status;
    }
    if (Existing == NULL) return EFI_LOAD_ERROR;
    Status = BlsFileClose(&Existing);
    if (Status != EFI_SUCCESS) return Status;
  }
  return EFI_VOLUME_FULL;
}

static VOID BlsFillBmpHeader(OUT UINT8 *Bmp, IN UINT32 Width, IN UINT32 Height, IN UINT32 PixelBytes) {
  ZeroMem(Bmp, BLS_BMP_HEADER_SIZE);
  Bmp[0] = 'B';
  Bmp[1] = 'M';
  BlsPutLe32(Bmp + 2, BLS_BMP_HEADER_SIZE + PixelBytes);
  BlsPutLe32(Bmp + 10, BLS_BMP_HEADER_SIZE);
  BlsPutLe32(Bmp + 14, 40);
  BlsPutLe32(Bmp + 18, Width);
  BlsPutLe32(Bmp + 22, Height);
  BlsPutLe16(Bmp + 26, 1);
  BlsPutLe16(Bmp + 28, 32);
  BlsPutLe32(Bmp + 30, 0);
  BlsPutLe32(Bmp + 34, PixelBytes);
}

/* Resolve the GOP that actually backs the active console.  UEFI permits one GOP
   instance per framebuffer, so LocateProtocol() is ambiguous on multi-GPU
   machines and may return a framebuffer unrelated to ConsoleOut. */
static EFI_STATUS BlsResolveConsoleGop(OUT EFI_GRAPHICS_OUTPUT_PROTOCOL **Gop) {
  EFI_STATUS Status;
  EFI_GRAPHICS_OUTPUT_PROTOCOL *Resolved;

  if (Gop == NULL) return EFI_INVALID_PARAMETER;
  *Gop = NULL;
  if (gBS == NULL) return EFI_UNSUPPORTED;

  if (gST != NULL && gST->ConsoleOutHandle != NULL) {
    Resolved = NULL;
    Status = BlsGetProtocolInterface(
               gST->ConsoleOutHandle,
               &gEfiGraphicsOutputProtocolGuid,
               (VOID **)&Resolved
               );
    if (Status == EFI_SUCCESS && Resolved != NULL) {
      *Gop = Resolved;
      return EFI_SUCCESS;
    }

    if (gBS->LocateDevicePath != NULL) {
      EFI_DEVICE_PATH_PROTOCOL *ConsolePath;

      ConsolePath = NULL;
      Status = BlsGetProtocolInterface(
                 gST->ConsoleOutHandle,
                 &gEfiDevicePathProtocolGuid,
                 (VOID **)&ConsolePath
                 );
      if (Status == EFI_SUCCESS && ConsolePath != NULL &&
          IsDevicePathValid(ConsolePath, BLS_MAX_DEVICE_PATH_BYTES)) {
        EFI_DEVICE_PATH_PROTOCOL *RemainingPath;
        EFI_HANDLE GopHandle;

        RemainingPath = ConsolePath;
        GopHandle = NULL;
        Status = gBS->LocateDevicePath(
                        &gEfiGraphicsOutputProtocolGuid,
                        &RemainingPath,
                        &GopHandle
                        );
        if (Status == EFI_SUCCESS && GopHandle != NULL && RemainingPath != NULL) {
          Resolved = NULL;
          Status = BlsGetProtocolInterface(
                     GopHandle,
                     &gEfiGraphicsOutputProtocolGuid,
                     (VOID **)&Resolved
                     );
          if (Status == EFI_SUCCESS && Resolved != NULL) {
            *Gop = Resolved;
            return EFI_SUCCESS;
          }
        }
      }
    }
  }

  /* Without a console/device-path match, a global fallback is safe only when
     firmware exposes exactly one GOP handle.  With two or more framebuffers,
     choosing the first handle would make capture nondeterministic. */
  if (gBS->LocateHandle != NULL) {
    EFI_HANDLE *Handles;
    UINTN Count;

    Handles = NULL;
    Count = 0;
    Status = BlsLocateProtocolHandlesBounded(
               &gEfiGraphicsOutputProtocolGuid,
               BLS_SCREENSHOT_GOP_HANDLE_SCAN_MAX,
               &Handles,
               &Count
               );
    if (Status == EFI_SUCCESS) {
      if (Count == 1U && Handles[0] != NULL) {
        Resolved = NULL;
        Status = BlsGetProtocolInterface(
                   Handles[0],
                   &gEfiGraphicsOutputProtocolGuid,
                   (VOID **)&Resolved
                   );
        FreePool(Handles);
        if (Status == EFI_SUCCESS && Resolved != NULL) {
          *Gop = Resolved;
          return EFI_SUCCESS;
        }
        return (Status != EFI_SUCCESS) ? Status : EFI_NOT_FOUND;
      }
      FreePool(Handles);
      return EFI_NOT_FOUND;
    }
    if (Status != EFI_NOT_FOUND && Status != EFI_COMPROMISED_DATA) return Status;
  }

  return EFI_NOT_FOUND;
}

EFI_STATUS BlsSaveScreenshot(IN EFI_HANDLE VolumeHandle, OUT CHAR16 *SavedPath, IN UINTN SavedPathChars) {
  EFI_STATUS Status;
  EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL *BltRow;
  EFI_FILE_PROTOCOL *Root;
  EFI_FILE_PROTOCOL *File;
  UINT8 Header[BLS_BMP_HEADER_SIZE];
  UINTN Width;
  UINTN Height;
  UINT64 PixelBytes64;
  UINT64 RowBytes64;
  UINT32 PixelBytes;
  UINTN RowBytes;
  UINTN X;
  UINTN Y;
  CHAR16 Path[BLS_MAX_PATH];
  BOOLEAN DataDurable;

  if (SavedPath != NULL && SavedPathChars > 0) SavedPath[0] = L'\0';
  if (VolumeHandle == NULL) return EFI_INVALID_PARAMETER;

  Gop = NULL;
  Status = BlsResolveConsoleGop(&Gop);
  if (Status != EFI_SUCCESS || Gop == NULL || Gop->Blt == NULL || Gop->Mode == NULL ||
      Gop->Mode->Info == NULL ||
      Gop->Mode->SizeOfInfo < sizeof(EFI_GRAPHICS_OUTPUT_MODE_INFORMATION) ||
      Gop->Mode->MaxMode == 0U || Gop->Mode->Mode >= Gop->Mode->MaxMode) return EFI_UNSUPPORTED;

  Width = Gop->Mode->Info->HorizontalResolution;
  Height = Gop->Mode->Info->VerticalResolution;
  if (Width == 0 || Height == 0 ||
      Width > BLS_SCREENSHOT_DIMENSION_MAX || Height > BLS_SCREENSHOT_DIMENSION_MAX) {
    return EFI_UNSUPPORTED;
  }

  PixelBytes64 = (UINT64)Width * (UINT64)Height * sizeof(EFI_GRAPHICS_OUTPUT_BLT_PIXEL);
  RowBytes64 = (UINT64)Width * sizeof(EFI_GRAPHICS_OUTPUT_BLT_PIXEL);
  if (PixelBytes64 == 0 || PixelBytes64 > BLS_SCREENSHOT_PIXEL_BYTES_MAX ||
      PixelBytes64 > MAX_UINT32 - BLS_BMP_HEADER_SIZE ||
      RowBytes64 == 0 || RowBytes64 > MAX_UINTN) {
    return EFI_UNSUPPORTED;
  }

  PixelBytes = (UINT32)PixelBytes64;
  RowBytes = (UINTN)RowBytes64;

  BltRow = AllocateZeroPool(RowBytes);
  if (BltRow == NULL) return EFI_OUT_OF_RESOURCES;

  Root = NULL;
  Status = BlsOpenVolumeRoot(VolumeHandle, &Root);
  if (Status != EFI_SUCCESS || Root == NULL) {
    FreePool(BltRow);
    return (Status != EFI_SUCCESS) ? Status : EFI_DEVICE_ERROR;
  }

  Status = BlsEnsureDirectory(Root, L"\\EFI");
  if (Status == EFI_SUCCESS) Status = BlsEnsureDirectory(Root, L"\\EFI\\BOOT");
  if (Status == EFI_SUCCESS) Status = BlsEnsureDirectory(Root, BLS_SCREENSHOT_DIR);
  if (Status != EFI_SUCCESS) {
    BlsFileCloseBestEffort(&Root);
    FreePool(BltRow);
    return Status;
  }

  Status = BlsFindScreenshotPath(Root, Path, ARRAY_SIZE(Path));
  if (Status != EFI_SUCCESS) {
    BlsFileCloseBestEffort(&Root);
    FreePool(BltRow);
    return Status;
  }

  /* A caller that requests the saved pathname must receive it in full.
     Validate before opening the BMP: silently truncating only the returned
     pathname would report a successful screenshot with an unusable location. */
  if (SavedPath != NULL && SavedPathChars > 0U &&
      BlsStrLen16Bounded(Path, ARRAY_SIZE(Path)) + 1U > SavedPathChars) {
    BlsFileCloseBestEffort(&Root);
    FreePool(BltRow);
    return EFI_BUFFER_TOO_SMALL;
  }

  File = NULL;
  Status = BlsFileOpen(Root, &File, Path, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
  if (Status != EFI_SUCCESS) {
    BlsFileCloseBestEffort(&File);
    BlsFileCloseBestEffort(&Root);
    FreePool(BltRow);
    return Status;
  }
  if (File == NULL) {
    BlsFileCloseBestEffort(&Root);
    FreePool(BltRow);
    return EFI_DEVICE_ERROR;
  }
  if (File->SetPosition == NULL) {
    Status = EFI_UNSUPPORTED;
  } else {
    Status = File->SetPosition(File, 0);
  }
  if (Status != EFI_SUCCESS) {
    EFI_STATUS DeleteStatus;
    DeleteStatus = BlsFileDelete(&File);
    if (DeleteStatus != EFI_SUCCESS) Status = DeleteStatus;
    BlsFileCloseBestEffort(&Root);
    FreePool(BltRow);
    return Status;
  }

  BlsFillBmpHeader(Header, (UINT32)Width, (UINT32)Height, PixelBytes);
  {
    UINTN WriteSize;
    WriteSize = sizeof(Header);
    Status = BlsFileWriteAll(File, Header, WriteSize);
  }

  for (Y = 0; Status == EFI_SUCCESS && Y < Height; Y++) {
    UINTN WriteSize;
    Status = Gop->Blt(Gop, BltRow, EfiBltVideoToBltBuffer, 0, Height - 1 - Y, 0, 0, Width, 1, 0);
    if (Status != EFI_SUCCESS) break;
    /* EFI_GRAPHICS_OUTPUT_BLT_PIXEL is standardized as B,G,R,Reserved,
       which is byte-for-byte the 32-bit BMP B,G,R,unused layout.  Keep a
       single row buffer and clear only the unused byte instead of allocating
       and copying a second full row. */
    for (X = 0; X < Width; X++) {
      BltRow[X].Reserved = 0;
    }
    WriteSize = RowBytes;
    Status = BlsFileWriteAll(File, BltRow, WriteSize);
  }

  if (Status == EFI_SUCCESS) Status = BlsFileFlush(File);
  DataDurable = (BOOLEAN)(Status == EFI_SUCCESS);
  if (DataDurable) {
    EFI_STATUS CloseStatus;
    CloseStatus = BlsFileClose(&File);
    if (Status == EFI_SUCCESS && CloseStatus != EFI_SUCCESS) Status = CloseStatus;
  } else {
    /* Delete the incomplete file through its existing handle. Delete closes
       the handle on every return path and avoids a close/reopen race. */
    EFI_STATUS DeleteStatus;
    DeleteStatus = BlsFileDelete(&File);
    if (DeleteStatus != EFI_SUCCESS) Status = DeleteStatus;
  }
  {
    EFI_STATUS RootCloseStatus;
    RootCloseStatus = BlsFileClose(&Root);
    if (Status == EFI_SUCCESS && RootCloseStatus != EFI_SUCCESS) Status = RootCloseStatus;
  }
  FreePool(BltRow);

  if (Status == EFI_SUCCESS && SavedPath != NULL && SavedPathChars > 0) {
    BlsSafeStrCpy(SavedPath, SavedPathChars, Path);
  }
  return Status;
}

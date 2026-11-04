/** @file
  BootLoaderSelector utility helpers.
  Credits: ©DNMTechLabs / dnmtechlabs@gmail.com
**/
#include "Utils.h"
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>

#define BLS_INFO_ALLOCATION_MAX (1024U * 1024U)
#define BLS_INFO_RETRY_MAX      8U

UINTN BlsStrLen16Bounded(IN CONST CHAR16 *Str, IN UINTN MaxChars) {
  UINTN Len = 0;
  if (Str == NULL || MaxChars == 0) return 0;
  while (Len < MaxChars && Str[Len] != L'\0') Len++;
  return Len;
}

UINTN BlsStrLen16(IN CONST CHAR16 *Str) {
  return BlsStrLen16Bounded(Str, 65535);
}

VOID BlsSafeStrCpy(OUT CHAR16 *Dst, IN UINTN DstChars, IN CONST CHAR16 *Src) {
  UINTN Index;
  if (Dst == NULL || DstChars == 0) return;
  if (Src == NULL) {
    Dst[0] = L'\0';
    return;
  }
  for (Index = 0; Index + 1 < DstChars && Src[Index] != L'\0'; Index++) {
    Dst[Index] = Src[Index];
  }
  Dst[Index] = L'\0';
}

VOID BlsSafeStrCat(OUT CHAR16 *Dst, IN UINTN DstChars, IN CONST CHAR16 *Src) {
  UINTN Len;
  if (Dst == NULL || DstChars == 0 || Src == NULL) return;
  Len = BlsStrLen16Bounded(Dst, DstChars);
  if (Len >= DstChars) {
    Dst[DstChars - 1] = L'\0';
    return;
  }
  BlsSafeStrCpy(Dst + Len, DstChars - Len, Src);
}

BOOLEAN BlsStrEq(IN CONST CHAR16 *A, IN CONST CHAR16 *B) {
  UINTN Index;
  if (A == NULL || B == NULL) return FALSE;
  for (Index = 0; Index < 65535 && (A[Index] != L'\0' || B[Index] != L'\0'); Index++) {
    if (A[Index] != B[Index]) return FALSE;
  }
  return (Index < 65535);
}

#define BLS_LOCATE_HANDLE_RETRY_MAX 8U

EFI_STATUS BlsLocateProtocolHandlesBounded(
  IN EFI_GUID *Protocol,
  IN UINTN MaxHandles,
  OUT EFI_HANDLE **Handles,
  OUT UINTN *Count
  ) {
  EFI_STATUS Status;
  EFI_HANDLE *Buffer;
  UINTN BufferSize;
  UINTN AllocatedSize;
  UINTN Attempts;
  UINTN MaximumBytes;

  if (Handles == NULL || Count == NULL) return EFI_INVALID_PARAMETER;
  *Handles = NULL;
  *Count = 0U;
  if (Protocol == NULL || MaxHandles == 0U) return EFI_INVALID_PARAMETER;
  if (gBS == NULL || gBS->LocateHandle == NULL) return EFI_UNSUPPORTED;
  if (MaxHandles > MAX_UINTN / sizeof(EFI_HANDLE)) return EFI_BAD_BUFFER_SIZE;

  MaximumBytes = MaxHandles * sizeof(EFI_HANDLE);
  Buffer = NULL;
  BufferSize = 0U;
  AllocatedSize = 0U;
  Attempts = 0U;

  while (TRUE) {
    Status = gBS->LocateHandle(ByProtocol, Protocol, NULL, &BufferSize, Buffer);
    if (Status == EFI_SUCCESS) {
      UINTN HandleCount;

      if (BufferSize == 0U) {
        if (Buffer != NULL) FreePool(Buffer);
        return EFI_NOT_FOUND;
      }
      if (Buffer == NULL || BufferSize > AllocatedSize ||
          (BufferSize % sizeof(EFI_HANDLE)) != 0U) {
        if (Buffer != NULL) FreePool(Buffer);
        return EFI_COMPROMISED_DATA;
      }
      HandleCount = BufferSize / sizeof(EFI_HANDLE);
      if (HandleCount == 0U || HandleCount > MaxHandles) {
        FreePool(Buffer);
        return EFI_COMPROMISED_DATA;
      }
      *Handles = Buffer;
      *Count = HandleCount;
      return EFI_SUCCESS;
    }

    if (Status == EFI_NOT_FOUND) {
      if (Buffer != NULL) FreePool(Buffer);
      return EFI_NOT_FOUND;
    }
    if (Status != EFI_BUFFER_TOO_SMALL) {
      if (Buffer != NULL) FreePool(Buffer);
      return Status;
    }

    /* LocateHandle() reports the exact byte requirement without allocating.
       Bound that firmware-owned value before any pool request. A changing
       protocol database may legitimately grow between calls, so permit bounded
       retries only when the required size strictly increases. */
    if (Attempts >= BLS_LOCATE_HANDLE_RETRY_MAX || BufferSize == 0U ||
        BufferSize > MaximumBytes || (BufferSize % sizeof(EFI_HANDLE)) != 0U ||
        BufferSize <= AllocatedSize) {
      if (Buffer != NULL) FreePool(Buffer);
      return EFI_COMPROMISED_DATA;
    }
    Attempts++;

    if (Buffer != NULL) FreePool(Buffer);
    /* LocateHandle() overwrites every returned EFI_HANDLE; zero-filling this
       bounded scratch buffer would add memory traffic without improving safety. */
    Buffer = AllocatePool(BufferSize);
    if (Buffer == NULL) return EFI_OUT_OF_RESOURCES;
    AllocatedSize = BufferSize;
  }
}

static CHAR8 BlsAsciiLower(CHAR8 C) {
  if (C >= 'A' && C <= 'Z') return (CHAR8)(C - 'A' + 'a');
  return C;
}

BOOLEAN BlsStriEqAscii(IN CONST CHAR8 *A, IN CONST CHAR8 *B) {
  UINTN Index;
  if (A == NULL || B == NULL) return FALSE;
  for (Index = 0; Index < 65535 && (A[Index] != '\0' || B[Index] != '\0'); Index++) {
    if (BlsAsciiLower(A[Index]) != BlsAsciiLower(B[Index])) return FALSE;
  }
  return (Index < 65535);
}

UINTN BlsAsciiLenBounded(IN CONST CHAR8 *Str, IN UINTN MaxLen) {
  UINTN Len = 0;
  if (Str == NULL) return 0;
  while (Len < MaxLen && Str[Len] != '\0') Len++;
  return Len;
}

BOOLEAN BlsAsciiContains(IN CONST CHAR8 *Haystack, IN CONST CHAR8 *Needle) {
  UINTN H, N, I;
  if (Haystack == NULL || Needle == NULL) return FALSE;
  H = AsciiStrLen(Haystack);
  N = AsciiStrLen(Needle);
  if (N == 0 || H < N) return FALSE;
  for (I = 0; I + N <= H; I++) {
    UINTN J;
    BOOLEAN Match = TRUE;
    for (J = 0; J < N; J++) {
      if (BlsAsciiLower(Haystack[I + J]) != BlsAsciiLower(Needle[J])) {
        Match = FALSE;
        break;
      }
    }
    if (Match) return TRUE;
  }
  return FALSE;
}

VOID BlsAsciiToUnicode(OUT CHAR16 *Dst, IN UINTN DstChars, IN CONST CHAR8 *Src) {
  UINTN Index;
  if (Dst == NULL || DstChars == 0) return;
  if (Src == NULL) {
    Dst[0] = L'\0';
    return;
  }
  for (Index = 0; Index + 1 < DstChars && Src[Index] != '\0'; Index++) {
    Dst[Index] = (CHAR16)(UINT8)Src[Index];
  }
  Dst[Index] = L'\0';
}

VOID BlsTrimAscii(CHAR8 *Str) {
  UINTN Start, End, Len, I;
  if (Str == NULL) return;
  Len = BlsAsciiLenBounded(Str, 65535);
  Start = 0;
  while (Start < Len && (Str[Start] == ' ' || Str[Start] == '\t' || Str[Start] == '\r' || Str[Start] == '\n')) Start++;
  End = Len;
  while (End > Start && (Str[End - 1] == ' ' || Str[End - 1] == '\t' || Str[End - 1] == '\r' || Str[End - 1] == '\n')) End--;
  if (Start > 0) {
    for (I = 0; Start + I < End; I++) Str[I] = Str[Start + I];
    Str[I] = '\0';
  } else if (End < Len) {
    Str[End] = '\0';
  }
}


EFI_STATUS BlsGetProtocolInterface(
  IN EFI_HANDLE Handle,
  IN EFI_GUID *Protocol,
  OUT VOID **Interface
  ) {
  EFI_STATUS Status;

  if (Interface == NULL) return EFI_INVALID_PARAMETER;
  *Interface = NULL;
  if (Handle == NULL || Protocol == NULL) return EFI_INVALID_PARAMETER;
  if (gBS == NULL || gBS->OpenProtocol == NULL || gImageHandle == NULL) return EFI_UNSUPPORTED;

  /* UEFI 2.11 retains HandleProtocol() only for compatibility with old EFI
     software and directs new applications/drivers to OpenProtocol().
     GET_PROTOCOL is the non-owning query mode: it records the application as
     the agent, returns the protocol interface, and does not require a matching
     CloseProtocol(). */
  Status = gBS->OpenProtocol(
                  Handle,
                  Protocol,
                  Interface,
                  gImageHandle,
                  NULL,
                  EFI_OPEN_PROTOCOL_GET_PROTOCOL
                  );
  if (Status != EFI_SUCCESS) *Interface = NULL;
  return Status;
}

EFI_STATUS BlsOpenVolumeRoot(IN EFI_HANDLE VolumeHandle, OUT EFI_FILE_PROTOCOL **Root) {
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *FileSystem;
  EFI_STATUS Status;

  if (Root == NULL) return EFI_INVALID_PARAMETER;
  *Root = NULL;
  if (VolumeHandle == NULL) return EFI_INVALID_PARAMETER;
  FileSystem = NULL;
  Status = BlsGetProtocolInterface(
             VolumeHandle,
             &gEfiSimpleFileSystemProtocolGuid,
             (VOID **)&FileSystem
             );
  if (Status != EFI_SUCCESS) return Status;
  if (FileSystem == NULL || FileSystem->OpenVolume == NULL) return EFI_UNSUPPORTED;

  Status = FileSystem->OpenVolume(FileSystem, Root);
  if (Status != EFI_SUCCESS) {
    /* Root is an OUT result of OpenVolume and is only ours after success. */
    *Root = NULL;
    return Status;
  }
  if (*Root == NULL) return EFI_DEVICE_ERROR;
  return EFI_SUCCESS;
}

EFI_STATUS BlsFileOpen(
  IN EFI_FILE_PROTOCOL *Parent,
  OUT EFI_FILE_PROTOCOL **File,
  IN CONST CHAR16 *Path,
  IN UINT64 OpenMode,
  IN UINT64 Attributes
  ) {
  EFI_STATUS Status;

  if (File == NULL) return EFI_INVALID_PARAMETER;
  *File = NULL;
  if (Parent == NULL || Path == NULL) return EFI_INVALID_PARAMETER;
  /* UEFI 2.11 §13.5.2 permits exactly Read, Read/Write, or
     Create/Read/Write. Attributes are meaningful only for Create and may
     contain only EFI_FILE_VALID_ATTR bits. Reject invalid combinations before
     calling firmware so a future caller cannot feed a non-conforming request
     into an implementation that fails unpredictably. */
  if (OpenMode != EFI_FILE_MODE_READ &&
      OpenMode != (EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE) &&
      OpenMode != (EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE)) {
    return EFI_INVALID_PARAMETER;
  }
  if (((OpenMode & EFI_FILE_MODE_CREATE) == 0U && Attributes != 0U) ||
      (Attributes & ~((UINT64)EFI_FILE_VALID_ATTR)) != 0U) {
    return EFI_INVALID_PARAMETER;
  }
  if (Parent->Open == NULL) return EFI_UNSUPPORTED;

  Status = Parent->Open(Parent, File, (CHAR16 *)Path, OpenMode, Attributes);
  if (Status != EFI_SUCCESS) {
    /* NewHandle is an OUT result of Open and is only ours after success. */
    *File = NULL;
    return Status;
  }
  if (*File == NULL) return EFI_DEVICE_ERROR;
  return EFI_SUCCESS;
}

EFI_STATUS BlsFileGetInfoAlloc(
  IN EFI_FILE_PROTOCOL *File,
  IN CONST EFI_GUID *InformationType,
  IN UINTN MinimumSize,
  OUT VOID **Information,
  OUT UINTN *InformationSize
  ) {
  EFI_STATUS Status;
  VOID *Buffer;
  UINTN BufferSize;
  UINTN AllocatedSize;
  UINTN Attempts;

  if (Information == NULL || InformationSize == NULL) return EFI_INVALID_PARAMETER;
  *Information = NULL;
  *InformationSize = 0;
  if (File == NULL || InformationType == NULL || MinimumSize == 0) return EFI_INVALID_PARAMETER;
  if (File->GetInfo == NULL) return EFI_UNSUPPORTED;

  Buffer = NULL;
  BufferSize = 0;
  AllocatedSize = 0;
  Attempts = 0;
  while (TRUE) {
    Status = File->GetInfo(File, (EFI_GUID *)InformationType, &BufferSize, Buffer);
    if (Status != EFI_BUFFER_TOO_SMALL) break;
    if (Attempts >= BLS_INFO_RETRY_MAX || BufferSize < MinimumSize ||
        BufferSize <= AllocatedSize || BufferSize > BLS_INFO_ALLOCATION_MAX) {
      Status = EFI_COMPROMISED_DATA;
      break;
    }
    Attempts++;

    if (Buffer != NULL) FreePool(Buffer);
    Buffer = AllocateZeroPool(BufferSize);
    if (Buffer == NULL) {
      Status = EFI_OUT_OF_RESOURCES;
      break;
    }
    AllocatedSize = BufferSize;
  }

  if (Status == EFI_SUCCESS) {
    if (Buffer == NULL || BufferSize < MinimumSize || BufferSize > AllocatedSize) {
      Status = EFI_COMPROMISED_DATA;
    } else {
      *Information = Buffer;
      *InformationSize = BufferSize;
      return EFI_SUCCESS;
    }
  }

  if (Buffer != NULL) FreePool(Buffer);
  return Status;
}


EFI_STATUS BlsFileReadExact(IN EFI_FILE_PROTOCOL *File, OUT VOID *Buffer, IN UINTN BufferSize) {
  EFI_STATUS Status;
  UINTN ReadSize;

  if (File == NULL || (Buffer == NULL && BufferSize != 0)) return EFI_INVALID_PARAMETER;
  if (File->Read == NULL) return EFI_UNSUPPORTED;
  if (BufferSize == 0) return EFI_SUCCESS;

  /* For a regular file UEFI Read() returns the complete requested length unless
     the request reaches EOF, in which case the successful transfer is short.
     This helper promises an exact transfer, so a short success is EOF rather
     than a reason for another firmware call. */
  ReadSize = BufferSize;
  Status = File->Read(File, &ReadSize, Buffer);
  if (Status != EFI_SUCCESS) return Status;
  if (ReadSize == BufferSize) return EFI_SUCCESS;
  if (ReadSize < BufferSize) return EFI_END_OF_FILE;

  /* Firmware must never report more bytes than the supplied buffer. */
  return EFI_COMPROMISED_DATA;
}

EFI_STATUS BlsFileWriteAll(IN EFI_FILE_PROTOCOL *File, IN CONST VOID *Buffer, IN UINTN BufferSize) {
  EFI_STATUS Status;
  UINTN WriteSize;

  if (File == NULL || (Buffer == NULL && BufferSize != 0)) return EFI_INVALID_PARAMETER;
  if (File->Write == NULL) return EFI_UNSUPPORTED;
  if (BufferSize == 0) return EFI_SUCCESS;

  /* UEFI 2.11 File.Write() states that partial writes only occur when the
     write attempt reports a data error. EFI_SUCCESS therefore transfers the
     complete requested chunk. A short/oversized successful result is a
     non-conforming firmware response and must fail closed; retrying could turn
     a corrupted transfer contract into apparent data durability. */
  WriteSize = BufferSize;
  Status = File->Write(File, &WriteSize, (VOID *)Buffer);
  if (Status != EFI_SUCCESS) return Status;
  if (WriteSize != BufferSize) return EFI_COMPROMISED_DATA;
  return EFI_SUCCESS;
}

EFI_STATUS BlsFileFlush(IN EFI_FILE_PROTOCOL *File) {
  if (File == NULL) return EFI_INVALID_PARAMETER;
  if (File->Flush == NULL) return EFI_UNSUPPORTED;
  return File->Flush(File);
}

EFI_STATUS BlsFileDelete(IN OUT EFI_FILE_PROTOCOL **File) {
  EFI_FILE_PROTOCOL *Handle;
  EFI_STATUS CloseStatus;

  if (File == NULL) return EFI_INVALID_PARAMETER;
  if (*File == NULL) return EFI_SUCCESS;

  Handle = *File;
  *File = NULL;
  if (Handle->Delete != NULL) {
    /* UEFI Delete closes the file handle on every return path. */
    return Handle->Delete(Handle);
  }

  /* A non-conforming file protocol without Delete must still be closed. */
  if (Handle->Close == NULL) return EFI_UNSUPPORTED;
  CloseStatus = Handle->Close(Handle);
  return (CloseStatus == EFI_SUCCESS) ? EFI_UNSUPPORTED : CloseStatus;
}

EFI_STATUS BlsFileClose(IN OUT EFI_FILE_PROTOCOL **File) {
  EFI_FILE_PROTOCOL *Handle;

  if (File == NULL) return EFI_INVALID_PARAMETER;
  if (*File == NULL) return EFI_SUCCESS;
  Handle = *File;
  *File = NULL;
  if (Handle->Close == NULL) return EFI_UNSUPPORTED;
  return Handle->Close(Handle);
}

EFI_STATUS BlsCloseTimerEvent(IN OUT EFI_EVENT *Event) {
  EFI_EVENT Handle;
  EFI_STATUS Status;

  if (Event == NULL) return EFI_INVALID_PARAMETER;
  if (*Event == NULL) return EFI_SUCCESS;
  if (gBS == NULL || gBS->CloseEvent == NULL) return EFI_UNSUPPORTED;

  /* CloseEvent ends the event lifecycle.  Once it has been invoked, the
     caller must not use the handle again even if a non-conforming firmware
     returns an unexpected status.  Invalidate our only local reference before
     calling into firmware so no cleanup path can accidentally close it twice. */
  Handle = *Event;
  *Event = NULL;
  /* UEFI CloseEvent() itself removes the reference, closes and frees the
     event. An explicit TimerCancel immediately before CloseEvent is redundant
     firmware I/O and is not required to terminate a timer event. */
  Status = gBS->CloseEvent(Handle);
  return Status;
}

VOID BlsFileCloseBestEffort(IN OUT EFI_FILE_PROTOCOL **File) {
  if (File == NULL) return;
  (VOID)BlsFileClose(File);
}

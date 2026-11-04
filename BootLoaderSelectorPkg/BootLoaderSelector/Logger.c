/** @file
  BootLoaderSelector debug logger.
  Credits: ©DNMTechLabs / dnmtechlabs@gmail.com
**/
#include "Logger.h"
#include <Library/BaseMemoryLib.h>
#include <Library/BaseLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/SimpleFileSystem.h>
#include <Guid/FileInfo.h>

#define BLS_LOG_PATH L"\\EFI\\BOOT\\BootLoaderSelector.log"
#define BLS_LOG_DIR  L"\\EFI\\BOOT"
#define BLS_FILE_INFO_MIN_SIZE (SIZE_OF_EFI_FILE_INFO + sizeof(CHAR16))



static EFI_STATUS BlsLogRestoreFileSize(IN EFI_FILE_PROTOCOL *File, IN UINT64 FileSize) {
  EFI_FILE_INFO *Info;
  EFI_STATUS Status;
  UINTN InfoSize;

  if (File == NULL) return EFI_INVALID_PARAMETER;
  if (File->SetInfo == NULL) return EFI_UNSUPPORTED;

  Info = NULL;
  InfoSize = 0;
  Status = BlsFileGetInfoAlloc(
             File,
             &gEfiFileInfoGuid,
             BLS_FILE_INFO_MIN_SIZE,
             (VOID **)&Info,
             &InfoSize
             );

  if (Status == EFI_SUCCESS) {
    if (Info == NULL || Info->Size < BLS_FILE_INFO_MIN_SIZE || Info->Size > InfoSize ||
        (Info->Attribute & EFI_FILE_DIRECTORY) != 0) {
      Status = EFI_COMPROMISED_DATA;
    } else {
      Info->FileSize = FileSize;
      Status = File->SetInfo(File, &gEfiFileInfoGuid, InfoSize, Info);
      if (Status == EFI_SUCCESS) Status = BlsFileFlush(File);
    }
  }

  if (Info != NULL) FreePool(Info);
  return Status;
}

static EFI_STATUS BlsLogSnapshotFile(
  IN EFI_FILE_PROTOCOL *File,
  IN UINT64 FileSize,
  OUT CHAR8 **Snapshot
  ) {
  CHAR8 *Buffer;
  EFI_STATUS Status;

  if (Snapshot == NULL) return EFI_INVALID_PARAMETER;
  *Snapshot = NULL;
  if (File == NULL) return EFI_INVALID_PARAMETER;
  if (FileSize == 0U) return EFI_SUCCESS;
  if (FileSize > (UINT64)BLS_LOG_FILE_MAX_BYTES || FileSize > (UINT64)MAX_UINTN) {
    return EFI_BAD_BUFFER_SIZE;
  }
  if (File->SetPosition == NULL) return EFI_UNSUPPORTED;

  Buffer = AllocatePool((UINTN)FileSize);
  if (Buffer == NULL) return EFI_OUT_OF_RESOURCES;
  Status = File->SetPosition(File, 0U);
  if (Status == EFI_SUCCESS) {
    Status = BlsFileReadExact(File, Buffer, (UINTN)FileSize);
  }
  if (Status != EFI_SUCCESS) {
    FreePool(Buffer);
    return Status;
  }

  *Snapshot = Buffer;
  return EFI_SUCCESS;
}

static EFI_STATUS BlsLogRestoreSnapshot(
  IN EFI_FILE_PROTOCOL *File,
  IN CONST CHAR8 *Snapshot,
  IN UINTN SnapshotSize
  ) {
  EFI_STATUS Status;

  if (File == NULL || (Snapshot == NULL && SnapshotSize != 0U)) return EFI_INVALID_PARAMETER;
  if (File->SetPosition == NULL) return EFI_UNSUPPORTED;

  Status = BlsLogRestoreFileSize(File, 0U);
  if (Status != EFI_SUCCESS) return Status;
  Status = File->SetPosition(File, 0U);
  if (Status != EFI_SUCCESS) return Status;
  Status = BlsFileWriteAll(File, Snapshot, SnapshotSize);
  if (Status == EFI_SUCCESS) Status = BlsFileFlush(File);
  return Status;
}

static VOID BlsLogClear(IN OUT BLS_LOGGER *Logger) {
  if (Logger == NULL) return;
  Logger->Length = 0;
  Logger->Buffer[0] = '\0';
  Logger->Truncated = FALSE;
}

static EFI_STATUS BlsLogEnsureDirectory(IN EFI_FILE_PROTOCOL *Root, IN CONST CHAR16 *Path) {
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

VOID BlsLogInit(OUT BLS_LOGGER *Logger, IN BOOLEAN Enabled, IN EFI_HANDLE VolumeHandle) {
  if (Logger == NULL) return;
  ZeroMem(Logger, sizeof(*Logger));
  Logger->Enabled = Enabled;
  Logger->VolumeHandle = VolumeHandle;
}

VOID BlsLogSetEnabled(IN OUT BLS_LOGGER *Logger, IN BOOLEAN Enabled, IN EFI_HANDLE VolumeHandle) {
  BOOLEAN VolumeChanged;

  if (Logger == NULL) return;
  VolumeChanged = (BOOLEAN)(Logger->VolumeHandle != VolumeHandle);
  Logger->VolumeHandle = VolumeHandle;
  if (!Enabled) {
    Logger->Enabled = FALSE;
    BlsLogClear(Logger);
    Logger->FileInitialized = FALSE;
    return;
  }
  if (VolumeChanged) Logger->FileInitialized = FALSE;
  Logger->Enabled = TRUE;
}

VOID BlsLog(IN OUT BLS_LOGGER *Logger, IN CONST CHAR8 *Message) {
  UINTN Len;
  UINTN Remaining;
  CONST CHAR8 *TruncatedText;
  UINTN TruncatedLen;

  if (Logger == NULL || !Logger->Enabled || Message == NULL) return;
  /* A single malformed or unexpectedly large diagnostic must not make the
     logger scan beyond the maximum data it could ever retain. */
  Len = BlsAsciiLenBounded(Message, sizeof(Logger->Buffer));
  if (Len == 0) return;
  if (Logger->Length >= sizeof(Logger->Buffer)) {
    Logger->Length = sizeof(Logger->Buffer) - 1;
    Logger->Buffer[Logger->Length] = '\0';
    Logger->Truncated = TRUE;
    return;
  }

  Remaining = sizeof(Logger->Buffer) - Logger->Length;
  if (Remaining < 2 || Len > Remaining - 2) {
    if (!Logger->Truncated) {
      TruncatedText = "[log truncated]";
      TruncatedLen = AsciiStrLen(TruncatedText);
      Remaining = sizeof(Logger->Buffer) - Logger->Length;
      if (Remaining >= 2 && TruncatedLen <= Remaining - 2) {
        CopyMem(Logger->Buffer + Logger->Length, TruncatedText, TruncatedLen);
        Logger->Length += TruncatedLen;
        Logger->Buffer[Logger->Length++] = '\n';
        Logger->Buffer[Logger->Length] = '\0';
      }
      Logger->Truncated = TRUE;
    }
    return;
  }
  CopyMem(Logger->Buffer + Logger->Length, Message, Len);
  Logger->Length += Len;
  Logger->Buffer[Logger->Length++] = '\n';
  Logger->Buffer[Logger->Length] = '\0';
}

EFI_STATUS BlsLogFlush(IN OUT BLS_LOGGER *Logger) {
  EFI_FILE_PROTOCOL *Root;
  EFI_FILE_PROTOCOL *File;
  EFI_STATUS Status;
  EFI_STATUS CloseStatus;
  EFI_STATUS RootCloseStatus;
  EFI_STATUS RollbackStatus;
  UINTN Size;
  UINT64 OriginalSize;
  BOOLEAN DataDurable;
  CHAR8 *RolloverSnapshot;
  UINTN RolloverSnapshotSize;

  if (Logger == NULL || !Logger->Enabled || Logger->VolumeHandle == NULL || Logger->Length == 0) return EFI_SUCCESS;

  RolloverSnapshot = NULL;
  RolloverSnapshotSize = 0U;
  Root = NULL;
  Status = BlsOpenVolumeRoot(Logger->VolumeHandle, &Root);
  if (Status != EFI_SUCCESS) return Status;

  Status = BlsLogEnsureDirectory(Root, L"\\EFI");
  if (Status == EFI_SUCCESS) Status = BlsLogEnsureDirectory(Root, BLS_LOG_DIR);
  if (Status != EFI_SUCCESS) {
    BlsFileCloseBestEffort(&Root);
    return Status;
  }

  /* Open the destination without deleting the previous session first.
     Deleting it here made a transient write/flush failure irreversibly erase
     the last durable diagnostics. A bounded snapshot below protects the
     replacement until the new bytes are durable. */

  File = NULL;
  Status = BlsFileOpen(Root, &File, BLS_LOG_PATH, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
  if (Status != EFI_SUCCESS) {
    BlsFileCloseBestEffort(&File);
    BlsFileCloseBestEffort(&Root);
    return Status;
  }
  if (File == NULL) {
    BlsFileCloseBestEffort(&Root);
    return EFI_DEVICE_ERROR;
  }
  if (File->SetPosition == NULL || (Logger->FileInitialized && File->GetPosition == NULL)) {
    BlsFileCloseBestEffort(&File);
    BlsFileCloseBestEffort(&Root);
    return EFI_UNSUPPORTED;
  }

  Status = File->SetPosition(File, Logger->FileInitialized ? MAX_UINT64 : 0);
  if (Status != EFI_SUCCESS) {
    BlsFileCloseBestEffort(&File);
    BlsFileCloseBestEffort(&Root);
    return Status;
  }

  OriginalSize = 0;
  if (Logger->FileInitialized) {
    Status = File->GetPosition(File, &OriginalSize);
    if (Status != EFI_SUCCESS) {
      BlsFileCloseBestEffort(&File);
      BlsFileCloseBestEffort(&Root);
      return Status;
    }
  } else {
    EFI_FILE_INFO *Info;
    UINTN InfoSize;

    /* The first flush replaces the previous session instead of appending it.
       Inspect the on-disk file before truncating it; a malformed or oversized
       file cannot safely be snapshotted within the configured memory budget. */
    Info = NULL;
    InfoSize = 0U;
    Status = BlsFileGetInfoAlloc(
               File, &gEfiFileInfoGuid, BLS_FILE_INFO_MIN_SIZE,
               (VOID **)&Info, &InfoSize
               );
    if (Status == EFI_SUCCESS) {
      if (Info == NULL || Info->Size < BLS_FILE_INFO_MIN_SIZE ||
          Info->Size > InfoSize || (Info->Attribute & EFI_FILE_DIRECTORY) != 0) {
        Status = EFI_COMPROMISED_DATA;
      } else {
        OriginalSize = Info->FileSize;
        if (OriginalSize > (UINT64)BLS_LOG_FILE_MAX_BYTES) Status = EFI_BAD_BUFFER_SIZE;
      }
    }
    if (Info != NULL) FreePool(Info);
    if (Status != EFI_SUCCESS) {
      BlsFileCloseBestEffort(&File);
      BlsFileCloseBestEffort(&Root);
      return Status;
    }
  }

  Size = Logger->Length;
  if ((UINT64)Size > (UINT64)BLS_LOG_FILE_MAX_BYTES) {
    BlsFileCloseBestEffort(&File);
    BlsFileCloseBestEffort(&Root);
    return EFI_BAD_BUFFER_SIZE;
  }

  /* Keep one bounded session log instead of allowing repeated refresh/return
     cycles to grow BootLoaderSelector.log without limit on the ESP. When the
     next append would exceed the cap, discard the older persisted session
     portion and start the same log file from offset zero with the newest
     buffered diagnostics. This avoids rotation artifacts and bounds I/O. */
  if ((!Logger->FileInitialized && OriginalSize != 0U) ||
      OriginalSize > (UINT64)BLS_LOG_FILE_MAX_BYTES ||
      (UINT64)Size > (UINT64)BLS_LOG_FILE_MAX_BYTES - OriginalSize) {
    /* A first-session replacement or rollover changes previously durable
       bytes. Snapshot a bounded log before truncation and restore it after
       any failure, preserving the old diagnostics until the new flush commits. */
    if (OriginalSize <= (UINT64)BLS_LOG_FILE_MAX_BYTES && OriginalSize > 0U) {
      Status = BlsLogSnapshotFile(File, OriginalSize, &RolloverSnapshot);
      if (Status != EFI_SUCCESS) {
        BlsFileCloseBestEffort(&File);
        BlsFileCloseBestEffort(&Root);
        return Status;
      }
      RolloverSnapshotSize = (UINTN)OriginalSize;
    }

    Status = BlsLogRestoreFileSize(File, 0U);
    if (Status != EFI_SUCCESS) {
      if (RolloverSnapshot != NULL) {
        RollbackStatus = BlsLogRestoreSnapshot(File, RolloverSnapshot, RolloverSnapshotSize);
        if (RollbackStatus != EFI_SUCCESS) Logger->FileInitialized = FALSE;
        FreePool(RolloverSnapshot);
      }
      BlsFileCloseBestEffort(&File);
      BlsFileCloseBestEffort(&Root);
      return Status;
    }
    Status = File->SetPosition(File, 0U);
    if (Status != EFI_SUCCESS) {
      if (RolloverSnapshot != NULL) {
        RollbackStatus = BlsLogRestoreSnapshot(File, RolloverSnapshot, RolloverSnapshotSize);
        if (RollbackStatus != EFI_SUCCESS) Logger->FileInitialized = FALSE;
        FreePool(RolloverSnapshot);
      }
      BlsFileCloseBestEffort(&File);
      BlsFileCloseBestEffort(&Root);
      return Status;
    }
    OriginalSize = 0U;
  }

  Status = BlsFileWriteAll(File, Logger->Buffer, Size);
  if (Status == EFI_SUCCESS) Status = BlsFileFlush(File);
  DataDurable = (BOOLEAN)(Status == EFI_SUCCESS);

  if (!DataDurable) {
    /* A failed append may already have changed the file. Roll it back before a
       retry so the same buffered lines cannot be duplicated. During rollover,
       restore the bounded snapshot captured before truncation. */
    if (RolloverSnapshot != NULL) {
      RollbackStatus = BlsLogRestoreSnapshot(File, RolloverSnapshot, RolloverSnapshotSize);
    } else {
      RollbackStatus = BlsLogRestoreFileSize(File, OriginalSize);
    }
    if (RollbackStatus != EFI_SUCCESS) Logger->FileInitialized = FALSE;
  }

  if (RolloverSnapshot != NULL) FreePool(RolloverSnapshot);
  CloseStatus = BlsFileClose(&File);
  RootCloseStatus = BlsFileClose(&Root);

  if (DataDurable) {
    /* Write + Flush is the data durability boundary. Do not replay the same
       buffer merely because a later lifecycle close reported an error. */
    Logger->FileInitialized = TRUE;
    BlsLogClear(Logger);
  }

  if (Status == EFI_SUCCESS && CloseStatus != EFI_SUCCESS) Status = CloseStatus;
  if (Status == EFI_SUCCESS && RootCloseStatus != EFI_SUCCESS) Status = RootCloseStatus;
  return Status;
}

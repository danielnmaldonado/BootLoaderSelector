/* Credits: ©DNMTechLabs / dnmtechlabs@gmail.com */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef NULL
#undef NULL
#endif
#include <Uefi.h>
#include <Guid/FileInfo.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include "../../BootLoaderSelectorPkg/BootLoaderSelector/Logger.h"

EFI_HANDLE gImageHandle = NULL;
EFI_SYSTEM_TABLE *gST = NULL;
EFI_BOOT_SERVICES *gBS = NULL;
EFI_GUID gEfiFileInfoGuid = EFI_FILE_INFO_ID;

static EFI_FILE_PROTOCOL gRoot;
static EFI_FILE_PROTOCOL gDir;
static EFI_FILE_PROTOCOL gFile;
static CHAR8 gStorage[BLS_LOG_FILE_MAX_BYTES + BLS_MAX_LOG + 64U];
static UINTN gStorageLen;
static UINT64 gPosition;
static EFI_STATUS gWriteStatus;
static UINTN gWriteFailuresRemaining;
static EFI_STATUS gFlushStatus;
static UINTN gFlushFailuresRemaining;
static EFI_STATUS gSetInfoStatus;
static EFI_STATUS gFileCloseStatus;
static UINTN gRollbackCalls;
static UINTN gWriteCalls;

VOID *EFIAPI ZeroMem(OUT VOID *Buffer, IN UINTN Length) { return memset(Buffer, 0, (size_t)Length); }
VOID *EFIAPI CopyMem(OUT VOID *DestinationBuffer, IN CONST VOID *SourceBuffer, IN UINTN Length) {
  return memcpy(DestinationBuffer, SourceBuffer, (size_t)Length);
}
VOID *EFIAPI AllocatePool(IN UINTN AllocationSize) { return malloc((size_t)AllocationSize); }
VOID EFIAPI FreePool(IN JCONST VOID *Buffer) { free((void *)Buffer); }
UINTN EFIAPI AsciiStrLen(IN CONST CHAR8 *String) { return (UINTN)strlen(String); }
UINTN BlsAsciiLenBounded(IN CONST CHAR8 *Str, IN UINTN MaxLen) {
  UINTN i;
  if (Str == NULL) return 0;
  for (i = 0; i < MaxLen && Str[i] != '\0'; i++) {}
  return i;
}

static BOOLEAN PathEq(IN CONST CHAR16 *A, IN CONST CHAR16 *B) {
  UINTN i = 0;
  if (A == NULL || B == NULL) return FALSE;
  while (A[i] != L'\0' && B[i] != L'\0') {
    if (A[i] != B[i]) return FALSE;
    i++;
  }
  return (BOOLEAN)(A[i] == B[i]);
}

static EFI_STATUS EFIAPI TestSetPosition(IN EFI_FILE_PROTOCOL *This, IN UINT64 Position) {
  (void)This;
  gPosition = (Position == MAX_UINT64) ? (UINT64)gStorageLen : Position;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI TestGetPosition(IN EFI_FILE_PROTOCOL *This, OUT UINT64 *Position) {
  (void)This;
  if (Position == NULL) return EFI_INVALID_PARAMETER;
  *Position = gPosition;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI TestSetInfo(
  IN EFI_FILE_PROTOCOL *This,
  IN EFI_GUID *InformationType,
  IN UINTN BufferSize,
  IN VOID *Buffer
  ) {
  EFI_FILE_INFO *Info;
  (void)This; (void)InformationType; (void)BufferSize;
  gRollbackCalls++;
  if (gSetInfoStatus != EFI_SUCCESS) return gSetInfoStatus;
  Info = (EFI_FILE_INFO *)Buffer;
  assert(Info != NULL);
  assert(Info->FileSize <= sizeof(gStorage));
  gStorageLen = (UINTN)Info->FileSize;
  return EFI_SUCCESS;
}

EFI_STATUS BlsOpenVolumeRoot(IN EFI_HANDLE VolumeHandle, OUT EFI_FILE_PROTOCOL **Root) {
  if (VolumeHandle == NULL || Root == NULL) return EFI_INVALID_PARAMETER;
  *Root = &gRoot;
  return EFI_SUCCESS;
}

EFI_STATUS BlsFileOpen(
  IN EFI_FILE_PROTOCOL *Parent,
  OUT EFI_FILE_PROTOCOL **File,
  IN CONST CHAR16 *Path,
  IN UINT64 OpenMode,
  IN UINT64 Attributes
  ) {
  (void)Parent;
  if (File == NULL || Path == NULL) return EFI_INVALID_PARAMETER;
  *File = NULL;
  if ((Attributes & EFI_FILE_DIRECTORY) != 0) {
    *File = &gDir;
    return EFI_SUCCESS;
  }
  if (PathEq(Path, L"\\EFI\\BOOT\\BootLoaderSelector.log")) {
    if ((OpenMode & EFI_FILE_MODE_CREATE) == 0 && gStorageLen == 0) return EFI_NOT_FOUND;
    *File = &gFile;
    return EFI_SUCCESS;
  }
  return EFI_NOT_FOUND;
}

EFI_STATUS BlsFileGetInfoAlloc(
  IN EFI_FILE_PROTOCOL *File,
  IN CONST EFI_GUID *InformationType,
  IN UINTN MinimumSize,
  OUT VOID **Information,
  OUT UINTN *InformationSize
  ) {
  EFI_FILE_INFO *Info;
  UINTN Size;
  (void)File; (void)InformationType;
  if (Information == NULL || InformationSize == NULL) return EFI_INVALID_PARAMETER;
  Size = sizeof(EFI_FILE_INFO);
  assert(Size >= MinimumSize);
  Info = calloc(1, (size_t)Size);
  assert(Info != NULL);
  Info->Size = Size;
  Info->FileSize = gStorageLen;
  Info->PhysicalSize = gStorageLen;
  *Information = Info;
  *InformationSize = Size;
  return EFI_SUCCESS;
}

EFI_STATUS BlsFileReadExact(IN EFI_FILE_PROTOCOL *File, OUT VOID *Buffer, IN UINTN BufferSize) {
  (void)File;
  if (Buffer == NULL && BufferSize != 0U) return EFI_INVALID_PARAMETER;
  if (gPosition > (UINT64)gStorageLen || BufferSize > gStorageLen - (UINTN)gPosition) return EFI_END_OF_FILE;
  memcpy(Buffer, gStorage + (UINTN)gPosition, (size_t)BufferSize);
  gPosition += BufferSize;
  return EFI_SUCCESS;
}

EFI_STATUS BlsFileWriteAll(IN EFI_FILE_PROTOCOL *File, IN CONST VOID *Buffer, IN UINTN BufferSize) {
  (void)File;
  gWriteCalls++;
  if (gWriteFailuresRemaining > 0U) {
    gWriteFailuresRemaining--;
    return EFI_DEVICE_ERROR;
  }
  if (gWriteStatus != EFI_SUCCESS) return gWriteStatus;
  if (gPosition > sizeof(gStorage) || BufferSize > sizeof(gStorage) - (UINTN)gPosition) return EFI_VOLUME_FULL;
  memcpy(gStorage + (UINTN)gPosition, Buffer, (size_t)BufferSize);
  gPosition += BufferSize;
  if (gStorageLen < (UINTN)gPosition) gStorageLen = (UINTN)gPosition;
  return EFI_SUCCESS;
}

EFI_STATUS BlsFileFlush(IN EFI_FILE_PROTOCOL *File) {
  (void)File;
  if (gFlushFailuresRemaining > 0U) {
    gFlushFailuresRemaining--;
    return EFI_DEVICE_ERROR;
  }
  return gFlushStatus;
}
EFI_STATUS BlsFileDelete(IN OUT EFI_FILE_PROTOCOL **File) {
  if (File == NULL) return EFI_INVALID_PARAMETER;
  *File = NULL;
  gStorageLen = 0;
  gPosition = 0;
  return EFI_SUCCESS;
}
EFI_STATUS BlsFileClose(IN OUT EFI_FILE_PROTOCOL **File) {
  EFI_FILE_PROTOCOL *Value;
  if (File == NULL || *File == NULL) return EFI_SUCCESS;
  Value = *File;
  *File = NULL;
  if (Value == &gFile) return gFileCloseStatus;
  return EFI_SUCCESS;
}
VOID BlsFileCloseBestEffort(IN OUT EFI_FILE_PROTOCOL **File) { if (File != NULL) *File = NULL; }

#include "../../BootLoaderSelectorPkg/BootLoaderSelector/Logger.c"

static VOID ResetIo(VOID) {
  memset(&gRoot, 0, sizeof(gRoot));
  memset(&gDir, 0, sizeof(gDir));
  memset(&gFile, 0, sizeof(gFile));
  memset(gStorage, 0, sizeof(gStorage));
  gFile.SetPosition = TestSetPosition;
  gFile.GetPosition = TestGetPosition;
  gFile.SetInfo = TestSetInfo;
  gStorageLen = 0;
  gPosition = 0;
  gWriteStatus = EFI_SUCCESS;
  gWriteFailuresRemaining = 0U;
  gFlushStatus = EFI_SUCCESS;
  gFlushFailuresRemaining = 0U;
  gSetInfoStatus = EFI_SUCCESS;
  gFileCloseStatus = EFI_SUCCESS;
  gRollbackCalls = 0;
  gWriteCalls = 0;
}

int main(void) {
  BLS_LOGGER logger;
  CHAR8 huge[BLS_MAX_LOG + 64U];
  EFI_STATUS status;
  UINTN durableLen;

  ResetIo();
  BlsLogInit(&logger, TRUE, (EFI_HANDLE)(UINTN)1U);
  BlsLog(&logger, "first");
  status = BlsLogFlush(&logger);
  assert(status == EFI_SUCCESS);
  assert(logger.FileInitialized == TRUE);
  assert(logger.Length == 0U);
  assert(gStorageLen == 6U);
  assert(memcmp(gStorage, "first\n", 6U) == 0);

  BlsLog(&logger, "second");
  durableLen = gStorageLen;
  gWriteStatus = EFI_DEVICE_ERROR;
  status = BlsLogFlush(&logger);
  assert(status == EFI_DEVICE_ERROR);
  assert(logger.Length != 0U);
  assert(gRollbackCalls == 1U);
  assert(gStorageLen == durableLen);
  assert(logger.FileInitialized == TRUE);

  gWriteStatus = EFI_SUCCESS;
  gFlushStatus = EFI_DEVICE_ERROR;
  gSetInfoStatus = EFI_DEVICE_ERROR;
  status = BlsLogFlush(&logger);
  assert(status == EFI_DEVICE_ERROR);
  assert(logger.FileInitialized == FALSE);
  assert(logger.Length != 0U);

  gFlushStatus = EFI_SUCCESS;
  gSetInfoStatus = EFI_SUCCESS;
  gStorageLen = 0U;
  status = BlsLogFlush(&logger);
  assert(status == EFI_SUCCESS);
  assert(logger.Length == 0U);
  assert(logger.FileInitialized == TRUE);

  BlsLog(&logger, "close-error-is-durable");
  gFileCloseStatus = EFI_DEVICE_ERROR;
  status = BlsLogFlush(&logger);
  assert(status == EFI_DEVICE_ERROR);
  assert(logger.Length == 0U);
  assert(logger.FileInitialized == TRUE);

  BlsLogSetEnabled(&logger, FALSE, NULL);
  assert(logger.Enabled == FALSE);
  assert(logger.Length == 0U);
  assert(logger.FileInitialized == FALSE);

  BlsLogSetEnabled(&logger, TRUE, (EFI_HANDLE)(UINTN)2U);
  memset(huge, 'A', sizeof(huge));
  huge[sizeof(huge) - 1U] = '\0';
  BlsLog(&logger, huge);
  assert(logger.Truncated == TRUE);
  assert(logger.Length < BLS_MAX_LOG);

  /* Repeated debug flushes remain in one bounded on-disk log. The newest
     buffered diagnostics replace the older persisted prefix once the cap
     would be exceeded. */
  ResetIo();
  BlsLogInit(&logger, TRUE, (EFI_HANDLE)(UINTN)3U);
  BlsLog(&logger, "seed");
  assert(BlsLogFlush(&logger) == EFI_SUCCESS);
  assert(logger.FileInitialized == TRUE);
  memset(gStorage, 'O', BLS_LOG_FILE_MAX_BYTES - 2U);
  gStorageLen = BLS_LOG_FILE_MAX_BYTES - 2U;
  gPosition = gStorageLen;
  BlsLog(&logger, "latest");
  assert(BlsLogFlush(&logger) == EFI_SUCCESS);
  assert(gRollbackCalls == 1U);
  assert(gStorageLen == 7U);
  assert(memcmp(gStorage, "latest\n", 7U) == 0);
  assert(logger.Length == 0U);
  assert(logger.FileInitialized == TRUE);

  /* A failed rollover must preserve the previously durable log. The v2.09
     implementation truncated the old file before attempting the replacement
     write, then rolled back to length zero after a write/flush failure. */
  ResetIo();
  BlsLogInit(&logger, TRUE, (EFI_HANDLE)(UINTN)4U);
  logger.FileInitialized = TRUE;
  memset(gStorage, 'P', BLS_LOG_FILE_MAX_BYTES - 2U);
  gStorageLen = BLS_LOG_FILE_MAX_BYTES - 2U;
  gPosition = gStorageLen;
  BlsLog(&logger, "new-after-rollover");
  gWriteFailuresRemaining = 1U;
  status = BlsLogFlush(&logger);
  assert(status == EFI_DEVICE_ERROR);
  assert(gStorageLen == BLS_LOG_FILE_MAX_BYTES - 2U);
  for (UINTN i = 0; i < gStorageLen; i++) assert(gStorage[i] == 'P');
  assert(logger.Length != 0U);
  assert(logger.FileInitialized == TRUE);

  /* SetInfo may have changed FileSize before a transient Flush failure is
     reported. That failure must also restore the pre-rollover durable bytes. */
  ResetIo();
  BlsLogInit(&logger, TRUE, (EFI_HANDLE)(UINTN)5U);
  logger.FileInitialized = TRUE;
  memset(gStorage, 'Q', BLS_LOG_FILE_MAX_BYTES - 2U);
  gStorageLen = BLS_LOG_FILE_MAX_BYTES - 2U;
  gPosition = gStorageLen;
  BlsLog(&logger, "new-after-truncate-flush");
  gFlushFailuresRemaining = 1U;
  status = BlsLogFlush(&logger);
  assert(status == EFI_DEVICE_ERROR);
  assert(gStorageLen == BLS_LOG_FILE_MAX_BYTES - 2U);
  for (UINTN i = 0; i < gStorageLen; i++) assert(gStorage[i] == 'Q');
  assert(logger.Length != 0U);
  assert(logger.FileInitialized == TRUE);

  /* A new session must not destroy the previous durable log before its
     replacement is successfully persisted. The old implementation called
     Delete() before the first write, permanently losing this diagnostic on
     write/flush failure. */
  ResetIo();
  BlsLogInit(&logger, TRUE, (EFI_HANDLE)(UINTN)6U);
  memcpy(gStorage, "previous\n", 9U);
  gStorageLen = 9U;
  BlsLog(&logger, "new-session");
  gWriteFailuresRemaining = 1U;
  status = BlsLogFlush(&logger);
  assert(status == EFI_DEVICE_ERROR);
  assert(gStorageLen == 9U);
  assert(memcmp(gStorage, "previous\n", 9U) == 0);
  assert(logger.Length != 0U);
  assert(logger.FileInitialized == FALSE);
  gWriteFailuresRemaining = 0U;
  assert(BlsLogFlush(&logger) == EFI_SUCCESS);
  assert(gStorageLen == sizeof("new-session\n") - 1U);
  assert(memcmp(gStorage, "new-session\n", gStorageLen) == 0);

  /* The same initial replacement must roll back after a flush failure. */
  ResetIo();
  BlsLogInit(&logger, TRUE, (EFI_HANDLE)(UINTN)7U);
  memcpy(gStorage, "old-flush\n", 10U);
  gStorageLen = 10U;
  BlsLog(&logger, "new-flush");
  gFlushFailuresRemaining = 1U;
  status = BlsLogFlush(&logger);
  assert(status == EFI_DEVICE_ERROR);
  assert(gStorageLen == 10U);
  assert(memcmp(gStorage, "old-flush\n", 10U) == 0);
  assert(logger.Length != 0U);

  /* A pre-existing oversized log cannot be snapshotted safely. Fail closed
     without deleting or truncating it, and keep the new diagnostics buffered. */
  ResetIo();
  BlsLogInit(&logger, TRUE, (EFI_HANDLE)(UINTN)8U);
  gStorageLen = BLS_LOG_FILE_MAX_BYTES + 1U;
  memcpy(gStorage, "oversized", 9U);
  BlsLog(&logger, "new-after-oversize");
  status = BlsLogFlush(&logger);
  assert(status == EFI_BAD_BUFFER_SIZE);
  assert(gStorageLen == BLS_LOG_FILE_MAX_BYTES + 1U);
  assert(memcmp(gStorage, "oversized", 9U) == 0);
  assert(logger.Length != 0U);
  assert(logger.FileInitialized == FALSE);

  /* Failures to resize/truncate must not destroy the existing session log. */
  ResetIo();
  BlsLogInit(&logger, TRUE, (EFI_HANDLE)(UINTN)9U);
  memcpy(gStorage, "old-resize\n", 11U);
  gStorageLen = 11U;
  BlsLog(&logger, "new-resize");
  gSetInfoStatus = EFI_DEVICE_ERROR;
  status = BlsLogFlush(&logger);
  assert(status == EFI_DEVICE_ERROR);
  assert(gStorageLen == 11U);
  assert(memcmp(gStorage, "old-resize\n", 11U) == 0);
  assert(logger.Length != 0U);

  puts("logger_lifecycle: PASS");
  return 0;
}

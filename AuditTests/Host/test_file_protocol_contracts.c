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
#include <Protocol/SimpleFileSystem.h>

EFI_HANDLE gImageHandle = NULL;
EFI_SYSTEM_TABLE *gST = NULL;
EFI_BOOT_SERVICES *gBS = NULL;
EFI_GUID gEfiSimpleFileSystemProtocolGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
EFI_GUID gEfiFileInfoGuid = EFI_FILE_INFO_ID;

VOID *EFIAPI AllocateZeroPool(IN UINTN AllocationSize) { return calloc(1, (size_t)AllocationSize); }
VOID EFIAPI FreePool(IN JCONST VOID *Buffer) { free((void *)Buffer); }
VOID *EFIAPI CopyMem(OUT VOID *DestinationBuffer, IN CONST VOID *SourceBuffer, IN UINTN Length) {
  return memcpy(DestinationBuffer, SourceBuffer, (size_t)Length);
}
VOID *EFIAPI ZeroMem(OUT VOID *Buffer, IN UINTN Length) { return memset(Buffer, 0, (size_t)Length); }
UINTN EFIAPI AsciiStrLen(IN CONST CHAR8 *String) { return (UINTN)strlen(String); }

static UINTN gReadCalls;
static UINTN gWriteCalls;
static UINTN gRequestedSize;
static UINTN gReturnedSize;
static EFI_STATUS gIoStatus;

static EFI_STATUS EFIAPI TestRead(
  IN EFI_FILE_PROTOCOL *This,
  IN OUT UINTN *BufferSize,
  OUT VOID *Buffer
  ) {
  (void)This;
  gReadCalls++;
  assert(BufferSize != NULL);
  assert(*BufferSize == gRequestedSize);
  if (Buffer != NULL && gReturnedSize <= *BufferSize) memset(Buffer, 0x5a, (size_t)gReturnedSize);
  *BufferSize = gReturnedSize;
  return gIoStatus;
}

static EFI_STATUS EFIAPI TestWrite(
  IN EFI_FILE_PROTOCOL *This,
  IN OUT UINTN *BufferSize,
  IN VOID *Buffer
  ) {
  (void)This;
  (void)Buffer;
  gWriteCalls++;
  assert(BufferSize != NULL);
  assert(*BufferSize == gRequestedSize);
  *BufferSize = gReturnedSize;
  return gIoStatus;
}

#include "../../BootLoaderSelectorPkg/BootLoaderSelector/Utils.c"

int main(void) {
  EFI_FILE_PROTOCOL File;
  EFI_STATUS Status;
  UINT8 Buffer[8];

  memset(&File, 0, sizeof(File));
  File.Read = TestRead;
  File.Write = TestWrite;

  /* Successful exact read. */
  gReadCalls = 0U;
  gRequestedSize = sizeof(Buffer);
  gReturnedSize = sizeof(Buffer);
  gIoStatus = EFI_SUCCESS;
  Status = BlsFileReadExact(&File, Buffer, sizeof(Buffer));
  assert(Status == EFI_SUCCESS);
  assert(gReadCalls == 1U);

  /* A successful short read means the request reached EOF. An exact-read
     helper must report that immediately instead of issuing another firmware
     call. */
  gReadCalls = 0U;
  gRequestedSize = sizeof(Buffer);
  gReturnedSize = 4U;
  gIoStatus = EFI_SUCCESS;
  Status = BlsFileReadExact(&File, Buffer, sizeof(Buffer));
  assert(Status == EFI_END_OF_FILE);
  assert(gReadCalls == 1U);

  /* Firmware must never claim it returned more bytes than requested. */
  gReadCalls = 0U;
  gRequestedSize = sizeof(Buffer);
  gReturnedSize = sizeof(Buffer) + 1U;
  gIoStatus = EFI_SUCCESS;
  Status = BlsFileReadExact(&File, Buffer, sizeof(Buffer));
  assert(Status == EFI_COMPROMISED_DATA);
  assert(gReadCalls == 1U);

  /* Successful write must report the complete requested transfer. */
  gWriteCalls = 0U;
  gRequestedSize = sizeof(Buffer);
  gReturnedSize = sizeof(Buffer);
  gIoStatus = EFI_SUCCESS;
  Status = BlsFileWriteAll(&File, Buffer, sizeof(Buffer));
  assert(Status == EFI_SUCCESS);
  assert(gWriteCalls == 1U);

  /* UEFI permits a partial write only together with an error. A short write
     paired with EFI_SUCCESS is a protocol violation and must not be retried. */
  gWriteCalls = 0U;
  gRequestedSize = sizeof(Buffer);
  gReturnedSize = sizeof(Buffer) - 1U;
  gIoStatus = EFI_SUCCESS;
  Status = BlsFileWriteAll(&File, Buffer, sizeof(Buffer));
  assert(Status == EFI_COMPROMISED_DATA);
  assert(gWriteCalls == 1U);

  /* Likewise for an impossible oversized success result. */
  gWriteCalls = 0U;
  gRequestedSize = sizeof(Buffer);
  gReturnedSize = sizeof(Buffer) + 1U;
  gIoStatus = EFI_SUCCESS;
  Status = BlsFileWriteAll(&File, Buffer, sizeof(Buffer));
  assert(Status == EFI_COMPROMISED_DATA);
  assert(gWriteCalls == 1U);

  /* A real write error is propagated verbatim even if BufferSize reports a
     partial transfer, matching the protocol contract. */
  gWriteCalls = 0U;
  gRequestedSize = sizeof(Buffer);
  gReturnedSize = 3U;
  gIoStatus = EFI_VOLUME_FULL;
  Status = BlsFileWriteAll(&File, Buffer, sizeof(Buffer));
  assert(Status == EFI_VOLUME_FULL);
  assert(gWriteCalls == 1U);

  puts("file_protocol_contracts: PASS");
  return 0;
}

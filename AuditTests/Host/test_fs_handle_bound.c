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
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/SimpleFileSystem.h>

EFI_HANDLE gImageHandle = NULL;
EFI_SYSTEM_TABLE *gST = NULL;
EFI_BOOT_SERVICES *gBS = NULL;
EFI_GUID gEfiSimpleFileSystemProtocolGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
EFI_GUID gEfiFileInfoGuid = EFI_FILE_INFO_ID;

static UINTN gLocateCount;
static UINTN gLocateCalls;
static UINTN gAllocateCount;
static UINTN gFreeCount;
static EFI_STATUS gLocateStatus;
static BOOLEAN gGrowOnce;
static BOOLEAN gMisalignedSize;
static BOOLEAN gStallTooSmall;

VOID *EFIAPI AllocateZeroPool(IN UINTN Size) {
  VOID *buffer;
  gAllocateCount++;
  buffer = calloc(1U, (size_t)Size);
  return buffer;
}

VOID *EFIAPI AllocatePool(IN UINTN Size) {
  VOID *buffer;
  gAllocateCount++;
  buffer = malloc((size_t)Size);
  return buffer;
}

VOID EFIAPI FreePool(IN JCONST VOID *Buffer) {
  gFreeCount++;
  free((VOID *)Buffer);
}

UINTN EFIAPI AsciiStrLen(IN CONST CHAR8 *String) {
  return String ? (UINTN)strlen(String) : 0U;
}

static EFI_STATUS EFIAPI TestLocateHandle(
  IN EFI_LOCATE_SEARCH_TYPE SearchType,
  IN EFI_GUID *Protocol OPTIONAL,
  IN VOID *SearchKey OPTIONAL,
  IN OUT UINTN *BufferSize,
  OUT EFI_HANDLE *Buffer
  ) {
  UINTN requiredCount;
  UINTN requiredBytes;
  UINTN index;

  (void)SearchKey;
  gLocateCalls++;
  assert(SearchType == ByProtocol);
  assert(Protocol == &gEfiSimpleFileSystemProtocolGuid);
  assert(BufferSize != NULL);

  if (gLocateStatus != EFI_SUCCESS) {
    *BufferSize = 0xBADU;
    return gLocateStatus;
  }

  if (gMisalignedSize) {
    *BufferSize = sizeof(EFI_HANDLE) + 1U;
    return EFI_BUFFER_TOO_SMALL;
  }

  requiredCount = gLocateCount;
  if (gGrowOnce && gLocateCalls >= 2U) requiredCount++;
  requiredBytes = requiredCount * sizeof(EFI_HANDLE);
  if (requiredCount == 0U) return EFI_NOT_FOUND;

  if (gStallTooSmall && Buffer != NULL) {
    *BufferSize = requiredBytes;
    return EFI_BUFFER_TOO_SMALL;
  }

  if (Buffer == NULL || *BufferSize < requiredBytes) {
    *BufferSize = requiredBytes;
    return EFI_BUFFER_TOO_SMALL;
  }

  for (index = 0U; index < requiredCount; index++) {
    Buffer[index] = (EFI_HANDLE)(UINTN)(index + 1U);
  }
  *BufferSize = requiredBytes;
  return EFI_SUCCESS;
}

#include "../../BootLoaderSelectorPkg/BootLoaderSelector/Utils.c"
#include "../../BootLoaderSelectorPkg/BootLoaderSelector/BootScan.c"

static VOID ResetCounters(VOID) {
  gLocateCalls = 0U;
  gAllocateCount = 0U;
  gFreeCount = 0U;
  gGrowOnce = FALSE;
  gMisalignedSize = FALSE;
  gStallTooSmall = FALSE;
}

int main(void) {
  EFI_BOOT_SERVICES bootServices;
  EFI_HANDLE *handles;
  UINTN count;
  EFI_STATUS status;

  memset(&bootServices, 0, sizeof(bootServices));
  bootServices.LocateHandle = TestLocateHandle;
  gBS = &bootServices;

  /* Oversized firmware metadata is rejected before any allocation. */
  gLocateStatus = EFI_SUCCESS;
  gLocateCount = BLS_FS_HANDLE_SCAN_MAX + 1U;
  ResetCounters();
  handles = (EFI_HANDLE *)(UINTN)1;
  count = 99U;
  status = BlsLocateFsHandles(&handles, &count);
  assert(status == EFI_COMPROMISED_DATA);
  assert(handles == NULL && count == 0U);
  assert(gLocateCalls == 1U);
  assert(gAllocateCount == 0U && gFreeCount == 0U);

  /* Handle buffers are arrays: a byte count not aligned to EFI_HANDLE is
     malformed firmware metadata and must be rejected before allocation. */
  gLocateStatus = EFI_SUCCESS;
  gLocateCount = 1U;
  ResetCounters();
  gMisalignedSize = TRUE;
  handles = NULL;
  count = 0U;
  status = BlsLocateFsHandles(&handles, &count);
  assert(status == EFI_COMPROMISED_DATA);
  assert(handles == NULL && count == 0U);
  assert(gLocateCalls == 1U && gAllocateCount == 0U && gFreeCount == 0U);

  /* A failing size query transfers no ownership and leaves outputs normalized. */
  gLocateStatus = EFI_DEVICE_ERROR;
  gLocateCount = 2U;
  ResetCounters();
  handles = (EFI_HANDLE *)(UINTN)1;
  count = 99U;
  status = BlsLocateFsHandles(&handles, &count);
  assert(status == EFI_DEVICE_ERROR);
  assert(handles == NULL && count == 0U);
  assert(gAllocateCount == 0U && gFreeCount == 0U);

  /* Stable protocol database: one bounded allocation, then success. */
  gLocateStatus = EFI_SUCCESS;
  gLocateCount = 2U;
  ResetCounters();
  handles = NULL;
  count = 0U;
  status = BlsLocateFsHandles(&handles, &count);
  assert(status == EFI_SUCCESS);
  assert(handles != NULL && count == 2U);
  assert(gLocateCalls == 2U && gAllocateCount == 1U && gFreeCount == 0U);
  FreePool(handles);
  assert(gFreeCount == 1U);

  /* BUFFER_TOO_SMALL without a larger requirement cannot make progress.
     Reject it instead of looping on a broken firmware implementation. */
  gLocateStatus = EFI_SUCCESS;
  gLocateCount = 2U;
  ResetCounters();
  gStallTooSmall = TRUE;
  handles = NULL;
  count = 0U;
  status = BlsLocateFsHandles(&handles, &count);
  assert(status == EFI_COMPROMISED_DATA);
  assert(handles == NULL && count == 0U);
  assert(gLocateCalls == 2U && gAllocateCount == 1U && gFreeCount == 1U);

  /* A protocol installation racing the query may grow the requirement once.
     Retry is bounded and the obsolete smaller buffer is released exactly once. */
  gLocateStatus = EFI_SUCCESS;
  gLocateCount = 1U;
  ResetCounters();
  gGrowOnce = TRUE;
  handles = NULL;
  count = 0U;
  status = BlsLocateFsHandles(&handles, &count);
  assert(status == EFI_SUCCESS);
  assert(handles != NULL && count == 2U);
  assert(gLocateCalls == 3U && gAllocateCount == 2U && gFreeCount == 1U);
  FreePool(handles);
  assert(gFreeCount == 2U);

  puts("fs_handle_bound: PASS");
  return 0;
}

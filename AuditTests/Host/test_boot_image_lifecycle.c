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
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/DevicePath.h>

#include "../../BootLoaderSelectorPkg/BootLoaderSelector/BootScan.h"
#include "../../BootLoaderSelectorPkg/BootLoaderSelector/Utils.h"

EFI_HANDLE gImageHandle = NULL;
EFI_SYSTEM_TABLE *gST = NULL;
EFI_BOOT_SERVICES *gBS = NULL;
EFI_GUID gEfiDevicePathProtocolGuid = EFI_DEVICE_PATH_PROTOCOL_GUID;

static EFI_STATUS gLoadStatus;
static EFI_STATUS gStartStatus;
static EFI_STATUS gUnloadStatus;
static BOOLEAN gLoadReturnsHandle;
static UINTN gLoadCalls;
static UINTN gFileDevicePathCalls;
static UINTN gStartCalls;
static UINTN gProtocolQueryCalls;
static BOOLEAN gProtocolAvailable;
static UINTN gUnloadCalls;
static UINTN gFreeCalls;
static BOOLEAN gReturnExitData;
static BOOLEAN gReturnOversizedExitData;
static BOOLEAN gScribbleExitDataOnSuccess;
static BOOLEAN gScribbleExitDataOnFailure;
static BOOLEAN gFreedNonOwnedExitData;
static UINTN gWatchdogCalls;
static UINTN gWatchdogArmCalls;
static UINTN gWatchdogDisableCalls;
static EFI_STATUS gWatchdogArmStatus;
static EFI_STATUS gWatchdogDisableStatus;
static UINTN gSequence;
static UINTN gLoadOrder;
static BOOLEAN gLastBootPolicy;
static UINTN gWatchdogArmOrder;
static UINTN gStartOrder;
static UINTN gWatchdogDisableOrder;
#define NON_OWNED_EXIT_DATA ((CHAR16 *)(UINTN)0xDEADBEEFU)

VOID *EFIAPI CopyMem(OUT VOID *Destination, IN CONST VOID *Source, IN UINTN Length) {
  return memcpy(Destination, Source, (size_t)Length);
}

VOID EFIAPI FreePool(IN JCONST VOID *Buffer) {
  if (Buffer != NULL) {
    gFreeCalls++;
    if (Buffer == NON_OWNED_EXIT_DATA) {
      gFreedNonOwnedExitData = TRUE;
      return;
    }
    free((void *)Buffer);
  }
}

EFI_DEVICE_PATH_PROTOCOL *EFIAPI FileDevicePath(
  IN EFI_HANDLE Device OPTIONAL,
  IN CONST CHAR16 *FileName
  ) {
  EFI_DEVICE_PATH_PROTOCOL *Path;

  (void)Device;
  (void)FileName;
  gFileDevicePathCalls++;
  Path = calloc(1U, sizeof(*Path));
  return Path;
}

EFI_DEVICE_PATH_PROTOCOL *EFIAPI AppendDevicePath(
  IN CONST EFI_DEVICE_PATH_PROTOCOL *Src1 OPTIONAL,
  IN CONST EFI_DEVICE_PATH_PROTOCOL *Src2 OPTIONAL
  ) {
  if (Src1 == NULL || Src2 == NULL) return NULL;
  return calloc(1U, sizeof(EFI_DEVICE_PATH_PROTOCOL));
}

BOOLEAN EFIAPI IsDevicePathValid(
  IN CONST EFI_DEVICE_PATH_PROTOCOL *DevicePath,
  IN UINTN MaxSize
  ) {
  (void)MaxSize;
  return (BOOLEAN)(DevicePath != NULL);
}

UINTN BlsStrLen16Bounded(IN CONST CHAR16 *Str, IN UINTN MaxChars) {
  UINTN Length;

  if (Str == NULL) return 0U;
  for (Length = 0U; Length < MaxChars; Length++) {
    if (Str[Length] == L'\0') return Length;
  }
  return MaxChars;
}

static EFI_STATUS EFIAPI TestLoadImage(
  IN BOOLEAN BootPolicy,
  IN EFI_HANDLE ParentImageHandle,
  IN EFI_DEVICE_PATH_PROTOCOL *DevicePath,
  IN VOID *SourceBuffer OPTIONAL,
  IN UINTN SourceSize,
  OUT EFI_HANDLE *ImageHandle
  ) {
  gLastBootPolicy = BootPolicy;
  (void)ParentImageHandle;
  (void)DevicePath;
  (void)SourceBuffer;
  (void)SourceSize;
  gLoadCalls++;
  gLoadOrder = ++gSequence;
  if (ImageHandle == NULL) return EFI_INVALID_PARAMETER;
  *ImageHandle = gLoadReturnsHandle ? (EFI_HANDLE)(UINTN)0x2222U : NULL;
  return gLoadStatus;
}

static EFI_STATUS EFIAPI TestStartImage(
  IN EFI_HANDLE ImageHandle,
  OUT UINTN *ExitDataSize,
  OUT CHAR16 **ExitData OPTIONAL
  ) {
  (void)ImageHandle;
  gStartCalls++;
  gStartOrder = ++gSequence;
  if (ExitDataSize != NULL) *ExitDataSize = 0U;
  if (ExitData != NULL) *ExitData = NULL;
  if (((gScribbleExitDataOnSuccess && gStartStatus == EFI_SUCCESS) ||
       (gScribbleExitDataOnFailure && gStartStatus != EFI_SUCCESS)) &&
      ExitDataSize != NULL && ExitData != NULL) {
    *ExitData = NON_OWNED_EXIT_DATA;
    *ExitDataSize = 64U;
  } else if (gReturnExitData && ExitDataSize != NULL && ExitData != NULL) {
    CHAR16 *Text;
    Text = calloc(6U, sizeof(CHAR16));
    assert(Text != NULL);
    Text[0] = L'c'; Text[1] = L'h'; Text[2] = L'i'; Text[3] = L'l'; Text[4] = L'd';
    Text[5] = gReturnOversizedExitData ? L'!' : L'\0';
    *ExitData = Text;
    *ExitDataSize = gReturnOversizedExitData ? 4096U : 6U * sizeof(CHAR16);
  }
  return gStartStatus;
}

EFI_STATUS BlsGetProtocolInterface(
  IN EFI_HANDLE Handle, IN EFI_GUID *Protocol, OUT VOID **Interface
  ) {
  static EFI_DEVICE_PATH_PROTOCOL VolumePath;
  (void)Handle;
  gProtocolQueryCalls++;
  if (Interface == NULL) return EFI_INVALID_PARAMETER;
  *Interface = NULL;
  if (!gProtocolAvailable) return EFI_UNSUPPORTED;
  if (Protocol != &gEfiDevicePathProtocolGuid) return EFI_NOT_FOUND;
  *Interface = &VolumePath;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI TestUnloadImage(IN EFI_HANDLE ImageHandle) {
  (void)ImageHandle;
  gUnloadCalls++;
  return gUnloadStatus;
}

#include "../../BootLoaderSelectorPkg/BootLoaderSelector/BootScan.c"


static EFI_STATUS EFIAPI TestSetWatchdogTimer(
  IN UINTN Timeout,
  IN UINT64 WatchdogCode,
  IN UINTN DataSize,
  IN CHAR16 *WatchdogData OPTIONAL
  ) {
  (void)DataSize;
  (void)WatchdogData;
  gWatchdogCalls++;
  if (Timeout == 0U) {
    assert(WatchdogCode == 0U);
    gWatchdogDisableCalls++;
    gWatchdogDisableOrder = ++gSequence;
    return gWatchdogDisableStatus;
  } else {
    assert(Timeout == BLS_CHILD_WATCHDOG_SECONDS);
    assert(WatchdogCode == BLS_CHILD_WATCHDOG_CODE);
    assert(WatchdogCode > 0xFFFFULL);
    gWatchdogArmCalls++;
    gWatchdogArmOrder = ++gSequence;
    return gWatchdogArmStatus;
  }
}

static VOID ResetState(VOID) {
  gLoadStatus = EFI_SUCCESS;
  gStartStatus = EFI_SUCCESS;
  gUnloadStatus = EFI_SUCCESS;
  gLoadReturnsHandle = TRUE;
  gLoadCalls = 0U;
  gFileDevicePathCalls = 0U;
  gStartCalls = 0U;
  gProtocolQueryCalls = 0U;
  gProtocolAvailable = TRUE;
  gUnloadCalls = 0U;
  gFreeCalls = 0U;
  gReturnExitData = FALSE;
  gReturnOversizedExitData = FALSE;
  gScribbleExitDataOnSuccess = FALSE;
  gScribbleExitDataOnFailure = FALSE;
  gFreedNonOwnedExitData = FALSE;
  gWatchdogCalls = 0U;
  gWatchdogArmCalls = 0U;
  gWatchdogDisableCalls = 0U;
  gWatchdogArmStatus = EFI_SUCCESS;
  gWatchdogDisableStatus = EFI_SUCCESS;
  gSequence = 0U;
  gLoadOrder = 0U;
  gLastBootPolicy = FALSE;
  gWatchdogArmOrder = 0U;
  gStartOrder = 0U;
  gWatchdogDisableOrder = 0U;
}

int main(void) {
  EFI_BOOT_SERVICES BootServices;
  BLS_BOOT_ENTRY Entry;
  EFI_STATUS Status;
  EFI_STATUS StartStatus;

  memset(&BootServices, 0, sizeof(BootServices));
  BootServices.LoadImage = TestLoadImage;
  BootServices.StartImage = TestStartImage;
  BootServices.UnloadImage = TestUnloadImage;
  BootServices.SetWatchdogTimer = TestSetWatchdogTimer;
  gBS = &BootServices;

  memset(&Entry, 0, sizeof(Entry));
  Entry.VolumeHandle = (EFI_HANDLE)(UINTN)0x1111U;
  Entry.Path[0] = L'\\';
  Entry.Path[1] = L'x';
  Entry.Path[2] = L'\0';

  ResetState();
  gProtocolAvailable = FALSE;
  StartStatus = EFI_SUCCESS;
  Status = BlsStartBootEntry((EFI_HANDLE)(UINTN)1U, &Entry, &StartStatus);
  assert(Status == EFI_UNSUPPORTED);
  assert(StartStatus == EFI_NOT_STARTED);
  assert(gFileDevicePathCalls == 0U);
  assert(gLoadCalls == 0U && gStartCalls == 0U && gUnloadCalls == 0U);

  gProtocolAvailable = TRUE;

  /* A malformed/incomplete Boot Services table cannot satisfy the mandatory
     boot-manager watchdog lifecycle. Reject it before allocating/loading the child. */
  ResetState();
  BootServices.SetWatchdogTimer = NULL;
  StartStatus = EFI_SUCCESS;
  Status = BlsStartBootEntry((EFI_HANDLE)(UINTN)1U, &Entry, &StartStatus);
  assert(Status == EFI_UNSUPPORTED);
  assert(StartStatus == EFI_NOT_STARTED);
  assert(gFileDevicePathCalls == 0U);
  assert(gLoadCalls == 0U && gStartCalls == 0U && gUnloadCalls == 0U);
  BootServices.SetWatchdogTimer = TestSetWatchdogTimer;

  ResetState();
  StartStatus = EFI_ABORTED;
  Status = BlsStartBootEntry((EFI_HANDLE)(UINTN)1U, &Entry, &StartStatus);
  assert(Status == EFI_SUCCESS);
  assert(StartStatus == EFI_SUCCESS);
  assert(gLoadCalls == 1U && gStartCalls == 1U);
  assert(gLastBootPolicy == TRUE); /* UEFI boot-manager boot selection */
  assert(gProtocolQueryCalls == 1U);
  assert(gUnloadCalls == 1U); /* resident-success probe/cleanup */
  assert(gFreeCalls == 2U);
  assert(gWatchdogCalls == 2U);
  assert(gWatchdogArmCalls == 1U && gWatchdogDisableCalls == 1U);
  assert(gLoadOrder < gWatchdogArmOrder);
  assert(gWatchdogArmOrder < gStartOrder);
  assert(gStartOrder < gWatchdogDisableOrder);

  /* A successfully started UEFI driver may legally return EFI_SUCCESS and
     remain resident.  A selector launching a bootloader path must not leave
     such an unexpected image installed when control comes back.  Probe the
     loaded-image handle after a successful StartImage return: an application/
     OS-loader has already been removed (EFI_INVALID_PARAMETER), while an
     unloadable driver can be cleaned up explicitly. */
  ResetState();
  gUnloadStatus = EFI_INVALID_PARAMETER; /* normal application already unloaded */
  {
    BOOLEAN LifecycleCompromised;
    LifecycleCompromised = FALSE;
    Status = BlsStartBootEntryEx((EFI_HANDLE)(UINTN)1U, &Entry, &StartStatus,
                                 NULL, 0U, &LifecycleCompromised);
    assert(Status == EFI_SUCCESS);
    assert(StartStatus == EFI_SUCCESS);
    assert(LifecycleCompromised == FALSE);
    assert(gUnloadCalls == 1U);
  }

  ResetState();
  gUnloadStatus = EFI_SUCCESS; /* resident driver with a working Unload() */
  {
    BOOLEAN LifecycleCompromised;
    LifecycleCompromised = FALSE;
    Status = BlsStartBootEntryEx((EFI_HANDLE)(UINTN)1U, &Entry, &StartStatus,
                                 NULL, 0U, &LifecycleCompromised);
    assert(Status == EFI_SUCCESS);
    assert(StartStatus == EFI_SUCCESS);
    assert(LifecycleCompromised == FALSE);
    assert(gUnloadCalls == 1U);
  }

  ResetState();
  gUnloadStatus = EFI_UNSUPPORTED; /* resident driver cannot be unloaded */
  {
    BOOLEAN LifecycleCompromised;
    LifecycleCompromised = FALSE;
    Status = BlsStartBootEntryEx((EFI_HANDLE)(UINTN)1U, &Entry, &StartStatus,
                                 NULL, 0U, &LifecycleCompromised);
    assert(Status == EFI_UNSUPPORTED);
    assert(StartStatus == EFI_SUCCESS);
    assert(LifecycleCompromised == TRUE);
    assert(gUnloadCalls == 1U);
  }

  /* UEFI 2.11 boot-manager contract: a watchdog programming failure must
     prevent StartImage rather than silently starting the child unprotected. */
  ResetState();
  gWatchdogArmStatus = EFI_DEVICE_ERROR;
  StartStatus = EFI_SUCCESS;
  Status = BlsStartBootEntry((EFI_HANDLE)(UINTN)1U, &Entry, &StartStatus);
  assert(Status == EFI_DEVICE_ERROR);
  assert(StartStatus == EFI_NOT_STARTED);
  assert(gWatchdogArmCalls == 1U);
  assert(gStartCalls == 0U);
  assert(gUnloadCalls == 1U);
  assert(gWatchdogDisableCalls == 0U);

  /* EFI_UNSUPPORTED has a precise UEFI meaning: the platform has no watchdog.
     Preserve boot compatibility and still make the required teardown call when
     StartImage returns. */
  ResetState();
  gWatchdogArmStatus = EFI_UNSUPPORTED;
  gWatchdogDisableStatus = EFI_UNSUPPORTED;
  Status = BlsStartBootEntry((EFI_HANDLE)(UINTN)1U, &Entry, &StartStatus);
  assert(Status == EFI_SUCCESS);
  assert(StartStatus == EFI_SUCCESS);
  assert(gStartCalls == 1U);
  assert(gUnloadCalls == 1U);
  assert(gWatchdogArmCalls == 1U && gWatchdogDisableCalls == 1U);

  /* If a successfully armed timer cannot be disabled after the child returns,
     report the cleanup failure explicitly so the selector can exit to its parent
     boot manager instead of remaining interactive under a stale reset timer. */
  ResetState();
  gWatchdogDisableStatus = EFI_DEVICE_ERROR;
  {
    BOOLEAN LifecycleCompromised;
    LifecycleCompromised = FALSE;
    Status = BlsStartBootEntryEx((EFI_HANDLE)(UINTN)1U, &Entry, &StartStatus,
                                 NULL, 0U, &LifecycleCompromised);
    assert(Status == EFI_DEVICE_ERROR);
    assert(StartStatus == EFI_SUCCESS);
    assert(LifecycleCompromised == TRUE);
    assert(gStartCalls == 1U);
    assert(gWatchdogArmCalls == 1U && gWatchdogDisableCalls == 1U);
  }

  ResetState();
  gStartStatus = EFI_ABORTED;
  gUnloadStatus = EFI_INVALID_PARAMETER; /* conforming child exit: handle already gone */
  StartStatus = EFI_SUCCESS;
  Status = BlsStartBootEntry((EFI_HANDLE)(UINTN)1U, &Entry, &StartStatus);
  assert(Status == EFI_ABORTED);
  assert(StartStatus == EFI_ABORTED);
  assert(gProtocolQueryCalls == 1U);
  assert(gUnloadCalls == 1U);
  assert(gFreeCalls == 2U);

  ResetState();
  gScribbleExitDataOnSuccess = TRUE;
  {
    CHAR16 ExitText[16];
    memset(ExitText, 0, sizeof(ExitText));
    Status = BlsStartBootEntryEx((EFI_HANDLE)(UINTN)1U, &Entry, &StartStatus, ExitText, ARRAY_SIZE(ExitText), NULL);
    assert(Status == EFI_SUCCESS);
    assert(ExitText[0] == L'\0'); /* UEFI: ExitData is invalid for EFI_SUCCESS */
    assert(gFreeCalls == 2U); /* file-only + combined path; success OUT is not owned */
    assert(gFreedNonOwnedExitData == FALSE);
  }

  ResetState();
  gReturnExitData = TRUE;
  gReturnOversizedExitData = TRUE;
  gStartStatus = EFI_ABORTED;
  gUnloadStatus = EFI_INVALID_PARAMETER; /* conforming child exit */
  {
    CHAR16 ExitText[16];
    memset(ExitText, 0x5a, sizeof(ExitText));
    Status = BlsStartBootEntryEx((EFI_HANDLE)(UINTN)1U, &Entry, &StartStatus, ExitText, ARRAY_SIZE(ExitText), NULL);
    assert(Status == EFI_ABORTED);
    assert(StartStatus == EFI_ABORTED);
    assert(ExitText[0] == L'\0');
    assert(gUnloadCalls == 1U);
    assert(gFreeCalls == 3U); /* file-only + combined path + rejected firmware buffer */
  }

  /* A non-conforming direct StartImage error must not leak the loaded image or
     transfer ownership of firmware-scribbled OUT data. */
  ResetState();
  gStartStatus = EFI_DEVICE_ERROR;
  gScribbleExitDataOnFailure = TRUE;
  Status = BlsStartBootEntry((EFI_HANDLE)(UINTN)1U, &Entry, &StartStatus);
  assert(Status == EFI_DEVICE_ERROR);
  assert(StartStatus == EFI_DEVICE_ERROR);
  assert(gUnloadCalls == 1U);
  assert(gFreeCalls == 2U);
  assert(gFreedNonOwnedExitData == FALSE);

  /* Direct StartImage policy rejection leaves the loaded image owned by the
     caller. Hostile OUT data on that failure is not owned and must be ignored. */
  ResetState();
  gStartStatus = EFI_SECURITY_VIOLATION;
  gScribbleExitDataOnFailure = TRUE;
  Status = BlsStartBootEntry((EFI_HANDLE)(UINTN)1U, &Entry, &StartStatus);
  assert(Status == EFI_SECURITY_VIOLATION);
  assert(StartStatus == EFI_SECURITY_VIOLATION);
  assert(gProtocolQueryCalls == 1U);
  assert(gUnloadCalls == 1U);
  assert(gFreeCalls == 2U);
  assert(gFreedNonOwnedExitData == FALSE);

  /* The same status can be the child's own exit code. In that case firmware
     has already removed the image handle and ExitData ownership is transferred. */
  ResetState();
  gStartStatus = EFI_SECURITY_VIOLATION;
  gUnloadStatus = EFI_INVALID_PARAMETER;
  gReturnExitData = TRUE;
  {
    CHAR16 ExitText[16];
    memset(ExitText, 0, sizeof(ExitText));
    Status = BlsStartBootEntryEx((EFI_HANDLE)(UINTN)1U, &Entry, &StartStatus, ExitText, ARRAY_SIZE(ExitText), NULL);
    assert(Status == EFI_SECURITY_VIOLATION);
    assert(StartStatus == EFI_SECURITY_VIOLATION);
    assert(ExitText[0] == L'c' && ExitText[4] == L'd' && ExitText[5] == L'\0');
    assert(gUnloadCalls == 1U);
    assert(gFreeCalls == 3U);
  }

  /* EFI_INVALID_PARAMETER is likewise a legal child exit code. A direct
     StartImage rejection must not make us dereference firmware-scribbled OUTs. */
  ResetState();
  gStartStatus = EFI_INVALID_PARAMETER;
  gScribbleExitDataOnFailure = TRUE;
  Status = BlsStartBootEntry((EFI_HANDLE)(UINTN)1U, &Entry, &StartStatus);
  assert(Status == EFI_INVALID_PARAMETER);
  assert(StartStatus == EFI_INVALID_PARAMETER);
  assert(gUnloadCalls == 1U);
  assert(gFreeCalls == 2U);
  assert(gFreedNonOwnedExitData == FALSE);

  ResetState();
  gStartStatus = EFI_INVALID_PARAMETER;
  gUnloadStatus = EFI_INVALID_PARAMETER;
  gReturnExitData = TRUE;
  {
    CHAR16 ExitText[16];
    memset(ExitText, 0, sizeof(ExitText));
    Status = BlsStartBootEntryEx((EFI_HANDLE)(UINTN)1U, &Entry, &StartStatus, ExitText, ARRAY_SIZE(ExitText), NULL);
    assert(Status == EFI_INVALID_PARAMETER);
    assert(ExitText[0] == L'c' && ExitText[5] == L'\0');
    assert(gUnloadCalls == 1U);
    assert(gFreeCalls == 3U);
  }

  ResetState();
  gLoadStatus = EFI_DEVICE_ERROR;
  gLoadReturnsHandle = TRUE; /* hostile firmware scribbles a non-owned OUT */
  Status = BlsStartBootEntry((EFI_HANDLE)(UINTN)1U, &Entry, &StartStatus);
  assert(Status == EFI_DEVICE_ERROR);
  assert(StartStatus == EFI_NOT_STARTED);
  assert(gStartCalls == 0U);
  assert(gUnloadCalls == 0U); /* only SECURITY_VIOLATION transfers a handle on error */
  assert(gWatchdogCalls == 0U); /* watchdog starts only after successful LoadImage */
  assert(gFreeCalls == 2U);

  ResetState();
  gLoadStatus = EFI_SECURITY_VIOLATION;
  Status = BlsStartBootEntry((EFI_HANDLE)(UINTN)1U, &Entry, &StartStatus);
  assert(Status == EFI_SECURITY_VIOLATION);
  assert(gStartCalls == 0U);
  assert(gProtocolQueryCalls == 1U);
  assert(gUnloadCalls == 1U);
  assert(gWatchdogCalls == 0U);
  assert(gFreeCalls == 2U);

  ResetState();
  gStartStatus = EFI_SECURITY_VIOLATION;
  gUnloadStatus = EFI_INVALID_PARAMETER;
  Status = BlsStartBootEntry((EFI_HANDLE)(UINTN)1U, &Entry, &StartStatus);
  assert(Status == EFI_SECURITY_VIOLATION);
  assert(StartStatus == EFI_SECURITY_VIOLATION);
  assert(gUnloadCalls == 1U);

  ResetState();
  gStartStatus = EFI_SECURITY_VIOLATION;
  gUnloadStatus = EFI_DEVICE_ERROR;
  {
    BOOLEAN LifecycleCompromised;
    LifecycleCompromised = FALSE;
    Status = BlsStartBootEntryEx((EFI_HANDLE)(UINTN)1U, &Entry, &StartStatus,
                                 NULL, 0U, &LifecycleCompromised);
    assert(Status == EFI_DEVICE_ERROR);
    assert(StartStatus == EFI_SECURITY_VIOLATION);
    assert(LifecycleCompromised == TRUE);
    assert(gUnloadCalls == 1U);
  }

  puts("boot_image_lifecycle: PASS");
  return 0;
}

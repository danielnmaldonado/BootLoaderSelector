/* Credits: ©DNMTechLabs / dnmtechlabs@gmail.com */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#ifdef NULL
#undef NULL
#endif
#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>

EFI_HANDLE gImageHandle = NULL;
EFI_SYSTEM_TABLE *gST = NULL;
EFI_BOOT_SERVICES *gBS = NULL;

static UINTN gWatchdogCalls;
static UINTN gLastTimeout;
static UINT64 gLastCode;
static UINTN gLastDataSize;
static CHAR16 *gLastData;
static EFI_STATUS gWatchdogStatus = EFI_SUCCESS;

static EFI_STATUS EFIAPI TestSetWatchdogTimer(
  IN UINTN Timeout,
  IN UINT64 WatchdogCode,
  IN UINTN DataSize,
  IN CHAR16 *WatchdogData OPTIONAL
  ) {
  gWatchdogCalls++;
  gLastTimeout = Timeout;
  gLastCode = WatchdogCode;
  gLastDataSize = DataSize;
  gLastData = WatchdogData;
  return gWatchdogStatus;
}

#include "../../BootLoaderSelectorPkg/BootLoaderSelector/Main.c"

int main(void) {
  EFI_BOOT_SERVICES BootServices;

  gBS = NULL;
  gWatchdogCalls = 0U;
  BlsTryDisableInheritedBootWatchdog();
  assert(gWatchdogCalls == 0U);

  memset(&BootServices, 0, sizeof(BootServices));
  gBS = &BootServices;
  BlsTryDisableInheritedBootWatchdog();
  assert(gWatchdogCalls == 0U);

  BootServices.SetWatchdogTimer = TestSetWatchdogTimer;
  gLastTimeout = MAX_UINTN;
  gLastCode = MAX_UINT64;
  gLastDataSize = MAX_UINTN;
  gLastData = (CHAR16 *)(UINTN)1U;
  BlsTryDisableInheritedBootWatchdog();
  assert(gWatchdogCalls == 1U);
  assert(gLastTimeout == 0U);
  assert(gLastCode == 0U);
  assert(gLastDataSize == 0U);
  assert(gLastData == NULL);

  gWatchdogStatus = EFI_DEVICE_ERROR;
  BlsTryDisableInheritedBootWatchdog();
  assert(gWatchdogCalls == 2U);

  puts("watchdog_lifecycle: PASS");
  return 0;
}

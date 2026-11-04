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

static UINTN gSetTimerCalls;
static UINTN gCloseEventCalls;
static EFI_EVENT gLastTimerEvent;
static EFI_EVENT gLastCloseEvent;
static EFI_STATUS gCloseEventStatus;

static EFI_STATUS EFIAPI TestSetTimer(
  IN EFI_EVENT Event,
  IN EFI_TIMER_DELAY Type,
  IN UINT64 TriggerTime
  ) {
  gSetTimerCalls++;
  gLastTimerEvent = Event;
  assert(Type == TimerCancel);
  assert(TriggerTime == 0U);
  return EFI_DEVICE_ERROR;
}

static EFI_STATUS EFIAPI TestCloseEvent(IN EFI_EVENT Event) {
  gCloseEventCalls++;
  gLastCloseEvent = Event;
  return gCloseEventStatus;
}

#include "../../BootLoaderSelectorPkg/BootLoaderSelector/Utils.c"

int main(void) {
  EFI_BOOT_SERVICES BootServices;
  EFI_EVENT Event;
  EFI_STATUS Status;
  EFI_EVENT Handle;

  memset(&BootServices, 0, sizeof(BootServices));
  BootServices.SetTimer = TestSetTimer;
  BootServices.CloseEvent = TestCloseEvent;
  gBS = &BootServices;

  Handle = (EFI_EVENT)(UINTN)0x1234U;
  Event = Handle;
  gSetTimerCalls = 0U;
  gCloseEventCalls = 0U;
  gCloseEventStatus = EFI_SUCCESS;
  Status = BlsCloseTimerEvent(&Event);
  assert(Status == EFI_SUCCESS);
  assert(Event == NULL);
  assert(gSetTimerCalls == 0U);
  assert(gCloseEventCalls == 1U);
  assert(gLastCloseEvent == Handle);

  /* Even an impossible-by-spec error return must not leave a stale event
     reference that a caller could pass back to CloseEvent a second time. */
  Handle = (EFI_EVENT)(UINTN)0x5678U;
  Event = Handle;
  gCloseEventStatus = EFI_DEVICE_ERROR;
  Status = BlsCloseTimerEvent(&Event);
  assert(Status == EFI_DEVICE_ERROR);
  assert(Event == NULL);
  assert(gSetTimerCalls == 0U);
  assert(gCloseEventCalls == 2U);
  assert(gLastCloseEvent == Handle);

  Status = BlsCloseTimerEvent(&Event);
  assert(Status == EFI_SUCCESS);
  assert(gCloseEventCalls == 2U);

  puts("event_lifecycle: PASS");
  return 0;
}

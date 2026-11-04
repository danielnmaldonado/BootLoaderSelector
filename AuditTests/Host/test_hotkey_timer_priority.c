/* Credits: ©DNMTechLabs / dnmtechlabs@gmail.com */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#ifdef NULL
#undef NULL
#endif
#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/UefiBootServicesTableLib.h>

EFI_HANDLE gImageHandle = NULL;
EFI_SYSTEM_TABLE *gST = NULL;
EFI_BOOT_SERVICES *gBS = NULL;

static UINTN gWaitCalls;
static UINTN gReadCalls;
static UINTN gCheckCalls;
static UINTN gCloseCalls;
static UINTN gCheckSignalsAfter;
static BOOLEAN gReturnArrow;
static BOOLEAN gReturnNotReady;
static BOOLEAN gReturnEmpty;
static BOOLEAN gSingleEventWait;
static BOOLEAN gCreateFailsWithScribble;
static EFI_EVENT gPoisonEvent = (EFI_EVENT)(UINTN)0xDEADU;

EFI_STATUS BlsCloseTimerEvent(IN OUT EFI_EVENT *Event);

VOID *EFIAPI ZeroMem(OUT VOID *Buffer, IN UINTN Length) {
  return memset(Buffer, 0, (size_t)Length);
}

EFI_STATUS BlsCloseTimerEvent(IN OUT EFI_EVENT *Event) {
  if (Event == NULL || *Event == NULL) return EFI_SUCCESS;
  assert(*Event != gPoisonEvent);
  *Event = NULL;
  gCloseCalls++;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI TestCreateEvent(
  IN UINT32 Type,
  IN EFI_TPL NotifyTpl,
  IN EFI_EVENT_NOTIFY NotifyFunction OPTIONAL,
  IN VOID *NotifyContext OPTIONAL,
  OUT EFI_EVENT *Event
  ) {
  (VOID)NotifyTpl;
  (VOID)NotifyFunction;
  (VOID)NotifyContext;
  assert(Type == EVT_TIMER);
  assert(Event != NULL);
  if (gCreateFailsWithScribble) {
    *Event = gPoisonEvent;
    return EFI_DEVICE_ERROR;
  }
  *Event = (EFI_EVENT)(UINTN)0x1234U;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI TestSetTimer(
  IN EFI_EVENT Event,
  IN EFI_TIMER_DELAY Type,
  IN UINT64 TriggerTime
  ) {
  assert(Event == (EFI_EVENT)(UINTN)0x1234U);
  assert(Type == TimerRelative);
  assert(TriggerTime == 20000000ULL);
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI TestWaitForEvent(
  IN UINTN NumberOfEvents,
  IN EFI_EVENT *Event,
  OUT UINTN *Index
  ) {
  assert(NumberOfEvents == (gSingleEventWait ? 1U : 2U));
  assert(Event != NULL);
  assert(Index != NULL);
  gWaitCalls++;
  if (gWaitCalls > 2048U) return EFI_DEVICE_ERROR;

  /* Model a conforming firmware with a continuously signaled keyboard event
     at index 0. Once the timer is also signaled, WaitForEvent still returns
     index 0 because events are evaluated in array order. */
  *Index = 0U;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI TestCheckEvent(IN EFI_EVENT Event) {
  assert(Event == (EFI_EVENT)(UINTN)0x1234U);
  gCheckCalls++;
  if (gCheckCalls >= gCheckSignalsAfter) return EFI_SUCCESS;
  return EFI_NOT_READY;
}

static EFI_STATUS EFIAPI TestReadKeyStroke(
  IN EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This,
  OUT EFI_INPUT_KEY *Key
  ) {
  (VOID)This;
  assert(Key != NULL);
  gReadCalls++;
  if (gReturnNotReady) return EFI_NOT_READY;
  if (gReturnEmpty) {
    Key->ScanCode = SCAN_NULL;
    Key->UnicodeChar = CHAR_NULL;
  } else if (gReturnArrow) {
    Key->ScanCode = SCAN_UP;
    Key->UnicodeChar = CHAR_NULL;
  } else {
    Key->ScanCode = SCAN_NULL;
    Key->UnicodeChar = L'x';
  }
  return EFI_SUCCESS;
}

#include "../../BootLoaderSelectorPkg/BootLoaderSelector/Main.c"

static VOID ResetCounters(VOID) {
  gWaitCalls = 0U;
  gReadCalls = 0U;
  gCheckCalls = 0U;
  gCloseCalls = 0U;
  gReturnEmpty = FALSE;
  gSingleEventWait = FALSE;
  gCreateFailsWithScribble = FALSE;
}

int main(void) {
  EFI_BOOT_SERVICES BootServices;
  EFI_SYSTEM_TABLE SystemTable;
  EFI_SIMPLE_TEXT_INPUT_PROTOCOL ConIn;
  BOOLEAN ShowSelector;

  memset(&BootServices, 0, sizeof(BootServices));
  memset(&SystemTable, 0, sizeof(SystemTable));
  memset(&ConIn, 0, sizeof(ConIn));

  BootServices.CreateEvent = TestCreateEvent;
  BootServices.SetTimer = TestSetTimer;
  BootServices.WaitForEvent = TestWaitForEvent;
  BootServices.CheckEvent = TestCheckEvent;
  /* BlsWaitForSelectorHotkey only validates this member; cleanup is provided
     by the host BlsCloseTimerEvent stub above. */
  BootServices.CloseEvent = (EFI_CLOSE_EVENT)(UINTN)1U;
  ConIn.WaitForKey = (EFI_EVENT)(UINTN)0xABCDU;
  ConIn.ReadKeyStroke = TestReadKeyStroke;
  SystemTable.ConIn = &ConIn;
  gBS = &BootServices;
  gST = &SystemTable;


  /* OUT event handles are owned only when CreateEvent succeeds. A broken
     firmware that scribbles the OUT parameter while returning an error must
     never trick cleanup into closing that unowned value. */
  ResetCounters();
  gCreateFailsWithScribble = TRUE;
  ShowSelector = BlsWaitForSelectorHotkey();
  assert(ShowSelector == TRUE);
  assert(gWaitCalls == 0U);
  assert(gReadCalls == 0U);
  assert(gCloseCalls == 0U);

  /* Continuous non-selector input must not extend the two-second window. */
  ResetCounters();
  gReturnArrow = FALSE;
  gReturnNotReady = FALSE;
  gCheckSignalsAfter = 3U;
  ShowSelector = BlsWaitForSelectorHotkey();
  assert(ShowSelector == FALSE);
  assert(gWaitCalls == 3U);
  assert(gReadCalls == 3U);
  assert(gCheckCalls == 3U);
  assert(gCloseCalls == 1U);

  /* A spurious/stale WaitForKey signal followed by EFI_NOT_READY must also
     observe an already-expired timer instead of spinning on the key event. */
  ResetCounters();
  gReturnArrow = FALSE;
  gReturnNotReady = TRUE;
  gCheckSignalsAfter = 2U;
  ShowSelector = BlsWaitForSelectorHotkey();
  assert(ShowSelector == FALSE);
  assert(gWaitCalls == 2U);
  assert(gReadCalls == 2U);
  assert(gCheckCalls == 2U);
  assert(gCloseCalls == 1U);

  /* Preserve the existing priority of a real selector arrow at the boundary. */
  ResetCounters();
  gReturnArrow = TRUE;
  gReturnNotReady = FALSE;
  gCheckSignalsAfter = 1U;
  ShowSelector = BlsWaitForSelectorHotkey();
  assert(ShowSelector == TRUE);
  assert(gWaitCalls == 1U);
  assert(gReadCalls == 1U);
  assert(gCheckCalls == 0U);
  assert(gCloseCalls == 1U);

  /* A partially implemented firmware without CheckEvent must fail safe to
     showing the selector instead of allowing a permanently signaled keyboard
     event to starve the hotkey timeout forever. */
  ResetCounters();
  gReturnArrow = FALSE;
  gReturnNotReady = TRUE;
  gCheckSignalsAfter = MAX_UINTN;
  BootServices.CheckEvent = NULL;
  ShowSelector = BlsWaitForSelectorHotkey();
  assert(ShowSelector == TRUE);
  assert(gWaitCalls == 0U);
  assert(gReadCalls == 0U);
  assert(gCloseCalls == 0U);
  BootServices.CheckEvent = TestCheckEvent;

  /* Even with CheckEvent present, doubly non-conforming firmware can keep the
     keyboard signaled forever while never reporting the timer as expired. The
     selector probe must fail safe after a bounded number of iterations. */
  ResetCounters();
  gReturnArrow = FALSE;
  gReturnNotReady = TRUE;
  gCheckSignalsAfter = MAX_UINTN;
  ShowSelector = BlsWaitForSelectorHotkey();
  assert(ShowSelector == TRUE);
  assert(gWaitCalls == BLS_HOTKEY_SPIN_MAX);
  assert(gReadCalls == BLS_HOTKEY_SPIN_MAX);
  assert(gCheckCalls == BLS_HOTKEY_SPIN_MAX);
  assert(gCloseCalls == 1U);

  /* The post-failure "press any key" wait must not livelock if firmware
     repeatedly reports a signaled input event but returns an empty key. */
  ResetCounters();
  gReturnEmpty = TRUE;
  gSingleEventWait = TRUE;
  BlsWaitForRealKey();
  assert(gWaitCalls == BLS_NOT_READY_SPIN_MAX);
  assert(gReadCalls == BLS_NOT_READY_SPIN_MAX);

  puts("hotkey_timer_priority: PASS");
  return 0;
}

/* Credits: ©DNMTechLabs / dnmtechlabs@gmail.com */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#ifdef NULL
#undef NULL
#endif
#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include "../../BootLoaderSelectorPkg/BootLoaderSelector/BootScan.h"
#include "../../BootLoaderSelectorPkg/BootLoaderSelector/Config.h"
#include "../../BootLoaderSelectorPkg/BootLoaderSelector/HardwareInfo.h"
#include "../../BootLoaderSelectorPkg/BootLoaderSelector/Menu.h"
#include "../../BootLoaderSelectorPkg/BootLoaderSelector/Screenshot.h"
#include "../../BootLoaderSelectorPkg/BootLoaderSelector/Ui.h"
#include "../../BootLoaderSelectorPkg/BootLoaderSelector/Utils.h"

EFI_HANDLE gImageHandle = NULL;
EFI_SYSTEM_TABLE *gST = NULL;
EFI_BOOT_SERVICES *gBS = NULL;

static UINTN gWaitCalls;
static UINTN gReadCalls;
static UINTN gCheckCalls;
static UINTN gDeadlineChecks;
static UINTN gTimerChecks;
static UINTN gCloseCalls;
static UINTN gCreateCalls;
static UINTN gSetTimerCalls;
static UINTN gUiPrintCalls;
static EFI_HANDLE gScreenshotVolumeSeen;
static UINTN gUiCols = 80U;
static UINTN gUiRows = 25U;
static UINTN gCreateMode;
static UINTN gInputMode;
static EFI_EVENT gTimerEvent = (EFI_EVENT)(UINTN)0x1234U;
static EFI_EVENT gDeadlineEvent = (EFI_EVENT)(UINTN)0x5678U;
static EFI_EVENT gPoisonEvent = (EFI_EVENT)(UINTN)0xDEADU;

VOID *EFIAPI ZeroMem(OUT VOID *Buffer, IN UINTN Length) {
  return memset(Buffer, 0, (size_t)Length);
}

VOID *EFIAPI CopyMem(OUT VOID *DestinationBuffer, IN CONST VOID *SourceBuffer, IN UINTN Length) {
  return memcpy(DestinationBuffer, SourceBuffer, (size_t)Length);
}

UINTN EFIAPI UnicodeSPrint(
  OUT CHAR16 *StartOfBuffer,
  IN UINTN BufferSize,
  IN CONST CHAR16 *FormatString,
  ...
  ) {
  (void)FormatString;
  if (StartOfBuffer != NULL && BufferSize >= sizeof(CHAR16)) StartOfBuffer[0] = L'\0';
  return 0;
}

UINTN EFIAPI UnicodeVSPrint(
  OUT CHAR16 *StartOfBuffer,
  IN UINTN BufferSize,
  IN CONST CHAR16 *FormatString,
  IN VA_LIST Marker
  ) {
  (void)FormatString;
  (void)Marker;
  if (StartOfBuffer != NULL && BufferSize >= sizeof(CHAR16)) StartOfBuffer[0] = L'\0';
  return 0;
}

UINTN BlsStrLen16(IN CONST CHAR16 *Str) {
  UINTN Len;
  if (Str == NULL) return 0;
  for (Len = 0; Len < 65535U && Str[Len] != L'\0'; Len++) {}
  return Len;
}

UINTN BlsStrLen16Bounded(IN CONST CHAR16 *Str, IN UINTN MaxChars) {
  UINTN Len;
  if (Str == NULL) return 0;
  for (Len = 0; Len < MaxChars && Str[Len] != L'\0'; Len++) {}
  return Len;
}


VOID BlsSafeStrCpy(OUT CHAR16 *Dst, IN UINTN DstChars, IN CONST CHAR16 *Src) {
  UINTN Index;
  if (Dst == NULL || DstChars == 0) return;
  Index = 0;
  while (Src != NULL && Index + 1U < DstChars && Src[Index] != L'\0') {
    Dst[Index] = Src[Index];
    Index++;
  }
  Dst[Index] = L'\0';
}

BOOLEAN BlsConfigMatchesEntry(IN CONST BLS_CONFIG *Config, IN CONST BLS_BOOT_ENTRY *Entry) {
  if (Config == NULL || Entry == NULL) return FALSE;
  /* Test-only selector hook: Debug chooses the Clover entry so keyboard
     contract cases can start from the middle row and detect accidental
     Home/End/Page navigation additions. */
  return (BOOLEAN)(Config->Debug && Entry->Type == BlsLoaderClover);
}

EFI_STATUS BlsSaveConfig(IN EFI_HANDLE VolumeHandle, IN CONST BLS_CONFIG *Config) {
  (void)VolumeHandle;
  (void)Config;
  return EFI_SUCCESS;
}

EFI_STATUS BlsSaveScreenshot(IN EFI_HANDLE VolumeHandle, OUT CHAR16 *SavedPath, IN UINTN SavedPathChars) {
  gScreenshotVolumeSeen = VolumeHandle;
  if (SavedPath != NULL && SavedPathChars > 0) SavedPath[0] = L'\0';
  return EFI_UNSUPPORTED;
}

VOID BlsUiGetSize(OUT UINTN *Cols, OUT UINTN *Rows) {
  if (Cols != NULL) *Cols = gUiCols;
  if (Rows != NULL) *Rows = gUiRows;
}
VOID BlsUiClear(VOID) {}
VOID BlsUiBox(IN UINTN Col, IN UINTN Row, IN UINTN Width, IN UINTN Height, IN UINTN Attr) {
  (void)Col; (void)Row; (void)Width; (void)Height; (void)Attr;
}
VOID BlsUiCentered(IN UINTN Row, IN UINTN Attr, IN CONST CHAR16 *Text) {
  (void)Row; (void)Attr; (void)Text;
}
VOID BlsUiHLine(IN UINTN Col, IN UINTN Row, IN UINTN Width, IN UINTN Attr, IN CHAR16 Ch) {
  (void)Col; (void)Row; (void)Width; (void)Attr; (void)Ch;
}
VOID BlsUiPrintAt(IN UINTN Col, IN UINTN Row, IN UINTN Attr, IN CONST CHAR16 *Format, ...) {
  (void)Col; (void)Row; (void)Attr; (void)Format;
  gUiPrintCalls++;
}

EFI_STATUS BlsCloseTimerEvent(IN OUT EFI_EVENT *Event) {
  if (Event == NULL) return EFI_INVALID_PARAMETER;
  if (*Event != NULL) {
    assert(*Event != gPoisonEvent);
    *Event = NULL;
    gCloseCalls++;
  }
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI TestCreateEvent(
  IN UINT32 Type,
  IN EFI_TPL NotifyTpl,
  IN EFI_EVENT_NOTIFY NotifyFunction OPTIONAL,
  IN VOID *NotifyContext OPTIONAL,
  OUT EFI_EVENT *Event
  ) {
  (void)NotifyTpl;
  (void)NotifyFunction;
  (void)NotifyContext;
  assert(Type == EVT_TIMER);
  assert(Event != NULL);
  gCreateCalls++;
  if ((gCreateMode == 1U && gCreateCalls == 1U) ||
      (gCreateMode == 2U && gCreateCalls == 2U)) {
    *Event = NULL;
    return EFI_SUCCESS;
  }
  if ((gCreateMode == 3U && gCreateCalls == 1U) ||
      (gCreateMode == 4U && gCreateCalls == 2U)) {
    *Event = gPoisonEvent;
    return EFI_DEVICE_ERROR;
  }
  *Event = (gCreateCalls == 1U) ? gTimerEvent : gDeadlineEvent;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI TestSetTimer(IN EFI_EVENT Event, IN EFI_TIMER_DELAY Type, IN UINT64 TriggerTime) {
  gSetTimerCalls++;
  assert(Event != NULL);
  if (Event == gTimerEvent) {
    assert(Type == TimerPeriodic);
    assert(TriggerTime == 10000000ULL);
  } else {
    assert(Event == gDeadlineEvent);
    assert(Type == TimerRelative);
    assert(TriggerTime == 30000000ULL);
  }
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI TestWaitForEvent(IN UINTN NumberOfEvents, IN EFI_EVENT *Events, OUT UINTN *Index) {
  assert(NumberOfEvents == 3U || NumberOfEvents == 1U);
  assert(Events != NULL);
  assert(Index != NULL);
  gWaitCalls++;
  if (gWaitCalls > 128U) return EFI_DEVICE_ERROR;
  *Index = 0U;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI TestCheckEvent(IN EFI_EVENT Event) {
  gCheckCalls++;
  if (Event == gDeadlineEvent) {
    gDeadlineChecks++;
    return (gDeadlineChecks >= 3U) ? EFI_SUCCESS : EFI_NOT_READY;
  }
  assert(Event == gTimerEvent);
  gTimerChecks++;
  return (gInputMode == 10U) ? EFI_NOT_READY : EFI_SUCCESS;
}

static EFI_STATUS EFIAPI TestReadKeyStroke(IN EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This, OUT EFI_INPUT_KEY *Key) {
  (void)This;
  assert(Key != NULL);
  gReadCalls++;
  if (gInputMode == 0U || gInputMode == 10U) return EFI_NOT_READY;
  if (gInputMode == 1U) {
    Key->ScanCode = SCAN_NULL;
    Key->UnicodeChar = CHAR_NULL;
    return EFI_SUCCESS;
  }
  if (gInputMode == 3U) {
    Key->ScanCode = (gReadCalls == 1U) ? SCAN_F1 : ((gReadCalls == 2U) ? SCAN_NULL : SCAN_ESC);
    Key->UnicodeChar = (gReadCalls == 2U) ? CHAR_CARRIAGE_RETURN : CHAR_NULL;
    return EFI_SUCCESS;
  }
  if (gInputMode == 4U) {
    Key->ScanCode = (gReadCalls <= 2U) ? SCAN_NULL : SCAN_ESC;
    Key->UnicodeChar = (gReadCalls == 1U) ? L' ' : ((gReadCalls == 2U) ? CHAR_CARRIAGE_RETURN : CHAR_NULL);
    return EFI_SUCCESS;
  }
  if (gInputMode == 9U) {
    Key->ScanCode = (gReadCalls == 1U) ? SCAN_F10 : SCAN_ESC;
    Key->UnicodeChar = CHAR_NULL;
    return EFI_SUCCESS;
  }
  if (gInputMode >= 5U && gInputMode <= 8U) {
    if (gReadCalls == 1U) {
      static CONST UINT16 UnmappedScans[] = { SCAN_HOME, SCAN_END, SCAN_PAGE_UP, SCAN_PAGE_DOWN };
      Key->ScanCode = UnmappedScans[gInputMode - 5U];
      Key->UnicodeChar = CHAR_NULL;
    } else {
      Key->ScanCode = SCAN_NULL;
      Key->UnicodeChar = CHAR_CARRIAGE_RETURN;
    }
    return EFI_SUCCESS;
  }
  Key->ScanCode = SCAN_ESC;
  Key->UnicodeChar = CHAR_NULL;
  return EFI_SUCCESS;
}

#include "../../BootLoaderSelectorPkg/BootLoaderSelector/Menu.c"

static VOID ResetCounters(VOID) {
  gWaitCalls = 0U;
  gReadCalls = 0U;
  gCheckCalls = 0U;
  gDeadlineChecks = 0U;
  gTimerChecks = 0U;
  gCloseCalls = 0U;
  gCreateCalls = 0U;
  gSetTimerCalls = 0U;
  gUiPrintCalls = 0U;
  gScreenshotVolumeSeen = NULL;
  gCreateMode = 0U;
}

static VOID RunStarvationCase(IN UINTN InputMode) {
  BLS_CONFIG Config;
  BLS_BOOT_LIST List;
  UINTN Selected;
  BLS_MENU_ACTION Action;
  EFI_STATUS Status;

  memset(&Config, 0, sizeof(Config));
  memset(&List, 0, sizeof(List));
  Config.Timeout = 3U;
  List.Count = 1U;
  gInputMode = InputMode;
  ResetCounters();

  Status = BlsRunMenu(NULL, &Config, NULL, NULL, &List, FALSE, &Selected, &Action);
  assert(Status == EFI_SUCCESS);
  assert(Action == BlsMenuBoot);
  assert(Selected == 0U);
  assert(gWaitCalls == 3U);
  assert(gReadCalls == 3U);
  assert(gDeadlineChecks == 3U);
  assert(gTimerChecks == 2U);
  /* Three spurious keyboard wakeups must not repaint the unchanged counter.
     This is 20 prints (including one initial counter) rather than the
     previous 22, which included two redundant firmware draw operations. */
  assert(gUiPrintCalls == ((InputMode == 10U) ? 20U : 22U));
  assert(gCloseCalls == 2U);
}

int main(void) {
  EFI_BOOT_SERVICES BootServices;
  EFI_SYSTEM_TABLE SystemTable;
  EFI_SIMPLE_TEXT_INPUT_PROTOCOL ConIn;
  BLS_CONFIG Config;
  BLS_BOOT_LIST List;
  UINTN Selected;
  BLS_MENU_ACTION Action;
  EFI_STATUS Status;

  memset(&BootServices, 0, sizeof(BootServices));
  memset(&SystemTable, 0, sizeof(SystemTable));
  memset(&ConIn, 0, sizeof(ConIn));
  BootServices.CreateEvent = TestCreateEvent;
  BootServices.SetTimer = TestSetTimer;
  BootServices.WaitForEvent = TestWaitForEvent;
  BootServices.CheckEvent = TestCheckEvent;
  BootServices.CloseEvent = (EFI_CLOSE_EVENT)(UINTN)1U;
  ConIn.WaitForKey = (EFI_EVENT)(UINTN)0xABCDU;
  ConIn.ReadKeyStroke = TestReadKeyStroke;
  SystemTable.ConIn = &ConIn;
  gBS = &BootServices;
  gST = &SystemTable;

  RunStarvationCase(0U); /* spurious WaitForKey + EFI_NOT_READY */
  RunStarvationCase(1U); /* success with a null/non-key input record */
  RunStarvationCase(10U); /* spurious input without a timer tick: no repaint */

  /* A real key must retain priority at the timer boundary and cancel autoboot. */
  memset(&Config, 0, sizeof(Config));
  memset(&List, 0, sizeof(List));
  Config.Timeout = 3U;
  List.Count = 1U;
  gInputMode = 2U;
  ResetCounters();
  Status = BlsRunMenu(NULL, &Config, NULL, NULL, &List, FALSE, &Selected, &Action);
  assert(Status == EFI_SUCCESS);
  assert(Action == BlsMenuExit);
  assert(gWaitCalls == 1U);
  assert(gReadCalls == 1U);
  assert(gCheckCalls == 0U);
  assert(gCloseCalls == 2U);

  /* F1 Help and Space Details are modal diagnostics. They cancel autoboot once,
     consume only the explicit dismissal key, redraw, and then return control to
     the normal selector without leaking timer events. */
  memset(&Config, 0, sizeof(Config));
  memset(&List, 0, sizeof(List));
  Config.Timeout = 3U;
  List.Count = 1U;
  List.Entries[0].Type = BlsLoaderOpenCore;
  BlsSafeStrCpy(List.Entries[0].Name, ARRAY_SIZE(List.Entries[0].Name), L"OpenCore");
  BlsSafeStrCpy(List.Entries[0].Path, ARRAY_SIZE(List.Entries[0].Path), L"\\EFI\\OC\\OpenCore.efi");
  BlsSafeStrCpy(List.Entries[0].VolumeLabel, ARRAY_SIZE(List.Entries[0].VolumeLabel), L"EFI");
  BlsSafeStrCpy(List.Entries[0].VolumeDevicePath, ARRAY_SIZE(List.Entries[0].VolumeDevicePath), L"GPT:TEST");
  gInputMode = 3U;
  ResetCounters();
  Status = BlsRunMenu(NULL, &Config, NULL, NULL, &List, FALSE, &Selected, &Action);
  assert(Status == EFI_SUCCESS);
  assert(Action == BlsMenuExit);
  assert(gReadCalls == 3U);
  assert(gWaitCalls == 3U);
  assert(gCloseCalls == 2U);

  gInputMode = 4U;
  ResetCounters();
  Status = BlsRunMenu(NULL, &Config, NULL, NULL, &List, FALSE, &Selected, &Action);
  assert(Status == EFI_SUCCESS);
  assert(Action == BlsMenuExit);
  assert(gReadCalls == 3U);
  assert(gWaitCalls == 3U);
  assert(gCloseCalls == 2U);

  /* Keyboard contract regression: Round 41 briefly added Home/End/Page keys.
     They are intentionally NOT selector commands. Starting from the middle
     entry, each must leave selection unchanged and the following Enter must
     boot that same entry. Production key handling is not modified by this test. */
  {
    UINTN mode;

    memset(&Config, 0, sizeof(Config));
    memset(&List, 0, sizeof(List));
    Config.Timeout = 0U;
    Config.Debug = TRUE;
    List.Count = 3U;
    List.Entries[0].Type = BlsLoaderOpenCore;
    List.Entries[1].Type = BlsLoaderClover;
    List.Entries[2].Type = BlsLoaderOpenCore;
    for (mode = 5U; mode <= 8U; mode++) {
      gInputMode = mode;
      ResetCounters();
      Status = BlsRunMenu(NULL, &Config, NULL, NULL, &List, FALSE, &Selected, &Action);
      assert(Status == EFI_SUCCESS);
      assert(Action == BlsMenuBoot);
      assert(Selected == 1U);
      assert(gReadCalls == 2U);
    }
  }

  /* Screenshot storage is independent from configuration write authorization.
     A malformed plist may intentionally leave ConfigVolumeHandle NULL while F10
     must still target the selector/image volume. */
  memset(&Config, 0, sizeof(Config));
  memset(&List, 0, sizeof(List));
  Config.Timeout = 0U;
  List.Count = 1U;
  gInputMode = 9U;
  ResetCounters();
  Status = BlsRunMenu(NULL, &Config, NULL, (EFI_HANDLE)(UINTN)0xBEEFU, &List, FALSE, &Selected, &Action);
  assert(Status == EFI_SUCCESS);
  assert(Action == BlsMenuExit);
  assert(gScreenshotVolumeSeen == (EFI_HANDLE)(UINTN)0xBEEFU);

  /* Non-conforming firmware must never receive SetTimer(NULL) when CreateEvent
     reports success without returning the requested handle. */
  memset(&Config, 0, sizeof(Config));
  memset(&List, 0, sizeof(List));
  Config.Timeout = 3U;
  List.Count = 1U;
  gInputMode = 2U;
  ResetCounters();
  gCreateMode = 1U;
  Status = BlsRunMenu(NULL, &Config, NULL, NULL, &List, FALSE, &Selected, &Action);
  assert(Status == EFI_SUCCESS);
  assert(Action == BlsMenuExit);
  assert(gCreateCalls == 1U);
  assert(gSetTimerCalls == 0U);
  assert(gCloseCalls == 0U);

  ResetCounters();
  gInputMode = 2U;
  gCreateMode = 2U;
  Status = BlsRunMenu(NULL, &Config, NULL, NULL, &List, FALSE, &Selected, &Action);
  assert(Status == EFI_SUCCESS);
  assert(Action == BlsMenuExit);
  assert(gCreateCalls == 2U);
  assert(gSetTimerCalls == 0U);
  assert(gCloseCalls == 1U);


  /* A failed CreateEvent call does not transfer ownership of its OUT value,
     even if broken firmware scribbles a non-NULL handle there. Cleanup must
     close only events acquired by successful calls. */
  ResetCounters();
  gInputMode = 2U;
  gCreateMode = 3U;
  Status = BlsRunMenu(NULL, &Config, NULL, NULL, &List, FALSE, &Selected, &Action);
  assert(Status == EFI_SUCCESS);
  assert(Action == BlsMenuExit);
  assert(gCreateCalls == 1U);
  assert(gSetTimerCalls == 0U);
  assert(gCloseCalls == 0U);

  ResetCounters();
  gInputMode = 2U;
  gCreateMode = 4U;
  Status = BlsRunMenu(NULL, &Config, NULL, NULL, &List, FALSE, &Selected, &Action);
  assert(Status == EFI_SUCCESS);
  assert(Action == BlsMenuExit);
  assert(gCreateCalls == 2U);
  assert(gSetTimerCalls == 0U);
  assert(gCloseCalls == 1U);

  /* Selection movement within the same viewport should repaint only the two
     affected rows; crossing a viewport boundary must fall back to full redraw. */
  memset(&List, 0, sizeof(List));
  List.Count = 3U;
  gUiCols = 80U;
  gUiRows = 25U;
  gUiPrintCalls = 0U;
  assert(BlsDrawSelectionDelta(&List, 0U, 1U, FALSE) == TRUE);
  assert(gUiPrintCalls == 2U);

  memset(&List, 0, sizeof(List));
  List.Count = BLS_MAX_ENTRIES;
  gUiPrintCalls = 0U;
  assert(BlsDrawSelectionDelta(&List, 12U, 13U, FALSE) == FALSE);
  assert(gUiPrintCalls == 0U);

  gUiRows = 7U;
  gUiPrintCalls = 0U;
  assert(BlsDrawSelectionDelta(&List, 0U, 1U, FALSE) == FALSE);
  assert(gUiPrintCalls == 0U);
  gUiRows = 25U;

  /* Options navigation/value edits have fixed geometry too: repaint only the
     rows whose selection/value actually changed instead of re-emitting the
     complete panel and explanatory text on every key press. */
  memset(&Config, 0, sizeof(Config));
  Config.Timeout = 10U;
  gUiCols = 80U;
  gUiRows = 25U;
  gUiPrintCalls = 0U;
  BlsDrawOptionsSelectionDelta(&Config, 0U, 1U);
  assert(gUiPrintCalls == 2U);
  gUiPrintCalls = 0U;
  BlsDrawOptionValueDelta(&Config, 0U);
  assert(gUiPrintCalls == 1U);
  gUiRows = 12U;
  gUiPrintCalls = 0U;
  BlsDrawOptionsSelectionDelta(&Config, 2U, 3U);
  assert(gUiPrintCalls == 2U);
  gUiRows = 25U;

  /* A corrupted/non-NUL-terminated hardware field must stay inside its
     BLS_MAX_TEXT ownership boundary even when a wide console offers more room.
     Before Round 55 this read one CHAR16 beyond CorruptValue under ASan. */
  {
    CHAR16 CorruptValue[BLS_MAX_TEXT];
    UINTN Index;

    for (Index = 0U; Index < ARRAY_SIZE(CorruptValue); Index++) {
      CorruptValue[Index] = L'X';
    }
    gUiCols = 220U;
    gUiPrintCalls = 0U;
    BlsDrawHardwareField(0U, 0U, 200U, L"GPU", CorruptValue);
    assert(gUiPrintCalls == 1U);
    gUiCols = 80U;
  }

  /* With no countdown/deadline available, a permanently stale WaitForKey must
     terminate at the selector's own spurious-signal budget rather than spin. */
  memset(&Config, 0, sizeof(Config));
  memset(&List, 0, sizeof(List));
  Config.Timeout = 0U;
  List.Count = 1U;
  gInputMode = 0U;
  ResetCounters();
  Status = BlsRunMenu(NULL, &Config, NULL, NULL, &List, FALSE, &Selected, &Action);
  assert(Status == EFI_DEVICE_ERROR);
  assert(gReadCalls == BLS_SPURIOUS_KEY_SIGNAL_MAX);

  /* Options uses the same bounded policy. */
  memset(&Config, 0, sizeof(Config));
  Config.Timeout = 10U;
  gInputMode = 0U;
  ResetCounters();
  Status = BlsRunOptions(&Config, NULL, NULL);
  assert(Status == EFI_DEVICE_ERROR);
  assert(gReadCalls == BLS_SPURIOUS_KEY_SIGNAL_MAX);

  puts("menu_countdown_starvation: PASS");
  return 0;
}

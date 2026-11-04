/* Credits: ©DNMTechLabs / dnmtechlabs@gmail.com */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#ifdef NULL
#undef NULL
#endif
#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/PrintLib.h>
#include "../../BootLoaderSelectorPkg/BootLoaderSelector/Utils.h"

EFI_HANDLE gImageHandle = NULL;
EFI_SYSTEM_TABLE *gST = NULL;
EFI_BOOT_SERVICES *gBS = NULL;

/* A minimal host print shim for the UI empty-string side-effect regression.
   The only exercised formatting is %s with a CHAR16 string. */
UINTN BlsStrLen16(IN CONST CHAR16 *Str) {
  UINTN Length = 0U;
  if (Str == NULL) return 0U;
  while (Length < 65535U && Str[Length] != L'\0') Length++;
  return Length;
}

UINTN EFIAPI UnicodeVSPrint(
  OUT CHAR16 *StartOfBuffer, IN UINTN BufferSize,
  IN CONST CHAR16 *FormatString, IN VA_LIST Marker
  ) {
  CONST CHAR16 *Text;
  UINTN Capacity;
  UINTN Index;

  if (StartOfBuffer == NULL || BufferSize < sizeof(CHAR16) ||
      FormatString == NULL) return 0U;
  if (FormatString[0] != L'%' || FormatString[1] != L's' ||
      FormatString[2] != L'\0') {
    StartOfBuffer[0] = L'\0';
    return 0U;
  }
  Text = VA_ARG(Marker, CONST CHAR16 *);
  Capacity = BufferSize / sizeof(CHAR16);
  Index = 0U;
  while (Text != NULL && Index + 1U < Capacity && Text[Index] != L'\0') {
    StartOfBuffer[Index] = Text[Index];
    Index++;
  }
  StartOfBuffer[Index] = L'\0';
  return Index;
}

typedef enum {
  TestModesUnsupported,
  TestModesHugeOnly,
  TestModesHugeThenValid,
  TestModesSmallThenValid,
  TestModesCurrentValid,
  TestModesTwoValid,
  TestModesCurrentWide
} TEST_MODE_SCENARIO;

static UINTN gQueryCount;
static UINTN gQueryNullModeOnMode;
static UINTN gQuerySwitchModeOnMode;
static INT32 gQuerySwitchModeTo;
static BOOLEAN gEnableCursorNullMode;
static UINTN gSetModeCount;
static EFI_STATUS gSetModeStatus;
static BOOLEAN gSetModeNoopOnSuccess;
static BOOLEAN gSetModeAppliesDespiteError;
static BOOLEAN gSetModeNullModeOnSuccess;
static UINTN gSetAttributeCount;
static UINTN gSetCursorCount;
static UINTN gUiOperationCount;
static CHAR8 gUiOperations[32];
static UINTN gOutputStringCount;
static UINTN gOutputChars;
static TEST_MODE_SCENARIO gScenario;

/* Deliberately non-conforming console: QueryMode reports a result while
   invalidating its shared Mode structure, as a previous SetMode mock does. */
static EFI_STATUS TestQueryModeSuccess(
  IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, IN UINTN ModeNumber
  ) {
  if (This != NULL && This->Mode != NULL && gQuerySwitchModeOnMode == ModeNumber) {
    This->Mode->Mode = gQuerySwitchModeTo;
  }
  if (gQueryNullModeOnMode == ModeNumber && This != NULL) This->Mode = NULL;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI TestQueryMode(
  IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
  IN UINTN ModeNumber,
  OUT UINTN *Columns,
  OUT UINTN *Rows
  ) {
  gQueryCount++;
  if (Columns == NULL || Rows == NULL) return EFI_INVALID_PARAMETER;
  *Columns = 0;
  *Rows = 0;

  if (gScenario == TestModesHugeOnly && ModeNumber == 0) {
    *Columns = MAX_UINTN;
    *Rows = MAX_UINTN;
    return TestQueryModeSuccess(This, ModeNumber);
  }
  if (gScenario == TestModesCurrentValid && ModeNumber == 0) {
    *Columns = 100;
    *Rows = 30;
    return TestQueryModeSuccess(This, ModeNumber);
  }
  if (gScenario == TestModesCurrentWide) {
    if (ModeNumber == 0) {
      *Columns = 120;
      *Rows = 40;
      return TestQueryModeSuccess(This, ModeNumber);
    }
    if (ModeNumber == 1) {
      *Columns = 100;
      *Rows = 30;
      return TestQueryModeSuccess(This, ModeNumber);
    }
  }
  if (gScenario == TestModesSmallThenValid) {
    if (ModeNumber == 0) {
      *Columns = 60;
      *Rows = 15;
      return TestQueryModeSuccess(This, ModeNumber);
    }
    if (ModeNumber == 1) {
      *Columns = 100;
      *Rows = 30;
      return TestQueryModeSuccess(This, ModeNumber);
    }
  }
  if (gScenario == TestModesTwoValid) {
    if (ModeNumber == 0) {
      *Columns = 80;
      *Rows = 25;
      return TestQueryModeSuccess(This, ModeNumber);
    }
    if (ModeNumber == 1) {
      *Columns = 100;
      *Rows = 30;
      return TestQueryModeSuccess(This, ModeNumber);
    }
  }
  if (gScenario == TestModesHugeThenValid) {
    if (ModeNumber == 0) {
      *Columns = MAX_UINTN;
      *Rows = MAX_UINTN;
      return TestQueryModeSuccess(This, ModeNumber);
    }
    if (ModeNumber == 1) {
      *Columns = 100;
      *Rows = 30;
      return TestQueryModeSuccess(This, ModeNumber);
    }
  }
  return EFI_UNSUPPORTED;
}

static EFI_STATUS EFIAPI TestSetMode(
  IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
  IN UINTN ModeNumber
  ) {
  gSetModeCount++;
  if (This == NULL || This->Mode == NULL || ModeNumber > MAX_INT32) return EFI_INVALID_PARAMETER;
  if (gSetModeStatus != EFI_SUCCESS) {
    if (gSetModeAppliesDespiteError) This->Mode->Mode = (INT32)ModeNumber;
    return gSetModeStatus;
  }
  if (gSetModeNoopOnSuccess) return EFI_SUCCESS;
  if (gSetModeNullModeOnSuccess) { This->Mode = NULL; return EFI_SUCCESS; }
  This->Mode->Mode = (INT32)ModeNumber;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI TestEnableCursor(
  IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
  IN BOOLEAN Visible
  ) {
  if (This == NULL || This->Mode == NULL) return EFI_INVALID_PARAMETER;
  if (gUiOperationCount < sizeof(gUiOperations)) gUiOperations[gUiOperationCount++] = Visible ? 'V' : 'H';
  This->Mode->CursorVisible = Visible;
  if (gEnableCursorNullMode) This->Mode = NULL;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI TestSetAttribute(
  IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
  IN UINTN Attribute
  ) {
  if (This == NULL || This->Mode == NULL || Attribute > MAX_INT32) return EFI_INVALID_PARAMETER;
  gSetAttributeCount++;
  This->Mode->Attribute = (INT32)Attribute;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI TestSetCursorPosition(
  IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
  IN UINTN Column,
  IN UINTN Row
  ) {
  if (This == NULL || This->Mode == NULL || Column > MAX_INT32 || Row > MAX_INT32) return EFI_INVALID_PARAMETER;
  gSetCursorCount++;
  if (gUiOperationCount < sizeof(gUiOperations)) gUiOperations[gUiOperationCount++] = 'P';
  This->Mode->CursorColumn = (INT32)Column;
  This->Mode->CursorRow = (INT32)Row;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI TestOutputString(
  IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
  IN CHAR16 *String
  ) {
  UINTN Length;

  (void)This;
  if (String == NULL) return EFI_INVALID_PARAMETER;
  for (Length = 0U; String[Length] != L'\0'; Length++) {}
  gOutputStringCount++;
  gOutputChars += Length;
  return EFI_SUCCESS;
}

#include "../../BootLoaderSelectorPkg/BootLoaderSelector/Ui.c"

static VOID ResetUiCache(VOID) {
  mBlsUiSizeCached = FALSE;
  mBlsUiCols = 80;
  mBlsUiRows = 25;
  mBlsUiOriginalSaved = FALSE;
  mBlsUiOriginalMode = -1;
  mBlsUiOriginalAttribute = -1;
  mBlsUiOriginalCursorColumn = 0;
  mBlsUiOriginalCursorRow = 0;
  mBlsUiOriginalCursorVisible = TRUE;
  mBlsUiModeStatus = EFI_SUCCESS;
  gQueryNullModeOnMode = MAX_UINTN;
  gQuerySwitchModeOnMode = MAX_UINTN;
  gQuerySwitchModeTo = 0;
  gEnableCursorNullMode = FALSE;
  gSetModeStatus = EFI_SUCCESS;
  gSetModeNoopOnSuccess = FALSE;
  gSetModeAppliesDespiteError = FALSE;
  gSetModeNullModeOnSuccess = FALSE;
  gSetAttributeCount = 0U;
  gSetCursorCount = 0U;
  gUiOperationCount = 0U;
  memset(gUiOperations, 0, sizeof(gUiOperations));
}

int main(void) {
  EFI_SIMPLE_TEXT_OUTPUT_MODE mode;
  EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL conOut;
  EFI_SYSTEM_TABLE systemTable;
  UINTN cols;
  UINTN rows;

  memset(&mode, 0, sizeof(mode));
  memset(&conOut, 0, sizeof(conOut));
  memset(&systemTable, 0, sizeof(systemTable));

  conOut.QueryMode = TestQueryMode;
  conOut.SetMode = TestSetMode;
  conOut.EnableCursor = TestEnableCursor;
  conOut.SetAttribute = TestSetAttribute;
  conOut.SetCursorPosition = TestSetCursorPosition;
  conOut.OutputString = TestOutputString;
  conOut.Mode = &mode;
  systemTable.ConOut = &conOut;
  gST = &systemTable;

  /* Startup state capture must be side-effect free. Remembered autoboot can
     chainload without ever showing selector UI, so capturing the restoration
     snapshot must not query, switch, hide the cursor, or otherwise touch the
     console protocol. */
  gScenario = TestModesCurrentValid;
  mode.MaxMode = 2;
  mode.Mode = 0;
  mode.Attribute = 7;
  mode.CursorColumn = 12;
  mode.CursorRow = 8;
  mode.CursorVisible = TRUE;
  gQueryCount = 0U;
  gSetModeCount = 0U;
  ResetUiCache();
  BlsUiCaptureOriginalState();
  assert(mBlsUiOriginalSaved == TRUE);
  assert(mBlsUiOriginalMode == 0);
  assert(mBlsUiOriginalAttribute == 7);
  assert(mBlsUiOriginalCursorColumn == 12);
  assert(mBlsUiOriginalCursorRow == 8);
  assert(mBlsUiOriginalCursorVisible == TRUE);
  assert(gQueryCount == 0U);
  assert(gSetModeCount == 0U);
  assert(gUiOperationCount == 0U);
  assert(mode.CursorVisible == TRUE);

  /* No-menu remembered autoboot: the selector captured this console but
     never touched it. Restore must therefore be a zero-I/O operation; the
     old implementation redundantly toggles the cursor, queries the mode,
     moves the cursor, and sets the same attribute. */
  BlsUiRestoreOriginalState();
  assert(gSetModeCount == 0U);
  assert(gSetAttributeCount == 0U);
  assert(gSetCursorCount == 0U);
  assert(gQueryCount == 0U);
  assert(gUiOperationCount == 0U);
  assert(mode.CursorVisible == TRUE);

  /* Even when only the color attribute changed, restoring it must not
     unnecessarily blink the cursor or query its unchanged position. */
  mode.Attribute = 3;
  BlsUiRestoreOriginalState();
  assert(mode.Attribute == 7);
  assert(gSetAttributeCount == 1U);
  assert(gSetCursorCount == 0U);
  assert(gQueryCount == 0U);
  assert(gUiOperationCount == 0U);

  /* Hidden cursor state is also a no-op when captured and restored untouched. */
  ResetUiCache();
  mode.CursorVisible = FALSE;
  BlsUiCaptureOriginalState();
  BlsUiRestoreOriginalState();
  assert(gSetAttributeCount == 0U);
  assert(gSetCursorCount == 0U);
  assert(gQueryCount == 0U);
  assert(gUiOperationCount == 0U);
  assert(mode.CursorVisible == FALSE);

  gScenario = TestModesUnsupported;
  mode.MaxMode = MAX_INT32;
  mode.Mode = -1;
  mode.CursorVisible = TRUE;
  gQueryCount = 0;
  gSetModeCount = 0;
  ResetUiCache();
  BlsUiInitReadableMode();
  assert(gQueryCount == 1024U);
  assert(gSetModeCount == 0U);
  assert(mode.CursorVisible == FALSE);
  assert(gUiOperationCount >= 1U && gUiOperations[0] == 'H');

  /* MaxMode is the number of valid mode indices. A malformed firmware Mode
     value outside [0, MaxMode) must never be reflected back into QueryMode(). */
  gScenario = TestModesUnsupported;
  mode.MaxMode = 2;
  mode.Mode = 7;
  mode.CursorVisible = TRUE;
  gQueryCount = 0U;
  gSetModeCount = 0U;
  ResetUiCache();
  BlsUiInitReadableMode();
  BlsUiGetSize(&cols, &rows);
  assert(gQueryCount == 2U);
  assert(gSetModeCount == 0U);
  assert(cols == 80U && rows == 25U);
  assert(mode.CursorVisible == FALSE);

  /* If malformed firmware reports an invalid current Mode but has a usable
     valid mode, select the valid mode and never replay the invalid original
     index through SetMode() during state restoration. */
  gScenario = TestModesCurrentValid;
  mode.MaxMode = 2;
  mode.Mode = 7;
  mode.Attribute = 7;
  mode.CursorColumn = 0;
  mode.CursorRow = 0;
  mode.CursorVisible = TRUE;
  gQueryCount = 0U;
  gSetModeCount = 0U;
  ResetUiCache();
  BlsUiInitReadableMode();
  assert(gQueryCount == 2U);
  assert(gSetModeCount == 1U);
  assert(mode.Mode == 0);
  BlsUiRestoreOriginalState();
  assert(gSetModeCount == 1U);
  assert(mode.Mode == 0);

  gScenario = TestModesCurrentValid;
  mode.MaxMode = 2;
  mode.Mode = 0;
  mode.CursorVisible = TRUE;
  gQueryCount = 0U;
  gSetModeCount = 0U;
  ResetUiCache();
  BlsUiInitReadableMode();
  BlsUiGetSize(&cols, &rows);
  assert(gQueryCount == 1U);
  assert(gSetModeCount == 0U);
  assert(cols == 100U && rows == 30U);
  assert(mode.CursorVisible == FALSE);

  /* REGRESSION: a readable current firmware console (120x40) must not
     switch to 100x30 while opening the menu. That SetMode causes a visible
     jump/flicker and an unnecessary full firmware text-mode reinitialization. */
  gScenario = TestModesCurrentWide;
  mode.MaxMode = 2;
  mode.Mode = 0;
  mode.CursorVisible = TRUE;
  gQueryCount = 0U;
  gSetModeCount = 0U;
  ResetUiCache();
  BlsUiInitReadableMode();
  BlsUiGetSize(&cols, &rows);
  assert(gQueryCount == 1U);
  assert(gSetModeCount == 0U);
  assert(mode.Mode == 0);
  assert(cols == 120U && rows == 40U);

  /* A broken QueryMode() can report the dimensions of mode 0 while
     changing the *active* mode to 1. Do not cache 120x40 for a 100x30
     console: this can visibly misplace menu elements and cause an invalid
     SetCursorPosition. Only the error path may perform an extra query. */
  gScenario = TestModesCurrentWide;
  mode.MaxMode = 2;
  mode.Mode = 0;
  mode.CursorVisible = FALSE;
  gQueryCount = 0U;
  ResetUiCache();
  gQuerySwitchModeOnMode = 0U;
  gQuerySwitchModeTo = 1;
  BlsUiInitReadableMode();
  BlsUiGetSize(&cols, &rows);
  assert(mode.Mode == 1);
  assert(BlsUiLastModeStatus() == EFI_DEVICE_ERROR);
  assert(cols == 100U && rows == 30U);
  assert(gQueryCount == 2U);
  gQuerySwitchModeOnMode = MAX_UINTN;

  /* Cache refresh is also called after returning from a child EFI image.
     A QueryMode side effect must not populate the cache with stale geometry. */
  mode.Mode = 0;
  gQueryCount = 0U;
  ResetUiCache();
  gQuerySwitchModeOnMode = 0U;
  gQuerySwitchModeTo = 1;
  BlsUiGetSize(&cols, &rows);
  assert(mode.Mode == 1);
  assert(cols == 80U && rows == 25U);
  assert(gQueryCount == 1U);
  gQuerySwitchModeOnMode = MAX_UINTN;

  /* QueryMode can also change the active mode during the bounded mode
     scan, after the initial current-mode query succeeded but was unusable. */
  gScenario = TestModesHugeThenValid;
  mode.MaxMode = 2;
  mode.Mode = 0;
  mode.CursorVisible = FALSE;
  gQueryCount = 0U;
  gSetModeCount = 0U;
  ResetUiCache();
  gQuerySwitchModeOnMode = 1U;
  gQuerySwitchModeTo = 1;
  BlsUiInitReadableMode();
  BlsUiGetSize(&cols, &rows);
  assert(mode.Mode == 1);
  assert(BlsUiLastModeStatus() == EFI_DEVICE_ERROR);
  assert(gSetModeCount == 0U);
  assert(cols == 100U && rows == 30U);
  assert(gQueryCount == 3U);
  gQuerySwitchModeOnMode = MAX_UINTN;

  gScenario = TestModesHugeOnly;
  mode.MaxMode = 1;
  mode.Mode = 0;
  gQueryCount = 0;
  gSetModeCount = 0;
  ResetUiCache();
  BlsUiInitReadableMode();
  BlsUiGetSize(&cols, &rows);
  assert(gQueryCount == 1U);
  assert(gSetModeCount == 0U);
  assert(cols == 80U && rows == 25U);
  BlsUiGetSize(&cols, &rows);
  assert(gQueryCount == 1U);

  gScenario = TestModesHugeThenValid;
  mode.MaxMode = 2;
  mode.Mode = 0;
  mode.Attribute = 7;
  mode.CursorColumn = 11;
  mode.CursorRow = 9;
  mode.CursorVisible = TRUE;
  gQueryCount = 0;
  gSetModeCount = 0;
  ResetUiCache();
  BlsUiInitReadableMode();
  BlsUiGetSize(&cols, &rows);
  assert(gQueryCount == 2U);
  assert(gSetModeCount == 1U);
  assert(mode.Mode == 1);
  assert(mode.CursorVisible == FALSE);
  assert(cols == 100U && rows == 30U);

  /* If the preferred SetMode() fails, retain/cache the actual current mode
     rather than assuming the requested geometry became active. */
  gScenario = TestModesHugeThenValid;
  mode.MaxMode = 2;
  mode.Mode = 0;
  gQueryCount = 0U;
  gSetModeCount = 0U;
  ResetUiCache();
  gSetModeStatus = EFI_DEVICE_ERROR;
  BlsUiInitReadableMode();
  BlsUiGetSize(&cols, &rows);
  assert(gQueryCount == 2U);
  assert(gSetModeCount == 1U);
  assert(BlsUiLastModeStatus() == EFI_DEVICE_ERROR);
  assert(mode.Mode == 0);
  assert(cols == 80U && rows == 25U);

  /* A false-success SetMode() reply during menu initialization must never
     advertise the requested geometry when firmware kept the old mode. */
  ResetUiCache();
  conOut.Mode = &mode;
  gScenario = TestModesHugeThenValid;
  mode.MaxMode = 2;
  mode.Mode = 0;
  mode.CursorVisible = TRUE;
  gSetModeNoopOnSuccess = TRUE;
  gSetModeCount = 0U;
  BlsUiInitReadableMode();
  BlsUiGetSize(&cols, &rows);
  assert(gSetModeCount == 1U);
  assert(mode.Mode == 0);
  assert(BlsUiLastModeStatus() == EFI_DEVICE_ERROR);
  assert(cols == 80U && rows == 25U);
  gSetModeNoopOnSuccess = FALSE;

  /* If the old mode was valid but too small, a false SetMode success must
     retain its actual geometry rather than caching the selected larger one. */
  ResetUiCache();
  conOut.Mode = &mode;
  gScenario = TestModesSmallThenValid;
  mode.MaxMode = 2;
  mode.Mode = 0;
  mode.CursorVisible = TRUE;
  gSetModeNoopOnSuccess = TRUE;
  BlsUiInitReadableMode();
  BlsUiGetSize(&cols, &rows);
  assert(BlsUiLastModeStatus() == EFI_DEVICE_ERROR);
  assert(mode.Mode == 0);
  assert(cols == 60U && rows == 15U);
  gSetModeNoopOnSuccess = FALSE;

  /* A firmware may update Mode before returning a non-success code. The
     selector must use the *actual* geometry after that partial transition:
     60x15 -> 100x30, rather than a guessed 80x25, to avoid clipped or
     misplaced menu elements. Preserve the error status for diagnostics. */
  ResetUiCache();
  conOut.Mode = &mode;
  gScenario = TestModesSmallThenValid;
  mode.MaxMode = 2;
  mode.Mode = 0;
  mode.CursorVisible = TRUE;
  gSetModeStatus = EFI_DEVICE_ERROR;
  gSetModeAppliesDespiteError = TRUE;
  BlsUiInitReadableMode();
  BlsUiGetSize(&cols, &rows);
  assert(mode.Mode == 1);
  assert(BlsUiLastModeStatus() == EFI_DEVICE_ERROR);
  assert(cols == 100U && rows == 30U);
  gSetModeStatus = EFI_SUCCESS;
  gSetModeAppliesDespiteError = FALSE;

  /* Firmware can invalidate Mode even after returning EFI_SUCCESS. The
     visible-menu path must survive that as the restoration path already does. */
  ResetUiCache();
  conOut.Mode = &mode;
  gScenario = TestModesHugeThenValid;
  mode.MaxMode = 2;
  mode.Mode = 0;
  mode.CursorVisible = TRUE;
  gSetModeNullModeOnSuccess = TRUE;
  gSetModeCount = 0U;
  BlsUiInitReadableMode();
  BlsUiGetSize(&cols, &rows);
  assert(gSetModeCount == 1U);
  assert(conOut.Mode == NULL);
  assert(BlsUiLastModeStatus() == EFI_DEVICE_ERROR);
  assert(cols == 80U && rows == 25U);
  conOut.Mode = &mode;
  gSetModeNullModeOnSuccess = FALSE;

  /* A partial text-output implementation without QueryMode() must still have
     its caller-visible state snapshotted/restored, and fallback dimensions
     must be cached without repeated firmware queries. */
  ResetUiCache();
  conOut.QueryMode = NULL;
  mode.Mode = 0;
  mode.Attribute = 6;
  mode.CursorColumn = 4;
  mode.CursorRow = 3;
  mode.CursorVisible = TRUE;
  BlsUiInitReadableMode();
  BlsUiGetSize(&cols, &rows);
  assert(cols == 80U && rows == 25U);
  assert(mode.CursorVisible == FALSE);
  mode.Attribute = 1;
  mode.CursorColumn = 0;
  mode.CursorRow = 0;
  mode.CursorVisible = FALSE;
  BlsUiRestoreOriginalState();
  assert(mode.Attribute == 6);
  assert(mode.CursorColumn == 4);
  assert(mode.CursorRow == 3);
  assert(mode.CursorVisible == TRUE);
  conOut.QueryMode = TestQueryMode;

  /* Avoid redundant console protocol calls when attribute/cursor state is
     already exactly what the draw operation requests. */
  ResetUiCache();
  gScenario = TestModesHugeThenValid;
  mode.Mode = 1;
  mode.MaxMode = 2;
  mode.Attribute = BLS_ATTR_MUTED;
  mode.CursorColumn = 2;
  mode.CursorRow = 3;
  mBlsUiSizeCached = TRUE;
  mBlsUiCols = 100U;
  mBlsUiRows = 30U;
  BlsUiSet(2U, 3U, BLS_ATTR_MUTED);
  assert(gSetAttributeCount == 0U);
  assert(gSetCursorCount == 0U);
  BlsUiSet(3U, 3U, BLS_ATTR_ACCENT);
  assert(gSetAttributeCount == 1U);
  assert(gSetCursorCount == 1U);

  /* Empty formatted UI output is a no-op. It should not move the cursor,
     change the color attribute, or call firmware OutputString(""). */
  mode.Attribute = BLS_ATTR_BG;
  mode.CursorColumn = 3;
  mode.CursorRow = 4;
  gSetAttributeCount = 0U;
  gSetCursorCount = 0U;
  gOutputStringCount = 0U;
  BlsUiPrintAt(10U, 11U, BLS_ATTR_ACCENT, L"%s", L"");
  assert(gOutputStringCount == 0U);
  assert(gSetAttributeCount == 0U);
  assert(gSetCursorCount == 0U);
  assert(mode.CursorColumn == 3 && mode.CursorRow == 4);

  /* Ordinary full clears must reuse the known geometry instead of polling
     QueryMode() again. Only the explicit external-image ownership boundary
     invalidates the cache, and the next clear refreshes it exactly once. */
  gScenario = TestModesCurrentValid;
  mode.Mode = 0;
  mode.MaxMode = 2;
  mBlsUiSizeCached = TRUE;
  mBlsUiCols = 100U;
  mBlsUiRows = 30U;
  gQueryCount = 0U;
  BlsUiClear();
  BlsUiClear();
  assert(gQueryCount == 0U);
  BlsUiInvalidateSizeCache();
  BlsUiClear();
  assert(gQueryCount == 1U);
  BlsUiClear();
  assert(gQueryCount == 1U);

  /* The cache refresh path is also reached after StartImage() returns. If a
     child or broken firmware leaves Mode outside [0, MaxMode), never reflect
     that invalid index back into QueryMode(); cache the safe fallback once. */
  mode.Mode = 7;
  mode.MaxMode = 2;
  gQueryCount = 0U;
  BlsUiInvalidateSizeCache();
  BlsUiClear();
  assert(gQueryCount == 0U);
  BlsUiGetSize(&cols, &rows);
  assert(cols == 80U && rows == 25U);
  BlsUiClear();
  assert(gQueryCount == 0U);

  /* A mode captured as valid at application entry can become unavailable
     after a child image changes the console. Revalidate against the current
     MaxMode before restoring, rather than issuing an out-of-range SetMode(). */
  ResetUiCache();
  gScenario = TestModesHugeThenValid;
  mode.MaxMode = 2;
  mode.Mode = 1;
  mode.Attribute = 7;
  mode.CursorColumn = 0;
  mode.CursorRow = 0;
  mode.CursorVisible = TRUE;
  BlsUiInitReadableMode();
  assert(mBlsUiOriginalMode == 1);
  mode.MaxMode = 1;
  mode.Mode = 0;
  gSetModeCount = 0U;
  BlsUiRestoreOriginalState();
  assert(gSetModeCount == 0U);
  assert(mode.Mode == 0);

  /* UEFI requires SetCursorPosition() coordinates to be inside the active
     mode geometry. If a child makes the entry mode unavailable, do not replay
     coordinates that only fit the old, larger mode. */
  ResetUiCache();
  gScenario = TestModesTwoValid;
  mode.MaxMode = 2;
  mode.Mode = 1;
  mode.Attribute = 7;
  mode.CursorColumn = 99;
  mode.CursorRow = 29;
  mode.CursorVisible = TRUE;
  BlsUiInitReadableMode();
  assert(mBlsUiOriginalMode == 1);
  mode.MaxMode = 1;
  mode.Mode = 0;
  mode.CursorColumn = 0;
  mode.CursorRow = 0;
  gSetCursorCount = 0U;
  BlsUiRestoreOriginalState();
  assert(gSetCursorCount == 0U);
  assert(mode.CursorColumn == 0);
  assert(mode.CursorRow == 0);

  /* The same cross-mode restore remains valid when the captured coordinates
     fit inside the currently active geometry. */
  ResetUiCache();
  gScenario = TestModesTwoValid;
  mode.MaxMode = 2;
  mode.Mode = 1;
  mode.Attribute = 7;
  mode.CursorColumn = 12;
  mode.CursorRow = 10;
  mode.CursorVisible = TRUE;
  BlsUiInitReadableMode();
  mode.MaxMode = 1;
  mode.Mode = 0;
  mode.CursorColumn = 0;
  mode.CursorRow = 0;
  gSetCursorCount = 0U;
  BlsUiRestoreOriginalState();
  assert(gSetCursorCount == 1U);
  assert(mode.CursorColumn == 12);
  assert(mode.CursorRow == 10);

  /* REGRESSION: with the original mode still advertised, a firmware
     SetMode() failure must not issue a cursor move in the *other* active
     mode. A cursor location that fits both geometries still belongs to
     the caller's entry mode and may produce a visible jump on failure. */
  ResetUiCache();
  gScenario = TestModesTwoValid;
  mode.MaxMode = 2;
  mode.Mode = 1;
  mode.Attribute = 7;
  mode.CursorColumn = 12;
  mode.CursorRow = 10;
  mode.CursorVisible = TRUE;
  BlsUiCaptureOriginalState();
  mode.Mode = 0;
  mode.CursorColumn = 0;
  mode.CursorRow = 0;
  gSetModeStatus = EFI_DEVICE_ERROR;
  gSetModeCount = 0U;
  gSetCursorCount = 0U;
  BlsUiRestoreOriginalState();
  assert(gSetModeCount == 1U);
  assert(mode.Mode == 0);
  assert(gSetCursorCount == 0U);
  assert(mode.CursorColumn == 0 && mode.CursorRow == 0);
  gSetModeStatus = EFI_SUCCESS;

  /* Broken firmware may claim successful SetMode() without switching modes.
     Treat a post-call mismatch like a failed restoration and avoid relayout. */
  ResetUiCache();
  gScenario = TestModesTwoValid;
  mode.MaxMode = 2;
  mode.Mode = 1;
  mode.CursorColumn = 14;
  mode.CursorRow = 11;
  mode.CursorVisible = TRUE;
  BlsUiCaptureOriginalState();
  mode.Mode = 0;
  mode.CursorColumn = 0;
  mode.CursorRow = 0;
  gSetModeNoopOnSuccess = TRUE;
  gSetModeCount = 0U;
  gSetCursorCount = 0U;
  BlsUiRestoreOriginalState();
  assert(gSetModeCount == 1U);
  assert(mode.Mode == 0);
  assert(gSetCursorCount == 0U);
  assert(mode.CursorColumn == 0 && mode.CursorRow == 0);
  gSetModeNoopOnSuccess = FALSE;

  /* A broken SetMode() callback can even invalidate the published Mode
     pointer. Never dereference it after the callback without rechecking. */
  ResetUiCache();
  conOut.Mode = &mode;
  gScenario = TestModesTwoValid;
  mode.MaxMode = 2;
  mode.Mode = 1;
  mode.CursorColumn = 14;
  mode.CursorRow = 11;
  BlsUiCaptureOriginalState();
  mode.Mode = 0;
  gSetModeNullModeOnSuccess = TRUE;
  BlsUiRestoreOriginalState();
  assert(conOut.Mode == NULL);
  conOut.Mode = &mode;
  gSetModeNullModeOnSuccess = FALSE;

  /* A non-conforming QueryMode() callback may invalidate the console Mode
     pointer. Cursor restore must not dereference it or issue a position move
     using geometry from a no-longer-available Mode structure. */
  ResetUiCache();
  conOut.Mode = &mode;
  gScenario = TestModesTwoValid;
  mode.MaxMode = 2;
  mode.Mode = 1;
  mode.CursorColumn = 12;
  mode.CursorRow = 10;
  mode.CursorVisible = TRUE;
  BlsUiCaptureOriginalState();
  mode.CursorColumn = 0;
  mode.CursorRow = 0;
  gQueryNullModeOnMode = 1U;
  gSetCursorCount = 0U;
  BlsUiRestoreOriginalState();
  assert(conOut.Mode == NULL);
  assert(gSetCursorCount == 0U);
  conOut.Mode = &mode;
  gQueryNullModeOnMode = MAX_UINTN;
  gQuerySwitchModeOnMode = MAX_UINTN;
  gQuerySwitchModeTo = 0;

  /* Same issue during initialization: the current-mode QueryMode() can
     report success yet invalidate Mode. Fail closed and preserve no bad
     geometry assumptions or cursor calls. */
  ResetUiCache();
  conOut.Mode = &mode;
  gScenario = TestModesCurrentValid;
  mode.MaxMode = 1;
  mode.Mode = 0;
  mode.CursorVisible = FALSE;
  gQueryNullModeOnMode = 0U;
  BlsUiInitReadableMode();
  assert(conOut.Mode == NULL);
  assert(BlsUiLastModeStatus() == EFI_DEVICE_ERROR);
  BlsUiGetSize(&cols, &rows);
  assert(cols == 80U && rows == 25U);
  conOut.Mode = &mode;
  gQueryNullModeOnMode = MAX_UINTN;
  gQuerySwitchModeOnMode = MAX_UINTN;
  gQuerySwitchModeTo = 0;

  /* A non-current mode query can likewise invalidate Mode halfway through
     the mode scan. No subsequent SetMode() call is safe in that situation. */
  ResetUiCache();
  conOut.Mode = &mode;
  gScenario = TestModesSmallThenValid;
  mode.MaxMode = 2;
  mode.Mode = 0;
  mode.CursorVisible = FALSE;
  gQueryNullModeOnMode = 1U;
  gSetModeCount = 0U;
  BlsUiInitReadableMode();
  assert(conOut.Mode == NULL);
  assert(BlsUiLastModeStatus() == EFI_DEVICE_ERROR);
  assert(gSetModeCount == 0U);
  conOut.Mode = &mode;
  gQueryNullModeOnMode = MAX_UINTN;
  gQuerySwitchModeOnMode = MAX_UINTN;
  gQuerySwitchModeTo = 0;

  /* The initial cursor-hide callback can also invalidate the console Mode
     pointer on bad firmware. Never touch the freed Mode state while
     initializing the menu after that callback. */
  ResetUiCache();
  conOut.Mode = &mode;
  gScenario = TestModesCurrentValid;
  mode.MaxMode = 1;
  mode.Mode = 0;
  mode.CursorVisible = TRUE;
  gEnableCursorNullMode = TRUE;
  BlsUiInitReadableMode();
  assert(conOut.Mode == NULL);
  assert(BlsUiLastModeStatus() == EFI_DEVICE_ERROR);
  BlsUiGetSize(&cols, &rows);
  assert(cols == 80U && rows == 25U);
  gEnableCursorNullMode = FALSE;
  conOut.Mode = &mode;

  /* Bits 7 and above are reserved by UEFI SIMPLE_TEXT_OUTPUT. Never replay
     a malformed firmware attribute snapshot through SetAttribute(). */
  ResetUiCache();
  gScenario = TestModesCurrentValid;
  mode.MaxMode = 1;
  mode.Mode = 0;
  mode.Attribute = 0x80;
  mode.CursorColumn = 0;
  mode.CursorRow = 0;
  mode.CursorVisible = TRUE;
  BlsUiInitReadableMode();
  assert(mBlsUiOriginalAttribute == -1);
  mode.Attribute = 7;
  gSetAttributeCount = 0U;
  BlsUiRestoreOriginalState();
  assert(gSetAttributeCount == 0U);
  assert(mode.Attribute == 7);

  /* Horizontal separators are emitted in bounded chunks rather than one
     firmware OutputString() call per character. */
  mBlsUiSizeCached = TRUE;
  mBlsUiCols = 100U;
  mBlsUiRows = 30U;
  gOutputStringCount = 0U;
  gOutputChars = 0U;
  BlsUiHLine(0U, 0U, 100U, BLS_ATTR_MUTED, L'-');
  assert(gOutputStringCount == 2U);
  assert(gOutputChars == 100U);

  /* Menu/child activity may alter console state; returning to the caller must
     restore the mode, attributes and cursor snapshot from application entry. */
  ResetUiCache();
  gScenario = TestModesHugeThenValid;
  mode.MaxMode = 2;
  mode.Mode = 0;
  mode.Attribute = 7;
  mode.CursorColumn = 11;
  mode.CursorRow = 9;
  mode.CursorVisible = TRUE;
  BlsUiInitReadableMode();
  assert(mode.Mode == 1);
  mode.Attribute = 1;
  mode.CursorColumn = 0;
  mode.CursorRow = 0;
  mode.CursorVisible = FALSE;
  gUiOperationCount = 0U;
  memset(gUiOperations, 0, sizeof(gUiOperations));
  BlsUiRestoreOriginalState();
  assert(mode.Mode == 0);
  assert(mode.Attribute == 7);
  assert(mode.CursorColumn == 11);
  assert(mode.CursorRow == 9);
  assert(mode.CursorVisible == TRUE);
  assert(gUiOperationCount >= 2U);
  assert(gUiOperations[gUiOperationCount - 2U] == 'P');
  assert(gUiOperations[gUiOperationCount - 1U] == 'V');

  puts("ui_mode_bound: PASS");
  return 0;
}

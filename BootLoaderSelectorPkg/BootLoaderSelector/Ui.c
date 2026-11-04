/** @file
  BootLoaderSelector UI helpers.
  Credits: ©DNMTechLabs / dnmtechlabs@gmail.com
**/
#include "Ui.h"
#include "Utils.h"
#include <Library/BaseMemoryLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>

/* Firmware metadata is untrusted. These bounds are far above practical
   UEFI text consoles while preventing pathological draw/query loops. */
#define BLS_TEXT_MODE_SCAN_MAX 1024U
#define BLS_TEXT_COLUMNS_MAX   1024U
#define BLS_TEXT_ROWS_MAX       512U
#define BLS_TEXT_ATTRIBUTE_MASK  0x7FU

static BOOLEAN mBlsUiSizeCached = FALSE;
static UINTN   mBlsUiCols       = 80;
static UINTN   mBlsUiRows       = 25;
static BOOLEAN mBlsUiOriginalSaved = FALSE;
static INT32   mBlsUiOriginalMode = -1;
static INT32   mBlsUiOriginalAttribute = -1;
static INT32   mBlsUiOriginalCursorColumn = 0;
static INT32   mBlsUiOriginalCursorRow = 0;
static BOOLEAN mBlsUiOriginalCursorVisible = TRUE;
static EFI_STATUS mBlsUiModeStatus = EFI_SUCCESS;

VOID BlsUiCaptureOriginalState(VOID) {
  /* Snapshot only the already-published SIMPLE_TEXT_OUTPUT mode state. Do not
     probe or switch modes here: remembered autoboot may never render selector
     UI, so QueryMode()/SetMode() work belongs on the first visible UI path. */
  if (mBlsUiOriginalSaved || gST == NULL || gST->ConOut == NULL || gST->ConOut->Mode == NULL) return;
  if (gST->ConOut->Mode->Mode >= 0 &&
      gST->ConOut->Mode->MaxMode > 0 &&
      gST->ConOut->Mode->Mode < gST->ConOut->Mode->MaxMode) {
    mBlsUiOriginalMode = gST->ConOut->Mode->Mode;
  } else {
    /* Never replay an out-of-range firmware mode through SetMode() on exit. */
    mBlsUiOriginalMode = -1;
  }
  if (gST->ConOut->Mode->Attribute >= 0 &&
      (UINT32)gST->ConOut->Mode->Attribute <= BLS_TEXT_ATTRIBUTE_MASK) {
    mBlsUiOriginalAttribute = gST->ConOut->Mode->Attribute;
  } else {
    /* UEFI SIMPLE_TEXT_OUTPUT reserves every attribute bit above bit 6.
       Do not replay malformed firmware state through SetAttribute(). */
    mBlsUiOriginalAttribute = -1;
  }
  mBlsUiOriginalCursorColumn = gST->ConOut->Mode->CursorColumn;
  mBlsUiOriginalCursorRow = gST->ConOut->Mode->CursorRow;
  mBlsUiOriginalCursorVisible = gST->ConOut->Mode->CursorVisible;
  mBlsUiOriginalSaved = TRUE;
}


EFI_STATUS BlsUiLastModeStatus(VOID) {
  return mBlsUiModeStatus;
}

static BOOLEAN BlsUiDimensionsAreUsable(IN UINTN Cols, IN UINTN Rows) {
  return (BOOLEAN)(Cols > 0 && Rows > 0 &&
                   Cols <= BLS_TEXT_COLUMNS_MAX && Rows <= BLS_TEXT_ROWS_MAX);
}

static BOOLEAN BlsUiCanRestoreCursorPosition(IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut) {
  UINTN Cols;
  UINTN Rows;
  INT32 CurrentMode;

  if (ConOut == NULL || ConOut->Mode == NULL ||
      mBlsUiOriginalCursorColumn < 0 || mBlsUiOriginalCursorRow < 0) {
    return FALSE;
  }

  CurrentMode = ConOut->Mode->Mode;
  if (CurrentMode < 0 || ConOut->Mode->MaxMode <= 0 ||
      CurrentMode >= ConOut->Mode->MaxMode) {
    return FALSE;
  }

  /* A partial console without QueryMode() cannot prove the geometry. Preserve
     the old behavior only when the exact entry mode is still active; moving a
     cursor snapshot across modes without dimensions is not spec-safe. */
  if (ConOut->QueryMode == NULL) {
    return (BOOLEAN)(mBlsUiOriginalMode >= 0 && CurrentMode == mBlsUiOriginalMode);
  }

  Cols = 0;
  Rows = 0;
  if (ConOut->QueryMode(ConOut, (UINTN)CurrentMode, &Cols, &Rows) != EFI_SUCCESS ||
      Cols == 0 || Rows == 0) {
    return FALSE;
  }
  /* QueryMode() normally leaves Mode intact. If broken firmware invalidates
     or changes it, the just-reported dimensions no longer authorize a move.
     The caller would otherwise dereference Mode after this callback. */
  if (ConOut->Mode == NULL || ConOut->Mode->Mode != CurrentMode ||
      ConOut->Mode->MaxMode <= CurrentMode) return FALSE;

  return (BOOLEAN)((UINTN)mBlsUiOriginalCursorColumn < Cols &&
                   (UINTN)mBlsUiOriginalCursorRow < Rows);
}

VOID BlsUiRestoreOriginalState(VOID) {
  EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;
  BOOLEAN ModeRestoreFailed;
  if (!mBlsUiOriginalSaved || gST == NULL || gST->ConOut == NULL) return;
  ConOut = gST->ConOut;
  ModeRestoreFailed = FALSE;
  /* The remembered fast-boot path may capture the caller's text state and
     never display any selector UI. If the child also returns without touching
     the console, restoring the identical snapshot must not issue redundant
     SetAttribute()/SetCursorPosition()/EnableCursor()/QueryMode() calls.
     In particular a hide/show cursor pair on that path causes a visible
     blink on some firmware without restoring any actual state. */
  if (ConOut->Mode != NULL && mBlsUiOriginalMode >= 0 &&
      ConOut->Mode->Mode == mBlsUiOriginalMode &&
      mBlsUiOriginalAttribute >= 0 &&
      ConOut->Mode->Attribute == mBlsUiOriginalAttribute &&
      ConOut->Mode->CursorColumn == mBlsUiOriginalCursorColumn &&
      ConOut->Mode->CursorRow == mBlsUiOriginalCursorRow &&
      ConOut->Mode->CursorVisible == mBlsUiOriginalCursorVisible) {
    mBlsUiSizeCached = FALSE;
    return;
  }
  if (mBlsUiOriginalMode >= 0 && ConOut->Mode != NULL &&
      ConOut->Mode->MaxMode > 0 &&
      mBlsUiOriginalMode < ConOut->Mode->MaxMode &&
      ConOut->Mode->Mode != mBlsUiOriginalMode && ConOut->SetMode != NULL) {
    /* A firmware error (or a false successful reply) means the entry mode
       was not restored. Never move the cursor based on that failed change;
       leave the actual mode/position undisturbed instead of causing a jump. */
    EFI_STATUS ModeStatus;
    ModeStatus = ConOut->SetMode(ConOut, (UINTN)mBlsUiOriginalMode);
    if (ModeStatus != EFI_SUCCESS || ConOut->Mode == NULL ||
        ConOut->Mode->Mode != mBlsUiOriginalMode) {
      ModeRestoreFailed = TRUE;
    }
  }
  if (ConOut->SetAttribute != NULL && mBlsUiOriginalAttribute >= 0 &&
      (UINT32)mBlsUiOriginalAttribute <= BLS_TEXT_ATTRIBUTE_MASK &&
      (ConOut->Mode == NULL || ConOut->Mode->Attribute != mBlsUiOriginalAttribute)) {
    (VOID)ConOut->SetAttribute(ConOut, (UINTN)mBlsUiOriginalAttribute);
  }

  /* Only query geometry/move the cursor when the captured coordinates differ.
     A no-op move costs a firmware query and can briefly show a transient cursor
     in a different location. Keep the cursor hidden while a real move runs. */
  if (!ModeRestoreFailed && ConOut->SetCursorPosition != NULL && ConOut->Mode != NULL &&
      (ConOut->Mode->CursorColumn != mBlsUiOriginalCursorColumn ||
       ConOut->Mode->CursorRow != mBlsUiOriginalCursorRow) &&
      BlsUiCanRestoreCursorPosition(ConOut)) {
    if (ConOut->EnableCursor != NULL && ConOut->Mode->CursorVisible) {
      (VOID)ConOut->EnableCursor(ConOut, FALSE);
    }
    (VOID)ConOut->SetCursorPosition(ConOut, (UINTN)mBlsUiOriginalCursorColumn, (UINTN)mBlsUiOriginalCursorRow);
  }
  if (ConOut->EnableCursor != NULL &&
      (ConOut->Mode == NULL || ConOut->Mode->CursorVisible != mBlsUiOriginalCursorVisible)) {
    (VOID)ConOut->EnableCursor(ConOut, mBlsUiOriginalCursorVisible);
  }
  mBlsUiSizeCached = FALSE;
}

static VOID BlsUiCacheSize(IN UINTN Cols, IN UINTN Rows) {
  if (!BlsUiDimensionsAreUsable(Cols, Rows)) {
    Cols = 80;
    Rows = 25;
  }
  mBlsUiCols = Cols;
  mBlsUiRows = Rows;
  mBlsUiSizeCached = TRUE;
}

static VOID BlsUiCacheCurrentSize(VOID) {
  UINTN TmpC;
  UINTN TmpR;
  INT32 QueriedMode;
  EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;

  TmpC = 0;
  TmpR = 0;
  ConOut = (gST != NULL) ? gST->ConOut : NULL;
  if (ConOut != NULL && ConOut->Mode != NULL && ConOut->QueryMode != NULL &&
      ConOut->Mode->Mode >= 0 && ConOut->Mode->MaxMode > 0 &&
      ConOut->Mode->Mode < ConOut->Mode->MaxMode) {
    QueriedMode = ConOut->Mode->Mode;
    if (ConOut->QueryMode(ConOut, (UINTN)QueriedMode, &TmpC, &TmpR) == EFI_SUCCESS &&
        /* Malformed callbacks can silently change the active mode. Dimensions
           returned for the previous mode must never become the live UI cache. */
        ConOut->Mode != NULL && ConOut->Mode->Mode == QueriedMode &&
        ConOut->Mode->MaxMode > QueriedMode &&
        BlsUiDimensionsAreUsable(TmpC, TmpR)) {
      BlsUiCacheSize(TmpC, TmpR);
      return;
    }
  }

  /* Cache the conservative fallback too. Re-querying a broken firmware mode
     on every draw operation can turn a recoverable QueryMode() defect into
     visible lag and excessive firmware I/O. */
  BlsUiCacheSize(80, 25);
}

VOID BlsUiInitReadableMode(VOID) {
  UINTN Mode;
  UINTN BestMode;
  UINTN BestCols;
  UINTN BestRows;
  UINTN FallbackMode;
  UINTN FallbackCols;
  UINTN FallbackRows;
  UINTN MaxMode;
  UINTN CurrentMode;
  INT32 InitialActiveMode;
  UINTN CurrentCols;
  UINTN CurrentRows;
  UINTN Cols;
  UINTN Rows;
  UINTN BestScore;
  UINTN FallbackScore;
  BOOLEAN CurrentModeValid;
  BOOLEAN CurrentQuerySucceeded;
  BOOLEAN CurrentDimensionsUsable;
  BOOLEAN BestModeFound;
  BOOLEAN FallbackModeFound;

  if (gST == NULL || gST->ConOut == NULL || gST->ConOut->Mode == NULL) return;

  /* Save the caller's console state before depending on QueryMode(). Some
     firmware exposes a usable Mode structure but an incomplete QueryMode()
     implementation; such firmware must still get its attribute/cursor state
     restored on application exit. */
  BlsUiCaptureOriginalState();
  mBlsUiModeStatus = EFI_SUCCESS;
  mBlsUiSizeCached = FALSE;

  /* Hide the cursor before any potentially slow firmware mode probing. This
     also covers partial consoles without QueryMode(); if SetMode later makes
     the cursor visible again, the post-switch check below hides it once more. */
  if (gST->ConOut->EnableCursor != NULL && gST->ConOut->Mode->CursorVisible) {
    (VOID)gST->ConOut->EnableCursor(gST->ConOut, FALSE);
    if (gST->ConOut->Mode == NULL) {
      /* Firmware callbacks may invalidate the console state. Never probe
         QueryMode or the current mode through an invalidated pointer. */
      mBlsUiModeStatus = EFI_DEVICE_ERROR;
      BlsUiCacheSize(80, 25);
      return;
    }
  }

  if (gST->ConOut->QueryMode == NULL) {
    BlsUiCacheSize(80, 25);
    return;
  }

  /* SIMPLE_TEXT_OUTPUT_MODE.MaxMode is the number of mode numbers accepted by
     QueryMode()/SetMode(). Firmware that publishes a non-negative current Mode
     outside that range is internally inconsistent; do not feed that invalid
     index back into QueryMode(), because broken console implementations are
     exactly where an out-of-range firmware call is least safe. */
  InitialActiveMode = gST->ConOut->Mode->Mode;
  CurrentModeValid = (BOOLEAN)(gST->ConOut->Mode->Mode >= 0 &&
                               gST->ConOut->Mode->MaxMode > 0 &&
                               gST->ConOut->Mode->Mode < gST->ConOut->Mode->MaxMode);
  CurrentMode = CurrentModeValid ? (UINTN)gST->ConOut->Mode->Mode : 0;
  CurrentCols = 0;
  CurrentRows = 0;
  CurrentQuerySucceeded = FALSE;
  CurrentDimensionsUsable = FALSE;
  if (CurrentModeValid) {
    CurrentQuerySucceeded = (BOOLEAN)(gST->ConOut->QueryMode(
                              gST->ConOut, CurrentMode, &CurrentCols, &CurrentRows
                              ) == EFI_SUCCESS);
    if (gST->ConOut->Mode == NULL) {
      /* A non-conforming QueryMode callback destroyed the published state.
         Fail closed rather than dereferencing Mode->CursorVisible below. */
      mBlsUiModeStatus = EFI_DEVICE_ERROR;
      BlsUiCacheSize(80, 25);
      return;
    }
    if (gST->ConOut->Mode->Mode != InitialActiveMode) {
      /* QueryMode is a query, not a mode switch. If faulty firmware changes
         the active mode anyway, the returned geometry belongs to the old
         mode. Recheck the actual mode once and stop probing this firmware. */
      mBlsUiModeStatus = EFI_DEVICE_ERROR;
      BlsUiCacheCurrentSize();
      return;
    }
  }
  if (CurrentQuerySucceeded) {
    if (BlsUiDimensionsAreUsable(CurrentCols, CurrentRows)) {
      CurrentDimensionsUsable = TRUE;
      /* A readable current mode must remain active: switching from e.g.
         120x40 to a preferred 100x30 unnecessarily resizes the firmware
         console on first draw and causes a visible flash/reflow. Only switch
         when the current geometry cannot accommodate the minimum UI. The
         existing BlsUiDimensionsAreUsable() bounds still reject hostile sizes. */
      if (CurrentCols >= 80 && CurrentRows >= 20) {
        if (gST->ConOut->EnableCursor != NULL && gST->ConOut->Mode->CursorVisible) {
          (VOID)gST->ConOut->EnableCursor(gST->ConOut, FALSE);
        }
        BlsUiCacheSize(CurrentCols, CurrentRows);
        return;
      }
    }
  }

  BestMode = 0;
  BestCols = 0;
  BestRows = 0;
  FallbackMode = 0;
  FallbackCols = 0;
  FallbackRows = 0;
  BestModeFound = FALSE;
  FallbackModeFound = FALSE;
  BestScore = (UINTN)-1;
  FallbackScore = (UINTN)-1;
  MaxMode = (gST->ConOut->Mode->MaxMode > 0) ? (UINTN)gST->ConOut->Mode->MaxMode : 0;
  if (MaxMode > BLS_TEXT_MODE_SCAN_MAX) MaxMode = BLS_TEXT_MODE_SCAN_MAX;

  /* Track the preferred readable mode and the fallback in one bounded pass.
     The previous two-pass search could issue 2,048 QueryMode() calls on a
     malformed MaxMode value before the selector displayed anything. */
  for (Mode = 0; Mode < MaxMode; Mode++) {
    UINTN Score;

    /* The current mode was already queried above. Reuse those dimensions
       instead of issuing the same firmware call a second time. */
    if (CurrentModeValid && Mode == CurrentMode) {
      if (!CurrentQuerySucceeded) continue;
      Cols = CurrentCols;
      Rows = CurrentRows;
      if (!BlsUiDimensionsAreUsable(Cols, Rows)) continue;
    } else {
      Cols = 0;
      Rows = 0;
      EFI_STATUS QueryStatus;
      QueryStatus = gST->ConOut->QueryMode(gST->ConOut, Mode, &Cols, &Rows);
      if (gST->ConOut->Mode == NULL) {
        mBlsUiModeStatus = EFI_DEVICE_ERROR;
        BlsUiCacheSize(80, 25);
        return;
      }
      if (gST->ConOut->Mode->Mode != InitialActiveMode) {
        mBlsUiModeStatus = EFI_DEVICE_ERROR;
        BlsUiCacheCurrentSize();
        return;
      }
      if (QueryStatus != EFI_SUCCESS) continue;
      if (!BlsUiDimensionsAreUsable(Cols, Rows)) continue;
    }
    if (Cols < 80 || Rows < 20) continue;

    if (Rows <= (MAX_UINTN - Cols) / 4) {
      Score = Cols + Rows * 4;
      if (Score < FallbackScore) {
        FallbackScore = Score;
        FallbackMode = Mode;
        FallbackCols = Cols;
        FallbackRows = Rows;
        FallbackModeFound = TRUE;
      }
    }

    if (Cols <= 100 && Rows <= 35) {
      Score = (100 - Cols) + (((Rows > 30) ? (Rows - 30) : (30 - Rows)) * 4);
      if (Score < BestScore) {
        BestScore = Score;
        BestMode = Mode;
        BestCols = Cols;
        BestRows = Rows;
        BestModeFound = TRUE;
      }
    }
  }

  if (!BestModeFound && FallbackModeFound) {
    BestMode = FallbackMode;
    BestCols = FallbackCols;
    BestRows = FallbackRows;
    BestModeFound = TRUE;
  }

  if (BestModeFound && (!CurrentModeValid || BestMode != CurrentMode)) {
    if (gST->ConOut->SetMode != NULL) {
      mBlsUiModeStatus = gST->ConOut->SetMode(gST->ConOut, BestMode);
      /* Do not trust a success code until firmware publishes the requested
         mode. Otherwise menu geometry can be cached for an inactive console;
         a broken callback may also invalidate the Mode pointer entirely. */
      if (mBlsUiModeStatus == EFI_SUCCESS &&
          (gST->ConOut->Mode == NULL ||
           gST->ConOut->Mode->Mode != (INT32)BestMode)) {
        mBlsUiModeStatus = EFI_DEVICE_ERROR;
      }
      if (mBlsUiModeStatus == EFI_SUCCESS) {
        BlsUiCacheSize(BestCols, BestRows);
      } else if (CurrentDimensionsUsable && gST->ConOut->Mode != NULL &&
                 gST->ConOut->Mode->Mode == (INT32)CurrentMode) {
        BlsUiCacheSize(CurrentCols, CurrentRows);
      } else if (gST->ConOut->Mode != NULL &&
                 gST->ConOut->Mode->Mode >= 0 &&
                 gST->ConOut->Mode->MaxMode > 0 &&
                 gST->ConOut->Mode->Mode < gST->ConOut->Mode->MaxMode &&
                 (!CurrentModeValid ||
                  gST->ConOut->Mode->Mode != (INT32)CurrentMode)) {
        /* Some firmware changes the text mode before reporting a failure.
           A post-call Mode change invalidates pre-switch dimensions; query
           the actual active geometry once rather than guessing 80x25 and
           drawing the selector at positions that no longer match the screen. */
        BlsUiCacheCurrentSize();
      } else {
        BlsUiCacheSize(80, 25);
      }
    } else {
      mBlsUiModeStatus = EFI_UNSUPPORTED;
      if (CurrentDimensionsUsable) BlsUiCacheSize(CurrentCols, CurrentRows);
      else BlsUiCacheSize(80, 25);
    }
  } else if (BestModeFound) {
    BlsUiCacheSize(BestCols, BestRows);
  } else if (CurrentDimensionsUsable) {
    BlsUiCacheSize(CurrentCols, CurrentRows);
  } else {
    BlsUiCacheSize(80, 25);
  }

  if (gST->ConOut->Mode != NULL && gST->ConOut->EnableCursor != NULL &&
      gST->ConOut->Mode->CursorVisible) {
    (VOID)gST->ConOut->EnableCursor(gST->ConOut, FALSE);
  }
}

VOID BlsUiGetSize(OUT UINTN *Cols, OUT UINTN *Rows) {
  if (!mBlsUiSizeCached) BlsUiCacheCurrentSize();
  if (Cols != NULL) *Cols = mBlsUiCols;
  if (Rows != NULL) *Rows = mBlsUiRows;
}

VOID BlsUiInvalidateSizeCache(VOID) {
  /* Only external EFI images are expected to change the active text mode
     behind the selector. Keep ordinary clears/redraws on the cached geometry
     and explicitly invalidate at that ownership boundary instead of polling
     QueryMode() on every ClearScreen(). */
  mBlsUiSizeCached = FALSE;
}

VOID BlsUiClear(VOID) {
  EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;

  if (gST == NULL || gST->ConOut == NULL) return;
  ConOut = gST->ConOut;
  /* Geometry is cached across ordinary redraws. The caller invalidates it
     explicitly after an external EFI image returns and may have changed the
     console mode. This avoids repeated firmware QueryMode() I/O here. */
  if (!mBlsUiSizeCached) BlsUiCacheCurrentSize();
  if (ConOut->SetAttribute != NULL &&
      (ConOut->Mode == NULL || ConOut->Mode->Attribute != BLS_ATTR_BG)) {
    (VOID)ConOut->SetAttribute(ConOut, BLS_ATTR_BG);
  }
  if (ConOut->ClearScreen != NULL) (VOID)ConOut->ClearScreen(ConOut);
  if (ConOut->EnableCursor != NULL &&
      (ConOut->Mode == NULL || ConOut->Mode->CursorVisible)) {
    (VOID)ConOut->EnableCursor(ConOut, FALSE);
  }
  if (ConOut->SetCursorPosition != NULL &&
      (ConOut->Mode == NULL || ConOut->Mode->CursorColumn != 0 || ConOut->Mode->CursorRow != 0)) {
    (VOID)ConOut->SetCursorPosition(ConOut, 0, 0);
  }
}

VOID BlsUiSet(IN UINTN Col, IN UINTN Row, IN UINTN Attr) {
  UINTN Cols;
  UINTN Rows;

  if (gST == NULL || gST->ConOut == NULL || gST->ConOut->SetAttribute == NULL ||
      gST->ConOut->SetCursorPosition == NULL) return;
  BlsUiGetSize(&Cols, &Rows);
  if (Col >= Cols || Row >= Rows) return;
  if (gST->ConOut->Mode == NULL || gST->ConOut->Mode->Attribute != (INT32)Attr) {
    (VOID)gST->ConOut->SetAttribute(gST->ConOut, Attr);
  }
  if (gST->ConOut->Mode == NULL ||
      gST->ConOut->Mode->CursorColumn != (INT32)Col ||
      gST->ConOut->Mode->CursorRow != (INT32)Row) {
    (VOID)gST->ConOut->SetCursorPosition(gST->ConOut, Col, Row);
  }
}

VOID BlsUiPrintAt(IN UINTN Col, IN UINTN Row, IN UINTN Attr, IN CONST CHAR16 *Format, ...) {
  VA_LIST Marker;
  CHAR16 Buffer[256];
  UINTN Cols;
  UINTN Rows;
  UINTN MaxChars;
  UINTN Len;

  if (Format == NULL || gST == NULL || gST->ConOut == NULL ||
      gST->ConOut->OutputString == NULL || gST->ConOut->SetAttribute == NULL ||
      gST->ConOut->SetCursorPosition == NULL) return;
  BlsUiGetSize(&Cols, &Rows);
  if (Col >= Cols || Row >= Rows) return;

  VA_START(Marker, Format);
  UnicodeVSPrint(Buffer, sizeof(Buffer), Format, Marker);
  VA_END(Marker);
  /* An empty format result has no pixels to render. Avoid changing the
     cursor/attribute state and issuing an empty firmware OutputString(). */
  if (Buffer[0] == L'\0') return;

  /*
    Several firmware text consoles scroll the whole screen when a print reaches
    the lower-right cell. Keep that cell untouched to avoid visual jumps.
  */
  if (Row + 1 >= Rows) {
    if (Col + 1 >= Cols) return;
    MaxChars = Cols - Col - 1;
  } else {
    MaxChars = Cols - Col;
  }
  if (MaxChars == 0) return;
  if (MaxChars >= ARRAY_SIZE(Buffer)) MaxChars = ARRAY_SIZE(Buffer) - 1;
  Len = BlsStrLen16(Buffer);
  if (Len > MaxChars) Buffer[MaxChars] = L'\0';

  BlsUiSet(Col, Row, Attr);
  (VOID)gST->ConOut->OutputString(gST->ConOut, Buffer);
}

VOID BlsUiHLine(IN UINTN Col, IN UINTN Row, IN UINTN Width, IN UINTN Attr, IN CHAR16 Ch) {
  CHAR16 Chunk[65];
  UINTN ChunkChars;
  UINTN Remaining;
  UINTN I;
  UINTN Cols;
  UINTN Rows;

  if (Width == 0 || gST == NULL || gST->ConOut == NULL ||
      gST->ConOut->OutputString == NULL || gST->ConOut->SetAttribute == NULL ||
      gST->ConOut->SetCursorPosition == NULL) return;
  BlsUiGetSize(&Cols, &Rows);
  if (Col >= Cols || Row >= Rows) return;
  if (Row + 1 >= Rows) {
    if (Col + 1 >= Cols) return;
    if (Width > Cols - Col - 1) Width = Cols - Col - 1;
  } else if (Width > Cols - Col) {
    Width = Cols - Col;
  }
  if (Width == 0) return;

  for (I = 0; I + 1U < ARRAY_SIZE(Chunk); I++) Chunk[I] = Ch;
  Chunk[ARRAY_SIZE(Chunk) - 1U] = L'\0';

  BlsUiSet(Col, Row, Attr);
  Remaining = Width;
  while (Remaining > 0) {
    ChunkChars = (Remaining < ARRAY_SIZE(Chunk) - 1U) ? Remaining : ARRAY_SIZE(Chunk) - 1U;
    Chunk[ChunkChars] = L'\0';
    (VOID)gST->ConOut->OutputString(gST->ConOut, Chunk);
    Chunk[ChunkChars] = Ch;
    Remaining -= ChunkChars;
  }
}

VOID BlsUiCentered(IN UINTN Row, IN UINTN Attr, IN CONST CHAR16 *Text) {
  UINTN Cols;
  UINTN Rows;
  UINTN Len;
  UINTN Col;
  if (Text == NULL) return;
  BlsUiGetSize(&Cols, &Rows);
  Len = BlsStrLen16(Text);
  Col = (Cols > Len) ? (Cols - Len) / 2 : 0;
  if (Row < Rows) BlsUiPrintAt(Col, Row, Attr, L"%s", Text);
}

VOID BlsUiBox(IN UINTN Col, IN UINTN Row, IN UINTN Width, IN UINTN Height, IN UINTN Attr) {
  UINTN R;
  UINTN Inner;
  UINTN Cols;
  UINTN Rows;

  BlsUiGetSize(&Cols, &Rows);
  if (Width < 2 || Height < 2 || Col >= Cols || Row >= Rows) return;
  if (Width > Cols - Col) Width = Cols - Col;
  if (Height > Rows - Row) Height = Rows - Row;
  if (Width < 2 || Height < 2) return;
  Inner = Width - 2;
  BlsUiPrintAt(Col, Row, Attr, L"+");
  BlsUiHLine(Col + 1, Row, Inner, Attr, L'-');
  BlsUiPrintAt(Col + Width - 1, Row, Attr, L"+");
  for (R = 1; R + 1 < Height; R++) {
    BlsUiPrintAt(Col, Row + R, Attr, L"|");
    BlsUiPrintAt(Col + Width - 1, Row + R, Attr, L"|");
  }
  BlsUiPrintAt(Col, Row + Height - 1, Attr, L"+");
  BlsUiHLine(Col + 1, Row + Height - 1, Inner, Attr, L'-');
  BlsUiPrintAt(Col + Width - 1, Row + Height - 1, Attr, L"+");
}

/** @file
  BootLoaderSelector menu.
  Credits: ©DNMTechLabs / dnmtechlabs@gmail.com
**/
#include "Menu.h"
#include "Screenshot.h"
#include "Ui.h"
#include "Utils.h"
#include <Library/BaseMemoryLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

#define BLS_OPTIONS_INDEX(List) ((List) != NULL ? (List)->Count : 0)
#define BLS_TIMEOUT_MAX        30
#define BLS_OPTION_COUNT       4
#define BLS_SPURIOUS_KEY_SIGNAL_MAX 64U

static VOID BlsPrintFixedRow(IN UINTN Col, IN UINTN Row, IN UINTN Width, IN UINTN Attr, IN CONST CHAR16 *Format, ...);

static BOOLEAN BlsInputKeyIsEmpty(IN CONST EFI_INPUT_KEY *Key) {
  if (Key == NULL) return TRUE;
  return (BOOLEAN)(Key->ScanCode == SCAN_NULL && Key->UnicodeChar == CHAR_NULL);
}

static UINTN BlsFooterRow(VOID) {
  UINTN Cols;
  UINTN Rows;

  BlsUiGetSize(&Cols, &Rows);
  (VOID)Cols;
  if (Rows > 5) return Rows - 3;
  return (Rows > 0) ? Rows - 1 : 0;
}

static UINTN BlsFooterLineRow(VOID) {
  UINTN FooterRow;

  FooterRow = BlsFooterRow();
  return (FooterRow > 0) ? FooterRow - 1 : 0;
}

static UINTN BlsDefaultIndex(IN CONST BLS_CONFIG *Config, IN CONST BLS_BOOT_LIST *List) {
  UINTN Index;
  if (Config == NULL || List == NULL || List->Count == 0) return 0;
  for (Index = 0; Index < List->Count; Index++) {
    if (BlsConfigMatchesEntry(Config, &List->Entries[Index])) return Index;
  }
  return 0;
}

static UINTN BlsSelectableCount(IN CONST BLS_BOOT_LIST *List) {
  return ((List != NULL) ? List->Count : 0) + 1;
}

static BOOLEAN BlsSelectionIsOptions(IN CONST BLS_BOOT_LIST *List, IN UINTN Selected) {
  return (Selected == BLS_OPTIONS_INDEX(List));
}

static UINTN BlsCountdownCol(IN UINTN Cols) {
  return (Cols > 28) ? Cols - 28 : 0;
}

static UINTN BlsCountdownWidth(IN UINTN Cols) {
  return (Cols > 28) ? 27 : Cols;
}

static VOID BlsClearCountdownStatus(IN UINTN Cols) {
  BlsPrintFixedRow(BlsCountdownCol(Cols), 1, BlsCountdownWidth(Cols), BLS_ATTR_BG, L"");
}

static VOID BlsDrawCountdownStatus(IN UINTN Cols, IN UINTN Remaining) {
  BlsPrintFixedRow(BlsCountdownCol(Cols), 1, BlsCountdownWidth(Cols), BLS_ATTR_MUTED, L"Boot in %u sec", (UINT32)Remaining);
}

static VOID BlsDrawHardwareField(IN UINTN Col, IN UINTN Row, IN UINTN Width, IN CONST CHAR16 *Label, IN CONST CHAR16 *Value) {
  CHAR16 Line[256];
  UINTN I;
  UINTN Pos;
  UINTN MaxValue;
  UINTN ValueLength;
  UINTN CopyLength;
  UINTN EllipsisPos;
  BOOLEAN Truncated;

  if (Width == 0 || Label == NULL) return;
  if (Width >= ARRAY_SIZE(Line)) Width = ARRAY_SIZE(Line) - 1;

  for (I = 0; I < Width; I++) Line[I] = L' ';
  Line[Width] = L'\0';

  for (I = 0; I < 6 && Label[I] != L'\0' && I < Width; I++) {
    Line[I] = Label[I];
  }

  if (Width > 6) Line[6] = L' ';
  if (Width > 7) Line[7] = L':';
  if (Width > 8) Line[8] = L' ';

  Pos = 9;
  if (Value != NULL && Width > Pos) {
    MaxValue = Width - Pos;
    ValueLength = BlsStrLen16Bounded(Value, BLS_MAX_TEXT);
    CopyLength = (ValueLength < MaxValue) ? ValueLength : MaxValue;
    for (I = 0; I < CopyLength; I++) {
      Line[Pos + I] = Value[I];
    }

    /* Hardware strings are contractually BLS_MAX_TEXT. Fail closed if a
       firmware-derived field lost its terminator instead of probing beyond
       the owning structure while rendering on a wide console. */
    Truncated = (BOOLEAN)(ValueLength == BLS_MAX_TEXT || ValueLength > MaxValue);
    if (Truncated && CopyLength >= 3U) {
      EllipsisPos = Pos + CopyLength - 3U;
      Line[EllipsisPos] = L'.';
      Line[EllipsisPos + 1U] = L'.';
      Line[EllipsisPos + 2U] = L'.';
    }
  }

  BlsUiPrintAt(Col, Row, BLS_ATTR_BG, L"%s", Line);
}

static EFI_STATUS BlsCancelCountdown(
  IN OUT EFI_EVENT *TimerEvent,
  IN OUT EFI_EVENT *DeadlineEvent,
  IN OUT BOOLEAN *CountdownActive,
  IN UINTN Cols
  ) {
  BOOLEAN WasActive;
  EFI_STATUS Status;
  EFI_STATUS DeadlineStatus;

  WasActive = (BOOLEAN)((CountdownActive != NULL && *CountdownActive) ||
                        (TimerEvent != NULL && *TimerEvent != NULL) ||
                        (DeadlineEvent != NULL && *DeadlineEvent != NULL));
  if (CountdownActive != NULL) *CountdownActive = FALSE;
  Status = (TimerEvent != NULL) ? BlsCloseTimerEvent(TimerEvent) : EFI_INVALID_PARAMETER;
  DeadlineStatus = (DeadlineEvent != NULL) ? BlsCloseTimerEvent(DeadlineEvent) : EFI_INVALID_PARAMETER;
  if (Status == EFI_SUCCESS && DeadlineStatus != EFI_SUCCESS) Status = DeadlineStatus;
  if (WasActive) BlsClearCountdownStatus(Cols);
  return Status;
}

static VOID BlsDrawHeader(IN CONST BLS_HARDWARE_INFO *Hw) {
  UINTN Cols;
  UINTN Rows;
  UINTN BoxW;
  UINTN BoxX;
  UINTN FieldW;
  CHAR16 MemoryText[BLS_MAX_TEXT];
  CHAR16 CpuText[BLS_MAX_TEXT];
  UINT64 MemGB;
  UINT64 MemRest;

  (VOID)Rows;
  BlsUiGetSize(&Cols, &Rows);
  BoxW = (Cols >= 78) ? 74 : (Cols > 8 ? Cols - 6 : Cols);
  BoxX = (Cols > BoxW) ? (Cols - BoxW) / 2 : 0;
  FieldW = (BoxW > 6) ? BoxW - 6 : BoxW;

  BlsUiCentered(1, BLS_ATTR_TITLE, L"BootLoaderSelector");
  BlsUiCentered(2, BLS_ATTR_MUTED, L"OpenCore / Clover UEFI Selector");
  BlsUiHLine(BoxX, 4, BoxW, BLS_ATTR_ACCENT, L'=');

  if (Hw == NULL) return;

  BlsUiBox(BoxX, 6, BoxW, 7, BLS_ATTR_MUTED);
  BlsUiPrintAt(BoxX + 3, 6, BLS_ATTR_ACCENT, L" Hardware ");
  MemoryText[0] = L'\0';
  if (Hw->MemoryMB >= 1024) {
    MemGB = Hw->MemoryMB / 1024;
    MemRest = Hw->MemoryMB % 1024;
    if (Hw->MemoryType[0] != L'\0' && Hw->MemoryMHz > 0) {
      UnicodeSPrint(
        MemoryText,
        sizeof(MemoryText),
        L"%Lu GB %s / %u %s",
        MemGB + (MemRest >= 512 ? 1 : 0),
        Hw->MemoryType,
        Hw->MemoryMHz,
        Hw->MemorySpeedIsMtps ? L"MT/s" : L"MHz"
        );
    } else if (Hw->MemoryType[0] != L'\0') {
      UnicodeSPrint(MemoryText, sizeof(MemoryText), L"%Lu GB %s", MemGB + (MemRest >= 512 ? 1 : 0), Hw->MemoryType);
    } else {
      UnicodeSPrint(MemoryText, sizeof(MemoryText), L"%Lu GB", MemGB + (MemRest >= 512 ? 1 : 0));
    }
  } else if (Hw->MemoryMB > 0) {
    UnicodeSPrint(MemoryText, sizeof(MemoryText), L"%Lu MB", Hw->MemoryMB);
  } else {
    UnicodeSPrint(MemoryText, sizeof(MemoryText), L"Unknown");
  }

  CpuText[0] = L'\0';
  if (Hw->Cores > 0 && Hw->LogicalProcessors > 0 && Hw->CpuMHz > 0) {
    UnicodeSPrint(CpuText, sizeof(CpuText), L"%s / %uC %uT / %u MHz",
                  Hw->Cpu, Hw->Cores, Hw->LogicalProcessors, Hw->CpuMHz);
  } else if (Hw->Cores > 0 && Hw->LogicalProcessors > 0) {
    UnicodeSPrint(CpuText, sizeof(CpuText), L"%s / %uC %uT", Hw->Cpu, Hw->Cores, Hw->LogicalProcessors);
  } else {
    BlsSafeStrCpy(CpuText, ARRAY_SIZE(CpuText), Hw->Cpu);
  }
  if (Hw->MemoryEstimated && MemoryText[0] != L'\0') {
    CHAR16 Estimated[BLS_MAX_TEXT];
    UnicodeSPrint(Estimated, sizeof(Estimated), L"~%s (firmware map)", MemoryText);
    BlsSafeStrCpy(MemoryText, ARRAY_SIZE(MemoryText), Estimated);
  }

  BlsDrawHardwareField(BoxX + 3, 8, FieldW, L"CPU", CpuText);
  BlsDrawHardwareField(BoxX + 3, 9, FieldW, L"GPU", Hw->Gpu);
  BlsDrawHardwareField(BoxX + 3, 10, FieldW, L"BOARD", Hw->Board);
  BlsDrawHardwareField(BoxX + 3, 11, FieldW, L"MEMORY", MemoryText);
}

static UINTN BlsMenuStartRow(IN BOOLEAN HardwareVisible) {
  UINTN Rows;

  if (HardwareVisible) return 15;
  Rows = 0;
  BlsUiGetSize(NULL, &Rows);
  if (Rows > 0 && Rows < 8) return 0;
  if (Rows < 10) return 1;
  if (Rows < 12) return 3;
  return (Rows < 16) ? 5 : 7;
}

static CONST BLS_HARDWARE_INFO *BlsSelectDisplayHardware(
  IN CONST BLS_HARDWARE_INFO *Hw,
  IN CONST BLS_CONFIG *Config
  ) {
  UINTN Cols;
  UINTN Rows;

  if (Config == NULL) return NULL;
  if (Hw == NULL || !Config->ShowHardwareInfo) return NULL;
  BlsUiGetSize(&Cols, &Rows);
  (VOID)Cols;

  /* The hardware card uses rows 6-12. Below 21 rows it would leave no
     visible boot entry plus the Options row, so use the compact layout. */
  if (Rows < 21) return NULL;
  return Hw;
}

typedef struct {
  UINTN X;
  UINTN W;
  UINTN Row;
  UINTN FooterLine;
  UINTN OptionsRow;
  UINTN FirstVisible;
  UINTN VisibleCount;
} BLS_ENTRY_VIEW;

static VOID BlsGetEntryView(
  IN CONST BLS_BOOT_LIST *List,
  IN UINTN Selected,
  IN BOOLEAN HardwareVisible,
  OUT BLS_ENTRY_VIEW *View
  ) {
  UINTN Cols;
  UINTN Rows;
  UINTN EntrySlots;

  if (View == NULL) return;
  ZeroMem(View, sizeof(*View));
  BlsUiGetSize(&Cols, &Rows);
  View->FooterLine = BlsFooterLineRow();
  View->W = (Cols >= 78) ? 74 : (Cols > 8 ? Cols - 6 : Cols);
  View->X = (Cols > View->W) ? (Cols - View->W) / 2 : 0;
  View->Row = BlsMenuStartRow(HardwareVisible);
  View->OptionsRow = View->FooterLine;

  if (View->FooterLine <= View->Row) return;

  if (List == NULL || List->Count == 0) {
    View->OptionsRow = View->Row + 4;
    if (View->OptionsRow >= View->FooterLine) View->OptionsRow = View->FooterLine - 1;
    return;
  }

  View->OptionsRow = View->Row + List->Count + 1;
  if (View->OptionsRow < View->FooterLine) {
    View->FirstVisible = 0;
    View->VisibleCount = List->Count;
    return;
  }

  View->OptionsRow = View->FooterLine - 1;
  EntrySlots = (View->OptionsRow > View->Row) ? View->OptionsRow - View->Row : 0;
  if (EntrySlots > List->Count) EntrySlots = List->Count;

  View->FirstVisible = 0;
  if (EntrySlots > 0 && List->Count > EntrySlots) {
    if (Selected < List->Count) {
      if (Selected >= EntrySlots) View->FirstVisible = Selected - EntrySlots + 1;
    } else {
      View->FirstVisible = List->Count - EntrySlots;
    }
    if (View->FirstVisible + EntrySlots > List->Count) {
      View->FirstVisible = List->Count - EntrySlots;
    }
  }
  View->VisibleCount = EntrySlots;
  (VOID)Rows;
}

static BOOLEAN BlsDrawSelectableRow(
  IN CONST BLS_BOOT_LIST *List,
  IN CONST BLS_ENTRY_VIEW *View,
  IN UINTN RowSelection,
  IN UINTN ActiveSelection
  ) {
  UINTN Attr;
  CHAR16 Marker;

  if (View == NULL || View->FooterLine <= View->Row) return FALSE;

  if (List != NULL && RowSelection < List->Count &&
      RowSelection >= View->FirstVisible &&
      RowSelection - View->FirstVisible < View->VisibleCount) {
    Attr = (RowSelection == ActiveSelection) ?
             (List->Entries[RowSelection].IsUsb ? BLS_ATTR_USB_SELECT : BLS_ATTR_SELECT) :
             (List->Entries[RowSelection].IsUsb ? BLS_ATTR_WARN : BLS_ATTR_BG);
    Marker = (RowSelection == ActiveSelection) ? L'>' : L' ';
    BlsPrintFixedRow(View->X, View->Row + (RowSelection - View->FirstVisible), View->W, Attr,
                     L"%c %-10s %.18s %s",
                     Marker,
                     List->Entries[RowSelection].Name,
                     List->Entries[RowSelection].VolumeLabel,
                     List->Entries[RowSelection].Path);
    return TRUE;
  }

  if (RowSelection == BLS_OPTIONS_INDEX(List) && View->OptionsRow < View->FooterLine) {
    Attr = (RowSelection == ActiveSelection) ? BLS_ATTR_SELECT : BLS_ATTR_BG;
    Marker = (RowSelection == ActiveSelection) ? L'>' : L' ';
    BlsPrintFixedRow(View->X, View->OptionsRow, View->W, Attr,
                     L"%c Options     Timeout / default boot settings", Marker);
    return TRUE;
  }

  return FALSE;
}

static BOOLEAN BlsDrawSelectionDelta(
  IN CONST BLS_BOOT_LIST *List,
  IN UINTN OldSelected,
  IN UINTN NewSelected,
  IN BOOLEAN HardwareVisible
  ) {
  BLS_ENTRY_VIEW OldView;
  BLS_ENTRY_VIEW NewView;
  UINTN Cols;
  UINTN Rows;

  if (OldSelected == NewSelected) return TRUE;
  BlsUiGetSize(&Cols, &Rows);
  (VOID)Cols;
  /* Tiny consoles already use heavily compacted layouts. Keep the original
     full redraw path there rather than risking stale text at layout edges. */
  if (Rows < 8) return FALSE;

  BlsGetEntryView(List, OldSelected, HardwareVisible, &OldView);
  BlsGetEntryView(List, NewSelected, HardwareVisible, &NewView);
  if (OldView.X != NewView.X || OldView.W != NewView.W || OldView.Row != NewView.Row ||
      OldView.FooterLine != NewView.FooterLine || OldView.OptionsRow != NewView.OptionsRow ||
      OldView.FirstVisible != NewView.FirstVisible || OldView.VisibleCount != NewView.VisibleCount) {
    return FALSE;
  }

  if (!BlsDrawSelectableRow(List, &NewView, OldSelected, NewSelected)) return FALSE;
  if (!BlsDrawSelectableRow(List, &NewView, NewSelected, NewSelected)) return FALSE;
  return TRUE;
}

static VOID BlsDrawEntries(IN CONST BLS_BOOT_LIST *List, IN UINTN Selected, IN BOOLEAN HardwareVisible) {
  BLS_ENTRY_VIEW View;
  UINTN Index;
  UINTN ClearRow;

  BlsGetEntryView(List, Selected, HardwareVisible, &View);

  if (View.Row >= 2) {
    if (List != NULL && List->Truncated) {
      BlsPrintFixedRow(View.X, View.Row - 2, View.W, BLS_ATTR_ACCENT, L"Available Bootloaders (showing %u of %u)",
                       (UINT32)List->Count, (UINT32)List->TotalFound);
    } else {
      BlsPrintFixedRow(View.X, View.Row - 2, View.W, BLS_ATTR_ACCENT, L"Available Bootloaders");
    }
    BlsUiHLine(View.X, View.Row - 1, View.W, BLS_ATTR_MUTED, L'-');
  } else if (View.Row == 1) {
    BlsPrintFixedRow(View.X, 0, View.W, BLS_ATTR_ACCENT, L"Available Bootloaders");
  }

  if (View.FooterLine <= View.Row) return;

  for (ClearRow = View.Row; ClearRow < View.FooterLine; ClearRow++) {
    BlsPrintFixedRow(View.X, ClearRow, View.W, BLS_ATTR_BG, L"");
  }

  if (List == NULL || List->Count == 0) {
    BlsPrintFixedRow(View.X, View.Row, View.W, BLS_ATTR_WARN, L"No OpenCore or Clover loader was found.");
    if (View.Row + 1 < View.OptionsRow) {
      BlsPrintFixedRow(View.X, View.Row + 1, View.W, BLS_ATTR_MUTED, L"Expected: \\EFI\\OC\\OpenCore.efi or \\EFI\\CLOVER\\CLOVERX64.efi");
    }
    if (View.Row + 2 < View.OptionsRow) {
      BlsPrintFixedRow(View.X, View.Row + 2, View.W, BLS_ATTR_MUTED, L"Use F5 after copying or mounting an EFI volume.");
    }
  } else {
    if (View.VisibleCount < List->Count && View.VisibleCount > 0 && View.Row > 0) {
      BlsPrintFixedRow(View.X, (View.Row >= 2) ? View.Row - 2 : 0, View.W, BLS_ATTR_ACCENT,
                       L"Available Bootloaders (%u-%u of %u)",
                       (UINT32)(View.FirstVisible + 1),
                       (UINT32)(View.FirstVisible + View.VisibleCount),
                       (UINT32)(List->Truncated ? List->TotalFound : List->Count));
    }

    for (Index = 0; Index < View.VisibleCount; Index++) {
      (VOID)BlsDrawSelectableRow(List, &View, View.FirstVisible + Index, Selected);
    }
  }

  (VOID)BlsDrawSelectableRow(List, &View, BLS_OPTIONS_INDEX(List), Selected);
}

static VOID BlsDrawFooter(VOID) {
  UINTN Cols;
  UINTN Rows;
  UINTN FooterRow;
  UINTN LineRow;
  UINTN VersionLen;
  UINTN HelpWidth;
  CONST CHAR16 *HelpText;
  BlsUiGetSize(&Cols, &Rows);
  if (Rows < 6) return;
  FooterRow = BlsFooterRow();
  LineRow = BlsFooterLineRow();
  BlsUiHLine(0, LineRow, (Cols > 0) ? Cols - 1 : Cols, BLS_ATTR_MUTED, L'-');

  VersionLen = BlsStrLen16(BLS_VERSION_TEXT);
  if (Cols >= 116U) {
    HelpText = L"F1: Help   Space: Details   Enter/Right: Boot/Open   Esc/Left: Exit/Back   Up/Down: Select   F5: Refresh   F10: PrintScreen";
  } else if (Cols >= 96U) {
    HelpText = L"F1: Help   Space: Details   Enter: Boot   Esc: Exit   Up/Down: Select   F5: Refresh   F10: PrintScreen";
  } else {
    HelpText = L"F1: Help   Enter: Boot   Esc: Exit   Up/Down: Select   F5: Refresh";
  }

  if (Cols > VersionLen + 3) {
    HelpWidth = Cols - VersionLen - 3;
    BlsPrintFixedRow(0, FooterRow, HelpWidth, BLS_ATTR_MUTED, L"%s", HelpText);
    BlsUiPrintAt(Cols - VersionLen - 1, FooterRow, BLS_ATTR_MUTED, L"%s", BLS_VERSION_TEXT);
  } else {
    BlsUiCentered(FooterRow, BLS_ATTR_MUTED, BLS_VERSION_TEXT);
  }
}

static VOID BlsDrawScreen(IN CONST BLS_HARDWARE_INFO *Hw, IN CONST BLS_BOOT_LIST *List, IN UINTN Selected, IN BOOLEAN FullRedraw) {
  if (FullRedraw) BlsUiClear();
  BlsDrawHeader(Hw);
  BlsDrawEntries(List, Selected, Hw != NULL);
  BlsDrawFooter();
}

static UINTN BlsPanelWidth(IN UINTN Cols, IN UINTN Preferred, IN UINTN Minimum) {
  UINTN Width;
  if (Cols <= 8) return Cols;
  Width = Preferred;
  if (Width > Cols - 8) Width = Cols - 8;
  if (Width < Minimum && Cols > 4) Width = Cols - 4;
  if (Width > Cols) Width = Cols;
  return Width;
}

static VOID BlsTextEllipsis(IN OUT CHAR16 *Text, IN UINTN MaxChars) {
  UINTN Len;
  if (Text == NULL || MaxChars == 0) return;
  Len = BlsStrLen16(Text);
  if (Len <= MaxChars) return;
  if (MaxChars <= 3) {
    Text[MaxChars] = L'\0';
    return;
  }
  Text[MaxChars - 3] = L'.';
  Text[MaxChars - 2] = L'.';
  Text[MaxChars - 1] = L'.';
  Text[MaxChars] = L'\0';
}

static VOID BlsPrintFixedRow(IN UINTN Col, IN UINTN Row, IN UINTN Width, IN UINTN Attr, IN CONST CHAR16 *Format, ...) {
  VA_LIST Marker;
  CHAR16 Text[256];
  CHAR16 Line[256];
  UINTN I;
  UINTN CopyLen;

  if (Width == 0 || Format == NULL) return;
  if (Width >= ARRAY_SIZE(Line)) Width = ARRAY_SIZE(Line) - 1;

  Text[0] = L'\0';
  VA_START(Marker, Format);
  UnicodeVSPrint(Text, sizeof(Text), Format, Marker);
  VA_END(Marker);
  BlsTextEllipsis(Text, Width);

  for (I = 0; I < Width; I++) Line[I] = L' ';
  Line[Width] = L'\0';
  CopyLen = BlsStrLen16(Text);
  if (CopyLen > Width) CopyLen = Width;
  for (I = 0; I < CopyLen; I++) Line[I] = Text[I];

  BlsUiPrintAt(Col, Row, Attr, L"%s", Line);
}


static EFI_STATUS BlsWaitForModalDismiss(VOID) {
  EFI_EVENT Event;
  EFI_INPUT_KEY Key;
  EFI_STATUS Status;
  UINTN EventIndex;
  UINTN SpuriousSignals;

  if (gBS == NULL || gBS->WaitForEvent == NULL ||
      gST == NULL || gST->ConIn == NULL || gST->ConIn->WaitForKey == NULL ||
      gST->ConIn->ReadKeyStroke == NULL) {
    return EFI_UNSUPPORTED;
  }

  Event = gST->ConIn->WaitForKey;
  SpuriousSignals = 0U;
  while (TRUE) {
    EventIndex = MAX_UINTN;
    Status = gBS->WaitForEvent(1U, &Event, &EventIndex);
    if (Status != EFI_SUCCESS) return Status;
    if (EventIndex != 0U) return EFI_DEVICE_ERROR;

    ZeroMem(&Key, sizeof(Key));
    Status = gST->ConIn->ReadKeyStroke(gST->ConIn, &Key);
    if (Status == EFI_NOT_READY || (Status == EFI_SUCCESS && BlsInputKeyIsEmpty(&Key))) {
      SpuriousSignals++;
      if (SpuriousSignals >= BLS_SPURIOUS_KEY_SIGNAL_MAX) return EFI_DEVICE_ERROR;
      continue;
    }
    return Status;
  }
}

static UINTN BlsDrawWrappedField(
  IN UINTN X,
  IN UINTN Row,
  IN UINTN MaxRow,
  IN UINTN Width,
  IN UINTN Attr,
  IN CONST CHAR16 *Label,
  IN CONST CHAR16 *Value
  ) {
  CHAR16 Line[256];
  UINTN LabelLen;
  UINTN ValueLen;
  UINTN ValueOffset;
  BOOLEAN FirstLine;

  if (Width == 0U || Row >= MaxRow || Label == NULL) return Row;
  if (Width >= ARRAY_SIZE(Line)) Width = ARRAY_SIZE(Line) - 1U;
  LabelLen = BlsStrLen16Bounded(Label, ARRAY_SIZE(Line) - 1U);
  ValueLen = (Value != NULL) ? BlsStrLen16Bounded(Value, BLS_MAX_VOLUME_ID) : 0U;
  ValueOffset = 0U;
  FirstLine = TRUE;

  do {
    UINTN PrefixLen;
    UINTN Capacity;
    UINTN CopyChars;
    UINTN Pos;
    UINTN Index;

    if (Row >= MaxRow) break;
    ZeroMem(Line, sizeof(Line));
    Pos = 0U;
    PrefixLen = 0U;

    if (FirstLine) {
      for (Index = 0U; Index < LabelLen && Pos + 1U < ARRAY_SIZE(Line); Index++) {
        Line[Pos++] = Label[Index];
      }
      if (Pos + 2U < ARRAY_SIZE(Line)) {
        Line[Pos++] = L':';
        Line[Pos++] = L' ';
      }
      PrefixLen = Pos;
    } else if (Width > 2U) {
      Line[Pos++] = L' ';
      Line[Pos++] = L' ';
      PrefixLen = Pos;
    }

    Capacity = (Width > PrefixLen) ? Width - PrefixLen : 0U;
    if (ValueLen == 0U) {
      CONST CHAR16 EmptyText[] = L"(none)";
      UINTN EmptyLen;
      EmptyLen = ARRAY_SIZE(EmptyText) - 1U;
      CopyChars = (EmptyLen < Capacity) ? EmptyLen : Capacity;
      for (Index = 0U; Index < CopyChars; Index++) Line[Pos++] = EmptyText[Index];
    } else {
      CopyChars = ValueLen - ValueOffset;
      if (CopyChars > Capacity) CopyChars = Capacity;
      for (Index = 0U; Index < CopyChars; Index++) {
        Line[Pos++] = Value[ValueOffset + Index];
      }
      ValueOffset += CopyChars;
    }
    Line[Pos] = L'\0';
    BlsPrintFixedRow(X, Row, Width, Attr, L"%s", Line);
    Row++;
    FirstLine = FALSE;

    if (Capacity == 0U) break;
  } while (ValueLen > 0U && ValueOffset < ValueLen);

  return Row;
}

static VOID BlsDrawHelpScreen(VOID) {
  static CONST CHAR16 *HelpLines[] = {
    L"Enter / Right Arrow  Boot the selected OpenCore or Clover entry",
    L"Space                Show full details for the selected boot entry",
    L"Up / Down            Move selection; lists scroll on compact consoles",
    L"F5                   Rescan EFI filesystem volumes",
    L"F10                  Save a screenshot when GOP is available",
    L"Esc / Left Arrow     Exit or return to the previous screen",
    L"Options              Timeout, remembered loader, hardware card and debug log",
    L"Yellow entries       Loader was discovered on a USB device path",
    L"Debug log            \\EFI\\BOOT\\BootLoaderSelector.log",
    L"Configuration        \\EFI\\BOOT\\BLS.plist"
  };
  UINTN Cols;
  UINTN Rows;
  UINTN Width;
  UINTN X;
  UINTN Row;
  UINTN MaxRow;
  UINTN Index;

  BlsUiClear();
  BlsUiGetSize(&Cols, &Rows);
  Width = (Cols > 8U) ? Cols - 8U : Cols;
  if (Width > 100U) Width = 100U;
  X = (Cols > Width) ? (Cols - Width) / 2U : 0U;
  MaxRow = (Rows > 3U) ? Rows - 3U : Rows;

  if (Rows > 0U) BlsUiCentered(0U, BLS_ATTR_TITLE, L"BootLoaderSelector Help");
  Row = (Rows > 4U) ? 2U : 1U;
  for (Index = 0U; Index < ARRAY_SIZE(HelpLines) && Row < MaxRow; Index++, Row++) {
    BlsPrintFixedRow(X, Row, Width, (Index < 6U) ? BLS_ATTR_BG : BLS_ATTR_MUTED,
                     L"%s", HelpLines[Index]);
  }
  if (Rows >= 3U) {
    BlsUiHLine(0U, BlsFooterLineRow(), (Cols > 0U) ? Cols - 1U : Cols, BLS_ATTR_MUTED, L'-');
    BlsUiCentered(BlsFooterRow(), BLS_ATTR_MUTED, L"Press any key to return");
  }
}

static VOID BlsDrawEntryDetails(
  IN CONST BLS_BOOT_ENTRY *Entry,
  IN UINTN EntryIndex,
  IN UINTN EntryCount
  ) {
  UINTN Cols;
  UINTN Rows;
  UINTN Width;
  UINTN X;
  UINTN Row;
  UINTN MaxRow;
  CONST CHAR16 *TypeText;
  CHAR16 Position[48];

  if (Entry == NULL) return;
  BlsUiClear();
  BlsUiGetSize(&Cols, &Rows);
  Width = (Cols > 8U) ? Cols - 8U : Cols;
  if (Width > 100U) Width = 100U;
  X = (Cols > Width) ? (Cols - Width) / 2U : 0U;
  MaxRow = (Rows > 3U) ? Rows - 3U : Rows;

  TypeText = (Entry->Type == BlsLoaderOpenCore) ? L"OpenCore" :
             ((Entry->Type == BlsLoaderClover) ? L"Clover" : L"Other EFI loader");
  UnicodeSPrint(Position, sizeof(Position), L"Entry %u of %u", (UINT32)(EntryIndex + 1U), (UINT32)EntryCount);

  if (Rows > 0U) BlsUiCentered(0U, BLS_ATTR_TITLE, L"Boot Entry Details");
  Row = (Rows > 4U) ? 2U : 1U;
  Row = BlsDrawWrappedField(X, Row, MaxRow, Width, BLS_ATTR_MUTED, L"Position", Position);
  Row = BlsDrawWrappedField(X, Row, MaxRow, Width, BLS_ATTR_BG, L"Loader", Entry->Name);
  Row = BlsDrawWrappedField(X, Row, MaxRow, Width, BLS_ATTR_BG, L"Type", TypeText);
  Row = BlsDrawWrappedField(X, Row, MaxRow, Width, BLS_ATTR_BG, L"Volume", Entry->VolumeLabel);
  Row = BlsDrawWrappedField(X, Row, MaxRow, Width, BLS_ATTR_BG, L"Path", Entry->Path);
  Row = BlsDrawWrappedField(X, Row, MaxRow, Width, BLS_ATTR_MUTED, L"Volume identity", Entry->VolumeDevicePath);
  (VOID)BlsDrawWrappedField(X, Row, MaxRow, Width,
                            Entry->IsUsb ? BLS_ATTR_WARN : BLS_ATTR_MUTED,
                            L"Transport", Entry->IsUsb ? L"USB" : L"Non-USB / firmware path");

  if (Rows >= 3U) {
    BlsUiHLine(0U, BlsFooterLineRow(), (Cols > 0U) ? Cols - 1U : Cols, BLS_ATTR_MUTED, L'-');
    BlsUiCentered(BlsFooterRow(), BLS_ATTR_MUTED, L"Press any key to return");
  }
}

typedef struct {
  UINTN Cols;
  UINTN Rows;
  UINTN FooterRow;
  UINTN LineRow;
  BOOLEAN Compact;
  UINTN RowX;
  UINTN RowWidth;
  UINTN FirstOptionRow;
  UINTN BoxW;
  UINTN BoxX;
  UINTN BoxY;
  UINTN InnerW;
  UINTN TextX;
} BLS_OPTIONS_LAYOUT;

static VOID BlsGetOptionsLayout(OUT BLS_OPTIONS_LAYOUT *Layout) {
  if (Layout == NULL) return;
  ZeroMem(Layout, sizeof(*Layout));
  BlsUiGetSize(&Layout->Cols, &Layout->Rows);
  Layout->FooterRow = (Layout->Rows > 5U) ? Layout->Rows - 3U :
                      ((Layout->Rows > 0U) ? Layout->Rows - 1U : 0U);
  Layout->LineRow = (Layout->FooterRow > 0U) ? Layout->FooterRow - 1U : 0U;
  Layout->Compact = (BOOLEAN)(Layout->Rows < 19U);

  if (Layout->Compact) {
    Layout->FirstOptionRow = (Layout->Rows >= 10U) ? 1U : 0U;
    Layout->RowWidth = (Layout->Cols > 4U) ? Layout->Cols - 4U : Layout->Cols;
    Layout->RowX = (Layout->Cols > Layout->RowWidth) ? (Layout->Cols - Layout->RowWidth) / 2U : 0U;
    return;
  }

  Layout->BoxW = BlsPanelWidth(Layout->Cols, 72U, 54U);
  Layout->BoxX = (Layout->Cols > Layout->BoxW) ? (Layout->Cols - Layout->BoxW) / 2U : 0U;
  Layout->BoxY = (Layout->Rows > 18U) ? ((Layout->Rows - 12U) / 2U) : 3U;
  if (Layout->BoxY < 4U) Layout->BoxY = 4U;
  if (Layout->Rows > 14U && Layout->BoxY + 12U >= Layout->LineRow) {
    Layout->BoxY = (Layout->LineRow >= 13U) ? Layout->LineRow - 13U : 1U;
  }
  Layout->InnerW = (Layout->BoxW > 6U) ? Layout->BoxW - 6U : Layout->BoxW;
  Layout->TextX = Layout->BoxX + 3U;
  Layout->RowX = Layout->TextX;
  Layout->RowWidth = Layout->InnerW;
  Layout->FirstOptionRow = Layout->BoxY + 2U;
}

static VOID BlsDrawOptionRow(
  IN CONST BLS_CONFIG *Config,
  IN UINTN SelectedOption,
  IN UINTN OptionIndex,
  IN CONST BLS_OPTIONS_LAYOUT *Layout
  ) {
  UINTN Attr;
  CHAR16 Marker;
  UINTN Row;

  if (Layout == NULL || OptionIndex >= BLS_OPTION_COUNT || Layout->RowWidth == 0U) return;
  Attr = (OptionIndex == SelectedOption) ? BLS_ATTR_SELECT : BLS_ATTR_BG;
  Marker = (OptionIndex == SelectedOption) ? L'>' : L' ';
  Row = Layout->FirstOptionRow + OptionIndex;

  switch (OptionIndex) {
    case 0U:
      BlsPrintFixedRow(Layout->RowX, Row, Layout->RowWidth, Attr,
                       L"%c TIMEOUT  : %u seconds", Marker,
                       (UINT32)((Config != NULL) ? Config->Timeout : 10U));
      break;
    case 1U:
      BlsPrintFixedRow(Layout->RowX, Row, Layout->RowWidth, Attr,
                       L"%c REMEMBER : %s", Marker,
                       (Config == NULL || Config->RememberLastSelected) ? L"Enabled" : L"Disabled");
      break;
    case 2U:
      BlsPrintFixedRow(Layout->RowX, Row, Layout->RowWidth, Attr,
                       L"%c HARDWARE : %s", Marker,
                       (Config == NULL || Config->ShowHardwareInfo) ? L"Enabled" : L"Disabled");
      break;
    default:
      BlsPrintFixedRow(Layout->RowX, Row, Layout->RowWidth, Attr,
                       L"%c DEBUG    : %s", Marker,
                       (Config != NULL && Config->Debug) ? L"Enabled" : L"Disabled");
      break;
  }
}

static VOID BlsDrawOptionsSelectionDelta(
  IN CONST BLS_CONFIG *Config,
  IN UINTN OldSelectedOption,
  IN UINTN NewSelectedOption
  ) {
  BLS_OPTIONS_LAYOUT Layout;

  if (OldSelectedOption >= BLS_OPTION_COUNT || NewSelectedOption >= BLS_OPTION_COUNT) return;
  BlsGetOptionsLayout(&Layout);
  if (OldSelectedOption == NewSelectedOption) {
    BlsDrawOptionRow(Config, NewSelectedOption, NewSelectedOption, &Layout);
    return;
  }
  BlsDrawOptionRow(Config, NewSelectedOption, OldSelectedOption, &Layout);
  BlsDrawOptionRow(Config, NewSelectedOption, NewSelectedOption, &Layout);
}

static VOID BlsDrawOptionValueDelta(IN CONST BLS_CONFIG *Config, IN UINTN SelectedOption) {
  BLS_OPTIONS_LAYOUT Layout;

  if (SelectedOption >= BLS_OPTION_COUNT) return;
  BlsGetOptionsLayout(&Layout);
  BlsDrawOptionRow(Config, SelectedOption, SelectedOption, &Layout);
}

static VOID BlsDrawOptionsScreen(IN CONST BLS_CONFIG *Config, IN UINTN SelectedOption, IN BOOLEAN FullRedraw) {
  BLS_OPTIONS_LAYOUT Layout;
  UINTN OptionIndex;

  if (FullRedraw) BlsUiClear();
  BlsGetOptionsLayout(&Layout);

  /* On short text consoles the full 13-row box would overlap the footer and
     hide selectable values. Keep every option visible using the space that
     the firmware actually reports. */
  if (Layout.Compact) {
    if (Layout.FirstOptionRow > 0U) {
      BlsUiCentered(0U, BLS_ATTR_TITLE, L"BootLoaderSelector Options");
    }
    for (OptionIndex = 0U; OptionIndex < BLS_OPTION_COUNT; OptionIndex++) {
      BlsDrawOptionRow(Config, SelectedOption, OptionIndex, &Layout);
    }
    if (Layout.LineRow > Layout.FirstOptionRow + 3U) {
      BlsUiHLine(0U, Layout.LineRow, (Layout.Cols > 0U) ? Layout.Cols - 1U : Layout.Cols,
                 BLS_ATTR_MUTED, L'-');
      BlsUiCentered(Layout.FooterRow, BLS_ATTR_MUTED,
                    L"Up/Down: Select   Left/Right: Change   Enter: Save   Esc: Cancel");
    }
    return;
  }

  BlsUiCentered(1U, BLS_ATTR_TITLE, L"BootLoaderSelector Options");
  if (Layout.Rows > 22U) {
    BlsUiCentered(2U, BLS_ATTR_MUTED,
                  L"Up/Down: Select   Left/Right: Change   Enter: Save   Esc: Cancel   F10: PrintScreen");
  }

  BlsUiBox(Layout.BoxX, Layout.BoxY, Layout.BoxW, 13U, BLS_ATTR_MUTED);
  BlsUiPrintAt(Layout.BoxX + 3U, Layout.BoxY, BLS_ATTR_ACCENT, L" Options ");

  for (OptionIndex = 0U; OptionIndex < BLS_OPTION_COUNT; OptionIndex++) {
    BlsDrawOptionRow(Config, SelectedOption, OptionIndex, &Layout);
  }

  BlsPrintFixedRow(Layout.TextX, Layout.BoxY + 7U, Layout.InnerW, BLS_ATTR_MUTED,
                   L"Remember enabled boots saved loader silently.");
  BlsPrintFixedRow(Layout.TextX, Layout.BoxY + 8U, Layout.InnerW, BLS_ATTR_MUTED,
                   L"Hardware off skips SMBIOS/PCI info for faster menu.");
  BlsPrintFixedRow(Layout.TextX, Layout.BoxY + 9U, Layout.InnerW, BLS_ATTR_MUTED,
                   L"Debug writes BootLoaderSelector.log on the selector EFI volume.");
  BlsPrintFixedRow(Layout.TextX, Layout.BoxY + 10U, Layout.InnerW, BLS_ATTR_MUTED,
                   L"Timeout 0 disables countdown when selector is shown.");

  if (Layout.Rows >= 3U) {
    BlsUiHLine(0U, Layout.LineRow, (Layout.Cols > 0U) ? Layout.Cols - 1U : Layout.Cols,
               BLS_ATTR_MUTED, L'-');
    if (Layout.Cols >= 92U) {
      BlsUiCentered(Layout.FooterRow, BLS_ATTR_MUTED,
                    L"Up/Down: Select   Left/Right: Change   Enter: Save   Esc: Cancel   F10: PrintScreen");
    } else {
      BlsUiCentered(Layout.FooterRow, BLS_ATTR_MUTED,
                    L"Up/Down: Select   Left/Right: Change   Enter: Save   Esc: Cancel");
    }
  }
}

static EFI_STATUS BlsCommitOptions(
  IN OUT BLS_CONFIG *Config,
  IN EFI_HANDLE ConfigVolumeHandle,
  IN BOOLEAN Changed
  ) {
  EFI_STATUS Status;

  if (Config == NULL) return EFI_INVALID_PARAMETER;
  if (!Changed && !Config->ForceRewrite) return EFI_SUCCESS;
  if (ConfigVolumeHandle == NULL) {
    Config->ForceRewrite = TRUE;
    return EFI_WARN_WRITE_FAILURE;
  }

  Status = BlsSaveConfig(ConfigVolumeHandle, Config);
  Config->ForceRewrite = (BOOLEAN)(Status != EFI_SUCCESS);
  return (Status == EFI_SUCCESS) ? EFI_SUCCESS : EFI_WARN_WRITE_FAILURE;
}

static EFI_STATUS BlsRunOptions(
  IN OUT BLS_CONFIG *Config,
  IN EFI_HANDLE ConfigVolumeHandle,
  IN EFI_HANDLE ScreenshotVolumeHandle
  ) {
  EFI_STATUS Status;
  EFI_INPUT_KEY Key;
  EFI_EVENT Event;
  UINTN EventIndex;
  UINTN Timeout;
  BOOLEAN Remember;
  BOOLEAN ShowHardware;
  BOOLEAN Debug;
  BOOLEAN Changed;
  UINTN SelectedOption;
  UINTN SpuriousSignals;
  BLS_CONFIG OriginalConfig;

  if (Config == NULL) return EFI_INVALID_PARAMETER;
  if (gBS == NULL || gBS->WaitForEvent == NULL ||
      gST == NULL || gST->ConIn == NULL || gST->ConIn->WaitForKey == NULL ||
      gST->ConIn->ReadKeyStroke == NULL) return EFI_UNSUPPORTED;
  CopyMem(&OriginalConfig, Config, sizeof(OriginalConfig));
  Timeout = Config->Timeout;
  if (Timeout > BLS_TIMEOUT_MAX) Timeout = BLS_TIMEOUT_MAX;
  Remember = Config->RememberLastSelected;
  ShowHardware = Config->ShowHardwareInfo;
  Debug = Config->Debug;
  SelectedOption = 0;
  SpuriousSignals = 0;

  BlsDrawOptionsScreen(Config, SelectedOption, TRUE);
  while (TRUE) {
    Event = gST->ConIn->WaitForKey;
    EventIndex = MAX_UINTN;
    Status = gBS->WaitForEvent(1, &Event, &EventIndex);
    if (Status != EFI_SUCCESS) {
      CopyMem(Config, &OriginalConfig, sizeof(*Config));
      return Status;
    }
    if (EventIndex != 0) {
      CopyMem(Config, &OriginalConfig, sizeof(*Config));
      return EFI_DEVICE_ERROR;
    }

    ZeroMem(&Key, sizeof(Key));
    Status = gST->ConIn->ReadKeyStroke(gST->ConIn, &Key);
    if (Status == EFI_NOT_READY || (Status == EFI_SUCCESS && BlsInputKeyIsEmpty(&Key))) {
      SpuriousSignals++;
      if (SpuriousSignals >= BLS_SPURIOUS_KEY_SIGNAL_MAX) {
        CopyMem(Config, &OriginalConfig, sizeof(*Config));
        return EFI_DEVICE_ERROR;
      }
      continue;
    }
    if (Status != EFI_SUCCESS) {
      CopyMem(Config, &OriginalConfig, sizeof(*Config));
      return Status;
    }
    SpuriousSignals = 0;

    if (Key.ScanCode == SCAN_UP) {
      UINTN OldSelectedOption;
      OldSelectedOption = SelectedOption;
      SelectedOption = (SelectedOption == 0) ? BLS_OPTION_COUNT - 1 : SelectedOption - 1;
      BlsDrawOptionsSelectionDelta(Config, OldSelectedOption, SelectedOption);
    } else if (Key.ScanCode == SCAN_DOWN) {
      UINTN OldSelectedOption;
      OldSelectedOption = SelectedOption;
      SelectedOption = (SelectedOption + 1 >= BLS_OPTION_COUNT) ? 0 : SelectedOption + 1;
      BlsDrawOptionsSelectionDelta(Config, OldSelectedOption, SelectedOption);
    } else if (Key.ScanCode == SCAN_LEFT) {
      BOOLEAN ValueChanged;
      ValueChanged = TRUE;
      if (SelectedOption == 0) {
        if (Timeout > 0) {
          Timeout--;
          Config->Timeout = Timeout;
        } else {
          ValueChanged = FALSE;
        }
      } else if (SelectedOption == 1) {
        Remember = !Remember;
        Config->RememberLastSelected = Remember;
      } else if (SelectedOption == 2) {
        ShowHardware = !ShowHardware;
        Config->ShowHardwareInfo = ShowHardware;
      } else {
        Debug = !Debug;
        Config->Debug = Debug;
      }
      if (ValueChanged) BlsDrawOptionValueDelta(Config, SelectedOption);
    } else if (Key.ScanCode == SCAN_RIGHT) {
      BOOLEAN ValueChanged;
      ValueChanged = TRUE;
      if (SelectedOption == 0) {
        if (Timeout < BLS_TIMEOUT_MAX) {
          Timeout++;
          Config->Timeout = Timeout;
        } else {
          ValueChanged = FALSE;
        }
      } else if (SelectedOption == 1) {
        Remember = !Remember;
        Config->RememberLastSelected = Remember;
      } else if (SelectedOption == 2) {
        ShowHardware = !ShowHardware;
        Config->ShowHardwareInfo = ShowHardware;
      } else {
        Debug = !Debug;
        Config->Debug = Debug;
      }
      if (ValueChanged) BlsDrawOptionValueDelta(Config, SelectedOption);
    } else if (Key.ScanCode == SCAN_F1) {
      BlsDrawHelpScreen();
      Status = BlsWaitForModalDismiss();
      if (Status != EFI_SUCCESS) {
        CopyMem(Config, &OriginalConfig, sizeof(*Config));
        return Status;
      }
      BlsDrawOptionsScreen(Config, SelectedOption, TRUE);
    } else if (Key.ScanCode == SCAN_F10) {
      CHAR16 ShotPath[BLS_MAX_PATH];
      EFI_STATUS ShotStatus;
      ShotPath[0] = L'\0';
      ShotStatus = BlsSaveScreenshot(ScreenshotVolumeHandle, ShotPath, ARRAY_SIZE(ShotPath));
      BlsDrawOptionsScreen(Config, SelectedOption, FALSE);
      if (ShotStatus == EFI_SUCCESS) {
        BlsUiCentered(BlsFooterRow(), BLS_ATTR_ACCENT, L"PrintScreen saved");
      } else {
        BlsUiCentered(BlsFooterRow(), BLS_ATTR_WARN, L"PrintScreen failed");
      }
    } else if (Key.ScanCode == SCAN_NULL && Key.UnicodeChar == CHAR_CARRIAGE_RETURN) {
      Config->Timeout = Timeout;
      Config->RememberLastSelected = Remember;
      Config->ShowHardwareInfo = ShowHardware;
      Config->Debug = Debug;
      Changed = (BOOLEAN)(Config->Timeout != OriginalConfig.Timeout ||
                          Config->RememberLastSelected != OriginalConfig.RememberLastSelected ||
                          Config->ShowHardwareInfo != OriginalConfig.ShowHardwareInfo ||
                          Config->Debug != OriginalConfig.Debug);
      Status = BlsCommitOptions(Config, ConfigVolumeHandle, Changed);
      return Status;
    } else if (Key.ScanCode == SCAN_ESC) {
      /* Esc is a real cancel action. Restore the complete entry snapshot so
         partial changes never leak into the session after Back or an input
         failure. A pre-existing canonical rewrite may still be retried. */
      CopyMem(Config, &OriginalConfig, sizeof(*Config));
      if (Config->ForceRewrite) {
        return BlsCommitOptions(Config, ConfigVolumeHandle, FALSE);
      }
      return EFI_SUCCESS;
    }
  }
}

static BOOLEAN BlsIsTimeoutKey(IN EFI_INPUT_KEY *Key) {
  return BlsInputKeyIsEmpty(Key);
}

static BOOLEAN BlsConsumeSignaledCountdownTick(IN EFI_EVENT TimerEvent, IN OUT UINTN *Remaining) {
  if (TimerEvent == NULL || Remaining == NULL || *Remaining == 0 ||
      gBS == NULL || gBS->CheckEvent == NULL) {
    return FALSE;
  }
  if (gBS->CheckEvent(TimerEvent) != EFI_SUCCESS) return FALSE;
  /* Periodic ticks update display only. The independent one-shot deadline is
     the sole authority for autoboot, so coalesced or early periodic events
     cannot shorten or lengthen the configured timeout. */
  if (*Remaining > 1) (*Remaining)--;
  return FALSE;
}

EFI_STATUS BlsRunMenu(
  IN CONST BLS_HARDWARE_INFO *Hw,
  IN OUT BLS_CONFIG *Config,
  IN EFI_HANDLE ConfigVolumeHandle,
  IN EFI_HANDLE ScreenshotVolumeHandle,
  IN OUT BLS_BOOT_LIST *List,
  IN BOOLEAN SuppressCountdown,
  OUT UINTN *SelectedIndex,
  OUT BLS_MENU_ACTION *Action
  ) {
  EFI_STATUS Status;
  UINTN Selected;
  UINTN OldSelected;
  UINTN EventIndex;
  EFI_EVENT Events[3];
  EFI_EVENT TimerEvent;
  EFI_EVENT DeadlineEvent;
  EFI_EVENT CreatedEvent;
  EFI_INPUT_KEY Key;
  UINTN Remaining;
  UINTN LastCountdownRendered;
  UINTN Cols;
  BOOLEAN CountdownActive;
  UINTN SpuriousSignals;
  CONST BLS_HARDWARE_INFO *DisplayHw;
  EFI_STATUS ReturnStatus;

  if (Config == NULL || List == NULL || List->Count > BLS_MAX_ENTRIES ||
      SelectedIndex == NULL || Action == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  if (gBS == NULL || gBS->WaitForEvent == NULL ||
      gST == NULL || gST->ConIn == NULL || gST->ConIn->WaitForKey == NULL ||
      gST->ConIn->ReadKeyStroke == NULL) return EFI_UNSUPPORTED;
  *SelectedIndex = 0;
  *Action = BlsMenuExit;
  ReturnStatus = EFI_SUCCESS;
  Selected = BlsDefaultIndex(Config, List);
  Remaining = (Config != NULL) ? Config->Timeout : 0;
  LastCountdownRendered = MAX_UINTN;
  if (Remaining > BLS_TIMEOUT_MAX) Remaining = BLS_TIMEOUT_MAX;
  CountdownActive = (!SuppressCountdown && List->Count > 0 && Remaining > 0);
  SpuriousSignals = 0;

  TimerEvent = NULL;
  DeadlineEvent = NULL;
  DisplayHw = BlsSelectDisplayHardware(Hw, Config);
  BlsDrawScreen(DisplayHw, List, Selected, TRUE);
  BlsUiGetSize(&Cols, NULL);

  if (CountdownActive) {
    if (gBS->CreateEvent == NULL || gBS->SetTimer == NULL || gBS->CloseEvent == NULL ||
        Remaining > MAX_UINT64 / 10000000ULL) {
      CountdownActive = FALSE;
    } else {
      CreatedEvent = NULL;
      Status = gBS->CreateEvent(EVT_TIMER, TPL_CALLBACK, NULL, NULL, &CreatedEvent);
      if (Status == EFI_SUCCESS) {
        if (CreatedEvent == NULL) {
          Status = EFI_DEVICE_ERROR;
        } else {
          TimerEvent = CreatedEvent;
        }
      }
      if (Status == EFI_SUCCESS) {
        CreatedEvent = NULL;
        Status = gBS->CreateEvent(EVT_TIMER, TPL_CALLBACK, NULL, NULL, &CreatedEvent);
        if (Status == EFI_SUCCESS) {
          if (CreatedEvent == NULL) {
            Status = EFI_DEVICE_ERROR;
          } else {
            DeadlineEvent = CreatedEvent;
          }
        }
      }
      if (Status == EFI_SUCCESS) {
        Status = gBS->SetTimer(TimerEvent, TimerPeriodic, 10000000ULL);
      }
      if (Status == EFI_SUCCESS) {
        Status = gBS->SetTimer(DeadlineEvent, TimerRelative, (UINT64)Remaining * 10000000ULL);
      }
      if (Status != EFI_SUCCESS || TimerEvent == NULL || DeadlineEvent == NULL) {
        (VOID)BlsCloseTimerEvent(&TimerEvent);
        (VOID)BlsCloseTimerEvent(&DeadlineEvent);
        CountdownActive = FALSE;
      }
    }
  }

  while (TRUE) {
    EventIndex = MAX_UINTN;
    if (CountdownActive && TimerEvent != NULL && DeadlineEvent != NULL) {
      /* Repaint the counter only when its text actually changes. A spurious
         WaitForKey signal may return repeatedly without consuming a second;
         redrawing on each pass needlessly invokes slow console firmware and
         can flicker on affected UEFI implementations. */
      if (Remaining != LastCountdownRendered) {
        BlsDrawCountdownStatus(Cols, Remaining);
        LastCountdownRendered = Remaining;
      }
      Events[0] = gST->ConIn->WaitForKey;
      Events[1] = DeadlineEvent;
      Events[2] = TimerEvent;
      Status = gBS->WaitForEvent(3, Events, &EventIndex);
    } else {
      Events[0] = gST->ConIn->WaitForKey;
      Status = gBS->WaitForEvent(1, Events, &EventIndex);
    }

    if (Status != EFI_SUCCESS) {
      ReturnStatus = Status;
      break;
    }
    if ((CountdownActive && TimerEvent != NULL && DeadlineEvent != NULL && EventIndex >= ARRAY_SIZE(Events)) ||
        ((!CountdownActive || TimerEvent == NULL || DeadlineEvent == NULL) && EventIndex != 0)) {
      ReturnStatus = EFI_DEVICE_ERROR;
      break;
    }

    if (CountdownActive && DeadlineEvent != NULL && EventIndex == 1) {
      *SelectedIndex = Selected;
      *Action = BlsMenuBoot;
      break;
    }
    if (CountdownActive && TimerEvent != NULL && EventIndex == 2) {
      if (Remaining > 1) Remaining--;
      continue;
    }

    ZeroMem(&Key, sizeof(Key));
    Status = gST->ConIn->ReadKeyStroke(gST->ConIn, &Key);
    if (Status == EFI_NOT_READY) {
      /* WaitForKey precedes the periodic timer in Events[]. A stale/spurious
         keyboard signal can therefore win repeatedly after the timer is also
         signaled. Consume a pending timer tick explicitly so such firmware
         behavior cannot suspend autoboot indefinitely. */
      if (CountdownActive && DeadlineEvent != NULL && gBS->CheckEvent != NULL &&
          gBS->CheckEvent(DeadlineEvent) == EFI_SUCCESS) {
        *SelectedIndex = Selected;
        *Action = BlsMenuBoot;
        break;
      }
      if (CountdownActive && TimerEvent != NULL &&
          BlsConsumeSignaledCountdownTick(TimerEvent, &Remaining)) {
        *SelectedIndex = Selected;
        *Action = BlsMenuBoot;
        break;
      }
      SpuriousSignals++;
      if (SpuriousSignals >= BLS_SPURIOUS_KEY_SIGNAL_MAX) {
        ReturnStatus = EFI_DEVICE_ERROR;
        break;
      }
      continue;
    }
    if (Status != EFI_SUCCESS) {
      ReturnStatus = Status;
      break;
    }
    if (BlsIsTimeoutKey(&Key)) {
      if (CountdownActive && DeadlineEvent != NULL && gBS->CheckEvent != NULL &&
          gBS->CheckEvent(DeadlineEvent) == EFI_SUCCESS) {
        *SelectedIndex = Selected;
        *Action = BlsMenuBoot;
        break;
      }
      if (CountdownActive && TimerEvent != NULL &&
          BlsConsumeSignaledCountdownTick(TimerEvent, &Remaining)) {
        *SelectedIndex = Selected;
        *Action = BlsMenuBoot;
        break;
      }
      SpuriousSignals++;
      if (SpuriousSignals >= BLS_SPURIOUS_KEY_SIGNAL_MAX) {
        ReturnStatus = EFI_DEVICE_ERROR;
        break;
      }
      continue;
    }
    SpuriousSignals = 0;

    /* A signaled WaitForKey event is not itself proof that a key was read.
       Cancel autoboot only after ReadKeyStroke returns a real key, so a
       spurious signal/EFI_NOT_READY cannot disable the countdown. This also
       ensures Enter, F5 and F10 cancel the timer consistently. */
    Status = BlsCancelCountdown(&TimerEvent, &DeadlineEvent, &CountdownActive, Cols);
    if (Status != EFI_SUCCESS) {
      ReturnStatus = Status;
      break;
    }

    if (Key.ScanCode == SCAN_F1) {
      BlsDrawHelpScreen();
      Status = BlsWaitForModalDismiss();
      if (Status != EFI_SUCCESS) {
        ReturnStatus = Status;
        break;
      }
      DisplayHw = BlsSelectDisplayHardware(Hw, Config);
      BlsDrawScreen(DisplayHw, List, Selected, TRUE);
    } else if (Key.ScanCode == SCAN_NULL && Key.UnicodeChar == L' ' &&
               List->Count > 0U && Selected < List->Count) {
      BlsDrawEntryDetails(&List->Entries[Selected], Selected, List->Count);
      Status = BlsWaitForModalDismiss();
      if (Status != EFI_SUCCESS) {
        ReturnStatus = Status;
        break;
      }
      DisplayHw = BlsSelectDisplayHardware(Hw, Config);
      BlsDrawScreen(DisplayHw, List, Selected, TRUE);
    } else if (Key.ScanCode == SCAN_UP) {
      if (BlsSelectableCount(List) > 0) {
        OldSelected = Selected;
        Selected = (Selected == 0) ? BlsSelectableCount(List) - 1 : Selected - 1;
        DisplayHw = BlsSelectDisplayHardware(Hw, Config);
        if (!BlsDrawSelectionDelta(List, OldSelected, Selected, DisplayHw != NULL)) {
          BlsDrawEntries(List, Selected, DisplayHw != NULL);
        }
      }
    } else if (Key.ScanCode == SCAN_DOWN) {
      if (BlsSelectableCount(List) > 0) {
        OldSelected = Selected;
        Selected = (Selected + 1 >= BlsSelectableCount(List)) ? 0 : Selected + 1;
        DisplayHw = BlsSelectDisplayHardware(Hw, Config);
        if (!BlsDrawSelectionDelta(List, OldSelected, Selected, DisplayHw != NULL)) {
          BlsDrawEntries(List, Selected, DisplayHw != NULL);
        }
      }
    } else if (Key.ScanCode == SCAN_LEFT) {
      *Action = BlsMenuExit;
      break;
    } else if (Key.ScanCode == SCAN_ESC) {
      *Action = BlsMenuExit;
      break;
    } else if (Key.ScanCode == SCAN_F5) {
      *Action = BlsMenuRefresh;
      break;
    } else if (Key.ScanCode == SCAN_F10) {
      CHAR16 ShotPath[BLS_MAX_PATH];
      EFI_STATUS ShotStatus;
      ShotPath[0] = L'\0';
      ShotStatus = BlsSaveScreenshot(ScreenshotVolumeHandle, ShotPath, ARRAY_SIZE(ShotPath));
      DisplayHw = BlsSelectDisplayHardware(Hw, Config);
      BlsDrawEntries(List, Selected, DisplayHw != NULL);
      BlsDrawFooter();
      if (ShotStatus == EFI_SUCCESS) {
        BlsUiCentered(BlsFooterRow(), BLS_ATTR_ACCENT, L"PrintScreen saved to \\EFI\\BOOT\\Screenshots");
      } else {
        BlsUiCentered(BlsFooterRow(), BLS_ATTR_WARN, L"PrintScreen failed");
      }
    } else if (Key.ScanCode == SCAN_RIGHT || (Key.ScanCode == SCAN_NULL && Key.UnicodeChar == CHAR_CARRIAGE_RETURN)) {
      if (BlsSelectionIsOptions(List, Selected)) {
        EFI_STATUS OptionsStatus;

        OptionsStatus = BlsRunOptions(Config, ConfigVolumeHandle, ScreenshotVolumeHandle);
        if (EFI_ERROR(OptionsStatus)) {
          ReturnStatus = OptionsStatus;
          *Action = BlsMenuExit;
          break;
        }
        if (Config != NULL && Config->ShowHardwareInfo && Hw == NULL) {
          *Action = BlsMenuRefresh;
          break;
        }
        Remaining = (Config != NULL) ? Config->Timeout : 0;
        if (Remaining > BLS_TIMEOUT_MAX) Remaining = BLS_TIMEOUT_MAX;
        DisplayHw = BlsSelectDisplayHardware(Hw, Config);
        BlsDrawScreen(DisplayHw, List, Selected, TRUE);
        if (OptionsStatus == EFI_WARN_WRITE_FAILURE) {
          BlsUiCentered(BlsFooterRow(),
                        BLS_ATTR_WARN,
                        L"Settings changed for this session, but BLS.plist could not be saved");
        }
      } else if (List->Count > 0) {
        *SelectedIndex = Selected;
        *Action = BlsMenuBoot;
        break;
      }
    }
  }

  Status = BlsCloseTimerEvent(&TimerEvent);
  if (ReturnStatus == EFI_SUCCESS && Status != EFI_SUCCESS) ReturnStatus = Status;
  Status = BlsCloseTimerEvent(&DeadlineEvent);
  if (ReturnStatus == EFI_SUCCESS && Status != EFI_SUCCESS) ReturnStatus = Status;
  return ReturnStatus;
}

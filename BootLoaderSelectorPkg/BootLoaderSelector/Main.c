/** @file
  BootLoaderSelector main entry point.
  Clean UEFI selector for OpenCore and Clover.
  Credits: ©DNMTechLabs / dnmtechlabs@gmail.com
**/
#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiApplicationEntryPoint.h>
#include <Library/UefiBootServicesTableLib.h>
#include "BootScan.h"
#include "Config.h"
#include "HardwareInfo.h"
#include "Logger.h"
#include "Menu.h"
#include "Ui.h"
#include "Utils.h"

#define BLS_KEY_DRAIN_MAX 64U
#define BLS_NOT_READY_SPIN_MAX 1024U
#define BLS_HOTKEY_SPIN_MAX 1024U

EFI_STATUS EFIAPI UefiMain(IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable);

static UINTN BlsFindRememberedEntry(
  IN CONST BLS_BOOT_LIST *List,
  IN CONST BLS_CONFIG *Config,
  IN EFI_STATUS ScanStatus
  ) {
  UINTN Index;
  UINTN FoundIndex;

  if (List == NULL || Config == NULL || Config->DefaultBootloader[0] == L'\0') return MAX_UINTN;
  if (ScanStatus != EFI_SUCCESS || List->PreferredMatchesFound != 1U) return MAX_UINTN;

  FoundIndex = MAX_UINTN;
  for (Index = 0; Index < List->Count; Index++) {
    if (!BlsConfigMatchesEntry(Config, &List->Entries[Index])) continue;
    if (FoundIndex != MAX_UINTN) return MAX_UINTN;
    FoundIndex = Index;
  }
  return FoundIndex;
}

static VOID BlsMoveEntry(IN OUT BLS_BOOT_LIST *List, IN UINTN From, IN UINTN To) {
  BLS_BOOT_ENTRY Entry;
  UINTN Index;

  if (List == NULL || From >= List->Count || To >= List->Count || From == To) return;
  CopyMem(&Entry, &List->Entries[From], sizeof(Entry));
  for (Index = From; Index > To; Index--) {
    CopyMem(&List->Entries[Index], &List->Entries[Index - 1], sizeof(List->Entries[Index]));
  }
  CopyMem(&List->Entries[To], &Entry, sizeof(List->Entries[To]));
}

static VOID BlsPrioritizeVolume(IN OUT BLS_BOOT_LIST *List, IN EFI_HANDLE PreferredVolume) {
  UINTN Index;
  UINTN InsertAt;

  if (List == NULL || PreferredVolume == NULL || List->Count < 2) return;
  InsertAt = 0;
  for (Index = 0; Index < List->Count; Index++) {
    if (List->Entries[Index].VolumeHandle == PreferredVolume) {
      BlsMoveEntry(List, Index, InsertAt);
      InsertAt++;
    }
  }
}

static VOID BlsUnicodeToAsciiLogField(
  IN CONST CHAR16 *Source,
  OUT CHAR8 *Destination,
  IN UINTN DestinationSize
  ) {
  UINTN Index;

  if (Destination == NULL || DestinationSize == 0U) return;
  Destination[0] = '\0';
  if (Source == NULL) return;

  for (Index = 0U; Index + 1U < DestinationSize && Source[Index] != L'\0'; Index++) {
    CHAR16 Ch;
    Ch = Source[Index];
    Destination[Index] = (Ch >= 0x20U && Ch <= 0x7EU) ? (CHAR8)Ch : '?';
  }
  Destination[Index] = '\0';
}

static VOID BlsLogStatus(
  IN OUT BLS_LOGGER *Logger,
  IN CONST CHAR8 *Context,
  IN EFI_STATUS Status
  ) {
  CHAR8 Line[192];

  if (Logger == NULL || !Logger->Enabled || Context == NULL) return;
  AsciiSPrint(Line, sizeof(Line), "%a: status=0x%Lx", Context, (UINT64)Status);
  BlsLog(Logger, Line);
}

static VOID BlsLogBootScan(
  IN OUT BLS_LOGGER *Logger,
  IN CONST BLS_BOOT_LIST *List,
  IN EFI_STATUS ScanStatus
  ) {
  CHAR8 Line[512];
  UINTN Index;

  if (Logger == NULL || !Logger->Enabled || List == NULL) return;
  AsciiSPrint(
    Line,
    sizeof(Line),
    "Boot scan: status=0x%Lx visible=%u discovered=%u preferred=%u truncated=%a",
    (UINT64)ScanStatus,
    (UINT32)List->Count,
    (UINT32)List->TotalFound,
    (UINT32)List->PreferredMatchesFound,
    List->Truncated ? "yes" : "no"
    );
  BlsLog(Logger, Line);

  for (Index = 0U; Index < List->Count; Index++) {
    CHAR8 Name[96];
    CHAR8 Volume[96];
    CHAR8 Path[160];
    CONST BLS_BOOT_ENTRY *Entry;

    Entry = &List->Entries[Index];
    BlsUnicodeToAsciiLogField(Entry->Name, Name, sizeof(Name));
    BlsUnicodeToAsciiLogField(Entry->VolumeLabel, Volume, sizeof(Volume));
    BlsUnicodeToAsciiLogField(Entry->Path, Path, sizeof(Path));
    AsciiSPrint(
      Line,
      sizeof(Line),
      "Boot entry %u: loader=%a volume=%a path=%a transport=%a",
      (UINT32)(Index + 1U),
      Name,
      Volume,
      Path,
      Entry->IsUsb ? "USB" : "non-USB"
      );
    BlsLog(Logger, Line);
  }
}


static VOID BlsWaitForRealKey(VOID) {
  EFI_EVENT Event;
  EFI_INPUT_KEY Key;
  EFI_STATUS Status;
  UINTN EventIndex;
  UINTN DrainCount;
  UINTN NotReadyCount;

  if (gBS == NULL || gBS->WaitForEvent == NULL || gST == NULL || gST->ConIn == NULL ||
      gST->ConIn->WaitForKey == NULL || gST->ConIn->ReadKeyStroke == NULL) return;

  Event = gST->ConIn->WaitForKey;
  NotReadyCount = 0;
  while (TRUE) {
    EventIndex = MAX_UINTN;
    Status = gBS->WaitForEvent(1, &Event, &EventIndex);
    if (Status != EFI_SUCCESS || EventIndex != 0) return;

    ZeroMem(&Key, sizeof(Key));
    Status = gST->ConIn->ReadKeyStroke(gST->ConIn, &Key);
    if (Status == EFI_NOT_READY ||
        (Status == EFI_SUCCESS && Key.ScanCode == SCAN_NULL && Key.UnicodeChar == CHAR_NULL)) {
      /* Some firmware can leave WaitForKey signaled while returning either
         EFI_NOT_READY or an empty key. Bound both forms of spurious input so
         the post-boot-failure diagnostic cannot livelock forever. */
      NotReadyCount++;
      if (NotReadyCount >= BLS_NOT_READY_SPIN_MAX) return;
      continue;
    }
    if (Status != EFI_SUCCESS) return;

    /* Consume only keys that were already queued with the acknowledged key,
       preventing the same press from immediately acting on the redrawn menu. */
    DrainCount = 0;
    do {
      ZeroMem(&Key, sizeof(Key));
      Status = gST->ConIn->ReadKeyStroke(gST->ConIn, &Key);
      DrainCount++;
    } while (Status == EFI_SUCCESS && DrainCount < BLS_KEY_DRAIN_MAX);
    return;
  }
}

static VOID BlsTryDisableInheritedBootWatchdog(VOID) {
  if (gBS == NULL || gBS->SetWatchdogTimer == NULL) return;

  /* The firmware boot manager arms a five-minute watchdog before starting a
     boot option. BootLoaderSelector is interactive and can legitimately remain
     active longer than that, so do not inherit a stale reset deadline. This is
     deliberately best-effort: a platform unable to reprogram its watchdog must
     not be converted into an immediate, guaranteed boot failure. */
  (VOID)gBS->SetWatchdogTimer(0U, 0U, 0U, NULL);
}



static BOOLEAN BlsSelectorHotkeyTimerExpired(IN EFI_EVENT TimerEvent) {
  if (TimerEvent == NULL || gBS == NULL || gBS->CheckEvent == NULL) return FALSE;
  return (BOOLEAN)(gBS->CheckEvent(TimerEvent) == EFI_SUCCESS);
}

static BOOLEAN BlsWaitForSelectorHotkey(VOID) {
  EFI_STATUS Status;
  EFI_EVENT Events[2];
  EFI_EVENT TimerEvent;
  EFI_EVENT CreatedEvent;
  UINTN EventIndex;
  EFI_INPUT_KEY Key;
  BOOLEAN ShowSelector;
  EFI_STATUS CleanupStatus;
  UINTN NonSelectorCount;

  TimerEvent = NULL;
  NonSelectorCount = 0U;
  ShowSelector = TRUE;

  if (gBS == NULL || gBS->CreateEvent == NULL || gBS->SetTimer == NULL ||
      gBS->WaitForEvent == NULL || gBS->CheckEvent == NULL ||
      gBS->CloseEvent == NULL ||
      gST == NULL || gST->ConIn == NULL || gST->ConIn->WaitForKey == NULL ||
      gST->ConIn->ReadKeyStroke == NULL) return TRUE;

  CreatedEvent = NULL;
  Status = gBS->CreateEvent(EVT_TIMER, TPL_CALLBACK, NULL, NULL, &CreatedEvent);
  if (Status != EFI_SUCCESS) {
    goto Done;
  }
  if (CreatedEvent == NULL) {
    goto Done;
  }
  /* CreateEvent() transfers ownership only on success. Keep its OUT value in
     a temporary until that contract is established so a failing/non-conforming
     firmware cannot make cleanup close an unowned scribbled handle. */
  TimerEvent = CreatedEvent;

  Status = gBS->SetTimer(TimerEvent, TimerRelative, 20000000ULL);
  if (Status != EFI_SUCCESS) {
    goto Done;
  }
  Events[0] = gST->ConIn->WaitForKey;
  Events[1] = TimerEvent;

  while (TRUE) {
    Status = gBS->WaitForEvent(2, Events, &EventIndex);
    if (Status != EFI_SUCCESS || EventIndex >= ARRAY_SIZE(Events)) {
      goto Done;
    }
    if (EventIndex == 1) {
      ShowSelector = FALSE;
      break;
    }

    ZeroMem(&Key, sizeof(Key));
    Status = gST->ConIn->ReadKeyStroke(gST->ConIn, &Key);
    if (Status == EFI_NOT_READY) {
      if (BlsSelectorHotkeyTimerExpired(TimerEvent)) {
        ShowSelector = FALSE;
        break;
      }
      NonSelectorCount++;
      if (NonSelectorCount >= BLS_HOTKEY_SPIN_MAX) goto Done;
      continue;
    }
    if (Status != EFI_SUCCESS) goto Done;
    if (Key.ScanCode == SCAN_UP || Key.ScanCode == SCAN_DOWN ||
        Key.ScanCode == SCAN_LEFT || Key.ScanCode == SCAN_RIGHT) {
      break;
    }

    /* WaitForEvent checks array entries from first to last. With WaitForKey
       first, a continuously queued non-selector key can otherwise win every
       iteration and starve an already-expired timer indefinitely. Preserve
       selector-key priority, then explicitly observe the timeout after keys
       that do not activate the selector. */
    if (BlsSelectorHotkeyTimerExpired(TimerEvent)) {
      ShowSelector = FALSE;
      break;
    }

    /* A firmware can violate both event contracts at once: keep WaitForKey
       permanently signaled while CheckEvent never reports the timer deadline.
       Bound that pathological loop so malformed firmware cannot turn the
       startup hotkey probe into an unbounded CPU spin. Normal firmware exits
       through the two-second timer long before this defensive ceiling. */
    NonSelectorCount++;
    if (NonSelectorCount >= BLS_HOTKEY_SPIN_MAX) goto Done;
  }

Done:
  CleanupStatus = BlsCloseTimerEvent(&TimerEvent);
  if (CleanupStatus != EFI_SUCCESS) ShowSelector = TRUE;
  return ShowSelector;
}

EFI_STATUS EFIAPI UefiMain(IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable) {
  EFI_STATUS Status;
  BLS_CONFIG Config;
  EFI_HANDLE ConfigVolume;
  EFI_HANDLE SelectorVolume;
  BLS_LOGGER *Logger;
  BLS_HARDWARE_INFO Hw;
  BLS_BOOT_LIST *List;
  UINTN Selected;
  BLS_MENU_ACTION Action;
  EFI_STATUS StartStatus;
  CHAR16 ExitDataText[192];
  BOOLEAN HardwareLoaded;
  BOOLEAN AutoBootFailed;
  BOOLEAN BootLifecycleCompromised;
  BOOLEAN UiModeInitialized;
  EFI_STATUS ReturnStatus;

  (VOID)SystemTable;
  if (gBS == NULL) return EFI_NOT_READY;
  BlsTryDisableInheritedBootWatchdog();

  /* Keep large session aggregates off the shared firmware stack. */
  List = AllocateZeroPool(sizeof(*List));
  if (List == NULL) return EFI_OUT_OF_RESOURCES;
  Logger = AllocateZeroPool(sizeof(*Logger));
  if (Logger == NULL) {
    FreePool(List);
    return EFI_OUT_OF_RESOURCES;
  }
  /* Preserve the caller's console state immediately, but defer expensive mode
     probing/switching until selector UI is actually needed. A remembered
     autoboot can therefore chainload without up to BLS_TEXT_MODE_SCAN_MAX
     QueryMode() calls or a visible SetMode() transition. */
  BlsUiCaptureOriginalState();
  ConfigVolume = NULL;
  SelectorVolume = BlsGetImageVolumeHandle(ImageHandle);
  HardwareLoaded = FALSE;
  AutoBootFailed = FALSE;
  UiModeInitialized = FALSE;
  ReturnStatus = EFI_SUCCESS;
  ZeroMem(&Hw, sizeof(Hw));

  BlsConfigDefaults(&Config);
  Status = BlsLoadConfig(ImageHandle, &Config, &ConfigVolume);
  if (SelectorVolume == NULL) SelectorVolume = ConfigVolume;
  if (Status != EFI_SUCCESS && Status != EFI_WARN_WRITE_FAILURE) {
    BlsConfigDefaults(&Config);
    ConfigVolume = NULL;
  }

  BlsLogInit(Logger, Config.Debug, ConfigVolume);
  BlsLog(Logger, "BootLoaderSelector " BLS_VERSION_ASCII " started");
  if (Status == EFI_WARN_WRITE_FAILURE) {
    BlsLog(Logger, "Configuration loaded but normalization could not be saved");
    BlsLogStatus(Logger, "Configuration normalization save", Status);
  } else if (Status != EFI_SUCCESS) {
    BlsLogStatus(Logger, "Configuration load; defaults applied", Status);
  }

  while (TRUE) {
    if (Config.RememberLastSelected && Config.HasRememberedSelection) {
      Status = BlsScanBootloadersPreferred(
                 List,
                 Config.DefaultPath,
                 Config.DefaultVolumeLabel,
                 Config.DefaultVolumeDevicePath
                 );
    } else {
      Status = BlsScanBootloaders(List);
    }
    if (List->Count > 0) {
      BlsPrioritizeVolume(List, SelectorVolume);
      if (List->Truncated) {
        BlsLog(Logger, "Boot scan found more entries than the display cap; additional entries were omitted");
      }
      if (Status == EFI_SUCCESS) {
        BlsLog(Logger, "Boot scan completed");
      } else {
        BlsLog(Logger, "Boot scan completed with filesystem access/lifecycle errors");
      }
    } else if (Status != EFI_SUCCESS) {
      BlsLog(Logger, "Boot scan failed");
    } else {
      BlsLog(Logger, "Boot scan completed");
    }
    BlsLogBootScan(Logger, List, Status);

    Selected = 0;
    Action = BlsMenuExit;

    if (!AutoBootFailed && Config.RememberLastSelected && Config.HasRememberedSelection && List->Count > 0) {
      UINTN RememberedIndex;

      RememberedIndex = BlsFindRememberedEntry(List, &Config, Status);
      if (RememberedIndex != MAX_UINTN && RememberedIndex < List->Count) {
        if (!BlsWaitForSelectorHotkey()) {
          Selected = RememberedIndex;
          Action = BlsMenuBoot;
        }
      } else {
        /* Never autoboot an arbitrary first match when the persisted identity
           is missing or ambiguous. Keep the menu available without countdown
           so the user can select the intended volume explicitly. */
        AutoBootFailed = TRUE;
        BlsLog(Logger, "Remembered boot selection unavailable or ambiguous");
      }
    }

    if (Action != BlsMenuBoot) {
      if (!UiModeInitialized) {
        BlsUiInitReadableMode();
        UiModeInitialized = TRUE;
        if (BlsUiLastModeStatus() != EFI_SUCCESS) {
          BlsLog(Logger, "Console SetMode failed; continuing with firmware current mode");
        }
      }
      if (Config.ShowHardwareInfo && !HardwareLoaded) {
        BlsReadHardwareInfo(&Hw);
        HardwareLoaded = TRUE;
        BlsLog(Logger, "Hardware info loaded");
      }
      Status = BlsRunMenu((Config.ShowHardwareInfo && HardwareLoaded) ? &Hw : NULL, &Config, ConfigVolume, SelectorVolume, List, AutoBootFailed, &Selected, &Action);
      if (Status != EFI_SUCCESS) {
        BlsLog(Logger, "Menu failed");
        BlsLogStatus(Logger, "Menu", Status);
        ReturnStatus = Status;
        break;
      }
      BlsLogSetEnabled(Logger, Config.Debug, ConfigVolume);
    }

    if (Action == BlsMenuRefresh) {
      continue;
    }

    if (Action == BlsMenuBoot && Selected < List->Count) {
      if (Config.RememberLastSelected) {
        if (BlsConfigRememberedEntryNeedsUpdate(&Config, &List->Entries[Selected])) {
          BlsConfigRememberEntry(&Config, &List->Entries[Selected]);
          Config.ForceRewrite = TRUE;
          if (ConfigVolume != NULL) {
            Status = BlsSaveConfig(ConfigVolume, &Config);
            if (Status == EFI_SUCCESS) {
              Config.ForceRewrite = FALSE;
            } else {
              BlsLog(Logger, "Remembered boot selection save failed");
              BlsLogStatus(Logger, "Remembered boot selection save", Status);
            }
          } else {
            BlsLog(Logger, "Remembered boot selection has no writable configuration volume");
          }
        }
      }
      BlsLog(Logger, "Starting selected bootloader");
      BlsLogFlush(Logger);

      /* Keep the transition clean only when selector UI was actually shown.
         A remembered autoboot has no menu to erase; clearing there creates a
         blank intermediate frame and an otherwise unnecessary firmware query. */
      if (UiModeInitialized) BlsUiClear();

      StartStatus = EFI_NOT_STARTED;
      ExitDataText[0] = L'\0';
      BootLifecycleCompromised = FALSE;
      Status = BlsStartBootEntryEx(
                 ImageHandle, &List->Entries[Selected], &StartStatus,
                 ExitDataText, ARRAY_SIZE(ExitDataText), &BootLifecycleCompromised);
      /* StartImage returning always means control came back from the child (or
         policy prevented it from starting). The selector must not mistake an
         EFI_SUCCESS return for a one-way handoff and silently exit. The child
         watchdog is armed/disarmed by BlsStartBootEntryEx around StartImage. */
      /* The child owns the console while StartImage() runs and may return in a
         different text mode. Revalidate geometry exactly once on the next UI
         operation instead of forcing QueryMode() on every normal redraw. */
      BlsUiInvalidateSizeCache();
      {
        CHAR8 LogText[128];
        AsciiSPrint(LogText, sizeof(LogText), "Bootloader returned: call=0x%Lx child=0x%Lx",
                    (UINT64)Status, (UINT64)StartStatus);
        BlsLog(Logger, LogText);
      }
      if (ExitDataText[0] != L'\0') {
        BlsLog(Logger, "Bootloader returned ExitData (see on-screen diagnostic)");
      }
      BlsLogFlush(Logger);
      AutoBootFailed = TRUE;

      /* A failed watchdog disable or failed image cleanup means continuing the
         interactive selector could leave a live reset timer or an orphaned image
         in firmware state. Return to the parent boot manager immediately; it owns
         the outer boot lifecycle and can perform its mandated watchdog teardown. */
      if (BootLifecycleCompromised) {
        BlsLog(Logger, "Boot lifecycle cleanup failed; exiting selector immediately");
        BlsLogFlush(Logger);
        ReturnStatus = Status;
        break;
      }

      if (EFI_ERROR(Status)) {
        CHAR16 StatusText[96];
        if (!UiModeInitialized) {
          BlsUiInitReadableMode();
          UiModeInitialized = TRUE;
          if (BlsUiLastModeStatus() != EFI_SUCCESS) {
            BlsLog(Logger, "Console SetMode failed; continuing with firmware current mode");
          }
        }
        BlsUiClear();
        BlsUiCentered(3, BLS_ATTR_ERROR, L"Bootloader failed or returned an error");
        UnicodeSPrint(StatusText, sizeof(StatusText), L"Status: 0x%Lx", (UINT64)Status);
        BlsUiCentered(5, BLS_ATTR_MUTED, StatusText);
        if (ExitDataText[0] != L'\0') BlsUiCentered(6, BLS_ATTR_MUTED, ExitDataText);
        BlsUiCentered(8, BLS_ATTR_MUTED, L"Press any key to return to BootLoaderSelector");
        BlsWaitForRealKey();
      }
      continue;
    }

    break;
  }

  BlsLog(Logger, "BootLoaderSelector exited");
  BlsLogFlush(Logger);
  BlsUiRestoreOriginalState();
  FreePool(Logger);
  FreePool(List);
  return ReturnStatus;
}

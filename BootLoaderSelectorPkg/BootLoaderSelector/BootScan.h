/** @file
  BootLoaderSelector boot entry scanning and launching.
  Credits: ©DNMTechLabs / dnmtechlabs@gmail.com
**/
#ifndef BOOTLOADER_SELECTOR_BOOTSCAN_H
#define BOOTLOADER_SELECTOR_BOOTSCAN_H

#include <Uefi.h>
#include "Utils.h"

typedef enum {
  BlsLoaderOpenCore = 0,
  BlsLoaderClover   = 1,
  BlsLoaderOther    = 2
} BLS_LOADER_TYPE;

typedef struct {
  BLS_LOADER_TYPE Type;
  CHAR16          Name[BLS_MAX_TEXT];
  CHAR16          Path[BLS_MAX_PATH];
  CHAR16          VolumeLabel[BLS_MAX_TEXT];
  CHAR16          VolumeDevicePath[BLS_MAX_VOLUME_ID];
  BOOLEAN         IsUsb;
  EFI_HANDLE      VolumeHandle;
} BLS_BOOT_ENTRY;

typedef struct {
  BLS_BOOT_ENTRY Entries[BLS_MAX_ENTRIES];
  UINTN          Count;
  UINTN          TotalFound;
  UINTN          PreferredMatchesFound;
  BOOLEAN        Truncated;
} BLS_BOOT_LIST;

EFI_STATUS BlsLocateFsHandles(OUT EFI_HANDLE **Handles, OUT UINTN *Count);
EFI_STATUS BlsScanBootloaders(OUT BLS_BOOT_LIST *List);
EFI_STATUS BlsScanBootloadersPreferred(
  OUT BLS_BOOT_LIST *List,
  IN CONST CHAR16 *PreferredPath OPTIONAL,
  IN CONST CHAR16 *PreferredVolumeLabel OPTIONAL,
  IN CONST CHAR16 *PreferredVolumeDevicePath OPTIONAL
  );
EFI_STATUS BlsStartBootEntry(IN EFI_HANDLE ImageHandle, IN CONST BLS_BOOT_ENTRY *Entry, OUT EFI_STATUS *StartStatus);
EFI_STATUS BlsStartBootEntryEx(
  IN EFI_HANDLE ImageHandle,
  IN CONST BLS_BOOT_ENTRY *Entry,
  OUT EFI_STATUS *StartStatus,
  OUT CHAR16 *ExitDataText OPTIONAL,
  IN UINTN ExitDataTextChars,
  OUT BOOLEAN *LifecycleCompromised OPTIONAL
  );

#endif

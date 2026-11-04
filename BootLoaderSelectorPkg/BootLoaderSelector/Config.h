/** @file
  BootLoaderSelector configuration.
  Credits: ©DNMTechLabs / dnmtechlabs@gmail.com
**/
#ifndef BOOTLOADER_SELECTOR_CONFIG_H
#define BOOTLOADER_SELECTOR_CONFIG_H

#include <Uefi.h>
#include "BootScan.h"
#include "Utils.h"

typedef struct {
  CHAR16  DefaultBootloader[BLS_MAX_TEXT];
  CHAR16  DefaultPath[BLS_MAX_PATH];
  CHAR16  DefaultVolumeLabel[BLS_MAX_TEXT];
  CHAR16  DefaultVolumeDevicePath[BLS_MAX_VOLUME_ID];
  UINTN   Timeout;
  BOOLEAN Debug;
  BOOLEAN ShowHardwareInfo;
  BOOLEAN RememberLastSelected;
  BOOLEAN HasRememberedSelection;
  BOOLEAN ForceRewrite;
} BLS_CONFIG;

VOID BlsConfigDefaults(OUT BLS_CONFIG *Config);
EFI_HANDLE BlsGetImageVolumeHandle(IN EFI_HANDLE ImageHandle);
EFI_STATUS BlsLoadConfig(IN EFI_HANDLE ImageHandle, OUT BLS_CONFIG *Config, OUT EFI_HANDLE *ConfigVolumeHandle);
EFI_STATUS BlsSaveConfig(IN EFI_HANDLE VolumeHandle, IN CONST BLS_CONFIG *Config);
BOOLEAN BlsConfigMatchesEntry(IN CONST BLS_CONFIG *Config, IN CONST BLS_BOOT_ENTRY *Entry);
BOOLEAN BlsConfigRememberedEntryNeedsUpdate(IN CONST BLS_CONFIG *Config, IN CONST BLS_BOOT_ENTRY *Entry);
VOID BlsConfigRememberEntry(IN OUT BLS_CONFIG *Config, IN CONST BLS_BOOT_ENTRY *Entry);

#endif

/** @file
  BootLoaderSelector menu.
  Credits: ©DNMTechLabs / dnmtechlabs@gmail.com
**/
#ifndef BOOTLOADER_SELECTOR_MENU_H
#define BOOTLOADER_SELECTOR_MENU_H

#include <Uefi.h>
#include "BootScan.h"
#include "Config.h"
#include "HardwareInfo.h"

typedef enum {
  BlsMenuBoot = 0,
  BlsMenuExit = 1,
  BlsMenuRefresh = 2
} BLS_MENU_ACTION;

EFI_STATUS BlsRunMenu(
  IN CONST BLS_HARDWARE_INFO *Hw,
  IN OUT BLS_CONFIG *Config,
  IN EFI_HANDLE ConfigVolumeHandle,
  IN EFI_HANDLE ScreenshotVolumeHandle,
  IN OUT BLS_BOOT_LIST *List,
  IN BOOLEAN SuppressCountdown,
  OUT UINTN *SelectedIndex,
  OUT BLS_MENU_ACTION *Action
  );

#endif

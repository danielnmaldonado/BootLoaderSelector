/** @file
  BootLoaderSelector screenshot support.
  Credits: ©DNMTechLabs / dnmtechlabs@gmail.com
**/
#ifndef BOOTLOADER_SELECTOR_SCREENSHOT_H
#define BOOTLOADER_SELECTOR_SCREENSHOT_H

#include <Uefi.h>

EFI_STATUS BlsSaveScreenshot(IN EFI_HANDLE VolumeHandle, OUT CHAR16 *SavedPath, IN UINTN SavedPathChars);

#endif

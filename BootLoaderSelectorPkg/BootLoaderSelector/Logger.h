/** @file
  BootLoaderSelector debug logger.
  Credits: ©DNMTechLabs / dnmtechlabs@gmail.com
**/
#ifndef BOOTLOADER_SELECTOR_LOGGER_H
#define BOOTLOADER_SELECTOR_LOGGER_H

#include <Uefi.h>
#include "Utils.h"

#define BLS_LOG_FILE_MAX_BYTES (128U * 1024U)

#if BLS_LOG_FILE_MAX_BYTES < BLS_MAX_LOG
#error "BLS_LOG_FILE_MAX_BYTES must hold at least one complete in-memory log buffer"
#endif

typedef struct {
  BOOLEAN    Enabled;
  EFI_HANDLE VolumeHandle;
  CHAR8      Buffer[BLS_MAX_LOG];
  UINTN      Length;
  BOOLEAN    Truncated;
  BOOLEAN    FileInitialized;
} BLS_LOGGER;

VOID BlsLogInit(OUT BLS_LOGGER *Logger, IN BOOLEAN Enabled, IN EFI_HANDLE VolumeHandle);
VOID BlsLogSetEnabled(IN OUT BLS_LOGGER *Logger, IN BOOLEAN Enabled, IN EFI_HANDLE VolumeHandle);
VOID BlsLog(IN OUT BLS_LOGGER *Logger, IN CONST CHAR8 *Message);
EFI_STATUS BlsLogFlush(IN OUT BLS_LOGGER *Logger);

#endif

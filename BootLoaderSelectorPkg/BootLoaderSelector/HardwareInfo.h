/** @file
  BootLoaderSelector hardware information.
  Credits: ©DNMTechLabs / dnmtechlabs@gmail.com
**/
#ifndef BOOTLOADER_SELECTOR_HARDWARE_INFO_H
#define BOOTLOADER_SELECTOR_HARDWARE_INFO_H

#include <Uefi.h>
#include "Utils.h"

typedef struct {
  CHAR16 Cpu[BLS_MAX_TEXT];
  CHAR16 Board[BLS_MAX_TEXT];
  CHAR16 Gpu[BLS_MAX_TEXT];
  CHAR16 MemoryType[16];
  UINT32 LogicalProcessors;
  UINT32 Cores;
  UINT32 CpuMHz;
  UINT64 MemoryMB;
  UINT32 MemoryMHz;
  BOOLEAN MemorySpeedIsMtps;
  BOOLEAN MemoryEstimated;
} BLS_HARDWARE_INFO;

VOID BlsReadHardwareInfo(OUT BLS_HARDWARE_INFO *Info);

#endif

/* Credits: ©DNMTechLabs / dnmtechlabs@gmail.com */
#include <assert.h>
#include <stdio.h>

#ifdef NULL
#undef NULL
#endif
#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>

#include "../../BootLoaderSelectorPkg/BootLoaderSelector/BootScan.h"
#include "../../BootLoaderSelectorPkg/BootLoaderSelector/Config.h"

EFI_HANDLE gImageHandle = NULL;
EFI_SYSTEM_TABLE *gST = NULL;
EFI_BOOT_SERVICES *gBS = NULL;

static BOOLEAN mConfigMatchesEntry = TRUE;

BOOLEAN BlsConfigMatchesEntry(IN CONST BLS_CONFIG *Config, IN CONST BLS_BOOT_ENTRY *Entry) {
  (void)Config;
  (void)Entry;
  return mConfigMatchesEntry;
}

#include "../../BootLoaderSelectorPkg/BootLoaderSelector/Main.c"

static VOID SetBootloaderName(IN OUT BLS_CONFIG *Config) {
  Config->DefaultBootloader[0] = L'O';
  Config->DefaultBootloader[1] = L'C';
  Config->DefaultBootloader[2] = L'\0';
}

int main(void) {
  BLS_BOOT_LIST List = {0};
  BLS_CONFIG Config = {0};

  List.Count = 1U;
  SetBootloaderName(&Config);

  /* No pre-scan preferred match means persisted identity is incomplete or
     cannot be proven unique across all discovered volumes. Never autoboot. */
  List.PreferredMatchesFound = 0U;
  assert(BlsFindRememberedEntry(&List, &Config, EFI_SUCCESS) == MAX_UINTN);

  /* Exactly one globally counted preferred candidate may be selected. */
  List.PreferredMatchesFound = 1U;
  assert(BlsFindRememberedEntry(&List, &Config, EFI_SUCCESS) == 0U);

  /* A partial scan cannot prove global uniqueness, even when one preferred
     candidate was already encountered before the filesystem error. */
  assert(BlsFindRememberedEntry(&List, &Config, EFI_DEVICE_ERROR) == MAX_UINTN);

  /* Multiple preferred candidates are ambiguous even if only one is visible. */
  List.PreferredMatchesFound = 2U;
  assert(BlsFindRememberedEntry(&List, &Config, EFI_SUCCESS) == MAX_UINTN);

  /* A unique preferred candidate still needs the full config matcher. */
  List.PreferredMatchesFound = 1U;
  mConfigMatchesEntry = FALSE;
  assert(BlsFindRememberedEntry(&List, &Config, EFI_SUCCESS) == MAX_UINTN);

  puts("remembered autoboot integrity tests passed");
  return 0;
}

/** @file
  Credits: ©DNMTechLabs / dnmtechlabs@gmail.com
  Deterministic UEFI child used only by the OVMF CI smoke test.
  It returns EFI_SUCCESS immediately so BootLoaderSelector exercises a real
  LoadImage/StartImage/return lifecycle without recursively chainloading itself.
**/
#include <Uefi.h>
#include <Library/UefiApplicationEntryPoint.h>

EFI_STATUS EFIAPI UefiMain(IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable);

EFI_STATUS EFIAPI UefiMain(IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable) {
  (VOID)ImageHandle;
  (VOID)SystemTable;
  return EFI_SUCCESS;
}

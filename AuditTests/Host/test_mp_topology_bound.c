/* Credits: ©DNMTechLabs / dnmtechlabs@gmail.com */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef NULL
#undef NULL
#endif
#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BaseLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Pi/PiMultiPhase.h>
#include <Protocol/MpService.h>

EFI_HANDLE gImageHandle = NULL;
EFI_SYSTEM_TABLE *gST = NULL;
EFI_BOOT_SERVICES *gBS = NULL;
EFI_GUID gEfiMpServiceProtocolGuid = EFI_MP_SERVICES_PROTOCOL_GUID;

static EFI_MP_SERVICES_PROTOCOL gMp;
static UINTN gProcessorInfoCalls;
static UINTN gTotalProcessors;
static UINTN gEnabledProcessors;
static BOOLEAN gDuplicateLocation;
static BOOLEAN gReverseOrder;
static UINT32 gCpuidTopologyScenario;

UINT32 EFIAPI AsmCpuidEx(
  IN UINT32 Index,
  IN UINT32 SubIndex,
  OUT UINT32 *Eax OPTIONAL,
  OUT UINT32 *Ebx OPTIONAL,
  OUT UINT32 *Ecx OPTIONAL,
  OUT UINT32 *Edx OPTIONAL
  ) {
  UINT32 LocalEax;
  UINT32 LocalEbx;
  UINT32 LocalEcx;
  UINT32 LocalEdx;

  LocalEax = 0U;
  LocalEbx = 0U;
  LocalEcx = 0U;
  LocalEdx = 0U;

  if (gCpuidTopologyScenario == 1U && Index == 0x1FU) {
    if (SubIndex == 0U) {
      LocalEbx = 2U;
      LocalEcx = (1U << 8);
    } else if (SubIndex == 1U) {
      LocalEbx = 8U;
      LocalEcx = (2U << 8) | 1U;
    } else if (SubIndex == 2U) {
      LocalEbx = 16U;
      LocalEcx = (5U << 8) | 2U;
    }
  } else if (gCpuidTopologyScenario == 2U && Index == 0x0BU) {
    if (SubIndex == 0U) {
      LocalEbx = 2U;
      LocalEcx = (1U << 8);
    } else if (SubIndex == 1U) {
      LocalEbx = 8U;
      LocalEcx = (2U << 8) | 1U;
    }
  } else if (gCpuidTopologyScenario == 3U && Index == 0x1FU) {
    if (SubIndex == 0U) {
      LocalEbx = 2U;
      LocalEcx = (1U << 8);
    } else if (SubIndex == 1U) {
      LocalEbx = 8U;
      LocalEcx = (2U << 8) | 1U;
    } else if (SubIndex == 2U) {
      LocalEbx = 15U;
      LocalEcx = (5U << 8) | 2U;
    }
  }

  if (Eax != NULL) *Eax = LocalEax;
  if (Ebx != NULL) *Ebx = LocalEbx;
  if (Ecx != NULL) *Ecx = LocalEcx;
  if (Edx != NULL) *Edx = LocalEdx;
  return Index;
}

VOID *EFIAPI AllocateZeroPool(IN UINTN AllocationSize) {
  return calloc(1, (size_t)AllocationSize);
}

VOID EFIAPI FreePool(IN JCONST VOID *Buffer) {
  free((void *)Buffer);
}

VOID *EFIAPI ZeroMem(OUT VOID *Buffer, IN UINTN Length) {
  return memset(Buffer, 0, (size_t)Length);
}

static EFI_STATUS EFIAPI TestLocateProtocol(
  IN EFI_GUID *Protocol,
  IN VOID *Registration OPTIONAL,
  OUT VOID **Interface
  ) {
  (void)Protocol;
  (void)Registration;
  if (Interface == NULL) return EFI_INVALID_PARAMETER;
  *Interface = &gMp;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI TestGetNumber(
  IN EFI_MP_SERVICES_PROTOCOL *This,
  OUT UINTN *NumberOfProcessors,
  OUT UINTN *NumberOfEnabledProcessors
  ) {
  (void)This;
  *NumberOfProcessors = gTotalProcessors;
  *NumberOfEnabledProcessors = gEnabledProcessors;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI TestGetInfo(
  IN EFI_MP_SERVICES_PROTOCOL *This,
  IN UINTN ProcessorNumber,
  OUT EFI_PROCESSOR_INFORMATION *ProcessorInfoBuffer
  ) {
  (void)This;
  gProcessorInfoCalls++;
  memset(ProcessorInfoBuffer, 0, sizeof(*ProcessorInfoBuffer));
  UINTN LocationIndex;

  ProcessorInfoBuffer->StatusFlag = PROCESSOR_ENABLED_BIT;
  ProcessorInfoBuffer->Location.Package = 0;
  LocationIndex = gReverseOrder ? (gTotalProcessors - 1U - ProcessorNumber) : ProcessorNumber;
  ProcessorInfoBuffer->Location.Core = gDuplicateLocation ? 0U : (UINT32)(LocationIndex / 2U);
  ProcessorInfoBuffer->Location.Thread = gDuplicateLocation ? 0U : (UINT32)(LocationIndex % 2U);
  return EFI_SUCCESS;
}

#include "../../BootLoaderSelectorPkg/BootLoaderSelector/HardwareInfo.c"

int main(void) {
  EFI_BOOT_SERVICES bootServices;
  UINT32 logical;
  UINT32 cores;

  memset(&bootServices, 0, sizeof(bootServices));
  memset(&gMp, 0, sizeof(gMp));
  bootServices.LocateProtocol = TestLocateProtocol;
  gBS = &bootServices;
  gMp.GetNumberOfProcessors = TestGetNumber;
  gMp.GetProcessorInfo = TestGetInfo;

  /* CPUID.1FH may expose hierarchy above Core (for example Die). The last
     valid domain is immediately below the implied package and therefore must
     drive this diagnostic package-level fallback count. */
  gCpuidTopologyScenario = 1U;
  logical = 0U;
  cores = 0U;
  assert(BlsReadCpuTopologyLeaf(0x1FU, &logical, &cores) == TRUE);
  assert(logical == 16U && cores == 8U);

  /* Preserve the legacy CPUID.0BH interpretation: the Core-domain EBX is the
     package-level count for this leaf. */
  gCpuidTopologyScenario = 2U;
  logical = 0U;
  cores = 0U;
  assert(BlsReadCpuTopologyLeaf(0x0BU, &logical, &cores) == TRUE);
  assert(logical == 8U && cores == 4U);

  /* Malformed hierarchy must fail closed without modifying caller outputs. */
  gCpuidTopologyScenario = 3U;
  logical = 77U;
  cores = 88U;
  assert(BlsReadCpuTopologyLeaf(0x1FU, &logical, &cores) == FALSE);
  assert(logical == 77U && cores == 88U);

  gTotalProcessors = 4097U;
  gEnabledProcessors = 4097U;
  gProcessorInfoCalls = 0;
  logical = 99U;
  cores = 99U;
  assert(BlsReadMpServicesTopology(&logical, &cores) == FALSE);
  assert(gProcessorInfoCalls == 0U);
  assert(logical == 99U && cores == 99U);

  gTotalProcessors = 4U;
  gEnabledProcessors = 4U;
  gProcessorInfoCalls = 0;
  gDuplicateLocation = FALSE;
  gReverseOrder = FALSE;
  logical = 0U;
  cores = 0U;
  assert(BlsReadMpServicesTopology(&logical, &cores) == TRUE);
  assert(gProcessorInfoCalls == 4U);
  assert(logical == 4U && cores == 2U);

  /* Exercise the maximum supported topology in reverse physical order. This
     forces the in-place O(n log n) normalization path and guards against a
     return to quadratic previous-location scans. */
  gTotalProcessors = BLS_MP_PROCESSOR_SCAN_MAX;
  gEnabledProcessors = BLS_MP_PROCESSOR_SCAN_MAX;
  gProcessorInfoCalls = 0U;
  gDuplicateLocation = FALSE;
  gReverseOrder = TRUE;
  logical = 0U;
  cores = 0U;
  assert(BlsReadMpServicesTopology(&logical, &cores) == TRUE);
  assert(gProcessorInfoCalls == BLS_MP_PROCESSOR_SCAN_MAX);
  assert(logical == (UINT32)BLS_MP_PROCESSOR_SCAN_MAX);
  assert(cores == (UINT32)(BLS_MP_PROCESSOR_SCAN_MAX / 2U));

  gTotalProcessors = 4U;
  gEnabledProcessors = 4U;
  gReverseOrder = FALSE;
  gDuplicateLocation = TRUE;
  gProcessorInfoCalls = 0;
  logical = 77U;
  cores = 77U;
  assert(BlsReadMpServicesTopology(&logical, &cores) == FALSE);
  assert(logical == 77U && cores == 77U);

  puts("mp_topology_bound: PASS");
  return 0;
}

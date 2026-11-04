/* Credits: ©DNMTechLabs / dnmtechlabs@gmail.com */
#if defined(__linux__) && defined(__x86_64__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__linux__) && defined(__x86_64__)
#include <sys/mman.h>
#endif

#ifdef NULL
#undef NULL
#endif
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Guid/SmBios.h>
#include <IndustryStandard/SmBios.h>
#include <Protocol/PciIo.h>
#include <Protocol/GraphicsOutput.h>
#include "../../BootLoaderSelectorPkg/BootLoaderSelector/Utils.h"

EFI_HANDLE gImageHandle = NULL;
EFI_SYSTEM_TABLE *gST = NULL;
EFI_BOOT_SERVICES *gBS = NULL;
EFI_GUID gEfiPciIoProtocolGuid = EFI_PCI_IO_PROTOCOL_GUID;
EFI_GUID gEfiGraphicsOutputProtocolGuid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
EFI_GUID gEfiDevicePathProtocolGuid = EFI_DEVICE_PATH_PROTOCOL_GUID;
EFI_GUID gEfiSmbios3TableGuid = SMBIOS3_TABLE_GUID;
EFI_GUID gEfiSmbiosTableGuid = SMBIOS_TABLE_GUID;

static UINTN gFreeCount;
static UINTN gAllocateCount;
static UINTN gFailAllocationCall;
static UINTN gGetMemoryMapCalls;
static UINTN gProtocolQueryCalls;
static UINTN gPciLocateCount;
static UINTN gDevicePathCalls;
static BOOLEAN gShortSuccessMemoryMap;
static BOOLEAN gConsoleGopSuccess;
static BOOLEAN gGpuDirectNoPathScenario;
static EFI_GRAPHICS_OUTPUT_PROTOCOL gConsoleGop;
static EFI_GRAPHICS_OUTPUT_PROTOCOL gWrongGop;
static EFI_HANDLE gConsoleHandle = (EFI_HANDLE)(UINTN)0x55U;
static EFI_HANDLE gActiveGopHandle = (EFI_HANDLE)(UINTN)0x56U;
static EFI_HANDLE gWrongGopHandle = (EFI_HANDLE)(UINTN)0x57U;
typedef struct {
  EFI_DEVICE_PATH_PROTOCOL Node;
  EFI_DEVICE_PATH_PROTOCOL End;
} TEST_DEVICE_PATH;
static TEST_DEVICE_PATH gActiveGopPath;
static TEST_DEVICE_PATH gWrongGopPath;
static UINTN gMemoryMapScenario;
static UINTN gMemoryMapBase;
static BOOLEAN gLocateFailureWithOut;
static BOOLEAN gFreedNonOwnedHandleBuffer;
#define NON_OWNED_HANDLE_BUFFER ((EFI_HANDLE *)(UINTN)0xBADC0FFEU)

enum {
  MemoryMapScenarioDefault = 0,
  MemoryMapScenarioContiguous,
  MemoryMapScenarioContiguousReversed,
  MemoryMapScenarioGap,
  MemoryMapScenarioMmio,
  MemoryMapScenarioMmioPort,
  MemoryMapScenarioUnusable,
  MemoryMapScenarioOversizedMetadata,
  MemoryMapScenarioSecondCallError,
  MemoryMapScenarioNoProgress,
  MemoryMapScenarioFutureDescriptorVersion,
  MemoryMapScenarioInvalidDescriptorVersion,
  MemoryMapScenarioSmbios3Prefix
};

BOOLEAN EFIAPI CompareGuid(IN CONST GUID *Guid1, IN CONST GUID *Guid2) {
  return (BOOLEAN)(memcmp(Guid1, Guid2, sizeof(*Guid1)) == 0);
}

INTN EFIAPI CompareMem(IN CONST VOID *DestinationBuffer, IN CONST VOID *SourceBuffer, IN UINTN Length) {
  return (INTN)memcmp(DestinationBuffer, SourceBuffer, (size_t)Length);
}

VOID *EFIAPI AllocateZeroPool(IN UINTN AllocationSize) {
  gAllocateCount++;
  if (gFailAllocationCall != 0U && gAllocateCount == gFailAllocationCall) return NULL;
  return calloc(1, (size_t)AllocationSize);
}

VOID EFIAPI FreePool(IN JCONST VOID *Buffer) {
  gFreeCount++;
  if (Buffer == NON_OWNED_HANDLE_BUFFER) {
    gFreedNonOwnedHandleBuffer = TRUE;
    return;
  }
  free((void *)Buffer);
}

VOID *EFIAPI ZeroMem(OUT VOID *Buffer, IN UINTN Length) {
  return memset(Buffer, 0, (size_t)Length);
}

VOID *EFIAPI CopyMem(OUT VOID *DestinationBuffer, IN CONST VOID *SourceBuffer, IN UINTN Length) {
  return memcpy(DestinationBuffer, SourceBuffer, (size_t)Length);
}

UINTN EFIAPI UnicodeSPrint(
  OUT CHAR16 *StartOfBuffer,
  IN UINTN BufferSize,
  IN CONST CHAR16 *FormatString,
  ...
  ) {
  (void)StartOfBuffer;
  (void)BufferSize;
  (void)FormatString;
  return 0;
}

EFI_DEVICE_PATH_PROTOCOL *EFIAPI DevicePathFromHandle(IN EFI_HANDLE Handle) {
  (void)Handle;
  gDevicePathCalls++;
  return NULL;
}

BOOLEAN EFIAPI IsDevicePathValid(IN CONST EFI_DEVICE_PATH_PROTOCOL *DevicePath, IN UINTN MaxSize) {
  if (gGpuDirectNoPathScenario &&
      (DevicePath == &gActiveGopPath.Node || DevicePath == &gWrongGopPath.Node)) {
    return (BOOLEAN)(MaxSize >= sizeof(TEST_DEVICE_PATH));
  }
  return FALSE;
}

UINTN EFIAPI GetDevicePathSize(IN CONST EFI_DEVICE_PATH_PROTOCOL *DevicePath) {
  if (gGpuDirectNoPathScenario &&
      (DevicePath == &gActiveGopPath.Node || DevicePath == &gWrongGopPath.Node)) {
    return sizeof(TEST_DEVICE_PATH);
  }
  return 0U;
}


INTN EFIAPI AsciiStriCmp(IN CONST CHAR8 *FirstString, IN CONST CHAR8 *SecondString) {
  UINT8 Left;
  UINT8 Right;

  if (FirstString == NULL || SecondString == NULL) return 1;
  while (*FirstString != '\0' || *SecondString != '\0') {
    Left = (UINT8)*FirstString;
    Right = (UINT8)*SecondString;
    if (Left >= 'A' && Left <= 'Z') Left = (UINT8)(Left + ('a' - 'A'));
    if (Right >= 'A' && Right <= 'Z') Right = (UINT8)(Right + ('a' - 'A'));
    if (Left != Right) return (Left < Right) ? -1 : 1;
    if (*FirstString != '\0') FirstString++;
    if (*SecondString != '\0') SecondString++;
  }
  return 0;
}

VOID BlsSafeStrCpy(OUT CHAR16 *Dst, IN UINTN DstChars, IN CONST CHAR16 *Src) {
  UINTN Index;
  if (Dst == NULL || DstChars == 0) return;
  if (Src == NULL) { Dst[0] = L'\0'; return; }
  for (Index = 0; Index + 1U < DstChars && Src[Index] != L'\0'; Index++) Dst[Index] = Src[Index];
  Dst[Index] = L'\0';
}

VOID BlsSafeStrCat(OUT CHAR16 *Dst, IN UINTN DstChars, IN CONST CHAR16 *Src) {
  UINTN DstIndex;
  UINTN SrcIndex;
  if (Dst == NULL || DstChars == 0 || Src == NULL) return;
  for (DstIndex = 0; DstIndex < DstChars && Dst[DstIndex] != L'\0'; DstIndex++) { }
  if (DstIndex >= DstChars) { Dst[DstChars - 1U] = L'\0'; return; }
  for (SrcIndex = 0; DstIndex + 1U < DstChars && Src[SrcIndex] != L'\0'; SrcIndex++) {
    Dst[DstIndex++] = Src[SrcIndex];
  }
  Dst[DstIndex] = L'\0';
}

BOOLEAN BlsStrEq(IN CONST CHAR16 *A, IN CONST CHAR16 *B) {
  UINTN Index;
  if (A == NULL || B == NULL) return FALSE;
  for (Index = 0; Index < 65535U && (A[Index] != L'\0' || B[Index] != L'\0'); Index++) {
    if (A[Index] != B[Index]) return FALSE;
  }
  return (BOOLEAN)(Index < 65535U);
}

VOID BlsAsciiToUnicode(OUT CHAR16 *Dst, IN UINTN DstChars, IN CONST CHAR8 *Src) {
  UINTN Index;
  if (Dst == NULL || DstChars == 0) return;
  if (Src == NULL) { Dst[0] = L'\0'; return; }
  for (Index = 0; Index + 1U < DstChars && Src[Index] != '\0'; Index++) Dst[Index] = (CHAR16)(UINT8)Src[Index];
  Dst[Index] = L'\0';
}

static EFI_STATUS EFIAPI TestLocateHandlePresence(
  IN EFI_LOCATE_SEARCH_TYPE SearchType,
  IN EFI_GUID *Protocol OPTIONAL,
  IN VOID *SearchKey OPTIONAL,
  IN OUT UINTN *BufferSize,
  OUT EFI_HANDLE *Buffer
  ) {
  (void)SearchType;
  (void)Protocol;
  (void)SearchKey;
  (void)BufferSize;
  (void)Buffer;
  return EFI_UNSUPPORTED;
}

EFI_STATUS BlsLocateProtocolHandlesBounded(
  IN EFI_GUID *Protocol,
  IN UINTN MaxHandles,
  OUT EFI_HANDLE **Handles,
  OUT UINTN *Count
  ) {
  if (Protocol == NULL || Handles == NULL || Count == NULL || MaxHandles == 0U) {
    return EFI_INVALID_PARAMETER;
  }
  *Handles = NULL;
  *Count = 0U;
  if (gLocateFailureWithOut) return EFI_DEVICE_ERROR;

  if (Protocol == &gEfiGraphicsOutputProtocolGuid) {
    if (!gGpuDirectNoPathScenario) return EFI_NOT_FOUND;
    if (2U > MaxHandles) return EFI_COMPROMISED_DATA;
    *Handles = AllocateZeroPool(2U * sizeof(**Handles));
    if (*Handles == NULL) return EFI_OUT_OF_RESOURCES;
    (*Handles)[0] = gWrongGopHandle;
    (*Handles)[1] = gActiveGopHandle;
    *Count = 2U;
    return EFI_SUCCESS;
  }

  if (gPciLocateCount == 0U) return EFI_NOT_FOUND;
  if (gPciLocateCount > MaxHandles) return EFI_COMPROMISED_DATA;
  *Handles = AllocateZeroPool(gPciLocateCount * sizeof(**Handles));
  if (*Handles == NULL) return EFI_OUT_OF_RESOURCES;
  (*Handles)[0] = (EFI_HANDLE)(UINTN)1;
  *Count = gPciLocateCount;
  return EFI_SUCCESS;
}

EFI_STATUS BlsGetProtocolInterface(
  IN EFI_HANDLE Handle, IN EFI_GUID *Protocol, OUT VOID **Interface
  ) {
  static EFI_GRAPHICS_OUTPUT_PROTOCOL Gop;
  gProtocolQueryCalls++;
  if (Interface == NULL) return EFI_INVALID_PARAMETER;
  *Interface = NULL;
  if (gGpuDirectNoPathScenario) {
    if (Protocol == &gEfiGraphicsOutputProtocolGuid) {
      if (Handle == gConsoleHandle || Handle == gActiveGopHandle) { *Interface = &gConsoleGop; return EFI_SUCCESS; }
      if (Handle == gWrongGopHandle) { *Interface = &gWrongGop; return EFI_SUCCESS; }
    }
    if (Protocol == &gEfiDevicePathProtocolGuid) {
      if (Handle == gActiveGopHandle) { *Interface = &gActiveGopPath.Node; return EFI_SUCCESS; }
      if (Handle == gWrongGopHandle) { *Interface = &gWrongGopPath.Node; return EFI_SUCCESS; }
      return EFI_UNSUPPORTED;
    }
    return EFI_UNSUPPORTED;
  }
  (void)Handle;
  if (gConsoleGopSuccess && Protocol == &gEfiGraphicsOutputProtocolGuid) { *Interface = &Gop; return EFI_SUCCESS; }
  return EFI_DEVICE_ERROR;
}

static EFI_STATUS EFIAPI TestGetMemoryMap(
  IN OUT UINTN *MemoryMapSize,
  OUT EFI_MEMORY_DESCRIPTOR *MemoryMap,
  OUT UINTN *MapKey,
  OUT UINTN *DescriptorSize,
  OUT UINT32 *DescriptorVersion
  ) {
  EFI_MEMORY_DESCRIPTOR descriptors[2];
  UINTN requiredSize;

  gGetMemoryMapCalls++;
  if (MemoryMapSize == NULL || MapKey == NULL || DescriptorSize == NULL || DescriptorVersion == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  *DescriptorSize = (gMemoryMapScenario == MemoryMapScenarioFutureDescriptorVersion)
                      ? (sizeof(EFI_MEMORY_DESCRIPTOR) + 16U)
                      : sizeof(EFI_MEMORY_DESCRIPTOR);
  *DescriptorVersion = (gMemoryMapScenario == MemoryMapScenarioFutureDescriptorVersion)
                         ? (EFI_MEMORY_DESCRIPTOR_VERSION + 1U)
                         : ((gMemoryMapScenario == MemoryMapScenarioInvalidDescriptorVersion)
                              ? 0U
                              : EFI_MEMORY_DESCRIPTOR_VERSION);
  *MapKey = 1U;

  if (gMemoryMapScenario == MemoryMapScenarioOversizedMetadata) {
    *MemoryMapSize = sizeof(EFI_MEMORY_DESCRIPTOR) * 4097U;
    return EFI_BUFFER_TOO_SMALL;
  }

  requiredSize = (gMemoryMapScenario == MemoryMapScenarioContiguous ||
                  gMemoryMapScenario == MemoryMapScenarioContiguousReversed ||
                  gMemoryMapScenario == MemoryMapScenarioGap)
                   ? sizeof(descriptors)
                   : *DescriptorSize;
  if (MemoryMap == NULL || *MemoryMapSize < requiredSize) {
    *MemoryMapSize = requiredSize;
    return EFI_BUFFER_TOO_SMALL;
  }
  if (gMemoryMapScenario == MemoryMapScenarioSecondCallError) {
    *MemoryMapSize = requiredSize;
    return EFI_DEVICE_ERROR;
  }
  if (gMemoryMapScenario == MemoryMapScenarioNoProgress) {
    *MemoryMapSize = requiredSize;
    return EFI_BUFFER_TOO_SMALL;
  }
  if (gShortSuccessMemoryMap) {
    *MemoryMapSize = 0U;
    return EFI_SUCCESS;
  }

  memset(descriptors, 0, sizeof(descriptors));
  descriptors[0].Type = (gMemoryMapScenario == MemoryMapScenarioMmio)
                          ? EfiMemoryMappedIO
                          : ((gMemoryMapScenario == MemoryMapScenarioMmioPort)
                               ? EfiMemoryMappedIOPortSpace
                               : ((gMemoryMapScenario == MemoryMapScenarioUnusable)
                                    ? EfiUnusableMemory
                                    : EfiBootServicesData));
  descriptors[0].PhysicalStart = 0U;
  descriptors[0].NumberOfPages = MAX_UINT64 / EFI_PAGE_SIZE;
  if (gMemoryMapScenario == MemoryMapScenarioSmbios3Prefix) {
    descriptors[0].PhysicalStart = (EFI_PHYSICAL_ADDRESS)gMemoryMapBase;
    descriptors[0].NumberOfPages = 2U;
  }

  if (gMemoryMapScenario == MemoryMapScenarioContiguous ||
      gMemoryMapScenario == MemoryMapScenarioContiguousReversed ||
      gMemoryMapScenario == MemoryMapScenarioGap) {
    descriptors[0].PhysicalStart = 0x1000U;
    descriptors[0].NumberOfPages = 1U;
    descriptors[1].Type = EfiBootServicesData;
    descriptors[1].PhysicalStart = (gMemoryMapScenario == MemoryMapScenarioGap) ? 0x3000U : 0x2000U;
    descriptors[1].NumberOfPages = 1U;
    if (gMemoryMapScenario == MemoryMapScenarioContiguousReversed) {
      EFI_MEMORY_DESCRIPTOR temporary;
      temporary = descriptors[0];
      descriptors[0] = descriptors[1];
      descriptors[1] = temporary;
    }
    memcpy(MemoryMap, descriptors, sizeof(descriptors));
  } else {
    memset(MemoryMap, 0, (size_t)requiredSize);
    memcpy(MemoryMap, descriptors, sizeof(EFI_MEMORY_DESCRIPTOR));
  }
  *MemoryMapSize = requiredSize;
  return EFI_SUCCESS;
}

#include "../../BootLoaderSelectorPkg/BootLoaderSelector/HardwareInfo.c"


static BOOLEAN TestFirmwareRangeIsMapped(IN UINTN Address, IN UINTN Length) {
  BLS_FIRMWARE_MEMORY_MAP Snapshot;
  BOOLEAN Mapped;

  if (!BlsFirmwareMemoryMapAcquire(&Snapshot)) return FALSE;
  Mapped = BlsFirmwareRangeIsMappedInMap(&Snapshot, Address, Length);
  BlsFirmwareMemoryMapRelease(&Snapshot);
  return Mapped;
}

static UINTN TestFirmwareMappedPrefixLength(IN UINTN Address, IN UINTN Length) {
  BLS_FIRMWARE_MEMORY_MAP Snapshot;
  UINTN Prefix;

  if (!BlsFirmwareMemoryMapAcquire(&Snapshot)) return 0U;
  Prefix = BlsFirmwareMappedPrefixLengthInMap(&Snapshot, Address, Length);
  BlsFirmwareMemoryMapRelease(&Snapshot);
  return Prefix;
}

static VOID *TestFindConfigTable(IN EFI_GUID *Guid) {
  BLS_FIRMWARE_MEMORY_MAP Snapshot;
  VOID *Table;

  if (!BlsFirmwareMemoryMapAcquire(&Snapshot)) return NULL;
  Table = BlsFindConfigTableInMap(Guid, &Snapshot);
  BlsFirmwareMemoryMapRelease(&Snapshot);
  return Table;
}


static VOID SetSmbios2Checksums(IN OUT SMBIOS_TABLE_ENTRY_POINT *Entry) {
  UINT8 *bytes;
  UINT8 sum;
  UINTN index;
  UINTN intermediateOffset;

  Entry->EntryPointStructureChecksum = 0U;
  Entry->IntermediateChecksum = 0U;
  bytes = (UINT8 *)Entry;
  intermediateOffset = OFFSET_OF(SMBIOS_TABLE_ENTRY_POINT, IntermediateAnchorString);
  sum = 0U;
  for (index = intermediateOffset; index < sizeof(*Entry); index++) {
    sum = (UINT8)(sum + bytes[index]);
  }
  Entry->IntermediateChecksum = (UINT8)(0x100U - (UINT32)sum);

  sum = 0U;
  for (index = 0U; index < Entry->EntryPointLength; index++) {
    sum = (UINT8)(sum + bytes[index]);
  }
  Entry->EntryPointStructureChecksum = (UINT8)(0x100U - (UINT32)sum);
}

static VOID SetSmbios3Checksum(IN OUT SMBIOS_TABLE_3_0_ENTRY_POINT *Entry) {
  UINT8 *bytes;
  UINT8 sum;
  UINTN index;

  Entry->EntryPointStructureChecksum = 0;
  bytes = (UINT8 *)Entry;
  sum = 0;
  for (index = 0; index < Entry->EntryPointLength; index++) sum = (UINT8)(sum + bytes[index]);
  Entry->EntryPointStructureChecksum = (UINT8)(0x100U - (UINT32)sum);
}

int main(void) {
  EFI_SYSTEM_TABLE systemTable;
  EFI_CONFIGURATION_TABLE configTable;
  EFI_GUID requestedGuid = { 0x12345678, 0x1234, 0x5678, { 1, 2, 3, 4, 5, 6, 7, 8 } };
  UINTN vendorMarker;
  SMBIOS_TABLE_3_0_ENTRY_POINT smbios3;
  SMBIOS_TABLE_ENTRY_POINT smbios2;
  EFI_BOOT_SERVICES bootServices;
  BLS_HARDWARE_INFO info;

  memset(&systemTable, 0, sizeof(systemTable));
  gST = &systemTable;
  memset(&bootServices, 0, sizeof(bootServices));
  bootServices.GetMemoryMap = TestGetMemoryMap;
  gBS = &bootServices;
  gShortSuccessMemoryMap = TRUE;
  assert(TestFirmwareRangeIsMapped(0x1000U, 16U) == FALSE);
  gShortSuccessMemoryMap = FALSE;

  /* SMBIOS 3.5+ text is normative UTF-8 without a BOM. Verify strict UTF-8
     decoding, UCS-2-safe replacement of non-representable values,
     malformed-input rejection and the compatibility path retained for
     pre-3.5 firmware. */
  {
    CHAR16 Converted[16];
    CHAR16 Small[2];
    CONST CHAR8 Utf8Name[] = "M\xC3\xBCller";
    CONST CHAR8 Utf8Emoji[] = "\xF0\x9F\x98\x80";
    CONST CHAR8 InvalidOverlong[] = "\xC0\xAF";
    CONST CHAR8 InvalidContinuation[] = "\x80";
    CONST CHAR8 InvalidTruncated[] = "\xE2\x82";
    CONST CHAR8 InvalidSurrogate[] = "\xED\xA0\x80";
    CONST CHAR8 InvalidTooHigh[] = "\xF4\x90\x80\x80";
    CONST CHAR8 UefiReservedPrivateUse[] = "\xEF\x80\x80"; /* U+F000 */
    CONST CHAR8 BomPrefixed[] = "\xEF\xBB\xBFName";
    CONST CHAR8 InvalidAfterPrefix[] = "A\xC0\xAF";
    CONST CHAR8 LegacyEightBit[] = "\xE9";

    assert(BlsSmbiosStringToUnicode(
             Converted, ARRAY_SIZE(Converted), Utf8Name, sizeof(Utf8Name) - 1U, 3U, 9U) == TRUE);
    assert(Converted[0] == L'M');
    assert(Converted[1] == (CHAR16)0x00FCU);
    assert(Converted[2] == L'l');
    assert(Converted[5] == L'r');
    assert(Converted[6] == L'\0');

    assert(BlsSmbiosStringToUnicode(
             Converted, ARRAY_SIZE(Converted), Utf8Emoji, sizeof(Utf8Emoji) - 1U, 3U, 9U) == TRUE);
    assert(Converted[0] == (CHAR16)0xFFFDU);
    assert(Converted[1] == L'\0');

    assert(BlsSmbiosStringToUnicode(
             Converted, ARRAY_SIZE(Converted), UefiReservedPrivateUse,
             sizeof(UefiReservedPrivateUse) - 1U, 3U, 9U) == TRUE);
    assert(Converted[0] == (CHAR16)0xFFFDU);
    assert(Converted[1] == L'\0');

    {
      CONST CHAR8 TruncatedSupplementary[] = "A\xF0\x9F\x98\x80" "B";
      CHAR16 Truncated[3];

      assert(BlsSmbiosStringToUnicode(
               Truncated, ARRAY_SIZE(Truncated), TruncatedSupplementary,
               sizeof(TruncatedSupplementary) - 1U, 3U, 9U) == TRUE);
      assert(Truncated[0] == L'A');
      assert(Truncated[1] == (CHAR16)0xFFFDU);
      assert(Truncated[2] == L'\0');
    }

    assert(BlsSmbiosStringToUnicode(
             Converted, ARRAY_SIZE(Converted), InvalidOverlong,
             sizeof(InvalidOverlong) - 1U, 3U, 9U) == FALSE);
    assert(Converted[0] == L'\0');
    assert(BlsSmbiosStringToUnicode(
             Converted, ARRAY_SIZE(Converted), InvalidContinuation,
             sizeof(InvalidContinuation) - 1U, 3U, 9U) == FALSE);
    assert(Converted[0] == L'\0');
    assert(BlsSmbiosStringToUnicode(
             Converted, ARRAY_SIZE(Converted), InvalidTruncated,
             sizeof(InvalidTruncated) - 1U, 3U, 9U) == FALSE);
    assert(Converted[0] == L'\0');
    assert(BlsSmbiosStringToUnicode(
             Converted, ARRAY_SIZE(Converted), InvalidSurrogate,
             sizeof(InvalidSurrogate) - 1U, 3U, 9U) == FALSE);
    assert(Converted[0] == L'\0');
    assert(BlsSmbiosStringToUnicode(
             Converted, ARRAY_SIZE(Converted), InvalidTooHigh,
             sizeof(InvalidTooHigh) - 1U, 3U, 9U) == FALSE);
    assert(Converted[0] == L'\0');
    assert(BlsSmbiosStringToUnicode(
             Converted, ARRAY_SIZE(Converted), BomPrefixed,
             sizeof(BomPrefixed) - 1U, 3U, 9U) == FALSE);
    assert(Converted[0] == L'\0');

    /* Do not stop validation merely because the destination is full. */
    assert(BlsSmbiosStringToUnicode(
             Small, ARRAY_SIZE(Small), InvalidAfterPrefix,
             sizeof(InvalidAfterPrefix) - 1U, 3U, 9U) == FALSE);
    assert(Small[0] == L'\0');

    assert(BlsSmbiosStringToUnicode(
             Converted, ARRAY_SIZE(Converted), LegacyEightBit,
             sizeof(LegacyEightBit) - 1U, 3U, 4U) == TRUE);
    assert(Converted[0] == (CHAR16)0x00E9U);
    assert(Converted[1] == L'\0');
  }

  gMemoryMapScenario = MemoryMapScenarioContiguous;
  assert(TestFirmwareRangeIsMapped(0x1800U, 0x1000U) == TRUE);
  gMemoryMapScenario = MemoryMapScenarioContiguousReversed;
  assert(TestFirmwareRangeIsMapped(0x1800U, 0x1000U) == TRUE);
  gMemoryMapScenario = MemoryMapScenarioGap;
  assert(TestFirmwareRangeIsMapped(0x1800U, 0x2000U) == FALSE);

  /* MMIO/port-space/unusable descriptors are not safe evidence
     that a firmware table can be dereferenced as ordinary memory. */
  gMemoryMapScenario = MemoryMapScenarioMmio;
  assert(TestFirmwareRangeIsMapped(0x1000U, 16U) == FALSE);
  gMemoryMapScenario = MemoryMapScenarioMmioPort;
  assert(TestFirmwareRangeIsMapped(0x1000U, 16U) == FALSE);
  gMemoryMapScenario = MemoryMapScenarioUnusable;
  assert(TestFirmwareRangeIsMapped(0x1000U, 16U) == FALSE);

  /* Reject absurd firmware metadata before allocating the advertised map. */
  gMemoryMapScenario = MemoryMapScenarioOversizedMetadata;
  gAllocateCount = 0U;
  gGetMemoryMapCalls = 0U;
  assert(TestFirmwareRangeIsMapped(0x1000U, 16U) == FALSE);
  assert(gGetMemoryMapCalls == 1U);
  assert(gAllocateCount == 0U);

  /* If firmware fails after the sizing call, the consolidated snapshot must
     release the pool buffer exactly once and fail closed. */
  gMemoryMapScenario = MemoryMapScenarioSecondCallError;
  gAllocateCount = 0U;
  gFreeCount = 0U;
  gGetMemoryMapCalls = 0U;
  assert(TestFirmwareRangeIsMapped(0x1000U, 16U) == FALSE);
  assert(gGetMemoryMapCalls == 2U);
  assert(gAllocateCount == 1U);
  assert(gFreeCount == 1U);

  /* EFI_BUFFER_TOO_SMALL means the supplied buffer is too small. Firmware
     that repeats it while reporting a required size already covered by our
     allocation is non-progressing; fail without reallocating/retrying. */
  gMemoryMapScenario = MemoryMapScenarioNoProgress;
  gAllocateCount = 0U;
  gFreeCount = 0U;
  gGetMemoryMapCalls = 0U;
  assert(TestFirmwareRangeIsMapped(0x1000U, 16U) == FALSE);
  assert(gGetMemoryMapCalls == 2U);
  assert(gAllocateCount == 1U);
  assert(gFreeCount == 1U);

  /* UEFI requires future EFI_MEMORY_DESCRIPTOR revisions to retain the
     current prefix and requires consumers to stride by DescriptorSize. */
  gMemoryMapScenario = MemoryMapScenarioFutureDescriptorVersion;
  gAllocateCount = 0U;
  gFreeCount = 0U;
  gGetMemoryMapCalls = 0U;
  assert(TestFirmwareRangeIsMapped(0x1000U, 16U) == TRUE);
  assert(gGetMemoryMapCalls == 2U);
  assert(gAllocateCount == 1U);
  assert(gFreeCount == 1U);

  /* A revision older than the prefix this build understands is invalid. */
  gMemoryMapScenario = MemoryMapScenarioInvalidDescriptorVersion;
  gAllocateCount = 0U;
  gFreeCount = 0U;
  gGetMemoryMapCalls = 0U;
  assert(TestFirmwareRangeIsMapped(0x1000U, 16U) == FALSE);
  assert(gGetMemoryMapCalls == 2U);
  assert(gAllocateCount == 1U);
  assert(gFreeCount == 1U);

  gMemoryMapScenario = MemoryMapScenarioOversizedMetadata;
  memset(&info, 0, sizeof(info));
  gAllocateCount = 0U;
  gGetMemoryMapCalls = 0U;
  BlsReadMemoryMapFallback(&info);
  assert(gGetMemoryMapCalls == 1U);
  assert(gAllocateCount == 0U);
  assert(info.MemoryMB == 0U);
  gMemoryMapScenario = MemoryMapScenarioDefault;

  systemTable.NumberOfTableEntries = BLS_CONFIG_TABLE_SCAN_MAX + 1U;
  systemTable.ConfigurationTable = (EFI_CONFIGURATION_TABLE *)(UINTN)1;
  assert(TestFindConfigTable(&requestedGuid) == NULL);

  vendorMarker = 42U;
  memset(&configTable, 0, sizeof(configTable));
  configTable.VendorGuid = requestedGuid;
  configTable.VendorTable = &vendorMarker;
  systemTable.NumberOfTableEntries = 1U;
  systemTable.ConfigurationTable = &configTable;
  assert(TestFindConfigTable(&requestedGuid) == &vendorMarker);

  memset(&smbios3, 0, sizeof(smbios3));
  memcpy(smbios3.AnchorString, "_SM3_", sizeof(smbios3.AnchorString));
  smbios3.EntryPointLength = (UINT8)sizeof(smbios3);
  smbios3.MajorVersion = 3U;
  smbios3.MinorVersion = 9U;
  smbios3.EntryPointRevision = 1U;
  smbios3.TableAddress = 0x1000U;
  smbios3.TableMaximumSize = BLS_SMBIOS_TABLE_SCAN_MAX + 1U;
  SetSmbios3Checksum(&smbios3);
  assert(BlsSmbios3EntryPointIsValid(&smbios3, sizeof(smbios3)) == FALSE);

  smbios3.TableMaximumSize = 4096U;
  SetSmbios3Checksum(&smbios3);
  assert(BlsSmbios3EntryPointIsValid(&smbios3, sizeof(smbios3)) == TRUE);

  /* SMBIOS 3.x declares a minimum 18h EPS length, not a permanent exact
     length. A future revision may append checksum-covered bytes while keeping
     revision-1 prefix semantics. */
  {
    UINT8 ExtendedRaw[sizeof(SMBIOS_TABLE_3_0_ENTRY_POINT) + 1U];
    SMBIOS_TABLE_3_0_ENTRY_POINT *Extended;

    memset(ExtendedRaw, 0, sizeof(ExtendedRaw));
    Extended = (SMBIOS_TABLE_3_0_ENTRY_POINT *)(VOID *)ExtendedRaw;
    memcpy(Extended->AnchorString, "_SM3_", sizeof(Extended->AnchorString));
    Extended->EntryPointLength = (UINT8)sizeof(ExtendedRaw);
    Extended->MajorVersion = 3U;
    Extended->MinorVersion = 9U;
    Extended->EntryPointRevision = 1U;
    Extended->TableAddress = 0x1000U;
    Extended->TableMaximumSize = 4096U;
    ExtendedRaw[sizeof(SMBIOS_TABLE_3_0_ENTRY_POINT)] = 0xA5U;
    SetSmbios3Checksum(Extended);
    assert(BlsSmbios3EntryPointIsValid(Extended, sizeof(ExtendedRaw)) == TRUE);
    ExtendedRaw[sizeof(SMBIOS_TABLE_3_0_ENTRY_POINT)] ^= 1U;
    assert(BlsSmbios3EntryPointIsValid(Extended, sizeof(ExtendedRaw)) == FALSE);
  }
  smbios3.EntryPointRevision = 0U;
  SetSmbios3Checksum(&smbios3);
  assert(BlsSmbios3EntryPointIsValid(&smbios3, sizeof(smbios3)) == FALSE);
  smbios3.EntryPointRevision = 1U;
  smbios3.Reserved = 1U;
  SetSmbios3Checksum(&smbios3);
  assert(BlsSmbios3EntryPointIsValid(&smbios3, sizeof(smbios3)) == FALSE);
  smbios3.Reserved = 0U;
  smbios3.MajorVersion = 2U;
  SetSmbios3Checksum(&smbios3);
  assert(BlsSmbios3EntryPointIsValid(&smbios3, sizeof(smbios3)) == FALSE);

  /* SMBIOS 2.1 historically allowed the published 1Eh entry-point length typo;
     2.2 and later require the corrected 1Fh structure length. */
  memset(&smbios2, 0, sizeof(smbios2));
  memcpy(smbios2.AnchorString, "_SM_", sizeof(smbios2.AnchorString));
  memcpy(smbios2.IntermediateAnchorString, "_DMI_", sizeof(smbios2.IntermediateAnchorString));
  smbios2.EntryPointLength = (UINT8)sizeof(smbios2);
  smbios2.MajorVersion = 2U;
  smbios2.MinorVersion = 2U;
  smbios2.TableAddress = 0x1000U;
  smbios2.TableLength = 256U;
  smbios2.MaxStructureSize = 64U;
  SetSmbios2Checksums(&smbios2);
  assert(BlsSmbios2EntryPointIsValid(&smbios2, sizeof(smbios2)) == TRUE);

  /* Current SMBIOS conformance rules permit a 32-bit EPS length greater than
     1Fh for SMBIOS 3.7+ implementations. Preserve the exact historical rules
     for earlier versions while accepting checksum-covered future extensions. */
  {
    UINT8 Extended2Raw[sizeof(SMBIOS_TABLE_ENTRY_POINT) + 1U];
    SMBIOS_TABLE_ENTRY_POINT *Extended2;

    memset(Extended2Raw, 0, sizeof(Extended2Raw));
    Extended2 = (SMBIOS_TABLE_ENTRY_POINT *)(VOID *)Extended2Raw;
    memcpy(Extended2->AnchorString, "_SM_", sizeof(Extended2->AnchorString));
    memcpy(Extended2->IntermediateAnchorString, "_DMI_", sizeof(Extended2->IntermediateAnchorString));
    Extended2->EntryPointLength = (UINT8)sizeof(Extended2Raw);
    Extended2->MajorVersion = 3U;
    Extended2->MinorVersion = 9U;
    Extended2->TableAddress = 0x1000U;
    Extended2->TableLength = 256U;
    Extended2->MaxStructureSize = 64U;
    Extended2Raw[sizeof(SMBIOS_TABLE_ENTRY_POINT)] = 0xA5U;
    SetSmbios2Checksums(Extended2);
    assert(BlsSmbios2EntryPointIsValid(Extended2, sizeof(Extended2Raw)) == TRUE);
    assert(BlsSmbios2EntryPointIsValid(Extended2, sizeof(*Extended2)) == FALSE);
    Extended2->MinorVersion = 6U;
    SetSmbios2Checksums(Extended2);
    assert(BlsSmbios2EntryPointIsValid(Extended2, sizeof(Extended2Raw)) == FALSE);
    Extended2->MinorVersion = 9U;
    SetSmbios2Checksums(Extended2);
    Extended2Raw[sizeof(SMBIOS_TABLE_ENTRY_POINT)] ^= 1U;
    assert(BlsSmbios2EntryPointIsValid(Extended2, sizeof(Extended2Raw)) == FALSE);
  }
  smbios2.EntryPointLength = 0x1EU;
  smbios2.MinorVersion = 1U;
  SetSmbios2Checksums(&smbios2);
  assert(BlsSmbios2EntryPointIsValid(&smbios2, sizeof(smbios2)) == TRUE);
  smbios2.MinorVersion = 2U;
  SetSmbios2Checksums(&smbios2);
  assert(BlsSmbios2EntryPointIsValid(&smbios2, sizeof(smbios2)) == FALSE);
  smbios2.EntryPointLength = (UINT8)sizeof(smbios2);
  smbios2.EntryPointRevision = 1U;
  SetSmbios2Checksums(&smbios2);
  assert(BlsSmbios2EntryPointIsValid(&smbios2, sizeof(smbios2)) == FALSE);
  smbios2.EntryPointRevision = 0U;
  smbios2.FormattedArea[2] = 1U;
  SetSmbios2Checksums(&smbios2);
  assert(BlsSmbios2EntryPointIsValid(&smbios2, sizeof(smbios2)) == FALSE);
  smbios2.FormattedArea[2] = 0U;
  smbios2.MaxStructureSize = 0U;
  SetSmbios2Checksums(&smbios2);
  assert(BlsSmbios2EntryPointIsValid(&smbios2, sizeof(smbios2)) == FALSE);
  smbios2.MaxStructureSize = 257U;
  SetSmbios2Checksums(&smbios2);
  assert(BlsSmbios2EntryPointIsValid(&smbios2, sizeof(smbios2)) == FALSE);

  /* The SMBIOS 2.x Maximum Structure Size covers the formatted bytes plus
     string-set. A structure larger than that advertised bound must fail closed. */
  {
    UINT8 RawStructure[sizeof(SMBIOS_STRUCTURE) + 8U];
    SMBIOS_STRUCTURE *Hdr;
    SMBIOS_STRUCTURE *NextHdr;

    memset(RawStructure, 0, sizeof(RawStructure));
    Hdr = (SMBIOS_STRUCTURE *)(VOID *)RawStructure;
    Hdr->Type = SMBIOS_TYPE_SYSTEM_INFORMATION;
    Hdr->Length = (UINT8)sizeof(*Hdr);
    NextHdr = (SMBIOS_STRUCTURE *)(VOID *)(RawStructure + sizeof(RawStructure));
    assert(BlsSmbiosStructureFitsMaximum(Hdr, NextHdr, sizeof(RawStructure)) == TRUE);
    assert(BlsSmbiosStructureFitsMaximum(Hdr, NextHdr, sizeof(RawStructure) - 1U) == FALSE);
  }

  /* Accepted SMBIOS tables are version 2.1 or later, where 0xFF00 through
     0xFFFF are reserved and therefore cannot identify a structure. */
  {
    UINT8 SeenHandles[BLS_SMBIOS_HANDLE_BITMAP_BYTES];

    memset(SeenHandles, 0, sizeof(SeenHandles));
    assert(BlsSmbiosMarkHandleUnique(SeenHandles, 0xFEFFU) == TRUE);
    assert(BlsSmbiosMarkHandleUnique(SeenHandles, 0xFEFFU) == FALSE);
    assert(BlsSmbiosMarkHandleUnique(SeenHandles, 0xFF00U) == FALSE);
    assert(BlsSmbiosMarkHandleUnique(SeenHandles, 0xFFFEU) == FALSE);
    assert(BlsSmbiosMarkHandleUnique(SeenHandles, 0xFFFFU) == FALSE);
  }

  /* Type 4 extended counts have explicit consistency and reserved-value rules. */
  {
    UINT32 CountValue;
    assert(BlsDecodeSmbiosProcessorCount(0xFFU, FALSE, 0U, &CountValue) == TRUE);
    assert(CountValue == 255U);
    assert(BlsDecodeSmbiosProcessorCount(8U, TRUE, 8U, &CountValue) == TRUE);
    assert(CountValue == 8U);
    assert(BlsDecodeSmbiosProcessorCount(8U, TRUE, 16U, &CountValue) == FALSE);
    assert(BlsDecodeSmbiosProcessorCount(0xFFU, TRUE, 256U, &CountValue) == TRUE);
    assert(CountValue == 256U);
    assert(BlsDecodeSmbiosProcessorCount(8U, TRUE, 256U, &CountValue) == FALSE);
    assert(BlsDecodeSmbiosProcessorCount(0xFFU, TRUE, 0xFFFFU, &CountValue) == FALSE);
  }

  /* SMBIOS 3.6+ Type 4 Thread Enabled reports the runnable thread count.
     The bundled header predates that field, so validate its wire-offset helper
     directly, including version, unknown/reserved and consistency gates. */
  {
    UINT8 RawType4[BLS_SMBIOS_TYPE4_THREAD_ENABLED_OFFSET + sizeof(UINT16)];
    SMBIOS_TABLE_TYPE4 *Type4;
    UINT16 EnabledWire;
    UINT32 EnabledThreads;

    memset(RawType4, 0, sizeof(RawType4));
    Type4 = (SMBIOS_TABLE_TYPE4 *)(VOID *)RawType4;
    Type4->Hdr.Length = (UINT8)sizeof(RawType4);
    EnabledWire = 12U;
    memcpy(RawType4 + BLS_SMBIOS_TYPE4_THREAD_ENABLED_OFFSET, &EnabledWire, sizeof(EnabledWire));
    assert(BlsSmbiosEnabledThreadCount(Type4, 3U, 5U, 16U, &EnabledThreads) == FALSE);
    assert(BlsSmbiosEnabledThreadCount(Type4, 3U, 6U, 16U, &EnabledThreads) == TRUE);
    assert(EnabledThreads == 12U);
    assert(BlsSmbiosEnabledThreadCount(Type4, 3U, 6U, 8U, &EnabledThreads) == FALSE);

    EnabledWire = 0U;
    memcpy(RawType4 + BLS_SMBIOS_TYPE4_THREAD_ENABLED_OFFSET, &EnabledWire, sizeof(EnabledWire));
    assert(BlsSmbiosEnabledThreadCount(Type4, 3U, 9U, 16U, &EnabledThreads) == FALSE);
    EnabledWire = 0xFFFFU;
    memcpy(RawType4 + BLS_SMBIOS_TYPE4_THREAD_ENABLED_OFFSET, &EnabledWire, sizeof(EnabledWire));
    assert(BlsSmbiosEnabledThreadCount(Type4, 3U, 9U, 16U, &EnabledThreads) == FALSE);
  }

  /* Type 17 extended size/speed fields are version-gated and reserve bit 31. */
  {
    SMBIOS_TABLE_TYPE17 Type17;
    memset(&Type17, 0, sizeof(Type17));
    Type17.Hdr.Length = (UINT8)sizeof(Type17);
    Type17.Size = 0x7FFFU;
    Type17.ExtendedSize = 16384U;
    assert(BlsSmbiosMemoryDeviceSizeMb(&Type17, 2U, 6U) == 0U);
    assert(BlsSmbiosMemoryDeviceSizeMb(&Type17, 2U, 7U) == 16384U);
    Type17.ExtendedSize = BIT31 | 16384U;
    assert(BlsSmbiosMemoryDeviceSizeMb(&Type17, 3U, 9U) == 0U);

    memset(&Type17, 0, sizeof(Type17));
    Type17.Hdr.Length = (UINT8)sizeof(Type17);
    Type17.Speed = 3200U;
    Type17.ConfiguredMemoryClockSpeed = 3600U;
    assert(BlsSmbiosMemoryDeviceSpeed(&Type17, 2U, 6U) == 3200U);
    assert(BlsSmbiosMemoryDeviceSpeed(&Type17, 2U, 7U) == 3600U);
    Type17.ConfiguredMemoryClockSpeed = 0xFFFFU;
    Type17.ExtendedConfiguredMemorySpeed = 70000U;
    assert(BlsSmbiosMemoryDeviceSpeed(&Type17, 3U, 2U) == 3200U);
    assert(BlsSmbiosMemoryDeviceSpeed(&Type17, 3U, 3U) == 70000U);
    Type17.ExtendedConfiguredMemorySpeed = BIT31 | 70000U;
    assert(BlsSmbiosMemoryDeviceSpeed(&Type17, 3U, 9U) == 3200U);
    assert(BlsSmbiosVersionAtLeast(3U, 0U, 3U, 1U) == FALSE);
    assert(BlsSmbiosVersionAtLeast(3U, 1U, 3U, 1U) == TRUE);
  }

  /* SMBIOS 3.x TableMaximumSize is an upper bound, not the actual table size.
     Keep only the page containing the real table mapped and advertise a larger
     maximum. The Type 127 terminator must still be parsed safely. */
  {
    UINT8 *Raw;
    UINT8 *Block;
    EFI_CONFIGURATION_TABLE *SmbiosConfig;
    SMBIOS_TABLE_3_0_ENTRY_POINT *SmbiosEntry;
    SMBIOS_TABLE_TYPE1 *Type1;
    SMBIOS_TABLE_TYPE4 *Type4;
    SMBIOS_TABLE_TYPE17 *Type17;
    SMBIOS_STRUCTURE *End;
    UINT8 *Strings;
    UINTN RawAddress;
    UINTN AlignedAddress;

    Raw = calloc(1, EFI_PAGE_SIZE * 4U);
    assert(Raw != NULL);
    RawAddress = (UINTN)Raw;
    AlignedAddress = (RawAddress + EFI_PAGE_SIZE - 1U) & ~((UINTN)EFI_PAGE_SIZE - 1U);
    Block = (UINT8 *)AlignedAddress;
    gMemoryMapBase = AlignedAddress;

    SmbiosConfig = (EFI_CONFIGURATION_TABLE *)(VOID *)(Block + 64U);
    SmbiosEntry = (SMBIOS_TABLE_3_0_ENTRY_POINT *)(VOID *)(Block + 256U);
    Type1 = (SMBIOS_TABLE_TYPE1 *)(VOID *)(Block + EFI_PAGE_SIZE);

    memset(SmbiosConfig, 0, sizeof(*SmbiosConfig));
    SmbiosConfig->VendorGuid = gEfiSmbios3TableGuid;
    SmbiosConfig->VendorTable = SmbiosEntry;
    systemTable.NumberOfTableEntries = 1U;
    systemTable.ConfigurationTable = SmbiosConfig;

    memset(SmbiosEntry, 0, sizeof(*SmbiosEntry));
    memcpy(SmbiosEntry->AnchorString, "_SM3_", sizeof(SmbiosEntry->AnchorString));
    SmbiosEntry->EntryPointLength = (UINT8)sizeof(*SmbiosEntry);
    SmbiosEntry->MajorVersion = 3U;
    SmbiosEntry->MinorVersion = 0U;
    SmbiosEntry->EntryPointRevision = 1U;
    SmbiosEntry->TableMaximumSize = EFI_PAGE_SIZE * 2U;
    SmbiosEntry->TableAddress = (UINT64)(UINTN)Type1;
    SetSmbios3Checksum(SmbiosEntry);

    memset(Type1, 0, sizeof(*Type1));
    Type1->Hdr.Type = SMBIOS_TYPE_SYSTEM_INFORMATION;
    Type1->Hdr.Length = (UINT8)sizeof(*Type1);
    Type1->Hdr.Handle = 0x0100U;
    Type1->Manufacturer = 1U;
    Type1->ProductName = 2U;
    Strings = (UINT8 *)Type1 + sizeof(*Type1);
    memcpy(Strings, "Maker", 6U);
    Strings += 6U;
    memcpy(Strings, "Product", 8U);
    Strings += 8U;
    *Strings++ = 0U;

    Type4 = (SMBIOS_TABLE_TYPE4 *)(VOID *)Strings;
    memset(Type4, 0, BLS_SMBIOS_TYPE4_THREAD_ENABLED_OFFSET + sizeof(UINT16));
    Type4->Hdr.Type = SMBIOS_TYPE_PROCESSOR_INFORMATION;
    Type4->Hdr.Length = (UINT8)(BLS_SMBIOS_TYPE4_THREAD_ENABLED_OFFSET + sizeof(UINT16));
    Type4->Hdr.Handle = 0x0400U;
    Type4->CurrentSpeed = 3600U;
    Type4->Status = BIT6;
    Type4->CoreCount = 8U;
    Type4->EnabledCoreCount = 6U;
    Type4->ThreadCount = 16U;
    Type4->CoreCount2 = 8U;
    Type4->EnabledCoreCount2 = 6U;
    Type4->ThreadCount2 = 16U;
    {
      UINT16 EnabledWire;
      EnabledWire = 12U;
      memcpy((UINT8 *)(VOID *)Type4 + BLS_SMBIOS_TYPE4_THREAD_ENABLED_OFFSET,
             &EnabledWire, sizeof(EnabledWire));
    }
    Strings = (UINT8 *)(VOID *)Type4 + Type4->Hdr.Length;
    *Strings++ = 0U;
    *Strings++ = 0U;

    Type17 = (SMBIOS_TABLE_TYPE17 *)(VOID *)Strings;
    memset(Type17, 0, sizeof(*Type17));
    Type17->Hdr.Type = SMBIOS_TYPE_MEMORY_DEVICE;
    Type17->Hdr.Length = (UINT8)sizeof(*Type17);
    Type17->Hdr.Handle = 0x1100U;
    Type17->Size = 16384U;
    Type17->MemoryType = MemoryTypeDdr4;
    Type17->ConfiguredMemoryClockSpeed = 3200U;
    Strings = (UINT8 *)Type17 + sizeof(*Type17);
    *Strings++ = 0U;
    *Strings++ = 0U;

    End = (SMBIOS_STRUCTURE *)(VOID *)Strings;
    memset(End, 0, sizeof(*End));
    End->Type = SMBIOS_TYPE_END_OF_TABLE;
    End->Length = (UINT8)sizeof(*End);
    End->Handle = 0x7F00U;
    Strings += sizeof(*End);
    *Strings++ = 0U;
    *Strings = 0U;

    gMemoryMapScenario = MemoryMapScenarioSmbios3Prefix;

    /* Allocation failures at either ownership boundary must leave the caller's
       state untouched and release only resources actually acquired. */
    {
      BLS_HARDWARE_INFO PreservedInfo;

      memset(&info, 0, sizeof(info));
      info.MemoryMB = 333U;
      info.Board[0] = L'R';
      info.Board[1] = L'\0';
      PreservedInfo = info;
      gGetMemoryMapCalls = 0U;
      gAllocateCount = 0U;
      gFreeCount = 0U;
      gFailAllocationCall = 1U;
      BlsReadSmbios(&info);
      assert(memcmp(&info, &PreservedInfo, sizeof(info)) == 0);
      assert(gGetMemoryMapCalls == 0U);
      assert(gAllocateCount == 1U);
      assert(gFreeCount == 0U);

      gGetMemoryMapCalls = 0U;
      gAllocateCount = 0U;
      gFreeCount = 0U;
      gFailAllocationCall = 2U;
      BlsReadSmbios(&info);
      assert(memcmp(&info, &PreservedInfo, sizeof(info)) == 0);
      assert(gGetMemoryMapCalls == 1U);
      assert(gAllocateCount == 2U);
      assert(gFreeCount == 1U);
      gFailAllocationCall = 0U;
    }

    memset(&info, 0, sizeof(info));
    gGetMemoryMapCalls = 0U;
    gAllocateCount = 0U;
    gFreeCount = 0U;
    BlsReadSmbios(&info);
    assert(info.Board[0] == L'M');
    assert(info.MemoryMB == 16384U);
    assert(info.MemoryMHz == 3200U);
    assert(info.MemorySpeedIsMtps == FALSE);
    /* One bounded memory-map snapshot serves configuration-table, entry-point
       and SMBIOS-table validation. A separate fixed 8 KiB transient bitmap
       enforces globally unique SMBIOS structure handles in O(1) per entry. */
    assert(gGetMemoryMapCalls == 2U);
    assert(gAllocateCount == 2U);
    assert(gFreeCount == 2U);

    /* Exercise the full discovery path with one forward-compatible EPS byte.
       The byte is inside the proven memory-map range and participates in the
       checksum; valid hardware data must remain discoverable. */
    SmbiosEntry->EntryPointLength = (UINT8)(sizeof(*SmbiosEntry) + 1U);
    ((UINT8 *)(VOID *)SmbiosEntry)[sizeof(*SmbiosEntry)] = 0x5AU;
    SetSmbios3Checksum(SmbiosEntry);
    memset(&info, 0, sizeof(info));
    gGetMemoryMapCalls = 0U;
    gAllocateCount = 0U;
    gFreeCount = 0U;
    BlsReadSmbios(&info);
    assert(info.Board[0] == L'M');
    assert(info.MemoryMB == 16384U);
    assert(gGetMemoryMapCalls == 2U);
    assert(gAllocateCount == 2U);
    assert(gFreeCount == 2U);
    SmbiosEntry->EntryPointLength = (UINT8)sizeof(*SmbiosEntry);
    ((UINT8 *)(VOID *)SmbiosEntry)[sizeof(*SmbiosEntry)] = 0U;
    SetSmbios3Checksum(SmbiosEntry);

    /* A forward-compatible declared length is accepted only when every byte is
       inside the memory-map-proven readable range. Keep the fixed 18h prefix at
       the exact end of the mapped region and put the extension one byte beyond
       it; discovery must fail closed without consuming the checksum byte. */
    {
      SMBIOS_TABLE_3_0_ENTRY_POINT *BoundaryEntry;
      BLS_HARDWARE_INFO PreservedInfo;

      BoundaryEntry = (SMBIOS_TABLE_3_0_ENTRY_POINT *)(VOID *)(
                        Block + (EFI_PAGE_SIZE * 2U) - sizeof(*BoundaryEntry));
      memset(BoundaryEntry, 0, sizeof(*BoundaryEntry) + 1U);
      memcpy(BoundaryEntry->AnchorString, "_SM3_", sizeof(BoundaryEntry->AnchorString));
      BoundaryEntry->EntryPointLength = (UINT8)(sizeof(*BoundaryEntry) + 1U);
      BoundaryEntry->MajorVersion = 3U;
      BoundaryEntry->MinorVersion = 9U;
      BoundaryEntry->EntryPointRevision = 1U;
      BoundaryEntry->TableMaximumSize = EFI_PAGE_SIZE;
      BoundaryEntry->TableAddress = (UINT64)(UINTN)Type1;
      ((UINT8 *)(VOID *)BoundaryEntry)[sizeof(*BoundaryEntry)] = 0x33U;
      SetSmbios3Checksum(BoundaryEntry);
      SmbiosConfig->VendorTable = BoundaryEntry;

      memset(&info, 0, sizeof(info));
      info.MemoryMB = 444U;
      info.Cores = 4U;
      info.Board[0] = L'B';
      info.Board[1] = L'\0';
      PreservedInfo = info;
      BlsReadSmbios(&info);
      assert(memcmp(&info, &PreservedInfo, sizeof(info)) == 0);

      SmbiosConfig->VendorTable = SmbiosEntry;
    }

    /* SMBIOS 3.1 changed the Type 17 speed unit from MHz to MT/s without
       changing the wire fields. Verify that the same table is labelled by the
       declared SMBIOS version rather than by heuristic interpretation. */
    SmbiosEntry->MinorVersion = 1U;
    SetSmbios3Checksum(SmbiosEntry);
    memset(&info, 0, sizeof(info));
    gGetMemoryMapCalls = 0U;
    gAllocateCount = 0U;
    gFreeCount = 0U;
    BlsReadSmbios(&info);
    assert(info.MemoryMB == 16384U);
    assert(info.MemoryMHz == 3200U);
    assert(info.MemorySpeedIsMtps == TRUE);
    assert(gGetMemoryMapCalls == 2U);
    assert(gAllocateCount == 2U);
    assert(gFreeCount == 2U);

    /* SMBIOS 3.6+ exposes the enabled thread count independently from socket
       capability. Prefer it for the fallback topology, but fail back to the
       established total-thread field when the enabled value is unknown,
       reserved or contradicts the total. */
    {
      UINT16 EnabledWire;

      SmbiosEntry->MinorVersion = 6U;
      SetSmbios3Checksum(SmbiosEntry);
      EnabledWire = 12U;
      memcpy((UINT8 *)(VOID *)Type4 + BLS_SMBIOS_TYPE4_THREAD_ENABLED_OFFSET,
             &EnabledWire, sizeof(EnabledWire));
      memset(&info, 0, sizeof(info));
      BlsReadSmbios(&info);
      assert(info.Cores == 6U);
      assert(info.LogicalProcessors == 12U);

      EnabledWire = 0U;
      memcpy((UINT8 *)(VOID *)Type4 + BLS_SMBIOS_TYPE4_THREAD_ENABLED_OFFSET,
             &EnabledWire, sizeof(EnabledWire));
      memset(&info, 0, sizeof(info));
      BlsReadSmbios(&info);
      assert(info.LogicalProcessors == 16U);

      EnabledWire = 17U;
      memcpy((UINT8 *)(VOID *)Type4 + BLS_SMBIOS_TYPE4_THREAD_ENABLED_OFFSET,
             &EnabledWire, sizeof(EnabledWire));
      memset(&info, 0, sizeof(info));
      BlsReadSmbios(&info);
      assert(info.LogicalProcessors == 16U);

      EnabledWire = 12U;
      memcpy((UINT8 *)(VOID *)Type4 + BLS_SMBIOS_TYPE4_THREAD_ENABLED_OFFSET,
             &EnabledWire, sizeof(EnabledWire));
      SmbiosEntry->MinorVersion = 1U;
      SetSmbios3Checksum(SmbiosEntry);
    }

    /* Full-table integration for the SMBIOS 3.5+ UTF-8 string contract.
       Valid multibyte manufacturer text must become UTF-16, while one malformed
       optional string is ignored without poisoning otherwise valid hardware
       data from the same table. */
    {
      UINT8 *ManufacturerString;

      ManufacturerString = (UINT8 *)Type1 + sizeof(*Type1);
      memcpy(ManufacturerString, "M\xC3\xBCll", 6U);
      SmbiosEntry->MinorVersion = 9U;
      SetSmbios3Checksum(SmbiosEntry);
      memset(&info, 0, sizeof(info));
      BlsReadSmbios(&info);
      assert(info.Board[0] == L'M');
      assert(info.Board[1] == (CHAR16)0x00FCU);
      assert(info.Board[2] == L'l');
      assert(info.Board[3] == L'l');
      assert(info.Board[4] == L' ');
      assert(info.Board[5] == L'P');
      assert(info.MemorySpeedIsMtps == TRUE);

      ManufacturerString[0] = (UINT8)'M';
      ManufacturerString[1] = 0xC0U;
      ManufacturerString[2] = 0xAFU;
      ManufacturerString[3] = (UINT8)'l';
      ManufacturerString[4] = (UINT8)'l';
      ManufacturerString[5] = 0U;
      memset(&info, 0, sizeof(info));
      BlsReadSmbios(&info);
      assert(info.Board[0] == L'P');
      assert(info.Board[1] == L'r');

      memcpy(ManufacturerString, "Maker", 6U);
      SmbiosEntry->MinorVersion = 1U;
      SetSmbios3Checksum(SmbiosEntry);
    }

    /* SMBIOS handles are table-wide identifiers and must be unique. A
       duplicate must invalidate the whole table rather than double-counting
       data from otherwise structurally valid records. */
    {
      BLS_HARDWARE_INFO PreservedInfo;
      UINT16 OriginalHandle;

      OriginalHandle = Type17->Hdr.Handle;
      Type17->Hdr.Handle = Type1->Hdr.Handle;
      memset(&info, 0, sizeof(info));
      info.MemoryMB = 555U;
      info.Cores = 5U;
      info.Board[0] = L'X';
      info.Board[1] = L'\0';
      PreservedInfo = info;
      BlsReadSmbios(&info);
      assert(memcmp(&info, &PreservedInfo, sizeof(info)) == 0);
      Type17->Hdr.Handle = OriginalHandle;
    }

    /* Reserved handles must invalidate the complete SMBIOS table just like a
       duplicate. Verify that no fields parsed before the bad structure leak
       into the caller's state. */
    {
      BLS_HARDWARE_INFO PreservedInfo;
      UINT16 OriginalHandle;

      OriginalHandle = Type17->Hdr.Handle;
      Type17->Hdr.Handle = SMBIOS_HANDLE_RESERVED_BEGIN;
      memset(&info, 0, sizeof(info));
      info.MemoryMB = 556U;
      info.Cores = 6U;
      info.Board[0] = L'W';
      info.Board[1] = L'\0';
      PreservedInfo = info;
      BlsReadSmbios(&info);
      assert(memcmp(&info, &PreservedInfo, sizeof(info)) == 0);
      Type17->Hdr.Handle = OriginalHandle;
    }

    /* A malformed/truncated SMBIOS table must be rejected transactionally.
       Parsed fields from earlier structures must never leak into Info when a
       later structure makes the table non-conformant. */
    {
      BLS_HARDWARE_INFO PreservedInfo;

      End->Type = 126U;
      memset(&info, 0, sizeof(info));
      info.MemoryMB = 777U;
      info.Cores = 7U;
      info.Board[0] = L'Z';
      info.Board[1] = L'\0';
      PreservedInfo = info;
      BlsReadSmbios(&info);
      assert(memcmp(&info, &PreservedInfo, sizeof(info)) == 0);
      End->Type = SMBIOS_TYPE_END_OF_TABLE;
    }

    /* Type 127 still owns a string-set terminator. Validate its final double
       NUL instead of accepting the EOT header before proving the structure is
       fully contained in the mapped table prefix. */
    {
      BLS_HARDWARE_INFO PreservedInfo;
      UINT8 *MappedEnd;
      UINT8 *Terminator;

      MappedEnd = (UINT8 *)(VOID *)Type1 + EFI_PAGE_SIZE;
      Terminator = (UINT8 *)(VOID *)End + sizeof(*End);
      assert(Terminator + 2U <= MappedEnd);
      memset(Terminator, 0xA5, (size_t)(MappedEnd - Terminator));

      memset(&info, 0, sizeof(info));
      info.MemoryMB = 888U;
      info.LogicalProcessors = 9U;
      info.Board[0] = L'Y';
      info.Board[1] = L'\0';
      PreservedInfo = info;
      BlsReadSmbios(&info);
      assert(memcmp(&info, &PreservedInfo, sizeof(info)) == 0);

      Terminator[0] = 0U;
      Terminator[1] = 0U;
    }

    assert(TestFirmwareMappedPrefixLength((UINTN)Type1, EFI_PAGE_SIZE * 2U) == EFI_PAGE_SIZE);
    assert(TestFirmwareRangeIsMapped((UINTN)Type1, EFI_PAGE_SIZE * 2U) == FALSE);
    free(Raw);
    gMemoryMapBase = 0U;
    gMemoryMapScenario = MemoryMapScenarioDefault;
  }

  /* DSP0134 permits 32-bit and 64-bit entry points to coexist and requires
     both to report the same major.minor version. If the preferred SMBIOS 3.x
     entry point is valid but its referenced table is malformed, a separately
     valid, same-version 32-bit subset must remain usable. This Linux/x64
     integration case reserves a real low-address mapping so the legacy 32-bit
     TableAddress can be dereferenced exactly as firmware would. */
#if defined(__linux__) && defined(__x86_64__)
  {
    UINT8 *Block;
    EFI_CONFIGURATION_TABLE *Tables;
    SMBIOS_TABLE_3_0_ENTRY_POINT *Smbios3Entry;
    SMBIOS_TABLE_ENTRY_POINT *Smbios2Entry;
    SMBIOS_STRUCTURE *Broken3Table;
    SMBIOS_TABLE_TYPE1 *LegacyType1;
    SMBIOS_STRUCTURE *LegacyEnd;
    UINT8 *Strings;
    UINT8 *LegacyTableEnd;
    UINTN LegacyType1Size;
    UINTN LegacyTableSize;

    Block = mmap(
              NULL,
              EFI_PAGE_SIZE * 2U,
              PROT_READ | PROT_WRITE,
              MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT,
              -1,
              0
              );
    assert(Block != MAP_FAILED);
    assert((UINTN)Block <= MAX_UINT32);
    memset(Block, 0, EFI_PAGE_SIZE * 2U);
    gMemoryMapBase = (UINTN)Block;
    gMemoryMapScenario = MemoryMapScenarioSmbios3Prefix;

    Tables = (EFI_CONFIGURATION_TABLE *)(VOID *)(Block + 64U);
    Smbios3Entry = (SMBIOS_TABLE_3_0_ENTRY_POINT *)(VOID *)(Block + 256U);
    Smbios2Entry = (SMBIOS_TABLE_ENTRY_POINT *)(VOID *)(Block + 512U);
    Broken3Table = (SMBIOS_STRUCTURE *)(VOID *)(Block + EFI_PAGE_SIZE);
    LegacyType1 = (SMBIOS_TABLE_TYPE1 *)(VOID *)(Block + EFI_PAGE_SIZE + 256U);

    Tables[0].VendorGuid = gEfiSmbios3TableGuid;
    Tables[0].VendorTable = Smbios3Entry;
    Tables[1].VendorGuid = gEfiSmbiosTableGuid;
    Tables[1].VendorTable = Smbios2Entry;
    systemTable.NumberOfTableEntries = 2U;
    systemTable.ConfigurationTable = Tables;

    memcpy(Smbios3Entry->AnchorString, "_SM3_", sizeof(Smbios3Entry->AnchorString));
    Smbios3Entry->EntryPointLength = (UINT8)sizeof(*Smbios3Entry);
    Smbios3Entry->MajorVersion = 3U;
    Smbios3Entry->MinorVersion = 9U;
    Smbios3Entry->EntryPointRevision = 1U;
    Smbios3Entry->TableMaximumSize = 64U;
    Smbios3Entry->TableAddress = (UINT64)(UINTN)Broken3Table;
    SetSmbios3Checksum(Smbios3Entry);
    Broken3Table->Type = SMBIOS_TYPE_SYSTEM_INFORMATION;
    Broken3Table->Length = 1U; /* Invalid: shorter than SMBIOS_STRUCTURE. */
    Broken3Table->Handle = 0x0300U;

    LegacyType1->Hdr.Type = SMBIOS_TYPE_SYSTEM_INFORMATION;
    LegacyType1->Hdr.Length = (UINT8)sizeof(*LegacyType1);
    LegacyType1->Hdr.Handle = 0x0100U;
    LegacyType1->Manufacturer = 1U;
    LegacyType1->ProductName = 2U;
    Strings = (UINT8 *)(VOID *)LegacyType1 + sizeof(*LegacyType1);
    memcpy(Strings, "Fallback", 9U);
    Strings += 9U;
    memcpy(Strings, "Legacy", 7U);
    Strings += 7U;
    *Strings++ = 0U;
    LegacyType1Size = (UINTN)Strings - (UINTN)LegacyType1;

    LegacyEnd = (SMBIOS_STRUCTURE *)(VOID *)Strings;
    LegacyEnd->Type = SMBIOS_TYPE_END_OF_TABLE;
    LegacyEnd->Length = (UINT8)sizeof(*LegacyEnd);
    LegacyEnd->Handle = 0x7F00U;
    LegacyTableEnd = (UINT8 *)(VOID *)LegacyEnd + sizeof(*LegacyEnd);
    *LegacyTableEnd++ = 0U;
    *LegacyTableEnd++ = 0U;
    LegacyTableSize = (UINTN)LegacyTableEnd - (UINTN)LegacyType1;

    memcpy(Smbios2Entry->AnchorString, "_SM_", sizeof(Smbios2Entry->AnchorString));
    memcpy(Smbios2Entry->IntermediateAnchorString, "_DMI_", sizeof(Smbios2Entry->IntermediateAnchorString));
    Smbios2Entry->EntryPointLength = (UINT8)sizeof(*Smbios2Entry);
    Smbios2Entry->MajorVersion = 3U;
    Smbios2Entry->MinorVersion = 9U;
    Smbios2Entry->EntryPointRevision = 0U;
    Smbios2Entry->MaxStructureSize = (UINT16)LegacyType1Size;
    Smbios2Entry->TableLength = (UINT16)LegacyTableSize;
    Smbios2Entry->TableAddress = (UINT32)(UINTN)LegacyType1;
    Smbios2Entry->NumberOfSmbiosStructures = 2U;
    SetSmbios2Checksums(Smbios2Entry);

    memset(&info, 0, sizeof(info));
    info.MemoryMB = 123U;
    gGetMemoryMapCalls = 0U;
    gAllocateCount = 0U;
    gFreeCount = 0U;
    BlsReadSmbios(&info);
    assert(info.Board[0] == L'F');
    assert(info.Board[8] == L' ');
    assert(info.Board[9] == L'L');
    assert(info.MemoryMB == 123U);
    assert(gGetMemoryMapCalls == 2U);
    assert(gAllocateCount == 2U);
    assert(gFreeCount == 2U);

    /* A mismatched coexisting legacy entry point is not the conforming subset
       promised by DSP0134 and must not be used to recover from a broken 3.x
       table. Preserve the caller state rather than parsing a different schema. */
    Smbios2Entry->MajorVersion = 2U;
    Smbios2Entry->MinorVersion = 8U;
    SetSmbios2Checksums(Smbios2Entry);
    memset(&info, 0, sizeof(info));
    info.MemoryMB = 456U;
    info.LogicalProcessors = 7U;
    info.Board[0] = L'Q';
    info.Board[1] = L'\0';
    {
      BLS_HARDWARE_INFO PreservedInfo;
      PreservedInfo = info;
      BlsReadSmbios(&info);
      assert(memcmp(&info, &PreservedInfo, sizeof(info)) == 0);
    }

    assert(munmap(Block, EFI_PAGE_SIZE * 2U) == 0);
    systemTable.NumberOfTableEntries = 0U;
    systemTable.ConfigurationTable = NULL;
    gMemoryMapBase = 0U;
    gMemoryMapScenario = MemoryMapScenarioDefault;
  }
#endif

  assert(BlsSmbiosStructureScanLimit(4096U, 8U) == 8U);
  assert(BlsSmbiosStructureScanLimit(BLS_SMBIOS_TABLE_SCAN_MAX, 0U) ==
         BLS_SMBIOS_STRUCTURE_SCAN_MAX);
  assert(BlsSmbiosStructureScanLimit(4096U, BLS_SMBIOS_STRUCTURE_SCAN_MAX + 1U) ==
         BLS_SMBIOS_STRUCTURE_SCAN_MAX);

  /* A structure whose double-NUL ends exactly at the table limit has a valid
     one-past-end successor. This is required for legacy SMBIOS 2.1 tables
     that predate the Type 127 end-of-table structure. */
  {
    UINT8 RawStructure[sizeof(SMBIOS_STRUCTURE) + 2U];
    SMBIOS_STRUCTURE *Hdr;
    SMBIOS_STRUCTURE *Limit;

    memset(RawStructure, 0, sizeof(RawStructure));
    Hdr = (SMBIOS_STRUCTURE *)(VOID *)RawStructure;
    Hdr->Type = SMBIOS_TYPE_SYSTEM_INFORMATION;
    Hdr->Length = (UINT8)sizeof(*Hdr);
    Limit = (SMBIOS_STRUCTURE *)(VOID *)(RawStructure + sizeof(RawStructure));
    assert(BlsNextSmbiosStruct(Hdr, Limit) == Limit);
  }

  memset(&bootServices, 0, sizeof(bootServices));
  bootServices.LocateHandle = TestLocateHandlePresence;
  bootServices.GetMemoryMap = TestGetMemoryMap;
  gBS = &bootServices;
  memset(&info, 0, sizeof(info));
  /* Console GOP discovery must query the console device path only once even
     when GOP succeeds but no valid device path is exposed by firmware. */
  systemTable.ConsoleOutHandle = (EFI_HANDLE)(UINTN)0x55U;
  gConsoleGopSuccess = TRUE;
  gDevicePathCalls = 0U;
  gProtocolQueryCalls = 0U;
  assert(BlsFindConsoleGopDevicePath() == NULL);
  assert(gDevicePathCalls == 0U);
  assert(gProtocolQueryCalls == 2U);
  systemTable.ConsoleOutHandle = NULL;
  gConsoleGopSuccess = FALSE;

  /* If ConsoleOut exposes GOP directly but no Device Path, the GOP interface
     identity still identifies the active framebuffer. With multiple GOPs, do
     not discard that identity and fall back to an arbitrary PCI display device. */
  memset(&gActiveGopPath, 0, sizeof(gActiveGopPath));
  memset(&gWrongGopPath, 0, sizeof(gWrongGopPath));
  gActiveGopPath.Node.Type = HARDWARE_DEVICE_PATH;
  gActiveGopPath.Node.SubType = HW_PCI_DP;
  gActiveGopPath.Node.Length[0] = (UINT8)sizeof(EFI_DEVICE_PATH_PROTOCOL);
  gWrongGopPath.Node = gActiveGopPath.Node;
  gActiveGopPath.End.Type = END_DEVICE_PATH_TYPE;
  gActiveGopPath.End.SubType = END_ENTIRE_DEVICE_PATH_SUBTYPE;
  gActiveGopPath.End.Length[0] = (UINT8)sizeof(EFI_DEVICE_PATH_PROTOCOL);
  gWrongGopPath.End = gActiveGopPath.End;
  gGpuDirectNoPathScenario = TRUE;
  systemTable.ConsoleOutHandle = gConsoleHandle;
  assert(BlsFindConsoleGopDevicePath() == &gActiveGopPath.Node);
  gGpuDirectNoPathScenario = FALSE;
  systemTable.ConsoleOutHandle = NULL;

  /* A failed bounded enumeration transfers no ownership and therefore must not
     trigger cleanup of a non-owned handle buffer. */
  gLocateFailureWithOut = TRUE;
  gFreedNonOwnedHandleBuffer = FALSE;
  gFreeCount = 0U;
  BlsReadGpuInfo(&info);
  assert(gFreedNonOwnedHandleBuffer == FALSE);
  assert(gFreeCount == 0U);
  assert(info.Gpu[0] == L'\0');
  gLocateFailureWithOut = FALSE;

  gPciLocateCount = BLS_PCI_HANDLE_SCAN_MAX + 1U;
  gFreeCount = 0;
  gProtocolQueryCalls = 0;
  BlsReadGpuInfo(&info);
  assert(gFreeCount == 0U);
  assert(gProtocolQueryCalls == 0U);
  assert(info.Gpu[0] == L'\0');

  puts("firmware_table_bounds: PASS");
  return 0;
}

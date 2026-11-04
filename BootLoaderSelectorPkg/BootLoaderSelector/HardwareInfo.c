/** @file
  BootLoaderSelector hardware information.
  Credits: ©DNMTechLabs / dnmtechlabs@gmail.com
**/
#include "HardwareInfo.h"
#include "Utils.h"
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DevicePathLib.h>
#include <Guid/SmBios.h>
#include <IndustryStandard/SmBios.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/PciIo.h>
#include <Pi/PiMultiPhase.h>
#include <Protocol/MpService.h>

#define BLS_MEMORY_MAP_ALLOCATION_MAX (64U * 1024U * 1024U)
#define BLS_MEMORY_MAP_RETRY_MAX      8U
#define BLS_MEMORY_DESCRIPTOR_SCAN_MAX 4096U
/* Defensive budgets for firmware-owned counts and table lengths. They are
   platform-agnostic and only reject metadata that would cause excessive work. */
#define BLS_MP_PROCESSOR_SCAN_MAX      4096U
#define BLS_CONFIG_TABLE_SCAN_MAX      4096U
#define BLS_PCI_HANDLE_SCAN_MAX        4096U
#define BLS_SMBIOS_TABLE_SCAN_MAX     (16U * 1024U * 1024U)
#define BLS_SMBIOS_STRUCTURE_SCAN_MAX 65536U
#define BLS_SMBIOS_HANDLE_BITMAP_BYTES (65536U / 8U)

static BOOLEAN BlsAsciiIsJunk(IN CONST CHAR8 *Str) {
  if (Str == NULL || Str[0] == '\0') return TRUE;
  if (AsciiStriCmp(Str, "To be filled by O.E.M.") == 0) return TRUE;
  if (AsciiStriCmp(Str, "Default string") == 0) return TRUE;
  if (AsciiStriCmp(Str, "System Product Name") == 0) return TRUE;
  if (AsciiStriCmp(Str, "System manufacturer") == 0) return TRUE;
  if (AsciiStriCmp(Str, "Base Board Product Name") == 0) return TRUE;
  if (AsciiStriCmp(Str, "Base Board Manufacturer") == 0) return TRUE;
  if (AsciiStriCmp(Str, "Unknown") == 0) return TRUE;
  if (AsciiStriCmp(Str, "None") == 0) return TRUE;
  if (AsciiStriCmp(Str, "N/A") == 0) return TRUE;
  return FALSE;
}

static BOOLEAN BlsChecksum8IsZero(IN CONST VOID *Buffer, IN UINTN BufferSize) {
  CONST UINT8 *Bytes;
  UINT8 Sum;
  UINTN Index;

  if (Buffer == NULL || BufferSize == 0) return FALSE;
  Bytes = (CONST UINT8 *)Buffer;
  Sum = 0;
  for (Index = 0; Index < BufferSize; Index++) {
    Sum = (UINT8)(Sum + Bytes[Index]);
  }
  return (BOOLEAN)(Sum == 0);
}

static BOOLEAN BlsMemoryMapDescriptorBudgetExceeded(IN UINTN MapSize, IN UINTN DescSize) {
  if (DescSize == 0) return TRUE;
  if (DescSize > MAX_UINTN / BLS_MEMORY_DESCRIPTOR_SCAN_MAX) return FALSE;
  return (BOOLEAN)(MapSize > DescSize * BLS_MEMORY_DESCRIPTOR_SCAN_MAX);
}

static BOOLEAN BlsFirmwareDescriptorIsReadable(IN UINT32 Type) {
  /* Configuration/SMBIOS pointers are dereferenced as ordinary memory. A
     firmware descriptor that represents MMIO, I/O port space or explicitly
     unusable memory is not proof that such a dereference is safe. Unknown
     memory types also fail closed through the EfiMaxMemoryType guard. Reserved
     memory remains accepted because legacy/firmware tables may legitimately
     live there while still being CPU-readable. */
  if (Type >= EfiMaxMemoryType) return FALSE;
  if (Type == EfiUnusableMemory || Type == EfiMemoryMappedIO ||
      Type == EfiMemoryMappedIOPortSpace) {
    return FALSE;
  }
  return TRUE;
}

typedef struct {
  EFI_MEMORY_DESCRIPTOR *Map;
  UINTN MapSize;
  UINTN DescSize;
  UINTN AllocatedSize;
} BLS_FIRMWARE_MEMORY_MAP;

static VOID BlsFirmwareMemoryMapRelease(IN OUT BLS_FIRMWARE_MEMORY_MAP *Snapshot) {
  if (Snapshot == NULL) return;
  if (Snapshot->Map != NULL) FreePool(Snapshot->Map);
  ZeroMem(Snapshot, sizeof(*Snapshot));
}

static BOOLEAN BlsFirmwareMemoryMapAcquire(OUT BLS_FIRMWARE_MEMORY_MAP *Snapshot) {
  UINTN MapSize;
  UINTN MapKey;
  UINTN DescSize;
  UINT32 DescVersion;
  UINTN Attempts;
  EFI_STATUS Status;

  if (Snapshot == NULL) return FALSE;
  ZeroMem(Snapshot, sizeof(*Snapshot));
  if (gBS == NULL || gBS->GetMemoryMap == NULL) return FALSE;

  MapSize = 0;
  DescSize = 0;
  Attempts = 0;
  while (TRUE) {
    Status = gBS->GetMemoryMap(&MapSize, Snapshot->Map, &MapKey, &DescSize, &DescVersion);
    if (Status != EFI_BUFFER_TOO_SMALL) break;
    if (Attempts >= BLS_MEMORY_MAP_RETRY_MAX || MapSize == 0 ||
        MapSize <= Snapshot->AllocatedSize ||
        MapSize > BLS_MEMORY_MAP_ALLOCATION_MAX ||
        DescSize < sizeof(EFI_MEMORY_DESCRIPTOR) ||
        BlsMemoryMapDescriptorBudgetExceeded(MapSize, DescSize) ||
        DescSize > (MAX_UINTN - MapSize) / 4U) {
      BlsFirmwareMemoryMapRelease(Snapshot);
      return FALSE;
    }
    Attempts++;
    MapSize += DescSize * 4U;
    if (MapSize > BLS_MEMORY_MAP_ALLOCATION_MAX) {
      BlsFirmwareMemoryMapRelease(Snapshot);
      return FALSE;
    }
    if (Snapshot->Map != NULL) FreePool(Snapshot->Map);
    Snapshot->Map = AllocateZeroPool(MapSize);
    if (Snapshot->Map == NULL) {
      ZeroMem(Snapshot, sizeof(*Snapshot));
      return FALSE;
    }
    Snapshot->AllocatedSize = MapSize;
  }

  /* UEFI keeps future EFI_MEMORY_DESCRIPTOR revisions backwards compatible
     with the current prefix and requires consumers to advance by DescriptorSize.
     Reject only an older/invalid revision; accept newer revisions while reading
     only the fields defined by this build. */
  if (Status != EFI_SUCCESS || Snapshot->Map == NULL ||
      DescVersion < EFI_MEMORY_DESCRIPTOR_VERSION ||
      DescSize < sizeof(EFI_MEMORY_DESCRIPTOR) || MapSize < DescSize ||
      MapSize > Snapshot->AllocatedSize || (MapSize % DescSize) != 0 ||
      BlsMemoryMapDescriptorBudgetExceeded(MapSize, DescSize)) {
    BlsFirmwareMemoryMapRelease(Snapshot);
    return FALSE;
  }

  Snapshot->MapSize = MapSize;
  Snapshot->DescSize = DescSize;
  return TRUE;
}

static UINTN BlsFirmwareMappedPrefixLengthInMap(
  IN CONST BLS_FIRMWARE_MEMORY_MAP *Snapshot,
  IN UINTN Address,
  IN UINTN MaxLength
  ) {
  EFI_MEMORY_DESCRIPTOR Descriptor;
  UINT64 Start;
  UINT64 End;
  UINT64 CoveredEnd;
  UINTN Offset;

  if (Snapshot == NULL || Snapshot->Map == NULL || Snapshot->DescSize < sizeof(Descriptor) ||
      Snapshot->MapSize < Snapshot->DescSize ||
      (Snapshot->MapSize % Snapshot->DescSize) != 0 || MaxLength == 0 ||
      Address > MAX_UINTN - MaxLength) {
    return 0;
  }

  Start = (UINT64)Address;
  End = Start + (UINT64)MaxLength;
  if (End < Start) return 0;

  /* Firmware memory-map descriptor order is not relied upon here. Grow the
     contiguous readable prefix from Address until MaxLength is covered or a
     physical gap is proven. The descriptor budget bounds quadratic rescans. */
  CoveredEnd = Start;
  while (CoveredEnd < End) {
    UINT64 PreviousEnd;

    PreviousEnd = CoveredEnd;
    for (Offset = 0; Offset <= Snapshot->MapSize - Snapshot->DescSize;
         Offset += Snapshot->DescSize) {
      UINT64 DescStart;
      UINT64 DescBytes;
      UINT64 DescEnd;

      CopyMem(&Descriptor, (UINT8 *)Snapshot->Map + Offset, sizeof(Descriptor));
      if (!BlsFirmwareDescriptorIsReadable(Descriptor.Type)) continue;
      DescStart = Descriptor.PhysicalStart;
      if (Descriptor.NumberOfPages > MAX_UINT64 / EFI_PAGE_SIZE) continue;
      DescBytes = Descriptor.NumberOfPages * EFI_PAGE_SIZE;
      DescEnd = DescStart + DescBytes;
      if (DescEnd < DescStart) continue;
      if (DescStart <= CoveredEnd && DescEnd > CoveredEnd) CoveredEnd = DescEnd;
      if (CoveredEnd >= End) return MaxLength;
    }
    if (CoveredEnd == PreviousEnd) break;
  }

  if (CoveredEnd <= Start) return 0;
  if (CoveredEnd - Start > (UINT64)MaxLength) return MaxLength;
  return (UINTN)(CoveredEnd - Start);
}

static BOOLEAN BlsFirmwareRangeIsMappedInMap(
  IN CONST BLS_FIRMWARE_MEMORY_MAP *Snapshot,
  IN UINTN Address,
  IN UINTN Length
  ) {
  if (Length == 0) return FALSE;
  return (BOOLEAN)(BlsFirmwareMappedPrefixLengthInMap(Snapshot, Address, Length) == Length);
}


static BOOLEAN BlsSmbiosVersionAtLeast(
  IN UINT8 MajorVersion,
  IN UINT8 MinorVersion,
  IN UINT8 RequiredMajor,
  IN UINT8 RequiredMinor
  ) {
  return (BOOLEAN)(MajorVersion > RequiredMajor ||
                   (MajorVersion == RequiredMajor && MinorVersion >= RequiredMinor));
}

static BOOLEAN BlsSmbios3EntryPointIsValid(
  IN CONST SMBIOS_TABLE_3_0_ENTRY_POINT *Entry,
  IN UINTN AvailableLength
  ) {
  /* DSP0134 defines 18h as the current SMBIOS 3.x EPS size, while the
     conformance rules require the declared length to be at least 18h. Accept
     forward-compatible extensions only when the complete declared EPS is in
     the caller-proven readable range. */
  if (Entry == NULL || AvailableLength < sizeof(*Entry) ||
      Entry->EntryPointLength < sizeof(*Entry) ||
      (UINTN)Entry->EntryPointLength > AvailableLength) return FALSE;
  if (CompareMem(Entry->AnchorString, "_SM3_", sizeof(Entry->AnchorString)) != 0) return FALSE;
  if (Entry->MajorVersion < 3U || Entry->EntryPointRevision != 1U || Entry->Reserved != 0U) return FALSE;
  if (!BlsChecksum8IsZero(Entry, Entry->EntryPointLength)) return FALSE;
  if (Entry->TableAddress == 0 || Entry->TableMaximumSize == 0 ||
      Entry->TableMaximumSize > BLS_SMBIOS_TABLE_SCAN_MAX) return FALSE;
  if ((UINT64)(UINTN)Entry->TableAddress != Entry->TableAddress) return FALSE;
  return TRUE;
}

static BOOLEAN BlsSmbios2EntryPointIsValid(
  IN CONST SMBIOS_TABLE_ENTRY_POINT *Entry,
  IN UINTN AvailableLength
  ) {
  CONST UINT8 *Intermediate;
  UINTN IntermediateSize;
  BOOLEAN Historical21Length;
  BOOLEAN ForwardCompatibleLength;
  UINTN Index;

  /* The fixed IEPS extends through SmbiosBcdRevision, so even the historical
     SMBIOS 2.1 main-EPS length typo (1Eh) still needs the complete 1Fh legacy
     structure to validate its independent intermediate checksum. */
  if (Entry == NULL || AvailableLength < sizeof(*Entry)) return FALSE;
  Historical21Length = (BOOLEAN)(Entry->MajorVersion == 2U && Entry->MinorVersion == 1U &&
                                 Entry->EntryPointLength == 0x1EU);
  ForwardCompatibleLength = (BOOLEAN)(
                              BlsSmbiosVersionAtLeast(
                                Entry->MajorVersion, Entry->MinorVersion, 3U, 7U) &&
                              Entry->EntryPointLength >= sizeof(*Entry));
  if (Entry->EntryPointLength != sizeof(*Entry) &&
      !Historical21Length && !ForwardCompatibleLength) return FALSE;
  if ((UINTN)Entry->EntryPointLength > AvailableLength) return FALSE;
  if (Entry->MajorVersion < 2U ||
      (Entry->MajorVersion == 2U && Entry->MinorVersion < 1U)) return FALSE;
  if (Entry->EntryPointRevision != 0U) return FALSE;
  for (Index = 0; Index < ARRAY_SIZE(Entry->FormattedArea); Index++) {
    if (Entry->FormattedArea[Index] != 0U) return FALSE;
  }
  if (CompareMem(Entry->AnchorString, "_SM_", sizeof(Entry->AnchorString)) != 0) return FALSE;
  if (CompareMem(Entry->IntermediateAnchorString, "_DMI_", sizeof(Entry->IntermediateAnchorString)) != 0) return FALSE;
  if (!BlsChecksum8IsZero(Entry, Entry->EntryPointLength)) return FALSE;

  Intermediate = (CONST UINT8 *)&Entry->IntermediateAnchorString[0];
  IntermediateSize = sizeof(*Entry) - OFFSET_OF(SMBIOS_TABLE_ENTRY_POINT, IntermediateAnchorString);
  if (!BlsChecksum8IsZero(Intermediate, IntermediateSize)) return FALSE;
  if (Entry->TableAddress == 0 || Entry->TableLength == 0) return FALSE;
  if (Entry->MaxStructureSize < sizeof(SMBIOS_STRUCTURE) + 2U ||
      Entry->MaxStructureSize > Entry->TableLength) return FALSE;
  return TRUE;
}

static BOOLEAN BlsSmbiosStructureFitsMaximum(
  IN CONST SMBIOS_STRUCTURE *Hdr,
  IN CONST SMBIOS_STRUCTURE *NextHdr,
  IN UINTN MaximumStructureSize
  ) {
  UINTN Start;
  UINTN End;

  if (Hdr == NULL || NextHdr == NULL || MaximumStructureSize < sizeof(SMBIOS_STRUCTURE) + 2U) {
    return FALSE;
  }
  Start = (UINTN)Hdr;
  End = (UINTN)NextHdr;
  if (End <= Start) return FALSE;
  return (BOOLEAN)(End - Start <= MaximumStructureSize);
}

static BOOLEAN BlsSmbiosMarkHandleUnique(
  IN OUT UINT8 *SeenHandles,
  IN UINT16 Handle
  ) {
  UINTN ByteIndex;
  UINT8 BitMask;

  if (SeenHandles == NULL) return FALSE;
  /* All SMBIOS entry points accepted by this parser are version 2.1 or
     later. DSP0134 reserves 0xFF00..0xFFFF in those versions, so such a
     value is not a valid structure identifier and must fail closed before
     touching the uniqueness bitmap. */
  if (Handle >= SMBIOS_HANDLE_RESERVED_BEGIN) return FALSE;
  ByteIndex = ((UINTN)Handle) >> 3;
  BitMask = (UINT8)(1U << (((UINTN)Handle) & 7U));
  if ((SeenHandles[ByteIndex] & BitMask) != 0U) return FALSE;
  SeenHandles[ByteIndex] = (UINT8)(SeenHandles[ByteIndex] | BitMask);
  return TRUE;
}

static BOOLEAN BlsDecodeSmbiosProcessorCount(
  IN UINT8 LegacyCount,
  IN BOOLEAN HasExtendedCount,
  IN UINT16 ExtendedCount,
  OUT UINT32 *Count
  ) {
  if (Count == NULL) return FALSE;
  *Count = 0U;

  if (!HasExtendedCount) {
    if (LegacyCount == 0U) return FALSE;
    *Count = LegacyCount;
    return TRUE;
  }

  if (ExtendedCount == 0U || ExtendedCount == 0xFFFFU) return FALSE;
  if (ExtendedCount <= 0x00FFU) {
    if (LegacyCount != (UINT8)ExtendedCount) return FALSE;
  } else if (LegacyCount != 0xFFU) {
    return FALSE;
  }

  *Count = ExtendedCount;
  return TRUE;
}

/* SMBIOS 3.6 extended Type 4 by a 16-bit Thread Enabled field at wire
   offset 30h. The bundled EDK2 headers predate that member, so read it by
   its specification-defined offset only after the formatted-length/version
   gates prove the field is present. */
#define BLS_SMBIOS_TYPE4_THREAD_ENABLED_OFFSET 0x30U

static BOOLEAN BlsSmbiosEnabledThreadCount(
  IN CONST SMBIOS_TABLE_TYPE4 *Type4,
  IN UINT8 MajorVersion,
  IN UINT8 MinorVersion,
  IN UINT32 MaximumThreadCount,
  OUT UINT32 *EnabledThreadCount
  ) {
  UINT16 WireCount;

  if (EnabledThreadCount == NULL) return FALSE;
  *EnabledThreadCount = 0U;
  if (Type4 == NULL ||
      !BlsSmbiosVersionAtLeast(MajorVersion, MinorVersion, 3U, 6U) ||
      Type4->Hdr.Length < BLS_SMBIOS_TYPE4_THREAD_ENABLED_OFFSET + sizeof(WireCount)) {
    return FALSE;
  }

  WireCount = 0U;
  CopyMem(&WireCount,
          (CONST UINT8 *)(CONST VOID *)Type4 + BLS_SMBIOS_TYPE4_THREAD_ENABLED_OFFSET,
          sizeof(WireCount));
  if (WireCount == 0U || WireCount == 0xFFFFU) return FALSE;
  if (MaximumThreadCount != 0U && (UINT32)WireCount > MaximumThreadCount) return FALSE;

  *EnabledThreadCount = WireCount;
  return TRUE;
}

static UINT64 BlsSmbiosMemoryDeviceSizeMb(
  IN CONST SMBIOS_TABLE_TYPE17 *Type17,
  IN UINT8 MajorVersion,
  IN UINT8 MinorVersion
  ) {
  UINT16 LegacySize;

  if (Type17 == NULL ||
      Type17->Hdr.Length < OFFSET_OF(SMBIOS_TABLE_TYPE17, Size) + sizeof(Type17->Size)) return 0U;

  LegacySize = Type17->Size;
  if (LegacySize == 0U || LegacySize == 0xFFFFU) return 0U;
  if (LegacySize == 0x7FFFU) {
    UINT32 ExtendedSize;

    if (!BlsSmbiosVersionAtLeast(MajorVersion, MinorVersion, 2U, 7U) ||
        Type17->Hdr.Length < OFFSET_OF(SMBIOS_TABLE_TYPE17, ExtendedSize) + sizeof(Type17->ExtendedSize)) {
      return 0U;
    }
    ExtendedSize = Type17->ExtendedSize;
    if ((ExtendedSize & BIT31) != 0U) return 0U;
    return (UINT64)(ExtendedSize & 0x7FFFFFFFU);
  }

  if ((LegacySize & 0x8000U) != 0U) {
    return ((UINT64)(LegacySize & 0x7FFFU) + 1023U) / 1024U;
  }
  return LegacySize;
}

static UINT32 BlsSmbiosExtendedSpeedIfValid(IN UINT32 ExtendedSpeed) {
  if ((ExtendedSpeed & BIT31) != 0U) return 0U;
  return ExtendedSpeed & 0x7FFFFFFFU;
}

static UINT32 BlsSmbiosMemoryDeviceSpeed(
  IN CONST SMBIOS_TABLE_TYPE17 *Type17,
  IN UINT8 MajorVersion,
  IN UINT8 MinorVersion
  ) {
  if (Type17 == NULL) return 0U;

  if (BlsSmbiosVersionAtLeast(MajorVersion, MinorVersion, 2U, 7U) &&
      Type17->Hdr.Length >= OFFSET_OF(SMBIOS_TABLE_TYPE17, ConfiguredMemoryClockSpeed) +
                            sizeof(Type17->ConfiguredMemoryClockSpeed)) {
    if (Type17->ConfiguredMemoryClockSpeed == 0xFFFFU) {
      if (BlsSmbiosVersionAtLeast(MajorVersion, MinorVersion, 3U, 3U) &&
          Type17->Hdr.Length >= OFFSET_OF(SMBIOS_TABLE_TYPE17, ExtendedConfiguredMemorySpeed) +
                                sizeof(Type17->ExtendedConfiguredMemorySpeed)) {
        UINT32 Speed;

        Speed = BlsSmbiosExtendedSpeedIfValid(Type17->ExtendedConfiguredMemorySpeed);
        if (Speed != 0U) return Speed;
      }
    } else if (Type17->ConfiguredMemoryClockSpeed != 0U) {
      return Type17->ConfiguredMemoryClockSpeed;
    }
  }

  if (BlsSmbiosVersionAtLeast(MajorVersion, MinorVersion, 2U, 3U) &&
      Type17->Hdr.Length >= OFFSET_OF(SMBIOS_TABLE_TYPE17, Speed) + sizeof(Type17->Speed)) {
    if (Type17->Speed == 0xFFFFU) {
      if (BlsSmbiosVersionAtLeast(MajorVersion, MinorVersion, 3U, 3U) &&
          Type17->Hdr.Length >= OFFSET_OF(SMBIOS_TABLE_TYPE17, ExtendedSpeed) + sizeof(Type17->ExtendedSpeed)) {
        return BlsSmbiosExtendedSpeedIfValid(Type17->ExtendedSpeed);
      }
      return 0U;
    }
    return Type17->Speed;
  }
  return 0U;
}

static BOOLEAN BlsDecodeUtf8CodePointBounded(
  IN CONST CHAR8 *Text,
  IN UINTN Remaining,
  OUT UINT32 *CodePoint,
  OUT UINTN *Consumed
  ) {
  UINT8 B0;
  UINT8 B1;
  UINT8 B2;
  UINT8 B3;

  if (Text == NULL || Remaining == 0 || CodePoint == NULL || Consumed == NULL) return FALSE;
  B0 = (UINT8)Text[0];

  if (B0 < 0x80U) {
    *CodePoint = B0;
    *Consumed = 1U;
    return TRUE;
  }

  if (B0 >= 0xC2U && B0 <= 0xDFU && Remaining >= 2U) {
    B1 = (UINT8)Text[1];
    if ((B1 & 0xC0U) != 0x80U) return FALSE;
    *CodePoint = ((UINT32)(B0 & 0x1FU) << 6) | (UINT32)(B1 & 0x3FU);
    *Consumed = 2U;
    return TRUE;
  }

  if (B0 >= 0xE0U && B0 <= 0xEFU && Remaining >= 3U) {
    B1 = (UINT8)Text[1];
    B2 = (UINT8)Text[2];
    if ((B1 & 0xC0U) != 0x80U || (B2 & 0xC0U) != 0x80U) return FALSE;
    if (B0 == 0xE0U && B1 < 0xA0U) return FALSE; /* Overlong. */
    if (B0 == 0xEDU && B1 >= 0xA0U) return FALSE; /* UTF-16 surrogate. */
    *CodePoint = ((UINT32)(B0 & 0x0FU) << 12) |
                 ((UINT32)(B1 & 0x3FU) << 6) |
                 (UINT32)(B2 & 0x3FU);
    *Consumed = 3U;
    return TRUE;
  }

  if (B0 >= 0xF0U && B0 <= 0xF4U && Remaining >= 4U) {
    B1 = (UINT8)Text[1];
    B2 = (UINT8)Text[2];
    B3 = (UINT8)Text[3];
    if ((B1 & 0xC0U) != 0x80U || (B2 & 0xC0U) != 0x80U || (B3 & 0xC0U) != 0x80U) return FALSE;
    if (B0 == 0xF0U && B1 < 0x90U) return FALSE; /* Overlong. */
    if (B0 == 0xF4U && B1 >= 0x90U) return FALSE; /* Above U+10FFFF. */
    *CodePoint = ((UINT32)(B0 & 0x07U) << 18) |
                 ((UINT32)(B1 & 0x3FU) << 12) |
                 ((UINT32)(B2 & 0x3FU) << 6) |
                 (UINT32)(B3 & 0x3FU);
    *Consumed = 4U;
    return TRUE;
  }

  return FALSE;
}

static BOOLEAN BlsSmbiosStringToUnicode(
  OUT CHAR16 *Dst,
  IN UINTN DstChars,
  IN CONST CHAR8 *Src,
  IN UINTN SrcBytes,
  IN UINT8 MajorVersion,
  IN UINT8 MinorVersion
  ) {
  UINTN InIndex;
  UINTN OutIndex;
  BOOLEAN OutputFull;

  if (Dst == NULL || DstChars == 0U) return FALSE;
  Dst[0] = L'\0';
  if (Src == NULL) return FALSE;

  /* SMBIOS 3.5 made UTF-8 (without a BOM) normative for text strings. Keep
     the historical byte-preserving conversion on older tables so this
     standards fix does not regress pre-3.5 firmware that used an OEM 8-bit
     code page. */
  if (!BlsSmbiosVersionAtLeast(MajorVersion, MinorVersion, 3U, 5U)) {
    for (InIndex = 0U; InIndex < SrcBytes && InIndex + 1U < DstChars; InIndex++) {
      Dst[InIndex] = (CHAR16)(UINT8)Src[InIndex];
    }
    Dst[InIndex] = L'\0';
    return TRUE;
  }

  if (SrcBytes >= 3U && (UINT8)Src[0] == 0xEFU &&
      (UINT8)Src[1] == 0xBBU && (UINT8)Src[2] == 0xBFU) {
    return FALSE; /* DSP0134 explicitly forbids a BOM. */
  }

  InIndex = 0U;
  OutIndex = 0U;
  OutputFull = FALSE;
  while (InIndex < SrcBytes) {
    UINT32 CodePoint;
    UINTN Consumed;

    if (!BlsDecodeUtf8CodePointBounded(
          Src + InIndex, SrcBytes - InIndex, &CodePoint, &Consumed)) {
      Dst[0] = L'\0';
      return FALSE;
    }

    if (!OutputFull) {
      CHAR16 Ucs2Char;

      /* SMBIOS carries Unicode scalar values in UTF-8, while UEFI CHAR16
         strings are UCS-2 and must not contain surrogate code units. Map
         values that UEFI cannot represent (and its reserved private-use
         console range) to U+FFFD rather than emitting invalid CHAR16 data. */
      if (CodePoint > 0xFFFFU ||
          (CodePoint >= 0xF000U && CodePoint <= 0xFCFFU)) {
        Ucs2Char = (CHAR16)0xFFFDU;
      } else {
        Ucs2Char = (CHAR16)CodePoint;
      }

      if (OutIndex + 1U < DstChars) {
        Dst[OutIndex++] = Ucs2Char;
      } else {
        OutputFull = TRUE;
      }
    }
    InIndex += Consumed;
  }

  Dst[OutIndex] = L'\0';
  return TRUE;
}

static VOID BlsBuildNameFromSmbios(
  IN OUT CHAR16 *Out,
  IN UINTN OutChars,
  IN CONST CHAR8 *Manufacturer,
  IN UINTN ManufacturerBytes,
  IN CONST CHAR8 *Product,
  IN UINTN ProductBytes,
  IN UINT8 MajorVersion,
  IN UINT8 MinorVersion
  ) {
  CHAR16 Mfg[BLS_MAX_TEXT];
  CHAR16 Prod[BLS_MAX_TEXT];
  BOOLEAN HasMfg;
  BOOLEAN HasProd;

  if (Out == NULL || OutChars == 0U) return;

  HasMfg = !BlsAsciiIsJunk(Manufacturer) &&
           BlsSmbiosStringToUnicode(
             Mfg, BLS_MAX_TEXT, Manufacturer, ManufacturerBytes, MajorVersion, MinorVersion);
  HasProd = !BlsAsciiIsJunk(Product) &&
            BlsSmbiosStringToUnicode(
              Prod, BLS_MAX_TEXT, Product, ProductBytes, MajorVersion, MinorVersion);

  if (!HasMfg && !HasProd) return;
  if (HasMfg && HasProd) {
    BlsSafeStrCpy(Out, OutChars, Mfg);
    if (!BlsStrEq(Out, Prod)) {
      BlsSafeStrCat(Out, OutChars, L" ");
      BlsSafeStrCat(Out, OutChars, Prod);
    }
  } else if (HasProd) {
    BlsSafeStrCpy(Out, OutChars, Prod);
  } else {
    BlsSafeStrCpy(Out, OutChars, Mfg);
  }
}

static VOID BlsReadCpuBrand(OUT CHAR16 *Out, IN UINTN OutChars) {
  UINT32 Regs[4];
  CHAR8 Brand[49];
  UINT32 MaxExt;
  UINTN I;

  BlsSafeStrCpy(Out, OutChars, L"Unknown CPU");
  AsmCpuid(0x80000000, &MaxExt, NULL, NULL, NULL);
  if (MaxExt < 0x80000004) return;

  ZeroMem(Brand, sizeof(Brand));
  for (I = 0; I < 3; I++) {
    AsmCpuid(0x80000002 + (UINT32)I, &Regs[0], &Regs[1], &Regs[2], &Regs[3]);
    CopyMem(Brand + I * 16, Regs, 16);
  }
  Brand[48] = '\0';
  BlsTrimAscii(Brand);
  if (Brand[0] != '\0') BlsAsciiToUnicode(Out, OutChars, Brand);
}

static BOOLEAN BlsReadCpuTopologyLeaf(IN UINT32 Leaf, OUT UINT32 *Logical, OUT UINT32 *Cores) {
  UINT32 Eax;
  UINT32 Ebx;
  UINT32 Ecx;
  UINT32 Edx;
  UINT32 SubLeaf;
  UINT32 SmtLogical;
  UINT32 HighestLevelLogical;

  if (Logical == NULL || Cores == NULL) return FALSE;
  SmtLogical = 0;
  HighestLevelLogical = 0;

  for (SubLeaf = 0; SubLeaf <= MAX_UINT8; SubLeaf++) {
    UINT32 LevelLogical;
    UINT32 LevelType;

    AsmCpuidEx(Leaf, SubLeaf, &Eax, &Ebx, &Ecx, &Edx);
    LevelLogical = Ebx & 0xFFFF;
    if (LevelLogical == 0) break;
    LevelType = (Ecx >> 8) & 0xFF;
    if (LevelType == 0) break;
    if (LevelType == 1) {
      SmtLogical = LevelLogical;
    }

    /*
      CPUID.1FH is an ordered hierarchy. Its last valid sub-leaf is the
      largest enumerated domain immediately below the implied package, so EBX
      there is the shipped logical-processor count at package scope. Keeping
      only the Core-domain EBX undercounts CPUs that also expose Module, Tile,
      Die or DieGrp domains. CPUID.0BH retains its traditional Core-domain
      package count. This is diagnostic fallback data only; MP Services remains
      authoritative when firmware provides it.
    */
    if (Leaf == 0x1FU) {
      HighestLevelLogical = LevelLogical;
    } else if (LevelType == 2U) {
      HighestLevelLogical = LevelLogical;
    }
  }

  if (HighestLevelLogical == 0) return FALSE;
  if (SmtLogical == 0) SmtLogical = 1;
  if (HighestLevelLogical < SmtLogical ||
      (HighestLevelLogical % SmtLogical) != 0) return FALSE;

  *Logical = HighestLevelLogical;
  *Cores = HighestLevelLogical / SmtLogical;
  if (*Cores == 0) *Cores = 1;
  return TRUE;
}

static VOID BlsReadCpuTopologyFallback(OUT UINT32 *Logical, OUT UINT32 *Cores) {
  UINT32 Eax;
  UINT32 Ebx;
  UINT32 Ecx;
  UINT32 Edx;
  UINT32 MaxBasic;
  UINT32 MaxExt;
  UINT32 LogicalCount;
  UINT32 CoreCount;

  LogicalCount = 1;
  CoreCount = 1;
  AsmCpuid(0, &MaxBasic, NULL, NULL, NULL);

  if (MaxBasic >= 0x1F && BlsReadCpuTopologyLeaf(0x1F, &LogicalCount, &CoreCount)) {
    if (Logical != NULL) *Logical = LogicalCount;
    if (Cores != NULL) *Cores = CoreCount;
    return;
  }
  if (MaxBasic >= 0x0B && BlsReadCpuTopologyLeaf(0x0B, &LogicalCount, &CoreCount)) {
    if (Logical != NULL) *Logical = LogicalCount;
    if (Cores != NULL) *Cores = CoreCount;
    return;
  }

  if (MaxBasic >= 1) {
    AsmCpuid(1, &Eax, &Ebx, &Ecx, &Edx);
    LogicalCount = (Ebx >> 16) & 0xFF;
    if (LogicalCount == 0) LogicalCount = 1;
  }

  AsmCpuid(0x80000000, &MaxExt, NULL, NULL, NULL);
  if (MaxExt >= 0x80000008) {
    AsmCpuid(0x80000008, &Eax, &Ebx, &Ecx, &Edx);
    CoreCount = (Ecx & 0xFF) + 1;
  }

  if (CoreCount <= 1 && MaxBasic >= 4) {
    AsmCpuidEx(4, 0, &Eax, &Ebx, &Ecx, &Edx);
    if ((Eax & 0x1F) != 0) {
      CoreCount = ((Eax >> 26) & 0x3F) + 1;
    }
  }

  if (CoreCount == 0) CoreCount = 1;
  if (LogicalCount < CoreCount) LogicalCount = CoreCount;
  if (Logical != NULL) *Logical = LogicalCount;
  if (Cores != NULL) *Cores = CoreCount;
}


typedef struct {
  EFI_CPU_PHYSICAL_LOCATION Location;
} BLS_MP_CPU_LOCATION;

static INTN BlsCompareMpCpuLocation(
  IN CONST BLS_MP_CPU_LOCATION *Left,
  IN CONST BLS_MP_CPU_LOCATION *Right
  ) {
  if (Left->Location.Package < Right->Location.Package) return -1;
  if (Left->Location.Package > Right->Location.Package) return 1;
  if (Left->Location.Core < Right->Location.Core) return -1;
  if (Left->Location.Core > Right->Location.Core) return 1;
  if (Left->Location.Thread < Right->Location.Thread) return -1;
  if (Left->Location.Thread > Right->Location.Thread) return 1;
  return 0;
}

static VOID BlsSwapMpCpuLocation(
  IN OUT BLS_MP_CPU_LOCATION *Left,
  IN OUT BLS_MP_CPU_LOCATION *Right
  ) {
  BLS_MP_CPU_LOCATION Temp;

  if (Left == Right) return;
  Temp = *Left;
  *Left = *Right;
  *Right = Temp;
}

static VOID BlsSiftDownMpCpuLocations(
  IN OUT BLS_MP_CPU_LOCATION *Locations,
  IN UINTN Count,
  IN UINTN Root
  ) {
  while (Root < Count / 2U) {
    UINTN Child;
    UINTN Larger;

    Child = Root * 2U + 1U;
    Larger = Child;
    if (Child + 1U < Count &&
        BlsCompareMpCpuLocation(&Locations[Child], &Locations[Child + 1U]) < 0) {
      Larger = Child + 1U;
    }
    if (BlsCompareMpCpuLocation(&Locations[Root], &Locations[Larger]) >= 0) break;
    BlsSwapMpCpuLocation(&Locations[Root], &Locations[Larger]);
    Root = Larger;
  }
}

static VOID BlsSortMpCpuLocations(IN OUT BLS_MP_CPU_LOCATION *Locations, IN UINTN Count) {
  UINTN Start;
  UINTN End;

  if (Locations == NULL || Count < 2U) return;
  for (Start = Count / 2U; Start > 0U; Start--) {
    BlsSiftDownMpCpuLocations(Locations, Count, Start - 1U);
  }
  for (End = Count; End > 1U; End--) {
    BlsSwapMpCpuLocation(&Locations[0], &Locations[End - 1U]);
    BlsSiftDownMpCpuLocations(Locations, End - 1U, 0U);
  }
}

static BOOLEAN BlsReadMpServicesTopology(OUT UINT32 *Logical, OUT UINT32 *Cores) {
  EFI_MP_SERVICES_PROTOCOL *MpServices;
  BLS_MP_CPU_LOCATION *EnabledLocations;
  EFI_PROCESSOR_INFORMATION ProcessorInfo;
  EFI_STATUS Status;
  UINTN TotalProcessors;
  UINTN EnabledProcessors;
  UINTN ProcessorIndex;
  UINTN ObservedEnabled;
  UINTN UniqueCores;
  BOOLEAN Valid;

  if (Logical == NULL || Cores == NULL) return FALSE;
  if (gBS == NULL || gBS->LocateProtocol == NULL) return FALSE;

  MpServices = NULL;
  Status = gBS->LocateProtocol(&gEfiMpServiceProtocolGuid, NULL, (VOID **)&MpServices);
  if (Status != EFI_SUCCESS || MpServices == NULL ||
      MpServices->GetNumberOfProcessors == NULL || MpServices->GetProcessorInfo == NULL) {
    return FALSE;
  }

  TotalProcessors = 0;
  EnabledProcessors = 0;
  Status = MpServices->GetNumberOfProcessors(MpServices, &TotalProcessors, &EnabledProcessors);
  if (Status != EFI_SUCCESS || TotalProcessors == 0 || EnabledProcessors == 0 ||
      EnabledProcessors > TotalProcessors ||
      TotalProcessors > BLS_MP_PROCESSOR_SCAN_MAX ||
      EnabledProcessors > BLS_MP_PROCESSOR_SCAN_MAX ||
      EnabledProcessors > MAX_UINTN / sizeof(*EnabledLocations)) {
    return FALSE;
  }

  EnabledLocations = AllocateZeroPool(EnabledProcessors * sizeof(*EnabledLocations));
  if (EnabledLocations == NULL) return FALSE;

  ObservedEnabled = 0;
  UniqueCores = 0;
  Valid = TRUE;
  for (ProcessorIndex = 0; ProcessorIndex < TotalProcessors; ProcessorIndex++) {
    ZeroMem(&ProcessorInfo, sizeof(ProcessorInfo));
    Status = MpServices->GetProcessorInfo(MpServices, ProcessorIndex, &ProcessorInfo);
    if (Status != EFI_SUCCESS) {
      Valid = FALSE;
      break;
    }
    if ((ProcessorInfo.StatusFlag & PROCESSOR_ENABLED_BIT) == 0) continue;
    if (ObservedEnabled >= EnabledProcessors) {
      Valid = FALSE;
      break;
    }

    EnabledLocations[ObservedEnabled].Location = ProcessorInfo.Location;
    ObservedEnabled++;
  }

  if (Valid && ObservedEnabled == EnabledProcessors) {
    BlsSortMpCpuLocations(EnabledLocations, ObservedEnabled);
    for (ProcessorIndex = 0; ProcessorIndex < ObservedEnabled; ProcessorIndex++) {
      EFI_CPU_PHYSICAL_LOCATION *Current;

      Current = &EnabledLocations[ProcessorIndex].Location;
      if (ProcessorIndex > 0U) {
        EFI_CPU_PHYSICAL_LOCATION *Previous;

        Previous = &EnabledLocations[ProcessorIndex - 1U].Location;
        if (Previous->Package == Current->Package &&
            Previous->Core == Current->Core &&
            Previous->Thread == Current->Thread) {
          /* The protocol promises one physical location per logical CPU.
             Duplicate locations make the topology unusable; keep the proven
             CPUID/SMBIOS fallback instead of publishing a false core count. */
          Valid = FALSE;
          break;
        }
        if (Previous->Package == Current->Package && Previous->Core == Current->Core) continue;
      }
      UniqueCores++;
    }
  } else {
    Valid = FALSE;
  }

  if (!Valid || UniqueCores == 0 || UniqueCores > EnabledProcessors || UniqueCores > MAX_UINT32) {
    Valid = FALSE;
  } else {
    *Logical = (UINT32)EnabledProcessors;
    *Cores = (UINT32)UniqueCores;
  }

  FreePool(EnabledLocations);
  return Valid;
}

static VOID *BlsFindConfigTableInMap(
  IN EFI_GUID *Guid,
  IN CONST BLS_FIRMWARE_MEMORY_MAP *Snapshot
  ) {
  UINTN Index;
  UINTN TableBytes;

  if (Guid == NULL || Snapshot == NULL || gST == NULL ||
      (gST->NumberOfTableEntries != 0 && gST->ConfigurationTable == NULL)) return NULL;
  if (gST->NumberOfTableEntries > BLS_CONFIG_TABLE_SCAN_MAX ||
      gST->NumberOfTableEntries > MAX_UINTN / sizeof(*gST->ConfigurationTable) ||
      (UINTN)gST->ConfigurationTable >
        MAX_UINTN - gST->NumberOfTableEntries * sizeof(*gST->ConfigurationTable)) {
    return NULL;
  }
  TableBytes = gST->NumberOfTableEntries * sizeof(*gST->ConfigurationTable);
  if (TableBytes != 0 &&
      !BlsFirmwareRangeIsMappedInMap(Snapshot, (UINTN)gST->ConfigurationTable, TableBytes)) {
    return NULL;
  }
  for (Index = 0; Index < gST->NumberOfTableEntries; Index++) {
    if (CompareGuid(&gST->ConfigurationTable[Index].VendorGuid, Guid)) {
      return gST->ConfigurationTable[Index].VendorTable;
    }
  }
  return NULL;
}


static CHAR8 *BlsGetSmbiosString(
  IN SMBIOS_STRUCTURE *Hdr,
  IN SMBIOS_STRUCTURE *Limit,
  IN UINT8 StringIndex,
  OUT UINTN *StringBytes OPTIONAL
  ) {
  CHAR8 *Str;
  CHAR8 *Scan;
  UINTN Current;
  UINTN Remaining;

  if (StringBytes != NULL) *StringBytes = 0U;
  if (Hdr == NULL || Limit == NULL || StringIndex == 0) return NULL;
  if ((UINTN)Hdr >= (UINTN)Limit) return NULL;
  Remaining = (UINTN)Limit - (UINTN)Hdr;
  if (Remaining < sizeof(SMBIOS_STRUCTURE)) return NULL;
  if (Hdr->Length < sizeof(SMBIOS_STRUCTURE) || Hdr->Length >= Remaining) return NULL;

  Str = (CHAR8 *)((UINTN)Hdr + Hdr->Length);
  Current = 1;
  while (Str < (CHAR8 *)Limit) {
    if (Str[0] == '\0') return NULL;
    if (Current == StringIndex) {
      Scan = Str;
      while (Scan < (CHAR8 *)Limit && Scan[0] != '\0') {
        Scan++;
      }
      if (Scan >= (CHAR8 *)Limit) return NULL;
      if (StringBytes != NULL) *StringBytes = (UINTN)(Scan - Str);
      return Str;
    }
    while (Str < (CHAR8 *)Limit && Str[0] != '\0') {
      Str++;
    }
    if (Str >= (CHAR8 *)Limit) return NULL;
    Str++;
    Current++;
  }
  return NULL;
}

static SMBIOS_STRUCTURE *BlsNextSmbiosStruct(IN SMBIOS_STRUCTURE *Hdr, IN SMBIOS_STRUCTURE *Limit) {
  CHAR8 *Ptr;
  UINTN Remaining;

  if (Hdr == NULL || Limit == NULL || (UINTN)Hdr >= (UINTN)Limit) return NULL;
  Remaining = (UINTN)Limit - (UINTN)Hdr;
  if (Remaining < sizeof(SMBIOS_STRUCTURE)) return NULL;
  if (Hdr->Length < sizeof(SMBIOS_STRUCTURE) || Hdr->Length > Remaining - 2) return NULL;
  Ptr = (CHAR8 *)((UINTN)Hdr + Hdr->Length);
  while (Ptr + 1 < (CHAR8 *)Limit) {
    if (Ptr[0] == '\0' && Ptr[1] == '\0') {
      Ptr += 2;
      if (Ptr > (CHAR8 *)Limit) return NULL;
      return (SMBIOS_STRUCTURE *)Ptr;
    }
    Ptr++;
  }
  return NULL;
}

static CONST CHAR16 *BlsMemoryTypeName(IN UINT8 Type) {
  switch (Type) {
    case MemoryTypeDdr:   return L"DDR";
    case MemoryTypeDdr2:  return L"DDR2";
    case MemoryTypeDdr3:  return L"DDR3";
    case MemoryTypeDdr4:  return L"DDR4";
    case MemoryTypeDdr5:  return L"DDR5";
    case MemoryTypeLpddr: return L"LPDDR";
    case MemoryTypeLpddr2:return L"LPDDR2";
    case MemoryTypeLpddr3:return L"LPDDR3";
    case MemoryTypeLpddr4:return L"LPDDR4";
    case MemoryTypeLpddr5:return L"LPDDR5";
    default:             return L"";
  }
}

static VOID BlsReadMemoryMapFallback(IN OUT BLS_HARDWARE_INFO *Info) {
  BLS_FIRMWARE_MEMORY_MAP Snapshot;
  EFI_MEMORY_DESCRIPTOR Descriptor;
  UINT64 Pages;
  UINT64 TotalPages;
  UINTN Offset;

  if (Info == NULL || Info->MemoryMB != 0) return;
  if (!BlsFirmwareMemoryMapAcquire(&Snapshot)) return;

  TotalPages = 0;
  for (Offset = 0; Offset <= Snapshot.MapSize - Snapshot.DescSize;
       Offset += Snapshot.DescSize) {
    CopyMem(&Descriptor, (UINT8 *)Snapshot.Map + Offset, sizeof(Descriptor));
    Pages = 0;
    switch (Descriptor.Type) {
      case EfiLoaderCode:
      case EfiLoaderData:
      case EfiBootServicesCode:
      case EfiBootServicesData:
      case EfiRuntimeServicesCode:
      case EfiRuntimeServicesData:
      case EfiConventionalMemory:
      case EfiACPIReclaimMemory:
      case EfiACPIMemoryNVS:
        Pages = Descriptor.NumberOfPages;
        break;
      default:
        break;
    }
    if (Pages > 0) {
      if (TotalPages > MAX_UINT64 - Pages) {
        TotalPages = MAX_UINT64;
      } else {
        TotalPages += Pages;
      }
    }
  }

  if (TotalPages > 0) {
    Info->MemoryMB = TotalPages / 256U;
    Info->MemoryEstimated = TRUE;
  }
  BlsFirmwareMemoryMapRelease(&Snapshot);
}

static UINTN BlsSmbiosStructureScanLimit(IN UINTN TableLength, IN UINTN ReportedCount) {
  UINTN MaxCount;

  MaxCount = ReportedCount;
  if (MaxCount == 0) {
    MaxCount = (TableLength / (sizeof(SMBIOS_STRUCTURE) + 2U)) + 1U;
  }
  if (MaxCount > BLS_SMBIOS_STRUCTURE_SCAN_MAX) {
    MaxCount = BLS_SMBIOS_STRUCTURE_SCAN_MAX;
  }
  return MaxCount;
}

static VOID BlsReadSmbios(OUT BLS_HARDWARE_INFO *Info) {
  SMBIOS_TABLE_ENTRY_POINT *Smbios;
  SMBIOS_TABLE_3_0_ENTRY_POINT *Smbios3;
  SMBIOS_STRUCTURE *Hdr;
  SMBIOS_STRUCTURE *Limit;
  UINTN Count;
  UINTN MaxCount;
  UINTN TableLength;
  UINTN ReportedCount;
  UINTN MaximumStructureSize;
  BOOLEAN BoardFound;
  BOOLEAN BoardIsMotherboard;
  BOOLEAN EndOfTableFound;
  BOOLEAN HasReportedCount;
  BOOLEAN RequiresEndOfTable;
  BOOLEAN TableValid;
  BOOLEAN PreferLegacySmbios;
  BOOLEAN ParsingSmbios3;
  BOOLEAN ResetForRetry;
  BOOLEAN ValidSmbios3EntryPointSeen;
  CHAR16 BoardFallback[BLS_MAX_TEXT];
  UINT32 TotalCores;
  UINT32 TotalThreads;
  CHAR16 DetectedMemoryType[16];
  UINT32 DetectedMemoryMHz;
  BOOLEAN MemoryTypeMixed;
  BOOLEAN MemorySpeedMixed;
  UINT32 PopulatedDimms;
  UINT32 KnownTypeDimms;
  UINT32 KnownSpeedDimms;
  UINT8 SmbiosMajorVersion;
  UINT8 SmbiosMinorVersion;
  UINT8 Smbios3MajorVersion;
  UINT8 Smbios3MinorVersion;
  BLS_FIRMWARE_MEMORY_MAP Snapshot;
  BLS_HARDWARE_INFO OriginalInfo;
  UINT8 *SeenHandles;

  if (Info == NULL) return;

  CopyMem(&OriginalInfo, Info, sizeof(OriginalInfo));
  PreferLegacySmbios = FALSE;
  ResetForRetry = FALSE;
  ValidSmbios3EntryPointSeen = FALSE;
  Smbios3 = NULL;
  Smbios3MajorVersion = 0;
  Smbios3MinorVersion = 0;
  if (gBS == NULL || gBS->GetMemoryMap == NULL) return;
  SeenHandles = AllocateZeroPool(BLS_SMBIOS_HANDLE_BITMAP_BYTES);
  if (SeenHandles == NULL) return;

  /* SMBIOS discovery validates several firmware-owned ranges. Capture one
     stable memory-map snapshot for the whole operation instead of allocating
     and querying firmware again for every pointer/range check. Allocate the
     transient handle bitmap before this snapshot; no pool allocation occurs
     inside the SMBIOS walk, so the snapshot remains stable until release. */
  if (!BlsFirmwareMemoryMapAcquire(&Snapshot)) {
    FreePool(SeenHandles);
    return;
  }

TrySmbiosTable:
  /* Parsing is transactional. When firmware publishes both SMBIOS entry
     points, prefer the modern 64-bit entry point on the normal path,
     but recover through the conforming 32-bit subset if the selected 64-bit
     table itself is inaccessible or malformed. Reinitialize every accumulator
     before a retry so data from the rejected table cannot leak into Info. */
  if (ResetForRetry) {
    CopyMem(Info, &OriginalInfo, sizeof(*Info));
    ZeroMem(SeenHandles, BLS_SMBIOS_HANDLE_BITMAP_BYTES);
    ResetForRetry = FALSE;
  }
  BoardFound = FALSE;
  BoardIsMotherboard = FALSE;
  EndOfTableFound = FALSE;
  HasReportedCount = FALSE;
  TableValid = FALSE;
  ReportedCount = 0;
  MaximumStructureSize = 0;
  BoardFallback[0] = L'\0';
  TotalCores = 0;
  TotalThreads = 0;
  DetectedMemoryType[0] = L'\0';
  DetectedMemoryMHz = 0;
  MemoryTypeMixed = FALSE;
  MemorySpeedMixed = FALSE;
  PopulatedDimms = 0;
  KnownTypeDimms = 0;
  KnownSpeedDimms = 0;
  ParsingSmbios3 = FALSE;

  if (!PreferLegacySmbios) {
    Smbios3 = (SMBIOS_TABLE_3_0_ENTRY_POINT *)
                BlsFindConfigTableInMap(&gEfiSmbios3TableGuid, &Snapshot);
    if (Smbios3 != NULL &&
        /* Prove the fixed prefix before reading EntryPointLength, then prove the
           complete declared EPS before its checksum walks extension bytes. */
        BlsFirmwareRangeIsMappedInMap(&Snapshot, (UINTN)Smbios3, sizeof(*Smbios3)) &&
        Smbios3->EntryPointLength >= sizeof(*Smbios3) &&
        BlsFirmwareRangeIsMappedInMap(
          &Snapshot, (UINTN)Smbios3, (UINTN)Smbios3->EntryPointLength) &&
        BlsSmbios3EntryPointIsValid(Smbios3, (UINTN)Smbios3->EntryPointLength)) {
      ValidSmbios3EntryPointSeen = TRUE;
      Smbios3MajorVersion = Smbios3->MajorVersion;
      Smbios3MinorVersion = Smbios3->MinorVersion;
      Hdr = (SMBIOS_STRUCTURE *)(UINTN)Smbios3->TableAddress;
      TableLength = BlsFirmwareMappedPrefixLengthInMap(
                      &Snapshot,
                      (UINTN)Hdr,
                      (UINTN)Smbios3->TableMaximumSize
                      );
      if (TableLength < sizeof(SMBIOS_STRUCTURE) + 2U) {
        PreferLegacySmbios = TRUE;
        goto TrySmbiosTable;
      }
      SmbiosMajorVersion = Smbios3MajorVersion;
      SmbiosMinorVersion = Smbios3MinorVersion;
      MaxCount = 0;
      RequiresEndOfTable = TRUE;
      ParsingSmbios3 = TRUE;
    } else {
      PreferLegacySmbios = TRUE;
      goto TrySmbiosTable;
    }
  } else {
    UINTN EntryPointAvailableLength;

    Smbios = (SMBIOS_TABLE_ENTRY_POINT *)
               BlsFindConfigTableInMap(&gEfiSmbiosTableGuid, &Snapshot);
    if (Smbios == NULL ||
        !BlsFirmwareRangeIsMappedInMap(&Snapshot, (UINTN)Smbios, sizeof(*Smbios))) goto Done;
    EntryPointAvailableLength = sizeof(*Smbios);
    if (Smbios->EntryPointLength > sizeof(*Smbios)) {
      if (!BlsFirmwareRangeIsMappedInMap(
            &Snapshot, (UINTN)Smbios, (UINTN)Smbios->EntryPointLength)) goto Done;
      EntryPointAvailableLength = (UINTN)Smbios->EntryPointLength;
    }
    if (!BlsSmbios2EntryPointIsValid(Smbios, EntryPointAvailableLength)) goto Done;
    /* DSP0134 requires coexisting 32-bit and 64-bit entry points to report the
       same SMBIOS major.minor version. The legacy table is only a trustworthy
       recovery subset of a rejected SMBIOS 3.x table when that invariant holds.
       If the modern EPS itself was valid but the legacy EPS advertises a
       different version, fail closed instead of mixing potentially unrelated
       structure schemas during recovery. */
    if (ValidSmbios3EntryPointSeen &&
        (Smbios->MajorVersion != Smbios3MajorVersion ||
         Smbios->MinorVersion != Smbios3MinorVersion)) {
      goto Done;
    }
    TableLength = (UINTN)Smbios->TableLength;
    Hdr = (SMBIOS_STRUCTURE *)(UINTN)Smbios->TableAddress;
    ReportedCount = (UINTN)Smbios->NumberOfSmbiosStructures;
    MaximumStructureSize = (UINTN)Smbios->MaxStructureSize;
    HasReportedCount = TRUE;
    if (ReportedCount == 0) goto Done;
    MaxCount = ReportedCount;
    SmbiosMajorVersion = Smbios->MajorVersion;
    SmbiosMinorVersion = Smbios->MinorVersion;
    RequiresEndOfTable = BlsSmbiosVersionAtLeast(SmbiosMajorVersion, SmbiosMinorVersion, 2U, 2U);
    if (!BlsFirmwareRangeIsMappedInMap(&Snapshot, (UINTN)Hdr, TableLength)) goto Done;
  }

  if (!HasReportedCount) {
    MaxCount = BlsSmbiosStructureScanLimit(TableLength, 0);
  }

  if ((UINTN)Hdr > MAX_UINTN - TableLength) goto Done;
  Limit = (SMBIOS_STRUCTURE *)((UINT8 *)Hdr + TableLength);
  Count = 0;

  while (Hdr != NULL && (UINTN)Hdr < (UINTN)Limit && Count < MaxCount) {
    SMBIOS_STRUCTURE *NextHdr;
    UINTN Remaining;

    Remaining = (UINTN)Limit - (UINTN)Hdr;
    if (Remaining < sizeof(SMBIOS_STRUCTURE)) goto Done;
    if (Hdr->Length < sizeof(SMBIOS_STRUCTURE) || Hdr->Length > Remaining - 2) goto Done;
    NextHdr = BlsNextSmbiosStruct(Hdr, Limit);
    if (NextHdr == NULL) goto Done;
    if (HasReportedCount && !BlsSmbiosStructureFitsMaximum(Hdr, NextHdr, MaximumStructureSize)) goto Done;
    if (!BlsSmbiosMarkHandleUnique(SeenHandles, Hdr->Handle)) goto Done;
    Count++;
    if (Hdr->Type == SMBIOS_TYPE_END_OF_TABLE) {
      EndOfTableFound = TRUE;
      Hdr = NextHdr;
      break;
    }

    if (Hdr->Type == SMBIOS_TYPE_SYSTEM_INFORMATION &&
        Hdr->Length >= OFFSET_OF(SMBIOS_TABLE_TYPE1, ProductName) + sizeof(((SMBIOS_TABLE_TYPE1 *)0)->ProductName)) {
      SMBIOS_TABLE_TYPE1 *Type1 = (SMBIOS_TABLE_TYPE1 *)Hdr;
      UINTN ManufacturerBytes;
      UINTN ProductBytes;
      CHAR8 *Manufacturer = BlsGetSmbiosString(Hdr, Limit, Type1->Manufacturer, &ManufacturerBytes);
      CHAR8 *Product = BlsGetSmbiosString(Hdr, Limit, Type1->ProductName, &ProductBytes);
      BlsBuildNameFromSmbios(
        BoardFallback, BLS_MAX_TEXT,
        Manufacturer, ManufacturerBytes, Product, ProductBytes,
        SmbiosMajorVersion, SmbiosMinorVersion);
    } else if (Hdr->Type == SMBIOS_TYPE_BASEBOARD_INFORMATION &&
               Hdr->Length >= OFFSET_OF(SMBIOS_TABLE_TYPE2, ProductName) + sizeof(((SMBIOS_TABLE_TYPE2 *)0)->ProductName)) {
      SMBIOS_TABLE_TYPE2 *Type2 = (SMBIOS_TABLE_TYPE2 *)Hdr;
      UINTN ManufacturerBytes;
      UINTN ProductBytes;
      CHAR8 *Manufacturer = BlsGetSmbiosString(Hdr, Limit, Type2->Manufacturer, &ManufacturerBytes);
      CHAR8 *Product = BlsGetSmbiosString(Hdr, Limit, Type2->ProductName, &ProductBytes);
      CHAR16 Candidate[BLS_MAX_TEXT];
      BOOLEAN IsMotherboard;

      Candidate[0] = L'\0';
      IsMotherboard = FALSE;
      if (Hdr->Length >= OFFSET_OF(SMBIOS_TABLE_TYPE2, FeatureFlag) + sizeof(Type2->FeatureFlag) &&
          Type2->FeatureFlag.Motherboard != 0) {
        IsMotherboard = TRUE;
      }
      if (Hdr->Length >= OFFSET_OF(SMBIOS_TABLE_TYPE2, BoardType) + sizeof(Type2->BoardType) &&
          Type2->BoardType == BaseBoardTypeMotherBoard) {
        IsMotherboard = TRUE;
      }

      BlsBuildNameFromSmbios(
        Candidate, BLS_MAX_TEXT,
        Manufacturer, ManufacturerBytes, Product, ProductBytes,
        SmbiosMajorVersion, SmbiosMinorVersion);
      if (Candidate[0] != L'\0' && (!BoardFound || (IsMotherboard && !BoardIsMotherboard))) {
        BlsSafeStrCpy(Info->Board, BLS_MAX_TEXT, Candidate);
        BoardFound = TRUE;
        BoardIsMotherboard = IsMotherboard;
      }
    } else if (Hdr->Type == SMBIOS_TYPE_PROCESSOR_INFORMATION &&
               Hdr->Length >= OFFSET_OF(SMBIOS_TABLE_TYPE4, CurrentSpeed) + sizeof(((SMBIOS_TABLE_TYPE4 *)0)->CurrentSpeed)) {
      SMBIOS_TABLE_TYPE4 *Type4;
      UINT32 CoreCount;
      UINT32 EnabledCoreCount;
      UINT32 ThreadCount;
      BOOLEAN SocketPopulated;

      Type4 = (SMBIOS_TABLE_TYPE4 *)Hdr;
      CoreCount = 0;
      ThreadCount = 0;
      SocketPopulated = TRUE;

      if (Hdr->Length >= OFFSET_OF(SMBIOS_TABLE_TYPE4, Status) + sizeof(Type4->Status)) {
        SocketPopulated = (BOOLEAN)((Type4->Status & BIT6) != 0);
      }
      if (!SocketPopulated) {
        Hdr = NextHdr;
        continue;
      }

      if (Type4->CurrentSpeed != 0 && Type4->CurrentSpeed > Info->CpuMHz) {
        Info->CpuMHz = Type4->CurrentSpeed;
      }

      if (Hdr->Length >= OFFSET_OF(SMBIOS_TABLE_TYPE4, CoreCount) + sizeof(Type4->CoreCount)) {
        BOOLEAN HasExtendedCount;
        UINT16 ExtendedCount;

        HasExtendedCount = (BOOLEAN)(BlsSmbiosVersionAtLeast(SmbiosMajorVersion, SmbiosMinorVersion, 3U, 0U) &&
                                     Hdr->Length >= OFFSET_OF(SMBIOS_TABLE_TYPE4, CoreCount2) + sizeof(Type4->CoreCount2));
        ExtendedCount = HasExtendedCount ? Type4->CoreCount2 : 0U;
        (VOID)BlsDecodeSmbiosProcessorCount(Type4->CoreCount, HasExtendedCount, ExtendedCount, &CoreCount);
      }
      if (Hdr->Length >= OFFSET_OF(SMBIOS_TABLE_TYPE4, EnabledCoreCount) + sizeof(Type4->EnabledCoreCount)) {
        BOOLEAN HasExtendedCount;
        UINT16 ExtendedCount;

        EnabledCoreCount = 0U;
        HasExtendedCount = (BOOLEAN)(BlsSmbiosVersionAtLeast(SmbiosMajorVersion, SmbiosMinorVersion, 3U, 0U) &&
                                     Hdr->Length >= OFFSET_OF(SMBIOS_TABLE_TYPE4, EnabledCoreCount2) +
                                                    sizeof(Type4->EnabledCoreCount2));
        ExtendedCount = HasExtendedCount ? Type4->EnabledCoreCount2 : 0U;
        if (BlsDecodeSmbiosProcessorCount(Type4->EnabledCoreCount, HasExtendedCount, ExtendedCount, &EnabledCoreCount) &&
            (CoreCount == 0U || EnabledCoreCount <= CoreCount)) {
          CoreCount = EnabledCoreCount;
        }
      }
      if (Hdr->Length >= OFFSET_OF(SMBIOS_TABLE_TYPE4, ThreadCount) + sizeof(Type4->ThreadCount)) {
        BOOLEAN HasExtendedCount;
        UINT16 ExtendedCount;

        HasExtendedCount = (BOOLEAN)(BlsSmbiosVersionAtLeast(SmbiosMajorVersion, SmbiosMinorVersion, 3U, 0U) &&
                                     Hdr->Length >= OFFSET_OF(SMBIOS_TABLE_TYPE4, ThreadCount2) + sizeof(Type4->ThreadCount2));
        ExtendedCount = HasExtendedCount ? Type4->ThreadCount2 : 0U;
        (VOID)BlsDecodeSmbiosProcessorCount(Type4->ThreadCount, HasExtendedCount, ExtendedCount, &ThreadCount);
      }

      {
        UINT32 EnabledThreadCount;

        /* Thread Count is the socket capability, while SMBIOS 3.6+ Thread
           Enabled is the number actually enabled by firmware. Match the MP
           Services path by preferring the enabled count when it is present
           and self-consistent. Unknown/reserved or contradictory values leave
           the established total-thread fallback untouched. */
        if (BlsSmbiosEnabledThreadCount(
              Type4, SmbiosMajorVersion, SmbiosMinorVersion,
              ThreadCount, &EnabledThreadCount) &&
            (CoreCount == 0U || EnabledThreadCount >= CoreCount)) {
          ThreadCount = EnabledThreadCount;
        }
      }

      if (CoreCount > 0 && ThreadCount < CoreCount) {
        /* An unknown or inconsistent thread count must not make a populated
           socket report fewer runnable threads than enabled cores. */
        ThreadCount = CoreCount;
      }
      if (CoreCount > 0) {
        TotalCores = (TotalCores > MAX_UINT32 - CoreCount) ? MAX_UINT32 : TotalCores + CoreCount;
      }
      if (ThreadCount > 0) {
        TotalThreads = (TotalThreads > MAX_UINT32 - ThreadCount) ? MAX_UINT32 : TotalThreads + ThreadCount;
      }
    } else if (Hdr->Type == SMBIOS_TYPE_MEMORY_DEVICE &&
               Hdr->Length >= OFFSET_OF(SMBIOS_TABLE_TYPE17, Size) + sizeof(((SMBIOS_TABLE_TYPE17 *)0)->Size)) {
      SMBIOS_TABLE_TYPE17 *Type17;
      UINT64 SizeMB;

      Type17 = (SMBIOS_TABLE_TYPE17 *)Hdr;
      SizeMB = BlsSmbiosMemoryDeviceSizeMb(Type17, SmbiosMajorVersion, SmbiosMinorVersion);

      if (SizeMB > 0) {
        if (PopulatedDimms < MAX_UINT32) PopulatedDimms++;
        if (Info->MemoryMB <= MAX_UINT64 - SizeMB) {
          Info->MemoryMB += SizeMB;
        } else {
          Info->MemoryMB = MAX_UINT64;
        }

        {
          CHAR16 CurrentType[16];
          UINT32 CurrentMHz;

          CurrentType[0] = L'\0';
          if (Hdr->Length >= OFFSET_OF(SMBIOS_TABLE_TYPE17, MemoryType) + sizeof(Type17->MemoryType)) {
            BlsSafeStrCpy(CurrentType, ARRAY_SIZE(CurrentType), BlsMemoryTypeName(Type17->MemoryType));
          }

          CurrentMHz = BlsSmbiosMemoryDeviceSpeed(Type17, SmbiosMajorVersion, SmbiosMinorVersion);

          if (CurrentType[0] != L'\0') {
            if (KnownTypeDimms < MAX_UINT32) KnownTypeDimms++;
            if (DetectedMemoryType[0] == L'\0') {
              BlsSafeStrCpy(DetectedMemoryType, ARRAY_SIZE(DetectedMemoryType), CurrentType);
            } else if (!BlsStrEq(DetectedMemoryType, CurrentType)) {
              MemoryTypeMixed = TRUE;
            }
          }
          if (CurrentMHz != 0) {
            if (KnownSpeedDimms < MAX_UINT32) KnownSpeedDimms++;
            if (DetectedMemoryMHz == 0) DetectedMemoryMHz = CurrentMHz;
            else if (DetectedMemoryMHz != CurrentMHz) MemorySpeedMixed = TRUE;
          }
        }
      }
    }
    Hdr = NextHdr;
  }

  if (HasReportedCount && Count != ReportedCount) goto Done;
  if (RequiresEndOfTable && !EndOfTableFound) goto Done;
  if (!EndOfTableFound && Hdr != Limit) goto Done;

  if (TotalCores > 0) Info->Cores = TotalCores;
  if (TotalThreads > 0) Info->LogicalProcessors = TotalThreads;
  if (MemoryTypeMixed) {
    BlsSafeStrCpy(Info->MemoryType, ARRAY_SIZE(Info->MemoryType), L"Mixed");
  } else if (PopulatedDimms != 0 && KnownTypeDimms == PopulatedDimms && DetectedMemoryType[0] != L'\0') {
    BlsSafeStrCpy(Info->MemoryType, ARRAY_SIZE(Info->MemoryType), DetectedMemoryType);
  }
  Info->MemoryMHz = (PopulatedDimms != 0 && KnownSpeedDimms == PopulatedDimms && !MemorySpeedMixed)
                      ? DetectedMemoryMHz : 0U;
  Info->MemorySpeedIsMtps = (BOOLEAN)(Info->MemoryMHz != 0U &&
                                      BlsSmbiosVersionAtLeast(SmbiosMajorVersion, SmbiosMinorVersion, 3U, 1U));

  if (!BoardFound && BoardFallback[0] != L'\0') {
    BlsSafeStrCpy(Info->Board, BLS_MAX_TEXT, BoardFallback);
  }
  TableValid = TRUE;

Done:
  if (!TableValid && ParsingSmbios3 && !PreferLegacySmbios) {
    /* DSP0134 permits both entry points to coexist. A corrupt/inaccessible
       SMBIOS 3.x table must not suppress a separately valid same-version
       32-bit subset; the retry path enforces that major.minor invariant. */
    PreferLegacySmbios = TRUE;
    ResetForRetry = TRUE;
    goto TrySmbiosTable;
  }
  if (!TableValid) {
    CopyMem(Info, &OriginalInfo, sizeof(*Info));
  }
  BlsFirmwareMemoryMapRelease(&Snapshot);
  FreePool(SeenHandles);
}

static CONST CHAR16 *BlsGpuVendorName(IN UINT16 VendorId) {
  switch (VendorId) {
    case 0x1002: return L"AMD";
    case 0x8086: return L"Intel";
    case 0x10DE: return L"NVIDIA";
    default:     return L"GPU";
  }
}

static CONST CHAR16 *BlsGpuFriendlyName(IN UINT16 VendorId, IN UINT16 DeviceId) {
  if (VendorId == 0x1002) {
    switch (DeviceId) {
      case 0x67DF: return L"AMD Radeon RX 470/480/570/580";
      case 0x67FF: return L"AMD Radeon RX 550/560";
      case 0x687F: return L"AMD Radeon RX Vega 56/64";
      case 0x66AF: return L"AMD Radeon VII";
      case 0x7340: return L"AMD Radeon RX 5500 XT";
      case 0x731F: return L"AMD Radeon RX 5700 XT";
      case 0x73FF: return L"AMD Radeon RX 6600/6600 XT/6650 XT";
      case 0x73DF: return L"AMD Radeon RX 6700/6750 XT";
      case 0x73BF: return L"AMD Radeon RX 6800/6900/6950 XT";
      default:     return NULL;
    }
  }
  if (VendorId == 0x8086) {
    switch (DeviceId) {
      case 0x3E9B: return L"Intel UHD Graphics 630";
      case 0x9BC5: return L"Intel UHD Graphics 630";
      case 0x4C8A: return L"Intel Iris Xe Graphics";
      case 0x46A6: return L"Intel Iris Xe Graphics";
      case 0x7D67: return L"Intel Graphics";
      case 0x56A0: return L"Intel Arc Graphics";
      default:     return NULL;
    }
  }
  return NULL;
}

static BOOLEAN BlsDevicePathIsPrefix(
  IN EFI_DEVICE_PATH_PROTOCOL *Prefix,
  IN EFI_DEVICE_PATH_PROTOCOL *Full
  ) {
  UINTN PrefixSize;
  UINTN FullSize;
  UINTN CompareSize;

  if (Prefix == NULL || Full == NULL ||
      !IsDevicePathValid(Prefix, BLS_MAX_DEVICE_PATH_BYTES) ||
      !IsDevicePathValid(Full, BLS_MAX_DEVICE_PATH_BYTES)) return FALSE;
  PrefixSize = GetDevicePathSize(Prefix);
  FullSize = GetDevicePathSize(Full);
  if (PrefixSize < sizeof(EFI_DEVICE_PATH_PROTOCOL) || FullSize < sizeof(EFI_DEVICE_PATH_PROTOCOL)) return FALSE;
  CompareSize = PrefixSize - sizeof(EFI_DEVICE_PATH_PROTOCOL);
  if (CompareSize == 0 || CompareSize > FullSize - sizeof(EFI_DEVICE_PATH_PROTOCOL)) return FALSE;
  return (BOOLEAN)(CompareMem(Prefix, Full, CompareSize) == 0);
}

static EFI_DEVICE_PATH_PROTOCOL *BlsFindConsoleGopDevicePath(VOID) {
  EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop;
  EFI_GRAPHICS_OUTPUT_PROTOCOL *ConsoleGop;
  EFI_DEVICE_PATH_PROTOCOL *ConsolePath;
  EFI_DEVICE_PATH_PROTOCOL *OnlyPath;
  EFI_HANDLE *Handles;
  UINTN Count;
  UINTN Index;
  UINTN ValidGopPaths;
  EFI_STATUS Status;

  if (gBS == NULL || gBS->LocateHandle == NULL) return NULL;
  ConsoleGop = NULL;
  ConsolePath = NULL;
  if (gST != NULL && gST->ConsoleOutHandle != NULL) {
    Status = BlsGetProtocolInterface(
               gST->ConsoleOutHandle,
               &gEfiGraphicsOutputProtocolGuid,
               (VOID **)&ConsoleGop
               );
    (VOID)BlsGetProtocolInterface(
            gST->ConsoleOutHandle,
            &gEfiDevicePathProtocolGuid,
            (VOID **)&ConsolePath
            );
    if (ConsolePath != NULL && !IsDevicePathValid(ConsolePath, BLS_MAX_DEVICE_PATH_BYTES)) ConsolePath = NULL;
    if (Status == EFI_SUCCESS && ConsoleGop != NULL && ConsolePath != NULL) return ConsolePath;
    if (Status != EFI_SUCCESS) ConsoleGop = NULL;
  }

  Handles = NULL;
  Count = 0;
  Status = BlsLocateProtocolHandlesBounded(
             &gEfiGraphicsOutputProtocolGuid,
             BLS_PCI_HANDLE_SCAN_MAX,
             &Handles,
             &Count
             );
  if (Status != EFI_SUCCESS) return NULL;

  OnlyPath = NULL;
  ValidGopPaths = 0;
  for (Index = 0; Index < Count; Index++) {
    EFI_DEVICE_PATH_PROTOCOL *GopPath;
    if (Handles[Index] == NULL) continue;
    Gop = NULL;
    Status = BlsGetProtocolInterface(Handles[Index], &gEfiGraphicsOutputProtocolGuid, (VOID **)&Gop);
    if (Status != EFI_SUCCESS || Gop == NULL) continue;
    GopPath = NULL;
    (VOID)BlsGetProtocolInterface(Handles[Index], &gEfiDevicePathProtocolGuid, (VOID **)&GopPath);
    if (GopPath == NULL || !IsDevicePathValid(GopPath, BLS_MAX_DEVICE_PATH_BYTES)) continue;

    /* ConsoleOutHandle may expose GOP directly without publishing a Device Path.
       In that valid topology, the protocol interface identity still tells us
       exactly which GOP handle backs the active console. Preserve that identity
       across enumeration instead of discarding it and later choosing an
       arbitrary PCI display controller on multi-GPU systems. */
    if (ConsoleGop != NULL && Gop == ConsoleGop) {
      FreePool(Handles);
      return GopPath;
    }

    ValidGopPaths++;
    if (ValidGopPaths == 1U) OnlyPath = GopPath;
    if (ConsolePath != NULL &&
        (BlsDevicePathIsPrefix(GopPath, ConsolePath) || BlsDevicePathIsPrefix(ConsolePath, GopPath))) {
      FreePool(Handles);
      return GopPath;
    }
  }

  FreePool(Handles);
  return (ValidGopPaths == 1U) ? OnlyPath : NULL;
}

static VOID BlsFormatGpu(OUT CHAR16 *Out, IN UINTN OutChars, IN UINT16 VendorId, IN UINT16 DeviceId) {
  CONST CHAR16 *Name;
  if (Out == NULL || OutChars == 0) return;
  Name = BlsGpuFriendlyName(VendorId, DeviceId);
  if (Name != NULL) UnicodeSPrint(Out, OutChars * sizeof(CHAR16), L"%s %04x:%04x", Name, VendorId, DeviceId);
  else UnicodeSPrint(Out, OutChars * sizeof(CHAR16), L"%s GPU %04x:%04x", BlsGpuVendorName(VendorId), VendorId, DeviceId);
}

static VOID BlsReadGpuInfo(IN OUT BLS_HARDWARE_INFO *Info) {
  EFI_STATUS Status;
  EFI_HANDLE *Handles;
  UINTN HandleCount;
  UINTN Index;
  EFI_PCI_IO_PROTOCOL *PciIo;
  EFI_DEVICE_PATH_PROTOCOL *ConsoleGopPath;
  UINT32 IdReg;
  UINT32 ClassReg;
  UINT8 BaseClass;
  UINT8 SubClass;
  UINT16 VendorId;
  UINT16 DeviceId;
  BOOLEAN FallbackSet;

  if (Info == NULL) return;
  if (gBS == NULL || gBS->LocateHandle == NULL) return;

  ConsoleGopPath = BlsFindConsoleGopDevicePath();

  Handles = NULL;
  HandleCount = 0;
  Status = BlsLocateProtocolHandlesBounded(
             &gEfiPciIoProtocolGuid,
             BLS_PCI_HANDLE_SCAN_MAX,
             &Handles,
             &HandleCount
             );
  if (Status != EFI_SUCCESS) return;

  FallbackSet = FALSE;
  for (Index = 0; Index < HandleCount; Index++) {
    EFI_DEVICE_PATH_PROTOCOL *PciPath;
    BOOLEAN ActivePath;

    if (Handles[Index] == NULL) continue;
    PciIo = NULL;
    Status = BlsGetProtocolInterface(Handles[Index], &gEfiPciIoProtocolGuid, (VOID **)&PciIo);
    if (Status != EFI_SUCCESS || PciIo == NULL || PciIo->Pci.Read == NULL) continue;

    IdReg = 0xFFFFFFFF;
    ClassReg = 0;
    Status = PciIo->Pci.Read(PciIo, EfiPciIoWidthUint32, 0x00, 1, &IdReg);
    if (Status != EFI_SUCCESS || IdReg == 0xFFFFFFFF) continue;
    Status = PciIo->Pci.Read(PciIo, EfiPciIoWidthUint32, 0x08, 1, &ClassReg);
    if (Status != EFI_SUCCESS) continue;

    VendorId = (UINT16)(IdReg & 0xFFFF);
    DeviceId = (UINT16)((IdReg >> 16) & 0xFFFF);
    BaseClass = (UINT8)((ClassReg >> 24) & 0xFF);
    SubClass = (UINT8)((ClassReg >> 16) & 0xFF);
    if (BaseClass != 0x03 || (SubClass != 0x00 && SubClass != 0x02 && SubClass != 0x80)) continue;

    PciPath = NULL;
    (VOID)BlsGetProtocolInterface(Handles[Index], &gEfiDevicePathProtocolGuid, (VOID **)&PciPath);
    ActivePath = BlsDevicePathIsPrefix(PciPath, ConsoleGopPath);
    if (!FallbackSet || ActivePath) {
      BlsFormatGpu(Info->Gpu, BLS_MAX_TEXT, VendorId, DeviceId);
      FallbackSet = TRUE;
    }
    if (ActivePath) break;
  }

  FreePool(Handles);
}

static VOID BlsNormalizeHardwareInfo(IN OUT BLS_HARDWARE_INFO *Info) {
  if (Info == NULL) return;
  if (Info->Cores == 0) Info->Cores = 1;
  if (Info->LogicalProcessors == 0) Info->LogicalProcessors = Info->Cores;
  if (Info->LogicalProcessors < Info->Cores) Info->LogicalProcessors = Info->Cores;
  if (Info->Gpu[0] == L'\0') BlsSafeStrCpy(Info->Gpu, BLS_MAX_TEXT, L"Unknown GPU");
}

VOID BlsReadHardwareInfo(OUT BLS_HARDWARE_INFO *Info) {
  if (Info == NULL) return;
  ZeroMem(Info, sizeof(*Info));
  BlsSafeStrCpy(Info->Cpu, BLS_MAX_TEXT, L"Unknown CPU");
  BlsSafeStrCpy(Info->Board, BLS_MAX_TEXT, L"Unknown board");
  BlsSafeStrCpy(Info->Gpu, BLS_MAX_TEXT, L"Unknown GPU");
  Info->LogicalProcessors = 1;
  Info->Cores = 1;
  BlsReadCpuBrand(Info->Cpu, BLS_MAX_TEXT);
  BlsReadCpuTopologyFallback(&Info->LogicalProcessors, &Info->Cores);
  BlsReadSmbios(Info);
  /* MP Services reports the logical CPUs enabled for this boot and their
     package/core/thread locations. Prefer it over CPUID diagnostic counts and
     static SMBIOS totals when the protocol is complete and self-consistent. */
  (VOID)BlsReadMpServicesTopology(&Info->LogicalProcessors, &Info->Cores);
  BlsReadMemoryMapFallback(Info);
  BlsReadGpuInfo(Info);
  BlsNormalizeHardwareInfo(Info);
}

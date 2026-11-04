/** @file
  BootLoaderSelector boot entry scanning and launching.
  Credits: ©DNMTechLabs / dnmtechlabs@gmail.com
**/
#include "BootScan.h"
#include "Utils.h"
#include <Library/BaseMemoryLib.h>
#include <Library/BaseLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/DevicePath.h>
#include <Protocol/SimpleFileSystem.h>
#include <Guid/FileSystemInfo.h>
#include <Guid/FileInfo.h>

#define BLS_FILE_INFO_MIN_SIZE  (SIZE_OF_EFI_FILE_INFO + sizeof(CHAR16))
/* A selector should never need to traverse an unbounded firmware handle list. */
#define BLS_FS_HANDLE_SCAN_MAX  1024U
/* ExitData is optional diagnostic text. Never let firmware-reported metadata
   make the selector scan more UTF-16 data than its diagnostic path can retain. */
#define BLS_EXIT_DATA_SCAN_MAX_BYTES (BLS_MAX_PATH * sizeof(CHAR16))
/* UEFI Boot Manager sequence: after LoadImage succeeds, arm a five-minute
   watchdog before StartImage and disable it if the child returns. Codes
   0x0000..0xFFFF are reserved to firmware. */
#define BLS_CHILD_WATCHDOG_SECONDS 300U
#define BLS_CHILD_WATCHDOG_CODE    0x00010000ULL

static EFI_STATUS BlsSetChildBootWatchdog(IN BOOLEAN Enable) {
  if (gBS == NULL || gBS->SetWatchdogTimer == NULL) return EFI_UNSUPPORTED;
  if (Enable) {
    return gBS->SetWatchdogTimer(
                  BLS_CHILD_WATCHDOG_SECONDS, BLS_CHILD_WATCHDOG_CODE, 0U, NULL);
  }
  return gBS->SetWatchdogTimer(0U, 0U, 0U, NULL);
}

static CONST UINT32 mSha256K[64] = {
  0x428a2f98U,0x71374491U,0xb5c0fbcfU,0xe9b5dba5U,0x3956c25bU,0x59f111f1U,0x923f82a4U,0xab1c5ed5U,
  0xd807aa98U,0x12835b01U,0x243185beU,0x550c7dc3U,0x72be5d74U,0x80deb1feU,0x9bdc06a7U,0xc19bf174U,
  0xe49b69c1U,0xefbe4786U,0x0fc19dc6U,0x240ca1ccU,0x2de92c6fU,0x4a7484aaU,0x5cb0a9dcU,0x76f988daU,
  0x983e5152U,0xa831c66dU,0xb00327c8U,0xbf597fc7U,0xc6e00bf3U,0xd5a79147U,0x06ca6351U,0x14292967U,
  0x27b70a85U,0x2e1b2138U,0x4d2c6dfcU,0x53380d13U,0x650a7354U,0x766a0abbU,0x81c2c92eU,0x92722c85U,
  0xa2bfe8a1U,0xa81a664bU,0xc24b8b70U,0xc76c51a3U,0xd192e819U,0xd6990624U,0xf40e3585U,0x106aa070U,
  0x19a4c116U,0x1e376c08U,0x2748774cU,0x34b0bcb5U,0x391c0cb3U,0x4ed8aa4aU,0x5b9cca4fU,0x682e6ff3U,
  0x748f82eeU,0x78a5636fU,0x84c87814U,0x8cc70208U,0x90befffaU,0xa4506cebU,0xbef9a3f7U,0xc67178f2U
};

static UINT32 BlsRor32(IN UINT32 Value, IN UINTN Bits) {
  /* SHA-256 is defined over 32-bit words modulo 2^32. Promote the shift
     source explicitly so integer-overflow sanitizers do not mistake the
     intentional discarded high bits for an arithmetic defect. Optimizing
     compilers reduce this back to the same native 32-bit rotate sequence. */
  return (UINT32)(((UINT64)Value >> Bits) | ((UINT64)Value << (32U - Bits)));
}

static VOID BlsSha256Transform(IN OUT UINT32 H[8], IN CONST UINT8 Block[64]) {
  UINT32 W[64];
  UINT32 A;
  UINT32 B;
  UINT32 C;
  UINT32 D;
  UINT32 E;
  UINT32 F;
  UINT32 G;
  UINT32 HH;
  UINTN Index;

  if (H == NULL || Block == NULL) return;

  for (Index = 0; Index < 16U; Index++) {
    UINTN Offset;
    Offset = Index * 4U;
    W[Index] = ((UINT32)Block[Offset] << 24) |
               ((UINT32)Block[Offset + 1U] << 16) |
               ((UINT32)Block[Offset + 2U] << 8) |
               (UINT32)Block[Offset + 3U];
  }
  for (Index = 16U; Index < 64U; Index++) {
    UINT32 S0;
    UINT32 S1;
    S0 = BlsRor32(W[Index - 15U], 7U) ^
         BlsRor32(W[Index - 15U], 18U) ^
         (W[Index - 15U] >> 3);
    S1 = BlsRor32(W[Index - 2U], 17U) ^
         BlsRor32(W[Index - 2U], 19U) ^
         (W[Index - 2U] >> 10);
    W[Index] = (UINT32)((UINT64)W[Index - 16U] + S0 + W[Index - 7U] + S1);
  }

  A = H[0];
  B = H[1];
  C = H[2];
  D = H[3];
  E = H[4];
  F = H[5];
  G = H[6];
  HH = H[7];
  for (Index = 0; Index < 64U; Index++) {
    UINT32 S0;
    UINT32 S1;
    UINT32 Ch;
    UINT32 Maj;
    UINT32 T1;
    UINT32 T2;

    S1 = BlsRor32(E, 6U) ^ BlsRor32(E, 11U) ^ BlsRor32(E, 25U);
    Ch = (E & F) ^ ((~E) & G);
    T1 = (UINT32)((UINT64)HH + S1 + Ch + mSha256K[Index] + W[Index]);
    S0 = BlsRor32(A, 2U) ^ BlsRor32(A, 13U) ^ BlsRor32(A, 22U);
    Maj = (A & B) ^ (A & C) ^ (B & C);
    T2 = (UINT32)((UINT64)S0 + Maj);
    HH = G;
    G = F;
    F = E;
    E = (UINT32)((UINT64)D + T1);
    D = C;
    C = B;
    B = A;
    A = (UINT32)((UINT64)T1 + T2);
  }

  H[0] = (UINT32)((UINT64)H[0] + A);
  H[1] = (UINT32)((UINT64)H[1] + B);
  H[2] = (UINT32)((UINT64)H[2] + C);
  H[3] = (UINT32)((UINT64)H[3] + D);
  H[4] = (UINT32)((UINT64)H[4] + E);
  H[5] = (UINT32)((UINT64)H[5] + F);
  H[6] = (UINT32)((UINT64)H[6] + G);
  H[7] = (UINT32)((UINT64)H[7] + HH);
}

static BOOLEAN BlsSha256(IN CONST UINT8 *Data, IN UINTN DataSize, OUT UINT8 Digest[32]) {
  UINT32 H[8] = {
    0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
    0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U
  };
  UINT8 Tail[128];
  UINTN Offset;
  UINTN TailSize;
  UINTN PaddedSize;
  UINTN Index;
  UINT64 BitLength;

  if (Data == NULL || Digest == NULL || DataSize > BLS_MAX_DEVICE_PATH_BYTES ||
      DataSize > MAX_UINT64 / 8U) {
    return FALSE;
  }

  Offset = 0U;
  while (DataSize - Offset >= 64U) {
    BlsSha256Transform(H, Data + Offset);
    Offset += 64U;
  }

  ZeroMem(Tail, sizeof(Tail));
  TailSize = DataSize - Offset;
  if (TailSize > 0U) CopyMem(Tail, Data + Offset, TailSize);
  Tail[TailSize] = 0x80U;
  PaddedSize = (TailSize + 1U + sizeof(UINT64) <= 64U) ? 64U : 128U;
  BitLength = (UINT64)DataSize * 8U;
  for (Index = 0; Index < sizeof(UINT64); Index++) {
    Tail[PaddedSize - 1U - Index] = (UINT8)(BitLength >> (Index * 8U));
  }

  BlsSha256Transform(H, Tail);
  if (PaddedSize == 128U) BlsSha256Transform(H, Tail + 64U);

  for (Index = 0; Index < 8U; Index++) {
    Digest[Index * 4U] = (UINT8)(H[Index] >> 24);
    Digest[Index * 4U + 1U] = (UINT8)(H[Index] >> 16);
    Digest[Index * 4U + 2U] = (UINT8)(H[Index] >> 8);
    Digest[Index * 4U + 3U] = (UINT8)H[Index];
  }
  return TRUE;
}

static BOOLEAN BlsDevicePathGptGuid(IN EFI_DEVICE_PATH_PROTOCOL *DevicePath, OUT EFI_GUID *Guid) {
  EFI_DEVICE_PATH_PROTOCOL *Node;
  if (DevicePath == NULL || Guid == NULL || !IsDevicePathValid(DevicePath, BLS_MAX_DEVICE_PATH_BYTES)) return FALSE;
  for (Node = DevicePath; !IsDevicePathEnd(Node); Node = NextDevicePathNode(Node)) {
    if (DevicePathType(Node) == MEDIA_DEVICE_PATH && DevicePathSubType(Node) == MEDIA_HARDDRIVE_DP) {
      HARDDRIVE_DEVICE_PATH *Hd = (HARDDRIVE_DEVICE_PATH *)Node;
      /* UEFI 2.11 §10.3.5.1 fixes this node at 42 bytes and defines a GPT
         identity only when Partition Format is GPT and Signature Type is GUID.
         Do not persist a malformed/MBR node as a GPT volume merely because its
         signature-type byte happens to contain SIGNATURE_TYPE_GUID. */
      if (DevicePathNodeLength(Node) == sizeof(*Hd) &&
          Hd->MBRType == MBR_TYPE_EFI_PARTITION_TABLE_HEADER &&
          Hd->SignatureType == SIGNATURE_TYPE_GUID) {
        CopyMem(Guid, Hd->Signature, sizeof(*Guid));
        return TRUE;
      }
    }
  }
  return FALSE;
}

static VOID BlsFormatSha256Identity(IN CONST UINT8 Digest[32], OUT CHAR16 *Text, IN UINTN TextChars) {
  static CONST CHAR16 Hex[] = L"0123456789abcdef";
  UINTN I;
  UINTN Pos;
  if (Digest == NULL || Text == NULL || TextChars < 74U) return;
  BlsSafeStrCpy(Text, TextChars, L"DPSHA256-");
  Pos = 9U;
  for (I = 0; I < 32U; I++) {
    Text[Pos++] = Hex[(Digest[I] >> 4) & 0x0FU];
    Text[Pos++] = Hex[Digest[I] & 0x0FU];
  }
  Text[Pos] = L'\0';
}

static CONST CHAR16 *mKnownPaths[] = {
  L"\\EFI\\OC\\OpenCore.efi",
  L"\\EFI\\BOOT\\OpenCore.efi",
  L"\\EFI\\CLOVER\\CLOVERX64.efi",
  L"\\EFI\\BOOT\\CLOVERX64.efi",
  NULL
};
#define BLS_KNOWN_PATH_COUNT (ARRAY_SIZE(mKnownPaths) - 1U)

EFI_STATUS BlsLocateFsHandles(OUT EFI_HANDLE **Handles, OUT UINTN *Count) {
  return BlsLocateProtocolHandlesBounded(
           &gEfiSimpleFileSystemProtocolGuid,
           BLS_FS_HANDLE_SCAN_MAX,
           Handles,
           Count
           );
}

static EFI_STATUS BlsRegularFileExistsInRoot(
  IN EFI_FILE_PROTOCOL *Root,
  IN CONST CHAR16 *Path,
  OUT BOOLEAN *Exists
  ) {
  EFI_FILE_PROTOCOL *File;
  EFI_FILE_INFO *Info;
  EFI_STATUS Status;
  EFI_STATUS CloseStatus;
  UINTN InfoSize;

  if (Exists == NULL) return EFI_INVALID_PARAMETER;
  *Exists = FALSE;
  if (Root == NULL || Path == NULL) return EFI_INVALID_PARAMETER;

  File = NULL;
  Status = BlsFileOpen(Root, &File, Path, EFI_FILE_MODE_READ, 0);
  if (Status == EFI_NOT_FOUND) return EFI_SUCCESS;
  if (Status != EFI_SUCCESS) return Status;

  Info = NULL;
  InfoSize = 0;
  Status = BlsFileGetInfoAlloc(
             File,
             &gEfiFileInfoGuid,
             BLS_FILE_INFO_MIN_SIZE,
             (VOID **)&Info,
             &InfoSize
             );

  if (Status == EFI_SUCCESS) {
    if (Info == NULL || Info->Size < BLS_FILE_INFO_MIN_SIZE || Info->Size > InfoSize) {
      Status = EFI_COMPROMISED_DATA;
    } else if ((Info->Attribute & EFI_FILE_DIRECTORY) == 0 && Info->FileSize > 0) {
      *Exists = TRUE;
    }
  }

  if (Info != NULL) FreePool(Info);
  CloseStatus = BlsFileClose(&File);
  if (Status == EFI_SUCCESS && CloseStatus != EFI_SUCCESS) Status = CloseStatus;
  return Status;
}

static VOID BlsReadVolumeLabelFromRoot(IN EFI_FILE_PROTOCOL *Root, OUT CHAR16 *Label, IN UINTN LabelChars) {
  EFI_FILE_SYSTEM_INFO *Info;
  UINTN InfoSize;
  EFI_STATUS Status;

  BlsSafeStrCpy(Label, LabelChars, L"EFI Volume");
  if (Root == NULL) return;

  Info = NULL;
  InfoSize = 0;
  Status = BlsFileGetInfoAlloc(
             Root,
             &gEfiFileSystemInfoGuid,
             SIZE_OF_EFI_FILE_SYSTEM_INFO + sizeof(CHAR16),
             (VOID **)&Info,
             &InfoSize
             );

  if (Status == EFI_SUCCESS && Info != NULL &&
      Info->Size <= InfoSize &&
      Info->Size >= SIZE_OF_EFI_FILE_SYSTEM_INFO + sizeof(CHAR16)) {
    UINTN AvailableChars;
    UINTN LabelLength;

    AvailableChars = ((UINTN)Info->Size - SIZE_OF_EFI_FILE_SYSTEM_INFO) / sizeof(CHAR16);
    LabelLength = BlsStrLen16Bounded(Info->VolumeLabel, AvailableChars);
    if (LabelLength > 0 && LabelLength < AvailableChars) {
      BlsSafeStrCpy(Label, LabelChars, Info->VolumeLabel);
    }
  }
  if (Info != NULL) FreePool(Info);
}

static VOID BlsReadVolumeDevicePath(IN EFI_DEVICE_PATH_PROTOCOL *DevicePath, OUT CHAR16 *Text, IN UINTN TextChars) {
  CHAR16 *DevicePathText;
  BOOLEAN NeedCompactIdentity;

  if (Text == NULL || TextChars == 0) return;
  Text[0] = L'\0';
  if (DevicePath == NULL || !IsDevicePathValid(DevicePath, BLS_MAX_DEVICE_PATH_BYTES)) return;

  /* Persist the complete, non-shortcut representation when it fits. Display-only
     or shortcut text may omit node details and is not a stable identity for
     autoboot. If the text form is too long, use GPT GUID or SHA-256 below. */
  DevicePathText = ConvertDevicePathToText(DevicePath, FALSE, FALSE);
  NeedCompactIdentity = TRUE;
  if (DevicePathText != NULL) {
    /* A partial device path is not a safe persistent volume identity. When the
       firmware text exceeds the fixed field, never store a prefix: derive a
       compact identity from the complete validated binary device path instead. */
    if (BlsStrLen16Bounded(DevicePathText, TextChars) < TextChars) {
      BlsSafeStrCpy(Text, TextChars, DevicePathText);
      NeedCompactIdentity = FALSE;
    }
    FreePool(DevicePathText);
  }

  /* ConvertDevicePathToText() is allowed to return NULL on allocation failure.
     A valid GPT/binary device path can still provide a strong identity without
     allocating its textual representation, so do not silently lose remembered
     selection identity merely because that optional conversion ran out of memory. */
  if (NeedCompactIdentity) {
    UINTN DevicePathSize;
    EFI_GUID PartitionGuid;
    UINT8 Digest[32];

    /* Do not fall back to an ambiguous volume label solely because the text
       form is unavailable or longer than the persisted field. A compact identity
       derived from the complete validated device path remains fail-closed. */
    DevicePathSize = GetDevicePathSize(DevicePath);
    if (BlsDevicePathGptGuid(DevicePath, &PartitionGuid)) {
      UnicodeSPrint(Text, TextChars * sizeof(CHAR16),
                    L"GPT-%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                    PartitionGuid.Data1, PartitionGuid.Data2, PartitionGuid.Data3,
                    PartitionGuid.Data4[0], PartitionGuid.Data4[1], PartitionGuid.Data4[2], PartitionGuid.Data4[3],
                    PartitionGuid.Data4[4], PartitionGuid.Data4[5], PartitionGuid.Data4[6], PartitionGuid.Data4[7]);
    } else if (DevicePathSize > 0 && DevicePathSize <= BLS_MAX_DEVICE_PATH_BYTES &&
               BlsSha256((CONST UINT8 *)DevicePath, DevicePathSize, Digest)) {
      BlsFormatSha256Identity(Digest, Text, TextChars);
    }
  }
}

static CHAR16 BlsChar16Lower(IN CHAR16 C) {
  if (C >= L'A' && C <= L'Z') return (CHAR16)(C - L'A' + L'a');
  return C;
}

static BOOLEAN BlsStrContains16Insensitive(IN CONST CHAR16 *Text, IN CONST CHAR16 *Needle) {
  UINTN TextLen;
  UINTN NeedleLen;
  UINTN Index;

  if (Text == NULL || Needle == NULL) return FALSE;
  TextLen = BlsStrLen16Bounded(Text, BLS_MAX_VOLUME_ID);
  NeedleLen = BlsStrLen16Bounded(Needle, BLS_MAX_VOLUME_ID);
  if (NeedleLen == 0 || TextLen < NeedleLen) return FALSE;

  for (Index = 0; Index + NeedleLen <= TextLen; Index++) {
    UINTN SubIndex;
    BOOLEAN Match;
    Match = TRUE;
    for (SubIndex = 0; SubIndex < NeedleLen; SubIndex++) {
      if (BlsChar16Lower(Text[Index + SubIndex]) != BlsChar16Lower(Needle[SubIndex])) {
        Match = FALSE;
        break;
      }
    }
    if (Match) return TRUE;
  }
  return FALSE;
}

static BOOLEAN BlsUsbDevicePathNodeIsConforming(IN CONST EFI_DEVICE_PATH_PROTOCOL *Node) {
  UINTN Length;
  UINT8 SubType;

  if (Node == NULL || DevicePathType(Node) != MESSAGING_DEVICE_PATH) return FALSE;
  Length = DevicePathNodeLength(Node);
  SubType = DevicePathSubType(Node);

  /* UEFI §10.3.4.5/.7 fixes the USB and USB-Class node sizes and bounds
     USB-WWID serial data to 64-or-fewer UTF-16 characters. USB-WWID requires
     a serial-number string; devices without one must use MSG_USB_DP. Generic
     IsDevicePathValid() validates traversal, not every subtype's wire format. */
  if (SubType == MSG_USB_DP) return (BOOLEAN)(Length == sizeof(USB_DEVICE_PATH));
  if (SubType == MSG_USB_CLASS_DP) return (BOOLEAN)(Length == sizeof(USB_CLASS_DEVICE_PATH));
  if (SubType == MSG_USB_WWID_DP) {
    UINTN SerialBytes;
    if (Length < sizeof(USB_WWID_DEVICE_PATH) + sizeof(CHAR16)) return FALSE;
    SerialBytes = Length - sizeof(USB_WWID_DEVICE_PATH);
    if ((SerialBytes % sizeof(CHAR16)) != 0) return FALSE;
    return (BOOLEAN)(SerialBytes <= 64U * sizeof(CHAR16));
  }
  return FALSE;
}

static BOOLEAN BlsDevicePathHasUsbNode(IN EFI_DEVICE_PATH_PROTOCOL *DevicePath) {
  EFI_DEVICE_PATH_PROTOCOL *Node;

  if (DevicePath == NULL || !IsDevicePathValid(DevicePath, BLS_MAX_DEVICE_PATH_BYTES)) return FALSE;
  Node = DevicePath;
  while (!IsDevicePathEnd(Node)) {
    if (BlsUsbDevicePathNodeIsConforming(Node)) return TRUE;
    Node = NextDevicePathNode(Node);
  }
  return FALSE;
}

static BOOLEAN BlsVolumeIsUsb(IN EFI_DEVICE_PATH_PROTOCOL *DevicePath, IN CONST CHAR16 *VolumeDevicePath) {
  /* When binary device-path data exists, it is the authoritative UEFI wire
     format. Do not let a textual conversion reclassify a malformed/non-USB
     binary path merely because it contains a USB-looking token. */
  if (DevicePath != NULL) return BlsDevicePathHasUsbNode(DevicePath);

  if (VolumeDevicePath != NULL) {
    if (BlsStrContains16Insensitive(VolumeDevicePath, L"Usb(") != FALSE) return TRUE;
    if (BlsStrContains16Insensitive(VolumeDevicePath, L"UsbWwid(") != FALSE) return TRUE;
    if (BlsStrContains16Insensitive(VolumeDevicePath, L"UsbClass(") != FALSE) return TRUE;
  }

  return FALSE;
}

static BLS_LOADER_TYPE BlsTypeFromPath(IN CONST CHAR16 *Path) {
  if (Path == NULL) return BlsLoaderOther;
  if (StrStr(Path, L"OpenCore.efi") != NULL) return BlsLoaderOpenCore;
  if (StrStr(Path, L"CLOVERX64.efi") != NULL) return BlsLoaderClover;
  return BlsLoaderOther;
}

static BOOLEAN BlsDuplicateEntry(IN CONST BLS_BOOT_LIST *List, IN EFI_HANDLE VolumeHandle, IN CONST CHAR16 *Path) {
  UINTN Index;
  if (List == NULL || Path == NULL) return TRUE;
  for (Index = 0; Index < List->Count; Index++) {
    if (List->Entries[Index].VolumeHandle == VolumeHandle && BlsStrEq(List->Entries[Index].Path, Path)) return TRUE;
  }
  return FALSE;
}

static BOOLEAN BlsPreferredCandidate(
  IN CONST CHAR16 *Path,
  IN CONST CHAR16 *VolumeLabel,
  IN CONST CHAR16 *VolumeDevicePath,
  IN CONST CHAR16 *PreferredPath,
  IN CONST CHAR16 *PreferredVolumeLabel,
  IN CONST CHAR16 *PreferredVolumeDevicePath
  ) {
  if (PreferredPath == NULL || PreferredPath[0] == L'\0' ||
      Path == NULL || !BlsStrEq(Path, PreferredPath)) return FALSE;

  if (PreferredVolumeDevicePath != NULL && PreferredVolumeDevicePath[0] != L'\0') {
    return (BOOLEAN)(VolumeDevicePath != NULL && BlsStrEq(VolumeDevicePath, PreferredVolumeDevicePath));
  }
  if (PreferredVolumeLabel != NULL && PreferredVolumeLabel[0] != L'\0') {
    return (BOOLEAN)(VolumeLabel != NULL && BlsStrEq(VolumeLabel, PreferredVolumeLabel));
  }
  return TRUE;
}

static VOID BlsFillEntry(
  OUT BLS_BOOT_ENTRY *Entry,
  IN EFI_HANDLE VolumeHandle,
  IN CONST CHAR16 *Path,
  IN CONST CHAR16 *VolumeLabel,
  IN CONST CHAR16 *VolumeDevicePath,
  IN BOOLEAN IsUsb
  ) {
  if (Entry == NULL) return;
  ZeroMem(Entry, sizeof(*Entry));
  Entry->Type = BlsTypeFromPath(Path);
  if (Entry->Type == BlsLoaderOpenCore) {
    BlsSafeStrCpy(Entry->Name, BLS_MAX_TEXT, L"OpenCore");
  } else if (Entry->Type == BlsLoaderClover) {
    BlsSafeStrCpy(Entry->Name, BLS_MAX_TEXT, L"Clover");
  } else {
    BlsSafeStrCpy(Entry->Name, BLS_MAX_TEXT, L"Bootloader");
  }
  BlsSafeStrCpy(Entry->Path, BLS_MAX_PATH, Path);
  BlsSafeStrCpy(Entry->VolumeLabel, BLS_MAX_TEXT, VolumeLabel != NULL ? VolumeLabel : L"EFI Volume");
  BlsSafeStrCpy(Entry->VolumeDevicePath, BLS_MAX_VOLUME_ID, VolumeDevicePath);
  Entry->IsUsb = IsUsb;
  Entry->VolumeHandle = VolumeHandle;
}

static VOID BlsAddEntry(
  IN OUT BLS_BOOT_LIST *List,
  IN EFI_HANDLE VolumeHandle,
  IN CONST CHAR16 *Path,
  IN CONST CHAR16 *VolumeLabel,
  IN CONST CHAR16 *VolumeDevicePath,
  IN BOOLEAN IsUsb,
  IN BOOLEAN Preferred
  ) {
  BLS_BOOT_ENTRY Candidate;
  if (List == NULL || VolumeHandle == NULL || Path == NULL) return;
  if (BlsDuplicateEntry(List, VolumeHandle, Path)) return;

  BlsFillEntry(&Candidate, VolumeHandle, Path, VolumeLabel, VolumeDevicePath, IsUsb);
  if (List->TotalFound < MAX_UINTN) List->TotalFound++;
  if (Preferred && List->PreferredMatchesFound < MAX_UINTN) List->PreferredMatchesFound++;

  if (List->Count < BLS_MAX_ENTRIES) {
    CopyMem(&List->Entries[List->Count], &Candidate, sizeof(Candidate));
    List->Count++;
    return;
  }

  List->Truncated = TRUE;
  /* Preserve the first preferred entry even when firmware enumeration places
     it after the display cap. PreferredMatchesFound still records ambiguity. */
  if (Preferred && List->PreferredMatchesFound == 1U && BLS_MAX_ENTRIES > 0U) {
    CopyMem(&List->Entries[BLS_MAX_ENTRIES - 1U], &Candidate, sizeof(Candidate));
  }
}

EFI_STATUS BlsScanBootloadersPreferred(
  OUT BLS_BOOT_LIST *List,
  IN CONST CHAR16 *PreferredPath OPTIONAL,
  IN CONST CHAR16 *PreferredVolumeLabel OPTIONAL,
  IN CONST CHAR16 *PreferredVolumeDevicePath OPTIONAL
  ) {
  EFI_STATUS Status;
  EFI_HANDLE *Handles;
  UINTN Count;
  UINTN H;
  UINTN P;
  EFI_STATUS ScanStatus;

  if (List == NULL) return EFI_INVALID_PARAMETER;
  ZeroMem(List, sizeof(*List));

  ScanStatus = EFI_SUCCESS;
  Status = BlsLocateFsHandles(&Handles, &Count);
  if (Status == EFI_NOT_FOUND) return EFI_SUCCESS;
  if (Status != EFI_SUCCESS) return Status;
  if (Handles == NULL || Count == 0) {
    if (Handles != NULL) FreePool(Handles);
    return EFI_SUCCESS;
  }

  for (H = 0; H < Count; H++) {
    EFI_FILE_PROTOCOL *Root;
    CHAR16 VolumeLabel[BLS_MAX_TEXT];
    CHAR16 VolumeDevicePath[BLS_MAX_VOLUME_ID];
    BOOLEAN IsUsb;
    EFI_DEVICE_PATH_PROTOCOL *VolumePath;
    BOOLEAN Found[BLS_KNOWN_PATH_COUNT];
    BOOLEAN AnyFound;

    if (Handles[H] == NULL) continue;
    Root = NULL;
    Status = BlsOpenVolumeRoot(Handles[H], &Root);
    if (Status != EFI_SUCCESS || Root == NULL) {
      if (Status == EFI_SUCCESS) Status = EFI_DEVICE_ERROR;
      if (ScanStatus == EFI_SUCCESS) ScanStatus = Status;
      continue;
    }

    /* Probe the only paths this selector can launch before collecting volume
       metadata. Most filesystem handles do not contain OpenCore/Clover, so
       deferring GetInfo(), DevicePath conversion and SHA-256 identity work
       removes avoidable firmware calls and allocations from the startup scan. */
    ZeroMem(Found, sizeof(Found));
    AnyFound = FALSE;
    for (P = 0; P < BLS_KNOWN_PATH_COUNT; P++) {
      BOOLEAN Exists;

      Exists = FALSE;
      Status = BlsRegularFileExistsInRoot(Root, mKnownPaths[P], &Exists);
      if (Exists) {
        Found[P] = TRUE;
        AnyFound = TRUE;
      }
      if (Status != EFI_SUCCESS && ScanStatus == EFI_SUCCESS) ScanStatus = Status;
    }

    if (AnyFound) {
      ZeroMem(VolumeLabel, sizeof(VolumeLabel));
      ZeroMem(VolumeDevicePath, sizeof(VolumeDevicePath));
      BlsReadVolumeLabelFromRoot(Root, VolumeLabel, ARRAY_SIZE(VolumeLabel));
      VolumePath = NULL;
      (VOID)BlsGetProtocolInterface(Handles[H], &gEfiDevicePathProtocolGuid, (VOID **)&VolumePath);
      if (VolumePath != NULL && !IsDevicePathValid(VolumePath, BLS_MAX_DEVICE_PATH_BYTES)) VolumePath = NULL;
      BlsReadVolumeDevicePath(VolumePath, VolumeDevicePath, ARRAY_SIZE(VolumeDevicePath));
      IsUsb = BlsVolumeIsUsb(VolumePath, VolumeDevicePath);

      for (P = 0; P < BLS_KNOWN_PATH_COUNT; P++) {
        BOOLEAN Preferred;

        if (!Found[P]) continue;
        Preferred = BlsPreferredCandidate(
                      mKnownPaths[P], VolumeLabel, VolumeDevicePath,
                      PreferredPath, PreferredVolumeLabel, PreferredVolumeDevicePath);
        BlsAddEntry(List, Handles[H], mKnownPaths[P], VolumeLabel, VolumeDevicePath, IsUsb, Preferred);
      }
    }

    Status = BlsFileClose(&Root);
    if (Status != EFI_SUCCESS && ScanStatus == EFI_SUCCESS) ScanStatus = Status;
  }

  FreePool(Handles);
  return ScanStatus;
}

EFI_STATUS BlsScanBootloaders(OUT BLS_BOOT_LIST *List) {
  return BlsScanBootloadersPreferred(List, NULL, NULL, NULL);
}

static EFI_STATUS BlsBuildFileDevicePath(
  IN EFI_HANDLE VolumeHandle,
  IN CONST CHAR16 *Path,
  OUT EFI_DEVICE_PATH_PROTOCOL **DevicePath
  ) {
  EFI_STATUS Status;
  EFI_DEVICE_PATH_PROTOCOL *VolumePath;
  EFI_DEVICE_PATH_PROTOCOL *FilePath;
  EFI_DEVICE_PATH_PROTOCOL *Combined;

  if (DevicePath == NULL) return EFI_INVALID_PARAMETER;
  *DevicePath = NULL;
  if (VolumeHandle == NULL || Path == NULL) return EFI_INVALID_PARAMETER;

  VolumePath = NULL;
  Status = BlsGetProtocolInterface(VolumeHandle, &gEfiDevicePathProtocolGuid, (VOID **)&VolumePath);
  if (Status != EFI_SUCCESS) return Status;
  if (VolumePath == NULL || !IsDevicePathValid(VolumePath, BLS_MAX_DEVICE_PATH_BYTES)) {
    return EFI_COMPROMISED_DATA;
  }

  /* FileDevicePath(NULL, ...) creates only the MEDIA_FILEPATH_DP node and does
     not query a handle.  Prepend the volume path obtained through OpenProtocol
     so the application does not fall back to the legacy HandleProtocol() path
     inside DevicePathFromHandle(). */
  FilePath = FileDevicePath(NULL, Path);
  if (FilePath == NULL) return EFI_OUT_OF_RESOURCES;
  if (!IsDevicePathValid(FilePath, BLS_MAX_DEVICE_PATH_BYTES)) {
    FreePool(FilePath);
    return EFI_COMPROMISED_DATA;
  }
  Combined = AppendDevicePath(VolumePath, FilePath);
  FreePool(FilePath);
  if (Combined == NULL) return EFI_OUT_OF_RESOURCES;
  if (!IsDevicePathValid(Combined, BLS_MAX_DEVICE_PATH_BYTES)) {
    FreePool(Combined);
    return EFI_COMPROMISED_DATA;
  }
  *DevicePath = Combined;
  return EFI_SUCCESS;
}

EFI_STATUS BlsStartBootEntryEx(
  IN EFI_HANDLE ImageHandle,
  IN CONST BLS_BOOT_ENTRY *Entry,
  OUT EFI_STATUS *StartStatus,
  OUT CHAR16 *ExitDataText OPTIONAL,
  IN UINTN ExitDataTextChars,
  OUT BOOLEAN *LifecycleCompromised OPTIONAL
  ) {
  EFI_STATUS Status;
  EFI_STATUS WatchdogArmStatus;
  EFI_STATUS WatchdogDisableStatus;
  EFI_DEVICE_PATH_PROTOCOL *DevicePath;
  EFI_HANDLE LoadedImage;
  UINTN ExitDataSize;
  CHAR16 *ExitData;

  if (StartStatus != NULL) *StartStatus = EFI_NOT_STARTED;
  if (LifecycleCompromised != NULL) *LifecycleCompromised = FALSE;
  if (ExitDataText != NULL && ExitDataTextChars > 0) ExitDataText[0] = L'\0';
  if (ImageHandle == NULL || Entry == NULL || Entry->VolumeHandle == NULL ||
      Entry->Path[0] == L'\0' ||
      BlsStrLen16Bounded(Entry->Path, BLS_MAX_PATH) >= BLS_MAX_PATH) {
    return EFI_INVALID_PARAMETER;
  }
  if (gBS == NULL || gBS->LoadImage == NULL || gBS->StartImage == NULL ||
      gBS->UnloadImage == NULL || gBS->SetWatchdogTimer == NULL) {
    return EFI_UNSUPPORTED;
  }

  DevicePath = NULL;
  Status = BlsBuildFileDevicePath(Entry->VolumeHandle, Entry->Path, &DevicePath);
  if (Status != EFI_SUCCESS) return Status;

  LoadedImage = NULL;
  /* This selector is acting as a boot manager for the chosen loader. UEFI
     LoadImage() uses BootPolicy=TRUE for a DevicePath being loaded as a boot
     selection, enabling the platform's boot-policy resolution semantics. */
  Status = gBS->LoadImage(TRUE, ImageHandle, DevicePath, NULL, 0, &LoadedImage);
  FreePool(DevicePath);
  if (Status != EFI_SUCCESS) {
    /* UEFI 2.11 LoadImage() returns a created, valid image handle on the error
       path only for EFI_SECURITY_VIOLATION. Other failures did not transfer an
       image to the caller, so an OUT value written by broken firmware must not
       be passed to UnloadImage(). */
    if (Status == EFI_SECURITY_VIOLATION && LoadedImage != NULL) {
      EFI_STATUS UnloadStatus;

      UnloadStatus = gBS->UnloadImage(LoadedImage);
      if (UnloadStatus != EFI_SUCCESS) {
        if (LifecycleCompromised != NULL) *LifecycleCompromised = TRUE;
        return UnloadStatus;
      }
    }
    LoadedImage = NULL;
    return Status;
  }
  if (LoadedImage == NULL) return EFI_LOAD_ERROR;

  ExitDataSize = 0;
  ExitData = NULL;
  WatchdogArmStatus = BlsSetChildBootWatchdog(TRUE);
  if (WatchdogArmStatus != EFI_SUCCESS && WatchdogArmStatus != EFI_UNSUPPORTED) {
    EFI_STATUS UnloadStatus;

    /* UEFI 2.11 boot-manager contract requires the five-minute watchdog to be
       enabled before StartImage(). EFI_UNSUPPORTED explicitly means the platform
       has no watchdog timer, so that case remains bootable. A programming error
       or invalid watchdog request must fail closed instead of silently starting
       an unprotected child. */
    UnloadStatus = gBS->UnloadImage(LoadedImage);
    if (UnloadStatus != EFI_SUCCESS) {
      if (LifecycleCompromised != NULL) *LifecycleCompromised = TRUE;
      return UnloadStatus;
    }
    return WatchdogArmStatus;
  }

  Status = gBS->StartImage(LoadedImage, &ExitDataSize, &ExitData);
  /* Make the required disable call whenever control returns. If a watchdog was
     actually armed, failure to disable it leaves selector execution exposed to
     an unexpected reset and is therefore a lifecycle-integrity failure. */
  WatchdogDisableStatus = BlsSetChildBootWatchdog(FALSE);
  if (WatchdogArmStatus == EFI_SUCCESS && WatchdogDisableStatus != EFI_SUCCESS &&
      LifecycleCompromised != NULL) {
    *LifecycleCompromised = TRUE;
  }
  if (StartStatus != NULL) *StartStatus = Status;

  /* Applications and OS loaders are automatically unloaded when they return,
     so probing their old image handle yields EFI_INVALID_PARAMETER.  A UEFI
     driver, however, may legally return EFI_SUCCESS and remain resident.
     Paths handled by this selector are bootloader applications, not drivers;
     if such an unexpected resident image can be unloaded, clean it up now.
     If it cannot, fail closed rather than keep running the selector with an
     untracked driver/protocol/callback lifetime installed in firmware state. */
  if (Status == EFI_SUCCESS) {
    EFI_STATUS UnloadStatus;

    UnloadStatus = gBS->UnloadImage(LoadedImage);
    if (UnloadStatus != EFI_SUCCESS && UnloadStatus != EFI_INVALID_PARAMETER) {
      if (LifecycleCompromised != NULL) *LifecycleCompromised = TRUE;
      if (WatchdogArmStatus == EFI_SUCCESS && WatchdogDisableStatus != EFI_SUCCESS) {
        return WatchdogDisableStatus;
      }
      return UnloadStatus;
    }
  }

  /* UEFI 2.11 defines EFI_SECURITY_VIOLATION and EFI_INVALID_PARAMETER as
     direct StartImage() errors; every other error value is normally the child's
     own exit status. Probe ownership before touching ExitData for every error so
     non-conforming firmware cannot leak a still-loaded image or trick us into
     freeing hostile OUT data after returning an unexpected direct error. A direct
     failure leaves the successfully loaded handle valid, so UnloadImage succeeds.
     A child that actually exits is unloaded by firmware and the old handle is
     invalid. This changes no conforming child-exit semantics. */
  if (Status != EFI_SUCCESS) {
    EFI_STATUS UnloadStatus;

    UnloadStatus = gBS->UnloadImage(LoadedImage);
    if (UnloadStatus == EFI_SUCCESS) {
      return (WatchdogArmStatus == EFI_SUCCESS && WatchdogDisableStatus != EFI_SUCCESS)
               ? WatchdogDisableStatus : Status;
    }
    if (UnloadStatus != EFI_INVALID_PARAMETER) {
      if (LifecycleCompromised != NULL) *LifecycleCompromised = TRUE;
      return UnloadStatus;
    }
  }

  /* On a non-success child exit, UEFI transfers ExitData ownership to the
     caller.  Bound both the firmware-reported size and the destination before
     reading the UTF-16 text, then release the pool exactly once. */
  if (Status != EFI_SUCCESS && ExitData != NULL && ExitDataText != NULL && ExitDataTextChars > 0 &&
      ExitDataSize >= sizeof(CHAR16) && ExitDataSize <= BLS_EXIT_DATA_SCAN_MAX_BYTES) {
    UINTN SourceChars;
    UINTN CopyChars;
    UINTN OutputChars;

    SourceChars = ExitDataSize / sizeof(CHAR16);
    OutputChars = ExitDataTextChars - 1U;
    if (SourceChars > OutputChars) SourceChars = OutputChars;
    CopyChars = BlsStrLen16Bounded(ExitData, SourceChars);
    if (CopyChars > 0) CopyMem(ExitDataText, ExitData, CopyChars * sizeof(CHAR16));
    ExitDataText[CopyChars] = L'\0';
  }
  if (Status != EFI_SUCCESS && ExitData != NULL) FreePool(ExitData);
  if (WatchdogArmStatus == EFI_SUCCESS && WatchdogDisableStatus != EFI_SUCCESS) {
    return WatchdogDisableStatus;
  }
  return Status;
}

EFI_STATUS BlsStartBootEntry(IN EFI_HANDLE ImageHandle, IN CONST BLS_BOOT_ENTRY *Entry, OUT EFI_STATUS *StartStatus) {
  return BlsStartBootEntryEx(ImageHandle, Entry, StartStatus, NULL, 0, NULL);
}

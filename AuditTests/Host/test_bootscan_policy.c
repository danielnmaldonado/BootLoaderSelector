/* Credits: ©DNMTechLabs / dnmtechlabs@gmail.com */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

#include "../../BootLoaderSelectorPkg/BootLoaderSelector/BootScan.h"
#include "../../BootLoaderSelectorPkg/BootLoaderSelector/Utils.h"

EFI_HANDLE gImageHandle = NULL;
EFI_SYSTEM_TABLE *gST = NULL;
EFI_BOOT_SERVICES *gBS = NULL;

typedef struct {
  HARDDRIVE_DEVICE_PATH      HardDrive;
  EFI_DEVICE_PATH_PROTOCOL  End;
} MOCK_GPT_DEVICE_PATH;

static MOCK_GPT_DEVICE_PATH mMockGptDevicePath;
static UINT8 mMockUsbDevicePath[160];
static BOOLEAN mUseMockDevicePath = FALSE;
static BOOLEAN mUseMockUsbDevicePath = FALSE;
static BOOLEAN mFailDevicePathText = FALSE;

VOID *EFIAPI ZeroMem(OUT VOID *Buffer, IN UINTN Length) { return memset(Buffer, 0, (size_t)Length); }
VOID *EFIAPI CopyMem(OUT VOID *Destination, IN CONST VOID *Source, IN UINTN Length) { return memcpy(Destination, Source, (size_t)Length); }
VOID EFIAPI FreePool(IN JCONST VOID *Buffer) { free((void *)Buffer); }
VOID *EFIAPI AllocateZeroPool(IN UINTN AllocationSize) { return calloc(1, (size_t)AllocationSize); }

UINTN BlsStrLen16Bounded(IN CONST CHAR16 *Str, IN UINTN MaxChars) {
  UINTN N = 0;
  if (Str == NULL) return 0;
  while (N < MaxChars && Str[N] != L'\0') N++;
  return N;
}
VOID BlsSafeStrCpy(OUT CHAR16 *Dst, IN UINTN DstChars, IN CONST CHAR16 *Src) {
  UINTN I = 0;
  if (Dst == NULL || DstChars == 0) return;
  while (Src != NULL && I + 1U < DstChars && Src[I] != L'\0') { Dst[I] = Src[I]; I++; }
  Dst[I] = L'\0';
}
BOOLEAN BlsStrEq(IN CONST CHAR16 *A, IN CONST CHAR16 *B) {
  UINTN I = 0;
  if (A == NULL || B == NULL) return FALSE;
  while (A[I] != L'\0' && B[I] != L'\0') { if (A[I] != B[I]) return FALSE; I++; }
  return (BOOLEAN)(A[I] == B[I]);
}
CHAR16 *EFIAPI StrStr(IN CONST CHAR16 *String, IN CONST CHAR16 *SearchString) {
  UINTN I, J;
  if (String == NULL || SearchString == NULL || SearchString[0] == L'\0') return (CHAR16 *)String;
  for (I = 0; String[I] != L'\0'; I++) {
    for (J = 0; SearchString[J] != L'\0' && String[I + J] == SearchString[J]; J++) {}
    if (SearchString[J] == L'\0') return (CHAR16 *)&String[I];
  }
  return NULL;
}
UINTN EFIAPI UnicodeSPrint(OUT CHAR16 *StartOfBuffer, IN UINTN BufferSize, IN CONST CHAR16 *FormatString, ...) {
  CONST CHAR16 Marker[] = L"GPT-FALLBACK";
  UINTN I;
  UINTN Chars = BufferSize / sizeof(CHAR16);
  (void)FormatString;
  if (StartOfBuffer == NULL || Chars == 0U) return 0;
  for (I = 0; I + 1U < Chars && Marker[I] != L'\0'; I++) StartOfBuffer[I] = Marker[I];
  StartOfBuffer[I] = L'\0';
  return I;
}
EFI_DEVICE_PATH_PROTOCOL *EFIAPI DevicePathFromHandle(IN EFI_HANDLE Handle) {
  (void)Handle;
  return mUseMockDevicePath ? (EFI_DEVICE_PATH_PROTOCOL *)&mMockGptDevicePath : NULL;
}
BOOLEAN EFIAPI IsDevicePathValid(IN CONST EFI_DEVICE_PATH_PROTOCOL *DevicePath, IN UINTN MaxSize) {
  CONST UINT8 *Base;
  UINTN Capacity;
  UINTN Offset;

  if (DevicePath == (EFI_DEVICE_PATH_PROTOCOL *)&mMockGptDevicePath && mUseMockDevicePath) {
    Base = (CONST UINT8 *)&mMockGptDevicePath;
    Capacity = sizeof(mMockGptDevicePath);
  } else if (DevicePath == (EFI_DEVICE_PATH_PROTOCOL *)mMockUsbDevicePath && mUseMockUsbDevicePath) {
    Base = mMockUsbDevicePath;
    Capacity = sizeof(mMockUsbDevicePath);
  } else {
    return FALSE;
  }
  if (MaxSize < sizeof(EFI_DEVICE_PATH_PROTOCOL)) return FALSE;
  if (Capacity > MaxSize) Capacity = MaxSize;

  Offset = 0U;
  while (Offset + sizeof(EFI_DEVICE_PATH_PROTOCOL) <= Capacity) {
    CONST EFI_DEVICE_PATH_PROTOCOL *Node;
    UINTN Length;

    Node = (CONST EFI_DEVICE_PATH_PROTOCOL *)(Base + Offset);
    Length = DevicePathNodeLength(Node);
    if (Node->Type == END_DEVICE_PATH_TYPE) {
      return (BOOLEAN)(Node->SubType == END_ENTIRE_DEVICE_PATH_SUBTYPE &&
                       Length == sizeof(EFI_DEVICE_PATH_PROTOCOL));
    }
    if (Length < sizeof(EFI_DEVICE_PATH_PROTOCOL) || Length > Capacity - Offset) return FALSE;
    Offset += Length;
  }
  return FALSE;
}
UINTN EFIAPI GetDevicePathSize(IN CONST EFI_DEVICE_PATH_PROTOCOL *DevicePath) {
  return (DevicePath == (EFI_DEVICE_PATH_PROTOCOL *)&mMockGptDevicePath) ? sizeof(mMockGptDevicePath) : 0U;
}
EFI_DEVICE_PATH_PROTOCOL *EFIAPI FileDevicePath(IN EFI_HANDLE Device OPTIONAL, IN CONST CHAR16 *FileName) { (void)Device; (void)FileName; return NULL; }
UINT8 EFIAPI DevicePathType(IN CONST VOID *Node) { return ((CONST EFI_DEVICE_PATH_PROTOCOL *)Node)->Type; }
UINT8 EFIAPI DevicePathSubType(IN CONST VOID *Node) { return ((CONST EFI_DEVICE_PATH_PROTOCOL *)Node)->SubType; }
UINTN EFIAPI DevicePathNodeLength(IN CONST VOID *Node) {
  CONST EFI_DEVICE_PATH_PROTOCOL *Path = (CONST EFI_DEVICE_PATH_PROTOCOL *)Node;
  return (UINTN)Path->Length[0] | ((UINTN)Path->Length[1] << 8U);
}
EFI_DEVICE_PATH_PROTOCOL *EFIAPI NextDevicePathNode(IN CONST VOID *Node) {
  CONST EFI_DEVICE_PATH_PROTOCOL *Path = (CONST EFI_DEVICE_PATH_PROTOCOL *)Node;
  UINTN Length = DevicePathNodeLength(Path);
  return (EFI_DEVICE_PATH_PROTOCOL *)((UINT8 *)(UINTN)Node + Length);
}
BOOLEAN EFIAPI IsDevicePathEnd(IN CONST VOID *Node) {
  return (BOOLEAN)(((CONST EFI_DEVICE_PATH_PROTOCOL *)Node)->Type == END_DEVICE_PATH_TYPE);
}
CHAR16 *EFIAPI ConvertDevicePathToText(
  IN CONST EFI_DEVICE_PATH_PROTOCOL *DevicePath,
  IN BOOLEAN DisplayOnly,
  IN BOOLEAN AllowShortcuts
  ) {
  (void)DevicePath;
  (void)DisplayOnly;
  (void)AllowShortcuts;
  if (mFailDevicePathText) return NULL;
  return NULL;
}

#include "../../BootLoaderSelectorPkg/BootLoaderSelector/BootScan.c"

static EFI_DEVICE_PATH_PROTOCOL *BuildMockUsbPath(IN UINT8 SubType, IN UINTN NodeLength) {
  EFI_DEVICE_PATH_PROTOCOL *Node;
  EFI_DEVICE_PATH_PROTOCOL *End;

  assert(NodeLength >= sizeof(EFI_DEVICE_PATH_PROTOCOL));
  assert(NodeLength + sizeof(EFI_DEVICE_PATH_PROTOCOL) <= sizeof(mMockUsbDevicePath));
  memset(mMockUsbDevicePath, 0, sizeof(mMockUsbDevicePath));
  Node = (EFI_DEVICE_PATH_PROTOCOL *)mMockUsbDevicePath;
  Node->Type = MESSAGING_DEVICE_PATH;
  Node->SubType = SubType;
  Node->Length[0] = (UINT8)(NodeLength & 0xFFU);
  Node->Length[1] = (UINT8)(NodeLength >> 8U);
  End = (EFI_DEVICE_PATH_PROTOCOL *)(mMockUsbDevicePath + NodeLength);
  End->Type = END_DEVICE_PATH_TYPE;
  End->SubType = END_ENTIRE_DEVICE_PATH_SUBTYPE;
  End->Length[0] = (UINT8)sizeof(EFI_DEVICE_PATH_PROTOCOL);
  End->Length[1] = 0U;
  mUseMockUsbDevicePath = TRUE;
  return Node;
}

int main(void) {
  BLS_BOOT_LIST List;
  UINTN I;
  CONST CHAR16 *Path = L"\\EFI\\OC\\OpenCore.efi";

  memset(&List, 0, sizeof(List));
  for (I = 0; I < BLS_MAX_ENTRIES + 1U; I++) {
    BlsAddEntry(&List, (EFI_HANDLE)(UINTN)(I + 1U), Path, L"EFI", L"DP", FALSE, FALSE);
  }
  assert(List.Count == BLS_MAX_ENTRIES);
  assert(List.TotalFound == BLS_MAX_ENTRIES + 1U);
  assert(List.Truncated == TRUE);

  BlsAddEntry(&List, (EFI_HANDLE)(UINTN)0xBEEFU, Path, L"Preferred", L"PREF-DP", FALSE, TRUE);
  assert(List.Count == BLS_MAX_ENTRIES);
  assert(List.TotalFound == BLS_MAX_ENTRIES + 2U);
  assert(List.PreferredMatchesFound == 1U);
  assert(List.Entries[BLS_MAX_ENTRIES - 1U].VolumeHandle == (EFI_HANDLE)(UINTN)0xBEEFU);

  /* A removable Block I/O property is no longer consulted. With no USB node
     and no USB text identity, the volume is correctly classified non-USB. */
  assert(BlsVolumeIsUsb(NULL, L"PciRoot(0)/HD(1,GPT,...)") == FALSE);
  assert(BlsVolumeIsUsb(NULL, L"PciRoot(0)/Usb(0x1,0x2)/HD(1,GPT,...)") == TRUE);

  {
    static CONST UINT8 ExpectedSha256Abc[32] = {
      0xbaU,0x78U,0x16U,0xbfU,0x8fU,0x01U,0xcfU,0xeaU,
      0x41U,0x41U,0x40U,0xdeU,0x5dU,0xaeU,0x22U,0x23U,
      0xb0U,0x03U,0x61U,0xa3U,0x96U,0x17U,0x7aU,0x9cU,
      0xb4U,0x10U,0xffU,0x61U,0xf2U,0x00U,0x15U,0xadU
    };
    static CONST struct {
      UINTN Length;
      UINT8 Digest[32];
    } BoundaryVectors[] = {
      { 55U, { 0x46U,0x3eU,0xb2U,0x8eU,0x72U,0xf8U,0x2eU,0x0aU,0x96U,0xc0U,0xa4U,0xccU,0x53U,0x69U,0x0cU,0x57U,0x12U,0x81U,0x13U,0x1fU,0x67U,0x2aU,0xa2U,0x29U,0xe0U,0xd4U,0x5aU,0xe5U,0x9bU,0x59U,0x8bU,0x59U } },
      { 56U, { 0xdaU,0x2aU,0xe4U,0xd6U,0xb3U,0x67U,0x48U,0xf2U,0xa3U,0x18U,0xf2U,0x3eU,0x7aU,0xb1U,0xdfU,0xdfU,0x45U,0xacU,0xdcU,0x9dU,0x04U,0x9bU,0xd8U,0x0eU,0x59U,0xdeU,0x82U,0xa6U,0x08U,0x95U,0xf5U,0x62U } },
      { 64U, { 0xfdU,0xeaU,0xb9U,0xacU,0xf3U,0x71U,0x03U,0x62U,0xbdU,0x26U,0x58U,0xcdU,0xc9U,0xa2U,0x9eU,0x8fU,0x9cU,0x75U,0x7fU,0xcfU,0x98U,0x11U,0x60U,0x3aU,0x8cU,0x44U,0x7cU,0xd1U,0xd9U,0x15U,0x11U,0x08U } },
      { 65U, { 0x4bU,0xfdU,0x2cU,0x8bU,0x6fU,0x1eU,0xecU,0x7aU,0x2aU,0xfeU,0xb4U,0x8bU,0x93U,0x4eU,0xe4U,0xb2U,0x69U,0x41U,0x82U,0x02U,0x7eU,0x6dU,0x0fU,0xc0U,0x75U,0x07U,0x4fU,0x2fU,0xabU,0xb3U,0x17U,0x81U } }
    };
    UINT8 Data[65];
    UINT8 Digest[32];
    UINTN VectorIndex;
    UINTN ByteIndex;

    assert(BlsSha256((CONST UINT8 *)"abc", 3U, Digest) == TRUE);
    assert(memcmp(Digest, ExpectedSha256Abc, sizeof(Digest)) == 0);
    for (ByteIndex = 0U; ByteIndex < ARRAY_SIZE(Data); ByteIndex++) Data[ByteIndex] = (UINT8)ByteIndex;
    for (VectorIndex = 0U; VectorIndex < ARRAY_SIZE(BoundaryVectors); VectorIndex++) {
      assert(BlsSha256(Data, BoundaryVectors[VectorIndex].Length, Digest) == TRUE);
      assert(memcmp(Digest, BoundaryVectors[VectorIndex].Digest, sizeof(Digest)) == 0);
    }
  }

  /* Regression: text conversion is optional and can fail with OOM. A validated
     GPT device path must still yield the strong compact identity. */
  {
    CHAR16 Identity[BLS_MAX_VOLUME_ID];
    EFI_GUID Guid = {
      0x12345678U, 0x9abcU, 0xdef0U,
      { 0x11U, 0x22U, 0x33U, 0x44U, 0x55U, 0x66U, 0x77U, 0x88U }
    };

    memset(&mMockGptDevicePath, 0, sizeof(mMockGptDevicePath));
    mMockGptDevicePath.HardDrive.Header.Type = MEDIA_DEVICE_PATH;
    mMockGptDevicePath.HardDrive.Header.SubType = MEDIA_HARDDRIVE_DP;
    mMockGptDevicePath.HardDrive.Header.Length[0] = (UINT8)(sizeof(HARDDRIVE_DEVICE_PATH) & 0xFFU);
    mMockGptDevicePath.HardDrive.Header.Length[1] = (UINT8)(sizeof(HARDDRIVE_DEVICE_PATH) >> 8U);
    memcpy(mMockGptDevicePath.HardDrive.Signature, &Guid, sizeof(Guid));
    mMockGptDevicePath.HardDrive.MBRType = MBR_TYPE_EFI_PARTITION_TABLE_HEADER;
    mMockGptDevicePath.HardDrive.SignatureType = SIGNATURE_TYPE_GUID;
    mMockGptDevicePath.End.Type = END_DEVICE_PATH_TYPE;
    mMockGptDevicePath.End.SubType = END_ENTIRE_DEVICE_PATH_SUBTYPE;
    mMockGptDevicePath.End.Length[0] = (UINT8)sizeof(EFI_DEVICE_PATH_PROTOCOL);
    mMockGptDevicePath.End.Length[1] = 0U;

    mUseMockDevicePath = TRUE;
    mFailDevicePathText = TRUE;
    memset(Identity, 0, sizeof(Identity));
    BlsReadVolumeDevicePath((EFI_DEVICE_PATH_PROTOCOL *)&mMockGptDevicePath, Identity, ARRAY_SIZE(Identity));
    assert(BlsStrEq(Identity, L"GPT-FALLBACK") == TRUE);

    /* UEFI 2.11 §10.3.5.1: GUID signature alone does not make an HD() node
       GPT. Partition Format must be GPT and the node length must be exactly
       42 bytes. Malformed/MBR nodes must fall back to the full-path digest. */
    {
      EFI_GUID Extracted;
      mMockGptDevicePath.HardDrive.MBRType = MBR_TYPE_PCAT;
      assert(BlsDevicePathGptGuid((EFI_DEVICE_PATH_PROTOCOL *)&mMockGptDevicePath, &Extracted) == FALSE);
      mMockGptDevicePath.HardDrive.MBRType = MBR_TYPE_EFI_PARTITION_TABLE_HEADER;
      mMockGptDevicePath.HardDrive.Header.Length[0] =
        (UINT8)((sizeof(HARDDRIVE_DEVICE_PATH) + 1U) & 0xFFU);
      mMockGptDevicePath.HardDrive.Header.Length[1] =
        (UINT8)((sizeof(HARDDRIVE_DEVICE_PATH) + 1U) >> 8U);
      assert(BlsDevicePathGptGuid((EFI_DEVICE_PATH_PROTOCOL *)&mMockGptDevicePath, &Extracted) == FALSE);
      mMockGptDevicePath.HardDrive.Header.Length[0] = (UINT8)(sizeof(HARDDRIVE_DEVICE_PATH) & 0xFFU);
      mMockGptDevicePath.HardDrive.Header.Length[1] = (UINT8)(sizeof(HARDDRIVE_DEVICE_PATH) >> 8U);
      assert(BlsDevicePathGptGuid((EFI_DEVICE_PATH_PROTOCOL *)&mMockGptDevicePath, &Extracted) == TRUE);
      assert(memcmp(&Extracted, &Guid, sizeof(Guid)) == 0);
    }
    mUseMockDevicePath = FALSE;
    mFailDevicePathText = FALSE;
  }

  /* UEFI 2.11 USB node wire-format regression: DevicePathLib validates generic
     traversal but not subtype-specific lengths. Do not classify malformed USB
     nodes as USB volumes merely from Type/SubType bytes. */
  assert(BlsDevicePathHasUsbNode(BuildMockUsbPath(MSG_USB_DP, sizeof(USB_DEVICE_PATH) + 1U)) == FALSE);
  assert(BlsVolumeIsUsb((EFI_DEVICE_PATH_PROTOCOL *)mMockUsbDevicePath, L"Usb(1,0)") == FALSE);
  assert(BlsDevicePathHasUsbNode(BuildMockUsbPath(MSG_USB_DP, sizeof(USB_DEVICE_PATH))) == TRUE);
  assert(BlsVolumeIsUsb((EFI_DEVICE_PATH_PROTOCOL *)mMockUsbDevicePath, L"not-used") == TRUE);
  assert(BlsDevicePathHasUsbNode(BuildMockUsbPath(MSG_USB_CLASS_DP, sizeof(USB_CLASS_DEVICE_PATH) + 1U)) == FALSE);
  assert(BlsDevicePathHasUsbNode(BuildMockUsbPath(MSG_USB_CLASS_DP, sizeof(USB_CLASS_DEVICE_PATH))) == TRUE);
  /* UEFI USB-WWID requires a serial-number string. A base 10-byte node has
     no serial and must not be classified as conforming USB-WWID. */
  assert(BlsDevicePathHasUsbNode(BuildMockUsbPath(MSG_USB_WWID_DP, sizeof(USB_WWID_DEVICE_PATH))) == FALSE);
  assert(BlsVolumeIsUsb((EFI_DEVICE_PATH_PROTOCOL *)mMockUsbDevicePath, L"UsbWwid(1234,5678,0,\"\")") == FALSE);
  assert(BlsDevicePathHasUsbNode(BuildMockUsbPath(MSG_USB_WWID_DP, sizeof(USB_WWID_DEVICE_PATH) + 1U)) == FALSE);
  assert(BlsDevicePathHasUsbNode(BuildMockUsbPath(MSG_USB_WWID_DP, sizeof(USB_WWID_DEVICE_PATH) + sizeof(CHAR16))) == TRUE);
  assert(BlsDevicePathHasUsbNode(BuildMockUsbPath(MSG_USB_WWID_DP, sizeof(USB_WWID_DEVICE_PATH) + 65U * sizeof(CHAR16))) == FALSE);
  mUseMockUsbDevicePath = FALSE;

  puts("bootscan_policy: PASS");
  return 0;
}

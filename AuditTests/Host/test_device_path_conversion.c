/* Credits: ©DNMTechLabs / dnmtechlabs@gmail.com
 * Exercises the linked converter and PrintLib using valid device paths only.
 * Pool doubles model UEFI ownership, including ReallocatePool failure semantics.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef NULL
#undef NULL
#endif
#define MDEPKG_NDEBUG
#define _PCD_GET_MODE_32_PcdMaximumDevicePathNodeCount 1024U
#define _PCD_GET_MODE_32_PcdMaximumUnicodeStringLength 0U
#define _PCD_GET_MODE_32_PcdMaximumAsciiStringLength 0U
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Guid/PcAnsi.h>
#include <Protocol/DebugPort.h>

static UINTN mAllocationCalls;
static UINTN mFailAllocation;
static UINTN mLiveAllocations;
static UINTN mValidConversions;
static UINTN mInjectedFailures;

VOID *EFIAPI AllocatePool(IN UINTN Size) {
  VOID *Buffer;
  mAllocationCalls++;
  if (mAllocationCalls == mFailAllocation) return NULL;
  Buffer = malloc((size_t)Size);
  assert(Buffer != NULL);
  mLiveAllocations++;
  return Buffer;
}
VOID EFIAPI FreePool(IN JCONST VOID *Buffer) {
  assert(Buffer != NULL);
  assert(mLiveAllocations != 0U);
  mLiveAllocations--;
  free((VOID *)Buffer);
}
VOID *EFIAPI AllocateZeroPool(IN UINTN Size) {
  VOID *Buffer = AllocatePool(Size);
  if (Buffer != NULL) memset(Buffer, 0, (size_t)Size);
  return Buffer;
}
VOID *EFIAPI AllocateCopyPool(IN UINTN Size, IN CONST VOID *Source) {
  VOID *Buffer = AllocatePool(Size);
  if (Buffer != NULL) memcpy(Buffer, Source, (size_t)Size);
  return Buffer;
}
VOID *EFIAPI ReallocatePool(IN UINTN OldSize, IN UINTN NewSize, IN VOID *OldBuffer OPTIONAL) {
  VOID *Buffer = AllocateZeroPool(NewSize);
  /* UEFI ReallocatePool retains OldBuffer if allocation fails. */
  if (Buffer == NULL) return NULL;
  if (OldBuffer != NULL) {
    memcpy(Buffer, OldBuffer, (size_t)(OldSize < NewSize ? OldSize : NewSize));
    FreePool(OldBuffer);
  }
  return Buffer;
}
VOID *EFIAPI CopyMem(OUT VOID *Destination, IN CONST VOID *Source, IN UINTN Size) { return memcpy(Destination, Source, (size_t)Size); }
VOID *EFIAPI ZeroMem(OUT VOID *Buffer, IN UINTN Size) { return memset(Buffer, 0, (size_t)Size); }
BOOLEAN EFIAPI CompareGuid(IN CONST GUID *A, IN CONST GUID *B) { return (BOOLEAN)(memcmp(A, B, sizeof(*A)) == 0); }
UINT16 EFIAPI ReadUnaligned16(IN CONST UINT16 *Buffer) {
  CONST UINT8 *Bytes = (CONST UINT8 *)Buffer;
  UINT16 Value = 0;
  UINTN Index;
  for (Index = 0; Index < sizeof(Value); Index++) Value |= (UINT16)((UINT16)Bytes[Index] << (Index * 8U));
  return Value;
}
UINT32 EFIAPI ReadUnaligned32(IN CONST UINT32 *Buffer) {
  CONST UINT8 *Bytes = (CONST UINT8 *)Buffer;
  UINT32 Value = 0;
  UINTN Index;
  for (Index = 0; Index < sizeof(Value); Index++) Value |= (UINT32)((UINT32)Bytes[Index] << (Index * 8U));
  return Value;
}
UINT64 EFIAPI ReadUnaligned64(IN CONST UINT64 *Buffer) {
  CONST UINT8 *Bytes = (CONST UINT8 *)Buffer;
  UINT64 Value = 0;
  UINTN Index;
  for (Index = 0; Index < sizeof(Value); Index++) Value |= (UINT64)((UINT64)Bytes[Index] << (Index * 8U));
  return Value;
}
UINT64 EFIAPI LShiftU64(IN UINT64 Value, IN UINTN Count) { return Value << Count; }
UINT64 EFIAPI DivU64x32Remainder(IN UINT64 Dividend, IN UINT32 Divisor, OUT UINT32 *Remainder OPTIONAL) {
  if (Remainder != NULL) *Remainder = (UINT32)(Dividend % Divisor);
  return Dividend / Divisor;
}
UINTN EFIAPI AsciiStrLen(IN CONST CHAR8 *String) { return (UINTN)strlen(String); }
UINTN EFIAPI StrLen(IN CONST CHAR16 *String) { UINTN Count = 0; while (String[Count] != 0) Count++; return Count; }
UINTN EFIAPI AsciiStrnLenS(IN CONST CHAR8 *String, IN UINTN MaxSize) { UINTN Count = 0; while (Count < MaxSize && String[Count] != 0) Count++; return Count; }
UINTN EFIAPI StrnLenS(IN CONST CHAR16 *String, IN UINTN MaxSize) { UINTN Count = 0; while (Count < MaxSize && String[Count] != 0) Count++; return Count; }

EFI_GUID gEfiPcAnsiGuid = EFI_PC_ANSI_GUID;
EFI_GUID gEfiVT100Guid = EFI_VT_100_GUID;
EFI_GUID gEfiVT100PlusGuid = EFI_VT_100_PLUS_GUID;
EFI_GUID gEfiVTUTF8Guid = EFI_VT_UTF8_GUID;
EFI_GUID gEfiUartDevicePathGuid = DEVICE_PATH_MESSAGING_UART_FLOW_CONTROL;
EFI_GUID gEfiSasDevicePathGuid = DEVICE_PATH_MESSAGING_SAS;
EFI_GUID gEfiDebugPortProtocolGuid = EFI_DEBUGPORT_PROTOCOL_GUID;
EFI_GUID gEfiVirtualDiskGuid = EFI_VIRTUAL_DISK_GUID;
EFI_GUID gEfiVirtualCdGuid = EFI_VIRTUAL_CD_GUID;
EFI_GUID gEfiPersistentVirtualDiskGuid = EFI_PERSISTENT_VIRTUAL_DISK_GUID;
EFI_GUID gEfiPersistentVirtualCdGuid = EFI_PERSISTENT_VIRTUAL_CD_GUID;

/* Upstream sources have their original EDK II warning profile. The harness
 * itself is compiled with the strict host flags. No converter is stubbed. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#endif
#ifdef RSIZE_MAX
#undef RSIZE_MAX
#endif
#include "../../MdePkg/Library/BasePrintLib/PrintLib.c"
#include "../../MdePkg/Library/BasePrintLib/PrintLibInternal.c"
#include "../../MdePkg/Library/UefiDevicePathLib/DevicePathUtilities.c"
#include "../../MdePkg/Library/UefiDevicePathLib/DevicePathToText.c"
#include "../../MdePkg/Library/UefiDevicePathLib/UefiDevicePathLib.c"
#pragma GCC diagnostic pop

static VOID SetHeader(EFI_DEVICE_PATH_PROTOCOL *Node, UINT8 Type, UINT8 SubType, UINTN Size) {
  Node->Type = Type;
  Node->SubType = SubType;
  Node->Length[0] = (UINT8)(Size & 255U);
  Node->Length[1] = (UINT8)(Size >> 8U);
}

static VOID CheckText(CONST EFI_DEVICE_PATH_PROTOCOL *Node, CONST CHAR16 *Expected) {
  CHAR16 *Text;
  UINTN AllocationCount;
  UINTN FailAt;
  mAllocationCalls = 0;
  mFailAllocation = 0;
  Text = ConvertDeviceNodeToText(Node, FALSE, TRUE);
  assert(Text != NULL);
  assert(StrLen(Text) == StrLen(Expected));
  assert(memcmp(Text, Expected, (size_t)((StrLen(Expected) + 1U) * sizeof(CHAR16))) == 0);
  AllocationCount = mAllocationCalls;
  mValidConversions++;
  FreePool(Text);
  assert(mLiveAllocations == 0U);
  for (FailAt = 1; FailAt <= AllocationCount; FailAt++) {
    mAllocationCalls = 0;
    mFailAllocation = FailAt;
    Text = ConvertDeviceNodeToText(Node, FALSE, TRUE);
    assert(Text == NULL);
    assert(mLiveAllocations == 0U);
    mInjectedFailures++;
  }
  mFailAllocation = 0;
}

static VOID CheckPath(CONST EFI_DEVICE_PATH_PROTOCOL *Path, CONST CHAR16 *Expected) {
  CHAR16 *Text;
  UINTN AllocationCount;
  UINTN FailAt;
  mAllocationCalls = 0;
  mFailAllocation = 0;
  Text = ConvertDevicePathToText(Path, FALSE, TRUE);
  assert(Text != NULL);
  assert(StrLen(Text) == StrLen(Expected));
  assert(memcmp(Text, Expected, (size_t)((StrLen(Expected) + 1U) * sizeof(CHAR16))) == 0);
  AllocationCount = mAllocationCalls;
  mValidConversions++;
  FreePool(Text);
  assert(mLiveAllocations == 0U);
  for (FailAt = 1; FailAt <= AllocationCount; FailAt++) {
    mAllocationCalls = 0;
    mFailAllocation = FailAt;
    Text = ConvertDevicePathToText(Path, FALSE, TRUE);
    assert(Text == NULL);
    assert(mLiveAllocations == 0U);
    mInjectedFailures++;
  }
  mFailAllocation = 0;
}

#pragma pack(push, 1)
int main(void) {
  PCI_DEVICE_PATH Pci;
  struct { USB_WWID_DEVICE_PATH Node; CHAR16 Serial[5]; } Wwid;
  struct { USB_WWID_DEVICE_PATH Node; CHAR16 Serial[4]; } WwidLengthDelimited;
  struct { EFI_DEVICE_PATH_PROTOCOL Header; CHAR8 Text[17]; } Uri;
  struct { ACPI_EXTENDED_HID_DEVICE_PATH Node; CHAR8 Strings[10]; } AcpiEx;
  struct { EFI_DEVICE_PATH_PROTOCOL Header; CHAR16 Name[22]; } File;
  struct {
    EFI_DEVICE_PATH_PROTOCOL First;
    CHAR16 FirstName[3];
    EFI_DEVICE_PATH_PROTOCOL Second;
    CHAR16 SecondName[2];
    EFI_DEVICE_PATH_PROTOCOL End;
  } CommaFilePath;
  struct { PCI_DEVICE_PATH Pci; USB_DEVICE_PATH Usb; EFI_DEVICE_PATH_PROTOCOL End; } Path;
  struct { PCI_DEVICE_PATH First; EFI_DEVICE_PATH_PROTOCOL Instance; PCI_DEVICE_PATH Second; EFI_DEVICE_PATH_PROTOCOL End; } Instances;
  struct { EFI_DEVICE_PATH_PROTOCOL Header; UINT8 Bytes[32]; } Generic;
  EFI_DEVICE_PATH_PROTOCOL End;
  struct { ISCSI_DEVICE_PATH Node; CHAR8 Name[26]; } Iscsi;
  struct { EFI_DEVICE_PATH_PROTOCOL Header; UINT8 IsIPv6; EFI_IP_ADDRESS Address; } Dns;
  struct { EFI_DEVICE_PATH_PROTOCOL Header; UINT32 Hid; UINT32 Uid; UINT32 Cid; CHAR8 Strings[3]; } AcpiRoot;
  struct { ACPI_ADR_DEVICE_PATH First; UINT32 Second; } AcpiAdr;
  struct { EFI_DEVICE_PATH_PROTOCOL Header; UINT16 Type; UINT16 Status; CHAR8 Name[5]; } Bbs;
  UART_FLOW_CONTROL_DEVICE_PATH Flow;
  SAS_DEVICE_PATH Sas;
  HARDDRIVE_DEVICE_PATH Hd;
  IPv4_DEVICE_PATH Ipv4;
  IPv6_DEVICE_PATH Ipv6;
  struct { PCI_DEVICE_PATH Pci; USB_WWID_DEVICE_PATH Wwid; CHAR16 Serial[4]; EFI_DEVICE_PATH_PROTOCOL End; } WwidPath;
  struct { PCI_DEVICE_PATH Pci; EFI_DEVICE_PATH_PROTOCOL Uri; CHAR8 Text[17]; EFI_DEVICE_PATH_PROTOCOL End; } UriPath;

  memset(&Pci, 0, sizeof(Pci));
  SetHeader(&Pci.Header, HARDWARE_DEVICE_PATH, HW_PCI_DP, sizeof(Pci));
  Pci.Device = 0x1f;
  Pci.Function = 2;
  CheckText(&Pci.Header, L"Pci(0x1F,0x2)");

  memset(&Wwid, 0, sizeof(Wwid));
  SetHeader(&Wwid.Node.Header, MESSAGING_DEVICE_PATH, MSG_USB_WWID_DP, sizeof(Wwid));
  Wwid.Node.VendorId = 0x1234;
  Wwid.Node.ProductId = 0x5678;
  Wwid.Node.InterfaceNumber = 1;
  memcpy(Wwid.Serial, L"ABCD", sizeof(Wwid.Serial));
  CheckText(&Wwid.Node.Header, L"UsbWwid(0x1234,0x5678,0x1,\"ABCD\")");
  memset(&WwidLengthDelimited, 0, sizeof(WwidLengthDelimited));
  SetHeader(&WwidLengthDelimited.Node.Header, MESSAGING_DEVICE_PATH, MSG_USB_WWID_DP, sizeof(WwidLengthDelimited));
  WwidLengthDelimited.Node.VendorId = 0x1234;
  WwidLengthDelimited.Node.ProductId = 0x5678;
  WwidLengthDelimited.Node.InterfaceNumber = 1;
  memcpy(WwidLengthDelimited.Serial, L"ABCD", sizeof(WwidLengthDelimited.Serial));
  CheckText(&WwidLengthDelimited.Node.Header, L"UsbWwid(0x1234,0x5678,0x1,\"ABCD\")");

  memset(&Uri, 0, sizeof(Uri));
  SetHeader(&Uri.Header, MESSAGING_DEVICE_PATH, MSG_URI_DP, sizeof(Uri));
  memcpy(Uri.Text, "https://uefi.org/", sizeof(Uri.Text));
  CheckText(&Uri.Header, L"Uri(https://uefi.org/)");

  memset(&AcpiEx, 0, sizeof(AcpiEx));
  SetHeader(&AcpiEx.Node.Header, ACPI_DEVICE_PATH, ACPI_EXTENDED_DP, sizeof(AcpiEx));
  memcpy(AcpiEx.Strings, "HID\0UID\0C\0", sizeof(AcpiEx.Strings));
  CheckText(&AcpiEx.Node.Header, L"AcpiEx(@@@0000,@@@0000,0x0,HID,C,UID)");

  memset(&File, 0, sizeof(File));
  SetHeader(&File.Header, MEDIA_DEVICE_PATH, MEDIA_FILEPATH_DP, sizeof(File));
  memcpy(File.Name, L"\\EFI\\BOOT\\BOOTX64.EFI", sizeof(L"\\EFI\\BOOT\\BOOTX64.EFI"));
  CheckText(&File.Header, L"\\EFI\\BOOT\\BOOTX64.EFI");

  /* A literal filename comma is unrelated to an End Instance node. */
  memset(&CommaFilePath, 0, sizeof(CommaFilePath));
  SetHeader(&CommaFilePath.First, MEDIA_DEVICE_PATH, MEDIA_FILEPATH_DP,
            sizeof(CommaFilePath.First) + sizeof(CommaFilePath.FirstName));
  memcpy(CommaFilePath.FirstName, L"A,", sizeof(CommaFilePath.FirstName));
  SetHeader(&CommaFilePath.Second, MEDIA_DEVICE_PATH, MEDIA_FILEPATH_DP,
            sizeof(CommaFilePath.Second) + sizeof(CommaFilePath.SecondName));
  memcpy(CommaFilePath.SecondName, L"B", sizeof(CommaFilePath.SecondName));
  SetHeader(&CommaFilePath.End, END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, sizeof(CommaFilePath.End));
  CheckPath(&CommaFilePath.First, L"A,/B");

  memset(&Path, 0, sizeof(Path));
  Path.Pci = Pci;
  SetHeader(&Path.Usb.Header, MESSAGING_DEVICE_PATH, MSG_USB_DP, sizeof(Path.Usb));
  Path.Usb.ParentPortNumber = 1;
  Path.Usb.InterfaceNumber = 2;
  SetHeader(&Path.End, END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, sizeof(Path.End));
  CheckPath(&Path.Pci.Header, L"Pci(0x1F,0x2)/USB(0x1,0x2)");

  memset(&Instances, 0, sizeof(Instances));
  Instances.First = Pci;
  Instances.Second = Pci;
  SetHeader(&Instances.Instance, END_DEVICE_PATH_TYPE, END_INSTANCE_DEVICE_PATH_SUBTYPE, sizeof(Instances.Instance));
  SetHeader(&Instances.End, END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, sizeof(Instances.End));
  CheckPath(&Instances.First.Header, L"Pci(0x1F,0x2),Pci(0x1F,0x2)");

  memset(&Generic, 0, sizeof(Generic));
  SetHeader(&Generic.Header, HARDWARE_DEVICE_PATH, 0x7f, sizeof(Generic));
  CheckText(&Generic.Header, L"HardwarePath(127,0000000000000000000000000000000000000000000000000000000000000000)");
  memset(&Iscsi, 0, sizeof(Iscsi));
  SetHeader(&Iscsi.Node.Header, MESSAGING_DEVICE_PATH, MSG_ISCSI_DP, sizeof(Iscsi));
  Iscsi.Node.LoginOption = BIT11;
  Iscsi.Node.TargetPortalGroupTag = 1;
  memcpy(Iscsi.Name, "iqn.2026-11.org.uefi:disk", sizeof("iqn.2026-11.org.uefi:disk"));
  CheckText(&Iscsi.Node.Header, L"iSCSI(iqn.2026-11.org.uefi:disk,0x1,0x0000000000000000,None,None,None,TCP)");

  memset(&Dns, 0, sizeof(Dns));
  SetHeader(&Dns.Header, MESSAGING_DEVICE_PATH, MSG_DNS_DP, sizeof(Dns));
  Dns.Address.v4.Addr[0] = 192;
  Dns.Address.v4.Addr[1] = 0;
  Dns.Address.v4.Addr[2] = 2;
  Dns.Address.v4.Addr[3] = 53;
  CheckText(&Dns.Header, L"Dns(192.0.2.53)");
  Dns.IsIPv6 = 1;
  memset(&Dns.Address, 0, sizeof(Dns.Address));
  Dns.Address.v6.Addr[0] = 0x20;
  Dns.Address.v6.Addr[1] = 0x01;
  Dns.Address.v6.Addr[2] = 0x0d;
  Dns.Address.v6.Addr[3] = 0xb8;
  Dns.Address.v6.Addr[15] = 0x53;
  CheckText(&Dns.Header, L"Dns(2001:0DB8:0000:0000:0000:0000:0000:0053)");

  memset(&AcpiRoot, 0, sizeof(AcpiRoot));
  SetHeader(&AcpiRoot.Header, ACPI_DEVICE_PATH, ACPI_EXTENDED_DP, sizeof(AcpiRoot));
  AcpiRoot.Hid = EISA_PNP_ID(0x0a03);
  CheckText(&AcpiRoot.Header, L"AcpiEx(PNP0A03,@@@0000,0x0,,,)");
  memset(&AcpiAdr, 0, sizeof(AcpiAdr));
  SetHeader(&AcpiAdr.First.Header, ACPI_DEVICE_PATH, ACPI_ADR_DP, sizeof(AcpiAdr));
  AcpiAdr.First.ADR = 0x1234;
  AcpiAdr.Second = 0x5678;
  CheckText(&AcpiAdr.First.Header, L"AcpiAdr(0x1234,0x5678)");

  memset(&Bbs, 0, sizeof(Bbs));
  SetHeader(&Bbs.Header, BBS_DEVICE_PATH, BBS_BBS_DP, sizeof(Bbs));
  Bbs.Type = BBS_TYPE_HARDDRIVE;
  memcpy(Bbs.Name, "Disk", sizeof(Bbs.Name));
  CheckText(&Bbs.Header, L"BBS(HD,Disk,0x0)");

  memset(&Flow, 0, sizeof(Flow));
  SetHeader(&Flow.Header, MESSAGING_DEVICE_PATH, MSG_VENDOR_DP, sizeof(Flow));
  Flow.Guid = gEfiUartDevicePathGuid;
  Flow.FlowControlMap = 1;
  CheckText(&Flow.Header, L"UartFlowCtrl(Hardware)");
  memset(&Sas, 0, sizeof(Sas));
  SetHeader(&Sas.Header, MESSAGING_DEVICE_PATH, MSG_VENDOR_DP, sizeof(Sas));
  Sas.Guid = gEfiSasDevicePathGuid;
  Sas.SasAddress = 0x1234;
  CheckText(&Sas.Header, L"SAS(0x1234,0x0,0x0,NoTopology,0,0,0,0x0)");

  memset(&Hd, 0, sizeof(Hd));
  SetHeader(&Hd.Header, MEDIA_DEVICE_PATH, MEDIA_HARDDRIVE_DP, sizeof(Hd));
  Hd.PartitionNumber = 1;
  Hd.PartitionStart = 0x800;
  Hd.PartitionSize = 0x100000;
  Hd.MBRType = MBR_TYPE_EFI_PARTITION_TABLE_HEADER;
  Hd.SignatureType = SIGNATURE_TYPE_GUID;
  {
    CONST EFI_GUID PartitionGuid = {0x01234567,0x89ab,0xcdef,{0x01,0x23,0x45,0x67,0x89,0xab,0xcd,0xef}};
    memcpy(Hd.Signature, &PartitionGuid, sizeof(PartitionGuid));
  }
  CheckText(&Hd.Header, L"HD(1,GPT,01234567-89AB-CDEF-0123-456789ABCDEF,0x800,0x100000)");

  memset(&Ipv4, 0, sizeof(Ipv4));
  SetHeader(&Ipv4.Header, MESSAGING_DEVICE_PATH, MSG_IPv4_DP, sizeof(Ipv4));
  Ipv4.Protocol = 6;
  Ipv4.RemoteIpAddress.Addr[0] = 192;
  Ipv4.RemoteIpAddress.Addr[2] = 2;
  Ipv4.RemoteIpAddress.Addr[3] = 1;
  CheckText(&Ipv4.Header, L"IPv4(192.0.2.1,TCP,DHCP,0.0.0.0,0.0.0.0,0.0.0.0)");
  SetHeader(&Ipv4.Header, MESSAGING_DEVICE_PATH, MSG_IPv4_DP, OFFSET_OF(IPv4_DEVICE_PATH, GatewayIpAddress));
  CheckText(&Ipv4.Header, L"IPv4(192.0.2.1,TCP,DHCP,0.0.0.0)");
  memset(&Ipv6, 0, sizeof(Ipv6));
  SetHeader(&Ipv6.Header, MESSAGING_DEVICE_PATH, MSG_IPv6_DP, sizeof(Ipv6));
  Ipv6.Protocol = 6;
  Ipv6.RemoteIpAddress.Addr[15] = 1;
  CheckText(&Ipv6.Header, L"IPv6(0000:0000:0000:0000:0000:0000:0000:0001,TCP,Static,0000:0000:0000:0000:0000:0000:0000:0000,0x0,0000:0000:0000:0000:0000:0000:0000:0000)");
  SetHeader(&Ipv6.Header, MESSAGING_DEVICE_PATH, MSG_IPv6_DP, OFFSET_OF(IPv6_DEVICE_PATH, PrefixLength));
  CheckText(&Ipv6.Header, L"IPv6(0000:0000:0000:0000:0000:0000:0000:0001,TCP,Static,0000:0000:0000:0000:0000:0000:0000:0000)");

  memset(&WwidPath, 0, sizeof(WwidPath));
  WwidPath.Pci = Pci;
  WwidPath.Wwid = WwidLengthDelimited.Node;
  memcpy(WwidPath.Serial, WwidLengthDelimited.Serial, sizeof(WwidPath.Serial));
  SetHeader(&WwidPath.End, END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, sizeof(WwidPath.End));
  CheckPath(&WwidPath.Pci.Header, L"Pci(0x1F,0x2)/UsbWwid(0x1234,0x5678,0x1,\"ABCD\")");
  memset(&UriPath, 0, sizeof(UriPath));
  UriPath.Pci = Pci;
  UriPath.Uri = Uri.Header;
  memcpy(UriPath.Text, Uri.Text, sizeof(UriPath.Text));
  SetHeader(&UriPath.End, END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, sizeof(UriPath.End));
  CheckPath(&UriPath.Pci.Header, L"Pci(0x1F,0x2)/Uri(https://uefi.org/)");

  SetHeader(&End, END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, sizeof(End));
  CheckPath(&End, L"");
  assert(mLiveAllocations == 0U);
  printf("device path conversion: %zu valid conversions, %zu allocation failure positions passed\n",
         (size_t)mValidConversions, (size_t)mInjectedFailures);
  return 0;
}
#pragma pack(pop)

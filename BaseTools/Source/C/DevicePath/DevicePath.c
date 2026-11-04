/** @file
  Definition for Device Path Tool.

Copyright (c) 2017 - 2018, Intel Corporation. All rights reserved.<BR>
SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include "UefiDevicePathLib.h"

//
// Utility Name
//
#define UTILITY_NAME  "DevicePath"

//
// Utility version information
//
#define UTILITY_MAJOR_VERSION 0
#define UTILITY_MINOR_VERSION 1

EFI_GUID gEfiDebugPortProtocolGuid = DEVICE_PATH_MESSAGING_DEBUGPORT;
EFI_GUID gEfiPcAnsiGuid = EFI_PC_ANSI_GUID;
EFI_GUID gEfiVT100Guid = EFI_VT_100_GUID;
EFI_GUID gEfiVT100PlusGuid = EFI_VT_100_PLUS_GUID;
EFI_GUID gEfiVTUTF8Guid = EFI_VT_UTF8_GUID;
EFI_GUID gEfiUartDevicePathGuid = EFI_UART_DEVICE_PATH_GUID;
EFI_GUID gEfiSasDevicePathGuid = EFI_SAS_DEVICE_PATH_GUID;
EFI_GUID gEfiVirtualDiskGuid = EFI_VIRTUAL_DISK_GUID;
EFI_GUID gEfiVirtualCdGuid = EFI_VIRTUAL_CD_GUID;
EFI_GUID gEfiPersistentVirtualDiskGuid = EFI_PERSISTENT_VIRTUAL_DISK_GUID;
EFI_GUID gEfiPersistentVirtualCdGuid = EFI_PERSISTENT_VIRTUAL_CD_GUID;

STATIC
VOID
Version (
  VOID
)
/*++

Routine Description:

  Displays the standard utility information to SDTOUT

Arguments:

  None

Returns:

  None

--*/
{
  fprintf (stdout, "%s Version %d.%d %s \n", UTILITY_NAME, UTILITY_MAJOR_VERSION, UTILITY_MINOR_VERSION, __BUILD_VERSION);
}

STATIC
VOID
Usage (
  VOID
  )
/*++

Routine Description:

  Displays the utility usage syntax to STDOUT

Arguments:

  None

Returns:

  None

--*/
{
  //
  // Summary usage
  //
  fprintf (stdout, "\nUsage: %s [options]\n\n", UTILITY_NAME);

  //
  // Copyright declaration
  //
  fprintf (stdout, "Copyright (c) 2017, Intel Corporation. All rights reserved.\n\n");
  //
  // Details Option
  //
  fprintf (stdout, "Options:\n");
  fprintf (stdout, "  DevicePathString      Device Path string is specified, no space character.\n"
                   "                        Example: \"PciRoot(0)/Pci(0,0)\"\n");

  fprintf (stdout, "  --version             Show program's version number and exit.\n");
  fprintf (stdout, "  -h, --help            Show this help message and exit.\n");
}


STATIC
VOID
PrintMem (
  CONST VOID *Buffer,
  UINTN      Count
  )
{
  CONST UINT8 *Bytes;
  UINTN       Idx;

  Bytes = Buffer;
  for (Idx = 0; Idx < Count; Idx++) {
    printf("0x%02x ", Bytes[Idx]);
  }
}

VOID
Ascii2UnicodeString (
  CONST CHAR8  *String,
  CHAR16       *UniString
  )
/*++

Routine Description:

  Write ascii string as unicode string format to FILE

Arguments:

  String      - Pointer to string that is written to FILE.
  UniString   - Pointer to unicode string

Returns:

  NULL

--*/
{
  if ((String == NULL) || (UniString == NULL)) {
    return;
  }

  while (*String != '\0') {
    *(UniString++) = (CHAR16)*(String++);
  }

  //
  // End the UniString with a NULL.
  //
  *UniString = '\0';
}

int
main (
  int   argc,
  CHAR8 *argv[]
  )
{
  CONST CHAR8               *Str;
  CHAR16                    *Str16;
  EFI_DEVICE_PATH_PROTOCOL  *DevicePath;
  EFI_DEVICE_PATH_PROTOCOL  *DevicePathStart;
  UINTN                     InputLength;
  UINTN                     AllocationSize;
  UINTN                     DevicePathSize;
  UINTN                     Offset;
  UINTN                     NodeLength;
  int                       Result;

  Str16          = NULL;
  DevicePathStart = NULL;
  Result          = STATUS_ERROR;

  if ((argc < 2) || (argv == NULL) || (argv[1] == NULL)) {
    Error (NULL, 0, 1001, "Missing options", "No input options specified.");
    Usage ();
    return STATUS_ERROR;
  }
  if ((stricmp (argv[1], "-h") == 0) || (stricmp (argv[1], "--help") == 0)) {
    Version ();
    Usage ();
    return STATUS_SUCCESS;
  }

  if (stricmp (argv[1], "--version") == 0) {
    Version ();
    return STATUS_SUCCESS;
  }

  Str         = argv[1];
  InputLength = strlen (Str);
  if (InputLength > (((UINTN)-1 / sizeof (CHAR16)) - 1U)) {
    fprintf (stderr, "Invalid option value, Device Path string is too long\n");
    goto Done;
  }

  AllocationSize = (InputLength + 1U) * sizeof (CHAR16);
  Str16 = (CHAR16 *)malloc (AllocationSize);
  if (Str16 == NULL) {
    fprintf (stderr, "Resource, memory cannot be allocated\n");
    goto Done;
  }

  Ascii2UnicodeString (Str, Str16);
  DevicePathStart = UefiDevicePathLibConvertTextToDevicePath (Str16);
  if (DevicePathStart == NULL) {
    fprintf (stderr, "Convert fail, Cannot convert text to a device path\n");
    goto Done;
  }

  DevicePathSize = GetDevicePathSize (DevicePathStart);
  if (DevicePathSize < END_DEVICE_PATH_LENGTH) {
    fprintf (stderr, "Convert fail, Invalid device path returned by converter\n");
    goto Done;
  }

  Offset = 0;
  while (Offset < DevicePathSize) {
    if (DevicePathSize - Offset < sizeof (EFI_DEVICE_PATH_PROTOCOL)) {
      fprintf (stderr, "Convert fail, Truncated device path node\n");
      goto Done;
    }

    DevicePath = (EFI_DEVICE_PATH_PROTOCOL *)((UINT8 *)DevicePathStart + Offset);
    NodeLength = (UINTN)DevicePath->Length[0] | ((UINTN)DevicePath->Length[1] << 8);
    if ((NodeLength < sizeof (EFI_DEVICE_PATH_PROTOCOL)) ||
        (NodeLength > DevicePathSize - Offset)) {
      fprintf (stderr, "Convert fail, Invalid device path node length\n");
      goto Done;
    }

    PrintMem (DevicePath, NodeLength);
    Offset += NodeLength;

    if ((DevicePath->Type == END_DEVICE_PATH_TYPE) &&
        (DevicePath->SubType == END_ENTIRE_DEVICE_PATH_SUBTYPE)) {
      if (Offset != DevicePathSize) {
        fprintf (stderr, "Convert fail, Trailing data after device path end node\n");
        goto Done;
      }

      putchar ('\n');
      Result = STATUS_SUCCESS;
      break;
    }
  }

  if (Result != STATUS_SUCCESS) {
    fprintf (stderr, "Convert fail, Device path has no end node\n");
  }

Done:
  free (DevicePathStart);
  free (Str16);
  return Result;
}

/** @file
The tool dumps the contents of a firmware volume

Copyright (c) 1999 - 2018, Intel Corporation. All rights reserved.<BR>
SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <assert.h>
#include <limits.h>
#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#ifdef _WIN32
#include <direct.h>
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#endif

#include <FvLib.h>
#include <Common/UefiBaseTypes.h>
#include <Common/UefiCapsule.h>
#include <Common/PiFirmwareFile.h>
#include <Common/PiFirmwareVolume.h>
#include <Guid/PiFirmwareFileSystem.h>
#include <IndustryStandard/PeImage.h>
#include <Protocol/GuidedSectionExtraction.h>

#include "Compress.h"
#include "Decompress.h"
#include "VolInfo.h"
#include "CommonLib.h"
#include "EfiUtilityMsgs.h"
#include "FirmwareVolumeBufferLib.h"
#include "OsPath.h"
#include "ParseGuidedSectionTools.h"
#include "StringFuncs.h"
#include "ParseInf.h"
#include "PeCoffLib.h"

//
// Utility global variables
//

EFI_GUID  gEfiCrc32GuidedSectionExtractionProtocolGuid = EFI_CRC32_GUIDED_SECTION_EXTRACTION_PROTOCOL_GUID;

#define UTILITY_MAJOR_VERSION      1
#define UTILITY_MINOR_VERSION      0

#define UTILITY_NAME         "VolInfo"

#define EFI_SECTION_ERROR EFIERR (100)

#define MAX_BASENAME_LEN  60  // not good to hardcode, but let's be reasonable
#define MAX_SECTION_NESTING 64U

//
// Structure to keep a list of guid-to-basenames
//
typedef struct _GUID_TO_BASENAME {
  struct _GUID_TO_BASENAME  *Next;
  INT8                      Guid[PRINTED_GUID_BUFFER_SIZE];
  INT8                      BaseName[MAX_BASENAME_LEN];
} GUID_TO_BASENAME;

static GUID_TO_BASENAME *mGuidBaseNameList = NULL;

//
// Store GUIDed Section guid->tool mapping
//
EFI_HANDLE mParsedGuidedSectionTools = NULL;

CHAR8* mUtilityFilename = NULL;

BOOLEAN EnableHash = FALSE;
CHAR8 *OpenSslPath = NULL;

STATIC
EFI_STATUS
ParseOffsetArgument (
  IN CONST CHAR8  *Text,
  OUT long        *Offset
  )
{
  CHAR8               *End;
  unsigned long long  Value;

  if (Text == NULL || Offset == NULL || Text[0] == '\0' || Text[0] == '-') {
    return EFI_INVALID_PARAMETER;
  }

  errno = 0;
  End = NULL;
  Value = strtoull (Text, &End, 0);
  if (errno == ERANGE || End == Text) {
    return EFI_ABORTED;
  }

  if (*End == 'k' || *End == 'K') {
    End++;
    if (Value > (unsigned long long)LONG_MAX / 1024ULL) {
      return EFI_ABORTED;
    }
    Value *= 1024ULL;
  }

  if (*End != '\0' || Value > (unsigned long long)LONG_MAX) {
    return EFI_ABORTED;
  }

  *Offset = (long)Value;
  return EFI_SUCCESS;
}

EFI_STATUS
ParseGuidBaseNameFile (
  CHAR8    *FileName
  );

EFI_STATUS
FreeGuidBaseNameList (
  VOID
  );

EFI_STATUS
PrintGuidName (
  IN UINT8    *GuidStr
  );

EFI_STATUS
ParseSection (
  IN UINT8  *SectionBuffer,
  IN UINT32 BufferLength
  );

EFI_STATUS
DumpDepexSection (
  IN UINT8    *Ptr,
  IN UINT32   SectionLength
  );

STATIC
EFI_STATUS
ReadHeader (
  IN FILE       *InputFile,
  OUT UINT32    *FvSize,
  OUT BOOLEAN   *ErasePolarity
  );

STATIC
EFI_STATUS
PrintFileInfo (
  EFI_FIRMWARE_VOLUME_HEADER  *FvImage,
  EFI_FFS_FILE_HEADER         *FileHeader,
  BOOLEAN                     ErasePolarity
  );

static
EFI_STATUS
PrintFvInfo (
  IN VOID     *Fv,
  IN UINTN    FvBufferSize,
  IN BOOLEAN  IsChildFv
  );

static
VOID
LoadGuidedSectionToolsTxt (
  IN CHAR8* FirmwareVolumeFilename
  );

STATIC
EFI_STATUS
CombinePath (
  IN  CONST CHAR8  *DefaultPath,
  IN  CONST CHAR8  *AppendPath,
  OUT CHAR8        *NewPath,
  IN  size_t       NewPathSize
  );

void
Usage (
  VOID
  );

STATIC
EFI_STATUS
BoundedUnicodeLength (
  IN CONST CHAR16  *String,
  IN UINTN         BufferSize,
  OUT UINTN        *Length
  )
{
  UINTN Index;
  UINTN CharacterCount;

  if (String == NULL || Length == NULL || BufferSize < sizeof (CHAR16)) {
    return EFI_INVALID_PARAMETER;
  }
  CharacterCount = BufferSize / sizeof (CHAR16);
  for (Index = 0; Index < CharacterCount; Index++) {
    CHAR16 Character;
    memcpy (&Character, (CONST UINT8 *)String + Index * sizeof (CHAR16), sizeof (Character));
    if (Character == 0) {
      *Length = Index;
      return EFI_SUCCESS;
    }
  }
  return EFI_VOLUME_CORRUPTED;
}

STATIC
VOID
Unicode2AsciiStringN (
  IN CONST CHAR16  *Source,
  IN UINTN         Length,
  OUT CHAR8        *Destination
  )
{
  UINTN Index;
  for (Index = 0; Index < Length; Index++) {
    CHAR16 Character;
    memcpy (&Character, (CONST UINT8 *)Source + Index * sizeof (CHAR16), sizeof (Character));
    Destination[Index] = (CHAR8)Character;
  }
  Destination[Length] = '\0';
}

STATIC
EFI_STATUS
CreateTemporaryFilePath (
  OUT CHAR8  **Path
  )
{
#ifdef _WIN32
  CHAR8 TempDirectory[MAX_PATH];
  CHAR8 TempFile[MAX_PATH];
  DWORD Length;

  if (Path == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  *Path = NULL;
  Length = GetTempPathA (sizeof (TempDirectory), TempDirectory);
  if (Length == 0 || Length >= sizeof (TempDirectory) ||
      GetTempFileNameA (TempDirectory, "vli", 0, TempFile) == 0) {
    return EFI_ABORTED;
  }
  *Path = CloneString (TempFile);
  if (*Path == NULL) {
    remove (TempFile);
    return EFI_OUT_OF_RESOURCES;
  }
#else
  CHAR8 Template[] = "/tmp/volinfo-XXXXXX";
  int Descriptor;

  if (Path == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  *Path = NULL;
  Descriptor = mkstemp (Template);
  if (Descriptor < 0) {
    return EFI_ABORTED;
  }
  if (close (Descriptor) != 0) {
    remove (Template);
    return EFI_ABORTED;
  }
  *Path = CloneString (Template);
  if (*Path == NULL) {
    remove (Template);
    return EFI_OUT_OF_RESOURCES;
  }
#endif
  return EFI_SUCCESS;
}

STATIC
int
RunExternalTool (
  IN CONST CHAR8  *Executable,
  IN CHAR8 *CONST Arguments[]
  )
{
  if (Executable == NULL || Arguments == NULL) {
    return -1;
  }
#ifdef _WIN32
  return (int)_spawnvp (_P_WAIT, Executable, (CONST CHAR8 *CONST *)Arguments);
#else
  pid_t Child;
  pid_t WaitResult;
  int Status;

  Child = fork ();
  if (Child < 0) {
    return -1;
  }
  if (Child == 0) {
    execvp (Executable, Arguments);
    _exit (127);
  }
  do {
    WaitResult = waitpid (Child, &Status, 0);
  } while (WaitResult < 0 && errno == EINTR);
  if (WaitResult < 0 || !WIFEXITED (Status)) {
    return -1;
  }
  return WEXITSTATUS (Status);
#endif
}

int
main (
  int       argc,
  char      *argv[]
  )
/*++

Routine Description:

  GC_TODO: Add function description

Arguments:

  argc  - GC_TODO: add argument description
  ]     - GC_TODO: add argument description

Returns:

  GC_TODO: add return values

--*/
{
  FILE                        *InputFile;
  size_t                      BytesRead;
  EFI_FIRMWARE_VOLUME_HEADER  *FvImage;
  UINT32                      FvSize;
  EFI_STATUS                  Status;
  long                        Offset;
  BOOLEAN                     ErasePolarity;
  UINT64                      LogLevel;
  CHAR8                       *OpenSslEnv;
  CHAR8                       *OpenSslCommand;
  size_t                      OpenSslPathSize;

  OpenSslEnv = NULL;
  SetUtilityName (UTILITY_NAME);
  //
  // Print utility header
  //
  printf ("%s Version %d.%d Build %s\n",
    UTILITY_NAME,
    UTILITY_MAJOR_VERSION,
    UTILITY_MINOR_VERSION,
    __BUILD_VERSION
    );

  if (argc == 1) {
    Usage ();
    return -1;
  }

  argc--;
  argv++;
  LogLevel = 0;
  Offset = 0;

  //
  // Look for help options
  //
  if ((strcmp(argv[0], "-h") == 0) || (strcmp(argv[0], "--help") == 0) ||
      (strcmp(argv[0], "-?") == 0) || (strcmp(argv[0], "/?") == 0)) {
    Usage();
    return  STATUS_SUCCESS;
  }
  //
  // Version has already be printed, so just return success
  //
  if (strcmp(argv[0], "--version") == 0) {
    return  STATUS_SUCCESS;
  }

  //
  // If they specified -x xref guid/basename cross-reference files, process it.
  // This will print the basename beside each file guid. To use it, specify
  // -x xref_filename to processdsc, then use xref_filename as a parameter
  // here.
  //
  while (argc > 0) {
    if ((strcmp(argv[0], "-x") == 0) || (strcmp(argv[0], "--xref") == 0)) {
      if (argc < 2) {
        Error (NULL, 0, 1001, "Missing option value", "%s requires a file name", argv[0]);
        return GetUtilityStatus ();
      }
      ParseGuidBaseNameFile (argv[1]);
      printf("ParseGuidBaseNameFile: %s\n", argv[1]);
      argc -= 2;
      argv += 2;
      continue;
    }
    if (strcmp(argv[0], "--offset") == 0) {
      if (argc < 2) {
        Error (NULL, 0, 1001, "Missing option value", "%s requires an offset", argv[0]);
        return GetUtilityStatus ();
      }
      Status = ParseOffsetArgument (argv[1], &Offset);
      if (EFI_ERROR (Status)) {
        Error (NULL, 0, 1003, "Invalid option value", "Offset = %s", argv[1]);
        return GetUtilityStatus ();
      }

      argc -= 2;
      argv += 2;
      continue;
    }
    if ((stricmp (argv[0], "--hash") == 0)) {
      if (EnableHash == TRUE) {
        //
        // --hash already given in the option, ignore this one
        //
        argc --;
        argv ++;
        continue;
      }
      EnableHash = TRUE;
      OpenSslCommand = "openssl";
      OpenSslEnv = getenv("OPENSSL_PATH");
      if (OpenSslEnv == NULL) {
        OpenSslPath = OpenSslCommand;
      } else {
        if (strlen (OpenSslEnv) > SIZE_MAX - strlen (OpenSslCommand) - 2) {
          Error (NULL, 0, 1003, "Invalid OPENSSL_PATH", "path is too long");
          return GetUtilityStatus ();
        }
        OpenSslPathSize = strlen (OpenSslEnv) + strlen (OpenSslCommand) + 2;
        OpenSslPath = malloc (OpenSslPathSize);
        if (OpenSslPath == NULL) {
          Error (NULL, 0, 4001, "Resource", "memory cannot be allocated!");
          return GetUtilityStatus ();
        }
        Status = CombinePath (OpenSslEnv, OpenSslCommand, OpenSslPath, OpenSslPathSize);
        if (EFI_ERROR (Status)) {
          free (OpenSslPath);
          OpenSslPath = NULL;
          Error (NULL, 0, 1003, "Invalid OPENSSL_PATH", "cannot construct the OpenSSL executable path");
          return GetUtilityStatus ();
        }
      }
      if (OpenSslPath == NULL){
        Error (NULL, 0, 3000, "Open SSL command not available.  Please verify PATH or set OPENSSL_PATH.", NULL);
        return GetUtilityStatus ();
      }
      argc --;
      argv ++;
      continue;
    }

    if ((stricmp (argv[0], "-v") == 0) || (stricmp (argv[0], "--verbose") == 0)) {
      SetPrintLevel (VERBOSE_LOG_LEVEL);
      argc --;
      argv ++;
      continue;
    }

    if ((stricmp (argv[0], "-q") == 0) || (stricmp (argv[0], "--quiet") == 0)) {
      SetPrintLevel (KEY_LOG_LEVEL);
      argc --;
      argv ++;
      continue;
    }

    if ((stricmp (argv[0], "-d") == 0) || (stricmp (argv[0], "--debug") == 0)) {
      if (argc < 2) {
        Error (NULL, 0, 1001, "Missing option value", "%s requires a debug level", argv[0]);
        return GetUtilityStatus ();
      }
      Status = AsciiStringToUint64 (argv[1], FALSE, &LogLevel);
      if (EFI_ERROR (Status)) {
        Error (NULL, 0, 1003, "Invalid option value", "%s = %s", argv[0], argv[1]);
        return -1;
      }
      if (LogLevel > 9) {
        Error (NULL, 0, 1003, "Invalid option value", "Debug Level range is 0-9, current input level is %d", (int) LogLevel);
        return -1;
      }
      SetPrintLevel (LogLevel);
      DebugMsg (NULL, 0, 9, "Debug Mode Set", "Debug Output Mode Level %s is set!", argv[1]);
      argc -= 2;
      argv += 2;
      continue;
    }

    mUtilityFilename = argv[0];
    argc --;
    argv ++;
  }

  //
  // Open the file containing the FV
  //
  if (mUtilityFilename == NULL) {
    Error (NULL, 0, 1001, "Missing option", "Input files are not specified");
    return GetUtilityStatus ();
  }
  InputFile = fopen (LongFilePath (mUtilityFilename), "rb");
  if (InputFile == NULL) {
    Error (NULL, 0, 0001, "Error opening the input file", mUtilityFilename);
    return GetUtilityStatus ();
  }
  //
  // Skip over pad bytes if specified. This is used if they prepend 0xff
  // data to the FV image binary.
  //
  if (Offset != 0 && fseek (InputFile, Offset, SEEK_SET) != 0) {
    Error (NULL, 0, 0004, "error seeking in input file", mUtilityFilename);
    fclose (InputFile);
    return GetUtilityStatus ();
  }
  //
  // Determine size of FV
  //
  Status = ReadHeader (InputFile, &FvSize, &ErasePolarity);
  if (EFI_ERROR (Status)) {
    Error (NULL, 0, 0003, "error parsing FV image", "%s Header is invalid", mUtilityFilename);
    fclose (InputFile);
    return GetUtilityStatus ();
  }
  //
  // Allocate a buffer for the FV image
  //
  FvImage = malloc (FvSize);
  if (FvImage == NULL) {
    Error (NULL, 0, 4001, "Resource: Memory can't be allocated", NULL);
    fclose (InputFile);
    return GetUtilityStatus ();
  }
  //
  // Seek to the start of the image, then read the entire FV to the buffer
  //
  if (fseek (InputFile, Offset, SEEK_SET) != 0) {
    Error (NULL, 0, 0004, "error seeking in input file", mUtilityFilename);
    free (FvImage);
    fclose (InputFile);
    return GetUtilityStatus ();
  }
  BytesRead = fread (FvImage, 1, FvSize, InputFile);
  fclose (InputFile);
  if (BytesRead != (size_t)FvSize) {
    Error (NULL, 0, 0004, "error reading FvImage from", mUtilityFilename);
    free (FvImage);
    return GetUtilityStatus ();
  }

  LoadGuidedSectionToolsTxt (mUtilityFilename);

  Status = PrintFvInfo (FvImage, FvSize, FALSE);
  if (EFI_ERROR (Status)) {
    free (FvImage);
    FreeGuidBaseNameList ();
    if (OpenSslEnv != NULL) {
      free (OpenSslPath);
    }
    return GetUtilityStatus ();
  }

  //
  // Clean up
  //
  free (FvImage);
  FreeGuidBaseNameList ();
  if (OpenSslEnv != NULL) {
    free (OpenSslPath);
  }
  return GetUtilityStatus ();
}


static
EFI_STATUS
PrintFvInfo (
  IN VOID     *Fv,
  IN UINTN    FvBufferSize,
  IN BOOLEAN  IsChildFv
  )
{
  EFI_STATUS                  Status;
  UINTN                       NumberOfFiles;
  BOOLEAN                     ErasePolarity;
  UINTN                       FvSize;
  EFI_FFS_FILE_HEADER         *CurrentFile;
  EFI_FIRMWARE_VOLUME_HEADER  *Header;
  UINTN                       Key;

  if (Fv == NULL || FvBufferSize < sizeof (EFI_FIRMWARE_VOLUME_HEADER)) {
    Error (NULL, 0, 0003, "error parsing FV image", "firmware volume header is truncated");
    return EFI_VOLUME_CORRUPTED;
  }

  Header = (EFI_FIRMWARE_VOLUME_HEADER *)Fv;
  if (Header->FvLength > FvBufferSize) {
    Error (NULL, 0, 0003, "error parsing FV image", "firmware volume length exceeds its containing buffer");
    return EFI_VOLUME_CORRUPTED;
  }

  Status = FvBufGetSize (Fv, &FvSize);
  if (EFI_ERROR (Status) || FvSize > FvBufferSize) {
    Error (NULL, 0, 0003, "error parsing FV image", "firmware volume header or block map is invalid");
    return EFI_VOLUME_CORRUPTED;
  }

  NumberOfFiles = 0;
  ErasePolarity = (Header->Attributes & EFI_FVB2_ERASE_POLARITY) != 0 ? TRUE : FALSE;
  Key = 0;
  Status = FvBufFindNextFile (Fv, &Key, (VOID **)&CurrentFile);
  if (Status == EFI_NOT_FOUND) {
    CurrentFile = NULL;
  } else if (EFI_ERROR (Status)) {
    Error (NULL, 0, 0003, "error parsing FV image", "cannot find the first file in the FV image");
    return Status;
  }

  while (CurrentFile != NULL) {
    NumberOfFiles++;
    Status = PrintFileInfo (Header, CurrentFile, ErasePolarity);
    if (EFI_ERROR (Status)) {
      Error (NULL, 0, 0003, "error parsing FV image", "failed to parse a file in the FV");
      return Status;
    }
    Status = FvBufFindNextFile (Fv, &Key, (VOID **)&CurrentFile);
    if (Status == EFI_NOT_FOUND) {
      CurrentFile = NULL;
    } else if (EFI_ERROR (Status)) {
      Error (NULL, 0, 0003, "error parsing FV image", "cannot find the next file in the FV image");
      return Status;
    }
  }

  if (IsChildFv) {
    printf ("There are a total of %d files in the child FV\n", (int)NumberOfFiles);
  } else {
    printf ("There are a total of %d files in this FV\n", (int)NumberOfFiles);
  }
  return EFI_SUCCESS;
}

UINT32
GetOccupiedSize (
  IN UINT32  ActualSize,
  IN UINT32  Alignment
  )
/*++

Routine Description:

  This function returns the next larger size that meets the alignment
  requirement specified.

Arguments:

  ActualSize      The size.
  Alignment       The desired alignment.

Returns:

  EFI_SUCCESS             Function completed successfully.
  EFI_ABORTED             The function encountered an error.

--*/
{
  UINT32  OccupiedSize;

  OccupiedSize = ActualSize;
  while ((OccupiedSize & (Alignment - 1)) != 0) {
    OccupiedSize++;
  }

  return OccupiedSize;
}

static
CHAR8 *
SectionNameToStr (
  IN EFI_SECTION_TYPE   Type
  )
/*++

Routine Description:

  Converts EFI Section names to Strings

Arguments:

  Type  - The EFI Section type

Returns:

  CHAR8* - Pointer to the String containing the section name.

--*/
{
  CHAR8 *SectionStr;
  CHAR8 *SectionTypeStringTable[] = {
    //
    // 0X00
    //
    "EFI_SECTION_ALL",
    //
    // 0x01
    //
    "EFI_SECTION_COMPRESSION",
    //
    // 0x02
    //
    "EFI_SECTION_GUID_DEFINED",
    //
    // 0x03
    //
    "Unknown section type - Reserved 0x03",
    //
    // 0x04
    //
    "Unknown section type - Reserved 0x04",
    //
    // 0x05
    //
    "Unknown section type - Reserved 0x05",
    //
    // 0x06
    //
    "Unknown section type - Reserved 0x06",
    //
    // 0x07
    //
    "Unknown section type - Reserved 0x07",
    //
    // 0x08
    //
    "Unknown section type - Reserved 0x08",
    //
    // 0x09
    //
    "Unknown section type - Reserved 0x09",
    //
    // 0x0A
    //
    "Unknown section type - Reserved 0x0A",
    //
    // 0x0B
    //
    "Unknown section type - Reserved 0x0B",
    //
    // 0x0C
    //
    "Unknown section type - Reserved 0x0C",
    //
    // 0x0D
    //
    "Unknown section type - Reserved 0x0D",
    //
    // 0x0E
    //
    "Unknown section type - Reserved 0x0E",
    //
    // 0x0F
    //
    "Unknown section type - Reserved 0x0E",
    //
    // 0x10
    //
    "EFI_SECTION_PE32",
    //
    // 0x11
    //
    "EFI_SECTION_PIC",
    //
    // 0x12
    //
    "EFI_SECTION_TE",
    //
    // 0x13
    //
    "EFI_SECTION_DXE_DEPEX",
    //
    // 0x14
    //
    "EFI_SECTION_VERSION",
    //
    // 0x15
    //
    "EFI_SECTION_USER_INTERFACE",
    //
    // 0x16
    //
    "EFI_SECTION_COMPATIBILITY16",
    //
    // 0x17
    //
    "EFI_SECTION_FIRMWARE_VOLUME_IMAGE ",
    //
    // 0x18
    //
    "EFI_SECTION_FREEFORM_SUBTYPE_GUID ",
    //
    // 0x19
    //
    "EFI_SECTION_RAW",
    //
    // 0x1A
    //
    "Unknown section type - 0x1A",
    //
    // 0x1B
    //
    "EFI_SECTION_PEI_DEPEX",
    //
    // 0x1C
    //
    "EFI_SECTION_SMM_DEPEX",
    //
    // 0x1C+
    //
    "Unknown section type - Reserved - beyond last defined section"
  };

  if (Type > EFI_SECTION_LAST_SECTION_TYPE) {
    Type = EFI_SECTION_LAST_SECTION_TYPE + 1;
  }

  SectionStr = malloc (100);
  if (SectionStr == NULL) {
    printf ("Error: Out of memory resources.\n");
    return SectionStr;
  }
  strcpy (SectionStr, SectionTypeStringTable[Type]);
  return SectionStr;
}

STATIC
EFI_STATUS
ReadHeader (
  IN FILE       *InputFile,
  OUT UINT32    *FvSize,
  OUT BOOLEAN   *ErasePolarity
  )
/*++

Routine Description:

  This function determines the size of the FV and the erase polarity.  The
  erase polarity is the FALSE value for file state.

Arguments:

  InputFile       The file that contains the FV image.
  FvSize          The size of the FV.
  ErasePolarity   The FV erase polarity.

Returns:

  EFI_SUCCESS             Function completed successfully.
  EFI_INVALID_PARAMETER   A required parameter was NULL or is out of range.
  EFI_ABORTED             The function encountered an error.

--*/
{
  EFI_FIRMWARE_VOLUME_HEADER  VolumeHeader;
  EFI_FV_BLOCK_MAP_ENTRY      BlockMap;
  UINTN                       Signature[2];
  UINTN                       BytesRead;
  UINTN                       MinimumHeaderSize;
  UINT64                      Size;
  UINT64                      BlockSize;
  UINT64                      FileBytesRemaining;
  long                        StartOffset;
  long                        EndOffset;
  size_t                      ReadSize;

  Size = 0;
  //
  // Check input parameters
  //
  if (InputFile == NULL || FvSize == NULL || ErasePolarity == NULL) {
    Error (__FILE__, __LINE__, 0, "application error", "invalid parameter to function");
    return EFI_INVALID_PARAMETER;
  }

  *FvSize = 0;
  *ErasePolarity = FALSE;
  MinimumHeaderSize = sizeof (EFI_FIRMWARE_VOLUME_HEADER);

  StartOffset = ftell (InputFile);
  if (StartOffset < 0 || fseek (InputFile, 0, SEEK_END) != 0) {
    return EFI_ABORTED;
  }
  EndOffset = ftell (InputFile);
  if (EndOffset < StartOffset || fseek (InputFile, StartOffset, SEEK_SET) != 0) {
    return EFI_ABORTED;
  }
  FileBytesRemaining = (UINT64)(unsigned long)(EndOffset - StartOffset);
  if (FileBytesRemaining < MinimumHeaderSize) {
    return EFI_ABORTED;
  }
  //
  // Read the fixed portion of the header before trusting any header fields.
  //
  ReadSize = fread (&VolumeHeader, sizeof (EFI_FIRMWARE_VOLUME_HEADER) - sizeof (EFI_FV_BLOCK_MAP_ENTRY), 1, InputFile);
  if (ReadSize != 1) {
    return EFI_ABORTED;
  }
  BytesRead = sizeof (EFI_FIRMWARE_VOLUME_HEADER) - sizeof (EFI_FV_BLOCK_MAP_ENTRY);
  if (VolumeHeader.Signature != EFI_FVH_SIGNATURE ||
      VolumeHeader.HeaderLength < MinimumHeaderSize ||
      VolumeHeader.HeaderLength > FileBytesRemaining ||
      VolumeHeader.FvLength < VolumeHeader.HeaderLength ||
      VolumeHeader.FvLength > FileBytesRemaining ||
      VolumeHeader.FvLength > MAX_UINT32) {
    return EFI_ABORTED;
  }

  Signature[0] = VolumeHeader.Signature;
  Signature[1] = 0;

  //
  // Print FV header information only after validating the fixed header.
  //
  printf ("Signature:        %s (%X)\n", (char *) Signature, (unsigned) VolumeHeader.Signature);
  printf ("Attributes:       %X\n", (unsigned) VolumeHeader.Attributes);

  if (VolumeHeader.Attributes & EFI_FVB2_READ_DISABLED_CAP) {
    printf ("       EFI_FVB2_READ_DISABLED_CAP\n");
  }

  if (VolumeHeader.Attributes & EFI_FVB2_READ_ENABLED_CAP) {
    printf ("       EFI_FVB2_READ_ENABLED_CAP\n");
  }

  if (VolumeHeader.Attributes & EFI_FVB2_READ_STATUS) {
    printf ("       EFI_FVB2_READ_STATUS\n");
  }

  if (VolumeHeader.Attributes & EFI_FVB2_WRITE_DISABLED_CAP) {
    printf ("       EFI_FVB2_WRITE_DISABLED_CAP\n");
  }

  if (VolumeHeader.Attributes & EFI_FVB2_WRITE_ENABLED_CAP) {
    printf ("       EFI_FVB2_WRITE_ENABLED_CAP\n");
  }

  if (VolumeHeader.Attributes & EFI_FVB2_WRITE_STATUS) {
    printf ("       EFI_FVB2_WRITE_STATUS\n");
  }

  if (VolumeHeader.Attributes & EFI_FVB2_LOCK_CAP) {
    printf ("       EFI_FVB2_LOCK_CAP\n");
  }

  if (VolumeHeader.Attributes & EFI_FVB2_LOCK_STATUS) {
    printf ("       EFI_FVB2_LOCK_STATUS\n");
  }

  if (VolumeHeader.Attributes & EFI_FVB2_STICKY_WRITE) {
    printf ("       EFI_FVB2_STICKY_WRITE\n");
  }

  if (VolumeHeader.Attributes & EFI_FVB2_MEMORY_MAPPED) {
    printf ("       EFI_FVB2_MEMORY_MAPPED\n");
  }

  if (VolumeHeader.Attributes & EFI_FVB2_ERASE_POLARITY) {
    printf ("       EFI_FVB2_ERASE_POLARITY\n");
    *ErasePolarity = TRUE;
  }

#if (PI_SPECIFICATION_VERSION < 0x00010000)
  if (VolumeHeader.Attributes & EFI_FVB2_ALIGNMENT) {
    printf ("       EFI_FVB2_ALIGNMENT\n");
  }

  if (VolumeHeader.Attributes & EFI_FVB2_ALIGNMENT_2) {
    printf ("       EFI_FVB2_ALIGNMENT_2\n");
  }

  if (VolumeHeader.Attributes & EFI_FVB2_ALIGNMENT_4) {
    printf ("       EFI_FVB2_ALIGNMENT_4\n");
  }

  if (VolumeHeader.Attributes & EFI_FVB2_ALIGNMENT_8) {
    printf ("       EFI_FVB2_ALIGNMENT_8\n");
  }

  if (VolumeHeader.Attributes & EFI_FVB2_ALIGNMENT_16) {
    printf ("       EFI_FVB2_ALIGNMENT_16\n");
  }

  if (VolumeHeader.Attributes & EFI_FVB2_ALIGNMENT_32) {
    printf ("       EFI_FVB2_ALIGNMENT_32\n");
  }

  if (VolumeHeader.Attributes & EFI_FVB2_ALIGNMENT_64) {
    printf ("        EFI_FVB2_ALIGNMENT_64\n");
  }

  if (VolumeHeader.Attributes & EFI_FVB2_ALIGNMENT_128) {
    printf ("        EFI_FVB2_ALIGNMENT_128\n");
  }

  if (VolumeHeader.Attributes & EFI_FVB2_ALIGNMENT_256) {
    printf ("        EFI_FVB2_ALIGNMENT_256\n");
  }

  if (VolumeHeader.Attributes & EFI_FVB2_ALIGNMENT_512) {
    printf ("        EFI_FVB2_ALIGNMENT_512\n");
  }

  if (VolumeHeader.Attributes & EFI_FVB2_ALIGNMENT_1K) {
    printf ("        EFI_FVB2_ALIGNMENT_1K\n");
  }

  if (VolumeHeader.Attributes & EFI_FVB2_ALIGNMENT_2K) {
    printf ("        EFI_FVB2_ALIGNMENT_2K\n");
  }

  if (VolumeHeader.Attributes & EFI_FVB2_ALIGNMENT_4K) {
    printf ("        EFI_FVB2_ALIGNMENT_4K\n");
  }

  if (VolumeHeader.Attributes & EFI_FVB2_ALIGNMENT_8K) {
    printf ("        EFI_FVB2_ALIGNMENT_8K\n");
  }

  if (VolumeHeader.Attributes & EFI_FVB2_ALIGNMENT_16K) {
    printf ("        EFI_FVB2_ALIGNMENT_16K\n");
  }

  if (VolumeHeader.Attributes & EFI_FVB2_ALIGNMENT_32K) {
    printf ("        EFI_FVB2_ALIGNMENT_32K\n");
  }

  if (VolumeHeader.Attributes & EFI_FVB2_ALIGNMENT_64K) {
    printf ("        EFI_FVB2_ALIGNMENT_64K\n");
  }

#else

  if (VolumeHeader.Attributes & EFI_FVB2_READ_LOCK_CAP) {
    printf ("       EFI_FVB2_READ_LOCK_CAP\n");
  }

  if (VolumeHeader.Attributes & EFI_FVB2_READ_LOCK_STATUS) {
    printf ("       EFI_FVB2_READ_LOCK_STATUS\n");
  }

  if (VolumeHeader.Attributes & EFI_FVB2_WRITE_LOCK_CAP) {
    printf ("       EFI_FVB2_WRITE_LOCK_CAP\n");
  }

  if (VolumeHeader.Attributes & EFI_FVB2_WRITE_LOCK_STATUS) {
    printf ("       EFI_FVB2_WRITE_LOCK_STATUS\n");
  }

  switch (VolumeHeader.Attributes & EFI_FVB2_ALIGNMENT) {
  case EFI_FVB2_ALIGNMENT_1:
    printf ("        EFI_FVB2_ALIGNMENT_1\n");
    break;
  case EFI_FVB2_ALIGNMENT_2:
    printf ("        EFI_FVB2_ALIGNMENT_2\n");
    break;
  case EFI_FVB2_ALIGNMENT_4:
    printf ("        EFI_FVB2_ALIGNMENT_4\n");
    break;
  case EFI_FVB2_ALIGNMENT_8:
    printf ("        EFI_FVB2_ALIGNMENT_8\n");
    break;
  case EFI_FVB2_ALIGNMENT_16:
    printf ("        EFI_FVB2_ALIGNMENT_16\n");
    break;
  case EFI_FVB2_ALIGNMENT_32:
    printf ("        EFI_FVB2_ALIGNMENT_32\n");
    break;
  case EFI_FVB2_ALIGNMENT_64:
    printf ("        EFI_FVB2_ALIGNMENT_64\n");
    break;
  case EFI_FVB2_ALIGNMENT_128:
    printf ("        EFI_FVB2_ALIGNMENT_128\n");
    break;
  case EFI_FVB2_ALIGNMENT_256:
    printf ("        EFI_FVB2_ALIGNMENT_256\n");
    break;
  case EFI_FVB2_ALIGNMENT_512:
    printf ("        EFI_FVB2_ALIGNMENT_512\n");
    break;
  case EFI_FVB2_ALIGNMENT_1K:
    printf ("        EFI_FVB2_ALIGNMENT_1K\n");
    break;
  case EFI_FVB2_ALIGNMENT_2K:
    printf ("        EFI_FVB2_ALIGNMENT_2K\n");
    break;
  case EFI_FVB2_ALIGNMENT_4K:
    printf ("        EFI_FVB2_ALIGNMENT_4K\n");
    break;
  case EFI_FVB2_ALIGNMENT_8K:
    printf ("        EFI_FVB2_ALIGNMENT_8K\n");
    break;
  case EFI_FVB2_ALIGNMENT_16K:
    printf ("        EFI_FVB2_ALIGNMENT_16K\n");
    break;
  case EFI_FVB2_ALIGNMENT_32K:
    printf ("        EFI_FVB2_ALIGNMENT_32K\n");
    break;
  case EFI_FVB2_ALIGNMENT_64K:
    printf ("        EFI_FVB2_ALIGNMENT_64K\n");
    break;
  case EFI_FVB2_ALIGNMENT_128K:
    printf ("        EFI_FVB2_ALIGNMENT_128K\n");
    break;
  case EFI_FVB2_ALIGNMENT_256K:
    printf ("        EFI_FVB2_ALIGNMENT_256K\n");
    break;
  case EFI_FVB2_ALIGNMENT_512K:
    printf ("        EFI_FVB2_ALIGNMENT_512K\n");
    break;
  case EFI_FVB2_ALIGNMENT_1M:
    printf ("        EFI_FVB2_ALIGNMENT_1M\n");
    break;
  case EFI_FVB2_ALIGNMENT_2M:
    printf ("        EFI_FVB2_ALIGNMENT_2M\n");
    break;
  case EFI_FVB2_ALIGNMENT_4M:
    printf ("        EFI_FVB2_ALIGNMENT_4M\n");
    break;
  case EFI_FVB2_ALIGNMENT_8M:
    printf ("        EFI_FVB2_ALIGNMENT_8M\n");
    break;
  case EFI_FVB2_ALIGNMENT_16M:
    printf ("        EFI_FVB2_ALIGNMENT_16M\n");
    break;
  case EFI_FVB2_ALIGNMENT_32M:
    printf ("        EFI_FVB2_ALIGNMENT_32M\n");
    break;
  case EFI_FVB2_ALIGNMENT_64M:
    printf ("        EFI_FVB2_ALIGNMENT_64M\n");
    break;
  case EFI_FVB2_ALIGNMENT_128M:
    printf ("        EFI_FVB2_ALIGNMENT_128M\n");
    break;
  case EFI_FVB2_ALIGNMENT_256M:
    printf ("        EFI_FVB2_ALIGNMENT_256M\n");
    break;
  case EFI_FVB2_ALIGNMENT_512M:
    printf ("        EFI_FVB2_ALIGNMENT_512M\n");
    break;
  case EFI_FVB2_ALIGNMENT_1G:
    printf ("        EFI_FVB2_ALIGNMENT_1G\n");
    break;
  case EFI_FVB2_ALIGNMENT_2G:
    printf ("        EFI_FVB2_ALIGNMENT_2G\n");
    break;
  default:
    break;
  }
#endif
  printf ("Header Length:         0x%08X\n", VolumeHeader.HeaderLength);
  printf ("File System ID:        ");
  PrintGuid (&VolumeHeader.FileSystemGuid);
  //
  // printf ("\n");
  //
  printf ("Revision:              0x%04X\n", VolumeHeader.Revision);

  do {
    if (BytesRead > VolumeHeader.HeaderLength - sizeof (EFI_FV_BLOCK_MAP_ENTRY)) {
      return EFI_ABORTED;
    }
    ReadSize = fread (&BlockMap, sizeof (EFI_FV_BLOCK_MAP_ENTRY), 1, InputFile);
    if (ReadSize != 1) {
      return EFI_ABORTED;
    }
    BytesRead += sizeof (EFI_FV_BLOCK_MAP_ENTRY);

    if ((BlockMap.NumBlocks == 0) != (BlockMap.Length == 0)) {
      return EFI_ABORTED;
    }
    if (BlockMap.NumBlocks != 0) {
      printf ("Number of Blocks:      0x%08X\n", (unsigned) BlockMap.NumBlocks);
      printf ("Block Length:          0x%08X\n", (unsigned) BlockMap.Length);
      BlockSize = (UINT64)BlockMap.NumBlocks * (UINT64)BlockMap.Length;
      if (Size > MAX_UINT64 - BlockSize) {
        return EFI_ABORTED;
      }
      Size += BlockSize;
    }

  } while (!(BlockMap.NumBlocks == 0 && BlockMap.Length == 0));

  if (BytesRead != VolumeHeader.HeaderLength) {
    printf ("ERROR: Header length not consistent with Block Maps!\n");
    return EFI_ABORTED;
  }

  if (VolumeHeader.FvLength != Size) {
    printf ("ERROR: Volume Size not consistant with Block Maps!\n");
    return EFI_ABORTED;
  }

  printf ("Total Volume Size:     0x%08X\n", (unsigned) Size);

  *FvSize = (UINT32)Size;

  //
  // rewind (InputFile);
  //
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
PrintFileInfo (
  EFI_FIRMWARE_VOLUME_HEADER  *FvImage,
  EFI_FFS_FILE_HEADER         *FileHeader,
  BOOLEAN                     ErasePolarity
  )
/*++

Routine Description:

  GC_TODO: Add function description

Arguments:

  FvImage       - GC_TODO: add argument description
  FileHeader    - GC_TODO: add argument description
  ErasePolarity - GC_TODO: add argument description

Returns:

  EFI_SUCCESS - GC_TODO: Add description for return value
  EFI_ABORTED - GC_TODO: Add description for return value

--*/
{
  UINT32              FileLength;
  UINT8               FileState;
  UINT8               Checksum;
  EFI_FFS_FILE_HEADER2 BlankHeader;
  EFI_STATUS          Status;
  UINT8               GuidBuffer[PRINTED_GUID_BUFFER_SIZE];
  UINT32              HeaderSize;
#if (PI_SPECIFICATION_VERSION < 0x00010000)
  UINT16              *Tail;
#endif
  //
  // Check if we have free space
  //
  HeaderSize = FvBufGetFfsHeaderSize(FileHeader);
  if (ErasePolarity) {
    memset (&BlankHeader, -1, HeaderSize);
  } else {
    memset (&BlankHeader, 0, HeaderSize);
  }

  if (memcmp (&BlankHeader, FileHeader, HeaderSize) == 0) {
    return EFI_SUCCESS;
  }
  //
  // Print file information.
  //
  printf ("============================================================\n");

  printf ("File Name:        ");
  PrintGuidToBuffer (&FileHeader->Name, GuidBuffer, sizeof (GuidBuffer), TRUE);
  printf ("%s  ", GuidBuffer);
  PrintGuidName (GuidBuffer);
  printf ("\n");

  //
  //  PrintGuid (&FileHeader->Name);
  //  printf ("\n");
  //
  FileLength = FvBufGetFfsFileSize (FileHeader);
  printf ("File Offset:      0x%08X\n", (unsigned) ((UINTN) FileHeader - (UINTN) FvImage));
  printf ("File Length:      0x%08X\n", (unsigned) FileLength);
  printf ("File Attributes:  0x%02X\n", FileHeader->Attributes);
  printf ("File State:       0x%02X\n", FileHeader->State);

  //
  // Print file state
  //
  FileState = GetFileState (ErasePolarity, FileHeader);

  switch (FileState) {

  case EFI_FILE_HEADER_CONSTRUCTION:
    printf ("        EFI_FILE_HEADER_CONSTRUCTION\n");
    return EFI_SUCCESS;

  case EFI_FILE_HEADER_INVALID:
    printf ("        EFI_FILE_HEADER_INVALID\n");
    return EFI_SUCCESS;

  case EFI_FILE_HEADER_VALID:
    printf ("        EFI_FILE_HEADER_VALID\n");
    Checksum  = CalculateSum8 ((UINT8 *) FileHeader, HeaderSize);
    Checksum  = (UINT8) (Checksum - FileHeader->IntegrityCheck.Checksum.File);
    Checksum  = (UINT8) (Checksum - FileHeader->State);
    if (Checksum != 0) {
      printf ("ERROR: Header checksum invalid.\n");
      return EFI_ABORTED;
    }

    return EFI_SUCCESS;

  case EFI_FILE_DELETED:
    printf ("        EFI_FILE_DELETED\n");
    goto ValidateFileState;

  case EFI_FILE_MARKED_FOR_UPDATE:
    printf ("        EFI_FILE_MARKED_FOR_UPDATE\n");
    goto ValidateFileState;

  case EFI_FILE_DATA_VALID:
    printf ("        EFI_FILE_DATA_VALID\n");

ValidateFileState:
    //
    // Calculate header checksum
    //
    Checksum  = CalculateSum8 ((UINT8 *) FileHeader, HeaderSize);
    Checksum  = (UINT8) (Checksum - FileHeader->IntegrityCheck.Checksum.File);
    Checksum  = (UINT8) (Checksum - FileHeader->State);
    if (Checksum != 0) {
      Error (NULL, 0, 0003, "error parsing FFS file", "FFS file with Guid %s has invalid header checksum", GuidBuffer);
      return EFI_ABORTED;
    }

    FileLength = FvBufGetFfsFileSize (FileHeader);

    if (FileHeader->Attributes & FFS_ATTRIB_CHECKSUM) {
      //
      // Calculate file checksum
      //
      Checksum  = CalculateSum8 ((UINT8 *)FileHeader + HeaderSize, FileLength - HeaderSize);
      Checksum  = Checksum + FileHeader->IntegrityCheck.Checksum.File;
      if (Checksum != 0) {
        Error (NULL, 0, 0003, "error parsing FFS file", "FFS file with Guid %s has invalid file checksum", GuidBuffer);
        return EFI_ABORTED;
      }
    } else {
      if (FileHeader->IntegrityCheck.Checksum.File != FFS_FIXED_CHECKSUM) {
        Error (NULL, 0, 0003, "error parsing FFS file", "FFS file with Guid %s has invalid header checksum -- not set to fixed value of 0xAA", GuidBuffer);
        return EFI_ABORTED;
      }
    }
#if (PI_SPECIFICATION_VERSION < 0x00010000)
    //
    // Verify tail if present
    //
    if (FileHeader->Attributes & FFS_ATTRIB_TAIL_PRESENT) {
      //
      // Verify tail is complement of integrity check field in the header.
      //
      Tail = (UINT16 *) ((UINTN) FileHeader + GetLength (FileHeader->Size) - sizeof (EFI_FFS_INTEGRITY_CHECK));
      if (FileHeader->IntegrityCheck.TailReference != (UINT16)~(*Tail)) {
        Error (NULL, 0, 0003, "error parsing FFS file", \
        "FFS file with Guid %s failed in the integrity check, tail is not the complement of the header field", GuidBuffer);
        return EFI_ABORTED;
      }
    }
 #endif
    break;

  default:
    Error (NULL, 0, 0003, "error parsing FFS file", "FFS file with Guid %s has the invalid/unrecognized file state bits", GuidBuffer);
    return EFI_ABORTED;
  }

  printf ("File Type:        0x%02X  ", FileHeader->Type);

  switch (FileHeader->Type) {

  case EFI_FV_FILETYPE_RAW:
    printf ("EFI_FV_FILETYPE_RAW\n");
    break;

  case EFI_FV_FILETYPE_FREEFORM:
    printf ("EFI_FV_FILETYPE_FREEFORM\n");
    break;

  case EFI_FV_FILETYPE_SECURITY_CORE:
    printf ("EFI_FV_FILETYPE_SECURITY_CORE\n");
    break;

  case EFI_FV_FILETYPE_PEI_CORE:
    printf ("EFI_FV_FILETYPE_PEI_CORE\n");
    break;

  case EFI_FV_FILETYPE_DXE_CORE:
    printf ("EFI_FV_FILETYPE_DXE_CORE\n");
    break;

  case EFI_FV_FILETYPE_PEIM:
    printf ("EFI_FV_FILETYPE_PEIM\n");
    break;

  case EFI_FV_FILETYPE_DRIVER:
    printf ("EFI_FV_FILETYPE_DRIVER\n");
    break;

  case EFI_FV_FILETYPE_COMBINED_PEIM_DRIVER:
    printf ("EFI_FV_FILETYPE_COMBINED_PEIM_DRIVER\n");
    break;

  case EFI_FV_FILETYPE_APPLICATION:
    printf ("EFI_FV_FILETYPE_APPLICATION\n");
    break;

  case EFI_FV_FILETYPE_SMM:
    printf ("EFI_FV_FILETYPE_SMM\n");
    break;

  case EFI_FV_FILETYPE_FIRMWARE_VOLUME_IMAGE:
    printf ("EFI_FV_FILETYPE_FIRMWARE_VOLUME_IMAGE\n");
    break;

  case EFI_FV_FILETYPE_COMBINED_SMM_DXE:
    printf ("EFI_FV_FILETYPE_COMBINED_SMM_DXE\n");
    break;

  case EFI_FV_FILETYPE_SMM_CORE:
    printf ("EFI_FV_FILETYPE_SMM_CORE\n");
    break;

  case EFI_FV_FILETYPE_MM_STANDALONE:
    printf ("EFI_FV_FILETYPE_MM_STANDALONE\n");
    break;

  case EFI_FV_FILETYPE_MM_CORE_STANDALONE:
    printf ("EFI_FV_FILETYPE_MM_CORE_STANDALONE\n");
    break;

  case EFI_FV_FILETYPE_FFS_PAD:
    printf ("EFI_FV_FILETYPE_FFS_PAD\n");
    break;

  default:
    printf ("\nERROR: Unrecognized file type %X.\n", FileHeader->Type);
    return EFI_ABORTED;
    break;
  }

  switch (FileHeader->Type) {

  case EFI_FV_FILETYPE_ALL:
  case EFI_FV_FILETYPE_RAW:
  case EFI_FV_FILETYPE_FFS_PAD:
    break;

  default:
    //
    // All other files have sections
    //
    Status = ParseSection (
              (UINT8 *) ((UINTN) FileHeader + HeaderSize),
              FvBufGetFfsFileSize (FileHeader) - HeaderSize
              );
    if (EFI_ERROR (Status)) {
      //
      // printf ("ERROR: Parsing the FFS file.\n");
      //
      return EFI_ABORTED;
    }
    break;
  }

  return EFI_SUCCESS;
}

typedef struct {
  UINT8   *Buffer;
  UINT32  Size;
} REBASE_IMAGE_FILE;

STATIC
EFI_STATUS
RebaseImageRead (
  IN     VOID    *FileHandle,
  IN     UINTN   FileOffset,
  IN OUT UINTN   *ReadSize,
  OUT    VOID    *Buffer
  )
{
  REBASE_IMAGE_FILE *File;

  if (FileHandle == NULL || ReadSize == NULL ||
      (Buffer == NULL && *ReadSize != 0)) {
    return EFI_INVALID_PARAMETER;
  }
  File = (REBASE_IMAGE_FILE *)FileHandle;
  if (FileOffset > File->Size || *ReadSize > File->Size - FileOffset) {
    return EFI_BAD_BUFFER_SIZE;
  }
  if (*ReadSize != 0) {
    memcpy (Buffer, File->Buffer + FileOffset, *ReadSize);
  }
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
GetPeHeaderLayout (
  IN CONST UINT8             *FileBuffer,
  IN UINT32                  FileSize,
  IN UINTN                   PeHeaderOffset,
  OUT EFI_IMAGE_FILE_HEADER  *FileHeader,
  OUT UINTN                  *OptionalHeaderOffset,
  OUT UINTN                  *SectionHeadersOffset,
  OUT UINT16                 *OptionalMagic
  )
{
  UINTN FileHeaderOffset;
  UINTN OptionalOffset;
  UINTN SectionOffset;
  UINTN SectionTableSize;

  if (FileBuffer == NULL || FileHeader == NULL ||
      OptionalHeaderOffset == NULL || SectionHeadersOffset == NULL ||
      OptionalMagic == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  if (PeHeaderOffset > FileSize || FileSize - PeHeaderOffset < sizeof (UINT32)) {
    return EFI_VOLUME_CORRUPTED;
  }
  FileHeaderOffset = PeHeaderOffset + sizeof (UINT32);
  if (FileHeaderOffset > FileSize || FileSize - FileHeaderOffset < sizeof (*FileHeader)) {
    return EFI_VOLUME_CORRUPTED;
  }
  memcpy (FileHeader, FileBuffer + FileHeaderOffset, sizeof (*FileHeader));

  OptionalOffset = FileHeaderOffset + sizeof (*FileHeader);
  if (OptionalOffset > FileSize ||
      FileHeader->SizeOfOptionalHeader > FileSize - OptionalOffset ||
      FileHeader->SizeOfOptionalHeader < sizeof (*OptionalMagic)) {
    return EFI_VOLUME_CORRUPTED;
  }
  memcpy (OptionalMagic, FileBuffer + OptionalOffset, sizeof (*OptionalMagic));

  SectionOffset = OptionalOffset + FileHeader->SizeOfOptionalHeader;
  SectionTableSize =
    (UINTN)FileHeader->NumberOfSections * sizeof (EFI_IMAGE_SECTION_HEADER);
  if (SectionOffset > FileSize || SectionTableSize > FileSize - SectionOffset) {
    return EFI_VOLUME_CORRUPTED;
  }
  *OptionalHeaderOffset = OptionalOffset;
  *SectionHeadersOffset = SectionOffset;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
SetAddressToSectionHeader (
  IN CONST CHAR8  *FileName,
  IN OUT UINT8    *FileBuffer,
  IN UINT32       FileSize,
  IN UINT64       NewPe32BaseAddress
  )
{
  EFI_STATUS                    Status;
  PE_COFF_LOADER_IMAGE_CONTEXT  ImageContext;
  REBASE_IMAGE_FILE             ReadContext;
  EFI_IMAGE_FILE_HEADER         FileHeader;
  EFI_IMAGE_SECTION_HEADER      SectionHeader;
  UINTN                         OptionalHeaderOffset;
  UINTN                         SectionHeadersOffset;
  UINT16                        OptionalMagic;
  UINTN                         Index;
  UINTN                         CurrentOffset;

  if (FileName == NULL || FileBuffer == NULL || FileSize == 0) {
    return EFI_INVALID_PARAMETER;
  }
  ReadContext.Buffer = FileBuffer;
  ReadContext.Size = FileSize;
  memset (&ImageContext, 0, sizeof (ImageContext));
  ImageContext.Handle = &ReadContext;
  ImageContext.ImageRead = RebaseImageRead;
  Status = PeCoffLoaderGetImageInfo (&ImageContext);
  if (EFI_ERROR (Status)) {
    Error (NULL, 0, 3000, "Invalid", "The input PeImage %s is not valid", FileName);
    return Status;
  }
  if (ImageContext.RelocationsStripped) {
    Error (NULL, 0, 3000, "Invalid", "The input PeImage %s has no relocation to be fixed up", FileName);
    return EFI_UNSUPPORTED;
  }
  Status = GetPeHeaderLayout (
             FileBuffer,
             FileSize,
             ImageContext.PeCoffHeaderOffset,
             &FileHeader,
             &OptionalHeaderOffset,
             &SectionHeadersOffset,
             &OptionalMagic
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }
  (VOID)OptionalHeaderOffset;
  (VOID)OptionalMagic;
  for (Index = 0; Index < FileHeader.NumberOfSections; Index++) {
    CurrentOffset = SectionHeadersOffset + Index * sizeof (SectionHeader);
    memcpy (&SectionHeader, FileBuffer + CurrentOffset, sizeof (SectionHeader));
    if ((SectionHeader.Characteristics & EFI_IMAGE_SCN_CNT_CODE) == 0) {
      memcpy (
        FileBuffer + CurrentOffset + offsetof (EFI_IMAGE_SECTION_HEADER, PointerToRelocations),
        &NewPe32BaseAddress,
        sizeof (NewPe32BaseAddress)
        );
      break;
    }
  }
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
RebaseImage (
  IN CONST CHAR8  *FileName,
  IN OUT UINT8    *FileBuffer,
  IN UINT32       FileSize,
  IN UINT64       NewPe32BaseAddress
  )
{
  EFI_STATUS                    Status;
  PE_COFF_LOADER_IMAGE_CONTEXT  ImageContext;
  REBASE_IMAGE_FILE             ReadContext;
  EFI_IMAGE_FILE_HEADER         FileHeader;
  EFI_IMAGE_SECTION_HEADER      SectionHeader;
  UINTN                         OptionalHeaderOffset;
  UINTN                         SectionHeadersOffset;
  UINT16                        OptionalMagic;
  UINT8                         *MemoryAllocation;
  UINTN                         AllocationSize;
  UINTN                         Index;
  UINTN                         CurrentOffset;
  UINTN                         ImageAddress;

  if (FileName == NULL || FileBuffer == NULL || FileSize == 0) {
    return EFI_INVALID_PARAMETER;
  }
  ReadContext.Buffer = FileBuffer;
  ReadContext.Size = FileSize;
  memset (&ImageContext, 0, sizeof (ImageContext));
  ImageContext.Handle = &ReadContext;
  ImageContext.ImageRead = RebaseImageRead;
  Status = PeCoffLoaderGetImageInfo (&ImageContext);
  if (EFI_ERROR (Status)) {
    Error (NULL, 0, 3000, "Invalid", "The input PeImage %s is not valid", FileName);
    return Status;
  }
  if (ImageContext.RelocationsStripped) {
    Error (NULL, 0, 3000, "Invalid", "The input PeImage %s has no relocation to be fixed up", FileName);
    return EFI_UNSUPPORTED;
  }
  if (ImageContext.SectionAlignment == 0 ||
      (ImageContext.SectionAlignment & (ImageContext.SectionAlignment - 1)) != 0 ||
      ImageContext.ImageSize > (UINT64)SIZE_MAX ||
      ImageContext.SectionAlignment > SIZE_MAX - (UINTN)ImageContext.ImageSize) {
    return EFI_VOLUME_CORRUPTED;
  }
  AllocationSize = (UINTN)ImageContext.ImageSize + ImageContext.SectionAlignment;
  MemoryAllocation = malloc (AllocationSize);
  if (MemoryAllocation == NULL) {
    Error (NULL, 0, 4001, "Resource", "memory cannot be allocated on rebase of %s", FileName);
    return EFI_OUT_OF_RESOURCES;
  }
  memset (MemoryAllocation, 0, AllocationSize);
  ImageAddress = ((UINTN)MemoryAllocation + ImageContext.SectionAlignment - 1) &
                 ~((UINTN)ImageContext.SectionAlignment - 1);
  ImageContext.ImageAddress = ImageAddress;
  Status = PeCoffLoaderLoadImage (&ImageContext);
  if (EFI_ERROR (Status)) {
    free (MemoryAllocation);
    Error (NULL, 0, 3000, "Invalid", "LoadImage() call failed on rebase of %s", FileName);
    return Status;
  }
  ImageContext.DestinationAddress = NewPe32BaseAddress;
  Status = PeCoffLoaderRelocateImage (&ImageContext);
  if (EFI_ERROR (Status)) {
    free (MemoryAllocation);
    Error (NULL, 0, 3000, "Invalid", "RelocateImage() call failed on rebase of %s", FileName);
    return Status;
  }

  Status = GetPeHeaderLayout (
             FileBuffer,
             FileSize,
             ImageContext.PeCoffHeaderOffset,
             &FileHeader,
             &OptionalHeaderOffset,
             &SectionHeadersOffset,
             &OptionalMagic
             );
  if (EFI_ERROR (Status)) {
    free (MemoryAllocation);
    return Status;
  }
  for (Index = 0; Index < FileHeader.NumberOfSections; Index++) {
    CurrentOffset = SectionHeadersOffset + Index * sizeof (SectionHeader);
    memcpy (&SectionHeader, FileBuffer + CurrentOffset, sizeof (SectionHeader));
    if (SectionHeader.PointerToRawData > FileSize ||
        SectionHeader.SizeOfRawData > FileSize - SectionHeader.PointerToRawData ||
        SectionHeader.VirtualAddress > ImageContext.ImageSize ||
        SectionHeader.SizeOfRawData > ImageContext.ImageSize - SectionHeader.VirtualAddress) {
      free (MemoryAllocation);
      return EFI_VOLUME_CORRUPTED;
    }
    if (SectionHeader.SizeOfRawData != 0) {
      memcpy (
        FileBuffer + SectionHeader.PointerToRawData,
        (VOID *)(UINTN)(ImageContext.ImageAddress + SectionHeader.VirtualAddress),
        SectionHeader.SizeOfRawData
        );
    }
  }

  if (OptionalMagic == EFI_IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
    UINT32 Base32;
    if (FileHeader.SizeOfOptionalHeader <
        offsetof (EFI_IMAGE_OPTIONAL_HEADER32, ImageBase) + sizeof (Base32) ||
        NewPe32BaseAddress > 0xffffffffULL) {
      free (MemoryAllocation);
      return EFI_VOLUME_CORRUPTED;
    }
    Base32 = (UINT32)NewPe32BaseAddress;
    memcpy (
      FileBuffer + OptionalHeaderOffset + offsetof (EFI_IMAGE_OPTIONAL_HEADER32, ImageBase),
      &Base32,
      sizeof (Base32)
      );
  } else if (OptionalMagic == EFI_IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
    if (FileHeader.SizeOfOptionalHeader <
        offsetof (EFI_IMAGE_OPTIONAL_HEADER64, ImageBase) + sizeof (NewPe32BaseAddress)) {
      free (MemoryAllocation);
      return EFI_VOLUME_CORRUPTED;
    }
    memcpy (
      FileBuffer + OptionalHeaderOffset + offsetof (EFI_IMAGE_OPTIONAL_HEADER64, ImageBase),
      &NewPe32BaseAddress,
      sizeof (NewPe32BaseAddress)
      );
  } else {
    free (MemoryAllocation);
    Error (NULL, 0, 3000, "Invalid", "unknown PE magic signature %X in PE32 image %s", OptionalMagic, FileName);
    return EFI_ABORTED;
  }
  free (MemoryAllocation);
  return SetAddressToSectionHeader (FileName, FileBuffer, FileSize, NewPe32BaseAddress);
}

STATIC
EFI_STATUS
CombinePath (
  IN  CONST CHAR8  *DefaultPath,
  IN  CONST CHAR8  *AppendPath,
  OUT CHAR8        *NewPath,
  IN  size_t       NewPathSize
  )
{
  size_t DefaultLength;
  size_t AppendLength;
  size_t Index;
  BOOLEAN NeedSeparator;

  if (DefaultPath == NULL || AppendPath == NULL || NewPath == NULL || NewPathSize == 0) {
    return EFI_INVALID_PARAMETER;
  }
  DefaultLength = strlen (DefaultPath);
  AppendLength = strlen (AppendPath);
  NeedSeparator =
    (DefaultLength != 0 && DefaultPath[DefaultLength - 1] != '/' &&
     DefaultPath[DefaultLength - 1] != '\\') ? TRUE : FALSE;
  if (DefaultLength > SIZE_MAX - AppendLength - (NeedSeparator ? 2U : 1U) ||
      DefaultLength + AppendLength + (NeedSeparator ? 2U : 1U) > NewPathSize) {
    return EFI_BAD_BUFFER_SIZE;
  }
  memcpy (NewPath, DefaultPath, DefaultLength);
  for (Index = 0; Index < DefaultLength; Index++) {
    if (NewPath[Index] == '\\') {
      NewPath[Index] = '/';
    }
  }
  if (NeedSeparator) {
    NewPath[DefaultLength++] = '/';
  }
  memcpy (NewPath + DefaultLength, AppendPath, AppendLength);
  NewPath[DefaultLength + AppendLength] = '\0';
  return EFI_SUCCESS;
}

STATIC
BOOLEAN
AllBytesAreValue (
  IN CONST UINT8  *Buffer,
  IN UINT32       Length,
  IN UINT8        Value
  )
{
  UINT32 Index;
  for (Index = 0; Index < Length; Index++) {
    if (Buffer[Index] != Value) {
      return FALSE;
    }
  }
  return TRUE;
}

STATIC
EFI_STATUS
ParseSectionInternal (
  IN UINT8   *SectionBuffer,
  IN UINT32  BufferLength,
  IN UINT32  Depth
  )
{
  UINT32 ParsedLength;

  if ((SectionBuffer == NULL && BufferLength != 0) || Depth > MAX_SECTION_NESTING) {
    return Depth > MAX_SECTION_NESTING ? EFI_SECTION_ERROR : EFI_INVALID_PARAMETER;
  }
  ParsedLength = 0;
  while (ParsedLength < BufferLength) {
    UINT8 *Ptr;
    UINT32 Remaining;
    UINT32 InitialLength;
    UINT32 SectionLength;
    UINT32 SectionHeaderLen;
    UINT32 OccupiedLength;
    EFI_SECTION_TYPE Type;
    CHAR8 *SectionName;
    EFI_STATUS Status;

    Ptr = SectionBuffer + ParsedLength;
    Remaining = BufferLength - ParsedLength;
    if (Remaining < sizeof (EFI_COMMON_SECTION_HEADER)) {
      if (AllBytesAreValue (Ptr, Remaining, 0xff)) {
        break;
      }
      Error (NULL, 0, 0003, "truncated section header", NULL);
      return EFI_SECTION_ERROR;
    }

    InitialLength = (UINT32)Ptr[0] | ((UINT32)Ptr[1] << 8) | ((UINT32)Ptr[2] << 16);
    Type = Ptr[3];
    if (InitialLength == 0xffffff && Type == 0xff) {
      ParsedLength += sizeof (EFI_COMMON_SECTION_HEADER);
      continue;
    }

    if (InitialLength == 0xffffff) {
      SectionHeaderLen = sizeof (EFI_COMMON_SECTION_HEADER2);
      if (Remaining < SectionHeaderLen) {
        Error (NULL, 0, 0003, "truncated extended section header", NULL);
        return EFI_SECTION_ERROR;
      }
      memcpy (&SectionLength, Ptr + sizeof (EFI_COMMON_SECTION_HEADER), sizeof (SectionLength));
    } else {
      SectionHeaderLen = sizeof (EFI_COMMON_SECTION_HEADER);
      SectionLength = InitialLength;
    }
    if (SectionLength < SectionHeaderLen || SectionLength > Remaining) {
      Error (NULL, 0, 0003, "invalid section length", "length 0x%X exceeds remaining buffer 0x%X", SectionLength, Remaining);
      return EFI_SECTION_ERROR;
    }

    SectionName = SectionNameToStr (Type);
    if (SectionName != NULL) {
      printf ("------------------------------------------------------------\n");
      printf ("  Type:  %s\n  Size:  0x%08X\n", SectionName, (unsigned)SectionLength);
      free (SectionName);
    }

    switch (Type) {
    case EFI_SECTION_RAW:
    case EFI_SECTION_PIC:
    case EFI_SECTION_TE:
    case EFI_SECTION_COMPATIBILITY16:
      break;

    case EFI_SECTION_PE32:
      if (EnableHash) {
        UINT32 PayloadLength;
        UINT8 *PayloadCopy;
        CHAR8 *InputPath;
        CHAR8 *OutputPath;
        CHAR8 *HashBuffer;
        UINT32 HashLength;
        CHAR8 *HashText;
        CHAR8 *HashValue;
        CHAR8 *Arguments[6];
        UINT32 Index;

        PayloadLength = SectionLength - SectionHeaderLen;
        if (PayloadLength == 0 || OpenSslPath == NULL) {
          Error (NULL, 0, 0003, "invalid PE32 section for hashing", NULL);
          return EFI_SECTION_ERROR;
        }
        PayloadCopy = malloc (PayloadLength);
        if (PayloadCopy == NULL) {
          return EFI_OUT_OF_RESOURCES;
        }
        memcpy (PayloadCopy, Ptr + SectionHeaderLen, PayloadLength);
        Status = RebaseImage ("PE32 section", PayloadCopy, PayloadLength, 0);
        if (EFI_ERROR (Status) && Status != EFI_UNSUPPORTED) {
          free (PayloadCopy);
          return EFI_SECTION_ERROR;
        }
        InputPath = NULL;
        OutputPath = NULL;
        Status = CreateTemporaryFilePath (&InputPath);
        if (!EFI_ERROR (Status)) {
          Status = CreateTemporaryFilePath (&OutputPath);
        }
        if (EFI_ERROR (Status)) {
          if (InputPath != NULL) { remove (InputPath); free (InputPath); }
          free (PayloadCopy);
          return Status;
        }
        Status = PutFileImage (InputPath, (CHAR8 *)PayloadCopy, PayloadLength);
        free (PayloadCopy);
        if (EFI_ERROR (Status)) {
          remove (InputPath); remove (OutputPath);
          free (InputPath); free (OutputPath);
          return EFI_SECTION_ERROR;
        }
        Arguments[0] = OpenSslPath;
        Arguments[1] = "sha1";
        Arguments[2] = "-out";
        Arguments[3] = OutputPath;
        Arguments[4] = InputPath;
        Arguments[5] = NULL;
        if (RunExternalTool (OpenSslPath, Arguments) != 0) {
          remove (InputPath); remove (OutputPath);
          free (InputPath); free (OutputPath);
          Error (NULL, 0, 3000, "Open SSL command failed", NULL);
          return EFI_SECTION_ERROR;
        }
        HashBuffer = NULL;
        HashLength = 0;
        Status = GetFileImage (OutputPath, &HashBuffer, &HashLength);
        remove (InputPath); remove (OutputPath);
        free (InputPath); free (OutputPath);
        if (EFI_ERROR (Status) || HashLength == 0 || HashLength == UINT32_MAX) {
          free (HashBuffer);
          return EFI_SECTION_ERROR;
        }
        HashText = malloc ((size_t)HashLength + 1U);
        if (HashText == NULL) {
          free (HashBuffer);
          return EFI_OUT_OF_RESOURCES;
        }
        memcpy (HashText, HashBuffer, HashLength);
        HashText[HashLength] = '\0';
        free (HashBuffer);
        HashValue = strrchr (HashText, '=');
        if (HashValue == NULL) {
          free (HashText);
          Error (NULL, 0, 0003, "Unexpected hash output format", NULL);
          return EFI_SECTION_ERROR;
        }
        HashValue++;
        while (*HashValue == ' ' || *HashValue == '\t') { HashValue++; }
        for (Index = (UINT32)strlen (HashValue); Index > 0; Index--) {
          if (HashValue[Index - 1] != '\r' && HashValue[Index - 1] != '\n' &&
              HashValue[Index - 1] != ' ' && HashValue[Index - 1] != '\t') {
            break;
          }
          HashValue[Index - 1] = '\0';
        }
        printf ("  SHA1: %s\n", HashValue);
        free (HashText);
      }
      break;

    case EFI_SECTION_USER_INTERFACE:
      {
        UINT32 PayloadLength;
        UINTN CharacterLength;
        CHAR8 *UiText;

        PayloadLength = SectionLength - SectionHeaderLen;
        Status = BoundedUnicodeLength ((CONST CHAR16 *)(CONST VOID *)(Ptr + SectionHeaderLen), PayloadLength, &CharacterLength);
        if (EFI_ERROR (Status)) {
          Error (NULL, 0, 0003, "unterminated user-interface string", NULL);
          return EFI_SECTION_ERROR;
        }
        UiText = malloc (CharacterLength + 1U);
        if (UiText == NULL) {
          return EFI_OUT_OF_RESOURCES;
        }
        Unicode2AsciiStringN ((CONST CHAR16 *)(CONST VOID *)(Ptr + SectionHeaderLen), CharacterLength, UiText);
        printf ("  String: %s\n", UiText);
        free (UiText);
      }
      break;

    case EFI_SECTION_FIRMWARE_VOLUME_IMAGE:
      Status = PrintFvInfo (Ptr + SectionHeaderLen, SectionLength - SectionHeaderLen, TRUE);
      if (EFI_ERROR (Status)) {
        Error (NULL, 0, 0003, "printing of FV section contents failed", NULL);
        return EFI_SECTION_ERROR;
      }
      break;

    case EFI_SECTION_FREEFORM_SUBTYPE_GUID:
      if (SectionLength < SectionHeaderLen + sizeof (EFI_GUID)) {
        return EFI_SECTION_ERROR;
      } else {
        EFI_GUID SubTypeGuid;
        memcpy (&SubTypeGuid, Ptr + SectionHeaderLen, sizeof (SubTypeGuid));
        printf ("  SubTypeGuid: ");
        PrintGuid (&SubTypeGuid);
        printf ("\n");
      }
      break;

    case EFI_SECTION_PEI_DEPEX:
    case EFI_SECTION_DXE_DEPEX:
    case EFI_SECTION_SMM_DEPEX:
      Status = DumpDepexSection (Ptr, SectionLength);
      if (EFI_ERROR (Status)) {
        return Status;
      }
      break;

    case EFI_SECTION_VERSION:
      {
        UINT16 BuildNumber;
        UINT32 StringBytes;
        UINTN CharacterLength;
        CHAR8 *VersionText;

        if (SectionLength < SectionHeaderLen + sizeof (BuildNumber)) {
          return EFI_SECTION_ERROR;
        }
        memcpy (&BuildNumber, Ptr + SectionHeaderLen, sizeof (BuildNumber));
        printf ("  Build Number:  0x%02X\n", BuildNumber);
        StringBytes = SectionLength - SectionHeaderLen - sizeof (BuildNumber);
        if (StringBytes == 0) {
          printf ("  Version Strg:  \n");
          break;
        }
        Status = BoundedUnicodeLength (
                   (CONST CHAR16 *)(CONST VOID *)(Ptr + SectionHeaderLen + sizeof (BuildNumber)),
                   StringBytes,
                   &CharacterLength
                   );
        if (EFI_ERROR (Status)) {
          Error (NULL, 0, 0003, "unterminated version string", NULL);
          return EFI_SECTION_ERROR;
        }
        VersionText = malloc (CharacterLength + 1U);
        if (VersionText == NULL) {
          return EFI_OUT_OF_RESOURCES;
        }
        Unicode2AsciiStringN (
          (CONST CHAR16 *)(CONST VOID *)(Ptr + SectionHeaderLen + sizeof (BuildNumber)),
          CharacterLength,
          VersionText
          );
        printf ("  Version Strg:  %s\n", VersionText);
        free (VersionText);
      }
      break;

    case EFI_SECTION_COMPRESSION:
      {
        UINT32 RealHeaderLength;
        UINT32 UncompressedLength;
        UINT8 CompressionType;
        UINT32 CompressedLength;
        UINT8 *UncompressedBuffer;
        BOOLEAN AllocatedOutput;

        RealHeaderLength = SectionHeaderLen + sizeof (UINT32) + sizeof (UINT8);
        if (SectionLength < RealHeaderLength) {
          return EFI_SECTION_ERROR;
        }
        memcpy (&UncompressedLength, Ptr + SectionHeaderLen, sizeof (UncompressedLength));
        CompressionType = Ptr[SectionHeaderLen + sizeof (UINT32)];
        CompressedLength = SectionLength - RealHeaderLength;
        printf ("  Uncompressed Length:  0x%08X\n", (unsigned)UncompressedLength);
        UncompressedBuffer = NULL;
        AllocatedOutput = FALSE;
        if (CompressionType == EFI_NOT_COMPRESSED) {
          printf ("  Compression Type:  EFI_NOT_COMPRESSED\n");
          if (CompressedLength != UncompressedLength) {
            Error (NULL, 0, 0003, "uncompressed section length mismatch", NULL);
            return EFI_SECTION_ERROR;
          }
          UncompressedBuffer = Ptr + RealHeaderLength;
        } else if (CompressionType == EFI_STANDARD_COMPRESSION) {
          UINT32 DestinationSize;
          UINT32 ScratchSize;
          UINT8 *ScratchBuffer;

          printf ("  Compression Type:  EFI_STANDARD_COMPRESSION\n");
          Status = EfiGetInfo (Ptr + RealHeaderLength, CompressedLength, &DestinationSize, &ScratchSize);
          if (EFI_ERROR (Status) || DestinationSize != UncompressedLength) {
            return EFI_SECTION_ERROR;
          }
          ScratchBuffer = ScratchSize == 0 ? NULL : malloc (ScratchSize);
          if (ScratchSize != 0 && ScratchBuffer == NULL) {
            return EFI_OUT_OF_RESOURCES;
          }
          UncompressedBuffer = UncompressedLength == 0 ? NULL : malloc (UncompressedLength);
          if (UncompressedLength != 0 && UncompressedBuffer == NULL) {
            free (ScratchBuffer);
            return EFI_OUT_OF_RESOURCES;
          }
          Status = EfiDecompress (
                     Ptr + RealHeaderLength,
                     CompressedLength,
                     UncompressedBuffer,
                     UncompressedLength,
                     ScratchBuffer,
                     ScratchSize
                     );
          free (ScratchBuffer);
          if (EFI_ERROR (Status)) {
            free (UncompressedBuffer);
            return EFI_SECTION_ERROR;
          }
          AllocatedOutput = TRUE;
        } else {
          Error (NULL, 0, 0003, "unrecognized compression type", "type 0x%X", CompressionType);
          return EFI_SECTION_ERROR;
        }
        Status = ParseSectionInternal (UncompressedBuffer, UncompressedLength, Depth + 1U);
        if (AllocatedOutput) {
          free (UncompressedBuffer);
        }
        if (EFI_ERROR (Status)) {
          return EFI_SECTION_ERROR;
        }
      }
      break;

    case EFI_SECTION_GUID_DEFINED:
      {
        UINT32 RealHeaderLength;
        EFI_GUID SectionGuid;
        UINT16 DataOffset;
        UINT16 Attributes;
        CHAR8 *ExtractionTool;

        RealHeaderLength = SectionHeaderLen + sizeof (EFI_GUID) + sizeof (UINT16) + sizeof (UINT16);
        if (SectionLength < RealHeaderLength) {
          return EFI_SECTION_ERROR;
        }
        memcpy (&SectionGuid, Ptr + SectionHeaderLen, sizeof (SectionGuid));
        memcpy (&DataOffset, Ptr + SectionHeaderLen + sizeof (EFI_GUID), sizeof (DataOffset));
        memcpy (&Attributes, Ptr + SectionHeaderLen + sizeof (EFI_GUID) + sizeof (UINT16), sizeof (Attributes));
        printf ("  SectionDefinitionGuid:  ");
        PrintGuid (&SectionGuid);
        printf ("\n  DataOffset:             0x%04X\n", (unsigned)DataOffset);
        printf ("  Attributes:             0x%04X\n", (unsigned)Attributes);
        if (DataOffset < RealHeaderLength || DataOffset > SectionLength) {
          Error (NULL, 0, 0003, "invalid GUID-defined section data offset", NULL);
          return EFI_SECTION_ERROR;
        }

        ExtractionTool = LookupGuidedSectionToolPath (mParsedGuidedSectionTools, &SectionGuid);
        if (ExtractionTool != NULL) {
          CHAR8 *InputPath;
          CHAR8 *OutputPath;
          UINT8 *OutputBuffer;
          UINT32 OutputLength;
          CHAR8 *Arguments[6];

          InputPath = NULL;
          OutputPath = NULL;
          Status = CreateTemporaryFilePath (&InputPath);
          if (!EFI_ERROR (Status)) {
            Status = CreateTemporaryFilePath (&OutputPath);
          }
          if (EFI_ERROR (Status)) {
            if (InputPath != NULL) { remove (InputPath); free (InputPath); }
            free (ExtractionTool);
            return Status;
          }
          Status = PutFileImage (InputPath, (CHAR8 *)Ptr + DataOffset, SectionLength - DataOffset);
          if (EFI_ERROR (Status)) {
            remove (InputPath); remove (OutputPath);
            free (InputPath); free (OutputPath); free (ExtractionTool);
            return EFI_SECTION_ERROR;
          }
          Arguments[0] = ExtractionTool;
          Arguments[1] = "-d";
          Arguments[2] = "-o";
          Arguments[3] = OutputPath;
          Arguments[4] = InputPath;
          Arguments[5] = NULL;
          if (RunExternalTool (ExtractionTool, Arguments) != 0) {
            remove (InputPath); remove (OutputPath);
            free (InputPath); free (OutputPath); free (ExtractionTool);
            Error (NULL, 0, 3000, "guided section extraction tool failed", NULL);
            return EFI_SECTION_ERROR;
          }
          free (ExtractionTool);
          OutputBuffer = NULL;
          OutputLength = 0;
          Status = GetFileImage (OutputPath, (CHAR8 **)&OutputBuffer, &OutputLength);
          remove (InputPath); remove (OutputPath);
          free (InputPath); free (OutputPath);
          if (EFI_ERROR (Status)) {
            free (OutputBuffer);
            return EFI_SECTION_ERROR;
          }
          Status = ParseSectionInternal (OutputBuffer, OutputLength, Depth + 1U);
          free (OutputBuffer);
          if (EFI_ERROR (Status)) {
            return EFI_SECTION_ERROR;
          }
        } else if (!CompareGuid (&SectionGuid, &gEfiCrc32GuidedSectionExtractionProtocolGuid)) {
          Status = ParseSectionInternal (Ptr + DataOffset, SectionLength - DataOffset, Depth + 1U);
          if (EFI_ERROR (Status)) {
            Error (NULL, 0, 0003, "parse of CRC32 GUIDED section failed", NULL);
            return EFI_SECTION_ERROR;
          }
        } else {
          Error (NULL, 0, 0003, "Error parsing section", "EFI_SECTION_GUID_DEFINED requires a matching extraction tool");
          return EFI_UNSUPPORTED;
        }
      }
      break;

    default:
      Error (NULL, 0, 0003, "unrecognized section type found", "section type = 0x%X", Type);
      return EFI_SECTION_ERROR;
    }

    if (SectionLength == Remaining) {
      break;
    }
    if (SectionLength > UINT32_MAX - 3U) {
      return EFI_SECTION_ERROR;
    }
    OccupiedLength = (SectionLength + 3U) & ~3U;
    if (OccupiedLength > Remaining) {
      Error (NULL, 0, 0003, "section alignment exceeds containing buffer", NULL);
      return EFI_SECTION_ERROR;
    }
    ParsedLength += OccupiedLength;
  }
  return EFI_SUCCESS;
}

EFI_STATUS
ParseSection (
  IN UINT8  *SectionBuffer,
  IN UINT32 BufferLength
  )
{
  return ParseSectionInternal (SectionBuffer, BufferLength, 0);
}

EFI_STATUS
DumpDepexSection (
  IN UINT8    *Ptr,
  IN UINT32   SectionLength
  )
{
  UINT32 HeaderLength;
  UINT8 GuidBuffer[PRINTED_GUID_BUFFER_SIZE];

  if (Ptr == NULL || SectionLength < sizeof (EFI_COMMON_SECTION_HEADER)) {
    return EFI_SECTION_ERROR;
  }
  HeaderLength = (Ptr[0] == 0xff && Ptr[1] == 0xff && Ptr[2] == 0xff) ?
                 sizeof (EFI_COMMON_SECTION_HEADER2) : sizeof (EFI_COMMON_SECTION_HEADER);
  if (SectionLength < HeaderLength) {
    return EFI_SECTION_ERROR;
  }
  Ptr += HeaderLength;
  SectionLength -= HeaderLength;
  while (SectionLength > 0) {
    UINT8 Opcode;
    Opcode = Ptr[0];
    printf ("        ");
    if (Opcode == EFI_DEP_BEFORE || Opcode == EFI_DEP_AFTER || Opcode == EFI_DEP_PUSH) {
      EFI_GUID Guid;
      if (SectionLength < 1U + sizeof (EFI_GUID)) {
        Error (NULL, 0, 0003, "truncated dependency expression GUID operand", NULL);
        return EFI_SECTION_ERROR;
      }
      memcpy (&Guid, Ptr + 1, sizeof (Guid));
      if (Opcode == EFI_DEP_BEFORE) {
        printf ("BEFORE\n        ");
      } else if (Opcode == EFI_DEP_AFTER) {
        printf ("AFTER\n        ");
      } else {
        printf ("PUSH\n        ");
      }
      PrintGuidToBuffer (&Guid, GuidBuffer, sizeof (GuidBuffer), TRUE);
      printf ("%s  ", GuidBuffer);
      PrintGuidName (GuidBuffer);
      printf ("\n");
      Ptr += 1U + sizeof (EFI_GUID);
      SectionLength -= 1U + sizeof (EFI_GUID);
      continue;
    }
    switch (Opcode) {
    case EFI_DEP_AND:   printf ("AND\n"); break;
    case EFI_DEP_OR:    printf ("OR\n"); break;
    case EFI_DEP_NOT:   printf ("NOT\n"); break;
    case EFI_DEP_TRUE:  printf ("TRUE\n"); break;
    case EFI_DEP_FALSE: printf ("FALSE\n"); break;
    case EFI_DEP_END:   printf ("END DEPEX\n"); break;
    case EFI_DEP_SOR:   printf ("SOR\n"); break;
    default:
      Error (NULL, 0, 0003, "unrecognized byte in dependency expression", "opcode 0x%X", Opcode);
      return EFI_SECTION_ERROR;
    }
    Ptr++;
    SectionLength--;
  }
  return EFI_SUCCESS;
}

EFI_STATUS
PrintGuidName (
  IN UINT8    *GuidStr
  )
/*++

Routine Description:

  GC_TODO: Add function description

Arguments:

  GuidStr - GC_TODO: add argument description

Returns:

  EFI_SUCCESS - GC_TODO: Add description for return value
  EFI_INVALID_PARAMETER - GC_TODO: Add description for return value

--*/
{
  GUID_TO_BASENAME  *GPtr;
  //
  // If we have a list of guid-to-basenames, then go through the list to
  // look for a guid string match. If found, print the basename to stdout,
  // otherwise return a failure.
  //
  GPtr = mGuidBaseNameList;
  while (GPtr != NULL) {
    if (_stricmp ((CHAR8*) GuidStr, (CHAR8*) GPtr->Guid) == 0) {
      printf ("%s", GPtr->BaseName);
      return EFI_SUCCESS;
    }

    GPtr = GPtr->Next;
  }

  return EFI_INVALID_PARAMETER;
}

EFI_STATUS
ParseGuidBaseNameFile (
  CHAR8    *FileName
  )
/*++

Routine Description:

  GC_TODO: Add function description

Arguments:

  FileName  - GC_TODO: add argument description

Returns:

  EFI_DEVICE_ERROR - GC_TODO: Add description for return value
  EFI_OUT_OF_RESOURCES - GC_TODO: Add description for return value
  EFI_SUCCESS - GC_TODO: Add description for return value

--*/
{
  FILE              *Fptr;
  CHAR8             Line[MAX_LINE_LEN];
  GUID_TO_BASENAME  *GPtr;

  if ((Fptr = fopen (LongFilePath (FileName), "r")) == NULL) {
    printf ("ERROR: Failed to open input cross-reference file '%s'\n", FileName);
    return EFI_DEVICE_ERROR;
  }

  while (fgets (Line, sizeof (Line), Fptr) != NULL) {
    //
    // Allocate space for another guid/basename element
    //
    GPtr = malloc (sizeof (GUID_TO_BASENAME));
    if (GPtr == NULL) {
      fclose (Fptr);
      return EFI_OUT_OF_RESOURCES;
    }

    memset ((char *) GPtr, 0, sizeof (GUID_TO_BASENAME));
    if (sscanf (Line, "%36s %59s", GPtr->Guid, GPtr->BaseName) == 2) {
      GPtr->Next        = mGuidBaseNameList;
      mGuidBaseNameList = GPtr;
    } else {
      //
      // Some sort of error. Just continue.
      //
      free (GPtr);
    }
  }

  fclose (Fptr);
  return EFI_SUCCESS;
}

EFI_STATUS
FreeGuidBaseNameList (
  VOID
  )
/*++

Routine Description:

  GC_TODO: Add function description

Arguments:

  None

Returns:

  EFI_SUCCESS - GC_TODO: Add description for return value

--*/
{
  GUID_TO_BASENAME  *Next;

  while (mGuidBaseNameList != NULL) {
    Next = mGuidBaseNameList->Next;
    free (mGuidBaseNameList);
    mGuidBaseNameList = Next;
  }

  return EFI_SUCCESS;
}


static
VOID
LoadGuidedSectionToolsTxt (
  IN CHAR8* FirmwareVolumeFilename
  )
{
  CHAR8* PeerFilename;
  CHAR8* Places[] = {
    NULL,
    //NULL,
    };
  UINTN Index;

  Places[0] = FirmwareVolumeFilename;
  //Places[1] = mUtilityFilename;

  mParsedGuidedSectionTools = NULL;

  for (Index = 0; Index < (sizeof(Places)/sizeof(Places[0])); Index++) {
    PeerFilename = OsPathPeerFilePath (Places[Index], "GuidedSectionTools.txt");
    //printf("Loading %s...\n", PeerFilename);
    if (OsPathExists (PeerFilename)) {
      mParsedGuidedSectionTools = ParseGuidedSectionToolsFile (PeerFilename);
    }
    free (PeerFilename);
    if (mParsedGuidedSectionTools != NULL) {
      return;
    }
  }
}


void
Usage (
  VOID
  )
/*++

Routine Description:

  GC_TODO: Add function description

Arguments:

  None

Returns:

  GC_TODO: add return values

--*/
{
  //
  // Summary usage
  //
  fprintf (stdout, "Usage: %s [options] <input_file>\n\n", UTILITY_NAME);

  //
  // Copyright declaration
  //
  fprintf (stdout, "Copyright (c) 2007 - 2018, Intel Corporation. All rights reserved.\n\n");
  fprintf (stdout, "  Display Tiano Firmware Volume FFS image information\n\n");

  //
  // Details Option
  //
  fprintf (stdout, "optional arguments:\n");
  fprintf (stdout, "  -h, --help\n\
            Show this help message and exit\n");
  fprintf (stdout, "  --version\n\
           Show program's version number and exit\n");
  fprintf (stdout, "  -d [DEBUG], --debug [DEBUG]\n\
            Output DEBUG statements, where DEBUG_LEVEL is 0 (min) - 9 (max)\n");
  fprintf (stdout, "  -v, --verbose\n\
            Print informational statements\n");
  fprintf (stdout, "  -q, --quiet\n\
            Returns the exit code, error messages will be displayed\n");
  fprintf (stdout, "  -s, --silent\n\
            Returns only the exit code; informational and error\n\
            messages are not displayed\n");
  fprintf (stdout, "  -x XREF_FILENAME, --xref XREF_FILENAME\n\
            Parse the basename to file-guid cross reference file(s)\n");
  fprintf (stdout, "  -f OFFSET, --offset OFFSET\n\
            The offset from the start of the input file to start \n\
            processing an FV\n");
  fprintf (stdout, "  --hash\n\
            Generate HASH value of the entire PE image\n");
  fprintf (stdout, "  --sfo\n\
            Reserved for future use\n");
}


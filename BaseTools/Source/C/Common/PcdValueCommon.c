/** @file
This file contains the PcdValue structure definition.

Copyright (c) 2017 - 2018, Intel Corporation. All rights reserved.<BR>

SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "CommonLib.h"
#include "PcdValueCommon.h"

typedef enum {
  PcdDataTypeBoolean,
  PcdDataTypeUint8,
  PcdDataTypeUint16,
  PcdDataTypeUint32,
  PcdDataTypeUint64,
  PcdDataTypePointer
} PCD_DATA_TYPE;

typedef struct {
  CHAR8          *SkuName;
  CHAR8          *DefaultValueName;
  CHAR8          *TokenSpaceGuidName;
  CHAR8          *TokenName;
  CHAR8          *DataType;
  CHAR8          *Value;
  PCD_DATA_TYPE  PcdDataType;
} PCD_ENTRY;

PCD_ENTRY  *PcdList;
UINT32     PcdListLength;

STATIC
VOID
RecordToken (
  UINT8   *FileBuffer,
  UINT32  PcdIndex,
  UINT32  TokenIndex,
  UINT32  TokenStart,
  UINT32  TokenEnd
  )
/*++

Routine Description:

  Record new token information

Arguments:

  FileBuffer    File Buffer to be record
  PcdIndex      Index of PCD in database
  TokenIndex    Index of Token
  TokenStart    Start of Token
  TokenEnd      End of Token

Returns:

  None
--*/
{
  CHAR8   *Token;
  size_t  TokenLength;

  if (TokenEnd < TokenStart) {
    fprintf (stderr, "Invalid token range in PCD database\n");
    exit (EXIT_FAILURE);
  }

  TokenLength = (size_t)TokenEnd - (size_t)TokenStart;
  if (TokenLength == SIZE_MAX) {
    fprintf (stderr, "PCD token length exceeds host limits\n");
    exit (EXIT_FAILURE);
  }

  Token = malloc (TokenLength + 1U);
  if (Token == NULL) {
    fprintf (stderr, "Can not allocate token buffer for PCD database\n");
    exit (EXIT_FAILURE);
  }

  memcpy (Token, &FileBuffer[TokenStart], TokenLength);
  Token[TokenLength] = 0;
  switch (TokenIndex) {
  case 0:
    PcdList[PcdIndex].SkuName = Token;
    break;
  case 1:
    PcdList[PcdIndex].DefaultValueName = Token;
    break;
  case 2:
    PcdList[PcdIndex].TokenSpaceGuidName = Token;
    break;
  case 3:
    PcdList[PcdIndex].TokenName = Token;
    break;
  case 4:
    PcdList[PcdIndex].DataType = Token;
    if (strcmp (Token, "BOOLEAN") == 0) {
      PcdList[PcdIndex].PcdDataType = PcdDataTypeBoolean;
    } else if (strcmp (Token, "UINT8") == 0) {
      PcdList[PcdIndex].PcdDataType = PcdDataTypeUint8;
    } else if (strcmp (Token, "UINT16") == 0) {
      PcdList[PcdIndex].PcdDataType = PcdDataTypeUint16;
    } else if (strcmp (Token, "UINT32") == 0) {
      PcdList[PcdIndex].PcdDataType = PcdDataTypeUint32;
    } else if (strcmp (Token, "UINT64") == 0) {
      PcdList[PcdIndex].PcdDataType = PcdDataTypeUint64;
    } else {
      PcdList[PcdIndex].PcdDataType = PcdDataTypePointer;
    }
    break;
  case 5:
    PcdList[PcdIndex].Value = Token;
    break;
  default:
    free (Token);
    break;
  }
}

STATIC
int
LookupPcdIndex (
  CHAR8  *SkuName             OPTIONAL,
  CHAR8  *DefaultValueName    OPTIONAL,
  CHAR8  *TokenSpaceGuidName,
  CHAR8  *TokenName
  )
/*++

Routine Description:

  Get PCD index in Pcd database

Arguments:

  SkuName               SkuName String
  DefaultValueName      DefaultValueName String
  TokenSpaceGuidName    TokenSpaceGuidName String
  TokenName             TokenName String

Returns:

  Index of PCD in Pcd database
--*/
{
  UINT32  Index;

  if (SkuName == NULL) {
    SkuName = "DEFAULT";
  }
  if (DefaultValueName == NULL) {
    DefaultValueName = "DEFAULT";
  }
  for (Index = 0; Index < PcdListLength; Index++) {
    if (strcmp(PcdList[Index].TokenSpaceGuidName, TokenSpaceGuidName) != 0) {
      continue;
    }
    if (strcmp(PcdList[Index].TokenName, TokenName) != 0) {
      continue;
    }
    if (strcmp(PcdList[Index].SkuName, SkuName) != 0) {
      continue;
    }
    if (strcmp(PcdList[Index].DefaultValueName, DefaultValueName) != 0) {
      continue;
    }
    return Index;
  }
  return -1;
}

UINT64
__PcdGet (
  CHAR8  *SkuName             OPTIONAL,
  CHAR8  *DefaultValueName    OPTIONAL,
  CHAR8  *TokenSpaceGuidName,
  CHAR8  *TokenName
  )
/*++

Routine Description:

  Get PCD value

Arguments:

  SkuName               SkuName String
  DefaultValueName      DefaultValueName String
  TokenSpaceGuidName    TokenSpaceGuidName String
  TokenName             TokenName String

Returns:

  PCD value
--*/
{
  int    Index;
  CHAR8  *End;

  Index = LookupPcdIndex (SkuName, DefaultValueName, TokenSpaceGuidName, TokenName);
  if (Index < 0) {
    fprintf (stderr, "PCD %s.%s.%s.%s is not in database\n", SkuName, DefaultValueName, TokenSpaceGuidName, TokenName);
    exit (EXIT_FAILURE);
  }
  switch (PcdList[Index].PcdDataType) {
  case PcdDataTypeBoolean:
  case PcdDataTypeUint8:
  case PcdDataTypeUint16:
  case PcdDataTypeUint32:
    return (UINT64)strtoul(PcdList[Index].Value, &End, 16);
    break;
  case PcdDataTypeUint64:
    return (UINT64)strtoul(PcdList[Index].Value, &End, 16);
    break;
  case PcdDataTypePointer:
    fprintf (stderr, "PCD %s.%s.%s.%s is structure.  Use PcdGetPtr()\n", SkuName, DefaultValueName, TokenSpaceGuidName, TokenName);
    exit (EXIT_FAILURE);
    break;
  }
  return 0;
}

VOID
__PcdSet (
  CHAR8   *SkuName             OPTIONAL,
  CHAR8   *DefaultValueName    OPTIONAL,
  CHAR8   *TokenSpaceGuidName,
  CHAR8   *TokenName,
  UINT64  Value
  )
/*++

Routine Description:

  Set PCD value

Arguments:

  SkuName               SkuName String
  DefaultValueName      DefaultValueName String
  TokenSpaceGuidName    TokenSpaceGuidName String
  TokenName             TokenName String
  Value                 PCD value to be set

Returns:

  None
--*/
{
  int    Index;
  CHAR8  *NewValue;
  int    FormatLength;

  Index = LookupPcdIndex (SkuName, DefaultValueName, TokenSpaceGuidName, TokenName);
  if (Index < 0) {
    fprintf (stderr, "PCD %s.%s.%s.%s is not in database\n", SkuName, DefaultValueName, TokenSpaceGuidName, TokenName);
    exit (EXIT_FAILURE);
  }
  if (PcdList[Index].PcdDataType == PcdDataTypePointer) {
    fprintf (stderr, "PCD %s.%s.%s.%s is structure.  Use PcdSetPtr()\n", SkuName, DefaultValueName, TokenSpaceGuidName, TokenName);
    exit (EXIT_FAILURE);
  }

  NewValue = malloc (20);
  if (NewValue == NULL) {
    fprintf (stderr, "Can not allocate PCD value buffer\n");
    exit (EXIT_FAILURE);
  }

  switch (PcdList[Index].PcdDataType) {
  case PcdDataTypeBoolean:
    FormatLength = snprintf (NewValue, 20, "%s", Value == 0 ? "0x00" : "0x01");
    break;
  case PcdDataTypeUint8:
    FormatLength = snprintf (NewValue, 20, "0x%02x", (unsigned)(UINT8)(Value & 0xff));
    break;
  case PcdDataTypeUint16:
    FormatLength = snprintf (NewValue, 20, "0x%04x", (unsigned)(UINT16)(Value & 0xffff));
    break;
  case PcdDataTypeUint32:
    FormatLength = snprintf (NewValue, 20, "0x%08x", (unsigned)(UINT32)(Value & 0xffffffff));
    break;
  case PcdDataTypeUint64:
    FormatLength = snprintf (NewValue, 20, "0x%016llx", (unsigned long long)Value);
    break;
  case PcdDataTypePointer:
  default:
    free (NewValue);
    fprintf (stderr, "Invalid PCD data type\n");
    exit (EXIT_FAILURE);
  }

  if (FormatLength < 0 || FormatLength >= 20) {
    free (NewValue);
    fprintf (stderr, "Can not format PCD value\n");
    exit (EXIT_FAILURE);
  }

  free (PcdList[Index].Value);
  PcdList[Index].Value = NewValue;
}

VOID *
__PcdGetPtr (
  CHAR8   *SkuName             OPTIONAL,
  CHAR8   *DefaultValueName    OPTIONAL,
  CHAR8   *TokenSpaceGuidName,
  CHAR8   *TokenName,
  UINT32  *Size
  )
/*++

Routine Description:

  Get PCD value buffer

Arguments:

  SkuName               SkuName String
  DefaultValueName      DefaultValueName String
  TokenSpaceGuidName    TokenSpaceGuidName String
  TokenName             TokenName String
  Size                  Size of PCD value buffer

Returns:

  PCD value buffer
--*/
{
  int    Index;
  CHAR8   *Value;
  UINT8   *Buffer;
  CHAR8   *End;

  Index = LookupPcdIndex (SkuName, DefaultValueName, TokenSpaceGuidName, TokenName);
  if (Index < 0) {
    fprintf (stderr, "PCD %s.%s.%s.%s is not in database\n", SkuName, DefaultValueName, TokenSpaceGuidName, TokenName);
    exit (EXIT_FAILURE);
  }
  switch (PcdList[Index].PcdDataType) {
  case PcdDataTypeBoolean:
  case PcdDataTypeUint8:
  case PcdDataTypeUint16:
  case PcdDataTypeUint32:
  case PcdDataTypeUint64:
    fprintf (stderr, "PCD %s.%s.%s.%s is a value.  Use PcdGet()\n", SkuName, DefaultValueName, TokenSpaceGuidName, TokenName);
    exit (EXIT_FAILURE);
    break;
  case PcdDataTypePointer:
    Value = &PcdList[Index].Value[1];
    for (*Size = 0, strtoul(Value, &End, 16); Value != End; strtoul(Value, &End, 16), *Size = *Size + 1) {
      Value = End + 1;
    }
    Buffer = malloc(*Size + 1);
    if (Buffer == NULL) {
      *Size = 0;
      return NULL;
    }
    Value = &PcdList[Index].Value[1];
    for (*Size = 0, Buffer[*Size] = (UINT8) strtoul(Value, &End, 16); Value != End; *Size = *Size + 1, Buffer[*Size] = (UINT8) strtoul(Value, &End, 16)) {
      Value = End + 1;
    }
    return Buffer;
  }
  *Size = 0;
  return 0;
}

VOID
__PcdSetPtr (
  CHAR8   *SkuName             OPTIONAL,
  CHAR8   *DefaultValueName    OPTIONAL,
  CHAR8   *TokenSpaceGuidName,
  CHAR8   *TokenName,
  UINT32  Size,
  UINT8   *Value
  )
/*++

Routine Description:

  Set PCD value buffer

Arguments:

  SkuName               SkuName String
  DefaultValueName      DefaultValueName String
  TokenSpaceGuidName    TokenSpaceGuidName String
  TokenName             TokenName String
  Size                  Size of PCD value
  Value                 Pointer to the updated PCD value buffer

Returns:

  None
--*/
{
  int           Index;
  UINT32        ValueIndex;
  size_t        AllocationSize;
  size_t        Offset;
  CHAR8         *NewValue;
  static const CHAR8 HexDigits[] = "0123456789abcdef";

  Index = LookupPcdIndex (SkuName, DefaultValueName, TokenSpaceGuidName, TokenName);
  if (Index < 0) {
    fprintf (stderr, "PCD %s.%s.%s.%s is not in database\n", SkuName, DefaultValueName, TokenSpaceGuidName, TokenName);
    exit (EXIT_FAILURE);
  }
  switch (PcdList[Index].PcdDataType) {
  case PcdDataTypeBoolean:
  case PcdDataTypeUint8:
  case PcdDataTypeUint16:
  case PcdDataTypeUint32:
  case PcdDataTypeUint64:
    fprintf (stderr, "PCD %s.%s.%s.%s is a value.  Use PcdGet()\n", SkuName, DefaultValueName, TokenSpaceGuidName, TokenName);
    exit (EXIT_FAILURE);
  case PcdDataTypePointer:
    if (Size != 0 && Value == NULL) {
      fprintf (stderr, "PCD pointer value is NULL for a non-zero size\n");
      exit (EXIT_FAILURE);
    }
#if SIZE_MAX <= UINT32_MAX
    if (Size > (UINT32)((SIZE_MAX - 3U) / 5U)) {
      fprintf (stderr, "PCD pointer value is too large\n");
      exit (EXIT_FAILURE);
    }
#endif
    AllocationSize = Size == 0 ? 3U : (size_t)Size * 5U + 2U;
    NewValue = malloc (AllocationSize);
    if (NewValue == NULL) {
      fprintf (stderr, "Can not allocate PCD pointer value buffer\n");
      exit (EXIT_FAILURE);
    }

    NewValue[0] = '{';
    Offset = 1;
    for (ValueIndex = 0; ValueIndex < Size; ValueIndex++) {
      NewValue[Offset++] = '0';
      NewValue[Offset++] = 'x';
      NewValue[Offset++] = HexDigits[(Value[ValueIndex] >> 4) & 0x0f];
      NewValue[Offset++] = HexDigits[Value[ValueIndex] & 0x0f];
      NewValue[Offset++] = ',';
    }
    if (Size != 0) {
      NewValue[Offset - 1] = '}';
    } else {
      NewValue[Offset++] = '}';
    }
    NewValue[Offset] = '\0';

    free (PcdList[Index].Value);
    PcdList[Index].Value = NewValue;
    break;
  }
}

STATIC
VOID
ReadInputFile (
  CHAR8   *InputFileName,
  UINT8   **FileBuffer,
  UINT32  *FileSize
  )
/*++

Routine Description:

  Read the file buffer from the input file.

Arguments:

  InputFileName Point to the input file name.
  FileBuffer    Point to the input file buffer.
  FileSize      Size of the file buffer.

Returns:

  None
--*/
{
  FILE    *InputFile;
  long    FileSizeLong;
  size_t  BytesRead;

  //
  // Open Input file and read file data.
  //
  InputFile = fopen (InputFileName, "rb");
  if (InputFile == NULL) {
    fprintf (stderr, "Error opening file %s\n", InputFileName);
    exit (EXIT_FAILURE);
  }

  //
  // Go to the end so that we can determine the file size
  //
  if (fseek (InputFile, 0, SEEK_END)) {
    fprintf (stderr, "Error reading input file %s\n", InputFileName);
    fclose (InputFile);
    exit (EXIT_FAILURE);
  }

  //
  // Get the file size
  //
  FileSizeLong = ftell (InputFile);
  if (FileSizeLong < 0 || (unsigned long)FileSizeLong > (unsigned long)MAX_UINT32) {
    fprintf (stderr, "Error parsing the input file %s\n", InputFileName);
    fclose (InputFile);
    exit (EXIT_FAILURE);
  }
  *FileSize = (UINT32)FileSizeLong;

  //
  // Allocate a buffer
  //
  *FileBuffer = *FileSize == 0 ? NULL : malloc (*FileSize);
  if (*FileSize != 0 && *FileBuffer == NULL) {
    fprintf (stderr, "Can not allocate buffer for input input file %s\n", InputFileName);
    fclose (InputFile);
    exit (EXIT_FAILURE);
  }

  //
  // Reset to the beginning of the file
  //
  if (fseek (InputFile, 0, SEEK_SET)) {
    fprintf (stderr, "Error reading the input file %s\n", InputFileName);
    fclose (InputFile);
    free (*FileBuffer);
    exit (EXIT_FAILURE);
  }

  //
  // Read all of the file contents.
  //
  BytesRead = fread (*FileBuffer, sizeof (UINT8), *FileSize, InputFile);
  if (BytesRead != (size_t)*FileSize) {
    fprintf (stderr, "Error reading the input file %s\n", InputFileName);
    fclose (InputFile);
    free (*FileBuffer);
    exit (EXIT_FAILURE);
  }

  //
  // Close the file
  //
  fclose (InputFile);
}

STATIC
VOID
ParseFile (
  UINT8   *FileBuffer,
  UINT32  FileSize
  )
/*++

Routine Description:

  Read the initial PCD value from the input file buffer.

Arguments:

  FileBuffer  Point to the input file buffer.
  FileSize    Size of the file buffer.

Returns:

  None
--*/
{
  UINT32  Index;
  UINT32  NumLines;
  UINT32  TokenIndex;
  UINT32  TokenStart;
  UINT32  Capacity;

  if (FileSize != 0 && FileBuffer == NULL) {
    fprintf (stderr, "PCD database buffer is missing\n");
    exit (EXIT_FAILURE);
  }

  NumLines = 0;
  for (Index = 0; Index < FileSize; Index++) {
    if (FileBuffer[Index] == '\r') {
      NumLines++;
      if (Index + 1 < FileSize && FileBuffer[Index + 1] == '\n') {
        Index++;
      }
    } else if (FileBuffer[Index] == '\n') {
      NumLines++;
    }
  }

  if (NumLines == MAX_UINT32) {
    fprintf (stderr, "PCD database contains too many records\n");
    exit (EXIT_FAILURE);
  }
  Capacity = NumLines + 1;
  PcdList = calloc((size_t)Capacity, sizeof(PcdList[0]));
  if (PcdList == NULL) {
    fprintf (stderr, "Can not allocate PCD database list\n");
    exit (EXIT_FAILURE);
  }

  TokenIndex = 0;
  PcdListLength = 0;
  TokenStart = 0;
  for (Index = 0; Index < FileSize; Index++) {
    BOOLEAN IsLineEnd;

    if (FileBuffer[Index] == ' ') {
      continue;
    }
    IsLineEnd = (BOOLEAN)(FileBuffer[Index] == '\n' || FileBuffer[Index] == '\r');
    if (FileBuffer[Index] != '|' && FileBuffer[Index] != '.' && !IsLineEnd) {
      continue;
    }

    if (!(IsLineEnd && TokenIndex == 0 && Index == TokenStart)) {
      if (PcdListLength >= Capacity) {
        fprintf (stderr, "PCD database record count exceeds allocation\n");
        exit (EXIT_FAILURE);
      }
      RecordToken (FileBuffer, PcdListLength, TokenIndex, TokenStart, Index);
    }

    if (IsLineEnd) {
      if (TokenIndex != 0) {
        PcdListLength++;
      }
      TokenIndex = 0;
      if (FileBuffer[Index] == '\r' && Index + 1 < FileSize && FileBuffer[Index + 1] == '\n') {
        Index++;
      }
    } else {
      TokenIndex++;
    }
    TokenStart = Index + 1;
  }

  if (FileSize > TokenStart) {
    if (PcdListLength >= Capacity) {
      fprintf (stderr, "PCD database record count exceeds allocation\n");
      exit (EXIT_FAILURE);
    }
    RecordToken (FileBuffer, PcdListLength, TokenIndex, TokenStart, FileSize);
    if (TokenIndex != 0) {
      PcdListLength++;
    }
  }
}

STATIC
VOID
WriteOutputFile (
  CHAR8   *OutputFileName
  )
/*++

Routine Description:

  Write the updated PCD value into the output file name.

Arguments:

  OutputFileName  Point to the output file name.

Returns:

  None
--*/
{
  FILE    *OutputFile;
  UINT32  Index;

  //
  // Open output file
  //
  OutputFile = fopen (OutputFileName, "wb");
  if (OutputFile == NULL) {
    fprintf (stderr, "Error opening file %s\n", OutputFileName);
    exit (EXIT_FAILURE);
  }

  for (Index = 0; Index < PcdListLength; Index++) {
    if (fprintf (
          OutputFile,
          "%s.%s.%s.%s|%s|%s\n",
          PcdList[Index].SkuName,
          PcdList[Index].DefaultValueName,
          PcdList[Index].TokenSpaceGuidName,
          PcdList[Index].TokenName,
          PcdList[Index].DataType,
          PcdList[Index].Value
          ) < 0)
    {
      fprintf (stderr, "Error writing file %s\n", OutputFileName);
      fclose (OutputFile);
      exit (EXIT_FAILURE);
    }
  }

  //
  // Done, write output file.
  //
  if (fclose (OutputFile) != 0) {
    fprintf (stderr, "Error closing file %s after writing\n", OutputFileName);
    exit (EXIT_FAILURE);
  }
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
  fprintf (stdout, "Usage: -i <input_file> -o <output_file>\n\n");
  fprintf (stdout, "optional arguments:\n");
  fprintf (stdout, "  -h, --help            Show this help message and exit\n");
  fprintf (stdout, "  -i INPUT_FILENAME, --input INPUT_FILENAME\n\
                        PCD Database Input file name\n");
  fprintf (stdout, "  -o OUTPUT_FILENAME, --output OUTPUT_FILENAME\n\
                        PCD Database Output file name\n");
}

STATIC
VOID
ParseArguments (
  int    argc,
  char   *argv[],
  CHAR8  **InputFileName,
  CHAR8  **OutputFileName
  )
/*++

Routine Description:

  Parse the input parameters to get the input/output file name.

Arguments:

  argc            Number of command line parameters.
  argv            Array of pointers to parameter strings.
  InputFileName   Point to the input file name.
  OutputFileName  Point to the output file name.

Returns:

  None
--*/
{
  if (argc == 1) {
    fprintf (stderr, "Missing options\n");
    exit (EXIT_FAILURE);
  }

  //
  // Parse command line
  //
  argc--;
  argv++;

  if ((stricmp (argv[0], "-h") == 0) || (stricmp (argv[0], "--help") == 0)) {
    Usage ();
    exit (EXIT_SUCCESS);
  }

  while (argc > 0) {
    if ((stricmp (argv[0], "-i") == 0) || (stricmp (argv[0], "--input") == 0)) {
      if (argv[1] == NULL || argv[1][0] == '-') {
        fprintf (stderr, "Invalid option value.  Input File name is missing for -i option\n");
        exit (EXIT_FAILURE);
      }
      *InputFileName = argv[1];
      argc -= 2;
      argv += 2;
      continue;
    }

    if ((stricmp (argv[0], "-o") == 0) || (stricmp (argv[0], "--output") == 0)) {
      if (argv[1] == NULL || argv[1][0] == '-') {
        fprintf (stderr, "Invalid option value.  Output File name is missing for -i option\n");
        exit (EXIT_FAILURE);
      }
      *OutputFileName = argv[1];
      argc -= 2;
      argv += 2;
      continue;
    }

    if (argv[0][0] == '-') {
      fprintf (stderr, "Unknown option %s\n", argv[0]);
      exit (EXIT_FAILURE);
    }
    argc --;
    argv ++;
  }

  //
  // Check Input parameters
  //
  if (*InputFileName == NULL) {
    fprintf (stderr, "Missing option.  Input files is not specified\n");
    exit (EXIT_FAILURE);
  }

  if (*OutputFileName == NULL) {
    fprintf (stderr, "Missing option.  Output file is not specified\n");
    exit (EXIT_FAILURE);
  }
}

int
PcdValueMain (
  int   argc,
  char  *argv[]
  )
/*++

Routine Description:

  Main function updates PCD values.

Arguments:

  argc            Number of command line parameters.
  argv            Array of pointers to parameter strings.

Returns:
  EXIT_SUCCESS
--*/
{
  CHAR8   *InputFileName;
  CHAR8   *OutputFileName;
  UINT8   *FileBuffer;
  UINT32  FileSize;

  InputFileName = NULL;
  OutputFileName = NULL;
  FileBuffer = NULL;
  FileSize = 0;

  //
  // Parse the input arguments
  //
  ParseArguments (argc, argv, &InputFileName, &OutputFileName);

  //
  // Open Input file and read file data.
  //
  ReadInputFile (InputFileName, &FileBuffer, &FileSize);

  //
  // Read the initial Pcd value
  //
  ParseFile (FileBuffer, FileSize);

  //
  // Customize PCD values in the PCD Database
  //
  PcdEntryPoint ();

  //
  // Save the updated PCD value
  //
  WriteOutputFile (OutputFileName);

  exit (EXIT_SUCCESS);
}

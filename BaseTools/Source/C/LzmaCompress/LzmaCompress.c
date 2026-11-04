/** @file
  LZMA Compress/Decompress tool (LzmaCompress)

  Based on LZMA SDK 18.05:
    LzmaUtil.c -- Test application for LZMA compression
    2018-04-30 : Igor Pavlov : Public domain

  Copyright (c) 2006 - 2018, Intel Corporation. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "Sdk/C/Alloc.h"
#include "Sdk/C/7zFile.h"
#include "Sdk/C/7zVersion.h"
#include "Sdk/C/LzmaDec.h"
#include "Sdk/C/LzmaEnc.h"
#include "Sdk/C/Bra.h"
#include "CommonLib.h"

#define LZMA_HEADER_SIZE (LZMA_PROPS_SIZE + 8)
#define RESULT_BUFFER_SIZE 2000U

typedef enum {
  NoConverter,
  X86Converter,
  MaxConverter
} CONVERTER_TYPE;

const char *kCantReadMessage = "Can not read input file";
const char *kCantWriteMessage = "Can not write output file";
const char *kCantAllocateMessage = "Can not allocate memory";
const char *kDataErrorMessage = "Data error";

static Bool mQuietMode = False;
static CONVERTER_TYPE mConType = NoConverter;

#define UTILITY_NAME "LzmaCompress"
#define UTILITY_MAJOR_VERSION 0
#define UTILITY_MINOR_VERSION 2
#define INTEL_COPYRIGHT \
  "Copyright (c) 2009-2018, Intel Corporation. All rights reserved."
static void AppendResult(char *buffer, const char *text)
{
  size_t length = strlen(buffer);
  if (length < RESULT_BUFFER_SIZE - 1U) {
    snprintf(buffer + length, RESULT_BUFFER_SIZE - length, "%s", text);
  }
}

void PrintHelp(char *buffer)
{
  AppendResult(buffer,
      "\n" UTILITY_NAME " - " INTEL_COPYRIGHT "\n"
      "Based on LZMA Utility " MY_VERSION_COPYRIGHT_DATE "\n"
      "\nUsage:  LzmaCompress -e|-d [options] <inputFile>\n"
             "  -e: encode file\n"
             "  -d: decode file\n"
             "  -o FileName, --output FileName: specify the output filename\n"
             "  --f86: enable converter for x86 code\n"
             "  -v, --verbose: increase output messages\n"
             "  -q, --quiet: reduce output messages\n"
             "  --debug [0-9]: set debug level\n"
             "  --version: display the program version and exit\n"
             "  -h, --help: display this help text\n"
             );
}

int PrintError(char *buffer, const char *message)
{
  AppendResult(buffer, "\nError: ");
  AppendResult(buffer, message);
  AppendResult(buffer, "\n");
  return 1;
}

int PrintErrorNumber(char *buffer, SRes val)
{
  size_t length = strlen(buffer);
  if (length < RESULT_BUFFER_SIZE - 1U) {
    snprintf(buffer + length, RESULT_BUFFER_SIZE - length, "\nError code: %x\n", (unsigned)val);
  }
  return 1;
}

int PrintUserError(char *buffer)
{
  return PrintError(buffer, "Incorrect command");
}

void PrintVersion(char *buffer)
{
  snprintf (buffer, RESULT_BUFFER_SIZE, "%s Version %d.%d %s ", UTILITY_NAME, UTILITY_MAJOR_VERSION, UTILITY_MINOR_VERSION, __BUILD_VERSION);
}

static SRes Encode(ISeqOutStream *outStream, ISeqInStream *inStream, UInt64 fileSize)
{
  SRes res;
  size_t inSize;
  Byte *inBuffer = 0;
  Byte *outBuffer = 0;
  Byte *filteredStream = 0;
  size_t outSize;
  CLzmaEncProps props;

  LzmaEncProps_Init(&props);
  LzmaEncProps_Normalize(&props);

  if (fileSize > (UInt64)SIZE_MAX) {
    return SZ_ERROR_MEM;
  }
  inSize = (size_t)fileSize;

  if (inSize != 0) {
    inBuffer = (Byte *)MyAlloc(inSize);
    if (inBuffer == 0)
      return SZ_ERROR_MEM;
  } else {
    return SZ_ERROR_INPUT_EOF;
  }

  if (SeqInStream_Read(inStream, inBuffer, inSize) != SZ_OK) {
    res = SZ_ERROR_READ;
    goto Done;
  }

  // Allocate at least 105% of the input size plus 64KB without overflow.
  if (inSize > SIZE_MAX - (inSize / 20U) ||
      inSize + (inSize / 20U) > SIZE_MAX - (1U << 16)) {
    res = SZ_ERROR_MEM;
    goto Done;
  }
  outSize = inSize + (inSize / 20U) + (1U << 16);
  if (outSize <= LZMA_HEADER_SIZE) {
    res = SZ_ERROR_MEM;
    goto Done;
  }
  outBuffer = (Byte *)MyAlloc(outSize);
  if (outBuffer == 0) {
    res = SZ_ERROR_MEM;
    goto Done;
  }

  {
    int i;
    for (i = 0; i < 8; i++)
      outBuffer[i + LZMA_PROPS_SIZE] = (Byte)(fileSize >> (8 * i));
  }

  if (mConType != NoConverter)
  {
    filteredStream = (Byte *)MyAlloc(inSize);
    if (filteredStream == 0) {
      res = SZ_ERROR_MEM;
      goto Done;
    }
    memcpy(filteredStream, inBuffer, inSize);

    if (mConType == X86Converter) {
      {
        UInt32 x86State;
        x86_Convert_Init(x86State);
        x86_Convert(filteredStream, (SizeT) inSize, 0, &x86State, 1);
      }
    }
  }

  {
    size_t outSizeProcessed = outSize - LZMA_HEADER_SIZE;
    size_t outPropsSize = LZMA_PROPS_SIZE;

    res = LzmaEncode(outBuffer + LZMA_HEADER_SIZE, &outSizeProcessed,
        mConType != NoConverter ? filteredStream : inBuffer, inSize,
        &props, outBuffer, &outPropsSize, 0,
        NULL, &g_Alloc, &g_Alloc);

    if (res != SZ_OK)
      goto Done;

    outSize = LZMA_HEADER_SIZE + outSizeProcessed;
  }

  if (outStream->Write(outStream, outBuffer, outSize) != outSize)
    res = SZ_ERROR_WRITE;

Done:
  MyFree(outBuffer);
  MyFree(inBuffer);
  MyFree(filteredStream);

  return res;
}

static SRes Decode(ISeqOutStream *outStream, ISeqInStream *inStream, UInt64 fileSize)
{
  SRes res;
  size_t inSize;
  Byte *inBuffer = 0;
  Byte *outBuffer = 0;
  size_t outSize = 0;
  size_t inSizePure;
  ELzmaStatus status;
  UInt64 outSize64 = 0;

  int i;

  if (fileSize > (UInt64)SIZE_MAX) {
    return SZ_ERROR_MEM;
  }
  inSize = (size_t)fileSize;

  if (inSize < LZMA_HEADER_SIZE)
    return SZ_ERROR_INPUT_EOF;

  inBuffer = (Byte *)MyAlloc(inSize);
  if (inBuffer == 0)
    return SZ_ERROR_MEM;

  if (SeqInStream_Read(inStream, inBuffer, inSize) != SZ_OK) {
    res = SZ_ERROR_READ;
    goto Done;
  }

  for (i = 0; i < 8; i++)
    outSize64 += ((UInt64)inBuffer[LZMA_PROPS_SIZE + i]) << (i * 8);

  //
  // BaseTools serializes firmware image and section sizes through 32-bit
  // fields.  Reject a malformed LZMA header before converting or allocating
  // an output buffer that cannot be represented by the consuming formats.
  //
  if (outSize64 > (UInt64)MAX_UINT32 || outSize64 > (UInt64)SIZE_MAX) {
    res = SZ_ERROR_DATA;
    goto Done;
  }
  outSize = (size_t)outSize64;
  if (outSize != 0) {
    outBuffer = (Byte *)MyAlloc(outSize);
    if (outBuffer == 0) {
      res = SZ_ERROR_MEM;
      goto Done;
    }
  } else {
    res = SZ_OK;
    goto Done;
  }

  inSizePure = inSize - LZMA_HEADER_SIZE;
  res = LzmaDecode(outBuffer, &outSize, inBuffer + LZMA_HEADER_SIZE, &inSizePure,
      inBuffer, LZMA_PROPS_SIZE, LZMA_FINISH_END, &status, &g_Alloc);

  if (res != SZ_OK)
    goto Done;

  if (mConType == X86Converter)
  {
    UInt32 x86State;
    x86_Convert_Init(x86State);
    x86_Convert(outBuffer, (SizeT) outSize, 0, &x86State, 0);
  }

  if (outStream->Write(outStream, outBuffer, outSize) != outSize)
    res = SZ_ERROR_WRITE;

Done:
  MyFree(outBuffer);
  MyFree(inBuffer);

  return res;
}

int main2(int numArgs, const char *args[], char *rs)
{
  CFileSeqInStream inStream;
  CFileOutStream outStream;
  int res;
  int encodeMode = 0;
  Bool modeWasSet = False;
  const char *inputFile = NULL;
  const char *outputFile = "file.tmp";
  int param;
  UInt64 fileSize;

  FileSeqInStream_CreateVTable(&inStream);
  File_Construct(&inStream.file);

  FileOutStream_CreateVTable(&outStream);
  File_Construct(&outStream.file);

  if (numArgs == 1)
  {
    PrintHelp(rs);
    return 0;
  }

  for (param = 1; param < numArgs; param++) {
    if (strcmp(args[param], "-e") == 0 || strcmp(args[param], "-d") == 0) {
      encodeMode = (args[param][1] == 'e');
      modeWasSet = True;
    } else if (strcmp(args[param], "--f86") == 0) {
      mConType = X86Converter;
    } else if (strcmp(args[param], "-o") == 0 ||
               strcmp(args[param], "--output") == 0) {
      if (numArgs < (param + 2)) {
        return PrintUserError(rs);
      }
      outputFile = args[++param];
    } else if (strcmp(args[param], "--debug") == 0) {
      if (numArgs < (param + 2)) {
        return PrintUserError(rs);
      }
      //
      // For now we silently ignore this parameter to achieve command line
      // parameter compatibility with other build tools.
      //
      param++;
    } else if (
                strcmp(args[param], "-h") == 0 ||
                strcmp(args[param], "--help") == 0
              ) {
      PrintHelp(rs);
      return 0;
    } else if (
                strcmp(args[param], "-v") == 0 ||
                strcmp(args[param], "--verbose") == 0
              ) {
      //
      // For now we silently ignore this parameter to achieve command line
      // parameter compatibility with other build tools.
      //
    } else if (
                strcmp(args[param], "-q") == 0 ||
                strcmp(args[param], "--quiet") == 0
              ) {
      mQuietMode = True;
    } else if (strcmp(args[param], "--version") == 0) {
      PrintVersion(rs);
      return 0;
    } else if (inputFile == NULL) {
      inputFile = args[param];
    } else {
      return PrintUserError(rs);
    }
  }

  if ((inputFile == NULL) || !modeWasSet) {
    return PrintUserError(rs);
  }

  {
    size_t t4 = sizeof(UInt32);
    size_t t8 = sizeof(UInt64);
    if (t4 != 4 || t8 != 8)
      return PrintError(rs, "Incorrect UInt32 or UInt64");
  }

  if (InFile_Open(&inStream.file, inputFile) != 0)
    return PrintError(rs, "Can not open input file");

  if (OutFile_Open(&outStream.file, outputFile) != 0) {
    File_Close(&inStream.file);
    return PrintError(rs, "Can not open output file");
  }

  if (File_GetLength(&inStream.file, &fileSize) != 0) {
    File_Close(&outStream.file);
    File_Close(&inStream.file);
    return PrintError(rs, kCantReadMessage);
  }

  if (encodeMode)
  {
    if (!mQuietMode) {
      printf("Encoding\n");
    }
    res = Encode(&outStream.vt, &inStream.vt, fileSize);
  }
  else
  {
    if (!mQuietMode) {
      printf("Decoding\n");
    }
    res = Decode(&outStream.vt, &inStream.vt, fileSize);
  }

  File_Close(&outStream.file);
  File_Close(&inStream.file);

  if (res != SZ_OK)
  {
    if (res == SZ_ERROR_MEM)
      return PrintError(rs, kCantAllocateMessage);
    else if (res == SZ_ERROR_DATA)
      return PrintError(rs, kDataErrorMessage);
    else if (res == SZ_ERROR_WRITE)
      return PrintError(rs, kCantWriteMessage);
    else if (res == SZ_ERROR_READ)
      return PrintError(rs, kCantReadMessage);
    return PrintErrorNumber(rs, res);
  }
  return 0;
}

int MY_CDECL main(int numArgs, const char *args[])
{
  char rs[RESULT_BUFFER_SIZE] = { 0 };
  int res = main2(numArgs, args, rs);
  if (strlen(rs) > 0) {
    puts(rs);
  }
  return res;
}

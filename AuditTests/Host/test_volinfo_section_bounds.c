/* Credits: ©DNMTechLabs / dnmtechlabs@gmail.com */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <Common/UefiBaseTypes.h>
#include <Common/PiFirmwareFile.h>
#include <Guid/PiFirmwareFileSystem.h>
#include <Protocol/GuidedSectionExtraction.h>
#include <VolInfo/VolInfo.h>

EFI_STATUS ParseSection (UINT8 *SectionBuffer, UINT32 BufferLength);

static int Failures;

static void
Check (
  int Condition,
  const char *Message
  )
{
  if (!Condition) {
    fprintf (stderr, "FAIL: %s\n", Message);
    Failures++;
  }
}

static void
SetSectionSize (
  UINT8 *Buffer,
  UINT32 Size
  )
{
  Buffer[0] = (UINT8)Size;
  Buffer[1] = (UINT8)(Size >> 8);
  Buffer[2] = (UINT8)(Size >> 16);
}

int
main (void)
{
  UINT8 Buffer[128];
  EFI_STATUS Status;
  UINT32 Length;
  UINT32 Seed;
  UINT32 Iteration;
  UINT8 *Nested;
  UINT32 NestedLength;
  UINT32 Level;
  EFI_GUID CrcGuid = EFI_CRC32_GUIDED_SECTION_EXTRACTION_PROTOCOL_GUID;

  for (Length = 1; Length < 4; Length++) {
    memset (Buffer, 0, sizeof (Buffer));
    Status = ParseSection (Buffer, Length);
    Check (EFI_ERROR (Status), "short non-padding section rejected");
    memset (Buffer, 0xff, sizeof (Buffer));
    Status = ParseSection (Buffer, Length);
    Check (!EFI_ERROR (Status), "short erased padding accepted");
  }

  memset (Buffer, 0, sizeof (Buffer));
  SetSectionSize (Buffer, 4);
  Buffer[3] = EFI_SECTION_RAW;
  Check (!EFI_ERROR (ParseSection (Buffer, 4)), "minimal raw section accepted");

  memset (Buffer, 0, sizeof (Buffer));
  SetSectionSize (Buffer, 3);
  Buffer[3] = EFI_SECTION_RAW;
  Check (EFI_ERROR (ParseSection (Buffer, 4)), "section shorter than header rejected");

  memset (Buffer, 0, sizeof (Buffer));
  SetSectionSize (Buffer, 12);
  Buffer[3] = EFI_SECTION_USER_INTERFACE;
  Buffer[4] = 'A';
  Buffer[5] = 0;
  Buffer[6] = 0;
  Buffer[7] = 0;
  Check (EFI_ERROR (ParseSection (Buffer, 8)), "declared size beyond buffer rejected");

  memset (Buffer, 0, sizeof (Buffer));
  SetSectionSize (Buffer, 6);
  Buffer[3] = EFI_SECTION_USER_INTERFACE;
  Buffer[4] = 'A';
  Buffer[5] = 0;
  Check (EFI_ERROR (ParseSection (Buffer, 6)), "unterminated UI string rejected");

  memset (Buffer, 0, sizeof (Buffer));
  SetSectionSize (Buffer, 8);
  Buffer[3] = EFI_SECTION_USER_INTERFACE;
  Buffer[4] = 'A';
  Check (!EFI_ERROR (ParseSection (Buffer, 8)), "terminated UI string accepted");

  memset (Buffer, 0, sizeof (Buffer));
  SetSectionSize (Buffer, 5);
  Buffer[3] = EFI_SECTION_DXE_DEPEX;
  Buffer[4] = EFI_DEP_PUSH;
  Check (EFI_ERROR (ParseSection (Buffer, 5)), "truncated depex GUID rejected");

  Nested = malloc (4);
  Check (Nested != NULL, "nested allocation");
  if (Nested == NULL) {
    return 1;
  }
  SetSectionSize (Nested, 4);
  Nested[3] = EFI_SECTION_RAW;
  NestedLength = 4;
  for (Level = 0; Level < 66; Level++) {
    UINT32 NewLength = 24U + NestedLength;
    UINT8 *NewBuffer = calloc (1, NewLength);
    Check (NewBuffer != NULL, "nested GUID allocation");
    if (NewBuffer == NULL) {
      free (Nested);
      return 1;
    }
    SetSectionSize (NewBuffer, NewLength);
    NewBuffer[3] = EFI_SECTION_GUID_DEFINED;
    memcpy (NewBuffer + 4, &CrcGuid, sizeof (CrcGuid));
    NewBuffer[20] = 24;
    memcpy (NewBuffer + 24, Nested, NestedLength);
    free (Nested);
    Nested = NewBuffer;
    NestedLength = NewLength;
  }
  Check (EFI_ERROR (ParseSection (Nested, NestedLength)), "excessive section nesting rejected");
  free (Nested);

  Seed = 0x31415926U;
  for (Iteration = 0; Iteration < 20000; Iteration++) {
    UINT32 Index;
    Length = Seed % sizeof (Buffer);
    for (Index = 0; Index < Length; Index++) {
      Seed = Seed * 1664525U + 1013904223U;
      Buffer[Index] = (UINT8)(Seed >> 24);
    }
    (void)ParseSection (Buffer, Length);
  }

  if (Failures != 0) {
    return 1;
  }
  puts ("VolInfo section bounds and fuzz tests passed.");
  return 0;
}

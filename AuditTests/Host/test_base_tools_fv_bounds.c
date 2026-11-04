/* Credits: ©DNMTechLabs / dnmtechlabs@gmail.com */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

#include <Common/UefiBaseTypes.h>
#include <Common/PiFirmwareFile.h>
#include <Common/PiFirmwareVolume.h>
#include <FirmwareVolumeBufferLib.h>

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
InitFv (
  UINT8 *Storage,
  UINTN StorageSize,
  UINT16 HeaderLength
  )
{
  EFI_FIRMWARE_VOLUME_HEADER *Fv;

  memset (Storage, 0, StorageSize);
  Fv = (EFI_FIRMWARE_VOLUME_HEADER *)(void *)Storage;
  Fv->Signature = EFI_FVH_SIGNATURE;
  Fv->FvLength = StorageSize;
  Fv->HeaderLength = HeaderLength;
  Fv->BlockMap[0].NumBlocks = 1;
  Fv->BlockMap[0].Length = (UINT32)StorageSize;
  Fv->BlockMap[1].NumBlocks = 0;
  Fv->BlockMap[1].Length = 0;
}

int
main (void)
{
  UINT8 FvStorage[512];
  EFI_FIRMWARE_VOLUME_HEADER *Fv;
  UINTN Size;
  EFI_STATUS Status;
  UINT8 Sections[32];
  UINTN Key;
  VOID *Section;
  VOID *File;
  EFI_GUID Name = { 0x12345678, 0x1234, 0x5678, { 1, 2, 3, 4, 5, 6, 7, 8 } };
  UINT8 Dummy = 0;
  UINT8 NewFileStorage[sizeof (EFI_FFS_FILE_HEADER)];
  EFI_FFS_FILE_HEADER *ExistingFile;
  EFI_FFS_FILE_HEADER *NewFile;
  EFI_FFS_FILE_HEADER *NewVtfFile;
  UINT8 LargeFileStorage[sizeof (EFI_FFS_FILE_HEADER2)];
  EFI_FFS_FILE_HEADER2 *LargeFile;
  UINTN Count;
  UINT8 TinyFvStorage[offsetof (EFI_FIRMWARE_VOLUME_HEADER, BlockMap) +
                      sizeof (EFI_FV_BLOCK_MAP_ENTRY)];
  EFI_FIRMWARE_VOLUME_HEADER *TinyFv;
  UINT8 OversizedVtfStorage[1024];
  EFI_FFS_FILE_HEADER *OversizedVtf;
  VOID *DynamicFv;
  VOID *OriginalDynamicFv;
  UINT8 LargeAddStorage[64];
  EFI_FFS_FILE_HEADER2 *LargeAddFile;

  InitFv (
    FvStorage,
    sizeof (FvStorage),
    (UINT16)(offsetof (EFI_FIRMWARE_VOLUME_HEADER, BlockMap) +
             2U * sizeof (EFI_FV_BLOCK_MAP_ENTRY))
    );
  Fv = (EFI_FIRMWARE_VOLUME_HEADER *)(void *)FvStorage;
  Status = FvBufGetSize (Fv, &Size);
  Check (!EFI_ERROR (Status) && Size == sizeof (FvStorage), "valid FV size");

  Key = 0;
  File = (VOID *)(UINTN)1;
  Status = FvBufFindNextFile (Fv, &Key, &File);
  Check (Status == EFI_NOT_FOUND && File == NULL, "empty erased FV has no files");

  Fv->HeaderLength = (UINT16)(offsetof (EFI_FIRMWARE_VOLUME_HEADER, BlockMap) +
                               sizeof (EFI_FV_BLOCK_MAP_ENTRY));
  Status = FvBufGetSize (Fv, &Size);
  Check (EFI_ERROR (Status), "missing block-map terminator rejected");

  Fv->HeaderLength = (UINT16)(offsetof (EFI_FIRMWARE_VOLUME_HEADER, BlockMap) +
                               2U * sizeof (EFI_FV_BLOCK_MAP_ENTRY));
  Fv->BlockMap[0].NumBlocks = 0xffffffffU;
  Fv->BlockMap[0].Length = 0xffffffffU;
  Status = FvBufGetSize (Fv, &Size);
  Check (EFI_ERROR (Status), "overflowing block map rejected");

  memset (Sections, 0, sizeof (Sections));
  Key = 0;
  Section = (VOID *)(UINTN)1;
  Status = FvBufFindNextSection (Sections, 1, &Key, &Section);
  Check (EFI_ERROR (Status) && Section == NULL, "truncated section rejected");

  Sections[0] = 0xff;
  Sections[1] = 0xff;
  Sections[2] = 0xff;
  Sections[3] = EFI_SECTION_RAW;
  Key = 0;
  Status = FvBufFindNextSection (Sections, 4, &Key, &Section);
  Check (EFI_ERROR (Status), "truncated extended section rejected");

  memset (Sections, 0, sizeof (Sections));
  Sections[0] = 4;
  Sections[3] = EFI_SECTION_RAW;
  Key = 0;
  Status = FvBufFindNextSection (Sections, 4, &Key, &Section);
  Check (!EFI_ERROR (Status) && Section == Sections && Key == 4, "valid minimal section accepted");

  File = (VOID *)(UINTN)1;
  Status = FvBufPackageFreeformRawFile (&Name, &Dummy, (UINTN)0xffffffffULL, &File);
  Check (EFI_ERROR (Status) && File == NULL, "oversized freeform file rejected before truncation");

  Check (FvBufExpand3ByteSize (NULL) == 0, "NULL compact-size pointer rejected safely");
  FvBufCompact3ByteSize (NULL, 1);
  Check (FvBufChecksumHeader (NULL) == EFI_INVALID_PARAMETER, "NULL FV checksum request rejected");
  Check (FvBufAddFileWithExtend (NULL, NewFileStorage) == EFI_INVALID_PARAMETER,
         "NULL FV pointer-to-pointer rejected by add-with-extend");
  Check (FvBufFindFileByName (Fv, NULL, &File) == EFI_INVALID_PARAMETER,
         "NULL file name rejected by file lookup");

  memset (LargeFileStorage, 0, sizeof (LargeFileStorage));
  LargeFile = (EFI_FFS_FILE_HEADER2 *)(void *)LargeFileStorage;
  LargeFile->Attributes = FFS_ATTRIB_LARGE_FILE | FFS_ATTRIB_CHECKSUM;
  LargeFile->ExtendedSize = sizeof (EFI_FFS_FILE_HEADER);
  Count = 99;
  Status = FvBufCountSections (LargeFile, &Count);
  Check (Status == EFI_VOLUME_CORRUPTED && Count == 0,
         "large FFS with ExtendedSize smaller than header rejected by section counter");
  Section = (VOID *)(UINTN)1;
  Status = FvBufFindSectionByType (LargeFile, EFI_SECTION_RAW, &Section);
  Check (Status == EFI_VOLUME_CORRUPTED && Section == NULL,
         "large FFS with ExtendedSize smaller than header rejected by section lookup");
  Status = FvBufChecksumFile (LargeFile);
  Check (Status == EFI_VOLUME_CORRUPTED,
         "large FFS with ExtendedSize smaller than header rejected by checksum");
  LargeFile->ExtendedSize = 0x100000040ULL;
  Check (FvBufGetFfsFileSize ((EFI_FFS_FILE_HEADER *)LargeFile) == 0,
         "large FFS size above legacy UINT32 range rejected instead of truncating");

  InitFv (
    FvStorage,
    sizeof (FvStorage),
    (UINT16)(offsetof (EFI_FIRMWARE_VOLUME_HEADER, BlockMap) +
             2U * sizeof (EFI_FV_BLOCK_MAP_ENTRY))
    );
  Fv = (EFI_FIRMWARE_VOLUME_HEADER *)(void *)FvStorage;
  Status = FvBufUnifyBlockSizes (Fv, 0);
  Check (Status == EFI_INVALID_PARAMETER, "zero block size rejected before modulo");
  if (sizeof (UINTN) > sizeof (UINT32)) {
    Status = FvBufUnifyBlockSizes (Fv, (UINTN)0x100000000ULL);
    Check (Status == EFI_INVALID_PARAMETER, "block size wider than FV block-map field rejected");
  }

  memset (TinyFvStorage, 0, sizeof (TinyFvStorage));
  TinyFv = (EFI_FIRMWARE_VOLUME_HEADER *)(void *)TinyFvStorage;
  TinyFv->Signature = EFI_FVH_SIGNATURE;
  TinyFv->FvLength = sizeof (TinyFvStorage);
  TinyFv->HeaderLength = sizeof (TinyFvStorage);
  TinyFv->BlockMap[0].NumBlocks = 1;
  TinyFv->BlockMap[0].Length = sizeof (TinyFvStorage);
  Status = FvBufUnifyBlockSizes (TinyFv, sizeof (TinyFvStorage));
  Check (Status == EFI_VOLUME_CORRUPTED, "unterminated block map rejected without reading past header");

  /*
   * Put a 40-byte valid file at offset 456.  A 24-byte VTF would begin at
   * offset 488 and overlap it; the helper must use the actual last-file size.
   */
  InitFv (FvStorage, sizeof (FvStorage), 456);
  Fv = (EFI_FIRMWARE_VOLUME_HEADER *)(void *)FvStorage;
  ExistingFile = (EFI_FFS_FILE_HEADER *)(void *)(FvStorage + Fv->HeaderLength);
  memset (ExistingFile, 0, 40);
  FvBufCompact3ByteSize (ExistingFile->Size, 40);
  ExistingFile->State = EFI_FILE_HEADER_VALID | EFI_FILE_DATA_VALID;

  memset (NewFileStorage, 0, sizeof (NewFileStorage));
  NewVtfFile = (EFI_FFS_FILE_HEADER *)(void *)NewFileStorage;
  FvBufCompact3ByteSize (NewVtfFile->Size, sizeof (NewFileStorage));
  NewVtfFile->State = EFI_FILE_DATA_VALID;
  Status = FvBufAddVtfFile (Fv, NewVtfFile);
  Check (Status == EFI_OUT_OF_RESOURCES, "overlapping VTF rejected using actual last-file size");

  /*
   * A valid-looking large-file header at the last 24 bytes of the FV does not
   * have room for ExtendedSize.  FvBufAddFile must reject it before reading
   * the 32-byte large-file header.
   */
  InitFv (FvStorage, sizeof (FvStorage), 488);
  Fv = (EFI_FIRMWARE_VOLUME_HEADER *)(void *)FvStorage;
  ExistingFile = (EFI_FFS_FILE_HEADER *)(void *)(FvStorage + 488);
  ExistingFile->Attributes = FFS_ATTRIB_LARGE_FILE;
  ExistingFile->State = EFI_FILE_HEADER_VALID | EFI_FILE_DATA_VALID;
  memset (NewFileStorage, 0, sizeof (NewFileStorage));
  NewFile = (EFI_FFS_FILE_HEADER *)(void *)NewFileStorage;
  FvBufCompact3ByteSize (NewFile->Size, sizeof (NewFileStorage));
  NewFile->State = EFI_FILE_DATA_VALID;
  Status = FvBufAddFile (Fv, NewFile);
  Check (Status == EFI_VOLUME_CORRUPTED, "truncated large-file header at FV end rejected before ExtendedSize read");

  memset (OversizedVtfStorage, 0, sizeof (OversizedVtfStorage));
  OversizedVtf = (EFI_FFS_FILE_HEADER *)(void *)OversizedVtfStorage;
  FvBufCompact3ByteSize (OversizedVtf->Size, sizeof (OversizedVtfStorage));
  OversizedVtf->State = EFI_FILE_DATA_VALID;
  InitFv (
    FvStorage,
    sizeof (FvStorage),
    (UINT16)(offsetof (EFI_FIRMWARE_VOLUME_HEADER, BlockMap) +
             2U * sizeof (EFI_FV_BLOCK_MAP_ENTRY))
    );
  Fv = (EFI_FIRMWARE_VOLUME_HEADER *)(void *)FvStorage;
  Status = FvBufAddVtfFile (Fv, OversizedVtf);
  Check (Status == EFI_OUT_OF_RESOURCES, "VTF larger than FV rejected before destination-pointer subtraction");

  DynamicFv = malloc (sizeof (FvStorage));
  Check (DynamicFv != NULL, "allocate FV for extension-overflow test");
  if (DynamicFv != NULL) {
    InitFv (
      (UINT8 *)DynamicFv,
      sizeof (FvStorage),
      (UINT16)(offsetof (EFI_FIRMWARE_VOLUME_HEADER, BlockMap) +
               2U * sizeof (EFI_FV_BLOCK_MAP_ENTRY))
      );
    OriginalDynamicFv = DynamicFv;
    Status = FvBufExtend (&DynamicFv, ~(UINTN)0);
    Check (Status == EFI_OUT_OF_RESOURCES && DynamicFv == OriginalDynamicFv,
           "extension overflow rejected without freeing or replacing original FV");
    Status = FvBufExtend (&DynamicFv, 0x40000000U);
    Check (Status == EFI_OUT_OF_RESOURCES && DynamicFv == OriginalDynamicFv,
           "extension rejected before creating an FV outside validated maximum size");
    free (DynamicFv);
  }

  DynamicFv = malloc (sizeof (FvStorage));
  Check (DynamicFv != NULL, "allocate FV for large-file extension test");
  if (DynamicFv != NULL) {
    InitFv ((UINT8 *)DynamicFv, sizeof (FvStorage), 456);
    memset (LargeAddStorage, 0, sizeof (LargeAddStorage));
    LargeAddFile = (EFI_FFS_FILE_HEADER2 *)(void *)LargeAddStorage;
    LargeAddFile->Attributes = FFS_ATTRIB_LARGE_FILE;
    LargeAddFile->ExtendedSize = sizeof (LargeAddStorage);
    LargeAddFile->State = EFI_FILE_DATA_VALID;
    Status = FvBufAddFileWithExtend (&DynamicFv, LargeAddFile);
    Size = 0;
    if (!EFI_ERROR (Status)) {
      Status = FvBufGetSize (DynamicFv, &Size);
    }
    Check (!EFI_ERROR (Status) && Size == 1024,
           "large FFS extension uses ExtendedSize instead of compact Size[3]");
    free (DynamicFv);
  }

  InitFv (FvStorage, sizeof (FvStorage), 456);
  Fv = (EFI_FIRMWARE_VOLUME_HEADER *)(void *)FvStorage;
  ExistingFile = (EFI_FFS_FILE_HEADER *)(void *)(FvStorage + 456);
  ExistingFile->State = EFI_FILE_HEADER_VALID | EFI_FILE_DATA_VALID;
  FvBufCompact3ByteSize (ExistingFile->Size, 1);
  Status = FvBufShrinkWrap (Fv);
  Check (Status == EFI_VOLUME_CORRUPTED && Fv->BlockMap[0].Length == sizeof (FvStorage),
         "shrink-wrap propagates corrupt file stream without mutating block map");

  if (Failures != 0) {
    return 1;
  }
  puts ("BaseTools firmware-volume bounds tests passed.");
  return 0;
}

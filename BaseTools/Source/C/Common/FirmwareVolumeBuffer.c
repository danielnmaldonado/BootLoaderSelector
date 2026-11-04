/** @file
EFI Firmware Volume routines which work on a Fv image in buffers.

Copyright (c) 1999 - 2018, Intel Corporation. All rights reserved.<BR>
SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include "FirmwareVolumeBufferLib.h"
#include "BinderFuncs.h"

//
// Local macros
//
#define EFI_TEST_FFS_ATTRIBUTES_BIT(FvbAttributes, TestAttributes, Bit) \
    ( \
      (BOOLEAN) ( \
          (FvbAttributes & EFI_FVB2_ERASE_POLARITY) ? (((~TestAttributes) & Bit) == Bit) : ((TestAttributes & Bit) == Bit) \
        ) \
    )


//
// Local prototypes
//

STATIC
UINT32
FvBufGetSecHdrLen(
   IN EFI_COMMON_SECTION_HEADER *SectionHeader
   )
{
  if (SectionHeader == NULL) {
    return 0;
  }
  if (FvBufExpand3ByteSize(SectionHeader->Size) == 0xffffff) {
    return sizeof(EFI_COMMON_SECTION_HEADER2);
  }
  return sizeof(EFI_COMMON_SECTION_HEADER);
}

STATIC
UINT32
FvBufGetSecFileLen (
  IN EFI_COMMON_SECTION_HEADER *SectionHeader
  )
{
  UINT32 Length;
  if (SectionHeader == NULL) {
    return 0;
  }
  Length = FvBufExpand3ByteSize(SectionHeader->Size);
  if (Length == 0xffffff) {
    Length = ((EFI_COMMON_SECTION_HEADER2 *)SectionHeader)->ExtendedSize;
  }
  return Length;
}

//
// Local prototypes
//

STATIC
UINT16
FvBufCalculateChecksum16 (
  IN UINT16       *Buffer,
  IN UINTN        Size
  );

STATIC
UINT8
FvBufCalculateChecksum8 (
  IN UINT8        *Buffer,
  IN UINTN        Size
  );

//
// Procedures start
//

EFI_STATUS
FvBufRemoveFileNew (
  IN OUT VOID *Fv,
  IN EFI_GUID *Name
  )
/*++

Routine Description:

  Clears out all files from the Fv buffer in memory

Arguments:

  SourceFv - Address of the Fv in memory, this firmware volume volume will
             be modified, if SourceFfsFile exists
  SourceFfsFile - Input FFS file to replace

Returns:

  EFI_SUCCESS
  EFI_NOT_FOUND

--*/
{
  EFI_STATUS                  Status;
  EFI_FFS_FILE_HEADER*        FileToRm;
  UINTN                       FileToRmLength;

  Status = FvBufFindFileByName(
    Fv,
    Name,
    (VOID **)&FileToRm
    );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  FileToRmLength = FvBufGetFfsFileSize (FileToRm);

  CommonLibBinderSetMem (
    FileToRm,
    FileToRmLength,
    (((EFI_FIRMWARE_VOLUME_HEADER*)Fv)->Attributes & EFI_FVB2_ERASE_POLARITY)
      ? 0xFF : 0
    );

  return EFI_SUCCESS;
}


EFI_STATUS
FvBufRemoveFile (
  IN OUT VOID *Fv,
  IN EFI_GUID *Name
  )
/*++

Routine Description:

  Clears out all files from the Fv buffer in memory

Arguments:

  SourceFv - Address of the Fv in memory, this firmware volume volume will
             be modified, if SourceFfsFile exists
  SourceFfsFile - Input FFS file to replace

Returns:

  EFI_SUCCESS
  EFI_NOT_FOUND

--*/
{
  EFI_STATUS                  Status;
  EFI_FFS_FILE_HEADER        *NextFile;
  EFI_FIRMWARE_VOLUME_HEADER *TempFv;
  UINTN                       FileKey;
  UINTN                       FvLength;

  Status = FvBufFindFileByName(
    Fv,
    Name,
    NULL
    );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = FvBufGetSize (Fv, &FvLength);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  TempFv = NULL;
  Status = FvBufDuplicate (Fv, (VOID **)&TempFv);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = FvBufClearAllFiles (TempFv);
  if (EFI_ERROR (Status)) {
    CommonLibBinderFree (TempFv);
    return Status;
  }

  // TempFv has been allocated.  It must now be freed
  // before returning.

  FileKey = 0;
  while (TRUE) {

    Status = FvBufFindNextFile (Fv, &FileKey, (VOID **)&NextFile);
    if (Status == EFI_NOT_FOUND) {
      break;
    } else if (EFI_ERROR (Status)) {
      CommonLibBinderFree (TempFv);
      return Status;
    }

    if (CommonLibBinderCompareGuid (Name, &NextFile->Name)) {
      continue;
    }
    else {
      Status = FvBufAddFile (TempFv, NextFile);
      if (EFI_ERROR (Status)) {
        CommonLibBinderFree (TempFv);
        return Status;
      }
    }
  }

  CommonLibBinderCopyMem (Fv, TempFv, FvLength);
  CommonLibBinderFree (TempFv);

  return EFI_SUCCESS;
}


EFI_STATUS
FvBufChecksumFile (
  IN OUT VOID *FfsFile
  )
/*++

Routine Description:

  Clears out all files from the Fv buffer in memory

Arguments:

  SourceFfsFile - Input FFS file to update the checksum for

Returns:

  EFI_SUCCESS
  EFI_NOT_FOUND

--*/
{
  EFI_FFS_FILE_HEADER* File = (EFI_FFS_FILE_HEADER*)FfsFile;
  EFI_FFS_FILE_STATE StateBackup;
  UINT32 FileSize;
  UINT32 HeaderSize;

  if (File == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  HeaderSize = FvBufGetFfsHeaderSize (File);
  FileSize = FvBufGetFfsFileSize (File);
  if (FileSize < HeaderSize) {
    return EFI_VOLUME_CORRUPTED;
  }

  //
  // Fill in checksums and state, they must be 0 for checksumming.
  //
  File->IntegrityCheck.Checksum.Header = 0;
  File->IntegrityCheck.Checksum.File = 0;
  StateBackup = File->State;
  File->State = 0;

  File->IntegrityCheck.Checksum.Header =
    FvBufCalculateChecksum8 (
      (UINT8 *) File,
      HeaderSize
      );

  if (File->Attributes & FFS_ATTRIB_CHECKSUM) {
    File->IntegrityCheck.Checksum.File = FvBufCalculateChecksum8 (
                                                (VOID*)((UINT8 *)File + HeaderSize),
                                                FileSize - HeaderSize
                                                );
  } else {
    File->IntegrityCheck.Checksum.File = FFS_FIXED_CHECKSUM;
  }

  File->State = StateBackup;

  return EFI_SUCCESS;
}


EFI_STATUS
FvBufChecksumHeader (
  IN OUT VOID *Fv
  )
/*++

Routine Description:

  Clears out all files from the Fv buffer in memory

Arguments:

  SourceFv - Address of the Fv in memory, this firmware volume volume will
             be modified, if SourceFfsFile exists
  SourceFfsFile - Input FFS file to replace

Returns:

  EFI_SUCCESS
  EFI_NOT_FOUND

--*/
{
  EFI_FIRMWARE_VOLUME_HEADER* FvHeader = (EFI_FIRMWARE_VOLUME_HEADER*)Fv;
  EFI_STATUS Status;
  UINTN FvSize;

  Status = FvBufGetSize (Fv, &FvSize);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  (VOID)FvSize;
  FvHeader->Checksum = 0;
  FvHeader->Checksum =
    FvBufCalculateChecksum16 (
      (UINT16*) FvHeader,
      FvHeader->HeaderLength / sizeof (UINT16)
      );

  return EFI_SUCCESS;
}


EFI_STATUS
FvBufDuplicate (
  IN VOID *SourceFv,
  IN OUT VOID **DestinationFv
  )
/*++

Routine Description:

  Clears out all files from the Fv buffer in memory

Arguments:

  SourceFv - Address of the Fv in memory
  DestinationFv - Output for destination Fv
    DestinationFv == NULL - invalid parameter
    *DestinationFv == NULL - memory will be allocated
    *DestinationFv != NULL - this address will be the destination

Returns:

  EFI_SUCCESS

--*/
{
  EFI_STATUS Status;
  UINTN size;

  if (DestinationFv == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Status = FvBufGetSize (SourceFv, &size);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (*DestinationFv == NULL) {
    *DestinationFv = CommonLibBinderAllocate (size);
    if (*DestinationFv == NULL) {
      return EFI_OUT_OF_RESOURCES;
    }
  }

  CommonLibBinderCopyMem (*DestinationFv, SourceFv, size);

  return EFI_SUCCESS;
}


EFI_STATUS
FvBufExtend (
  IN VOID **Fv,
  IN UINTN Size
  )
/*++

Routine Description:

  Extends a firmware volume by the given number of bytes.

  BUGBUG: Does not handle the case where the firmware volume has a
          VTF (Volume Top File).  The VTF will not be moved to the
          end of the extended FV.

Arguments:

  Fv - Source and destination firmware volume.
       Note: The original firmware volume buffer is freed!

  Size - The minimum size that the firmware volume is to be extended by.
         The FV may be extended more than this size.

Returns:

  EFI_SUCCESS

--*/
{
  EFI_STATUS Status;
  UINTN OldSize;
  UINTN NewSize;
  UINTN BlockCount;
  UINTN ExtensionSize;
  UINTN BlockLength;
  VOID* NewFv;

  EFI_FIRMWARE_VOLUME_HEADER* hdr;
  EFI_FV_BLOCK_MAP_ENTRY*     blk;

  if (Fv == NULL || *Fv == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Status = FvBufGetSize (*Fv, &OldSize);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // Locate the block map in the fv header
  //
  hdr = (EFI_FIRMWARE_VOLUME_HEADER*)*Fv;
  blk = hdr->BlockMap;

  //
  // Calculate the number of blocks needed to achieve the requested
  // size extension without overflowing the round-up expression.
  //
  BlockLength = (UINTN)blk->Length;
  if (BlockLength == 0) {
    return EFI_VOLUME_CORRUPTED;
  }

  BlockCount = Size / BlockLength;
  if ((Size % BlockLength) != 0) {
    BlockCount++;
  }

  if (BlockCount == 0) {
    return EFI_SUCCESS;
  }

  if (BlockCount > (UINTN)(0xffffffffU - blk->NumBlocks) ||
      BlockCount > (~(UINTN)0) / BlockLength) {
    return EFI_OUT_OF_RESOURCES;
  }

  ExtensionSize = BlockCount * BlockLength;
  if (OldSize > (~(UINTN)0) - ExtensionSize) {
    return EFI_OUT_OF_RESOURCES;
  }
  NewSize = OldSize + ExtensionSize;
  if ((UINT64)NewSize >= 0x40000000ULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  NewFv = CommonLibBinderAllocate (NewSize);
  if (NewFv == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  //
  // Copy the old data
  //
  CommonLibBinderCopyMem (NewFv, *Fv, OldSize);

  //
  // Free the old fv buffer
  //
  CommonLibBinderFree (*Fv);

  //
  // Locate the block map in the new fv header
  //
  hdr = (EFI_FIRMWARE_VOLUME_HEADER*)NewFv;
  hdr->FvLength = NewSize;
  blk = hdr->BlockMap;

  //
  // Update the block map for the new fv
  //
  blk->NumBlocks += (UINT32)BlockCount;

  //
  // Update the FV header checksum
  //
  FvBufChecksumHeader (NewFv);

  //
  // Clear out the new area of the FV
  //
  CommonLibBinderSetMem (
    (UINT8*)NewFv + OldSize,
    (NewSize - OldSize),
    (hdr->Attributes & EFI_FVB2_ERASE_POLARITY) ? 0xFF : 0
    );

  //
  // Set output with new fv that was created
  //
  *Fv = NewFv;

  return EFI_SUCCESS;

}


EFI_STATUS
FvBufClearAllFiles (
  IN OUT VOID *Fv
  )
/*++

Routine Description:

  Clears out all files from the Fv buffer in memory

Arguments:

  Fv - Address of the Fv in memory

Returns:

  EFI_SUCCESS

--*/

{
  EFI_FIRMWARE_VOLUME_HEADER *hdr = (EFI_FIRMWARE_VOLUME_HEADER*)Fv;
  EFI_STATUS Status;
  UINTN size = 0;

  Status = FvBufGetSize (Fv, &size);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  CommonLibBinderSetMem(
    (UINT8*)hdr + hdr->HeaderLength,
    size - hdr->HeaderLength,
    (hdr->Attributes & EFI_FVB2_ERASE_POLARITY) ? 0xFF : 0
    );

  return EFI_SUCCESS;
}


EFI_STATUS
FvBufGetSize (
  IN VOID *Fv,
  OUT UINTN *Size
  )
/*++

Routine Description:

  Returns the validated size of a firmware volume buffer.

Arguments:

  Fv   - Address of the firmware volume in memory.
  Size - Receives the validated firmware volume size.

Returns:

  EFI_SUCCESS           The block map and volume size are valid.
  EFI_INVALID_PARAMETER A required parameter is NULL.
  EFI_VOLUME_CORRUPTED  The header or block map is malformed.

--*/

{
  EFI_FIRMWARE_VOLUME_HEADER *Hdr;
  EFI_FV_BLOCK_MAP_ENTRY     *BlockMap;
  UINTN                      BlockMapOffset;
  UINTN                      MaxBlockMapEntries;
  UINTN                      Index;
  UINT64                     TotalSize;
  UINT64                     BlockSize;

  if (Fv == NULL || Size == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  *Size = 0;
  Hdr = (EFI_FIRMWARE_VOLUME_HEADER *)Fv;
  BlockMapOffset = (UINTN)((UINT8 *)&Hdr->BlockMap[0] - (UINT8 *)Hdr);

  if (Hdr->Signature != EFI_FVH_SIGNATURE ||
      Hdr->HeaderLength < sizeof (EFI_FIRMWARE_VOLUME_HEADER) ||
      Hdr->HeaderLength < BlockMapOffset + sizeof (EFI_FV_BLOCK_MAP_ENTRY) ||
      Hdr->FvLength < Hdr->HeaderLength ||
      Hdr->FvLength >= 0x40000000ULL ||
      Hdr->FvLength > (UINT64)(~(UINTN)0)) {
    return EFI_VOLUME_CORRUPTED;
  }

  MaxBlockMapEntries =
    (Hdr->HeaderLength - BlockMapOffset) / sizeof (EFI_FV_BLOCK_MAP_ENTRY);
  BlockMap = Hdr->BlockMap;
  TotalSize = 0;

  for (Index = 0; Index < MaxBlockMapEntries; Index++) {
    if (BlockMap[Index].Length == 0 && BlockMap[Index].NumBlocks == 0) {
      if (TotalSize == 0 || TotalSize != Hdr->FvLength) {
        return EFI_VOLUME_CORRUPTED;
      }
      *Size = (UINTN)TotalSize;
      return EFI_SUCCESS;
    }

    if (BlockMap[Index].Length == 0 || BlockMap[Index].NumBlocks == 0) {
      return EFI_VOLUME_CORRUPTED;
    }

    BlockSize = (UINT64)BlockMap[Index].Length *
                (UINT64)BlockMap[Index].NumBlocks;
    if (TotalSize > Hdr->FvLength ||
        BlockSize > Hdr->FvLength - TotalSize) {
      return EFI_VOLUME_CORRUPTED;
    }
    TotalSize += BlockSize;
  }

  return EFI_VOLUME_CORRUPTED;
}


EFI_STATUS
FvBufAddFile (
  IN OUT VOID *Fv,
  IN VOID *File
  )
/*++

Routine Description:

  Adds a new FFS file

Arguments:

  Fv - Address of the Fv in memory
  File - FFS file to add to Fv

Returns:

  EFI_SUCCESS

--*/
{
  EFI_FIRMWARE_VOLUME_HEADER *hdr = (EFI_FIRMWARE_VOLUME_HEADER*)Fv;

  EFI_FFS_FILE_HEADER *fhdr = NULL;
  EFI_FVB_ATTRIBUTES_2 FvbAttributes;
  UINTN offset;
  UINTN fsize;
  UINTN headerSize;
  UINTN newHeaderSize;
  UINTN newSize;
  UINTN clearLoop;

  EFI_STATUS Status;
  UINTN fvSize;

  if (Fv == NULL || File == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Status = FvBufGetSize (Fv, &fvSize);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  FvbAttributes = hdr->Attributes;
  newHeaderSize = FvBufGetFfsHeaderSize ((EFI_FFS_FILE_HEADER*)File);
  newSize = FvBufGetFfsFileSize ((EFI_FFS_FILE_HEADER*)File);
  if (newSize < newHeaderSize) {
    return EFI_INVALID_PARAMETER;
  }
  if (newSize > fvSize) {
    return EFI_OUT_OF_RESOURCES;
  }

  for(
      offset = (UINTN)ALIGN_POINTER (hdr->HeaderLength, 8);
      offset <= fvSize && newSize <= fvSize - offset;
      offset = (UINTN)ALIGN_POINTER (offset, 8)
    ) {

    fhdr = (EFI_FFS_FILE_HEADER*) ((UINT8*)hdr + offset);

    if (EFI_TEST_FFS_ATTRIBUTES_BIT(
          FvbAttributes,
          fhdr->State,
          EFI_FILE_HEADER_VALID
        )
      ) {
      // BUGBUG: Need to make sure that the new file does not already
      // exist.

      headerSize = FvBufGetFfsHeaderSize (fhdr);
      if (headerSize > fvSize - offset) {
        return EFI_VOLUME_CORRUPTED;
      }

      fsize = FvBufGetFfsFileSize (fhdr);
      if (fsize < headerSize || fsize > fvSize - offset) {
        return EFI_VOLUME_CORRUPTED;
      }

      offset = offset + fsize;
      continue;
    }

    clearLoop = 0;
    while ((clearLoop < newSize) &&
           (((UINT8*)fhdr)[clearLoop] ==
             (UINT8)((hdr->Attributes & EFI_FVB2_ERASE_POLARITY) ? 0xFF : 0)
           )
          ) {
      clearLoop++;
    }

    //
    // We found a place in the FV which is empty and big enough for
    // the new file
    //
    if (clearLoop >= newSize) {
      break;
    }

    offset = offset + 1; // Make some forward progress
  }

  if (offset > fvSize || newSize > fvSize - offset || fhdr == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  CommonLibBinderCopyMem (fhdr, File, newSize);

  return EFI_SUCCESS;
}


EFI_STATUS
FvBufAddFileWithExtend (
  IN OUT VOID **Fv,
  IN VOID *File
  )
/*++

Routine Description:

  Adds a new FFS file.  Extends the firmware volume if needed.

Arguments:

  Fv - Source and destination firmware volume.
       Note: If the FV is extended, then the original firmware volume
             buffer is freed!

  Size - The minimum size that the firmware volume is to be extended by.
         The FV may be extended more than this size.

Returns:

  EFI_SUCCESS

--*/
{
  EFI_STATUS Status;
  EFI_FFS_FILE_HEADER* NewFile;

  if (Fv == NULL || *Fv == NULL || File == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  NewFile = (EFI_FFS_FILE_HEADER*)File;

  //
  // Try to add to the capsule volume
  //
  Status = FvBufAddFile (*Fv, NewFile);
  if (Status == EFI_OUT_OF_RESOURCES) {
    //
    // Try to extend the capsule volume by the size of the file
    //
    Status = FvBufExtend (Fv, FvBufGetFfsFileSize (NewFile));
    if (EFI_ERROR (Status)) {
      return Status;
    }

    //
    // Now, try to add the file again
    //
    Status = FvBufAddFile (*Fv, NewFile);
  }

  return Status;
}


EFI_STATUS
FvBufAddVtfFile (
  IN OUT VOID *Fv,
  IN VOID *File
  )
/*++

Routine Description:

  Adds a new FFS VFT (Volume Top File) file.  In other words, adds the
  file to the end of the firmware volume.

Arguments:

  Fv - Address of the Fv in memory
  File - FFS file to add to Fv

Returns:

  EFI_SUCCESS

--*/
{
  EFI_STATUS Status;

  EFI_FIRMWARE_VOLUME_HEADER *hdr = (EFI_FIRMWARE_VOLUME_HEADER*)Fv;

  EFI_FFS_FILE_HEADER* NewFile;
  UINTN                NewFileSize;

  UINT8 erasedUint8;
  UINTN clearLoop;

  EFI_FFS_FILE_HEADER *LastFile;
  UINTN LastFileSize;

  UINTN fvSize;
  UINTN Key;

  if (Fv == NULL || File == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Status = FvBufGetSize (Fv, &fvSize);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  erasedUint8 = (UINT8)((hdr->Attributes & EFI_FVB2_ERASE_POLARITY) ? 0xFF : 0);
  NewFileSize = FvBufGetFfsFileSize ((EFI_FFS_FILE_HEADER*)File);

  if (NewFileSize < FvBufGetFfsHeaderSize ((EFI_FFS_FILE_HEADER*)File)) {
    return EFI_INVALID_PARAMETER;
  }
  if (NewFileSize > fvSize) {
    return EFI_OUT_OF_RESOURCES;
  }
  if ((NewFileSize & 7U) != 0) {
    return EFI_INVALID_PARAMETER;
  }

  //
  // Find the last file in the FV
  //
  Key = 0;
  LastFile = NULL;
  LastFileSize = 0;
  while (TRUE) {
    EFI_FFS_FILE_HEADER *CurrentFile;

    CurrentFile = NULL;
    Status = FvBufFindNextFile (Fv, &Key, (VOID **)&CurrentFile);
    if (EFI_ERROR (Status)) {
      break;
    }
    LastFile = CurrentFile;
    LastFileSize = FvBufGetFfsFileSize (CurrentFile);
  }
  if (Status != EFI_NOT_FOUND) {
    return Status;
  }

  //
  // If no files were found, then we start at the beginning of the FV
  //
  if (LastFile == NULL) {
    LastFile = (EFI_FFS_FILE_HEADER*)((UINT8*)hdr + hdr->HeaderLength);
    LastFileSize = 0;
  }

  //
  // We want to put the new file (VTF) at the end of the FV
  //
  NewFile = (EFI_FFS_FILE_HEADER*)((UINT8*)hdr + (fvSize - NewFileSize));

  //
  // Check to see if there is enough room for the VTF after the last file
  // found in the FV
  //
  if ((UINT8*)NewFile < ((UINT8*)LastFile + LastFileSize)) {
    return EFI_OUT_OF_RESOURCES;
  }

  //
  // Loop to determine if the end of the FV is empty
  //
  clearLoop = 0;
  while ((clearLoop < NewFileSize) &&
         (((UINT8*)NewFile)[clearLoop] == erasedUint8)
        ) {
    clearLoop++;
  }

  //
  // Check to see if there was not enough room for the file
  //
  if (clearLoop < NewFileSize) {
    return EFI_OUT_OF_RESOURCES;
  }

  CommonLibBinderCopyMem (NewFile, File, NewFileSize);

  return EFI_SUCCESS;
}


VOID
FvBufCompact3ByteSize (
  OUT VOID* SizeDest,
  IN UINT32 Size
  )
/*++

Routine Description:

  Expands the 3 byte size commonly used in Firmware Volume data structures

Arguments:

  Size - Address of the 3 byte array representing the size

Returns:

  UINT32

--*/
{
  if (SizeDest == NULL) {
    return;
  }

  ((UINT8*)SizeDest)[0] = (UINT8)Size;
  ((UINT8*)SizeDest)[1] = (UINT8)(Size >> 8);
  ((UINT8*)SizeDest)[2] = (UINT8)(Size >> 16);
}

UINT32
FvBufGetFfsFileSize (
  IN EFI_FFS_FILE_HEADER *Ffs
  )
/*++

Routine Description:

  Get the FFS file size.

Arguments:

  Ffs - Pointer to FFS header

Returns:

  UINT32

--*/
{
  if (Ffs == NULL) {
    return 0;
  }
  if (Ffs->Attributes & FFS_ATTRIB_LARGE_FILE) {
    UINT64 ExtendedSize;

    ExtendedSize = ((EFI_FFS_FILE_HEADER2 *)Ffs)->ExtendedSize;
    if (ExtendedSize > 0xffffffffULL) {
      return 0;
    }
    return (UINT32)ExtendedSize;
  }
  return FvBufExpand3ByteSize(Ffs->Size);
}

UINT32
FvBufGetFfsHeaderSize (
  IN EFI_FFS_FILE_HEADER *Ffs
  )
/*++

Routine Description:

  Get the FFS header size.

Arguments:

  Ffs - Pointer to FFS header

Returns:

  UINT32

--*/
{
  if (Ffs == NULL) {
    return 0;
  }
  if (Ffs->Attributes & FFS_ATTRIB_LARGE_FILE) {
    return sizeof(EFI_FFS_FILE_HEADER2);
  }
  return sizeof(EFI_FFS_FILE_HEADER);
}

UINT32
FvBufExpand3ByteSize (
  IN VOID* Size
  )
/*++

Routine Description:

  Expands the 3 byte size commonly used in Firmware Volume data structures

Arguments:

  Size - Address of the 3 byte array representing the size

Returns:

  UINT32

--*/
{
  if (Size == NULL) {
    return 0;
  }

  return ((UINT32)((UINT8 *)Size)[2] << 16) |
         ((UINT32)((UINT8 *)Size)[1] << 8) |
         (UINT32)((UINT8 *)Size)[0];
}

EFI_STATUS
FvBufFindNextFile (
  IN VOID *Fv,
  IN OUT UINTN *Key,
  OUT VOID **File
  )
/*++

Routine Description:

  Iterates through the files contained within the firmware volume

Arguments:

  Fv - Address of the Fv in memory
  Key - Should be 0 to get the first file.  After that, it should be
        passed back in without modifying its contents to retrieve
        subsequent files.
  File - Output file pointer
    File == NULL - invalid parameter
    otherwise - *File will be update to the location of the file

Returns:

  EFI_SUCCESS
  EFI_NOT_FOUND
  EFI_VOLUME_CORRUPTED

--*/
{
  EFI_FIRMWARE_VOLUME_HEADER *Hdr;
  EFI_FFS_FILE_HEADER        *FileHeader;
  EFI_FVB_ATTRIBUTES_2       FvbAttributes;
  EFI_STATUS                 Status;
  UINTN                      FvSize;
  UINTN                      HeaderSize;
  UINTN                      FileSize;
  UINTN                      Remaining;

  if (Fv == NULL || Key == NULL || File == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  *File = NULL;
  Hdr = (EFI_FIRMWARE_VOLUME_HEADER *)Fv;
  Status = FvBufGetSize (Fv, &FvSize);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (Hdr->HeaderLength > FvSize) {
    return EFI_VOLUME_CORRUPTED;
  }

  if (*Key == 0) {
    *Key = Hdr->HeaderLength;
  } else if (*Key > FvSize) {
    return EFI_INVALID_PARAMETER;
  }

  FvbAttributes = Hdr->Attributes;

  while (TRUE) {
    if (*Key > (UINTN)(~(UINTN)0) - 7U) {
      return EFI_VOLUME_CORRUPTED;
    }
    *Key = (UINTN)ALIGN_POINTER (*Key, 8);
    if (*Key > FvSize || FvSize - *Key < sizeof (EFI_FFS_FILE_HEADER)) {
      return EFI_NOT_FOUND;
    }

    FileHeader = (EFI_FFS_FILE_HEADER *)((UINT8 *)Hdr + *Key);
    Remaining = FvSize - *Key;

    //
    // Invalid or erased headers do not carry a trustworthy size.  Advance to
    // the next aligned candidate before interpreting Attributes or Size.
    //
    if (!EFI_TEST_FFS_ATTRIBUTES_BIT (
           FvbAttributes,
           FileHeader->State,
           EFI_FILE_HEADER_VALID
           ) ||
        EFI_TEST_FFS_ATTRIBUTES_BIT (
          FvbAttributes,
          FileHeader->State,
          EFI_FILE_HEADER_INVALID
          )) {
      (*Key)++;
      continue;
    }

    HeaderSize = (FileHeader->Attributes & FFS_ATTRIB_LARGE_FILE) != 0 ?
                   sizeof (EFI_FFS_FILE_HEADER2) :
                   sizeof (EFI_FFS_FILE_HEADER);
    if (Remaining < HeaderSize) {
      return EFI_VOLUME_CORRUPTED;
    }

    FileSize = FvBufGetFfsFileSize (FileHeader);
    if (FileSize < HeaderSize || FileSize > Remaining) {
      return EFI_VOLUME_CORRUPTED;
    }

    if (EFI_TEST_FFS_ATTRIBUTES_BIT (
          FvbAttributes,
          FileHeader->State,
          EFI_FILE_MARKED_FOR_UPDATE
          ) ||
        EFI_TEST_FFS_ATTRIBUTES_BIT (
          FvbAttributes,
          FileHeader->State,
          EFI_FILE_DELETED
          )) {
      *Key += FileSize;
      continue;
    }

    if (EFI_TEST_FFS_ATTRIBUTES_BIT (
          FvbAttributes,
          FileHeader->State,
          EFI_FILE_DATA_VALID
          )) {
      *File = FileHeader;
      *Key += FileSize;
      return EFI_SUCCESS;
    }

    (*Key)++;
  }
}

EFI_STATUS
FvBufFindFileByName (
  IN VOID *Fv,
  IN EFI_GUID *Name,
  OUT VOID **File
  )
/*++

Routine Description:

  Searches the Fv for a file by its name

Arguments:

  Fv - Address of the Fv in memory
  Name - Guid filename to search for in the firmware volume
  File - Output file pointer
    File == NULL - Only determine if the file exists, based on return
                   value from the function call.
    otherwise - *File will be update to the location of the file

Returns:

  EFI_SUCCESS
  EFI_NOT_FOUND
  EFI_VOLUME_CORRUPTED

--*/
{
  EFI_STATUS Status;
  UINTN Key;
  EFI_FFS_FILE_HEADER *NextFile;

  if (Fv == NULL || Name == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Key = 0;
  while (TRUE) {
    Status = FvBufFindNextFile (Fv, &Key, (VOID **)&NextFile);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    if (CommonLibBinderCompareGuid (Name, &NextFile->Name)) {
      if (File != NULL) {
        *File = NextFile;
      }
      return EFI_SUCCESS;
    }
  }

  return EFI_NOT_FOUND;
}


EFI_STATUS
FvBufFindFileByType (
  IN VOID *Fv,
  IN EFI_FV_FILETYPE Type,
  OUT VOID **File
  )
/*++

Routine Description:

  Searches the Fv for a file by its type

Arguments:

  Fv - Address of the Fv in memory
  Type - FFS FILE type to search for
  File - Output file pointer
    (File == NULL) -> Only determine if the file exists, based on return
                      value from the function call.
    otherwise -> *File will be update to the location of the file

Returns:

  EFI_SUCCESS
  EFI_NOT_FOUND
  EFI_VOLUME_CORRUPTED

--*/
{
  EFI_STATUS Status;
  UINTN Key;
  EFI_FFS_FILE_HEADER *NextFile;

  Key = 0;
  while (TRUE) {
    Status = FvBufFindNextFile (Fv, &Key, (VOID **)&NextFile);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    if (Type == NextFile->Type) {
      if (File != NULL) {
        *File = NextFile;
      }
      return EFI_SUCCESS;
    }
  }

  return EFI_NOT_FOUND;
}


EFI_STATUS
FvBufGetFileRawData (
  IN  VOID*     FfsFile,
  OUT VOID**    RawData,
  OUT UINTN*    RawDataSize
  )
/*++

Routine Description:

  Searches the requested file for raw data.

  This routine either returns all the payload of a EFI_FV_FILETYPE_RAW file,
  or finds the EFI_SECTION_RAW section within the file and returns its data.

Arguments:

  FfsFile - Address of the FFS file in memory
  RawData - Pointer to the raw data within the file
            (This is NOT allocated.  It is within the file.)
  RawDataSize - Size of the raw data within the file

Returns:

  EFI_STATUS

--*/
{
  EFI_STATUS          Status;
  EFI_FFS_FILE_HEADER *File;
  EFI_RAW_SECTION     *Section;
  UINTN               FileSize;
  UINTN               FileHeaderSize;
  UINTN               SectionSize;
  UINTN               SectionHeaderSize;

  if (FfsFile == NULL || RawData == NULL || RawDataSize == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  *RawData = NULL;
  *RawDataSize = 0;
  File = (EFI_FFS_FILE_HEADER *)FfsFile;
  FileSize = FvBufGetFfsFileSize (File);
  FileHeaderSize = FvBufGetFfsHeaderSize (File);
  if (FileSize < FileHeaderSize) {
    return EFI_VOLUME_CORRUPTED;
  }

  if (File->Type == EFI_FV_FILETYPE_RAW) {
    *RawData = (UINT8 *)File + FileHeaderSize;
    *RawDataSize = FileSize - FileHeaderSize;
    return EFI_SUCCESS;
  }

  Status = FvBufFindSectionByType (File, EFI_SECTION_RAW, (VOID **)&Section);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  SectionSize = FvBufGetSecFileLen (Section);
  SectionHeaderSize = FvBufGetSecHdrLen (Section);
  if (SectionSize < SectionHeaderSize) {
    return EFI_VOLUME_CORRUPTED;
  }

  *RawData = (UINT8 *)Section + SectionHeaderSize;
  *RawDataSize = SectionSize - SectionHeaderSize;
  return EFI_SUCCESS;
}

EFI_STATUS
FvBufPackageFreeformRawFile (
  IN EFI_GUID*  Filename,
  IN VOID*      RawData,
  IN UINTN      RawDataSize,
  OUT VOID**    FfsFile
  )
/*++

Routine Description:

  Packages up a FFS file containing the input raw data.

  The file created will have a type of EFI_FV_FILETYPE_FREEFORM, and will
  contain one EFI_FV_FILETYPE_RAW section.

Arguments:

  RawData - Pointer to the raw data to be packed
  RawDataSize - Size of the raw data to be packed
  FfsFile - Address of the packaged FFS file.
            Note: The called must deallocate this memory!

Returns:

  EFI_STATUS

--*/
{
  EFI_FFS_FILE_HEADER *NewFile;
  EFI_RAW_SECTION     *NewSection;
  UINT64              NewSectionSize64;
  UINT64              NewFileSize64;
  UINT32              NewFileSize;
  UINT32              NewSectionSize;
  UINT32              FfsHeaderLength;
  UINT32              SectionHeaderLength;

  if (Filename == NULL || FfsFile == NULL ||
      (RawData == NULL && RawDataSize != 0)) {
    return EFI_INVALID_PARAMETER;
  }
  *FfsFile = NULL;

  SectionHeaderLength = sizeof (EFI_RAW_SECTION);
  NewSectionSize64 = (UINT64)RawDataSize + SectionHeaderLength;
  if (NewSectionSize64 >= MAX_SECTION_SIZE) {
    SectionHeaderLength = sizeof (EFI_RAW_SECTION2);
    NewSectionSize64 = (UINT64)RawDataSize + SectionHeaderLength;
  }
  if (NewSectionSize64 > 0xffffffffULL) {
    return EFI_BAD_BUFFER_SIZE;
  }

  FfsHeaderLength = sizeof (EFI_FFS_FILE_HEADER);
  NewFileSize64 = NewSectionSize64 + FfsHeaderLength;
  if (NewFileSize64 >= MAX_FFS_SIZE) {
    FfsHeaderLength = sizeof (EFI_FFS_FILE_HEADER2);
    NewFileSize64 = NewSectionSize64 + FfsHeaderLength;
  }
  if (NewFileSize64 > 0xffffffffULL) {
    return EFI_BAD_BUFFER_SIZE;
  }

  NewSectionSize = (UINT32)NewSectionSize64;
  NewFileSize = (UINT32)NewFileSize64;
  NewFile = CommonLibBinderAllocate (NewFileSize);
  if (NewFile == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  CommonLibBinderSetMem (NewFile, NewFileSize, 0);

  NewSection = (EFI_RAW_SECTION *)((UINT8 *)NewFile + FfsHeaderLength);
  if (NewSectionSize >= MAX_SECTION_SIZE) {
    FvBufCompact3ByteSize (NewSection->Size, 0xffffff);
    ((EFI_RAW_SECTION2 *)NewSection)->ExtendedSize = NewSectionSize;
  } else {
    FvBufCompact3ByteSize (NewSection->Size, NewSectionSize);
  }
  NewSection->Type = EFI_SECTION_RAW;

  if (RawDataSize != 0) {
    CommonLibBinderCopyMem (
      (UINT8 *)NewSection + SectionHeaderLength,
      RawData,
      RawDataSize
      );
  }

  CommonLibBinderCopyMem (&NewFile->Name, Filename, sizeof (EFI_GUID));
  NewFile->Attributes = 0;
  if (NewFileSize >= MAX_FFS_SIZE) {
    FvBufCompact3ByteSize (NewFile->Size, 0);
    ((EFI_FFS_FILE_HEADER2 *)NewFile)->ExtendedSize = NewFileSize;
    NewFile->Attributes |= FFS_ATTRIB_LARGE_FILE;
  } else {
    FvBufCompact3ByteSize (NewFile->Size, NewFileSize);
  }
  NewFile->Type = EFI_FV_FILETYPE_FREEFORM;
  NewFile->IntegrityCheck.Checksum.Header =
    FvBufCalculateChecksum8 ((UINT8 *)NewFile, FfsHeaderLength);
  NewFile->IntegrityCheck.Checksum.File = FFS_FIXED_CHECKSUM;
  NewFile->State = (UINT8)~(
                         EFI_FILE_HEADER_CONSTRUCTION |
                         EFI_FILE_HEADER_VALID |
                         EFI_FILE_DATA_VALID
                         );

  *FfsFile = NewFile;
  return EFI_SUCCESS;
}

EFI_STATUS
FvBufFindNextSection (
  IN VOID *SectionsStart,
  IN UINTN TotalSectionsSize,
  IN OUT UINTN *Key,
  OUT VOID **Section
  )
/*++

Routine Description:

  Iterates through the sections contained within a given array of sections

Arguments:

  SectionsStart - Address of the start of the FFS sections array
  TotalSectionsSize - Total size of all the sections
  Key - Should be 0 to get the first section.  After that, it should be
        passed back in without modifying its contents to retrieve
        subsequent files.
  Section - Output section pointer
    (Section == NULL) -> invalid parameter
    otherwise -> *Section will be update to the location of the file

Returns:

  EFI_SUCCESS
  EFI_NOT_FOUND
  EFI_VOLUME_CORRUPTED

--*/
{
  EFI_COMMON_SECTION_HEADER *SectionHeader;
  UINTN                     SectionSize;
  UINTN                     SectionHeaderSize;
  UINTN                     Remaining;

  if (SectionsStart == NULL || Key == NULL || Section == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  *Section = NULL;

  if (*Key > (UINTN)(~(UINTN)0) - 3U) {
    return EFI_VOLUME_CORRUPTED;
  }
  *Key = (UINTN)ALIGN_POINTER (*Key, 4);
  if (*Key > TotalSectionsSize) {
    return EFI_INVALID_PARAMETER;
  }

  Remaining = TotalSectionsSize - *Key;
  if (Remaining == 0) {
    return EFI_NOT_FOUND;
  }
  if (Remaining < sizeof (EFI_COMMON_SECTION_HEADER)) {
    return EFI_VOLUME_CORRUPTED;
  }

  SectionHeader =
    (EFI_COMMON_SECTION_HEADER *)((UINT8 *)SectionsStart + *Key);
  SectionHeaderSize =
    FvBufExpand3ByteSize (SectionHeader->Size) == 0xffffff ?
      sizeof (EFI_COMMON_SECTION_HEADER2) :
      sizeof (EFI_COMMON_SECTION_HEADER);
  if (Remaining < SectionHeaderSize) {
    return EFI_VOLUME_CORRUPTED;
  }

  SectionSize = FvBufGetSecFileLen (SectionHeader);
  if (SectionSize < SectionHeaderSize || SectionSize > Remaining) {
    return EFI_VOLUME_CORRUPTED;
  }

  *Section = SectionHeader;
  *Key += SectionSize;
  return EFI_SUCCESS;
}

EFI_STATUS
FvBufCountSections (
  IN VOID* FfsFile,
  IN UINTN* Count
  )
/*++

Routine Description:

  Searches the FFS file and counts the number of sections found.
  The sections are NOT recursed.

Arguments:

  FfsFile - Address of the FFS file in memory
  Count - The location to store the section count in

Returns:

  EFI_SUCCESS
  EFI_NOT_FOUND
  EFI_VOLUME_CORRUPTED

--*/
{
  EFI_STATUS                 Status;
  UINTN                      Key;
  UINTN                      FileSize;
  UINTN                      HeaderSize;
  VOID*                      SectionStart;
  UINTN                      TotalSectionsSize;
  EFI_COMMON_SECTION_HEADER* NextSection;

  if (FfsFile == NULL || Count == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  *Count = 0;
  HeaderSize = FvBufGetFfsHeaderSize ((EFI_FFS_FILE_HEADER*)FfsFile);
  FileSize = FvBufGetFfsFileSize ((EFI_FFS_FILE_HEADER*)FfsFile);
  if (FileSize < HeaderSize) {
    return EFI_VOLUME_CORRUPTED;
  }

  SectionStart = (VOID*)((UINT8*)FfsFile + HeaderSize);
  TotalSectionsSize = FileSize - HeaderSize;
  Key = 0;
  while (TRUE) {
    Status = FvBufFindNextSection (
               SectionStart,
               TotalSectionsSize,
               &Key,
               (VOID **)&NextSection
               );
    if (Status == EFI_NOT_FOUND) {
      return EFI_SUCCESS;
    } else if (EFI_ERROR (Status)) {
      return Status;
    }

    //
    // Increment the section counter
    //
    *Count += 1;

  }

  return EFI_NOT_FOUND;
}


EFI_STATUS
FvBufFindSectionByType (
  IN VOID *FfsFile,
  IN UINT8 Type,
  OUT VOID **Section
  )
/*++

Routine Description:

  Searches the FFS file for a section by its type

Arguments:

  FfsFile - Address of the FFS file in memory
  Type - FFS FILE section type to search for
  Section - Output section pointer
    (Section == NULL) -> Only determine if the section exists, based on return
                         value from the function call.
    otherwise -> *Section will be update to the location of the file

Returns:

  EFI_SUCCESS
  EFI_NOT_FOUND
  EFI_VOLUME_CORRUPTED

--*/
{
  EFI_STATUS Status;
  UINTN Key;
  UINTN                      FileSize;
  UINTN                      HeaderSize;
  VOID*                      SectionStart;
  UINTN                      TotalSectionsSize;
  EFI_COMMON_SECTION_HEADER* NextSection;

  if (FfsFile == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  if (Section != NULL) {
    *Section = NULL;
  }

  HeaderSize = FvBufGetFfsHeaderSize ((EFI_FFS_FILE_HEADER*)FfsFile);
  FileSize = FvBufGetFfsFileSize ((EFI_FFS_FILE_HEADER*)FfsFile);
  if (FileSize < HeaderSize) {
    return EFI_VOLUME_CORRUPTED;
  }

  SectionStart = (VOID*)((UINT8*)FfsFile + HeaderSize);
  TotalSectionsSize = FileSize - HeaderSize;
  Key = 0;
  while (TRUE) {
    Status = FvBufFindNextSection (
               SectionStart,
               TotalSectionsSize,
               &Key,
               (VOID **)&NextSection
               );
    if (EFI_ERROR (Status)) {
      return Status;
    }

    if (Type == NextSection->Type) {
      if (Section != NULL) {
        *Section = NextSection;
      }
      return EFI_SUCCESS;
    }
  }

  return EFI_NOT_FOUND;
}


EFI_STATUS
FvBufShrinkWrap (
  IN VOID *Fv
  )
/*++

Routine Description:

  Shrinks a firmware volume (in place) to provide a minimal FV.

  BUGBUG: Does not handle the case where the firmware volume has a
          VTF (Volume Top File).  The VTF will not be moved to the
          end of the extended FV.

Arguments:

  Fv - Firmware volume.

Returns:

  EFI_SUCCESS

--*/
{
  EFI_STATUS Status;
  UINTN OldSize;
  UINT32 BlockCount;
  UINT32 NewBlockSize = 128;
  UINTN Key;
  EFI_FFS_FILE_HEADER* FileIt;
  VOID* EndOfLastFile;

  EFI_FIRMWARE_VOLUME_HEADER* FvHdr;

  Status = FvBufGetSize (Fv, &OldSize);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // Locate the block map in the fv header and validate the file stream before
  // changing any block-map state.
  //
  FvHdr = (EFI_FIRMWARE_VOLUME_HEADER*)Fv;

  //
  // Find the end of the last file.
  //
  Key = 0;
  EndOfLastFile = (UINT8*)FvHdr + FvHdr->FvLength;
  while (TRUE) {
    Status = FvBufFindNextFile (Fv, &Key, (VOID **)&FileIt);
    if (Status == EFI_NOT_FOUND) {
      break;
    }
    if (EFI_ERROR (Status)) {
      return Status;
    }
    EndOfLastFile =
      (VOID*)((UINT8*)FileIt + FvBufGetFfsFileSize (FileIt));
  }

  Status = FvBufUnifyBlockSizes (Fv, NewBlockSize);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // Set the BlockCount to have the minimal number of blocks for the Fv.
  //
  BlockCount = (UINT32)((UINTN)EndOfLastFile - (UINTN)Fv);
  BlockCount = BlockCount + NewBlockSize - 1;
  BlockCount = BlockCount / NewBlockSize;

  //
  // Adjust the block count to shrink the Fv in place.
  //
  FvHdr->BlockMap[0].NumBlocks = BlockCount;
  FvHdr->FvLength = BlockCount * NewBlockSize;

  //
  // Update the FV header checksum
  //
  FvBufChecksumHeader (Fv);

  return EFI_SUCCESS;

}


EFI_STATUS
FvBufUnifyBlockSizes (
  IN OUT VOID *Fv,
  IN UINTN BlockSize
  )
/*++

Routine Description:

  Searches the FFS file for a section by its type

Arguments:

  Fv - Address of the Fv in memory
  BlockSize - The size of the blocks to convert the Fv to.  If the total size
              of the Fv is not evenly divisible by this size, then
              EFI_INVALID_PARAMETER will be returned.

Returns:

  EFI_SUCCESS
  EFI_NOT_FOUND
  EFI_VOLUME_CORRUPTED

--*/
{
  EFI_FIRMWARE_VOLUME_HEADER *hdr;
  EFI_FV_BLOCK_MAP_ENTRY *blk;
  EFI_STATUS Status;
  UINTN Size;
  UINTN BlockMapOffset;
  UINTN MaxBlockMapEntries;
  UINTN Index;

  if (Fv == NULL || BlockSize == 0 || BlockSize > 0xffffffffU) {
    return EFI_INVALID_PARAMETER;
  }

  Status = FvBufGetSize (Fv, &Size);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  hdr = (EFI_FIRMWARE_VOLUME_HEADER*)Fv;
  BlockMapOffset = (UINTN)((UINT8 *)&hdr->BlockMap[0] - (UINT8 *)hdr);
  MaxBlockMapEntries =
    (hdr->HeaderLength - BlockMapOffset) / sizeof (EFI_FV_BLOCK_MAP_ENTRY);
  blk = NULL;
  for (Index = 0; Index < MaxBlockMapEntries; Index++) {
    if (hdr->BlockMap[Index].Length == 0 &&
        hdr->BlockMap[Index].NumBlocks == 0) {
      blk = &hdr->BlockMap[Index];
      break;
    }
  }
  if (blk == NULL) {
    return EFI_VOLUME_CORRUPTED;
  }

  //
  // Make sure that the Fv size is a multiple of the new block size.
  //
  if ((Size % BlockSize) != 0) {
    return EFI_INVALID_PARAMETER;
  }

  //
  // Zero out the entire block map.
  //
  CommonLibBinderSetMem (
    &hdr->BlockMap,
    (UINTN)blk - (UINTN)&hdr->BlockMap,
    0
    );

  //
  // Write out the single block map entry.
  //
  hdr->BlockMap[0].Length = (UINT32)BlockSize;
  hdr->BlockMap[0].NumBlocks = (UINT32)(Size / BlockSize);

  return EFI_SUCCESS;
}

STATIC
UINT16
FvBufCalculateSum16 (
  IN UINT16       *Buffer,
  IN UINTN        Size
  )
/*++

Routine Description:

  This function calculates the UINT16 sum for the requested region.

Arguments:

  Buffer      Pointer to buffer containing byte data of component.
  Size        Size of the buffer

Returns:

  The 16 bit checksum

--*/
{
  UINTN   Index;
  UINT16  Sum;

  Sum = 0;

  //
  // Perform the word sum for buffer
  //
  for (Index = 0; Index < Size; Index++) {
    Sum = (UINT16) (Sum + Buffer[Index]);
  }

  return (UINT16) Sum;
}


STATIC
UINT16
FvBufCalculateChecksum16 (
  IN UINT16       *Buffer,
  IN UINTN        Size
  )
/*++

Routine Description::

  This function calculates the value needed for a valid UINT16 checksum

Arguments:

  Buffer      Pointer to buffer containing byte data of component.
  Size        Size of the buffer

Returns:

  The 16 bit checksum value needed.

--*/
{
  return (UINT16)(0x10000 - FvBufCalculateSum16 (Buffer, Size));
}


STATIC
UINT8
FvBufCalculateSum8 (
  IN UINT8  *Buffer,
  IN UINTN  Size
  )
/*++

Description:

  This function calculates the UINT8 sum for the requested region.

Input:

  Buffer      Pointer to buffer containing byte data of component.
  Size        Size of the buffer

Return:

  The 8 bit checksum value needed.

--*/
{
  UINTN   Index;
  UINT8   Sum;

  Sum = 0;

  //
  // Perform the byte sum for buffer
  //
  for (Index = 0; Index < Size; Index++) {
    Sum = (UINT8) (Sum + Buffer[Index]);
  }

  return Sum;
}


STATIC
UINT8
FvBufCalculateChecksum8 (
  IN UINT8        *Buffer,
  IN UINTN        Size
  )
/*++

Description:

  This function calculates the value needed for a valid UINT8 checksum

Input:

  Buffer      Pointer to buffer containing byte data of component.
  Size        Size of the buffer

Return:

  The 8 bit checksum value needed.

--*/
{
  return (UINT8)(0x100 - FvBufCalculateSum8 (Buffer, Size));
}



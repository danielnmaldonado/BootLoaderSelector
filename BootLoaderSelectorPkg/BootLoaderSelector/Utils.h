/** @file
  BootLoaderSelector utility helpers.
  Credits: ©DNMTechLabs / dnmtechlabs@gmail.com
**/
#ifndef BOOTLOADER_SELECTOR_UTILS_H
#define BOOTLOADER_SELECTOR_UTILS_H

#include <Uefi.h>
#include <Protocol/SimpleFileSystem.h>
#include <Guid/FileInfo.h>

#define BLS_VERSION_ASCII "2.32"
#define BLS_VERSION_TEXT  L"BootLoaderSelector v2.32"

#define BLS_MAX_TEXT      128
#define BLS_MAX_PATH      192
#define BLS_MAX_VOLUME_ID 384
#define BLS_MAX_ENTRIES   16
#define BLS_MAX_LOG       8192
#define BLS_MAX_DEVICE_PATH_BYTES (64U * 1024U)

VOID BlsSafeStrCpy(OUT CHAR16 *Dst, IN UINTN DstChars, IN CONST CHAR16 *Src);
VOID BlsSafeStrCat(OUT CHAR16 *Dst, IN UINTN DstChars, IN CONST CHAR16 *Src);
BOOLEAN BlsStrEq(IN CONST CHAR16 *A, IN CONST CHAR16 *B);
BOOLEAN BlsStriEqAscii(IN CONST CHAR8 *A, IN CONST CHAR8 *B);
BOOLEAN BlsAsciiContains(IN CONST CHAR8 *Haystack, IN CONST CHAR8 *Needle);
UINTN BlsStrLen16(IN CONST CHAR16 *Str);
UINTN BlsStrLen16Bounded(IN CONST CHAR16 *Str, IN UINTN MaxChars);
UINTN BlsAsciiLenBounded(IN CONST CHAR8 *Str, IN UINTN MaxLen);
VOID BlsAsciiToUnicode(OUT CHAR16 *Dst, IN UINTN DstChars, IN CONST CHAR8 *Src);
VOID BlsTrimAscii(CHAR8 *Str);
EFI_STATUS BlsGetProtocolInterface(
  IN EFI_HANDLE Handle,
  IN EFI_GUID *Protocol,
  OUT VOID **Interface
  );
EFI_STATUS BlsLocateProtocolHandlesBounded(
  IN EFI_GUID *Protocol,
  IN UINTN MaxHandles,
  OUT EFI_HANDLE **Handles,
  OUT UINTN *Count
  );
EFI_STATUS BlsOpenVolumeRoot(IN EFI_HANDLE VolumeHandle, OUT EFI_FILE_PROTOCOL **Root);
EFI_STATUS BlsFileOpen(
  IN EFI_FILE_PROTOCOL *Parent,
  OUT EFI_FILE_PROTOCOL **File,
  IN CONST CHAR16 *Path,
  IN UINT64 OpenMode,
  IN UINT64 Attributes
  );
EFI_STATUS BlsFileGetInfoAlloc(
  IN EFI_FILE_PROTOCOL *File,
  IN CONST EFI_GUID *InformationType,
  IN UINTN MinimumSize,
  OUT VOID **Information,
  OUT UINTN *InformationSize
  );
EFI_STATUS BlsFileReadExact(IN EFI_FILE_PROTOCOL *File, OUT VOID *Buffer, IN UINTN BufferSize);
EFI_STATUS BlsFileWriteAll(IN EFI_FILE_PROTOCOL *File, IN CONST VOID *Buffer, IN UINTN BufferSize);
EFI_STATUS BlsFileFlush(IN EFI_FILE_PROTOCOL *File);
EFI_STATUS BlsFileDelete(IN OUT EFI_FILE_PROTOCOL **File);
EFI_STATUS BlsFileClose(IN OUT EFI_FILE_PROTOCOL **File);
EFI_STATUS BlsCloseTimerEvent(IN OUT EFI_EVENT *Event);
VOID BlsFileCloseBestEffort(IN OUT EFI_FILE_PROTOCOL **File);

#endif

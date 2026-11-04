/* Credits: ©DNMTechLabs / dnmtechlabs@gmail.com */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef NULL
#undef NULL
#endif
#include <Uefi.h>
#include <Guid/FileInfo.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>

EFI_HANDLE gImageHandle = NULL;
EFI_SYSTEM_TABLE *gST = NULL;
EFI_BOOT_SERVICES *gBS = NULL;
EFI_GUID gEfiSimpleFileSystemProtocolGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
EFI_GUID gEfiLoadedImageProtocolGuid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
EFI_GUID gEfiFileInfoGuid = EFI_FILE_INFO_ID;

static EFI_BOOT_SERVICES gBootServices;
static EFI_SIMPLE_FILE_SYSTEM_PROTOCOL gFs;
static EFI_LOADED_IMAGE_PROTOCOL gLoadedImage;
static EFI_FILE_PROTOCOL gRoot;
static EFI_FILE_PROTOCOL gDir;
static EFI_FILE_PROTOCOL gFinalFile;
static EFI_FILE_PROTOCOL gTmpFile;
static CONST CHAR8 *gFinalData;
static CONST CHAR8 *gTmpData;
static UINTN gFinalPos;
static UINTN gTmpPos;
static EFI_STATUS gTmpReadStatus;
static EFI_STATUS gTmpCleanupStatus;
static UINTN gTmpReadOnlyOpens;
static UINTN gCreateOpenCalls;
static UINTN gDirectoryOpenCalls;
static BOOLEAN gEfiDirectoryExists;
static UINTN gDeleteCalls;
static UINTN gCloseCalls;
static BOOLEAN gOpenVolumeErrorWithOut;
static BOOLEAN gFileOpenErrorWithOut;
static EFI_HANDLE gVolumeHandle = (EFI_HANDLE)(UINTN)0x2000U;
static EFI_HANDLE gTestImageHandle = (EFI_HANDLE)(UINTN)0x1000U;

VOID *EFIAPI AllocateZeroPool(IN UINTN AllocationSize) { return calloc(1, (size_t)AllocationSize); }
VOID EFIAPI FreePool(IN JCONST VOID *Buffer) { free((void *)Buffer); }
VOID *EFIAPI CopyMem(OUT VOID *DestinationBuffer, IN CONST VOID *SourceBuffer, IN UINTN Length) {
  return memcpy(DestinationBuffer, SourceBuffer, (size_t)Length);
}
VOID *EFIAPI ZeroMem(OUT VOID *Buffer, IN UINTN Length) { return memset(Buffer, 0, (size_t)Length); }
VOID *EFIAPI SetMem(OUT VOID *Buffer, IN UINTN Length, IN UINT8 Value) { return memset(Buffer, Value, (size_t)Length); }
INTN EFIAPI CompareMem(IN CONST VOID *DestinationBuffer, IN CONST VOID *SourceBuffer, IN UINTN Length) {
  return (INTN)memcmp(DestinationBuffer, SourceBuffer, (size_t)Length);
}
UINTN EFIAPI AsciiStrLen(IN CONST CHAR8 *String) { return (UINTN)strlen(String); }
INTN EFIAPI AsciiStrnCmp(IN CONST CHAR8 *FirstString, IN CONST CHAR8 *SecondString, IN UINTN Length) {
  return (INTN)strncmp(FirstString, SecondString, (size_t)Length);
}
CHAR8 *EFIAPI AsciiStrStr(IN CONST CHAR8 *String, IN CONST CHAR8 *SearchString) {
  return (CHAR8 *)strstr(String, SearchString);
}
RETURN_STATUS EFIAPI AsciiStrCpyS(OUT CHAR8 *Destination, IN UINTN DestMax, IN CONST CHAR8 *Source) {
  size_t n;
  if (Destination == NULL || Source == NULL || DestMax == 0) return RETURN_INVALID_PARAMETER;
  n = strlen(Source);
  if (n + 1U > (size_t)DestMax) return RETURN_BUFFER_TOO_SMALL;
  memcpy(Destination, Source, n + 1U);
  return RETURN_SUCCESS;
}
UINTN EFIAPI AsciiSPrint(OUT CHAR8 *StartOfBuffer, IN UINTN BufferSize, IN CONST CHAR8 *FormatString, ...) {
  VA_LIST marker;
  CONST CHAR8 *value;
  int written;
  if (StartOfBuffer == NULL || BufferSize == 0 || FormatString == NULL) return 0;
  if (strcmp(FormatString, "<key>%a</key>") != 0) { StartOfBuffer[0] = '\0'; return 0; }
  VA_START(marker, FormatString);
  value = VA_ARG(marker, CONST CHAR8 *);
  VA_END(marker);
  written = snprintf(StartOfBuffer, (size_t)BufferSize, "<key>%s</key>", value);
  if (written < 0) { StartOfBuffer[0] = '\0'; return 0; }
  return ((UINTN)written < BufferSize) ? (UINTN)written : BufferSize - 1U;
}

static BOOLEAN PathEq(IN CONST CHAR16 *A, IN CONST CHAR16 *B) {
  UINTN i = 0;
  if (A == NULL || B == NULL) return FALSE;
  while (A[i] != L'\0' && B[i] != L'\0') {
    if (A[i] != B[i]) return FALSE;
    i++;
  }
  return (BOOLEAN)(A[i] == B[i]);
}

static CONST CHAR8 *DataForFile(IN EFI_FILE_PROTOCOL *This, OUT UINTN **Position) {
  if (This == &gFinalFile) {
    if (Position != NULL) *Position = &gFinalPos;
    return gFinalData;
  }
  if (This == &gTmpFile) {
    if (Position != NULL) *Position = &gTmpPos;
    return gTmpData;
  }
  if (Position != NULL) *Position = NULL;
  return NULL;
}

static EFI_STATUS EFIAPI TestFileGetInfo(
  IN EFI_FILE_PROTOCOL *This,
  IN EFI_GUID *InformationType,
  IN OUT UINTN *BufferSize,
  OUT VOID *Buffer
  ) {
  EFI_FILE_INFO *Info;
  CONST CHAR8 *Data;
  UINTN Required;
  (void)InformationType;
  if (BufferSize == NULL) return EFI_INVALID_PARAMETER;
  Data = DataForFile(This, NULL);
  if (Data == NULL) return EFI_UNSUPPORTED;
  Required = SIZE_OF_EFI_FILE_INFO + sizeof(CHAR16);
  if (Buffer == NULL || *BufferSize < Required) {
    *BufferSize = Required;
    return EFI_BUFFER_TOO_SMALL;
  }
  Info = (EFI_FILE_INFO *)Buffer;
  memset(Info, 0, (size_t)Required);
  Info->Size = Required;
  Info->FileSize = strlen(Data);
  Info->PhysicalSize = Info->FileSize;
  *BufferSize = Required;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI TestFileSetPosition(IN EFI_FILE_PROTOCOL *This, IN UINT64 Position) {
  UINTN *Pos;
  (void)DataForFile(This, &Pos);
  if (Pos == NULL || Position > MAX_UINTN) return EFI_INVALID_PARAMETER;
  *Pos = (UINTN)Position;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI TestFileRead(IN EFI_FILE_PROTOCOL *This, IN OUT UINTN *BufferSize, OUT VOID *Buffer) {
  CONST CHAR8 *Data;
  UINTN *Pos;
  UINTN Len;
  UINTN Available;
  UINTN ReadSize;
  if (BufferSize == NULL) return EFI_INVALID_PARAMETER;
  Data = DataForFile(This, &Pos);
  if (Data == NULL || Pos == NULL) return EFI_UNSUPPORTED;
  Len = (UINTN)strlen(Data);
  Available = (*Pos < Len) ? Len - *Pos : 0U;
  ReadSize = (*BufferSize < Available) ? *BufferSize : Available;
  if (ReadSize != 0U && Buffer != NULL) memcpy(Buffer, Data + *Pos, (size_t)ReadSize);
  *Pos += ReadSize;
  *BufferSize = ReadSize;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI TestFileClose(IN EFI_FILE_PROTOCOL *This) { (void)This; gCloseCalls++; return EFI_SUCCESS; }
static EFI_STATUS EFIAPI TestFileDelete(IN EFI_FILE_PROTOCOL *This) {
  (void)This;
  gDeleteCalls++;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI TestRootOpen(
  IN EFI_FILE_PROTOCOL *This,
  OUT EFI_FILE_PROTOCOL **NewHandle,
  IN CHAR16 *FileName,
  IN UINT64 OpenMode,
  IN UINT64 Attributes
  ) {
  (void)This; (void)Attributes;
  if (NewHandle == NULL || FileName == NULL) return EFI_INVALID_PARAMETER;
  *NewHandle = NULL;
  if (gFileOpenErrorWithOut) {
    *NewHandle = &gTmpFile;
    return EFI_DEVICE_ERROR;
  }
  if ((OpenMode & EFI_FILE_MODE_CREATE) != 0) gCreateOpenCalls++;
  if ((Attributes & EFI_FILE_DIRECTORY) != 0) {
    if (PathEq(FileName, L"\\EFI")) {
      gEfiDirectoryExists = TRUE;
      gDirectoryOpenCalls++;
      *NewHandle = &gDir;
      return EFI_SUCCESS;
    }
    if (PathEq(FileName, L"\\EFI\\BOOT")) {
      if (!gEfiDirectoryExists) return EFI_NOT_FOUND;
      gDirectoryOpenCalls++;
      *NewHandle = &gDir;
      return EFI_SUCCESS;
    }
    return EFI_NOT_FOUND;
  }
  if (PathEq(FileName, L"\\EFI\\BOOT\\BLS.plist")) {
    if (gFinalData == NULL) return EFI_NOT_FOUND;
    gFinalPos = 0U;
    *NewHandle = &gFinalFile;
    return EFI_SUCCESS;
  }
  if (PathEq(FileName, L"\\EFI\\BOOT\\BLS.tmp")) {
    if (OpenMode == EFI_FILE_MODE_READ) {
      gTmpReadOnlyOpens++;
      if (gTmpReadStatus != EFI_SUCCESS) return gTmpReadStatus;
    } else if (gTmpCleanupStatus != EFI_SUCCESS) {
      return gTmpCleanupStatus;
    }
    if (gTmpData == NULL) return EFI_NOT_FOUND;
    gTmpPos = 0U;
    *NewHandle = &gTmpFile;
    return EFI_SUCCESS;
  }
  return EFI_NOT_FOUND;
}

static EFI_STATUS EFIAPI TestOpenVolume(IN EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *This, OUT EFI_FILE_PROTOCOL **Root) {
  (void)This;
  if (Root == NULL) return EFI_INVALID_PARAMETER;
  *Root = &gRoot;
  return gOpenVolumeErrorWithOut ? EFI_DEVICE_ERROR : EFI_SUCCESS;
}

static EFI_STATUS EFIAPI TestOpenProtocol(
  IN EFI_HANDLE Handle, IN EFI_GUID *Protocol, OUT VOID **Interface,
  IN EFI_HANDLE AgentHandle, IN EFI_HANDLE ControllerHandle, IN UINT32 Attributes
  ) {
  if (Interface == NULL) return EFI_INVALID_PARAMETER;
  *Interface = NULL;
  assert(AgentHandle == gImageHandle);
  assert(ControllerHandle == NULL);
  assert(Attributes == EFI_OPEN_PROTOCOL_GET_PROTOCOL);
  if (Handle == gTestImageHandle && Protocol == &gEfiLoadedImageProtocolGuid) { *Interface = &gLoadedImage; return EFI_SUCCESS; }
  if (Handle == gVolumeHandle && Protocol == &gEfiSimpleFileSystemProtocolGuid) { *Interface = &gFs; return EFI_SUCCESS; }
  return EFI_UNSUPPORTED;
}

EFI_STATUS BlsLocateFsHandles(OUT EFI_HANDLE **Handles, OUT UINTN *Count);
EFI_STATUS BlsLocateFsHandles(OUT EFI_HANDLE **Handles, OUT UINTN *Count) {
  if (Handles == NULL || Count == NULL) return EFI_INVALID_PARAMETER;
  *Handles = NULL;
  *Count = 0U;
  return EFI_NOT_FOUND;
}

#include "../../BootLoaderSelectorPkg/BootLoaderSelector/Config.c"
#include "../../BootLoaderSelectorPkg/BootLoaderSelector/Utils.c"

static VOID ResetFakeFs(VOID) {
  memset(&gBootServices, 0, sizeof(gBootServices));
  memset(&gFs, 0, sizeof(gFs));
  memset(&gLoadedImage, 0, sizeof(gLoadedImage));
  memset(&gRoot, 0, sizeof(gRoot));
  memset(&gDir, 0, sizeof(gDir));
  memset(&gFinalFile, 0, sizeof(gFinalFile));
  memset(&gTmpFile, 0, sizeof(gTmpFile));
  gBootServices.OpenProtocol = TestOpenProtocol;
  gFs.OpenVolume = TestOpenVolume;
  gLoadedImage.DeviceHandle = gVolumeHandle;
  gRoot.Open = TestRootOpen;
  gRoot.Close = TestFileClose;
  gDir.Close = TestFileClose;
  gFinalFile.GetInfo = TestFileGetInfo;
  gFinalFile.SetPosition = TestFileSetPosition;
  gFinalFile.Read = TestFileRead;
  gFinalFile.Close = TestFileClose;
  gFinalFile.Delete = TestFileDelete;
  gTmpFile.GetInfo = TestFileGetInfo;
  gTmpFile.SetPosition = TestFileSetPosition;
  gTmpFile.Read = TestFileRead;
  gTmpFile.Close = TestFileClose;
  gTmpFile.Delete = TestFileDelete;
  gBS = &gBootServices;
  gImageHandle = gTestImageHandle;
  gFinalData = NULL;
  gTmpData = NULL;
  gFinalPos = 0U;
  gTmpPos = 0U;
  gTmpReadStatus = EFI_NOT_FOUND;
  gTmpCleanupStatus = EFI_NOT_FOUND;
  gTmpReadOnlyOpens = 0U;
  gCreateOpenCalls = 0U;
  gDirectoryOpenCalls = 0U;
  gEfiDirectoryExists = FALSE;
  gDeleteCalls = 0U;
  gCloseCalls = 0U;
  gOpenVolumeErrorWithOut = FALSE;
  gFileOpenErrorWithOut = FALSE;
}

int main(void) {
  static CONST CHAR8 valid[] =
    "<plist version=\"1.0\"><dict>"
    "<key>DefaultBootloader</key><string>OpenCore</string>"
    "<key>DefaultPath</key><string>\\EFI\\OC\\OpenCore.efi</string>"
    "<key>DefaultVolumeLabel</key><string></string>"
    "<key>DefaultVolumeDevicePath</key><string></string>"
    "<key>Timeout</key><integer>7</integer>"
    "<key>Debug</key><false/>"
    "<key>ShowHardwareInfo</key><true/>"
    "<key>RememberLastSelected</key><true/>"
    "<key>HasRememberedSelection</key><false/>"
    "</dict></plist>";
  static CONST CHAR8 invalidUnknown[] =
    "<plist version=\"1.0\"><dict>"
    "<key>DefaultBootloader</key><string>OpenCore</string>"
    "<key>CustomUserKey</key><string>keep-me</string>"
    "</dict></plist>";
  BLS_CONFIG config;
  EFI_HANDLE configVolume;
  EFI_STATUS status;

  ResetFakeFs();
  gFinalData = valid;
  gTmpData = valid;
  gTmpReadStatus = EFI_ACCESS_DENIED;
  gTmpCleanupStatus = EFI_ACCESS_DENIED;
  BlsConfigDefaults(&config);
  status = BlsLoadConfigFromVolume(gVolumeHandle, &config);
  assert(status == EFI_SUCCESS);
  assert(config.Timeout == 7U);
  assert(gTmpReadOnlyOpens == 0U); /* valid final never reads staging */

  ResetFakeFs();
  gFinalData = invalidUnknown;
  gTmpData = NULL;
  gTmpReadStatus = EFI_NOT_FOUND;
  gTmpCleanupStatus = EFI_NOT_FOUND;
  configVolume = (EFI_HANDLE)(UINTN)0xDEADU;
  status = BlsLoadConfig(gTestImageHandle, &config, &configVolume);
  assert(status == EFI_LOAD_ERROR);
  assert(configVolume == NULL);
  assert(config.Timeout == 10U); /* safe session defaults */
  assert(gCreateOpenCalls == 0U); /* invalid plist was not rewritten */
  assert(gDeleteCalls == 0U);     /* invalid plist was not deleted */


  /* UEFI Open() resolves path components in sequence. A fresh/custom ESP may
     not have \EFI yet, so config persistence must create the parent before
     attempting \EFI\BOOT; opening only the nested path is not recursive. */
  ResetFakeFs();
  status = BlsEnsureConfigDirectoryHierarchy(&gRoot);
  assert(status == EFI_SUCCESS);
  assert(gDirectoryOpenCalls == 2U);
  assert(gEfiDirectoryExists == TRUE);

  /* OpenVolume()/Open() OUT handles become caller-owned only on success. A
     failing firmware implementation that scribbles a handle into the OUT
     argument must not make our error cleanup close an unowned object. */
  {
    EFI_FILE_PROTOCOL *root;
    EFI_FILE_PROTOCOL *file;

    ResetFakeFs();
    gOpenVolumeErrorWithOut = TRUE;
    root = (EFI_FILE_PROTOCOL *)(UINTN)1;
    status = BlsOpenVolumeRoot(gVolumeHandle, &root);
    assert(status == EFI_DEVICE_ERROR);
    assert(root == NULL);
    assert(gCloseCalls == 0U);

    ResetFakeFs();
    gFileOpenErrorWithOut = TRUE;
    file = (EFI_FILE_PROTOCOL *)(UINTN)1;
    status = BlsFileOpen(&gRoot, &file, L"\\EFI\\BOOT\\BLS.tmp", EFI_FILE_MODE_READ, 0U);
    assert(status == EFI_DEVICE_ERROR);
    assert(file == NULL);
    assert(gCloseCalls == 0U);
  }

  /* Enforce the exact EFI_FILE_PROTOCOL.Open() contract locally instead of
     relying on every firmware to reject reserved mode/attribute combinations. */
  {
    EFI_FILE_PROTOCOL *file;

    ResetFakeFs();
    file = (EFI_FILE_PROTOCOL *)(UINTN)1;
    status = BlsFileOpen(&gRoot, &file, L"\\EFI\\BOOT\\BLS.tmp", EFI_FILE_MODE_WRITE, 0U);
    assert(status == EFI_INVALID_PARAMETER);
    assert(file == NULL);
    assert(gCreateOpenCalls == 0U);

    file = (EFI_FILE_PROTOCOL *)(UINTN)1;
    status = BlsFileOpen(&gRoot, &file, L"\\EFI\\BOOT\\BLS.tmp", EFI_FILE_MODE_READ, EFI_FILE_HIDDEN);
    assert(status == EFI_INVALID_PARAMETER);
    assert(file == NULL);

    file = (EFI_FILE_PROTOCOL *)(UINTN)1;
    status = BlsFileOpen(&gRoot, &file, L"\\EFI\\BOOT\\BLS.tmp",
                         EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
                         EFI_FILE_VALID_ATTR + 1U);
    assert(status == EFI_INVALID_PARAMETER);
    assert(file == NULL);
  }

  puts("config_lifecycle: PASS");
  return 0;
}

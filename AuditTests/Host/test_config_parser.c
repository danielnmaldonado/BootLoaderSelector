/* Credits: ©DNMTechLabs / dnmtechlabs@gmail.com */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef NULL
#undef NULL
#endif
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>

EFI_HANDLE gImageHandle = NULL;
EFI_SYSTEM_TABLE *gST = NULL;
EFI_BOOT_SERVICES *gBS = NULL;

static UINTN gAllocationCalls;
static UINTN gFailAllocationAt;

VOID *EFIAPI AllocateZeroPool(IN UINTN AllocationSize) {
  gAllocationCalls++;
  if (gFailAllocationAt != 0U && gAllocationCalls == gFailAllocationAt) return NULL;
  return calloc(1, (size_t)AllocationSize);
}

VOID EFIAPI FreePool(IN JCONST VOID *Buffer) {
  free((void *)Buffer);
}

VOID *EFIAPI CopyMem(OUT VOID *DestinationBuffer, IN CONST VOID *SourceBuffer, IN UINTN Length) {
  return memcpy(DestinationBuffer, SourceBuffer, (size_t)Length);
}

VOID *EFIAPI ZeroMem(OUT VOID *Buffer, IN UINTN Length) {
  return memset(Buffer, 0, (size_t)Length);
}

VOID *EFIAPI SetMem(OUT VOID *Buffer, IN UINTN Length, IN UINT8 Value) {
  return memset(Buffer, Value, (size_t)Length);
}

INTN EFIAPI CompareMem(IN CONST VOID *DestinationBuffer, IN CONST VOID *SourceBuffer, IN UINTN Length) {
  return (INTN)memcmp(DestinationBuffer, SourceBuffer, (size_t)Length);
}

UINTN EFIAPI AsciiStrLen(IN CONST CHAR8 *String) {
  return (UINTN)strlen(String);
}

INTN EFIAPI AsciiStrnCmp(IN CONST CHAR8 *FirstString, IN CONST CHAR8 *SecondString, IN UINTN Length) {
  return (INTN)strncmp(FirstString, SecondString, (size_t)Length);
}

CHAR8 *EFIAPI AsciiStrStr(IN CONST CHAR8 *String, IN CONST CHAR8 *SearchString) {
  return (CHAR8 *)strstr(String, SearchString);
}

UINTN EFIAPI AsciiSPrint(
  OUT CHAR8 *StartOfBuffer,
  IN UINTN BufferSize,
  IN CONST CHAR8 *FormatString,
  ...
  ) {
  VA_LIST marker;
  CONST CHAR8 *value;
  int written;

  if (StartOfBuffer == NULL || BufferSize == 0 || FormatString == NULL) return 0;
  if (strcmp(FormatString, "<key>%a</key>") != 0) {
    StartOfBuffer[0] = '\0';
    return 0;
  }
  VA_START(marker, FormatString);
  value = VA_ARG(marker, CONST CHAR8 *);
  VA_END(marker);
  written = snprintf(StartOfBuffer, (size_t)BufferSize, "<key>%s</key>", value);
  if (written < 0) {
    StartOfBuffer[0] = '\0';
    return 0;
  }
  return ((UINTN)written < BufferSize) ? (UINTN)written : BufferSize - 1U;
}

#include "../../BootLoaderSelectorPkg/BootLoaderSelector/Config.c"
#include "../../BootLoaderSelectorPkg/BootLoaderSelector/Utils.c"

static UINT32 NextRandom(IN OUT UINT32 *State) {
  UINT32 value;
  value = *State;
  value ^= (UINT32)((UINT64)value << 13);
  value ^= value >> 17;
  value ^= (UINT32)((UINT64)value << 5);
  *State = value;
  return value;
}

int main(void) {
  static CONST CHAR8 valid[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
    "<plist version=\"1.0\"><dict>"
    "<key>DefaultBootloader</key><string>OpenCore</string>"
    "<key>DefaultPath</key><string>\\EFI\\OC\\OpenCore.efi</string>"
    "<key>DefaultVolumeLabel</key><string>EFI</string>"
    "<key>DefaultVolumeDevicePath</key><string>PciRoot(0x0)</string>"
    "<key>Timeout</key><integer>10</integer>"
    "<key>Debug</key><false/>"
    "<key>ShowHardwareInfo</key><true/>"
    "<key>RememberLastSelected</key><true/>"
    "<key>HasRememberedSelection</key><true/>"
    "</dict></plist>";
  static CONST CHAR8 duplicateTimeout[] =
    "<plist><dict><key>DefaultBootloader</key><string>OpenCore</string>"
    "<key>Timeout</key><integer>10</integer><key>Timeout</key><integer>5</integer>"
    "</dict></plist>";
  static CONST CHAR8 nestedKnownKey[] =
    "<plist><dict><key>Unknown</key><dict><key>Timeout</key><integer>1</integer></dict>"
    "<key>DefaultBootloader</key><string>OpenCore</string></dict></plist>";
  static CONST CHAR8 unknownTopLevel[] =
    "<plist><dict><key>DefaultBootloader</key><string>OpenCore</string>"
    "<key>CustomUserKey</key><string>preserve-me</string></dict></plist>";
  static CONST CHAR8 trailingData[] =
    "<plist><dict><key>DefaultBootloader</key><string>OpenCore</string></dict></plist><extra/>";
  CHAR8 malformedUtf8[] =
    "<plist><dict><key>DefaultBootloader</key><string>\xF0\x28\x8C\x28</string></dict></plist>";
  CHAR8 supplementaryUtf8[] =
    "<plist><dict><key>DefaultBootloader</key><string>Open\xF0\x9F\x98\x80"
    "Core</string></dict></plist>";
  static CONST CHAR8 supplementaryEntity[] =
    "<plist><dict><key>DefaultBootloader</key><string>Open&#x1F600;Core</string></dict></plist>";
  BLS_CONFIG config;
  UINT32 randomState;
  UINTN iteration;
  CHAR8 fuzz[2049];

  BlsConfigDefaults(&config);
  assert(BlsParseConfig(valid, &config) == EFI_SUCCESS);
  assert(config.Timeout == 10U);
  assert(config.Debug == FALSE);
  assert(config.ShowHardwareInfo == TRUE);
  assert(config.RememberLastSelected == TRUE);
  assert(config.HasRememberedSelection == TRUE);
  assert(BlsStrEq(config.DefaultBootloader, L"OpenCore"));
  assert(BlsStrEq(config.DefaultPath, L"\\EFI\\OC\\OpenCore.efi"));

  /* XML comments and processing instructions are markup, not character data.
     Preserve scalar semantics instead of converting ignored markup into spaces. */
  {
    static CONST CHAR8 commentedScalar[] =
      "<plist><dict><key>DefaultBootloader</key><string>Open<!--ignored-->Core</string></dict></plist>";
    static CONST CHAR8 processingInstructionScalar[] =
      "<plist><dict><key>DefaultBootloader</key><string>Open<?bls ignored?>Core</string></dict></plist>";

    BlsConfigDefaults(&config);
    assert(BlsParseConfig(commentedScalar, &config) == EFI_SUCCESS);
    assert(BlsStrEq(config.DefaultBootloader, L"OpenCore"));
    BlsConfigDefaults(&config);
    assert(BlsParseConfig(processingInstructionScalar, &config) == EFI_SUCCESS);
    assert(BlsStrEq(config.DefaultBootloader, L"OpenCore"));

    /* XML comments may not contain "--" in their body. */
    BlsConfigDefaults(&config);
    assert(BlsParseConfig(
      "<plist><dict><key>DefaultBootloader</key><string>Open<!--bad--comment-->Core</string></dict></plist>",
      &config
    ) == EFI_LOAD_ERROR);
  }

  BlsConfigDefaults(&config);
  assert(BlsParseConfig(duplicateTimeout, &config) == EFI_LOAD_ERROR);
  BlsConfigDefaults(&config);
  assert(BlsParseConfig(nestedKnownKey, &config) == EFI_LOAD_ERROR);
  BlsConfigDefaults(&config);
  assert(BlsParseConfig(unknownTopLevel, &config) == EFI_LOAD_ERROR);
  BlsConfigDefaults(&config);
  assert(BlsParseConfig(trailingData, &config) == EFI_LOAD_ERROR);
  BlsConfigDefaults(&config);
  assert(BlsParseConfig(malformedUtf8, &config) == EFI_LOAD_ERROR);
  /* UEFI CHAR16 is UCS-2 and must not contain UTF-16 surrogate pairs. A valid
     supplementary UTF-8 scalar is therefore invalid selector configuration. */
  BlsConfigDefaults(&config);
  assert(BlsParseConfig(supplementaryUtf8, &config) == EFI_LOAD_ERROR);
  BlsConfigDefaults(&config);
  assert(BlsParseConfig(supplementaryEntity, &config) == EFI_LOAD_ERROR);

  /* Allocation failure is an operational resource error, not malformed
     configuration. Preserve that distinction so callers do not silently
     fall back to a stale staging file under memory pressure. */
  gAllocationCalls = 0U;
  gFailAllocationAt = 1U;
  BlsConfigDefaults(&config);
  assert(BlsParseConfig(valid, &config) == EFI_OUT_OF_RESOURCES);
  assert(BlsConfigReadMayFallback(EFI_OUT_OF_RESOURCES) == FALSE);
  assert(BlsConfigReadMayFallback(EFI_LOAD_ERROR) == TRUE);
  gFailAllocationAt = 0U;

  /* The writer follows both the XML 1.0 character contract and UEFI UCS-2.
     Reject firmware-originated controls/noncharacters and surrogate code units
     before persistence, while preserving the three legal XML controls. */
  {
    CHAR8 escaped[128];
    CHAR16 invalidControl[] = {L'A', 0x0001U, L'B', L'\0'};
    CHAR16 invalidVerticalTab[] = {L'A', 0x000BU, L'B', L'\0'};
    CHAR16 invalidFffe[] = {L'A', 0xFFFEU, L'B', L'\0'};
    CHAR16 invalidFfff[] = {L'A', 0xFFFFU, L'B', L'\0'};
    CHAR16 validControls[] = {L'A', 0x0009U, 0x000AU, 0x000DU, L'B', L'\0'};
    CHAR16 invalidSupplementary[] = {L'A', 0xD83DU, 0xDE00U, L'B', L'\0'};

    assert(BlsUnicodeToUtf8XmlEscape(escaped, sizeof(escaped), invalidControl) == FALSE);
    assert(BlsUnicodeToUtf8XmlEscape(escaped, sizeof(escaped), invalidVerticalTab) == FALSE);
    assert(BlsUnicodeToUtf8XmlEscape(escaped, sizeof(escaped), invalidFffe) == FALSE);
    assert(BlsUnicodeToUtf8XmlEscape(escaped, sizeof(escaped), invalidFfff) == FALSE);
    assert(BlsUnicodeToUtf8XmlEscape(escaped, sizeof(escaped), validControls) == TRUE);
    assert(BlsUnicodeToUtf8XmlEscape(escaped, sizeof(escaped), invalidSupplementary) == FALSE);
  }

  {
    CHAR8 nested[4096];
    UINTN depth;

    strcpy(nested, "<dict><key>Debug</key>");
    for (depth = 0U; depth < BLS_XML_MAX_DEPTH; depth++) strcat(nested, "<array>");
    strcat(nested, "<true/>");
    for (depth = 0U; depth < BLS_XML_MAX_DEPTH; depth++) strcat(nested, "</array>");
    strcat(nested, "</dict>");
    assert(BlsValidateAndSanitizeRootDict(nested) == FALSE);
  }

  {
    BLS_BOOT_ENTRY entry;

    ZeroMem(&entry, sizeof(entry));
    BlsConfigDefaults(&config);
    BlsSafeStrCpy(config.DefaultBootloader, BLS_MAX_TEXT, L"OpenCore");
    BlsSafeStrCpy(config.DefaultPath, BLS_MAX_PATH, L"\\EFI\\OC\\OpenCore.efi");
    BlsSafeStrCpy(config.DefaultVolumeLabel, BLS_MAX_TEXT, L"EFI");
    BlsSafeStrCpy(config.DefaultVolumeDevicePath, BLS_MAX_VOLUME_ID, L"GPT-STRONG-ID");
    BlsSafeStrCpy(entry.Name, BLS_MAX_TEXT, L"OpenCore");
    BlsSafeStrCpy(entry.Path, BLS_MAX_PATH, L"\\EFI\\OC\\OpenCore.efi");
    BlsSafeStrCpy(entry.VolumeLabel, BLS_MAX_TEXT, L"EFI");

    /* Persisted strong identity is fail-closed: a matching label is not enough
       when the current volume could not expose its strong identity. */
    assert(BlsConfigMatchesEntry(&config, &entry) == FALSE);
    BlsSafeStrCpy(entry.VolumeDevicePath, BLS_MAX_VOLUME_ID, L"OTHER-STRONG-ID");
    assert(BlsConfigMatchesEntry(&config, &entry) == FALSE);
    BlsSafeStrCpy(entry.VolumeDevicePath, BLS_MAX_VOLUME_ID, L"GPT-STRONG-ID");
    assert(BlsConfigMatchesEntry(&config, &entry) == TRUE);
  }

  randomState = 0x9E3779B9U;
  for (iteration = 0; iteration < 200000U; iteration++) {
    UINTN length;
    UINTN index;

    length = (UINTN)(NextRandom(&randomState) % (ARRAY_SIZE(fuzz) - 1U));
    for (index = 0; index < length; index++) {
      fuzz[index] = (CHAR8)(NextRandom(&randomState) & 0x7FU);
      if (fuzz[index] == '\0') fuzz[index] = 'X';
    }
    fuzz[length] = '\0';
    BlsConfigDefaults(&config);
    (void)BlsParseConfig(fuzz, &config);
  }

  puts("config_parser: PASS");
  return 0;
}

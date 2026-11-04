/** @file
  BootLoaderSelector configuration.
  Credits: ©DNMTechLabs / dnmtechlabs@gmail.com
**/
#include "Config.h"
#include "BootScan.h"
#include "Utils.h"
#include <Library/BaseMemoryLib.h>
#include <Library/BaseLib.h>
#include <Library/PrintLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>
#include <Guid/FileInfo.h>

#define BLS_CONFIG_PATH L"\\EFI\\BOOT\\BLS.plist"
#define BLS_CONFIG_TMP  L"\\EFI\\BOOT\\BLS.tmp"
#define BLS_CONFIG_DIR  L"\\EFI\\BOOT"
#define BLS_CONFIG_MAX       65536
#define BLS_CONFIG_WRITE_MAX 8192
#define BLS_XML_MAX_DEPTH    32U
#define BLS_FILE_INFO_MIN_SIZE (SIZE_OF_EFI_FILE_INFO + sizeof(CHAR16))

typedef struct {
  CHAR8 Buffer[BLS_CONFIG_WRITE_MAX];
  CHAR8 EscapedDefaultName[BLS_MAX_TEXT * 6];
  CHAR8 EscapedDefaultPath[BLS_MAX_PATH * 6];
  CHAR8 EscapedDefaultVolumeLabel[BLS_MAX_TEXT * 6];
  CHAR8 EscapedDefaultVolumeDevicePath[BLS_MAX_VOLUME_ID * 6];
} BLS_CONFIG_WRITE_CONTEXT;

static BOOLEAN BlsConfigReadMayFallback(IN EFI_STATUS Status) {
  return (BOOLEAN)(Status == EFI_NOT_FOUND || Status == EFI_LOAD_ERROR);
}

static VOID BlsMergeConfigReadCleanupStatus(IN OUT EFI_STATUS *Status, IN EFI_STATUS CleanupStatus) {
  if (Status == NULL || CleanupStatus == EFI_SUCCESS) return;
  if (*Status == EFI_SUCCESS || BlsConfigReadMayFallback(*Status)) {
    *Status = CleanupStatus;
  }
}

static BOOLEAN BlsAsciiIsSpace(IN CHAR8 Ch) {
  return (Ch == ' ' || Ch == '\t' || Ch == '\r' || Ch == '\n');
}

static CONST CHAR8 *BlsSkipAsciiSpace(IN CONST CHAR8 *Start, IN CONST CHAR8 *End) {
  if (Start == NULL || End == NULL || Start > End) return NULL;
  while (Start < End && BlsAsciiIsSpace(*Start)) Start++;
  return Start;
}

VOID BlsConfigDefaults(OUT BLS_CONFIG *Config) {
  if (Config == NULL) return;
  ZeroMem(Config, sizeof(*Config));
  BlsSafeStrCpy(Config->DefaultBootloader, BLS_MAX_TEXT, L"OpenCore");
  Config->Timeout = 10;
  Config->Debug = FALSE;
  Config->ShowHardwareInfo = TRUE;
  Config->RememberLastSelected = TRUE;
  Config->HasRememberedSelection = FALSE;
  Config->ForceRewrite = FALSE;
}

BOOLEAN BlsConfigMatchesEntry(IN CONST BLS_CONFIG *Config, IN CONST BLS_BOOT_ENTRY *Entry) {
  if (Config == NULL || Entry == NULL) return FALSE;
  if (!BlsStrEq(Config->DefaultBootloader, Entry->Name)) return FALSE;
  if (Config->DefaultPath[0] != L'\0' && !BlsStrEq(Config->DefaultPath, Entry->Path)) return FALSE;

  if (Config->DefaultVolumeDevicePath[0] != L'\0') {
    /* A persisted strong volume identity must never degrade to a label-only
       match. If the current entry cannot expose the same strong identity, fail
       closed and leave the selector interactive instead of risking autoboot on
       a different volume that happens to share the same label. */
    if (Entry->VolumeDevicePath[0] == L'\0') return FALSE;
    return BlsStrEq(Config->DefaultVolumeDevicePath, Entry->VolumeDevicePath);
  }

  if (Config->DefaultVolumeLabel[0] != L'\0') {
    return BlsStrEq(Config->DefaultVolumeLabel, Entry->VolumeLabel);
  }

  return TRUE;
}

BOOLEAN BlsConfigRememberedEntryNeedsUpdate(IN CONST BLS_CONFIG *Config, IN CONST BLS_BOOT_ENTRY *Entry) {
  if (Config == NULL || Entry == NULL) return TRUE;
  if (!Config->HasRememberedSelection) return TRUE;
  if (!BlsStrEq(Config->DefaultBootloader, Entry->Name)) return TRUE;
  if (!BlsStrEq(Config->DefaultPath, Entry->Path)) return TRUE;
  if (!BlsStrEq(Config->DefaultVolumeLabel, Entry->VolumeLabel)) return TRUE;
  if (!BlsStrEq(Config->DefaultVolumeDevicePath, Entry->VolumeDevicePath)) return TRUE;
  return FALSE;
}

VOID BlsConfigRememberEntry(IN OUT BLS_CONFIG *Config, IN CONST BLS_BOOT_ENTRY *Entry) {
  if (Config == NULL || Entry == NULL) return;
  BlsSafeStrCpy(Config->DefaultBootloader, BLS_MAX_TEXT, Entry->Name);
  BlsSafeStrCpy(Config->DefaultPath, BLS_MAX_PATH, Entry->Path);
  BlsSafeStrCpy(Config->DefaultVolumeLabel, BLS_MAX_TEXT, Entry->VolumeLabel);
  BlsSafeStrCpy(Config->DefaultVolumeDevicePath, BLS_MAX_VOLUME_ID, Entry->VolumeDevicePath);
  Config->HasRememberedSelection = TRUE;
}

static EFI_STATUS BlsReadWholeFile(IN EFI_HANDLE VolumeHandle, IN CONST CHAR16 *Path, OUT CHAR8 **Buffer, OUT UINTN *Size) {
  EFI_STATUS Status;
  EFI_STATUS CloseStatus;
  EFI_FILE_PROTOCOL *Root;
  EFI_FILE_PROTOCOL *File;
  EFI_FILE_INFO *Info;
  UINTN InfoSize;
  UINTN ExpectedSize;
  UINTN Index;
  CHAR8 *Data;

  if (Buffer == NULL || Size == NULL || Path == NULL) return EFI_INVALID_PARAMETER;
  *Buffer = NULL;
  *Size = 0;
  Root = NULL;
  File = NULL;
  Info = NULL;
  Data = NULL;
  ExpectedSize = 0;

  Status = BlsOpenVolumeRoot(VolumeHandle, &Root);
  if (Status != EFI_SUCCESS) return Status;
  if (Root == NULL) return EFI_DEVICE_ERROR;

  Status = BlsFileOpen(Root, &File, Path, EFI_FILE_MODE_READ, 0);
  if (Status != EFI_SUCCESS) {
    CloseStatus = BlsFileClose(&Root);
    BlsMergeConfigReadCleanupStatus(&Status, CloseStatus);
    return Status;
  }

  InfoSize = 0;
  Status = BlsFileGetInfoAlloc(
             File,
             &gEfiFileInfoGuid,
             BLS_FILE_INFO_MIN_SIZE,
             (VOID **)&Info,
             &InfoSize
             );

  if (Status == EFI_SUCCESS) {
    if (Info == NULL || Info->Size < BLS_FILE_INFO_MIN_SIZE || Info->Size > InfoSize) {
      Status = EFI_COMPROMISED_DATA;
    } else if ((Info->Attribute & EFI_FILE_DIRECTORY) != 0 ||
               Info->FileSize == 0 || Info->FileSize > BLS_CONFIG_MAX) {
      Status = EFI_LOAD_ERROR;
    } else {
      ExpectedSize = (UINTN)Info->FileSize;
      Data = AllocateZeroPool(ExpectedSize + 1);
      if (Data == NULL) {
        Status = EFI_OUT_OF_RESOURCES;
      } else if (File->SetPosition == NULL || File->Read == NULL) {
        Status = EFI_UNSUPPORTED;
      } else {
        Status = File->SetPosition(File, 0);
        if (Status == EFI_SUCCESS) {
          Status = BlsFileReadExact(File, Data, ExpectedSize);
        }
        if (Status == EFI_SUCCESS) {
          UINT8 ExtraByte;
          UINTN ExtraSize;

          ExtraByte = 0;
          ExtraSize = sizeof(ExtraByte);
          Status = File->Read(File, &ExtraSize, &ExtraByte);
          if (Status == EFI_SUCCESS && ExtraSize != 0) {
            Status = EFI_LOAD_ERROR;
          }
        }
        if (Status == EFI_SUCCESS) {
          for (Index = 0; Index < ExpectedSize; Index++) {
            if (Data[Index] == '\0') {
              Status = EFI_LOAD_ERROR;
              break;
            }
          }
        }
      }
    }
  }

  if (Info != NULL) FreePool(Info);
  CloseStatus = BlsFileClose(&File);
  BlsMergeConfigReadCleanupStatus(&Status, CloseStatus);
  CloseStatus = BlsFileClose(&Root);
  BlsMergeConfigReadCleanupStatus(&Status, CloseStatus);

  if (Status != EFI_SUCCESS) {
    if (Data != NULL) FreePool(Data);
    return Status;
  }

  Data[ExpectedSize] = '\0';
  *Buffer = Data;
  *Size = ExpectedSize;
  return EFI_SUCCESS;
}

static BOOLEAN BlsXmlCodePointIsValid(IN UINT32 CodePoint) {
  if (CodePoint == 0x09 || CodePoint == 0x0A || CodePoint == 0x0D) return TRUE;
  if (CodePoint >= 0x20 && CodePoint <= 0xD7FF) return TRUE;
  if (CodePoint >= 0xE000 && CodePoint <= 0xFFFD) return TRUE;
  if (CodePoint >= 0x10000 && CodePoint <= 0x10FFFF) return TRUE;
  return FALSE;
}

static BOOLEAN BlsAppendCodePoint(
  OUT CHAR16 *Out,
  IN UINTN OutChars,
  IN OUT UINTN *OutIndex,
  IN UINT32 CodePoint
  ) {
  UINTN Index;

  if (Out == NULL || OutChars == 0 || OutIndex == NULL ||
      !BlsXmlCodePointIsValid(CodePoint) || CodePoint > 0xFFFFU) {
    return FALSE;
  }

  /* UEFI CHAR16 strings are UCS-2, not UTF-16. Supplementary Unicode scalar
     values would require surrogate pairs, which UEFI explicitly does not
     support. Reject them while parsing instead of storing invalid CHAR16 data
     that could later reach firmware file/path or console protocols. */
  Index = *OutIndex;
  if (Index + 1U >= OutChars) return FALSE;
  Out[Index++] = (CHAR16)CodePoint;

  *OutIndex = Index;
  return TRUE;
}

static BOOLEAN BlsDecodeUtf8CodePoint(
  IN CONST CHAR8 *Text,
  IN UINTN Remaining,
  OUT UINT32 *CodePoint,
  OUT UINTN *Consumed
  ) {
  UINT8 B0;
  UINT8 B1;
  UINT8 B2;
  UINT8 B3;

  if (Text == NULL || Remaining == 0 || CodePoint == NULL || Consumed == NULL) return FALSE;
  B0 = (UINT8)Text[0];

  if (B0 < 0x80) {
    *CodePoint = B0;
    *Consumed = 1;
    return TRUE;
  }

  if (B0 >= 0xC2 && B0 <= 0xDF && Remaining >= 2) {
    B1 = (UINT8)Text[1];
    if ((B1 & 0xC0) != 0x80) return FALSE;
    *CodePoint = ((UINT32)(B0 & 0x1F) << 6) | (UINT32)(B1 & 0x3F);
    *Consumed = 2;
    return TRUE;
  }

  if (B0 >= 0xE0 && B0 <= 0xEF && Remaining >= 3) {
    B1 = (UINT8)Text[1];
    B2 = (UINT8)Text[2];
    if ((B1 & 0xC0) != 0x80 || (B2 & 0xC0) != 0x80) return FALSE;
    if (B0 == 0xE0 && B1 < 0xA0) return FALSE;
    if (B0 == 0xED && B1 >= 0xA0) return FALSE;
    *CodePoint = ((UINT32)(B0 & 0x0F) << 12) |
                 ((UINT32)(B1 & 0x3F) << 6) |
                 (UINT32)(B2 & 0x3F);
    *Consumed = 3;
    return TRUE;
  }

  if (B0 >= 0xF0 && B0 <= 0xF4 && Remaining >= 4) {
    B1 = (UINT8)Text[1];
    B2 = (UINT8)Text[2];
    B3 = (UINT8)Text[3];
    if ((B1 & 0xC0) != 0x80 || (B2 & 0xC0) != 0x80 || (B3 & 0xC0) != 0x80) return FALSE;
    if (B0 == 0xF0 && B1 < 0x90) return FALSE;
    if (B0 == 0xF4 && B1 >= 0x90) return FALSE;
    *CodePoint = ((UINT32)(B0 & 0x07) << 18) |
                 ((UINT32)(B1 & 0x3F) << 12) |
                 ((UINT32)(B2 & 0x3F) << 6) |
                 (UINT32)(B3 & 0x3F);
    *Consumed = 4;
    return TRUE;
  }

  return FALSE;
}

static BOOLEAN BlsDecodeXmlEntity(
  IN CONST CHAR8 *Text,
  IN CONST CHAR8 *End,
  OUT UINT32 *CodePoint,
  OUT UINTN *Consumed
  ) {
  UINTN Remaining;

  if (Text == NULL || End == NULL || Text >= End || *Text != '&' || CodePoint == NULL || Consumed == NULL) return FALSE;
  Remaining = (UINTN)(End - Text);

  if (Remaining >= 5 && AsciiStrnCmp(Text, "&amp;", 5) == 0) {
    *CodePoint = '&';
    *Consumed = 5;
    return TRUE;
  }
  if (Remaining >= 4 && AsciiStrnCmp(Text, "&lt;", 4) == 0) {
    *CodePoint = '<';
    *Consumed = 4;
    return TRUE;
  }
  if (Remaining >= 4 && AsciiStrnCmp(Text, "&gt;", 4) == 0) {
    *CodePoint = '>';
    *Consumed = 4;
    return TRUE;
  }
  if (Remaining >= 6 && AsciiStrnCmp(Text, "&quot;", 6) == 0) {
    *CodePoint = '"';
    *Consumed = 6;
    return TRUE;
  }
  if (Remaining >= 6 && AsciiStrnCmp(Text, "&apos;", 6) == 0) {
    *CodePoint = '\'';
    *Consumed = 6;
    return TRUE;
  }

  if (Remaining >= 4 && Text[1] == '#') {
    UINTN Index;
    UINT32 Base;
    UINT32 Value;
    BOOLEAN HasDigit;

    Index = 2;
    Base = 10;
    if (Index < Remaining && (Text[Index] == 'x' || Text[Index] == 'X')) {
      Base = 16;
      Index++;
    }

    Value = 0;
    HasDigit = FALSE;
    while (Index < Remaining && Text[Index] != ';') {
      UINT32 Digit;
      CHAR8 Ch;

      Ch = Text[Index];
      if (Ch >= '0' && Ch <= '9') {
        Digit = (UINT32)(Ch - '0');
      } else if (Base == 16 && Ch >= 'a' && Ch <= 'f') {
        Digit = (UINT32)(Ch - 'a' + 10);
      } else if (Base == 16 && Ch >= 'A' && Ch <= 'F') {
        Digit = (UINT32)(Ch - 'A' + 10);
      } else {
        return FALSE;
      }

      if (Digit >= Base || Value > (0x10FFFFU - Digit) / Base) return FALSE;
      Value = Value * Base + Digit;
      HasDigit = TRUE;
      Index++;
    }

    if (!HasDigit || Index >= Remaining || Text[Index] != ';' || !BlsXmlCodePointIsValid(Value)) return FALSE;
    *CodePoint = Value;
    *Consumed = Index + 1;
    return TRUE;
  }

  return FALSE;
}

static UINTN BlsCountAsciiTokenInRange(
  IN CONST CHAR8 *Start,
  IN CONST CHAR8 *End,
  IN CONST CHAR8 *Token
  ) {
  UINTN Count;
  UINTN TokenLength;
  CONST CHAR8 *Cursor;

  if (Start == NULL || End == NULL || Token == NULL || Start > End) return MAX_UINTN;
  TokenLength = AsciiStrLen(Token);
  if (TokenLength == 0) return MAX_UINTN;

  Count = 0;
  Cursor = Start;
  while (Cursor < End && TokenLength <= (UINTN)(End - Cursor)) {
    if (CompareMem(Cursor, Token, TokenLength) == 0) {
      if (Count == MAX_UINTN) return MAX_UINTN;
      Count++;
      Cursor += TokenLength;
    } else {
      Cursor++;
    }
  }
  return Count;
}

static UINTN BlsCountPlistValuesAfterKey(
  IN CONST CHAR8 *KeyPos,
  IN UINTN KeyPatternLength,
  IN CONST CHAR8 *NextKey
  ) {
  static CONST CHAR8 *ValueTokens[] = {
    "<string>",
    "<integer>",
    "<true/>",
    "<true />",
    "<false/>",
    "<false />",
    "<real>",
    "<data>",
    "<date>",
    "<array>",
    "<dict>"
  };
  CONST CHAR8 *RegionStart;
  CONST CHAR8 *RegionEnd;
  UINTN Count;
  UINTN Index;

  if (KeyPos == NULL || KeyPatternLength == 0) return MAX_UINTN;
  RegionStart = KeyPos + KeyPatternLength;
  RegionEnd = NextKey;
  if (RegionEnd == NULL) RegionEnd = AsciiStrStr(RegionStart, "</dict>");
  if (RegionEnd == NULL || RegionEnd < RegionStart) return MAX_UINTN;

  Count = 0;
  for (Index = 0; Index < ARRAY_SIZE(ValueTokens); Index++) {
    UINTN TokenCount;

    TokenCount = BlsCountAsciiTokenInRange(RegionStart, RegionEnd, ValueTokens[Index]);
    if (TokenCount == MAX_UINTN || Count > MAX_UINTN - TokenCount) return MAX_UINTN;
    Count += TokenCount;
  }
  return Count;
}

static BOOLEAN BlsExtractString(IN CONST CHAR8 *Xml, IN CONST CHAR8 *Key, OUT CHAR16 *Out, IN UINTN OutChars) {
  CHAR8 Pattern[96];
  CONST CHAR8 *KeyPos;
  CONST CHAR8 *Start;
  CONST CHAR8 *End;
  CONST CHAR8 *NextKey;
  CONST CHAR8 *ValueStart;
  CONST CHAR8 *Cursor;
  UINTN OutIndex;

  if (Xml == NULL || Key == NULL || Out == NULL || OutChars == 0) return FALSE;
  Out[0] = L'\0';
  AsciiSPrint(Pattern, sizeof(Pattern), "<key>%a</key>", Key);
  KeyPos = AsciiStrStr(Xml, Pattern);
  if (KeyPos == NULL) return FALSE;
  NextKey = AsciiStrStr(KeyPos + 1, "<key>");
  if (BlsCountPlistValuesAfterKey(KeyPos, AsciiStrLen(Pattern), NextKey) != 1) return FALSE;
  Start = AsciiStrStr(KeyPos, "<string>");
  if (Start == NULL || (NextKey != NULL && Start > NextKey)) return FALSE;
  ValueStart = BlsSkipAsciiSpace(KeyPos + AsciiStrLen(Pattern), Start);
  if (ValueStart == NULL || ValueStart != Start) return FALSE;
  Start += 8;
  End = AsciiStrStr(Start, "</string>");
  if (End == NULL || End < Start || (NextKey != NULL && End > NextKey)) return FALSE;

  Cursor = Start;
  OutIndex = 0;
  while (Cursor < End) {
    UINT32 CodePoint;
    UINTN Consumed;
    BOOLEAN Decoded;

    CodePoint = 0;
    Consumed = 0;
    if (*Cursor == '&') {
      Decoded = BlsDecodeXmlEntity(Cursor, End, &CodePoint, &Consumed);
    } else if (*Cursor == '<') {
      Decoded = FALSE;
    } else {
      Decoded = BlsDecodeUtf8CodePoint(Cursor, (UINTN)(End - Cursor), &CodePoint, &Consumed);
    }

    if (!Decoded || !BlsAppendCodePoint(Out, OutChars, &OutIndex, CodePoint)) {
      Out[0] = L'\0';
      return FALSE;
    }
    Cursor += Consumed;
  }

  Out[OutIndex] = L'\0';
  return TRUE;
}

static BOOLEAN BlsExtractInteger(IN CONST CHAR8 *Xml, IN CONST CHAR8 *Key, OUT UINTN *Value) {
  CHAR8 Pattern[96];
  CONST CHAR8 *KeyPos;
  CONST CHAR8 *Start;
  CONST CHAR8 *End;
  CONST CHAR8 *NextKey;
  CONST CHAR8 *ValueStart;
  UINTN Result;
  BOOLEAN HasDigit;
  if (Xml == NULL || Key == NULL || Value == NULL) return FALSE;
  AsciiSPrint(Pattern, sizeof(Pattern), "<key>%a</key>", Key);
  KeyPos = AsciiStrStr(Xml, Pattern);
  if (KeyPos == NULL) return FALSE;
  NextKey = AsciiStrStr(KeyPos + 1, "<key>");
  if (BlsCountPlistValuesAfterKey(KeyPos, AsciiStrLen(Pattern), NextKey) != 1) return FALSE;
  Start = AsciiStrStr(KeyPos, "<integer>");
  if (Start == NULL || (NextKey != NULL && Start > NextKey)) return FALSE;
  ValueStart = BlsSkipAsciiSpace(KeyPos + AsciiStrLen(Pattern), Start);
  if (ValueStart == NULL || ValueStart != Start) return FALSE;
  End = AsciiStrStr(Start, "</integer>");
  if (End == NULL || End <= Start || (NextKey != NULL && End > NextKey)) return FALSE;
  Start += 9;
  while (Start < End && BlsAsciiIsSpace(*Start)) Start++;
  Result = 0;
  HasDigit = FALSE;
  while (Start < End && *Start >= '0' && *Start <= '9') {
    UINTN Digit;

    Digit = (UINTN)(*Start - '0');
    if (Result > (MAX_UINTN - Digit) / 10) return FALSE;
    HasDigit = TRUE;
    Result = Result * 10 + Digit;
    Start++;
  }
  while (Start < End && BlsAsciiIsSpace(*Start)) Start++;
  if (!HasDigit || Start != End) return FALSE;
  *Value = Result;
  return TRUE;
}
static BOOLEAN BlsExtractBoolean(IN CONST CHAR8 *Xml, IN CONST CHAR8 *Key, OUT BOOLEAN *Value, OUT BOOLEAN *NeedsRewrite) {
  CHAR8 Pattern[96];
  CONST CHAR8 *KeyPos;
  CONST CHAR8 *NextKey;
  CONST CHAR8 *TruePos;
  CONST CHAR8 *FalsePos;
  CONST CHAR8 *StringStart;
  CONST CHAR8 *StringEnd;
  CONST CHAR8 *ValueStart;
  CHAR8 Temp[16];
  UINTN Len;

  if (Xml == NULL || Key == NULL || Value == NULL) return FALSE;
  if (NeedsRewrite != NULL) *NeedsRewrite = FALSE;
  AsciiSPrint(Pattern, sizeof(Pattern), "<key>%a</key>", Key);
  KeyPos = AsciiStrStr(Xml, Pattern);
  if (KeyPos == NULL) return FALSE;

  NextKey = AsciiStrStr(KeyPos + 1, "<key>");
  if (BlsCountPlistValuesAfterKey(KeyPos, AsciiStrLen(Pattern), NextKey) != 1) return FALSE;
  TruePos = AsciiStrStr(KeyPos, "<true/>");
  if (TruePos == NULL) TruePos = AsciiStrStr(KeyPos, "<true />");
  FalsePos = AsciiStrStr(KeyPos, "<false/>");
  if (FalsePos == NULL) FalsePos = AsciiStrStr(KeyPos, "<false />");
  StringStart = AsciiStrStr(KeyPos, "<string>");
  if (TruePos != NULL && NextKey != NULL && TruePos > NextKey) TruePos = NULL;
  if (FalsePos != NULL && NextKey != NULL && FalsePos > NextKey) FalsePos = NULL;
  if (StringStart != NULL && NextKey != NULL && StringStart > NextKey) StringStart = NULL;
  if ((TruePos != NULL && FalsePos != NULL) ||
      (TruePos != NULL && StringStart != NULL) ||
      (FalsePos != NULL && StringStart != NULL)) {
    return FALSE;
  }

  ValueStart = BlsSkipAsciiSpace(KeyPos + AsciiStrLen(Pattern),
                                (NextKey != NULL) ? NextKey : KeyPos + AsciiStrLen(KeyPos));
  if (ValueStart == NULL ||
      (TruePos != ValueStart && FalsePos != ValueStart && StringStart != ValueStart)) {
    return FALSE;
  }

  if (TruePos != NULL) {
    *Value = TRUE;
    return TRUE;
  }
  if (FalsePos != NULL) {
    *Value = FALSE;
    return TRUE;
  }

  if (StringStart == NULL) return FALSE;
  StringStart += 8;
  StringEnd = AsciiStrStr(StringStart, "</string>");
  if (StringEnd == NULL || StringEnd <= StringStart || (NextKey != NULL && StringEnd > NextKey)) return FALSE;
  Len = (UINTN)(StringEnd - StringStart);
  if (Len >= sizeof(Temp)) return FALSE;
  CopyMem(Temp, StringStart, Len);
  Temp[Len] = '\0';
  BlsTrimAscii(Temp);
  if (BlsStriEqAscii(Temp, "true") || BlsStriEqAscii(Temp, "yes") || BlsStriEqAscii(Temp, "1")) {
    *Value = TRUE;
    if (NeedsRewrite != NULL) *NeedsRewrite = TRUE;
    return TRUE;
  }
  if (BlsStriEqAscii(Temp, "false") || BlsStriEqAscii(Temp, "no") || BlsStriEqAscii(Temp, "0")) {
    *Value = FALSE;
    if (NeedsRewrite != NULL) *NeedsRewrite = TRUE;
    return TRUE;
  }
  return FALSE;
}

static BOOLEAN BlsXmlHasKey(IN CONST CHAR8 *Xml, IN CONST CHAR8 *Key) {
  CHAR8 Pattern[96];
  if (Xml == NULL || Key == NULL) return FALSE;
  AsciiSPrint(Pattern, sizeof(Pattern), "<key>%a</key>", Key);
  return (BOOLEAN)(AsciiStrStr(Xml, Pattern) != NULL);
}

static BOOLEAN BlsConfigHasKnownKey(IN CONST CHAR8 *Xml) {
  if (Xml == NULL) return FALSE;
  return (BlsXmlHasKey(Xml, "DefaultBootloader") ||
          BlsXmlHasKey(Xml, "DefaultPath") ||
          BlsXmlHasKey(Xml, "DefaultVolumeLabel") ||
          BlsXmlHasKey(Xml, "DefaultVolumeDevicePath") ||
          BlsXmlHasKey(Xml, "Timeout") ||
          BlsXmlHasKey(Xml, "Debug") ||
          BlsXmlHasKey(Xml, "ShowHardwareInfo") ||
          BlsXmlHasKey(Xml, "RememberLastSelected") ||
          BlsXmlHasKey(Xml, "HasRememberedSelection"));
}

static BOOLEAN BlsXmlHasDuplicateKey(IN CONST CHAR8 *Xml, IN CONST CHAR8 *Key) {
  CHAR8 Pattern[96];
  CONST CHAR8 *First;
  UINTN PatternLen;

  if (Xml == NULL || Key == NULL) return FALSE;
  AsciiSPrint(Pattern, sizeof(Pattern), "<key>%a</key>", Key);
  PatternLen = AsciiStrLen(Pattern);
  if (PatternLen == 0) return FALSE;
  First = AsciiStrStr(Xml, Pattern);
  if (First == NULL) return FALSE;
  return (BOOLEAN)(AsciiStrStr(First + PatternLen, Pattern) != NULL);
}

static BOOLEAN BlsConfigHasDuplicateKnownKey(IN CONST CHAR8 *Xml) {
  if (Xml == NULL) return FALSE;
  return (BlsXmlHasDuplicateKey(Xml, "DefaultBootloader") ||
          BlsXmlHasDuplicateKey(Xml, "DefaultPath") ||
          BlsXmlHasDuplicateKey(Xml, "DefaultVolumeLabel") ||
          BlsXmlHasDuplicateKey(Xml, "DefaultVolumeDevicePath") ||
          BlsXmlHasDuplicateKey(Xml, "Timeout") ||
          BlsXmlHasDuplicateKey(Xml, "Debug") ||
          BlsXmlHasDuplicateKey(Xml, "ShowHardwareInfo") ||
          BlsXmlHasDuplicateKey(Xml, "RememberLastSelected") ||
          BlsXmlHasDuplicateKey(Xml, "HasRememberedSelection"));
}

static BOOLEAN BlsParseConfigDict(IN CONST CHAR8 *Xml, OUT BLS_CONFIG *Config) {
  BOOLEAN NeedsRewrite;
  UINTN Timeout;
  if (Xml == NULL || Config == NULL) return FALSE;

  if (AsciiStrStr(Xml, "<dict>") == NULL || AsciiStrStr(Xml, "</dict>") == NULL ||
      !BlsConfigHasKnownKey(Xml) || BlsConfigHasDuplicateKnownKey(Xml)) {
    return FALSE;
  }

  if (BlsXmlHasKey(Xml, "DefaultBootloader")) {
    if (!BlsExtractString(Xml, "DefaultBootloader", Config->DefaultBootloader, BLS_MAX_TEXT)) return FALSE;
  } else {
    Config->ForceRewrite = TRUE;
  }
  if (Config->DefaultBootloader[0] == L'\0') {
    BlsSafeStrCpy(Config->DefaultBootloader, BLS_MAX_TEXT, L"OpenCore");
    Config->ForceRewrite = TRUE;
  }
  if (BlsXmlHasKey(Xml, "DefaultPath")) {
    if (!BlsExtractString(Xml, "DefaultPath", Config->DefaultPath, BLS_MAX_PATH)) return FALSE;
  } else {
    Config->ForceRewrite = TRUE;
  }
  if (BlsXmlHasKey(Xml, "DefaultVolumeLabel")) {
    if (!BlsExtractString(Xml, "DefaultVolumeLabel", Config->DefaultVolumeLabel, BLS_MAX_TEXT)) return FALSE;
  } else {
    Config->ForceRewrite = TRUE;
  }
  if (BlsXmlHasKey(Xml, "DefaultVolumeDevicePath")) {
    if (!BlsExtractString(Xml, "DefaultVolumeDevicePath", Config->DefaultVolumeDevicePath, BLS_MAX_VOLUME_ID)) return FALSE;
  } else {
    Config->ForceRewrite = TRUE;
  }
  if (BlsXmlHasKey(Xml, "Timeout")) {
    if (!BlsExtractInteger(Xml, "Timeout", &Timeout)) return FALSE;
    if (Timeout > 30) {
      Timeout = 30;
      Config->ForceRewrite = TRUE;
    }
    Config->Timeout = Timeout;
  } else {
    Config->ForceRewrite = TRUE;
  }
  NeedsRewrite = FALSE;
  if (BlsXmlHasKey(Xml, "Debug")) {
    if (!BlsExtractBoolean(Xml, "Debug", &Config->Debug, &NeedsRewrite)) return FALSE;
    if (NeedsRewrite) Config->ForceRewrite = TRUE;
  } else {
    Config->ForceRewrite = TRUE;
  }

  NeedsRewrite = FALSE;
  if (BlsXmlHasKey(Xml, "ShowHardwareInfo")) {
    if (!BlsExtractBoolean(Xml, "ShowHardwareInfo", &Config->ShowHardwareInfo, &NeedsRewrite)) return FALSE;
    if (NeedsRewrite) Config->ForceRewrite = TRUE;
  } else {
    Config->ShowHardwareInfo = TRUE;
    Config->ForceRewrite = TRUE;
  }

  NeedsRewrite = FALSE;
  if (BlsXmlHasKey(Xml, "RememberLastSelected")) {
    if (!BlsExtractBoolean(Xml, "RememberLastSelected", &Config->RememberLastSelected, &NeedsRewrite)) return FALSE;
    if (NeedsRewrite) Config->ForceRewrite = TRUE;
  } else {
    Config->RememberLastSelected = TRUE;
    Config->ForceRewrite = TRUE;
  }

  NeedsRewrite = FALSE;
  if (BlsXmlHasKey(Xml, "HasRememberedSelection")) {
    if (!BlsExtractBoolean(Xml, "HasRememberedSelection", &Config->HasRememberedSelection, &NeedsRewrite)) return FALSE;
    if (NeedsRewrite) Config->ForceRewrite = TRUE;
  } else {
    Config->HasRememberedSelection = FALSE;
    Config->ForceRewrite = TRUE;
  }

  return TRUE;
}

static CONST CHAR8 *BlsFindXmlCommentClose(IN CONST CHAR8 *Start, IN CONST CHAR8 *End) {
  CONST CHAR8 *Cursor;

  if (Start == NULL || End == NULL || Start > End) return NULL;
  Cursor = Start;
  while ((UINTN)(End - Cursor) >= 2U) {
    if (Cursor[0] == '-' && Cursor[1] == '-') {
      if ((UINTN)(End - Cursor) >= 3U && Cursor[2] == '>') return Cursor;
      return NULL;
    }
    Cursor++;
  }
  return NULL;
}

static CONST CHAR8 *BlsSkipXmlIgnorable(IN CONST CHAR8 *Start, IN CONST CHAR8 *End) {
  CONST CHAR8 *Cursor;

  if (Start == NULL || End == NULL || Start > End) return NULL;
  Cursor = Start;
  while (Cursor < End) {
    CONST CHAR8 *Close;

    if (BlsAsciiIsSpace(*Cursor)) {
      Cursor++;
      continue;
    }
    if ((UINTN)(End - Cursor) >= 4 && CompareMem(Cursor, "<!--", 4) == 0) {
      Close = BlsFindXmlCommentClose(Cursor + 4, End);
      if (Close == NULL || Close + 3 > End) return NULL;
      Cursor = Close + 3;
      continue;
    }
    if ((UINTN)(End - Cursor) >= 2 && CompareMem(Cursor, "<?", 2) == 0) {
      Close = AsciiStrStr(Cursor + 2, "?>");
      if (Close == NULL || Close + 2 > End) return NULL;
      Cursor = Close + 2;
      continue;
    }
    break;
  }
  return Cursor;
}

static CONST CHAR8 *BlsFindXmlTagEnd(IN CONST CHAR8 *Start, IN CONST CHAR8 *End);

static CONST CHAR8 *BlsFindPlistOpen(IN CONST CHAR8 *Xml, IN CONST CHAR8 *End) {
  CONST CHAR8 *Cursor;

  if (Xml == NULL || End == NULL || Xml > End) return NULL;
  Cursor = Xml;
  if ((UINTN)(End - Cursor) >= 3 && (UINT8)Cursor[0] == 0xEF &&
      (UINT8)Cursor[1] == 0xBB && (UINT8)Cursor[2] == 0xBF) {
    Cursor += 3;
  }

  while (Cursor < End) {
    CONST CHAR8 *Close;

    Cursor = BlsSkipXmlIgnorable(Cursor, End);
    if (Cursor == NULL || Cursor >= End) return NULL;
    if ((UINTN)(End - Cursor) >= 9 && AsciiStrnCmp(Cursor, "<!DOCTYPE", 9) == 0) {
      Close = AsciiStrStr(Cursor + 9, ">");
      if (Close == NULL || Close + 1 > End) return NULL;
      Cursor = Close + 1;
      continue;
    }
    break;
  }

  if ((UINTN)(End - Cursor) < 7 || AsciiStrnCmp(Cursor, "<plist", 6) != 0 ||
      (Cursor[6] != '>' && !BlsAsciiIsSpace(Cursor[6]))) {
    return NULL;
  }
  {
    CONST CHAR8 *TagEnd;
    CONST CHAR8 *Scan;

    TagEnd = BlsFindXmlTagEnd(Cursor + 6, End);
    if (TagEnd == NULL) return NULL;
    Scan = TagEnd;
    while (Scan > Cursor + 6 && BlsAsciiIsSpace(Scan[-1])) Scan--;
    if (Scan > Cursor + 6 && Scan[-1] == '/') return NULL;
  }
  return Cursor;
}

static CONST CHAR8 *BlsFindRootDictClose(IN CONST CHAR8 *DictOpen, IN CONST CHAR8 *End) {
  CONST CHAR8 *Cursor;
  UINTN Depth;

  if (DictOpen == NULL || End == NULL || DictOpen >= End ||
      (UINTN)(End - DictOpen) < AsciiStrLen("<dict>") ||
      CompareMem(DictOpen, "<dict>", AsciiStrLen("<dict>")) != 0) {
    return NULL;
  }

  Cursor = DictOpen;
  Depth = 0;
  while (Cursor < End) {
    CONST CHAR8 *Close;

    if (*Cursor != '<') {
      Cursor++;
      continue;
    }
    if ((UINTN)(End - Cursor) >= 4 && CompareMem(Cursor, "<!--", 4) == 0) {
      Close = BlsFindXmlCommentClose(Cursor + 4, End);
      if (Close == NULL || Close + 3 > End) return NULL;
      Cursor = Close + 3;
      continue;
    }
    if ((UINTN)(End - Cursor) >= 2 && CompareMem(Cursor, "<?", 2) == 0) {
      Close = AsciiStrStr(Cursor + 2, "?>");
      if (Close == NULL || Close + 2 > End) return NULL;
      Cursor = Close + 2;
      continue;
    }
    if ((UINTN)(End - Cursor) >= AsciiStrLen("<dict>") &&
        CompareMem(Cursor, "<dict>", AsciiStrLen("<dict>")) == 0) {
      if (Depth == MAX_UINTN) return NULL;
      Depth++;
      Cursor += AsciiStrLen("<dict>");
      continue;
    }
    if ((UINTN)(End - Cursor) >= AsciiStrLen("</dict>") &&
        CompareMem(Cursor, "</dict>", AsciiStrLen("</dict>")) == 0) {
      if (Depth == 0) return NULL;
      Depth--;
      if (Depth == 0) return Cursor;
      Cursor += AsciiStrLen("</dict>");
      continue;
    }
    Cursor++;
  }
  return NULL;
}

static BOOLEAN BlsKeyRangeIsKnown(IN CONST CHAR8 *Start, IN CONST CHAR8 *End) {
  static CONST CHAR8 *KnownKeys[] = {
    "DefaultBootloader",
    "DefaultPath",
    "DefaultVolumeLabel",
    "DefaultVolumeDevicePath",
    "Timeout",
    "Debug",
    "ShowHardwareInfo",
    "RememberLastSelected",
    "HasRememberedSelection"
  };
  UINTN Length;
  UINTN Index;

  if (Start == NULL || End == NULL || Start > End) return FALSE;
  Length = (UINTN)(End - Start);
  for (Index = 0; Index < ARRAY_SIZE(KnownKeys); Index++) {
    UINTN KnownLength;

    KnownLength = AsciiStrLen(KnownKeys[Index]);
    if (Length == KnownLength && CompareMem(Start, KnownKeys[Index], Length) == 0) return TRUE;
  }
  return FALSE;
}

typedef struct {
  CONST CHAR8 *Name;
  UINTN       NameLength;
} BLS_XML_TAG_FRAME;

static BOOLEAN BlsXmlNameStart(IN CHAR8 Ch) {
  return (BOOLEAN)((Ch >= 'A' && Ch <= 'Z') ||
                   (Ch >= 'a' && Ch <= 'z') ||
                   Ch == '_' || Ch == ':');
}

static BOOLEAN BlsXmlNameChar(IN CHAR8 Ch) {
  return (BOOLEAN)(BlsXmlNameStart(Ch) ||
                   (Ch >= '0' && Ch <= '9') ||
                   Ch == '-' || Ch == '.');
}

static BOOLEAN BlsXmlTagNameEquals(
  IN CONST CHAR8 *Name,
  IN UINTN NameLength,
  IN CONST CHAR8 *Expected
  ) {
  UINTN ExpectedLength;

  if (Name == NULL || Expected == NULL) return FALSE;
  ExpectedLength = AsciiStrLen(Expected);
  return (BOOLEAN)(NameLength == ExpectedLength && CompareMem(Name, Expected, NameLength) == 0);
}

static CONST CHAR8 *BlsFindXmlTagEnd(IN CONST CHAR8 *Start, IN CONST CHAR8 *End) {
  CONST CHAR8 *Cursor;
  CHAR8 Quote;

  if (Start == NULL || End == NULL || Start > End) return NULL;
  Cursor = Start;
  Quote = '\0';
  while (Cursor < End) {
    if (Quote != '\0') {
      /* XML 1.0 forbids a raw '<' inside attribute values. Reject it here so
         text that looks like a plist key cannot be hidden inside an unknown
         element attribute and later discovered by scalar lookup helpers. */
      if (*Cursor == '<') return NULL;
      if (*Cursor == Quote) Quote = '\0';
    } else if (*Cursor == '\'' || *Cursor == '"') {
      Quote = *Cursor;
    } else if (*Cursor == '>') {
      return Cursor;
    }
    Cursor++;
  }
  return NULL;
}

static BOOLEAN BlsStripXmlIgnorableMarkup(IN OUT CHAR8 *Xml) {
  CONST CHAR8 *Read;
  CHAR8 *Write;
  CONST CHAR8 *End;
  UINTN Length;

  if (Xml == NULL) return FALSE;
  Length = BlsAsciiLenBounded(Xml, BLS_CONFIG_MAX + 1U);
  if (Length > BLS_CONFIG_MAX || Xml[Length] != '\0') return FALSE;

  Read = Xml;
  Write = Xml;
  End = Xml + Length;
  while (Read < End) {
    if ((UINTN)(End - Read) >= 4U && CompareMem(Read, "<!--", 4U) == 0) {
      CONST CHAR8 *Close;

      Close = BlsFindXmlCommentClose(Read + 4U, End);
      if (Close == NULL || Close + 3U > End) return FALSE;
      Read = Close + 3U;
      continue;
    }
    if ((UINTN)(End - Read) >= 2U && CompareMem(Read, "<?", 2U) == 0) {
      CHAR8 *Close;

      Close = AsciiStrStr(Read + 2U, "?>");
      if (Close == NULL || Close + 2U > End) return FALSE;
      Read = Close + 2U;
      continue;
    }
    *Write++ = *Read++;
  }
  *Write = '\0';
  return TRUE;
}

static BOOLEAN BlsValidateAndSanitizeRootDict(IN OUT CHAR8 *DictXml) {
  BLS_XML_TAG_FRAME Stack[BLS_XML_MAX_DEPTH];
  CONST CHAR8 *Cursor;
  CONST CHAR8 *XmlEnd;
  UINTN XmlLength;
  UINTN Depth;
  BOOLEAN RootSeen;
  BOOLEAN RootClosed;
  BOOLEAN RootExpectKey;
  CHAR8 RootChildKind;
  UINTN RootChildDepth;
  BOOLEAN Valid;

  if (DictXml == NULL) return FALSE;
  XmlLength = BlsAsciiLenBounded(DictXml, BLS_CONFIG_MAX + 1);
  if (XmlLength < AsciiStrLen("<dict></dict>") || XmlLength > BLS_CONFIG_MAX ||
      DictXml[XmlLength] != '\0' || CompareMem(DictXml, "<dict>", AsciiStrLen("<dict>")) != 0) {
    return FALSE;
  }

  /* BLS.plist has a deliberately flat, closed schema.  A fixed parser stack
     prevents a crafted but size-bounded XML file from forcing a heap allocation
     proportional to file length in the pre-boot environment.  The generated
     configuration needs only the root dict plus scalar values; 32 levels leave
     ample compatibility headroom while bounding parser memory deterministically. */
  ZeroMem(Stack, sizeof(Stack));

  Cursor = DictXml;
  XmlEnd = DictXml + XmlLength;
  Depth = 0;
  RootSeen = FALSE;
  RootClosed = FALSE;
  RootExpectKey = TRUE;
  RootChildKind = '\0';
  RootChildDepth = 0;
  Valid = FALSE;

  while (Cursor < XmlEnd) {
    CONST CHAR8 *NameStart;
    CONST CHAR8 *NameEnd;
    CONST CHAR8 *TagEnd;
    CONST CHAR8 *Scan;
    BOOLEAN Closing;
    BOOLEAN SelfClosing;
    UINTN NameLength;

    if (*Cursor != '<') {
      if ((Depth == 0 || Depth == 1) && !BlsAsciiIsSpace(*Cursor)) goto Done;
      Cursor++;
      continue;
    }

    if ((UINTN)(XmlEnd - Cursor) >= 4 && CompareMem(Cursor, "<!--", 4) == 0) {
      CONST CHAR8 *Close;

      Close = BlsFindXmlCommentClose(Cursor + 4, XmlEnd);
      if (Close == NULL || Close + 3 > XmlEnd) goto Done;
      Cursor = Close + 3;
      continue;
    }
    if ((UINTN)(XmlEnd - Cursor) >= 2 && CompareMem(Cursor, "<?", 2) == 0) {
      CONST CHAR8 *Close;

      Close = AsciiStrStr(Cursor + 2, "?>");
      if (Close == NULL || Close + 2 > XmlEnd) goto Done;
      Cursor = Close + 2;
      continue;
    }
    if ((UINTN)(XmlEnd - Cursor) >= 2 && CompareMem(Cursor, "<!", 2) == 0) goto Done;

    Cursor++;
    Closing = FALSE;
    if (Cursor < XmlEnd && *Cursor == '/') {
      Closing = TRUE;
      Cursor++;
    }
    if (Cursor >= XmlEnd || !BlsXmlNameStart(*Cursor)) goto Done;

    NameStart = Cursor;
    while (Cursor < XmlEnd && BlsXmlNameChar(*Cursor)) Cursor++;
    NameEnd = Cursor;
    NameLength = (UINTN)(NameEnd - NameStart);
    TagEnd = BlsFindXmlTagEnd(Cursor, XmlEnd);
    if (TagEnd == NULL) goto Done;

    SelfClosing = FALSE;
    Scan = TagEnd;
    while (Scan > NameEnd && BlsAsciiIsSpace(Scan[-1])) Scan--;
    if (!Closing && Scan > NameEnd && Scan[-1] == '/') SelfClosing = TRUE;

    if (Closing) {
      while (Cursor < TagEnd && BlsAsciiIsSpace(*Cursor)) Cursor++;
      if (Cursor != TagEnd || Depth == 0) goto Done;
      if (Stack[Depth - 1].NameLength != NameLength ||
          CompareMem(Stack[Depth - 1].Name, NameStart, NameLength) != 0) {
        goto Done;
      }

      if (Depth == RootChildDepth && RootChildKind != '\0') {
        if (RootChildKind == 'k') {
          RootExpectKey = FALSE;
        } else {
          RootExpectKey = TRUE;
        }
        RootChildKind = '\0';
        RootChildDepth = 0;
      }

      Depth--;
      if (Depth == 0) {
        if (!BlsXmlTagNameEquals(NameStart, NameLength, "dict") ||
            !RootSeen || !RootExpectKey || RootChildKind != '\0') {
          goto Done;
        }
        RootClosed = TRUE;
      }
      Cursor = TagEnd + 1;
      continue;
    }

    if (RootClosed) goto Done;
    if (Depth == 0) {
      if (RootSeen || SelfClosing || !BlsXmlTagNameEquals(NameStart, NameLength, "dict")) goto Done;
      Scan = NameEnd;
      while (Scan < TagEnd && BlsAsciiIsSpace(*Scan)) Scan++;
      if (Scan != TagEnd) goto Done;
      RootSeen = TRUE;
    } else if (Depth == 1) {
      if (RootExpectKey) {
        if (SelfClosing || !BlsXmlTagNameEquals(NameStart, NameLength, "key")) goto Done;
        RootChildKind = 'k';
      } else {
        if (BlsXmlTagNameEquals(NameStart, NameLength, "key")) goto Done;
        RootChildKind = 'v';
      }
      RootChildDepth = Depth + 1;
    }

    if (BlsXmlTagNameEquals(NameStart, NameLength, "key")) {
      CONST CHAR8 *KeyStart;
      CONST CHAR8 *KeyEnd;

      KeyStart = TagEnd + 1;
      KeyEnd = KeyStart;
      while (KeyEnd < XmlEnd && *KeyEnd != '<') KeyEnd++;
      if (KeyEnd >= XmlEnd ||
          (UINTN)(XmlEnd - KeyEnd) < AsciiStrLen("</key>") ||
          CompareMem(KeyEnd, "</key>", AsciiStrLen("</key>")) != 0) {
        goto Done;
      }
      /* Closed top-level schema: unknown keys would otherwise be accepted and
         silently discarded the next time the configuration is serialized. */
      if (Depth == 1) {
        if (!BlsKeyRangeIsKnown(KeyStart, KeyEnd)) goto Done;
      } else if (BlsKeyRangeIsKnown(KeyStart, KeyEnd)) {
        goto Done;
      }
    }

    if (SelfClosing) {
      if (Depth == 1) {
        if (RootChildKind != 'v') goto Done;
        RootExpectKey = TRUE;
        RootChildKind = '\0';
        RootChildDepth = 0;
      }
      Cursor = TagEnd + 1;
      continue;
    }

    if (Depth >= ARRAY_SIZE(Stack)) goto Done;
    Stack[Depth].Name = NameStart;
    Stack[Depth].NameLength = NameLength;
    Depth++;
    Cursor = TagEnd + 1;
  }

  Valid = (BOOLEAN)(RootSeen && RootClosed && Depth == 0 && RootExpectKey && RootChildKind == '\0');

Done:
  return Valid;
}

static EFI_STATUS BlsCopyValidatedPlistDict(IN CONST CHAR8 *Xml, OUT CHAR8 **DictXml) {
  CONST CHAR8 *PlistOpen;
  CONST CHAR8 *PlistOpenEnd;
  CONST CHAR8 *PlistClose;
  CONST CHAR8 *DictOpen;
  CONST CHAR8 *DictClose;
  CONST CHAR8 *Cursor;
  CONST CHAR8 *XmlEnd;
  UINTN DictSize;
  UINTN XmlLength;
  CHAR8 *Copy;

  if (DictXml == NULL) return EFI_INVALID_PARAMETER;
  *DictXml = NULL;
  if (Xml == NULL) return EFI_INVALID_PARAMETER;
  XmlLength = BlsAsciiLenBounded(Xml, BLS_CONFIG_MAX + 1);
  if (XmlLength == 0 || XmlLength > BLS_CONFIG_MAX || Xml[XmlLength] != '\0') return EFI_LOAD_ERROR;
  XmlEnd = Xml + XmlLength;

  PlistOpen = BlsFindPlistOpen(Xml, XmlEnd);
  if (PlistOpen == NULL) return EFI_LOAD_ERROR;
  PlistOpenEnd = BlsFindXmlTagEnd(PlistOpen + 6, XmlEnd);
  if (PlistOpenEnd == NULL) return EFI_LOAD_ERROR;
  PlistOpenEnd++;

  Cursor = BlsSkipXmlIgnorable(PlistOpenEnd, XmlEnd);
  if (Cursor == NULL || (UINTN)(XmlEnd - Cursor) < AsciiStrLen("<dict>") ||
      CompareMem(Cursor, "<dict>", AsciiStrLen("<dict>")) != 0) {
    return EFI_LOAD_ERROR;
  }
  DictOpen = Cursor;
  DictClose = BlsFindRootDictClose(DictOpen, XmlEnd);
  if (DictClose == NULL) return EFI_LOAD_ERROR;

  Cursor = BlsSkipXmlIgnorable(DictClose + AsciiStrLen("</dict>"), XmlEnd);
  if (Cursor == NULL || (UINTN)(XmlEnd - Cursor) < AsciiStrLen("</plist>") ||
      CompareMem(Cursor, "</plist>", AsciiStrLen("</plist>")) != 0) {
    return EFI_LOAD_ERROR;
  }
  PlistClose = Cursor;
  Cursor = BlsSkipXmlIgnorable(PlistClose + AsciiStrLen("</plist>"), XmlEnd);
  if (Cursor == NULL || Cursor != XmlEnd) return EFI_LOAD_ERROR;

  DictSize = (UINTN)((DictClose + AsciiStrLen("</dict>")) - DictOpen);
  if (DictSize == 0 || DictSize >= BLS_CONFIG_MAX) return EFI_LOAD_ERROR;
  Copy = AllocateZeroPool(DictSize + 1);
  if (Copy == NULL) return EFI_OUT_OF_RESOURCES;
  CopyMem(Copy, DictOpen, DictSize);
  Copy[DictSize] = '\0';
  if (!BlsValidateAndSanitizeRootDict(Copy) || !BlsStripXmlIgnorableMarkup(Copy)) {
    FreePool(Copy);
    return EFI_LOAD_ERROR;
  }

  *DictXml = Copy;
  return EFI_SUCCESS;
}

static EFI_STATUS BlsParseConfig(IN CONST CHAR8 *Xml, OUT BLS_CONFIG *Config) {
  CHAR8 *DictXml;
  BLS_CONFIG Parsed;
  EFI_STATUS Status;

  if (Xml == NULL || Config == NULL) return EFI_INVALID_PARAMETER;
  DictXml = NULL;
  Status = BlsCopyValidatedPlistDict(Xml, &DictXml);
  if (Status != EFI_SUCCESS) return Status;
  if (DictXml == NULL) return EFI_COMPROMISED_DATA;

  CopyMem(&Parsed, Config, sizeof(Parsed));
  if (!BlsParseConfigDict(DictXml, &Parsed)) {
    FreePool(DictXml);
    return EFI_LOAD_ERROR;
  }
  FreePool(DictXml);
  CopyMem(Config, &Parsed, sizeof(*Config));
  return EFI_SUCCESS;
}


static BOOLEAN BlsAppendAsciiText(
  OUT CHAR8 *Dst,
  IN UINTN DstSize,
  IN OUT UINTN *DstIndex,
  IN CONST CHAR8 *Text
  ) {
  UINTN Index;
  UINTN SourceIndex;

  if (Dst == NULL || DstSize == 0 || DstIndex == NULL || Text == NULL) return FALSE;
  Index = *DstIndex;
  for (SourceIndex = 0; Text[SourceIndex] != '\0'; SourceIndex++) {
    if (Index + 1 >= DstSize) return FALSE;
    Dst[Index++] = Text[SourceIndex];
  }
  *DstIndex = Index;
  return TRUE;
}

static BOOLEAN BlsAppendUtf8CodePoint(
  OUT CHAR8 *Dst,
  IN UINTN DstSize,
  IN OUT UINTN *DstIndex,
  IN UINT32 CodePoint
  ) {
  UINT8 Bytes[4];
  UINTN Count;
  UINTN Index;
  UINTN ByteIndex;

  if (Dst == NULL || DstSize == 0 || DstIndex == NULL || !BlsXmlCodePointIsValid(CodePoint)) return FALSE;

  if (CodePoint <= 0x7F) {
    Bytes[0] = (UINT8)CodePoint;
    Count = 1;
  } else if (CodePoint <= 0x7FF) {
    Bytes[0] = (UINT8)(0xC0 | (CodePoint >> 6));
    Bytes[1] = (UINT8)(0x80 | (CodePoint & 0x3F));
    Count = 2;
  } else if (CodePoint <= 0xFFFF) {
    Bytes[0] = (UINT8)(0xE0 | (CodePoint >> 12));
    Bytes[1] = (UINT8)(0x80 | ((CodePoint >> 6) & 0x3F));
    Bytes[2] = (UINT8)(0x80 | (CodePoint & 0x3F));
    Count = 3;
  } else {
    Bytes[0] = (UINT8)(0xF0 | (CodePoint >> 18));
    Bytes[1] = (UINT8)(0x80 | ((CodePoint >> 12) & 0x3F));
    Bytes[2] = (UINT8)(0x80 | ((CodePoint >> 6) & 0x3F));
    Bytes[3] = (UINT8)(0x80 | (CodePoint & 0x3F));
    Count = 4;
  }

  Index = *DstIndex;
  if (Index + Count >= DstSize) return FALSE;
  for (ByteIndex = 0; ByteIndex < Count; ByteIndex++) {
    Dst[Index++] = (CHAR8)Bytes[ByteIndex];
  }
  *DstIndex = Index;
  return TRUE;
}

static BOOLEAN BlsUnicodeToUtf8XmlEscape(
  OUT CHAR8 *Dst,
  IN UINTN DstSize,
  IN CONST CHAR16 *Src
  ) {
  UINTN SourceIndex;
  UINTN DstIndex;

  if (Dst == NULL || DstSize == 0) return FALSE;
  Dst[0] = '\0';
  if (Src == NULL) return TRUE;

  SourceIndex = 0;
  DstIndex = 0;
  while (Src[SourceIndex] != L'\0') {
    UINT32 CodePoint;
    CONST CHAR8 *Escape;
    CHAR16 Unit;

    Unit = Src[SourceIndex++];
    /* UEFI CHAR16 is UCS-2. Surrogate code units are never valid application
       text, even when they form a well-formed UTF-16 pair. Refuse to serialize
       them so persisted configuration can never normalize invalid UEFI strings
       into apparently valid UTF-8. */
    if (Unit >= 0xD800U && Unit <= 0xDFFFU) return FALSE;
    CodePoint = Unit;

    /* The serializer must emit XML 1.0 characters only. Firmware-originated
       volume labels/device-path text can contain control codes that are valid
       CHAR16 values but illegal in XML; publishing them would create a plist
       that cannot be parsed reliably on the next boot. Reject before any write. */
    if (!BlsXmlCodePointIsValid(CodePoint)) return FALSE;

    Escape = NULL;
    switch (CodePoint) {
      case '&':  Escape = "&amp;";  break;
      case '<':  Escape = "&lt;";   break;
      case '>':  Escape = "&gt;";   break;
      case '"': Escape = "&quot;"; break;
      case '\'': Escape = "&apos;"; break;
      default:   break;
    }

    if (Escape != NULL) {
      if (!BlsAppendAsciiText(Dst, DstSize, &DstIndex, Escape)) return FALSE;
    } else if (!BlsAppendUtf8CodePoint(Dst, DstSize, &DstIndex, CodePoint)) {
      return FALSE;
    }
  }

  Dst[DstIndex] = '\0';
  return TRUE;
}

static EFI_STATUS BlsDeleteFileIfPresent(IN EFI_FILE_PROTOCOL *Root, IN CONST CHAR16 *Path) {
  EFI_FILE_PROTOCOL *File;
  EFI_STATUS Status;

  if (Root == NULL || Path == NULL) return EFI_INVALID_PARAMETER;
  File = NULL;
  Status = BlsFileOpen(Root, &File, Path, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
  if (Status == EFI_NOT_FOUND) return EFI_SUCCESS;
  if (Status != EFI_SUCCESS) return Status;

  return BlsFileDelete(&File);
}

static VOID BlsDeleteFileOnVolumeBestEffort(IN EFI_HANDLE VolumeHandle, IN CONST CHAR16 *Path) {
  EFI_FILE_PROTOCOL *Root;

  if (VolumeHandle == NULL || Path == NULL) return;
  Root = NULL;
  if (BlsOpenVolumeRoot(VolumeHandle, &Root) == EFI_SUCCESS && Root != NULL) {
    (VOID)BlsDeleteFileIfPresent(Root, Path);
  }
  BlsFileCloseBestEffort(&Root);
}

static EFI_STATUS BlsWriteBufferToPath(
  IN EFI_FILE_PROTOCOL *Root,
  IN CONST CHAR16 *Path,
  IN CONST CHAR8 *Buffer,
  IN UINTN BufferSize,
  OUT BOOLEAN *DataDurable
  ) {
  EFI_STATUS Status;
  EFI_FILE_PROTOCOL *File;
  UINTN Size;

  if (DataDurable != NULL) *DataDurable = FALSE;
  if (Root == NULL || Path == NULL || Buffer == NULL || BufferSize == 0) return EFI_INVALID_PARAMETER;

  File = NULL;
  Status = BlsFileOpen(Root, &File, Path, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
  if (Status != EFI_SUCCESS) {
    BlsFileCloseBestEffort(&File);
    return Status;
  }
  if (File == NULL) return EFI_DEVICE_ERROR;
  if (File->SetPosition == NULL) {
    BlsFileCloseBestEffort(&File);
    return EFI_UNSUPPORTED;
  }

  Status = File->SetPosition(File, 0);
  if (Status != EFI_SUCCESS) {
    BlsFileCloseBestEffort(&File);
    return Status;
  }
  Size = BufferSize;
  Status = BlsFileWriteAll(File, Buffer, Size);
  if (Status == EFI_SUCCESS) Status = BlsFileFlush(File);
  if (Status == EFI_SUCCESS && DataDurable != NULL) *DataDurable = TRUE;
  {
    EFI_STATUS CloseStatus;
    CloseStatus = BlsFileClose(&File);
    if (Status == EFI_SUCCESS && CloseStatus != EFI_SUCCESS) Status = CloseStatus;
  }
  return Status;
}

static EFI_STATUS BlsEnsureConfigDirectoryHierarchy(IN EFI_FILE_PROTOCOL *Root) {
  EFI_STATUS Status;
  EFI_FILE_PROTOCOL *Dir;

  if (Root == NULL) return EFI_INVALID_PARAMETER;

  /* EFI_FILE_PROTOCOL.Open() resolves each path component in turn. CREATE
     creates only the final component in its existing parent, so a selector
     launched from a non-standard path cannot assume that \\EFI already exists.
     Create the hierarchy explicitly before staging the configuration. */
  Dir = NULL;
  Status = BlsFileOpen(
             Root,
             &Dir,
             L"\\EFI",
             EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
             EFI_FILE_DIRECTORY
             );
  if (Status != EFI_SUCCESS) {
    BlsFileCloseBestEffort(&Dir);
    return Status;
  }
  if (Dir == NULL) return EFI_DEVICE_ERROR;
  Status = BlsFileClose(&Dir);
  if (Status != EFI_SUCCESS) return Status;

  Status = BlsFileOpen(
             Root,
             &Dir,
             BLS_CONFIG_DIR,
             EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
             EFI_FILE_DIRECTORY
             );
  if (Status != EFI_SUCCESS) {
    BlsFileCloseBestEffort(&Dir);
    return Status;
  }
  if (Dir == NULL) return EFI_DEVICE_ERROR;
  return BlsFileClose(&Dir);
}

static EFI_STATUS BlsSaveConfigInternal(
  IN EFI_HANDLE VolumeHandle,
  IN CONST BLS_CONFIG *Config,
  IN BOOLEAN PreserveDurableTemp
  ) {
  EFI_STATUS Status;
  EFI_STATUS CleanupStatus;
  EFI_FILE_PROTOCOL *Root;
  BLS_CONFIG_WRITE_CONTEXT *Context;
  UINTN Size;
  BOOLEAN TempDurable;

  if (VolumeHandle == NULL || Config == NULL) return EFI_INVALID_PARAMETER;
  if (Config->Timeout > 30 ||
      BlsStrLen16Bounded(Config->DefaultBootloader, BLS_MAX_TEXT) >= BLS_MAX_TEXT ||
      BlsStrLen16Bounded(Config->DefaultPath, BLS_MAX_PATH) >= BLS_MAX_PATH ||
      BlsStrLen16Bounded(Config->DefaultVolumeLabel, BLS_MAX_TEXT) >= BLS_MAX_TEXT ||
      BlsStrLen16Bounded(Config->DefaultVolumeDevicePath, BLS_MAX_VOLUME_ID) >= BLS_MAX_VOLUME_ID) {
    return EFI_INVALID_PARAMETER;
  }

  Context = AllocateZeroPool(sizeof(*Context));
  if (Context == NULL) return EFI_OUT_OF_RESOURCES;
  Root = NULL;
  TempDurable = FALSE;

  if (!BlsUnicodeToUtf8XmlEscape(Context->EscapedDefaultName, sizeof(Context->EscapedDefaultName), Config->DefaultBootloader) ||
      !BlsUnicodeToUtf8XmlEscape(Context->EscapedDefaultPath, sizeof(Context->EscapedDefaultPath), Config->DefaultPath) ||
      !BlsUnicodeToUtf8XmlEscape(Context->EscapedDefaultVolumeLabel, sizeof(Context->EscapedDefaultVolumeLabel), Config->DefaultVolumeLabel) ||
      !BlsUnicodeToUtf8XmlEscape(Context->EscapedDefaultVolumeDevicePath, sizeof(Context->EscapedDefaultVolumeDevicePath), Config->DefaultVolumeDevicePath)) {
    Status = EFI_INVALID_PARAMETER;
    goto Done;
  }
  if (Context->EscapedDefaultName[0] == '\0') {
    AsciiStrCpyS(Context->EscapedDefaultName, sizeof(Context->EscapedDefaultName), "OpenCore");
  }

  AsciiSPrint(Context->Buffer, sizeof(Context->Buffer),
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
    "<plist version=\"1.0\">\n"
    "<dict>\n"
    "  <key>DefaultBootloader</key>\n"
    "  <string>%a</string>\n"
    "  <key>DefaultPath</key>\n"
    "  <string>%a</string>\n"
    "  <key>DefaultVolumeLabel</key>\n"
    "  <string>%a</string>\n"
    "  <key>DefaultVolumeDevicePath</key>\n"
    "  <string>%a</string>\n"
    "  <key>Timeout</key>\n"
    "  <integer>%u</integer>\n"
    "  <key>Debug</key>\n"
    "  <%a/>\n"
    "  <key>ShowHardwareInfo</key>\n"
    "  <%a/>\n"
    "  <key>RememberLastSelected</key>\n"
    "  <%a/>\n"
    "  <key>HasRememberedSelection</key>\n"
    "  <%a/>\n"
    "</dict>\n"
    "</plist>\n",
    Context->EscapedDefaultName,
    Context->EscapedDefaultPath,
    Context->EscapedDefaultVolumeLabel,
    Context->EscapedDefaultVolumeDevicePath,
    (UINT32)Config->Timeout,
    Config->Debug ? "true" : "false",
    Config->ShowHardwareInfo ? "true" : "false",
    Config->RememberLastSelected ? "true" : "false",
    Config->HasRememberedSelection ? "true" : "false");

  Size = BlsAsciiLenBounded(Context->Buffer, sizeof(Context->Buffer));
  if (Size == 0 || Size >= sizeof(Context->Buffer) ||
      !BlsAsciiContains(Context->Buffer, "</dict>") ||
      !BlsAsciiContains(Context->Buffer, "</plist>")) {
    Status = EFI_BUFFER_TOO_SMALL;
    goto Done;
  }

  Status = BlsOpenVolumeRoot(VolumeHandle, &Root);
  if (Status != EFI_SUCCESS) goto Done;
  if (Root == NULL) {
    Status = EFI_DEVICE_ERROR;
    goto Done;
  }

  Status = BlsEnsureConfigDirectoryHierarchy(Root);
  if (Status != EFI_SUCCESS) goto Done;

  if (!PreserveDurableTemp) {
    Status = BlsDeleteFileIfPresent(Root, BLS_CONFIG_TMP);
    if (Status != EFI_SUCCESS) goto Done;

    Status = BlsWriteBufferToPath(Root, BLS_CONFIG_TMP, Context->Buffer, Size, &TempDurable);
    if (Status != EFI_SUCCESS) {
      /* Preserve a staging file that completed Write + Flush. Even when Close
         reports a lifecycle error, BLS.tmp remains the only recoverable copy of
         the requested settings and must not be deleted as if its data were partial. */
      if (!TempDurable) {
        CleanupStatus = BlsDeleteFileIfPresent(Root, BLS_CONFIG_TMP);
        if (CleanupStatus != EFI_SUCCESS) Status = CleanupStatus;
      }
      goto Done;
    }
  }

  Status = BlsDeleteFileIfPresent(Root, BLS_CONFIG_PATH);
  if (Status != EFI_SUCCESS) goto Done;

  Status = BlsWriteBufferToPath(Root, BLS_CONFIG_PATH, Context->Buffer, Size, NULL);
  if (Status == EFI_SUCCESS) {
    /* The final plist is already durable. A stale identical staging file is
       harmless and must not be reported as a failed settings save. */
    (VOID)BlsDeleteFileIfPresent(Root, BLS_CONFIG_TMP);
  }

Done:
  CleanupStatus = BlsFileClose(&Root);
  if (Status == EFI_SUCCESS && CleanupStatus != EFI_SUCCESS) Status = CleanupStatus;
  FreePool(Context);
  return Status;
}

EFI_STATUS BlsSaveConfig(IN EFI_HANDLE VolumeHandle, IN CONST BLS_CONFIG *Config) {
  return BlsSaveConfigInternal(VolumeHandle, Config, FALSE);
}


EFI_HANDLE BlsGetImageVolumeHandle(IN EFI_HANDLE ImageHandle) {
  EFI_LOADED_IMAGE_PROTOCOL *LoadedImage;
  EFI_STATUS Status;

  if (ImageHandle == NULL) return NULL;
  LoadedImage = NULL;
  Status = BlsGetProtocolInterface(ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&LoadedImage);
  if (Status != EFI_SUCCESS || LoadedImage == NULL || LoadedImage->DeviceHandle == NULL) return NULL;
  return LoadedImage->DeviceHandle;
}

static EFI_STATUS BlsLoadConfigSnapshot(
  IN EFI_HANDLE VolumeHandle,
  IN CONST CHAR16 *Path,
  OUT BLS_CONFIG *Config
  ) {
  EFI_STATUS Status;
  CHAR8 *Data;
  UINTN Size;
  BLS_CONFIG Parsed;

  if (VolumeHandle == NULL || Path == NULL || Config == NULL) return EFI_INVALID_PARAMETER;

  Data = NULL;
  Size = 0;
  Status = BlsReadWholeFile(VolumeHandle, Path, &Data, &Size);
  if (Status != EFI_SUCCESS) return Status;
  if (Data == NULL || Size == 0) {
    if (Data != NULL) FreePool(Data);
    return EFI_LOAD_ERROR;
  }

  BlsConfigDefaults(&Parsed);
  Status = BlsParseConfig(Data, &Parsed);
  if (Status != EFI_SUCCESS) {
    FreePool(Data);
    return Status;
  }
  FreePool(Data);
  CopyMem(Config, &Parsed, sizeof(*Config));
  return EFI_SUCCESS;
}

static EFI_STATUS BlsLoadConfigFromVolume(IN EFI_HANDLE VolumeHandle, OUT BLS_CONFIG *Config) {
  EFI_STATUS TempStatus;
  EFI_STATUS FinalStatus;
  EFI_STATUS SaveStatus;
  BLS_CONFIG Candidate;

  if (VolumeHandle == NULL || Config == NULL) return EFI_INVALID_PARAMETER;

  /* Without a transaction generation identifier, a valid leftover staging
     file cannot be proven newer than a valid final plist. Treat the final file
     as authoritative and use BLS.tmp only for recovery when final is missing
     or malformed. This prevents stale staging files from rolling settings back. */
  FinalStatus = BlsLoadConfigSnapshot(VolumeHandle, BLS_CONFIG_PATH, &Candidate);
  if (FinalStatus == EFI_SUCCESS) {
    /* A validated final plist is authoritative.  A leftover staging file is
       irrelevant at this point and must never be able to invalidate a known-good
       configuration merely because opening/reading it fails. */
    CopyMem(Config, &Candidate, sizeof(*Config));
    BlsDeleteFileOnVolumeBestEffort(VolumeHandle, BLS_CONFIG_TMP);
    if (Config->ForceRewrite) {
      SaveStatus = BlsSaveConfig(VolumeHandle, Config);
      if (SaveStatus == EFI_SUCCESS) Config->ForceRewrite = FALSE;
      else return EFI_WARN_WRITE_FAILURE;
    }
    return EFI_SUCCESS;
  }

  if (!BlsConfigReadMayFallback(FinalStatus)) return FinalStatus;

  TempStatus = BlsLoadConfigSnapshot(VolumeHandle, BLS_CONFIG_TMP, &Candidate);
  if (TempStatus == EFI_SUCCESS) {
    Candidate.ForceRewrite = TRUE;
    CopyMem(Config, &Candidate, sizeof(*Config));
    /* The final copy is unusable, so the durable staging file is now the only
       recoverable configuration. Finalize it without deleting staging first. */
    SaveStatus = BlsSaveConfigInternal(VolumeHandle, Config, TRUE);
    if (SaveStatus == EFI_SUCCESS) Config->ForceRewrite = FALSE;
    return (SaveStatus == EFI_SUCCESS) ? EFI_SUCCESS : EFI_WARN_WRITE_FAILURE;
  }

  if (!BlsConfigReadMayFallback(TempStatus)) return TempStatus;
  if (TempStatus == EFI_LOAD_ERROR || FinalStatus == EFI_LOAD_ERROR) return EFI_LOAD_ERROR;
  return EFI_NOT_FOUND;
}

EFI_STATUS BlsLoadConfig(IN EFI_HANDLE ImageHandle, OUT BLS_CONFIG *Config, OUT EFI_HANDLE *ConfigVolumeHandle) {
  EFI_STATUS Status;
  EFI_STATUS SaveStatus;
  EFI_STATUS FirstReadError;
  EFI_HANDLE ImageVolumeHandle;
  EFI_HANDLE *Handles;
  UINTN Count;
  UINTN Index;

  if (Config == NULL) return EFI_INVALID_PARAMETER;
  BlsConfigDefaults(Config);
  if (ConfigVolumeHandle != NULL) *ConfigVolumeHandle = NULL;

  ImageVolumeHandle = BlsGetImageVolumeHandle(ImageHandle);
  if (ImageVolumeHandle != NULL) {
    Status = BlsLoadConfigFromVolume(ImageVolumeHandle, Config);
    if (Status == EFI_SUCCESS || Status == EFI_WARN_WRITE_FAILURE) {
      if (ConfigVolumeHandle != NULL) *ConfigVolumeHandle = ImageVolumeHandle;
      return Status;
    }
    if (!BlsConfigReadMayFallback(Status)) return Status;

    BlsConfigDefaults(Config);
    if (Status == EFI_LOAD_ERROR) {
      /* Never destroy a present but malformed/future-version configuration.
         Defaults are safe for this session, while the original file remains
         available for recovery or manual correction.  Keep the volume handle
         unset so later automatic remember/options writes cannot overwrite it. */
      if (ConfigVolumeHandle != NULL) *ConfigVolumeHandle = NULL;
      return EFI_LOAD_ERROR;
    }

    if (ConfigVolumeHandle != NULL) *ConfigVolumeHandle = ImageVolumeHandle;
    SaveStatus = BlsSaveConfig(ImageVolumeHandle, Config);
    Config->ForceRewrite = (BOOLEAN)(SaveStatus != EFI_SUCCESS);
    return (SaveStatus == EFI_SUCCESS) ? EFI_SUCCESS : EFI_WARN_WRITE_FAILURE;
  }

  Handles = NULL;
  Count = 0;
  Status = BlsLocateFsHandles(&Handles, &Count);
  if (Status != EFI_SUCCESS || Handles == NULL || Count == 0) return Status;

  FirstReadError = EFI_NOT_FOUND;
  for (Index = 0; Index < Count; Index++) {
    if (Handles[Index] == NULL) continue;
    BlsConfigDefaults(Config);
    Status = BlsLoadConfigFromVolume(Handles[Index], Config);
    if (Status == EFI_SUCCESS || Status == EFI_WARN_WRITE_FAILURE) {
      if (ConfigVolumeHandle != NULL) *ConfigVolumeHandle = Handles[Index];
      FreePool(Handles);
      return Status;
    }
    if (!BlsConfigReadMayFallback(Status) && FirstReadError == EFI_NOT_FOUND) {
      FirstReadError = Status;
    }
  }

  FreePool(Handles);
  BlsConfigDefaults(Config);
  return FirstReadError;
}

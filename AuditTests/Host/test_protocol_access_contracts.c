/* Credits: ©DNMTechLabs / dnmtechlabs@gmail.com */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef NULL
#undef NULL
#endif
#include <Uefi.h>
#include <Guid/FileInfo.h>
#include <Library/BaseLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/SimpleFileSystem.h>
#include "../../BootLoaderSelectorPkg/BootLoaderSelector/Utils.h"
EFI_HANDLE gImageHandle = NULL;
EFI_SYSTEM_TABLE *gST = NULL;
EFI_BOOT_SERVICES *gBS = NULL;
EFI_GUID gEfiSimpleFileSystemProtocolGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
EFI_GUID gEfiFileInfoGuid = EFI_FILE_INFO_ID;
static EFI_HANDLE gExpectedHandle, gExpectedAgent;
static EFI_GUID gTestProtocol = {0x12345678U,0x1234U,0x5678U,{1,2,3,4,5,6,7,8}};
static VOID *gExpectedInterface = (VOID *)(UINTN)0xFEED0000U;
static EFI_STATUS gOpenStatus;
static BOOLEAN gWriteHostileOut;
static UINTN gOpenCalls;
VOID *EFIAPI AllocateZeroPool(IN UINTN Size) { return calloc(1U,(size_t)Size); }
VOID EFIAPI FreePool(IN JCONST VOID *Buffer) { free((VOID *)Buffer); }
UINTN EFIAPI AsciiStrLen(IN CONST CHAR8 *String) { return String ? (UINTN)strlen(String) : 0U; }
static EFI_STATUS EFIAPI TestOpenProtocol(IN EFI_HANDLE Handle, IN EFI_GUID *Protocol, OUT VOID **Interface,
  IN EFI_HANDLE AgentHandle, IN EFI_HANDLE ControllerHandle, IN UINT32 Attributes) {
  gOpenCalls++;
  assert(Handle == gExpectedHandle); assert(Protocol == &gTestProtocol);
  assert(AgentHandle == gExpectedAgent); assert(ControllerHandle == NULL);
  assert(Attributes == EFI_OPEN_PROTOCOL_GET_PROTOCOL); assert(Interface != NULL);
  if (gOpenStatus == EFI_SUCCESS) *Interface = gExpectedInterface;
  else if (gWriteHostileOut) *Interface = (VOID *)(UINTN)0xBADCAFEU;
  return gOpenStatus;
}
#include "../../BootLoaderSelectorPkg/BootLoaderSelector/Utils.c"
int main(void) {
  EFI_BOOT_SERVICES Bs; VOID *Interface; EFI_STATUS Status;
  memset(&Bs,0,sizeof(Bs)); gExpectedHandle=(EFI_HANDLE)(UINTN)0x1111U;
  gExpectedAgent=(EFI_HANDLE)(UINTN)0x2222U; gImageHandle=gExpectedAgent; gBS=&Bs;
  Interface=gExpectedInterface; Status=BlsGetProtocolInterface(gExpectedHandle,&gTestProtocol,&Interface);
  assert(Status==EFI_UNSUPPORTED && Interface==NULL && gOpenCalls==0U);
  Bs.OpenProtocol=TestOpenProtocol; gImageHandle=NULL; Interface=gExpectedInterface;
  Status=BlsGetProtocolInterface(gExpectedHandle,&gTestProtocol,&Interface);
  assert(Status==EFI_UNSUPPORTED && Interface==NULL && gOpenCalls==0U);
  gImageHandle=gExpectedAgent; gOpenStatus=EFI_SUCCESS; gWriteHostileOut=FALSE; Interface=NULL;
  Status=BlsGetProtocolInterface(gExpectedHandle,&gTestProtocol,&Interface);
  assert(Status==EFI_SUCCESS && Interface==gExpectedInterface && gOpenCalls==1U);
  gOpenStatus=EFI_DEVICE_ERROR; gWriteHostileOut=TRUE; Interface=gExpectedInterface;
  Status=BlsGetProtocolInterface(gExpectedHandle,&gTestProtocol,&Interface);
  assert(Status==EFI_DEVICE_ERROR && Interface==NULL && gOpenCalls==2U);
  Interface=gExpectedInterface; assert(BlsGetProtocolInterface(NULL,&gTestProtocol,&Interface)==EFI_INVALID_PARAMETER); assert(Interface==NULL);
  Interface=gExpectedInterface; assert(BlsGetProtocolInterface(gExpectedHandle,NULL,&Interface)==EFI_INVALID_PARAMETER); assert(Interface==NULL);
  assert(BlsGetProtocolInterface(gExpectedHandle,&gTestProtocol,NULL)==EFI_INVALID_PARAMETER); assert(gOpenCalls==2U);
  puts("protocol_access_contracts: PASS"); return 0;
}

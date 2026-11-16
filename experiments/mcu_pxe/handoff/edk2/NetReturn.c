#include <Uefi.h>
#include <Protocol/ShellParameters.h>
#include <Library/BaseLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include "NetHandoffProtocol.h"

#define RETURN_WAIT_US 30000000U
#define RETURN_POLL_US 100000U

/* 无参数只显示许可；仅用户输入 return 才开始拆卸和归还，不判断测试 PASS/FAIL。 */
EFI_STATUS EFIAPI NetReturnEntry (EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_GUID Guid = NET_HANDOFF_PROTOCOL_GUID;
  NET_HANDOFF_PROTOCOL *Control;
  NET_HANDOFF_INFO Info;
  EFI_SHELL_PARAMETERS_PROTOCOL *Args = NULL;
  EFI_STATUS Status;
  UINT64 Begin;
  (VOID)SystemTable;
  Status = gBS->LocateProtocol (&Guid, NULL, (VOID **)&Control);
  if (EFI_ERROR (Status)) {
    Print (L"No authorized SerialNet handoff driver: %r\n", Status);
    return Status;
  }
  Status = Control->GetInfo (Control, &Info);
  if (EFI_ERROR (Status)) return Status;
  if (Info.Revision != NET_HANDOFF_REVISION) return EFI_UNSUPPORTED;
  Print (L"SerialNet session=%u state=%u IP=%u.%u.%u.%u\n",
         Info.Session, Info.State, Info.Ip[0], Info.Ip[1], Info.Ip[2], Info.Ip[3]);
  (VOID)gBS->HandleProtocol (ImageHandle, &gEfiShellParametersProtocolGuid, (VOID **)&Args);
  if (Args == NULL || Args->Argc == 1) {
    Print (L"After MemTest86 exits, run: NetReturn.efi return\n");
    return EFI_SUCCESS;
  }
  if (Args->Argc != 2 || StrCmp (Args->Argv[1], L"return") != 0) {
    Print (L"Usage: NetReturn.efi [return]\n");
    return EFI_INVALID_PARAMETER;
  }
  Begin = GetTimeInNanoSecond (GetPerformanceCounter ()) / 1000U;
  do {
    Status = Control->ReturnNetwork (Control);
    if (Status != EFI_NOT_READY) break;
    /* MCU 排空以太发送并恢复 IP 后才应答成功；每次查询使用新的事务序号。 */
    MicroSecondDelay (RETURN_POLL_US);
  } while (GetTimeInNanoSecond (GetPerformanceCounter ()) / 1000U - Begin < RETURN_WAIT_US);
  if (!EFI_ERROR (Status)) {
    Print (L"Network returned to MCU. PC may reconnect to the MCU address.\n");
  } else {
    Print (L"Return incomplete: %r. Network stays disabled; retry NetReturn.efi return.\n", Status);
  }
  return Status;
}

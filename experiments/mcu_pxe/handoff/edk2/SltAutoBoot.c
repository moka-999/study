/* 授权后的单次PXE编排；应用返回后归还网络，不把EFI返回值当作DDR测试结论。 */
#include "SltAutoBoot.h"
#include "NetHandoffProtocol.h"
#include <Library/BaseMemoryLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootManagerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/SimpleNetwork.h>

#define SLT_MANUAL_WINDOW_US  3000000U
#define SLT_KEY_POLL_US       10000U
#define SLT_RETURN_WINDOW_US  30000000U
#define SLT_RETURN_POLL_US    10000U

STATIC EFI_GUID mSltHandoffGuid = NET_HANDOFF_PROTOCOL_GUID;
STATIC BOOLEAN mSltAttempted;

/* 只接受授权网卡的完整IPv4 DHCP路径，排除其它MAC、IPv6和HTTP路径。 */
STATIC BOOLEAN IsSltIpv4Path (
  CONST EFI_DEVICE_PATH_PROTOCOL *Path,
  CONST EFI_DEVICE_PATH_PROTOCOL *NicPath
  )
{
  UINTN NicSize, PrefixSize;
  CONST IPv4_DEVICE_PATH *Ip;
  if (Path == NULL || NicPath == NULL) return FALSE;
  NicSize = GetDevicePathSize (NicPath);
  if (NicSize <= END_DEVICE_PATH_LENGTH) return FALSE;
  PrefixSize = NicSize - END_DEVICE_PATH_LENGTH;
  if (GetDevicePathSize (Path) != PrefixSize + sizeof (*Ip) + END_DEVICE_PATH_LENGTH ||
      CompareMem (Path, NicPath, PrefixSize) != 0) return FALSE;
  Ip = (CONST IPv4_DEVICE_PATH *)((CONST UINT8 *)Path + PrefixSize);
  return DevicePathType (&Ip->Header) == MESSAGING_DEVICE_PATH &&
         DevicePathSubType (&Ip->Header) == MSG_IPv4_DP &&
         DevicePathNodeLength (&Ip->Header) == sizeof (*Ip) &&
         !Ip->StaticIpAddress && IsDevicePathEnd (NextDevicePathNode (&Ip->Header));
}

/* 仅ESC跳过自动运行，保留授权网卡供手动PXE或NetReturn；不写持久化设置。 */
STATIC BOOLEAN ManualRequested (VOID)
{
  UINTN Elapsed;
  EFI_INPUT_KEY Key;
  for (Elapsed = 0; Elapsed < SLT_MANUAL_WINDOW_US; Elapsed += SLT_KEY_POLL_US) {
    if (gST->ConIn != NULL &&
        !EFI_ERROR (gST->ConIn->ReadKeyStroke (gST->ConIn, &Key)) &&
        Key.ScanCode == SCAN_ESC) return TRUE;
    MicroSecondDelay (SLT_KEY_POLL_US);
  }
  return FALSE;
}

/* 只在应用已经返回/未能启动时调用；NOT_READY有界等待，其他错误留给手动重试。 */
STATIC EFI_STATUS ReturnSltNetwork (NET_HANDOFF_PROTOCOL *Control)
{
  UINT64 Begin;
  EFI_STATUS Status;
  Begin = GetTimeInNanoSecond (GetPerformanceCounter ()) / 1000U;
  do {
    Status = Control->ReturnNetwork (Control);
    if (Status != EFI_NOT_READY) return Status;
    MicroSecondDelay (SLT_RETURN_POLL_US);
  } while (GetTimeInNanoSecond (GetPerformanceCounter ()) / 1000U - Begin < SLT_RETURN_WINDOW_US);
  return EFI_TIMEOUT;
}

BOOLEAN SltAutoBoot (VOID)
{
  EFI_HANDLE *Handles;
  UINTN Count, Index, OptionCount;
  NET_HANDOFF_PROTOCOL *Control;
  NET_HANDOFF_INFO Info;
  EFI_SIMPLE_NETWORK_PROTOCOL *Snp;
  EFI_DEVICE_PATH_PROTOCOL *NicPath;
  EFI_BOOT_MANAGER_LOAD_OPTION *Options;
  EFI_STATUS Status;

  if (mSltAttempted) return FALSE;
  Handles = NULL;
  Status = gBS->LocateHandleBuffer (ByProtocol, &mSltHandoffGuid, NULL, &Count, &Handles);
  if (EFI_ERROR (Status)) return FALSE;
  if (Count != 1) {
    FreePool (Handles);
    Print (L"[slt-auto] Ambiguous handoff controllers; use manual menu.\n");
    return TRUE;
  }
  Status = gBS->HandleProtocol (Handles[0], &mSltHandoffGuid, (VOID **)&Control);
  if (EFI_ERROR (Status)) { FreePool (Handles); return FALSE; }
  Status = Control->GetInfo (Control, &Info);
  if (EFI_ERROR (Status) || Info.Revision != NET_HANDOFF_REVISION ||
      Info.State != NET_HANDOFF_ACTIVE) { FreePool (Handles); return FALSE; }

  mSltAttempted = TRUE;
  Print (L"\nSLT-AUTO-20261111 session=%u IP=%u.%u.%u.%u\n",
         Info.Session, Info.Ip[0], Info.Ip[1], Info.Ip[2], Info.Ip[3]);
  Print (L"[slt-auto] ESC within 3 seconds for manual mode.\n");
  if (ManualRequested ()) {
    FreePool (Handles);
    Print (L"[slt-auto] Manual mode; network remains with RK.\n");
    return TRUE;
  }

  Status = gBS->HandleProtocol (Handles[0], &gEfiSimpleNetworkProtocolGuid, (VOID **)&Snp);
  NicPath = DevicePathFromHandle (Handles[0]);
  if (!EFI_ERROR (Status) && NicPath != NULL && Snp->Mode != NULL) {
    Status = gBS->ConnectController (Handles[0], NULL, NULL, TRUE);
    /* 已经连接的网卡也可能没有新驱动可绑定，仍核对现有完整PXE启动项。 */
    if (!EFI_ERROR (Status) || Status == EFI_ALREADY_STARTED || Status == EFI_NOT_FOUND) {
      EfiBootManagerRefreshAllBootOption ();
      Options = EfiBootManagerGetLoadOptions (&OptionCount, LoadOptionTypeBoot);
      Status = EFI_NOT_FOUND;
      for (Index = 0; Index < OptionCount; ++Index) {
        if ((Options[Index].Attributes & LOAD_OPTION_ACTIVE) == 0 ||
            (Options[Index].Attributes & LOAD_OPTION_CATEGORY) != LOAD_OPTION_CATEGORY_BOOT ||
            !IsSltIpv4Path (Options[Index].FilePath, NicPath)) continue;
        Print (L"[slt-auto] PXEv4 start: %s\n", Options[Index].Description);
        /* Boot同步返回才表示应用已退出；测试FAIL仍由PC读取上传报告判断。 */
        EfiBootManagerBoot (&Options[Index]);
        Status = Options[Index].Status;
        break;
      }
      EfiBootManagerFreeLoadOptions (Options, OptionCount);
    }
  } else if (!EFI_ERROR (Status)) Status = EFI_NOT_FOUND;
  FreePool (Handles);

  Print (L"[slt-auto] PXE/application returned: %r; test result is in PC report.\n", Status);
  Print (L"[slt-auto] Returning network to MCU.\n");
  Status = ReturnSltNetwork (Control);
  if (EFI_ERROR (Status)) {
    Print (L"[slt-auto] Return incomplete: %r; retry NetReturn.efi return.\n", Status);
  } else {
    Print (L"[slt-auto] Network returned to MCU. PC may reconnect to the MCU address.\n");
  }
  return TRUE;
}

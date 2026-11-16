#ifndef NET_HANDOFF_PROTOCOL_H
#define NET_HANDOFF_PROTOCOL_H

#include <Uefi.h>

/* 仅供本机 UEFI 应用控制当前网卡；不表示 PC 测试结果或 DHCP 租约状态。 */
#define NET_HANDOFF_PROTOCOL_GUID \
  {0xf01e4dc1, 0x77dd, 0x47f0, {0x87, 0x4b, 0x91, 0x43, 0x62, 0x6c, 0x03, 0x21}}
#define NET_HANDOFF_REVISION 1U
#define NET_HANDOFF_ACTIVE 1U
#define NET_HANDOFF_RETURNING 2U
#define NET_HANDOFF_RELEASED 3U

typedef struct {
  UINT32 Revision;
  UINT32 Session;
  UINT32 State;
  UINT8 Ip[4];
  UINT8 Mask[4];
  UINT8 Gateway[4];
} NET_HANDOFF_INFO;

typedef struct NET_HANDOFF_PROTOCOL NET_HANDOFF_PROTOCOL;
struct NET_HANDOFF_PROTOCOL {
  EFI_STATUS (EFIAPI *GetInfo)(NET_HANDOFF_PROTOCOL *This, NET_HANDOFF_INFO *Info);
  /* 仅在测试应用已经退出后调用；失败保持禁发，调用方可再次显式重试。 */
  EFI_STATUS (EFIAPI *ReturnNetwork)(NET_HANDOFF_PROTOCOL *This);
};

#endif

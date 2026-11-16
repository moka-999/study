#ifndef HANDOFF_DHCP4_H
#define HANDOFF_DHCP4_H

#include <Uefi.h>
#include <Protocol/Dhcp4.h>

/* 仅在首次DISCOVER发送前调用；普通网卡保持原包，授权网卡申请本轮实际IP。 */
EFI_STATUS HandoffDhcp4Discover (
  EFI_HANDLE Controller, EFI_DHCP4_PROTOCOL *Dhcp4,
  EFI_DHCP4_PACKET *Packet, EFI_DHCP4_PACKET **NewPacket
  );

#endif

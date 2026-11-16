#include "HandoffDhcp4.h"
#include "NetHandoffProtocol.h"
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/UefiBootServicesTableLib.h>

#define DHCP_OPTION_REQUESTED_IP 50U
#define IPV4_ADDRESS_BYTES 4U

EFI_STATUS HandoffDhcp4Discover (
  EFI_HANDLE Controller, EFI_DHCP4_PROTOCOL *Dhcp4,
  EFI_DHCP4_PACKET *Packet, EFI_DHCP4_PACKET **NewPacket
  )
{
  EFI_GUID Guid = NET_HANDOFF_PROTOCOL_GUID;
  NET_HANDOFF_PROTOCOL *Control;
  NET_HANDOFF_INFO Info;
  EFI_STATUS Status;
  struct { UINT8 Code, Length, Ip[IPV4_ADDRESS_BYTES]; } Requested;
  EFI_DHCP4_PACKET_OPTION *Options[1];

  if (Dhcp4 == NULL || Packet == NULL || NewPacket == NULL) return EFI_INVALID_PARAMETER;
  *NewPacket = NULL;
  Status = gBS->HandleProtocol (Controller, &Guid, (VOID **)&Control);
  if (Status == EFI_UNSUPPORTED || Status == EFI_NOT_FOUND) return EFI_SUCCESS;
  if (EFI_ERROR (Status)) return Status;
  Status = Control->GetInfo (Control, &Info);
  if (EFI_ERROR (Status)) return Status;
  if (Info.Revision != NET_HANDOFF_REVISION || Info.State != NET_HANDOFF_ACTIVE)
    return EFI_ACCESS_DENIED;

  Requested.Code = DHCP_OPTION_REQUESTED_IP;
  Requested.Length = IPV4_ADDRESS_BYTES;
  CopyMem (Requested.Ip, Info.Ip, sizeof (Requested.Ip));
  Options[0] = (EFI_DHCP4_PACKET_OPTION *)&Requested;
  /* Build替换已有同号选项，避免重复50；新包由DHCP驱动接管并释放。 */
  Status = Dhcp4->Build (Dhcp4, Packet, 0, NULL, 1, Options, NewPacket);
  if (!EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "SerialNet: DHCP requested IP=%u.%u.%u.%u session=%u\n",
      Info.Ip[0], Info.Ip[1], Info.Ip[2], Info.Ip[3], Info.Session));
  }
  return Status;
}

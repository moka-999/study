#include <Uefi.h>
#include <Protocol/SimpleNetwork.h>
#include <Protocol/DevicePath.h>
#include <Protocol/PxeBaseCode.h>
#include <Guid/EventGroup.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include "serial_net.h"

/* B1原理图第6/14页：GPIO2_B6/RX、GPIO2_B7/TX经U4008接MCU UART4。 */
#define UART_BASE 0xFEB40000U
#define UART_USR  0x7CU
#define UART_IOMUX 0xFD5F804CU
#define UART_INPUT 0xFD5FA194U
#define UART_CLKSEL 0xFD7C03ACU
#define UART_CLKGATE 0xFD7C0830U
#define UART_RESET 0xFD7C0A30U
#define UART_CLOCK_MASK (BIT2 | BIT13)
/* IPv4初始化会加入224.0.0.1；MCU配套接收IPv4组播，不开放其它单播。 */
#define FILTERS (EFI_SIMPLE_NETWORK_RECEIVE_UNICAST | EFI_SIMPLE_NETWORK_RECEIVE_BROADCAST | \
                 EFI_SIMPLE_NETWORK_RECEIVE_PROMISCUOUS_MULTICAST)

STATIC EFI_SIMPLE_NETWORK_PROTOCOL mSnp;
STATIC EFI_SIMPLE_NETWORK_MODE mMode;
STATIC EFI_HANDLE mHandle;
STATIC struct { MAC_ADDR_DEVICE_PATH Mac; EFI_DEVICE_PATH_PROTOCOL End; } mPath;
STATIC EFI_LOCK mLock = EFI_INITIALIZE_LOCK_VARIABLE (TPL_NOTIFY);
STATIC sn_frame_t mRequest, mResponse;
STATIC sn_parser_t mParser;
STATIC UINT8 mWire[SN_WIRE_MAX], mPacket[SN_ETH_MAX];
STATIC UINTN mPacketSize;
STATIC VOID *mRecycle;
STATIC UINT32 mSession, mSequence;
STATIC UINT64 mLastPoll;
STATIC BOOLEAN mFailed;

STATIC UINT64 NowUs (VOID)
{
  return GetTimeInNanoSecond (GetPerformanceCounter ()) / 1000U;
}

STATIC EFI_STATUS UartInit (VOID)
{
  UINT32 Mux, Clock, Gate, Reset, Input, Lcr, Divisor = MAX_UINT32;
  /* UART1使用主CRU；切换24MHz时保持本外设复位，不改共享PLL或UART2。 */
  MmioWrite32 (UART_CLKGATE, UART_CLOCK_MASK << 16);
  MmioWrite32 (UART_RESET, (UART_CLOCK_MASK << 16) | UART_CLOCK_MASK);
  MmioWrite32 (UART_CLKSEL, 0x00030002U);
  MicroSecondDelay (10);
  MmioWrite32 (UART_RESET, UART_CLOCK_MASK << 16);
  MicroSecondDelay (10);
  /* 仅配置B6/B7，保留同寄存器B4/B5的FSPI等复用；RX输入显式使能。 */
  MmioWrite32 (UART_IOMUX, 0xFF00AA00U);
  MmioWrite32 (UART_INPUT, (BIT6 << 16) | BIT6);
  MmioWrite32 (UART_BASE + 0x88U, 7U);
  MicroSecondDelay (100);
  MmioWrite32 (UART_BASE + 0x04U, 0U);
  MmioWrite32 (UART_BASE + 0x10U, 0U);
  MmioWrite32 (UART_BASE + 0x0CU, 0x83U);
  /* 24MHz / (16 * 13) = 115384.6；相对115200偏差约0.16%。 */
  MmioWrite32 (UART_BASE, 13U);
  MmioWrite32 (UART_BASE + 0x04U, 0U);
  /* DLAB确认后才读DLL/DLH，避免把RBR中的接收字节当成分频值。 */
  Lcr = MmioRead32 (UART_BASE + 0x0CU);
  if ((Lcr & BIT7) != 0)
    Divisor = (MmioRead32 (UART_BASE) & 0xFFU) |
              ((MmioRead32 (UART_BASE + 0x04U) & 0xFFU) << 8);
  MmioWrite32 (UART_BASE + 0x0CU, 3U);
  MmioWrite32 (UART_BASE + 0x98U, 1U);
  MmioWrite32 (UART_BASE + 0x9CU, 0U);
  MmioWrite32 (UART_BASE + 0xA0U, 0U);
  Mux = MmioRead32 (UART_IOMUX);
  Clock = MmioRead32 (UART_CLKSEL);
  Gate = MmioRead32 (UART_CLKGATE);
  Reset = MmioRead32 (UART_RESET);
  Input = MmioRead32 (UART_INPUT);
  Lcr = MmioRead32 (UART_BASE + 0x0CU);
  DEBUG ((DEBUG_ERROR, "SerialNet: UART1_M0 diag=4 mux=%08x clk=%08x gate=%08x rst=%08x ie=%08x lcr=%02x div=%u usr=%02x\n",
          Mux, Clock, Gate, Reset, Input, Lcr, Divisor, MmioRead32 (UART_BASE + UART_USR)));
  if ((Mux & 0xFF00U) != 0xAA00U || (Clock & 3U) != 2U ||
      (Gate & UART_CLOCK_MASK) != 0 || (Reset & UART_CLOCK_MASK) != 0 || (Input & BIT6) == 0 ||
      Lcr != 3U || Divisor != 13U) return EFI_DEVICE_ERROR;
  return EFI_SUCCESS;
}

/* 调用者持有TPL_NOTIFY锁，收包期间不让网络定时回调重入同一UART事务。 */
STATIC EFI_STATUS Rpc (UINT8 Op, CONST VOID *Data, UINTN Size)
{
  UINTN Attempt, Position;
  UINT16 WireSize;
  UINT64 Begin;
  if (mFailed) return EFI_DEVICE_ERROR;
  if (Size > SN_ETH_MAX || (Size && !Data)) return EFI_INVALID_PARAMETER;
  mRequest.op = Op; mRequest.status = 0; mRequest.session = mSession;
  mRequest.sequence = ++mSequence; mRequest.length = (UINT16)Size;
  if (Size) CopyMem (mRequest.data, Data, Size);
  WireSize = sn_pack (&mRequest, mWire, sizeof (mWire));
  for (Attempt = 0; Attempt < 3; ++Attempt) {
    UINTN Received = 0, Bad = 0, Unmatched = 0;
    /* 超时重试同一请求，MCU重复应答，不重复网络发送或领取下一接收包。 */
    sn_parser_reset (&mParser);
    Begin = NowUs ();
    for (Position = 0; Position < WireSize && NowUs () - Begin < 4000000U;) {
      if ((MmioRead32 (UART_BASE + UART_USR) & BIT1) != 0)
        MmioWrite32 (UART_BASE, mWire[Position++]);
      else MicroSecondDelay (10);
    }
    while (Position == WireSize && NowUs () - Begin < 4000000U) {
      if ((MmioRead32 (UART_BASE + UART_USR) & BIT3) != 0) {
        int Result = sn_feed (&mParser, (UINT8)MmioRead32 (UART_BASE), &mResponse);
        ++Received;
        if (Result < 0) ++Bad;
        if (Result == 1 && mResponse.op == (Op | SN_REPLY) &&
            mResponse.session == mSession && mResponse.sequence == mSequence) {
          if (Op == SN_HELLO) {
            DEBUG ((DEBUG_ERROR, "SerialNet: HELLO reply status=%u tx=%u rx=%u\n",
                    mResponse.status, (UINT32)Position, (UINT32)Received));
          }
          if (mResponse.status == SN_OK) return EFI_SUCCESS;
          if (mResponse.status == SN_EMPTY) return EFI_NOT_READY;
          if (mResponse.status == SN_NOT_READY) return EFI_NO_MEDIA;
          return EFI_DEVICE_ERROR;
        }
        if (Result == 1) ++Unmatched;
      } else MicroSecondDelay (10);
    }
    /* 只在一次事务超时后打印，避免逐字节日志改变收发时序。 */
    DEBUG ((DEBUG_ERROR, "SerialNet: attempt=%u op=%u seq=%u tx=%u/%u rx=%u bad=%u other=%u usr=%02x\n",
            (UINT32)Attempt + 1U, Op, mSequence, (UINT32)Position, WireSize,
            (UINT32)Received, (UINT32)Bad, (UINT32)Unmatched, MmioRead32 (UART_BASE + UART_USR)));
  }
  /* 不跳过未知事务继续发送新序号；复位RK/重载驱动后重新握手。 */
  mFailed = TRUE; mMode.MediaPresent = FALSE;
  DEBUG ((DEBUG_ERROR, "SerialNet: UART transaction timeout op=%u seq=%u\n", Op, mSequence));
  return EFI_TIMEOUT;
}

STATIC EFI_STATUS Ready (EFI_SIMPLE_NETWORK_PROTOCOL *This)
{
  if (This != &mSnp) return EFI_INVALID_PARAMETER;
  if (mMode.State == EfiSimpleNetworkStopped) return EFI_NOT_STARTED;
  if (mMode.State != EfiSimpleNetworkInitialized) return EFI_DEVICE_ERROR;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS Poll (VOID)
{
  EFI_STATUS Status;
  if (mPacketSize) return EFI_SUCCESS;
  if (NowUs () - mLastPoll < 10000U) return EFI_NOT_READY;
  mLastPoll = NowUs ();
  Status = Rpc (SN_POLL, NULL, 0);
  if (Status == EFI_NO_MEDIA) mMode.MediaPresent = FALSE;
  if (Status == EFI_NOT_READY || !EFI_ERROR (Status)) mMode.MediaPresent = TRUE;
  if (!EFI_ERROR (Status)) {
    STATIC CONST UINT8 Broadcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    UINT8 *P = mResponse.data;
    UINT32 Filter = 0;
    if (mResponse.length < 14) return EFI_NOT_READY;
    if (CompareMem (P, mMode.CurrentAddress.Addr, 6) == 0)
      Filter = EFI_SIMPLE_NETWORK_RECEIVE_UNICAST;
    else if (CompareMem (P, Broadcast, 6) == 0)
      Filter = EFI_SIMPLE_NETWORK_RECEIVE_BROADCAST;
    else if ((P[0] & 1U) != 0)
      Filter = EFI_SIMPLE_NETWORK_RECEIVE_PROMISCUOUS_MULTICAST;
    if ((mMode.ReceiveFilterSetting & Filter) == 0) return EFI_NOT_READY;
    mPacketSize = mResponse.length; CopyMem (mPacket, P, mPacketSize);
  }
  return Status;
}

STATIC EFI_STATUS EFIAPI Start (EFI_SIMPLE_NETWORK_PROTOCOL *This)
{
  if (This != &mSnp) return EFI_INVALID_PARAMETER;
  if (mMode.State != EfiSimpleNetworkStopped) return EFI_ALREADY_STARTED;
  mMode.State = EfiSimpleNetworkStarted;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI Initialize (EFI_SIMPLE_NETWORK_PROTOCOL *This, UINTN Rx, UINTN Tx)
{
  EFI_STATUS Status;
  if (This != &mSnp) return EFI_INVALID_PARAMETER;
  if (mMode.State == EfiSimpleNetworkStopped) return EFI_NOT_STARTED;
  if (mMode.State == EfiSimpleNetworkInitialized) return EFI_ALREADY_STARTED;
  if (Rx || Tx) return EFI_UNSUPPORTED;
  Status = EfiAcquireLockOrFail (&mLock); if (EFI_ERROR (Status)) return EFI_NOT_READY;
  Status = Rpc (SN_RESET, NULL, 0);
  if (!EFI_ERROR (Status)) {
    mPacketSize = 0; mRecycle = NULL; mMode.State = EfiSimpleNetworkInitialized;
  }
  EfiReleaseLock (&mLock); return Status;
}

STATIC EFI_STATUS EFIAPI Reset (EFI_SIMPLE_NETWORK_PROTOCOL *This, BOOLEAN Extended)
{
  EFI_STATUS Status = Ready (This);
  (VOID)Extended;
  if (EFI_ERROR (Status)) return Status;
  Status = EfiAcquireLockOrFail (&mLock); if (EFI_ERROR (Status)) return EFI_NOT_READY;
  Status = Rpc (SN_RESET, NULL, 0);
  if (!EFI_ERROR (Status)) { mPacketSize = 0; mRecycle = NULL; }
  EfiReleaseLock (&mLock); return Status;
}

STATIC EFI_STATUS EFIAPI Shutdown (EFI_SIMPLE_NETWORK_PROTOCOL *This)
{
  EFI_STATUS Status = Ready (This);
  if (EFI_ERROR (Status)) return Status;
  Status = EfiAcquireLockOrFail (&mLock); if (EFI_ERROR (Status)) return EFI_NOT_READY;
  Status = Rpc (SN_STOP, NULL, 0);
  if (!EFI_ERROR (Status)) {
    mPacketSize = 0; mRecycle = NULL; mMode.State = EfiSimpleNetworkStarted;
  }
  EfiReleaseLock (&mLock); return Status;
}

STATIC EFI_STATUS EFIAPI Stop (EFI_SIMPLE_NETWORK_PROTOCOL *This)
{
  if (This != &mSnp) return EFI_INVALID_PARAMETER;
  if (mMode.State == EfiSimpleNetworkStopped) return EFI_NOT_STARTED;
  if (mMode.State != EfiSimpleNetworkStarted) return EFI_DEVICE_ERROR;
  mMode.State = EfiSimpleNetworkStopped; return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI ReceiveFilters (EFI_SIMPLE_NETWORK_PROTOCOL *This, UINT32 Enable,
    UINT32 Disable, BOOLEAN ResetMcast, UINTN Count, EFI_MAC_ADDRESS *Mac)
{
  EFI_STATUS Status = Ready (This);
  (VOID)Mac;
  if (EFI_ERROR (Status)) return Status;
  /* MaxMCastFilterCount=0：仅支持全组播；清空列表时忽略Count/Mac。 */
  if (((Enable | Disable) & ~FILTERS) || (!ResetMcast && Count))
    Status = EFI_INVALID_PARAMETER;
  else
    mMode.ReceiveFilterSetting = (mMode.ReceiveFilterSetting | Enable) & ~Disable;
  DEBUG ((DEBUG_ERROR, "SerialNet: filters enable=%02x disable=%02x reset=%u count=%u setting=%02x status=%r\n",
          Enable, Disable, ResetMcast, (UINT32)Count, mMode.ReceiveFilterSetting, Status));
  return Status;
}

STATIC EFI_STATUS EFIAPI Station (EFI_SIMPLE_NETWORK_PROTOCOL *This, BOOLEAN ResetAddress, EFI_MAC_ADDRESS *New)
{
  EFI_STATUS Status = Ready (This);
  if (EFI_ERROR (Status)) return Status;
  if (ResetAddress || (New && CompareMem (New, &mMode.PermanentAddress, 6) == 0)) return EFI_SUCCESS;
  return EFI_UNSUPPORTED;
}

STATIC EFI_STATUS EFIAPI Statistics (EFI_SIMPLE_NETWORK_PROTOCOL *This, BOOLEAN ResetStats,
                                    UINTN *Size, EFI_NETWORK_STATISTICS *Stats)
{
  (VOID)This; (VOID)ResetStats; (VOID)Size; (VOID)Stats; return EFI_UNSUPPORTED;
}
STATIC EFI_STATUS EFIAPI Mcast (EFI_SIMPLE_NETWORK_PROTOCOL *This, BOOLEAN Ipv6,
                              EFI_IP_ADDRESS *Ip, EFI_MAC_ADDRESS *Mac)
{
  EFI_STATUS Status = Ready (This);
  if (EFI_ERROR (Status)) return Status;
  if (!Ip || !Mac) return EFI_INVALID_PARAMETER;
  if (Ipv6) return EFI_UNSUPPORTED;
  if ((Ip->v4.Addr[0] & 0xF0U) != 0xE0U) return EFI_INVALID_PARAMETER;
  /* IPv4组播地址的低23位映射到01:00:5e，IPv6不在本实验范围内。 */
  ZeroMem (Mac, sizeof (*Mac));
  Mac->Addr[0] = 1; Mac->Addr[2] = 0x5E;
  Mac->Addr[3] = Ip->v4.Addr[1] & 0x7FU;
  Mac->Addr[4] = Ip->v4.Addr[2]; Mac->Addr[5] = Ip->v4.Addr[3];
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI NvData (EFI_SIMPLE_NETWORK_PROTOCOL *This, BOOLEAN Read,
                               UINTN Offset, UINTN Size, VOID *Buffer)
{
  (VOID)This; (VOID)Read; (VOID)Offset; (VOID)Size; (VOID)Buffer; return EFI_UNSUPPORTED;
}

STATIC EFI_STATUS EFIAPI GetStatus (EFI_SIMPLE_NETWORK_PROTOCOL *This, UINT32 *Interrupt, VOID **TxBuffer)
{
  EFI_STATUS Status = Ready (This);
  if (EFI_ERROR (Status)) return Status;
  if (!Interrupt && !TxBuffer) return EFI_INVALID_PARAMETER;
  Status = EfiAcquireLockOrFail (&mLock); if (EFI_ERROR (Status)) return EFI_NOT_READY;
  if (Interrupt) {
    (VOID)Poll ();
    *Interrupt = (mPacketSize ? EFI_SIMPLE_NETWORK_RECEIVE_INTERRUPT : 0) |
                 (mRecycle ? EFI_SIMPLE_NETWORK_TRANSMIT_INTERRUPT : 0);
  }
  if (TxBuffer) { *TxBuffer = mRecycle; mRecycle = NULL; }
  EfiReleaseLock (&mLock); return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI Transmit (EFI_SIMPLE_NETWORK_PROTOCOL *This, UINTN HeaderSize,
    UINTN Size, VOID *Buffer, EFI_MAC_ADDRESS *Src, EFI_MAC_ADDRESS *Dst, UINT16 *Protocol)
{
  EFI_STATUS Status = Ready (This);
  UINT8 *P = Buffer;
  if (EFI_ERROR (Status)) return Status;
  if (!P || Size < 14 || Size > SN_ETH_MAX ||
      (HeaderSize && (HeaderSize != 14 || !Dst || !Protocol))) return EFI_INVALID_PARAMETER;
  Status = EfiAcquireLockOrFail (&mLock); if (EFI_ERROR (Status)) return EFI_NOT_READY;
  if (mRecycle) { EfiReleaseLock (&mLock); return EFI_NOT_READY; }
  if (HeaderSize) {
    CopyMem (P, Dst, 6); CopyMem (P + 6, Src ? Src : &mMode.CurrentAddress, 6);
    P[12] = (UINT8)(*Protocol >> 8); P[13] = (UINT8)*Protocol;
  }
  Status = Rpc (SN_SEND, Buffer, Size);
  if (!EFI_ERROR (Status)) mRecycle = Buffer;
  EfiReleaseLock (&mLock); return Status;
}

STATIC EFI_STATUS EFIAPI Receive (EFI_SIMPLE_NETWORK_PROTOCOL *This, UINTN *HeaderSize,
    UINTN *Size, VOID *Buffer, EFI_MAC_ADDRESS *Src, EFI_MAC_ADDRESS *Dst, UINT16 *Protocol)
{
  EFI_STATUS Status = Ready (This);
  if (EFI_ERROR (Status)) return Status;
  if (!Size || !Buffer) return EFI_INVALID_PARAMETER;
  Status = EfiAcquireLockOrFail (&mLock); if (EFI_ERROR (Status)) return EFI_NOT_READY;
  Status = Poll ();
  if (!EFI_ERROR (Status)) {
    if (*Size < mPacketSize) { *Size = mPacketSize; Status = EFI_BUFFER_TOO_SMALL; }
    else {
      *Size = mPacketSize; CopyMem (Buffer, mPacket, mPacketSize);
      if (HeaderSize) *HeaderSize = 14;
      if (Dst) { ZeroMem (Dst, sizeof (*Dst)); CopyMem (Dst, mPacket, 6); }
      if (Src) { ZeroMem (Src, sizeof (*Src)); CopyMem (Src, mPacket + 6, 6); }
      if (Protocol) *Protocol = (UINT16)((UINT16)mPacket[12] << 8 | mPacket[13]);
      mPacketSize = 0;
    }
  }
  EfiReleaseLock (&mLock); return Status;
}

STATIC VOID EFIAPI WaitPacket (EFI_EVENT Event, VOID *Context)
{
  (VOID)Context;
  if (EFI_ERROR (Ready (&mSnp)) || EFI_ERROR (EfiAcquireLockOrFail (&mLock))) return;
  if (!EFI_ERROR (Poll ())) gBS->SignalEvent (Event);
  EfiReleaseLock (&mLock);
}

STATIC VOID EFIAPI ConnectNetwork (EFI_EVENT Event, VOID *Context)
{
  EFI_STATUS Status;
  UINTN Pass;
  VOID *Pxe;
  (VOID)Context;
  for (Pass = 0; Pass < 10; ++Pass) {
    Status = gBS->ConnectController (mHandle, NULL, NULL, TRUE);
    DEBUG ((DEBUG_ERROR, "SerialNet: network bind %u: %r\n", Pass, Status));
    if (!EFI_ERROR (gBS->HandleProtocol (mHandle, &gEfiPxeBaseCodeProtocolGuid, &Pxe))) break;
  }
  gBS->CloseEvent (Event);
}

EFI_STATUS EFIAPI SerialNetEntry (EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_STATUS Status;
  EFI_EVENT Event;
  UINT64 Stamp;
  (VOID)ImageHandle; (VOID)SystemTable;
  Status = UartInit ();
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "SerialNet: UART configuration readback failed: %r\n", Status));
    return Status;
  }
  Stamp = GetPerformanceCounter ();
  mSession = (UINT32)(Stamp ^ (Stamp >> 32)) | 1U;
  EfiAcquireLock (&mLock);
  Status = Rpc (SN_HELLO, NULL, 0);
  EfiReleaseLock (&mLock);
  if (EFI_ERROR (Status) || mResponse.length != 9 ||
      mResponse.data[7] != (UINT8)SN_MTU || mResponse.data[8] != (UINT8)(SN_MTU >> 8)) {
    DEBUG ((DEBUG_ERROR, "SerialNet: MCU handshake failed: %r\n", Status));
    return EFI_DEVICE_ERROR;
  }
  mMode.State = EfiSimpleNetworkStopped;
  mMode.HwAddressSize = 6; mMode.MediaHeaderSize = 14;
  mMode.MaxPacketSize = SN_MTU; mMode.IfType = 1;
  mMode.ReceiveFilterMask = FILTERS; mMode.ReceiveFilterSetting = FILTERS;
  mMode.MediaPresentSupported = TRUE; mMode.MediaPresent = mResponse.data[6] != 0;
  mMode.MultipleTxSupported = FALSE; mMode.MacAddressChangeable = FALSE;
  CopyMem (&mMode.CurrentAddress, mResponse.data, 6);
  CopyMem (&mMode.PermanentAddress, mResponse.data, 6);
  SetMem (&mMode.BroadcastAddress, 6, 0xFF);
  mSnp.Revision = EFI_SIMPLE_NETWORK_PROTOCOL_REVISION;
  mSnp.Start = Start; mSnp.Stop = Stop; mSnp.Initialize = Initialize;
  mSnp.Reset = Reset; mSnp.Shutdown = Shutdown; mSnp.ReceiveFilters = ReceiveFilters;
  mSnp.StationAddress = Station; mSnp.Statistics = Statistics;
  mSnp.MCastIpToMac = Mcast; mSnp.NvData = NvData;
  mSnp.GetStatus = GetStatus; mSnp.Transmit = Transmit; mSnp.Receive = Receive; mSnp.Mode = &mMode;
  Status = gBS->CreateEvent (EVT_NOTIFY_WAIT, TPL_CALLBACK, WaitPacket, NULL, &mSnp.WaitForPacket);
  if (EFI_ERROR (Status)) return Status;
  mPath.Mac.Header.Type = MESSAGING_DEVICE_PATH; mPath.Mac.Header.SubType = MSG_MAC_ADDR_DP;
  SetDevicePathNodeLength (&mPath.Mac.Header, sizeof (mPath.Mac));
  CopyMem (&mPath.Mac.MacAddress, &mMode.CurrentAddress, 6); mPath.Mac.IfType = 1;
  SetDevicePathEndNode (&mPath.End);
  Status = gBS->InstallMultipleProtocolInterfaces (&mHandle,
      &gEfiSimpleNetworkProtocolGuid, &mSnp, &gEfiDevicePathProtocolGuid, &mPath, NULL);
  if (EFI_ERROR (Status)) { gBS->CloseEvent (mSnp.WaitForPacket); return Status; }
  Status = gBS->CreateEventEx (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, ConnectNetwork,
                             NULL, &gEfiEndOfDxeEventGroupGuid, &Event);
  if (EFI_ERROR (Status)) {
    gBS->UninstallMultipleProtocolInterfaces (mHandle, &gEfiSimpleNetworkProtocolGuid,
        &mSnp, &gEfiDevicePathProtocolGuid, &mPath, NULL);
    gBS->CloseEvent (mSnp.WaitForPacket); return Status;
  }
  DEBUG ((DEBUG_ERROR, "SerialNet: UART1_M0 115200, SNP MAC=%02x:%02x:%02x:%02x:%02x:%02x\n",
    mResponse.data[0],mResponse.data[1],mResponse.data[2],mResponse.data[3],mResponse.data[4],mResponse.data[5]));
  return EFI_SUCCESS;
}

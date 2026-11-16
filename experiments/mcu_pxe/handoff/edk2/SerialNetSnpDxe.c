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
#include "ip_guard.h"
#include "NetHandoffProtocol.h"
#include <Library/CruLib.h>

/* B1原理图第6/14页：GPIO2_B6/RX、GPIO2_B7/TX经U4008接MCU UART4。 */
#define UART_BASE 0xFEB40000U
#define UART_TX_EMPTY BIT6
#define UART_BUSY BIT0
#define UART_IOMUX 0xFD5F804CU
#define UART_INPUT 0xFD5FA194U
#define UART_CLKSEL 0xFD7C03ACU
#define UART_SOURCE 0xFD7C03A4U
#define UART_FRACTION 0xFD7C03A8U
#define UART_SOURCE_MASK (BIT14 | (31U << 9))
#define UART_FRACTION_GATES (BIT11 | BIT12)
#define UART_MUX_MASK 3U
#define UART_MUX_FRACTION 1U
#define UART_OVERSAMPLE 16U
#define UART_DRAIN_TIMEOUT_US 100000U
#define UART_CLKGATE 0xFD7C0830U
#define UART_RESET 0xFD7C0A30U
#define UART_CLOCK_MASK (BIT2 | BIT13)
#define RPC_ATTEMPTS 3U
#define RPC_TIMEOUT_US 4000000U
#define GRANT_TIMEOUT_US 60000000U
#define GRANT_POLL_US 100000U
#define RECEIVE_POLL_US 10000U
#define RECEIVE_FAST_POLL_US 1000U
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
STATIC UINT32 mBaud = SN_BOOT_BAUD;
STATIC UINT64 mLastPoll;
STATIC BOOLEAN mFailed;
STATIC BOOLEAN mReleasePending, mDetached;
STATIC EFI_GUID mHandoffGuid = NET_HANDOFF_PROTOCOL_GUID;
STATIC NET_HANDOFF_INFO mInfo;
STATIC NET_HANDOFF_PROTOCOL mControl;

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
  DEBUG ((DEBUG_ERROR, "SerialNet: UART1_M0 diag=H1 mux=%08x clk=%08x gate=%08x rst=%08x ie=%08x lcr=%02x div=%u usr=%02x\n",
          Mux, Clock, Gate, Reset, Input, Lcr, Divisor, MmioRead32 (UART_BASE + UART_USR_OFFSET)));
  if ((Mux & 0xFF00U) != 0xAA00U || (Clock & 3U) != 2U ||
      (Gate & UART_CLOCK_MASK) != 0 || (Reset & UART_CLOCK_MASK) != 0 || (Input & BIT6) == 0 ||
      Lcr != 3U || Divisor != 13U) return EFI_DEVICE_ERROR;
  mBaud = SN_BOOT_BAUD;
  return EFI_SUCCESS;
}

/* 只读实际GPLL；分数输出为baud*16，不改共享PLL，也不接受截断后的近似速率。 */
STATIC UINT32 DataClockFraction (UINT32 Baud)
{
  UINT32 Parent = HAL_CRU_ClkGetFreq (PLL_GPLL);
  UINT32 Target = Baud * UART_OVERSAMPLE;
  UINT32 A = Parent, B = Target, R;
  if (Target == 0 || Parent < Target) return 0;
  while (B != 0) { R = A % B; A = B; B = R; }
  if (Target / A > MAX_UINT16 || Parent / A > MAX_UINT16) return 0;
  return ((Target / A) << 16) | (Parent / A);
}

STATIC EFI_STATUS UartDataBaud (UINT32 Baud, UINT32 Fraction)
{
  UINT64 Begin = NowUs ();
  UINT32 Divisor, Lcr;
  while ((MmioRead32 (UART_BASE + UART_LSR_OFFSET) & UART_TX_EMPTY) == 0 ||
         (MmioRead32 (UART_BASE + UART_USR_OFFSET) & UART_BUSY) != 0) {
    if (NowUs () - Begin >= UART_DRAIN_TIMEOUT_US) return EFI_TIMEOUT;
    MicroSecondDelay (10);
  }
  /* RK3588 UART1独占CON41的9..14位、CON42、CON43的0..1位；保留UART2。 */
  MmioWrite32 (UART_SOURCE, UART_SOURCE_MASK << 16);
  MmioWrite32 (UART_FRACTION, Fraction);
  MmioWrite32 (UART_CLKGATE, UART_FRACTION_GATES << 16);
  MmioWrite32 (UART_CLKSEL, (UART_MUX_MASK << 16) | UART_MUX_FRACTION);
  MmioWrite32 (UART_BASE + 0x0CU, 0x83U);
  MmioWrite32 (UART_BASE, 1U);
  MmioWrite32 (UART_BASE + 0x04U, 0U);
  Lcr = MmioRead32 (UART_BASE + 0x0CU);
  Divisor = MAX_UINT32;
  if ((Lcr & BIT7) != 0)
    Divisor = (MmioRead32 (UART_BASE) & 0xFFU) |
              ((MmioRead32 (UART_BASE + 0x04U) & 0xFFU) << 8);
  MmioWrite32 (UART_BASE + 0x0CU, 3U);
  if ((MmioRead32 (UART_SOURCE) & UART_SOURCE_MASK) != 0 ||
      MmioRead32 (UART_FRACTION) != Fraction ||
      (MmioRead32 (UART_CLKGATE) & (UART_CLOCK_MASK | UART_FRACTION_GATES)) != 0 ||
      (MmioRead32 (UART_CLKSEL) & UART_MUX_MASK) != UART_MUX_FRACTION ||
      MmioRead32 (UART_BASE + 0x0CU) != 3U || Divisor != 1U) return EFI_DEVICE_ERROR;
  mBaud = Baud;
  return EFI_SUCCESS;
}

/* 调用者持有TPL_NOTIFY锁，收包期间不让网络定时回调重入同一UART事务。 */
STATIC EFI_STATUS Rpc (UINT8 Op, CONST VOID *Data, UINTN Size)
{
  UINTN Attempt, Position;
  UINT16 WireSize;
  UINT64 Begin;
  /* 数据事务未知时仍允许显式归还；MCU 的 RELEASE 负责排空已入队帧。 */
  if (mFailed && Op != SN_RELEASE) return EFI_DEVICE_ERROR;
  if (Size > SN_ETH_MAX || (Size && !Data)) return EFI_INVALID_PARAMETER;
  /* 归还应答丢失后保留原事务，下一次人工重试仍取 MCU 的同序号缓存。 */
  if (!(Op == SN_RELEASE && mReleasePending)) {
    mRequest.op = Op; mRequest.status = 0; mRequest.session = mSession;
    mRequest.sequence = ++mSequence; mRequest.length = (UINT16)Size;
    if (Size) CopyMem (mRequest.data, Data, Size);
    if (Op == SN_RELEASE) mReleasePending = TRUE;
  }
  WireSize = sn_pack (&mRequest, mWire, sizeof (mWire));
  for (Attempt = 0; Attempt < RPC_ATTEMPTS; ++Attempt) {
    UINTN Received = 0, Bad = 0, Unmatched = 0;
    /* 超时重试同一请求，MCU重复应答，不重复网络发送或领取下一接收包。 */
    sn_parser_reset (&mParser);
    Begin = NowUs ();
    for (Position = 0; Position < WireSize && NowUs () - Begin < RPC_TIMEOUT_US;) {
      if ((MmioRead32 (UART_BASE + UART_USR_OFFSET) & BIT1) != 0)
        MmioWrite32 (UART_BASE, mWire[Position++]);
      else MicroSecondDelay (10);
    }
    while (Position == WireSize && NowUs () - Begin < RPC_TIMEOUT_US) {
      if ((MmioRead32 (UART_BASE + UART_USR_OFFSET) & BIT3) != 0) {
        int Result = sn_feed (&mParser, (UINT8)MmioRead32 (UART_BASE), &mResponse);
        ++Received;
        if (Result < 0) ++Bad;
        if (Result == 1 && mResponse.op == (Op | SN_REPLY) &&
            mResponse.session == mSession && mResponse.sequence == mSequence) {
          if (Op == SN_RELEASE) mReleasePending = FALSE;
          if (Op == SN_HELLO && mResponse.status != SN_NOT_READY) {
            DEBUG ((DEBUG_ERROR, "SerialNet: HELLO reply status=%u tx=%u rx=%u\n",
                    mResponse.status, (UINT32)Position, (UINT32)Received));
          }
          if (mResponse.status == SN_OK) return EFI_SUCCESS;
          if (mResponse.status == SN_EMPTY) return EFI_NOT_READY;
          if (mResponse.status == SN_NOT_READY)
            return Op == SN_HELLO || Op == SN_RELEASE ? EFI_NOT_READY : EFI_NO_MEDIA;
          return EFI_DEVICE_ERROR;
        }
        if (Result == 1) ++Unmatched;
      } else MicroSecondDelay (10);
    }
    /* 只在一次事务超时后打印，避免逐字节日志改变收发时序。 */
    DEBUG ((DEBUG_ERROR, "SerialNet: attempt=%u op=%u seq=%u tx=%u/%u rx=%u bad=%u other=%u usr=%02x\n",
            (UINT32)Attempt + 1U, Op, mSequence, (UINT32)Position, WireSize,
            (UINT32)Received, (UINT32)Bad, (UINT32)Unmatched, MmioRead32 (UART_BASE + UART_USR_OFFSET)));
  }
  /* 不跳过未知事务继续发送新序号；复位RK/重载驱动后重新握手。 */
  mFailed = TRUE; mMode.MediaPresent = FALSE;
  DEBUG ((DEBUG_ERROR, "SerialNet: UART transaction timeout op=%u seq=%u\n", Op, mSequence));
  return EFI_TIMEOUT;
}

/* 四字节按网络序转整数，仅用于校验 MCU 授权的地址和连续掩码。 */
STATIC UINT32 AddressValue (CONST UINT8 *P)
{
  return (UINT32)P[0] << 24 | (UINT32)P[1] << 16 | (UINT32)P[2] << 8 | P[3];
}

STATIC BOOLEAN ValidGrant (VOID)
{
  UINT32 Ip, Mask, HostMask, Gateway;
  UINT8 *P = mResponse.data;
  if (mResponse.length != SN_HELLO_INFO_SIZE || (P[0] & 1U) != 0 ||
      P[SN_HELLO_LINK_OFFSET] > 1U || P[SN_HELLO_MTU_OFFSET] != (UINT8)SN_MTU ||
      P[SN_HELLO_MTU_OFFSET + 1U] != (UINT8)(SN_MTU >> 8)) return FALSE;
  if ((P[0] | P[1] | P[2] | P[3] | P[4] | P[5]) == 0) return FALSE;
  Ip = AddressValue (P + SN_HELLO_IP_OFFSET);
  Mask = AddressValue (P + SN_HELLO_MASK_OFFSET);
  Gateway = AddressValue (P + SN_HELLO_GATEWAY_OFFSET);
  HostMask = ~Mask;
  if (Mask == 0 || HostMask < 3U || (HostMask & (HostMask + 1U)) != 0 ||
      P[SN_HELLO_IP_OFFSET] == 0 || P[SN_HELLO_IP_OFFSET] == 127 || P[SN_HELLO_IP_OFFSET] >= 224 ||
      (Ip & HostMask) == 0 || (Ip & HostMask) == HostMask) return FALSE;
  if (Gateway != 0 && ((Gateway & Mask) != (Ip & Mask) ||
      (Gateway & HostMask) == 0 || (Gateway & HostMask) == HostMask)) return FALSE;
  return TRUE;
}

/* NOT_READY 是明确未授权，新序号重查；静默超时仍保留通信故障，不自行接管 IP。 */
STATIC EFI_STATUS AwaitGrant (VOID)
{
  EFI_STATUS Status;
  UINT64 Begin = NowUs ();
  do {
    EfiAcquireLock (&mLock);
    Status = Rpc (SN_HELLO, NULL, 0);
    EfiReleaseLock (&mLock);
    if (!EFI_ERROR (Status)) {
      if (!ValidGrant ()) return EFI_PROTOCOL_ERROR;
      mInfo.Revision = NET_HANDOFF_REVISION;
      mInfo.Session = mSession;
      mInfo.State = NET_HANDOFF_ACTIVE;
      CopyMem (mInfo.Ip, mResponse.data + SN_HELLO_IP_OFFSET, sizeof (mInfo.Ip));
      CopyMem (mInfo.Mask, mResponse.data + SN_HELLO_MASK_OFFSET, sizeof (mInfo.Mask));
      CopyMem (mInfo.Gateway, mResponse.data + SN_HELLO_GATEWAY_OFFSET, sizeof (mInfo.Gateway));
      return EFI_SUCCESS;
    }
    if (Status != EFI_NOT_READY) return Status;
    MicroSecondDelay (GRANT_POLL_US);
  } while (NowUs () - Begin < GRANT_TIMEOUT_US);
  return EFI_TIMEOUT;
}

/* 仅明确拒绝才从4M尝试2M；应答未知不能换新速率或跳过高速HELLO确认。 */
STATIC EFI_STATUS NegotiateBaud (VOID)
{
  STATIC CONST UINT32 Rates[] = {SN_DATA_BAUD, SN_LEGACY_DATA_BAUD};
  UINT8 Rate[SN_RATE_REQUEST_SIZE], Mac[6];
  UINT32 Baud = SN_BOOT_BAUD, Fraction = 0;
  EFI_STATUS Status = EFI_SUCCESS;
  CopyMem (Mac, mResponse.data + SN_HELLO_MAC_OFFSET, sizeof (Mac));
  EfiAcquireLock (&mLock);
  for (UINTN Candidate = 0; Candidate < ARRAY_SIZE (Rates); ++Candidate) {
    Fraction = DataClockFraction (Rates[Candidate]);
    if (Fraction == 0) continue;
    for (UINTN Index = 0; Index < sizeof (Rate); ++Index)
      Rate[Index] = (UINT8)(Rates[Candidate] >> (Index * 8U));
    Status = Rpc (SN_HELLO, Rate, sizeof (Rate));
    if (Status == EFI_DEVICE_ERROR && !mFailed && mResponse.status == SN_INVALID)
      continue;
    if (EFI_ERROR (Status)) goto Done;
    Baud = Rates[Candidate];
    break;
  }
  if (Baud == SN_BOOT_BAUD) {
    DEBUG ((DEBUG_ERROR, "SerialNet: no agreed data rate, keeping baud=%u\n", mBaud));
    Status = EFI_SUCCESS;
    goto Done;
  }
  if (EFI_ERROR (Status)) goto Done;
  if (mResponse.length != sizeof (Rate) || CompareMem (Rate, mResponse.data, sizeof (Rate)) != 0) {
    Status = EFI_PROTOCOL_ERROR;
    goto Done;
  }
  MicroSecondDelay (SN_RATE_SWITCH_GUARD_US);
  Status = UartDataBaud (Baud, Fraction);
  if (EFI_ERROR (Status)) goto Done;
  sn_parser_reset (&mParser);
  Status = Rpc (SN_HELLO, NULL, 0);
  if (!EFI_ERROR (Status) &&
      (!ValidGrant () || CompareMem (mResponse.data, Mac, sizeof (Mac)) != 0 ||
       CompareMem (mResponse.data + SN_HELLO_IP_OFFSET, mInfo.Ip, sizeof (mInfo.Ip)) != 0 ||
       CompareMem (mResponse.data + SN_HELLO_MASK_OFFSET, mInfo.Mask, sizeof (mInfo.Mask)) != 0 ||
       CompareMem (mResponse.data + SN_HELLO_GATEWAY_OFFSET, mInfo.Gateway, sizeof (mInfo.Gateway)) != 0))
    Status = EFI_PROTOCOL_ERROR;
  if (!EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "SerialNet: UART data baud=%u confirmed\n", mBaud));
  }
Done:
  EfiReleaseLock (&mLock);
  return Status;
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
  if (mInfo.State != NET_HANDOFF_ACTIVE) return EFI_NOT_READY;
  if (mPacketSize) return EFI_SUCCESS;
  if (NowUs () - mLastPoll < (mBaud != SN_BOOT_BAUD ? RECEIVE_FAST_POLL_US : RECEIVE_POLL_US))
    return EFI_NOT_READY;
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
    if (!sn_ip_guard_rx (P, mResponse.length, mMode.CurrentAddress.Addr, mInfo.Ip))
      return EFI_NOT_READY;
    mPacketSize = mResponse.length; CopyMem (mPacket, P, mPacketSize);
  }
  return Status;
}

STATIC EFI_STATUS EFIAPI Start (EFI_SIMPLE_NETWORK_PROTOCOL *This)
{
  if (This != &mSnp) return EFI_INVALID_PARAMETER;
  if (mInfo.State != NET_HANDOFF_ACTIVE) return EFI_ACCESS_DENIED;
  if (mMode.State != EfiSimpleNetworkStopped) return EFI_ALREADY_STARTED;
  mMode.State = EfiSimpleNetworkStarted;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI Initialize (EFI_SIMPLE_NETWORK_PROTOCOL *This, UINTN Rx, UINTN Tx)
{
  EFI_STATUS Status;
  if (This != &mSnp) return EFI_INVALID_PARAMETER;
  if (mInfo.State != NET_HANDOFF_ACTIVE) return EFI_ACCESS_DENIED;
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
  if (mInfo.State != NET_HANDOFF_ACTIVE) return EFI_ACCESS_DENIED;
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
  /* 控制器拆卸期间只完成 SNP 状态转换；显式 RELEASE 才归还地址。 */
  Status = mInfo.State == NET_HANDOFF_ACTIVE ? Rpc (SN_STOP, NULL, 0) : EFI_SUCCESS;
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
  if (mInfo.State != NET_HANDOFF_ACTIVE) return EFI_ACCESS_DENIED;
  if (EFI_ERROR (Status)) return Status;
  if (!P || Size < 14 || Size > SN_ETH_MAX ||
      (HeaderSize && (HeaderSize != 14 || !Dst || !Protocol))) return EFI_INVALID_PARAMETER;
  Status = EfiAcquireLockOrFail (&mLock); if (EFI_ERROR (Status)) return EFI_NOT_READY;
  if (mRecycle) { EfiReleaseLock (&mLock); return EFI_NOT_READY; }
  if (HeaderSize) {
    CopyMem (P, Dst, 6); CopyMem (P + 6, Src ? Src : &mMode.CurrentAddress, 6);
    P[12] = (UINT8)(*Protocol >> 8); P[13] = (UINT8)*Protocol;
  }
  if (!sn_ip_guard_tx (P, (UINT16)Size, mMode.CurrentAddress.Addr, mInfo.Ip)) {
    EfiReleaseLock (&mLock);
    return EFI_ACCESS_DENIED;
  }
  Status = Rpc (SN_SEND, Buffer, Size);
  if (!EFI_ERROR (Status)) mRecycle = Buffer;
  EfiReleaseLock (&mLock); return Status;
}

STATIC EFI_STATUS EFIAPI Receive (EFI_SIMPLE_NETWORK_PROTOCOL *This, UINTN *HeaderSize,
    UINTN *Size, VOID *Buffer, EFI_MAC_ADDRESS *Src, EFI_MAC_ADDRESS *Dst, UINT16 *Protocol)
{
  EFI_STATUS Status = Ready (This);
  if (mInfo.State != NET_HANDOFF_ACTIVE) return EFI_ACCESS_DENIED;
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
  for (Pass = 0; mInfo.State == NET_HANDOFF_ACTIVE && Pass < 10; ++Pass) {
    Status = gBS->ConnectController (mHandle, NULL, NULL, TRUE);
    DEBUG ((DEBUG_ERROR, "SerialNet: network bind %u: %r\n", Pass, Status));
    if (!EFI_ERROR (gBS->HandleProtocol (mHandle, &gEfiPxeBaseCodeProtocolGuid, &Pxe))) break;
  }
  gBS->CloseEvent (Event);
}

STATIC EFI_STATUS EFIAPI GetHandoffInfo (NET_HANDOFF_PROTOCOL *This, NET_HANDOFF_INFO *Info)
{
  if (This != &mControl || Info == NULL) return EFI_INVALID_PARAMETER;
  CopyMem (Info, &mInfo, sizeof (*Info));
  return EFI_SUCCESS;
}

/* 调用方应在测试应用退出后显式执行；绝不由 SNP Stop、超时或 DHCP RELEASE 触发。 */
STATIC EFI_STATUS EFIAPI ReturnNetwork (NET_HANDOFF_PROTOCOL *This)
{
  EFI_STATUS Status;
  if (This != &mControl) return EFI_INVALID_PARAMETER;
  if (mInfo.State == NET_HANDOFF_RELEASED) return EFI_SUCCESS;
  if (mInfo.State != NET_HANDOFF_ACTIVE && mInfo.State != NET_HANDOFF_RETURNING)
    return EFI_NOT_READY;
  Status = EfiAcquireLockOrFail (&mLock);
  if (EFI_ERROR (Status)) return EFI_NOT_READY;
  mInfo.State = NET_HANDOFF_RETURNING;
  mMode.MediaPresent = FALSE;
  EfiReleaseLock (&mLock);
  if (!mDetached) {
    /* 拆卸会调用 SNP Shutdown/Stop，不能持有其共用锁。 */
    Status = gBS->DisconnectController (mHandle, NULL, NULL);
    if (EFI_ERROR (Status)) return Status;
    if (mMode.State != EfiSimpleNetworkStopped) return EFI_DEVICE_ERROR;
    mDetached = TRUE;
  }
  Status = EfiAcquireLockOrFail (&mLock);
  if (EFI_ERROR (Status)) return EFI_NOT_READY;
  mPacketSize = 0;
  mRecycle = NULL;
  Status = Rpc (SN_RELEASE, NULL, 0);
  if (!EFI_ERROR (Status)) mInfo.State = NET_HANDOFF_RELEASED;
  EfiReleaseLock (&mLock);
  return Status;
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
  Status = AwaitGrant ();
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "SerialNet: MCU handshake failed: %r\n", Status));
    return Status;
  }
  mMode.State = EfiSimpleNetworkStopped;
  mMode.HwAddressSize = 6; mMode.MediaHeaderSize = 14;
  mMode.MaxPacketSize = SN_MTU; mMode.IfType = 1;
  mMode.ReceiveFilterMask = FILTERS; mMode.ReceiveFilterSetting = FILTERS;
  mMode.MediaPresentSupported = TRUE; mMode.MediaPresent = mResponse.data[SN_HELLO_LINK_OFFSET] != 0;
  mMode.MultipleTxSupported = FALSE; mMode.MacAddressChangeable = FALSE;
  CopyMem (&mMode.CurrentAddress, mResponse.data, 6);
  CopyMem (&mMode.PermanentAddress, mResponse.data, 6);
  Status = NegotiateBaud ();
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "SerialNet: baud negotiation failed: %r; power cycle RK to retry\n", Status));
    return Status;
  }
  SetMem (&mMode.BroadcastAddress, 6, 0xFF);
  mSnp.Revision = EFI_SIMPLE_NETWORK_PROTOCOL_REVISION;
  mSnp.Start = Start; mSnp.Stop = Stop; mSnp.Initialize = Initialize;
  mSnp.Reset = Reset; mSnp.Shutdown = Shutdown; mSnp.ReceiveFilters = ReceiveFilters;
  mSnp.StationAddress = Station; mSnp.Statistics = Statistics;
  mSnp.MCastIpToMac = Mcast; mSnp.NvData = NvData;
  mSnp.GetStatus = GetStatus; mSnp.Transmit = Transmit; mSnp.Receive = Receive; mSnp.Mode = &mMode;
  mControl.GetInfo = GetHandoffInfo;
  mControl.ReturnNetwork = ReturnNetwork;
  Status = gBS->CreateEvent (EVT_NOTIFY_WAIT, TPL_CALLBACK, WaitPacket, NULL, &mSnp.WaitForPacket);
  if (EFI_ERROR (Status)) return Status;
  mPath.Mac.Header.Type = MESSAGING_DEVICE_PATH; mPath.Mac.Header.SubType = MSG_MAC_ADDR_DP;
  SetDevicePathNodeLength (&mPath.Mac.Header, sizeof (mPath.Mac));
  CopyMem (&mPath.Mac.MacAddress, &mMode.CurrentAddress, 6); mPath.Mac.IfType = 1;
  SetDevicePathEndNode (&mPath.End);
  Status = gBS->InstallMultipleProtocolInterfaces (&mHandle,
      &gEfiSimpleNetworkProtocolGuid, &mSnp, &gEfiDevicePathProtocolGuid, &mPath,
      &mHandoffGuid, &mControl, NULL);
  if (EFI_ERROR (Status)) { gBS->CloseEvent (mSnp.WaitForPacket); return Status; }
  Status = gBS->CreateEventEx (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, ConnectNetwork,
                             NULL, &gEfiEndOfDxeEventGroupGuid, &Event);
  if (EFI_ERROR (Status)) {
    gBS->UninstallMultipleProtocolInterfaces (mHandle, &gEfiSimpleNetworkProtocolGuid,
        &mSnp, &gEfiDevicePathProtocolGuid, &mPath, &mHandoffGuid, &mControl, NULL);
    gBS->CloseEvent (mSnp.WaitForPacket); return Status;
  }
  DEBUG ((DEBUG_ERROR, "SerialNet: UART1_M0 baud=%u, SNP MAC=%02x:%02x:%02x:%02x:%02x:%02x\n",
    mBaud, mMode.CurrentAddress.Addr[0],mMode.CurrentAddress.Addr[1],mMode.CurrentAddress.Addr[2],
    mMode.CurrentAddress.Addr[3],mMode.CurrentAddress.Addr[4],mMode.CurrentAddress.Addr[5]));
  DEBUG ((DEBUG_ERROR, "SerialNet: granted session=%u IP=%u.%u.%u.%u\n",
          mInfo.Session, mInfo.Ip[0], mInfo.Ip[1], mInfo.Ip[2], mInfo.Ip[3]));
  return EFI_SUCCESS;
}

/* 实际自动入口配真实UEFI头；模拟Boot Services，验证授权、目标筛选和退出后归还。 */
#include "SltAutoBoot.c"

#define STR_(x) #x
#define STR(x) STR_(x)
#define CHECK(x) do { if (!(x)) fail("FAIL " STR(__LINE__) " " #x "\n"); } while (0)

static void message(const char *text)
{
  register UINT32 r0 __asm__("r0") = 4U;
  register const char *r1 __asm__("r1") = text;
  __asm__ volatile("bkpt 0xAB" : "+r"(r0), "+r"(r1) : : "memory");
}
static _Noreturn void finish(UINT32 code)
{
  UINT32 block[] = {0x20026U, code};
  register UINT32 r0 __asm__("r0") = 0x20U;
  register UINT32 *r1 __asm__("r1") = block;
  __asm__ volatile("bkpt 0xAB" : "+r"(r0), "+r"(r1) : : "memory");
  for (;;) {}
}
static _Noreturn void fail(const char *text) { message(text); finish(1U); }
void test_abort(void) { fail("FAIL CPU exception\n"); }

VOID *EFIAPI ZeroMem(VOID *p, UINTN n)
{
  for (UINTN i = 0; i < n; ++i) ((UINT8 *)p)[i] = 0;
  return p;
}
VOID *EFIAPI CopyMem(VOID *d, CONST VOID *s, UINTN n)
{
  for (UINTN i = 0; i < n; ++i) ((UINT8 *)d)[i] = ((CONST UINT8 *)s)[i];
  return d;
}
INTN EFIAPI CompareMem(CONST VOID *a, CONST VOID *b, UINTN n)
{
  for (UINTN i = 0; i < n; ++i)
    if (((CONST UINT8 *)a)[i] != ((CONST UINT8 *)b)[i]) return 1;
  return 0;
}
VOID EFIAPI FreePool(VOID *p) { (VOID)p; }
UINTN EFIAPI Print(CONST CHAR16 *format, ...) { (VOID)format; return 0; }

static UINT64 clock_us;
UINT64 EFIAPI GetPerformanceCounter(VOID) { return clock_us; }
UINT64 EFIAPI GetTimeInNanoSecond(UINT64 ticks) { return ticks * 1000U; }
UINTN EFIAPI MicroSecondDelay(UINTN us) { clock_us += us; return us; }

#pragma pack(1)
typedef struct { MAC_ADDR_DEVICE_PATH mac; EFI_DEVICE_PATH_PROTOCOL end; } nic_path_t;
typedef struct { MAC_ADDR_DEVICE_PATH mac; IPv4_DEVICE_PATH ip; EFI_DEVICE_PATH_PROTOCOL end; } pxe_path_t;
#pragma pack()
static nic_path_t nic_path;
static pxe_path_t paths[6];
static EFI_BOOT_MANAGER_LOAD_OPTION options[6];
static EFI_HANDLE handles[2] = {(EFI_HANDLE)1, (EFI_HANDLE)2};
static NET_HANDOFF_INFO info;
static EFI_SIMPLE_NETWORK_MODE mode;
static EFI_SIMPLE_NETWORK_PROTOCOL snp = {.Mode = &mode};
static UINTN handle_count, option_count, boots, releases, return_busy, refreshed, connected;
static UINTN selected, free_options;
static EFI_STATUS locate_status, info_status, connect_status, boot_status, release_status;
static BOOLEAN escape, boot_returned;

UINT8 EFIAPI DevicePathType(CONST VOID *p) { return ((CONST EFI_DEVICE_PATH_PROTOCOL *)p)->Type; }
UINT8 EFIAPI DevicePathSubType(CONST VOID *p) { return ((CONST EFI_DEVICE_PATH_PROTOCOL *)p)->SubType; }
UINTN EFIAPI DevicePathNodeLength(CONST VOID *p)
{
  CONST EFI_DEVICE_PATH_PROTOCOL *node = p;
  return (UINTN)node->Length[0] | ((UINTN)node->Length[1] << 8);
}
EFI_DEVICE_PATH_PROTOCOL *EFIAPI NextDevicePathNode(CONST VOID *p)
{
  return (EFI_DEVICE_PATH_PROTOCOL *)((UINT8 *)p + DevicePathNodeLength(p));
}
BOOLEAN EFIAPI IsDevicePathEnd(CONST VOID *p)
{
  return DevicePathType(p) == END_DEVICE_PATH_TYPE && DevicePathSubType(p) == END_ENTIRE_DEVICE_PATH_SUBTYPE;
}
UINT16 EFIAPI SetDevicePathNodeLength(VOID *p, UINTN size)
{
  EFI_DEVICE_PATH_PROTOCOL *node = p;
  node->Length[0] = (UINT8)size; node->Length[1] = (UINT8)(size >> 8); return (UINT16)size;
}
VOID EFIAPI SetDevicePathEndNode(VOID *p)
{
  EFI_DEVICE_PATH_PROTOCOL *node = p;
  node->Type = END_DEVICE_PATH_TYPE; node->SubType = END_ENTIRE_DEVICE_PATH_SUBTYPE;
  SetDevicePathNodeLength(p, END_DEVICE_PATH_LENGTH);
}

UINTN EFIAPI GetDevicePathSize(CONST EFI_DEVICE_PATH_PROTOCOL *path)
{
  UINTN total = 0;
  if (path == NULL) return 0;
  for (UINTN i = 0; i < 10; ++i) {
    UINTN n = DevicePathNodeLength(path);
    CHECK(n >= END_DEVICE_PATH_LENGTH);
    total += n;
    if (IsDevicePathEnd(path)) return total;
    path = NextDevicePathNode(path);
  }
  fail("FAIL unterminated test path\n");
}
EFI_DEVICE_PATH_PROTOCOL *EFIAPI DevicePathFromHandle(EFI_HANDLE handle)
{
  CHECK(handle == handles[0]); return (EFI_DEVICE_PATH_PROTOCOL *)&nic_path;
}
static EFI_STATUS EFIAPI get_info(NET_HANDOFF_PROTOCOL *self, NET_HANDOFF_INFO *out)
{
  (VOID)self; *out = info; return info_status;
}
static EFI_STATUS EFIAPI return_network(NET_HANDOFF_PROTOCOL *self)
{
  (VOID)self;
  CHECK(boots == 0 || boot_returned);
  ++releases;
  if (return_busy) { --return_busy; return EFI_NOT_READY; }
  return release_status;
}
static NET_HANDOFF_PROTOCOL control = {.GetInfo = get_info, .ReturnNetwork = return_network};
EFI_GUID gEfiSimpleNetworkProtocolGuid = EFI_SIMPLE_NETWORK_PROTOCOL_GUID;
static EFI_STATUS EFIAPI locate(EFI_LOCATE_SEARCH_TYPE kind, EFI_GUID *guid, VOID *key,
                                UINTN *count, EFI_HANDLE **out)
{
  CHECK(kind == ByProtocol && !CompareMem(guid, &mSltHandoffGuid, sizeof(*guid)));
  (VOID)key;
  if (EFI_ERROR(locate_status)) return locate_status;
  *count = handle_count; *out = handles; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI handle_protocol(EFI_HANDLE handle, EFI_GUID *guid, VOID **out)
{
  CHECK(handle == handles[0]);
  if (!CompareMem(guid, &mSltHandoffGuid, sizeof(*guid))) *out = &control;
  else { CHECK(!CompareMem(guid, &gEfiSimpleNetworkProtocolGuid, sizeof(*guid))); *out = &snp; }
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI connect_controller(EFI_HANDLE handle, EFI_HANDLE *drivers,
                                             EFI_DEVICE_PATH_PROTOCOL *remaining, BOOLEAN recursive)
{
  CHECK(handle == handles[0] && drivers == NULL && remaining == NULL && recursive);
  ++connected; return connect_status;
}
static EFI_STATUS EFIAPI read_key(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *self, EFI_INPUT_KEY *key)
{
  (VOID)self;
  if (!escape) return EFI_NOT_READY;
  key->ScanCode = SCAN_ESC; key->UnicodeChar = 0; return EFI_SUCCESS;
}
static EFI_SIMPLE_TEXT_INPUT_PROTOCOL input = {.ReadKeyStroke = read_key};
static EFI_SYSTEM_TABLE system_table = {.ConIn = &input};
EFI_SYSTEM_TABLE *gST = &system_table;
static EFI_BOOT_SERVICES boot_services = {
  .LocateHandleBuffer = locate, .HandleProtocol = handle_protocol, .ConnectController = connect_controller
};
EFI_BOOT_SERVICES *gBS = &boot_services;
VOID EFIAPI EfiBootManagerRefreshAllBootOption(VOID) { ++refreshed; }
EFI_BOOT_MANAGER_LOAD_OPTION *EFIAPI EfiBootManagerGetLoadOptions(UINTN *count, EFI_BOOT_MANAGER_LOAD_OPTION_TYPE type)
{
  CHECK(type == LoadOptionTypeBoot); *count = option_count; return options;
}
EFI_STATUS EFIAPI EfiBootManagerFreeLoadOptions(EFI_BOOT_MANAGER_LOAD_OPTION *p, UINTN count)
{
  CHECK(p == options && count == option_count); ++free_options; return EFI_SUCCESS;
}
VOID EFIAPI EfiBootManagerBoot(EFI_BOOT_MANAGER_LOAD_OPTION *option)
{
  CHECK(releases == 0); ++boots; selected = (UINTN)(option - options);
  option->Status = boot_status; boot_returned = TRUE;
}

static void reset(void)
{
  mSltAttempted = FALSE; clock_us = 0;
  handle_count = 1; option_count = 6;
  boots = releases = return_busy = refreshed = connected = free_options = 0;
  selected = MAX_UINTN; escape = boot_returned = FALSE;
  locate_status = info_status = connect_status = boot_status = release_status = EFI_SUCCESS;
  info = (NET_HANDOFF_INFO){.Revision = NET_HANDOFF_REVISION, .Session = 123, .State = NET_HANDOFF_ACTIVE,
                            .Ip = {192,168,1,201}};
  ZeroMem(&nic_path, sizeof(nic_path));
  nic_path.mac.Header.Type = MESSAGING_DEVICE_PATH;
  nic_path.mac.Header.SubType = MSG_MAC_ADDR_DP;
  SetDevicePathNodeLength(&nic_path.mac.Header, sizeof(nic_path.mac));
  nic_path.mac.MacAddress.Addr[0] = 6; nic_path.mac.MacAddress.Addr[1] = 8;
  nic_path.mac.IfType = 1;
  SetDevicePathEndNode(&nic_path.end);
  for (UINTN i = 0; i < 6; ++i) {
    ZeroMem(&paths[i], sizeof(paths[i]));
    paths[i].mac = nic_path.mac;
    paths[i].ip.Header.Type = MESSAGING_DEVICE_PATH;
    paths[i].ip.Header.SubType = MSG_IPv4_DP;
    SetDevicePathNodeLength(&paths[i].ip.Header, sizeof(paths[i].ip));
    SetDevicePathEndNode(&paths[i].end);
    options[i] = (EFI_BOOT_MANAGER_LOAD_OPTION){.OptionType = LoadOptionTypeBoot,
      .Attributes = LOAD_OPTION_ACTIVE, .Description = L"test PXEv4", .FilePath = (EFI_DEVICE_PATH_PROTOCOL *)&paths[i]};
  }
  paths[0].mac.MacAddress.Addr[1] ^= 1U;
  paths[1].ip.Header.SubType = MSG_IPv6_DP;
  options[2].Attributes = 0;
  options[3].Attributes |= LOAD_OPTION_CATEGORY_APP;
}

void test_main(void)
{
  reset(); locate_status = EFI_NOT_FOUND;
  CHECK(!SltAutoBoot() && boots == 0 && releases == 0);
  reset(); info.Revision += 1;
  CHECK(!SltAutoBoot() && boots == 0 && releases == 0);
  reset(); info.State = NET_HANDOFF_RELEASED;
  CHECK(!SltAutoBoot() && boots == 0 && releases == 0);
  reset(); handle_count = 2;
  CHECK(SltAutoBoot() && boots == 0 && releases == 0);
  reset(); escape = TRUE;
  CHECK(SltAutoBoot() && boots == 0 && releases == 0 && connected == 0);

  reset(); return_busy = 2;
  CHECK(SltAutoBoot() && selected == 4 && boots == 1 && releases == 3 && free_options == 1);
  CHECK(!SltAutoBoot() && boots == 1);
  reset(); boot_status = EFI_LOAD_ERROR;
  CHECK(SltAutoBoot() && boots == 1 && releases == 1);
  reset(); option_count = 4;
  CHECK(SltAutoBoot() && boots == 0 && releases == 1);
  reset(); connect_status = EFI_ALREADY_STARTED;
  CHECK(SltAutoBoot() && boots == 1 && releases == 1);
  reset(); connect_status = EFI_NOT_FOUND;
  CHECK(SltAutoBoot() && boots == 1 && releases == 1);
  reset(); connect_status = EFI_DEVICE_ERROR;
  CHECK(SltAutoBoot() && boots == 0 && releases == 1);
  reset(); release_status = EFI_DEVICE_ERROR;
  CHECK(SltAutoBoot() && boots == 1 && releases == 1);
  reset(); release_status = EFI_NOT_READY;
  CHECK(SltAutoBoot() && boots == 1 && releases == SLT_RETURN_WINDOW_US / SLT_RETURN_POLL_US);
  CHECK(clock_us == SLT_MANUAL_WINDOW_US + SLT_RETURN_WINDOW_US);

  reset(); paths[4].ip.StaticIpAddress = TRUE;
  CHECK(SltAutoBoot() && selected == 5 && boots == 1);
  reset(); option_count = 0;
  CHECK(SltAutoBoot() && boots == 0 && releases == 1);
  CHECK(!IsSltIpv4Path(NULL, (EFI_DEVICE_PATH_PROTOCOL *)&nic_path));
  message("PASS: authorized SLT auto boot, single PXEv4, return after exit, manual and failure paths\n");
  finish(0U);
}

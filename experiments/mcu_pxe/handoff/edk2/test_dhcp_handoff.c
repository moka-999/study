/* 实际DHCP授权辅助层配真实UEFI接口；验证目标网卡、运行IP和Build所有权合同。 */
#include "HandoffDhcp4.c"

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
VOID *EFIAPI CopyMem(VOID *d, CONST VOID *s, UINTN n)
{
  for (UINTN i = 0; i < n; ++i) ((UINT8 *)d)[i] = ((CONST UINT8 *)s)[i];
  return d;
}
static NET_HANDOFF_INFO info;
static EFI_STATUS handle_status, info_status, build_status;
static UINTN builds;
static UINT8 option_wire[6];
static EFI_DHCP4_PACKET seed, replacement;
static EFI_DHCP4_PACKET *result;
static EFI_HANDLE nic = (EFI_HANDLE)(UINTN)1;
static EFI_STATUS EFIAPI get_info(NET_HANDOFF_PROTOCOL *this, NET_HANDOFF_INFO *out)
{
  (VOID)this;
  CopyMem(out, &info, sizeof(info));
  return info_status;
}
static NET_HANDOFF_PROTOCOL control = {.GetInfo = get_info};
static EFI_STATUS EFIAPI handle_protocol(EFI_HANDLE handle, EFI_GUID *guid, VOID **out)
{
  EFI_GUID expected = NET_HANDOFF_PROTOCOL_GUID;
  CHECK(handle == nic);
  for (UINTN i = 0; i < sizeof(expected); ++i) CHECK(((UINT8 *)guid)[i] == ((UINT8 *)&expected)[i]);
  *out = &control;
  return handle_status;
}
static EFI_BOOT_SERVICES services = {.HandleProtocol = handle_protocol};
EFI_BOOT_SERVICES *gBS = &services;
static EFI_STATUS EFIAPI build(EFI_DHCP4_PROTOCOL *this, EFI_DHCP4_PACKET *packet,
  UINT32 delete_count, UINT8 *deletes, UINT32 append_count,
  EFI_DHCP4_PACKET_OPTION *append[], EFI_DHCP4_PACKET **out)
{
  (VOID)this;
  ++builds;
  CHECK(packet == &seed && delete_count == 0U && deletes == NULL && append_count == 1U);
  CHECK(append[0]->OpCode == 50U && append[0]->Length == 4U);
  CopyMem(option_wire, append[0], sizeof(option_wire));
  if (!EFI_ERROR(build_status)) *out = &replacement;
  return build_status;
}
static EFI_DHCP4_PROTOCOL dhcp = {.Build = build};
static EFI_STATUS discover(void) { return HandoffDhcp4Discover(nic, &dhcp, &seed, &result); }

void test_main(void)
{
  handle_status = EFI_UNSUPPORTED; result = &seed;
  CHECK(discover() == EFI_SUCCESS && result == NULL && builds == 0U);
  handle_status = EFI_NOT_FOUND;
  CHECK(discover() == EFI_SUCCESS && result == NULL && builds == 0U);
  handle_status = EFI_DEVICE_ERROR;
  CHECK(discover() == EFI_DEVICE_ERROR && builds == 0U);
  handle_status = EFI_SUCCESS; info.Revision = NET_HANDOFF_REVISION;
  CHECK(discover() == EFI_ACCESS_DENIED && builds == 0U);
  info.State = NET_HANDOFF_ACTIVE; info.Session = 72U;
  info.Ip[0] = 192U; info.Ip[1] = 168U; info.Ip[2] = 1U; info.Ip[3] = 201U;
  CHECK(discover() == EFI_SUCCESS && result == &replacement && builds == 1U);
  CHECK(option_wire[0] == 50U && option_wire[1] == 4U);
  for (UINTN i = 0; i < sizeof(info.Ip); ++i) CHECK(option_wire[i + 2U] == info.Ip[i]);
  /* 换槽后的真实IP必须重新读取，不沿用上次201或编译期常量。 */
  info.Ip[2] = 210U; info.Ip[3] = 18U;
  CHECK(discover() == EFI_SUCCESS && option_wire[4] == 210U && option_wire[5] == 18U);
  build_status = EFI_OUT_OF_RESOURCES;
  CHECK(discover() == EFI_OUT_OF_RESOURCES && result == NULL);
  info_status = EFI_NOT_READY;
  CHECK(discover() == EFI_NOT_READY && result == NULL);
  CHECK(HandoffDhcp4Discover(nic, &dhcp, &seed, NULL) == EFI_INVALID_PARAMETER);
  message("PASS: DHCP authorized Option 50, current IP, ordinary NIC and allocation errors\n");
  finish(0U);
}

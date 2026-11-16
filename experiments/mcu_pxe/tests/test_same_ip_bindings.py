"""验证同IP绑定配置及会话门禁；仅访问临时INI，不启动服务、不验证实际IP交接。"""
import configparser
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


MODULE_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(MODULE_ROOT))
if os.name == "nt":
    import same_ip_bindings as bindings


MAC_A = "02:AA:BB:CC:DD:01"
MAC_B = "02:AA:BB:CC:DD:02"
IP_A = "192.168.1.201"
IP_B = "192.168.1.202"
SESSION_A = "round-1.site_1"
SESSION_B = "round-2.site_1"
TEMPLATE = """[DHCP]
Mask=255.255.255.0
PoolSize=0
BootFile=BOOTAA64.efi

[TFTPD32]
LocalIP=192.168.1.200
DHCP LocalIP=192.168.1.200
Services=1

[Keep Existing Settings]
Unrelated=unchanged
"""


@unittest.skipUnless(os.name == "nt", "需要Windows原生INI API，不用模拟实现替代")
class SameIpBindingsTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="same-ip-bindings-")
        self.addCleanup(self.directory.cleanup)
        self.path = Path(self.directory.name) / "tftpd64.ini"
        self.path.write_bytes(TEMPLATE.replace("\n", "\r\n").encode("ascii"))
        self.lock_path = self.path.with_name(self.path.name + ".lock")

    def assert_snapshot(self, snapshot, expected_bindings, expected_locks=None):
        self.assertEqual(snapshot["server_ip"], "192.168.1.200")
        self.assertEqual(snapshot["network"], "192.168.1.0/24")
        self.assertIs(snapshot["dhcp_enabled"], False)
        self.assertEqual(snapshot["bindings"], expected_bindings)
        self.assertEqual(snapshot["locks"], expected_locks or {})

    def assert_rejected_without_change(self, operation):
        before = self.path.read_bytes()
        with self.assertRaises(ValueError):
            operation()
        self.assertEqual(self.path.read_bytes(), before)
        self.assertFalse(self.lock_path.exists(), "失败不能遗留本进程创建的文件锁")

    def read_ini_independently(self):
        # 冒号属于MAC键，不能让ConfigParser把它当作键值分隔符。
        parser = configparser.ConfigParser(interpolation=None, delimiters=("=",))
        parser.optionxform = str
        with self.path.open(encoding="ascii") as stream:
            parser.read_file(stream)
        return parser

    def test_empty_template_is_read_without_modification(self):
        before = self.path.read_bytes()
        self.assert_snapshot(bindings.read_bindings(self.path), {})
        self.assertEqual(self.path.read_bytes(), before)
        self.assertFalse(self.lock_path.exists())

    def test_two_boards_update_independently_and_normalize_mac(self):
        self.assert_snapshot(bindings.update_binding(self.path, MAC_A.lower(), IP_A),
                             {MAC_A: IP_A})
        self.assert_snapshot(bindings.update_binding(self.path, MAC_B, IP_B),
                             {MAC_A: IP_A, MAC_B: IP_B})
        expected = {MAC_A: "192.168.1.203", MAC_B: IP_B}
        self.assert_snapshot(bindings.update_binding(self.path, MAC_A, expected[MAC_A]),
                             expected)
        self.assert_snapshot(bindings.read_bindings(self.path), expected)
        parser = self.read_ini_independently()
        for mac, ip in expected.items():
            self.assertEqual(parser.get("DHCP", mac), ip)
            self.assertNotIn(mac.lower(), parser["DHCP"])
        self.assertFalse(self.lock_path.exists())

    def test_same_address_update_is_idempotent(self):
        first = bindings.update_binding(self.path, MAC_A, IP_A)
        self.assertEqual(bindings.update_binding(self.path, MAC_A.lower(), IP_A), first)
        self.assert_snapshot(bindings.read_bindings(self.path), {MAC_A: IP_A})

    def test_duplicate_ip_is_rejected_without_affecting_either_board(self):
        bindings.update_binding(self.path, MAC_A, IP_A)
        bindings.update_binding(self.path, MAC_B, IP_B)
        self.assert_rejected_without_change(
            lambda: bindings.update_binding(self.path, MAC_B, IP_A))
        self.assert_snapshot(bindings.read_bindings(self.path), {MAC_A: IP_A, MAC_B: IP_B})

    def test_invalid_mac_is_rejected(self):
        invalid = ("", "02-AA-BB-CC-DD-01", "02:A:BB:CC:DD:01", "02:AA:BB:CC:DD",
                   "02:AA:BB:CC:DD:01:02", "02:AA:BB:CC:DD:GG", "00:00:00:00:00:00",
                   "01:AA:BB:CC:DD:01", "FF:FF:FF:FF:FF:FF", "02:AA:BB:CC:DD:01\n")
        for mac in invalid:
            with self.subTest(mac=mac):
                self.assert_rejected_without_change(
                    lambda: bindings.update_binding(self.path, mac, IP_A))

    def test_nonzero_unicast_mac_does_not_require_local_prefix(self):
        mac = "00:11:22:33:44:55"
        self.assert_snapshot(bindings.update_binding(self.path, mac, IP_A), {mac: IP_A})

    def test_invalid_or_reserved_ip_is_rejected(self):
        bindings.update_binding(self.path, MAC_A, IP_A)
        invalid = ("192.168.1.0", "192.168.1.255", "192.168.1.200", "192.168.2.201",
                   "0.0.0.0", "255.255.255.255", "192.168.1.256", "192.168.1.201/24", "::1")
        for ip in invalid:
            with self.subTest(ip=ip):
                self.assert_rejected_without_change(
                    lambda: bindings.update_binding(self.path, MAC_A, ip))

    def test_persistent_session_lock_allows_only_same_ip_update(self):
        bindings.update_binding(self.path, MAC_A, IP_A)
        expected = {MAC_A: IP_A}
        locked = {MAC_A: SESSION_A}
        self.assert_snapshot(bindings.lock_binding(self.path, MAC_A.lower(), SESSION_A, IP_A),
                             expected, locked)
        self.assert_snapshot(bindings.read_bindings(self.path), expected, locked)
        self.assertTrue(self.read_ini_independently().has_section("SLT Binding Locks"))
        self.assert_snapshot(bindings.lock_binding(self.path, MAC_A, SESSION_A, IP_A), expected, locked)
        self.assert_snapshot(bindings.update_binding(self.path, MAC_A.lower(), IP_A), expected, locked)
        for operation in (
            lambda: bindings.update_binding(self.path, MAC_A, "192.168.1.203"),
            lambda: bindings.remove_binding(self.path, MAC_A),
            lambda: bindings.lock_binding(self.path, MAC_A, SESSION_B, IP_A),
            lambda: bindings.unlock_binding(self.path, MAC_A, SESSION_B),
        ):
            with self.subTest(operation=operation):
                self.assert_rejected_without_change(operation)
        self.assert_snapshot(bindings.update_binding(self.path, MAC_B, IP_B),
                             {MAC_A: IP_A, MAC_B: IP_B}, locked)

    def test_late_unlock_cannot_release_a_new_session(self):
        bindings.update_binding(self.path, MAC_A, IP_A)
        bindings.lock_binding(self.path, MAC_A, SESSION_A, IP_A)
        self.assert_snapshot(bindings.unlock_binding(self.path, MAC_A.lower(), SESSION_A),
                             {MAC_A: IP_A})
        bindings.lock_binding(self.path, MAC_A, SESSION_B, IP_A)
        self.assert_rejected_without_change(
            lambda: bindings.unlock_binding(self.path, MAC_A, SESSION_A))
        self.assert_snapshot(bindings.read_bindings(self.path), {MAC_A: IP_A}, {MAC_A: SESSION_B})
        bindings.unlock_binding(self.path, MAC_A, SESSION_B)
        self.assert_snapshot(bindings.remove_binding(self.path, MAC_A.lower()), {})
        # 非空禁用值阻止Tftpd64回退注册表；不能删键或依赖可能截断的空键枚举。
        parser = self.read_ini_independently()
        self.assertTrue(parser.has_option("DHCP", MAC_A))
        self.assertEqual(parser.get("DHCP", MAC_A), "255.255.255.255")

    def test_locking_unknown_mac_is_rejected(self):
        self.assert_rejected_without_change(
            lambda: bindings.lock_binding(self.path, MAC_A, SESSION_A, IP_A))

    def test_stale_connection_cannot_lock_a_reassigned_ip(self):
        bindings.update_binding(self.path, MAC_A, IP_A)
        current_ip = "192.168.1.203"
        bindings.update_binding(self.path, MAC_A, current_ip)
        self.assert_rejected_without_change(
            lambda: bindings.lock_binding(self.path, MAC_A, SESSION_A, IP_A))
        self.assert_snapshot(bindings.read_bindings(self.path), {MAC_A: current_ip})
        self.assert_snapshot(bindings.lock_binding(self.path, MAC_A, SESSION_B, current_ip),
                             {MAC_A: current_ip}, {MAC_A: SESSION_B})
        self.assert_rejected_without_change(
            lambda: bindings.lock_binding(self.path, MAC_A, SESSION_B, IP_A))

    def test_session_format_and_length_are_checked(self):
        bindings.update_binding(self.path, MAC_A, IP_A)
        for session in ("", "x" * 65, "round 1", "round/1", "round:1", "\n", "会话1"):
            with self.subTest(session=session):
                self.assert_rejected_without_change(
                    lambda: bindings.lock_binding(self.path, MAC_A, session, IP_A))
        for session in ("a", "A" * 64, "aZ09._-"):
            with self.subTest(valid_session=session):
                self.assert_snapshot(bindings.lock_binding(self.path, MAC_A, session, IP_A),
                                     {MAC_A: IP_A}, {MAC_A: session})
                bindings.unlock_binding(self.path, MAC_A, session)

    def test_nonzero_pool_size_prevents_writing(self):
        self.path.write_bytes(TEMPLATE.replace("PoolSize=0", "PoolSize=1").encode("ascii"))
        self.assert_rejected_without_change(
            lambda: bindings.update_binding(self.path, MAC_A, IP_A))

    def test_unrelated_configuration_is_preserved_and_dhcp_stays_disabled(self):
        bindings.update_binding(self.path, MAC_A, IP_A)
        bindings.update_binding(self.path, MAC_B, IP_B)
        bindings.lock_binding(self.path, MAC_A, SESSION_A, IP_A)
        bindings.unlock_binding(self.path, MAC_A, SESSION_A)
        snapshot = bindings.remove_binding(self.path, MAC_A)
        self.assert_snapshot(snapshot, {MAC_B: IP_B})
        parser = self.read_ini_independently()
        for section, key, value in (
            ("DHCP", "Mask", "255.255.255.0"), ("DHCP", "PoolSize", "0"),
            ("DHCP", "BootFile", "BOOTAA64.efi"), ("TFTPD32", "LocalIP", "192.168.1.200"),
            ("TFTPD32", "DHCP LocalIP", "192.168.1.200"), ("TFTPD32", "Services", "1"),
            ("Keep Existing Settings", "Unrelated", "unchanged"),
        ):
            with self.subTest(section=section, key=key):
                self.assertEqual(parser.get(section, key), value)

    def test_existing_process_lock_rejects_all_writes_and_is_not_removed(self):
        bindings.update_binding(self.path, MAC_A, IP_A)
        bindings.update_binding(self.path, MAC_B, IP_B)
        bindings.lock_binding(self.path, MAC_B, SESSION_B, IP_B)
        self.lock_path.write_bytes(b"another-process")
        before = self.path.read_bytes()
        for operation in (
            lambda: bindings.update_binding(self.path, MAC_A, "192.168.1.203"),
            lambda: bindings.lock_binding(self.path, MAC_A, SESSION_A, IP_A),
            lambda: bindings.unlock_binding(self.path, MAC_B, SESSION_B),
            lambda: bindings.remove_binding(self.path, MAC_A),
        ):
            with self.subTest(operation=operation):
                with self.assertRaises(OSError):
                    operation()
                self.assertEqual(self.path.read_bytes(), before)
                self.assertEqual(self.lock_path.read_bytes(), b"another-process")

    def test_fresh_process_reads_persisted_binding_and_session(self):
        bindings.update_binding(self.path, MAC_A, IP_A)
        bindings.lock_binding(self.path, MAC_A, SESSION_A, IP_A)
        # 新进程重新走真实Windows API，避免当前进程缓存掩盖未落盘的数据。
        script = (
            "import json,sys; from pathlib import Path; sys.path.insert(0,sys.argv[1]); "
            "import same_ip_bindings as b; print(json.dumps(b.read_bindings(Path(sys.argv[2]))))"
        )
        result = subprocess.run([sys.executable, "-B", "-X", "utf8", "-c", script,
                                 str(MODULE_ROOT), str(self.path)],
                                capture_output=True, text=True, encoding="utf-8", timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_snapshot(json.loads(result.stdout), {MAC_A: IP_A}, {MAC_A: SESSION_A})

    def test_168_board_configuration_keeps_last_disabled_entry_nonempty(self):
        expected = {
            f"02:00:00:00:{tester:02X}:{site:02X}": f"192.168.210.{10 * tester + site}"
            for tester in range(1, 22) for site in range(1, 9)
        }
        entries = "\n".join(f"{mac}={ip}" for mac, ip in expected.items())
        template = TEMPLATE.replace("192.168.1.200", "192.168.210.9")
        template = template.replace("\n[TFTPD32]", f"\n{entries}\n[TFTPD32]")
        self.path.write_bytes(template.replace("\n", "\r\n").encode("ascii"))
        snapshot = bindings.read_bindings(self.path)
        self.assertEqual(snapshot["server_ip"], "192.168.210.9")
        self.assertEqual(snapshot["network"], "192.168.210.0/24")
        self.assertIs(snapshot["dhcp_enabled"], False)
        self.assertEqual(snapshot["bindings"], expected)
        last_mac = "02:00:00:00:15:08"
        snapshot = bindings.remove_binding(self.path, last_mac)
        expected.pop(last_mac)
        self.assertEqual(snapshot["bindings"], expected)
        self.assertEqual(bindings.read_bindings(self.path)["bindings"], expected)
        parser = self.read_ini_independently()
        self.assertEqual(parser.get("DHCP", last_mac), "255.255.255.255")
        self.assertEqual(parser.get("DHCP", "02:00:00:00:01:01"), "192.168.210.11")
        self.assertEqual(parser.get("DHCP", "PoolSize"), "0")
        self.assertEqual(parser.get("TFTPD32", "Services"), "1")


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
"""Rendering oracle, fixture tests and daemon/security integration. No desktop needed."""
import importlib.util
import os
from pathlib import Path
import random
import select
import shlex
import signal
import socket
import subprocess
import tempfile
import time
import unittest
from datetime import datetime
from unittest.mock import patch
from zoneinfo import ZoneInfo

ROOT = Path(__file__).resolve().parents[1]
BIN = ROOT / "src/everbar/everbar"
DRIVER = ROOT / "src/everbar/test-status"
spec = importlib.util.spec_from_file_location("reference", ROOT / "tests/reference_everbar.py")
ref = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ref)


def driver(*args, check=True):
    return subprocess.run([str(DRIVER), *map(str, args)], text=True, capture_output=True, check=check)


class Rendering(unittest.TestCase):
    def test_original_markup_1000_states(self):
        rng = random.Random(20261119)
        for _ in range(1000):
            cpu = rng.choice([-1, 0, 1, 50, 99, 100])
            temp = rng.choice([-1, 0, 42, 84, 85, 100])
            bat = rng.choice([-1, 0, 14, 15, 39, 40, 64, 65, 89, 90, 100])
            vol = rng.choice([-1, 0, 1, 49, 50, 100, 150])
            charge, wifi, wc, bt, bc, mute = [rng.choice([False, True]) for _ in range(6)]
            wc, bc = wifi and wc, bt and bc
            ram = rng.choice(["--", "12.3 GiB", "512 MiB"])

            def run(*args):
                return {
                    ("nmcli", "radio", "wifi"): "enabled" if wifi else "disabled",
                    ("nmcli", "-t", "-f", "TYPE,STATE", "device", "status"): "wifi:connected" if wc else "wifi:disconnected",
                    ("bluetoothctl", "show"): "Powered: yes" if bt else "Powered: no",
                    ("bluetoothctl", "devices", "Connected"): "Device AA:BB test" if bc else "",
                    ("wpctl", "get-volume", "@DEFAULT_AUDIO_SINK@"): "" if vol < 0 else f"Volume: {vol / 100:.2f}" + (" [MUTED]" if mute else ""),
                }[args]

            with patch.multiple(ref, run=run, ram_usage=lambda: ram,
                                cpu_percent=lambda *_: None if cpu < 0 else cpu,
                                temperature=lambda: None if temp == -1 else temp,
                                battery=lambda: (None if bat < 0 else bat, charge)):
                expected = ref.status(None, None)
            actual = driver("render", cpu, temp, bat, vol, int(charge), int(wifi), int(wc),
                            int(bt), int(bc), int(mute), ram, ref.ACTION).stdout.rstrip("\n")
            self.assertEqual(actual, expected)

    def test_clock_dst_and_english_names(self):
        for epoch in (0, 1774745999, 1774746000, 1792889999, 1792890000, 1795080000):
            now = datetime.fromtimestamp(epoch, ZoneInfo("Europe/Oslo"))
            expected = f"{now:%B} {now.day} {now:%A %H:%M}"
            self.assertEqual(driver("clock", epoch).stdout.strip(), expected)

    def test_action_quoting(self):
        for path in ("/home/user/.local/bin/everbar", "/home/a b/everbar", "/home/a'b/everbar", "/home/a;touch HACK/everbar"):
            quoted = driver("quote", path).stdout.strip()
            self.assertEqual(shlex.split(quoted), [path])
        for path in ("/home/a)b/everbar", "/home/a^b/everbar", "/home/a\nb/everbar"):
            self.assertNotEqual(driver("quote", path, check=False).returncode, 0)

    def test_arithmetic_and_buffer_bounds(self):
        self.assertEqual(driver("selftest").stdout.strip(), "ok")


class Sensors(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="everbar-test-")
        self.root = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def write(self, path, text):
        p = self.root / path.lstrip("/")
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(str(text) + "\n")

    def battery(self, index, **values):
        for key, value in values.items():
            self.write(f"/sys/class/power_supply/BAT{index}/{key}", value)

    def compare(self):
        def mapped(path):
            path = str(path)
            return self.root / path.lstrip("/") if path.startswith(("/proc/", "/sys/")) else Path(path)
        with patch.object(ref, "Path", mapped):
            ram, temp, (bat, charge), cpu = ref.ram_usage(), ref.temperature(), ref.battery(), ref.cpu_sample()
        result = driver("sensors", self.root).stdout.rstrip("\n").split("\t")
        self.assertEqual(result[:4], [ram, str(-1 if temp is None else temp), str(-1 if bat is None else bat), str(int(charge))])
        self.assertEqual(list(map(int, result[4:])), [*(cpu or (0, 0)), int(cpu is not None)])

    def test_missing_everything(self):
        self.compare()

    def test_normal_sensors(self):
        self.write("/proc/stat", "cpu  10 2 3 50 5 1 2 7 99 99\ncpu0 1 2 3 4")
        self.write("/proc/meminfo", "MemTotal: 16777216 kB\nMemAvailable: 12345678 kB")
        self.write("/sys/class/thermal/thermal_zone0/type", "acpitz")
        self.write("/sys/class/thermal/thermal_zone1/type", "x86_pkg_temp")
        self.write("/sys/class/thermal/thermal_zone1/temp", 84500)
        self.battery(0, energy_now=20, energy_full=40, status="Charging")
        self.compare()

    def test_small_ram(self):
        self.write("/proc/meminfo", "MemTotal: 524288 kB\nMemAvailable: 123456 kB")
        self.compare()

    def test_weighted_batteries(self):
        self.battery(0, energy_now=10, energy_full=100, status="Discharging")
        self.battery(1, energy_now=200, energy_full=200, status="Full")
        self.compare()

    def test_charge_voltage_conversion(self):
        self.battery(0, charge_now=10, charge_full=20, voltage_min_design=12000000)
        self.battery(1, energy_now=30, energy_full=120)
        self.compare()

    def test_unlike_units(self):
        self.battery(0, charge_now=10, charge_full=20)
        self.battery(1, energy_now=30, energy_full=120)
        self.compare()

    def test_capacity_and_absent_battery(self):
        self.battery(0, capacity=125, status="Charging")
        self.battery(1, capacity=20, present=0)
        self.compare()

    def test_malformed_sensors(self):
        self.write("/proc/stat", "cpu nope")
        self.write("/proc/meminfo", "malformed")
        self.battery(0, energy_now="bad", energy_full=0, capacity=14)
        self.compare()


class Daemon(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="everbar-", dir=os.environ.get("TMPDIR"))
        self.runtime = Path(self.temp.name)
        self.env = dict(os.environ, XDG_RUNTIME_DIR=str(self.runtime),
                        DBUS_SYSTEM_BUS_ADDRESS=f"unix:path={self.runtime}/missing-bus")
        self.process = None
        self.listeners = []

    def tearDown(self):
        if self.process and self.process.poll() is None:
            self.process.terminate()
        if self.process:
            self.process.communicate(timeout=5)
        for listener in self.listeners:
            listener.close()
        self.temp.cleanup()

    def start(self):
        self.process = subprocess.Popen([str(BIN)], env=self.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        deadline = time.monotonic() + 4
        while not (self.runtime / "everbar-refresh.sock").exists():
            if self.process.poll() is not None:
                self.fail(self.process.communicate()[1].decode())
            if time.monotonic() >= deadline:
                self.fail("daemon startup timed out")
            time.sleep(.01)

    def listener(self, name="dwlb-0"):
        directory = self.runtime / "dwlb"
        directory.mkdir(exist_ok=True)
        listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        listener.bind(str(directory / name))
        listener.listen(16)
        self.listeners.append(listener)
        return listener

    def receive(self, listener, timeout=3):
        self.assertTrue(select.select([listener], [], [], timeout)[0], "missing dwlb message")
        connection, _ = listener.accept()
        with connection:
            return connection.recv(4096).decode()

    def test_direct_protocol_shutdown_and_single_instance(self):
        listener = self.listener()
        self.start()
        messages = [self.receive(listener), self.receive(listener)]
        self.assertTrue(any(m.startswith("all title ") for m in messages))
        self.assertTrue(any(m.startswith("all status    ^fg(") for m in messages))
        self.assertEqual((self.runtime / "everbar-refresh.sock").stat().st_mode & 0o777, 0o600)
        second = subprocess.run([str(BIN)], env=self.env, capture_output=True, timeout=3)
        self.assertEqual(second.returncode, 0, second.stderr)
        self.process.terminate()
        self.assertEqual(self.process.wait(timeout=3), 0)
        self.assertFalse((self.runtime / "everbar-refresh.sock").exists())
        self.assertTrue((self.runtime / "everbar-refresh.lock").exists())

    def test_late_and_restarted_bar(self):
        self.start()
        listener = self.listener()
        self.assertEqual({self.receive(listener).split()[1] for _ in range(2)}, {"status", "title"})
        listener.close()
        (self.runtime / "dwlb/dwlb-0").unlink()
        listener = self.listener()
        self.assertEqual({self.receive(listener).split()[1] for _ in range(2)}, {"status", "title"})

    def test_unresponsive_bar_does_not_block_shutdown(self):
        listener = self.listener()
        listener.listen(0)
        self.start()
        time.sleep(.1)
        self.process.terminate()
        self.assertEqual(self.process.wait(timeout=2), 0)

    def test_refuse_symlink_lock(self):
        target = self.runtime / "precious"
        target.write_text("unchanged")
        (self.runtime / "everbar-refresh.lock").symlink_to(target)
        result = subprocess.run([str(BIN)], env=self.env, capture_output=True, timeout=3)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(target.read_text(), "unchanged")

    def test_refuse_non_socket(self):
        target = self.runtime / "everbar-refresh.sock"
        target.write_text("unchanged")
        result = subprocess.run([str(BIN)], env=self.env, capture_output=True, timeout=3)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(target.read_text(), "unchanged")

    def test_unknown_action_and_missing_daemon(self):
        for action in ("invalid", "wifi", "mute"):
            result = subprocess.run([str(BIN), action], env=self.env, capture_output=True, timeout=3)
            self.assertNotEqual(result.returncode, 0)

    def test_no_children(self):
        self.start()
        time.sleep(.2)
        children = Path(f"/proc/{self.process.pid}/task/{self.process.pid}/children").read_text().strip()
        self.assertEqual(children, "")


class PowerMenu(unittest.TestCase):
    def test_only_explicit_successful_selection_runs_loginctl(self):
        with tempfile.TemporaryDirectory(prefix="everbar-power-") as directory:
            path = Path(directory)
            for name, content in {
                "fuzzel": '#!/bin/sh\ncat >/dev/null\nprintf "%s\\n" "$CHOICE"\nexit "$MENU_EXIT"\n',
                "loginctl": '#!/bin/sh\nprintf "%s" "$1" > "$RESULT"\n',
            }.items():
                (path / name).write_text(content)
                (path / name).chmod(0o755)
            result = path / "result"
            for choice, code, expected in [("Cancel", 0, None), ("", 0, None), ("Power off", 1, None),
                                           ("garbage", 0, None), ("Power off\nReboot", 0, None), ("Power off", 0, "poweroff"),
                                           ("Reboot", 0, "reboot"), ("Hibernate", 0, "hibernate")]:
                result.unlink(missing_ok=True)
                env = dict(os.environ, PATH=f"{path}:{os.environ['PATH']}", CHOICE=choice,
                           MENU_EXIT=str(code), RESULT=str(result))
                subprocess.run([str(BIN), "power"], env=env, check=True, timeout=3)
                self.assertEqual(result.read_text() if result.exists() else None, expected)


if __name__ == "__main__":
    unittest.main(verbosity=2)

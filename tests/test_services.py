#!/usr/bin/env python3
"""Optional integration tests: private D-Bus mocks + real, private PipeWire.

Requires Python dbus-next, dbus-daemon, pipewire, pw-cli, pw-dump, pw-metadata.
Never connects to the host's services or changes its network/audio state.
"""
import asyncio
import json
import os
from pathlib import Path
import shutil
import tempfile
import time
import unittest

from dbus_next import Message, MessageType, Variant
from dbus_next.aio import MessageBus

ROOT = Path(__file__).resolve().parents[1]
BIN = ROOT / "src/everbar/everbar"
NM = "org.freedesktop.NetworkManager"
NP = "/org/freedesktop/NetworkManager"
ACTIVE = NP + "/ActiveConnection/1"
BZ = "org.bluez"
ADAPTER = "/org/bluez/hci0"
PROPS = "org.freedesktop.DBus.Properties"


class NativeServices(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="everbar-native-")
        self.path = Path(self.temp.name)
        self.env = dict(os.environ, XDG_RUNTIME_DIR=str(self.path),
                        PIPEWIRE_RUNTIME_DIR=str(self.path), PIPEWIRE_REMOTE="pipewire-0",
                        DBUS_SYSTEM_BUS_ADDRESS=f"unix:path={self.path}/bus")
        self.children = []
        self.buses = []
        self.server = None
        self.addAsyncCleanup(self.cleanup)
        self.messages = []
        self.wifi, self.bluetooth = True, True
        self.active = True
        self.connected = True
        self.query_count = 0
        self.dbus_config = self.path / "dbus.conf"
        self.dbus_config.write_text(f'''<busconfig>
          <type>session</type><listen>unix:path={self.path}/bus</listen>
          <policy context="default"><allow user="*"/><allow own="*"/><allow send_destination="*"/>
          <allow receive_sender="*"/></policy></busconfig>''')
        (self.path / "dwlb").mkdir()

        async def receive(reader, writer):
            self.messages.append((await reader.read(4096)).decode())
            writer.close()
            await writer.wait_closed()

        self.server = await asyncio.start_unix_server(receive, path=self.path / "dwlb/dwlb-0")
        self.bus_process = await self.start_bus()
        self.bus = await self.mock_bus()
        self.pw = await self.start_pw()
        self.bar = await self.spawn(str(BIN))
        await self.wait_status(lambda s: "^fg(A7C080)\U000f05a9" in s and
                               "^fg(7FBBB3)\U000f00af" in s and "100%" in self.sound(s))

    async def cleanup(self):
        for child in reversed(self.children):
            if child.returncode is None:
                child.terminate()
                try:
                    await asyncio.wait_for(child.wait(), 3)
                except TimeoutError:
                    child.kill()
                    await child.wait()
            if os.environ.get("EVERBAR_TEST_LOGS"):
                print((await child.stderr.read()).decode())
        for bus in self.buses:
            bus.disconnect()
            try:
                await bus.wait_for_disconnect()
            except Exception:
                pass
            # dbus-next 0.2.x can leave the socket object open after disconnect.
            bus._stream.close()
            bus._sock.close()
        if self.server:
            self.server.close()
            await self.server.wait_closed()
        log = self.path / "valgrind.log"
        if log.exists():
            text = log.read_text()
            self.assertIn("ERROR SUMMARY: 0 errors", text, text)
        self.temp.cleanup()

    async def spawn(self, *args):
        if args[0] == str(BIN) and os.environ.get("EVERBAR_TEST_VALGRIND"):
            args = ("valgrind", "--leak-check=full", "--errors-for-leak-kinds=definite,indirect",
                    "--error-exitcode=99", "--log-file=" + str(self.path / "valgrind.log"), *args)
        process = await asyncio.create_subprocess_exec(*args, env=self.env,
                    stdout=asyncio.subprocess.DEVNULL, stderr=asyncio.subprocess.PIPE)
        self.children.append(process)
        return process

    async def wait_path(self, path, process):
        for _ in range(200):
            if path.exists():
                return
            if process.returncode is not None:
                self.fail((await process.stderr.read()).decode())
            await asyncio.sleep(.01)
        self.fail(f"server did not create {path}")

    async def start_bus(self):
        process = await self.spawn("dbus-daemon", "--nofork", "--config-file=" + str(self.dbus_config))
        await self.wait_path(self.path / "bus", process)
        return process

    async def start_pw(self):
        process = await self.spawn("pipewire", "--config", str(ROOT / "tests/pipewire.conf"))
        await self.wait_path(self.path / "pipewire-0", process)
        return process

    def changed(self, path, interface, values):
        self.bus.send(Message.new_signal(path, PROPS, "PropertiesChanged", "sa{sv}as", [interface, values, []]))

    async def mock_bus(self):
        bus = await MessageBus(bus_address=self.env["DBUS_SYSTEM_BUS_ADDRESS"]).connect()
        self.buses.append(bus)
        await bus.request_name(NM)
        await bus.request_name(BZ)

        def handler(message):
            if message.message_type != MessageType.METHOD_CALL:
                return
            if os.environ.get("EVERBAR_TEST_LOGS"):
                print("MOCK", message.path, message.member, message.body)
            if message.member == "GetAll" and message.path == NP:
                self.query_count += 1
                return Message.new_method_return(message, "a{sv}", [{
                    "WirelessEnabled": Variant("b", self.wifi),
                    "ActiveConnections": Variant("ao", [ACTIVE] if self.active else []),
                }])
            if message.member == "GetAll" and message.path == ACTIVE:
                self.query_count += 1
                return Message.new_method_return(message, "a{sv}", [{
                    "Type": Variant("s", "802-11-wireless"), "State": Variant("u", 2),
                }])
            if message.member == "GetManagedObjects":
                self.query_count += 1
                return Message.new_method_return(message, "a{oa{sa{sv}}}", [{
                    ADAPTER: {"org.bluez.Adapter1": {"Powered": Variant("b", self.bluetooth)}},
                    ADAPTER + "/dev_AA_BB": {"org.bluez.Device1": {"Connected": Variant("b", self.connected)}},
                }])
            if message.member == "Set":
                interface, name, value = message.body
                if message.path == NP and name == "WirelessEnabled":
                    self.wifi = value.value
                elif message.path == ADAPTER and name == "Powered":
                    self.bluetooth = value.value
                else:
                    return Message.new_error(message, "org.freedesktop.DBus.Error.InvalidArgs", "unexpected Set")
                self.changed(message.path, interface, {name: value})
                return Message.new_method_return(message)
        bus.add_message_handler(handler)
        return bus

    def latest(self):
        return next((m for m in reversed(self.messages) if m.startswith("all status ")), "")

    @staticmethod
    def sound(status):
        return status.split("  │  ")[5] if "  │  " in status else ""

    async def wait_status(self, predicate, timeout=7):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if predicate(self.latest()):
                return self.latest()
            if self.bar.returncode is not None:
                self.fail((await self.bar.stderr.read()).decode())
            await asyncio.sleep(.02)
        self.fail(f"status did not match: {self.latest()!r}")

    async def command(self, *args):
        p = await asyncio.create_subprocess_exec(*args, env=self.env,
                    stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE)
        stdout, stderr = await asyncio.wait_for(p.communicate(), 5)
        self.assertEqual(p.returncode, 0, stderr.decode())
        return stdout.decode()

    async def node_id(self, name):
        objects = json.loads(await self.command("pw-dump"))
        return next(str(o["id"]) for o in objects if o.get("info", {}).get("props", {}).get("node.name") == name)

    async def test_network_bluetooth_clicks_and_external_changes(self):
        await self.command(str(BIN), "wifi")
        await self.wait_status(lambda s: "^fg(859289)\U000f05aa" in s)
        self.assertFalse(self.wifi)
        await self.command(str(BIN), "wifi")
        await self.wait_status(lambda s: "^fg(A7C080)\U000f05a9" in s)
        await self.command(str(BIN), "bluetooth")
        await self.wait_status(lambda s: "^fg(859289)\U000f00b2" in s)
        self.assertFalse(self.bluetooth)
        self.bluetooth = True
        self.changed(ADAPTER, "org.bluez.Adapter1", {"Powered": Variant("b", True)})
        await self.wait_status(lambda s: "^fg(7FBBB3)\U000f00af" in s)
        self.active = False
        self.changed(NP, NM, {"ActiveConnections": Variant("ao", [])})
        await self.wait_status(lambda s: "^fg(859289)\U000f05a9" in s)
        count = self.query_count
        await asyncio.sleep(2.2)
        self.assertEqual(self.query_count, count, "services must not be polled")

    async def test_audio_volume_mute_default_switch_and_restart(self):
        node = await self.node_id("test-sink")
        await self.command("pw-cli", "set-param", node, "Props", "{ channelVolumes: [ 0.125, 0.729 ], mute: false }")
        await self.wait_status(lambda s: "50%" in self.sound(s))
        await self.command(str(BIN), "mute")
        await self.wait_status(lambda s: "^fg(859289)\U000f075f 50%" in self.sound(s))
        props = await self.command("pw-cli", "enum-params", node, "Props")
        self.assertIn("Bool true", props)
        await self.command("pw-metadata", "-n", "default", "0", "default.audio.sink", '{"name":"other-sink"}', "Spa:String:JSON")
        await self.wait_status(lambda s: "100%" in self.sound(s) and "^fg(D3C6AA)" in self.sound(s))
        self.pw.terminate()
        await self.pw.wait()
        await self.wait_status(lambda s: "\uf026 --" in self.sound(s))
        self.pw = await self.start_pw()
        await self.wait_status(lambda s: "100%" in self.sound(s))

    async def test_service_owner_and_bus_restart(self):
        await self.bus.release_name(NM)
        await self.wait_status(lambda s: "\U000f05aa" in s)
        await self.bus.request_name(NM)
        await self.wait_status(lambda s: "^fg(A7C080)\U000f05a9" in s)
        self.bus_process.terminate()
        await self.bus_process.wait()
        await self.wait_status(lambda s: "\U000f05aa" in s and "\U000f00b2" in s)
        (self.path / "bus").unlink(missing_ok=True)
        self.bus_process = await self.start_bus()
        self.bus = await self.mock_bus()
        await self.wait_status(lambda s: "^fg(A7C080)\U000f05a9" in s and "^fg(7FBBB3)\U000f00af" in s)


if __name__ == "__main__":
    required = ("dbus-daemon", "pipewire", "pw-cli", "pw-dump", "pw-metadata")
    missing = [name for name in required if not shutil.which(name)]
    if missing:
        raise SystemExit("Missing integration tools: " + ", ".join(missing))
    unittest.main(verbosity=2)

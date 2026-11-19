# Native everbar

Everbar is now a small, single-threaded C program in `src/everbar`. The original
Python implementation lives in `tests/reference_everbar.py` only as a regression
oracle; it is not installed or launched by the new version.

## What stays the same

- Everforest colors, Nerd Font icons, item order, separators and padding.
- Temperature warning at 85°C, battery thresholds and charging indication.
- CPU/RAM formatting and weighted multi-battery handling, including conversion
  from charge to energy using design voltage. Unlike units are never added.
- English month/day names, `Europe/Oslo` timezone and minute-resolution clock.
- Left-click Wi-Fi, Bluetooth, mute and the four-entry Fuzzel power menu.
- `~/.local/bin/everbar`, `everbar-refresh.sock`, the singleton lock, and the
  existing `everbar-start` theme/startup command.

The action path is the actual running executable, so a locally built binary can
also be tested without installing it. Shell metacharacters in paths are quoted;
paths containing characters dwlb's markup cannot represent safely are rejected.

## Resource use

| Before | Now |
| --- | --- |
| Python interpreter resident in memory | Native binary with two direct service libraries |
| Up to seven helper processes every two seconds | No helper processes in the status daemon |
| Repeated `nmcli` / `bluetoothctl` / `wpctl` queries | Subscriptions to D-Bus properties and PipeWire parameters |
| Two `dwlb` executions every cycle | Nonblocking Unix socket writes, only for changed text |
| Battery queried every two seconds | Battery queried every ten seconds |
| All queries run in sequence | Asynchronous service snapshots and parameter events |

CPU, RAM and temperature are sampled every two seconds. Clock text changes at
minute boundaries. Network and audio updates wake the loop immediately; D-Bus
signal bursts are coalesced for 40 ms. Unchanged title/status text is not resent.
Socket discovery on the existing two-second sensor tick detects late-starting or
restarted dwlb instances and sends their initial text even if values are unchanged.
Up to 64 simultaneous dwlb instances are tracked; monitor outputs within each
instance are addressed with `all` as before.

PipeWire's loop provides epoll, I/O watches and D-Bus timeout sources. Libdbus
read/write watches sharing a descriptor are combined into one epoll source.
There is no polling thread or permanent command-output reader. Missing/disconnected
services show unavailable/off indicators and are retried; once connected, idle
network and Bluetooth services are not polled. Initial D-Bus connection/authentication
uses libdbus's connection handshake; subsequent status requests are asynchronous
and have two-second timeouts.

No numerical memory/CPU reduction is guaranteed across machines: dynamic libraries,
allocator behavior and enabled services affect RSS. Compare both versions on your
machine, using PSS or private memory as well as RSS, and include the old version's
child processes when comparing CPU work.

One development-container snapshot (Alpine/musl, Python 3.14, optimized C build)
measured 3,064 KiB RSS / 1,807 KiB PSS for C, versus 11,788 KiB RSS / 9,251 KiB
PSS for the original Python daemon. C was connected to the private D-Bus mocks
and real test PipeWire server; Python used tiny shell stand-ins for its helper
commands. These are daemon-only `/proc/PID/smaps_rollup` snapshots, not a
full-desktop benchmark or a measurement on the target Void Linux machine.

## Audio correctness

The effective default sink comes from WirePlumber's `default.audio.sink` metadata.
Unknown defaults are shown as `--`; mute never guesses a different output.
Device Route parameters take precedence over node Props when the sink belongs
to a hardware route, matching WirePlumber's mixer behavior. Hardware mute updates
the route with `save=true`; virtual outputs use node Props. The visible percentage
uses cubic volume scaling and WirePlumber 0.5's first-channel convention.

Changing default output, removing nodes/metadata and restarting PipeWire invalidate
the relevant cached state. No PipeWire audio stream is created.

With multiple Bluetooth controllers, everbar deterministically selects the
lexicographically first adapter (normally hci0) and only counts its connected
devices. This may differ from an interactively selected bluetoothctl controller;
single-controller behavior is unchanged. Wi-Fi follows NetworkManager's
`WirelessEnabled` property and activated `802-11-wireless` connections.

## Build and install

Requires a C11 compiler, make, pkg-config, libdbus-1 development headers and
libpipewire-0.3 / SPA development headers. Runtime also needs the corresponding
shared libraries, PipeWire client configuration/modules and timezone data.
It uses neither libsystemd nor PulseAudio. NetworkManager and BlueZ remain the
network/Bluetooth backends. Fuzzel and loginctl are only launched for the explicit
power-menu action; normal clicks go through the daemon's private control socket.

```sh
make -C src/everbar
make -C src/everbar test
```

Stop the old everbar before replacing it: the Python and C versions intentionally
share the singleton lock, so starting C while Python runs only wakes Python.
Back up the existing installed executable/script first. Then:

```sh
make -C src/everbar install
```

Installation defaults to `$HOME/.local/bin/everbar`. `PREFIX`, `BINDIR`, `DESTDIR`,
`CC`, `CFLAGS`, `CPPFLAGS` and `LDFLAGS` can be overridden. To roll back, stop the C
daemon and restore your saved executable/script. No compositor or dwlb source
changes are necessary.

The C daemon cleans up its socket on SIGINT/SIGTERM/SIGHUP. After SIGKILL, the next
instance safely replaces the stale socket after acquiring the lock. The lock
inode is deliberately retained. Unsafe lock files, unexpected socket paths and
group/world-writable runtime directories are rejected rather than overwritten.
Click messages must originate from the same Unix UID.

Useful commands:

```sh
everbar --help
everbar --once   # local sensors only; no daemon or service connections
everbar refresh # force a sensor refresh in the running daemon
```

## Tests and remaining host checks

`make -C src/everbar test` runs C arithmetic/buffer/PipeWire-POD/route tests and
Python tests covering 1,000 byte-exact rendering comparisons with the original,
sensor fixtures, DST boundaries, shell quoting, direct socket messages, late and
restarted bars, singleton behavior, stalled sockets, safe shutdown, unsafe paths,
and cancelled/failed/confirmed power-menu selections. The tests use private
temporary directories and fake power-menu programs, never the real power action.

Optional native-service integration tests need `dbus-next` for the test Python,
`dbus-daemon`, `pipewire`, `pw-cli`, `pw-dump` and `pw-metadata`:

```sh
make -C src/everbar test-integration
```

They start a private D-Bus with NetworkManager/BlueZ mocks and a real private,
hardware-free PipeWire server. They cover external state changes, click setters,
unbalanced volume, mute, default-output switching, service owner changes and
whole D-Bus/PipeWire restarts. They never connect to the desktop's real services.
Set `EVERBAR_TEST_VALGRIND=1` to run the integration daemon under Valgrind, if
installed. Set `EVERBAR_TEST_LOGS=1` for service logs.

These checks do not substitute for a visual test with your dwlb/font setup,
NetworkManager/BlueZ policy permissions, or testing your physical audio hardware.
Route parameter parsing/selection/setters have unit coverage; real ALSA mixer and
Bluetooth hardware still need a smoke test on your machine before daily use.

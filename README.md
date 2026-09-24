# powerprofile

Power profiles for MSI laptops from one config file: any number of named
profiles (`bat`, `ac`, `gaming`, `saver`, ...), each setting the CPU energy
performance preference, turbo and performance cap, RAPL power limits, PCIe
ASPM, Wi-Fi power saving, the embedded controller through the
[msi-ec](https://github.com/BeardOverflow/msi-ec) driver (performance
profile, fan mode, cooler boost, super battery, charge thresholds, keyboard
backlight), nvidia-powerd, screen brightness and display refresh rate.

Written for an MSI GL66 (i7-11800H, RTX 3060). No daemon, no D-Bus.

## How it splits

- **`powerprofile`** runs as root and applies a profile: sysfs writes, the
  EC, NetworkManager, `nvidia-powerd`, brightness. It never talks to X.
- **`powerprofile-x`** runs as you, inside your X session, and sets the
  refresh rate of the internal panel from the profile `powerprofile`
  recorded in `/run/powerprofile-last`.

They share only the config file and that state file, so the X half needs no
privileges, no `XAUTHORITY` and no output name: it uses your own X connection
and finds the panel by itself.

The EC support is the attribute table of
[msiecctl](https://github.com/dimgerasimou/msiecctl), used directly: it is
the complete set of files this program will ever write there, and each value
is checked against the firmware's own accepted list first. `msiecd` is not
needed, `powerprofile` already has the privileges, but the two coexist.

## Build / install

```
make
sudo make install
sudo systemctl daemon-reload
sudo systemctl enable powerprofile
sudo udevadm control --reload
```

`make install` never overwrites an existing `/etc/powerprofile.conf`, and
`make uninstall` leaves it in place. `PREFIX=` and `DESTDIR=` are honoured.

### Dependencies

- C compiler (gcc/clang), make
- `libX11` and `libXrandr` development files (`powerprofile-x` only)
- Linux with `intel_pstate` and `intel-rapl`
- the `msi-ec` kernel module, loaded, for the EC keys (`lsmod | grep msi_ec`)
- `iw`, for `wifi_powersave` (NetworkManager is optional: with it a reconnect
  keeps the setting)

## Setup

1. Edit `/etc/powerprofile.conf`: set `backlight` and adjust the profiles.
   It must be owned by root and not writable by others.
2. Passwordless manual switching: `conf/powerprofile.sudoers`, instructions
   inside. Add one line per manual profile.
3. Put `powerprofile-x &` in your window manager's autostart.
4. TLP: empty its CPU options (`CPU_ENERGY_PERF_POLICY_ON_AC=""`,
   `..._ON_BAT`, `CPU_BOOST_ON_AC`, `CPU_BOOST_ON_BAT`,
   `CPU_SCALING_GOVERNOR_ON_*`) or remove it. Both would write EPP and turbo.

The systemd unit applies the profile at boot and after resume; the udev rule
does it when the AC adapter is plugged or unplugged.

## Usage

```
powerprofile                          # status (default)
powerprofile --list                   # the profile names
powerprofile --auto                   # bat or ac, keeping a manual override that still fits
powerprofile --profile gaming         # apply one profile now
powerprofile --dry-run --profile saver
powerprofile-x --once --verbose       # apply the current refresh rate once
```

`--auto` and `--profile` need root unless `--dry-run` is given. See
`powerprofile --help`, `man powerprofile`, `man powerprofile-x` and
`man powerprofile.conf`.

## Config

`/etc/powerprofile.conf`, INI style, full-line `#` comments only. `[bat]` and
`[ac]` are required; any other section is a manual override.

`[default]` is what every profile starts from: a profile takes each key it
leaves out from there, so it only lists what differs. This is what makes
switching reliable. Without it, a setting one profile changes
(`super_battery`, `cooler_boost`, ...) stays changed in the next profile that
does not name it. A key that neither the profile nor `[default]` sets leaves
that setting alone, except `pl1`/`pl2`: those restore the firmware default.
`[default]` can hold any profile key but `only`, and is not a profile you can
apply.

A value a key does not accept makes the file invalid, and is reported with
its section and key.

| key | |
|-----|-|
| `backlight` | global: device under `/sys/class/backlight`, default the first one |
| `epp` | `performance`, `balance_performance`, `balance_power`, `power`; needs HWP, see Notes |
| `governor` | cpufreq governor for every CPU; what exists depends on the driver, an unavailable one is refused |
| `epb` | `performance`, `balance-performance`, `normal`, `balance-power`, `power`; works without HWP |
| `aspm` | `default`, `powersave`, `powersupersave`; needs a kernel that allows it, see Notes |
| `turbo` | `on` / `off` |
| `max_perf` | 1-100, % of max, 100 = no cap |
| `pl1`, `pl2` | RAPL sustained / burst limit, watts |
| `nvidia_powerd` | `on` / `off`, Dynamic Boost; needs firmware support, see Notes |
| `wifi_powersave` | `on` / `off`, every wireless interface |
| `brightness` | 1-100, % of max |
| `refresh` | Hz or `max`, applied by `powerprofile-x` |
| `only` | `ac` or `bat`: usable on that power source only |
| `run` | extra shell command, runs as root |
| `shift_mode` `fan_mode` `cooler_boost` `super_battery` `charge_start` `charge_end` `kbd_backlight` | EC attributes; `msiecctl --list` shows what your firmware has |

`conf/powerprofile.conf` is a full example, with a `[default]` baseline and
each profile listing only its differences.

## Behaviour

- Every profile is applied in full: its own keys, then `[default]`'s for the
  rest. Nothing a previous profile set is left behind, unless no section
  names it.
- A manual override (`gaming`, `saver`, ...) lasts until it stops fitting its
  `only`, or another profile is applied: plugging in drops a `bat`-only
  override, unplugging drops an `ac`-only one.
- Brightness is applied only when the profile changes, so re-applying it
  after resume never undoes a brightness you set by hand.
- The firmware RAPL limits are read the first time the tool runs after boot
  and restored whenever a profile leaves `pl1`/`pl2` empty.
- Wi-Fi power saving is set on every wireless interface, found in sysfs, so
  no device name is needed. `iw` changes the running device at once;
  NetworkManager cannot, it refuses to reapply `802-11-wireless.powersave` to
  an active connection. The setting is also put on the interface's connection,
  in memory only (`--temporary`), so a later reconnect keeps it. No connection
  profile is edited and nothing reconnects.
- EC values the firmware does not accept, and attributes it does not have,
  are reported and skipped; the rest of the profile is still applied.
- `powerprofile-x` acts on the profile name, at startup and when it changes,
  never on a rewrite of the same profile, so a manual `xrandr` change stays.
  `max` is the highest rate at the current resolution, a number picks the
  closest. It waits with inotify, and exits when X does.

## Testing

```
make test
```

Runs the attribute-table tests, then builds `powerprofile` against a
synthetic sysfs tree with a fake msi-ec, `iw`, `nmcli` and `systemctl`, and walks
it through battery, AC, overrides, EC writes, Wi-Fi, dry runs and config
errors under ASan/UBSan. Nothing touches the real machine. `make debug`
builds with the sanitizers and `-fanalyzer`.

## Notes

- A Wi-Fi connection made after a profile was applied uses NetworkManager's
  default until the profile is applied again.
- CPU frequency policy depends on the cpufreq driver. `epp` needs HWP (Intel
  Speed Shift); without it the kernel starts `intel_pstate` in passive mode
  (the `intel_cpufreq` driver), where there is no EPP and the controls are
  `governor` and `epb`. `powerprofile --status` shows the driver, the
  governor and `epp: n/a`, and applying a key the driver lacks warns. HWP is
  also what provides `cpufreq/base_frequency`, which `nvidia-powerd` reads.
  If your BIOS has an "Intel Speed Shift" option, enabling it brings `epp`
  back.
- `aspm` is refused by the kernel (`Operation not permitted`) when the
  firmware has PCIe ASPM disabled; `dmesg | grep -i aspm` says whether it is.
- `nvidia_powerd` only does something where the firmware allows Dynamic
  Boost. Otherwise the service starts and logs that the firmware disabled
  it (`journalctl -u nvidia-powerd`); leave the key out then.
- dGPU runtime PM (`NVreg_DynamicPowerManagement`), PCI runtime PM and audio
  power saving are static kernel/udev settings, not profile switching.
- Firmware may lock the RAPL limits; those writes then fail with a warning
  and everything else still applies.
- The systemd unit's sandboxing is untested under a real systemd.
- Licensed under the GPL v3, like msiecctl, whose EC code it contains.

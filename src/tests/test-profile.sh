#!/bin/sh
# See LICENSE file for copyright and license details.
#
# Builds powerprofile against a synthetic sysfs tree, a fake msi-ec, and fake
# iw, nmcli and systemctl, then walks it through the transitions that matter:
# battery and AC, manual overrides, EC writes, Wi-Fi, and config errors.
# Nothing here touches the real machine. See the `test` target in the
# Makefile; CC and CFLAGS come from there.
#
# usage: test-profile.sh BUILDDIR

dir=${1:-build/tests}
src=$(dirname "$0")/..
root=$dir/profile-root
S=$root/sys
failures=0

ok() {
	printf '%s %s\n' "$1" "$2"
}

# expect WHAT WANTED GOT
expect() {
	if [ "$2" = "$3" ]; then
		ok "ok    " "$1"
	else
		ok "FAIL  " "$1 (wanted \"$2\", got \"$3\")"
		failures=$((failures + 1))
	fi
}

val() {
	cat "$1" 2>/dev/null || echo "-"
}

rm -rf "$root"
mkdir -p "$root/bin" "$root/run" \
	"$S/devices/system/cpu/cpu0/cpufreq" "$S/devices/system/cpu/cpu1/cpufreq" \
	"$S/devices/system/cpu/intel_pstate" "$S/module/pcie_aspm/parameters" \
	"$S/class/powercap/intel-rapl:0" "$S/class/backlight/intel_backlight" \
	"$S/class/power_supply/ACAD" "$S/class/power_supply/BAT1" \
	"$S/devices/platform/msi-ec/leds/msiacpi::kbd_backlight" \
	"$S/class/net/lo" "$S/class/net/eth0" \
	"$S/class/net/wlan0/wireless" "$S/class/net/wlan1/wireless" \
	"$S/devices/system/cpu/cpu0/power" "$S/devices/system/cpu/cpu1/power"

for c in 0 1; do
	echo balance_performance > "$S/devices/system/cpu/cpu$c/cpufreq/energy_performance_preference"
done
echo intel_pstate > "$S/devices/system/cpu/cpu0/cpufreq/scaling_driver"
echo active > "$S/devices/system/cpu/intel_pstate/status"
echo "performance powersave schedutil ondemand conservative userspace" \
	> "$S/devices/system/cpu/cpu0/cpufreq/scaling_available_governors"
for c in 0 1; do
	echo powersave > "$S/devices/system/cpu/cpu$c/cpufreq/scaling_governor"
	echo 6 > "$S/devices/system/cpu/cpu$c/power/energy_perf_bias"
done
echo 0 > "$S/devices/system/cpu/intel_pstate/no_turbo"
echo 100 > "$S/devices/system/cpu/intel_pstate/max_perf_pct"
echo default > "$S/module/pcie_aspm/parameters/policy"
echo 45000000 > "$S/class/powercap/intel-rapl:0/constraint_0_power_limit_uw"
echo 109000000 > "$S/class/powercap/intel-rapl:0/constraint_1_power_limit_uw"
echo 96000 > "$S/class/backlight/intel_backlight/max_brightness"
echo 50000 > "$S/class/backlight/intel_backlight/brightness"
echo Mains > "$S/class/power_supply/ACAD/type"
echo Battery > "$S/class/power_supply/BAT1/type"
echo 100 > "$S/class/power_supply/BAT1/charge_control_end_threshold"
echo 0 > "$S/class/power_supply/BAT1/charge_control_start_threshold"

ec=$S/devices/platform/msi-ec
echo comfort > "$ec/shift_mode"
printf 'eco\ncomfort\nsport\nturbo\n' > "$ec/available_shift_modes"
echo auto > "$ec/fan_mode"
printf 'auto\nsilent\nbasic\nadvanced\n' > "$ec/available_fan_modes"
echo off > "$ec/cooler_boost"
echo off > "$ec/super_battery"
echo 2 > "$ec/leds/msiacpi::kbd_backlight/brightness"
echo 3 > "$ec/leds/msiacpi::kbd_backlight/max_brightness"

# fake nmcli and iw: two wireless interfaces, one with a connection
cat > "$root/bin/nmcli" <<'NM'
#!/bin/sh
echo "$*" >> "$LOGDIR/nmcli.log"
if [ "$1" = "-t" ]; then
	printf 'lo:\neth0:11111111-1111-1111-1111-111111111111\n'
	printf 'wlan0:22222222-2222-2222-2222-222222222222\nwlan1:\n'
fi
NM
cat > "$root/bin/iw" <<'IW'
#!/bin/sh
echo "$*" >> "$LOGDIR/iw.log"
IW
cat > "$root/bin/systemctl" <<'SC'
#!/bin/sh
echo "$*" >> "$LOGDIR/systemctl.log"
SC
chmod +x "$root/bin/nmcli" "$root/bin/iw" "$root/bin/systemctl"
export LOGDIR=$root PATH="$root/bin:$PATH"

cat > "$root/powerprofile.conf" <<CONF
backlight = intel_backlight

[default]
governor = schedutil
epb = normal
aspm = default
turbo = on
max_perf = 100
nvidia_powerd = off
wifi_powersave = off
shift_mode = comfort
fan_mode = auto
cooler_boost = off
super_battery = off
kbd_backlight = 2
epp = balance_performance

[bat]
epp = power
epb = balance-power
aspm = powersupersave
turbo = off
pl1 = 20
pl2 = 35
wifi_powersave = on
brightness = 40
shift_mode = eco
fan_mode = silent
kbd_backlight = 0
run = echo bat >> $root/run.log

[ac]
brightness = 80
charge_end = 80

[gaming]
only = ac
epp = performance
governor = performance
epb = performance
nvidia_powerd = on
shift_mode = turbo
fan_mode = advanced
cooler_boost = on

[saver]
only = bat
epp = power
epb = power
aspm = powersupersave
turbo = off
max_perf = 50
pl1 = 10
pl2 = 15
wifi_powersave = on
brightness = 20
shift_mode = eco
super_battery = on
CONF
conf=$root/powerprofile.conf

$CC ${CFLAGS:--O0} -DVERSION='"test"' -DSYSFS="\"$S\"" -DRUNDIR="\"$root/run\"" \
	-DREQUIRE_ROOT=0 -DEC_PLATFORM_DIR="\"$ec\"" \
	-DEC_SUPPLY_DIR="\"$S/class/power_supply\"" -I"$src" \
	-o "$root/powerprofile" "$src/powerprofile.c" "$src/ec.c" "$src/ini.c" "$src/utils.c" \
	|| { ok "FAIL  " "build"; exit 1; }

pp() {
	"$root/powerprofile" --config "$conf" "$@"
}

state() {
	printf 'epp=%s no_turbo=%s max_perf=%s aspm=%s PL1=%s PL2=%s bright=%s | ' \
		"$(val "$S/devices/system/cpu/cpu1/cpufreq/energy_performance_preference")" \
		"$(val "$S/devices/system/cpu/intel_pstate/no_turbo")" \
		"$(val "$S/devices/system/cpu/intel_pstate/max_perf_pct")" \
		"$(val "$S/module/pcie_aspm/parameters/policy")" \
		"$(val "$S/class/powercap/intel-rapl:0/constraint_0_power_limit_uw")" \
		"$(val "$S/class/powercap/intel-rapl:0/constraint_1_power_limit_uw")" \
		"$(val "$S/class/backlight/intel_backlight/brightness")"
	printf 'last=%s override=%s' "$(val "$root/run/powerprofile-last")" \
		"$(val "$root/run/powerprofile-override")"
}

cpustate() {
	printf 'gov=%s epb=%s' "$(val "$S/devices/system/cpu/cpu1/cpufreq/scaling_governor")" \
		"$(val "$S/devices/system/cpu/cpu1/power/energy_perf_bias")"
}

ecstate() {
	printf 'shift=%s fan=%s boost=%s super=%s kbd=%s charge_end=%s' \
		"$(val "$ec/shift_mode")" "$(val "$ec/fan_mode")" "$(val "$ec/cooler_boost")" \
		"$(val "$ec/super_battery")" "$(val "$ec/leds/msiacpi::kbd_backlight/brightness")" \
		"$(val "$S/class/power_supply/BAT1/charge_control_end_threshold")"
}

battery() { echo 0 > "$S/class/power_supply/ACAD/online"; }
plug()    { echo 1 > "$S/class/power_supply/ACAD/online"; }

# --- battery, then a manual override on battery ---
battery
pp --auto >/dev/null 2>&1
expect "auto on battery applies bat" \
	"epp=power no_turbo=1 max_perf=100 aspm=powersupersave PL1=20000000 PL2=35000000 bright=38400 | last=bat override=-" "$(state)"
expect "bat sets the EC attributes" \
	"shift=eco fan=silent boost=off super=off kbd=0 charge_end=100" "$(ecstate)"
expect "bat runs its run= command" "bat" "$(val "$root/run.log")"
expect "bat sets the governor from [default] and its own epb" "gov=schedutil epb=balance-power" "$(cpustate)"

: > "$root/nmcli.log"
pp --profile saver >/dev/null 2>&1
expect "saver applies on battery" \
	"epp=power no_turbo=1 max_perf=50 aspm=powersupersave PL1=10000000 PL2=15000000 bright=19200 | last=saver override=saver" "$(state)"
expect "saver takes the governor from [default]" "gov=schedutil epb=power" "$(cpustate)"
expect "saver turns super_battery on and takes the rest from [default]" \
	"shift=eco fan=auto boost=off super=on kbd=2 charge_end=100" "$(ecstate)"

echo 77777 > "$S/class/backlight/intel_backlight/brightness"
pp --auto >/dev/null 2>&1
expect "auto keeps saver, and leaves a dimmed screen alone" \
	"77777" "$(val "$S/class/backlight/intel_backlight/brightness")"
expect "auto on battery keeps the override" "saver" "$(val "$root/run/powerprofile-override")"

pp --profile gaming >/dev/null 2>&1
expect "gaming is refused on battery" "1" "$?"

# --- plug in ---
plug
pp --auto >/dev/null 2>&1
expect "plugging in restores firmware RAPL limits and applies ac" \
	"epp=balance_performance no_turbo=0 max_perf=100 aspm=default PL1=45000000 PL2=109000000 bright=76800 | last=ac override=-" "$(state)"
expect "ac inherits the governor and epb from [default]" "gov=schedutil epb=normal" "$(cpustate)"
expect "ac inherits [default], so super_battery is off again" \
	"shift=comfort fan=auto boost=off super=off kbd=2 charge_end=80" "$(ecstate)"

: > "$root/systemctl.log"
pp --profile gaming >/dev/null 2>&1
expect "gaming applies on AC, brightness untouched" \
	"epp=performance no_turbo=0 max_perf=100 aspm=default PL1=45000000 PL2=109000000 bright=76800 | last=gaming override=gaming" "$(state)"
expect "gaming sets the performance governor and epb" "gov=performance epb=performance" "$(cpustate)"
expect "gaming sets turbo shift mode and cooler boost" \
	"shift=turbo fan=advanced boost=on super=off kbd=2 charge_end=80" "$(ecstate)"
expect "gaming starts nvidia-powerd" "start --no-block nvidia-powerd" "$(val "$root/systemctl.log")"

pp --auto >/dev/null 2>&1
expect "auto on AC keeps gaming" "gaming" "$(val "$root/run/powerprofile-last")"

# --- unplug ---
battery
pp --auto >/dev/null 2>&1
expect "unplugging drops the AC-only override and applies bat" \
	"epp=power no_turbo=1 max_perf=100 aspm=powersupersave PL1=20000000 PL2=35000000 bright=38400 | last=bat override=-" "$(state)"

expect "leaving gaming puts the governor and epb back" "gov=schedutil epb=balance-power" "$(cpustate)"
expect "leaving gaming turns cooler_boost off; charge_end, named by no default, is left alone" \
	"shift=eco fan=silent boost=off super=off kbd=0 charge_end=80" "$(ecstate)"

# --- Wi-Fi: iw for the running device, NetworkManager for the next connection ---
: > "$root/nmcli.log"; : > "$root/iw.log"
pp --profile bat >/dev/null 2>&1
expect "wifi on: iw runs on every wireless interface, none is named in the config" \
"dev wlan0 set power_save on
dev wlan1 set power_save on" "$(sort "$root/iw.log")"
expect "wifi on: the connection is set in memory only, and only where there is one" \
"-t -f DEVICE,CON-UUID device
connection modify --temporary uuid 22222222-2222-2222-2222-222222222222 802-11-wireless.powersave 3" "$(val "$root/nmcli.log")"
expect "wifi: nothing asks NetworkManager to reapply a running device" "0" "$(grep -c reapply "$root/nmcli.log")"

plug; : > "$root/nmcli.log"; : > "$root/iw.log"
pp --profile ac >/dev/null 2>&1
expect "wifi off: iw turns power saving off" \
"dev wlan0 set power_save off
dev wlan1 set power_save off" "$(sort "$root/iw.log")"
expect "wifi off maps to powersave 2" \
	"connection modify --temporary uuid 22222222-2222-2222-2222-222222222222 802-11-wireless.powersave 2" \
	"$(grep modify "$root/nmcli.log")"
battery

# without NetworkManager only the running device changes, and that is not an error
mv "$root/bin/nmcli" "$root/bin/nmcli.off"; : > "$root/iw.log"
pp --profile bat >/dev/null 2>&1
expect "wifi: works without NetworkManager" "2" "$(wc -l < "$root/iw.log" | tr -d ' ')"
mv "$root/bin/nmcli.off" "$root/bin/nmcli"

# --- a setting this machine cannot take is said, not silently skipped ---
mkdir -p "$root/hold"
for c in 0 1; do
	mv "$S/devices/system/cpu/cpu$c/cpufreq/energy_performance_preference" "$root/hold/epp$c"
done
echo intel_cpufreq > "$S/devices/system/cpu/cpu0/cpufreq/scaling_driver"
echo passive > "$S/devices/system/cpu/intel_pstate/status"
out=$(pp --dry-run --profile bat 2>&1)
case $out in
*"epp needs HWP (Intel Speed Shift), which is not active here (cpufreq driver: intel_cpufreq)"*)
	ok "ok    " "a missing EPP is reported with the reason and the cpufreq driver" ;;
*) ok "FAIL  " "missing EPP not reported: $out"; failures=$((failures + 1)) ;;
esac
out=$(pp --status 2>&1)
case $out in
*"cpufreq: intel_cpufreq  intel_pstate: passive  governor: schedutil"*"epp: n/a  epb: "*)
	ok "ok    " "--status shows the driver, the governor and epp n/a" ;;
*) ok "FAIL  " "--status output: $out"; failures=$((failures + 1)) ;;
esac
for c in 0 1; do
	mv "$root/hold/epp$c" "$S/devices/system/cpu/cpu$c/cpufreq/energy_performance_preference"
done
for c in 0 1; do
	mv "$S/devices/system/cpu/cpu$c/power/energy_perf_bias" "$root/hold/epb$c"
done
out=$(pp --dry-run --profile bat 2>&1)
case $out in
*"epb is not available on this CPU"*) ok "ok    " "a missing EPB is reported" ;;
*) ok "FAIL  " "missing EPB not reported: $out"; failures=$((failures + 1)) ;;
esac
for c in 0 1; do
	mv "$root/hold/epb$c" "$S/devices/system/cpu/cpu$c/power/energy_perf_bias"
done
echo intel_pstate > "$S/devices/system/cpu/cpu0/cpufreq/scaling_driver"
echo active > "$S/devices/system/cpu/intel_pstate/status"

# --- dry run ---
before=$(state)
out=$(pp --dry-run --profile saver 2>&1)
expect "--dry-run changes nothing" "$before" "$(state)"
case $out in
*'would write "1" to'*no_turbo*'would set shift_mode to "eco"'*) ok "ok    " "--dry-run says what it would do" ;;
*) ok "FAIL  " "--dry-run output: $out"; failures=$((failures + 1)) ;;
esac
: > "$root/nmcli.log"; : > "$root/iw.log"
out=$(pp --dry-run --profile bat 2>&1)
case $out in
*"would run: iw dev wlan0 set power_save on"*"would run: nmcli connection modify --temporary uuid 2222"*)
	ok "ok    " "--dry-run shows the iw and nmcli commands" ;;
*) ok "FAIL  " "--dry-run wifi output: $out"; failures=$((failures + 1)) ;;
esac
expect "--dry-run runs neither iw nor the modifying nmcli command" \
	"-t -f DEVICE,CON-UUID device|0" "$(val "$root/nmcli.log")|$(wc -c < "$root/iw.log" | tr -d ' ')"

# --- listing ---
expect "--list prints the profile names" "bat ac gaming saver" "$(pp --list | tr '\n' ' ' | sed 's/ $//')"
pp --status >/dev/null 2>&1
expect "--status runs" "0" "$?"

# --- config errors ---
bad() { # WHAT SNIPPET
	printf '[bat]\nepp = power\n[ac]\nepp = power\n%s\n' "$2" > "$root/bad.conf"
	"$root/powerprofile" --config "$root/bad.conf" --list >/dev/null 2>&1
	expect "config error: $1" "1" "$?"
}
bad "bad epp word" "[x]
epp = turbo"
bad "turbo not on/off" "[x]
turbo = maybe"
bad "max_perf out of range" "[x]
max_perf = 500"
bad "refresh not a number" "[x]
refresh = fast"
bad "bad epb word" "[x]
epb = turbo"
bad "duplicate section" "[bat]"
bad "only in [default]" "[default]
only = ac"
bad "[default] twice" "[default]
[default]"
bad "bad value in [default]" "[default]
turbo = maybe"
printf '[bat]\nepp = power\n' > "$root/bad.conf"
"$root/powerprofile" --config "$root/bad.conf" --list >/dev/null 2>&1
expect "config error: missing [ac]" "1" "$?"
printf '[bat]\nepp = power\nbogus = 1\n[ac]\nepp = power\n' > "$root/bad.conf"
"$root/powerprofile" --config "$root/bad.conf" --list >/dev/null 2>&1
expect "an unknown key only warns" "0" "$?"

# --- values only the firmware can judge are refused when applied, not written ---
printf '[bat]\nshift_mode = ludicrous\nfan_mode = basic\n[ac]\nepp = power\n' > "$root/bad.conf"
before=$(val "$ec/shift_mode")
"$root/powerprofile" --config "$root/bad.conf" --profile bat >/dev/null 2>&1
expect "an EC value the firmware rejects is not written" "$before" "$(val "$ec/shift_mode")"
expect "...and the next attribute still is" "basic" "$(val "$ec/fan_mode")"

# --- a governor this driver does not offer is refused, not written ---
printf '[bat]\ngovernor = ludicrous\n[ac]\nepp = power\n' > "$root/bad.conf"
before=$(val "$S/devices/system/cpu/cpu1/cpufreq/scaling_governor")
out=$("$root/powerprofile" --config "$root/bad.conf" --profile bat 2>&1)
expect "an unavailable governor is not written" "$before" "$(val "$S/devices/system/cpu/cpu1/cpufreq/scaling_governor")"
case $out in
*'governor "ludicrous" is not available'*"supported: performance powersave schedutil"*)
	ok "ok    " "...and the message lists what is supported" ;;
*) ok "FAIL  " "governor message: $out"; failures=$((failures + 1)) ;;
esac

# --- a config anybody could have edited is refused, when running as root ---
if [ "$(id -u)" = 0 ]; then
	cp "$conf" "$root/open.conf"
	chmod 666 "$root/open.conf"
	"$root/powerprofile" --config "$root/open.conf" --list >/dev/null 2>&1
	expect "a world-writable config is refused as root" "1" "$?"
fi

# --- options ---
"$root/powerprofile" --profile >/dev/null 2>&1
expect "--profile needs a name" "1" "$?"
"$root/powerprofile" --auto --list >/dev/null 2>&1
expect "only one action at a time" "1" "$?"
"$root/powerprofile" --profile nope --config "$conf" >/dev/null 2>&1
expect "an unknown profile is refused" "1" "$?"
"$root/powerprofile" --profile default --config "$conf" >/dev/null 2>&1
expect "[default] is not a profile you can apply" "1" "$?"

if [ "$failures" -ne 0 ]; then
	echo "$failures test(s) failed"
	exit 1
fi
echo "all profile tests passed"

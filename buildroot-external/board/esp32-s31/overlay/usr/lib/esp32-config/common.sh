#!/bin/sh
# Shared helpers. Sourcing this file does not apply or create settings.
PATH="${PATH:-/usr/sbin:/usr/bin:/sbin:/bin}"
export PATH
CONF_DIR="${ESP32_CONFIG_DIR:-/etc/esp32-conf}"
RUN_DIR="${ESP32_CONFIG_RUN_DIR:-/run/esp32-config}"
WIFI_CONF="${CONF_DIR}/wifi.conf"
WIFI_PROFILE="${CONF_DIR}/wpa_supplicant.conf"
BT_CONF="${CONF_DIR}/bluetooth.conf"
SYSTEM_CONF="${CONF_DIR}/system.conf"
UI_NOTICE="${UI_NOTICE:-}"

die()
{
	printf 'esp32-config: %s\n' "$*" >&2
	exit 1
}


warn()
{
	printf 'esp32-config: %s\n' "$*" >&2
}


have()
{
	command -v "$1" >/dev/null 2>&1
}


display_ascii()
{
	local original="$1" cleaned
	cleaned="$(printf '%s' "$original" | LC_ALL=C tr -cd '\040-\176')"
	if [ "$cleaned" = "$original" ]; then
		printf '%s' "$cleaned"
	elif [ -n "$cleaned" ]; then
		printf '%s [non-ASCII characters hidden]' "$cleaned"
	else
		printf '%s' '[non-ASCII text]'
	fi
}


sanitize_output()
{
	local source="$1" destination="$2" escape
	escape="$(printf '\033')"
	# Strip common ANSI control sequences first, then keep only characters that
	# are dependable on a small serial console. Non-ASCII network names are
	# labelled by the higher-level summaries instead of leaking broken glyphs.
	LC_ALL=C sed "s/${escape}\\[[0-9;?]*[ -\/]*[@-~]//g; s/\r$//" "$source" |
		LC_ALL=C tr -cd '\011\012\040-\176' >"$destination"
}


require_root()
{
	[ "$(id -u)" -eq 0 ] || die "this operation must be run as root"
}


create_file_if_missing()
{
	local file="$1" mode="$2" temporary
	shift 2
	[ -e "$file" ] && return 0
	temporary="$(mktemp "${file}.tmp.XXXXXX")" || return 1
	if ! printf '%s\n' "$@" >"$temporary"; then
		rm -f "$temporary"
		return 1
	fi
	chmod "$mode" "$temporary" || {
		rm -f "$temporary"
		return 1
	}
	mv "$temporary" "$file"
}


ensure_config()
{
	local current_hostname
	umask 077
	mkdir -p "$CONF_DIR" "$RUN_DIR" /run/wpa_supplicant || return 1
	chmod 0700 "$CONF_DIR" "$RUN_DIR" /run/wpa_supplicant 2>/dev/null || true
	current_hostname="$(hostname 2>/dev/null || printf '%s' esp32-s31)"
	create_file_if_missing "$SYSTEM_CONF" 0600 \
		"hostname=${current_hostname}" || return 1
	create_file_if_missing "$WIFI_CONF" 0600 \
		'enabled=0' 'interface=wlan0' 'dhcp=1' || return 1
	create_file_if_missing "$BT_CONF" 0600 \
		'enabled=0' 'index=0' 'le=1' || return 1
}


conf_get()
{
	local file="$1" key="$2" default_value="$3" value
	value="$(sed -n "s/^${key}=//p" "$file" 2>/dev/null | tail -n 1)"
	[ -n "$value" ] && printf '%s\n' "$value" || printf '%s\n' "$default_value"
}


conf_set()
{
	local file="$1" key="$2" value="$3" temporary
	case "$key" in
		''|*[!a-z0-9_]* ) return 1 ;;
	esac
	case "$value" in
		*'
'* ) return 1 ;;
	esac
	temporary="$(mktemp "${file}.tmp.XXXXXX")" || return 1
	if ! ESP32_CONF_KEY="$key" ESP32_CONF_VALUE="$value" awk '
		BEGIN { wanted = ENVIRON["ESP32_CONF_KEY"]; replacement = ENVIRON["ESP32_CONF_VALUE"]; found = 0 }
		index($0, wanted "=") == 1 {
			if (!found) print wanted "=" replacement
			found = 1
			next
		}
		{ print }
		END { if (!found) print wanted "=" replacement }
	' "$file" >"$temporary"; then
		rm -f "$temporary"
		return 1
	fi
	chmod 0600 "$temporary" || {
		rm -f "$temporary"
		return 1
	}
	mv "$temporary" "$file"
}


valid_enabled()
{
	[ "$1" = 0 ] || [ "$1" = 1 ]
}


valid_interface()
{
	case "$1" in
		''|*[!A-Za-z0-9_.:-]* ) return 1 ;;
	esac
}


valid_index()
{
	case "$1" in
		''|*[!0-9]* ) return 1 ;;
	esac
}


wifi_pidfile()
{
	local interface="$1"
	printf '%s/wpa_supplicant.%s.pid\n' "$RUN_DIR" "$interface"
}


dhcp_pidfile()
{
	local interface="$1"
	printf '%s/udhcpc.%s.pid\n' "$RUN_DIR" "$interface"
}


radio_prepare()
{
	local wifi_enabled bluetooth_enabled have_wifi have_bluetooth profile old
	wifi_enabled="$(conf_get "$WIFI_CONF" enabled 0)"
	bluetooth_enabled="$(conf_get "$BT_CONF" enabled 0)"
	have_wifi=0
	have_bluetooth=0
	grep -qx okay /proc/device-tree/soc/radio/wifi/status 2>/dev/null &&
		have_wifi=1
	grep -qx okay /proc/device-tree/soc/radio/bluetooth/status 2>/dev/null &&
		have_bluetooth=1
	case "${wifi_enabled}:${bluetooth_enabled}" in
	1:1) profile=radio-combo ;;
	1:0) profile=radio-wifi ;;
	0:1) profile=radio-bluetooth ;;
	*) return 1 ;;
	esac
	if [ "${wifi_enabled}:${bluetooth_enabled}" != \
	     "${have_wifi}:${have_bluetooth}" ] &&
	   ! grep -q '^esp32s31_radio ' /proc/modules 2>/dev/null; then
		for old in radio-combo radio-wifi radio-bluetooth; do
			s31-overlay remove "$old" >/dev/null 2>&1 || true
		done
		s31-overlay apply "$profile" || return 1
	fi
	/etc/init.d/S00s31-radio start
}


stop_managed_pid()
{
	local pidfile="$1" expected="$2" pid count started current state proc_dir
	proc_dir="${ESP32_CONFIG_PROC_DIR:-/proc}"
	[ -r "$pidfile" ] || return 0
	pid="$(sed -n '1p' "$pidfile")"
	case "$pid" in
		''|*[!0-9]*|0|1) warn "ignoring invalid pid file ${pidfile}"; rm -f "$pidfile"; return 0 ;;
	esac
	if ! kill -0 "$pid" 2>/dev/null; then rm -f "$pidfile"; return 0; fi
	if [ "$(sed -n '1p' "${proc_dir}/${pid}/comm" 2>/dev/null)" != "$expected" ]; then
		warn "ignoring stale pid file ${pidfile}"
		rm -f "$pidfile"
		return 0
	fi
	started="$(awk '{ print $22 }' "${proc_dir}/${pid}/stat" 2>/dev/null)"
	case "$started" in ''|*[!0-9]*) warn "cannot verify process $pid"; return 1 ;; esac
	kill "$pid" 2>/dev/null || true
	count=0
	while kill -0 "$pid" 2>/dev/null; do
		current="$(awk '{ print $22 }' "${proc_dir}/${pid}/stat" 2>/dev/null)"
		state="$(awk '{ print $3 }' "${proc_dir}/${pid}/stat" 2>/dev/null)"
		# The original process exited if its start time changed. A zombie has
		# already released its descriptors and does not block radio teardown.
		[ "$current" = "$started" ] && [ "$state" != Z ] || break
		[ "$count" -lt 5 ] || {
			warn "$expected did not stop; its pid file was retained"
			return 1
		}
		sleep 1
		count=$((count + 1))
	done
	rm -f "$pidfile"
}


wifi_stop()
{
	local interface
	ensure_config || return 1
	interface="$(conf_get "$WIFI_CONF" interface wlan0)"
	valid_interface "$interface" || {
		warn "invalid Wi-Fi interface in ${WIFI_CONF}"
		return 1
	}
	stop_managed_pid "$(dhcp_pidfile "$interface")" udhcpc || return 1
	stop_managed_pid "$(wifi_pidfile "$interface")" wpa_supplicant || return 1
	if have ip && ip link show dev "$interface" >/dev/null 2>&1; then
		ip -4 addr flush dev "$interface" 2>/dev/null || true
		ip link set dev "$interface" down 2>/dev/null || true
	fi
}


read_secret()
{
	local saved_stty result
	SECRET=''
	# Keep input hidden on a terminal; callers define their stdin protocol.
	if [ ! -t 0 ]; then
		IFS= read -r SECRET
		return $?
	fi
	saved_stty="$(stty -g)" || return 1
	trap 'stty "$saved_stty" 2>/dev/null || true' 0
	trap 'stty "$saved_stty" 2>/dev/null || true; exit 130' 1 2 3 15
	stty -echo || { trap - 0 1 2 3 15; return 1; }
	IFS= read -r SECRET
	result=$?
	stty "$saved_stty"
	trap - 0 1 2 3 15
	printf '\n'
	return "$result"
}


valid_hostname()
{
	[ "${#1}" -le 63 ] || return 1
	case "$1" in
		''|-*|*-|*..*|*[!A-Za-z0-9.-]* ) return 1 ;;
	esac
}


system_apply()
{
	local configured_hostname
	require_root
	ensure_config || return 1
	configured_hostname="$(conf_get "$SYSTEM_CONF" hostname esp32-s31)"
	valid_hostname "$configured_hostname" || {
		warn "invalid hostname in ${SYSTEM_CONF}"
		return 1
	}
	hostname "$configured_hostname"
}


system_hostname()
{
	local requested="${1:-}"
	require_root
	ensure_config || return 1
	if [ -z "$requested" ]; then
		printf 'Hostname: '
		IFS= read -r requested
	fi
	valid_hostname "$requested" || die "invalid hostname"
	conf_set "$SYSTEM_CONF" hostname "$requested" || return 1
	system_apply
	printf 'Hostname set to %s.\n' "$requested"
}


valid_chip()
{
	local suffix
	case "$1" in
		gpiochip* ) suffix="${1#gpiochip}" ;;
		* ) return 1 ;;
	esac
	valid_index "$suffix"
}


valid_line()
{
	case "$1" in
		''|*[!0-9]* ) return 1 ;;
	esac
}


safe_s31_line()
{
	local chip="$1" line="$2" information
	valid_chip "$chip" && valid_line "$line" || return 1
	# Chip numbers depend on probe order; identify the S31 by its device label.
	information="$(LC_ALL=C gpiodetect "$chip" 2>/dev/null)" || return 1
	case "$information" in
		"$chip [20583000.pinctrl] (62 lines)" ) ;;
		"$chip [20583000.pinctrl]"* ) return 1 ;;
		"$chip ["*"] ("*" lines)" ) return 0 ;;
		* ) return 1 ;;
	esac
	line="$(printf '%s\n' "$line" | sed 's/^0*//')"
	line="${line:-0}"
	[ "${#line}" -le 2 ] && [ "$line" -lt 62 ] || return 1
	case "$line" in
		26|27|28|29|30|31|32|33|34|41|58|59 ) return 1 ;;
	esac
}


gpio_get()
{
	local chip="$1" line="$2"
	safe_s31_line "$chip" "$line" || die "unsafe or invalid GPIO line"
	gpioinfo -c "$chip" "$line"
	gpioget -c "$chip" "$line"
}


gpio_pulse_cleanup()
{
	local chip="$1" line="$2" pid="$3"
	kill "$pid" 2>/dev/null || true
	wait "$pid" 2>/dev/null || true
	# Explicitly return legacy test pins to input. Current kernels also release
	# pinmux ownership and disable output when the request is closed.
	gpioget -c "$chip" "$line" >/dev/null 2>&1
}


gpio_pulse()
{
	local chip="$1" line="$2" value="$3" seconds="${4:-3}" pid remaining
	require_root
	safe_s31_line "$chip" "$line" || die "unsafe or invalid GPIO line"
	[ "$value" = 0 ] || [ "$value" = 1 ] || die "GPIO value must be 0 or 1"
	case "$seconds" in
		''|*[!0-9]* ) die "duration must be an integer" ;;
	esac
	[ "$seconds" -ge 1 ] && [ "$seconds" -le 30 ] ||
		die "duration must be between 1 and 30 seconds"
	gpioinfo -c "$chip" "$line" || return 1
	printf 'Driving %s line %s to %s for %s seconds.\n' \
		"$chip" "$line" "$value" "$seconds"
	gpioset -c "$chip" -C esp32-config-test "${line}=${value}" &
	pid=$!
	trap 'gpio_pulse_cleanup "$chip" "$line" "$pid"' 0
	trap 'exit 1' 1 2 3 15
	sleep 1
	if ! kill -0 "$pid" 2>/dev/null; then
		gpio_pulse_cleanup "$chip" "$line" "$pid" || true
		trap - 0 1 2 3 15
		return 1
	fi
	remaining=$((seconds - 1))
	[ "$remaining" -gt 0 ] && sleep "$remaining"
	if ! gpio_pulse_cleanup "$chip" "$line" "$pid"; then
		trap - 0 1 2 3 15
		die "GPIO request released but the line could not be returned to input"
	fi
	trap - 0 1 2 3 15
	printf 'GPIO request released; the line was returned to input.\n'
}


ui_prepare_terminal()
{
	local size rows columns
	TERM="${TERM:-vt100}"
	[ "$TERM" = dumb ] && TERM=vt100
	size="$(stty size 2>/dev/null || true)"
	rows="${size%% *}"
	columns="${size#* }"
	case "$rows" in ''|0|*[!0-9]*) rows=24 ;; esac
	case "$columns" in ''|0|*[!0-9]*) columns=80 ;; esac
	LINES="${LINES:-$rows}"
	COLUMNS="${COLUMNS:-$columns}"
	LC_ALL=C
	LANG=C
	NCURSES_NO_UTF8_ACS=1
	export TERM LINES COLUMNS LC_ALL LANG NCURSES_NO_UTF8_ACS
}


ui_dialog()
{
	dialog --ascii-lines --no-shadow --backtitle 'ESP32-S31 Setup' "$@"
}


ui_wait_for_startup()
{
	local count=0
	[ -e /run/rcS.log ] || return 0
	[ -e /run/rcS.done ] && return 0
	ui_dialog --title 'System startup' --infobox \
		'Finishing system startup. Please wait...' 6 58
	while [ ! -e /run/rcS.done ] && [ "$count" -lt 45 ]; do
		sleep 1
		count=$((count + 1))
	done
	[ -e /run/rcS.done ] || ui_dialog --title 'System startup' --msgbox \
		'Startup is taking longer than expected. You may continue, or inspect /run/rcS.log for details.' 9 68
}


ui_error()
{
	ui_dialog --title 'Check setting' --msgbox "$1" 9 66
}


ui_show_command()
{
	local title="$1" output clean result
	shift
	output="$(mktemp "${RUN_DIR}/dialog.XXXXXX")" || return 1
	clean="${output}.clean"
	# Run in a subshell so a validation helper can abort one action without
	# terminating the dialog session itself.
	( "$@" ) >"$output" 2>&1
	result=$?
	sanitize_output "$output" "$clean"
	if [ ! -s "$clean" ]; then
		printf '%s\n' 'No information was returned.' >"$clean"
	fi
	if [ "$result" -ne 0 ]; then
		printf '\nThe operation did not complete (error %s).\n' "$result" >>"$clean"
	fi
	ui_dialog --title "$title" --exit-label Back --textbox "$clean" 20 74
	rm -f "$output" "$clean"
	return "$result"
}


ui_run_action()
{
	local title="$1" busy="$2" success="$3" output clean result
	shift 3
	output="$(mktemp "${RUN_DIR}/action.XXXXXX")" || return 1
	clean="${output}.clean"
	ui_dialog --title "$title" --infobox "$busy" 6 62
	( "$@" ) >"$output" 2>&1
	result=$?
	sanitize_output "$output" "$clean"
	if [ "$result" -eq 0 ]; then
		UI_NOTICE="$success"
	elif [ "$result" -eq 2 ]; then
		UI_NOTICE="$(tail -n 3 "$clean")"
		[ -n "$UI_NOTICE" ] || UI_NOTICE='Saved; waiting for the connection.'
	else
		UI_NOTICE=''
		{
			printf '%s\n\n' 'The operation could not be completed.'
			[ -s "$clean" ] && cat "$clean"
		} >"${clean}.message"
		mv "${clean}.message" "$clean"
		ui_dialog --title "$title - details" --exit-label Back --textbox "$clean" 20 74
	fi
	rm -f "$output" "$clean"
	return "$result"
}

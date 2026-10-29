#!/bin/sh
# System settings shared by the menu, CLI and boot scripts.

system_paths()
{
	TIME_CONF="${CONF_DIR}/time.conf"
	AUTOSTART_CONF="${CONF_DIR}/autostart.conf"
	AUTOSTART_ARGS="${CONF_DIR}/autostart.args"
	ZONEINFO_DIR="${ESP32_CONFIG_ZONEINFO_DIR:-/usr/share/zoneinfo}"
	LOCALTIME_FILE="${ESP32_CONFIG_LOCALTIME:-/etc/localtime}"
}

system_ensure_extra()
{
	system_paths
	ensure_config || return 1
	create_file_if_missing "$TIME_CONF" 0600 \
		'timezone=Etc/UTC' 'ntp=0' 'server=pool.ntp.org' || return 1
	create_file_if_missing "$AUTOSTART_CONF" 0600 \
		'enabled=0' 'executable=' || return 1
	[ -e "$AUTOSTART_ARGS" ] || (umask 077; : >"$AUTOSTART_ARGS")
}

system_validate_text()
{
	[ -f "$1" ] && [ -r "$1" ] || return 1
	[ "$(LC_ALL=C tr -d '\011\012\040-\176\200-\377' <"$1" | wc -c)" -eq 0 ]
}

system_validate_kv()
{
	# Values are data. Never source a configuration file as shell code.
	system_validate_text "$1" || return 1
	LC_ALL=C awk -v keys=" $2 " '
		/^[ \t]*#/ || /^[ \t]*$/ { next }
		{
			p = index($0, "="); if (!p) exit 1
			key = substr($0, 1, p - 1)
			if (index(keys, " " key " ") == 0 || seen[key]++) exit 1
			if (length($0) > 2048 || $0 ~ /[[:cntrl:]]/) exit 1
		}
	' "$1"
}

system_valid_timezone()
{
	system_paths
	case "$1" in ''|/*|*..*|*[!A-Za-z0-9_+/-]*) return 1 ;; esac
	[ "${#1}" -le 128 ] && [ -f "${ZONEINFO_DIR}/$1" ] || return 1
	[ "$(dd if="${ZONEINFO_DIR}/$1" bs=4 count=1 2>/dev/null)" = TZif ]
}

system_valid_ntp_server()
{
	case "$1" in ''|-*|*[!A-Za-z0-9.:-]*) return 1 ;; esac
	[ "${#1}" -le 253 ]
}

system_validate_time_config()
{
	system_validate_kv "$1" 'timezone ntp server' || return 1
	system_valid_timezone "$(conf_get "$1" timezone Etc/UTC)" &&
		valid_enabled "$(conf_get "$1" ntp 0)" &&
		system_valid_ntp_server "$(conf_get "$1" server pool.ntp.org)"
}

system_validate_autostart_config()
{
	local file="$1" args="$2" executable enabled
	system_validate_kv "$file" 'enabled executable' || return 1
	enabled="$(conf_get "$file" enabled 0)"
	executable="$(conf_get "$file" executable '')"
	valid_enabled "$enabled" || return 1
	if [ -n "$executable" ]; then
		case "$executable" in /*) ;; *) return 1 ;; esac
		[ "${#executable}" -le 1024 ] || return 1
	fi
	[ "$enabled" = 0 ] || [ -n "$executable" ] || return 1
	[ -f "$args" ] || return 1
	system_validate_text "$args" && [ "$(wc -c <"$args")" -le 65536 ] || return 1
	LC_ALL=C awk 'length($0) > 1024 || NR > 64 || /\r/ { exit 1 }' "$args"
}

system_atomic_file()
{
	local destination="$1" temporary
	shift
	temporary="$(mktemp "${destination}.tmp.XXXXXX")" || return 1
	if ! printf '%s\n' "$@" >"$temporary" || ! chmod 0600 "$temporary" ||
	   ! mv -f "$temporary" "$destination"; then
		rm -f "$temporary"
		return 1
	fi
}

system_time_stop()
{
	if system_pid_alive "${RUN_DIR}/ntpd.owner"; then
		stop_managed_pid "${RUN_DIR}/ntpd.pid" ntpd || return 1
	fi
	rm -f "${RUN_DIR}/ntpd.pid" "${RUN_DIR}/ntpd.owner" "${RUN_DIR}/ntpd.server"
}

system_time_apply()
{
	local zone enabled server temporary old_server child
	system_ensure_extra || return 1
	system_validate_time_config "$TIME_CONF" || { warn 'Invalid date/time settings.'; return 1; }
	zone="$(conf_get "$TIME_CONF" timezone Etc/UTC)"
	enabled="$(conf_get "$TIME_CONF" ntp 0)"
	server="$(conf_get "$TIME_CONF" server pool.ntp.org)"
	[ ! -d "$LOCALTIME_FILE" ] || return 1
	temporary="${LOCALTIME_FILE}.esp32-config.$$"
	rm -f "$temporary"
	ln -s "${ZONEINFO_DIR}/${zone}" "$temporary" &&
		mv -f "$temporary" "$LOCALTIME_FILE" || { rm -f "$temporary"; return 1; }
	system_atomic_file "${LOCALTIME_FILE%/*}/timezone" "$zone" &&
		chmod 0644 "${LOCALTIME_FILE%/*}/timezone" || return 1
	# libc reads /etc/localtime, including each zone's daylight-saving rules.
	if [ "$enabled" = 0 ]; then
		system_time_stop
		return
	fi
	have ntpd || { warn 'Network time support is not installed.'; return 1; }
	have setsid || { warn 'Service startup support is not installed.'; return 1; }
	old_server="$(cat "${RUN_DIR}/ntpd.server" 2>/dev/null || true)"
	if [ "$old_server" = "$server" ] && system_pid_alive "${RUN_DIR}/ntpd.owner"; then
		return 0
	fi
	system_time_stop || return 1
	(trap '' HUP; exec setsid ntpd -n -p "$server") </dev/null >/dev/null 2>&1 &
	child=$!
	printf '%s\n' "$child" >"${RUN_DIR}/ntpd.pid"
	printf '%s\n' "$server" >"${RUN_DIR}/ntpd.server"
	system_record_pid "$child" "${RUN_DIR}/ntpd.owner" || {
		warn 'Network time service did not start.'; return 1;
	}
}

system_time_configure()
{
	local zone="$1" enabled="$2" server="$3"
	require_root
	system_ensure_extra || return 1
	system_valid_timezone "$zone" || { warn 'Choose an installed time zone.'; return 1; }
	valid_enabled "$enabled" && system_valid_ntp_server "$server" || {
		warn 'Invalid network time settings.'; return 1;
	}
	[ "$enabled" = 0 ] || have ntpd || { warn 'Network time support is not installed.'; return 1; }
	system_atomic_file "$TIME_CONF" "timezone=$zone" "ntp=$enabled" "server=$server" || return 1
	system_time_apply || { warn 'Date/time settings were saved but could not be applied completely.'; return 1; }
}

system_time_status()
{
	local applied enabled
	system_ensure_extra || return 1
	applied="$(readlink "$LOCALTIME_FILE" 2>/dev/null || printf '%s' 'system default')"
	printf 'Date and time: %s\n' "$(date '+%Y-%m-%d %H:%M:%S %Z')"
	printf 'Applied time zone: %s\n' "${applied#"${ZONEINFO_DIR}/"}"
	printf 'Saved time zone: %s\n' "$(conf_get "$TIME_CONF" timezone Etc/UTC)"
	enabled=disabled
	[ "$(conf_get "$TIME_CONF" ntp 0)" != 1 ] || enabled=enabled
	printf 'Automatic time at startup: %s\n' "$enabled"
	if system_pid_alive "${RUN_DIR}/ntpd.owner"; then
		printf 'Time service: running\n'
	else
		printf 'Time service: stopped\n'
	fi
	printf 'Saved time server: %s\n' "$(conf_get "$TIME_CONF" server pool.ntp.org)"
	[ ! -r "${RUN_DIR}/time-sync.log" ] || tail -n 4 "${RUN_DIR}/time-sync.log"
}

system_time_sync()
{
	local server result
	require_root
	system_ensure_extra || return 1
	system_validate_time_config "$TIME_CONF" || return 1
	have ntpd && have timeout || { warn 'Network time support is not installed.'; return 1; }
	server="$(conf_get "$TIME_CONF" server pool.ntp.org)"
	system_time_stop || return 1
	# BusyBox ntpd -q waits for a successful clock update; timeout is reported
	# honestly instead of treating an available daemon as synchronized time.
	timeout 30 ntpd -n -q -p "$server" >"${RUN_DIR}/time-sync.log" 2>&1
	result=$?
	system_time_apply || return 1
	[ "$result" -eq 0 ] || { cat "${RUN_DIR}/time-sync.log"; warn 'Time synchronization did not complete.'; return 1; }
	date '+Time synchronized: %Y-%m-%d %H:%M:%S %Z'
}

system_time_set()
{
	local value="$1" zone
	require_root
	system_ensure_extra || return 1
	printf '%s\n' "$value" | LC_ALL=C grep -Eq \
		'^[0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2}$' || {
		warn 'Use YYYY-MM-DD HH:MM:SS.'; return 1;
	}
	zone="$(conf_get "$TIME_CONF" timezone Etc/UTC)"
	system_valid_timezone "$zone" || return 1
	TZ=":${ZONEINFO_DIR}/${zone}" date -d "$value" >/dev/null 2>&1 || {
		warn 'Invalid date or time.'; return 1;
	}
	system_time_stop || return 1
	TZ=":${ZONEINFO_DIR}/${zone}" date -s "$value" || { system_time_apply; return 1; }
	conf_set "$TIME_CONF" ntp 0
}

system_record_pid()
{
	local pid="$1" file="$2" stamp
	case "$pid" in ''|*[!0-9]*) return 1 ;; esac
	stamp="$(sed 's/.*) //' "/proc/${pid}/stat" 2>/dev/null | awk '{print $20}')"
	[ -n "$stamp" ] && printf '%s %s\n' "$pid" "$stamp" >"$file"
}

system_pid_alive()
{
	local file="$1" pid stamp actual
	[ -r "$file" ] || return 1
	read -r pid stamp <"$file" || return 1
	case "$pid:$stamp" in *[!0-9:]*|:*|*:) return 1 ;; esac
	[ "$pid" -gt 1 ] || return 1
	actual="$(sed 's/.*) //' "/proc/${pid}/stat" 2>/dev/null | awk '{print $20}')"
	[ -n "$actual" ] && [ "$actual" = "$stamp" ] && kill -0 "$pid" 2>/dev/null
}

system_userapp_configure()
{
	local executable="$1" stage enabled argument
	shift
	require_root
	system_ensure_extra || return 1
	case "$executable" in /*) ;; *) warn 'Use an absolute executable or script path.'; return 1 ;; esac
	[ -f "$executable" ] && [ -x "$executable" ] || { warn 'The selected file is not executable.'; return 1; }
	stage="$(mktemp -d "${CONF_DIR}/.autostart.XXXXXX")" || return 1
	enabled="$(conf_get "$AUTOSTART_CONF" enabled 0)"
	printf 'enabled=%s\nexecutable=%s\n' "$enabled" "$executable" >"${stage}/autostart.conf"
	: >"${stage}/autostart.args"
	for argument in "$@"; do
		case "$argument" in *'
'*) rm -rf "$stage"; warn 'Arguments cannot contain a newline.'; return 1 ;; esac
		printf '%s\n' "$argument" >>"${stage}/autostart.args"
	done
	if ! system_validate_autostart_config "${stage}/autostart.conf" "${stage}/autostart.args" ||
	   ! maintenance_install "$stage" autostart.conf autostart.args; then
		rm -rf "$stage"
		return 1
	fi
	rm -rf "$stage"
}

system_userapp_enabled()
{
	local enabled="$1"
	require_root
	system_ensure_extra || return 1
	valid_enabled "$enabled" || return 1
	if [ "$enabled" = 1 ]; then
		[ -x "$(conf_get "$AUTOSTART_CONF" executable '')" ] || {
			warn 'Choose an executable before enabling startup.'; return 1;
		}
	fi
	conf_set "$AUTOSTART_CONF" enabled "$enabled"
}

system_userapp_run()
{
	local executable argument child result
	system_ensure_extra || return 1
	system_validate_autostart_config "$AUTOSTART_CONF" "$AUTOSTART_ARGS" || return 1
	executable="$(conf_get "$AUTOSTART_CONF" executable '')"
	[ -x "$executable" ] || { warn 'The configured program is not executable.'; return 1; }
	set --
	while IFS= read -r argument || [ -n "$argument" ]; do
		set -- "$@" "$argument"
	done <"$AUTOSTART_ARGS"
	printf 'Started: %s\nProgram: %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$executable" >"${RUN_DIR}/userapp.status"
	"$executable" "$@" &
	child=$!
	system_record_pid "$child" "${RUN_DIR}/userapp.child" || true
	trap 'if system_pid_alive "${RUN_DIR}/userapp.child"; then kill "$child" 2>/dev/null || true; fi' TERM INT
	wait "$child"
	result=$?
	# A trapped signal can interrupt wait before the child actually exits.
	if system_pid_alive "${RUN_DIR}/userapp.child"; then
		wait "$child"
		result=$?
	fi
	printf 'Exited: %s\nExit status: %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$result" >>"${RUN_DIR}/userapp.status"
	rm -f "${RUN_DIR}/userapp.child" "${RUN_DIR}/userapp.pid" "${RUN_DIR}/userapp.owner"
	return "$result"
}

system_userapp_start()
{
	local child
	require_root
	system_ensure_extra || return 1
	if [ "${1:-now}" = boot ]; then
		[ "$(conf_get "$AUTOSTART_CONF" enabled 0)" = 1 ] || return 0
	fi
	if system_pid_alive "${RUN_DIR}/userapp.owner"; then return 0; fi
	system_validate_autostart_config "$AUTOSTART_CONF" "$AUTOSTART_ARGS" || return 1
	[ -x "$(conf_get "$AUTOSTART_CONF" executable '')" ] || { warn 'The configured program is not executable.'; return 1; }
	have setsid || { warn 'Service startup support is not installed.'; return 1; }
	# One supervisor records the child's outcome. Input is detached from the
	# serial console; neither command strings nor argument files are evaluated.
	# Program output is not accumulated on the small RAM filesystem. A program
	# that needs logs can choose its own destination and rotation policy.
	(trap '' HUP; exec setsid "${ESP32_CONFIG_COMMAND:-/usr/sbin/esp32-config}" system autostart run) </dev/null >/dev/null 2>&1 &
	child=$!
	printf '%s\n' "$child" >"${RUN_DIR}/userapp.pid"
	system_record_pid "$child" "${RUN_DIR}/userapp.owner" || true
}

system_userapp_stop()
{
	local pid stamp count=0
	require_root
	if system_pid_alive "${RUN_DIR}/userapp.owner"; then
		read -r pid stamp <"${RUN_DIR}/userapp.owner"
		kill "$pid" || return 1
		while system_pid_alive "${RUN_DIR}/userapp.owner" && [ "$count" -lt 5 ]; do
			sleep 1
			count=$((count + 1))
		done
		if system_pid_alive "${RUN_DIR}/userapp.owner"; then
			warn 'The program has not stopped.'
			return 1
		fi
	fi
}

system_userapp_status()
{
	local enabled=disabled
	system_ensure_extra || return 1
	[ "$(conf_get "$AUTOSTART_CONF" enabled 0)" != 1 ] || enabled=enabled
	printf 'Run at startup: %s\nProgram: %s\n' \
		"$enabled" "$(conf_get "$AUTOSTART_CONF" executable '(none)')"
	if system_pid_alive "${RUN_DIR}/userapp.owner"; then printf 'State: running\n'; else printf 'State: stopped\n'; fi
	[ ! -r "${RUN_DIR}/userapp.status" ] || cat "${RUN_DIR}/userapp.status"
}

system_extra_apply()
{
	system_time_apply
}

system_password()
{
	require_root
	# Use the system password tool on its terminal; no password is captured in
	# dialog output, command arguments, config files or action logs.
	passwd root
}

system_choose_timezone()
{
	local zone relative
	system_paths
	set --
	# The image retains only selected zones. Build the menu from actual TZif
	# files, including any extra zones installed by the user.
	for zone in $(find "$ZONEINFO_DIR" -type f 2>/dev/null | sort); do
		relative="${zone#"${ZONEINFO_DIR}/"}"
		system_valid_timezone "$relative" || continue
		set -- "$@" "$relative" "$relative"
	done
	[ "$#" -gt 0 ] || { ui_error 'No time zone data is installed in this image.'; return 1; }
	ui_dialog --stdout --title 'Time zone' --default-item "$(conf_get "$TIME_CONF" timezone Etc/UTC)" \
		--menu 'Choose a time zone' 18 72 10 "$@"
}

system_time_menu()
{
	local choice zone enabled server value
	system_ensure_extra || return 1
	while :; do
		choice="$(ui_dialog --stdout --title 'Date and time' --cancel-label Back --menu \
			"$(system_time_status)${UI_NOTICE:+\n$UI_NOTICE}" 19 74 5 \
			configure 'Time zone and automatic time' sync 'Synchronize now' set 'Set date and time manually')" || return 0
		case "$choice" in
		configure)
			zone="$(system_choose_timezone)" || continue
			enabled="$(ui_dialog --stdout --title 'Automatic time' --default-item "$(conf_get "$TIME_CONF" ntp 0)" \
				--menu 'Set the clock from a network time server?' 10 65 2 1 Enabled 0 Disabled)" || continue
			server="$(conf_get "$TIME_CONF" server pool.ntp.org)"
			if [ "$enabled" = 1 ]; then
				server="$(ui_dialog --stdout --title 'Time server' --ok-label 'Save and apply' \
					--inputbox 'NTP server name or IP address' 9 66 "$server")" || continue
			fi
			ui_run_action 'Date and time' 'Applying date/time settings...' '' system_time_configure "$zone" "$enabled" "$server" ;;
		sync) ui_run_action 'Network time' 'Synchronizing time (up to 30 seconds)...' '' system_time_sync ;;
		set)
			value="$(ui_dialog --stdout --title 'Set date and time' --ok-label Apply --inputbox \
				'Local time: YYYY-MM-DD HH:MM:SS. Applying turns automatic time off.' 10 72 "$(date '+%Y-%m-%d %H:%M:%S')")" || continue
			ui_run_action 'Date and time' 'Setting the clock...' '' system_time_set "$value" ;;
		esac
	done
}

system_userapp_menu()
{
	local choice executable stage argument enabled
	system_ensure_extra || return 1
	while :; do
		choice="$(ui_dialog --stdout --title 'Startup program' --cancel-label Back --menu \
			"$(system_userapp_status)${UI_NOTICE:+\n$UI_NOTICE}" 21 74 6 configure 'Program and arguments' \
			enabled 'Run at startup' start 'Start saved program' stop 'Stop program')" || return 0
		case "$choice" in
		configure)
			executable="$(ui_dialog --stdout --title 'Startup program' --inputbox \
				'Absolute executable/script path. The program must stay in the foreground; scripts should exec their worker.' 10 74 "$(conf_get "$AUTOSTART_CONF" executable '')")" || continue
			stage="$(mktemp "${RUN_DIR}/arguments.XXXXXX")" || return 1
			cp "$AUTOSTART_ARGS" "$stage"
			argument="$(ui_dialog --stdout --title 'Arguments: one per line' --ok-label Save \
				--editbox "$stage" 17 74)"
			choice=$?
			rm -f "$stage"
			[ "$choice" -eq 0 ] || continue
			set --
			stage="$(mktemp "${RUN_DIR}/arguments.XXXXXX")" || return 1
			[ -z "$argument" ] || printf '%s\n' "$argument" >"$stage"
			while IFS= read -r argument || [ -n "$argument" ]; do set -- "$@" "$argument"; done <"$stage"
			rm -f "$stage"
			ui_run_action 'Startup program' 'Saving startup program...' '' system_userapp_configure "$executable" "$@" ;;
		enabled)
			enabled="$(ui_dialog --stdout --title 'Run at startup' --default-item "$(conf_get "$AUTOSTART_CONF" enabled 0)" \
				--menu 'Start this program when Linux boots?' 10 65 2 1 Enabled 0 Disabled)" || continue
			ui_run_action 'Startup program' 'Saving startup setting...' '' system_userapp_enabled "$enabled" ;;
		start) ui_run_action 'Startup program' 'Starting program...' '' system_userapp_start ;;
		stop) ui_run_action 'Startup program' 'Stopping program...' '' system_userapp_stop ;;
		esac
	done
}

system_menu()
{
	local choice requested
	system_ensure_extra || return 1
	while :; do
		choice="$(ui_dialog --stdout --title System --cancel-label Back --menu \
			"Hostname: $(hostname 2>/dev/null)${UI_NOTICE:+\n$UI_NOTICE}" 15 72 5 \
			hostname 'Hostname' password 'Change login password' time 'Date and time' autostart 'Startup program')" || return 0
		case "$choice" in
		hostname)
			requested="$(ui_dialog --stdout --title Hostname --ok-label 'Save and apply' --inputbox \
				'Device hostname' 8 64 "$(conf_get "$SYSTEM_CONF" hostname esp32-s31)")" || continue
			ui_run_action Hostname 'Applying hostname...' '' system_hostname "$requested" ;;
		password) ui_dialog --clear; system_password; printf '\nPress Enter to return.'; IFS= read -r requested ;;
		time) system_time_menu ;;
		autostart) system_userapp_menu ;;
		esac
	done
}

system_cli()
{
	local section="${1:-}" command
	[ "$#" -eq 0 ] || shift
	case "$section" in
	hostname) [ "$#" -le 1 ] || return 2; system_hostname "${1:-}" ;;
	password) [ "$#" -eq 0 ] || return 2; system_password ;;
	time)
		command="${1:-status}"; [ "$#" -eq 0 ] || shift
		case "$command" in
		configure) [ "$#" -eq 3 ] || return 2 ;;
		set) [ "$#" -eq 1 ] || return 2 ;;
		*) [ "$#" -eq 0 ] || return 2 ;;
		esac
		case "$command" in
		status) system_time_status ;; apply) system_time_apply ;; stop) system_time_stop ;;
		configure) [ "$#" -eq 3 ] && system_time_configure "$@" ;;
		sync) system_time_sync ;; set) [ "$#" -eq 1 ] && system_time_set "$1" ;;
		*) return 2 ;; esac ;;
	autostart)
		command="${1:-status}"; [ "$#" -eq 0 ] || shift
		case "$command" in
		configure) [ "$#" -ge 1 ] || return 2 ;;
		*) [ "$#" -eq 0 ] || return 2 ;;
		esac
		case "$command" in
		status) system_userapp_status ;; configure) [ "$#" -ge 1 ] && system_userapp_configure "$@" ;;
		enable) system_userapp_enabled 1 ;; disable) system_userapp_enabled 0 ;;
		start) system_userapp_start ;; boot) system_userapp_start boot ;;
		stop) system_userapp_stop ;; run) system_userapp_run ;;
		*) return 2 ;; esac ;;
	*) return 2 ;;
	esac
}

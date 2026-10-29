#!/bin/sh
# Configuration-only backups. No programs, login passwords, Bluetooth bonds,
# mounted storage, runtime state or unrelated files are included.

maintenance_names()
{
	printf '%s\n' system.conf wifi.conf wpa_supplicant.conf bluetooth.conf \
		overlays.conf gpio.conf time.conf autostart.conf autostart.args \
		swap.conf storage.conf usb.conf
}

maintenance_pending_message()
{
	[ ! -r "${RUN_DIR}/pending-restart" ] || cat "${RUN_DIR}/pending-restart"
}

maintenance_mark_pending()
{
	printf '%s\n' 'Restored/reset settings are pending. Restart Linux to apply them.' >"${RUN_DIR}/pending-restart"
}

maintenance_install()
(
	# Stage files have already been validated. A missing selected file means
	# removal. Rename old files aside first so failures can restore every one.
	# This lock also serializes startup-program two-file saves with imports.
	stage="$1"
	shift
	[ -d "$stage" ] || exit 1
	mkdir -p "$CONF_DIR" || exit 1
	lock="${CONF_DIR}/.install-lock"
	mkdir "$lock" 2>/dev/null || { warn 'Another settings operation is running.'; exit 1; }
	backup="$(mktemp -d "${CONF_DIR}/.rollback.XXXXXX")" || { rmdir "$lock"; exit 1; }
	committed=''
	finished=0
	rollback()
	{
		result=$?
		trap - EXIT HUP INT TERM
		if [ "$finished" != 1 ]; then
			for name in $committed; do
				rm -f "${CONF_DIR}/${name}"
				if [ -e "${backup}/${name}" ]; then
					mv -f "${backup}/${name}" "${CONF_DIR}/${name}" || {
						warn "Restore ${name} from ${backup} before continuing."
						rmdir "$lock" 2>/dev/null || true
						exit 1
					}
				fi
			done
		fi
		rm -rf "$backup"
		rmdir "$lock" 2>/dev/null || true
		exit "$result"
	}
	trap rollback EXIT
	trap 'exit 1' HUP INT TERM
	for name in "$@"; do
		case " $(maintenance_names | tr '\n' ' ') " in *" $name "*) ;; *) exit 1 ;; esac
		[ ! -L "${CONF_DIR}/${name}" ] && [ ! -d "${CONF_DIR}/${name}" ] || {
			warn "Cannot replace nonregular settings file: ${name}"; exit 1;
		}
		[ ! -L "${stage}/${name}" ] && [ ! -d "${stage}/${name}" ] || exit 1
	done
	for name in "$@"; do
		if [ -e "${CONF_DIR}/${name}" ]; then
			mv "${CONF_DIR}/${name}" "${backup}/${name}" || exit 1
		fi
		committed="$name $committed"
		if [ -f "${stage}/${name}" ]; then
			chmod 0600 "${stage}/${name}" &&
				mv "${stage}/${name}" "${CONF_DIR}/${name}" || exit 1
		fi
	done
	finished=1
)

maintenance_validate_overlays()
{
	# Saved overlays are one declarative record per line. Reject shell syntax,
	# duplicate profiles and malformed assignments before any runtime apply.
	LC_ALL=C awk '
		/^[ \t]*#/ || /^[ \t]*$/ { next }
		{
			if (length($0) > 511 || $0 !~ /^overlay\.[a-z0-9_-]+=[a-z0-9_-]+([ \t]+[a-zA-Z0-9_.-]+=[a-zA-Z0-9_.:-]+)*$/) exit 1
			p=index($0,"="); name=substr($0,9,p-9); value=substr($0,p+1)
			split(value,word,/[ \t]+/)
			if (word[1] != name || seen[name]++) exit 1
			if (++count > 32) exit 1
		}
	' "$1" || return 1
	have s31-overlay && s31-overlay check "$1"
}

maintenance_validate_stage()
{
	local stage="$1" name file
	[ "$(cat "${stage}/format" 2>/dev/null)" = 'esp32-config-backup 1' ] || {
		warn 'Unsupported configuration backup format.'; return 1;
	}
	for name in $(maintenance_names); do
		file="${stage}/${name}"
		[ -e "$file" ] || continue
		[ -f "$file" ] && [ ! -L "$file" ] || return 1
		system_validate_text "$file" && [ "$(wc -c <"$file")" -le 65536 ] || {
			warn "Invalid text or oversized settings file: ${name}"; return 1;
		}
		case "$name" in
		system.conf)
			system_validate_kv "$file" hostname &&
				valid_hostname "$(conf_get "$file" hostname esp32-s31)" ;;
		wifi.conf)
			wifi_validate_files "$file" "${stage}/wpa_supplicant.conf" ;;
		wpa_supplicant.conf)
			[ -f "${stage}/wifi.conf" ] &&
				wifi_validate_files "${stage}/wifi.conf" "$file" ;;
		bluetooth.conf) bt_validate_file "$file" ;;
		overlays.conf) maintenance_validate_overlays "$file" ;;
		gpio.conf) have s31-gpio && s31-gpio check "$file" ;;
		time.conf) system_validate_time_config "$file" ;;
		autostart.conf)
			system_validate_autostart_config "$file" "${stage}/autostart.args" ;;
		autostart.args)
			[ -f "${stage}/autostart.conf" ] &&
				system_validate_autostart_config "${stage}/autostart.conf" "$file" ;;
		swap.conf) system_validate_kv "$file" 'enabled device zram_fallback zram_size_kib' && storage_validate_swap_config "$file" ;;
		storage.conf) system_validate_kv "$file" 'enabled device path readonly' && storage_validate_mount_config "$file" ;;
		usb.conf) system_validate_kv "$file" 'mode serial_login address serial' && usb_validate_config "$file" ;;
		esac || { warn "Invalid or unsupported settings in ${name}."; return 1; }
	done
}

maintenance_backup()
(
	require_root
	ensure_config || exit 1
	have s31-config-archive || { warn 'Configuration archive support is not installed.'; exit 1; }
	destination="$1"
	case "$destination" in /*) ;; *) warn 'Use an absolute backup path.'; exit 1 ;; esac
	[ ! -e "$destination" ] && [ ! -L "$destination" ] || {
		warn 'That backup path already exists. Choose another filename.'; exit 1;
	}
	stage="$(mktemp -d "${RUN_DIR}/backup.XXXXXX")" || exit 1
	temporary=''
	trap 'rm -rf "$stage"; [ -z "$temporary" ] || rm -f "$temporary"' EXIT
	trap 'exit 1' HUP INT TERM
	printf '%s\n' 'esp32-config-backup 1' >"${stage}/format"
	set -- format
	for name in $(maintenance_names); do
		[ -e "${CONF_DIR}/${name}" ] || continue
		[ -f "${CONF_DIR}/${name}" ] && [ ! -L "${CONF_DIR}/${name}" ] || {
			warn "Cannot export nonregular settings file: ${name}"; exit 1;
		}
		cp "${CONF_DIR}/${name}" "${stage}/${name}" || exit 1
		set -- "$@" "$name"
	done
	maintenance_validate_stage "$stage" || exit 1
	temporary="$(mktemp "${destination}.tmp.XXXXXX")" || exit 1
	chmod 0600 "$temporary" || exit 1
	tar -cf "$temporary" -C "$stage" "$@" || exit 1
	mkdir "${stage}/verify" && s31-config-archive unpack "$temporary" "${stage}/verify" || exit 1
	# Link creates the final name without overwriting an existing backup. On
	# filesystems without hardlinks, a same-directory no-clobber move is used.
	if ln "$temporary" "$destination" 2>/dev/null; then
		rm -f "$temporary"
	else
		[ ! -e "$destination" ] && [ ! -L "$destination" ] || exit 1
		mv -n "$temporary" "$destination" || exit 1
		[ ! -e "$temporary" ] || exit 1
	fi
	temporary=''
	printf 'Configuration saved to %s\n' "$destination"
)

maintenance_restore()
(
	require_root
	ensure_config || exit 1
	have s31-config-archive || { warn 'Configuration archive support is not installed.'; exit 1; }
	stage="$(mktemp -d "${CONF_DIR}/.restore.XXXXXX")" || exit 1
	trap 'rm -rf "$stage"' EXIT
	trap 'exit 1' HUP INT TERM
	s31-config-archive unpack "$1" "$stage" || exit 1
	maintenance_validate_stage "$stage" || exit 1
	# A backup is a snapshot of tool settings. Absent known files are removed;
	# future reads recreate their defaults. Unrelated files are untouched.
	maintenance_install "$stage" $(maintenance_names) || exit 1
	maintenance_mark_pending || warn 'Could not record the pending restart notice.'
	printf '%s\n' 'Configuration restored. Restart Linux to apply all restored settings.'
	printf '%s\n' 'Running programs, GPIO and network connections have not been restarted.'
)

maintenance_reset()
(
	require_root
	ensure_config || exit 1
	section="$1"
	case "$section" in
	network) set -- wifi.conf wpa_supplicant.conf ;;
	bluetooth) set -- bluetooth.conf ;;
	interfaces) set -- overlays.conf usb.conf ;;
	gpio) set -- gpio.conf ;;
	system) set -- system.conf time.conf autostart.conf autostart.args ;;
	memory) set -- swap.conf storage.conf ;;
	all) set -- $(maintenance_names) ;;
	*) warn 'Choose network, bluetooth, interfaces, gpio, system, memory or all.'; exit 2 ;;
	esac
	stage="$(mktemp -d "${CONF_DIR}/.reset.XXXXXX")" || exit 1
	trap 'rm -rf "$stage"' EXIT
	trap 'exit 1' HUP INT TERM
	# Explicit defaults avoid preserving the old current hostname or falling
	# back to a legacy swap policy after reset.
	for name in "$@"; do
		case "$name" in
		system.conf) printf '%s\n' 'hostname=esp32-s31' >"${stage}/${name}" ;;
		swap.conf) printf '%s\n' 'enabled=0' 'device=auto' 'zram_fallback=0' 'zram_size_kib=8192' >"${stage}/${name}" ;;
		esac
	done
	maintenance_install "$stage" "$@" || exit 1
	maintenance_mark_pending || warn 'Could not record the pending restart notice.'
	printf 'Reset %s settings. Restart Linux to apply the defaults.\n' "$section"
)

maintenance_menu()
{
	local choice path section
	ensure_config || return 1
	while :; do
		choice="$(ui_dialog --stdout --title Maintenance --cancel-label Back --menu \
			"${UI_NOTICE:-Configuration backup and restore}\n$(maintenance_pending_message)" 16 74 5 backup 'Export configuration' restore 'Import configuration' \
			reset 'Reset configuration' status 'Full system status')" || return 0
		case "$choice" in
		backup)
			path="$(ui_dialog --stdout --title 'Export configuration' --ok-label Export --inputbox \
				'Backup path (.tar). Includes saved Wi-Fi credentials; store it privately.' 10 74 \
				"/root/esp32-config-$(date '+%Y%m%d-%H%M%S').tar")" || continue
			ui_run_action 'Configuration backup' 'Saving configuration...' "Saved to ${path}" maintenance_backup "$path" ;;
		restore)
			path="$(ui_dialog --stdout --title 'Import configuration' --inputbox 'Absolute path of a configuration backup (.tar)' 9 72 '')" || continue
			ui_dialog --title 'Import configuration' --yes-label Import --no-label Back --yesno \
				'Replace esp32-config settings from this backup? They will take effect after restart. User files and login passwords are preserved.' 10 72 || continue
			ui_run_action 'Configuration restore' 'Checking and restoring configuration...' \
				'Configuration restored. Restart Linux to apply it.' maintenance_restore "$path" ;;
		reset)
			section="$(ui_dialog --stdout --title 'Reset configuration' --cancel-label Back --menu \
				'Choose which settings to reset' 18 68 7 network Network bluetooth Bluetooth interfaces Interfaces \
				gpio GPIO system System memory 'Memory and storage' all 'All esp32-config settings')" || continue
			ui_dialog --title 'Reset configuration' --yes-label Reset --no-label Back --yesno \
				"Reset ${section} settings? Defaults take effect after restart. User files and login passwords are preserved." 10 72 || continue
			ui_run_action 'Reset configuration' 'Resetting settings...' 'Settings reset. Restart Linux to apply them.' maintenance_reset "$section" ;;
		status) ui_show_command 'System status' show_status ;;
		esac
	done
}

maintenance_cli()
{
	local command="${1:-}"
	[ "$#" -eq 0 ] || shift
	case "$command" in
	backup) [ "$#" -eq 1 ] && maintenance_backup "$1" ;;
	restore) [ "$#" -eq 1 ] && maintenance_restore "$1" ;;
	reset) [ "$#" -eq 1 ] && maintenance_reset "$1" ;;
	*) return 2 ;;
	esac
}

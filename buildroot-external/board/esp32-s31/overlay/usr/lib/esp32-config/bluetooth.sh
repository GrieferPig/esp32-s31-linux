# Bluetooth policy and menus. This file is sourced by esp32-config.

valid_bt_name()
{
	local bytes clean
	bytes="$(printf '%s' "$1" | LC_ALL=C wc -c)"
	[ "$bytes" -ge 1 ] && [ "$bytes" -le 29 ] || return 1
	clean="$(printf '%s' "$1" | LC_ALL=C tr -d '\000-\037\177')"
	[ "$clean" = "$1" ]
}

bt_validate_file()
{
	local file="$1" line key value seen=' '
	[ -r "$file" ] || return 1
	LC_ALL=C grep -q '[[:cntrl:]]' "$file" && return 1
	while IFS= read -r line || [ -n "$line" ]; do
		case "$line" in ''|'#'*) continue ;; *=*) ;; *) return 1 ;; esac
		key="${line%%=*}"
		value="${line#*=}"
		case "$seen" in *" $key "*) return 1 ;; esac
		seen="${seen}${key} "
		case "$key" in
			enabled) valid_enabled "$value" || return 1 ;;
			index) [ "$value" = 0 ] || return 1 ;;
			le) [ "$value" = 1 ] || return 1 ;;
			name) valid_bt_name "$value" || return 1 ;;
			*) return 1 ;;
		esac
	done <"$file"
}

bt_name()
{
	conf_get "$BT_CONF" name 'S31 Radio'
}

bt_running()
{
	bluetooth_service status >/dev/null 2>&1
}

bt_apply()
{
	local enabled
	require_root
	ensure_config || return 1
	bt_validate_file "$BT_CONF" || {
		warn "invalid Bluetooth settings"
		return 1
	}
	enabled="$(conf_get "$BT_CONF" enabled 0)"
	if [ "$enabled" = 0 ]; then
		bluetooth_service stop || return 1
		return 0
	fi
	radio_prepare || return 1
	have rfkill && rfkill unblock bluetooth 2>/dev/null || true
	bluetooth_service start || return 1
	bt_running || {
		warn "Bluetooth did not remain running"
		return 1
	}
}

bt_info()
{
	ensure_config || return 1
	printf 'Device name   : %s\n' "$(bt_name)"
	if [ "$(conf_get "$BT_CONF" enabled 0)" = 1 ]; then
		printf 'Bluetooth     : Enabled\n'
	else
		printf 'Bluetooth     : Disabled\n'
	fi
	if bt_running; then
		printf 'Service       : Running\n'
	else
		printf 'Service       : Stopped\n'
	fi
	printf 'Services      : Classic A2DP transport and BLE GAP/GATT\n'
}

bt_set_enabled()
{
	local enabled="$1" previous
	require_root
	valid_enabled "$enabled" || return 1
	ensure_config || return 1
	previous="$(conf_get "$BT_CONF" enabled 0)"
	[ "$previous" = "$enabled" ] && return 0
	conf_set "$BT_CONF" enabled "$enabled" || return 1
	radio_reload_and_apply_all
}

bt_set_name()
{
	local name="$1" previous
	require_root
	ensure_config || return 1
	valid_bt_name "$name" || {
		warn 'Bluetooth names must contain 1 to 29 bytes and no control characters'
		return 1
	}
	previous="$(bt_name)"
	[ "$previous" = "$name" ] && return 0
	conf_set "$BT_CONF" name "$name" || return 1
	if [ "$(conf_get "$BT_CONF" enabled 0)" = 1 ]; then
		bluetooth_service restart || {
			warn 'The name was saved, but Bluetooth could not restart. Choose Retry.'
			return 1
		}
	fi
}

bt_restart()
{
	require_root
	ensure_config || return 1
	[ "$(conf_get "$BT_CONF" enabled 0)" = 1 ] || {
		warn 'Enable Bluetooth before starting its service.'
		return 1
	}
	# Restore the configured radio mode if an earlier apply did not finish.
	radio_reload_and_apply_all
}

bt_clear_pairings()
{
	require_root
	ensure_config || return 1
	[ "$(conf_get "$BT_CONF" enabled 0)" = 1 ] && bt_running || {
		warn 'Enable Bluetooth and start its service before clearing pairings.'
		return 1
	}
	bluetooth_service clear-pairings
}

bt_confirm_radio_change()
{
	local prompt="$1"
	if [ "$(conf_get "$WIFI_CONF" enabled 0)" = 1 ]; then
		prompt="${prompt}\n\nThe shared radio will restart, temporarily disconnecting Wi-Fi."
	fi
	ui_dialog --title Bluetooth --yesno "$prompt" 10 68
}

bt_edit_name()
{
	local name
	name="$(bt_name)"
	while :; do
		name="$(ui_dialog --stdout --title 'Bluetooth name' --ok-label Apply \
			--cancel-label Back --inputbox \
			'Name shown to nearby devices (up to 29 bytes).' 9 68 "$name")" || return
		if ! valid_bt_name "$name"; then
			ui_error 'Enter 1 to 29 bytes without control characters.'
			continue
		fi
		[ "$name" = "$(bt_name)" ] && return 0
		if bt_running; then
			ui_dialog --title 'Change Bluetooth name' --yesno \
				'Bluetooth will restart and disconnect its devices to use the new name.' 9 68 || return
		fi
		ui_run_action 'Bluetooth name' 'Applying the device name...' '' bt_set_name "$name"
		return
	done
}

bt_menu()
{
	local choice enabled state toggle summary
	while :; do
		enabled="$(conf_get "$BT_CONF" enabled 0)"
		if [ "$enabled" = 1 ]; then
			toggle='Enabled: Yes'
			if bt_running; then state=Running; else state='Stopped - choose Retry'; fi
		else
			toggle='Enabled: No'
			state=Disabled
		fi
		summary="Name: $(display_ascii "$(bt_name)")\nStatus: ${state}"
		set -- --stdout --title Bluetooth --cancel-label Back --menu "$summary" 18 72 7 \
			enabled "$toggle" name 'Device name'
		if [ "$enabled" = 1 ]; then
			if bt_running; then
				set -- "$@" clear 'Clear saved pairings'
			else
				set -- "$@" retry 'Retry starting Bluetooth'
			fi
		fi
		choice="$(ui_dialog "$@")" || return
		case "$choice" in
			enabled)
				if [ "$enabled" = 1 ]; then
					bt_confirm_radio_change 'Turn Bluetooth off and disconnect its devices?' || continue
					ui_run_action Bluetooth 'Turning Bluetooth off...' '' bt_set_enabled 0
				else
					if [ "$(conf_get "$WIFI_CONF" enabled 0)" = 1 ]; then
						bt_confirm_radio_change 'Turn Bluetooth on?' || continue
					fi
					ui_run_action Bluetooth 'Starting Bluetooth...' '' bt_set_enabled 1
				fi
				;;
			name) bt_edit_name ;;
			clear)
				ui_dialog --title 'Clear saved pairings' --yesno \
					'Disconnect Bluetooth devices and forget all saved pairings? Remove this board from your devices before pairing again.' 10 70 || continue
				ui_run_action Bluetooth 'Clearing pairings and restarting Bluetooth...' '' bt_clear_pairings
				;;
			retry)
				if [ "$(conf_get "$WIFI_CONF" enabled 0)" = 1 ]; then
					bt_confirm_radio_change 'Retry starting Bluetooth?' || continue
				fi
				ui_run_action Bluetooth 'Starting Bluetooth...' '' bt_restart
				;;
		esac
	done
}

bluetooth_cli()
{
	local command="${1:-}"
	[ "$#" -gt 0 ] && shift
	case "$command" in
		info|status) [ "$#" -eq 0 ] || return 2; bt_info ;;
		enable) [ "$#" -eq 0 ] || return 2; bt_set_enabled 1 ;;
		disable) [ "$#" -eq 0 ] || return 2; bt_set_enabled 0 ;;
		name)
			case "$#" in
				0) bt_name ;;
				1) bt_set_name "$1" ;;
				*) return 2 ;;
			esac ;;
		clear-pairings) [ "$#" -eq 0 ] || return 2; bt_clear_pairings ;;
		restart) [ "$#" -eq 0 ] || return 2; bt_restart ;;
		scan) warn 'The Bluetooth service does not provide scanning.'; return 1 ;;
		*) warn 'Usage: esp32-config bluetooth info|enable|disable|name [NAME]|clear-pairings|restart'; return 2 ;;
	esac
}

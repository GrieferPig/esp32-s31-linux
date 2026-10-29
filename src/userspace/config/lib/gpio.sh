#!/bin/sh
# GPIO configuration pages. Sourced by esp32-config after common helpers.

gpio_backend()
{
	ESP32_CONFIG_DIR="$CONF_DIR" ESP32_CONFIG_RUN_DIR="$RUN_DIR" s31-gpio "$@"
}

gpio_ensure_service()
{
	require_root
	ensure_config || return 1
	have s31-gpio || { warn 'GPIO configuration support is not installed.'; return 1; }
	gpio_backend ping 2>/dev/null && return 0
	ESP32_CONFIG_DIR="$CONF_DIR" ESP32_CONFIG_RUN_DIR="$RUN_DIR" \
		/etc/init.d/S46s31-gpio ensure
}

gpio_apply()
{
	require_root
	ensure_config || return 1
	if ! gpio_backend ping >/dev/null 2>&1; then
		[ -f "${CONF_DIR}/gpio.conf" ] || return 0
		gpio_backend check "${CONF_DIR}/gpio.conf" || return 1
		awk '$1 !~ /^#/ && ($2 == "input" || $2 == "low" || $2 == "high") {found=1} END {exit !found}' \
			"${CONF_DIR}/gpio.conf" || return 0
	fi
	gpio_ensure_service || return 1
	gpio_backend apply
}

gpio_status()
{
	gpio_backend list
}

gpio_set()
{
	gpio_ensure_service || return 1
	gpio_backend set "$@"
}

gpio_reset()
{
	gpio_ensure_service || return 1
	gpio_backend reset
}

gpio_cli()
{
	local action="${1:-list}"
	[ "$#" -eq 0 ] || shift
	case "$action" in
		list|status ) [ "$#" -eq 0 ] || return 1; gpio_status ;;
		set ) gpio_set "$@" ;;
		read ) [ "$#" -eq 1 ] || return 1; gpio_backend read "$1" ;;
		apply ) [ "$#" -eq 0 ] || return 1; gpio_apply ;;
		reset ) [ "$#" -eq 0 ] || return 1; gpio_reset ;;
		info ) gpioinfo -c "${1:-gpiochip0}" ;;
		get ) [ "$#" -eq 2 ] || return 1; gpio_get "$1" "$2" ;;
		pulse ) [ "$#" -ge 3 ] && [ "$#" -le 4 ] || return 1; gpio_pulse "$@" ;;
		* ) warn 'Use gpio list, set, read, apply, reset, info, get or pulse.'; return 1 ;;
	esac
}

gpio_mode_label()
{
	case "$1" in
		application ) printf '%s' 'Application controlled' ;;
		input ) printf 'Input (%s)' "$2" ;;
		low ) printf '%s' 'Output low' ;;
		high ) printf '%s' 'Output high' ;;
		busy ) printf '%s' 'In use' ;;
		reserved ) printf '%s' 'Reserved' ;;
		* ) printf '%s' 'Unavailable' ;;
	esac
}

gpio_edit_pin()
{
	local pin="$1" mode="$2" bias="$3" value="$4" owner="$5"
	local draft="$6" draft_bias="$7" choice selected output header
	if [ "$mode" = reserved ] || [ "$mode" = unavailable ]; then
		ui_error "GPIO${pin} is ${mode}."
		return
	fi
	if [ "$mode" = busy ] && [ "$draft" = application ]; then
		ui_error "GPIO${pin} is in use by ${owner}. Change its interface settings or stop the owning application before configuring it as GPIO."
		return
	fi
	while :; do
		header="Current: $(gpio_mode_label "$mode" "$bias")"
		[ "$value" = - ] || header="${header}; level ${value}"
		[ "$owner" = - ] || header="${header}\nUsed by: ${owner}"
		header="${header}\nSaved changes are restored when Linux starts."
		set -- mode "Mode: $(gpio_mode_label "$draft" "$draft_bias")"
		[ "$draft" != input ] || set -- "$@" bias "Input pull: ${draft_bias}"
		[ "$mode" != input ] || set -- "$@" read 'Refresh input level'
		set -- "$@" save 'Save and apply'
		choice="$(ui_dialog --stdout --title "GPIO${pin}" --cancel-label Back \
			--menu "$header" 17 72 6 "$@")" || return 0
		case "$choice" in
			mode )
				if [ "$mode" = busy ]; then
					# A stale saved setting may be removed without touching its new owner.
					set -- application 'Remove the saved GPIO assignment'
				else
					set -- application 'Application controlled' input 'Input' \
						low 'Output low' high 'Output high'
				fi
				selected="$(ui_dialog --stdout --title "GPIO${pin} mode" \
					--default-item "$draft" --cancel-label Back --menu \
					'Choose how to use this pin.' 14 64 5 "$@")" || continue
				draft="$selected"
				[ "$draft" = input ] || draft_bias=none
				;;
			bias )
				selected="$(ui_dialog --stdout --title "GPIO${pin} input pull" \
					--default-item "$draft_bias" --cancel-label Back --menu \
					'Choose the input pull resistor.' 13 64 4 \
					none 'No pull' up 'Pull-up' down 'Pull-down')" || continue
				draft_bias="$selected"
				;;
			read )
				output="$(gpio_backend read "$pin" 2>&1)" && value="$output" || ui_error "$output"
				;;
			save )
				output="$(gpio_set "$pin" "$draft" "$draft_bias" 2>&1)"
				if [ "$?" -eq 0 ]; then
					[ -z "$output" ] || ui_error "$output"
					return
				fi
				ui_error "$output"
				;;
		esac
	done
}

gpio_menu()
{
	local listing error choice pin mode bias value owner wanted wanted_bias description row tab
	ensure_config || return 1
	listing="$(mktemp "${RUN_DIR}/gpio-list.XXXXXX")" || return 1
	error="${listing}.error"
	tab="$(printf '\t')"
	while :; do
		if ! gpio_status >"$listing" 2>"$error"; then
			ui_error "$(cat "$error")"
			break
		fi
		set --
		while IFS="$tab" read -r pin mode bias value owner wanted wanted_bias; do
			description="$(gpio_mode_label "$mode" "$bias")"
			[ "$value" = - ] || description="${description}; level ${value}"
			[ "$owner" = - ] || [ "$owner" = esp32-config ] || description="${description}: ${owner}"
			if [ "$mode" != "$wanted" ] || [ "$bias" != "$wanted_bias" ]; then
				case "$mode:$wanted" in
					busy:application|reserved:application|unavailable:application ) ;;
					* ) description="${description}; saved $(gpio_mode_label "$wanted" "$wanted_bias")" ;;
				esac
			fi
			set -- "$@" "$pin" "$description"
		done <"$listing"
		choice="$(ui_dialog --stdout --title 'GPIO' --cancel-label Back --menu \
			'Choose a GPIO pin to configure.' 21 78 13 "$@")" || break
		row="$(awk -F '\t' -v pin="$choice" '$1 == pin {print; exit}' "$listing")"
		[ -n "$row" ] || continue
		# All fields are generated by the helper; read preserves their boundaries.
		IFS="$tab" read -r pin mode bias value owner wanted wanted_bias <<EOF
$row
EOF
		gpio_edit_pin "$pin" "$mode" "$bias" "$value" "$owner" "$wanted" "$wanted_bias"
	done
	rm -f "$listing" "$error"
}

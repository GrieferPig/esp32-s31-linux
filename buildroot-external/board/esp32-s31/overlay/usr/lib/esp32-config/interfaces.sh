# Configuration pages driven by the metadata in the packaged DT overlays.

interface_feature()
{
	grep -qx "$1=1" "${ESP32_CONFIG_FEATURES_FILE:-/usr/share/esp32-config/kernel-features}" 2>/dev/null
}

interface_available()
{
	case "$1" in
		uart1|uart2|uart3) interface_feature uart ;;
		uart3-dma) interface_feature uart && interface_feature uart_dma && interface_feature ahb_gdma ;;
		i2c0|i2c1) interface_feature i2c ;;
		gpspi2|gpspi3) interface_feature spi ;;
		gpspi2-target|gpspi3-target) interface_feature spi && interface_feature spi_target ;;
		sdmmc0|sdmmc1|sdmmc-dual|sdmmc-uhs) interface_feature mmc ;;
		i2s0|i2s1) interface_feature sound ;;
		gmac) interface_feature ethernet ;;
		twai0|twai1) interface_feature can ;;
		pwm-counter) interface_feature ledc && interface_feature mcpwm && interface_feature sdm && interface_feature counter ;;
		analog) interface_feature adc && interface_feature dac && interface_feature touch && interface_feature comparator && interface_feature hwmon ;;
		lp) interface_feature lp ;;
		timers) interface_feature system_timers && interface_feature gptimer ;;
		watchdogs) interface_feature watchdog ;;
		gdma) interface_feature ahb_gdma ;;
		# Shared radio profiles and USB role are configured by their own pages.
		*) return 1 ;;
	esac
}

interface_label()
{
	case "$1" in
		uart1) printf 'UART 1' ;; uart2) printf 'UART 2' ;;
		uart3) printf 'UART 3' ;; uart3-dma) printf 'UART 3 with DMA' ;;
		i2c0) printf 'I2C 0' ;; i2c1) printf 'I2C 1' ;;
		gpspi2) printf 'SPI 2 controller' ;; gpspi3) printf 'SPI 3 controller' ;;
		gpspi2-target) printf 'SPI 2 target' ;; gpspi3-target) printf 'SPI 3 target' ;;
		sdmmc0) printf 'SDMMC slot 0' ;; sdmmc1) printf 'SDMMC slot 1' ;;
		sdmmc-dual) printf 'SDMMC dual slot' ;; sdmmc-uhs) printf 'SDMMC UHS' ;;
		i2s0) printf 'I2S 0' ;; i2s1) printf 'I2S 1' ;;
		gmac) printf 'Ethernet GMAC' ;;
		twai0) printf 'CAN 0' ;; twai1) printf 'CAN 1' ;;
		pwm-counter) printf 'PWM and pulse counter' ;;
		analog) printf 'Analog interfaces' ;; lp) printf 'Low-power peripherals' ;;
		timers) printf 'Timers' ;; watchdogs) printf 'Watchdogs' ;;
		gdma) printf 'DMA controllers' ;; *) printf '%s' "$1" ;;
	esac
}

interface_field_label()
{
	case "$1" in
		clock-frequency) printf 'Bus speed (Hz)' ;;
		bus-width) printf 'Data bus width' ;;
		*.*) printf '%s GPIO' "$(printf '%s' "$1" | tr '[:lower:]' '[:upper:]' | tr '.' ' ')" ;;
		*) printf '%s' "$1" ;;
	esac
}

interface_valid_gpio()
{
	case "$1" in ''|*[!0-9]*|0[0-9]*) return 1 ;; esac
	[ "${#1}" -le 2 ] && [ "$1" -le 61 ] || return 1
	case "$1" in 26|27|28|29|30|31|32|33|34|41|58|59) return 1 ;; esac
}

interface_make_draft()
{
	# CURRENT is trustworthy only when the manager and its current record agree.
	# A disabled interface starts from its saved settings, then DT defaults.
	awk -F '\t' 'BEGIN { OFS="\t" }
		$1 == "active" { active=$2 }
		$1 == "current_known" { known=$2 }
		$1 == "route" || $1 == "parameter" {
			value=(active==1 ? (known==1 ? $4 : "-") : ($5!="-" ? $5 : $3))
			print $1,$2,value,$6
		}' "$1" >"$2"
}

interface_draft_set()
{
	local file="$1" key="$2" value="$3"
	awk -F '\t' -v key="$key" -v value="$value" 'BEGIN { OFS="\t" }
		$2 == key { $3=value } { print }' "$file" >"${file}.new" &&
		mv "${file}.new" "$file"
}

interface_apply_draft()
{
	local name="$1" enabled="$2" file="$3" kind key value choices tab
	if [ "$enabled" = 0 ]; then
		s31-overlay remove "$name"
		return
	fi
	set -- "$name"
	tab="$(printf '\t')"
	while IFS="$tab" read -r kind key value choices; do
		case "$kind:$choices" in
			route:matrix-input|route:matrix-output|route:matrix-bidirectional)
				interface_valid_gpio "$value" || { warn "Choose a valid GPIO for $key."; return 1; }
				set -- "$@" "$key=$value" ;;
			parameter:*)
				case ",$choices," in *",$value,"*) ;; *) warn "Choose a valid value for $key."; return 1 ;; esac
				set -- "$@" "$key=$value" ;;
		esac
	done <"$file"
	s31-overlay apply "$@"
}

interface_values_match()
{
	awk -F '\t' -v column="$3" '
		NR==FNR { if ($1=="route" || $1=="parameter") reference[$2]=$column; next }
		{ if (!($2 in reference) || $3!=reference[$2]) different=1 }
		END { exit different }' "$1" "$2"
}

interface_storage_busy()
{
	local device path rest proc="${ESP32_CONFIG_PROC_DIR:-/proc}"
	case "$1" in sdmmc*) ;; *) return 1 ;; esac
	[ -r "$proc/mounts" ] && [ -r "$proc/swaps" ] || {
		warn 'Could not check active SDMMC storage. Leave the interface unchanged.'
		return 0
	}
	# Both slots use the same SDMMC controller. Refuse a controller change while
	# any of its media is mounted or used as swap, including another slot.
	while read -r device path rest; do
		case "$device" in
			/dev/mmcblk*) warn "$device is mounted at $path. Unmount it before changing SDMMC settings."; return 0 ;;
		esac
	done <"$proc/mounts"
	while read -r device rest; do
		case "$device" in
			/dev/mmcblk*) warn "$device is active swap. Disable it before changing SDMMC settings."; return 0 ;;
		esac
	done <"$proc/swaps"
	return 1
}

interface_commit_draft()
{
	local name="$1" enabled="$2" draft="$3" metadata="${3}.latest" active saved known runtime_same=0
	# Re-read at commit time: another configuration command may have changed the
	# interface while this page was open.
	s31-overlay describe "$name" >"$metadata" || return 1
	active="$(awk -F '\t' '$1=="active" {print $2}' "$metadata")"
	saved="$(awk -F '\t' '$1=="saved" {print $2}' "$metadata")"
	known="$(awk -F '\t' '$1=="current_known" {print $2}' "$metadata")"
	[ "$active" != unknown ] || { warn 'The current interface state is unavailable.'; return 1; }
	[ "$enabled:$active:$known" != 1:1:0 ] || {
		warn 'Current pin values are unavailable. Disable the interface before reconfiguring it.'
		return 1
	}
	if [ "$enabled:$active:$saved" = 0:0:0 ]; then return 0; fi
	if [ "$enabled:$active:$known" = 1:1:1 ] && interface_values_match "$metadata" "$draft" 4; then
		runtime_same=1
		if [ "$saved" = 1 ] && interface_values_match "$metadata" "$draft" 5; then return 0; fi
	fi
	# Removing a saved-but-inactive selection cannot disturb the controller.
	# The backend also compares normalized DTBOs, so saving already-active
	# values for startup does not unbind a mounted controller.
	if [ "$runtime_same" = 0 ] && [ "$enabled:$active" != 0:0 ] && interface_storage_busy "$name"; then return 1; fi
	interface_apply_draft "$name" "$enabled" "$draft"
}

interface_edit()
{
	local name="$1" directory metadata draft active saved known enabled title state choice
	local kind key value choices row selected label tab pins output saved_value
	directory="$(mktemp -d "$RUN_DIR/interface.XXXXXX")" || return 1
	metadata="$directory/metadata"; draft="$directory/draft"
	if ! s31-overlay describe "$name" >"$metadata" 2>"$directory/error"; then
		ui_error "$(cat "$directory/error")"
		rm -rf "$directory"
		return 1
	fi
	active="$(awk -F '\t' '$1=="active" {print $2}' "$metadata")"
	saved="$(awk -F '\t' '$1=="saved" {print $2}' "$metadata")"
	known="$(awk -F '\t' '$1=="current_known" {print $2}' "$metadata")"
	if [ "$active" = unknown ]; then
		ui_error 'The interface manager is unavailable. The current configuration could not be read.'
		rm -rf "$directory"
		return 1
	fi
	interface_make_draft "$metadata" "$draft" || { rm -rf "$directory"; return 1; }
	enabled="$active"
	[ "$saved" = 0 ] || enabled=1
	title="$(interface_label "$name")"
	pins="$(awk -F '\t' '$1=="fixed_gpio" {printf "%s%s",sep,$2; sep=", "}' "$metadata")"
	tab="$(printf '\t')"
	while :; do
		case "$active" in 1) state='Current: enabled' ;; *) state='Current: disabled' ;; esac
		case "$saved" in 1) state="$state; saved for startup: enabled" ;; *) state="$state; saved for startup: disabled" ;; esac
		if [ "$active" = 1 ] && [ "$known" != 1 ]; then
			state="$state\nCurrent pin values are unavailable. Disable this interface before choosing a new configuration."
		fi
		[ -z "$pins" ] || state="$state\nFixed wiring: GPIO $pins"
		state="$state\nEdit settings, then choose Save and apply. Back discards this draft."
		[ "$enabled" != 0 ] || state="$state\nSaving Disabled removes this interface's saved settings."
		case "$enabled" in 1) label=Enabled ;; *) label=Disabled ;; esac
		set -- enabled "Use interface: $label"
		while IFS="$tab" read -r kind key value choices; do
			[ "$enabled" = 1 ] && [ "$active:$known" != 1:0 ] || continue
			label="$(interface_field_label "$key")"
			saved_value="$(awk -F '\t' -v key="$key" '$2==key {print $5; exit}' "$metadata")"
			if [ "$saved" = 1 ] && [ "$saved_value" != "$value" ]; then
				label="$label: $value (saved: $saved_value)"
			else
				label="$label: $value"
			fi
			case "$kind:$choices" in
				route:matrix-input|route:matrix-output|route:matrix-bidirectional|parameter:*)
				set -- "$@" "$key" "$label" ;;
				route:*) set -- "$@" "$key" "$label (fixed)" ;;
			esac
		done <"$draft"
		choice="$(ui_dialog --stdout --title "$title" --cancel-label Back --menu "$state" 21 78 11 "$@" save 'Save and apply')" || break
		case "$choice" in
			enabled)
				selected="$(ui_dialog --stdout --title "$title" --cancel-label Back --default-item "$enabled" \
					--menu 'Use this interface' 12 64 2 1 Enabled 0 Disabled)" || continue
				enabled="$selected" ;;
			save)
				if [ "$enabled:$active:$known" = 1:1:0 ]; then
					ui_error 'Disable this interface first; its current pin values could not be read.'
					continue
				fi
				if ui_run_action "$title" 'Applying interface settings...' "$title settings applied." \
					interface_commit_draft "$name" "$enabled" "$draft"; then break; fi ;;
			*)
				row="$(awk -F '\t' -v key="$choice" '$2==key {print; exit}' "$draft")"
				[ -n "$row" ] || continue
				IFS="$tab" read -r kind key value choices <<ROW
$row
ROW
				label="$(interface_field_label "$key")"
				case "$kind:$choices" in
					route:matrix-input|route:matrix-output|route:matrix-bidirectional)
						while :; do
							selected="$(ui_dialog --stdout --title "$label" --cancel-label Back --inputbox \
								'GPIO number (0-61). Pins reserved for flash or fixed board functions cannot be used.' 10 72 "$value")" || { selected=''; break; }
							interface_valid_gpio "$selected" && break
							value="$selected"
							ui_error 'Choose GPIO 0-61, excluding 26-34, 41, 58 and 59. Enter decimal without a leading zero.'
						done
						[ -n "$selected" ] || continue ;;
					parameter:*)
						set --
						for output in $(printf '%s' "$choices" | tr ',' ' '); do
							case "$key:$output" in
								clock-frequency:*) label="$output Hz" ;;
								bus-width:*) label="$output data bits" ;;
								*) label="$output" ;;
							esac
							set -- "$@" "$output" "$label"
						done
						selected="$(ui_dialog --stdout --title "$(interface_field_label "$key")" --cancel-label Back \
							--default-item "$value" --menu 'Choose a value' 15 68 6 "$@")" || continue ;;
					*) ui_error 'This signal uses fixed wiring for this interface.'; continue ;;
				esac
				interface_draft_set "$draft" "$key" "$selected" || ui_error 'Could not update the draft.' ;;
		esac
	done
	rm -rf "$directory"
}

interfaces_menu()
{
	local profiles current choice name label status query_status
	profiles="$(mktemp "$RUN_DIR/interfaces.XXXXXX")" || return 1
	while :; do
		s31-overlay list >"$profiles" 2>/dev/null || :
		current="$(s31-overlay status 2>/dev/null)"; query_status=$?
		current="$(printf '%s\n' "$current" | sed -n 's/^active: \([^ ]*\).*/\1/p')"
		set -- gpio 'GPIO - input, output and application control' usb 'USB - host, serial or network'
		while IFS= read -r name; do
			interface_available "$name" || continue
			label="$(interface_label "$name")"; status=Disabled
			if [ "$query_status" -ne 0 ]; then status=Unavailable
			elif printf '%s\n' "$current" | grep -qx "$name"; then status=Enabled; fi
			set -- "$@" "$name" "$label - $status"
		done <"$profiles"
		choice="$(ui_dialog --stdout --title Interfaces --cancel-label Back --menu \
			"Choose an interface to edit its settings. Only interfaces supported by this image are shown.${UI_NOTICE:+\n$UI_NOTICE}" 21 78 13 "$@")" || break
		UI_NOTICE=''
		case "$choice" in gpio) gpio_menu ;; usb) usb_menu ;; *) interface_available "$choice" && interface_edit "$choice" ;; esac
	done
	rm -f "$profiles"
}

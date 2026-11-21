# Explicit USB role actions; persistent settings use /etc/default and ifupdown.

usb_proc() { printf '%s/%s\n' "${ESP32_CONFIG_PROC_DIR:-/proc}" "$1"; }
usb_sys() { printf '%s/%s\n' "${ESP32_CONFIG_SYS_DIR:-/sys}" "$1"; }
usb_error() { printf '%s\n' "$*" >&2; return 1; }
usb_mount_present() { awk -v path="$1" '$2==path {found=1} END {exit !found}' "$(usb_proc mounts)"; }

usb_gadget_root()
{
	printf '%s/s31\n' "${ESP32_CONFIG_GADGET_DIR:-/sys/kernel/config/usb_gadget}"
}

usb_option()
{
	local file="$1" MODE=host SERIAL=esp32-s31
	[ ! -r "$file" ] || . "$file"
	case "$2" in MODE) printf '%s\n' "$MODE" ;; SERIAL) printf '%s\n' "$SERIAL" ;; esac
}

usb_validate_config()
{
	local mode serial
	mode="$(usb_option "$1" MODE)"; serial="$(usb_option "$1" SERIAL)"
	case "$mode" in host|serial|network) ;; *) return 1 ;; esac
	case "$serial" in ''|*[!A-Za-z0-9-]*) return 1 ;; esac
	[ "${#serial}" -le 32 ]
}

usb_mode_available()
{
	local mode="$1"
	[ "$mode" = host ] && return 0
	interface_feature usb_gadget && have s31-overlay && have mount || return 1
	case "$mode" in
		serial) interface_feature usb_acm ;;
		network) interface_feature usb_ecm && have ip ;;
		*) return 1 ;;
	esac
}

usb_device_on_host()
{
	local name path
	name="${1##*/}"
	path="$(readlink -f "$(usb_sys "class/block/$name")" 2>/dev/null)"
	case "$path" in */usb[0-9]*/*) return 0 ;; esac
	# sd* on this board is USB storage, including early/incomplete sysfs.
	case "$name" in sd[a-z]|sd[a-z][0-9]*) return 0 ;; esac
	return 1
}

usb_host_busy()
{
	local device path rest failed=1
	while read -r device path rest; do
		usb_device_on_host "$device" || continue
		printf 'USB storage is mounted at %s (%s). Unmount it before changing USB mode.\n' "$path" "$device" >&2
		failed=0
	done <"$(usb_proc mounts)"
	while read -r device rest; do
		usb_device_on_host "$device" || continue
		printf 'USB swap is active on %s. Disable it before changing USB mode.\n' "$device" >&2
		failed=0
	done <"$(usb_proc swaps)"
	return "$failed"
}

usb_overlay_active()
{
	s31-overlay status 2>/dev/null | grep -q '^active: usb-device '
}

usb_stop()
{
	local gadget="$(usb_gadget_root)" network
	[ -d "$gadget" ] || { rm -f "$RUN_DIR/usb.mode" "$RUN_DIR/usb.network" "$RUN_DIR/usb.serial"; return 0; }
	# Other ConfigFS gadgets remain intact.
	if [ -r "$gadget/UDC" ] && [ -n "$(cat "$gadget/UDC")" ]; then
		printf '\n' >"$gadget/UDC" || return 1
	fi
	if [ -r "$RUN_DIR/usb.network" ]; then
		network="$(cat "$RUN_DIR/usb.network")"
		case "$network" in ''|*[!A-Za-z0-9_.-]*) return 1 ;; esac
		if [ -d "$(usb_sys "class/net/$network")" ]; then
			ifdown -i "${NETWORK_CONF%/*}/usb" "$network" || return 1
		fi
	fi
	[ ! -L "$gadget/configs/c.1/acm.usb0" ] || rm "$gadget/configs/c.1/acm.usb0" || return 1
	[ ! -L "$gadget/configs/c.1/ecm.usb0" ] || rm "$gadget/configs/c.1/ecm.usb0" || return 1
	for network in "$gadget/configs/c.1/strings/0x409" "$gadget/configs/c.1" \
		"$gadget/functions/acm.usb0" "$gadget/functions/ecm.usb0" "$gadget/strings/0x409"; do
		[ ! -d "$network" ] || rmdir "$network" || return 1
	done
	rmdir "$gadget" || return 1
	rm -f "$RUN_DIR/usb.mode" "$RUN_DIR/usb.network" "$RUN_DIR/usb.serial"
}


usb_configfs_prepare()
{
	local parent="${ESP32_CONFIG_GADGET_DIR:-/sys/kernel/config/usb_gadget}"
	[ -d "$parent" ] && return 0
	grep -Eq '(^|[[:space:]])configfs$' "$(usb_proc filesystems)" || {
		usb_error 'ConfigFS is unavailable in this image.'; return 1;
	}
	usb_mount_present /sys/kernel/config || mount -t configfs none /sys/kernel/config || return 1
	[ -d "$parent" ] || { usb_error 'USB gadget ConfigFS is unavailable.'; return 1; }
}

usb_create_gadget()
{
	local file="$1" mode gadget function udc='' candidate serial tailmac network count port temporary result
	mode="$(usb_option "$file" MODE)"
	gadget="$(usb_gadget_root)"
	serial="$(usb_option "$file" SERIAL)"
	usb_configfs_prepare || return 1
	[ ! -d "$gadget" ] || { usb_error 'The managed USB gadget already exists.'; return 1; }
	mkdir "$gadget" || return 1
	# Generic Linux development gadget IDs.
	printf '0x1d6b\n' >"$gadget/idVendor" || return 1
	printf '0x0104\n' >"$gadget/idProduct" || return 1
	printf '0x0200\n' >"$gadget/bcdUSB" || return 1
	mkdir -p "$gadget/strings/0x409" "$gadget/configs/c.1/strings/0x409" || return 1
	printf '%s\n' "$serial" >"$gadget/strings/0x409/serialnumber" || return 1
	printf 'ESP32-S31\n' >"$gadget/strings/0x409/manufacturer" || return 1
	printf 'ESP32-S31 Linux %s\n' "$mode" >"$gadget/strings/0x409/product" || return 1
	printf '%s\n' "$mode" >"$gadget/configs/c.1/strings/0x409/configuration" || return 1
	printf '250\n' >"$gadget/configs/c.1/MaxPower" || return 1
	case "$mode" in serial) function=acm.usb0 ;; network) function=ecm.usb0 ;; *) return 2 ;; esac
	mkdir "$gadget/functions/$function" || return 1
	if [ "$mode" = network ]; then
		# Derive stable locally administered MACs from the saved board identity.
		tailmac="$(printf '%s' "$serial" | cksum | awk '{n=$1; printf "%02x:%02x:%02x:%02x", int(n/16777216)%256,int(n/65536)%256,int(n/256)%256,n%256}')"
		printf '02:31:%s\n' "$tailmac" >"$gadget/functions/$function/dev_addr" || return 1
		printf '06:31:%s\n' "$tailmac" >"$gadget/functions/$function/host_addr" || return 1
	fi
	ln -s "$gadget/functions/$function" "$gadget/configs/c.1/$function" || return 1
	# s31-overlay re-probes DWC2 so the new dr_mode takes effect.
	usb_overlay_active || s31-overlay apply usb-device --volatile || return 1
	count=0
	while [ "$count" -lt 30 ]; do
		for candidate in "$(usb_sys class/udc)"/*; do
			[ -d "$candidate" ] || continue
			[ -z "$udc" ] || { usb_error 'More than one USB device controller is present.'; return 1; }
			udc="${candidate##*/}"
		done
		[ -z "$udc" ] || break
		sleep 0.1; count=$((count + 1))
	done
	[ -n "$udc" ] || { usb_error 'No USB device controller became available.'; return 1; }
	printf '%s\n' "$udc" >"$gadget/UDC" || return 1
	if [ "$mode" = network ]; then
		network="$(cat "$gadget/functions/$function/ifname" 2>/dev/null)"
		case "$network" in ''|*[!A-Za-z0-9_.-]*) usb_error 'USB network interface was not created.'; return 1 ;; esac
		# ConfigFS selects the interface name. Preserve all user-supplied
		# ifupdown options while updating that binding.
		temporary="$(mktemp "$RUN_DIR/usb-interface.XXXXXX")" || return 1
		sed "s/^iface [^ ]* inet /iface $network inet /" "${NETWORK_CONF%/*}/usb" >"$temporary" &&
			save_file "${NETWORK_CONF%/*}/usb" 0644 "$temporary"
		result=$?; rm -f "$temporary"; [ "$result" -eq 0 ] || return "$result"
		ifup -i "${NETWORK_CONF%/*}/usb" "$network" || return 1
		printf '%s\n' "$network" >"$RUN_DIR/usb.network" || return 1
	else
		# The fixed Serial/JTAG driver can already own ttyGS0.
		port="$(cat "$gadget/functions/$function/port_num" 2>/dev/null)"
		case "$port" in ''|*[!0-9]*) usb_error 'USB serial port number is unavailable.'; return 1 ;; esac
		port="ttyGS$port"
		printf '%s\n' "$port" >"$RUN_DIR/usb.serial" || return 1
	fi
	printf '%s\n' "$mode" >"$RUN_DIR/usb.mode"
}

usb_apply_config()
{
	local file="$1" mode
	usb_validate_config "$file" || { usb_error 'Invalid USB settings.'; return 1; }
	mode="$(usb_option "$file" MODE)"
	usb_mode_available "$mode" || { usb_error "USB $mode support is unavailable in this image."; return 1; }
	if [ "$mode" != host ]; then
		usb_host_busy && return 1
	fi
	mkdir -p "$RUN_DIR" || return 1
	usb_stop || return 1
	if [ "$mode" = host ]; then
		if usb_overlay_active; then s31-overlay remove usb-device --volatile || return 1; fi
		printf 'host\n' >"$RUN_DIR/usb.mode"
		return $?
	fi
	if ! usb_create_gadget "$file"; then
		usb_stop || usb_error 'USB setup cleanup failed.'
		if usb_overlay_active; then s31-overlay remove usb-device --volatile || usb_error 'Could not restore USB host mode.'; fi
		return 1
	fi
	return 0
}

usb_apply()
{
	# Unconfigured machines keep their existing overlay/host behavior.
	[ -f "$USB_CONF" ] || return 0
	usb_apply_config "$USB_CONF"
}

usb_configure()
{
	local mode="$1" serial
	case "$mode" in host|serial|network) ;; *) return 2 ;; esac
	serial="$(usb_option "$USB_CONF" SERIAL)"
	case "$serial" in ''|*[!A-Za-z0-9-]*) usb_error 'Set a valid SERIAL in /etc/default/s31-usb.'; return 1 ;; esac
	write_lines "$USB_CONF" 0644 "MODE=$mode" "SERIAL=$serial" || return 1
	printf 'USB settings saved. Use usb apply to switch roles.\n'
}

usb_status()
{
	local mode
	mode="$(usb_option "$USB_CONF" MODE)"
	printf 'Saved mode: %s\n' "$mode"
	printf 'Current mode: %s\n' "$(cat "$RUN_DIR/usb.mode" 2>/dev/null || printf 'not managed')"
	if [ "$mode" = network ]; then
		cat "${NETWORK_CONF%/*}/usb"
		printf 'Configure the computer with another address on the same subnet. No DHCP server is started.\n'
	elif [ "$mode" = serial ]; then
		printf 'Application serial port: %s\n' \
			"$(if [ -s "$RUN_DIR/usb.serial" ]; then printf '/dev/%s' "$(cat "$RUN_DIR/usb.serial")"; else printf 'not active'; fi)"
		printf 'The main console uses the separate fixed USB Serial/JTAG port.\n'
	fi
	if ! usb_mode_available serial && ! usb_mode_available network; then
		printf 'This image supports USB host mode only.\n'
	fi
}

usb_menu()
{
	local mode current
	while :; do
		current="$(usb_option "$USB_CONF" MODE)"
		set -- host 'Save host mode'
		usb_mode_available serial && set -- "$@" serial 'Save serial mode'
		usb_mode_available network && set -- "$@" network 'Save network mode (address in interfaces.d/usb)'
		mode="$(ui_dialog --stdout --title USB --cancel-label Back --menu \
			"Saved role: $current. Saving does not switch roles.${UI_NOTICE:+\n$UI_NOTICE}" 16 76 4 \
			"$@" apply 'Apply saved USB settings')" || return 0
		if [ "$mode" = apply ]; then
			ui_dialog --title USB --yesno 'Switching roles disconnects the OTG port. Continue?' 8 70 || continue
			ui_run_action USB 'Switching USB role...' '' usb_apply
		else
			ui_run_action USB 'Saving settings...' 'Saved. Use Apply to switch roles.' usb_configure "$mode"
		fi
	done
}

usb_cli()
{
	case "${1:-status}" in
		status) [ "$#" -le 1 ] || return 2; usb_status ;;
		configure) [ "$#" -eq 2 ] || return 2; usb_configure "$2" ;;
		apply) [ "$#" -eq 1 ] || return 2; usb_apply ;;
		stop) [ "$#" -eq 1 ] || return 2; usb_stop ;;
		*) usb_error 'Usage: esp32-config usb status|configure MODE|apply|stop'; return 2 ;;
	esac
}

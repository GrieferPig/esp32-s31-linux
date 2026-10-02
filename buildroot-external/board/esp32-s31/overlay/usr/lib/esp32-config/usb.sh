# USB role and ConfigFS gadget policy. Source storage.sh first.

usb_gadget_root()
{
	printf '%s/s31\n' "${ESP32_CONFIG_GADGET_DIR:-/sys/kernel/config/usb_gadget}"
}

usb_valid_address()
{
	local address="$1" host prefix component saved_ifs
	case "$address" in */*) ;; *) return 1 ;; esac
	host="${address%/*}"; prefix="${address##*/}"
	case "$host" in .*|*.|*..*|*[!0-9.]*) return 1 ;; esac
	case "$prefix" in ''|*[!0-9]*) return 1 ;; esac
	[ "${#prefix}" -le 2 ] && [ "$prefix" -ge 1 ] && [ "$prefix" -le 30 ] || return 1
	saved_ifs="$IFS"; IFS=.; set -- $host; IFS="$saved_ifs"
	[ "$#" -eq 4 ] || return 1
	for component in "$@"; do
		case "$component" in ''|*[!0-9]*|0[0-9]*) return 1 ;; esac
		[ "${#component}" -le 3 ] && [ "$component" -le 255 ] || return 1
	done
	[ "$1" -gt 0 ] && [ "$1" -lt 224 ] && [ "$1" -ne 127 ] || return 1
	awk -v ip="$host" -v p="$prefix" 'BEGIN {
		split(ip,a,"."); n=((a[1]*256+a[2])*256+a[3])*256+a[4];
		size=2^(32-p); host=n%size; exit(host==0 || host==size-1)
	}'
}

usb_validate_config()
{
	local file="$1" mode login serial
	storage_validate_keys "$file" 'mode serial_login address serial' || return 1
	mode="$(conf_get "$file" mode host)"
	login="$(conf_get "$file" serial_login 0)"
	serial="$(conf_get "$file" serial esp32-s31)"
	case "$mode" in host|serial|network) ;; *) return 1 ;; esac
	case "$login" in 0|1) ;; *) return 1 ;; esac
	case "$serial" in ''|*[!A-Za-z0-9-]*) return 1 ;; esac
	[ "${#serial}" -le 32 ] || return 1
	usb_valid_address "$(conf_get "$file" address 192.168.7.2/24)"
}

usb_mode_available()
{
	local mode="$1"
	[ "$mode" = host ] && return 0
	storage_feature usb_gadget && have s31-overlay && have mount || return 1
	case "$mode" in
		serial) storage_feature usb_acm ;;
		network) storage_feature usb_ecm && have ip ;;
		*) return 1 ;;
	esac
}

usb_device_on_host()
{
	local name path
	name="${1##*/}"
	path="$(readlink -f "$(storage_sys "class/block/$name")" 2>/dev/null)"
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
	done <"$(storage_proc mounts)"
	while read -r device rest; do
		usb_device_on_host "$device" || continue
		printf 'USB swap is active on %s. Disable it before changing USB mode.\n' "$device" >&2
		failed=0
	done <"$(storage_proc swaps)"
	return "$failed"
}

usb_overlay_active()
{
	s31-overlay status 2>/dev/null | grep -q '^active: usb-device '
}

usb_console_stop()
{
	local pid cmdline count=0 file="$RUN_DIR/usb-console.pid"
	[ -r "$file" ] || return 0
	pid="$(cat "$file")"
	case "$pid" in ''|*[!0-9]*) storage_error 'Invalid USB console PID file.'; return 1 ;; esac
	if [ -d "$(storage_proc "$pid")" ]; then
		cmdline="$(tr '\000' ' ' <"$(storage_proc "$pid/cmdline")")"
		case "$cmdline" in *'/esp32-config/usb-console.sh'*) ;; *) storage_error 'USB console process identity changed.'; return 1 ;; esac
		kill -TERM "$pid" || return 1
		while kill -0 "$pid" 2>/dev/null && [ "$count" -lt 20 ]; do
			sleep 0.1; count=$((count + 1))
		done
		kill -0 "$pid" 2>/dev/null && { storage_error 'USB console did not stop.'; return 1; }
	fi
	rm -f "$file"
}

usb_allow_console()
{
	local port="$1" file="${ESP32_CONFIG_SECURETTY_FILE:-/etc/securetty}" temporary
	case "$port" in ttyGS[0-9]*) ;; *) return 1 ;; esac
	case "${port#ttyGS}" in ''|*[!0-9]*) return 1 ;; esac
	# A missing securetty file has no terminal allowlist. Do not create one and
	# inadvertently change existing UART login policy.
	[ -e "$file" ] || return 0
	[ -f "$file" ] && [ ! -L "$file" ] || {
		storage_error 'Cannot update the terminal login allowlist.'; return 1;
	}
	grep -qx "$port" "$file" && return 0
	temporary="$(mktemp "${file}.XXXXXX")" || return 1
	if ! { cat "$file" >"$temporary" && printf '\n%s\n' "$port" >>"$temporary" && chmod 0600 "$temporary"; }; then
		rm -f "$temporary"; return 1
	fi
	mv "$temporary" "$file"
}

usb_stop()
{
	local gadget="$(usb_gadget_root)" network
	usb_console_stop || return 1
	[ -d "$gadget" ] || { rm -f "$RUN_DIR/usb.mode" "$RUN_DIR/usb.network" "$RUN_DIR/usb.serial" "$RUN_DIR/usb.applied"; return 0; }
	# Other ConfigFS gadgets remain intact.
	if [ -r "$gadget/UDC" ] && [ -n "$(cat "$gadget/UDC")" ]; then
		printf '\n' >"$gadget/UDC" || return 1
	fi
	if [ -r "$RUN_DIR/usb.network" ]; then
		network="$(cat "$RUN_DIR/usb.network")"
		case "$network" in ''|*[!A-Za-z0-9_.-]*) return 1 ;; esac
		if [ -d "$(storage_sys "class/net/$network")" ]; then
			ip link set dev "$network" down || return 1
		fi
	fi
	[ ! -L "$gadget/configs/c.1/acm.usb0" ] || rm "$gadget/configs/c.1/acm.usb0" || return 1
	[ ! -L "$gadget/configs/c.1/ecm.usb0" ] || rm "$gadget/configs/c.1/ecm.usb0" || return 1
	for network in "$gadget/configs/c.1/strings/0x409" "$gadget/configs/c.1" \
		"$gadget/functions/acm.usb0" "$gadget/functions/ecm.usb0" "$gadget/strings/0x409"; do
		[ ! -d "$network" ] || rmdir "$network" || return 1
	done
	rmdir "$gadget" || return 1
	rm -f "$RUN_DIR/usb.mode" "$RUN_DIR/usb.network" "$RUN_DIR/usb.serial" "$RUN_DIR/usb.applied"
}

usb_is_current()
{
	local mode gadget
	cmp -s "$1" "$RUN_DIR/usb.applied" || return 1
	mode="$(conf_get "$1" mode host)"
	[ "$(cat "$RUN_DIR/usb.mode" 2>/dev/null)" = "$mode" ] || return 1
	if [ "$mode" = host ]; then
		! usb_overlay_active
	else
		gadget="$(usb_gadget_root)"
		[ -r "$gadget/UDC" ] && [ -n "$(cat "$gadget/UDC")" ]
	fi
}

usb_configfs_prepare()
{
	local parent="${ESP32_CONFIG_GADGET_DIR:-/sys/kernel/config/usb_gadget}"
	[ -d "$parent" ] && return 0
	grep -Eq '(^|[[:space:]])configfs$' "$(storage_proc filesystems)" || {
		storage_error 'ConfigFS is unavailable in this image.'; return 1;
	}
	storage_mount_present /sys/kernel/config || mount -t configfs none /sys/kernel/config || return 1
	[ -d "$parent" ] || { storage_error 'USB gadget ConfigFS is unavailable.'; return 1; }
}

usb_create_gadget()
{
	local file="$1" mode gadget function udc='' candidate serial tailmac network count port
	mode="$(conf_get "$file" mode host)"
	gadget="$(usb_gadget_root)"
	serial="$(conf_get "$file" serial esp32-s31)"
	usb_configfs_prepare || return 1
	[ ! -d "$gadget" ] || { storage_error 'The managed USB gadget already exists.'; return 1; }
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
		for candidate in "$(storage_sys class/udc)"/*; do
			[ -d "$candidate" ] || continue
			[ -z "$udc" ] || { storage_error 'More than one USB device controller is present.'; return 1; }
			udc="${candidate##*/}"
		done
		[ -z "$udc" ] || break
		sleep 0.1; count=$((count + 1))
	done
	[ -n "$udc" ] || { storage_error 'No USB device controller became available.'; return 1; }
	printf '%s\n' "$udc" >"$gadget/UDC" || return 1
	if [ "$mode" = network ]; then
		network="$(cat "$gadget/functions/$function/ifname" 2>/dev/null)"
		case "$network" in ''|*[!A-Za-z0-9_.-]*) storage_error 'USB network interface was not created.'; return 1 ;; esac
		ip link set dev "$network" up || return 1
		ip addr add "$(conf_get "$file" address 192.168.7.2/24)" dev "$network" || return 1
		printf '%s\n' "$network" >"$RUN_DIR/usb.network" || return 1
	else
		# The fixed Serial/JTAG driver can already own ttyGS0.
		port="$(cat "$gadget/functions/$function/port_num" 2>/dev/null)"
		case "$port" in ''|*[!0-9]*) storage_error 'USB serial port number is unavailable.'; return 1 ;; esac
		port="ttyGS$port"
		printf '%s\n' "$port" >"$RUN_DIR/usb.serial" || return 1
		if [ "$(conf_get "$file" serial_login 0)" = 1 ]; then
			count=0
			while [ ! -c "/dev/$port" ] && [ "$count" -lt 20 ]; do sleep 0.1; count=$((count + 1)); done
			[ -c "/dev/$port" ] || { storage_error 'USB serial port did not appear.'; return 1; }
			usb_allow_console "$port" || { storage_error 'USB login could not be enabled for this terminal.'; return 1; }
			/bin/sh "${ESP32_CONFIG_LIB_DIR:-/usr/lib/esp32-config}/usb-console.sh" "$port" </dev/null >"$RUN_DIR/usb-console.log" 2>&1 &
			printf '%s\n' "$!" >"$RUN_DIR/usb-console.pid" || return 1
		fi
	fi
	printf '%s\n' "$mode" >"$RUN_DIR/usb.mode"
}

usb_apply_config()
{
	local file="$1" mode
	usb_validate_config "$file" || { storage_error 'Invalid USB settings.'; return 1; }
	mode="$(conf_get "$file" mode host)"
	usb_mode_available "$mode" || { storage_error "USB $mode support is unavailable in this image."; return 1; }
	usb_is_current "$file" && return 0
	if [ "$mode" != host ]; then
		usb_host_busy && return 1
		if [ "$mode" = serial ] && [ "$(conf_get "$file" serial_login 0)" = 1 ]; then
			[ -x /sbin/getty ] || { storage_error 'Serial login is unavailable in this image.'; return 1; }
		fi
	fi
	mkdir -p "$RUN_DIR" || return 1
	usb_stop || return 1
	if [ "$mode" = host ]; then
		if usb_overlay_active; then s31-overlay remove usb-device --volatile || return 1; fi
		printf 'host\n' >"$RUN_DIR/usb.mode" && cp "$file" "$RUN_DIR/usb.applied"
		return $?
	fi
	if ! usb_create_gadget "$file"; then
		usb_stop || storage_error 'USB setup cleanup failed.'
		if usb_overlay_active; then s31-overlay remove usb-device --volatile || storage_error 'Could not restore USB host mode.'; fi
		return 1
	fi
	cp "$file" "$RUN_DIR/usb.applied"
}

usb_apply()
{
	# Unconfigured machines keep their existing overlay/host behavior.
	[ -f "$CONF_DIR/usb.conf" ] || return 0
	usb_apply_config "$CONF_DIR/usb.conf"
}

usb_configure()
{
	local mode="$1" login="${2:-0}" address="${3:-192.168.7.2/24}" serial temporary
	mkdir -p "$CONF_DIR" "$RUN_DIR" || return 1
	serial="$(conf_get "$CONF_DIR/usb.conf" serial '')"
	if [ -z "$serial" ]; then
		serial="$(tr -d '-' <"$(storage_proc sys/kernel/random/uuid)" 2>/dev/null | cut -c1-24)"
		[ -n "$serial" ] || { storage_error 'Could not generate a USB device identity.'; return 1; }
	fi
	temporary="$(mktemp "$CONF_DIR/usb.conf.XXXXXX")" || return 1
	printf 'mode=%s\nserial_login=%s\naddress=%s\nserial=%s\n' "$mode" "$login" "$address" "$serial" >"$temporary"
	chmod 0600 "$temporary"
	if [ -f "$CONF_DIR/usb.conf" ] && cmp -s "$temporary" "$CONF_DIR/usb.conf" &&
	   usb_is_current "$temporary"; then rm -f "$temporary"; return 0; fi
	if ! usb_apply_config "$temporary"; then
		rm -f "$temporary"
		[ ! -f "$CONF_DIR/usb.conf" ] || usb_apply || storage_error 'Previous USB settings could not be restored.'
		return 1
	fi
	mv "$temporary" "$CONF_DIR/usb.conf" || { storage_error 'USB mode changed, but saving the configuration failed.'; return 1; }
}

usb_status()
{
	local mode
	mode="$(conf_get "$CONF_DIR/usb.conf" mode host)"
	printf 'Saved mode: %s\n' "$mode"
	printf 'Current mode: %s\n' "$(cat "$RUN_DIR/usb.mode" 2>/dev/null || printf 'not managed')"
	if [ "$mode" = network ]; then
		printf 'Board address: %s\n' "$(conf_get "$CONF_DIR/usb.conf" address 192.168.7.2/24)"
		printf 'Configure the computer with another address on the same subnet. No DHCP server is started.\n'
	elif [ "$mode" = serial ]; then
		printf 'Board serial port: %s\nLogin console: %s\n' \
			"$(if [ -s "$RUN_DIR/usb.serial" ]; then printf '/dev/%s' "$(cat "$RUN_DIR/usb.serial")"; else printf 'not active'; fi)" \
			"$(storage_enabled_label "$(conf_get "$CONF_DIR/usb.conf" serial_login 0)")"
	fi
	if ! usb_mode_available serial && ! usb_mode_available network; then
		printf 'This image supports USB host mode only.\n'
	fi
}

usb_menu()
{
	local mode login address current
	while :; do
		current="$(conf_get "$CONF_DIR/usb.conf" mode host)"
		set -- host 'Host: connect USB devices'
		usb_mode_available serial && set -- "$@" serial 'USB serial connection'
		usb_mode_available network && set -- "$@" network 'USB network connection (ECM)'
		if [ "$#" -eq 2 ]; then ui_show_command USB usb_status; return 0; fi
		mode="$(ui_dialog --stdout --title USB --cancel-label Back --default-item "$current" --menu "Current setting: $current${UI_NOTICE:+\n$UI_NOTICE}" 15 74 4 "$@")" || return 0
		UI_NOTICE=''
		login="$(conf_get "$CONF_DIR/usb.conf" serial_login 0)"
		address="$(conf_get "$CONF_DIR/usb.conf" address 192.168.7.2/24)"
		if [ "$mode" = serial ]; then
			set -- 0 'Application serial port'
			[ ! -x /sbin/getty ] || set -- "$@" 1 'Linux login console'
			login="$(ui_dialog --stdout --title 'USB serial' --cancel-label Back --default-item "$login" --menu 'Use the serial connection for' 13 70 2 "$@")" || continue
		elif [ "$mode" = network ]; then
			while :; do
				address="$(ui_dialog --stdout --title 'USB network' --cancel-label Back --inputbox 'Board IPv4 address/prefix. Set a different address on the same subnet on the computer (for example 192.168.7.1/24).' 11 74 "$address")" || { address=''; break; }
				usb_valid_address "$address" && break
				ui_error 'Enter a valid host IPv4 address and prefix, for example 192.168.7.2/24.'
			done
			[ -n "$address" ] || continue
		fi
		if [ "$current" != host ]; then
			ui_dialog --title 'USB connection' --yesno 'Applying USB settings disconnects the current USB connection. Continue?' 8 70 || continue
		fi
		ui_run_action USB 'Applying USB settings...' 'USB settings applied.' usb_configure "$mode" "$login" "$address" || true
	done
}

usb_cli()
{
	case "${1:-status}" in
		status) [ "$#" -le 1 ] || return 2; usb_status ;;
		configure) [ "$#" -ge 2 ] && [ "$#" -le 4 ] || return 2; shift; usb_configure "$@" ;;
		apply) [ "$#" -eq 1 ] || return 2; usb_apply ;;
		stop) [ "$#" -eq 1 ] || return 2; usb_stop ;;
		*) storage_error 'Usage: esp32-config usb status|configure MODE [SERIAL_LOGIN [IP/PREFIX]]|apply|stop'; return 2 ;;
	esac
}

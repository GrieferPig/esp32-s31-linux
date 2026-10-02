# Memory and removable storage policy. This file only defines functions.

storage_proc()
{
	printf '%s/%s\n' "${ESP32_CONFIG_PROC_DIR:-/proc}" "$1"
}

storage_sys()
{
	printf '%s/%s\n' "${ESP32_CONFIG_SYS_DIR:-/sys}" "$1"
}

storage_error()
{
	printf '%s\n' "$*" >&2
	return 1
}

storage_feature()
{
	[ "$(conf_get "${ESP32_CONFIG_FEATURES_FILE:-/usr/share/esp32-config/kernel-features}" "$1" 0)" = 1 ]
}

storage_validate_keys()
{
	# Fixed schemas make imports and hand-edited files unambiguous.
	[ -f "$1" ] || return 0
	LC_ALL=C awk -v keys="$2" '
		BEGIN { n=split(keys,list," "); for(i=1;i<=n;i++) allowed[list[i]]=1 }
		/^[ \t]*#/ || /^[ \t]*$/ { next }
		{ p=index($0,"="); k=substr($0,1,p-1)
		  if (!p || !allowed[k] || seen[k]++ || length($0)>256) exit 1 }
	' "$1"
}

storage_enabled_label()
{
	if [ "$1" = 1 ]; then printf 'Enabled'; else printf 'Disabled'; fi
}

storage_valid_device()
{
	case "$1" in
		auto) return 0 ;;
		UUID=*) case "${1#UUID=}" in ''|*[!A-Fa-f0-9-]*) return 1 ;; esac ;;
		/dev/*) case "${1#/dev/}" in ''|*[!A-Za-z0-9_.-]*) return 1 ;; esac ;;
		*) return 1 ;;
	esac
}

storage_devices()
{
	local device
	for device in "${ESP32_CONFIG_DEV_DIR:-/dev}"/sd[a-z] \
		"${ESP32_CONFIG_DEV_DIR:-/dev}"/sd[a-z][0-9]* \
		"${ESP32_CONFIG_DEV_DIR:-/dev}"/mmcblk[0-9] \
		"${ESP32_CONFIG_DEV_DIR:-/dev}"/mmcblk[0-9]p[0-9]*; do
		[ -b "$device" ] && printf '%s\n' "$device"
	done
	return 0
}

storage_blkid_value()
{
	local output
	have blkid || return 1
	output="$(blkid "$1" 2>/dev/null)" || return 1
	printf '%s\n' "$output" | sed -n "s/.* $2=\"\([^\"]*\)\".*/\1/p"
}

storage_device_id()
{
	local uuid
	uuid="$(storage_blkid_value "$1" UUID)"
	if [ -n "$uuid" ] && storage_valid_device "UUID=$uuid"; then
		printf 'UUID=%s\n' "$uuid"
	else
		printf '%s\n' "$1"
	fi
}

storage_resolve_device()
{
	local wanted="$1" device found=''
	storage_valid_device "$wanted" || return 1
	case "$wanted" in
		UUID=*)
			for device in $(storage_devices); do
				[ "$(storage_blkid_value "$device" UUID)" = "${wanted#UUID=}" ] || continue
				# Duplicate UUIDs must not select a disk arbitrarily.
				[ -z "$found" ] || return 1
				found="$device"
			done
			[ -n "$found" ] && printf '%s\n' "$found" ;;
		/dev/*) [ -b "$wanted" ] && readlink -f "$wanted" ;;
		*) return 1 ;;
	esac
}

storage_in_swaps()
{
	awk -v dev="$1" '$1 == dev { found=1 } END { exit !found }' "$(storage_proc swaps)" 2>/dev/null
}

storage_swap_signature()
{
	local signature
	[ -b "$1" ] || return 1
	# S31 uses 4 KiB pages. No write is needed to identify an existing swap.
	signature="$(dd if="$1" bs=1 skip=4086 count=10 2>/dev/null)"
	[ "$signature" = SWAPSPACE2 ] || [ "$signature" = SWAP-SPACE ]
}

storage_legacy_value()
{
	local value
	# Read the old optional assignment file as data, never as shell code.
	value="$(sed -n "s/^$1=//p" "${ESP32_CONFIG_LEGACY_SWAP_FILE:-/etc/default/s31-swap}" 2>/dev/null | tail -n 1)"
	case "$value" in \"*\") value="${value#\"}"; value="${value%\"}" ;; \'*\') value="${value#\'}"; value="${value%\'}" ;; esac
	printf '%s\n' "${value:-$2}"
}

storage_swap_values()
{
	local file="${1:-$CONF_DIR/swap.conf}"
	STORAGE_SWAP_ENABLED="$(conf_get "$file" enabled 1)"
	STORAGE_SWAP_DEVICE="$(conf_get "$file" device "$(storage_legacy_value USB_SWAP_DEVICE "${S31_USB_SWAP_DEVICE:-auto}")")"
	STORAGE_ZRAM_FALLBACK="$(conf_get "$file" zram_fallback "$(storage_legacy_value ZRAM_FALLBACK "${S31_ZRAM_FALLBACK:-0}")")"
	STORAGE_ZRAM_SIZE="$(conf_get "$file" zram_size_kib "$(storage_legacy_value ZRAM_SIZE_KIB "${S31_ZRAM_SIZE_KIB:-8192}")")"
}

storage_validate_swap_config()
{
	storage_validate_keys "$1" 'enabled device zram_fallback zram_size_kib' || return 1
	storage_swap_values "$1"
	case "$STORAGE_SWAP_ENABLED:$STORAGE_ZRAM_FALLBACK" in 0:0|0:1|1:0|1:1) ;; *) return 1 ;; esac
	storage_valid_device "$STORAGE_SWAP_DEVICE" || return 1
	case "$STORAGE_ZRAM_SIZE" in ''|*[!0-9]*) return 1 ;; esac
	[ "${#STORAGE_ZRAM_SIZE}" -le 7 ] && [ "$STORAGE_ZRAM_SIZE" -ge 1024 ] && [ "$STORAGE_ZRAM_SIZE" -le 1048576 ]
}

storage_swap_stop()
{
	local device remaining failed=0 owned="$RUN_DIR/swaps"
	[ -f "$owned" ] || return 0
	remaining="$(mktemp "$RUN_DIR/swaps.XXXXXX")" || return 1
	while IFS= read -r device; do
		storage_valid_device "$device" || { failed=1; continue; }
		if storage_in_swaps "$device" && ! swapoff "$device"; then
			printf '%s\n' "$device" >>"$remaining"
			storage_error "Could not stop swap on $device; it is still in use."
			failed=1
			continue
		fi
		if storage_in_swaps "$device" &&
		   { [ ! -f "$RUN_DIR/swaps" ] || [ "$(cat "$RUN_DIR/swaps")" = "$device" ]; }; then
			printf '%s\n' "$device" >>"$remaining"
			storage_error "Swap remains active on $device."
			failed=1
			continue
		fi
		if [ "$device" = /dev/zram0 ] && [ -w "$(storage_sys block/zram0/reset)" ]; then
			printf '1\n' >"$(storage_sys block/zram0/reset)" || failed=1
		fi
	done <"$owned"
	if [ -s "$remaining" ]; then mv "$remaining" "$owned"; else rm -f "$remaining" "$owned"; fi
	return "$failed"
}

storage_swap_claim()
{
	local device="$1"
	# An already active swap belongs to its existing owner. Do not adopt it.
	storage_in_swaps "$device" && return 0
	mkdir -p "$RUN_DIR" || return 1
	[ ! -e "$RUN_DIR/swaps" ] || [ -w "$RUN_DIR/swaps" ] || return 1
	swapon "$device" || return 1
	if ! printf '%s\n' "$device" >>"$RUN_DIR/swaps"; then
		swapoff "$device"
		return 1
	fi
	return 0
}

storage_swap_apply()
{
	local boot="${1:-0}" file="${2:-$CONF_DIR/swap.conf}" device='' candidate usb_mode
	storage_validate_swap_config "$file" || { storage_error 'Invalid swap settings.'; return 1; }
	[ -r "$(storage_proc swaps)" ] && have swapon && have swapoff || {
		storage_error 'Swap is not available in this image.'; return 1;
	}
	if [ "$STORAGE_SWAP_ENABLED" = 0 ]; then storage_swap_stop; return $?; fi
	# Device mode removes the board's USB host controller. Boot must not wait
	# for host disks that cannot appear in the saved USB mode.
	usb_mode="$(conf_get "$CONF_DIR/usb.conf" mode host)"
	if [ "$usb_mode" != host ]; then
		case "$STORAGE_SWAP_DEVICE" in
			auto|/dev/sd*)
				printf 'USB swap skipped while USB device mode is selected.\n'
				[ "$boot" = 1 ]; return $? ;;
		esac
	fi
	if [ "$STORAGE_SWAP_DEVICE" = auto ]; then
		for candidate in $(storage_devices); do
			case "$candidate" in */sd*) ;; *) continue ;; esac
			storage_swap_signature "$candidate" || continue
			device="$candidate"
			break
		done
	else
		device="$(storage_resolve_device "$STORAGE_SWAP_DEVICE")" || device=''
	fi
	if [ "$usb_mode" != host ]; then
		case "$device" in /dev/sd*)
			printf 'USB swap skipped while USB device mode is selected.\n'
			[ "$boot" = 1 ]; return $? ;;
		esac
	fi
	if [ -n "$device" ]; then
		storage_swap_signature "$device" || { storage_error "$device has no swap signature."; return 1; }
		if storage_in_swaps "$device"; then
			printf 'Swap already active on %s.\n' "$device"
			return 0
		fi
		storage_swap_stop || return 1
		storage_swap_claim "$device" || return 1
		printf 'Swap active on %s.\n' "$device"
		return 0
	fi
	if [ "$STORAGE_ZRAM_FALLBACK" = 1 ]; then
		storage_feature zram && [ -b /dev/zram0 ] && [ -w "$(storage_sys block/zram0/disksize)" ] || {
			storage_error 'Compressed RAM swap is unavailable in this image.'; return 1;
		}
		storage_in_swaps /dev/zram0 && return 0
		storage_swap_stop || return 1
		printf '%sK\n' "$STORAGE_ZRAM_SIZE" >"$(storage_sys block/zram0/disksize)" || return 1
		mkswap /dev/zram0 >/dev/null || return 1
		storage_swap_claim /dev/zram0
		return $?
	fi
	printf 'No configured swap device is present.\n'
	[ "$boot" = 1 ]
}

storage_swap_configure()
{
	local enabled="$1" device="$2" temporary
	case "$enabled" in 0|1) ;; *) return 2 ;; esac
	storage_valid_device "$device" || return 2
	mkdir -p "$CONF_DIR" "$RUN_DIR" || return 1
	temporary="$(mktemp "$CONF_DIR/swap.conf.XXXXXX")" || return 1
	storage_swap_values
	printf 'enabled=%s\ndevice=%s\nzram_fallback=%s\nzram_size_kib=%s\n' \
		"$enabled" "$device" "$STORAGE_ZRAM_FALLBACK" "$STORAGE_ZRAM_SIZE" >"$temporary"
	chmod 0600 "$temporary"
	if ! storage_swap_apply 0 "$temporary"; then rm -f "$temporary"; return 1; fi
	mv "$temporary" "$CONF_DIR/swap.conf" || { storage_error 'Swap changed, but saving the configuration failed.'; return 1; }
}

storage_mount_capable()
{
	have mount && have umount && have blkid &&
		grep -Eq '(^|[[:space:]])(ext2|ext3|ext4|vfat|exfat|f2fs)$' "$(storage_proc filesystems)"
}

storage_valid_mount_path()
{
	case "$1" in /mnt/?*|/media/?*) ;; *) return 1 ;; esac
	case "$1" in *[!A-Za-z0-9_./-]*|*/../*|*/./*|*/..|*/.|*//*|*/) return 1 ;; esac
	[ "${#1}" -le 128 ]
}

storage_validate_mount_config()
{
	local file="$1" device path enabled readonly
	storage_validate_keys "$file" 'enabled device path readonly' || return 1
	device="$(conf_get "$file" device '')"
	path="$(conf_get "$file" path /mnt/storage)"
	enabled="$(conf_get "$file" enabled 0)"
	readonly="$(conf_get "$file" readonly 0)"
	case "$enabled:$readonly" in 0:0|0:1|1:0|1:1) ;; *) return 1 ;; esac
	[ "$device" != auto ] && storage_valid_device "$device" && storage_valid_mount_path "$path"
}

storage_mount_present()
{
	awk -v path="$1" '$2 == path { found=1 } END { exit !found }' "$(storage_proc mounts)" 2>/dev/null
}

storage_mount_stop()
{
	local device path actual file="$RUN_DIR/storage.mount"
	[ -r "$file" ] || return 0
	device="$(conf_get "$file" device '')"
	path="$(conf_get "$file" path '')"
	storage_valid_device "$device" && storage_valid_mount_path "$path" || return 1
	if storage_mount_present "$path"; then
		actual="$(awk -v path="$path" '$2==path { print $1; exit }' "$(storage_proc mounts)")"
		[ "$actual" = "$device" ] || { storage_error "$path is now used by another mount."; return 1; }
		umount "$path" || { storage_error "$path is busy; close files on this volume and try again."; return 1; }
		storage_mount_present "$path" && { storage_error "$path remains mounted."; return 1; }
	fi
	rm -f "$file"
}

storage_mount_apply()
{
	local boot="${1:-0}" file="${2:-$CONF_DIR/storage.conf}" device path type option component current='' oldmode
	[ -f "$file" ] || return 0
	storage_validate_mount_config "$file" || { storage_error 'Invalid storage settings.'; return 1; }
	[ "$boot" != 1 ] || [ "$(conf_get "$file" enabled 0)" = 1 ] || return 0
	storage_mount_capable || { storage_error 'Removable filesystem support is unavailable in this image.'; return 1; }
	device="$(storage_resolve_device "$(conf_get "$file" device '')")" || {
		printf 'Configured storage device is absent.\n'; [ "$boot" = 1 ]; return $?;
	}
	storage_in_swaps "$device" && { storage_error "$device is being used as swap."; return 1; }
	type="$(storage_blkid_value "$device" TYPE)"
	case "$type" in ext2|ext3|ext4|vfat|exfat|f2fs) ;; *) storage_error 'Select an existing supported filesystem.'; return 1 ;; esac
	grep -Eq "(^|[[:space:]])${type}$" "$(storage_proc filesystems)" || {
		storage_error "The current image has no $type filesystem driver."; return 1;
	}
	path="$(conf_get "$file" path /mnt/storage)"
	option=rw
	[ "$(conf_get "$file" readonly 0)" = 0 ] || option=ro
	# A lexical /mnt path must not redirect through symlinks into the rootfs.
	oldmode="$IFS"; IFS=/
	for component in $path; do
		[ -n "$component" ] || continue
		current="$current/$component"
		if [ -L "$current" ]; then IFS="$oldmode"; storage_error 'The mount path may not contain symlinks.'; return 1; fi
	done
	IFS="$oldmode"
	if storage_mount_present "$path"; then
		if [ "$(conf_get "$RUN_DIR/storage.mount" device '')" = "$device" ] &&
		   [ "$(conf_get "$RUN_DIR/storage.mount" path '')" = "$path" ] &&
		   [ "$(awk -v path="$path" '$2==path { print $1; exit }' "$(storage_proc mounts)")" = "$device" ]; then
			mount -o "remount,$option" "$path" || return 1
			return 0
		fi
		storage_error "$path is already mounted by another service."; return 1
	fi
	awk -v dev="$device" '$1==dev { found=1 } END { exit !found }' "$(storage_proc mounts)" && {
		storage_error "$device is already mounted."; return 1;
	}
	storage_mount_stop || return 1
	mkdir -p "$path" "$RUN_DIR" || return 1
	[ -z "$(ls -A "$path" 2>/dev/null)" ] || { storage_error 'Choose an empty mount directory.'; return 1; }
	mount -t "$type" -o "$option" "$device" "$path" || return 1
	if ! printf 'device=%s\npath=%s\n' "$device" "$path" >"$RUN_DIR/storage.mount"; then
		umount "$path"; return 1
	fi
}

storage_mount_configure()
{
	local device="$1" path="$2" enabled="$3" readonly="$4" temporary resolved
	mkdir -p "$CONF_DIR" "$RUN_DIR" || return 1
	resolved="$(storage_resolve_device "$device")" || { storage_error 'Storage device is absent or ambiguous.'; return 1; }
	device="$(storage_device_id "$resolved")"
	temporary="$(mktemp "$CONF_DIR/storage.conf.XXXXXX")" || return 1
	printf 'enabled=%s\ndevice=%s\npath=%s\nreadonly=%s\n' "$enabled" "$device" "$path" "$readonly" >"$temporary"
	chmod 0600 "$temporary"
	if ! storage_mount_apply 0 "$temporary"; then rm -f "$temporary"; return 1; fi
	mv "$temporary" "$CONF_DIR/storage.conf" || { storage_error 'Storage mounted, but saving the configuration failed.'; return 1; }
}

storage_status()
{
	storage_swap_values
	printf 'SWAP\nUse at startup: %s\nDevice: %s\n' "$STORAGE_SWAP_ENABLED" "$STORAGE_SWAP_DEVICE"
	cat "$(storage_proc swaps)" 2>/dev/null || true
	printf '\nREMOVABLE STORAGE\n'
	if ! storage_mount_capable; then printf 'Filesystem support unavailable in this image.\n'; fi
	if [ -f "$CONF_DIR/storage.conf" ]; then cat "$CONF_DIR/storage.conf"; else printf 'No volume configured.\n'; fi
	[ ! -r "$RUN_DIR/storage.mount" ] || cat "$RUN_DIR/storage.mount"
	return 0
}

storage_apply()
{
	local result=0
	storage_swap_apply 1 || result=1
	storage_mount_apply 1 || result=1
	return "$result"
}

storage_select_device()
{
	local kind="$1" device type id choice
	set --
	for device in $(storage_devices); do
		if [ "$kind" = swap ]; then
			storage_swap_signature "$device" || continue
			type=swap
		else
			type="$(storage_blkid_value "$device" TYPE)"
			case "$type" in ext2|ext3|ext4|vfat|exfat|f2fs) ;; *) continue ;; esac
			grep -Eq "(^|[[:space:]])${type}$" "$(storage_proc filesystems)" || continue
		fi
		id="$(storage_device_id "$device")"
		set -- "$@" "$id" "$device ($type)"
	done
	[ "$#" -gt 0 ] || { ui_error 'No supported device found. Connect a drive containing an existing filesystem or swap partition.'; return 1; }
	ui_dialog --stdout --title 'Select storage device' --cancel-label Back --menu 'Device' 19 74 10 "$@"
}

storage_swap_menu()
{
	local choice device summary
	while :; do
		storage_swap_values
		summary="Use at startup: $(storage_enabled_label "$STORAGE_SWAP_ENABLED")\nDevice: $STORAGE_SWAP_DEVICE${UI_NOTICE:+\n$UI_NOTICE}"
		choice="$(ui_dialog --stdout --title Swap --cancel-label Back --menu "$summary" 17 72 5 \
			enable 'Select device and enable' disable 'Disable managed swap' status 'Current usage')" || return 0
		UI_NOTICE=''
		case "$choice" in
			enable) device="$(storage_select_device swap)" || continue
				ui_run_action Swap 'Enabling swap...' 'Swap enabled.' storage_swap_configure 1 "$device" || true ;;
			disable) ui_run_action Swap 'Stopping managed swap...' 'Swap disabled.' storage_swap_configure 0 "$STORAGE_SWAP_DEVICE" || true ;;
			status) ui_show_command Swap storage_status ;;
		esac
	done
}

storage_volume_menu()
{
	local choice device path enabled readonly
	while :; do
		choice="$(ui_dialog --stdout --title 'Removable storage' --cancel-label Back --menu \
			"$(conf_get "$CONF_DIR/storage.conf" device 'No device selected')\nMount: $(conf_get "$CONF_DIR/storage.conf" path /mnt/storage)\nUse at startup: $(storage_enabled_label "$(conf_get "$CONF_DIR/storage.conf" enabled 0)")${UI_NOTICE:+\n$UI_NOTICE}" 18 74 5 \
			configure 'Select volume and mount' mount 'Mount saved volume' unmount 'Safely unmount' autostart 'Enable / disable startup mount')" || return 0
		UI_NOTICE=''
		case "$choice" in
			configure)
				device="$(storage_select_device volume)" || continue
				path="$(conf_get "$CONF_DIR/storage.conf" path /mnt/storage)"
				while :; do
					path="$(ui_dialog --stdout --title 'Mount directory' --cancel-label Back --inputbox 'Directory under /mnt or /media' 9 70 "$path")" || { path=''; break; }
					storage_valid_mount_path "$path" && break
					ui_error 'Use a directory such as /mnt/storage. Use letters, numbers, underscores and dashes.'
				done
				storage_valid_mount_path "$path" || continue
				readonly="$(ui_dialog --stdout --title 'Storage access' --cancel-label Back --default-item "$(conf_get "$CONF_DIR/storage.conf" readonly 0)" --menu 'Access mode' 12 66 2 0 'Read and write' 1 'Read only')" || continue
				enabled="$(ui_dialog --stdout --title 'Storage startup' --cancel-label Back --default-item "$(conf_get "$CONF_DIR/storage.conf" enabled 1)" --menu 'Mount at startup' 12 66 2 1 Enabled 0 Disabled)" || continue
				ui_run_action Storage 'Mounting volume...' 'Volume mounted and settings saved.' storage_mount_configure "$device" "$path" "$enabled" "$readonly" || true ;;
			mount) ui_run_action Storage 'Mounting saved volume...' 'Volume mounted.' storage_mount_apply || true ;;
			unmount) ui_run_action Storage 'Unmounting volume...' 'Volume unmounted.' storage_mount_stop || true ;;
			autostart)
				[ -f "$CONF_DIR/storage.conf" ] || { ui_error 'Select a volume first.'; continue; }
				enabled="$(ui_dialog --stdout --title 'Storage startup' --cancel-label Back --default-item "$(conf_get "$CONF_DIR/storage.conf" enabled 0)" --menu 'Mount at startup' 12 66 2 1 Enabled 0 Disabled)" || continue
				if conf_set "$CONF_DIR/storage.conf" enabled "$enabled"; then UI_NOTICE='Startup setting saved.'; else ui_error 'Could not save settings.'; fi ;;
		esac
	done
}

storage_menu()
{
	local choice
	while :; do
		set -- swap 'Swap settings'
		storage_mount_capable && set -- "$@" volume 'Removable storage'
		choice="$(ui_dialog --stdout --title 'Memory & storage' --cancel-label Back --menu "Configure memory and connected storage${UI_NOTICE:+\n$UI_NOTICE}" 15 72 5 "$@" status 'Current usage')" || return 0
		UI_NOTICE=''
		case "$choice" in swap) storage_swap_menu ;; volume) storage_volume_menu ;; status) ui_show_command 'Memory & storage' storage_status ;; esac
	done
}

storage_cli()
{
	local command="${1:-status}"
	[ "$#" -eq 0 ] || shift
	case "$command" in
		status) [ "$#" -eq 0 ] || return 2; storage_status ;;
		swap)
			case "${1:-status}" in
				status) [ "$#" -le 1 ] || return 2; storage_status ;;
				enable) [ "$#" -eq 2 ] || return 2; storage_swap_configure 1 "$2" ;;
				disable) [ "$#" -eq 1 ] || return 2; storage_swap_values; storage_swap_configure 0 "$STORAGE_SWAP_DEVICE" ;;
				apply) [ "$#" -eq 1 ] || return 2; storage_swap_apply ;;
				stop) [ "$#" -eq 1 ] || return 2; storage_swap_stop ;;
				*) return 2 ;;
			esac ;;
		configure) [ "$#" -eq 4 ] || { storage_error 'Usage: esp32-config storage configure DEVICE PATH AUTOSTART READONLY'; return 2; }; storage_mount_configure "$@" ;;
		mount) [ "$#" -eq 0 ] || return 2; storage_mount_apply ;;
		unmount) [ "$#" -eq 0 ] || return 2; storage_mount_stop ;;
		autostart) [ "$#" -eq 1 ] && { [ "$1" = 0 ] || [ "$1" = 1 ]; } && [ -f "$CONF_DIR/storage.conf" ] || return 2; conf_set "$CONF_DIR/storage.conf" enabled "$1" ;;
		apply) [ "$#" -eq 0 ] || return 2; storage_apply ;;
		stop) [ "$#" -eq 0 ] || return 2; storage_mount_stop && storage_swap_stop ;;
		*) storage_error 'Usage: esp32-config storage status|swap|configure|mount|unmount|autostart'; return 2 ;;
	esac
}

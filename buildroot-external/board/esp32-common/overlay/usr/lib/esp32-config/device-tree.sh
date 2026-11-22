# Inspect the boot FDT and stage property changes as a complete DTB.

dt_operation()
{
	local command base directory saved input draft result
	command="$1"
	shift
	base="$CONFIG_ROOT/sys/firmware/fdt"
	directory="$CONFIG_ROOT/persist/etc/esp32-conf"
	saved="$directory/device-tree.dtb"
	input="$base"
	if [ -f "$saved" ]; then
		input="$saved"
	fi

	case "$command" in
		nodes) [ "$#" -eq 0 ] && esp32-dt nodes "$input" ;;
		properties) [ "$#" -eq 1 ] && esp32-dt properties "$input" "$1" ;;
		get) [ "$#" -eq 2 ] && esp32-dt get "$input" "$@" ;;
		export) [ "$#" -eq 0 ] && cat "$input" ;;
		save)
			[ "$#" -ge 3 ] || return 2
			mkdir -p "$directory" || return 1
			draft="$(mktemp "$directory/device-tree.XXXXXX")" || return 1
			if esp32-dt set "$input" "$@" >"$draft" &&
			   chmod 0600 "$draft" && mv -f "$draft" "$saved"; then
				result=0
			else
				result=1
			fi
			rm -f "$draft"
			return "$result"
			;;
		discard)
			[ "$#" -eq 0 ] || return 2
			rm -f "$saved"
			;;
		*)
			warn 'Usage: esp32-config dt nodes|properties PATH|get PATH PROPERTY|save PATH PROPERTY TYPE [VALUE ...]|export|discard'
			return 2
			;;
	esac
}

dt_cli()
{
	case "$1" in
		save|discard)
			mountpoint -q "$CONFIG_ROOT/persist" || { warn "/persist is not mounted."; return 1; }
			dt_locked_operation "$@" 9>"$RUN_DIR/device-tree.lock"
			;;
		*) dt_operation "$@" ;;
	esac
}

dt_locked_operation()
{
	flock -x 9 || return 1
	dt_operation "$@"
}

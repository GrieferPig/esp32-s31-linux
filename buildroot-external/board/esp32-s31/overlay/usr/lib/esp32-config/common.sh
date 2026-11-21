#!/bin/sh
# Shared display and file-editing helpers; settings use their native files.
RUN_DIR="${ESP32_CONFIG_RUN_DIR:-/run/esp32-config}"
NETWORK_CONF="${ESP32_CONFIG_NETWORK_CONF:-/etc/network/interfaces.d/wlan0}"
USB_CONF="${ESP32_CONFIG_USB_CONF:-/etc/default/s31-usb}"
UI_NOTICE="${UI_NOTICE:-}"

prepare_runtime()
{
	umask 077
	mkdir -p "$RUN_DIR"
}

save_file()
{
	local destination="$1" mode="$2" source="$3" temporary
	if [ -L "$destination" ]; then destination="$(readlink -f "$destination")" || return 1; fi
	mkdir -p "${destination%/*}" || return 1
	temporary="$(mktemp "${destination}.XXXXXX")" || return 1
	if ! cat "$source" >"$temporary" || ! chmod "$mode" "$temporary" || ! mv -f "$temporary" "$destination"; then
		rm -f "$temporary"; return 1
	fi
}

write_lines()
{
	local destination="$1" mode="$2" temporary
	shift 2
	temporary="$(mktemp "$RUN_DIR/settings.XXXXXX")" || return 1
	printf '%s\n' "$@" >"$temporary" && save_file "$destination" "$mode" "$temporary"
	local result=$?
	rm -f "$temporary"
	return "$result"
}

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



interface_feature()
{
	grep -qx "$1=1" "${ESP32_CONFIG_FEATURES_FILE:-/usr/share/esp32-config/kernel-features}" 2>/dev/null
}

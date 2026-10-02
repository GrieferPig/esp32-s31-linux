#!/bin/sh
set -eu
test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
temp_dir=$(mktemp -d)
trap 'rm -rf "$temp_dir"' EXIT HUP INT TERM
CONF_DIR="${temp_dir}/config"
RUN_DIR="${temp_dir}/run"
mkdir -p "$CONF_DIR" "$RUN_DIR"
# Exercise the legacy diagnostic guard with reordered GPIO controllers.
. "${test_dir}/../../buildroot-external/board/esp32-s31/overlay/usr/lib/esp32-config/common.sh"
CONF_DIR="${temp_dir}/config"
RUN_DIR="${temp_dir}/run"
gpiodetect()
{
	case "$1" in
		gpiochip0) printf 'gpiochip0 [external-expander] (64 lines)\n' ;;
		gpiochip2) printf 'gpiochip2 [20583000.pinctrl] (62 lines)\n' ;;
		*) return 1 ;;
	esac
}
safe_s31_line gpiochip2 004
safe_s31_line gpiochip0 26
if safe_s31_line gpiochip2 026 || safe_s31_line gpiochip2 58 ||
   safe_s31_line gpiochip2 4294967296 || safe_s31_line gpiochip9 4; then
	echo 'Legacy GPIO controller identity/reservation guard failed.' >&2
	exit 1
fi
. "${test_dir}/../../buildroot-external/board/esp32-s31/overlay/usr/lib/esp32-config/gpio.sh"

ensure_config() { return 0; }
ui_error() { printf '%s\n' "$*" >>"${temp_dir}/errors"; }
gpio_set() { printf '%s %s %s\n' "$1" "$2" "$3" >>"${temp_dir}/applied"; }
gpio_status() { cat "${temp_dir}/lines"; }
ui_dialog()
{
	printf '%s\n' "$*" >>"${temp_dir}/dialogs"
	local selection
	selection=$(head -n 1 "${temp_dir}/answers")
	tail -n +2 "${temp_dir}/answers" >"${temp_dir}/answers.new"
	mv "${temp_dir}/answers.new" "${temp_dir}/answers"
	[ -n "$selection" ] && [ "$selection" != CANCEL ] || return 1
	printf '%s\n' "$selection"
}
printf '4\tapplication\tnone\t-\t-\tapplication\tnone\n' >"${temp_dir}/lines"

# Editing and backing out must not change the pin or the saved configuration.
printf '4\nmode\nhigh\nCANCEL\nCANCEL\n' >"${temp_dir}/answers"
gpio_menu
[ ! -e "${temp_dir}/applied" ]
[ ! -e "${temp_dir}/errors" ]

# A committed output is a durable level setting, with no test-duration step.
printf '4\nmode\nhigh\nsave\nCANCEL\n' >"${temp_dir}/answers"
gpio_menu
[ "$(cat "${temp_dir}/applied")" = '4 high none' ]

# Bias selection remains a draft until Save and apply is chosen.
: >"${temp_dir}/applied"
printf '4\nmode\ninput\nbias\nup\nsave\nCANCEL\n' >"${temp_dir}/answers"
gpio_menu
[ "$(cat "${temp_dir}/applied")" = '4 input up' ]

# Selecting a busy peripheral pin must not call the configuration backend.
: >"${temp_dir}/applied"
printf '5\tbusy\tnone\t-\ttest-peripheral\tapplication\tnone\n' >"${temp_dir}/lines"
printf '5\nCANCEL\n' >"${temp_dir}/answers"
gpio_menu
[ ! -s "${temp_dir}/applied" ]
case "$(cat "${temp_dir}/errors")" in *test-peripheral*) ;; *) exit 1 ;; esac

# Applying an empty configuration does not start an otherwise unused daemon.
require_root() { return 0; }
gpio_backend() { case "$1" in ping) return 1 ;; *) return 0 ;; esac; }
gpio_ensure_service() { : >"${temp_dir}/service-started"; }
gpio_apply
[ ! -e "${temp_dir}/service-started" ]
printf '# No GPIO assignments\n' >"${CONF_DIR}/gpio.conf"
gpio_apply
[ ! -e "${temp_dir}/service-started" ]
printf '4 input up\n' >"${CONF_DIR}/gpio.conf"
gpio_apply
[ -e "${temp_dir}/service-started" ]
echo 'GPIO menu cancel, commit, bias and ownership tests passed.'

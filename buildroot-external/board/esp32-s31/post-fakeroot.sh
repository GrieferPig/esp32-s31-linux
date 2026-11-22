#!/bin/sh
set -eu
target_dir="$1"

# Keep factory defaults in SquashFS for first boot. Existing target directories
# can still contain the /init left behind by a previous Buildroot overlay.
rm -f "$target_dir/init"
mkdir -p "$target_dir/persist"
store="$target_dir/usr/lib/s31-persist"
while read -r action path; do
	case "$action" in link|copy) ;; *) continue ;; esac
	entry="$target_dir$path"
	default="$store/defaults$path"
	if [ -L "$entry" ] && [ "$(readlink "$entry")" = "/persist$path" ]; then
		continue
	fi
	if [ -e "$entry" ] || [ -L "$entry" ]; then
		mkdir -p "${default%/*}"
		if [ "$action" = copy ]; then
			cp -a "$entry" "$default"
		else
			mv -fT "$entry" "$default"
		fi
	fi
	if [ "$action" = link ]; then
		mkdir -p "${entry%/*}"
		ln -s "/persist$path" "$entry"
	fi
done < "$store/paths"
mkdir -m 0700 -p "$store/defaults/root"
mkdir -m 0755 -p "$store/defaults/home"

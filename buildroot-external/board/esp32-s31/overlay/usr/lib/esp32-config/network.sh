#!/bin/sh
# Network policy and the esp32-config network pages. Sourced by the frontend.

radio_service()
{
	ESP32_CONFIG_DIR="$CONF_DIR" "${ESP32_CONFIG_RADIO_SERVICE:-/etc/init.d/S00s31-radio}" "$@"
}

bluetooth_service()
{
	local service="${ESP32_CONFIG_BT_SERVICE:-/etc/init.d/S40btstack}"
	[ -x "$service" ] || return 0
	S31_BTSTACK_CONFIG="$BT_CONF" "$service" "$@"
}

radio_prepare()
{
	radio_service start
}

radio_reload_and_apply_all()
{
	local result=0 code
	wifi_stop || return 1
	bluetooth_service stop || return 1
	radio_service restart || return 1
	# The shared controller must finish Bluetooth setup before Wi-Fi joins.
	bt_apply || result=1
	wifi_apply
	code=$?
	[ "$code" -eq 1 ] && result=1
	[ "$code" -eq 2 ] && [ "$result" -eq 0 ] && result=2
	return "$result"
}

network_valid_ipv4()
{
	case "$1" in ''|*[!0-9.]*) return 1 ;; esac
	LC_ALL=C awk -v address="$1" 'BEGIN {
		if (split(address, octet, ".") != 4) exit 1
		for (i = 1; i <= 4; i++) {
			if (octet[i] !~ /^[0-9]+$/ || length(octet[i]) > 3 ||
			    (length(octet[i]) > 1 && substr(octet[i], 1, 1) == "0") ||
			    octet[i] + 0 > 255) exit 1
		}
	}' </dev/null
}

network_valid_host()
{
	network_valid_ipv4 "$1" || return 1
	case "${1%%.*}" in 0|127) return 1 ;; esac
	[ "${1%%.*}" -lt 224 ]
}

network_valid_dns()
{
	network_valid_ipv4 "$1" || return 1
	[ "${1%%.*}" -gt 0 ] && [ "${1%%.*}" -lt 224 ]
}

network_validate_policy()
{
	local dhcp="$1" address="$2" prefix="$3" gateway="$4" dns_mode="$5" servers="$6" server carriage
	carriage="$(printf '\r')"
	valid_enabled "$dhcp" || { warn 'Choose automatic or manual IPv4 addressing.'; return 1; }
	case "$dns_mode" in auto|manual) ;; *) warn 'Choose automatic or manual DNS.'; return 1 ;; esac
	[ -z "$address" ] || network_valid_host "$address" || { warn 'Invalid saved IPv4 address.'; return 1; }
	[ -z "$gateway" ] || network_valid_host "$gateway" || { warn 'Invalid saved gateway.'; return 1; }
	case "$prefix" in ''|*[!0-9]*) warn 'The prefix must be a number from 0 to 32.'; return 1 ;; esac
	[ "${#prefix}" -le 2 ] && [ "$prefix" -le 32 ] || { warn 'The prefix must be a number from 0 to 32.'; return 1; }
	if [ "$dhcp" = 0 ]; then
		network_valid_host "$address" || { warn 'Enter a valid unicast IPv4 address.'; return 1; }
		case "$prefix" in ''|*[!0-9]*) warn 'The prefix must be a number from 0 to 32.'; return 1 ;; esac
		[ "${#prefix}" -le 2 ] && [ "$prefix" -le 32 ] || {
			warn 'The prefix must be a number from 0 to 32.'; return 1;
		}
		[ "$dns_mode" = manual ] || { warn 'Manual IPv4 needs manually specified DNS (the list may be empty).'; return 1; }
		if [ -n "$gateway" ]; then
			network_valid_host "$gateway" || { warn 'Enter a valid gateway or leave it empty.'; return 1; }
			LC_ALL=C awk -v ip="$address" -v gw="$gateway" -v prefix="$prefix" 'function number(s, a) {
				split(s, a, "."); return a[1] * 16777216 + a[2] * 65536 + a[3] * 256 + a[4]
			} BEGIN { size = 2 ^ (32 - prefix); if (prefix != 32 && int(number(ip) / size) != int(number(gw) / size)) exit 1 }' </dev/null || {
				warn 'The gateway must be in the selected subnet (a /32 host route is also supported).'; return 1;
			}
		fi
	fi
	case "$servers" in *'
'*|*"$carriage"*) warn 'Enter DNS addresses on one line, separated by spaces.'; return 1 ;; esac
	# Globbing is disabled in the subshell so input can never expand filenames.
	( set -f
	  set -- $servers
	  [ "$#" -le 3 ] || { warn 'Use at most three DNS servers.'; exit 1; }
	  for server do
		  network_valid_dns "$server" || { warn "Invalid DNS server: $server"; exit 1; }
	  done
	) || return 1
	return 0
}

network_policy_valid()
{
	network_validate_policy \
		"$(conf_get "$WIFI_CONF" dhcp 1)" \
		"$(conf_get "$WIFI_CONF" address '')" \
		"$(conf_get "$WIFI_CONF" prefix 24)" \
		"$(conf_get "$WIFI_CONF" gateway '')" \
		"$(conf_get "$WIFI_CONF" dns_mode auto)" \
		"$(conf_get "$WIFI_CONF" dns_servers '')"
}

network_save_policy()
{
	local dhcp="$1" address="$2" prefix="$3" gateway="$4" dns_mode="$5" servers="$6" temporary
	require_root
	ensure_config || return 1
	if [ "$dhcp" = 1 ]; then address=''; prefix=24; gateway=''; fi
	network_validate_policy "$dhcp" "$address" "$prefix" "$gateway" "$dns_mode" "$servers" || return 1
	servers="$(printf '%s\n' "$servers" | awk '{$1=$1; print}')"
	temporary="$(mktemp "${WIFI_CONF}.tmp.XXXXXX")" || return 1
	# All fields are numeric addresses or fixed enums after validation. Rewrite
	# the complete policy once: cancellation and invalid input change nothing.
	if ! {
		sed '/^dhcp=/d; /^address=/d; /^prefix=/d; /^gateway=/d; /^dns_mode=/d; /^dns_servers=/d' "$WIFI_CONF"
		printf 'dhcp=%s\naddress=%s\nprefix=%s\ngateway=%s\ndns_mode=%s\ndns_servers=%s\n' \
			"$dhcp" "$address" "$prefix" "$gateway" "$dns_mode" "$servers"
	} >"$temporary"; then
		rm -f "$temporary"; return 1
	fi
	chmod 0600 "$temporary" && mv "$temporary" "$WIFI_CONF"
}

network_write_dns()
{
	local servers="$1" search_domain="${2:-}" destination temporary server
	destination="${ESP32_CONFIG_RESOLV_CONF:-/etc/resolv.conf}"
	# Preserve the resolv.conf symlink and replace its target atomically.
	if [ -L "$destination" ]; then
		[ -e "$destination" ] || touch "$destination" || return 1
		destination="$(readlink -f "$destination")" || return 1
	fi
	temporary="$(mktemp "${destination}.tmp.XXXXXX")" || return 1
	(
		set -f
		# DHCP supplies this string. Ignore malformed search values rather than
		# inserting new resolver directives from untrusted network data.
		case "$search_domain" in ''|*[!A-Za-z0-9_.\ -]*) ;; *) printf 'search %s\n' "$search_domain" ;; esac
		for server in $servers; do
			network_valid_dns "$server" && printf 'nameserver %s\n' "$server"
		done
		:
	) >"$temporary" || { rm -f "$temporary"; return 1; }
	chmod 0644 "$temporary" && mv "$temporary" "$destination"
}

network_apply_address()
{
	local interface="$1" address prefix gateway
	if [ "$(conf_get "$WIFI_CONF" dhcp 1)" = 0 ]; then
		address="$(conf_get "$WIFI_CONF" address '')"
		prefix="$(conf_get "$WIFI_CONF" prefix 24)"
		gateway="$(conf_get "$WIFI_CONF" gateway '')"
		ip -4 addr flush dev "$interface" || return 1
		ip addr add "${address}/${prefix}" dev "$interface" || return 1
		ip -4 route flush dev "$interface" scope global 2>/dev/null || true
		if [ -n "$gateway" ]; then
			[ "$prefix" != 32 ] || ip route replace "${gateway}/32" dev "$interface" || return 1
			ip route replace default via "$gateway" dev "$interface" || return 1
		fi
	fi
	if [ "$(conf_get "$WIFI_CONF" dns_mode auto)" = manual ]; then
		network_write_dns "$(conf_get "$WIFI_CONF" dns_servers '')" || return 1
	fi
}

wifi_state()
{
	printf '%s\n' "$1" >"${RUN_DIR}/wifi.state"
	shift
	printf '%s\n' "$*" >"${RUN_DIR}/wifi.message"
	printf '%s\n' "$*"
}

wifi_apply()
{
	local enabled interface command_name count state pidfile code address dhcp
	require_root
	ensure_config || return 1
	enabled="$(conf_get "$WIFI_CONF" enabled 0)"
	interface="$(conf_get "$WIFI_CONF" interface wlan0)"
	valid_enabled "$enabled" && valid_interface "$interface" || {
		wifi_state failed 'Invalid Wi-Fi settings.'; return 1;
	}
	if [ "$enabled" = 0 ]; then
		wifi_stop || return 1
		wifi_state disabled 'Wi-Fi is off.'
		return 0
	fi
	[ -s "$WIFI_PROFILE" ] || { wifi_state failed 'Choose a network before connecting.'; return 1; }
	network_policy_valid || { wifi_state failed 'Correct the saved address settings before connecting.'; return 1; }
	for command_name in ip wpa_supplicant wpa_cli; do
		have "$command_name" || { wifi_state failed "Required tool unavailable: $command_name"; return 1; }
	done
	dhcp="$(conf_get "$WIFI_CONF" dhcp 1)"
	[ "$dhcp" = 0 ] || have udhcpc || { wifi_state failed 'DHCP client unavailable.'; return 1; }
	radio_prepare || { wifi_state failed 'The radio could not be started.'; return 1; }
	wifi_stop || return 1
	ip link set dev "$interface" up || { wifi_state failed 'The Wi-Fi interface is unavailable.'; return 1; }
	pidfile="$(wifi_pidfile "$interface")"
	if ! wpa_supplicant -B -D nl80211 -i "$interface" -c "$WIFI_PROFILE" -P "$pidfile" \
		>"${RUN_DIR}/wpa_supplicant.log" 2>&1; then
		wifi_state failed 'The Wi-Fi connection process could not start.'; return 1
	fi
	network_apply_address "$interface" || { wifi_state failed 'The address settings could not be applied.'; return 1; }
	if [ "$dhcp" = 1 ]; then
		# Keep the client available to renew the lease. It can also obtain an
		# address if association finishes after the interactive wait expires.
		ESP32_CONFIG_DIR="$CONF_DIR" ESP32_CONFIG_RUN_DIR="$RUN_DIR" \
			udhcpc -b -i "$interface" -p "$(dhcp_pidfile "$interface")" \
			-s "${ESP32_CONFIG_DHCP_SCRIPT:-/usr/share/udhcpc/default.script}" -t 5 -T 2 \
			>"${RUN_DIR}/udhcpc.${interface}.log" 2>&1 &
	fi
	wifi_state associating 'Connecting to the wireless network...'
	count=0
	state=''
	while [ "$count" -lt "${ESP32_CONFIG_ASSOC_TIMEOUT:-20}" ]; do
		state="$(wpa_cli -p /run/wpa_supplicant -i "$interface" status 2>/dev/null | sed -n 's/^wpa_state=//p')"
		[ "$state" = COMPLETED ] && break
		sleep 1
		count=$((count + 1))
	done
	if [ "$state" != COMPLETED ]; then
		if ! wpa_cli -p /run/wpa_supplicant -i "$interface" ping 2>/dev/null | grep -q '^PONG$'; then
			wifi_state failed 'Settings saved; the Wi-Fi connection process stopped.'; return 1
		fi
		wifi_state pending 'Settings saved; the connection is still in progress.'
		return 2
	fi
	wifi_state addressing 'Connected to the wireless network; getting an IP address...'
	count=0
	while [ "$count" -lt "${ESP32_CONFIG_ADDRESS_TIMEOUT:-15}" ]; do
		address="$(ip -4 address show dev "$interface" 2>/dev/null | awk '/ inet / { print $2; exit }')"
		if [ -n "$address" ]; then
			wifi_state connected "Connected: $address"
			return 0
		fi
		sleep 1
		count=$((count + 1))
	done
	wifi_state pending 'Settings saved; Wi-Fi is associated and the IP address is still pending.'
	return 2
}

wifi_text_hex()
{
	LC_ALL=C od -An -v -tx1 | tr -d ' \n'
}

wifi_hex_valid()
{
	case "$1" in ''|*[!0-9a-fA-F]*) return 1 ;; esac
	[ "${#1}" -le 64 ] && [ $((${#1} % 2)) -eq 0 ]
}

wifi_hex_bytes()
{
	LC_ALL=C awk -v hex="$1" 'function digit(c) { return index("0123456789abcdef", tolower(c)) - 1 }
		BEGIN { for (i = 1; i < length(hex); i += 2) printf "%c", digit(substr(hex,i,1))*16 + digit(substr(hex,i+1,1)) }' </dev/null
}

wifi_hex_label()
{
	# Numeric tags retain the identity. Labels are printable console text only.
	LC_ALL=C awk -v hex="$1" 'function digit(c) { return index("0123456789abcdef", tolower(c)) - 1 }
		BEGIN { for (i = 1; i < length(hex); i += 2) {
			h = substr(hex,i,2); n = digit(substr(h,1,1))*16 + digit(substr(h,2,1))
			if (n >= 32 && n <= 126 && n != 92) printf "%c", n
			else printf "\\x%s", tolower(h)
		} }' </dev/null
}

wifi_saved_hex()
{
	local saved value
	saved="$(conf_get "$WIFI_CONF" ssid_hex '')"
	if wifi_hex_valid "$saved"; then printf '%s\n' "$saved"; return; fi
	# Read the older wpa_passphrase profile for display/migration. New writes
	# always use hexadecimal SSIDs, so spaces, quotes and controls stay exact.
	value="$(sed -n 's/^[[:space:]]*ssid=//p' "$WIFI_PROFILE" 2>/dev/null | head -n 1)"
	case "$value" in
		\"*\") value="${value#\"}"; value="${value%\"}"; printf '%s' "$value" | wifi_text_hex ;;
		*) wifi_hex_valid "$value" && printf '%s\n' "$value" ;;
	esac
}

wifi_save_hex_profile()
{
	local hex="$1" password="$2" security="${3:-psk}" raw_ssid key temporary policy backup carriage password_bytes
	carriage="$(printf '\r')"
	require_root
	ensure_config || return 1
	wifi_hex_valid "$hex" || { warn 'The network name must contain 1 to 32 bytes.'; return 1; }
	case "$security" in
		open) key='' ;;
		psk)
			password_bytes="$(printf '%s' "$password" | LC_ALL=C wc -c)"
			case "$password" in *'
'*|*"$carriage"*) warn 'The password must be entered on one line.'; return 1 ;; esac
			if [ "$password_bytes" -eq 64 ]; then
				case "$password" in *[!0-9a-fA-F]*) warn 'A 64-character key must be hexadecimal.'; return 1 ;; esac
				key="$password"
			else
				[ "$password_bytes" -ge 8 ] && [ "$password_bytes" -le 63 ] || { warn 'The password must contain 8 to 63 bytes.'; return 1; }
				# wpa_passphrase takes an argv SSID, which cannot contain NUL.
				# The explicit hexadecimal CLI accepts a precomputed key for it.
				printf '%s' "$hex" | sed 's/../& /g' | grep -qw 00 && {
					warn 'This SSID contains a zero byte; use configure-hex with a 64-digit precomputed key.'; return 1;
				}
				have wpa_passphrase || { warn 'wpa_passphrase is unavailable.'; return 1; }
				raw_ssid="$(wifi_hex_bytes "$hex"; printf '.')"
				raw_ssid="${raw_ssid%.}"
				# Retain only the derived key. Never write the commented plaintext
				# password or the tool's quoted SSID into a file.
				key="$(printf '%s\n' "$password" | wpa_passphrase "$raw_ssid" | sed -n 's/^[[:space:]]*psk=\([0-9a-fA-F]\{64\}\)$/\1/p')"
				[ "${#key}" -eq 64 ] || { warn 'The network key could not be generated.'; return 1; }
			fi
			;;
		*) warn 'This network security type is not supported by this setup page.'; return 1 ;;
	esac
	password=''
	temporary="$(mktemp "${WIFI_PROFILE}.tmp.XXXXXX")" || return 1
	policy="$(mktemp "${WIFI_CONF}.tmp.XXXXXX")" || { rm -f "$temporary"; return 1; }
	backup="${temporary}.old"
	{
		printf 'ctrl_interface=/run/wpa_supplicant\nupdate_config=0\nap_scan=1\nnetwork={\n\tssid=%s\n\tscan_ssid=1\n' "$hex"
		if [ "$security" = open ]; then printf '\tkey_mgmt=NONE\n'; else printf '\tpsk=%s\n' "$key"; fi
		printf '}\n'
	} >"$temporary" || { rm -f "$temporary" "$policy"; return 1; }
	{
		sed '/^enabled=/d; /^ssid_hex=/d' "$WIFI_CONF"
		printf 'enabled=1\nssid_hex=%s\n' "$hex"
	} >"$policy" || { rm -f "$temporary" "$policy"; return 1; }
	chmod 0600 "$temporary" "$policy" || { rm -f "$temporary" "$policy"; return 1; }
	if [ -f "$WIFI_PROFILE" ]; then
		cp -p "$WIFI_PROFILE" "$backup" || { rm -f "$temporary" "$policy"; return 1; }
	fi
	if ! mv "$temporary" "$WIFI_PROFILE"; then rm -f "$temporary" "$policy" "$backup"; return 1; fi
	if ! mv "$policy" "$WIFI_CONF"; then
		if [ -f "$backup" ]; then mv "$backup" "$WIFI_PROFILE"; else rm -f "$WIFI_PROFILE"; fi
		rm -f "$policy"; return 1
	fi
	rm -f "$backup"
	return 0
}

wifi_save_profile()
{
	local hex
	hex="$(printf '%s' "$1" | wifi_text_hex)"
	wifi_save_hex_profile "$hex" "$2" "${3:-psk}"
}

wifi_set_enabled()
{
	local requested="$1" current
	require_root
	ensure_config || return 1
	valid_enabled "$requested" || return 1
	current="$(conf_get "$WIFI_CONF" enabled 0)"
	[ "$requested" != "$current" ] || return 0
	[ "$requested" = 0 ] || [ -s "$WIFI_PROFILE" ] || { warn 'Choose a network first.'; return 1; }
	[ "$requested" = 0 ] || network_policy_valid || return 1
	conf_set "$WIFI_CONF" enabled "$requested" || return 1
	radio_reload_and_apply_all
}

wifi_connect_saved()
{
	local previous_enabled="${1:-1}"
	if [ "$previous_enabled" = 0 ]; then radio_reload_and_apply_all; else wifi_apply; fi
}

wifi_forget()
{
	local previous temporary
	require_root
	ensure_config || return 1
	previous="$(conf_get "$WIFI_CONF" enabled 0)"
	temporary="$(mktemp "${WIFI_CONF}.tmp.XXXXXX")" || return 1
	{
		sed '/^enabled=/d; /^ssid_hex=/d' "$WIFI_CONF"
		printf 'enabled=0\nssid_hex=\n'
	} >"$temporary" || { rm -f "$temporary"; return 1; }
	chmod 0600 "$temporary" && mv "$temporary" "$WIFI_CONF" || return 1
	rm -f "$WIFI_PROFILE" || return 1
	if [ "$previous" = 1 ]; then radio_reload_and_apply_all; else wifi_stop; fi
}

wifi_status()
{
	local interface state address saved
	ensure_config || return 1
	interface="$(conf_get "$WIFI_CONF" interface wlan0)"
	[ "$(conf_get "$WIFI_CONF" enabled 0)" = 1 ] && printf 'Wi-Fi: On\n' || printf 'Wi-Fi: Off\n'
	saved="$(wifi_saved_hex)"
	[ -z "$saved" ] || printf 'Saved network: %s\n' "$(wifi_hex_label "$saved")"
	state="$(wpa_cli -p /run/wpa_supplicant -i "$interface" status 2>/dev/null | sed -n 's/^wpa_state=//p')"
	address="$(ip -4 address show dev "$interface" 2>/dev/null | awk '/ inet / { print $2; exit }')"
	case "$state" in
		COMPLETED) [ -n "$address" ] && printf 'Connected: %s\n' "$address" || printf 'Associated; IP address pending\n' ;;
		SCANNING|ASSOCIATING|ASSOCIATED|4WAY_HANDSHAKE|GROUP_HANDSHAKE) printf 'Connecting...\n' ;;
		*) printf 'Not connected\n' ;;
	esac
}

wifi_scan_parse()
{
	# wpa_cli uses printf_encode: \xNN, \\, \n, \r, \t and \e.
	# Decode to hex in the machine field, never recover an SSID from its label.
	LC_ALL=C awk -F '\t' '
	function hex(value, i,c,n,nextc,out) {
		out=""
		for (i=1; i<=length(value); i++) {
			c=substr(value,i,1)
			if (c=="\\") {
				nextc=substr(value,++i,1)
				if (nextc=="x" && substr(value,i+1,2) ~ /^[[:xdigit:]][[:xdigit:]]$/) { out=out tolower(substr(value,i+1,2)); i+=2; continue }
				if (nextc=="n") n=10; else if (nextc=="r") n=13; else if (nextc=="t") n=9; else if (nextc=="e") n=27
				else if (nextc=="\\" || nextc=="\"") n=ord[nextc]
				else return ""
			} else n=ord[c]
			out=out sprintf("%02x",n)
		}
		return out
	}
	BEGIN { for (j=1; j<256; j++) ord[sprintf("%c",j)]=j }
	NF>=5 && $1 ~ /^[[:xdigit:]][[:xdigit:]]:/ {
		value=$5; for (j=6;j<=NF;j++) value=value "\t" $j
		h=hex(value); if (length(h)<2 || length(h)>64) next
		if ($4 ~ /EAP|SAE|OWE|WEP/ && $4 !~ /PSK/) security="unsupported"
		else if ($4 ~ /PSK/) security="psk"
		else if ($4 ~ /WPA|RSN|WEP/) security="unsupported"
		else security="open"
		if (!seen[h,security]++) printf "%s\t%s\t%s\n", h,$3,security
	}'
}

wifi_scan_begin()
{
	local interface mode command_name
	WIFI_SCAN_TEMP=0
	WIFI_SCAN_RADIO_CHANGED=0
	WIFI_SCAN_LINK_CHANGED=0
	WIFI_SCAN_DIR=''
	WIFI_SCAN_READY=0
	for command_name in ip wpa_supplicant wpa_cli; do
		have "$command_name" || { warn "Required tool unavailable: $command_name"; return 1; }
	done
	interface="$(conf_get "$WIFI_CONF" interface wlan0)"
	valid_interface "$interface" || return 1
	WIFI_SCAN_DIR="$(mktemp -d "${RUN_DIR}/scan.XXXXXX")" || return 1
	if [ "$(conf_get "$WIFI_CONF" enabled 0)" = 0 ]; then
		WIFI_SCAN_RADIO_CHANGED=1
		wifi_stop || return 1
		bluetooth_service stop || return 1
		mode=wifi
		[ "$(conf_get "$BT_CONF" enabled 0)" = 0 ] || mode=combo
		S31_RADIO_VOLATILE_MODE="$mode" radio_service restart || return 1
		# Starting the controller directly avoids applying the disabled Wi-Fi
		# policy while the temporary combo mode is in use.
		bluetooth_service start || return 1
	else
		radio_prepare || return 1
	fi
	ip link show dev "$interface" 2>/dev/null | grep -q '<[^>]*UP[,>]' || WIFI_SCAN_LINK_CHANGED=1
	ip link set dev "$interface" up || return 1
	if ! wpa_cli -p /run/wpa_supplicant -i "$interface" ping 2>/dev/null | grep -q '^PONG$'; then
		printf 'ctrl_interface=/run/wpa_supplicant\nupdate_config=0\nap_scan=1\n' >"${WIFI_SCAN_DIR}/supplicant.conf"
		WIFI_SCAN_TEMP=1
		wpa_supplicant -B -D nl80211 -i "$interface" -c "${WIFI_SCAN_DIR}/supplicant.conf" \
			-P "$(wifi_pidfile "$interface")" >"${WIFI_SCAN_DIR}/supplicant.log" 2>&1 || return 1
	fi
	WIFI_SCAN_READY=1
}

wifi_scan_end()
{
	local result=0 interface
	interface="$(conf_get "$WIFI_CONF" interface wlan0)"
	if [ "${WIFI_SCAN_TEMP:-0}" = 1 ]; then stop_managed_pid "$(wifi_pidfile "$interface")" wpa_supplicant; fi
	if [ "${WIFI_SCAN_RADIO_CHANGED:-0}" = 1 ]; then
		radio_reload_and_apply_all || result=$?
	elif [ "${WIFI_SCAN_LINK_CHANGED:-0}" = 1 ]; then
		ip link set dev "$interface" down 2>/dev/null || result=1
	fi
	[ -z "${WIFI_SCAN_DIR:-}" ] || rm -rf "$WIFI_SCAN_DIR"
	WIFI_SCAN_DIR=''
	WIFI_SCAN_TEMP=0
	WIFI_SCAN_RADIO_CHANGED=0
	WIFI_SCAN_LINK_CHANGED=0
	WIFI_SCAN_READY=0
	return "$result"
}

wifi_scan_commit()
{
	local interface
	interface="$(conf_get "$WIFI_CONF" interface wlan0)"
	WIFI_SCAN_COMMITTED="${WIFI_SCAN_READY:-0}"
	if [ "${WIFI_SCAN_TEMP:-0}" = 1 ]; then
		stop_managed_pid "$(wifi_pidfile "$interface")" wpa_supplicant || return 1
	fi
	# The chosen profile is now saved. Keep the prepared controller and link
	# instead of cycling Bluetooth again between scanning and connecting.
	[ -z "${WIFI_SCAN_DIR:-}" ] || rm -rf "$WIFI_SCAN_DIR"
	WIFI_SCAN_DIR=''
	WIFI_SCAN_TEMP=0
	WIFI_SCAN_RADIO_CHANGED=0
	WIFI_SCAN_LINK_CHANGED=0
	WIFI_SCAN_READY=0
}

wifi_scan_collect()
{
	local destination="$1" interface count=0
	interface="$(conf_get "$WIFI_CONF" interface wlan0)"
	wpa_cli -p /run/wpa_supplicant -i "$interface" scan 2>/dev/null | grep -q '^OK$' || return 1
	# scan_results may initially contain the previous scan. A bounded delay
	# keeps the serial UI responsive; Refresh obtains a later result.
	sleep "${ESP32_CONFIG_SCAN_DELAY:-2}"
	wpa_cli -p /run/wpa_supplicant -i "$interface" scan_results >"${destination}.raw" 2>/dev/null || return 1
	wifi_scan_parse <"${destination}.raw" >"$destination"
	rm -f "${destination}.raw"
}

wifi_scan()
{
	local result=0 list hex signal security tab
	require_root
	ensure_config || return 1
	# Subshell traps restore the radio even when a CLI scan is interrupted.
	(
		trap 'wifi_scan_end >/dev/null 2>&1' 0
		trap 'exit 130' 1 2 3 15
		wifi_scan_begin || exit 1
		list="${WIFI_SCAN_DIR}/networks"
		wifi_scan_collect "$list" || exit 1
		tab="$(printf '\t')"
		while IFS="$tab" read -r hex signal security; do
			printf '%s dBm  %s  %s\n' "$signal" "$security" "$(wifi_hex_label "$hex")"
		done <"$list"
	)
}

wifi_choose_network()
{
	local choice list hex signal security tab number manual raw_ssid
	WIFI_SELECTED_HEX=''
	WIFI_SELECTED_SECURITY=''
	if [ "$(conf_get "$WIFI_CONF" enabled 0)" = 0 ] && [ "$(conf_get "$BT_CONF" enabled 0)" = 1 ]; then
		ui_dialog --title 'Find Wi-Fi networks' --yesno \
			'Scanning briefly reconnects Bluetooth. Continue?' 8 64 || return 1
	fi
	ui_dialog --title 'Wi-Fi' --infobox 'Finding nearby networks...' 6 60
	wifi_scan_begin || {
		wifi_scan_end >/dev/null 2>&1 || true
		ui_error 'The scan could not start. You can enter a network name manually.'
		WIFI_SCAN_DIR="$(mktemp -d "${RUN_DIR}/scan.XXXXXX")" || return 1
	}
	while :; do
		list="${WIFI_SCAN_DIR}/networks"
		wifi_scan_collect "$list" 2>/dev/null || : >"$list"
		set --
		number=0
		tab="$(printf '\t')"
		while IFS="$tab" read -r hex signal security; do
			number=$((number + 1))
			set -- "$@" "$number" "$(wifi_hex_label "$hex")  [$signal dBm, $security]"
		done <"$list"
		choice="$(ui_dialog --stdout --title 'Choose Wi-Fi network' --cancel-label Back --menu \
			'Select a network or enter its name manually.' 20 76 12 "$@" \
			manual 'Enter network name' refresh 'Refresh networks')" || { wifi_scan_end >/dev/null 2>&1; return 1; }
		case "$choice" in
			refresh) continue ;;
			manual)
				raw_ssid=''
				while :; do
					raw_ssid="$(ui_dialog --stdout --title 'Network name' --cancel-label Back --inputbox \
						'Enter the network name (SSID).' 9 68 "$raw_ssid")" || break
					hex="$(printf '%s' "$raw_ssid" | wifi_text_hex)"
					wifi_hex_valid "$hex" || { ui_error 'The network name must contain 1 to 32 bytes.'; continue; }
					security="$(ui_dialog --stdout --title 'Network security' --cancel-label Back --menu \
						'Choose the security used by this network.' 12 66 3 psk 'WPA/WPA2 Personal' open 'Open network')" || continue
					WIFI_SELECTED_HEX="$hex"
					WIFI_SELECTED_SECURITY="$security"
					break
				done
				[ -n "$WIFI_SELECTED_HEX" ] || continue
				;;
			*)
				case "$choice" in ''|*[!0-9]*) continue ;; esac
				hex="$(sed -n "${choice}p" "$list" | cut -f1)"
				security="$(sed -n "${choice}p" "$list" | cut -f3)"
				wifi_hex_valid "$hex" || continue
				if [ "$security" = unsupported ]; then
					ui_error 'This setup page supports open networks and WPA/WPA2 Personal. Select a supported network.'
					continue
				fi
				WIFI_SELECTED_HEX="$hex"
				WIFI_SELECTED_SECURITY="$security"
				;;
		esac
		return 0
	done
}

wifi_show_logs()
{
	local interface
	interface="$(conf_get "$WIFI_CONF" interface wlan0)"
	[ ! -f "${RUN_DIR}/wifi.message" ] || cat "${RUN_DIR}/wifi.message"
	[ ! -f "${RUN_DIR}/wifi-last-action.log" ] || tail -n 40 "${RUN_DIR}/wifi-last-action.log"
	[ ! -f "${RUN_DIR}/wpa_supplicant.log" ] || tail -n 40 "${RUN_DIR}/wpa_supplicant.log"
	[ ! -f "${RUN_DIR}/udhcpc.${interface}.log" ] || tail -n 40 "${RUN_DIR}/udhcpc.${interface}.log"
}

wifi_ui_connect()
{
	local old_enabled="$1" result
	ui_dialog --title 'Wi-Fi' --infobox 'Connecting to Wi-Fi. Please wait...' 6 64
	rm -f "${RUN_DIR}/wifi.message" "${RUN_DIR}/wifi.state"
	wifi_connect_saved "$old_enabled" >"${RUN_DIR}/wifi-last-action.log" 2>&1
	result=$?
	case "$result" in
		0) return 0 ;;
		2) ui_dialog --title 'Wi-Fi' --msgbox \
			"$(cat "${RUN_DIR}/wifi.message" 2>/dev/null)
You can leave this page while the connection continues." 9 70 ;;
		*)
			if [ "$(cat "${RUN_DIR}/wifi.state" 2>/dev/null)" = connected ]; then
				ui_error 'Wi-Fi connected, but another radio service could not be restored. View details for the error.'
			else
				ui_error "$(cat "${RUN_DIR}/wifi.message" 2>/dev/null || printf 'Settings saved, but the connection could not be started. View details for the error.')"
			fi
			;;
	esac
	return "$result"
}

wifi_setup_wizard()
(
	local hex security password='' old_enabled choice result saved error_file
	trap 'wifi_scan_end >/dev/null 2>&1 || warn "The previous radio settings could not be restored."' 0
	trap 'exit 130' 1 2 3 15
	wifi_choose_network || return
	hex="$WIFI_SELECTED_HEX"
	security="$WIFI_SELECTED_SECURITY"
	saved=0
	while :; do
		if [ "$security" = psk ] && [ "$saved" = 0 ]; then
			password="$(ui_dialog --stdout --title "$(wifi_hex_label "$hex")" --ok-label Connect --cancel-label Back \
				--passwordbox 'Enter the Wi-Fi password (8 to 63 bytes, or a 64-digit key).' 10 72 "$password")" || return
		elif [ "$security" = open ] && [ "$saved" = 0 ]; then
			choice="$(ui_dialog --stdout --title "$(wifi_hex_label "$hex")" --cancel-label Back --menu \
				'This is an open network.' 10 66 1 connect 'Connect')" || return
		fi
		old_enabled="$(conf_get "$WIFI_CONF" enabled 0)"
		if [ "$saved" = 0 ]; then
			error_file="${RUN_DIR}/wifi-validation.log"
			if ! wifi_save_hex_profile "$hex" "$password" "$security" 2>"$error_file"; then
				ui_error "$(cat "$error_file")"
				continue
			fi
			password=''
			saved=1
			if ! wifi_scan_commit; then ui_error 'The scan process could not be stopped. Settings were saved.'; return 1; fi
			[ "$WIFI_SCAN_COMMITTED" = 0 ] || old_enabled=1
		fi
		wifi_ui_connect "$old_enabled" && return
		while :; do
			choice="$(ui_dialog --stdout --title 'Wi-Fi connection' --cancel-label Back --menu \
				"$(wifi_status)" 17 72 6 retry 'Retry connection' password 'Change password' network 'Choose another network' details 'View details')" || return
			case "$choice" in
				retry) break ;;
				password) [ "$security" = psk ] || { ui_error 'This network has no password.'; continue; }; saved=0; break ;;
				network) wifi_choose_network || return; hex="$WIFI_SELECTED_HEX"; security="$WIFI_SELECTED_SECURITY"; saved=0; break ;;
				details) ui_show_command 'Wi-Fi details' wifi_show_logs ;;
			esac
		done
	done
)

wifi_configure()
{
	local ssid password second old_enabled
	require_root
	ensure_config || return 1
	printf 'SSID: '
	IFS= read -r ssid || return 1
	printf 'Passphrase (8-63 bytes): '
	read_secret || return 1
	password="$SECRET"; SECRET=''
	if [ ! -t 0 ]; then
		# Preserve the established three-line save-only automation interface.
		IFS= read -r second || return 1
		[ "$password" = "$second" ] || { warn 'Passphrases do not match.'; return 1; }
		wifi_save_profile "$ssid" "$password"
		return $?
	fi
	old_enabled="$(conf_get "$WIFI_CONF" enabled 0)"
	wifi_save_profile "$ssid" "$password" || return 1
	password=''
	wifi_connect_saved "$old_enabled"
}

wifi_menu()
{
	local choice enabled label
	while :; do
		enabled="$(conf_get "$WIFI_CONF" enabled 0)"
		[ "$enabled" = 1 ] && label='Turn Wi-Fi off' || label='Turn Wi-Fi on'
		choice="$(ui_dialog --stdout --title 'Wi-Fi' --cancel-label Back --menu \
			"$(wifi_status)${UI_NOTICE:+\n$UI_NOTICE}" 18 74 8 connect 'Connect / change network' enabled "$label" \
			retry 'Reconnect saved network' forget 'Forget saved network')" || return
		UI_NOTICE=''
		case "$choice" in
			connect) wifi_setup_wizard ;;
			enabled)
				if [ "$enabled" = 1 ]; then
					ui_dialog --title 'Turn Wi-Fi off' --yesno 'Disconnect Wi-Fi and turn it off?' 8 66 || continue
					ui_run_action 'Wi-Fi' 'Updating radio settings...' 'Wi-Fi is off.' wifi_set_enabled 0
				else
					[ -s "$WIFI_PROFILE" ] || { wifi_setup_wizard; continue; }
					conf_set "$WIFI_CONF" enabled 1 && wifi_ui_connect 0
				fi
				;;
			retry) [ -s "$WIFI_PROFILE" ] || { wifi_setup_wizard; continue; }; conf_set "$WIFI_CONF" enabled 1 && wifi_ui_connect "$enabled" ;;
			forget) ui_dialog --title 'Forget network' --yesno 'Disconnect and remove the saved network?' 8 66 && ui_run_action 'Wi-Fi' 'Removing network...' 'Saved network removed.' wifi_forget ;;
		esac
	done
}

network_status()
{
	local interface
	wifi_status
	printf '\n'
	interface="$(conf_get "$WIFI_CONF" interface wlan0)"
	if [ "$(conf_get "$WIFI_CONF" dhcp 1)" = 1 ]; then
		printf 'IPv4: Automatic (DHCP)\n'
	else
		printf 'IPv4: %s/%s\n' "$(conf_get "$WIFI_CONF" address 'Not configured')" "$(conf_get "$WIFI_CONF" prefix 24)"
		printf 'Gateway: %s\n' "$(conf_get "$WIFI_CONF" gateway 'None')"
	fi
	if [ "$(conf_get "$WIFI_CONF" dns_mode auto)" = auto ]; then printf 'DNS: Automatic\n'; else printf 'DNS: %s\n' "$(conf_get "$WIFI_CONF" dns_servers 'None')"; fi
	printf '\nCurrent addresses:\n'
	ip -4 address show dev "$interface" 2>/dev/null | awk '/ inet / {print $2}'
	printf 'Current routes:\n'
	ip -4 route show dev "$interface" 2>/dev/null
	printf 'Current DNS:\n'
	sed -n '/^nameserver /p' "${ESP32_CONFIG_RESOLV_CONF:-/etc/resolv.conf}" 2>/dev/null
}

network_apply_saved()
{
	if [ "$(conf_get "$WIFI_CONF" enabled 0)" = 1 ]; then wifi_apply; else printf 'Address settings saved. They will apply when Wi-Fi connects.\n'; fi
}

network_address_menu()
{
	local dhcp address prefix gateway dns_mode servers choice answer error_file changed result
	dhcp="$(conf_get "$WIFI_CONF" dhcp 1)"
	address="$(conf_get "$WIFI_CONF" address '')"
	prefix="$(conf_get "$WIFI_CONF" prefix 24)"
	gateway="$(conf_get "$WIFI_CONF" gateway '')"
	dns_mode="$(conf_get "$WIFI_CONF" dns_mode auto)"
	servers="$(conf_get "$WIFI_CONF" dns_servers '')"
	while :; do
		set -- mode "IPv4: $([ "$dhcp" = 1 ] && printf Automatic || printf Manual)"
		if [ "$dhcp" = 0 ]; then
			set -- "$@" address "Address: ${address:-Not set}" prefix "Prefix: $prefix" gateway "Gateway: ${gateway:-None}"
		fi
		set -- "$@" dns "DNS: $dns_mode" servers "DNS servers: ${servers:-None}" apply 'Save and apply'
		choice="$(ui_dialog --stdout --title 'Address settings' --cancel-label Back --menu \
			'Edit these settings, then choose Save and apply. Back discards this page.' 19 76 10 "$@")" || return
		case "$choice" in
			mode)
				answer="$(ui_dialog --stdout --title 'IPv4 addressing' --cancel-label Back --default-item "$dhcp" --menu 'Choose how to obtain an address.' 12 68 2 1 'Automatic (DHCP)' 0 'Manual')" || continue
				dhcp="$answer"; if [ "$dhcp" = 1 ]; then address=''; prefix=24; gateway=''; else dns_mode=manual; fi ;;
			address) answer="$(ui_dialog --stdout --title 'IPv4 address' --inputbox 'Device IPv4 address.' 9 66 "$address")" && address="$answer" ;;
			prefix) answer="$(ui_dialog --stdout --title 'Network prefix' --inputbox 'Prefix length (0 to 32), for example 24.' 9 66 "$prefix")" && prefix="$answer" ;;
			gateway) answer="$(ui_dialog --stdout --title 'Gateway' --inputbox 'Gateway IPv4 address. Leave empty for a local-only network.' 10 70 "$gateway")" && gateway="$answer" ;;
			dns)
				if [ "$dhcp" = 0 ]; then ui_error 'Manual addressing uses manual DNS. Leave the server list empty if DNS is not needed.'; continue; fi
				answer="$(ui_dialog --stdout --title 'DNS' --cancel-label Back --default-item "$dns_mode" --menu 'Choose DNS policy.' 12 66 2 auto 'Automatic from DHCP' manual 'Specify DNS servers')" && dns_mode="$answer" ;;
			servers) answer="$(ui_dialog --stdout --title 'DNS servers' --inputbox 'Up to three IPv4 addresses, separated by spaces. Empty means no DNS.' 10 72 "$servers")" && { servers="$answer"; dns_mode=manual; } ;;
			apply)
				error_file="${RUN_DIR}/network-validation.log"
				if ! network_validate_policy "$dhcp" "$address" "$prefix" "$gateway" "$dns_mode" "$servers" 2>"$error_file"; then ui_error "$(cat "$error_file")"; continue; fi
				if [ "$(conf_get "$WIFI_CONF" enabled 0)" = 1 ]; then
					ui_dialog --title 'Apply address settings' --yesno 'Reconnect Wi-Fi to apply these address settings?' 8 68 || continue
				fi
				network_save_policy "$dhcp" "$address" "$prefix" "$gateway" "$dns_mode" "$servers" || { ui_error 'Address settings could not be saved.'; continue; }
				ui_dialog --title 'Network' --infobox 'Applying address settings...' 6 64
				network_apply_saved >"${RUN_DIR}/network-apply.log" 2>&1
				result=$?
				[ "$result" -eq 0 ] || ui_dialog --title 'Network' --msgbox "$(cat "${RUN_DIR}/network-apply.log")" 14 74
				return ;;
		esac
	done
}

network_menu()
{
	local choice
	while :; do
		choice="$(ui_dialog --stdout --title 'Network' --cancel-label Back --menu \
			"$(wifi_status)${UI_NOTICE:+\n$UI_NOTICE}" 14 74 4 wifi 'Wi-Fi network' address 'IP address and DNS')" || return
		UI_NOTICE=''
		case "$choice" in wifi) wifi_menu ;; address) network_address_menu ;; esac
	done
}

wifi_cli()
{
	local action="${1:-}" ssid password old_enabled security
	[ "$#" -eq 0 ] || shift
	ensure_config || return 1
	case "$action" in
		status) [ "$#" -eq 0 ] || return 2; wifi_status ;;
		scan) [ "$#" -eq 0 ] || return 2; wifi_scan ;;
		configure) [ "$#" -eq 0 ] || return 2; wifi_configure ;;
		connect|configure-hex)
			[ "$#" -ge 1 ] && [ "$#" -le 2 ] || return 2
			ssid="$1"; security="${2:-psk}"
			case "$security" in psk|open) ;; *) return 2 ;; esac
			password=''
			if [ "$security" = psk ]; then
				[ ! -t 0 ] || printf 'Wi-Fi password: '
				read_secret || return 1
				password="$SECRET"; SECRET=''
			fi
			old_enabled="$(conf_get "$WIFI_CONF" enabled 0)"
			if [ "$action" = configure-hex ]; then wifi_save_hex_profile "$ssid" "$password" "$security" || return 1; else wifi_save_profile "$ssid" "$password" "$security" || return 1; fi
			password=''
			wifi_connect_saved "$old_enabled"
			;;
		enable) [ "$#" -eq 0 ] || return 2; wifi_set_enabled 1 ;;
		disable) [ "$#" -eq 0 ] || return 2; wifi_set_enabled 0 ;;
		forget) [ "$#" -eq 0 ] || return 2; wifi_forget ;;
		*) return 2 ;;
	esac
}

network_cli()
{
	local action="${1:-}" dns_mode=auto servers='' address prefix gateway
	[ "$#" -eq 0 ] || shift
	ensure_config || return 1
	case "$action" in
		status) [ "$#" -eq 0 ] || return 2; network_status ;;
		dhcp)
			if [ "$#" -gt 0 ]; then dns_mode="$1"; shift; fi
			servers="$*"
			network_save_policy 1 '' 24 '' "$dns_mode" "$servers" && network_apply_saved
			;;
		static)
			[ "$#" -ge 3 ] || return 2
			address="$1"; prefix="$2"; gateway="$3"; shift 3
			[ "$gateway" != - ] || gateway=''
			network_save_policy 0 "$address" "$prefix" "$gateway" manual "$*" && network_apply_saved
			;;
		*) return 2 ;;
	esac
}

wifi_validate_files()
{
	# Validate a staged backup without modifying it or starting services.
	local WIFI_CONF="$1" WIFI_PROFILE="$2" key value enabled hex profile_hex
	[ -f "$WIFI_CONF" ] || return 1
	LC_ALL=C awk -F= '
		/^[[:space:]]*$/ || /^#/ { next }
		{
			if (NF < 2 || $1 !~ /^(enabled|interface|dhcp|address|prefix|gateway|dns_mode|dns_servers|ssid_hex)$/ || seen[$1]++) exit 1
		}' "$WIFI_CONF" || return 1
	enabled="$(conf_get "$WIFI_CONF" enabled 0)"
	valid_enabled "$enabled" && valid_interface "$(conf_get "$WIFI_CONF" interface wlan0)" || return 1
	network_policy_valid || return 1
	hex="$(conf_get "$WIFI_CONF" ssid_hex '')"
	[ -z "$hex" ] || wifi_hex_valid "$hex" || return 1
	if [ ! -s "$WIFI_PROFILE" ]; then [ "$enabled" = 0 ]; return; fi
	LC_ALL=C awk '
		{ sub(/^[[:space:]]*/, ""); sub(/[[:space:]]*$/, "") }
		/^$/ { next }
		/^ctrl_interface=\/run\/wpa_supplicant$/ { if (inside || headers[$0]++) exit 1; next }
		/^update_config=0$/ || /^ap_scan=1$/ { if (inside || headers[$0]++) exit 1; next }
		/^network=\{$/ { if (blocks++ || inside) exit 1; inside=1; next }
		/^}$/ { if (!inside) exit 1; inside=0; closed++; next }
		inside && /^ssid=/ {
			if (ssid++) exit 1
			value=substr($0,6)
			if (value ~ /^[[:xdigit:]]+$/) {
				if (length(value)>64 || length(value)%2) exit 1
			} else if (value ~ /^"[^"\\]*"$/) {
				if (length(value)<3 || length(value)>34) exit 1
			} else exit 1
			next
		}
		inside && /^psk=[[:xdigit:]]+$/ { if (psk++ || length($0)!=68) exit 1; next }
		inside && /^key_mgmt=NONE$/ { if (open++) exit 1; next }
		inside && /^scan_ssid=1$/ { if (scan++) exit 1; next }
		{ exit 1 }
		END { if (blocks!=1 || closed!=1 || inside || ssid!=1 || psk+open!=1) exit 1 }
	' "$WIFI_PROFILE" || return 1
	if [ -n "$hex" ]; then
		profile_hex="$(sed -n 's/^[[:space:]]*ssid=//p' "$WIFI_PROFILE")"
		[ "$profile_hex" = "$hex" ] || return 1
	fi
	return 0
}

#!/usr/bin/env bash
# End-to-end test: drives the installed portal through xdg-desktop-portal with
# the same libportal/libei calls Deskflow uses, and checks the results in
# Hyprland and in a probe window. Takes over the pointer for a few seconds.
set -uo pipefail

cd "$(dirname "$0")/.."
CLIENT=build/rd-test-client
PROBE_APP=dev.hrdp.Probe
pass=0 failed=0

ok() { echo "  PASS  $*"; pass=$((pass + 1)); }
bad() { echo "  FAIL  $*"; failed=$((failed + 1)); }

for cmd in hyprctl jq python3; do
	command -v "$cmd" >/dev/null || { echo "missing $cmd"; exit 2; }
done
[[ -x $CLIENT ]] || { echo "build the client first: make build/rd-test-client"; exit 2; }

probe_log=$(mktemp)
cleanup() {
	[[ -n ${CLIENT_PID:-} ]] && kill "$CLIENT_PID" 2>/dev/null
	[[ -n ${PROBE_PID:-} ]] && kill "$PROBE_PID" 2>/dev/null
	rm -f "$probe_log"
}
trap cleanup EXIT

# ---- start the client ----------------------------------------------------
coproc RD { exec "$CLIENT"; }
CLIENT_PID=$RD_PID

reply=""
send() {
	echo "$*" >&"${RD[1]}"
	IFS= read -r -t 5 reply <&"${RD[0]}" || reply="(timeout)"
}

echo "connecting through the portal..."
while IFS= read -r -t 10 line <&"${RD[0]}"; do
	echo "  client: $line"
	[[ $line == READY ]] && break
	[[ $line == ERR* ]] && { bad "portal session: $line"; exit 1; }
done
[[ ${line:-} == READY ]] && ok "RemoteDesktop session + ConnectToEIS + devices ready" ||
	{ bad "client never became ready"; exit 1; }

# ---- absolute motion -----------------------------------------------------
send regions
echo "  regions: ${reply#OK }"
read -r ox oy < <(hyprctl monitors -j | jq -r '[min_by(.x).x, min_by(.y).y] | "\(.[0]) \(.[1])"')
# Hyprland reports monitor positions in logical units already
mapfile -t regions < <(tr ' ' '\n' <<<"${reply#OK }")

cursor() { hyprctl cursorpos | tr -d ' '; }

check_abs() { # region-x region-y label
	send abs "$1" "$2"
	sleep 0.1
	local want_x=$((ox + $1)) want_y=$((oy + $2)) got
	got=$(cursor)
	local gx=${got%,*} gy=${got#*,}
	if (( gx >= want_x - 1 && gx <= want_x + 1 && gy >= want_y - 1 && gy <= want_y + 1 )); then
		ok "absolute $3 -> $got"
	else
		bad "absolute $3: wanted $want_x,$want_y got $got"
	fi
}

for r in "${regions[@]}"; do
	IFS=, read -r rx ry rw rh rname <<<"$r"
	echo "  testing region $rname (${rw}x${rh} at $rx,$ry)"
	check_abs $((rx + rw / 2)) $((ry + rh / 2)) "$rname centre"
	check_abs "$rx" "$ry" "$rname top-left"
	check_abs $((rx + rw - 1)) "$ry" "$rname top-right"
	check_abs $((rx + rw - 1)) $((ry + rh / 2)) "$rname right edge"
	check_abs $((rx + rw - 1)) $((ry + rh - 1)) "$rname bottom-right"
	check_abs "$rx" $((ry + rh - 1)) "$rname bottom-left"
done

# ---- relative motion -----------------------------------------------------
IFS=, read -r rx ry rw rh _ <<<"${regions[0]}"
send abs $((rx + rw / 2)) $((ry + rh / 2))
sleep 0.1
before=$(cursor)
send rel 40 -25
sleep 0.1
after=$(cursor)
dx=$(( ${after%,*} - ${before%,*} )) dy=$(( ${after#*,} - ${before#*,} ))
(( dx >= 35 && dx <= 45 && dy <= -20 && dy >= -30 )) &&
	ok "relative motion (40,-25) moved by ($dx,$dy)" || bad "relative motion moved by ($dx,$dy)"

# ---- buttons, scroll and keys into the probe window ----------------------
python3 tests/probe-window.py >"$probe_log" 2>/dev/null &
PROBE_PID=$!
# wait until Hyprland has finished tiling the window
win="" prev=""
for _ in $(seq 50); do
	sleep 0.1
	win=$(hyprctl clients -j | jq -c --arg c "$PROBE_APP" '[.[] | select(.class == $c)][0] | {at, size}')
	[[ $win != *null* && $win == "$prev" ]] && break
	prev=$win
done
if [[ $win == *null* || -z $win ]]; then
	bad "probe window did not appear"
else
	read -r wx wy ww wh < <(jq -r '"\(.at[0]) \(.at[1]) \(.size[0]) \(.size[1])"' <<<"$win")
	for _ in $(seq 5); do
		send abs $((wx - ox + ww / 2)) $((wy - oy + wh / 2))
		sleep 0.2
		[[ $(hyprctl activewindow -j | jq -r .class) == "$PROBE_APP" ]] && break
	done
	active=$(hyprctl activewindow -j | jq -r .class)
	if [[ $active != "$PROBE_APP" ]]; then
		bad "probe window is not focused (focused: $active); skipping button/key tests"
	else
		send button 272 1; send button 272 0  # BTN_LEFT
		send button 273 1; send button 273 0  # BTN_RIGHT
		send scroll 120; send scroll -240
		send key 35 1; send key 35 0          # h
		send key 23 1; send key 23 0          # i
		send key 42 1; send key 30 1; send key 30 0; send key 42 0  # shift+a
		send key 45 1                         # x, deliberately left pressed
		sleep 0.3

		grep -q '^button-press 1$' "$probe_log" && grep -q '^button-release 1$' "$probe_log" &&
			ok "left click" || bad "left click"
		grep -q '^button-press 3$' "$probe_log" && grep -q '^button-release 3$' "$probe_log" &&
			ok "right click" || bad "right click"
		grep -q '^scroll 0 1$' "$probe_log" && ok "wheel down one detent" || bad "wheel down"
		grep -q '^scroll 0 -2$' "$probe_log" && ok "wheel up two detents" || bad "wheel up"
		grep -q '^key-press h 35$' "$probe_log" && grep -q '^key-release h 35$' "$probe_log" &&
			ok "key h" || bad "key h"
		grep -q '^key-press i 23$' "$probe_log" && ok "key i" || bad "key i"
		grep -q '^key-press A 30$' "$probe_log" && ok "shift+a gives A (modifiers work)" ||
			bad "shift+a did not give A"

		# disconnecting must release the key still held down
		send quit
		wait "$CLIENT_PID" 2>/dev/null
		CLIENT_PID=""
		sleep 0.5
		grep -q '^key-release x 45$' "$probe_log" &&
			ok "held key released when the client disconnects" || bad "held key stuck after disconnect"
	fi
fi

echo
echo "probe window events:"
sed 's/^/  /' "$probe_log"
echo
echo "$pass passed, $failed failed"
(( failed == 0 ))

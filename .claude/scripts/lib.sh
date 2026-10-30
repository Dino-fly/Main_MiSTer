# shellcheck shell=bash
# Shared helpers for the .claude/scripts tools. Sourced, never executed.
#
# Everything machine-specific (device addresses, credentials, local checks)
# lives OUTSIDE every repository, under ~/.config/classicui/. Nothing in
# this directory may contain an address, a password or a name.

CLASSICUI_CFG="${CLASSICUI_CFG:-$HOME/.config/classicui}"
DEVICES_ENV="$CLASSICUI_CFG/devices.env"
SS_ENV_FILE="${SS_ENV:-$CLASSICUI_CFG/ss.env}"
MD5_MARKER="$CLASSICUI_CFG/md5-verified"      # written by mister-deploy, read by the Bash hook
MAIN_REPO="$HOME/workspace/Main_MiSTer" # the checkout that must never be cleaned or switched
TOOLCHAIN="$MAIN_REPO/gcc-arm-10.2-2020.11-x86_64-arm-none-linux-gnueabihf"
BUILD_IMAGE="mister-build-amd64:ubuntu20"

MISTER_HOST=""; MISTER_USER="root"; WINBOX_HOST=""; WINBOX_USER=""; WINBOX_FP=""

load_devices() {
	# shellcheck disable=SC1090
	[ -f "$DEVICES_ENV" ] && . "$DEVICES_ENV"
	: "${MISTER_USER:=root}"
}

need_mister() {
	load_devices
	[ -n "$MISTER_HOST" ] || die "MISTER_HOST is empty in $DEVICES_ENV - run mister-find --write" 2
}

# OpenSSH 10 prints a three-line post-quantum warning on stderr for every server
# whose key exchange is classical; the device's dropbear is one. Drop only those lines.
pq_filter() {
	grep -Ev '^\*\* (WARNING: connection is not using a post-quantum|This session may be vulnerable|The server may need to be upgraded|See https://openssh\.com/pq\.html)' || true
}

SSH_OPTS=(-n -o BatchMode=yes -o ConnectTimeout=6 -o StrictHostKeyChecking=accept-new -o LogLevel=ERROR)

# mister_ssh <remote command...>  - key auth only, never a password, stdin closed.
mister_ssh() {
	ssh "${SSH_OPTS[@]}" "${MISTER_USER}@${MISTER_HOST}" "$@" 2> >(pq_filter >&2)
}

# mister_scp <local> <remote-path>   /  mister_scp_from <remote-path> <local>
mister_scp()      { scp -q -o BatchMode=yes -o ConnectTimeout=6 -o LogLevel=ERROR "$1" "${MISTER_USER}@${MISTER_HOST}:$2" 2> >(pq_filter >&2); }
mister_scp_from() { scp -q -o BatchMode=yes -o ConnectTimeout=6 -o LogLevel=ERROR "${MISTER_USER}@${MISTER_HOST}:$1" "$2" 2> >(pq_filter >&2); }

# Port test with a real connect timeout. macOS nc: -G is the connect timeout; -w alone
# is an idle timeout and lets a silent host stall the sweep for half a minute.
port_open() { nc -z -G 1 -w 1 "$1" "$2" >/dev/null 2>&1; }

local_md5() { md5 -q "$1"; }

die() { echo "FAIL: $1" >&2; exit "${2:-1}"; }

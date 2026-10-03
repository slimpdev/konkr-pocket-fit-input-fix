#!/usr/bin/env bash
set -Eeuo pipefail

VERSION="0.2.0"
REPO="slimpdev/konkr-pocket-fit-input-fix"
RAW_BASE="https://raw.githubusercontent.com/${REPO}/main"

BIN="/usr/local/sbin/konkr-input-fix"
SERVICE="/etc/systemd/system/konkr-input-fix.service"
SERVICE_DROPIN_DIR="/etc/systemd/system/konkr-input-fix.service.d"
SERVICE_DROPIN="$SERVICE_DROPIN_DIR/10-clean-ready.conf"
IP_DROPIN_DIR="/etc/systemd/system/inputplumber.service.d"
IP_DROPIN="$IP_DROPIN_DIR/20-konkr-input-fix.conf"
IP_DIR="/etc/inputplumber/devices.d"
IGNORE_CFG="$IP_DIR/00-konkr-input-fix-ignore-raw.yaml"
ELITE_CFG="$IP_DIR/40-konkr-pocket-fit-elite-filtered.yaml"
READY="/run/konkr-input-fix.ready"
FILTERED_NAME="KONKR Filtered Gamepad"

log()  { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
ok()   { printf '\033[1;32m OK\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33mWARN\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31mERR\033[0m %s\n' "$*" >&2; exit 1; }

usage() {
    cat <<EOF_USAGE
KONKR Pocket FIT family input fix installer v$VERSION

Supports:
  - KONKR Pocket FIT (SM8650; XInput or DirectInput controller mode)
  - KONKR Pocket FIT Elite (SM8750)

Usage:
  sudo ./install.sh             Install/update the fix
  sudo ./install.sh --status    Show current status
  sudo ./install.sh --uninstall Remove the fix
  sudo ./install.sh --force     Skip model compatibility check
EOF_USAGE
}

ensure_root() {
    if [[ $EUID -eq 0 ]]; then return; fi
    if [[ -f "${BASH_SOURCE[0]:-}" ]]; then
        exec sudo bash "${BASH_SOURCE[0]}" "$@"
    fi
    die "Run this installer as root (for example: curl ... | sudo bash)."
}

read_dt() {
    local p="$1"
    [[ -r "$p" ]] || return 0
    tr '\0' '\n' <"$p" 2>/dev/null || true
}

detect_variant() {
    local compat model
    compat="$(read_dt /sys/firmware/devicetree/base/compatible)"
    model="$(read_dt /sys/firmware/devicetree/base/model | head -n1)"

    if grep -qx 'konkr,pocket-fit-elite' <<<"$compat" || [[ "$model" == *"KONKR Pocket FIT Elite"* ]]; then
        printf 'elite\n'
        return 0
    fi
    if grep -qx 'konkr,pocket-fit' <<<"$compat" || [[ "$model" == "KONKR Pocket FIT" ]]; then
        printf 'fit\n'
        return 0
    fi
    return 1
}

choose_compiler() {
    local cc
    for cc in cc gcc clang; do
        if command -v "$cc" >/dev/null 2>&1; then
            command -v "$cc"
            return 0
        fi
    done
    return 1
}

find_named_event() {
    local wanted="$1" e name
    for e in /sys/class/input/event*; do
        [[ -e "$e/device/name" ]] || continue
        name="$(cat "$e/device/name" 2>/dev/null || true)"
        [[ "$name" == "$wanted" ]] && { basename "$e"; return 0; }
    done
    return 1
}

check_prereqs() {
    [[ -e /dev/uinput ]] || modprobe uinput 2>/dev/null || true
    [[ -e /dev/uinput ]] || die "/dev/uinput is not available."
    command -v inputplumber >/dev/null 2>&1 || die "InputPlumber is not installed."
    command -v systemctl >/dev/null 2>&1 || die "systemd is required."
}

get_source() {
    local out="$1" local_source=""

    if [[ -n "${KONKR_FIX_SOURCE:-}" ]]; then
        local_source="$KONKR_FIX_SOURCE"
    elif [[ -n "${BASH_SOURCE[0]:-}" && -f "${BASH_SOURCE[0]}" ]]; then
        local_source="$(cd "$(dirname "${BASH_SOURCE[0]}")" 2>/dev/null && pwd)/src/konkr-input-fix.c"
    fi

    if [[ -n "$local_source" && -f "$local_source" ]]; then
        cp "$local_source" "$out"
        return 0
    fi

    command -v curl >/dev/null 2>&1 || die "curl is required when installing without a cloned repository."
    log "Downloading filter source from GitHub"
    curl -fsSL "$RAW_BASE/src/konkr-input-fix.c" -o "$out" || die "Failed to download filter source."
}

write_ignore_config() {
    local variant="$1"
    mkdir -p "$IP_DIR"

    if [[ "$variant" == "elite" ]]; then
        cat >"$IGNORE_CFG" <<'EOF_YAML'
version: 1
kind: CompositeDevice
name: KONKR Pocket FIT Elite - Ignore Raw Controller

options:
    auto_manage: true

matches:
    - udev:
          sys_path: /sys/firmware/devicetree/base
          attributes:
              - name: compatible
                value: konkr,pocket-fit-elite

source_devices:
    - group: gamepad
      ignore: true
      evdev:
          name: AYANEO MCU Gamepad
          handler: event*
EOF_YAML
    else
        cat >"$IGNORE_CFG" <<'EOF_YAML'
version: 1
kind: CompositeDevice
name: KONKR Pocket FIT - Ignore Raw Controller

options:
    auto_manage: true

matches:
    - udev:
          sys_path: /sys/firmware/devicetree/base
          attributes:
              - name: model
                value: KONKR Pocket FIT

source_devices:
    - group: gamepad
      ignore: true
      evdev:
          vendor_id: "4001"
          product_id: "0428"
          handler: event*
    - group: gamepad
      ignore: true
      evdev:
          vendor_id: "045e"
          product_id: "028e"
          handler: event*
EOF_YAML
    fi
}

write_elite_config() {
    local variant="$1"
    if [[ "$variant" != "elite" ]]; then
        rm -f "$ELITE_CFG"
        return
    fi

    cat >"$ELITE_CFG" <<'EOF_YAML'
version: 1
kind: CompositeDevice
name: KONKR Pocket FIT Elite Filtered

maximum_sources: 0
options:
    auto_manage: true

matches:
    - udev:
          sys_path: /sys/firmware/devicetree/base
          attributes:
              - name: compatible
                value: konkr,pocket-fit-elite

source_devices:
    - group: gamepad
      evdev:
          name: KONKR Filtered Gamepad
          handler: event*
    - group: keyboard
      capability_map_id: konkr3
      evdev:
          name: KONKR System Buttons
          handler: event*

target_devices:
    - xbox-elite
    - keyboard
    - mouse
EOF_YAML
}

write_systemd() {
    local variant="$1"

    cat >"$SERVICE" <<EOF_UNIT
[Unit]
Description=KONKR Pocket FIT input debounce filter
Before=inputplumber.service

[Service]
Type=simple
Environment=KONKR_VARIANT=$variant
ExecStart=$BIN
ExecStartPost=/bin/sh -c 'for i in \$(seq 1 100); do [ -e $READY ] && exit 0; sleep 0.05; done; exit 1'
Restart=always
RestartSec=1

[Install]
WantedBy=multi-user.target
EOF_UNIT

    mkdir -p "$SERVICE_DROPIN_DIR" "$IP_DROPIN_DIR"
    cat >"$SERVICE_DROPIN" <<EOF_UNIT
[Service]
ExecStartPre=/usr/bin/rm -f $READY
EOF_UNIT

    cat >"$IP_DROPIN" <<'EOF_UNIT'
[Unit]
Requires=konkr-input-fix.service
After=konkr-input-fix.service
EOF_UNIT
}

status_cmd() {
    local variant="unknown"
    variant="$(detect_variant 2>/dev/null || true)"
    echo "KONKR variant: ${variant:-unknown}"
    echo "binary: $($BIN --version 2>/dev/null || echo not-installed)"
    echo "konkr-input-fix: $(systemctl is-active konkr-input-fix 2>/dev/null || true)"
    echo "inputplumber:    $(systemctl is-active inputplumber 2>/dev/null || true)"
    echo
    echo "Input devices:"
    for e in /sys/class/input/event*; do
        [[ -r "$e/device/name" ]] || continue
        printf '%-9s %s\n' "${e##*/}" "$(cat "$e/device/name" 2>/dev/null || true)"
    done
    echo
    echo "InputPlumber hidden nodes:"
    find /dev/inputplumber/by-hidden -maxdepth 1 -type l -printf '%p -> %l\n' 2>/dev/null | sort || true
    echo
    echo "Recent logs:"
    journalctl -u konkr-input-fix -u inputplumber -b --no-pager 2>/dev/null | tail -40 || true
}

uninstall_cmd() {
    log "Stopping InputPlumber and removing KONKR input fix"
    systemctl stop inputplumber 2>/dev/null || true
    systemctl disable --now konkr-input-fix.service 2>/dev/null || true
    rm -f "$SERVICE" "$SERVICE_DROPIN" "$IP_DROPIN" "$IGNORE_CFG" "$ELITE_CFG" "$BIN" "$READY"
    rmdir "$SERVICE_DROPIN_DIR" "$IP_DROPIN_DIR" 2>/dev/null || true
    systemctl daemon-reload
    systemctl reset-failed konkr-input-fix.service inputplumber.service 2>/dev/null || true
    systemctl start inputplumber 2>/dev/null || true
    ok "Removed. InputPlumber returned to its stock controller path."
}

install_cmd() {
    local force="$1" variant cc tmp filtered

    if ! variant="$(detect_variant)"; then
        [[ "$force" == 1 ]] || die "Unsupported device. Expected KONKR Pocket FIT or Pocket FIT Elite."
        warn "Unable to identify model; --force defaults to Elite source handling."
        variant="elite"
    fi

    check_prereqs
    cc="$(choose_compiler)" || die "A C compiler is required (cc, gcc, or clang)."
    log "Detected KONKR variant: $variant"

    systemctl stop inputplumber 2>/dev/null || true
    systemctl stop konkr-input-fix.service 2>/dev/null || true

    tmp="$(mktemp -d)"
    trap 'rm -rf "${tmp:-}"' EXIT
    get_source "$tmp/konkr-input-fix.c"

    log "Compiling userspace filter with $cc"
    "$cc" -O2 -Wall -Wextra "$tmp/konkr-input-fix.c" -o "$tmp/konkr-input-fix"
    install -m755 "$tmp/konkr-input-fix" "$BIN"

    write_ignore_config "$variant"
    write_elite_config "$variant"
    write_systemd "$variant"

    systemctl daemon-reload
    systemctl enable konkr-input-fix.service >/dev/null
    systemctl restart konkr-input-fix.service
    systemctl restart inputplumber.service
    sleep 1

    systemctl is-active --quiet konkr-input-fix.service || die "konkr-input-fix failed to start. Run: journalctl -u konkr-input-fix -b"
    systemctl is-active --quiet inputplumber.service || die "InputPlumber failed to start. Run: journalctl -u inputplumber -b"
    [[ -e "$READY" ]] || warn "Ready marker is missing even though the service is active."

    filtered="$(find_named_event "$FILTERED_NAME" || true)"
    [[ -n "$filtered" ]] || die "Filtered virtual gamepad was not created."

    ok "Installed KONKR Pocket FIT input fix v$VERSION ($variant)."
    echo "Filtered controller: $filtered"
    echo "Run: sudo $0 --status"
}

main() {
    local arg="${1:-}" force=0
    case "$arg" in
        -h|--help) usage; exit 0 ;;
        --status) ensure_root "$@"; status_cmd; exit 0 ;;
        --uninstall) ensure_root "$@"; uninstall_cmd; exit 0 ;;
        --force) force=1 ;;
        "") ;;
        *) usage; exit 2 ;;
    esac
    ensure_root "$@"
    install_cmd "$force"
}

main "$@"

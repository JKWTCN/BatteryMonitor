#!/usr/bin/env bash

set -euo pipefail

readonly RULE_FILE="/etc/udev/rules.d/70-batterymonitor.rules"

rules_content() {
    cat <<'EOF'
# BatteryMonitor: allow the active desktop user to access supported HID devices.

# ASUS / ROG
KERNEL=="hidraw*", SUBSYSTEM=="hidraw", ATTRS{idVendor}=="0b05", TAG+="uaccess"

# Logitech
KERNEL=="hidraw*", SUBSYSTEM=="hidraw", ATTRS{idVendor}=="046d", TAG+="uaccess"

# Lenovo-branded Logitech receiver
KERNEL=="hidraw*", SUBSYSTEM=="hidraw", ATTRS{idVendor}=="17ef", TAG+="uaccess"

# Razer
KERNEL=="hidraw*", SUBSYSTEM=="hidraw", ATTRS{idVendor}=="1532", TAG+="uaccess"
EOF
}

usage() {
    cat <<EOF
Usage: $0 [install|uninstall|status|print]

  install    Install the BatteryMonitor udev rules (default)
  uninstall  Remove the installed rules
  status     Show whether the rules are installed
  print      Print the rules without changing the system
EOF
}

as_root() {
    if (( EUID == 0 )); then
        "$@"
    elif command -v sudo >/dev/null 2>&1; then
        sudo "$@"
    else
        echo "Error: this operation requires root privileges, but sudo was not found." >&2
        exit 1
    fi
}

reload_udev() {
    if ! command -v udevadm >/dev/null 2>&1; then
        echo "Warning: udevadm was not found; reload the udev rules manually." >&2
        return
    fi

    as_root udevadm control --reload-rules
    as_root udevadm trigger --subsystem-match=hidraw
}

install_rules() {
    local temp_file
    temp_file="$(mktemp)"
    rules_content >"$temp_file"

    if ! as_root install -Dm0644 "$temp_file" "$RULE_FILE"; then
        rm -f -- "$temp_file"
        return 1
    fi
    rm -f -- "$temp_file"
    reload_udev

    echo "Installed: $RULE_FILE"
    echo "Please unplug and reconnect the wireless receiver, then restart BatteryMonitor."
}

uninstall_rules() {
    if [[ -e "$RULE_FILE" ]]; then
        as_root rm -f -- "$RULE_FILE"
        reload_udev
        echo "Removed: $RULE_FILE"
    else
        echo "Not installed: $RULE_FILE"
    fi
}

show_status() {
    if [[ -f "$RULE_FILE" ]]; then
        echo "Installed: $RULE_FILE"
        cat "$RULE_FILE"
    else
        echo "Not installed: $RULE_FILE"
        return 1
    fi
}

case "${1:-install}" in
    install)
        install_rules
        ;;
    uninstall)
        uninstall_rules
        ;;
    status)
        show_status
        ;;
    print)
        rules_content
        ;;
    -h|--help|help)
        usage
        ;;
    *)
        echo "Unknown command: $1" >&2
        usage >&2
        exit 2
        ;;
esac

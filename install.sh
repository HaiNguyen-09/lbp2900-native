#!/usr/bin/env bash
#
# Canon LASER SHOT LBP-2900 Standalone CAPT Driver Suite
# One-Click Automated Installer (Debian, Ubuntu, Linux Mint)
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

CUSTOM_ICON=""
ACTION="install"
FORCE_DEPS=0

for arg in "$@"; do
    case "$arg" in
        --uninstall)
            ACTION="uninstall"
            ;;
        --deps)
            FORCE_DEPS=1
            ;;
        --icon=*)
            CUSTOM_ICON="${arg#*=}"
            ;;
        --help|-h)
            echo "Usage: ./install.sh [OPTIONS]"
            echo ""
            echo "Options:"
            echo "  --icon=/path/to/icon.png   Use a custom image for the desktop and status monitor icon"
            echo "  --deps                     Force installing package dependencies via apt"
            echo "  --uninstall                Revert changes and uninstall the driver suite"
            echo "  --help, -h                 Show this help message"
            exit 0
            ;;
        *)
            if [[ -f "$arg" ]]; then
                CUSTOM_ICON="$arg"
            fi
            ;;
    esac
done

# If no custom icon specified, check user download or repository asset
if [[ -z "$CUSTOM_ICON" ]]; then
    if [[ -f "/home/phoenix/Downloads/IMG_0664.PNG" ]]; then
        CUSTOM_ICON="/home/phoenix/Downloads/IMG_0664.PNG"
    elif [[ -f "$SCRIPT_DIR/icons/canon-lbp2900-master.png" ]]; then
        CUSTOM_ICON="$SCRIPT_DIR/icons/canon-lbp2900-master.png"
    fi
fi

# Helper to run root commands safely (only if root or passwordless/prompted sudo)
run_as_root() {
    if [ "$(id -u)" -eq 0 ]; then
        "$@"
    elif command -v sudo >/dev/null 2>&1; then
        sudo "$@"
    else
        echo "Warning: root privileges needed for '$*'; skipping." >&2
    fi
}

has_sudo() {
    if [ "$(id -u)" -eq 0 ]; then
        return 0
    fi
    if sudo -n true 2>/dev/null; then
        return 0
    fi
    return 1
}

# -----------------------------------------------------------------------------
# UNINSTALL LOGIC
# -----------------------------------------------------------------------------
if [[ "$ACTION" == "uninstall" ]]; then
    echo "=== Uninstalling Canon LBP-2900 Driver Suite ==="
    
    # Stop user service
    systemctl --user stop lbp2900d.service 2>/dev/null || true
    systemctl --user disable lbp2900d.service 2>/dev/null || true
    rm -f "$HOME/.config/systemd/user/lbp2900d.service"
    systemctl --user daemon-reload 2>/dev/null || true

    # Remove user local files
    rm -f "$HOME/.local/share/applications/canon-lbp2900.desktop"
    rm -f "$HOME/.cups/client.conf"
    for sz in 16 24 32 48 64 128 256 512; do
        rm -f "$HOME/.local/share/icons/hicolor/${sz}x${sz}/apps/canon-lbp2900.png"
    done
    gtk-update-icon-cache -f -t "$HOME/.local/share/icons/hicolor" 2>/dev/null || true

    # Remove system files if sudo is accessible
    if has_sudo || [ "$(id -u)" -eq 0 ]; then
        echo "Removing system files..."
        run_as_root rm -f /usr/local/bin/lbp2900d /usr/local/bin/lbp2900-ui /usr/local/bin/lbp2900ctl
        run_as_root rm -f /usr/local/bin/lp /usr/local/bin/lpr /usr/local/bin/lpstat /usr/local/bin/lpq /usr/local/bin/lprm /usr/local/bin/cancel
        run_as_root rm -f /etc/udev/rules.d/99-canon-lbp2900.rules
        run_as_root rm -f /usr/share/applications/canon-lbp2900.desktop
        run_as_root rm -f /usr/share/pixmaps/canon-lbp2900.png
        for sz in 16 24 32 48 64 128 256 512; do
            run_as_root rm -f "/usr/share/icons/hicolor/${sz}x${sz}/apps/canon-lbp2900.png"
        done
        run_as_root rm -f /etc/cups/client.conf
        run_as_root systemctl unmask cups.service cups.socket cups.path 2>/dev/null || true
        run_as_root udevadm control --reload-rules 2>/dev/null || true
        run_as_root udevadm trigger 2>/dev/null || true
    fi

    echo "=== Uninstallation complete. Old CUPS can now be re-enabled if desired with: ==="
    echo "  sudo systemctl enable --now cups"
    exit 0
fi

# -----------------------------------------------------------------------------
# INSTALL LOGIC
# -----------------------------------------------------------------------------
echo "=========================================================================="
echo "  Canon LASER SHOT LBP-2900 Standalone Driver Suite — Automated Installer"
echo "=========================================================================="
echo ""

# 1. Check Dependencies / Build Binaries
echo "[1/6] Compiling driver, GUI monitor, and CLI utilities..."
if ! make all >/dev/null 2>&1 || [ "$FORCE_DEPS" -eq 1 ]; then
    if command -v apt-get >/dev/null 2>&1; then
        echo "  -> Compiling requires development packages. Installing via apt-get..."
        run_as_root apt-get update -qq
        run_as_root apt-get install -y build-essential pkg-config ghostscript \
            libusb-1.0-0 libusb-1.0-0-dev libcups2-dev libcupsimage2-dev \
            libgtk-4-1 libgtk-4-dev libcairo2 libcairo2-dev python3-gi gir1.2-gdkpixbuf-2.0
        make clean && make all
    else
        echo "Error: make failed and apt-get is not available. Please install dependencies." >&2
        exit 1
    fi
fi
echo "  -> Binaries successfully built in $SCRIPT_DIR/bin/"

# 2. Process and Install Multi-Resolution Icons
echo "[2/6] Processing and installing application icon..."
if [[ -n "$CUSTOM_ICON" && -f "$CUSTOM_ICON" ]]; then
    echo "  -> Processing master icon: $CUSTOM_ICON"
    python3 "$SCRIPT_DIR/scripts/process_icon.py" "$CUSTOM_ICON" "$SCRIPT_DIR/icons"
fi

# Install icons into user directory
for sz in 16 24 32 48 64 128 256 512; do
    if [[ -f "$SCRIPT_DIR/icons/hicolor/${sz}x${sz}/apps/canon-lbp2900.png" ]]; then
        mkdir -p "$HOME/.local/share/icons/hicolor/${sz}x${sz}/apps"
        cp "$SCRIPT_DIR/icons/hicolor/${sz}x${sz}/apps/canon-lbp2900.png" "$HOME/.local/share/icons/hicolor/${sz}x${sz}/apps/"
        if has_sudo || [ "$(id -u)" -eq 0 ]; then
            run_as_root mkdir -p "/usr/share/icons/hicolor/${sz}x${sz}/apps"
            run_as_root cp "$SCRIPT_DIR/icons/hicolor/${sz}x${sz}/apps/canon-lbp2900.png" "/usr/share/icons/hicolor/${sz}x${sz}/apps/"
        fi
    fi
done

if has_sudo || [ "$(id -u)" -eq 0 ]; then
    run_as_root mkdir -p /usr/share/pixmaps
    if [[ -f "$SCRIPT_DIR/icons/pixmaps/canon-lbp2900.png" ]]; then
        run_as_root cp "$SCRIPT_DIR/icons/pixmaps/canon-lbp2900.png" /usr/share/pixmaps/
    fi
    run_as_root gtk-update-icon-cache -f -t /usr/share/icons/hicolor 2>/dev/null || true
fi
gtk-update-icon-cache -f -t "$HOME/.local/share/icons/hicolor" 2>/dev/null || true
echo "  -> Multi-resolution icons installed (16x16 to 512x512)."

# 3. Install Desktop Application Launcher
echo "[3/6] Installing desktop launcher (Canon LBP2900 Status Monitor)..."
mkdir -p "$HOME/.local/share/applications"
cat > "$HOME/.local/share/applications/canon-lbp2900.desktop" << EOF
[Desktop Entry]
Version=1.0
Type=Application
Name=Canon LBP2900 Status Monitor
GenericName=Printer Status Monitor
Comment=Standalone Canon LASER SHOT LBP-2900 Status Monitor & Controller
Exec=$SCRIPT_DIR/bin/lbp2900-ui
Icon=canon-lbp2900
Terminal=false
Categories=System;Utility;Printing;HardwareSettings;
StartupNotify=true
StartupWMClass=canon-lbp2900
Keywords=printer;canon;lbp2900;laser;print;
EOF

if has_sudo || [ "$(id -u)" -eq 0 ]; then
    run_as_root cp "$HOME/.local/share/applications/canon-lbp2900.desktop" /usr/share/applications/
    run_as_root update-desktop-database /usr/share/applications 2>/dev/null || true
fi
update-desktop-database "$HOME/.local/share/applications" 2>/dev/null || true
echo "  -> Desktop entry installed to ~/.local/share/applications/canon-lbp2900.desktop"

# 4. Configure Client Printing Redirection & Symlinks
echo "[4/6] Setting up client printing routing and command-line symlinks..."
mkdir -p "$HOME/.cups"
echo "ServerName /tmp/lbp2900-cups.sock" > "$HOME/.cups/client.conf"

mkdir -p "$HOME/.local/bin"
ln -sf "$SCRIPT_DIR/bin/lbp2900d" "$HOME/.local/bin/lbp2900d"
ln -sf "$SCRIPT_DIR/bin/lbp2900-ui" "$HOME/.local/bin/lbp2900-ui"
ln -sf "$SCRIPT_DIR/bin/lbp2900ctl" "$HOME/.local/bin/lbp2900ctl"
ln -sf "$SCRIPT_DIR/bin/lbp2900ctl" "$HOME/.local/bin/lp"
ln -sf "$SCRIPT_DIR/bin/lbp2900ctl" "$HOME/.local/bin/lpr"
ln -sf "$SCRIPT_DIR/bin/lbp2900ctl" "$HOME/.local/bin/lpstat"
ln -sf "$SCRIPT_DIR/bin/lbp2900ctl" "$HOME/.local/bin/lpq"
ln -sf "$SCRIPT_DIR/bin/lbp2900ctl" "$HOME/.local/bin/lprm"
ln -sf "$SCRIPT_DIR/bin/lbp2900ctl" "$HOME/.local/bin/cancel"

if has_sudo || [ "$(id -u)" -eq 0 ]; then
    echo "ServerName /tmp/lbp2900-cups.sock" | run_as_root tee /etc/cups/client.conf >/dev/null
    run_as_root ln -sf "$SCRIPT_DIR/bin/lbp2900d" /usr/local/bin/lbp2900d
    run_as_root ln -sf "$SCRIPT_DIR/bin/lbp2900-ui" /usr/local/bin/lbp2900-ui
    run_as_root ln -sf "$SCRIPT_DIR/bin/lbp2900ctl" /usr/local/bin/lbp2900ctl
    run_as_root ln -sf "$SCRIPT_DIR/bin/lbp2900ctl" /usr/local/bin/lp
    run_as_root ln -sf "$SCRIPT_DIR/bin/lbp2900ctl" /usr/local/bin/lpr
    run_as_root ln -sf "$SCRIPT_DIR/bin/lbp2900ctl" /usr/local/bin/lpstat
    run_as_root ln -sf "$SCRIPT_DIR/bin/lbp2900ctl" /usr/local/bin/lpq
    run_as_root ln -sf "$SCRIPT_DIR/bin/lbp2900ctl" /usr/local/bin/lprm
    run_as_root ln -sf "$SCRIPT_DIR/bin/lbp2900ctl" /usr/local/bin/cancel

    # Configure udev rules
    run_as_root tee /etc/udev/rules.d/99-canon-lbp2900.rules >/dev/null << 'UDEV_EOF'
# Canon LASER SHOT LBP-2900 (04a9:2676) direct user-space CAPT access
SUBSYSTEM=="usb", ATTR{idVendor}=="04a9", ATTR{idProduct}=="2676", MODE="0666", GROUP="lp"
UDEV_EOF
    run_as_root udevadm control --reload-rules
    run_as_root udevadm trigger

    # Stop and disable cups services
    run_as_root systemctl stop cups.service cups.socket cups.path cups-browsed.service 2>/dev/null || true
    run_as_root systemctl disable cups.service cups.socket cups.path cups-browsed.service 2>/dev/null || true
    run_as_root systemctl mask cups.service cups.socket cups.path 2>/dev/null || true
fi

# 5. Configure systemd User Service
echo "[5/6] Starting lbp2900d user background service..."
mkdir -p "$HOME/.config/systemd/user"
cat > "$HOME/.config/systemd/user/lbp2900d.service" << EOF
[Unit]
Description=Canon LASER SHOT LBP-2900 Standalone CAPT Print Daemon (No-CUPS Engine)
After=graphical-session.target

[Service]
Type=simple
WorkingDirectory=$SCRIPT_DIR
ExecStart=$SCRIPT_DIR/bin/lbp2900d --ui-bin $SCRIPT_DIR/bin/lbp2900-ui
Restart=always
RestartSec=2
Environment=DISPLAY=:0
Environment=XDG_RUNTIME_DIR=/run/user/%U

[Install]
WantedBy=default.target
EOF

systemctl --user daemon-reload
systemctl --user enable --now lbp2900d.service
sleep 1

# 6. Verification
echo "[6/6] Verifying installation with test suite..."
"$SCRIPT_DIR/bin/verify_harness"

echo ""
echo "=========================================================================="
echo "  INSTALLATION SUCCESSFUL!"
echo "=========================================================================="
echo "  Icon:            Added from $CUSTOM_ICON"
echo "  Status Monitor:  Open from App Launcher or run: ./bin/lbp2900ctl ui"
echo "  Check Status:    Run: ./bin/lbp2900ctl status"
echo "  Print Files:     Run: lp document.pdf  (or click Print in any app)"
echo "  To Uninstall:    Run: ./install.sh --uninstall"
echo "=========================================================================="

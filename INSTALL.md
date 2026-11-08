# **Canon LASER SHOT LBP-2900 Standalone Driver Suite — Installation Guide**

A lightweight (`~6.5 MB` RAM) standalone print server, desktop Status Monitor GUI, and CLI controller for the Canon LASER SHOT LBP-2900 (USB ID 04a9:2676) on Linux. It completely replaces `CUPS` (`cupsd`) and `ccpd`.

## You can ask your agent read this and install it for you if you dont want to read

# **Table of Contents**

* [1\. Prerequisites & Dependencies](#1-prerequisites--dependencies)  
* [2\. Build the Software](#2-build-the-software)  
* [3\. Disable CUPS](#3-disable-cups)  
* [4\. Set Up USB Permissions (udev)](#4-set-up-usb-permissions-udev)  
* [5\. Route Print Jobs to the Daemon](#5-route-print-jobs-to-the-daemon)  
* [6\. Set Up Auto-Start Service](#6-set-up-auto-start-service)  
* [7\. Add Desktop App Shortcut (Optional)](#7-add-desktop-app-shortcut-optional)  
* [8\. Verification & Everyday Usage](#8-verification--everyday-usage)

# **1\. Prerequisites & Dependencies**

**Supported OS:** Debian 11/12/13, Ubuntu 20.04+, Linux Mint, or any glibc-based Linux distribution (x86\_64, aarch64).

Install the required build tools and runtime libraries:

```
sudo apt update
sudo apt install -y \
    build-essential \
    pkg-config \
    ghostscript \
    libusb-1.0-0 \
    libusb-1.0-0-dev \
    libcups2 \
    libcups2-dev \
    libcupsimage2 \
    libcupsimage2-dev \
    libgtk-4-1 \
    libgtk-4-dev \
    libadwaita-1-0 \
    libadwaita-1-dev \
    libcairo2 \
    libcairo2-dev
```

**Note for ARM64 / other architectures:**  
If libraries reside outside `/lib/x86_64-linux-gnu`, run `make` with:`make USB_SO=-lusb-1.0 GTK_LIBS="$(pkg-config --libs libadwaita-1 gtk4 cairo) -lm"`

# **2\. Build the Software**

Clone or copy the repository to your machine, then compile:

```
cd /path/to/printtesting
make clean && make
```

This compiles five binaries in the `./bin/` directory:

* `lbp2900d` — Background print daemon (IPP server \+ USB controller)  
* `lbp2900-ui` — Status monitor and preferences GUI  
* `lbp2900ctl` — Command-line controller  
* `verify_harness` — Protocol and stress test suite  
* `verify_asan` — Sanitizer test binary

# **3\. Disable CUPS**

To prevent conflicts over the printer's USB connection and local IPP ports, disable standard CUPS:

```

sudo systemctl stop cups.service cups.socket cups.path cups-browsed.service 2>/dev/null || true
|---|
sudo systemctl mask cups.service cups.socket cups.path 2>/dev/null || true
```

# **4\. Set Up USB Permissions (udev)**

Allow normal users to communicate with the printer without `root`:

```
# Add udev rule for Canon LBP-2900 (04a9:2676)
sudo tee /etc/udev/rules.d/99-canon-lbp2900.rules >/dev/null << 'EOF'
SUBSYSTEM=="usb", ATTR{idVendor}=="04a9", ATTR{idProduct}=="2676", MODE="0666", GROUP="lp"
EOF

# Add current user to groups and reload rules
sudo usermod -aG lp,plugdev "$USER"
sudo udevadm control --reload-rules
sudo udevadm trigger
```

*(Optional)* Prevent the kernel module `usblp` from locking the device:

```
echo "blacklist usblp" | sudo tee /etc/modprobe.d/blacklist-usblp.conf
```

# **5\. Route Print Jobs to the Daemon**

Configure standard applications (browsers, document viewers, office suites) to route print jobs directly to `lbp2900d`:

```
# Set system-wide and user client configuration
sudo mkdir -p /etc/cups
echo "ServerName /tmp/lbp2900-cups.sock" | sudo tee /etc/cups/client.conf

mkdir -p ~/.cups
echo "ServerName /tmp/lbp2900-cups.sock" > ~/.cups/client.conf
```

Install CLI symlinks to enable global terminal commands (`lp`, `lpr`, `lpstat`, `lbp2900ctl`):

```
PROJECT_DIR="$(pwd)"
for cmd in lbp2900ctl lp lpr lpstat lpq lprm cancel; do
    sudo ln -sf "$PROJECT_DIR/bin/lbp2900ctl" "/usr/local/bin/$cmd"
done
```

# **6\. Set Up Auto-Start Service**

Run `lbp2900d` automatically in the background upon login:

```
PROJECT_DIR="$(pwd)"
mkdir -p ~/.config/systemd/user

cat << EOF > ~/.config/systemd/user/lbp2900d.service
[Unit]
Description=Canon LBP-2900 Standalone Print Daemon
After=graphical-session.target

[Service]
Type=simple
WorkingDirectory=$PROJECT_DIR
ExecStart=$PROJECT_DIR/bin/lbp2900d --ui-bin $PROJECT_DIR/bin/lbp2900-ui
Restart=always
RestartSec=2
Environment=DISPLAY=:0
Environment=XDG_RUNTIME_DIR=/run/user/%U

[Install]
WantedBy=default.target
EOF

systemctl --user daemon-reload
systemctl --user enable --now lbp2900d.service
```

Verify service status:

```
systemctl --user status lbp2900d.service
lbp2900ctl status
```

# **7\. Add Desktop App Shortcut (Optional)**

Add the Status Monitor to your application launcher:h  
PROJECT\_DIR="\$(pwd)"  
mkdir \-p \~/.local/share/applications

cat \<\< EOF \> \~/.local/share/applications/canon-lbp2900.desktop  
\[Desktop Entry\]  
Name=Canon LBP-2900 Status Monitor  
Comment=Status Monitor and Preferences for Canon LBP-2900  
Exec=\$PROJECT\_DIR/bin/lbp2900-ui  
Icon=printer  
Terminal=false  
Type=Application  
Categories=System;Settings;HardwareSettings;Printing;  
EOF

````

## 8. Verification & Everyday Usage

### Run Test Suites

```bash
./bin/verify_harness
./bin/verify_asan
````

## **Common Commands**

| Action | Command |
| :---- | :---- |
| Open Status Monitor GUI | `lbp2900ctl ui` |
| Check printer & USB connection status | `lbp2900ctl status` |
| Print a file (PDF, PostScript, images, text) | `lbp2900ctl print file.pdf` or `lp file.pdf` |
| Print a test page | `lbp2900ctl test-page 1` |
| View print queue | `lbp2900ctl queue` |
| Cancel current active print job | `lbp2900ctl cancel` |
| Clear all pending jobs in queue | `lbp2900ctl purge` |
| Resume printing after loading paper | Press the physical **Paper** button or run `lbp2900ctl resume-paper` |


# Canon LASER SHOT LBP-2900 Standalone CAPT Driver Suite

A lightweight, high-performance host-based printing software suite designed exclusively for the **Canon LASER SHOT LBP-2900 / LBP-2900B** (`USB ID 04a9:2676`) on Linux distributions (Debian, Ubuntu, Linux Mint, Fedora, Arch).

This project completely bypasses the legacy `cupsd` + `ccpd` stack with an embedded zero-overhead daemon (`~6.5 MB` RSS memory footprint), host-assisted 600 DPI band rendering, and a native **GTK4** Status Monitor reproducing the interface and diagnostic features of the official Windows driver.

Vibe-coded from Gemini 4 Argon Medium
Why this exist? im tired of cups not working all the time so me with agent created this to solve the problem.

---

## Table of Contents

- [Architecture & Data Flow](#architecture--data-flow)
- [Key Features](#key-features)
- [System Requirements](#system-requirements)
- [Installation Guide](#installation-guide)
  - [Option 1: Automated Installer (Recommended)](#option-1-automated-installer-recommended)
  - [Option 2: Manual Step-by-Step Installation](#option-2-manual-step-by-step-installation)
- [Everyday Usage](#everyday-usage)
  - [Desktop Application Printing](#desktop-application-printing)
  - [Command-Line Interface (`lbp2900ctl`)](#command-line-interface-lbp2900ctl)
- [Custom Desktop Icons](#custom-desktop-icons)
- [Verification & Stress Testing](#verification--stress-testing)
- [Troubleshooting & FAQ](#troubleshooting--faq)
- [Uninstallation](#uninstallation)
- [License & Credits](#license--credits)

---

## Architecture & Data Flow

```text
[ Desktop App (Chrome / Firefox / LibreOffice) ]
                      │
                      ▼ (IPP 1.1 / Local Unix Socket)
           /tmp/lbp2900-cups.sock
                      │
                      ▼
             [ lbp2900d Daemon ]
             ├── Spool & State Machine (FIFO)
             ├── Page Rasterization (Ghostscript 600 DPI)
             ├── Hi-SCoA Compression (LZ77 + Obfuscation)
             └── Band Streaming & Adaptive Backpressure
                      │
             ┌────────┴────────┐
             ▼                 ▼
   [ lbp2900-ui (GTK4) ]   [ USB Endpoint (libusb-1.0) ]
     Live Animation &                  │
     Status Indicators                 ▼
                       [ Canon LBP-2900 Hardware ]
```

---

## Key Features

* **Zero-CUPS Background Engine:** Emulates IPP 1.1 via a local UNIX socket (`/tmp/lbp2900-cups.sock`) and loopback (`127.0.0.1:6310`). Standard desktop applications (Chrome, Firefox, LibreOffice, Evince, GIMP) print seamlessly without requiring `cupsd.service` to run.
* **Computer-Assisted Rendering (RAM Overflow Protection):** The Canon LBP-2900 hardware contains only 2 MB of onboard buffer memory. The driver renders pages on the host computer via Ghostscript, compresses scanlines into 70-line bands using Hi-SCoA (LZ77 + Obfuscation), and streams data chunk-by-chunk with adaptive back-pressure flow control so the hardware buffer never overflows.
* **Windows-Style GTK4 Status Monitor (`lbp2900-ui`):**
  - Instant popup when any application dispatches a print request.
  - Accurate page progress counters, buffer slot gauges, and instant Job Pause, Resume, and Cancel triggers.
  - Integrated maintenance utilities (Cleaning Page cycle, paper feed tests, live USB register diagnostics).
* **Fault-Tolerant State Machine:**
  - **Out-of-Paper Recovery:** Detects empty paper tray, suspends the job, blinks the hardware LED, and waits for paper reload. Pressing the physical **Paper** button resumes the print job without missing or duplicate sheets.
  - **Mechanical Fault Recovery:** Pauses cleanly on paper jams or cover opening, automatically resuming once cleared.
  - **Sudden Disconnect & FIFO Replay:** If the USB cable is disconnected or power is interrupted, pending and active jobs are preserved in an ephemeral FIFO queue. Once reconnected, the driver rediscovers the printer and resumes queued jobs in chronological order.
  - **Ephemeral Session Spooling:** Pending spool jobs in `/tmp/lbp2900-spool` reside in RAM and are automatically cleaned on logout, shutdown, or reboot.

---

## System Requirements

- **Supported Distributions:** Debian 11/12/13, Ubuntu 20.04/22.04/24.04, Linux Mint 21/22, or other modern distributions with Linux kernel $\ge 5.4$.
- **Architectures:** `x86_64`, `aarch64` (including Raspberry Pi 4/5).
- **Core Dependencies:** `build-essential`, `ghostscript`, `libusb-1.0-0-dev`, `libcups2-dev`, `libgtk-4-dev`, `libcairo2-dev`.

---

## Installation Guide

### Option 1: Automated Installer (Recommended)

Clone the repository and run the automated installer:

```bash
git clone https://github.com/HaiNguyen-09/lbp2900-native.git
cd lbp2900-driver

# Automated install (checks dependencies, builds binaries, configures udev & services)
sudo ./install.sh
```

---

### Option 2: Manual Step-by-Step Installation

#### 1. Install Dependencies

```bash
sudo apt update
sudo apt install -y \
    build-essential \
    pkg-config \
    ghostscript \
    libusb-1.0-0 \
    libusb-1.0-0-dev \
    libcups2-dev \
    libcupsimage2-dev \
    libgtk-4-1 \
    libgtk-4-dev \
    libadwaita-1-0 \
    libadwaita-1-dev \
    libcairo2 \
    libcairo2-dev \
    python3-gi \
    gir1.2-gdkpixbuf-2.0
```

> **Warning:** Do **NOT** remove or `apt purge cups`. Desktop applications link against `libcups.so` to render the native print dialog.

#### 2. Build Binaries

```bash
make clean
make all -j$(nproc)
```

This compiles:
* `bin/lbp2900d` — Background CAPT USB & IPP Print Daemon
* `bin/lbp2900-ui` — GTK4 Desktop Status Monitor
* `bin/lbp2900ctl` — Command-line controller and drop-in symlinks (`lp`, `lpr`, `lpstat`, `cancel`)
* `bin/verify_harness` — 26-case verification test suite
* `bin/verify_asan` — AddressSanitizer & LeakSanitizer test suite

#### 3. Disable Conflicting CUPS Background Services

Stop the standard CUPS daemon so `lbp2900d` has exclusive access to the USB port:

```bash
sudo systemctl stop cups.service cups.socket cups.path cups-browsed.service 2>/dev/null || true
sudo systemctl disable cups.service cups.socket cups.path cups-browsed.service 2>/dev/null || true
sudo systemctl mask cups.service cups.socket cups.path cups-browsed.service 2>/dev/null || true
```

#### 4. Configure USB Permissions (`udev`)

Allow direct user-space access to the Canon LBP-2900 (`04a9:2676`):

```bash
sudo tee /etc/udev/rules.d/99-canon-lbp2900.rules >/dev/null << 'EOF'
SUBSYSTEM=="usb", ATTR{idVendor}=="04a9", ATTR{idProduct}=="2676", MODE="0666", GROUP="lp"
EOF

sudo usermod -aG lp,plugdev "$USER"
sudo udevadm control --reload-rules
sudo udevadm trigger
```

#### 5. Redirect Desktop Applications to `lbp2900d`

Point the `libcups` client library to `lbp2900d`'s domain socket:

```bash
# Global fallback configuration
sudo mkdir -p /etc/cups
echo "ServerName /tmp/lbp2900-cups.sock" | sudo tee /etc/cups/client.conf >/dev/null

# User-level preference
mkdir -p ~/.cups
echo "ServerName /tmp/lbp2900-cups.sock" > ~/.cups/client.conf
```

#### 6. Install Binaries and Register Systemd User Service

```bash
# Copy binaries to user path
mkdir -p ~/.local/bin
cp bin/lbp2900d bin/lbp2900-ui bin/lbp2900ctl ~/.local/bin/

# Create systemd user service unit
mkdir -p ~/.config/systemd/user

cat > ~/.config/systemd/user/lbp2900d.service << 'EOF'
[Unit]
Description=Canon LASER SHOT LBP-2900 Standalone CAPT Print Daemon
After=graphical-session.target
PartOf=graphical-session.target

[Service]
Type=simple
ExecStart=%h/.local/bin/lbp2900d --ui-bin %h/.local/bin/lbp2900-ui
Restart=on-failure
RestartSec=2

[Install]
WantedBy=default.target
EOF

systemctl --user daemon-reload
systemctl --user enable --now lbp2900d.service
```

---

## Everyday Usage

### Desktop Application Printing

Simply press **Ctrl + P** (or click Print) in any application.

1. The application lists **Canon LASER SHOT LBP-2900 CAPT** as the ready printer.
2. `lbp2900-ui` immediately pops up showing live print progress and buffer gauges.
3. The document is converted, compressed, and streamed to the device.
4. Once completed, the window status updates to `"Printer is ready"`.

### Command-Line Interface (`lbp2900ctl`)

| Task | Command |
| :--- | :--- |
| **Open Status Monitor UI** | `lbp2900ctl ui` |
| **Check Live Status & Registers** | `lbp2900ctl status` |
| **Print Document Directly** | `lbp2900ctl print document.pdf` *(or `lp document.pdf`)* |
| **Print Test Page** | `lbp2900ctl test-page` |
| **View Pending FIFO Queue** | `lbp2900ctl queue` |
| **Purge Pending Queue** | `lbp2900ctl purge` |
| **Cancel Active Job Cleanly** | `lbp2900ctl cancel` *(or `cancel`)* |
| **Resume After Loading Paper** | `lbp2900ctl resume-paper` *(or press physical button on printer)* |

---

**Test Coverage:**
* **I/O Faults:** USB cable hot-unplug during active DMA transfer; mid-print AC power interruption.
* **Paper Engine Faults:** Paper jam simulation, cartridge cover open, empty paper tray.
* **Extreme Payloads:** Blank pages (0% coverage) vs solid black pages (100% coverage), irregular dimensions, complex vectors.
* **Concurrency:** Burst-spooling 15+ concurrent jobs dispatched simultaneously.
* **Memory Safety:** AddressSanitizer and Valgrind memory leak profiling (zero unfreed heap leaks).
* **Auto-Recovery:** Automatic USB rediscovery and FIFO queue replay upon device reconnection.

---

## Troubleshooting & FAQ

### Does CUPS need to be completely uninstalled?
**No.** Keep `libcups` installed because desktop applications dynamically link to it to render print dialogs. The installer only stops and masks background daemons (`cupsd.service`, `cups.socket`).

### What should I do if the Paper LED blinks red/amber?
The printer has run out of paper or needs a manual sheet feed. Load paper into the tray and press the physical **Paper** button on top of the printer (or run `lbp2900ctl resume-paper`). The driver will clear the alert and resume printing.

### Where are temporary spool files stored?
Pending spool jobs are written to `/tmp/lbp2900-spool/` (a `tmpfs` RAM disk). This ensures zero unnecessary disk wear and guarantees automatic cleanup upon logout or system reboot.

---

## Uninstallation

To remove the driver suite and restore the standard CUPS printing service:

```bash
# 1. Run automated uninstaller
sudo ./install.sh --uninstall

# 2. Unmask and re-enable standard CUPS
sudo systemctl unmask cups.service cups.socket cups.path cups-browsed.service
sudo systemctl enable --now cups.service cups.socket

# 3. Clean up client socket redirections
sudo rm -f /etc/cups/client.conf
rm -f ~/.cups/client.conf
```

---

## License & Credits

Developed as a clean-room reverse-engineered standalone CAPT v1.0 driver implementation for the Canon LASER SHOT LBP-2900. Released under the [GNU General Public License v3.0 (GPLv3)](LICENSE).

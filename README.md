# Canon LASER SHOT LBP-2900 Standalone CAPT Driver Suite

A lightweight, standalone host-based printing software suite designed exclusively for the **Canon LASER SHOT LBP-2900 / LBP-2900B** (`USB ID 04a9:2676`) on Linux (Debian, Ubuntu, Linux Mint). 

This project completely replaces standard `cupsd` (`CUPS`) and `ccpd` with an embedded zero-overhead daemon (`~6.5 MB` RSS memory footprint), host-assisted 600 DPI band rendering, and a native **GTK4** Status Monitor that reproduces the look and function of the official Windows driver.

---

## Key Features

* **Complete CUPS Daemon Replacement:** Runs without `cupsd`. Emulates IPP 1.1 (`127.0.0.1:6310` and `/tmp/lbp2900-cups.sock`) so standard Linux applications (Chrome, Firefox, LibreOffice, Evince, GIMP) print seamlessly without needing `cupsd.service` running.
* **Computer-Assisted Rendering (RAM Protection):** The Canon LBP-2900 hardware has only 2MB of internal buffer. The driver renders pages on the host computer via Ghostscript, compresses scanlines into 70-line bands using Hi-SCoA (LZ77 + Obfuscation), and streams data chunk-by-chunk with adaptive back-pressure flow control so the printer's memory never overflows.
* **Windows-Style GTK4 Status Monitor (`lbp2900-ui`):**
  - Instant popup when any application issues a print request.
  - Real-time animated paper feed indicator and LED indicators (Green Ready, Amber Printing, Red Alert).
  - Accurate page progress counters, buffer slot gauges, and instant Job Pause, Resume, and Cancel buttons.
  - Maintenance & utility tools (Cleaning Page cycle, paper feed tests, live USB diagnostics).
* **Fault-Tolerant State Machine:**
  - **Out-of-Paper Recovery:** Detects empty paper tray, pauses the job, sets the printer's physical button LED to blink, and waits for paper reload. Pressing the physical **Paper** button resumes the print job without duplicate or missing pages.
  - **Mechanical Interruption Handling:** Pauses cleanly on paper jam or cartridge cover open, resuming without duplicating already printed sheets once cleared.
  - **Sudden Disconnect & FIFO Replay:** If the USB cable is yanked mid-transfer or the printer is powered off, pending/active jobs are preserved in an ephemeral FIFO queue. Once reconnected, the driver rediscovers the printer and resumes queued jobs in chronological order.
  - **Ephemeral Session Spooling:** Pending spool jobs in `/tmp/lbp2900-spool` are automatically wiped on logout, shutdown, or reboot.
* **Custom Desktop Icon:** Ships with multi-resolution desktop icons (16×16 to 512×512) and desktop application launcher for quick access.

---

## Installation Guide

### Option 1: One-Click Automated Installer (Recommended)

Run the automated installer script:

```bash
cd /path/to/printtesting

# Automated install (auto-detects dependencies, builds binaries, configures icons and services)
./install.sh
```

To install system-wide symlinks (`/usr/local/bin/lp`, `/usr/local/bin/lpr`, `/usr/local/bin/cancel`) and USB `udev` rules:
```bash
sudo ./install.sh
```

To specify a custom high-resolution image for your application icon:
```bash
./install.sh --icon=/path/to/your-icon.png
```

To uninstall and revert system settings:
```bash
./install.sh --uninstall
```

---

### Option 2: Manual Installation Steps

#### 1. System Requirements & Dependencies

Supported on **Debian 11/12/13, Ubuntu 20.04/22.04/24.04, Linux Mint**, and other modern Linux distributions (`x86_64` or `aarch64`).

Install the required build tools and libraries:

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
    libcairo2 \
    libcairo2-dev \
    python3-gi \
    gir1.2-gdkpixbuf-2.0
```

> **Note:** Do **NOT** remove or `apt purge cups`. Desktop applications depend on `libcups.so` to show standard print dialogs.

#### 2. Build Binaries

```bash
cd /path/to/printtesting
make clean
make all
```

This generates:
* `bin/lbp2900d` — Background CAPT USB & IPP Print Daemon
* `bin/lbp2900-ui` — GTK4 Desktop Status Monitor
* `bin/lbp2900ctl` — Command-line controller and drop-in symlinks (`lp`, `lpr`, `lpstat`, `cancel`)
* `bin/verify_harness` — 26-check verification test suite
* `bin/verify_asan` — AddressSanitizer & LeakSanitizer test suite

#### 3. Disable Conflicting CUPS Background Services

Stop the standard CUPS daemon so `lbp2900d` has exclusive access to the USB port:

```bash
sudo systemctl stop cups.service cups.socket cups.path cups-browsed.service 2>/dev/null || true
sudo systemctl disable cups.service cups.socket cups.path cups-browsed.service 2>/dev/null || true
sudo systemctl mask cups.service cups.socket cups.path 2>/dev/null || true
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
sudo mkdir -p /etc/cups
echo "ServerName /tmp/lbp2900-cups.sock" | sudo tee /etc/cups/client.conf >/dev/null

mkdir -p ~/.cups
echo "ServerName /tmp/lbp2900-cups.sock" > ~/.cups/client.conf
```

#### 6. Enable the Background Service (`systemd --user`)

```bash
PROJECT_DIR="$(pwd)"
mkdir -p ~/.config/systemd/user

cat > ~/.config/systemd/user/lbp2900d.service << EOF
[Unit]
Description=Canon LASER SHOT LBP-2900 Standalone CAPT Print Daemon (No-CUPS Engine)
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

---

## Adding Custom Icons

The suite supports custom icons for both the desktop application menu and the GTK4 window title bar:

1. **Automatic Generation from Any High-Resolution Image:**
   ```bash
   ./scripts/process_icon.py /path/to/your-image.png icons
   ```
   The script automatically detects the bounding box of the graphic, centers it with standard ~86% padding, and downsamples it via Lanczos/Hyper filtering into `16x16`, `24x24`, `32x32`, `48x48`, `64x64`, `128x128`, `256x256`, and `512x512` resolutions.

2. **Install Icons to Desktop:**
   ```bash
   for sz in 16 24 32 48 64 128 256 512; do
       mkdir -p "$HOME/.local/share/icons/hicolor/${sz}x${sz}/apps"
       cp "icons/hicolor/${sz}x${sz}/apps/canon-lbp2900.png" "$HOME/.local/share/icons/hicolor/${sz}x${sz}/apps/"
   done
   gtk-update-icon-cache -f -t "$HOME/.local/share/icons/hicolor"
   ```

---

## Everyday Usage & Commands

### Printing from Applications
Simply press **Ctrl + P** (or click Print) in any application (Google Chrome, Firefox, Document Viewer / Evince, LibreOffice).
The application will list **Canon LASER SHOT LBP-2900 CAPT** as the default ready printer. When submitted:
1. `lbp2900-ui` immediately pops up showing live print status.
2. The document is converted, compressed, and streamed to the printer.
3. Once completed, the window status updates to `"Printer is ready"`.

### Command-Line Interface (`lbp2900ctl`)
| Task | Command |
| :--- | :--- |
| **Open Status Monitor UI** | `lbp2900ctl ui` (or `./bin/lbp2900ctl ui`) |
| **Check Live Status & Registers** | `lbp2900ctl status` |
| **Print Any Document** | `lp document.pdf` (or `lbp2900ctl print document.pdf`) |
| **Print a Test Page** | `lbp2900ctl test-page 1` |
| **View Pending FIFO Queue** | `lbp2900ctl queue` |
| **Purge Pending Queue** | `lbp2900ctl purge` |
| **Cancel Active Job Cleanly** | `cancel` (or `lbp2900ctl cancel`) |
| **Resume After Loading Paper** | Press physical button on LBP-2900 or run `lbp2900ctl resume-paper` |

---

## Verification & Stress-Testing

To ensure rock-solid stability against buffer overflows, USB disconnects, and mechanical interruptions, run the test harnesses:

```bash
# Standard 26-Check Verification Harness
./bin/verify_harness

# AddressSanitizer & LeakSanitizer Build
./bin/verify_asan
```

Test coverage includes:
* **Group 1:** Sudden USB cable disconnect and power-cut handling.
* **Group 2:** Mechanical fault recovery (Paper Jam, Cover Open, Missing Cartridge).
* **Group 3:** Extreme data payloads (blank page vs 100% black page, irregular dimensions, complex vectors).
* **Group 4:** High concurrency bursts (15+ concurrent jobs dispatched simultaneously).
* **Group 5:** AddressSanitizer memory leak profiling (zero unfreed heap leaks).
* **Group 6:** Automatic USB rediscovery and FIFO queue replay upon reconnection.

---

## Frequently Asked Questions (FAQ)

### Does CUPS need to be uninstalled?
**No.** Keep `libcups` installed because desktop applications link against it to show print dialogs. `install.sh` only stops and masks the background daemon (`cupsd.service`).

### What should I do if the Paper LED blinks red?
The red blinking LED indicates the printer has run out of paper or needs a sheet feed. Load paper into the tray and press the physical **Paper** button on top of the printer (or click **Resume Paper** in the UI). The driver will automatically extinguish the blinking light and resume printing.

### How can I uninstall this software?
Run:
```bash
./install.sh --uninstall
```
To re-enable old CUPS afterwards:
```bash
sudo systemctl enable --now cups
```

---

## License & Credits

Developed as a clean-room reverse-engineered standalone CAPT v1.0 driver implementation for the Canon LASER SHOT LBP-2900. Released under the GNU General Public License v3.0 (GPLv3).

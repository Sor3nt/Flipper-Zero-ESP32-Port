# Flip Pi SSH Terminal 2.1 — T-Embed CC1101 Plus

This FAP displays a persistent SSH terminal running on your Kali Pi. The T-Embed
connects to the companion using verified HTTPS on the same Wi-Fi LAN, or paired,
encrypted BLE. The companion opens an actual OpenSSH PTY connection to the Pi's
localhost SSH server as your ordinary Kali user. SSH credentials and host-key
trust stay on the Pi. This is an SSH-backed terminal bridge, not a native ESP32
SSH client; other SSH destinations can be reached from the terminal with `ssh`.

## Upgrade to Flip Pi 2.1

**If running pi2 or earlier, flash 2.0.0-pi3 from the full firmware package.**
If pi3 is already installed, update only the FAP and Pi companion.
Earlier firmware shares FreeRTOS thread-local slot 0 between Furi and ESP-IDF
pthreads. HTTPS/DNS can crash in `pthread_getspecific()` because the two systems
interpret that pointer differently. pi3 reserves slot 1 for Furi and keeps slot 0
for pthreads. Replacing the FAP alone cannot fix this core firmware fault.

Update the Pi companion from this package for the native 26-column viewport
and private terminal-query compatibility fix. Keep your existing SD configuration,
certificate and token. Updating this FAP on pi3 does not require another reflash.

1. Exit the old app. Replace `SD/apps/GPIO/flip_pi.fap` with the new FAP.
2. Copy the package's `Kali` folder to the Pi and SSH into Kali as your normal user.
3. From that folder run:

```bash
bash install_terminal.sh
```

The installer adds `openssh-client`, `openssh-server`, `tmux` and `python3-pyte`;
creates a dedicated localhost-only SSH key; pins the SSH server's public keys
from local files; verifies authentication; installs the upgraded companion and
restarts its service. Existing `token`, `pi.crt`, `pi.key` and SD config remain
valid. Run as the same user who installed the original companion, not root.
The key grants this user terminal access; it does not configure passwordless sudo.

If Kali reports a read-only filesystem, resolve that before running installers.

## First installation

```bash
sudo apt update
sudo apt install -y python3 openssl bluez python3-bleak
sudo systemctl enable --now bluetooth
bash install.sh 192.168.1.50
bash install_terminal.sh
```

Use the Pi's actual Wi-Fi IPv4 address. Copy the contents of
`~/.local/share/flip-pi/sd/` to the T-Embed SD root. These include
`apps_data/flip_pi/config.txt` and `apps_data/flip_pi/pi.crt`. Keep the token private.
Save the same LAN in firmware WLAN settings. Open Apps > GPIO > Flip Pi Wireless.
Select **Connect Wi-Fi**. A successful connection opens the shell display.
Certificate and hostname verification are enabled. This firmware's existing
SDK configuration disables certificate date checks; do not mistake this for
full date-validating PKI. Reserve the Pi's LAN IP, or rerun initial setup and
recopy its SD certificate/config when changing the IP.

## Screen and controls

- The Pi terminal is 80 columns x 24 rows. The device shows a native 320 x 170
  green-on-black display with 26 columns x 8 rows and a crisp monospace font.
  ANSI processing happens on the Pi. Only changed frames are redrawn; the
  renderer uses a 5 KB DMA stripe, not a full-screen RGB framebuffer.
- **Compact shell prompt** replaces the Bash/Zsh prompt in the current shell
  with a short ASCII prompt. Use it while the shell is waiting for a command.
  It does not edit your shell configuration. Box-drawing characters in output
  are converted to ASCII; use Pan left/right for lines wider than 26 columns.
- Rotate to scroll through output/history. OK returns to the main menu.
- Hold OK to open **Type input / command**. The existing encoder keyboard enters
  text. **Save sends the text followed by Enter** to the current terminal program.
- Use **Special keys** for Ctrl+C, Enter, Tab, Escape, arrows, Backspace, Ctrl+D,
  Ctrl+Z, Ctrl+L, Home, End and Page Up/Down. Arrow Up recalls shell history.
- **Pan left/right** moves through wide output. **Return to live view** resets
  scroll and horizontal position. Selecting **Terminal** resumes the display.
- Prompts, including sudo passwords, can be answered through Type input. The
  device keyboard itself displays the typed text; input is cleared after sending
  and is not written to diagnostic files. Remote terminal echo follows the program.
- Ordinary commands retain their directory, variables and terminal state.
  Interactive programs can run beyond the old 10-second limit. Ctrl+C interrupts.
- **Disconnect SSH** or exit closes this SSH connection, while the `flip-pi` tmux
  session and its jobs stay on the Pi. Connect again to attach. To end the shell
  and its session deliberately, type `exit` at the shell prompt. To inspect from
  another computer: `tmux attach -t flip-pi` after SSH login as the same user.

The display is monochrome ASCII and a small viewport. Full-screen programs may
require panning, scrolling and their own keyboard controls; this is not a Linux
graphical desktop or complete xterm implementation. Wi-Fi is the responsive mode;
BLE uses the same shell bridge with a slower update rate. Heavy tools, particularly
Metasploit, remain limited by the Pi Zero 2 W's RAM and CPU.

## Kali tool templates

**Kali tools** prompts for the settings below and then shows the assembled command
in the keyboard for review/editing. Save executes it in the current shell; use
Ctrl+C to return to a shell prompt before launching another tool. Values are
shell-quoted, and oversized or invalid values are rejected.

| Template | Settings requested |
| --- | --- |
| Nmap | Target IP/hostname |
| Hydra SSH | Target, username, password wordlist path |
| John the Ripper | Hash file and wordlist paths |
| SQLmap | URL with parameters |
| Netcat | Target and TCP port |
| Nikto | Target URL |
| Gobuster directory scan | URL and wordlist path |
| Bettercap console | Network interface |
| Aircrack-ng | Capture file and wordlist path |
| Tcpdump | Network interface; preset captures 100 packets |
| Metasploit console | Opens `msfconsole` |
| Tor status/start/stop | Service control |
| Curl through Tor | URL |
| DNS lookup | Hostname |

Defaults use loopback/example targets and sample local file paths. Enter your
actual authorized test target and existing file paths. Any other command can be
entered manually. The templates do not install tools or create wordlists/captures.
Optional installation of the listed CLI tools:

```bash
bash install_tools.sh
```

## Optional BLE

Pair the Pi with **T-Embed Flip Pi** after selecting Connect BLE in the app:

```text
bluetoothctl
power on
agent KeyboardOnly
default-agent
scan on
pair AA:BB:CC:DD:EE:FF
trust AA:BB:CC:DD:EE:FF
quit
```

Use the actual T-Embed address and enter its displayed pairing PIN. The Pi is the
BLE central; it connects only to that configured device. BLE mode waits 15 seconds
for the bridge. If pairing takes longer, finish pairing then select Connect BLE
again. For boot-time bridge startup, from the Kali folder:

```bash
bash install_ble_service.sh AA:BB:CC:DD:EE:FF
```

Stop any manually running BLE bridge first. Wi-Fi and BLE are alternative radio
modes on T-Embed; the tmux session lets you reattach when changing transport.

## Boot startup and diagnostics

```bash
sudo systemctl enable --now ssh flip-pi.service
systemctl is-enabled flip-pi.service
systemctl is-active flip-pi.service
journalctl -u flip-pi.service -n 50 --no-pager
```

The service restarts on failure. Optional BLE service is `flip-pi-ble.service`.
On the device, **Diagnostics** shows the app version, available heap and last
persisted execution phase. The same phase is saved in
`SD/apps_data/flip_pi/last_run.txt`; it contains no command, password, token or output.
If a reboot recurs, send that file's contents and the USB panic/backtrace. On a PC
with Python/pyserial, for example:

```bash
python -m serial.tools.miniterm COM4 115200
```

Use the actual port, open the terminal before reproducing, and capture the panic
and reset messages. Build tests cannot identify the exact cause of a device reboot.

## Memory and validation

A 24-KB worker stack provides more room for TLS than the former 8-KB worker. The
HTTP client stays open across terminal polls, avoiding repeated TLS setup. Each
response is bounded to 2 KB; the rendered text is 131 bytes. The Pi retains at
most 200 history lines and one SSH connection per companion process. Ordinary
command output is no longer accumulated in multiple 4-KB buffers on the device.

Production UI callbacks and template quoting are host-tested. Backend tests cover
persistent state, prompts, bounded output, auth/replay and OpenSSH client PTY/Ctrl+C
against an isolated SSH test server. Linked API imports are checked against pi2.
Physical T-Embed/Pi testing and BLE runtime validation remain required. tmux
reconnection integration can be run with `FLIP_PI_TEST_TMUX=1` where Unix-domain
sockets are supported; it is blocked by the build environment's socket restrictions.

MIT license for the companion/app code. Dependencies retain their own licenses.

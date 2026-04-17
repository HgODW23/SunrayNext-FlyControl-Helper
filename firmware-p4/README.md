# P4 Firmware

The P4 firmware is the application-facing side of SunrayNext-FlyControl-Helper. It runs on ESP32-P4 and provides Wi-Fi connectivity, UART bridging, runtime console commands, and MAVLink forwarding between the network side and a flight controller.

## What This Firmware Does

- Initializes the Wi-Fi station stack
- Starts the MAVLink bridge layer
- Receives traffic over UDP or TCP
- Forwards network traffic to UART ports connected to the flight controller
- Exposes runtime configuration through an ESP-IDF REPL console

## Main Modules

| Path | Responsibility |
| --- | --- |
| `main/main.c` | Boot sequence and subsystem initialization |
| `main/wifi_app.c` | Wi-Fi station management |
| `main/network_app.cpp` | UDP/TCP socket handling |
| `main/network_cmd.c` | Console commands for network setup |
| `main/mavlink_bridge.cpp` | UART and MAVLink bridge logic |
| `main/bridge_cmd.c` | Console commands for UART and bridge control |
| `main/console_app.c` | REPL console setup and command registration |

## Hardware Defaults

| UART | Purpose | TX | RX | Default Baud |
| --- | --- | --- | --- | --- |
| TELEM1 | Primary flight controller link | GPIO20 | GPIO21 | 115200 |
| TELEM2 | Secondary flight controller link | GPIO10 | GPIO11 | 115200 |
| DEBUG | Debug serial port | GPIO22 | GPIO23 | 115200 |

Default network values:

- UDP port: `8888`
- TCP port: `8889`
- Max TCP clients: `5`

## Build and Flash

Make sure the ESP-IDF environment is active, then run:

```bash
idf.py build
idf.py flash monitor
```

## Runtime Console Commands

### Wi-Fi

| Command | Description |
| --- | --- |
| `wifi scan [-s|--sort <rssi|ssid>]` | Scan visible APs and list SSID/RSSI/security/channel/BSSID |
| `wifi connect <ssid> [<password>] [-b|--bssid <mac>]` | Connect to target Wi-Fi, supports optional BSSID lock |
| `wifi disconnect` | Disconnect from current Wi-Fi |
| `wifi status` | Show mode, link, DHCP, IP/mask/gateway, SSID and RSSI |
| `wifi reconnect` | Reconnect using last successful profile stored in NVS |
| `wifi mac [<mac>]` | Show current MAC or apply temporary MAC spoofing (until reboot) |
| `echo <text>` | Echo test input |

### Network

| Command | Description |
| --- | --- |
| `net_type <udp|tcp>` | Select the active transport |
| `net_mode <client|server>` | Set client or server mode |
| `net_target <ip> <port>` | Set the remote target in client mode |
| `net_port <port>` | Set the local listening port in server mode |
| `net_start` | Start the network service |
| `net_stop` | Stop the network service |
| `net_send <message>` | Send a test string |
| `net_status` | Print current network status and statistics |
| `net_localip` | Print the current local IP |

### UART / Bridge

| Command | Description |
| --- | --- |
| `uart_baud [id] [baud]` | Show or change UART baud rates |
| `uart_en <id> <0|1>` | Disable or enable a UART channel |
| `uart_status` | Show UART traffic statistics |
| `uart_help` | Print bridge command help |

## Typical Workflow

1. Flash the firmware.
2. Connect the board to Wi-Fi:

   ```text
   wifi scan
   wifi connect <ssid> [password]
   ```

3. Configure the network source:

   ```text
   net_type udp
   net_target <pc_ip> 8888
   ```

4. Enable the UART link to the flight controller:

   ```text
   uart_en 1 1
   ```

## Wi-Fi Examples

```text
wifi scan --sort rssi
wifi connect MyRouter MyPassword
wifi connect MyRouter -b AA:BB:CC:DD:EE:FF
wifi status
wifi disconnect
wifi reconnect
wifi mac
wifi mac 02:11:22:33:44:55
```

Notes:
- Connection success is considered valid only after DHCP IP is acquired.
- If multiple APs share the same SSID and `-b` is not provided, CLI will list candidates and ask for an index.
- MAC spoofing is temporary and resets to the hardware default after reboot.

## MAVLink Notes

The bridge is intended for transparent MAVLink forwarding and includes parsing helpers, traffic statistics, and CRC-related checks inside the bridge layer. The current codebase is especially oriented around simulator and remote-control workflows that send MAVLink traffic over the network and forward it to the controller over UART.

## Development Notes

- Review `main/mavlink_bridge.h` before changing UART pins, baud rates, or default ports.
- Validate console workflows after changing any network or bridge command behavior.
- Keep changes aligned with the current ESP-IDF console pattern already used by the project.

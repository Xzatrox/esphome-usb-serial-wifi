# esphome-usb-serial-wifi

Expose a USB serial adapter (e.g. CP210x) over WiFi TCP using ESPHome on a Seeed Studio XIAO ESP32-S3.

Primary use case: expose a **Home Assistant Connect ZBT-1** (SkyConnect) Zigbee adapter connected via USB hub to Home Assistant ZHA over the network.

## Hardware

- [Seeed Studio XIAO ESP32-S3](https://wiki.seeedstudio.com/xiao_esp32s3_getting_started/)
- [Home Assistant Connect ZBT-1](https://www.home-assistant.io/connectzbt1/) (CP2102N + EFR32 Zigbee)
- USB hub with external power (required — see Power section below)

## Features

- USB host mode on ESP32-S3 internal OTG PHY
- USB hub support (ESP-IDF `CONFIG_USB_HOST_HUBS_SUPPORTED`)
- CP210x vendor-specific USB-UART driver with correct RTS/DTR modem handshaking
- TCP stream server on configurable port
- Hot-plug: device reconnects automatically after unplug/replug

## Quick Start

1. Copy `esphome_components/` to your ESPHome config directory (e.g. `/config/esphome/esphome_components/`)
2. Copy `esphome-usb-serial-wifi.yaml` to your ESPHome config directory and edit WiFi credentials
3. Flash to XIAO ESP32-S3
4. In Home Assistant ZHA, set serial port to `socket://<device-ip>:6638`

## Wiring

Connect the USB hub's upstream port to the XIAO ESP32-S3 USB-C port. The hub must have its own external power supply — the ZBT-1 requires more current than the XIAO can supply.

The XIAO ESP32-S3 USB-C port serves dual purpose: USB JTAG/UART (default) and USB OTG host. This project uses it in OTG host mode. Logger is redirected to hardware UART0 (`hardware_uart: UART0`) to free the USB-C port.

## Power

The ZBT-1 draws up to 150 mA. A self-powered USB hub with a PD charger is required. The hub's upstream port still needs VBUS from the XIAO to detect the host — GPIO12 on the XIAO ESP32-S3 controls the VBUS enable switch and is driven high automatically by the patched `usb_host` component.

## ESPHome Version

Tested with ESPHome **2026.7.2** and ESP-IDF **5.5.5**.

## Component Patches

This project includes three local ESPHome component overrides under `esphome_components/`. All patches are minimal and targeted.

### `usb_host` — USB OTG PHY host mode init

**Problem:** The ESP32-S3 USB-C port defaults to device/JTAG mode. ESPHome's built-in `usb_host` component calls `usb_host_install()` without initializing the OTG PHY in host mode first, so the USB peripheral never starts.

**Fix** (`usb_host_component.cpp`):
- Drive GPIO12 high to enable VBUS on the XIAO ESP32-S3 USB-C port
- Call `usb_new_phy()` with `USB_OTG_MODE_HOST` before `usb_host_install()`
- Set `skip_phy_setup = true` in `usb_host_config_t` to prevent double-init

```cpp
gpio_set_direction(GPIO_NUM_12, GPIO_MODE_OUTPUT);
gpio_set_level(GPIO_NUM_12, 1);

usb_phy_config_t phy_config = {
    .controller = USB_PHY_CTRL_OTG,
    .target = USB_PHY_TARGET_INT,
    .otg_mode = USB_OTG_MODE_HOST,
    .otg_speed = USB_PHY_SPEED_UNDEFINED,
};
usb_new_phy(&phy_config, &phy_handle);

usb_host_config_t config{};
config.skip_phy_setup = true;
usb_host_install(&config);
```

**Header:** `<esp_private/usb_phy.h>` (ESP-IDF private API, available in 5.x)

### `usb_host` — USB hub support

**Problem:** Devices behind a USB hub are not enumerated by default.

**Fix** (sdkconfig): Enable `CONFIG_USB_HOST_HUBS_SUPPORTED=y` in the ESPHome YAML. ESP-IDF 5.5 includes `ext_hub.c` and `ext_port.c` but they are disabled by default via Kconfig.

```yaml
esp32:
  framework:
    type: esp-idf
    sdkconfig_options:
      CONFIG_USB_HOST_HUBS_SUPPORTED: "y"
      CONFIG_USB_HOST_HUB_MULTI_LEVEL: "y"
```

### `usb_uart` — CP210x RTS/DTR modem handshaking

**Problem:** The EFR32 Zigbee chip inside the ZBT-1 does not respond to ASH reset frames (bellows/ZHA times out with `TimeoutError`). The upstream ESPHome CP210x driver sets baud rate and line control but never asserts RTS/DTR via the CP210x `SET_MHS` vendor command. Without RTS asserted, the EFR32 UART receiver is held in reset.

**Fix** (`cp210x.cpp`): Add a `SET_MHS` step after `SET_BAUDRATE` to assert both DTR (bit 0) and RTS (bit 1):

```cpp
case 3: {
    // Assert DTR+RTS — required to release EFR32 reset line on ZBT-1
    static constexpr uint16_t MHS_DTR_RTS = 0x0303;  // set DTR+RTS, mask DTR+RTS
    this->config_transfer_(USB_VENDOR_IFC | usb_host::USB_DIR_OUT, SET_MHS, MHS_DTR_RTS, channel->index_);
    return true;
}
```

The `SET_MHS` value `0x0303` sets bits [1:0] of the high byte (mask) and bits [1:0] of the low byte (value), asserting both RTS and DTR simultaneously.

### `stream_server` — ESPHome 2026.x API compatibility

**Problem:** The upstream [oxan/esphome-stream-server](https://github.com/oxan/esphome-stream-server) fails to compile with ESPHome 2026.x due to the `get_use_address()` API change (now `get_use_address_to()` taking a `std::span`).

**Fix** (`stream_server.cpp`): Use `get_use_address_to()` with a stack-allocated buffer:

```cpp
char addr[70] = {};
auto addr_span = std::span<char, 70>(addr, 70);
esphome::network::get_use_address_to(addr_span);
ESP_LOGCONFIG(TAG, "  Address: %s:%u", addr, this->port_);
```

## Home Assistant ZHA Configuration

Set the serial port to:
```
socket://<device-ip>:6638
```

- Radio type: **EZSP**
- Baudrate: **115200**
- Flow control: **none**

The ZBT-1 uses EZSP/ASH firmware (not CPC). bellows sends ASH reset frames (`1a1a...c038bc7e`) and expects an RSTACK response. This works once RTS is asserted via `SET_MHS`.

## Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `usb_host_install failed` | PHY double-init | Set `skip_phy_setup = true` |
| No `New device` in logs | PHY in device mode | Call `usb_new_phy()` with `USB_OTG_MODE_HOST` |
| Hub not detected | Hub support disabled | Add `CONFIG_USB_HOST_HUBS_SUPPORTED: "y"` |
| `Channel not initialised` | USB client not registered | PHY not running, see above |
| ZHA `TimeoutError` | EFR32 in reset | Add `SET_MHS` DTR+RTS to CP210x init |
| Logger garbled / no output | USB-C used for JTAG | Set `hardware_uart: UART0` in logger |

## USB/IP — Bluetooth Dongle over WiFi

The `usb_ip` component exposes any USB device connected to the ESP32-S3 as a USB/IP server. The primary use case is a Bluetooth USB dongle with an external antenna, exposed to Home Assistant as a local USB device via the Linux USB/IP kernel module.

### Hardware

- Any USB Bluetooth adapter (tested: Realtek RTL8761BUV, VID `0x0BDA` PID `0xA728`)
- Connected via USB hub to XIAO ESP32-S3 (same hub as ZBT-1 if desired)

### YAML

```yaml
external_components:
  - source:
      type: local
      path: esphome_components
    components: [usb_host, usb_ip]

usb_host:
  max_transfer_requests: 32

usb_ip:
  port: 3240  # standard USB/IP port
```

### Home Assistant / Linux setup

On the HA host (or any Linux machine):

```bash
# Load the USB/IP VHCI kernel module
sudo modprobe vhci-hcd

# List devices exported by the ESP32
usbip list -r 10.0.0.78

# Attach the device (creates a local /dev/bus/usb entry)
sudo usbip attach -r 10.0.0.78 -b 1-1

# Verify it appeared
lsusb

# Detach when done
sudo usbip detach -p 0
```

Once attached, the Bluetooth adapter appears as a local USB device and the standard `btusb` kernel driver binds to it automatically. Home Assistant's Bluetooth integration will detect it.

### Notes

- USB/IP standard port is 3240
- Only one client can be attached at a time
- The component exposes whichever USB device enumerates first on the hub port
- Hot-plug is supported: if the dongle is unplugged and replugged, the server updates automatically (client must re-attach)
- The `usb_host` PHY init and hub support fixes from the Zigbee proxy apply here identically

## License

MIT

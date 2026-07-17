# ESP8266 Wireless UART Bridge

A bidirectional, low-latency wireless UART bridge for two ESP-01s modules using ESP-NOW. Bytes received on UART0 by either module are transmitted wirelessly and written to UART0 on the other module. No Wi-Fi access point is required.

This project targets the ESP8266EX with 1 MB flash and uses the native ESP8266_RTOS_SDK v3.4 API. It does not use Arduino or PlatformIO.

## Features

- Bidirectional UART0-to-UART0 transport at 115200 baud, 8N1.
- ESP-NOW broadcast pairing by default; optional fixed-peer addressing.
- Wi-Fi station mode without connecting to an access point.
- Fixed Wi-Fi channel 1 and disabled power saving for low latency.
- One in-flight ESP-NOW frame at a time to avoid `ESP_ERR_ESPNOW_NO_MEM` failures.
- Sequence-number deduplication for retransmitted ESP-NOW frames.
- FreeRTOS queue between the Wi-Fi receive callback and UART output task.
- Maximum 248 UART data bytes per 250-byte ESP-NOW packet.
- ESP-01s GPIO2 onboard LED indicates accepted TX and delivered RX activity.
- Runtime SDK logging disabled to keep UART0 data clean.
- Standalone PowerShell build, flash, and monitor workflow.

## Hardware

Two ESP-01s modules are required. Each module needs a stable 3.3 V supply capable of handling ESP8266 current peaks. Do not power an ESP-01s directly from a weak USB-to-UART adapter regulator.

Connect each module as follows:

| ESP-01s pin | Connection |
| --- | --- |
| VCC | Regulated 3.3 V |
| GND | Common ground |
| EN / CH_PD | Pull up to 3.3 V |
| GPIO0 | Pull up for normal boot; pull low while resetting to flash |
| GPIO2 | Pull up for normal boot; reused as an active-low onboard activity LED after startup |
| TXD / GPIO1 | UART output to the attached device's RX |
| RXD / GPIO3 | UART input from the attached device's TX |

Use 3.3 V UART logic. The ESP8266 pins are not 5 V tolerant.

## Toolchain

The tested Windows setup is:

- ESP8266_RTOS_SDK v3.4 at `D:\Project\ESP8266_RTOS_SDK`
- Xtensa GCC 8.4.0 at `C:\Espressif\xtensa-lx106-elf\bin`
- CMake installed with `winget install Kitware.CMake`
- Ninja installed with `winget install Ninja-build.Ninja`
- `mconf-idf.exe` available in `C:\Tools\bin`
- Python 3 and Git available on `PATH`

The build script validates every required tool before configuring. Override non-default locations with these environment variables:

```powershell
$env:IDF_PATH = 'D:\Project\ESP8266_RTOS_SDK'
$env:ESP_TOOLCHAIN = 'C:\Espressif\xtensa-lx106-elf\bin'
$env:ESP_CMAKE_DIR = 'C:\Program Files\CMake\bin'
$env:ESP_TOOLS_BIN = 'C:\Tools\bin'
```

## Build

From PowerShell in the project directory:

```powershell
.\build.ps1
```

Force a clean configure and rebuild after changing `sdkconfig` or build settings:

```powershell
.\build.ps1 -Clean
```

Generated images are written to `build/`:

- `build/bootloader/bootloader.bin`
- `build/partition_table/partition-table.bin`
- `build/wireless_uart.bin`

The script passes `-DPYTHON_DEPS_CHECKED=1` because this old SDK has stale upper-version constraints for otherwise compatible Python packages. It also passes `-DCMAKE_POLICY_VERSION_MINIMUM=3.5` for compatibility with CMake 4.x.

## Flash and monitor

Put the ESP-01s into download mode by holding GPIO0 low while resetting it, then run:

```powershell
.\build.ps1 -Flash -Port COM5
```

Build, flash, and open the serial monitor:

```powershell
.\build.ps1 -Flash -Monitor -Port COM5
```

Use `Ctrl+]` to exit the monitor. The UART data rate and default flash baud are both 115200.

## Pairing and configuration

The main firmware settings are near the top of `main/main.c`:

```c
#define WUART_WIFI_CHANNEL 1
#define WUART_UART_BAUD    115200
/* #define WUART_USE_FIXED_PEER */
#define WUART_PEER_MAC { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }
```

Broadcast mode works without configuration when only one bridge pair is in range. If multiple bridge pairs share channel 1, enable `WUART_USE_FIXED_PEER` and set each unit's `WUART_PEER_MAC` to the station MAC address of its partner. Rebuild and flash each unit with the corresponding partner address.

Both endpoints must use the same Wi-Fi channel and UART settings.

## Operation notes

- The bridge provides packet deduplication but no application-level acknowledgment, retry, encryption, or guaranteed delivery.
- Broadcast mode may be received by every compatible bridge in range on channel 1.
- The common ESP-01s onboard LED is active-low on GPIO2. Continuous traffic may keep it visibly illuminated because nearby events extend the 30 ms pulse.
- UART bytes already removed from the local receive buffer are lost if `esp_now_send()` immediately rejects a frame. The flow-control gate minimizes this condition but does not make the protocol lossless.
- The ESP8266 ROM emits a short boot message on UART0 during reset. Runtime SDK logging is disabled after startup, but the ROM message cannot be suppressed by application code.
- A sender restart resets its 8-bit sequence counter. The receiver only drops consecutive duplicate sequence numbers, so normal counter wraparound is accepted.

## Project layout

```text
Wireless-UART/
|-- CMakeLists.txt
|-- Makefile
|-- build.ps1
|-- sdkconfig
`-- main/
    |-- CMakeLists.txt
    |-- component.mk
    `-- main.c
```

`sdkconfig` is intentionally committed to lock the ESP-01s 1 MB flash layout and build settings. Generated build output remains ignored by Git.

## License

No license file is currently included. Add a license before redistributing the project.

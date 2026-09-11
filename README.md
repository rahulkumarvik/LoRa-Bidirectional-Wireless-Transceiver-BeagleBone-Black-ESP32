# LoRa Bidirectional Wireless Transceiver — BeagleBone Black & ESP32

A two-way, switch-triggered wireless messaging link over LoRa (SX1278, 433MHz)
between a BeagleBone Black and an ESP32. Each board has a push-button switch;
pressing it sends a short message to the other board. Both boards otherwise
sit continuously in receive mode, ready to catch a message at any time.

## Features

- Bidirectional communication — either board can transmit or receive
- Multithreaded design on both ends (POSIX threads on the BBB, FreeRTOS
  tasks on the ESP32), with a mutex protecting shared access to the radio
- Register-level SX1278 driver in C on the BeagleBone Black (no external
  LoRa library — direct SPI register access via `libgpiod` + `spidev`)
- Arduino `LoRa` library-based implementation on the ESP32
- RSSI reporting on every received packet
- CRC error detection and reporting

## Repository layout

```
.
├── lora.h        # Public interface: what main.c is allowed to call
├── lora.c        # SX1278 register/SPI driver + GPIO handling (BeagleBone Black)
├── main.c        # Thread logic and program entry point (BeagleBone Black)
├── Makefile      # Builds lora.c + main.c into ./transceiver
├── esp32_lora_threaded.ino   # ESP32 sketch (Arduino IDE)
└── README.md
```

## Hardware required

- 1x BeagleBone Black
- 1x ESP32-WROOM-32 DevKit
- 2x SX1278 LoRa module (433MHz), one per board
- 2x push-button switch, one per board
- 2x 433MHz antenna (or wire stub) — required for usable range
- Jumper wires

## Wiring

### BeagleBone Black ↔ SX1278

| SX1278 | BBB Pin |
|---|---|
| VCC | P9_3 / P9_4 (3.3V) |
| GND | P9_1 |
| SCK | P9_22 (SPI0_SCLK) |
| MISO | P9_21 (SPI0_D0) |
| MOSI | P9_18 (SPI0_D1) |
| NSS/CS | P9_17 (SPI0_CS0) |
| RESET | P9_23 (gpiochip0, line 17) |

### BeagleBone Black ↔ Switch

| Leg | BBB Pin |
|---|---|
| Leg 1 | P9_12 (gpiochip0, line 28) |
| Leg 2 | GND |

### ESP32 ↔ SX1278

| SX1278 | ESP32 Pin |
|---|---|
| VCC | 3V3 |
| GND | GND |
| SCK | G18 |
| MISO | G19 |
| MOSI | G23 |
| NSS/CS | G5 |
| RESET | G14 |
| DIO0 | G26 |

### ESP32 ↔ Switch

| Leg | ESP32 Pin |
|---|---|
| Leg 1 | G4 |
| Leg 2 | GND |

Both switches use each board's internal pull-up (set in software) — idle =
HIGH, pressed = LOW. No external resistor is needed.

## LoRa radio configuration

Both boards must use identical settings for the link to work:

| Parameter | Value |
|---|---|
| Frequency | 433 MHz |
| Bandwidth | 125 kHz |
| Spreading Factor | SF7 |
| Coding Rate | 4/5 |
| Header mode | Explicit |
| Preamble | 8 symbols |
| Sync word | 0x12 |
| CRC | Enabled |

## Building and running — BeagleBone Black

**Dependencies:**

```bash
sudo apt install libgpiod-dev
```

SPI0 must be enabled and exposed as `/dev/spidev0.0`. Check with:

```bash
ls /dev/spidev*
```

If it's missing, enable the SPI0 device-tree overlay in `/boot/uEnv.txt`
(the exact line varies by image — check the file's contents directly) and
reboot.

**Build:**

```bash
make
```

**Run** (root is required for `/dev/spidev0.0` and GPIO access):

```bash
sudo ./transceiver
```

**Clean:**

```bash
make clean
```

## Building and running — ESP32

1. In Arduino IDE, add the ESP32 boards manager URL (File → Preferences),
   then install **esp32 by Espressif Systems** via Boards Manager.
2. Select **Tools → Board → DOIT ESP32 DEVKIT V1**.
3. Install the **LoRa** library by Sandeep Mistry (Sketch → Include Library
   → Manage Libraries).
4. Select the correct COM port and upload `esp32_lora_threaded.ino`.
5. Open the Serial Monitor at **115200 baud**.

## Usage

1. Power on both boards and confirm each prints its "ready" banner.
2. Press the BeagleBone Black's switch — the ESP32's Serial Monitor should
   show `Received: Hello` with an RSSI value.
3. Press the ESP32's switch — the BeagleBone Black's terminal should show
   `[RX] Received: "..."` with an RSSI value.

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| `SX1278 version register` doesn't read `0x12` | Wiring or power issue on SPI lines |
| Nothing received on either side | Missing/loose antenna, or mismatched LoRa config between boards |
| Reception works one way but not the other | Check the antenna on the non-receiving board's SX1278 module specifically — transmission at high power can succeed even with a poor antenna, while reception is much more sensitive to it |
| Garbled Serial Monitor output on ESP32 | Baud rate mismatch, or a wire touching the ESP32's TXD/RXD (UART0) pins |
| Switch registers multiple presses per tap | Mechanical bounce — check wiring and consider increasing the debounce delay |

## Possible extensions

- Message queue between the listening thread and a separate logging/
  processing thread
- ACK/retransmission for reliable delivery
- Persistent logging of received messages with timestamps
- Migration to a full LoRaWAN stack for multi-node deployments

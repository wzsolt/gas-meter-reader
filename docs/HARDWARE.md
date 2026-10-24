# Hardware and wiring

[Back to README](../README.md) · [Installation order](INSTALL.md)

The board is a **Seeed XIAO ESP32-C6 with 4 MB flash**, connected to a two-wire Geckollect normally open reed sensor. The diagram is a logical schematic, **not the physical pin order**. Follow the board's printed D-pin labels and the GPIO table.

![English wiring diagram](wiring-en.png)

Download: [English SVG](wiring-en.svg) · [English PNG](wiring-en.png) · [Hungarian SVG](wiring.svg) · [Hungarian PNG](wiring.png).

## Bill of materials

| Reference | Component |
|---|---|
| Board | Seeed XIAO ESP32-C6 |
| Battery | Protected single-cell 18650 Li-ion, 3.7 V nominal / 4.2 V fully charged, suitable holder |
| R1 | 1 MΩ, 1% |
| R2 | 330 kΩ, 1% |
| R3 | 100 kΩ external reed pull-up |
| R4 | 1 kΩ LED series resistor |
| C1 | 100 nF non-polarized ceramic capacitor |
| LED1 | External green LED |
| SW1 | Normally open momentary push button |
| Sensor | Two-wire normally open reed contact, such as Geckollect GE |

The schematic omits the antenna. The firmware selects the external antenna, so connect a suitable U.FL antenna for radio use.

## GPIO connections

| Board label | GPIO | Connect to |
|---|---:|---|
| D0 / A0 | 0 / ADC1_CH0 | R1/R2/C1 divider junction |
| D1 / A1 | 1 | Reed input; R3 pulls this node up to 3V3 |
| D2 / A2 | 2 | Push button to GND; firmware enables the internal pull-up |
| D3 | 21 | Unused |
| D4 | 22 | R4 → LED anode; LED cathode → GND |
| 3V3 | — | Top of R3 |
| GND | — | Reed return, button return, divider return and LED return |
| Rear BAT+ / BAT− | — | Battery positive / negative |

**All grounds are common: XIAO GND = battery negative. D0, D1, D2 and D4 are independent signals.**

### Battery voltage divider

```text
BAT+ ── R1 1 MΩ / 1% ──●── R2 330 kΩ / 1% ── GND
                        ├── D0 / GPIO0
                        └── C1 100 nF ── GND
```

At 4.2 V, the ADC node is about 1.042 V and the divider draws about 3.16 µA. C1 is connected in parallel with R2. The firmware uses a fixed ratio of 1330/330, calibrated ADC readings, 6 dB attenuation and a median of 16 samples.

The divider is permanently connected. There is no divider-enable GPIO. Do not substitute a disconnected lower resistor or the earlier 100 kΩ / 100 kΩ arrangement without updating and validating the ADC design. Compare the voltage shown by HA with a multimeter; resistor ratio, ADC accuracy and leakage affect the result.

### Reed contact

```text
3V3 ── R3 100 kΩ ──●── D1 / GPIO1
                    └── normally open reed ── GND
```

The two sensor wires have no polarity. Open contact reads HIGH; closed contact reads LOW. The external pull-up is required. Stable LOW for 30 ms counts once; stable HIGH for 30 ms rearms it. A permanently closed reed should not cause repeated counting or a wake loop.

The 100 kΩ pull-up draws about 33 µA while the reed is closed. Include this in real sleep-current measurements.

### Button and LED

```text
D2 / GPIO2 ── normally open button ── GND
D4 / GPIO22 ── R4 1 kΩ ── LED anode → cathode ── GND
```

Use the two push-button terminals that actually close when pressed; four-legged buttons often have two permanently connected pairs. The button does not need an external pull-up.

The LED anode is typically the longer lead; the cathode is the shorter lead next to the flat edge. The external LED is active HIGH. The board's GPIO15 user LED is active LOW and is kept off during normal operation.

### Battery and antenna

Connect the cell to the rear battery pads with correct polarity. The divider's BAT+ is the same positive node. Use one cell only.

Radio startup drives GPIO3 LOW, waits 100 ms, then drives GPIO14 HIGH to select the external antenna. These RF-control pins are separate from the D0/D1/D2/D4 wiring.

## Before mounting on a meter

Use the meter's external pulse-reading interface; do not alter gas plumbing or meter seals. Check the meter's printed pulse constant. The default software value is 0.01 m³ per pulse.

Measure the shortest actual HIGH/LOW interval and compare it with wake latency and debounce time. See [hardware acceptance checks](TESTING.md#hardware-acceptance).

## Rebuilding the diagrams

The PNG and SVG diagrams are included in Git. Regeneration is optional:

```sh
python3 -m venv .venv-diagram
.venv-diagram/bin/python -m pip install Pillow
.venv-diagram/bin/python tools/draw_wiring.py --lang en
.venv-diagram/bin/python tools/draw_wiring.py --lang hu
```

The renderer uses Arial on macOS or DejaVu Sans where available. If neither is installed, pass `--font-regular /path/to/regular.ttf --font-bold /path/to/bold.ttf`. Geometry and component values are shared between languages.

Board reference: [Seeed XIAO ESP32-C6 pin map and antenna selection](https://wiki.seeedstudio.com/xiao_esp32c6_getting_started/).

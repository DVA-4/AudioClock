# Hardware

## Components

| Part | Details |
|---|---|
| MCU | Teensy 4.0 (NXP i.MX RT1062, 600 MHz Cortex-M7) |
| Audio | PJRC Audio Board Rev D (SGTL5000), stacked on the Teensy |
| Level shifter | BSS138 4-channel bidirectional module (3.3 V ↔ 5 V) |
| Display | SSD1306 0.96″ OLED, 128×64, I2C address 0x3C, run at 1 MHz |
| Mic | PUI AOM-5024L electret condenser |
| Pots | 3 × linear (10k typical) |
| Switches | 1 × SPDT (input), 1 × SP3T or ON-OFF-ON (responsiveness) |
| Enclosure | 3D-printed PLA, 10° slanted panel, slightly larger than a Hammond 1590BB |
| Power | 9 V DC barrel → linear regulator → 5 V → Teensy's 3.3 V regulator |

A linear regulator was chosen on purpose so that no switching noise gets
into the audio path.

**Do not overclock.** The firmware has enough headroom at 600 MHz, and a
sealed PLA enclosure has no airflow.

## Pinout (Teensy 4.0)

| Pin | Function | Notes |
|---|---|---|
| 1 (TX1) | MIDI out | → BSS138 ch1 → DIN-5 pin 5 |
| 2 | HOLD input | `INPUT_PULLUP`, never driven. Contact to GND = press. |
| 5 | Input switch | GND = LINE, 3.3 V = MIC (`INPUT_PULLDOWN`) |
| 9 | Beat LED | → 1 kΩ → LED anode |
| 10 | CV clock out | → BSS138 ch3 → 3.5 mm jack tip. The only CV output. |
| 14 (A0) | BIAS pot | wiper |
| 15 (A1) | DIV/MULT pot | wiper |
| 16 (A2) | GATE pot | wiper |
| 18 / 19 | I2C SDA / SCL | Shared by the OLED and the Audio Board codec |
| 22 (A8) | RESP switch | 3-state, read with `analogRead` (below) |
| 6, 7, 8, 20, 21, 23 | Audio Board I2S | Handled by stacking; no wiring needed |

### Pins to avoid

- **Pin 3.** It's dead on the original prototype (damaged in an earlier
  project). On a fresh Teensy it works, but the firmware doesn't use it.
- **Pin 4.** It isn't analog-capable, so it can't read the 3-state switch.
- **Pin 17 (A3).** Serial4 TX holds this pin and interferes with `analogRead`.

### 3-state RESP switch

```
3.3 V ── 100 kΩ ──┬── 100 kΩ ── GND
                  │
               pin 22 ── switch common
switch throws: GND  |  (open)  |  3.3 V
```

`analogRead` thresholds: < 200 = FST, 200–800 = MED, > 800 = SLW. The divider
holds the centre (open) position at mid-rail, so it reads reliably instead of
floating.

## Level shifter

```
LV rail → 3.3 V      HV rail → 5 V      GND → GND

ch1  LV1 ← pin 1    HV1 → 220 Ω → DIN pin 5        (MIDI TX)
ch2  LV2 ← pin 2    HV2 ← 3.5 mm HOLD jack tip     (input)
ch3  LV3 ← pin 10   HV3 → 3.5 mm CV jack tip       (CV out)
ch4  spare
```

Channel 2 used to be a CV output (removed in Build 50007). Since pin 2 is
now only an input, the BSS138 isn't needed on that channel anymore. A panel
push button wired straight from pin 2 to GND works just as well, and so
does a jack.

## MIDI out

```
+5 V ── 220 Ω ── DIN pin 4
HV1  ── 220 Ω ── DIN pin 5
                 DIN pin 2 ── GND (shield)
```

**Pin 4 goes to +5 V and pin 5 goes to the TX signal. They are not the same
node.** Current flows from pin 4, through the receiver's optocoupler LED, into
pin 5, and to ground whenever TX is low. Earlier versions of the project's
notes showed both pins fed from HV1. Wired that way, no current can ever
flow and MIDI won't work.

You don't need an optocoupler, because isolation is only required on
MIDI **in**. Sinking current is the BSS138's strong direction, which is what
this current loop needs.

## Mic input

```
AOM-5024L  VDD → Audio Board mic bias (3.3 V)
           GND → Audio Board GND
           OUT → Audio Board MIC IN
```

No external components are needed. Mic gain is set in firmware
(`MIC_GAIN_DB = 40`).

## Build notes

- Solder with the board unpowered and take ESD precautions. The first
  prototype was lost to a short during rewiring.
- Keep the OLED I2C wires short if you want to run it at 1 MHz.
- A centre detent or panel mark at 12 o'clock on the BIAS pot (120 BPM)
  makes half-time readings easy to spot at a glance.

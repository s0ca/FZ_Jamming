# nRF24 Jammer BETA - Flipper Zero

A 2.4 GHz RF noise/interference tool for the Flipper Zero, driven by one to four
**nRF24L01+** modules. It provides broadband sweeping and single‑channel
"freeze" jamming across the Bluetooth, BLE, Wi‑Fi, Zigbee and drone bands, with
a fully code‑drawn UI (no bitmap assets).

> This is heavily inspired by
> [`FZ_nRF24_jammer`](https://github.com/W0rthlessS0ul/FZ_nRF24_jammer) by
> **W0rthlessS0ul**. See [Credits](#credits).

---

## ⚠️ Legal disclaimer - read this first

Transmitting a continuous carrier or spamming packets on the 2.4 GHz band is
**radio jamming**. In most countries (US: FCC, EU: RED/national regulators, etc.)
operating a jammer is **illegal**, including on unlicensed ISM bands, and can
carry heavy fines or criminal charges. It also disrupts other people's devices
(headphones, medical devices, Wi‑Fi, security systems…).

This project is provided **for education and authorized testing only**, to be
used **exclusively inside a shielded RF enclosure / Faraday cage or on
equipment you own and control**. You are solely responsible for how you use it.
The authors accept no liability.

---

## Table of contents

- [Features](#features)
- [Hardware](#hardware)
- [Wiring](#wiring)
- [Building &amp; installing](#building--installing)
- [Controls](#controls)
- [Menu reference (every option explained)](#menu-reference-every-option-explained)
- [Settings reference](#settings-reference)
- [How it works](#how-it-works)
- [Effectiveness &amp; limitations (honest notes)](#effectiveness--limitations-honest-notes)
- [Settings persistence](#settings-persistence)
- [Credits](#credits)
- [License](#license)

---

## Features

- Jamming profiles for **Bluetooth (BR/EDR), BLE, Wi‑Fi, Zigbee, drones** and a
  free **Misc** range mode.
- **Freeze** mode on Bluetooth: stop hopping and park all carriers on the
  current channel, then fine‑tune the frequency live.
- Support for **1 to 4 nRF24L01+ modules** running in parallel.
- **Separate** vs **Together** multi‑module strategies.
- Adjustable **per‑channel dwell** for the Bluetooth sweep.
- Clean, fully **code‑drawn UI** (vertical list menu, live status screens) no
  PNG assets, small `.fap`.
- Persistent settings.

---

## Hardware

- **Flipper Zero.**
- **1–4× nRF24L01+** modules (the "+" variant; PA/LNA versions transmit
  stronger but draw more current).
- Jumper wires, or a ready‑made nRF24 adapter board for the Flipper GPIO.

The app enables the Flipper's **5 V OTG rail (pin 1)** on start. Power your
module according to its board:

- **Bare nRF24L01+** modules are **3.3 V** feed VCC from **pin 9 (3V3)**, never 5 V.
- **PA/LNA boards with an on‑board regulator + level shifter** usually accept
  **5 V on pin 1**. Check your board's silkscreen/spec.

---

## Wiring

All modules share the same SPI bus; each module gets its own **CE** and **CSN**
pins. Pin numbers below are the physical Flipper GPIO header pins.

### Shared SPI bus (every module)

| Signal | Flipper pin | MCU pin |
|--------|-------------|---------|
| SCK    | 5           | PB3     |
| MISO   | 3           | PA6     |
| MOSI   | 2           | PA7     |
| GND    | 8 / 11 / 18 | GND     |
| VCC    | 9 (3V3) *or* 1 (5V) see [Hardware](#hardware) | - |

### Per‑module CE / CSN - "Default" SPI mode (up to 4 modules)

| Module | CE (pin) | CSN (pin) |
|--------|----------|-----------|
| 1      | 6  (PB2)          | 4  (PA4)          |
| 2      | 10 (SWC / PA14)   | 7  (PC3)          |
| 3      | 15 (PC1)          | 12 (SIO / PA13)   |
| 4      | 17 (iButton)      | 16 (PC0)          |

> Modules 2–4 use the SWD debug pins (10/12) and the iButton pin (17). You can't
> use SWD debugging at the same time as a 3+ module setup.

### "Extra" SPI mode (single 2‑in‑1 nRF24 + CC1101 board)

Some combo boards put the nRF24's CSN on **pin 7 (PC3)** because the CC1101
occupies pin 4. In that case select **Settings → SPI Pin → Extra 7**. Only
**one** module is used in this mode:

| Signal | Flipper pin |
|--------|-------------|
| CE     | 6  (PB2)    |
| CSN    | 7  (PC3)    |

On boot the app auto‑detects how many modules respond and shows the count. If
none are found you'll see a **"No module"** screen - check wiring and power.

---

## Building &amp; installing

The app is built with **uFBT** (micro Flipper Build Tool).

### 1. Install uFBT

```bash
# Linux / macOS
python3 -m pip install --upgrade ufbt
# Windows
py -m pip install --upgrade ufbt
```

### 2. Build

From the application directory (the folder containing `application.fam`):

```bash
ufbt
```

The compiled package is written to `dist/jammer_beta.fap`.

### 3. Install &amp; run

Connect the Flipper by USB (data‑capable cable) and make sure **qFlipper** and
**lab.flipper.net** are **closed**, then:

```bash
ufbt launch     # build + upload + run on the Flipper
```

Or install the `.fap` manually: copy `dist/jammer_beta.fap` to your Flipper's
SD card under `apps/GPIO/` (via qFlipper or a card reader). It then appears in
**Apps → GPIO → nRF24 Jammer BETA**.

### Useful uFBT commands

| Command | Purpose |
|---------|---------|
| `ufbt`            | Build the `.fap` into `dist/` |
| `ufbt launch`     | Build, upload and run on the Flipper |
| `ufbt cli`        | Open the Flipper CLI |
| `ufbt update`     | Update the bundled firmware SDK |
| `ufbt format`     | Apply `clang-format` to the sources |
| `ufbt vscode_dist`| Generate a VS Code project config |
| `ufbt -h`         | List all commands |

> Building requires internet the first time so uFBT can fetch the firmware SDK.

---

## Controls

General navigation of the main menu:

| Key | Action |
|-----|--------|
| **Up / Down** | Move the selection in the list |
| **OK**        | Enter / start the highlighted item |
| **Back**      | Leave a sub‑screen, or exit the app from the main menu |

Context‑specific keys are described per menu below.

---

## Menu reference (every option explained)

Frequencies use the nRF24 convention: **channel _k_ = (2400 + _k_) MHz**
(e.g. channel 40 = 2440 MHz). The nRF24 can address channels 0–125
(2400–2525 MHz).

### Bluetooth

Jams **Bluetooth BR/EDR** (Classic) using a **continuous carrier** hopped across
the band. Press **OK** to start; **Back** to stop.

While jamming:

- **OK** - toggle **Freeze**. In *HOP* it sweeps; in *HOLD* it stops on the
  current channel and concentrates **every module on that single frequency**.
- **Up / Down / Left / Right** - while frozen, tune the held channel
  (hold a key to sweep quickly). The screen shows the live frequency in MHz.

The hop pattern depends on **Settings → Bluetooth**:

- **List** - cycles a curated subset of channels. Fewer channels ⇒ each is
  revisited more often ⇒ more energy per targeted channel (but channels outside
  the list are never hit).
- **Random** - random channels in the BT band (2–80 / 2402–2480 MHz).
- **Bruteforce** - linear sweep of the full BT band (channels 2–80).

### Drone

Broadband **continuous‑carrier** sweep over the **entire nRF24 range
(channels 0–125, 2400–2525 MHz)** - wider than Bluetooth to cover common 2.4 GHz
drone control/telemetry links. **OK** start, **Back** stop. Method
(**Bruteforce** or **Random**) is set in **Settings → Drone**.

### WiFi

Targets 2.4 GHz Wi‑Fi by **spamming packets** across the channel's ~22 MHz width.
Press **OK** to open the sub‑screen:

- **Up / Down** - switch mode:
  - **All channels** - sweeps Wi‑Fi channels 1–13 continuously.
  - **Select channel** - pick one Wi‑Fi channel.
- If **Select channel**: press **OK**, then **Up/Down/Left/Right** to choose the
  Wi‑Fi channel (**1–14**), then **OK** to start.
- **Back** - step back / stop.

Each Wi‑Fi channel _n_ is mapped to nRF24 channels `(n‑1)·5 + 1` … `+ 23` to
cover the whole 22 MHz Wi‑Fi channel.

### BLE

Targets **Bluetooth Low Energy**. Press **OK** to open, **Up/Down** to choose,
**OK** to start:

- **Advertising channels** - spams packets on the 3 BLE advertising channels
  (nRF24 channels 2 / 26 / 80 = 2402 / 2426 / 2480 MHz). This is what disrupts
  device discovery/pairing.
- **Data channels** - continuous‑carrier sweep across the BLE data channels
  (even channels 2…80).

### Zigbee

**Packet‑spam** across the 16 Zigbee channels (11–26). Each Zigbee channel is
mapped to its nRF24 equivalent (`4 + 5·(z‑11)` … `+2`). **OK** start,
**Back** stop.

### Misc

A **free‑range** mode where you define the band yourself.

1. Press **OK**: you're in **Set Start**.
   - **Up / Down** - set the start channel (0–125). Tap repeatedly or hold to
     move faster (×1 → ×9 → ×90 acceleration).
   - **Left / Right** - switch the **Mode**:
     - **Channel switching** - continuous carrier swept over the range.
     - **Packet sending** - packet spam over the range.
   - **OK** - confirm and move to **Set Stop**.
2. **Set Stop**: same keys to set the stop channel (must be **>** start).
   **OK** starts jamming.
3. **Back** steps back through the screens.

### Settings

Opens the configuration list - see [Settings reference](#settings-reference).
**OK** or **Back** saves.

### Infos

Shows app name, author and revision. **Back** returns.

---

## Settings reference

Navigate with **Up/Down**; change a value with **Left/Right**; **OK**/**Back**
saves.

| Setting | Values | Meaning |
|---------|--------|---------|
| **SPI Pin**   | `Default 4` / `Extra 7` | CSN pin for module 1. `Default 4` = standalone nRF24 (CSN on pin 4). `Extra 7` = 2‑in‑1 nRF24+CC1101 board (CSN on pin 7, single module). |
| **Modules**   | `Separate` / `Together` | Multi‑module strategy - see below. |
| **Bluetooth** | `List` / `Random` / `Brute` | Hop pattern for the Bluetooth profile. |
| **Drone**     | `Brute` / `Random` | Sweep pattern for the Drone profile. |
| **BT Dwell**  | `0 / 100 / 200 / 400 us` | Time spent per channel during the Bluetooth sweep - see below. |
| **BT Dither** | `Off` / `1 ch` / `2 ch` | Per‑carrier dithering — widen each carrier's footprint - see below. |

### Modules: Separate vs Together

- **Together** - all modules are written to the **same channel** at the same
  time. Maximum power on one channel at a time (redundant carriers stack).
- **Separate** - the modules are **spread across the band** so several distinct,
  well‑separated channels are jammed **simultaneously**. With _N_ modules on the
  Bluetooth sweep, module _i_ is offset by roughly `i · (band / N)`, so the _N_
  carriers form an even comb that sweeps together. This is the more effective
  strategy against frequency‑hopping links, because it keeps more channels "bad"
  at once. (Single‑module setups behave identically in both modes.)

### BT Dwell

Changing the nRF24 channel forces the PLL to re‑lock (on the order of ~130 µs).
A very tight sweep can hop away before the carrier is fully established, so the
energy actually radiated per channel is poor. **BT Dwell** inserts a short pause
after each channel write so the carrier settles:

- `0 us` - fastest sweep (original behavior), highest revisit rate, weakest
  per‑visit presence.
- `100–400 us` - fewer revisits per second, but a cleaner, stronger carrier on
  each visited channel.

There is **no universally best value** - it depends on the target and the number
of modules. Treat it as a knob to experiment with.

### BT Dither

A single carrier only occupies **1 MHz** (one channel). **BT Dither** makes each
carrier **micro‑hop around its center channel** so it smears its energy over
several adjacent channels instead of one:

- `Off` - one carrier = one channel (default, original behavior).
- `1 ch` - the carrier cycles `center, +1, -1` → ~3 MHz footprint.
- `2 ch` - the carrier cycles `center, +1, -1, +2, -2` → ~5 MHz footprint.

Dither applies to **both the sweep and the Freeze** mode. It is most useful with
**Freeze**: instead of parking on a single frequency, the carrier(s) cover a few
channels around it, which is more forgiving on a target that drifts slightly.

The trade‑off: a wider footprint means **lower power density per channel**. Each
dithered channel gets a short balanced dwell (the **BT Dwell** value if set,
otherwise ~60 µs) so the carrier spends comparable time on each. As with dwell,
it's a knob to test by ear - there is no single best value, and it does not
defeat AFH hopping (see [limitations](#effectiveness--limitations-honest-notes)).

---

## How it works

The app uses two jamming techniques depending on the profile:

- **Continuous carrier (CW).** Puts the nRF24 into constant‑wave mode
  (`RF_SETUP` `CONT_WAVE` + `PLL_LOCK`) and rapidly rewrites the `RF_CH` register
  to move the tone across the band. Used by **Bluetooth, Drone, BLE data,
  Misc → Channel switching**.
- **Packet spam.** Repeatedly transmits no‑ACK payloads on the target channels,
  raising the effective noise/collision level. Used by **Wi‑Fi, Zigbee,
  BLE advertising, Misc → Packet sending**.

The Bluetooth loop advances **one channel per iteration** (rather than sweeping
the whole band per iteration). This keeps a well‑defined "current channel" so
**Freeze** can park exactly there, lets the loop react instantly to stop/freeze,
and - in **Separate** mode - spreads multiple modules evenly across the band.

---

## Effectiveness &amp; limitations (honest notes)

RF jamming with a single nRF24 is **not magic**, and it's important to
understand why, especially for Bluetooth audio:

- **Bluetooth Classic uses Adaptive Frequency Hopping (AFH):** it hops
  **1600 times per second** across **79 channels** and the master maintains a
  channel map that **actively removes jammed channels** from the hop sequence
  (keeping a spec‑mandated **minimum of ~20 good channels**). So:
  - **Freeze (single channel) barely affects a hopping audio link** - the link
    detects the bad channel and routes around it. Freeze is meant for
    **fixed‑frequency** targets (a device parked on one channel, some
    proprietary 2.4 GHz gear, BLE advertising on a specific channel), not for a
    hopping A2DP stream.
  - To meaningfully disrupt a hopping link you must keep **many** channels bad
    **at the same time** - which is exactly why **multiple modules in Separate
    mode** is the strongest configuration here.
- **List vs Bruteforce** is a coverage/concentration trade‑off: List hammers a
  few channels hard; Bruteforce spreads thinner over the whole band.
- A single unmodulated carrier is a comparatively weak jammer per channel;
  packet spam or multiple spread carriers are often more disruptive.

Sources on the Bluetooth behavior:
[Rohde &amp; Schwarz - BR/EDR AFH app note](https://scdn.rohde-schwarz.com/ur/pws/dl_downloads/dl_application/application_notes/1c108/1C108_0e_Bluetooth_BR_EDR_AFH.pdf),
[Electronics Notes - Bluetooth Classic](https://www.electronics-notes.com/articles/connectivity/bluetooth/bluetooth-classic-technology-operation.php).

---

## Settings persistence

Settings are stored on the Flipper SD card at:

```
/ext/apps_data/jammer/settings.txt
```

They are saved when you leave/confirm the Settings screen and on app exit, and
reloaded on next launch.

---

## Credits

- Original project: **W0rthlessS0ul** -
  [`FZ_nRF24_jammer`](https://github.com/W0rthlessS0ul/FZ_nRF24_jammer).
- This enhanced version (Freeze mode, code‑drawn UI, BT‑band bruteforce,
  adjustable dwell, multi‑module spread, Infos screen): **Mathias s0ca**.
- Built on the Flipper Zero firmware and the bundled `nrf24` library.

---

## License

Released under the **MIT License** - see [`LICENSE`](LICENSE).

This project reuses and modifies code from
[`FZ_nRF24_jammer`](https://github.com/W0rthlessS0ul/FZ_nRF24_jammer)
(© 2025 W0rthlessS0ul), which is itself MIT‑licensed. As required by MIT, the
original copyright notice is retained in the `LICENSE` file alongside the
copyright for the modifications (© 2026 Mathias s0ca).

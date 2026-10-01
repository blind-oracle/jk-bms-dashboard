# BMS Dashboard

Firmware for a small battery-powered display that shows live data from a
JK-B/BD BMS (the "JK BMS" with the 4-pin GPS port). It is built for the
DFRobot FireBeetle 2 ESP32-C6 and a 240x240 ST7789 SPI display, with one
push button.

What it does:

- polls the BMS over its TTL "GPS" UART (protocol frames start with `4E 57`)
  once a second;
- shows pack voltage, current, power, SOC, cell voltages, temperatures, alarms
  and BMS details on four pages;
- switches the BMS **discharge output** (the discharge MOSFET) on or off with a
  deliberate hold-and-release gesture;
- goes into deep sleep after 5 minutes without a button press, with the display
  shut down. A button press wakes it.

The dashboard only displays data and sends the discharge switch command. It
does not protect anything: the BMS does. Nothing is monitored while the
dashboard sleeps.

## Hardware

| Part | Notes |
|---|---|
| DFRobot FireBeetle 2 ESP32-C6 (DFR1075) | ESP32-C6, 4 MB flash, USB-C, Li-ion charger, battery connector |
| 240x240 ST7789 SPI display module, 8 pins | Pins: GND, VCC, SCL, SDA, RES, DC, CS, BLK. Usually sold as a 1.3" or 1.54" IPS module. The module must have a CS pin. |
| Momentary push button | Normally open |
| Single-cell Li-ion/LiPo battery | Fits the FireBeetle battery connector (JST PH 2.0). The board charges it from USB. |
| JK BMS GPS-port cable | 4-pin 1.25 mm pitch JST-style plug on the BMS side |
| Optional: 100 kΩ resistor | External pull-up for the button |
| Optional: ADUM1201 digital isolator | UART isolation, see [Safety](#safety) |
| Optional: AO3401 P-MOSFET + 100 kΩ resistor | Display power switch for the lowest sleep current, see [Sleep current](#deep-sleep-and-sleep-current) |

## Wiring

### Display (8-pin ST7789 module)

| Display pin | FireBeetle | Silk on the board |
|---|---|---|
| VCC | 3V3 | 3V3 |
| GND | GND | GND |
| SCL | GPIO23 | 23/SCK |
| SDA | GPIO22 | 22/MO |
| RES | GPIO19 | 19/SDA |
| DC | GPIO20 | 20/SCL |
| CS | GPIO21 | 21/MI |
| BLK | GPIO1 | 1/D6 |

GPIO20 and GPIO19 also appear on the right header as "SCL" and "SDA" (I2C).
Leave those pads unconnected.

### Button

| Button | FireBeetle |
|---|---|
| One side | GPIO3 (silk 3/A2) |
| Other side | GND |

The firmware enables the internal pull-up, so no resistor is needed. With long
wires, add a 100 kΩ pull-up from GPIO3 to 3V3. The button must be on GPIO0-7:
only those pins can wake the ESP32-C6 from deep sleep, and GPIO0 is taken by the
battery sense divider.

### JK BMS GPS port

The port labels are as printed on the BMS side.

| BMS GPS port | FireBeetle |
|---|---|
| GND | GND |
| TX | GPIO4 (silk 4/A3/LP_RX), the ESP32 RX |
| RX | GPIO5 (silk 5/A4/LP_TX), the ESP32 TX |
| VBAT | **Not connected** |

The UART runs at 115200 baud, 8N1, 3.3 V logic. Connect it directly: no level
shifter is needed. Do not use 5 V logic.

### Board battery

Plug the Li-ion cell into the FireBeetle's battery connector. **Check the
polarity first.** JST PH battery leads from different vendors are wired both
ways round, and a reversed cell damages the board. The firmware measures the
cell through the board's built-in divider on GPIO0. Nothing needs wiring.

### Pins left free

Pins deliberately left unused:

- GPIO8, 9 and 15 are strapping pins. GPIO9 is the BOOT button and GPIO15 is the
  green LED.
- GPIO12 and 13 are USB.
- GPIO16 and 17 are UART0, which carries the ROM boot log.

## Safety

- **Never connect the VBAT pin of the GPS port.** It carries the full pack
  voltage and destroys the ESP32 instantly.
- **The dashboard's GND becomes the BMS GND (B-).** On a JK BMS the MOSFETs sit
  between B- and P-. A USB host or charger whose ground is tied to P-, to earth
  or to the system the pack powers can bridge B- and P- through the USB cable.
  That bypasses the BMS MOSFETs and can carry large currents through the cable.
  While the dashboard is wired to the BMS, do one of these:
  - leave USB unplugged and run from the board's own battery;
  - use a battery-powered laptop that is not connected to anything else;
  - isolate the UART with an ADUM1201 (or similar) digital isolator. The
    dashboard's GND is then no longer B-. The BMS side of the isolator needs
    its own 3.3 V supply referenced to B-, for example a small step-down
    regulator from the pack. Never connect the pack voltage to the isolator
    directly.
- The BMS UART is 3.3 V logic. A 5 V USB-TTL adapter on the same lines can
  damage both sides.
- Toggling the discharge output switches the pack's load off or on. Make sure
  that is safe for whatever the pack powers before you use the gesture.

## Build, flash and monitor

Requirements: [PlatformIO Core](https://platformio.org/install) (CLI or the VS
Code extension). On the first build, PlatformIO downloads the ESP32 platform
(espressif32 7.1.3, ESP-IDF 6.1). The ESP-IDF component manager fetches LVGL
9.6 and esp_lvgl_port 2.9 into `managed_components/`. `dependencies.lock` pins
their versions; commit it.

| Task | Command |
|---|---|
| Build | `pio run` |
| Flash | `pio run -t upload` |
| Serial monitor | `pio device monitor` |
| Configuration menu | `pio run -t menuconfig` |
| Host unit tests (protocol parser) | `pio test -e native` |

The console is on the native USB-C port (USB-Serial/JTAG). The baud rate does
not matter. The host tests need a C compiler on the host (gcc or clang).

**USB disappears while the board sleeps.** The USB port only exists while the
ESP32-C6 is awake. Before uploading:

- press the button to wake the board, then upload within the sleep timeout
  (5 minutes by default); or
- hold BOOT, tap RST and release BOOT. This enters the ROM download mode, which
  does not sleep. Tap RST after flashing if the board does not restart by itself.

`pio device monitor` reconnects by itself when the port comes back after a sleep
or reset. For development, `BMSD_NO_SLEEP_WITH_USB_HOST` keeps the board awake
while a USB host is connected. A plain USB charger does not count as a host.

## Configuration (menuconfig)

All settings are in `pio run -t menuconfig`, menu **BMS Dashboard**. Changes are
saved to `sdkconfig.dfrobot_firebeetle2_esp32c6`, and the next build uses them.
To make a setting the project default, put it in `sdkconfig.defaults`
(for example `CONFIG_BMSD_SLEEP_TIMEOUT_S=60`).

Two build-system rules apply:

- Changes to `sdkconfig.defaults` do not reliably reach an existing
  `sdkconfig.dfrobot_firebeetle2_esp32c6`. **After editing `sdkconfig.defaults`,
  delete `sdkconfig.dfrobot_firebeetle2_esp32c6`.** The next build recreates it.
  Deleting it also discards all menuconfig changes.
- PlatformIO does not notice changes to `src/Kconfig.projbuild` or
  `src/idf_component.yml` by itself. **After editing either one, touch
  `src/CMakeLists.txt`.** For the same reason, `src/CMakeLists.txt` lists the
  sources explicitly. Add any new source file to its `SRCS` list.

Options. The symbol names are shown without the `CONFIG_` prefix; press `/` in
menuconfig to search for one.

**Pins**

| Option | Default | Meaning |
|---|---|---|
| `BMSD_PIN_LCD_SCLK` | 23 | LCD SCL (SPI clock) |
| `BMSD_PIN_LCD_MOSI` | 22 | LCD SDA (SPI data) |
| `BMSD_PIN_LCD_CS` | 21 | LCD CS |
| `BMSD_PIN_LCD_DC` | 20 | LCD DC |
| `BMSD_PIN_LCD_RST` | 19 | LCD RES |
| `BMSD_PIN_LCD_BL` | 1 | LCD BLK (backlight). An LP GPIO (0-7) is recommended: its hold in deep sleep is guaranteed. |
| `BMSD_PIN_LCD_PWR` | -1 | Optional display power switch, -1 = not used |
| `BMSD_LCD_PWR_ACTIVE_HIGH` | n | Leave off for a P-MOSFET high-side switch (GPIO low = display on) |
| `BMSD_PIN_BUTTON` | 3 | Button to GND. Must be GPIO0-7. |
| `BMSD_PIN_BMS_RX` | 4 | ESP32 RX, to the BMS TX pin |
| `BMSD_PIN_BMS_TX` | 5 | ESP32 TX, to the BMS RX pin |
| `BMSD_PIN_VBAT_ADC` | 0 | Board battery sense (ADC1) |

**Timing**

| Option | Default | Meaning |
|---|---|---|
| `BMSD_SLEEP_TIMEOUT_S` | 300 | Seconds without a button press before deep sleep. 0 = never sleep. |
| `BMSD_SLEEP_WARN_S` | 10 | Dim the screen and warn this many seconds before sleeping. 0 = no warning. Limited to half the timeout. |
| `BMSD_POLL_INTERVAL_MS` | 1000 | BMS poll interval (200-10000) |
| `BMSD_BMS_RX_TIMEOUT_MS` | 500 | Reply timeout per poll (50-2000) |
| `BMSD_BMS_OFFLINE_POLLS` | 3 | Consecutive failed polls before the BMS is shown as offline |

**Button**

| Option | Default | Meaning |
|---|---|---|
| `BMSD_HOLD_START_MS` | 500 | Hold time before the output card appears. A shorter press is a short press. |
| `BMSD_LONG_PRESS_MS` | 2000 | Hold time that arms the output switch. Must be at least 500 ms more than the hold-start time. |
| `BMSD_ARM_WINDOW_MS` | 3000 | After arming, release within this window to switch. Holding longer cancels. |

**Display**

| Option | Default | Meaning |
|---|---|---|
| `BMSD_LCD_SPI_MHZ` | 40 | SPI clock, 10-80 MHz |
| `BMSD_LCD_SPI_MODE` | 0 | SPI mode (0-3) |
| `BMSD_LCD_INVERT` | y | Invert colours (most IPS panels need this) |
| `BMSD_LCD_BGR` | n | Panel uses BGR element order |
| `BMSD_LCD_ROTATION` | 0° | 0°, 90°, 180° or 270° |
| `BMSD_LCD_GAP_X` | 0 (80 for 270°) | Column offset of the 240x240 glass in the 240x320 controller RAM |
| `BMSD_LCD_GAP_Y` | 0 (80 for 180°) | Row offset |
| `BMSD_LCD_BL_ACTIVE_HIGH` | y | Backlight polarity |
| `BMSD_LCD_BRIGHTNESS_PCT` | 100 | Normal backlight brightness |
| `BMSD_LCD_DIM_PCT` | 20 | Brightness during the sleep warning |
| `BMSD_LCD_FAST_WAKE` | y | After a button wake, skip the display reset and init. Not used with the power switch. |

The gap follows the rotation automatically unless you have set a gap value
yourself. In that case, set it to match: 180° needs Y=80 and 270° needs X=80.

**BMS**

| Option | Default | Meaning |
|---|---|---|
| `BMSD_JK_FRAME_SOURCE` | GPS (0x02) | Source byte in our requests: GPS (0x02, `BMSD_JK_FRAME_SOURCE_GPS`, as current esphome-jk-bms) or PC (0x03, `BMSD_JK_FRAME_SOURCE_PC`) |
| `BMSD_JK_WRITE_DELAY_MS` | 200 | Delay between the password frame and the write frame |
| `BMSD_JK_SEND_ACTIVATION` | n | Send the activation frame (0x01) while the BMS is offline. Untested. |

**Board battery**

| Option | Default | Meaning |
|---|---|---|
| `BMSD_VBAT_DIVIDER_X1000` | 2000 | Divider ratio x1000 (the FireBeetle has 1M/1M, so 2.000) |
| `BMSD_VBAT_OFFSET_MV` | 0 | Calibration offset added to the reading, in mV |
| `BMSD_VBAT_WARN_MV` | 3400 | Low-battery warning threshold, in mV |
| `BMSD_VBAT_SAMPLES` | 16 | ADC samples averaged per reading |

**Development**

| Option | Default | Meaning |
|---|---|---|
| `BMSD_DEMO_MODE` | n | Synthetic BMS data. The UART is not used. |
| `BMSD_NO_SLEEP_WITH_USB_HOST` | n | Do not sleep while a USB host is connected |

## Using it

### Button gestures

| Gesture | Effect |
|---|---|
| Short press (under 0.5 s) | Next page |
| Hold 0.5 s | The output card appears: "Hold: enable output", "Hold: disable output" or "Hold: disable output (tripped)", with a progress bar |
| Keep holding to 2 s | The card says "Release to switch output ON" (or OFF): the switch is armed |
| Release within 3 s of arming | The command is sent. "Switching output..." is shown, then the result. |
| Release between 0.5 s and 2 s | "Cancelled". Nothing is sent and the page does not change. |
| Hold more than 3 s after arming | "Cancelled". Nothing is sent, so a stuck button never switches the output. |
| Any press during "Sleeping in N s" | Cancels the sleep and restores the brightness. That press does nothing else. |
| The press that wakes the board | Wakes it, nothing else. The board always wakes on the Main page. |

The times are the defaults of `BMSD_HOLD_START_MS`, `BMSD_LONG_PRESS_MS` and
`BMSD_ARM_WINDOW_MS`.

The switch command is absolute. If the discharge output is on, the gesture
switches it off. If it is off, the gesture switches it on. The decision is made
when the card appears. "Tripped" means the discharge setting is on, but a BMS
protection keeps the MOSFET off. Switching then turns the setting off.

The gesture is refused with a short message when the switch cannot be done
safely:

- "No data": no frame from the BMS yet;
- "BMS offline": the link is down (the banner says so too);
- "No fresh data": the last frame is too old to trust, because a few polls in a
  row were missed;
- "Busy": a command is still running;
- "No display": the display failed to start, so the output is never switched
  without the cards on screen.

If the "Release to switch ..." card could not be drawn, releasing shows
"Cancelled" and nothing is sent.

Results:

| Message | Meaning |
|---|---|
| "Output ON" / "Output OFF" | Confirmed by reading the BMS back after the write |
| "Enabled - blocked by BMS" | The setting is on, but a protection keeps the MOSFET off (seen on two readings 300 ms apart) |
| "No reply from BMS" (red) | The BMS neither acknowledged the write nor answered afterwards |
| "Sent - state unknown" (red) | The BMS acknowledged the write, but no frame could be read back to check it |
| "Not confirmed - output is ON/OFF" (red) | The state read back after the write does not match the request |
| "Not confirmed - output still enabled" (red) | Switching off failed while tripped: the setting is still on, so the output returns when the protection clears |

A command that is not confirmed is retried once (password + write) before the
result is shown.

### Pages

The pages form a ring: Main, then Cells, then Alarms (only while an alarm is
active), then Details. If the alarms clear while the Alarms page is shown, the
display returns to Main.

- **Main**: SOC in large digits with a bar, and the CHG / DSG / BAL state. DSG
  is green when on, grey when off and amber when tripped. Below that:
  - pack voltage and power (in kW from 10 kW);
  - current, positive while charging and negative while discharging;
  - cell range, for example "3.301-3.345 (44 mV)";
  - temperatures (MOS and the reported sensors).

  A banner at the bottom shows the highest-priority alarm, plus "+N" for more.
  Protections are red and the low-capacity warning is amber. Until the first
  frame it says "Connecting...", and while the link is down it says
  "BMS offline".
- **Cells**: the minimum, maximum and difference, with balancing state, and every
  cell voltage. Up to 16 cells use two columns and 17-24 use three. The lowest
  cell is blue and the highest red when they differ by 5 mV or more. Cells
  reading 0 mV show a grey "--". Without cell data the page says "No cell data".
- **Alarms**: every active alarm. Protections are listed first in red, then the
  low-capacity warning in amber.
- **Details**:
  - temperatures, cycles, cycle Ah, nominal Ah, remaining and actual Ah;
  - CHG and DSG setting and MOSFET state. "(prot.)" marks a protection holding
    the MOSFET off.
  - balancer, BMS software version, manufacturer, BMS uptime and alarm count.

Until the first frame arrives, every value shows "--". After the link is lost,
the last values stay on screen greyed out.

### Status strip

The top line of every page shows:

- page dots, with the current page highlighted;
- the BMS link dot: green while data is fresh, amber when it is getting old,
  red when the BMS is offline;
- the board battery voltage with a battery icon. Below `BMSD_VBAT_WARN_MV` it
  turns red and blinks, and "Board battery low" is shown once per wake. The
  warning clears 50 mV above the threshold. The dashboard keeps running: this
  is a warning only.

While the board runs from USB, the battery reading shows the charger voltage,
about 4.2 V, not the cell voltage.

## Deep sleep and sleep current

`BMSD_SLEEP_WARN_S` (default 10 s) before `BMSD_SLEEP_TIMEOUT_S` (default 300 s)
without a button press, the display dims and counts down "Sleeping in N s". A
press during the countdown cancels it. When the timeout is reached, the board:

- stops polling;
- switches the backlight off;
- puts the panel into sleep (DISPOFF + SLPIN), or cuts its power when the
  optional switch is fitted;
- holds the display pins at fixed levels;
- leaves the UART pins high-impedance, so a powered-off BMS is never fed
  through them;
- enters deep sleep.

Sleep does not start during a button gesture (pressed, holding or armed) or
while a switch command is running. The BMS keeps its output state while the
dashboard sleeps.

The button wakes the board. The display comes back on the Main page in a
fraction of a second, and the waking press does nothing else.

Special cases:

- **Stuck button.** If the press that woke the board lasts more than 10 s, the
  board goes back to sleep. If the button is still held when the board goes to
  sleep, for whatever reason, the board waits up to 10 s for the release. If the
  button stays held, the board sleeps in a mode that wakes on the release. It
  then goes straight back to normal sleep without lighting the display. The
  next press wakes it normally.
- **Crash loop.** After 3 watchdog, panic or brownout resets in a row without
  reaching deep sleep, the board parks the display and sleeps until the button
  is pressed. Reaching deep sleep or a button wake clears the counter. So does
  normal running for 60 s with sleep disabled, or for the sleep timeout plus
  60 s while sleep is held off (for example by a USB host). A brownout loop usually means
  the board's cell is nearly flat: charge it. Check the serial log for the cause.
- **Crash while going to sleep.** If the board resets while it is shutting
  down, the next boot finishes the shutdown without lighting the display.

**Measuring sleep current**

1. Unplug USB.
2. Put a multimeter on its µA range in series with the battery lead.
3. Let the board fall asleep.

Expect about 16-36 µA for the board, plus the display module's sleep current.
The 3V3 rail stays on in deep sleep, so the panel stays powered in its sleep
mode, typically adding a few tens of µA.

The awake current (tens of mA) drops too much voltage across a meter's µA range,
which can brown out the board. Short the meter, or use its mA range, while the
board is awake, and switch to the µA range only once it sleeps. A power
profiler avoids the problem.

If the reading is much higher:

- **The module's BLK pull-up.** Many modules pull BLK up to VCC. Holding BLK low
  in sleep then costs 3.3 V / R, which is 330 µA for 10 kΩ. Remove or raise that
  resistor, or use the power switch below.
- **The optional display power switch.** It removes the display's current
  completely. It is a high-side P-MOSFET switch in the display's VCC line:

  | AO3401 (P-MOSFET) pin | Connect to |
  |---|---|
  | Source (S) | 3V3 |
  | Drain (D) | Display VCC (instead of 3V3) |
  | Gate (G) | GPIO18 (silk 18/D7), plus a 100 kΩ resistor to 3V3 |

  The gate pull-up keeps the display off whenever GPIO18 is not driven low:
  during reset, boot and deep sleep. Set `BMSD_PIN_LCD_PWR=18` and leave
  `BMSD_LCD_PWR_ACTIVE_HIGH` off: GPIO low means the display is on. With the
  switch, all display lines are driven low during sleep, and every wake does a
  full display init (fast wake is not used).

## Troubleshooting

| Symptom | What to try |
|---|---|
| Colours inverted (black background shows white) | Toggle `BMSD_LCD_INVERT` |
| Red and blue swapped | Toggle `BMSD_LCD_BGR` |
| Picture shifted, or a band of garbage at one edge | Rotation and gap: 180° needs `BMSD_LCD_GAP_Y=80`, 270° needs `BMSD_LCD_GAP_X=80` |
| Garbage or a wrong picture after waking, fine after a reset | Disable `BMSD_LCD_FAST_WAKE` |
| Backlight never lights, or is on while asleep | `BMSD_LCD_BL_ACTIVE_HIGH`; check the BLK wire |
| Backlight on but nothing drawn | Check SCL/SDA/CS/DC/RES wiring; try `BMSD_LCD_SPI_MHZ=20` or another `BMSD_LCD_SPI_MODE` |
| "Connecting..." forever, or "BMS offline" | Swap RX and TX (BMS TX goes to GPIO4). Check the GND wire. Try `BMSD_JK_FRAME_SOURCE` = PC (0x03). Make sure the BMS is switched on and awake. |
| Switching always says "No reply from BMS" | Try `BMSD_JK_FRAME_SOURCE` = PC (0x03) or a longer `BMSD_JK_WRITE_DELAY_MS` |
| No USB port on the computer | The board is asleep: press the button, or hold BOOT and tap RST |
| Board battery shows about 4.2 V | Normal while USB is connected |
| Board restarts a few times, then stays dark until the next press | Crash-loop protection. Read the reset cause in the serial log (`pio device monitor`). |

The serial log shows the reset and wake cause, the key settings, switch commands
and their results, and BMS communication problems.

## Demo mode

Enable `BMSD_DEMO_MODE` to try the display without a BMS. The UART is not used.
The firmware cycles through canned scenarios every 10 s:

- a 16S pack discharging at -12.34 A;
- a 24S pack charging at +175 A / 17.4 kW, balancing, at -20 °C, with one cell
  reading 0 mV;
- five alarms, including battery-box overtemperature, with the discharge output
  tripped;
- the BMS stops answering: the last values turn grey, then "BMS offline".

For the first 2 s after boot the simulated BMS is silent, so "Connecting..." is
shown. The switch gesture works, and its simulated results rotate through
success, no reply and "Enabled - blocked by BMS".

## Project layout

| File | Contents |
|---|---|
| `platformio.ini` | Build environments: the firmware and `native` host tests |
| `boards/dfrobot_firebeetle2_esp32c6.json` | Board definition (the platform does not ship one) |
| `CMakeLists.txt` | ESP-IDF project file |
| `sdkconfig.defaults` | Project defaults: flash size, console, watchdog, LVGL configuration |
| `dependencies.lock` | Pinned versions of LVGL and esp_lvgl_port |
| `src/CMakeLists.txt` | Component sources (listed explicitly) and IDF requirements |
| `src/idf_component.yml` | Component manager dependencies (LVGL, esp_lvgl_port) |
| `src/Kconfig.projbuild` | The "BMS Dashboard" menuconfig options |
| `src/app_config.h` | Kconfig values as 0/1 and numeric constants, rotation table, sanity checks |
| `src/app_events.c/.h` | Event queue from the button and BMS task to the app loop |
| `src/main.c` | Boot flow and app loop: navigation, output switch flow, inactivity and sleep, watchdog |
| `src/jk_proto.c/.h` | JK protocol in pure C: frame builders, checksum, receive assembler, parser, alarm names |
| `src/jk_bms.c/.h` | BMS task: UART1, fixed-rate polling, snapshot, discharge switch command, demo data |
| `src/button.c/.h` | Button state machine: short press, hold, arm, release, stuck detection |
| `src/battery.c/.h` | Board battery voltage through the ADC with calibration |
| `src/display.c/.h` | SPI, ST7789 panel, LVGL port, backlight PWM, fast wake, sleep preparation |
| `src/power.c/.h` | Wake handling, stuck-button and crash-loop paths, pin holds, deep-sleep entry |
| `src/ui.c/.h` | UI framework: page ring, status strip, overlay card |
| `src/ui_main.c` | Main page |
| `src/ui_cells.c` | Cells page |
| `src/ui_alarms.c` | Alarms page |
| `src/ui_details.c` | Details page |
| `test/test_jk_proto/test_main.c` | Host unit tests for the protocol module (`pio test -e native`) |

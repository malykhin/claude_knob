# Claude Knob hardware

Reference for building a case around the **LilyGO T-Knob** with a **Waveshare 1.28inch LCD Module** (GC9A01, 240×240) mounted on its static centre.

## Wiring the display to the top PCB

The T-Knob's static top breakout (T-Motor-TOP V1.0) exposes 4 GPIOs: IO19, IO8, TXD (IO16) and RXD (IO17), plus 3V3, 5VIN and GND. The top and bottom 4-pin headers duplicate the side pads.

Cut the PH2.0 cable that comes with the module and solder it to the pads:

| Module pin | Top PCB pad | Why |
|---|---|---|
| VCC | 3V3 | |
| GND | GND | |
| CLK | IO19 | SPI clock |
| DIN | IO8 | SPI data. IO8 is a strapping pin, but the board pulls it up and the LCD only reads it, so boot is unaffected |
| DC | TXD (IO16) | Free, because the knob talks over USB |
| BL | RXD (IO17) | PWM backlight, so it can dim at night |
| CS | GND | The LCD is the only device on its bus, so it stays selected |
| RST | 3V3 | The firmware resets it with a command instead |

There are only 4 free GPIOs, which is why CS and RST are tied off. The LCD uses the ESP32-C6's only general-purpose SPI host, so the firmware reads the MT6701 encoder by bit-banging its SSI interface.

The module ends up mounted turned 45° clockwise relative to the knob's "up". The firmware rotates the image back in software (`LCD_ROTATE_DEG` in `firmware/t-knob/include/board.h`). If you mount it at a different angle, change that value to match.

## Reference dimensions

Measured from LilyGO's STEP model ([`3D_File/H718-20240809.stp`](https://github.com/Xinyuan-LilyGO/T-Knob/tree/master/3D_File)). Import that file into your CAD tool for the real geometry. Coordinates: z is the knob axis, z = 0 is the bottom of the motor mount base, and +y points at the USB-C port.

| Feature | Value |
|---|---|
| Static bottom (PCB, cover, mount base) | Ø32 mm, z −5.5 to about 6 |
| Rotor bell (spins) | Ø32 mm, top face at z 18.75 |
| Rotor top holes | 3× Ø1.6 on r = 10.5 mm (PCD 21) at 0°/120°/240°; 2× Ø2.0 on r = 10.5 mm at 90°/270°. Likely M1.6 threads (M2 doesn't fit; the model derives from SmartKnob, whose 32 mm motor has M1.6 rotor holes with only ~1.5 mm of thread engagement), so use short screws |
| Static hollow shaft | Ø11.5 mm, up to z 23.1 |
| Top breakout PCB | Ø17.4 mm, z 22.55 to 23.55 |
| Top PCB screws | 2×, Ø1.8 holes at (0, ±4), into Ø1.3 pilot holes in the shaft (M1.6) |
| USB-C receptacle | +y side, centred on x = 0, z ≈ 0 |
| Side buttons (BOOT/RST) | On the PCB edge at about 139° and −11°, z ≈ −2 |

Waveshare 1.28inch LCD Module ([drawing](https://www.waveshare.com/1.28inch-lcd-module.htm)):

| Feature | Value |
|---|---|
| PCB outline | Ø37.5 mm circle plus a 14.28 × 3.59 mm flex tab, 40.4 mm overall |
| Visible glass | Ø32.4 mm active area |
| Mounting holes | 4× M2 at about (±13.5, ±9.3) mm from centre (scaled from the drawing, ±0.3 mm) |
| Connector | PH2.0 8-pin on the back (VCC GND DIN CLK CS DC RST BL) |

Not in any drawing; measure on the real parts: the module's thickness, how far the PH2.0 connector sticks out of the back, and where exactly the spinning rotor bell starts above the static base.

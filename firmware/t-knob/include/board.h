// Pin map for the LilyGO T-Knob (github.com/Xinyuan-LilyGO/T-Knob, schematic T-MotorDriver-C6 V1.0).
#pragma once

// MT6701 magnetic angle sensor (SSI, read as SPI mode 1)
#define MAG_CS 7
#define MAG_CLK 14
#define MAG_DO 15

// TMC6300 three-phase driver (6 PWM inputs)
#define DRV_UH 6
#define DRV_UL 5
#define DRV_VH 1
#define DRV_VL 3
#define DRV_WH 0
#define DRV_WL 4
#define DRV_DIAG 2 // high = fault

#define MOTOR_POLE_PAIRS 7
#define SUPPLY_VOLTS 5.0f // USB VBUS through a diode

#define BOOT_BTN 9 // active low; hold at power-up to force motor recalibration
#define LED_COUNT 4
#define LED_PINS {23, 22, 21, 20} // LED1..LED4, wired 3V3 -> LED -> 100R -> GPIO (active low)
// IO18 drives the buzzer footprint (BS1 via an NPN), which isn't populated: unused.

// Static top breakout (T-Motor-TOP V1.0): IO19, IO8, TXD (IO16), RXD (IO17), 3V3, 5VIN, GND.
// IO8 is a strapping pin with a 10K pull-up; IO16/17 are UART0, free because we talk over USB.

// GC9A01 240x240 round LCD wired to the breakout. CS is tied to GND, RST to 3V3.
// The LCD owns SPI2 (the only general-purpose SPI host), so the MT6701 is bit-banged.
#define LCD_SCLK 19
#define LCD_MOSI 8
#define LCD_DC 16 // TXD pad
#define LCD_BL 17 // RXD pad, PWM backlight (active high)
#define LCD_SIZE 240
#define LCD_SPI_HZ 27000000 // conservative for jumper wires; GC9A01 usually manages 40-80 MHz
// The panel is mounted turned 45 deg clockwise; the image is drawn turned back by this much
// (degrees, positive = clockwise).
#define LCD_ROTATE_DEG -45
// Dial angle (clockwise from 12 o'clock) = SIGN * (knob angle from the first session's detent)
// + OFFSET. Flip SIGN if the pointer turns against the wheel.
#define DISPLAY_ANGLE_SIGN 1
#define DISPLAY_ANGLE_OFFSET_DEG 0

// Flip if turning clockwise moves to the previous session.
#define KNOB_REVERSE 0

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

// Flip if turning clockwise moves to the previous session.
#define KNOB_REVERSE 0

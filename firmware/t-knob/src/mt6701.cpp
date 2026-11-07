#include <Arduino.h>
#include <soc/gpio_reg.h>

#include "board.h"
#include "mt6701.h"

// The MT6701's SSI output is a 24-bit frame:
//   [23:10] angle (14 bit)  [9] overspeed  [8] push  [7:6] field  [5:0] CRC6 (x^6 + x + 1)
//
// Bit-banged rather than on SPI2: the ESP32-C6 has a single general-purpose SPI host and
// the round LCD needs it. SPI mode 1 timing: the sensor shifts on CLK rising, we sample
// while CLK is high, just before it falls. Direct register access keeps a frame ~10 us.

static inline void clk(bool high) { REG_WRITE(high ? GPIO_OUT_W1TS_REG : GPIO_OUT_W1TC_REG, BIT(MAG_CLK)); }
static inline void cs(bool high) { REG_WRITE(high ? GPIO_OUT_W1TS_REG : GPIO_OUT_W1TC_REG, BIT(MAG_CS)); }
static inline void settle() { for (int i = 0; i < 6; i++) __asm__ __volatile__("nop"); }

void mt6701_init()
{
    pinMode(MAG_CS, OUTPUT);
    pinMode(MAG_CLK, OUTPUT);
    pinMode(MAG_DO, INPUT);
    cs(true);
    clk(false);
}

static uint8_t crc6(uint32_t data18)
{
    uint8_t crc = 0;
    for (int i = 17; i >= 0; i--) {
        uint8_t fb = ((crc >> 5) & 1) ^ ((data18 >> i) & 1);
        crc = (crc << 1) & 0x3F;
        if (fb) crc ^= 0x03;
    }
    return crc;
}

Mt6701Reading mt6701_read()
{
    Mt6701Reading r = {};
    uint32_t frame = 0;
    cs(false);
    delayMicroseconds(1); // CSN low to first clock edge
    for (int i = 0; i < 24; i++) {
        clk(true);
        settle();
        frame = (frame << 1) | ((REG_READ(GPIO_IN_REG) >> MAG_DO) & 1);
        clk(false);
        settle();
    }
    cs(true);

    r.ok = crc6(frame >> 6) == (frame & 0x3F);
    r.angle = (float)(frame >> 10) * (2.0f * PI / 16384.0f);
    r.overspeed = (frame >> 9) & 1;
    r.pressed = (frame >> 8) & 1;
    r.field = (frame >> 6) & 3;
    return r;
}

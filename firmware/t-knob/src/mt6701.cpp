#include <Arduino.h>
#include <driver/spi_master.h>

#include "board.h"
#include "mt6701.h"

// The MT6701's SSI output is a 24-bit frame:
//   [23:10] angle (14 bit)  [9] overspeed  [8] push  [7:6] field  [5:0] CRC6 (x^6 + x + 1)
static spi_device_handle_t dev;

void mt6701_init()
{
    spi_bus_config_t bus = {};
    bus.mosi_io_num = -1;
    bus.miso_io_num = MAG_DO;
    bus.sclk_io_num = MAG_CLK;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = 4;
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_DISABLED));

    spi_device_interface_config_t cfg = {};
    cfg.mode = 1;
    cfg.clock_speed_hz = 4000000;
    cfg.spics_io_num = MAG_CS;
    cfg.cs_ena_pretrans = 4;
    cfg.queue_size = 1;
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &cfg, &dev));
    // Keep the bus to ourselves so each read skips arbitration.
    ESP_ERROR_CHECK(spi_device_acquire_bus(dev, portMAX_DELAY));
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
    spi_transaction_t t = {};
    t.flags = SPI_TRANS_USE_RXDATA;
    t.length = 24;
    t.rxlength = 24;
    Mt6701Reading r = {};
    if (spi_device_polling_transmit(dev, &t) != ESP_OK) return r;

    uint32_t frame = ((uint32_t)t.rx_data[0] << 16) | ((uint32_t)t.rx_data[1] << 8) | t.rx_data[2];
    r.ok = crc6(frame >> 6) == (frame & 0x3F);
    r.angle = (float)(frame >> 10) * (2.0f * PI / 16384.0f);
    r.overspeed = (frame >> 9) & 1;
    r.pressed = (frame >> 8) & 1;
    r.field = (frame >> 6) & 3;
    return r;
}

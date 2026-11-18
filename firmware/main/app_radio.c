#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"            // esp_rom_delay_us: short busy-wait delays.
#include "freertos/FreeRTOS.h"      // Required by task.h and semphr.h.
#include "freertos/task.h"          // vTaskDelay.
#include "freertos/semphr.h"        // Mutex that guards the SPI buffers.
#include "esp_attr.h"               // DMA_ATTR, WORD_ALIGNED_ATTR.
#include <string.h> 
#include "board.h"                 
#include "app_radio.h" 
#include <stdio.h>

static spi_device_handle_t s_spi;
esp_err_t err;

static esp_err_t wait_busy(void){

    for (int i = 0; i < 100000; i++){
        if (gpio_get_level(PIN_LORA_BUSY) == 0){
            return ESP_OK;
        }
        esp_rom_delay_us(10);
    }

    printf("Radio is stuck busy\n");
    return ESP_ERR_TIMEOUT;

}

// The ESP-IDF 6.x SPI driver needs DMA buffers in DMA-capable RAM at a 4-byte-aligned address. Otherwise it
// allocates a temporary copy for every transfer and crashes when RAM is low and that allocation fails.
// Every transfer therefore goes through these two fixed buffers. The lock keeps two tasks from using them at once.
#define SPI_BUF_LEN 264                                    // At least max_transfer_sz (260), and a multiple of 4.
static DMA_ATTR WORD_ALIGNED_ATTR uint8_t s_dma_tx[SPI_BUF_LEN];
static DMA_ATTR WORD_ALIGNED_ATTR uint8_t s_dma_rx[SPI_BUF_LEN];
static StaticSemaphore_t s_spi_lock_mem;
static SemaphoreHandle_t s_spi_lock;

static esp_err_t radio_transfer(const uint8_t *tx, uint8_t *rx, int len){

    if (len <= 0 || len > SPI_BUF_LEN) return ESP_ERR_INVALID_SIZE;
    // The lock is created on the first call, which radio_init makes before any task uses the radio.
    if (!s_spi_lock) s_spi_lock = xSemaphoreCreateMutexStatic(&s_spi_lock_mem);
    xSemaphoreTake(s_spi_lock, portMAX_DELAY);

    esp_err_t r = wait_busy();
    if (r == ESP_OK) {
        memcpy(s_dma_tx, tx, len);
        spi_transaction_t t;
        memset(&t, 0, sizeof(t));
        t.length = len * 8;
        t.tx_buffer = s_dma_tx;
        t.rx_buffer = s_dma_rx;
        r = spi_device_polling_transmit(s_spi, &t);
        if (r == ESP_OK && rx) memcpy(rx, s_dma_rx, len);
    }

    xSemaphoreGive(s_spi_lock);
    return r;
}

static esp_err_t radio_command(const uint8_t *tx, int len){

    uint8_t rx[LORA_MAX_PAYLOAD + 3] = {0};

    if (len> (int)sizeof(rx)){
        return ESP_ERR_INVALID_SIZE;
    }

    return radio_transfer(tx, rx, len);

}

static uint16_t s_preamble = 12;        // Preamble symbols: 12 for FreeMesh. A foreign SOS packet uses its own value.

esp_err_t radio_apply(const settings_t *s){
    // Standby: the radio accepts new settings only in standby.
    const uint8_t standby[] = { 0x80, 0x00 };
    err = radio_command(standby, sizeof(standby));
    if (err != ESP_OK) return err;

    // Set the RF frequency. The register value is freq_hz * 2^25 / 32 MHz, the crystal frequency (datasheet section 13.4.1).
    uint32_t val = (uint32_t)(((uint64_t) s->freq_hz << 25) / 32000000ULL);
    uint8_t freq[] = {0x86, (uint8_t)(val >> 24), (uint8_t)(val >> 16), (uint8_t)(val >> 8), (uint8_t)val};
    err = radio_command(freq, sizeof(freq));
    if (err != ESP_OK) return err;

    // Set the TX power in dBm. 0x04 selects a 200 us ramp time (datasheet Table 13-41).
    uint8_t txp[] = {0x8E, (uint8_t)s->tx_dbm, 0x04};
    err = radio_command(txp, sizeof(txp));
    if (err != ESP_OK) return err; 

    uint8_t bw_code;

    switch (s->bw_khz){
        case 125:
            bw_code = 0x04;
            break;
        case 250:
            bw_code = 0x05;
            break;
        case 500:
            bw_code = 0x06;
            break;
        default:
            bw_code = 0x04;
            break;
    }

    // Low data rate optimization (LDRO) is on when one symbol lasts 16.38 ms or longer.
    uint32_t sym_us = ((uint32_t)1 << s->sf) * 1000 / s->bw_khz; // Symbol time in microseconds: 2^SF / BW.
    uint8_t ldro = (sym_us >= 16380) ? 0x01 : 0x00; // Example: SF11 at 125 kHz gives 16384 us, so LDRO is on.

    // Set the modulation parameters (SX1262 datasheet (Rev 2.2), p.90-93).
    uint8_t mod[] = {0x8B,
                    s->sf, // Spreading factor.
                    bw_code, // Bandwidth code.
                    0x01, // Coding rate 4/5: 1 error-correcting bit per 4 data bits (Table 13-49).
                    ldro // LDRO flag from above.
                    };
    err = radio_command(mod, sizeof(mod));
    if (err != ESP_OK) return err;

    // FreeMesh uses the chip's default sync word 0x12 ("private"), stored in registers 0x0740-0x0741 as 0x1424
    // (datasheet register table, p.70).
    // Write it again because a foreign SOS packet may have changed it.
    const uint8_t sync[] = {0x0D, 0x07, 0x40, 0x14, 0x24};
    err = radio_command(sync, sizeof(sync));
    s_preamble = 12;
    return err;
}

// Send one packet with another network's radio settings (the SOS to Meshtastic and MeshCore boxes).
// Afterwards, call radio_apply(&device_settings) and radio_start_rx() to return to the FreeMesh settings.
esp_err_t radio_send_foreign(const radio_foreign_t *f, const uint8_t *data, uint8_t len){
    const uint8_t standby[] = { 0x80, 0x00 };
    err = radio_command(standby, sizeof(standby));
    if (err != ESP_OK) return err;
    uint32_t val = (uint32_t)(((uint64_t) f->freq_hz << 25) / 32000000ULL);
    uint8_t freq[] = {0x86, (uint8_t)(val >> 24), (uint8_t)(val >> 16), (uint8_t)(val >> 8), (uint8_t)val};
    if ((err = radio_command(freq, sizeof(freq))) != ESP_OK) return err;
    uint8_t mod[] = {0x8B, f->sf, f->bw_code, f->cr, f->ldro};
    if ((err = radio_command(mod, sizeof(mod))) != ESP_OK) return err;
    uint8_t sync[] = {0x0D, 0x07, 0x40, (uint8_t)(f->sync_word >> 8), (uint8_t)f->sync_word};
    if ((err = radio_command(sync, sizeof(sync))) != ESP_OK) return err;
    s_preamble = f->preamble;
    return radio_send(data, len);
}



esp_err_t radio_init(void){
    gpio_reset_pin(PIN_LORA_RST);
    gpio_reset_pin(PIN_LORA_RXEN);
    gpio_reset_pin(PIN_LORA_BUSY);
    gpio_reset_pin(PIN_LORA_DIO1);

    gpio_set_direction(PIN_LORA_RST, GPIO_MODE_OUTPUT);
    gpio_set_direction(PIN_LORA_RXEN, GPIO_MODE_OUTPUT);
    gpio_set_direction(PIN_LORA_BUSY, GPIO_MODE_INPUT);
    gpio_set_direction(PIN_LORA_DIO1, GPIO_MODE_INPUT);

    gpio_set_level(PIN_LORA_RXEN, 0);

    // Reset: hold the reset pin low for 200 us, then wait for BUSY to go low (datasheet p.53 and p.55).
    gpio_set_level(PIN_LORA_RST, 0);
    esp_rom_delay_us(200);
    gpio_set_level(PIN_LORA_RST, 1);
    esp_rom_delay_us(10);
    if (wait_busy() != ESP_OK) {           // BUSY low means the radio is ready.
        return ESP_ERR_TIMEOUT;            // The radio never left reset.
    }

    spi_bus_config_t buscfg = {
        .mosi_io_num = PIN_LORA_MOSI,
        .miso_io_num = PIN_LORA_MISO,
        .sclk_io_num = PIN_LORA_SCK,
        .max_transfer_sz = 260,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1
    };

    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t intrfcecfg = {
        .clock_speed_hz = 8000000,
        .mode = 0,
        .spics_io_num = PIN_LORA_NSS,
        .queue_size = 1,
    };

    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &intrfcecfg, &s_spi));

    // Go to standby on the RC oscillator (datasheet section 13.1.2).
    const uint8_t standby[] = { 0x80, 0x00 };
    err = radio_command(standby, sizeof(standby));
    if (err != ESP_OK) return err;

    // DIO3 powers the TCXO: 1.8 V (datasheet Table 13-35), with a 5 ms startup time
    // (0x000140 steps of 15.625 us, datasheet section 13.3.6).
    const uint8_t tcxo[] = { 0x97, 0x02, 0x00, 0x01, 0x40 };
    err = radio_command(tcxo, sizeof(tcxo));
    if (err != ESP_OK) return err;

    // Clear the device error flags.
    const uint8_t clear[] = { 0x07, 0x00, 0x00 };
    err = radio_command(clear, sizeof(clear));
    if (err != ESP_OK) return err;

    // Calibrate all blocks.
    const uint8_t calibrate[] = { 0x89, 0x7F };
    err = radio_command(calibrate, sizeof(calibrate));
    if (err != ESP_OK) return err;

    // DIO2 controls the antenna switch automatically (datasheet p.86).
    const uint8_t antenna[] = { 0x9D, 0x01 };
    err = radio_command(antenna, sizeof(antenna));
    if (err != ESP_OK) return err;

    // Packet type: LoRa.
    const uint8_t lora[] = { 0x8A, 0x01 };
    err = radio_command(lora, sizeof(lora));
    if (err != ESP_OK) return err;

    // Image calibration for the 902-928 MHz band (datasheet Table 9-2).
    uint8_t img[] = {0x98, 0xE1, 0xE9};
    err = radio_command(img, sizeof(img));
    if (err != ESP_OK) return err; 

    // PA configuration: the SX1262 +22 dBm row of datasheet Table 13-21. radio_apply sets the actual TX power.
    uint8_t pa[] = {0x95, 0x04, 0x07, 0x00, 0x01};
    err = radio_command(pa, sizeof(pa));
    if (err != ESP_OK) return err;

    // TX and RX data both start at offset 0 of the radio's buffer (datasheet p.51-52).
    uint8_t bba[] = {0x8F, 0x00, 0x00};
    err = radio_command(bba, sizeof(bba));
    if (err != ESP_OK) return err;

    radio_apply(&device_settings); // Frequency, TX power, modulation and sync word.

    // Enable the interrupts and route them to DIO1 (datasheet Table 13-29).
    uint8_t irq[] = {0x08,
                    0x02,
                    0x63, // IRQ mask 0x0263: bit 0 TxDone, 1 RxDone, 5 HeaderErr, 6 CrcErr, 9 Timeout.
                    0x02,
                    0x63, // DIO1 mask: the same bits.
                    0x00, 0x00, 0x00, 0x00 // DIO2 and DIO3 masks: unused.
                    };
    err = radio_command(irq, sizeof(irq));
    if (err != ESP_OK) return err;

    return ESP_OK;

}

void radio_self_test(void){
    const uint8_t tx[] = {0x1D, 0x07, 0x40, 0x00, 0x00, 0x00};
    uint8_t rx[6] = {0};

    err =  radio_transfer(tx, rx, sizeof(tx));

    if (err == ESP_OK){
        printf("received 0x%02X, 0x%02X\n", rx[4], rx[5]);
    } else {
        printf("Radio self test failed! Error code: 0x%X\n", err);
    }
}

void radio_health_printer(void){
    const uint8_t tx[] = {0xC0, 0x00};
    uint8_t rx[sizeof(tx)] = {0};

    err = radio_transfer(tx, rx, sizeof(tx));

    if (err == ESP_OK){
        uint8_t status = rx[1];
        uint8_t mode = (status >> 4) & 0x7;
        uint8_t cmd = (status >> 1) & 0x7;
        printf("Radio chip mode = %d, Radio command status =  %d\n", mode, cmd);
    } else {
        printf("Radio health check failed! Error code: 0x%X\n", err);
    }
    const uint8_t tx2[] = {0x17, 0x00, 0x00, 0x00};
    uint8_t rx2[sizeof(tx2)] = {0};

    err = radio_transfer(tx2, rx2, sizeof(tx2));

    if (err == ESP_OK){
        uint16_t errors = (rx2[2] << 8) | rx2[3];
        printf("Errors=0x%04X\n", errors);
    } else {
        printf("Radio health check failed! Error code: 0x%X\n", err);
    }

}

static esp_err_t set_packet_params(uint8_t len){
    uint8_t pktparam[]= {0x8C,
                        (uint8_t)(s_preamble >> 8),
                        (uint8_t)s_preamble, // Preamble symbols: a longer preamble helps weak signals but costs airtime.
                        0x00, // Variable-length packet.
                        len,
                        0x01, // CRC on.
                        0x00 // Standard IQ. All fields: datasheet Tables 13-66 to 13-70.
                        };
    return radio_command(pktparam, sizeof(pktparam));
}

static uint16_t get_interrupt(void){
    const uint8_t getirq[] = {0x12, 0x00, 0x00, 0x00};
    uint8_t irq_rx[sizeof(getirq)] = {0};
    err = radio_transfer(getirq, irq_rx, sizeof(getirq));
    if (err != ESP_OK){
        printf("Getting interrupts failed: %s\n", esp_err_to_name(err));
        return 0;
    }
    return (uint16_t)((irq_rx[2] << 8) | irq_rx[3]);
}

static esp_err_t clear_interrupt(void){
    uint8_t const clearirq[] = {0x02, 0xFF, 0xFF};
    return radio_command(clearirq, sizeof(clearirq));
}

esp_err_t radio_send(const uint8_t *data, uint8_t len){
    if (len > LORA_MAX_PAYLOAD){
        len = LORA_MAX_PAYLOAD;
    }
    gpio_set_level(PIN_LORA_RXEN, 0);
    err = set_packet_params(len);
    if (err != ESP_OK){
        printf("Setting packet params failed during sending %s\n", esp_err_to_name(err));
        return err;
    }

    uint8_t buf[LORA_MAX_PAYLOAD + 3];                 // Opcode, offset, then the data.
    buf[0] = 0x0E;                   // WriteBuffer opcode.
    buf[1] = 0x00;                   // Offset 0 in the radio's buffer.
    memcpy(&buf[2], data, len);      // The payload follows the 2 header bytes.
    err = radio_command(buf, len + 2);
    if (err != ESP_OK){
        printf("Sending over radio failed%s\n", esp_err_to_name(err));
        return err;
    }
    err = clear_interrupt();
    if (err != ESP_OK){
        printf("Clearing interrput failed %s\n", esp_err_to_name(err));
        return err;
    }

    // Start transmitting with the radio's own timeout off (timeout 0, datasheet Table 13-7). The loop below limits the wait.
    uint8_t txcommand[] = {0x83, 0x00, 0x00, 0x00};
    err = radio_command(txcommand, sizeof(txcommand));
    if (err != ESP_OK) return err;

    for (int i = 0; i < 400; i++) {                  // At most 400 x 10 ms = 4 s. A slow Meshtastic SOS takes about 1 s.
        if (gpio_get_level(PIN_LORA_DIO1) == 1) break;   // DIO1 high: the radio raised an interrupt.
        vTaskDelay(pdMS_TO_TICKS(10));              // Lets other tasks run while waiting.
    }

    uint16_t result = get_interrupt();

    err = clear_interrupt();
    if (err != ESP_OK){
        printf("Clearing interrput failed %s\n", esp_err_to_name(err));
        return err;
    }

    if (result & 0x0001) {
        printf("TX Done\n");
        return ESP_OK;
    } else {
        printf("TX failed irq=0x%04X\n", result); 
        return ESP_FAIL;
    }

}

esp_err_t radio_start_rx(void){
    gpio_set_level(PIN_LORA_RXEN, 1);
    err = set_packet_params(255); // Accept packets up to the maximum length.
    if (err != ESP_OK){
        printf("Setting packet params failed during receiving %s\n", esp_err_to_name(err));
        return err;
    }
    err = clear_interrupt();
    if (err != ESP_OK){
        printf("Clearing interrput failed %s\n", esp_err_to_name(err));
        return err;
    }
    
    const uint8_t rxcmd[] = {0x82, 0xFF, 0xFF, 0xFF};   // Continuous receive: timeout 0xFFFFFF (datasheet p.75).
    err = radio_command(rxcmd, sizeof(rxcmd));
    if (err != ESP_OK) return err;
    return ESP_OK;
}

// Check for a received packet. Returns its length, or -1 when there is none or it is corrupted.
int radio_check_rx(uint8_t *buf, int max_len, int *rssi_dbm, float *snr_db)
{
    
    if (gpio_get_level(PIN_LORA_DIO1) == 0) return -1;
    uint16_t irq = get_interrupt();
    clear_interrupt();

    // Ignore every interrupt except RxDone. Drop the packet on HeaderErr (0x20) or CrcErr (0x40).
    if (!(irq & 0x0002)) return -1;
    if (irq & 0x0060) {
        printf("Bad packet, irq=0x%04X\n", irq);
        return -1;
    }

    // GetRxBufferStatus: the packet length and where it starts in the buffer (datasheet p.101, Table 13-78).
    const uint8_t st_tx[] = {0x13, 0x00, 0x00, 0x00};
    uint8_t st_rx[sizeof(st_tx)] = {0};
    if (radio_transfer(st_tx, st_rx, sizeof(st_tx)) != ESP_OK) return -1;
    int len = st_rx[2];                 // PayloadLengthRx.
    uint8_t start = st_rx[3];           // RxStartBufferPointer.
    if (len > max_len) len = max_len;   // Never overflow the caller's buffer.
    if (len > LORA_MAX_PAYLOAD ) len = LORA_MAX_PAYLOAD ;             // Must fit the read buffers below.

    // ReadBuffer: opcode 0x1E, offset, one NOP, then one NOP per data byte (datasheet p.84, Table 13-27).
    uint8_t rb_tx[LORA_MAX_PAYLOAD + 3] = {0};            // Zeros are NOPs.
    uint8_t rb_rx[LORA_MAX_PAYLOAD + 3] = {0};
    rb_tx[0] = 0x1E;                    // ReadBuffer opcode.
    rb_tx[1] = start;                   // Where the packet begins.
    if (radio_transfer(rb_tx, rb_rx, len + 3) != ESP_OK) return -1;
    memcpy(buf, &rb_rx[3], len);        // The data starts after the 3 header bytes.

    // GetPacketStatus: signal strength and quality (datasheet p.101, Tables 13-79 and 13-80).
    const uint8_t ps_tx[] = {0x14, 0x00, 0x00, 0x00, 0x00};
    uint8_t ps_rx[sizeof(ps_tx)] = {0};
    if (radio_transfer(ps_tx, ps_rx, sizeof(ps_tx)) == ESP_OK) {
        *rssi_dbm = -ps_rx[2] / 2;              // RSSI in dBm.
        *snr_db   = (int8_t)ps_rx[3] / 4.0f;    // SNR in dB, can be negative.
    }

    return len;
}
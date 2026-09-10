/*
 * lora.h - SX1278 LoRa radio driver + GPIO helpers (BeagleBone Black)
 *
 * Shared declarations for lora.c and main.c.
 */

#ifndef LORA_H
#define LORA_H

#include <stdint.h>
#include <pthread.h>
#include <gpiod.h>

/* ---------- Config ---------- */
#define SPI_DEVICE      "/dev/spidev0.0"

#define RESET_CHIP      "gpiochip0"
#define RESET_LINE      17   /* P9_23 */

#define SWITCH_CHIP     "gpiochip0"
#define SWITCH_LINE     28   /* P9_12 */

#define FREQUENCY_HZ    433000000UL
#define SPI_SPEED_HZ    5000000
#define MESSAGE         "Hello"
#define DEBOUNCE_MS     200
#define MAX_PACKET_LEN  255

/* ---------- SX1278 Registers ---------- */
#define REG_FIFO                 0x00
#define REG_OP_MODE               0x01
#define REG_FRF_MSB               0x06
#define REG_FRF_MID               0x07
#define REG_FRF_LSB               0x08
#define REG_PA_CONFIG              0x09
#define REG_OCP                  0x0B
#define REG_FIFO_ADDR_PTR          0x0D
#define REG_FIFO_TX_BASE_ADDR      0x0E
#define REG_FIFO_RX_BASE_ADDR      0x0F
#define REG_FIFO_RX_CURRENT_ADDR   0x10
#define REG_IRQ_FLAGS              0x12
#define REG_RX_NB_BYTES            0x13
#define REG_PKT_RSSI_VALUE         0x1A
#define REG_MODEM_CONFIG_1         0x1D
#define REG_MODEM_CONFIG_2         0x1E
#define REG_PREAMBLE_MSB           0x20
#define REG_PREAMBLE_LSB           0x21
#define REG_PAYLOAD_LENGTH         0x22
#define REG_SYNC_WORD              0x39
#define REG_DIO_MAPPING_1          0x40
#define REG_VERSION                0x42
#define REG_PA_DAC                 0x4D

#define MODE_LONG_RANGE_MODE   0x80
#define MODE_SLEEP             0x00
#define MODE_STDBY             0x01
#define MODE_TX                0x03
#define MODE_RX_CONTINUOUS     0x05

#define IRQ_RX_DONE_MASK        0x40
#define IRQ_TX_DONE_MASK        0x08
#define IRQ_PAYLOAD_CRC_ERROR   0x20

#define FXOSC   32000000.0
#define FSTEP   (FXOSC / 524288.0)

/* ---------- Shared state ---------- */
extern int spi_fd;
extern struct gpiod_line *reset_line;
extern struct gpiod_line *switch_line;
extern pthread_mutex_t radio_mutex;

/* ---------- GPIO setup/teardown ---------- */
void gpio_setup(void);
void gpio_cleanup(void);

/* ---------- SPI setup ---------- */
int spi_setup(void);

/* ---------- Low-level register access (caller must hold radio_mutex
   for anything beyond a single isolated call) ---------- */
void write_reg(uint8_t addr, uint8_t value);
uint8_t read_reg(uint8_t addr);

/* ---------- Radio control ---------- */
void set_mode(uint8_t mode);
void set_frequency(unsigned long freq_hz);
void radio_reset(void);
void init_lora(void);

/* ---------- Thread entry points ---------- */
void *listen_thread_func(void *arg);
void *switch_thread_func(void *arg);

#endif /* LORA_H */

/*
 * lora.c - SX1278 LoRa radio driver + GPIO helpers + worker threads
 *
 * Two pthreads (defined here, started from main.c) run concurrently:
 *   1. listen_thread_func  - constantly checks for incoming packets, prints them.
 *   2. switch_thread_func  - watches the switch pin; on a press, sends "Hello".
 *
 * Both threads touch the same physical radio, so every register access
 * is wrapped in radio_mutex to make sure they never talk to the SPI bus
 * at the same time.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <stdint.h>
#include <math.h>
#include <pthread.h>
#include <gpiod.h>
#include <linux/spi/spidev.h>
#include <sys/ioctl.h>

#include "lora.h"

/* ---------- Shared state (definitions) ---------- */
int spi_fd = -1;
static struct gpiod_chip *reset_chip = NULL;
struct gpiod_line *reset_line = NULL;
static struct gpiod_chip *switch_chip = NULL;
struct gpiod_line *switch_line = NULL;

/* The lock every thread must hold before touching the radio over SPI */
pthread_mutex_t radio_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ---------- SPI byte-level register access (NOT thread-safe on its own -
   callers must hold radio_mutex before calling these) ---------- */

static uint8_t spi_transfer_byte(uint8_t addr_byte, uint8_t data_byte) {
    uint8_t tx[2] = { addr_byte, data_byte };
    uint8_t rx[2] = { 0, 0 };

    struct spi_ioc_transfer tr;
    memset(&tr, 0, sizeof(tr));
    tr.tx_buf = (unsigned long)tx;
    tr.rx_buf = (unsigned long)rx;
    tr.len = 2;
    tr.speed_hz = SPI_SPEED_HZ;
    tr.bits_per_word = 8;

    if (ioctl(spi_fd, SPI_IOC_MESSAGE(1), &tr) < 1) {
        perror("SPI transfer failed");
        exit(1);
    }
    return rx[1];
}

void write_reg(uint8_t addr, uint8_t value) {
    spi_transfer_byte(addr | 0x80, value);
}

uint8_t read_reg(uint8_t addr) {
    return spi_transfer_byte(addr & 0x7F, 0x00);
}

/* ---------- SX1278 control ---------- */

void set_mode(uint8_t mode) {
    write_reg(REG_OP_MODE, MODE_LONG_RANGE_MODE | mode);
    usleep(10000);
}

void set_frequency(unsigned long freq_hz) {
    uint32_t frf = (uint32_t)((double)freq_hz / FSTEP);
    write_reg(REG_FRF_MSB, (frf >> 16) & 0xFF);
    write_reg(REG_FRF_MID, (frf >> 8) & 0xFF);
    write_reg(REG_FRF_LSB, frf & 0xFF);
}

void radio_reset(void) {
    gpiod_line_set_value(reset_line, 0);
    usleep(10000);
    gpiod_line_set_value(reset_line, 1);
    usleep(10000);
}

void init_lora(void) {
    set_mode(MODE_SLEEP);
    set_frequency(FREQUENCY_HZ);

    write_reg(REG_FIFO_TX_BASE_ADDR, 0x00);
    write_reg(REG_FIFO_RX_BASE_ADDR, 0x00);

    write_reg(REG_MODEM_CONFIG_1, 0x72);
    write_reg(REG_MODEM_CONFIG_2, 0x74);

    write_reg(REG_PREAMBLE_MSB, 0x00);
    write_reg(REG_PREAMBLE_LSB, 0x08);

    write_reg(REG_SYNC_WORD, 0x12);

    write_reg(REG_PA_CONFIG, 0x8F);
    write_reg(REG_PA_DAC, 0x87);
    write_reg(REG_OCP, 0x3B);

    set_mode(MODE_STDBY);
}

/* Send data, wait for TxDone, then return the radio to RX_CONTINUOUS.
   Caller must already hold radio_mutex. */
static int radio_send_locked(const uint8_t *data, uint8_t len, double timeout_sec) {
    set_mode(MODE_STDBY);

    write_reg(REG_FIFO_ADDR_PTR, 0x00);
    for (int i = 0; i < len; i++) {
        write_reg(REG_FIFO, data[i]);
    }
    write_reg(REG_PAYLOAD_LENGTH, len);
    write_reg(REG_DIO_MAPPING_1, 0x40);

    set_mode(MODE_TX);

    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);

    uint8_t flag_value = 0x00;
    int tx_done = 0;

    while (1) {
        flag_value = read_reg(REG_IRQ_FLAGS);
        if (flag_value & IRQ_TX_DONE_MASK) {
            tx_done = 1;
            write_reg(REG_IRQ_FLAGS, IRQ_TX_DONE_MASK);
            break;
        }
        clock_gettime(CLOCK_MONOTONIC, &now);
        double elapsed = (now.tv_sec - start.tv_sec) + (now.tv_nsec - start.tv_nsec) / 1e9;
        if (elapsed > timeout_sec) break;
        usleep(5000);
    }

    printf("  [TX] IRQ_FLAGS=0x%02X -> %s\n", flag_value,
           tx_done ? "TxDone (sent OK)" : "timed out / failed");

    /* Back to listening */
    write_reg(REG_FIFO_ADDR_PTR, 0x00);
    write_reg(REG_DIO_MAPPING_1, 0x00);
    set_mode(MODE_RX_CONTINUOUS);

    return tx_done;
}

/* Non-blocking receive check. Caller must already hold radio_mutex. */
static void radio_check_receive_locked(void) {
    uint8_t irq = read_reg(REG_IRQ_FLAGS);

    if (!(irq & IRQ_RX_DONE_MASK)) {
        return;
    }

    write_reg(REG_IRQ_FLAGS, irq);

    if (irq & IRQ_PAYLOAD_CRC_ERROR) {
        printf("  [RX] Packet received but CRC error -> discarded\n");
        return;
    }

    uint8_t current_addr = read_reg(REG_FIFO_RX_CURRENT_ADDR);
    uint8_t nb_bytes = read_reg(REG_RX_NB_BYTES);

    if (nb_bytes == 0 || nb_bytes > MAX_PACKET_LEN) {
        return;
    }

    write_reg(REG_FIFO_ADDR_PTR, current_addr);

    uint8_t buf[MAX_PACKET_LEN + 1];
    for (int i = 0; i < nb_bytes; i++) {
        buf[i] = read_reg(REG_FIFO);
    }
    buf[nb_bytes] = '\0';

    int rssi_raw = read_reg(REG_PKT_RSSI_VALUE);
    int rssi_dbm = rssi_raw - 164; /* LF port (433MHz) offset */

    printf("  [RX] Received: \"%s\"  RSSI: %d dBm\n", buf, rssi_dbm);
}

/* ---------- Thread 1: listener ---------- */

void *listen_thread_func(void *arg) {
    (void)arg;
    while (1) {
        pthread_mutex_lock(&radio_mutex);
        radio_check_receive_locked();
        pthread_mutex_unlock(&radio_mutex);

        usleep(10000); /* poll every 10ms when not holding the lock */
    }
    return NULL;
}

/* ---------- Thread 2: switch watcher / sender ---------- */

void *switch_thread_func(void *arg) {
    (void)arg;
    int last_state = gpiod_line_get_value(switch_line); /* expect 1 (HIGH) idle */

    while (1) {
        int current_state = gpiod_line_get_value(switch_line);

        if (last_state == 1 && current_state == 0) {
            printf("Switch pressed -> sending \"%s\"\n", MESSAGE);

            pthread_mutex_lock(&radio_mutex);
            radio_send_locked((const uint8_t *)MESSAGE, (uint8_t)strlen(MESSAGE), 2.0);
            pthread_mutex_unlock(&radio_mutex);

            usleep(DEBOUNCE_MS * 1000);
            current_state = gpiod_line_get_value(switch_line);
        }

        last_state = current_state;
        usleep(10000);
    }
    return NULL;
}

/* ---------- libgpiod setup ---------- */

void gpio_setup(void) {
    reset_chip = gpiod_chip_open_by_name(RESET_CHIP);
    if (!reset_chip) { perror("open reset chip"); exit(1); }
    reset_line = gpiod_chip_get_line(reset_chip, RESET_LINE);
    if (!reset_line) { perror("get reset line"); exit(1); }
    if (gpiod_line_request_output(reset_line, "sx1278-reset", 1) < 0) {
        perror("request reset line as output");
        exit(1);
    }

    switch_chip = gpiod_chip_open_by_name(SWITCH_CHIP);
    if (!switch_chip) { perror("open switch chip"); exit(1); }
    switch_line = gpiod_chip_get_line(switch_chip, SWITCH_LINE);
    if (!switch_line) { perror("get switch line"); exit(1); }

    if (gpiod_line_request_input_flags(switch_line, "sx1278-switch",
                                        GPIOD_LINE_REQUEST_FLAG_BIAS_PULL_UP) < 0) {
        perror("request switch line with pull-up (needs libgpiod 1.5+/kernel 5.5+)");
        exit(1);
    }
}

void gpio_cleanup(void) {
    if (reset_line) gpiod_line_release(reset_line);
    if (switch_line) gpiod_line_release(switch_line);
    if (reset_chip) gpiod_chip_close(reset_chip);
    if (switch_chip) gpiod_chip_close(switch_chip);
}

/* ---------- SPI setup ---------- */

int spi_setup(void) {
    spi_fd = open(SPI_DEVICE, O_RDWR);
    if (spi_fd < 0) {
        perror("Failed to open SPI device");
        return -1;
    }
    uint8_t mode = SPI_MODE_0;
    uint8_t bits = 8;
    uint32_t speed = SPI_SPEED_HZ;
    ioctl(spi_fd, SPI_IOC_WR_MODE, &mode);
    ioctl(spi_fd, SPI_IOC_WR_BITS_PER_WORD, &bits);
    ioctl(spi_fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed);
    return 0;
}

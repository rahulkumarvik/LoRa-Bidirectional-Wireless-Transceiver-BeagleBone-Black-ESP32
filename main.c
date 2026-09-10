/*
 * main.c - Two-way LoRa link on a BeagleBone Black + SX1278 - THREADED VERSION.
 *
 * Wiring:
 *   SX1278 VCC   -> BBB 3.3V (P9_3 / P9_4)
 *   SX1278 GND   -> BBB GND  (P9_1)
 *   SX1278 SCK   -> P9_22 (SPI0_SCLK)
 *   SX1278 MISO  -> P9_21 (SPI0_D0)
 *   SX1278 MOSI  -> P9_18 (SPI0_D1)
 *   SX1278 NSS   -> P9_17 (SPI0_CS0, handled automatically by the SPI overlay)
 *   SX1278 RESET -> P9_23 (gpiochip0, line 17)
 *
 *   Switch leg 1 -> P9_12 (gpiochip0, line 28, requested with internal pull-up)
 *   Switch leg 2 -> GND
 *
 * Install:
 *   sudo apt install libgpiod-dev
 * Compile:
 *   make
 * Run:
 *   sudo ./sx1278_threaded
 */

#include <stdio.h>
#include <stdint.h>
#include <pthread.h>
#include <unistd.h>

#include "lora.h"

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0); /* prints show up immediately, even over ssh */

    gpio_setup();

    if (spi_setup() < 0) {
        gpio_cleanup();
        return 1;
    }

    radio_reset();
    uint8_t version = read_reg(REG_VERSION);
    printf("SX1278 version register: 0x%02X\n", version);
    if (version != 0x12) {
        printf("WARNING: unexpected version - check wiring/power\n");
    }
    init_lora();

    write_reg(REG_DIO_MAPPING_1, 0x00);
    set_mode(MODE_RX_CONTINUOUS);

    printf("Threaded transceiver ready.\n");
    printf("  - listen_thread  : always listening for incoming packets\n");
    printf("  - switch_thread  : press the switch on P9_12 to send \"%s\"\n", MESSAGE);
    printf("Press Ctrl+C to quit.\n\n");

    pthread_t listen_thread, switch_thread;
    pthread_create(&listen_thread, NULL, listen_thread_func, NULL);
    pthread_create(&switch_thread, NULL, switch_thread_func, NULL);

    /* Main thread just waits for the two worker threads (they never exit) */
    pthread_join(listen_thread, NULL);
    pthread_join(switch_thread, NULL);

    close(spi_fd);
    gpio_cleanup();
    return 0;
}

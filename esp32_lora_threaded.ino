/*
 * Two-way LoRa link on an ESP32 + SX1278 - THREADED VERSION.
 *
 * Two FreeRTOS tasks run concurrently (the ESP32's Arduino core is built
 * on FreeRTOS, so a "task" here is the same idea as a pthread on Linux):
 *   1. listenTask  - constantly checks for incoming packets, prints them.
 *   2. switchTask  - watches a physical switch pin; on a press, sends a message.
 *
 * Both tasks touch the same physical radio, so every access is wrapped
 * in xSemaphoreTake/xSemaphoreGive (a mutex) so they never talk to the
 * SPI bus at the same time.
 *
 * Wiring (matches the ESP32-WROOM-32 DevKit board):
 *   SX1278 VCC   -> 3V3
 *   SX1278 GND   -> GND
 *   SX1278 SCK   -> G18
 *   SX1278 MISO  -> G19
 *   SX1278 MOSI  -> G23
 *   SX1278 NSS   -> G5
 *   SX1278 RESET -> G14
 *   SX1278 DIO0  -> G26
 *
 *   Switch leg 1 -> G4 (uses internal pull-up, no external resistor needed)
 *   Switch leg 2 -> GND
 */

#include <SPI.h>
#include <LoRa.h>

#define LORA_SS     5
#define LORA_RST    14
#define LORA_DIO0   26

#define SWITCH_PIN  4
#define DEBOUNCE_MS 200

const char *MESSAGE = "Hello from ESP32";

/* The lock every task must hold before touching the radio */
SemaphoreHandle_t loraMutex;

TaskHandle_t listenTaskHandle;
TaskHandle_t switchTaskHandle;

/* ---------- Radio helpers (NOT thread-safe on their own -
   callers must hold loraMutex before calling these) ---------- */

void sendMessageLocked(const char *outgoing) {
  LoRa.beginPacket();
  LoRa.print(outgoing);
  LoRa.endPacket();      /* blocks until TxDone */

  Serial.print("Sent: ");
  Serial.println(outgoing);

  LoRa.receive();        /* endPacket() leaves radio in standby - go back to listening */
}

void checkReceiveLocked() {
  int packetSize = LoRa.parsePacket();

  if (packetSize) {
    String received = "";
    while (LoRa.available()) {
      received += (char)LoRa.read();
    }

    Serial.print("Received: ");
    Serial.println(received);
    Serial.print("RSSI: ");
    Serial.println(LoRa.packetRssi());
  }
}

/* ---------- Task 1: listener ---------- */

void listenTask(void *pvParameters) {
  for (;;) {
    if (xSemaphoreTake(loraMutex, portMAX_DELAY) == pdTRUE) {
      checkReceiveLocked();
      xSemaphoreGive(loraMutex);
    }
    vTaskDelay(pdMS_TO_TICKS(10)); /* yield to other tasks between checks */
  }
}

/* ---------- Task 2: switch watcher / sender ---------- */

void switchTask(void *pvParameters) {
  int lastState = digitalRead(SWITCH_PIN); /* expect HIGH idle (pull-up) */

  for (;;) {
    int currentState = digitalRead(SWITCH_PIN);

    if (lastState == HIGH && currentState == LOW) {
      Serial.println("Switch pressed -> sending");

      if (xSemaphoreTake(loraMutex, portMAX_DELAY) == pdTRUE) {
        sendMessageLocked(MESSAGE);
        xSemaphoreGive(loraMutex);
      }

      vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_MS));
      currentState = digitalRead(SWITCH_PIN);
    }

    lastState = currentState;
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void setup() {
  Serial.begin(115200);
  while (!Serial);

  Serial.println();
  Serial.println("ESP32 LoRa Threaded Transceiver");

  pinMode(SWITCH_PIN, INPUT_PULLUP);

  SPI.begin(18, 19, 23, LORA_SS);
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);

  if (!LoRa.begin(433E6)) {
    Serial.println("LoRa initialization failed!");
    while (1);
  }

  /* These MUST match the BeagleBone SX1278. */
  LoRa.setSpreadingFactor(7);
  LoRa.setSignalBandwidth(125E3);
  LoRa.setCodingRate4(5);
  LoRa.enableCrc();
  LoRa.setSyncWord(0x12);

  LoRa.receive(); /* start out listening */

  loraMutex = xSemaphoreCreateMutex();

  Serial.println("LoRa transceiver ready");
  Serial.println("  - listenTask : always listening for incoming packets");
  Serial.println("  - switchTask : press the switch on G4 to send a message");
  Serial.println();

  /* Create the two tasks. Core 1 is the app core on most ESP32 boards
     (core 0 handles WiFi/BT internals), so pin both worker tasks there. */
  xTaskCreatePinnedToCore(listenTask, "listenTask", 4096, NULL, 1, &listenTaskHandle, 1);
  xTaskCreatePinnedToCore(switchTask, "switchTask", 4096, NULL, 1, &switchTaskHandle, 1);
}

void loop() {
  /* Nothing to do here - all real work happens in listenTask and switchTask */
  vTaskDelay(pdMS_TO_TICKS(1000));
}

// 16-channel EEG firmware for DIY-EEG-OpenMind: two Low-Cost-EEG boards stacked.
// Based on Xavier Asghar, Low-Cost-EEG, GPL v3, and firmware/eeg of this repo.
//
// Board A (env:board_a) is the bottom board. It reads its own ADS1299 (channels 1-8),
// receives channels 9-16 from board B over UART (J5 to J5) and sends 16 channels over BLE.
// Board B (env:board_b) is the top board. It reads its ADS1299 and sends every sample to A.
// BLE is off on board B.
//
// J5 wiring: A TX (GPIO43) -> B RX (GPIO44), B TX (GPIO43) -> A RX (GPIO44), GND -> GND.
//
// BLE frame: 55 bytes per sample = counter + 27 bytes of A (3 status + 8x3) + 27 bytes of B.
// 3 samples (165 bytes) per notification so the payload fits on macOS.
//
// SYNC_CLOCK 0: each ADS1299 runs on its own oscillator. A pairs each of its samples with
//               the newest sample from B. Fine for neurofeedback and band power.
// SYNC_CLOCK 1: A outputs its clock on CLK (pin 37), B takes it on CLK with CLKSEL tied to GND.
//               Needs the wire between pins 37 and the CLKSEL change on board B (see docs/05).
//               Set the same value for both boards.
#include <Arduino.h>
#include <ADS1299.h>

#ifndef SYNC_CLOCK
#define SYNC_CLOCK 0
#endif

#if !defined(BOARD_A) && !defined(BOARD_B)
#error "Build env:board_a or env:board_b"
#endif

#ifdef BOARD_A
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <esp_gap_ble_api.h>
#endif

ADS1299 ADS;

// ADS1299 DEFINITIONS
#define RESET_PIN  14
#define DRDY_PIN   9
#define CS_PIN     10
#define MOSI_PIN   11
#define SCK_PIN    12
#define MISO_PIN   13
#define START_PIN  18

// LINK BETWEEN BOARDS (J5)
#define LINK_RX_PIN 44
#define LINK_TX_PIN 43
#define LINK_BAUD   921600
#define FRAME_A     0xA5            // B -> A sample frame: A5 5A cnt data[27] xor
#define FRAME_B     0x5A
#define CMD_HEAD    0xC3            // A -> B command: C3 cmd ~cmd
#define CMD_LEADOFF_ON  'L'
#define CMD_LEADOFF_OFF 'l'
#define CMD_TEST_ON     'T'
#define CMD_TEST_OFF    't'
#define CMD_GO          'G'         // SYNC_CLOCK 1: B may raise START
#define CMD_READY       'R'         // SYNC_CLOCK 1: B -> A, B is configured and waits for GO
HardwareSerial Link(1);

// HARDWARE BUTTON DEFINITION
#define BOOT_BUTTON_PIN 0
volatile uint32_t buttonEdges = 0;

SemaphoreHandle_t drdySemaphore;
SemaphoreHandle_t spiMutex;
bool leadOffEnabled = true;
bool testSignal = false;

void IRAM_ATTR dataReadyISR();
void IRAM_ATTR buttonISR();

// --------------------------------------------------------------------------------------------------------------------------------------
// ADS1299 configuration shared by both boards

void configureADS(bool isA) {
  // CONFIG1: 250 SPS. On A with SYNC_CLOCK the clock goes out on the CLK pin (CLK_EN).
  ADS.WREG(CONFIG1, (isA && SYNC_CLOCK) ? 0b10110110 : 0b10010110);
  ADS.WREG(GPIO, 0b00000000);
  // CONFIG2: internal test signal source, 2 Hz. Only used when a channel selects it.
  ADS.WREG(CONFIG2, 0b11010001);
  // CONFIG3: A drives BIAS (bias lead-off sense only while lead-off is on), B keeps only the reference buffer.
  ADS.WREG(CONFIG3, isA ? (leadOffEnabled ? 0b11111110 : 0b11111100) : 0b11100000);
  ADS.WREG(CONFIG4, 0b00000010);    // Lead-off comparator on

  ADS.WREG(MISC1, 0b00100100);      // SRB1 to all inverting inputs, REF electrode shared by both boards
  ADS.WREG(BIAS_SENSP, isA ? 0b00000011 : 0b00000000);
  ADS.WREG(BIAS_SENSN, 0b00000000);

  ADS.WREG(LOFF, 0b00000100);
  ADS.WREG(LOFF_SENSP, leadOffEnabled ? 0b11111111 : 0b00000000);
  // REF lead-off is sensed on A only (channel 1 N, which is SRB1).
  ADS.WREG(LOFF_SENSN, (isA && leadOffEnabled) ? 0b00000001 : 0b00000000);

  uint8_t chset = testSignal ? 0b01100101 : 0b01100000;   // gain 24, normal input or test signal
  for (int i = CH1SET; i <= CH8SET; i++) ADS.WREG(i, chset);
}

void reconfigure(bool isA) {
  xSemaphoreTake(spiMutex, portMAX_DELAY);
  digitalWrite(START_PIN, LOW);
  delay(10);
  ADS.SDATAC();
  if (isA && !leadOffEnabled) {
    uint8_t config3 = ADS.RREG(CONFIG3);
    Serial.println((config3 & 0b00000001) ? "Bias Disconnected" : "Bias Connected");
  }
  configureADS(isA);
  ADS.RDATAC();
  digitalWrite(START_PIN, HIGH);
  delay(10);
  xSemaphoreGive(spiMutex);
}

bool readID() {
  uint8_t id = ADS.RREG(ID);
  if (id == 0x3E) return true;
  Serial.printf("ADS1299 ID 0x%02X, expected 0x3E\n", id);
  return false;
}

void IRAM_ATTR dataReadyISR() {
  BaseType_t woken = pdFALSE;
  xSemaphoreGiveFromISR(drdySemaphore, &woken);
  if (woken) portYIELD_FROM_ISR();
}

void IRAM_ATTR buttonISR() {
  buttonEdges++;
}

// ======================================================================================================================================
#ifdef BOARD_A

#define bleServerName "ESP32_EEG_DATA"
#define SERVICE_UUID "91bad492-b950-4226-aa2b-4ede9fa42f59"
#define CHARACTERISTIC_UUID "cba1d466-344c-4be3-ab3f-189f80dd7518"
#define FRAME_LEN 55
#define FRAMES_PER_NOTIFY 3

BLECharacteristic *pCharacteristic = NULL;
QueueHandle_t packetQueue;
volatile bool deviceConnected = false;
volatile uint32_t droppedSamples = 0;
uint8_t sampleCounter = 0;

// Newest sample from B
portMUX_TYPE bMux = portMUX_INITIALIZER_UNLOCKED;
uint8_t bLatest[27];
volatile uint32_t bSeq = 0;
volatile uint32_t bLastMs = 0;
volatile uint32_t bBadFrames = 0;
SemaphoreHandle_t bFresh;
volatile bool bReady = false;
uint32_t bUsed = 0, bRepeated = 0;

class MyServerCallbacks: public BLEServerCallbacks {
  void onConnect(BLEServer* pServer, esp_ble_gatts_cb_param_t *param) {
    deviceConnected = true;
    Serial.println("Device connected!");
    pServer->updateConnParams(param->connect.remote_bda, 6, 12, 0, 400);
    esp_ble_gap_set_pkt_data_len(param->connect.remote_bda, 251);
  };
  void onDisconnect(BLEServer* pServer) {
    deviceConnected = false;
    Serial.println("Device disconnected. Restarting advertising...");
    pServer->getAdvertising()->start();
  }
};

void sendCommand(uint8_t cmd) {
  uint8_t c[3] = {CMD_HEAD, cmd, (uint8_t)~cmd};
  Link.write(c, 3);
}

// Parses frames from B. Bytes before a valid header (B's boot log) are skipped.
void linkRxTask(void * pvParameters) {
  uint8_t buf[29];
  int state = 0, n = 0;
  for (;;) {
    while (Link.available()) {
      uint8_t b = Link.read();
      if (state == 0) {
        if (b == FRAME_A) state = 1;
        else if (b == CMD_HEAD) state = 10;
      } else if (state == 1) {
        state = (b == FRAME_B) ? 2 : (b == FRAME_A ? 1 : 0);
        n = 0;
      } else if (state == 2) {
        buf[n++] = b;
        if (n == 29) {                      // cnt + 27 data + xor
          uint8_t x = 0;
          for (int i = 0; i < 28; i++) x ^= buf[i];
          if (x == buf[28]) {
            portENTER_CRITICAL(&bMux);
            memcpy(bLatest, &buf[1], 27);
            bSeq++;
            portEXIT_CRITICAL(&bMux);
            bLastMs = millis();
            xSemaphoreGive(bFresh);
          } else {
            bBadFrames++;
          }
          state = 0;
        }
      } else if (state == 10) {
        buf[0] = b; state = 11;
      } else if (state == 11) {
        if ((uint8_t)~buf[0] == b && buf[0] == CMD_READY) bReady = true;
        state = 0;
      }
    }
    vTaskDelay(1);
  }
}

void readADSDataTask(void * pvParameters) {
  uint8_t frame[FRAME_LEN];
  uint32_t lastSeq = 0;
  for (;;) {
    if (xSemaphoreTake(drdySemaphore, portMAX_DELAY) != pdTRUE) continue;
    xSemaphoreTake(spiMutex, portMAX_DELAY);
    ADS.updateDataPacket(frame);            // fills frame[1..27]
    xSemaphoreGive(spiMutex);
    frame[0] = sampleCounter++;

    // B's sample of the same moment arrives about 0.5 ms later. Wait up to 3 ms for it,
    // otherwise repeat the previous one (clocks drift when SYNC_CLOCK is 0).
    xSemaphoreTake(bFresh, pdMS_TO_TICKS(3));
    portENTER_CRITICAL(&bMux);
    memcpy(&frame[28], bLatest, 27);
    uint32_t seq = bSeq;
    portEXIT_CRITICAL(&bMux);
    if (seq != lastSeq) bUsed++; else bRepeated++;
    lastSeq = seq;

    if (xQueueSend(packetQueue, frame, 0) != pdTRUE) {
      droppedSamples++;
      if (droppedSamples == 1 || droppedSamples % 50 == 0) Serial.printf("Dropped %u samples\n", droppedSamples);
    }
  }
}

void sendDataTask(void * pvParameters) {
  uint8_t one[FRAME_LEN];
  uint8_t batch[FRAME_LEN * FRAMES_PER_NOTIFY];
  int count = 0;
  for (;;) {
    if (!deviceConnected) {
      xQueueReset(packetQueue);
      count = 0;
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    if (xQueueReceive(packetQueue, &one, portMAX_DELAY) == pdPASS) {
      memcpy(&batch[count * FRAME_LEN], one, FRAME_LEN);
      if (++count == FRAMES_PER_NOTIFY) {
        pCharacteristic->setValue(batch, sizeof(batch));
        pCharacteristic->notify();
        count = 0;
      }
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.printf("Board A, 16 channels, SYNC_CLOCK=%d\n", SYNC_CLOCK);

  pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(BOOT_BUTTON_PIN), buttonISR, FALLING);

  packetQueue = xQueueCreate(256, FRAME_LEN);
  spiMutex = xSemaphoreCreateMutex();
  bFresh = xSemaphoreCreateBinary();
  memset(bLatest, 0, sizeof(bLatest));

  Link.setRxBufferSize(4096);
  Link.begin(LINK_BAUD, SERIAL_8N1, LINK_RX_PIN, LINK_TX_PIN);
  xTaskCreatePinnedToCore(linkRxTask, "LinkRx", 4096, NULL, 2, NULL, 0);

  BLEDevice::init(bleServerName);
  BLEDevice::setMTU(512);
  BLEServer *pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());
  BLEService *pService = pServer->createService(SERVICE_UUID);
  pCharacteristic = pService->createCharacteristic(CHARACTERISTIC_UUID, BLECharacteristic::PROPERTY_NOTIFY);
  pCharacteristic->addDescriptor(new BLE2902());
  pService->start();
  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pServer->getAdvertising()->start();
  delay(1000);

  ADS.setup(DRDY_PIN, CS_PIN, SCK_PIN, MISO_PIN, MOSI_PIN, RESET_PIN, START_PIN);
  digitalWrite(START_PIN, LOW);
  delay(10);
  ADS.SDATAC();
  if (!readID()) {
    Serial.println("Board A: replace the ADS1299. Streaming stopped.");
    while (true) delay(1000);
  }
  configureADS(true);

  drdySemaphore = xSemaphoreCreateBinary();
  attachInterrupt(digitalPinToInterrupt(DRDY_PIN), dataReadyISR, FALLING);

#if SYNC_CLOCK
  // The clock is on the CLK pin now. Wait until B has read its ID and is configured.
  uint32_t t0 = millis();
  while (!bReady && millis() - t0 < 8000) delay(10);
  Serial.println(bReady ? "Board B ready" : "Board B did not answer, starting anyway");
#endif

  ADS.RDATAC();
  digitalWrite(START_PIN, HIGH);
#if SYNC_CLOCK
  sendCommand(CMD_GO);
#endif
  delay(10);

  xTaskCreatePinnedToCore(readADSDataTask, "ReadTask", 4096, NULL, 3, NULL, 1);
  xTaskCreatePinnedToCore(sendDataTask, "SendTask", 4096, NULL, 1, NULL, 0);
  Serial.println("Setup complete. Commands: t = test signal on/off, s = link status\n");
}

void loop() {
  static uint32_t seenEdges = 0, lastToggleMs = 0, lastReport = 0;
  static bool bWasAlive = false;

  if (buttonEdges != seenEdges) {
    seenEdges = buttonEdges;
    if (millis() - lastToggleMs > 300) {
      lastToggleMs = millis();
      leadOffEnabled = !leadOffEnabled;
      reconfigure(true);
      sendCommand(leadOffEnabled ? CMD_LEADOFF_ON : CMD_LEADOFF_OFF);
      Serial.println(leadOffEnabled ? "Lead-off enabled (A and B)\n" : "Lead-off disabled (A and B)\n");
    }
  }

  while (Serial.available()) {
    char c = Serial.read();
    if (c == 't') {
      testSignal = !testSignal;
      reconfigure(true);
      sendCommand(testSignal ? CMD_TEST_ON : CMD_TEST_OFF);
      Serial.println(testSignal ? "Test signal ON on all 16 channels" : "Test signal OFF, electrodes");
    } else if (c == 's') {
      lastReport = 0;
    }
  }

  bool alive = millis() - bLastMs < 200;
  if (alive != bWasAlive) {
    Serial.println(alive ? "Board B: data is coming" : "Board B: NO DATA. Channels 9-16 hold the last value.");
    bWasAlive = alive;
  }
  if (millis() - lastReport > 10000) {
    lastReport = millis();
    Serial.printf("B samples used %u, repeated %u, bad frames %u\n", bUsed, bRepeated, bBadFrames);
  }
  delay(20);
}

#endif  // BOARD_A

// ======================================================================================================================================
#ifdef BOARD_B

uint8_t sampleCounter = 0;

void sendFrame(const uint8_t *data27) {
  uint8_t f[31];
  f[0] = FRAME_A; f[1] = FRAME_B; f[2] = sampleCounter++;
  memcpy(&f[3], data27, 27);
  uint8_t x = 0;
  for (int i = 2; i < 30; i++) x ^= f[i];
  f[30] = x;
  Link.write(f, 31);
}

void readAndForwardTask(void * pvParameters) {
  uint8_t packet[28];
  for (;;) {
    if (xSemaphoreTake(drdySemaphore, portMAX_DELAY) != pdTRUE) continue;
    xSemaphoreTake(spiMutex, portMAX_DELAY);
    ADS.updateDataPacket(packet);           // fills packet[1..27]
    xSemaphoreGive(spiMutex);
    sendFrame(&packet[1]);
  }
}

// Commands from A arrive as C3 cmd ~cmd. A's boot log on the same wire is ignored.
int readCommand() {
  static int state = 0;
  static uint8_t cmd = 0;
  while (Link.available()) {
    uint8_t b = Link.read();
    if (state == 0) { if (b == CMD_HEAD) state = 1; }
    else if (state == 1) { cmd = b; state = 2; }
    else { state = 0; if ((uint8_t)~cmd == b) return cmd; }
  }
  return -1;
}

void sendReady() {
  uint8_t c[3] = {CMD_HEAD, CMD_READY, (uint8_t)~CMD_READY};
  Link.write(c, 3);
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.printf("Board B, channels 9-16, SYNC_CLOCK=%d\n", SYNC_CLOCK);

  Link.begin(LINK_BAUD, SERIAL_8N1, LINK_RX_PIN, LINK_TX_PIN);
  spiMutex = xSemaphoreCreateMutex();

  ADS.setup(DRDY_PIN, CS_PIN, SCK_PIN, MISO_PIN, MOSI_PIN, RESET_PIN, START_PIN);
  digitalWrite(START_PIN, LOW);
  delay(10);

#if SYNC_CLOCK
  // Without A's clock the ADS1299 does not answer. Retry until A has turned CLK_EN on.
  bool ok = false;
  for (int i = 0; i < 100 && !ok; i++) {
    ADS.RESET();
    delay(5);
    ADS.SDATAC();
    ok = ADS.RREG(ID) == 0x3E;
    if (!ok) delay(100);
  }
  if (!ok) {
    Serial.println("Board B: no ADS1299 answer. Check the CLK wire (pin 37), CLKSEL to GND and board A.");
    while (true) delay(1000);
  }
#else
  ADS.SDATAC();
  if (!readID()) {
    Serial.println("Board B: replace the ADS1299. Streaming stopped.");
    while (true) delay(1000);
  }
#endif
  configureADS(false);

  drdySemaphore = xSemaphoreCreateBinary();
  attachInterrupt(digitalPinToInterrupt(DRDY_PIN), dataReadyISR, FALLING);
  ADS.RDATAC();

#if SYNC_CLOCK
  // Start together with A: report ready until A sends GO.
  uint32_t t0 = millis(), lastReady = 0;
  for (;;) {
    if (millis() - lastReady > 200) { sendReady(); lastReady = millis(); }
    if (readCommand() == CMD_GO) break;
    if (millis() - t0 > 10000) { Serial.println("No GO from A, starting anyway"); break; }
    delayMicroseconds(20);
  }
#endif
  digitalWrite(START_PIN, HIGH);
  delay(10);

  xTaskCreatePinnedToCore(readAndForwardTask, "ReadTask", 4096, NULL, 3, NULL, 1);
  Serial.println("Setup complete. Sending samples to board A over J5.\n");
}

void loop() {
  int cmd = readCommand();
  if (cmd == CMD_LEADOFF_ON || cmd == CMD_LEADOFF_OFF) {
    leadOffEnabled = (cmd == CMD_LEADOFF_ON);
    reconfigure(false);
    Serial.println(leadOffEnabled ? "Lead-off enabled" : "Lead-off disabled");
  } else if (cmd == CMD_TEST_ON || cmd == CMD_TEST_OFF) {
    testSignal = (cmd == CMD_TEST_ON);
    reconfigure(false);
    Serial.println(testSignal ? "Test signal ON" : "Test signal OFF");
  }
  delay(5);
}

#endif  // BOARD_B

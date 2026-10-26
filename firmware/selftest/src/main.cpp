// Low-Cost-EEG self-test firmware.
// Open this folder (firmware/selftest) in PlatformIO and upload.
// Serial monitor: 115200. Commands: t = run all tests again, m = live monitor on/off.
#include <Arduino.h>
#include <ADS1299.h>
#include <stdarg.h>

ADS1299 ADS;

#define RESET_PIN  14
#define DRDY_PIN   9
#define CS_PIN     10
#define MOSI_PIN   11
#define SCK_PIN    12
#define MISO_PIN   13
#define START_PIN  18

#define N_SAMPLES  1000            // 4 s at 250 SPS

static int32_t buf[8][N_SAMPLES];
static uint8_t lastStatus[3];
static int fails = 0;

double lsbVolts(int gain) { return 4.5 / (gain * 8388608.0); }   // VREF = 4.5 V, 24-bit two's complement

bool readSample(int32_t *ch, uint32_t timeoutMs = 100) {
  uint32_t t0 = millis();
  while (digitalRead(DRDY_PIN) == HIGH) {
    if (millis() - t0 > timeoutMs) return false;
  }
  uint8_t p[28];
  ADS.updateDataPacket(p);           // p[1..3] status, p[4..27] eight channels
  memcpy(lastStatus, &p[1], 3);
  for (int i = 0; i < 8; i++) {
    int32_t v = ((int32_t)p[4 + 3 * i] << 16) | ((int32_t)p[5 + 3 * i] << 8) | p[6 + 3 * i];
    if (v & 0x800000) v |= 0xFF000000;
    ch[i] = v;
  }
  return true;
}

void configure(uint8_t chset, uint8_t config2, uint8_t misc1) {
  digitalWrite(START_PIN, LOW);
  ADS.SDATAC();
  ADS.WREG(CONFIG2, config2);
  ADS.WREG(MISC1, misc1);
  for (int r = CH1SET; r <= CH8SET; r++) ADS.WREG(r, chset);
  ADS.RDATAC();
  digitalWrite(START_PIN, HIGH);
  delay(50);
  int32_t ch[8];
  for (int i = 0; i < 50; i++) readSample(ch);     // let the digital filter settle
}

bool collect(int n) {
  int32_t ch[8];
  for (int k = 0; k < n; k++) {
    if (!readSample(ch)) return false;
    for (int i = 0; i < 8; i++) buf[i][k] = ch[i];
  }
  return true;
}

void stats(int i, int n, double &mean, double &rms, int32_t &mn, int32_t &mx) {
  double s = 0; mn = INT32_MAX; mx = INT32_MIN;
  for (int k = 0; k < n; k++) { s += buf[i][k]; mn = min(mn, buf[i][k]); mx = max(mx, buf[i][k]); }
  mean = s / n;
  double q = 0;
  for (int k = 0; k < n; k++) { double d = buf[i][k] - mean; q += d * d; }
  rms = sqrt(q / n);
}

void result(const char *name, bool ok, const char *fmt, ...) {
  char line[160];
  va_list ap; va_start(ap, fmt); vsnprintf(line, sizeof line, fmt, ap); va_end(ap);
  Serial.printf("[%s] %-22s %s\n", ok ? "PASS" : "FAIL", name, line);
  if (!ok) fails++;
}

void runTests() {
  fails = 0;
  Serial.println("\n=== Low-Cost-EEG self-test ===");
  Serial.printf("ESP32-S3: PSRAM %u bytes, flash %u bytes\n", ESP.getPsramSize(), ESP.getFlashChipSize());
  result("ESP32 PSRAM", ESP.getPsramSize() > 0, "%s", ESP.getPsramSize() > 0 ? "found" : "not found");

  digitalWrite(START_PIN, LOW);
  ADS.SDATAC();

  // 1. Device ID: 0x3E = ADS1299 (8 ch), 0x3D = ADS1299-6, 0x3C = ADS1299-4, 0x00/0xFF = no SPI answer
  uint8_t id = ADS.RREG(ID);
  result("ADS1299 ID", id == 0x3E, "0x%02X (expected 0x3E)", id);
  if (id == 0x00 || id == 0xFF) {
    Serial.println("No answer over SPI. Check DVDD (TP4), U4 pins, CS/SCLK/DIN/DOUT soldering. Other tests skipped.");
    return;
  }

  // 2. Register write/read-back checks the DIN line and the SPI timing
  bool rw = true;
  for (uint8_t pat : {0x5A, 0x25}) { ADS.WREG(CH8SET, pat); if (ADS.RREG(CH8SET) != pat) rw = false; }
  result("Register write/read", rw, "%s", rw ? "CH8SET 0x5A/0x25 read back" : "read-back differs");

  // 3. Reference buffer on, internal BIAS reference, 250 SPS
  ADS.WREG(CONFIG3, 0xEC);
  delay(200);
  ADS.WREG(CONFIG1, 0x96);
  ADS.WREG(CONFIG4, 0x00);
  ADS.WREG(LOFF_SENSP, 0x00);
  ADS.WREG(LOFF_SENSN, 0x00);
  ADS.WREG(BIAS_SENSP, 0x00);
  ADS.WREG(BIAS_SENSN, 0x00);

  // 4. Sample rate. Count finished reads for 1 s.
  //    If a sample is not read, DRDY only pulses high for about 2 µs, so an
  //    interrupt tally misses edges on a working board.
  ADS.RDATAC();
  digitalWrite(START_PIN, HIGH);
  int32_t ch[8];
  if (!readSample(ch, 100)) {
    result("DRDY rate", false, "no DRDY");
  } else {
    uint32_t t0 = millis();
    uint32_t reads = 0;
    while (millis() - t0 < 1000) {
      if (readSample(ch, 10)) reads++;
    }
    result("DRDY rate", reads >= 245 && reads <= 255, "%u Hz (expected 250)", reads);
  }

  // 5. Status word must start with 0xC (checks DOUT alignment)
  bool got = readSample(ch);
  result("Status header", got && (lastStatus[0] >> 4) == 0xC, "0x%X (expected 0xC)", lastStatus[0] >> 4);

  double mean, rms; int32_t mn, mx;

  // 6. Analog span seen from inside. MUX 011 measures (AVDD - AVSS) / 4.
  //    LM27762 sets ±2.5 V, so channels 3 and 4 should read about 5.0 V, not DVDD.
  configure(0x03, 0xC0, 0x00);
  if (collect(250)) {
    for (int i = 2; i <= 3; i++) {
      stats(i, 250, mean, rms, mn, mx);
      double span = mean * lsbVolts(1) * 4.0;
      result(i == 2 ? "AVDD-AVSS via CH3" : "AVDD-AVSS via CH4", span > 4.7 && span < 5.3, "%.3f V (expected 5.0)", span);
    }
  } else result("Supply measure", false, "no DRDY");

  // 7. Internal temperature sensor: 145300 uV at 25 C, +490 uV/C
  configure(0x04, 0xC0, 0x00);
  if (collect(250)) {
    stats(0, 250, mean, rms, mn, mx);
    double uv = mean * lsbVolts(1) * 1e6;
    double t = (uv - 145300.0) / 490.0 + 25.0;
    result("Chip temperature", t > 5 && t < 70, "%.1f C (%.0f uV)", t, uv);
  }

  // 8. Internal test signal, ~1 Hz square wave, gain 1: all 8 channels must match
  configure(0x05, 0xD0, 0x00);
  if (collect(N_SAMPLES)) {
    double pp[8], avg = 0;
    for (int i = 0; i < 8; i++) { stats(i, N_SAMPLES, mean, rms, mn, mx); pp[i] = (mx - mn) * lsbVolts(1) * 1e3; avg += pp[i] / 8; }
    for (int i = 0; i < 8; i++) {
      char nm[24]; snprintf(nm, sizeof nm, "Test signal CH%d", i + 1);
      bool ok = pp[i] > 1.5 && pp[i] < 4.5 && fabs(pp[i] - avg) < 0.02 * avg;
      result(nm, ok, "%.3f mV p-p (mean of 8: %.3f)", pp[i], avg);
    }
  } else result("Test signal", false, "no DRDY");

  // 9. Inputs shorted inside the chip, gain 24: the ADC's own noise
  configure(0x61, 0xC0, 0x00);
  if (collect(N_SAMPLES)) {
    for (int i = 0; i < 8; i++) {
      stats(i, N_SAMPLES, mean, rms, mn, mx);
      double r = rms * lsbVolts(24) * 1e6, pp = (mx - mn) * lsbVolts(24) * 1e6;
      char nm[24]; snprintf(nm, sizeof nm, "Internal noise CH%d", i + 1);
      result(nm, r < 0.5, "%.3f uV rms, %.2f uV p-p", r, pp);
    }
  }

  // 10. Real inputs through the connectors, gain 24, differential (no SRB).
  //     Connect every channel pin (1+..8+, 1-..8-) to GND on J3A before this test.
  configure(0x60, 0xC0, 0x00);
  if (collect(N_SAMPLES)) {
    for (int i = 0; i < 8; i++) {
      stats(i, N_SAMPLES, mean, rms, mn, mx);
      double r = rms * lsbVolts(24) * 1e6, off = mean * lsbVolts(24) * 1e3;
      bool rail = abs(mx) > 8000000 || abs(mn) > 8000000;
      char nm[24]; snprintf(nm, sizeof nm, "Input path CH%d", i + 1);
      result(nm, !rail && r < 1.0, "%.3f uV rms, offset %.3f mV%s", r, off, rail ? ", SATURATED (open input?)" : "");
    }
  }

  Serial.printf("=== %s: %d failed ===\n", fails ? "DONE WITH ERRORS" : "ALL PASSED", fails);
  Serial.println("Send 't' to repeat, 'm' for the live monitor (finger and lead-off test).");
}

bool monitor = false;

void startMonitor() {
  configure(0x60, 0xC0, 0x00);
  digitalWrite(START_PIN, LOW);
  ADS.SDATAC();
  ADS.WREG(CONFIG4, 0x02);          // lead-off comparators on
  ADS.WREG(LOFF, 0x00);             // 95 % threshold, 6 nA, DC
  ADS.WREG(LOFF_SENSP, 0xFF);
  ADS.WREG(LOFF_SENSN, 0xFF);
  ADS.RDATAC();
  digitalWrite(START_PIN, HIGH);
  Serial.println("Monitor: p-p in uV over 0.5 s per channel, then lead-off bits P/N (1 = electrode off).");
}

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 5000) delay(10);
  delay(500);
  ADS.setup(DRDY_PIN, CS_PIN, SCK_PIN, MISO_PIN, MOSI_PIN, RESET_PIN, START_PIN);
  runTests();
}

void loop() {
  if (Serial.available()) {
    char c = Serial.read();
    if (c == 't') { monitor = false; runTests(); }
    if (c == 'm') { monitor = !monitor; if (monitor) startMonitor(); else Serial.println("Monitor off"); }
  }
  if (monitor) {
    if (!collect(125)) { Serial.println("no DRDY"); delay(500); return; }
    double mean, rms; int32_t mn, mx;
    for (int i = 0; i < 8; i++) {
      stats(i, 125, mean, rms, mn, mx);
      Serial.printf("CH%d %8.1f  ", i + 1, (mx - mn) * lsbVolts(24) * 1e6);
    }
    uint8_t loffP = ((lastStatus[0] & 0x0F) << 4) | (lastStatus[1] >> 4);
    uint8_t loffN = ((lastStatus[1] & 0x0F) << 4) | (lastStatus[2] >> 4);
    Serial.print("| P:");
    for (int b = 0; b < 8; b++) Serial.print((loffP >> b) & 1);   // printed CH1..CH8
    Serial.print(" N:");
    for (int b = 0; b < 8; b++) Serial.print((loffN >> b) & 1);
    Serial.println();
  }
}

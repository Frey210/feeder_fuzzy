// firmware/esp32/feeder_esp32.ino
#include <Arduino.h>
#include "secrets.h"
#include <WiFi.h>
#include <BlynkSimpleEsp32.h>
#include <time.h>
#include <math.h>

namespace Config {
static const uint32_t UART_BAUD = 115200;
static const uint32_t MONITOR_BAUD = 115200;
static const int UART2_RX_PIN = 16;
static const int UART2_TX_PIN = 17;
static const uint32_t SAMPLE_INTERVAL_MS = 2000;
static const uint32_t BLYNK_CONNECT_TIMEOUT_MS = 5000;
static const uint32_t WIFI_TIMEOUT_MS = 15000;
static const int SLOT_MORNING_H = 7;
static const int SLOT_EVENING_H = 17;
}  // namespace Config

namespace VPin {
static const uint8_t SIM_TEMP = V1;
static const uint8_t BIOMASS = V2;
static const uint8_t MANUAL_FEED = V3;
static const uint8_t MODE_SELECT = V4;
static const uint8_t PWM_PERCENT = V5;
static const uint8_t GPS_100 = V6;
static const uint8_t SIM_EVENT = V7;
static const uint8_t TEST_IN = V8;

static const uint8_t TEMP_C = V20;
static const uint8_t FEED_REMAIN = V21;
static const uint8_t BIOMASS_OUT = V22;
static const uint8_t LAST_CMD_GRAMS = V23;
static const uint8_t LAST_PWM = V24;
static const uint8_t LAST_EVENT = V25;
}  // namespace VPin

#define VPIN_SIM_TEMP V1
#define VPIN_BIOMASS V2
#define VPIN_MANUAL_FEED V3
#define VPIN_MODE_SELECT V4
#define VPIN_PWM_PERCENT V5
#define VPIN_GPS_100 V6
#define VPIN_SIM_EVENT V7
#define VPIN_TEST_IN V8

BlynkTimer timer;
WidgetTerminal terminal(VPin::LAST_EVENT);

struct AppInputs {
  bool simMode = false;
  float simTempC = 28.0f;
  float simDistanceCm = 25.0f;
  double biomassG = 0.0;
  int modeSelect = 0;
  int pwmPercent = 50;
  int gramsPerSec100 = 5;
};

static AppInputs inputs;

String serialLine;
String monitorLine;
int lastSlotDayMorning = -1;
int lastSlotDayEvening = -1;
uint32_t lastPublishMs = 0;
bool lastTelemetryStale = false;

struct Telemetry {
  bool hasTemp = false;
  bool hasTempReal = false;
  bool hasDistance = false;
  bool hasFeed = false;
  bool hasBiomass = false;
  bool hasCmd = false;
  bool hasPwm = false;
  bool hasMode = false;
  bool hasState = false;
  bool hasSim = false;
  float tempC = NAN;
  float tempRealC = NAN;
  float distanceCm = NAN;
  float feedRemainingG = NAN;
  float biomassG = NAN;
  int lastCmdGrams = 0;
  int lastPwm = 0;
  int mode = -1;
  int state = -1;
  int sim = 0;
  String lastEvent = "NONE";
  uint32_t lastUpdateMs = 0;
};

static Telemetry telem;
static bool telemetryDirty = false;
static HardwareSerial &unoUart = Serial2;

static void sendToUno(const String &line) {
  unoUart.println(line);
}

static void mirrorToMonitor(const String &line) {
  Serial.println(line);
}

static bool timeValid() {
  time_t now = time(nullptr);
  return now > 1700000000;
}

static int dayOfYear() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return -1;
  return timeinfo.tm_yday;
}

static bool isScheduleSlotNow(int hour) {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return false;
  return (timeinfo.tm_hour == hour && timeinfo.tm_min == 0);
}

static void processSchedule() {
  if (inputs.modeSelect != 2) return;
  if (!timeValid()) return;

  int doy = dayOfYear();
  if (doy < 0) return;

  if (isScheduleSlotNow(Config::SLOT_MORNING_H) && lastSlotDayMorning != doy) {
    lastSlotDayMorning = doy;
    sendToUno("CMD:SCHED_07");
  }
  if (isScheduleSlotNow(Config::SLOT_EVENING_H) && lastSlotDayEvening != doy) {
    lastSlotDayEvening = doy;
    sendToUno("CMD:SCHED_17");
  }
}

static void syncToUno() {
  sendToUno(String("SET:MODE=") + inputs.modeSelect);
  sendToUno(String("SET:BIOMASS=") + inputs.biomassG);
  sendToUno(String("SET:PWM=") + inputs.pwmPercent);
  sendToUno(String("SET:GPS100=") + inputs.gramsPerSec100);
  sendToUno(String("SET:SIM_MODE=") + (inputs.simMode ? 1 : 0));
  if (inputs.simMode) {
    sendToUno(String("SET:SIM_TEMP=") + inputs.simTempC);
    sendToUno(String("SET:SIM_DIST=") + inputs.simDistanceCm);
  }
}

static bool isInvalidNumberToken(const String &value) {
  String lower = value;
  lower.toLowerCase();
  return (lower == "nan" || lower == "inf" || lower == "-inf" || lower == "null");
}

static bool parseFloatToken(const String &value, float *out) {
  if (value.length() == 0 || isInvalidNumberToken(value)) {
    return false;
  }
  *out = value.toFloat();
  if (isnan(*out)) {
    return false;
  }
  return true;
}

static bool extractJsonValue(const String &json, const String &key, String *out) {
  String needle = "\"" + key + "\":";
  int start = json.indexOf(needle);
  if (start < 0) return false;
  start += needle.length();
  while (start < json.length() && json[start] == ' ') start++;
  if (start >= json.length()) return false;

  if (json[start] == '"') {
    int end = json.indexOf('"', start + 1);
    if (end < 0) return false;
    *out = json.substring(start + 1, end);
    return true;
  }

  int end = start;
  while (end < json.length() && json[end] != ',' && json[end] != '}') end++;
  *out = json.substring(start, end);
  out->trim();
  return out->length() > 0;
}

static const char *stateName(int state) {
  switch (state) {
    case 0: return "IDLE";
    case 1: return "PRE";
    case 2: return "OPEN";
    case 3: return "RUN";
    case 4: return "CLOSE";
    case 5: return "POST";
    case 6: return "LOW";
    default: return "UNK";
  }
}

static String buildStatusLine(bool stale, uint32_t ageMs) {
  String s;
  s.reserve(80);
  s += "t=";
  s += String(millis() / 1000);
  s += "s ";
  if (stale) {
    s += "STALE ";
    s += String(ageMs / 1000);
    s += "s ";
  }
  s += "S:";
  s += telem.hasState ? stateName(telem.state) : "UNK";
  s += " M:";
  s += telem.hasMode ? String(telem.mode) : "-";
  s += " SIM:";
  s += telem.hasSim ? String(telem.sim) : "-";
  s += " F:";
  if (telem.hasFeed) {
    s += String((int)telem.feedRemainingG);
  } else {
    s += "-";
  }
  s += "g T:";
  if (telem.hasTemp) {
    s += String(telem.tempC, 1);
  } else {
    s += "-";
  }
  s += "C EVT:";
  s += telem.lastEvent;
  return s;
}

static bool parseTelemToken(const String &token) {
  int eq = token.indexOf('=');
  if (eq < 0) return false;
  String key = token.substring(0, eq);
  String val = token.substring(eq + 1);

  if (key == "T") {
    float v;
    if (parseFloatToken(val, &v)) {
      telem.tempC = v;
      telem.hasTemp = true;
      return true;
    }
  } else if (key == "FEED") {
    float v;
    if (parseFloatToken(val, &v)) {
      telem.feedRemainingG = v;
      telem.hasFeed = true;
      return true;
    }
  } else if (key == "BIOMASS") {
    float v;
    if (parseFloatToken(val, &v)) {
      telem.biomassG = v;
      telem.hasBiomass = true;
      return true;
    }
  } else if (key == "CMD") {
    telem.lastCmdGrams = val.toInt();
    telem.hasCmd = true;
    return true;
  } else if (key == "PWM") {
    telem.lastPwm = val.toInt();
    telem.hasPwm = true;
    return true;
  } else if (key == "EVENT") {
    telem.lastEvent = val;
    return true;
  } else if (key == "TR") {
    float v;
    if (parseFloatToken(val, &v)) {
      telem.tempRealC = v;
      telem.hasTempReal = true;
      return true;
    }
  } else if (key == "DIST") {
    float v;
    if (parseFloatToken(val, &v)) {
      telem.distanceCm = v;
      telem.hasDistance = true;
      return true;
    }
  } else if (key == "MODE") {
    telem.mode = val.toInt();
    telem.hasMode = true;
    return true;
  } else if (key == "STATE") {
    telem.state = val.toInt();
    telem.hasState = true;
    return true;
  } else if (key == "SIM") {
    telem.sim = val.toInt();
    telem.hasSim = true;
    return true;
  }
  return false;
}

static bool parseJsonTelemetry(const String &line) {
  if (!line.startsWith("{")) return false;

  bool updated = false;
  String value;

  if (extractJsonValue(line, "temp", &value)) {
    float v;
    if (parseFloatToken(value, &v)) {
      telem.tempC = v;
      telem.tempRealC = v;
      telem.hasTemp = true;
      telem.hasTempReal = true;
      updated = true;
    }
  }
  if (extractJsonValue(line, "distance_mm", &value)) {
    float v;
    if (parseFloatToken(value, &v)) {
      telem.distanceCm = v / 10.0f;
      telem.hasDistance = true;
      updated = true;
    }
  }
  if (extractJsonValue(line, "feed_estimate_g", &value)) {
    float v;
    if (parseFloatToken(value, &v)) {
      telem.feedRemainingG = v;
      telem.hasFeed = true;
      updated = true;
    }
  }
  if (extractJsonValue(line, "biomass", &value)) {
    float v;
    if (parseFloatToken(value, &v)) {
      telem.biomassG = v;
      telem.hasBiomass = true;
      updated = true;
    }
  }
  if (extractJsonValue(line, "fuzzy_output_g", &value)) {
    float v;
    if (parseFloatToken(value, &v)) {
      telem.lastCmdGrams = (int)round(v);
      telem.hasCmd = true;
      updated = true;
    }
  }
  if (extractJsonValue(line, "pwm", &value)) {
    telem.lastPwm = value.toInt();
    telem.hasPwm = true;
    updated = true;
  }
  if (extractJsonValue(line, "mode", &value)) {
    telem.hasMode = true;
    if (value == "A") telem.mode = 0;
    else if (value == "B") telem.mode = 1;
    else if (value == "C") telem.mode = 2;
    else if (value == "D:SMART") telem.mode = 3;
    else if (value == "SIM") telem.mode = inputs.modeSelect;
    updated = true;
  }
  if (extractJsonValue(line, "state", &value)) {
    telem.hasState = true;
    if (value == "IDLE") telem.state = 0;
    else if (value == "PRECHECK") telem.state = 1;
    else if (value == "SERVO_OPENING") telem.state = 2;
    else if (value == "MOTOR_RUNNING") telem.state = 3;
    else if (value == "SERVO_CLOSING") telem.state = 4;
    else if (value == "POSTLOG") telem.state = 5;
    else if (value == "ABORT_LOW_FEED") telem.state = 6;
    updated = true;
  }
  if (extractJsonValue(line, "event", &value)) {
    telem.lastEvent = value;
    updated = true;
  }
  if (extractJsonValue(line, "sim_mode", &value)) {
    value.toLowerCase();
    telem.sim = (value == "true" || value == "1") ? 1 : 0;
    telem.hasSim = true;
    updated = true;
  }

  return updated;
}

static void processSerialLine(const String &line) {
  bool updated = false;

  if (line.startsWith("TELEM,")) {
    int start = 6;
    while (start < line.length()) {
      int comma = line.indexOf(',', start);
      if (comma < 0) comma = line.length();
      String token = line.substring(start, comma);
      if (parseTelemToken(token)) {
        updated = true;
      }
      start = comma + 1;
    }
  } else {
    updated = parseJsonTelemetry(line);
  }

  if (updated) {
    telem.lastUpdateMs = millis();
    telemetryDirty = true;
  }
}

static void pollSerial() {
  while (unoUart.available() > 0) {
    char c = (char)unoUart.read();
    if (c == '\n') {
      if (serialLine.length() > 0) {
        mirrorToMonitor(serialLine);
        processSerialLine(serialLine);
      }
      serialLine = "";
    } else if (c != '\r') {
      serialLine += c;
      if (serialLine.length() > 200) {
        serialLine = "";
      }
    }
  }
}

static void pollMonitorSerial() {
  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    if (c == '\n') {
      if (monitorLine.length() > 0) {
        sendToUno(monitorLine);
      }
      monitorLine = "";
    } else if (c != '\r') {
      monitorLine += c;
      if (monitorLine.length() > 200) {
        monitorLine = "";
      }
    }
  }
}

static void publishTelemetry(bool stale, uint32_t ageMs) {
  if (!Blynk.connected()) return;
  if (telem.hasTemp) {
    Blynk.virtualWrite(VPin::TEMP_C, telem.tempC);
  }
  if (telem.hasFeed) {
    Blynk.virtualWrite(VPin::FEED_REMAIN, telem.feedRemainingG);
  }
  if (telem.hasBiomass) {
    Blynk.virtualWrite(VPin::BIOMASS_OUT, telem.biomassG);
  }
  if (telem.hasCmd) {
    Blynk.virtualWrite(VPin::LAST_CMD_GRAMS, telem.lastCmdGrams);
  }
  if (telem.hasPwm) {
    Blynk.virtualWrite(VPin::LAST_PWM, telem.lastPwm);
  }
  terminal.println(buildStatusLine(stale, ageMs));
  terminal.flush();
  telemetryDirty = false;
}

static void publishTelemetryTask() {
  uint32_t now = millis();
  uint32_t ageMs = (telem.lastUpdateMs == 0) ? 0xFFFFFFFF : (now - telem.lastUpdateMs);
  bool stale = (telem.lastUpdateMs == 0 || ageMs > 7000);
  if (stale != lastTelemetryStale) {
    telemetryDirty = true;
    lastTelemetryStale = stale;
  }
  if (telemetryDirty || (now - lastPublishMs >= 5000)) {
    publishTelemetry(stale, ageMs);
    lastPublishMs = now;
  }
}

BLYNK_CONNECTED() {
  Blynk.syncVirtual(VPin::SIM_TEMP, VPin::BIOMASS, VPin::MANUAL_FEED, VPin::MODE_SELECT,
                    VPin::PWM_PERCENT, VPin::GPS_100, VPin::SIM_EVENT, VPin::TEST_IN);
}

BLYNK_WRITE(VPIN_SIM_TEMP) {
  inputs.simTempC = param.asFloat();
  if (inputs.simMode) {
    sendToUno(String("SET:SIM_TEMP=") + inputs.simTempC);
  }
}

BLYNK_WRITE(VPIN_BIOMASS) {
  inputs.biomassG = param.asFloat();
  sendToUno(String("SET:BIOMASS=") + inputs.biomassG);
}

BLYNK_WRITE(VPIN_MANUAL_FEED) {
  if (param.asInt() == 1) {
    sendToUno("CMD:MANUAL");
    Blynk.virtualWrite(VPin::MANUAL_FEED, 0);
  }
}

BLYNK_WRITE(VPIN_MODE_SELECT) {
  inputs.modeSelect = param.asInt();
  sendToUno(String("SET:MODE=") + inputs.modeSelect);
}

BLYNK_WRITE(VPIN_PWM_PERCENT) {
  inputs.pwmPercent = param.asInt();
  if (inputs.pwmPercent < 0) inputs.pwmPercent = 0;
  if (inputs.pwmPercent > 100) inputs.pwmPercent = 100;
  sendToUno(String("SET:PWM=") + inputs.pwmPercent);
}

BLYNK_WRITE(VPIN_GPS_100) {
  inputs.gramsPerSec100 = param.asInt();
  if (inputs.gramsPerSec100 < 1) inputs.gramsPerSec100 = 1;
  sendToUno(String("SET:GPS100=") + inputs.gramsPerSec100);
}

BLYNK_WRITE(VPIN_SIM_EVENT) {
  int v = param.asInt();
  inputs.simMode = (v == 1);
  sendToUno(String("SET:SIM_MODE=") + (inputs.simMode ? 1 : 0));
  if (inputs.simMode) {
    sendToUno(String("SET:SIM_TEMP=") + inputs.simTempC);
    sendToUno(String("SET:SIM_DIST=") + inputs.simDistanceCm);
    sendToUno("CMD:SIM_EVT");
  }
  Blynk.virtualWrite(VPin::SIM_EVENT, 0);
}

BLYNK_WRITE(VPIN_TEST_IN) {
  inputs.simDistanceCm = param.asFloat();
  if (inputs.simMode) {
    sendToUno(String("SET:SIM_DIST=") + inputs.simDistanceCm);
  }
}

void setup() {
  Serial.begin(Config::MONITOR_BAUD);
  unoUart.begin(Config::UART_BAUD, SERIAL_8N1, Config::UART2_RX_PIN, Config::UART2_TX_PIN);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t startMs = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startMs < Config::WIFI_TIMEOUT_MS) {
    delay(200);
  }

  Blynk.config(BLYNK_AUTH_TOKEN);
  Blynk.connect(Config::BLYNK_CONNECT_TIMEOUT_MS);

  setenv("TZ", "WITA-8", 1);
  tzset();
  configTime(0, 0, "pool.ntp.org", "time.nist.gov", "time.google.com");

  timer.setInterval(1000, processSchedule);
  timer.setInterval(200, pollSerial);
  timer.setInterval(100, pollMonitorSerial);
  timer.setInterval(5000, syncToUno);
  timer.setInterval(2000, publishTelemetryTask);
}

void loop() {
  Blynk.run();
  timer.run();
}

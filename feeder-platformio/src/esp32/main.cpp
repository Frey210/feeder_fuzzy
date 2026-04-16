#include <Arduino.h>
#include "secrets.h"
#include <BlynkSimpleEsp32.h>
#include <WiFi.h>

namespace Config {
static const uint32_t MONITOR_BAUD = 115200;
static const uint32_t UART_BAUD = 115200;
static const int UART2_RX_PIN = 16;
static const int UART2_TX_PIN = 17;
}

namespace VPin {
static const uint8_t BIOMASS = V1;
static const uint8_t SIM_TEMP = V2;
static const uint8_t RUN_FEED = V3;
static const uint8_t PWM = V4;
static const uint8_t FEED_OUT = V20;
static const uint8_t TEMP_OUT = V21;
static const uint8_t FEED_ESTIMATE = V22;
static const uint8_t STATUS_TEXT = V23;
}

#define VPIN_BIOMASS V1
#define VPIN_SIM_TEMP V2
#define VPIN_RUN_FEED V3
#define VPIN_PWM V4

struct Telemetry {
  float tempC = NAN;
  float feedEstimateG = NAN;
  float fuzzyOutputG = NAN;
  float biomassG = NAN;
  int pwm = 0;
  String mode = "AUTO_FUZZY";
  String event = "IDLE";
  uint32_t timestamp = 0;
};

static HardwareSerial &unoSerial = Serial2;
static BlynkTimer timer;
static WidgetTerminal terminal(VPin::STATUS_TEXT);
static Telemetry telemetry;
static String fromUno;
static String fromMonitor;

static bool extractJsonValue(const String &json, const String &key, String *value) {
  String needle = "\"" + key + "\":";
  int start = json.indexOf(needle);
  if (start < 0) return false;
  start += needle.length();
  while (start < json.length() && json[start] == ' ') start++;
  if (start >= json.length()) return false;
  if (json[start] == '"') {
    int end = json.indexOf('"', start + 1);
    if (end < 0) return false;
    *value = json.substring(start + 1, end);
    return true;
  }
  int end = start;
  while (end < json.length() && json[end] != ',' && json[end] != '}') end++;
  *value = json.substring(start, end);
  value->trim();
  return value->length() > 0;
}

static void parseJsonTelemetry(const String &line) {
  String value;
  if (extractJsonValue(line, "timestamp", &value)) telemetry.timestamp = (uint32_t)value.toInt();
  if (extractJsonValue(line, "temp", &value)) telemetry.tempC = value.toFloat();
  if (extractJsonValue(line, "feed_estimate_g", &value)) telemetry.feedEstimateG = value.toFloat();
  if (extractJsonValue(line, "fuzzy_output_g", &value)) telemetry.fuzzyOutputG = value.toFloat();
  if (extractJsonValue(line, "biomass", &value)) telemetry.biomassG = value.toFloat();
  if (extractJsonValue(line, "pwm", &value)) telemetry.pwm = value.toInt();
  if (extractJsonValue(line, "mode", &value)) telemetry.mode = value;
  if (extractJsonValue(line, "event", &value)) telemetry.event = value;
}

static void publishTelemetry() {
  if (!Blynk.connected()) return;
  Blynk.virtualWrite(VPin::TEMP_OUT, telemetry.tempC);
  Blynk.virtualWrite(VPin::FEED_ESTIMATE, telemetry.feedEstimateG);
  Blynk.virtualWrite(VPin::FEED_OUT, telemetry.fuzzyOutputG);
  terminal.println(
      String("ts=") + telemetry.timestamp + " temp=" + telemetry.tempC + " feed=" + telemetry.fuzzyOutputG +
      " event=" + telemetry.event);
  terminal.flush();
}

static void pollUno() {
  while (unoSerial.available() > 0) {
    char c = (char)unoSerial.read();
    if (c == '\n') {
      if (fromUno.length() > 0) {
        Serial.println(fromUno);
        if (fromUno.startsWith("{")) {
          parseJsonTelemetry(fromUno);
          publishTelemetry();
        }
      }
      fromUno = "";
    } else if (c != '\r') {
      fromUno += c;
      if (fromUno.length() > 500) fromUno = "";
    }
  }
}

static void pollMonitor() {
  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    if (c == '\n') {
      if (fromMonitor.length() > 0) {
        unoSerial.println(fromMonitor);
      }
      fromMonitor = "";
    } else if (c != '\r') {
      fromMonitor += c;
      if (fromMonitor.length() > 160) fromMonitor = "";
    }
  }
}

BLYNK_WRITE(VPIN_BIOMASS) { unoSerial.println(String("SET_BIOMASS:") + param.asFloat()); }
BLYNK_WRITE(VPIN_SIM_TEMP) {
  unoSerial.println("SIM_MODE:ON");
  unoSerial.println(String("SET_TEMP:") + param.asFloat());
}
BLYNK_WRITE(VPIN_RUN_FEED) {
  if (param.asInt() == 1) {
    unoSerial.println("RUN_FEED");
    Blynk.virtualWrite(VPin::RUN_FEED, 0);
  }
}
BLYNK_WRITE(VPIN_PWM) { unoSerial.println(String("SET_PWM:") + param.asInt()); }

void setup() {
  Serial.begin(Config::MONITOR_BAUD);
  unoSerial.begin(Config::UART_BAUD, SERIAL_8N1, Config::UART2_RX_PIN, Config::UART2_TX_PIN);

  WiFi.mode(WIFI_STA);
  if (strlen(WIFI_SSID) > 0) {
    WiFi.begin(WIFI_SSID, WIFI_PASS);
  }

  if (strlen(BLYNK_AUTH_TOKEN) > 0) {
    Blynk.config(BLYNK_AUTH_TOKEN);
    Blynk.connect(5000);
  }

  timer.setInterval(100, pollUno);
  timer.setInterval(50, pollMonitor);
  timer.setInterval(3000, []() { unoSerial.println("REQUEST_STATUS"); });
}

void loop() {
  Blynk.run();
  timer.run();
}

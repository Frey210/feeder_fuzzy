#include <Arduino.h>
#include <DallasTemperature.h>
#include <LiquidCrystal_I2C.h>
#include <OneWire.h>
#include <Servo.h>
#include <VL53L1X.h>
#include <Wire.h>

#include "fuzzy_model.h"

namespace Pins {
static const uint8_t DS18B20 = 4;
static const uint8_t SERVO = 9;
static const uint8_t MOTOR_L_PWM = 5;
static const uint8_t MOTOR_R_PWM = 6;
static const uint8_t MOTOR_L_EN = 7;
static const uint8_t MOTOR_R_EN = 8;
static const uint8_t LCD_ADDR = 0x27;
static const uint8_t BTN_MANUAL = 2;
static const uint8_t LED_STATUS = 13;
static const uint8_t LED_LOW_FEED = 10;
}

namespace Config {
static const uint32_t SERIAL_BAUD = 115200;
static const uint32_t SAMPLE_INTERVAL_MS = 1000;
static const uint32_t BUTTON_DEBOUNCE_MS = 60;
static const int SERVO_OPEN_DEG = 30;
static const int SERVO_CLOSE_DEG = 0;
static const uint32_t MOTOR_PRESPIN_MS = 500;
static const uint32_t SERVO_STABILIZE_MS = 700;
static const uint32_t SERVO_SETTLE_MS = 500;
static const float MAX_RUNTIME_S = 60.0f;
static const uint16_t TOF_TIMING_BUDGET_US = 50000;
static const uint16_t TOF_PERIOD_MS = 60;
static const float DIST_EMPTY_CM = 40.0f;
static const float DIST_MAX_CM = 60.0f;
static const float MIN_FEED_RESERVE_G = 10.0f;
}

enum FeedState {
  FEED_IDLE,
  FEED_PRECHECK,
  FEED_SERVO_OPENING,
  FEED_MOTOR_RUNNING,
  FEED_SERVO_CLOSING,
  FEED_POSTLOG,
  FEED_ABORT_LOW_FEED
};

struct Inputs {
  bool simulation = false;
  float biomassG = 1000.0f;
  float simTempC = 28.0f;
  float simDistanceCm = 25.0f;
  bool simDistanceValid = false;
  int pwmPercent = 60;
  float gramsPerSecondAt100 = 8.0f;
};

struct RuntimeData {
  float tempC = NAN;
  float distanceCm = Config::DIST_EMPTY_CM;
  float feedEstimateG = 0.0f;
  float fuzzyOutputG = 0.0f;
  float durationS = 0.0f;
  float realWeightG = NAN;
  fuzzy::Decision fuzzyDecision = {};
};

OneWire oneWire(Pins::DS18B20);
DallasTemperature dallas(&oneWire);
LiquidCrystal_I2C lcd(Pins::LCD_ADDR, 16, 2);
Servo gateServo;
VL53L1X tofSensor;

static Inputs inputs;
static RuntimeData runtimeData;
static FeedState feedState = FEED_IDLE;
static bool tofReady = false;
static uint32_t lastSampleMs = 0;
static uint32_t stateStartMs = 0;
static uint32_t motorRunMs = 0;
static uint32_t buttonChangedMs = 0;
static int lastButtonState = HIGH;
static bool buttonLatched = false;
static char serialBuffer[160];
static uint8_t serialPos = 0;
static char lastEvent[24] = "IDLE";

static void motorStop() {
  analogWrite(Pins::MOTOR_L_PWM, 0);
  analogWrite(Pins::MOTOR_R_PWM, 0);
  digitalWrite(Pins::MOTOR_L_EN, LOW);
  digitalWrite(Pins::MOTOR_R_EN, LOW);
}

static void motorForward(int pwmPercent) {
  int duty = map(constrain(pwmPercent, 0, 100), 0, 100, 0, 255);
  digitalWrite(Pins::MOTOR_L_EN, HIGH);
  digitalWrite(Pins::MOTOR_R_EN, HIGH);
  analogWrite(Pins::MOTOR_L_PWM, 0);
  analogWrite(Pins::MOTOR_R_PWM, duty);
}

static float readDistanceCm() {
  if (!tofReady) return NAN;
  uint16_t mm = tofSensor.read();
  if (tofSensor.timeoutOccurred()) return NAN;
  float cm = mm / 10.0f;
  if (cm < 0.0f || cm > Config::DIST_MAX_CM) return NAN;
  return cm;
}

static float estimateFeedMassG(float distanceCm) {
  struct Point {
    float distCm;
    float massG;
  };

  static const Point curve[] = {
      {40.0f, 0.0f},
      {30.0f, 250.0f},
      {23.0f, 600.0f},
      {17.0f, 1000.0f},
      {12.0f, 1500.0f},
  };

  if (distanceCm >= curve[0].distCm) return 0.0f;
  for (size_t i = 1; i < sizeof(curve) / sizeof(curve[0]); ++i) {
    if (distanceCm <= curve[i - 1].distCm && distanceCm >= curve[i].distCm) {
      float t = (curve[i - 1].distCm - distanceCm) / (curve[i - 1].distCm - curve[i].distCm);
      return curve[i - 1].massG + t * (curve[i].massG - curve[i - 1].massG);
    }
  }
  return curve[(sizeof(curve) / sizeof(curve[0])) - 1].massG;
}

static void setEvent(const char *eventLabel) {
  strncpy(lastEvent, eventLabel, sizeof(lastEvent) - 1);
  lastEvent[sizeof(lastEvent) - 1] = '\0';
}

static void refreshSensorsAndFuzzy() {
  dallas.requestTemperatures();
  float measuredTemp = dallas.getTempCByIndex(0);
  if (measuredTemp > -100.0f && measuredTemp < 150.0f) {
    runtimeData.tempC = measuredTemp;
  }
  if (inputs.simulation) {
    runtimeData.tempC = inputs.simTempC;
  }

  float distanceCm = readDistanceCm();
  if (!isnan(distanceCm)) {
    runtimeData.distanceCm = distanceCm;
  }
  if (inputs.simulation && inputs.simDistanceValid) {
    runtimeData.distanceCm = inputs.simDistanceCm;
  }

  runtimeData.feedEstimateG = estimateFeedMassG(runtimeData.distanceCm);
  runtimeData.fuzzyDecision = fuzzy::evaluate(runtimeData.tempC, inputs.biomassG);
  runtimeData.fuzzyOutputG = runtimeData.fuzzyDecision.outputGrams;

  float gps = max(0.5f, inputs.gramsPerSecondAt100 * (inputs.pwmPercent / 100.0f));
  runtimeData.durationS = min(Config::MAX_RUNTIME_S, runtimeData.fuzzyOutputG / gps);
}

static void sendJsonTelemetry() {
  auto formatFloat = [](char *out, size_t len, float value, uint8_t decimals) {
    dtostrf(value, 0, decimals, out);
    while (out[0] == ' ') {
      memmove(out, out + 1, strlen(out));
    }
  };

  char tempBuf[16];
  char distBuf[16];
  char feedBuf[16];
  char biomassBuf[16];
  char fuzzyBuf[16];
  char durationBuf[16];
  char muTempCold[10];
  char muTempNormal[10];
  char muTempWarm[10];
  char muBioSmall[10];
  char muBioMedium[10];
  char muBioLarge[10];
  char muOutLow[10];
  char muOutMedium[10];
  char muOutHigh[10];

  formatFloat(tempBuf, sizeof(tempBuf), runtimeData.tempC, 2);
  formatFloat(distBuf, sizeof(distBuf), runtimeData.distanceCm * 10.0f, 2);
  formatFloat(feedBuf, sizeof(feedBuf), runtimeData.feedEstimateG, 2);
  formatFloat(biomassBuf, sizeof(biomassBuf), inputs.biomassG, 2);
  formatFloat(fuzzyBuf, sizeof(fuzzyBuf), runtimeData.fuzzyOutputG, 2);
  formatFloat(durationBuf, sizeof(durationBuf), runtimeData.durationS, 2);
  formatFloat(muTempCold, sizeof(muTempCold), runtimeData.fuzzyDecision.tempMembership[fuzzy::TEMP_COLD], 3);
  formatFloat(muTempNormal, sizeof(muTempNormal), runtimeData.fuzzyDecision.tempMembership[fuzzy::TEMP_NORMAL], 3);
  formatFloat(muTempWarm, sizeof(muTempWarm), runtimeData.fuzzyDecision.tempMembership[fuzzy::TEMP_WARM], 3);
  formatFloat(muBioSmall, sizeof(muBioSmall), runtimeData.fuzzyDecision.biomassMembership[fuzzy::BIO_SMALL], 3);
  formatFloat(muBioMedium, sizeof(muBioMedium), runtimeData.fuzzyDecision.biomassMembership[fuzzy::BIO_MEDIUM], 3);
  formatFloat(muBioLarge, sizeof(muBioLarge), runtimeData.fuzzyDecision.biomassMembership[fuzzy::BIO_LARGE], 3);
  formatFloat(muOutLow, sizeof(muOutLow), runtimeData.fuzzyDecision.outputActivation[fuzzy::FEED_LOW], 3);
  formatFloat(muOutMedium, sizeof(muOutMedium), runtimeData.fuzzyDecision.outputActivation[fuzzy::FEED_MEDIUM], 3);
  formatFloat(muOutHigh, sizeof(muOutHigh), runtimeData.fuzzyDecision.outputActivation[fuzzy::FEED_HIGH], 3);

  char payload[512];
  snprintf(
      payload,
      sizeof(payload),
      "{\"timestamp\":%lu,\"temp\":%s,\"distance_mm\":%s,\"feed_estimate_g\":%s,"
      "\"biomass\":%s,\"fuzzy_output_g\":%s,\"pwm\":%d,\"duration_s\":%s,"
      "\"mode\":\"%s\",\"event\":\"%s\",\"state\":%d,"
      "\"mu_temp_cold\":%s,\"mu_temp_normal\":%s,\"mu_temp_warm\":%s,"
      "\"mu_bio_small\":%s,\"mu_bio_medium\":%s,\"mu_bio_large\":%s,"
      "\"mu_out_low\":%s,\"mu_out_medium\":%s,\"mu_out_high\":%s}",
      millis(),
      tempBuf,
      distBuf,
      feedBuf,
      biomassBuf,
      fuzzyBuf,
      inputs.pwmPercent,
      durationBuf,
      inputs.simulation ? "SIM_FUZZY" : "AUTO_FUZZY",
      lastEvent,
      (int)feedState,
      muTempCold,
      muTempNormal,
      muTempWarm,
      muBioSmall,
      muBioMedium,
      muBioLarge,
      muOutLow,
      muOutMedium,
      muOutHigh);
  Serial.println(payload);
}

static void updateLcd() {
  lcd.setCursor(0, 0);
  lcd.print("T:");
  lcd.print(runtimeData.tempC, 1);
  lcd.print(" B:");
  lcd.print((int)inputs.biomassG);
  lcd.print("   ");

  lcd.setCursor(0, 1);
  lcd.print("F:");
  lcd.print(runtimeData.fuzzyOutputG, 1);
  lcd.print("g D:");
  lcd.print(runtimeData.distanceCm, 1);
  lcd.print("  ");
}

static void beginFeedEvent(const char *eventLabel) {
  refreshSensorsAndFuzzy();
  setEvent(eventLabel);
  motorRunMs = (uint32_t)(runtimeData.durationS * 1000.0f);
  feedState = FEED_PRECHECK;
  stateStartMs = millis();
}

static void handleFeedState() {
  uint32_t now = millis();
  switch (feedState) {
    case FEED_IDLE:
      digitalWrite(Pins::LED_LOW_FEED, LOW);
      break;
    case FEED_PRECHECK:
      if (runtimeData.feedEstimateG + Config::MIN_FEED_RESERVE_G < runtimeData.fuzzyOutputG) {
        feedState = FEED_ABORT_LOW_FEED;
        stateStartMs = now;
        setEvent("LOW_FEED_ABORT");
        digitalWrite(Pins::LED_LOW_FEED, HIGH);
        break;
      }
      motorForward(inputs.pwmPercent);
      feedState = FEED_SERVO_OPENING;
      stateStartMs = now;
      break;
    case FEED_SERVO_OPENING:
      if (now - stateStartMs >= Config::MOTOR_PRESPIN_MS) {
        gateServo.write(Config::SERVO_OPEN_DEG);
      }
      if (now - stateStartMs >= Config::MOTOR_PRESPIN_MS + Config::SERVO_STABILIZE_MS) {
        feedState = FEED_MOTOR_RUNNING;
        stateStartMs = now;
      }
      break;
    case FEED_MOTOR_RUNNING:
      if (now - stateStartMs >= motorRunMs) {
        motorStop();
        gateServo.write(Config::SERVO_CLOSE_DEG);
        feedState = FEED_SERVO_CLOSING;
        stateStartMs = now;
      }
      break;
    case FEED_SERVO_CLOSING:
      if (now - stateStartMs >= Config::SERVO_SETTLE_MS) {
        feedState = FEED_POSTLOG;
        stateStartMs = now;
      }
      break;
    case FEED_POSTLOG:
      refreshSensorsAndFuzzy();
      sendJsonTelemetry();
      setEvent("IDLE");
      feedState = FEED_IDLE;
      break;
    case FEED_ABORT_LOW_FEED:
      if (now - stateStartMs >= 3000) {
        setEvent("IDLE");
        feedState = FEED_IDLE;
      }
      break;
  }
}

static void handleButton() {
  int raw = digitalRead(Pins::BTN_MANUAL);
  uint32_t now = millis();
  if (raw != lastButtonState) {
    lastButtonState = raw;
    buttonChangedMs = now;
  }
  if ((now - buttonChangedMs) > Config::BUTTON_DEBOUNCE_MS) {
    if (raw == LOW && !buttonLatched && feedState == FEED_IDLE) {
      buttonLatched = true;
      beginFeedEvent("MANUAL_BUTTON");
    } else if (raw == HIGH) {
      buttonLatched = false;
    }
  }
}

static void parseSetCommand(const char *line) {
  if (strncmp(line, "SET_TEMP:", 9) == 0) {
    inputs.simTempC = atof(line + 9);
    inputs.simulation = true;
  } else if (strncmp(line, "SET_BIOMASS:", 12) == 0) {
    inputs.biomassG = atof(line + 12);
  } else if (strncmp(line, "SET_DISTANCE:", 13) == 0) {
    inputs.simDistanceCm = atof(line + 13);
    inputs.simDistanceValid = true;
    inputs.simulation = true;
  } else if (strncmp(line, "SET_PWM:", 8) == 0) {
    inputs.pwmPercent = constrain(atoi(line + 8), 0, 100);
  } else if (strncmp(line, "SET_GPS100:", 11) == 0) {
    inputs.gramsPerSecondAt100 = max(0.5f, atof(line + 11));
  } else if (strncmp(line, "SET_REAL_WEIGHT:", 16) == 0) {
    runtimeData.realWeightG = atof(line + 16);
  } else if (strncmp(line, "SET:", 4) == 0) {
    const char *kv = line + 4;
    if (strncmp(kv, "BIOMASS=", 8) == 0) inputs.biomassG = atof(kv + 8);
    else if (strncmp(kv, "PWM=", 4) == 0) inputs.pwmPercent = constrain(atoi(kv + 4), 0, 100);
    else if (strncmp(kv, "GPS100=", 7) == 0) inputs.gramsPerSecondAt100 = max(0.5f, atof(kv + 7));
    else if (strncmp(kv, "SIM_TEMP=", 9) == 0) {
      inputs.simTempC = atof(kv + 9);
      inputs.simulation = true;
    } else if (strncmp(kv, "SIM_DIST=", 9) == 0) {
      inputs.simDistanceCm = atof(kv + 9);
      inputs.simDistanceValid = true;
      inputs.simulation = true;
    }
  }
}

static void handleSerialCommand(const char *line) {
  if (strcmp(line, "SIM_MODE:ON") == 0) {
    inputs.simulation = true;
  } else if (strcmp(line, "SIM_MODE:OFF") == 0) {
    inputs.simulation = false;
    inputs.simDistanceValid = false;
  } else if (strcmp(line, "RUN_FEED") == 0) {
    if (feedState == FEED_IDLE) beginFeedEvent("REMOTE_FEED");
  } else if (strcmp(line, "REQUEST_STATUS") == 0 || strcmp(line, "PING") == 0) {
    refreshSensorsAndFuzzy();
    sendJsonTelemetry();
  } else if (strncmp(line, "RUN_TEST_", 9) == 0) {
    Serial.println("{\"ack\":\"TEST_CONTEXT_ACCEPTED\"}");
  } else {
    parseSetCommand(line);
  }
}

static void pollSerial() {
  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    if (c == '\n') {
      serialBuffer[serialPos] = '\0';
      if (serialPos > 0) handleSerialCommand(serialBuffer);
      serialPos = 0;
    } else if (c != '\r' && serialPos < sizeof(serialBuffer) - 1) {
      serialBuffer[serialPos++] = c;
    }
  }
}

void setup() {
  Serial.begin(Config::SERIAL_BAUD);

  pinMode(Pins::MOTOR_L_PWM, OUTPUT);
  pinMode(Pins::MOTOR_R_PWM, OUTPUT);
  pinMode(Pins::MOTOR_L_EN, OUTPUT);
  pinMode(Pins::MOTOR_R_EN, OUTPUT);
  pinMode(Pins::BTN_MANUAL, INPUT_PULLUP);
  pinMode(Pins::LED_STATUS, OUTPUT);
  pinMode(Pins::LED_LOW_FEED, OUTPUT);

  motorStop();
  gateServo.attach(Pins::SERVO);
  gateServo.write(Config::SERVO_CLOSE_DEG);

  Wire.begin();
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.print("Fuzzy feeder");

  dallas.begin();

  tofSensor.setTimeout(70);
  tofReady = tofSensor.init();
  if (tofReady) {
    tofSensor.setDistanceMode(VL53L1X::Long);
    tofSensor.setMeasurementTimingBudget(Config::TOF_TIMING_BUDGET_US);
    tofSensor.startContinuous(Config::TOF_PERIOD_MS);
  }

  setEvent("BOOT");
  refreshSensorsAndFuzzy();
  sendJsonTelemetry();
  setEvent("IDLE");
}

void loop() {
  pollSerial();

  uint32_t now = millis();
  if (now - lastSampleMs >= Config::SAMPLE_INTERVAL_MS) {
    lastSampleMs = now;
    refreshSensorsAndFuzzy();
    updateLcd();
    sendJsonTelemetry();
  }

  handleButton();
  handleFeedState();
  digitalWrite(Pins::LED_STATUS, feedState == FEED_IDLE ? LOW : HIGH);
}

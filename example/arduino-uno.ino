// firmware/uno/feeder_uno.ino
#include <Arduino.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <LiquidCrystal_I2C.h>
#include <Servo.h>
#include <VL53L1X.h>
#include <Wire.h>
#define MODE_D 3

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
}  // namespace Pins

namespace Config {
static const uint32_t SAMPLE_INTERVAL_MS = 1000;
static const float DIST_SENSOR_MAX_CM = 60.0f;
static const float MASS_MIN_G = 0.0f;
static const float MASS_MAX_G = 50000.0f;
static const float SENSOR_MOUNT_DROP_CM = 4.2f;
static const float DIST_EMPTY_CM = 40.0f;
static const uint32_t BTN_DEBOUNCE_MS = 60;
static const int SERVO_OPEN_DEG = 30;
static const int SERVO_CLOSE_DEG = 0;
static const uint32_t MOTOR_PRESPIN_MS = 600;
static const uint32_t SERVO_STABILIZE_MS = 800;
static const uint32_t SERVO_SETTLE_MS = 600;
static const uint32_t SERIAL_BAUD = 115200;
static const float MAX_MOTOR_RUNTIME_S = 60.0f;
static const float DIST_OFFSET_CM = 0.0f;
static const float DIST_SCALE = 1.0f;
static const bool DEBUG_DISTANCE = false;
static const float DIST_SLOW_ALPHA = 0.25f;
static const float DIST_FAST_ALPHA = 0.65f;
static const float DIST_FAST_THRESHOLD_CM = 1.0f;
static const float DIST_JITTER_CM = 0.12f;
static const float EMPTY_ZERO_BAND_CM = 1.2f;
static const uint16_t TOF_TIMING_BUDGET_US = 50000;
static const uint16_t TOF_PERIOD_MS = 60;
static const float MODE_D_EMPTY_DIST_CM = 2.0f;
}  // namespace Config

OneWire oneWire(Pins::DS18B20);
DallasTemperature dallas(&oneWire);
LiquidCrystal_I2C lcd(Pins::LCD_ADDR, 16, 2);
Servo valveServo;
VL53L1X tofSensor;

enum FeedState {
  IDLE,
  PRECHECK,
  SERVO_OPENING,
  MOTOR_RUNNING,
  SERVO_CLOSING,
  POSTLOG,
  ABORT_LOW_FEED
};

struct AppInputs {
  bool simMode = false;
  float simTempC = 28.0f;
  float simDistanceCm = Config::DIST_EMPTY_CM;
  double biomassG = 0.0;
  int modeSelect = 0;
  int pwmPercent = 50;
  int gramsPerSec100 = 5;
};

struct AppRequests {
  bool manualFeed = false;
  bool simEvent = false;
  bool schedMorning = false;
  bool schedEvening = false;
  bool schedModeD0800 = false;
  bool schedModeD1330 = false;
  bool schedModeD1900 = false;
};

static AppInputs inputs;
static AppRequests requests;

FeedState feedState = IDLE;
uint32_t stateStartMs = 0;
uint32_t motorRunMs = 0;
float lastTempC = NAN;
float lastTempCReal = NAN;
float lastFeedRemainingG = 0.0f;
float lastDistanceCm = Config::DIST_EMPTY_CM;
bool lastDistanceValid = false;
bool tofReady = false;
float lastFuzzyOutputG = 0.0f;
float lastDurationS = 0.0f;
bool simDistanceOverrideEnabled = false;

int lastCmdGrams = 0;
int lastPwm = 0;
char lastEventLabel[20] = "NONE";

String lcdLine1Cache;
String lcdLine2Cache;

uint32_t lastSampleMs = 0;
uint32_t lastBtnChangeMs = 0;
int lastBtnState = HIGH;
bool btnLatched = false;

char serialBuf[120];
uint8_t serialPos = 0;

static float clampf(float v, float lo, float hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

static void lcdSetLines(const String &line1, const String &line2) {
  if (line1 != lcdLine1Cache) {
    lcd.setCursor(0, 0);
    lcd.print("                ");
    lcd.setCursor(0, 0);
    lcd.print(line1);
    lcdLine1Cache = line1;
  }
  if (line2 != lcdLine2Cache) {
    lcd.setCursor(0, 1);
    lcd.print("                ");
    lcd.setCursor(0, 1);
    lcd.print(line2);
    lcdLine2Cache = line2;
  }
}

static void motorStop() {
  analogWrite(Pins::MOTOR_L_PWM, 0);
  analogWrite(Pins::MOTOR_R_PWM, 0);
  digitalWrite(Pins::MOTOR_L_EN, LOW);
  digitalWrite(Pins::MOTOR_R_EN, LOW);
}

static void motorForward(int pwm) {
  int duty = map(pwm, 0, 100, 0, 255);
  digitalWrite(Pins::MOTOR_L_EN, HIGH);
  digitalWrite(Pins::MOTOR_R_EN, HIGH);
  analogWrite(Pins::MOTOR_L_PWM, 0);
  analogWrite(Pins::MOTOR_R_PWM, duty);
}

static float readDistanceCm() {
  if (!tofReady) {
    return NAN;
  }

  const int samples = 3;
  float sum = 0.0f;
  int valid = 0;

  for (int i = 0; i < samples; i++) {
    uint16_t mm = tofSensor.read();
    float distanceCm = mm / 10.0f;
    if (tofSensor.timeoutOccurred()) {
      distanceCm = NAN;
    }

    if (!isnan(distanceCm) && distanceCm >= 1.0f && distanceCm <= Config::DIST_SENSOR_MAX_CM) {
      sum += distanceCm;
      valid++;
    }
    delay(5);
  }

  if (valid == 0) {
    return NAN;
  }
  return sum / valid;
}

static float estimateFeedMassG(float distanceCm) {
  struct CalPoint {
    float distCm;
    float massG;
  };

  static const CalPoint points[] = {
      {Config::DIST_EMPTY_CM, 0.0f},
      {Config::DIST_EMPTY_CM - 14.5f, 200.0f},
      {Config::DIST_EMPTY_CM - 17.6f, 400.0f},
      {Config::DIST_EMPTY_CM - 23.7f, 800.0f},
      {Config::DIST_EMPTY_CM - 27.0f, 1000.0f},
  };

  const int count = sizeof(points) / sizeof(points[0]);
  float emptyThresholdCm = points[0].distCm - Config::EMPTY_ZERO_BAND_CM;
  if (distanceCm >= emptyThresholdCm) {
    return points[0].massG;
  }

  for (int i = 1; i < count; i++) {
    float dHi = points[i - 1].distCm;
    float dLo = points[i].distCm;
    if (distanceCm <= dHi && distanceCm >= dLo) {
      float t = (dHi - distanceCm) / (dHi - dLo);
      float mass = points[i - 1].massG + t * (points[i].massG - points[i - 1].massG);
      if (mass < 20.0f) {
        mass = 0.0f;
      }
      return clampf(mass, Config::MASS_MIN_G, Config::MASS_MAX_G);
    }
  }

  float dHi = points[count - 2].distCm;
  float dLo = points[count - 1].distCm;
  float t = (dHi - distanceCm) / (dHi - dLo);
  float mass = points[count - 2].massG + t * (points[count - 1].massG - points[count - 2].massG);
  if (mass < 20.0f) {
    mass = 0.0f;
  }
  return clampf(mass, Config::MASS_MIN_G, Config::MASS_MAX_G);
}

static const char *modeToChar(int mode) {
  if (mode == 0) return "A";
  if (mode == 1) return "B";
  if (mode == MODE_D) return "D:SMART";
  return "C";
}

static const char *stateToString(FeedState state) {
  switch (state) {
    case IDLE: return "IDLE";
    case PRECHECK: return "PRECHECK";
    case SERVO_OPENING: return "SERVO_OPENING";
    case MOTOR_RUNNING: return "MOTOR_RUNNING";
    case SERVO_CLOSING: return "SERVO_CLOSING";
    case POSTLOG: return "POSTLOG";
    case ABORT_LOW_FEED: return "ABORT_LOW_FEED";
    default: return "UNKNOWN";
  }
}

static const char *jsonModeLabel() {
  if (inputs.simMode) return "SIM";
  return modeToChar(inputs.modeSelect);
}

static int computeCommandedGrams(float tempC) {
  if (inputs.modeSelect == 0) {
    return 50;
  }
  if (inputs.modeSelect == MODE_D) {
    if (tempC >= 32.0f && tempC <= 37.0f) return 50;
    if (tempC >= 24.0f && tempC < 32.0f) return 40;
    if (tempC >= 4.0f && tempC < 24.0f) return 30;
    return 30;
  }
  float pct = (tempC >= 25.0f && tempC <= 37.0f) ? 0.03f : 0.02f;
  return (int)round(inputs.biomassG * pct);
}

static void startFeedEvent(const char *eventLabel) {
  strncpy(lastEventLabel, eventLabel, sizeof(lastEventLabel) - 1);
  lastEventLabel[sizeof(lastEventLabel) - 1] = '\0';
  lastCmdGrams = computeCommandedGrams(lastTempC);
  lastFuzzyOutputG = lastCmdGrams;
  lastPwm = inputs.pwmPercent;

  double gps100 = (inputs.gramsPerSec100 < 1) ? 1.0 : inputs.gramsPerSec100;
  double gramsPerSec = gps100 * (inputs.pwmPercent / 100.0);
  if (gramsPerSec < 0.1) gramsPerSec = 0.1;

  double runtimeS = lastCmdGrams / gramsPerSec;
  if (runtimeS > Config::MAX_MOTOR_RUNTIME_S) runtimeS = Config::MAX_MOTOR_RUNTIME_S;
  lastDurationS = runtimeS;

  motorRunMs = (uint32_t)(runtimeS * 1000.0);

  feedState = PRECHECK;
  stateStartMs = millis();
}

static bool triggerDispenseEvent(const char *eventLabel) {
  if (feedState != IDLE) return false;

  if (inputs.modeSelect == MODE_D) {
    float distanceCm = inputs.simMode ? inputs.simDistanceCm : readDistanceCm();
    if (isnan(distanceCm)) {
      distanceCm = lastDistanceCm;
    }
    if (distanceCm <= Config::MODE_D_EMPTY_DIST_CM) {
      strncpy(lastEventLabel, "PAKAN_HABIS", sizeof(lastEventLabel) - 1);
      lastEventLabel[sizeof(lastEventLabel) - 1] = '\0';
      digitalWrite(Pins::LED_LOW_FEED, HIGH);
      lcdSetLines("D:SMART", "Pakan Habis");
      return false;
    }
  }

  startFeedEvent(eventLabel);
  return true;
}

static void sendTelemetry() {
  char tBuf[16];
  char trBuf[16];
  char distBuf[16];
  char feedBuf[16];
  char biomassBuf[16];
  char buf[220];
  char tJson[16];
  char distJson[16];
  char feedJson[16];
  char biomassJson[16];
  char fuzzyJson[16];
  char durationJson[16];

  auto formatFloat = [](char *out, size_t outLen, float v, uint8_t decimals) {
    if (isnan(v)) {
      strncpy(out, "nan", outLen);
      out[outLen - 1] = '\0';
      return;
    }
    dtostrf(v, 0, decimals, out);
    while (out[0] == ' ') {
      memmove(out, out + 1, strlen(out));
    }
  };

  formatFloat(tBuf, sizeof(tBuf), lastTempC, 2);
  formatFloat(trBuf, sizeof(trBuf), lastTempCReal, 2);
  formatFloat(distBuf, sizeof(distBuf), lastDistanceCm, 1);
  formatFloat(feedBuf, sizeof(feedBuf), lastFeedRemainingG, 0);
  formatFloat(biomassBuf, sizeof(biomassBuf), inputs.biomassG, 0);
  formatFloat(tJson, sizeof(tJson), lastTempC, 2);
  formatFloat(distJson, sizeof(distJson), isnan(lastDistanceCm) ? NAN : (lastDistanceCm * 10.0f), 2);
  formatFloat(feedJson, sizeof(feedJson), lastFeedRemainingG, 2);
  formatFloat(biomassJson, sizeof(biomassJson), inputs.biomassG, 2);
  formatFloat(fuzzyJson, sizeof(fuzzyJson), lastFuzzyOutputG, 2);
  formatFloat(durationJson, sizeof(durationJson), lastDurationS, 2);
  if (strcmp(tJson, "nan") == 0) strncpy(tJson, "null", sizeof(tJson));
  if (strcmp(distJson, "nan") == 0) strncpy(distJson, "null", sizeof(distJson));
  if (strcmp(feedJson, "nan") == 0) strncpy(feedJson, "null", sizeof(feedJson));
  if (strcmp(biomassJson, "nan") == 0) strncpy(biomassJson, "null", sizeof(biomassJson));
  if (strcmp(fuzzyJson, "nan") == 0) strncpy(fuzzyJson, "null", sizeof(fuzzyJson));
  if (strcmp(durationJson, "nan") == 0) strncpy(durationJson, "null", sizeof(durationJson));

  snprintf(
      buf,
      sizeof(buf),
      "TELEM,T=%s,TR=%s,DIST=%s,FEED=%s,MODE=%d,STATE=%d,EVENT=%s,BIOMASS=%s,CMD=%d,PWM=%d,SIM=%d",
      tBuf,
      trBuf,
      distBuf,
      feedBuf,
      inputs.modeSelect,
      (int)feedState,
      lastEventLabel,
      biomassBuf,
      lastCmdGrams,
      lastPwm,
      inputs.simMode ? 1 : 0);
  Serial.println(buf);

  char jsonBuf[320];
  snprintf(
      jsonBuf,
      sizeof(jsonBuf),
      "{\"timestamp\":%lu,\"temp\":%s,\"distance_mm\":%s,\"feed_estimate_g\":%s,"
      "\"biomass\":%s,\"fuzzy_output_g\":%s,\"pwm\":%d,\"duration_s\":%s,"
      "\"mode\":\"%s\",\"state\":\"%s\",\"event\":\"%s\",\"sim_mode\":%s}",
      millis(),
      tJson,
      distJson,
      feedJson,
      biomassJson,
      fuzzyJson,
      lastPwm,
      durationJson,
      jsonModeLabel(),
      stateToString(feedState),
      lastEventLabel,
      inputs.simMode ? "true" : "false");
  Serial.println(jsonBuf);
}

static void samplingTask() {
  dallas.requestTemperatures();
  float tempC = dallas.getTempCByIndex(0);
  if (tempC > -100 && tempC < 150) {
    lastTempCReal = tempC;
  }

  float distanceRawCm = readDistanceCm();
  float distanceInputCm = simDistanceOverrideEnabled ? inputs.simDistanceCm : distanceRawCm;
  float distanceCm = inputs.simMode ? distanceInputCm : distanceRawCm;
  if (!inputs.simMode && isnan(distanceRawCm)) {
    distanceCm = lastDistanceCm;
  } else {
    distanceCm = distanceCm * Config::DIST_SCALE + Config::DIST_OFFSET_CM;
  }
  if (!inputs.simMode) {
    if (lastDistanceValid) {
      float delta = fabsf(distanceCm - lastDistanceCm);
      float alpha = (delta >= Config::DIST_FAST_THRESHOLD_CM) ? Config::DIST_FAST_ALPHA : Config::DIST_SLOW_ALPHA;
      float filtered = (alpha * distanceCm) + ((1.0f - alpha) * lastDistanceCm);
      if (fabsf(filtered - lastDistanceCm) < Config::DIST_JITTER_CM) {
        filtered = lastDistanceCm;
      }
      distanceCm = filtered;
    }
    lastDistanceValid = true;
  }
  if (Config::DEBUG_DISTANCE) {
    Serial.print("RAW=");
    if (isnan(distanceRawCm)) Serial.print("nan");
    else Serial.print(distanceRawCm, 2);
    Serial.print(", RAW_TANKREF=");
    if (isnan(distanceRawCm)) Serial.print("nan");
    else Serial.print(distanceRawCm + Config::SENSOR_MOUNT_DROP_CM, 2);
    Serial.print(", CAL=");
    Serial.println(distanceCm, 2);
  }
  float distanceForFeed = clampf(distanceCm, 0.0f, Config::DIST_SENSOR_MAX_CM);
  lastDistanceCm = distanceCm;
  lastFeedRemainingG = estimateFeedMassG(distanceForFeed);

  float effectiveTemp = inputs.simMode ? inputs.simTempC : lastTempCReal;
  if (!isnan(effectiveTemp)) {
    lastTempC = effectiveTemp;
  }

  String realStr = isnan(lastTempCReal) ? "--" : String(lastTempCReal, 1);
  String line1 = (inputs.modeSelect == MODE_D)
                     ? "D:SMART R:" + realStr
                     : "M:" + String(modeToChar(inputs.modeSelect)) + " R:" + realStr;
  String line2;
  if (feedState == IDLE) {
    line2 = "D:" + String(distanceCm, 1) + " F:" + String((int)lastFeedRemainingG);
  } else if (feedState == ABORT_LOW_FEED) {
    line2 = "LOW FEED!";
  } else {
    line2 = "Feeding...";
  }

  lcdSetLines(line1, line2);
  sendTelemetry();
}

static void handleButton() {
  int raw = digitalRead(Pins::BTN_MANUAL);
  uint32_t now = millis();

  if (raw != lastBtnState) {
    lastBtnChangeMs = now;
    lastBtnState = raw;
  }

  if (now - lastBtnChangeMs > Config::BTN_DEBOUNCE_MS) {
    if (raw == LOW && !btnLatched && !requests.manualFeed) {
      requests.manualFeed = true;
      btnLatched = true;
    }
    if (raw == HIGH) {
      btnLatched = false;
    }
  }
}

static void updateFeedState() {
  uint32_t now = millis();

  switch (feedState) {
    case IDLE:
      digitalWrite(Pins::LED_LOW_FEED, LOW);
      break;

    case PRECHECK:
      if (inputs.modeSelect == 2) {
        if (lastFeedRemainingG + 1.0f < lastCmdGrams) {
          feedState = ABORT_LOW_FEED;
          stateStartMs = now;
          digitalWrite(Pins::LED_LOW_FEED, HIGH);
          strncat(lastEventLabel, "_ABORT", sizeof(lastEventLabel) - strlen(lastEventLabel) - 1);
          break;
        }
      }
      // Start spinner first so pellets near the outlet are already moving
      // before the valve opens, reducing jams at gate opening.
      motorForward(inputs.pwmPercent);
      feedState = SERVO_OPENING;
      stateStartMs = now;
      break;

    case SERVO_OPENING:
      if (now - stateStartMs >= Config::MOTOR_PRESPIN_MS) {
        valveServo.write(Config::SERVO_OPEN_DEG);
      }
      if (now - stateStartMs >= (Config::MOTOR_PRESPIN_MS + Config::SERVO_STABILIZE_MS)) {
        feedState = MOTOR_RUNNING;
        stateStartMs = now;
      }
      break;

    case MOTOR_RUNNING:
      if (now - stateStartMs >= motorRunMs) {
        motorStop();
        valveServo.write(Config::SERVO_CLOSE_DEG);
        feedState = SERVO_CLOSING;
        stateStartMs = now;
      }
      break;

    case SERVO_CLOSING:
      if (now - stateStartMs >= Config::SERVO_SETTLE_MS) {
        feedState = POSTLOG;
        stateStartMs = now;
      }
      break;

    case POSTLOG:
      feedState = IDLE;
      stateStartMs = now;
      break;

    case ABORT_LOW_FEED:
      if (now - stateStartMs >= 3000) {
        feedState = IDLE;
        stateStartMs = now;
      }
      break;
  }
}

static void handleSerialCommand(const char *line) {
  if (strncmp(line, "SET:", 4) == 0) {
    const char *kv = line + 4;
    if (strncmp(kv, "MODE=", 5) == 0) {
      inputs.modeSelect = atoi(kv + 5);
    } else if (strncmp(kv, "BIOMASS=", 8) == 0) {
      inputs.biomassG = atof(kv + 8);
    } else if (strncmp(kv, "PWM=", 4) == 0) {
      inputs.pwmPercent = atoi(kv + 4);
      if (inputs.pwmPercent < 0) inputs.pwmPercent = 0;
      if (inputs.pwmPercent > 100) inputs.pwmPercent = 100;
    } else if (strncmp(kv, "GPS100=", 7) == 0) {
      inputs.gramsPerSec100 = atoi(kv + 7);
      if (inputs.gramsPerSec100 < 1) inputs.gramsPerSec100 = 1;
    } else if (strncmp(kv, "SIM_MODE=", 9) == 0) {
      inputs.simMode = (atoi(kv + 9) == 1);
    } else if (strncmp(kv, "SIM_TEMP=", 9) == 0) {
      inputs.simTempC = atof(kv + 9);
    } else if (strncmp(kv, "SIM_DIST=", 9) == 0) {
      inputs.simDistanceCm = atof(kv + 9);
      simDistanceOverrideEnabled = true;
    }
  } else if (strncmp(line, "CMD:", 4) == 0) {
    const char *cmd = line + 4;
    if (strcmp(cmd, "MANUAL") == 0) {
      requests.manualFeed = true;
    } else if (strcmp(cmd, "SIM_EVT") == 0) {
      requests.simEvent = true;
    } else if (strcmp(cmd, "SCHED_07") == 0) {
      requests.schedMorning = true;
    } else if (strcmp(cmd, "SCHED_17") == 0) {
      requests.schedEvening = true;
    } else if (strcmp(cmd, "SCHED_08") == 0) {
      requests.schedModeD0800 = true;
    } else if (strcmp(cmd, "SCHED_1330") == 0) {
      requests.schedModeD1330 = true;
    } else if (strcmp(cmd, "SCHED_19") == 0) {
      requests.schedModeD1900 = true;
    }
  } else if (strncmp(line, "SET_TEMP:", 9) == 0) {
    inputs.simTempC = atof(line + 9);
    inputs.simMode = true;
  } else if (strncmp(line, "SET_BIOMASS:", 12) == 0) {
    inputs.biomassG = atof(line + 12);
  } else if (strncmp(line, "SET_DISTANCE:", 13) == 0) {
    inputs.simDistanceCm = atof(line + 13);
    simDistanceOverrideEnabled = true;
    inputs.simMode = true;
  } else if (strcmp(line, "SIM_MODE:ON") == 0) {
    inputs.simMode = true;
  } else if (strcmp(line, "SIM_MODE:OFF") == 0) {
    inputs.simMode = false;
    simDistanceOverrideEnabled = false;
  } else if (strcmp(line, "RUN_TEST_A") == 0) {
    inputs.modeSelect = 0;
  } else if (strcmp(line, "RUN_TEST_B") == 0) {
    inputs.modeSelect = 1;
  } else if (strcmp(line, "RUN_TEST_C") == 0) {
    inputs.modeSelect = 2;
  } else if (strcmp(line, "RUN_TEST_D") == 0) {
    inputs.modeSelect = 2;
  } else if (strcmp(line, "RUN_FEED") == 0) {
    requests.manualFeed = true;
  } else if (strcmp(line, "PING") == 0) {
    Serial.println("{\"ack\":\"PONG\"}");
  }
}

static void pollSerial() {
  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    if (c == '\n') {
      serialBuf[serialPos] = '\0';
      if (serialPos > 0) {
        handleSerialCommand(serialBuf);
      }
      serialPos = 0;
    } else if (c != '\r') {
      if (serialPos < sizeof(serialBuf) - 1) {
        serialBuf[serialPos++] = c;
      }
    }
  }
}

void setup() {
  Serial.begin(Config::SERIAL_BAUD);

  pinMode(Pins::MOTOR_L_PWM, OUTPUT);
  pinMode(Pins::MOTOR_R_PWM, OUTPUT);
  pinMode(Pins::MOTOR_L_EN, OUTPUT);
  pinMode(Pins::MOTOR_R_EN, OUTPUT);
  motorStop();

  pinMode(Pins::LED_STATUS, OUTPUT);
  pinMode(Pins::LED_LOW_FEED, OUTPUT);
  digitalWrite(Pins::LED_STATUS, LOW);
  digitalWrite(Pins::LED_LOW_FEED, LOW);

  pinMode(Pins::BTN_MANUAL, INPUT_PULLUP);

  valveServo.attach(Pins::SERVO);
  valveServo.write(Config::SERVO_CLOSE_DEG);

  Wire.begin();
  lcd.init();
  lcd.backlight();
  lcdSetLines("Booting...", "Please wait");

  dallas.begin();

  tofSensor.setTimeout(70);
  tofReady = tofSensor.init();
  if (tofReady) {
    tofSensor.setDistanceMode(VL53L1X::Long);
    tofSensor.setMeasurementTimingBudget(Config::TOF_TIMING_BUDGET_US);
    tofSensor.startContinuous(Config::TOF_PERIOD_MS);
  } else {
    lcdSetLines("VL53L1X FAIL", "Check wiring");
  }
}

void loop() {
  pollSerial();

  uint32_t now = millis();
  if (now - lastSampleMs >= Config::SAMPLE_INTERVAL_MS) {
    lastSampleMs = now;
    samplingTask();
  }

  handleButton();
  if (requests.manualFeed && feedState == IDLE) {
    requests.manualFeed = false;
    triggerDispenseEvent("MANUAL");
  }
  if (requests.simEvent && feedState == IDLE) {
    requests.simEvent = false;
    triggerDispenseEvent("SIM_EVT");
  }
  if (requests.schedMorning && feedState == IDLE) {
    requests.schedMorning = false;
    triggerDispenseEvent("SCHED_07");
  }
  if (requests.schedEvening && feedState == IDLE) {
    requests.schedEvening = false;
    triggerDispenseEvent("SCHED_17");
  }
  if (requests.schedModeD0800 && feedState == IDLE) {
    requests.schedModeD0800 = false;
    triggerDispenseEvent("SCHED_08");
  }
  if (requests.schedModeD1330 && feedState == IDLE) {
    requests.schedModeD1330 = false;
    triggerDispenseEvent("SCHED_1330");
  }
  if (requests.schedModeD1900 && feedState == IDLE) {
    requests.schedModeD1900 = false;
    triggerDispenseEvent("SCHED_19");
  }

  updateFeedState();
  digitalWrite(Pins::LED_STATUS, (feedState == IDLE) ? LOW : HIGH);
}

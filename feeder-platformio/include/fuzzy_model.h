#pragma once

#include <Arduino.h>
#include <math.h>

namespace fuzzy {

enum TempSet { TEMP_COLD = 0, TEMP_NORMAL = 1, TEMP_WARM = 2, TEMP_COUNT = 3 };
enum BiomassSet { BIO_SMALL = 0, BIO_MEDIUM = 1, BIO_LARGE = 2, BIO_COUNT = 3 };
enum FeedSet { FEED_LOW = 0, FEED_MEDIUM = 1, FEED_HIGH = 2, FEED_COUNT = 3 };

struct Decision {
  float tempMembership[TEMP_COUNT];
  float biomassMembership[BIO_COUNT];
  float outputActivation[FEED_COUNT];
  float outputGrams;
};

static inline float clampf(float value, float lo, float hi) {
  if (value < lo) return lo;
  if (value > hi) return hi;
  return value;
}

static inline float trimf(float x, float a, float b, float c) {
  if (x <= a || x >= c) return 0.0f;
  if (x == b) return 1.0f;
  if (x < b) return (b == a) ? 0.0f : (x - a) / (b - a);
  return (c == b) ? 0.0f : (c - x) / (c - b);
}

static inline float trapmf(float x, float a, float b, float c, float d) {
  if (x <= a || x >= d) return 0.0f;
  if (x >= b && x <= c) return 1.0f;
  if (x < b) return (b == a) ? 0.0f : (x - a) / (b - a);
  return (d == c) ? 0.0f : (d - x) / (d - c);
}

static inline float tempMembership(TempSet setId, float tempC) {
  switch (setId) {
    case TEMP_COLD: return trimf(tempC, 20.0f, 25.0f, 30.0f);
    case TEMP_NORMAL: return trimf(tempC, 26.0f, 30.0f, 34.0f);
    case TEMP_WARM: return trimf(tempC, 28.0f, 34.0f, 38.0f);
    default: return 0.0f;
  }
}

static inline float biomassMembership(BiomassSet setId, float biomassG) {
  switch (setId) {
    case BIO_SMALL: return trimf(biomassG, 0.0f, 1000.0f, 2000.0f);
    case BIO_MEDIUM: return trimf(biomassG, 1500.0f, 2500.0f, 3500.0f);
    case BIO_LARGE: return trimf(biomassG, 2000.0f, 4000.0f, 5500.0f);
    default: return 0.0f;
  }
}

static inline float outputMembership(FeedSet setId, float grams) {
  switch (setId) {
    case FEED_LOW: return trimf(grams, 0.0f, 30.0f, 60.0f);
    case FEED_MEDIUM: return trimf(grams, 40.0f, 70.0f, 100.0f);
    case FEED_HIGH: return trimf(grams, 80.0f, 110.0f, 150.0f);
    default: return 0.0f;
  }
}

static inline const char *tempLabel(TempSet setId) {
  switch (setId) {
    case TEMP_COLD: return "cold";
    case TEMP_NORMAL: return "normal";
    case TEMP_WARM: return "warm";
    default: return "unknown";
  }
}

static inline const char *biomassLabel(BiomassSet setId) {
  switch (setId) {
    case BIO_SMALL: return "small";
    case BIO_MEDIUM: return "medium";
    case BIO_LARGE: return "large";
    default: return "unknown";
  }
}

static inline const char *outputLabel(FeedSet setId) {
  switch (setId) {
    case FEED_LOW: return "low";
    case FEED_MEDIUM: return "medium";
    case FEED_HIGH: return "high";
    default: return "unknown";
  }
}

static inline Decision evaluate(float tempC, float biomassG) {
  Decision decision = {};
  for (int i = 0; i < TEMP_COUNT; ++i) {
    decision.tempMembership[i] = tempMembership((TempSet)i, tempC);
  }
  for (int i = 0; i < BIO_COUNT; ++i) {
    decision.biomassMembership[i] = biomassMembership((BiomassSet)i, biomassG);
  }

  struct Rule {
    TempSet tempSet;
    BiomassSet biomassSet;
    FeedSet outputSet;
  };

  static const Rule rules[] = {
      {TEMP_COLD, BIO_MEDIUM, FEED_LOW},
      {TEMP_NORMAL, BIO_SMALL, FEED_LOW},
      {TEMP_COLD, BIO_SMALL, FEED_LOW},
      {TEMP_COLD, BIO_LARGE, FEED_MEDIUM},
      {TEMP_NORMAL, BIO_MEDIUM, FEED_MEDIUM},
      {TEMP_NORMAL, BIO_LARGE, FEED_HIGH},
      {TEMP_WARM, BIO_SMALL, FEED_MEDIUM},
      {TEMP_WARM, BIO_MEDIUM, FEED_HIGH},
      {TEMP_WARM, BIO_LARGE, FEED_HIGH},
  };

  for (int i = 0; i < FEED_COUNT; ++i) {
    decision.outputActivation[i] = 0.0f;
  }

  for (size_t idx = 0; idx < sizeof(rules) / sizeof(rules[0]); ++idx) {
    const Rule &rule = rules[idx];
    float alpha = min(decision.tempMembership[rule.tempSet], decision.biomassMembership[rule.biomassSet]);
    decision.outputActivation[rule.outputSet] = max(decision.outputActivation[rule.outputSet], alpha);
  }

  float numerator = 0.0f;
  float denominator = 0.0f;
  for (int step = 0; step <= 150; ++step) {
    float grams = (float)step;
    float aggregated = 0.0f;
    for (int setId = 0; setId < FEED_COUNT; ++setId) {
      float clipped = min(decision.outputActivation[setId], outputMembership((FeedSet)setId, grams));
      aggregated = max(aggregated, clipped);
    }
    numerator += grams * aggregated;
    denominator += aggregated;
  }
  decision.outputGrams = (denominator > 0.0f) ? (numerator / denominator) : 0.0f;
  return decision;
}

}  // namespace fuzzy

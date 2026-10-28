#pragma once
#include <cmath>
#include <cstdint>
#include "esp_attr.h"

// ---------- Values that survive deep sleep ----------
RTC_DATA_ATTR int wake_count = 0;
RTC_DATA_ATTR float last_voc = NAN;

// ---------- Trend history (ring buffer) ----------
// One sample is stored each time the screen is redrawn (~every 5 min),
// so 48 samples is roughly 4 hours.
constexpr int HIST_LEN = 48;
constexpr int16_t HIST_NONE = INT16_MIN;

RTC_DATA_ATTR int16_t temp_hist[HIST_LEN] = {0};  // temperature x10
RTC_DATA_ATTR int16_t hum_hist[HIST_LEN] = {0};   // humidity x10
RTC_DATA_ATTR int16_t voc_hist[HIST_LEN] = {0};   // VOC index
RTC_DATA_ATTR int hist_head = 0;                  // next slot to write
RTC_DATA_ATTR int hist_count = 0;                 // how many slots are valid

// RTC memory survives software resets (e.g. OTA), so if this file's layout changes the
// old bytes can be misread. The magic number catches that and starts clean. Bump it
// whenever you change the RTC variables above.
constexpr uint32_t RTC_MAGIC = 0xA1E00003;
RTC_DATA_ATTR uint32_t rtc_magic = 0;

inline void rtc_check() {
  if (rtc_magic == RTC_MAGIC && hist_head >= 0 && hist_head < HIST_LEN && hist_count >= 0 &&
      hist_count <= HIST_LEN)
    return;
  for (int i = 0; i < HIST_LEN; i++) temp_hist[i] = hum_hist[i] = voc_hist[i] = 0;
  hist_head = 0;
  hist_count = 0;
  wake_count = 0;
  last_voc = NAN;
  rtc_magic = RTC_MAGIC;
}

inline void hist_push(float t, float h, float v) {
  temp_hist[hist_head] = std::isnan(t) ? HIST_NONE : (int16_t) lroundf(t * 10.0f);
  hum_hist[hist_head] = std::isnan(h) ? HIST_NONE : (int16_t) lroundf(h * 10.0f);
  voc_hist[hist_head] = std::isnan(v) ? HIST_NONE : (int16_t) lroundf(v);
  hist_head = (hist_head + 1) % HIST_LEN;
  if (hist_count < HIST_LEN) hist_count++;
}

// ---------- Trend arrows ----------
constexpr int TREND_LOOKBACK = 6;    // compare with the sample this many redraws ago (~30 min)
constexpr int TREND_NONE = 2;        // not enough data yet
constexpr bool SHOW_STEADY = false;  // true = also draw a sideways arrow when nothing is changing

// +1 rising, -1 falling, 0 steady, TREND_NONE if there isn't enough history.
// threshold is in real units (scale converts the stored ints back to real units).
inline int hist_trend(const int16_t *data, float threshold, float scale) {
  if (hist_count <= TREND_LOOKBACK) return TREND_NONE;
  int16_t now_raw = data[(hist_head - 1 + HIST_LEN) % HIST_LEN];
  int16_t old_raw = data[(hist_head - 1 - TREND_LOOKBACK + 2 * HIST_LEN) % HIST_LEN];
  if (now_raw == HIST_NONE || old_raw == HIST_NONE) return TREND_NONE;
  float d = (now_raw - old_raw) / scale;
  if (d >= threshold) return 1;
  if (d <= -threshold) return -1;
  return 0;
}

// ---------- Drawing helpers ----------
// Colours are left at the display default (black on white).

// Small bold arrow centred on (cx, cy), about 13px tall.
template<typename D>
void draw_arrow(D &it, int cx, int cy, int dir) {
  if (dir == TREND_NONE) return;
  if (dir == 0 && !SHOW_STEADY) return;
  for (int o = 0; o < 2; o++) {  // drawn twice, 1px apart, for a bolder stroke
    if (dir > 0) {  // up
      it.line(cx + o, cy + 6, cx + o, cy - 6);
      it.line(cx + o, cy - 6, cx - 4 + o, cy - 2);
      it.line(cx + o, cy - 6, cx + 4 + o, cy - 2);
    } else if (dir < 0) {  // down
      it.line(cx + o, cy - 6, cx + o, cy + 6);
      it.line(cx + o, cy + 6, cx - 4 + o, cy + 2);
      it.line(cx + o, cy + 6, cx + 4 + o, cy + 2);
    } else {  // steady (sideways)
      it.line(cx - 6, cy + o, cx + 6, cy + o);
      it.line(cx + 6, cy + o, cx + 2, cy - 4 + o);
      it.line(cx + 6, cy + o, cx + 2, cy + 4 + o);
    }
  }
}

// Sparkline: 2px per sample, so it is HIST_LEN*2 pixels wide.
// Returns false if there is no valid data; otherwise out_min/out_max hold the real data range.
template<typename D>
bool draw_spark(D &it, const int16_t *data, int x0, int y0, int h, float min_span, float scale,
                float &out_min, float &out_max) {
  it.horizontal_line(x0, y0 + h + 1, HIST_LEN * 2);  // baseline
  if (hist_count == 0) return false;

  int oldest = (hist_head - hist_count + HIST_LEN) % HIST_LEN;
  float mn = 1e9f, mx = -1e9f;
  bool any = false;
  for (int i = 0; i < hist_count; i++) {
    int16_t raw = data[(oldest + i) % HIST_LEN];
    if (raw == HIST_NONE) continue;
    float f = raw / scale;
    if (f < mn) mn = f;
    if (f > mx) mx = f;
    any = true;
  }
  if (!any) return false;
  out_min = mn;
  out_max = mx;

  // Keep a flat trace from looking like wild swings
  float span = mx - mn;
  if (span < min_span) {
    float mid = (mx + mn) / 2.0f;
    mn = mid - min_span / 2.0f;
    span = min_span;
  }

  int px = -1, py = -1;
  for (int i = 0; i < hist_count; i++) {
    int16_t raw = data[(oldest + i) % HIST_LEN];
    if (raw == HIST_NONE) {
      px = -1;
      continue;
    }
    float f = raw / scale;
    int x = x0 + 2 * (HIST_LEN - hist_count + i);  // newest sample sits at the right edge
    int y = y0 + h - (int) lroundf((f - mn) / span * h);
    if (px >= 0) {
      it.line(px, py, x, y);
      it.line(px, py + 1, x, y + 1);  // second line = bolder trace on e-ink
    }
    px = x;
    py = y;
  }
  if (px >= 0) it.filled_circle(px, py, 2);  // dot on the latest reading
  return true;
}

// Four signal bars, bottom-aligned at y_base. Each is 4px wide, 5px apart.
template<typename D>
void draw_wifi(D &it, int x, int y_base, float dbm) {
  int bars = std::isnan(dbm) ? 0 : (dbm > -55 ? 4 : (dbm > -65 ? 3 : (dbm > -75 ? 2 : 1)));
  for (int i = 0; i < 4; i++) {
    int bh = 3 + i * 3;
    int bx = x + i * 5;
    if (i < bars)
      it.filled_rectangle(bx, y_base - bh, 4, bh);
    else
      it.rectangle(bx, y_base - bh, 4, bh);
  }
}

// Battery outline (31x13 incl. nub). pct NaN = empty outline (unknown level).
template<typename D>
void draw_battery(D &it, int x, int y, float pct) {
  it.rectangle(x, y, 28, 13);
  it.filled_rectangle(x + 28, y + 4, 3, 5);
  if (!std::isnan(pct)) {
    float p = pct < 0 ? 0 : (pct > 100 ? 100 : pct);
    int w = (int) (24.0f * p / 100.0f);
    if (w > 0) it.filled_rectangle(x + 2, y + 2, w, 9);
  }
}

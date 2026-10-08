// WiFi presence detection - "is anyone unusual nearby", including through walls.
//
// Promiscuous-mode capture of 802.11 management frames sent by phones that are
// not connected to anything of ours. Probe requests are the main source: a
// phone with WiFi on keeps asking for networks it knows.
//
// ------------------------------------------------------------------------
// HOW A WINDOW BECOMES A WARNING
// ------------------------------------------------------------------------
//
// Every SNIFF_PERIOD_MS the radio listens for SNIFF_WINDOW_MS, hopping the
// non-overlapping channels, and counts the distinct devices it heard ABOVE
// SNIFF_MIN_RSSI. The count is compared with what is normal for the place:
//
//   learning   the first PRESENCE_LEARN_WINDOWS windows only measure, and
//              their average becomes the usual level ("ambient").
//   watching   excess = count - ambient. An excess of PRESENCE_WARN_EXCESS
//              or more, for PRESENCE_CONFIRM_WINDOWS windows IN A ROW, raises
//              the early warning. One quiet window clears it.
//   following  quiet windows nudge the ambient level, so a slowly changing
//              background is absorbed; a warning freezes it, so an actual
//              crowd is never learned as normal.
//
// The first version counted every MAC seen in the last five minutes down to
// -80 dBm against a fixed threshold of 3. In a building that is always
// exceeded - neighbours, passers-by, the owner's own phone - so it warned the
// moment it started and never cleared.
//
// ------------------------------------------------------------------------
// LIMITS THAT NO TUNING REMOVES
// ------------------------------------------------------------------------
//
// * The ESP8266 cannot sniff and stay connected at the same time. Each window
//   drops the WiFi link; MQTT reconnects afterwards.
// * Phones RANDOMIZE their MAC in probe requests (iOS 8+, Android 10+), so a
//   count is an ACTIVITY LEVEL, not a headcount. Randomized and stable
//   addresses are reported separately for that reason.
// * RSSI is signal strength, not distance; walls, bodies and orientation all
//   move it. SNIFF_MIN_RSSI is a crude radius, not a number of metres.
// * This captures identifiers broadcast in the clear. Fine on your own bench;
//   think before aiming it at a public room.
#pragma once

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include "config.h"

extern "C" {
  #include "user_interface.h"
}

// --- the SDK's promiscuous-mode callback payload -------------------------
//
// Layouts from the ESP8266 NONOS SDK. The callback hands us different shapes
// depending on len: 12 = RxControl only, 128 = a management frame with its
// first 112 bytes, anything else = data-frame headers we do not use. Only the
// len == 128 case carries probe requests.

struct SnifferRxControl {
  signed   rssi: 8;
  unsigned rate: 4;
  unsigned is_group: 1;
  unsigned: 1;
  unsigned sig_mode: 2;
  unsigned legacy_length: 12;
  unsigned damatch0: 1;
  unsigned damatch1: 1;
  unsigned bssidmatch0: 1;
  unsigned bssidmatch1: 1;
  unsigned MCS: 7;
  unsigned CWB: 1;
  unsigned HT_length: 16;
  unsigned Smoothing: 1;
  unsigned Not_Sounding: 1;
  unsigned: 1;
  unsigned Aggregation: 1;
  unsigned STBC: 2;
  unsigned FEC_CODING: 1;
  unsigned SGI: 1;
  unsigned rxend_state: 8;
  unsigned ampdu_cnt: 8;
  unsigned channel: 4;
  unsigned: 12;
};
struct SnifferMgmtFrame {
  SnifferRxControl rx_ctrl;
  uint8_t          buf[112];   // first 112 bytes of the 802.11 management frame
  uint16_t         cnt;
  uint16_t         len;
};

// 802.11 management header offsets within buf[].
static const uint8_t WIFI_FC_OFFSET    = 0;    // frame control
static const uint8_t WIFI_ADDR2_OFFSET = 10;   // source address - the device

// Frame control byte 0 = subtype<<4 | type<<2 | version. These management
// subtypes are all sent by a client device, never by an access point.
static const uint8_t FC_PROBE_REQUEST    = 0x40;
static const uint8_t FC_ASSOC_REQUEST    = 0x00;
static const uint8_t FC_REASSOC_REQUEST  = 0x20;
static const uint8_t FC_AUTHENTICATION   = 0xB0;

struct PresenceEntry {
  uint8_t mac[6];
  int8_t  rssi;
  bool    used;
};

struct PresenceStats {
  uint8_t       count;          // distinct nearby devices in the LAST window
  uint8_t       randomized;     // ...of which carried a randomized address
  uint8_t       stable;         // ...of which look like a real burned-in MAC
  int8_t        strongestRssi;  // 0 when nothing was heard
  float         ambient;        // the usual count for this place
  float         excess;         // count - ambient
  uint8_t       windows;        // windows completed since (re)learning began
  uint8_t       streak;         // consecutive windows over the threshold
  bool          learning;
  bool          warn;           // the early warning, as decided by the last window
  uint32_t      frames;         // frames counted in the last window
  unsigned long lastSniffMs;
};

// File-scope because the SDK callback takes no context pointer. It is written
// from that callback, so writes there stay short.
static PresenceEntry presenceTable[PRESENCE_TABLE_SIZE];
static volatile uint32_t presenceFrames = 0;
static PresenceStats presenceStatsNow = {};
static bool presenceSniffing = false;

// A locally-administered address (bit 1 of the first octet) is almost always a
// privacy-randomized one rather than a manufacturer-assigned MAC.
inline bool macIsRandomized(const uint8_t *mac) {
  return (mac[0] & 0x02) != 0;
}

// Called from the promiscuous callback: no printing, no allocation. When the
// table is full, further new devices are simply not recorded this window -
// 40 distinct nearby devices is already far past any warning threshold.
inline void presenceRecord(const uint8_t *mac, int8_t rssi) {
  int freeSlot = -1;
  for (int i = 0; i < PRESENCE_TABLE_SIZE; i++) {
    if (!presenceTable[i].used) {
      if (freeSlot < 0) freeSlot = i;
      continue;
    }
    if (memcmp(presenceTable[i].mac, mac, 6) == 0) {
      if (rssi > presenceTable[i].rssi) presenceTable[i].rssi = rssi;
      return;
    }
  }
  if (freeSlot < 0) return;
  memcpy(presenceTable[freeSlot].mac, mac, 6);
  presenceTable[freeSlot].rssi = rssi;
  presenceTable[freeSlot].used = true;
}

// Runs in SDK context. Keep it cheap.
inline void presenceSnifferCallback(uint8_t *buf, uint16_t len) {
  if (len != 128) return;     // not a management frame with a usable body

  const SnifferMgmtFrame *frame = (const SnifferMgmtFrame *)buf;
  const uint8_t fc = frame->buf[WIFI_FC_OFFSET];
  const bool fromClient = fc == FC_PROBE_REQUEST || fc == FC_ASSOC_REQUEST
                       || fc == FC_REASSOC_REQUEST || fc == FC_AUTHENTICATION;
  if (!fromClient) return;

  const int8_t rssi = frame->rx_ctrl.rssi;
  if (rssi < SNIFF_MIN_RSSI) return;      // too far away to be in our room

  presenceFrames++;
  presenceRecord(frame->buf + WIFI_ADDR2_OFFSET, rssi);
}

// Turn the table filled during one window into the stats and the warning.
inline void presenceFinishWindow() {
  PresenceStats &s = presenceStatsNow;
  s.count = s.randomized = s.stable = 0;
  s.strongestRssi = 0;
  for (int i = 0; i < PRESENCE_TABLE_SIZE; i++) {
    if (!presenceTable[i].used) continue;
    s.count++;
    if (macIsRandomized(presenceTable[i].mac)) s.randomized++;
    else s.stable++;
    if (s.strongestRssi == 0 || presenceTable[i].rssi > s.strongestRssi) {
      s.strongestRssi = presenceTable[i].rssi;
    }
  }
  s.frames = presenceFrames;
  s.lastSniffMs = millis();

  if (s.windows < 255) s.windows++;

  if (s.windows <= PRESENCE_LEARN_WINDOWS) {
    // Running mean of the learning windows becomes the usual level.
    s.ambient += ((float)s.count - s.ambient) / (float)s.windows;
    s.learning = true;
    s.excess = 0;
    s.streak = 0;
    s.warn = false;
    return;
  }

  s.learning = false;
  s.excess = (float)s.count - s.ambient;
  s.streak = s.excess >= PRESENCE_WARN_EXCESS ? (uint8_t)(s.streak + 1) : 0;
  s.warn = s.streak >= PRESENCE_CONFIRM_WINDOWS;

  // Only quiet windows teach the ambient level, so a real crowd is never
  // absorbed into "normal" while it is there.
  if (!s.warn && s.excess < PRESENCE_WARN_EXCESS) {
    s.ambient += ((float)s.count - s.ambient) * PRESENCE_AMBIENT_FOLLOW;
  }
}

// Forget the usual level and learn it again from scratch.
inline void presenceRelearn() {
  PresenceStats &s = presenceStatsNow;
  s.windows = 0;
  s.ambient = 0;
  s.excess = 0;
  s.streak = 0;
  s.warn = false;
  s.learning = true;
}

// --- the sniff window ----------------------------------------------------

// Blocking by design: the link is down for the whole window, so there is
// nothing useful to interleave. rebuildNetwork() is supplied by the caller so
// this header knows nothing about credentials or MQTT.
inline void presenceRunSniffWindow(void (*rebuildNetwork)()) {
  static const uint8_t channels[] = SNIFF_CHANNELS;
  static const uint8_t channelCount = sizeof(channels) / sizeof(channels[0]);

  presenceSniffing = true;
  memset(presenceTable, 0, sizeof(presenceTable));
  presenceFrames = 0;

  // Promiscuous mode only works in station opmode with nothing connected.
  WiFi.disconnect(false);
  WiFi.mode(WIFI_STA);
  delay(10);

  wifi_promiscuous_enable(0);
  wifi_set_promiscuous_rx_cb(presenceSnifferCallback);
  wifi_promiscuous_enable(1);

  // Hop the non-overlapping channels so a phone on 11 is not missed because
  // we happened to be listening on 1.
  const unsigned long deadline = millis() + SNIFF_WINDOW_MS;
  uint8_t index = 0;
  while ((long)(millis() - deadline) < 0) {
    wifi_set_channel(channels[index]);
    index = (index + 1) % channelCount;
    const unsigned long remaining = deadline - millis();
    delay(remaining < SNIFF_DWELL_MS ? remaining : SNIFF_DWELL_MS);   // yields to the SDK
  }

  wifi_promiscuous_enable(0);
  wifi_set_promiscuous_rx_cb(nullptr);

  presenceFinishWindow();
  presenceSniffing = false;
  rebuildNetwork();
}

inline bool presenceSniffDue() {
  return millis() - presenceStatsNow.lastSniffMs >= SNIFF_PERIOD_MS;
}

// Push the next window a full period away without sniffing. Used while the
// dashboard is driving the outputs by hand or running the self-test, where a
// window would cut the board off from the dashboard mid-action.
inline void presenceDeferSniff() {
  presenceStatsNow.lastSniffMs = millis();
}

inline bool presenceIsSniffing() {
  return presenceSniffing;
}

inline const PresenceStats &presenceStats() {
  return presenceStatsNow;
}

inline void presenceReset() {
  memset(presenceTable, 0, sizeof(presenceTable));
  presenceFrames = 0;
  presenceStatsNow = {};
  presenceStatsNow.learning = true;
  presenceStatsNow.lastSniffMs = millis();
}

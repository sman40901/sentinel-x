// WiFi device detection — "who is nearby", including through walls.
//
// Two independent signals, deliberately kept separate because they mean
// different things:
//
//   ASSOCIATED  devices joined to our own AP. Exact, continuous, free.
//   SNIFFED     devices that are NOT talking to us at all. Captured from
//               802.11 probe requests in promiscuous mode. This is the signal
//               that sees a phone in the next room.
//
// ------------------------------------------------------------------------
// THREE THINGS THAT MAKE THIS HARDER THAN IT LOOKS
// ------------------------------------------------------------------------
//
// 1. The ESP8266 cannot sniff and host an AP simultaneously. Promiscuous mode
//    requires station-only opmode, so opening a sniff window tears the AP
//    down: dashboard clients get kicked and the MQTT socket dies. We therefore
//    sniff in short scheduled bursts and rebuild the network afterwards.
//    SNIFF_WINDOW_MS / SNIFF_PERIOD_MS in config.h control the duty cycle.
//
// 2. Phones randomize their MAC in probe requests (iOS 8+, Android 10+). A
//    single phone can emit a dozen different addresses in a minute, so a
//    unique-MAC count is an ACTIVITY LEVEL and not a headcount. We split the
//    tally into randomized vs stable (the locally-administered bit tells them
//    apart) so the number can be read honestly.
//
// 3. Radios do not measure distance. RSSI correlates with it loosely and is
//    wrecked by walls, bodies and orientation. SNIFF_MIN_RSSI is a crude
//    radius control, not a metre count.
//
// This captures identifiers broadcast in the clear. That is fine for your own
// bench; think before aiming it at a public room.
#pragma once

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include "config.h"

extern "C" {
  #include "user_interface.h"
}

// --- the SDK's promiscuous-mode callback payload -------------------------
//
// These layouts come from the ESP8266 NONOS SDK. The callback hands us
// different shapes depending on len:
//
//   len == 12   RxControl only — a data frame whose body we are not given
//   len == 128  a management frame: RxControl + the first 112 bytes of it
//   otherwise   data-frame headers, which we do not use
//
// Only the len == 128 case carries probe requests, which is all we want.

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

// 802.11 management frame header offsets within buf[].
static const uint8_t WIFI_FC_OFFSET    = 0;    // frame control
static const uint8_t WIFI_ADDR2_OFFSET = 10;   // source address — the device itself

// Frame control byte 0 = subtype<<4 | type<<2 | version.
// Management type (0) with these subtypes are all client-originated.
static const uint8_t FC_PROBE_REQUEST    = 0x40;
static const uint8_t FC_ASSOC_REQUEST    = 0x00;
static const uint8_t FC_REASSOC_REQUEST  = 0x20;
static const uint8_t FC_AUTHENTICATION   = 0xB0;

struct PresenceEntry {
  uint8_t       mac[6];
  int8_t        rssi;
  unsigned long lastSeen;
  bool          randomized;
};

struct PresenceStats {
  uint8_t  associated;      // devices joined to our AP right now
  uint8_t  sniffedTotal;    // unique MACs in the table, not expired
  uint8_t  sniffedRandom;   // ...of which carry a randomized address
  uint8_t  sniffedStable;   // ...of which look like a real burned-in MAC
  int8_t   strongestRssi;   // closest thing we have to "how near"
  uint16_t score;           // the single number the LED logic consumes
  uint32_t framesSeen;      // raw frame counter, for sanity-checking the wiring
  unsigned long lastSniffMs;
};

// The table is file-scope because the SDK callback cannot take a context
// pointer, and is touched from that callback, so writes must be brief.
static PresenceEntry presenceTable[PRESENCE_TABLE_SIZE];
static volatile uint32_t presenceFrames = 0;
static unsigned long presenceLastSniff = 0;
static bool presenceSniffing = false;

// A locally-administered address (bit 1 of the first octet) is almost always a
// privacy-randomized one rather than a real manufacturer-assigned MAC.
inline bool macIsRandomized(const uint8_t *mac) {
  return (mac[0] & 0x02) != 0;
}

inline bool macEquals(const uint8_t *a, const uint8_t *b) {
  return memcmp(a, b, 6) == 0;
}

// Insert or refresh. Called from the promiscuous callback, so it stays short:
// no printing, no allocation. When the table is full the oldest entry is
// recycled, which biases toward recent activity — the thing we care about.
inline void presenceRecord(const uint8_t *mac, int8_t rssi) {
  const unsigned long now = millis();

  int oldestIndex = 0;
  unsigned long oldestSeen = ~0UL;

  for (int i = 0; i < PRESENCE_TABLE_SIZE; i++) {
    if (presenceTable[i].lastSeen != 0 && macEquals(presenceTable[i].mac, mac)) {
      presenceTable[i].lastSeen = now;
      if (rssi > presenceTable[i].rssi) {
        presenceTable[i].rssi = rssi;
      }
      return;
    }
    // A never-used slot wins outright; otherwise track the stalest.
    if (presenceTable[i].lastSeen == 0) {
      oldestIndex = i;
      oldestSeen = 0;
    } else if (presenceTable[i].lastSeen < oldestSeen) {
      oldestIndex = i;
      oldestSeen = presenceTable[i].lastSeen;
    }
  }

  memcpy(presenceTable[oldestIndex].mac, mac, 6);
  presenceTable[oldestIndex].rssi       = rssi;
  presenceTable[oldestIndex].lastSeen   = now;
  presenceTable[oldestIndex].randomized = macIsRandomized(mac);
}

// Runs in SDK context. Keep it cheap.
inline void presenceSnifferCallback(uint8_t *buf, uint16_t len) {
  if (len != 128) {
    return;   // not a management frame; no usable body was handed to us
  }

  const SnifferMgmtFrame *frame = (const SnifferMgmtFrame *)buf;
  const uint8_t fc = frame->buf[WIFI_FC_OFFSET];

  const bool fromClient = fc == FC_PROBE_REQUEST
                       || fc == FC_ASSOC_REQUEST
                       || fc == FC_REASSOC_REQUEST
                       || fc == FC_AUTHENTICATION;
  if (!fromClient) {
    return;
  }

  const int8_t rssi = frame->rx_ctrl.rssi;
  if (rssi < SNIFF_MIN_RSSI) {
    return;   // too far away to be in our room
  }

  presenceFrames++;
  presenceRecord(frame->buf + WIFI_ADDR2_OFFSET, rssi);
}

// --- the sniff window ----------------------------------------------------

// Blocking by design. The AP is down for the whole call, so there is nothing
// useful to interleave; keeping it synchronous makes the teardown/rebuild
// ordering obvious instead of spreading it across loop() states.
//
// rebuildNetwork() is supplied by the caller so this header does not need to
// know about secrets, the web server or MQTT.
inline void presenceRunSniffWindow(void (*rebuildNetwork)()) {
  static const uint8_t channels[] = SNIFF_CHANNELS;
  static const uint8_t channelCount = sizeof(channels) / sizeof(channels[0]);

  presenceSniffing = true;

  // Tear down. Promiscuous mode only works in station opmode, and only with
  // the station disconnected, so the AP has to go.
  WiFi.softAPdisconnect(false);
  WiFi.disconnect(false);
  WiFi.mode(WIFI_STA);
  delay(10);

  wifi_promiscuous_enable(0);
  wifi_set_promiscuous_rx_cb(presenceSnifferCallback);
  wifi_promiscuous_enable(1);

  // Hop the non-overlapping channels so a phone parked on 11 is not invisible
  // just because we happened to be listening on 1.
  const unsigned long deadline = millis() + SNIFF_WINDOW_MS;
  uint8_t index = 0;
  while (millis() < deadline) {
    wifi_set_channel(channels[index]);
    index = (index + 1) % channelCount;

    // delay() yields to the SDK, which is what actually lets frames arrive.
    const unsigned long remaining = deadline - millis();
    delay(remaining < SNIFF_DWELL_MS ? remaining : SNIFF_DWELL_MS);
  }

  wifi_promiscuous_enable(0);
  wifi_set_promiscuous_rx_cb(nullptr);

  presenceLastSniff = millis();
  presenceSniffing = false;

  rebuildNetwork();
}

inline bool presenceSniffDue() {
  return millis() - presenceLastSniff >= SNIFF_PERIOD_MS;
}

inline bool presenceIsSniffing() {
  return presenceSniffing;
}

// --- scoring -------------------------------------------------------------

inline PresenceStats presenceCollect() {
  PresenceStats stats = {};
  const unsigned long now = millis();

  for (int i = 0; i < PRESENCE_TABLE_SIZE; i++) {
    if (presenceTable[i].lastSeen == 0) {
      continue;
    }
    if (now - presenceTable[i].lastSeen > PRESENCE_EXPIRE_MS) {
      presenceTable[i].lastSeen = 0;      // reclaim the slot
      continue;
    }

    stats.sniffedTotal++;
    if (presenceTable[i].randomized) {
      stats.sniffedRandom++;
    } else {
      stats.sniffedStable++;
    }
    if (stats.strongestRssi == 0 || presenceTable[i].rssi > stats.strongestRssi) {
      stats.strongestRssi = presenceTable[i].rssi;
    }
  }

  stats.associated  = WiFi.softAPgetStationNum();
  stats.framesSeen  = presenceFrames;
  stats.lastSniffMs = presenceLastSniff;

  // Associated devices weigh more because each one is definitely a real,
  // distinct device. Sniffed MACs are inflated by randomization, so they
  // contribute less per hit.
  stats.score = (uint16_t)stats.associated * PRESENCE_ASSOC_WEIGHT
              + (uint16_t)stats.sniffedTotal * PRESENCE_SNIFF_WEIGHT;

  return stats;
}

inline void presenceReset() {
  memset(presenceTable, 0, sizeof(presenceTable));
  presenceFrames = 0;
}

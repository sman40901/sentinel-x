// Sentinel-X standalone radio diagnostic for the NodeMCU LoLin V3
// (ESP8266MOD / ESP-12F). USB/Serial only, 115200 baud.
// No sensor wiring, no credentials, no pairing, no network connection.
//
// ------------------------------------------------------------------------
// PORTED FROM ESP32 — AND IT LOST A FEATURE
// ------------------------------------------------------------------------
// The ESP32 version of this sketch alternated WiFi scans with BLE scans.
// THE ESP8266 HAS NO BLUETOOTH AT ALL — not BLE, not Classic. There is no
// library or workaround; the radio hardware simply isn't there. The BLE half
// has therefore been replaced with something this chip *can* do and that the
// main firmware actually relies on:
//
//   PASS 1  active scan for nearby 2.4 GHz access points and phone hotspots
//   PASS 2  promiscuous-mode capture of 802.11 probe requests, which is how
//           the main firmware detects phones that never connect to us
//
// Use this to prove the presence sniffer works before trusting it in the
// firmware. If pass 2 reports zero frames with phones in the room, the
// sniffer will not work in the firmware either.
//
// ------------------------------------------------------------------------
// HOW TO READ THE OUTPUT HONESTLY
// ------------------------------------------------------------------------
// * Phones randomize their MAC in probe requests (iOS 8+, Android 10+). One
//   phone can emit many addresses in a minute, so the unique count is an
//   ACTIVITY LEVEL, not a headcount. Addresses with the locally-administered
//   bit set are flagged "rnd" below.
// * RSSI is signal strength, not distance. Walls, bodies and orientation
//   wreck the correlation.
// * Probe requests are only emitted when a device is actively looking for
//   networks. A phone with a screen off and a known WiFi already joined may
//   stay silent for minutes. Absence of frames is not absence of people.
// * Neither pass counts people, and neither sees every nearby device.
//
// This is a passive listener: it never connects, pairs, deauthenticates, or
// sends anything. Observations are printed locally and discarded. SSIDs come
// off the air untrusted and are sanitized before printing.

#include <Arduino.h>
#include <ESP8266WiFi.h>

extern "C" {
  #include "user_interface.h"
}

// Non-overlapping 2.4 GHz channels, plus dwell time per channel.
const uint8_t CHANNELS[] = {1, 6, 11};
const unsigned long DWELL_MS = 700;
const unsigned long SNIFF_WINDOW_MS = 6000;
const int MIN_RSSI = -90;          // print everything; the firmware is stricter

const uint8_t TABLE_SIZE = 40;

struct SeenDevice {
  uint8_t mac[6];
  int8_t  rssi;
  bool    randomized;
};

SeenDevice seen[TABLE_SIZE];
uint8_t seenCount = 0;
uint32_t frameCount = 0;

// --- promiscuous-mode plumbing -------------------------------------------
//
// Layouts from the ESP8266 NONOS SDK. The callback hands us different shapes
// depending on len: 12 = RxControl only, 128 = a management frame plus its
// first 112 bytes, anything else = data-frame headers we don't use.

struct RxControl {
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

struct MgmtFrame {
  RxControl rx_ctrl;
  uint8_t   buf[112];
  uint16_t  cnt;
  uint16_t  len;
};

const uint8_t FC_PROBE_REQUEST = 0x40;   // subtype 4, type 0 (management)
const uint8_t ADDR2_OFFSET     = 10;     // source address in the 802.11 header

bool macIsRandomized(const uint8_t *mac) {
  return (mac[0] & 0x02) != 0;
}

// Runs in SDK context, so it stays short: no printing, no allocation.
void snifferCallback(uint8_t *buf, uint16_t len) {
  if (len != 128) {
    return;
  }
  const MgmtFrame *frame = (const MgmtFrame *)buf;
  if (frame->buf[0] != FC_PROBE_REQUEST) {
    return;
  }
  const int8_t rssi = frame->rx_ctrl.rssi;
  if (rssi < MIN_RSSI) {
    return;
  }

  frameCount++;
  const uint8_t *mac = frame->buf + ADDR2_OFFSET;

  for (uint8_t i = 0; i < seenCount; i++) {
    if (memcmp(seen[i].mac, mac, 6) == 0) {
      if (rssi > seen[i].rssi) {
        seen[i].rssi = rssi;
      }
      return;
    }
  }
  if (seenCount >= TABLE_SIZE) {
    return;
  }
  memcpy(seen[seenCount].mac, mac, 6);
  seen[seenCount].rssi = rssi;
  seen[seenCount].randomized = macIsRandomized(mac);
  seenCount++;
}

// SSIDs arrive from untrusted radio input; keep terminal control characters
// out of the serial output.
String printableLabel(const String &input) {
  String out;
  out.reserve(40);
  for (size_t i = 0; i < input.length() && i < 40; ++i) {
    const uint8_t c = (uint8_t)input[i];
    out += (c >= 32 && c <= 126) ? (char)c : '?';
  }
  return out.length() ? out : String("<hidden>");
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println();
  Serial.println(F("=== Sentinel-X radio diagnostic (ESP8266) ==="));
  Serial.println(F("Passive only. Never connects, pairs or transmits."));
  Serial.println(F("NOTE: this chip has no Bluetooth. WiFi only."));
  Serial.println(F("Neither pass counts people or sees every nearby device."));

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);
}

void scanAccessPoints() {
  Serial.println(F("\n--- Pass 1: 2.4 GHz access points and hotspots ---"));
  const int count = WiFi.scanNetworks(false, true);   // async=false, show_hidden=true
  if (count <= 0) {
    Serial.printf("No access points found (result %d)\n", count);
  } else {
    Serial.printf("%d access point(s):\n", count);
    for (int i = 0; i < count; i++) {
      Serial.printf("  AP | %s | ch %2d | %4d dBm | %s\n",
                    WiFi.BSSIDstr(i).c_str(), WiFi.channel(i), WiFi.RSSI(i),
                    printableLabel(WiFi.SSID(i)).c_str());
    }
  }
  WiFi.scanDelete();
}

void sniffProbeRequests() {
  Serial.printf("\n--- Pass 2: probe requests, %lu ms across channels 1/6/11 ---\n",
                SNIFF_WINDOW_MS);

  seenCount = 0;
  frameCount = 0;

  // Promiscuous mode needs station-only opmode with nothing connected.
  WiFi.disconnect();
  wifi_promiscuous_enable(0);
  wifi_set_promiscuous_rx_cb(snifferCallback);
  wifi_promiscuous_enable(1);

  const unsigned long deadline = millis() + SNIFF_WINDOW_MS;
  uint8_t index = 0;
  while (millis() < deadline) {
    wifi_set_channel(CHANNELS[index]);
    index = (index + 1) % (sizeof(CHANNELS) / sizeof(CHANNELS[0]));
    const unsigned long left = deadline - millis();
    delay(left < DWELL_MS ? left : DWELL_MS);   // delay() yields to the SDK
  }

  wifi_promiscuous_enable(0);
  wifi_set_promiscuous_rx_cb(NULL);

  uint8_t randomized = 0;
  for (uint8_t i = 0; i < seenCount; i++) {
    if (seen[i].randomized) randomized++;
  }

  Serial.printf("%lu probe request(s) from %u unique address(es)"
                "  [%u randomized, %u stable]\n",
                (unsigned long)frameCount, seenCount, randomized,
                (uint8_t)(seenCount - randomized));

  for (uint8_t i = 0; i < seenCount; i++) {
    Serial.printf("  DEV | %02X:%02X:%02X:%02X:%02X:%02X | %4d dBm | %s\n",
                  seen[i].mac[0], seen[i].mac[1], seen[i].mac[2],
                  seen[i].mac[3], seen[i].mac[4], seen[i].mac[5],
                  seen[i].rssi, seen[i].randomized ? "rnd" : "stable");
  }

  if (frameCount == 0) {
    Serial.println(F("  No frames. Either nothing nearby is probing right now"
                     " (normal with screens off), or the radio is not sniffing."));
  }
}

void loop() {
  scanAccessPoints();
  sniffProbeRequests();
  Serial.println(F("\n--- pause ---"));
  delay(10000);
}

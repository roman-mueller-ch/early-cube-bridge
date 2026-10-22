// Bridges the EARLY (Timeular) cube to the EARLY API: flipping the cube stops the
// running entry and starts the activity mapped to the side facing up.
#include <Arduino.h>
#include <HTTPClient.h>
#include <NimBLEDevice.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiMulti.h>
#include <sys/time.h>
#include <time.h>

#include "ca.h"
#include "config.h"

static_assert(sizeof(EARLY_API_KEY) > 1 && sizeof(EARLY_API_SECRET) > 1, "set the EARLY API key in include/config.h");

#define LOG(fmt, ...) Serial.printf("[%6lu s] " fmt "\n", millis() / 1000, ##__VA_ARGS__)

static const char* API = "https://api.early.app/api/v4";
static const NimBLEUUID ORIENTATION_SERVICE("c7e70010-c847-11e6-8175-8c89a55d403c");
static const NimBLEUUID ORIENTATION("c7e70012-c847-11e6-8175-8c89a55d403c");  // uint8, side facing up
static const uint32_t SETTLE_MS = 2000;  // ignore sides the cube only passes through while being flipped
static const uint32_t RETRY_MS = 10000;

struct Network {
  const char* ssid;
  const char* password;
};
static const Network NETWORKS[] = WIFI_NETWORKS;
static WiFiMulti wifi;

static const char* SIDES[] = SIDE_ACTIVITIES;
static_assert(sizeof(SIDES) / sizeof(SIDES[0]) == 8, "SIDE_ACTIVITIES needs 8 entries");

// --- Time --------------------------------------------------------------------

// EARLY timestamps are UTC without zone suffix, e.g. "2020-08-03T04:00:00.000".
static String isoTime(time_t t) {
  struct tm utc;
  gmtime_r(&t, &utc);
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S.000", &utc);
  return buf;
}

// Seconds since the epoch for a UTC date (newlib has no timegm).
static time_t utcEpoch(int year, int month, int day, int hour, int min, int sec) {
  year -= month <= 2;  // days_from_civil, see howardhinnant.github.io/date_algorithms.html
  long era = (year >= 0 ? year : year - 399) / 400;
  long yoe = year - era * 400;
  long doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  long days = era * 146097 + doe - 719468;
  return (time_t)days * 86400 + hour * 3600 + min * 60 + sec;
}

static time_t parseIsoTime(const String& s) {  // 0 if unreadable
  int y, mo, d, h, mi, se;
  if (sscanf(s.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se) != 6) return 0;
  return utcEpoch(y, mo, d, h, mi, se);
}

static bool clockSet() { return time(nullptr) > 1700000000; }

// Company networks often block NTP, so the clock is also set from the Date header of the API's
// responses ("Thu, 22 Oct 2026 17:21:00 GMT"), which is good to a second.
static void syncClock(const String& date) {
  static const char MONTHS[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
  int day, year, hour, min, sec;
  char month[4];
  if (sscanf(date.c_str(), "%*[^,], %d %3s %d %d:%d:%d", &day, month, &year, &hour, &min, &sec) != 6) return;
  const char* m = strstr(MONTHS, month);
  if (!m || (m - MONTHS) % 3) return;
  time_t server = utcEpoch(year, (m - MONTHS) / 3 + 1, day, hour, min, sec);
  time_t now = time(nullptr);
  if (now - server <= 2 && server - now <= 2) return;  // close enough, don't fight NTP
  struct timeval tv = {server, 0};
  settimeofday(&tv, nullptr);
  LOG("Clock set from the EARLY API: %s", date.c_str());
}

// --- EARLY API ---------------------------------------------------------------

static WiFiClientSecure tls;
static String token;

static int request(const char* method, const String& path, const String& body, String* response, bool auth) {
  HTTPClient http;
  http.begin(tls, String(API) + path);
  http.addHeader("Content-Type", "application/json");
  if (auth) http.addHeader("Authorization", "Bearer " + token);
  const char* headers[] = {"Date"};
  http.collectHeaders(headers, 1);
  int code = http.sendRequest(method, (uint8_t*)body.c_str(), body.length());
  if (code > 0) syncClock(http.header("Date"));
  String res = http.getString();
  http.end();
  if (response) *response = res;
  if (code < 200 || code >= 300) LOG("%s %s -> %d %s", method, path.c_str(), code, res.substring(0, 200).c_str());
  return code;
}

static String jsonString(const String& json, const char* key) {  // first "key":"value", or ""
  String needle = String("\"") + key + "\":\"";
  int start = json.indexOf(needle);
  if (start < 0) return "";
  start += needle.length();
  int end = json.indexOf('"', start);
  return end < 0 ? "" : json.substring(start, end);
}

static bool signIn() {
  String res;
  String body = String("{\"apiKey\":\"") + EARLY_API_KEY + "\",\"apiSecret\":\"" + EARLY_API_SECRET + "\"}";
  if (request("POST", "/developer/sign-in", body, &res, false) != 200) return false;
  token = jsonString(res, "token");
  if (token.isEmpty()) return false;
  LOG("Signed in to EARLY");
  return true;
}

static int api(const char* method, const String& path, const String& body = "", String* response = nullptr) {
  if (token.isEmpty() && !signIn()) return -1;
  int code = request(method, path, body, response, true);
  if (code == 401 && signIn()) code = request(method, path, body, response, true);
  return code;
}

static const time_t MIN_ENTRY_S = 60;  // EARLY doesn't keep time entries shorter than a minute

// Makes `activity` the running tracking (nullptr: nothing running) as of `flippedAtMs` (millis()
// when the cube reported the side), so a switch that is retried after an outage still gets the time
// of the flip. Leaves a matching tracking alone, so syncing after a restart doesn't split the
// current entry.
static bool switchTo(const char* activity, uint32_t flippedAtMs) {
  String current;
  int code = api("GET", "/tracking", "", &current);
  if (code != 200 && code != 404) return false;  // 404: nothing running
  bool running = code == 200;
  if (!running && !activity) return true;
  if (running && activity && current.indexOf(String("\"activity\":{\"id\":\"") + activity + "\"") >= 0) return true;

  if (!clockSet()) {  // neither NTP nor the API's Date header worked
    LOG("Clock not set");
    return false;
  }
  time_t now = time(nullptr);
  time_t at = now - (time_t)((millis() - flippedAtMs) / 1000);
  if (running) {
    time_t startedAt = parseIsoTime(jsonString(current, "startedAt"));
    if (!startedAt) {
      LOG("No startedAt in the current tracking: %s", current.substring(0, 200).c_str());
      return false;
    }
    // Started after the flip (e.g. in the web app while the bridge was offline): it can't be
    // stopped before it began, so the cube takes over from now.
    if (startedAt >= at) at = now;
    if (at - startedAt < MIN_ENTRY_S) {
      // Too short to become a time entry, so cancel it. Only ever for a tracking we know is
      // this short: a failed stop must never throw away a long entry.
      code = api("DELETE", "/tracking");
      if (code != 200 && code != 404 && code != 409) return false;  // 404/409: already gone
    } else {
      code = api("POST", "/tracking/stop", "{\"stoppedAt\":\"" + isoTime(at) + "\"}");
      if (code != 200 && code != 409) return false;  // 409: already stopped elsewhere
    }
  }
  if (!activity) return true;
  code = api("POST", String("/tracking/") + activity + "/start", "{\"startedAt\":\"" + isoTime(at) + "\"}");
  return code == 200 || code == 201;
}

// --- Cube --------------------------------------------------------------------

static volatile int reportedSide = -1;
static volatile uint32_t reportedAt = 0;

static void reportSide(int side) {
  reportedSide = side;
  reportedAt = millis();
  LOG("Cube side %d", side);
}

static void onOrientation(NimBLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
  if (len) reportSide(data[0]);
}

class ScanCallbacks : public NimBLEScanCallbacks {
 public:
  const NimBLEAdvertisedDevice* found = nullptr;
  void onResult(const NimBLEAdvertisedDevice* d) override {
    if (found || d->getName().rfind("Timeular", 0) != 0) return;
    found = d;
    NimBLEDevice::getScan()->stop();
  }
};

class ClientCallbacks : public NimBLEClientCallbacks {
  void onDisconnect(NimBLEClient*, int reason) override { LOG("Cube disconnected (reason %d)", reason); }
};

static ScanCallbacks scanCallbacks;
static ClientCallbacks clientCallbacks;
static NimBLEClient* client = nullptr;

static bool connectCube() {
  NimBLEScan* scan = NimBLEDevice::getScan();
  scanCallbacks.found = nullptr;
  scan->getResults(10000, false);
  const NimBLEAdvertisedDevice* cube = scanCallbacks.found;
  if (!cube) return false;

  if (!client) {
    client = NimBLEDevice::createClient();
    client->setClientCallbacks(&clientCallbacks, false);
    client->setConnectTimeout(10000);
  }
  bool ok = false;
  for (int attempt = 0; attempt < 3 && !ok; attempt++) ok = client->connect(cube);
  scan->clearResults();
  if (!ok) {
    LOG("Connecting to the cube failed (rc=%d)", client->getLastError());
    return false;
  }

  NimBLERemoteService* service = client->getService(ORIENTATION_SERVICE);
  NimBLERemoteCharacteristic* orientation = service ? service->getCharacteristic(ORIENTATION) : nullptr;
  if (!orientation || !orientation->subscribe(false, onOrientation, true)) {
    LOG("Cube has no orientation characteristic");
    client->disconnect();
    return false;
  }
  LOG("Cube connected");
  NimBLEAttValue value = orientation->readValue();
  if (value.length()) reportSide(value.data()[0]);
  return true;
}

// --- Status LED --------------------------------------------------------------
// Fast blink: no WiFi. Slow blink: no cube. Double blink: the last side couldn't be sent to EARLY
// (API error or no clock; it is retried). Short flash every 5 s: all good, and the board is alive.
// On a flip: steady on while the new side is being sent, then three quick blinks once EARLY has it.
// Runs in its own task because WiFi, scanning and HTTP calls block the loop for seconds.

static const int LED_PIN = 2;  // blue LED on ESP32 DevKit boards
enum class Status { NoWifi, NoCube, ApiError, Ok };
static volatile Status status = Status::NoWifi;
static volatile bool switching = false;
static volatile uint32_t confirmedAt = 0;  // millis() of the last successful switch, 0: none

static bool ledOn(Status s, uint32_t tick) {  // 100 ms per tick
  switch (s) {
    case Status::NoWifi: return tick % 2 == 0;
    case Status::NoCube: return tick % 10 < 5;
    case Status::ApiError: return tick % 20 < 2 || (tick % 20 >= 4 && tick % 20 < 6);
    case Status::Ok: return tick % 50 == 0;
  }
  return false;
}

static void ledTask(void*) {
  pinMode(LED_PIN, OUTPUT);
  for (uint32_t tick = 0;; tick++) {
    uint32_t sinceConfirm = millis() - confirmedAt;
    bool on = confirmedAt && sinceConfirm < 600 ? sinceConfirm / 100 % 2 == 0
              : switching                     ? true
                                              : ledOn(status, tick);
    digitalWrite(LED_PIN, on);
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// --- Main --------------------------------------------------------------------

static bool ensureWifi() {
  if (WiFi.status() == WL_CONNECTED) return true;
  status = Status::NoWifi;
  LOG("Connecting to WiFi");
  if (wifi.run(15000) != WL_CONNECTED) return false;
  LOG("WiFi connected to %s, IP %s", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
  return true;
}

void setup() {
  Serial.begin(115200);
  LOG("EARLY cube bridge");
  xTaskCreate(ledTask, "led", 2048, nullptr, 1, nullptr);

  WiFi.mode(WIFI_STA);
  for (const Network& network : NETWORKS) {
    if (*network.ssid) wifi.addAP(network.ssid, network.password);
  }
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  tls.setCACert(ROOT_CA);

  NimBLEDevice::init("early-cube-bridge");
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(&scanCallbacks, false);
  scan->setActiveScan(true);
}

void loop() {
  static int appliedSide = -1;
  static uint32_t lastAttempt = 0;
  static bool apiFailing = false;

  if (!ensureWifi()) {
    delay(5000);
    return;
  }
  if (!client || !client->isConnected()) {
    status = Status::NoCube;
    if (!connectCube()) delay(2000);
    return;
  }

  int side = reportedSide;
  uint32_t flippedAt = reportedAt;
  if (side == appliedSide) apiFailing = false;  // flipped back, nothing left to send
  switching = side >= 0 && side != appliedSide && !apiFailing;
  bool settled = millis() - flippedAt >= SETTLE_MS;
  bool retryDue = lastAttempt == 0 || millis() - lastAttempt >= RETRY_MS;
  if (side >= 0 && side != appliedSide && settled && retryDue) {
    const char* activity = side >= 1 && side <= 8 && *SIDES[side - 1] ? SIDES[side - 1] : nullptr;
    LOG("Side %d: %s", side, activity ? activity : "nothing (stop)");
    lastAttempt = millis();
    apiFailing = !switchTo(activity, flippedAt);
    if (!apiFailing) {
      appliedSide = side;
      lastAttempt = 0;
      switching = false;
      confirmedAt = millis();
      LOG("Done");
    }
  }
  status = apiFailing ? Status::ApiError : Status::Ok;
  delay(100);
}

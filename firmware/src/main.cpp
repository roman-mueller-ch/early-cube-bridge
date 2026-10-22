// Bridges the EARLY (Timeular) cube to the EARLY API: flipping the cube stops the
// running entry and starts the activity mapped to the side facing up.
#include <Arduino.h>
#include <HTTPClient.h>
#include <NimBLEDevice.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <time.h>

#include "ca.h"
#include "config.h"

static_assert(sizeof(WIFI_SSID) > 1, "set WIFI_SSID in include/config.h");
static_assert(sizeof(EARLY_API_KEY) > 1 && sizeof(EARLY_API_SECRET) > 1, "set the EARLY API key in include/config.h");

#define LOG(fmt, ...) Serial.printf("[%6lu s] " fmt "\n", millis() / 1000, ##__VA_ARGS__)

static const char* API = "https://api.early.app/api/v4";
static const NimBLEUUID ORIENTATION_SERVICE("c7e70010-c847-11e6-8175-8c89a55d403c");
static const NimBLEUUID ORIENTATION("c7e70012-c847-11e6-8175-8c89a55d403c");  // uint8, side facing up
static const uint32_t SETTLE_MS = 2000;  // ignore sides the cube only passes through while being flipped
static const uint32_t RETRY_MS = 10000;

static const char* SIDES[] = SIDE_ACTIVITIES;
static_assert(sizeof(SIDES) / sizeof(SIDES[0]) == 8, "SIDE_ACTIVITIES needs 8 entries");

// --- EARLY API ---------------------------------------------------------------

static WiFiClientSecure tls;
static String token;

static int request(const char* method, const String& path, const String& body, String* response, bool auth) {
  HTTPClient http;
  http.begin(tls, String(API) + path);
  http.addHeader("Content-Type", "application/json");
  if (auth) http.addHeader("Authorization", "Bearer " + token);
  int code = http.sendRequest(method, (uint8_t*)body.c_str(), body.length());
  String res = http.getString();
  http.end();
  if (response) *response = res;
  if (code < 200 || code >= 300) LOG("%s %s -> %d %s", method, path.c_str(), code, res.substring(0, 200).c_str());
  return code;
}

static bool signIn() {
  String res;
  String body = String("{\"apiKey\":\"") + EARLY_API_KEY + "\",\"apiSecret\":\"" + EARLY_API_SECRET + "\"}";
  if (request("POST", "/developer/sign-in", body, &res, false) != 200) return false;
  int start = res.indexOf("\"token\":\"");
  if (start < 0) return false;
  start += 9;
  token = res.substring(start, res.indexOf('"', start));
  LOG("Signed in to EARLY");
  return true;
}

static int api(const char* method, const String& path, const String& body = "") {
  if (token.isEmpty() && !signIn()) return -1;
  int code = request(method, path, body, nullptr, true);
  if (code == 401 && signIn()) code = request(method, path, body, nullptr, true);
  return code;
}

static String isoNow() {  // EARLY expects UTC without zone suffix
  time_t now = time(nullptr);
  struct tm utc;
  gmtime_r(&now, &utc);
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S.000", &utc);
  return buf;
}

// Stops whatever is running and starts `activity` (nullptr: just stop).
static bool switchTo(const char* activity) {
  String now = isoNow();
  int code = api("POST", "/tracking/stop", "{\"stoppedAt\":\"" + now + "\"}");
  if (code < 0 || code >= 500) return false;
  if (code != 200) {
    // nothing running, or the entry is shorter than the 1 minute EARLY accepts: drop it
    code = api("DELETE", "/tracking");
    if (code < 0 || code >= 500) return false;
  }
  if (!activity) return true;
  code = api("POST", String("/tracking/") + activity + "/start", "{\"startedAt\":\"" + now + "\"}");
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

// --- Main --------------------------------------------------------------------

static bool ensureWifi() {
  if (WiFi.status() == WL_CONNECTED) return true;
  LOG("Connecting to WiFi %s", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  for (int i = 0; i < 30 && WiFi.status() != WL_CONNECTED; i++) delay(500);
  if (WiFi.status() != WL_CONNECTED) return false;
  LOG("WiFi connected, IP %s", WiFi.localIP().toString().c_str());
  return true;
}

static bool clockSet() { return time(nullptr) > 1700000000; }

void setup() {
  Serial.begin(115200);
  LOG("EARLY cube bridge");

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  tls.setCACert(ROOT_CA);

  NimBLEDevice::init("early-cube-bridge");
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(&scanCallbacks, false);
  scan->setActiveScan(true);
}

void loop() {
  // On boot the current side is only taken as the starting point, so a restart
  // doesn't touch a tracking that was started or edited in the web app.
  static int appliedSide = -1;
  static bool baseline = true;
  static uint32_t lastAttempt = 0;

  if (!ensureWifi()) {
    delay(5000);
    return;
  }
  if (!client || !client->isConnected()) {
    if (!connectCube()) delay(2000);
    return;
  }

  int side = reportedSide;
  bool settled = millis() - reportedAt >= SETTLE_MS;
  bool retryDue = lastAttempt == 0 || millis() - lastAttempt >= RETRY_MS;
  if (side >= 0 && side != appliedSide && settled && retryDue && clockSet()) {
    if (baseline) {
      baseline = false;
      appliedSide = side;
      LOG("Starting on side %d, waiting for the next flip", side);
    } else {
      const char* activity = side >= 1 && side <= 8 && *SIDES[side - 1] ? SIDES[side - 1] : nullptr;
      LOG("Switching to %s", activity ? activity : "nothing (stop)");
      lastAttempt = millis();
      if (switchTo(activity)) {
        appliedSide = side;
        lastAttempt = 0;
        LOG("Done");
      }
    }
  }
  delay(100);
}

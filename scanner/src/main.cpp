// Finds the EARLY/Timeular cube, dumps its GATT table and prints every
// notification, so we can see which characteristic reports the side facing up.
#include <Arduino.h>
#include <NimBLEDevice.h>

static const char* NAME_HINTS[] = {"timeular", "zei", "early", "tracker"};

static bool looksLikeCube(const NimBLEAdvertisedDevice* d) {
  String name = d->getName().c_str();
  name.toLowerCase();
  for (auto hint : NAME_HINTS) {
    if (name.indexOf(hint) >= 0) return true;
  }
  return false;
}

static void printHex(const uint8_t* data, size_t len) {
  for (size_t i = 0; i < len; i++) Serial.printf("%02x ", data[i]);
}

static void onNotify(NimBLERemoteCharacteristic* chr, uint8_t* data, size_t len, bool isNotify) {
  Serial.printf("[%8lu ms] %s %s: ", millis(), isNotify ? "notify  " : "indicate", chr->getUUID().toString().c_str());
  printHex(data, len);
  Serial.println();
}

class ClientCallbacks : public NimBLEClientCallbacks {
  void onDisconnect(NimBLEClient* c, int reason) override {
    Serial.printf("Disconnected (reason %d), restarting in 5 s\n", reason);
    delay(5000);
    ESP.restart();
  }
};

static ClientCallbacks clientCallbacks;

// Side facing up; zeipy documents it as a uint8 with READ/INDICATE.
static const NimBLEUUID ORIENTATION("c7e70012-c847-11e6-8175-8c89a55d403c");
static NimBLERemoteCharacteristic* orientation = nullptr;

static bool subscribe(NimBLEClient* client, NimBLERemoteCharacteristic* chr) {
  bool indicate = !chr->canNotify();
  if (chr->subscribe(!indicate, onNotify, true)) return true;
  Serial.println("    subscribe failed, trying to pair/encrypt...");
  if (!client->secureConnection()) {
    Serial.println("    pairing failed");
    return false;
  }
  return chr->subscribe(!indicate, onNotify, true);
}

class ScanCallbacks : public NimBLEScanCallbacks {
 public:
  const NimBLEAdvertisedDevice* found = nullptr;
  void onResult(const NimBLEAdvertisedDevice* d) override {
    if (found || !looksLikeCube(d)) return;
    found = d;
    Serial.printf("  * %s type=%d rssi=%d connectable=%d name='%s' adv=%s\n", d->getAddress().toString().c_str(),
                  d->getAddressType(), d->getRSSI(), d->isConnectable(), d->getName().c_str(), d->toString().c_str());
    NimBLEDevice::getScan()->stop();
  }
};

static ScanCallbacks scanCallbacks;

static bool connectAndDump(const NimBLEAdvertisedDevice* cube) {
  NimBLEClient* client = NimBLEDevice::getDisconnectedClient();
  if (!client) client = NimBLEDevice::createClient();
  client->setClientCallbacks(&clientCallbacks, false);
  client->setConnectTimeout(10 * 1000);
  Serial.printf("Connecting to %s ...\n", cube->getAddress().toString().c_str());
  if (!client->connect(cube)) {
    int rc = client->getLastError();
    Serial.printf("Connect failed: rc=%d (%s)\n", rc, NimBLEUtils::returnCodeToString(rc));
    return false;
  }
  Serial.println("Connected");

  for (auto svc : client->getServices(true)) {
    Serial.printf("Service %s\n", svc->getUUID().toString().c_str());
    for (auto chr : svc->getCharacteristics(true)) {
      Serial.printf("  Char %s%s%s%s%s\n", chr->getUUID().toString().c_str(),
                    chr->canRead() ? " read" : "", chr->canWrite() ? " write" : "",
                    chr->canNotify() ? " notify" : "", chr->canIndicate() ? " indicate" : "");
      if (chr->canRead()) {
        NimBLEAttValue v = chr->readValue();
        Serial.print("    value: ");
        printHex(v.data(), v.length());
        Serial.printf(" '%s'\n", v.c_str());
      }
      if (chr->getUUID() == ORIENTATION || chr->getUUID() == NimBLEUUID((uint16_t)0x2a19)) {
        Serial.printf("    subscribe: %s\n", subscribe(client, chr) ? "ok" : "FAILED");
      }
      if (chr->getUUID() == ORIENTATION) orientation = chr;
    }
  }
  Serial.println("Ready. Flip the cube and watch for notifications.");
  return true;
}

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 5000) delay(10);
  Serial.println("\n== EARLY cube scanner ==");

  NimBLEDevice::init("cube-scanner");
  NimBLEDevice::setSecurityAuth(true, false, true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);

  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(&scanCallbacks, false);
  scan->setActiveScan(true);
  scan->setInterval(100);
  scan->setWindow(99);
}

void loop() {
  static bool connected = false;
  if (connected) {
    // also poll, in case the cube only indicates after some handshake
    static int last = -1;
    if (orientation) {
      NimBLEAttValue v = orientation->readValue();
      int side = v.length() ? v.data()[0] : -1;
      if (side != last) {
        Serial.printf("[%8lu ms] polled side: %d\n", millis(), side);
        last = side;
      }
    }
    delay(500);
    return;
  }
  Serial.println("Scanning (flip the cube to wake it up)...");
  scanCallbacks.found = nullptr;
  NimBLEDevice::getScan()->getResults(20 * 1000, false);
  if (!scanCallbacks.found) return;
  for (int attempt = 1; attempt <= 3 && !connected; attempt++) {
    Serial.printf("Attempt %d\n", attempt);
    connected = connectAndDump(scanCallbacks.found);
  }
  NimBLEDevice::getScan()->clearResults();
}

// GR III BLE GPS 验证程序（ESP32-C3）
//
// 串口命令（115200）：
//   p  进入配对模式（相机需先执行 Bluetooth Setting > Pairing）
//   x  清除所有配对记录
//   g  切换位置来源：固定坐标 / GPS 模块
//   w  立即写一次位置
//   s  打印状态
//   h  帮助

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <TinyGPSPlus.h>

// ---------------- 配置 ----------------
constexpr int GPS_RX_PIN = 20;
constexpr int GPS_TX_PIN = 21;
constexpr uint32_t GPS_BAUD = 9600;

constexpr bool DEFAULT_USE_GPS = false;
constexpr double FIXED_LAT = 31.230416;
constexpr double FIXED_LON = 121.473701;
constexpr double FIXED_ALT = 10.0;

constexpr int TZ_OFFSET_MINUTES = 8 * 60;
constexpr bool SYNC_CAMERA_CLOCK_FROM_GPS = true;

constexpr uint32_t WRITE_INTERVAL_MS = 5000;
constexpr uint32_t GPS_MAX_AGE_MS = 5000;
constexpr uint32_t RETRY_BACKOFF_MS = 10000;
constexpr uint32_t STATUS_INTERVAL_MS = 15000;
constexpr uint32_t PASSKEY_TIMEOUT_MS = 30000;

const char *DEVICE_NAME = "GR-GPS";
const char *PAIRED_DEVICE_NAME = "GR-GPS (ESP32)";

// ---------------- RICOH BLE UUID ----------------
static const NimBLEUUID GPS_SVC("84a0dd62-e8aa-4d0f-91db-819b6724c69e");
static const NimBLEUUID GPS_INFO_CHR("28f59d60-8b8e-4fcd-a81f-61bdb46595a9");
static const NimBLEUUID CAMERA_SVC("4b445988-caa0-4dd3-941d-37b4f52aca86");
static const NimBLEUUID DATE_TIME_CHR("fa46bbdd-8a8f-4796-8cf3-aa58949b130a");
static const NimBLEUUID GEO_TAG_CHR("a36afdcf-6b67-4046-9be7-28fb67dbc071");
static const NimBLEUUID BT_CTRL_SVC("0f291746-0c80-4726-87a7-3c501fd3b4b6");
static const NimBLEUUID PAIRED_NAME_CHR("fe3a32f8-a189-42de-a391-bc81ae4daa76");
static const NimBLEUUID INFO_SVC("9a5ed1c5-74cc-4c50-b5b6-66a48e7ccff1");
static const NimBLEUUID MODEL_CHR("35fe6272-6aa5-44d9-88e1-f09427f51a71");
static const NimBLEUUID FIRMWARE_CHR("b4eb8905-7411-40a6-a367-2834c2157ea7");

// ---------------- 时间工具 ----------------
struct DateTime {
  int year, month, day, hour, minute, second;
};

static int64_t daysFromCivil(int64_t y, int m, int d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const int64_t yoe = y - era * 400;
  const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

static int64_t toEpoch(const DateTime &t) {
  return daysFromCivil(t.year, t.month, t.day) * 86400 + t.hour * 3600 + t.minute * 60 + t.second;
}

static DateTime fromEpoch(int64_t s) {
  int64_t z = s >= 0 ? s / 86400 : (s - 86399) / 86400;
  const int64_t secs = s - z * 86400;
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const int64_t doe = z - era * 146097;
  const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const int64_t mp = (5 * doy + 2) / 153;
  DateTime t;
  t.day = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
  t.month = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
  t.year = static_cast<int>(yoe + era * 400 + (t.month <= 2));
  t.hour = static_cast<int>(secs / 3600);
  t.minute = static_cast<int>((secs % 3600) / 60);
  t.second = static_cast<int>(secs % 60);
  return t;
}

// ---------------- 全局状态 ----------------
enum class State { Scanning, Connecting, Connected, Backoff };

static State state = State::Scanning;
static Preferences prefs;
static String savedCameraName;
static volatile bool pairingMode = false;
static bool useGps = DEFAULT_USE_GPS;

static HardwareSerial GPSSerial(1);
static TinyGPSPlus gps;

static NimBLEClient *client = nullptr;
static NimBLERemoteCharacteristic *gpsChr = nullptr;
static NimBLEAddress targetAddr;
static std::string targetName;
static volatile bool targetFound = false;

static bool haveUtc = false;
static int64_t utcBaseEpoch = 0;
static uint32_t utcBaseMillis = 0;
static bool clockSyncedThisConnection = false;
static bool readbackDoneThisConnection = false;

static uint32_t lastWriteMs = 0;
static uint32_t lastStatusMs = 0;
static uint32_t backoffUntilMs = 0;
static uint32_t writeOk = 0;
static uint32_t writeFail = 0;

static void setUtcBase(int64_t epoch) {
  utcBaseEpoch = epoch;
  utcBaseMillis = millis();
  haveUtc = true;
}

static int64_t utcNow() {
  return utcBaseEpoch + static_cast<int64_t>((millis() - utcBaseMillis) / 1000);
}

static bool looksLikeRicoh(const std::string &name) {
  return name.rfind("GR", 0) == 0 || name.rfind("RICOH", 0) == 0;
}

static String hex(const uint8_t *data, size_t len) {
  String s;
  char b[4];
  for (size_t i = 0; i < len; i++) {
    snprintf(b, sizeof(b), "%02X", data[i]);
    s += b;
  }
  return s;
}

// ---------------- BLE 回调 ----------------
class ScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice *dev) override {
    if (targetFound) return;
    const std::string name = dev->getName();
    const bool ricoh = dev->isAdvertisingService(GPS_SVC) || looksLikeRicoh(name);
    if (!ricoh) return;

    if (pairingMode) {
      if (name.empty()) return;
    } else {
      if (savedCameraName.isEmpty() || name != savedCameraName.c_str()) return;
    }

    Serial.printf("[scan] 发现相机 %s  %s  RSSI=%d\n", name.c_str(),
                  dev->getAddress().toString().c_str(), dev->getRSSI());
    targetAddr = dev->getAddress();
    targetName = name;
    targetFound = true;
    NimBLEDevice::getScan()->stop();
  }
};

static uint32_t readPasskeyFromSerial(uint32_t timeoutMs) {
  String digits;
  const uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    while (Serial.available()) {
      const char c = Serial.read();
      if (c >= '0' && c <= '9') {
        digits += c;
        Serial.print(c);
        if (digits.length() == 6) {
          Serial.println();
          return digits.toInt();
        }
      } else if ((c == '\n' || c == '\r') && digits.length() > 0) {
        Serial.println();
        return digits.toInt();
      }
    }
    delay(10);
  }
  Serial.println("\n[pair] 输入超时");
  return 0;
}

class ClientCallbacks : public NimBLEClientCallbacks {
  void onConnect(NimBLEClient *) override { Serial.println("[ble] 已连接"); }

  void onConnectFail(NimBLEClient *, int reason) override {
    Serial.printf("[ble] 连接失败 reason=%d\n", reason);
  }

  void onDisconnect(NimBLEClient *, int reason) override {
    Serial.printf("[ble] 已断开 reason=%d\n", reason);
    gpsChr = nullptr;
  }

  void onConfirmPasskey(NimBLEConnInfo &info, uint32_t pin) override {
    if (!pairingMode) {
      Serial.println("[pair] 相机要求重新配对，但当前不在配对模式。按 p 进入配对模式。");
      NimBLEDevice::injectConfirmPasskey(info, false);
      return;
    }
    Serial.printf("[pair] 数字比较码 %06lu —— 核对相机屏幕上的数字，一致就在相机上确认\n",
                  static_cast<unsigned long>(pin));
    NimBLEDevice::injectConfirmPasskey(info, true);
  }

  void onPassKeyEntry(NimBLEConnInfo &info) override {
    if (!pairingMode) {
      Serial.println("[pair] 相机要求重新配对，但当前不在配对模式。按 p 进入配对模式。");
      NimBLEDevice::injectPassKey(info, 0);
      return;
    }
    Serial.println("[pair] 请输入相机屏幕上显示的 6 位认证码：");
    NimBLEDevice::injectPassKey(info, readPasskeyFromSerial(PASSKEY_TIMEOUT_MS));
  }

  void onAuthenticationComplete(NimBLEConnInfo &info) override {
    Serial.printf("[pair] 加密=%d 认证=%d 绑定=%d\n", info.isEncrypted(), info.isAuthenticated(),
                  info.isBonded());
  }
};

static ScanCallbacks scanCallbacks;
static ClientCallbacks clientCallbacks;

// ---------------- 相机读写 ----------------
static NimBLERemoteCharacteristic *findChr(const NimBLEUUID &svc, const NimBLEUUID &chr) {
  NimBLERemoteService *s = client->getService(svc);
  return s ? s->getCharacteristic(chr) : nullptr;
}

static void logStringChr(const char *label, const NimBLEUUID &svc, const NimBLEUUID &chr) {
  NimBLERemoteCharacteristic *c = findChr(svc, chr);
  if (c && c->canRead()) {
    const std::string v = c->readValue();
    Serial.printf("[cam] %s: %s\n", label, v.c_str());
  }
}

static void readCameraClock() {
  NimBLERemoteCharacteristic *c = findChr(CAMERA_SVC, DATE_TIME_CHR);
  if (!c || !c->canRead()) return;
  const NimBLEAttValue v = c->readValue();
  if (v.length() < 7) return;
  const uint8_t *d = v.data();
  const DateTime local{d[0] | (d[1] << 8), d[2], d[3], d[4], d[5], d[6]};
  Serial.printf("[cam] 相机时间 %04d-%02d-%02d %02d:%02d:%02d\n", local.year, local.month,
                local.day, local.hour, local.minute, local.second);
  if (!haveUtc && local.year >= 2020) {
    setUtcBase(toEpoch(local) - TZ_OFFSET_MINUTES * 60);
    Serial.println("[time] 暂用相机时间作为 UTC 基准（固定坐标模式下，时间戳准不准取决于相机时钟）");
  }
}

static void syncCameraClockFromGps() {
  if (!SYNC_CAMERA_CLOCK_FROM_GPS || clockSyncedThisConnection || !useGps || !haveUtc) return;
  if (!gps.date.isValid() || !gps.time.isValid()) return;
  NimBLERemoteCharacteristic *c = findChr(CAMERA_SVC, DATE_TIME_CHR);
  if (!c || !c->canWrite()) return;
  const DateTime t = fromEpoch(utcNow() + TZ_OFFSET_MINUTES * 60);
  const uint8_t buf[7] = {static_cast<uint8_t>(t.year & 0xFF), static_cast<uint8_t>(t.year >> 8),
                          static_cast<uint8_t>(t.month),       static_cast<uint8_t>(t.day),
                          static_cast<uint8_t>(t.hour),        static_cast<uint8_t>(t.minute),
                          static_cast<uint8_t>(t.second)};
  const bool ok = c->writeValue(buf, sizeof(buf), true);
  Serial.printf("[time] 用 GPS 时间校准相机时钟 %04d-%02d-%02d %02d:%02d:%02d => %s\n", t.year,
                t.month, t.day, t.hour, t.minute, t.second, ok ? "OK" : "失败");
  clockSyncedThisConnection = ok;
}

static void ensureGeoTagEnabled() {
  NimBLERemoteCharacteristic *c = findChr(CAMERA_SVC, GEO_TAG_CHR);
  if (!c) {
    Serial.println("[cam] 找不到 GEO Tag 特征值");
    return;
  }
  const NimBLEAttValue v = c->readValue();
  const bool enabled = v.length() > 0 && v.data()[0] == 1;
  Serial.printf("[cam] 相机位置记录开关: %s\n", enabled ? "开" : "关");
  if (!enabled) {
    const uint8_t on = 1;
    Serial.printf("[cam] 打开位置记录 => %s\n", c->writeValue(&on, 1, true) ? "OK" : "失败");
  }
}

static void setPairedDeviceName() {
  NimBLERemoteCharacteristic *c = findChr(BT_CTRL_SVC, PAIRED_NAME_CHR);
  if (c && c->canWrite()) {
    c->writeValue(reinterpret_cast<const uint8_t *>(PAIRED_DEVICE_NAME), strlen(PAIRED_DEVICE_NAME),
                  true);
  }
}

static void putDoubleBE(uint8_t *out, double value) {
  uint64_t bits;
  memcpy(&bits, &value, sizeof(bits));
  for (int i = 0; i < 8; i++) out[i] = static_cast<uint8_t>(bits >> (56 - 8 * i));
}

static bool writeLocation(double lat, double lon, double alt, const DateTime &utc) {
  if (!gpsChr) return false;
  uint8_t buf[32];
  putDoubleBE(buf + 0, lat);
  putDoubleBE(buf + 8, lon);
  putDoubleBE(buf + 16, alt);
  buf[24] = static_cast<uint8_t>(utc.year & 0xFF);
  buf[25] = static_cast<uint8_t>(utc.year >> 8);
  buf[26] = static_cast<uint8_t>(utc.month);
  buf[27] = static_cast<uint8_t>(utc.day);
  buf[28] = static_cast<uint8_t>(utc.hour);
  buf[29] = static_cast<uint8_t>(utc.minute);
  buf[30] = static_cast<uint8_t>(utc.second);
  buf[31] = 0;
  return gpsChr->writeValue(buf, sizeof(buf), true);
}

static void sendLocation() {
  double lat, lon, alt;
  if (useGps) {
    if (!gps.location.isValid() || gps.location.age() > GPS_MAX_AGE_MS) {
      Serial.println("[gps] 还没定位（或定位过期），本次不写入");
      return;
    }
    lat = gps.location.lat();
    lon = gps.location.lng();
    alt = gps.altitude.isValid() ? gps.altitude.meters() : 0.0;
  } else {
    lat = FIXED_LAT;
    lon = FIXED_LON;
    alt = FIXED_ALT;
  }
  if (!haveUtc) {
    Serial.println("[time] 还没有可用的时间基准，本次不写入");
    return;
  }

  const DateTime utc = fromEpoch(utcNow());
  const bool ok = writeLocation(lat, lon, alt, utc);
  ok ? writeOk++ : writeFail++;
  Serial.printf("[write] %s %.6f, %.6f, %.1fm  UTC %04d-%02d-%02d %02d:%02d:%02d => %s\n",
                useGps ? "GPS" : "固定", lat, lon, alt, utc.year, utc.month, utc.day, utc.hour,
                utc.minute, utc.second, ok ? "OK" : "失败");

  if (ok && !readbackDoneThisConnection && gpsChr && gpsChr->canRead()) {
    const NimBLEAttValue v = gpsChr->readValue();
    Serial.printf("[write] 回读 %u 字节: %s\n", static_cast<unsigned>(v.length()),
                  hex(v.data(), v.length()).c_str());
    readbackDoneThisConnection = true;
  }
}

static bool connectCamera() {
  if (!client) {
    client = NimBLEDevice::createClient();
    client->setClientCallbacks(&clientCallbacks, false);
    client->setConnectTimeout(10 * 1000);
  }

  Serial.printf("[ble] 连接 %s ...\n", targetName.c_str());
  if (!client->connect(targetAddr)) return false;

  Serial.println("[ble] 建立加密连接（首次会配对）...");
  if (!client->secureConnection()) {
    Serial.println("[ble] 加密/配对失败");
    client->disconnect();
    return false;
  }
  const NimBLEConnInfo info = client->getConnInfo();
  if (!info.isEncrypted() || !info.isBonded()) {
    Serial.println("[ble] 连接未加密或未绑定");
    client->disconnect();
    return false;
  }

  logStringChr("型号", INFO_SVC, MODEL_CHR);
  logStringChr("固件", INFO_SVC, FIRMWARE_CHR);

  gpsChr = findChr(GPS_SVC, GPS_INFO_CHR);
  if (!gpsChr || !gpsChr->canWrite()) {
    Serial.println("[cam] 找不到可写的 GPS Information 特征值");
    client->disconnect();
    return false;
  }

  setPairedDeviceName();
  ensureGeoTagEnabled();
  readCameraClock();

  if (pairingMode) {
    savedCameraName = targetName.c_str();
    prefs.putString("camera", savedCameraName);
    pairingMode = false;
    Serial.printf("[pair] 配对完成，已记住相机 %s，以后开机会自动连接\n", savedCameraName.c_str());
  }

  clockSyncedThisConnection = false;
  readbackDoneThisConnection = false;
  lastWriteMs = 0;
  return true;
}

// ---------------- GPS ----------------
static void pumpGps() {
  while (GPSSerial.available()) gps.encode(GPSSerial.read());
  if (gps.date.isValid() && gps.time.isValid() && gps.time.isUpdated() && gps.date.year() >= 2024) {
    const DateTime t{gps.date.year(), gps.date.month(), gps.date.day(),
                     gps.time.hour(), gps.time.minute(), gps.time.second()};
    setUtcBase(toEpoch(t));
  }
}

// ---------------- 串口命令 ----------------
static void printHelp() {
  Serial.println("命令: p=配对  x=清除配对  g=切换固定坐标/GPS  w=立即写入  s=状态  h=帮助");
}

static const char *stateName() {
  switch (state) {
    case State::Scanning: return "扫描中";
    case State::Connecting: return "连接中";
    case State::Connected: return "已连接";
    case State::Backoff: return "等待重试";
  }
  return "?";
}

static void printStatus() {
  Serial.printf("[status] %s | 相机=%s | 来源=%s | 配对模式=%d | 写入 OK=%lu 失败=%lu\n",
                stateName(), savedCameraName.isEmpty() ? "(未配对)" : savedCameraName.c_str(),
                useGps ? "GPS" : "固定", pairingMode ? 1 : 0, static_cast<unsigned long>(writeOk),
                static_cast<unsigned long>(writeFail));
  Serial.printf("[status] GPS 字符=%lu 校验失败=%lu 卫星=%lu 定位=%s",
                static_cast<unsigned long>(gps.charsProcessed()),
                static_cast<unsigned long>(gps.failedChecksum()),
                static_cast<unsigned long>(gps.satellites.isValid() ? gps.satellites.value() : 0),
                gps.location.isValid() ? "有" : "无");
  if (gps.location.isValid()) Serial.printf(" %.6f, %.6f", gps.location.lat(), gps.location.lng());
  Serial.println();
}

static void handleSerial() {
  while (Serial.available()) {
    switch (Serial.read()) {
      case 'p':
        pairingMode = true;
        Serial.println("[pair] 配对模式：请在相机上执行 Bluetooth Setting > Pairing");
        if (state == State::Backoff) state = State::Scanning;
        break;
      case 'x':
        if (client && client->isConnected()) client->disconnect();
        NimBLEDevice::deleteAllBonds();
        prefs.remove("camera");
        savedCameraName = "";
        Serial.println("[pair] 已清除配对记录。相机那边也要删除旧配对，然后按 p 重新配对");
        break;
      case 'g':
        useGps = !useGps;
        Serial.printf("[cfg] 位置来源: %s\n", useGps ? "GPS 模块" : "固定坐标");
        break;
      case 'w':
        if (state == State::Connected) sendLocation();
        break;
      case 's': printStatus(); break;
      case 'h': printHelp(); break;
      default: break;
    }
  }
}

// ---------------- 主流程 ----------------
static void startScan() {
  NimBLEScan *scan = NimBLEDevice::getScan();
  if (scan->isScanning()) return;
  targetFound = false;
  scan->start(0, false, true);
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("\n=== GR III BLE GPS 验证程序 ===");

  GPSSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);

  prefs.begin("grgps", false);
  savedCameraName = prefs.getString("camera", "");

  NimBLEDevice::init(DEVICE_NAME);
  NimBLEDevice::setSecurityAuth(true, true, true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_KEYBOARD_DISPLAY);
  NimBLEDevice::setSecurityInitKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
  NimBLEDevice::setSecurityRespKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);

  NimBLEScan *scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(&scanCallbacks, false);
  scan->setActiveScan(true);
  scan->setInterval(100);
  scan->setWindow(99);
  scan->setDuplicateFilter(false);
  scan->setMaxResults(0);

  if (savedCameraName.isEmpty()) {
    Serial.println("还没有配对相机。在相机上执行 Bluetooth Setting > Pairing，然后按 p。");
  } else {
    Serial.printf("已配对相机: %s，打开相机即可自动连接\n", savedCameraName.c_str());
  }
  Serial.printf("位置来源: %s\n", useGps ? "GPS 模块" : "固定坐标");
  printHelp();
}

void loop() {
  pumpGps();
  handleSerial();

  switch (state) {
    case State::Scanning:
      if (savedCameraName.isEmpty() && !pairingMode) break;
      startScan();
      if (targetFound) state = State::Connecting;
      break;

    case State::Connecting:
      if (connectCamera()) {
        state = State::Connected;
      } else {
        Serial.printf("[ble] %lu 秒后重试\n", static_cast<unsigned long>(RETRY_BACKOFF_MS / 1000));
        backoffUntilMs = millis() + RETRY_BACKOFF_MS;
        state = State::Backoff;
      }
      targetFound = false;
      break;

    case State::Connected:
      if (!client->isConnected()) {
        gpsChr = nullptr;
        state = State::Scanning;
        break;
      }
      syncCameraClockFromGps();
      if (lastWriteMs == 0 || millis() - lastWriteMs >= WRITE_INTERVAL_MS) {
        lastWriteMs = millis();
        sendLocation();
      }
      break;

    case State::Backoff:
      if (static_cast<int32_t>(millis() - backoffUntilMs) >= 0) state = State::Scanning;
      break;
  }

  if (millis() - lastStatusMs >= STATUS_INTERVAL_MS) {
    lastStatusMs = millis();
    printStatus();
  }
  delay(10);
}

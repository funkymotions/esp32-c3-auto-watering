#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <time.h>

#define PUMP_PIN 2
#define LED_PIN 8

const char* ssid = "ESP32-C3-Watering";
const char* password = "00000000";

AsyncWebServer server(80);
Preferences preferences;
RTC_DATA_ATTR unsigned long configWindowCompletedAtMs = 0;
void setupWebServer();
void loadOptionsFromStorage();
void initializeOptionsStorage();
static bool isDebugEnabled();
void connectToExternalWiFiAndSyncTime();

constexpr uint8_t LED_ON_LEVEL = LOW;
constexpr uint8_t LED_OFF_LEVEL = HIGH;
constexpr char OPTIONS_NAMESPACE[] = "options";
constexpr char OPTIONS_INITIALIZED_KEY[] = "__initialized";
constexpr unsigned long CONFIG_BLINK_INTERVAL_MIN_MS = 60;
constexpr unsigned long CONFIG_BLINK_INTERVAL_MAX_MS = 500;

static void setPumpState(bool isEnabled) {
  digitalWrite(PUMP_PIN, isEnabled ? HIGH : LOW);
  digitalWrite(LED_PIN, isEnabled ? LED_ON_LEVEL : LED_OFF_LEVEL);
}

static void setLedState(bool isEnabled) {
  digitalWrite(LED_PIN, isEnabled ? LED_ON_LEVEL : LED_OFF_LEVEL);
}

struct Option {
  String name;
  String displayName; // for future use, currently the same as name
  String type; // "number", "range" or "string"
  String value;
  String minValue; // for number/range type
  String maxValue; // for number/range type
};

// Example options
Option options[] = {
  {"debug", "Debug", "number", "0", "0", "1"},
  {"init_window", "Таймаут конфигурации по Wi-Fi (минуты)", "range", "2", "1", "10"},
  {"pump_duration", "Время работы насоса (секунды)", "range", "5", "1", "300"},
  {"sleep_duration", "Интервал между включениями (дни)", "range", "1", "1", "60"},
  {"pump_time", "Время включения насоса (чч:мм)", "string", "06:00", "", ""},
  {"ext_ssid", "Внешний SSID", "string", "", "", ""},
  {"ext_pwd", "Пароль внешнего SSID", "string", "", "", ""},
  {"deviceName", "Имя устройства", "string", ssid, "", ""}
};

const size_t optionsCount = sizeof(options) / sizeof(options[0]);

Option* findOption(const String& name) {
  for (size_t i = 0; i < optionsCount; ++i) {
    if (options[i].name == name) return &options[i];
  }
  return nullptr;
}

static void appendOptionJson(String& json, const Option& option) {
  json += "{";
  json += "\"name\": \"" + option.name + "\",";
  json += "\"displayName\": \"" + option.displayName + "\",";
  json += "\"type\": \"" + option.type + "\",";
  if (option.type == "range") {
    json += "\"value\":" + option.value;
    json += ",\"min\":" + option.minValue;
    json += ",\"max\":" + option.maxValue;
  } else if (option.type == "number") {
    json += "\"value\":" + option.value;
    if (option.minValue.length() > 0) {
      json += ",\"min\":" + option.minValue;
    }
    if (option.maxValue.length() > 0) {
      json += ",\"max\":" + option.maxValue;
    }
  } else {
    json += "\"value\":";
    json += "\"" + option.value + "\"";
  }
  json += "}";
}

static bool parseNumericValue(JsonVariantConst valueVariant, double& parsedValue, String& serializedValue) {
  if (valueVariant.is<int>()) {
    parsedValue = valueVariant.as<int>();
    serializedValue = String(valueVariant.as<int>());
    return true;
  }

  if (valueVariant.is<float>()) {
    parsedValue = valueVariant.as<float>();
    serializedValue = String(parsedValue, 6);
    return true;
  }

  if (valueVariant.is<const char*>()) {
    String rawValue = valueVariant.as<const char*>();
    char* endPtr = nullptr;
    parsedValue = strtod(rawValue.c_str(), &endPtr);
    if (endPtr == rawValue.c_str() || *endPtr != '\0') {
      return false;
    }
    serializedValue = rawValue;
    return true;
  }

  return false;
}

static bool parseRangeValue(const JsonDocument& doc, const char* key, const char* legacyKey, double& parsedValue, String& serializedValue) {
  JsonVariantConst valueVariant = doc[key];
  if (valueVariant.isNull() && legacyKey != nullptr) {
    valueVariant = doc[legacyKey];
  }

  if (valueVariant.isNull()) {
    return false;
  }

  return parseNumericValue(valueVariant, parsedValue, serializedValue);
}

static bool parseNumericString(const String& rawValue, double& parsedValue, String& serializedValue) {
  char* endPtr = nullptr;
  parsedValue = strtod(rawValue.c_str(), &endPtr);
  if (endPtr == rawValue.c_str() || *endPtr != '\0') {
    return false;
  }

  serializedValue = rawValue;
  return true;
}

static bool validateOptionValue(const Option& option, const String& candidateValue, String& errorMessage) {
  if (option.type == "number" || option.type == "range") {
    double parsedValue = 0.0;
    String normalizedValue;
    if (!parseNumericString(candidateValue, parsedValue, normalizedValue)) {
      errorMessage = "bad value type";
      return false;
    }

    if (option.minValue.length() > 0 && parsedValue < option.minValue.toDouble()) {
      errorMessage = "value below min";
      return false;
    }

    if (option.maxValue.length() > 0 && parsedValue > option.maxValue.toDouble()) {
      errorMessage = "value above max";
      return false;
    }
  }

  return true;
}

static bool persistOptionValue(const Option& option) {
  size_t storedLength = preferences.putString(option.name.c_str(), option.value);
  return storedLength == option.value.length();
}

static unsigned long readStoredOptionDurationMs(const char* optionName) {
  Option* option = findOption(optionName);
  String storedValue = preferences.getString(optionName, option->value);
  double parsedValue = 0.0;
  String normalizedValue;
  parseNumericString(storedValue, parsedValue, normalizedValue);
  option->value = normalizedValue;

  return static_cast<unsigned long>(parsedValue);
}

static unsigned long getOptionDurationMs(const char* optionName) {
  Option* option = findOption(optionName);
  double parsedValue = 0.0;
  String normalizedValue;
  parseNumericString(option->value, parsedValue, normalizedValue);
  if (String(optionName) == "sleep_duration") {
    // Если debug включён, значение в секундах, иначе в днях
    if (isDebugEnabled()) {
      return static_cast<unsigned long>(parsedValue * 1000UL);
    } else {
      return static_cast<unsigned long>(parsedValue * 24UL * 60UL * 60UL * 1000UL);
    }
  }
  if (String(optionName) == "pump_duration") {
    // Значение хранится в секундах, возвращаем миллисекунды
    return static_cast<unsigned long>(parsedValue * 1000UL);
  }
  return static_cast<unsigned long>(parsedValue);
}

static bool shouldRunConfigurationWindow() {
  return configWindowCompletedAtMs == 0;
}

static bool isDebugEnabled() {
  Option* debugOption = findOption("debug");
  return debugOption && debugOption->value == "1";
}

static void runConfigurationWindow() {
  unsigned long configurationWindowMinutes = readStoredOptionDurationMs("init_window");
  Serial.println("Configuration window started for " + String(configurationWindowMinutes) + " minute(s)");
  unsigned long startedAt = millis();
  bool ledEnabled = false;

  while (true) {
    configurationWindowMinutes = readStoredOptionDurationMs("init_window");
    unsigned long configurationWindowMs = configurationWindowMinutes * 60UL * 1000UL;
    unsigned long elapsedMs = millis() - startedAt;
    if (elapsedMs >= configurationWindowMs) {
      break;
    }

    unsigned long blinkIntervalMs = CONFIG_BLINK_INTERVAL_MIN_MS;
    if (configurationWindowMs > 0) {
      unsigned long intervalRangeMs = CONFIG_BLINK_INTERVAL_MAX_MS - CONFIG_BLINK_INTERVAL_MIN_MS;
      unsigned long remainingMs = configurationWindowMs - elapsedMs;
      blinkIntervalMs = CONFIG_BLINK_INTERVAL_MIN_MS + (intervalRangeMs * remainingMs) / configurationWindowMs;
    }

    ledEnabled = !ledEnabled;
    setLedState(ledEnabled);
    delay(blinkIntervalMs);
  }

  setLedState(false);
  configWindowCompletedAtMs = millis();
  Serial.println("Configuration window finished");

  // Синхронизация времени через внешний Wi-Fi после окна конфигурации
  Option* ssidOpt = findOption("ext_ssid");
  Option* passOpt = findOption("ext_pwd");
  String ssidStr = ssidOpt ? ssidOpt->value : "";
  String passStr = passOpt ? passOpt->value : "";
  Serial.println("External WiFi SSID: " + ssidStr);
  Serial.println("External WiFi Password: " + passStr);
  if (ssidStr.length() > 0 && passStr.length() > 0) {
    connectToExternalWiFiAndSyncTime();
  } else {
    Serial.println("External WiFi SSID or password not set, skipping time sync.");
  }
}

static void runPumpCycle() {
  unsigned long pumpDurationMs = getOptionDurationMs("pump_duration");
  Serial.println("Pump ON for " + String(pumpDurationMs) + " ms");
  setPumpState(true);
  delay(pumpDurationMs);
  setPumpState(false);
  Serial.println("Pump OFF");
}

unsigned long calculateNextWakeupMs() {
  // Получаем параметры
  Option* sleepOpt = findOption("sleep_duration");
  Option* timeOpt = findOption("pump_time");
  double intervalDays = 1.0;
  String normalizedValue;
  if (sleepOpt) parseNumericString(sleepOpt->value, intervalDays, normalizedValue);
  // Проверяем, задано ли время и получено ли реальное время
  bool pumpTimeSet = false;
  int targetHour = 6, targetMinute = 0;
  if (timeOpt) {
    String t = timeOpt->value;
    int sep = t.indexOf(":");
    if (sep > 0) {
      targetHour = t.substring(0, sep).toInt();
      targetMinute = t.substring(sep + 1).toInt();
      pumpTimeSet = true;
    }
  }
  time_t now = 0;
  struct tm timeinfo;
  bool timeValid = false;
  if (time(&now) && now > 100000) { // 100000 ~ 1970-01-02, значит время синхронизировано
    localtime_r(&now, &timeinfo);
    timeValid = true;
  }
  if (pumpTimeSet && timeValid) {
    struct tm next = timeinfo;
    next.tm_hour = targetHour;
    next.tm_min = targetMinute;
    next.tm_sec = 0;
    time_t nextTime = mktime(&next);
    if (nextTime <= now) {
      nextTime += (int)intervalDays * 24 * 60 * 60;
    }
    return (nextTime - now) * 1000UL;
  } else {
    // Если нет pump_time или не получено реальное время — fallback на sleep_duration с учётом debug
    return getOptionDurationMs("sleep_duration");
  }
}

void connectToExternalWiFiAndSyncTime() {
  Option* ssidOpt = findOption("ext_ssid");
  Option* passOpt = findOption("ext_pwd");
  String ssidStr = ssidOpt ? ssidOpt->value : "";
  String passStr = passOpt ? passOpt->value : "";
  if (ssidStr.length() == 0) return;

  WiFi.mode(WIFI_AP_STA);
  WiFi.begin(ssidStr.c_str(), passStr.c_str());
  Serial.println("Connecting to external WiFi: " + ssidStr);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(500);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nConnected to external WiFi");
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    // Ждём синхронизации времени
    struct tm timeinfo;
    for (int i = 0; i < 20; ++i) {
      if (getLocalTime(&timeinfo, 1000)) {
        Serial.printf("Time synced: %02d:%02d\n", timeinfo.tm_hour, timeinfo.tm_min);
        break;
      }
      delay(500);
    }
    WiFi.disconnect();
  } else {
    Serial.println("\nFailed to connect to external WiFi");
  }
  WiFi.mode(WIFI_AP_STA); // Оставляем AP
}

void enterConfiguredDeepSleep() {
  // Время уже синхронизировано после конфигурационного окна, повторно не подключаемся
  unsigned long sleepMs = calculateNextWakeupMs();
  uint64_t sleepUs = (uint64_t)sleepMs * 1000ULL;
  Serial.println("Entering deep sleep for " + String(sleepMs) + " ms");
  WiFi.softAPdisconnect(false); // AP остаётся
  WiFi.mode(WIFI_AP_STA);
  if(isDebugEnabled()) {
    delay(sleepMs);
  } else {
    esp_sleep_enable_timer_wakeup(sleepUs);
    esp_deep_sleep_start();
  }
}

void initializeOptionsStorage() {
  bool isInitialized = preferences.getBool(OPTIONS_INITIALIZED_KEY, false);

  if (!isInitialized) {
    Serial.println("First start detected, saving default option values");
    for (size_t i = 0; i < optionsCount; ++i) {
      if (!persistOptionValue(options[i])) {
        Serial.println("Failed to save default value for " + options[i].name);
      }
    }
    preferences.putBool(OPTIONS_INITIALIZED_KEY, true);
    return;
  }

  for (size_t i = 0; i < optionsCount; ++i) {
    if (preferences.isKey(options[i].name.c_str())) {
      continue;
    }

    if (!persistOptionValue(options[i])) {
      Serial.println("Failed to backfill default value for " + options[i].name);
      continue;
    }

    Serial.println("Saved default value for new option: " + options[i].name);
  }
}

void loadOptionsFromStorage() {
  for (size_t i = 0; i < optionsCount; ++i) {
    String storedValue = preferences.getString(options[i].name.c_str(), "");
    if (storedValue.isEmpty()) {
      continue;
    }

    String errorMessage;
    if (!validateOptionValue(options[i], storedValue, errorMessage)) {
      Serial.println("Ignoring invalid stored value for " + options[i].name + ": " + errorMessage);
      continue;
    }

    options[i].value = storedValue;
    Serial.println("Loaded option from storage: " + options[i].name + "=" + options[i].value);
  }
}

// --- API Handlers as functions ---
void handleListOptions(AsyncWebServerRequest *request) {
  String json = "{\"options\": [";
  for (size_t i = 0; i < optionsCount; ++i) {
    appendOptionJson(json, options[i]);
    if (i < optionsCount - 1) json += ",";
  }
  json += "]}";
  Serial.println("Responding with options: " + json);
  request->send(200, "application/json", json);
}

void handleGetOption(AsyncWebServerRequest *request) {
  String name = request->pathArg(0);
  Option* opt = findOption(name);
  if (!opt) {
    request->send(404, "application/json", "{}\n");
    return;
  }
  String json;
  appendOptionJson(json, *opt);
  Serial.println("Responding with option: " + json);
  request->send(200, "application/json", json);
}

void handlePostOption(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
  String name = request->pathArg(0);
  Option* opt = findOption(name);
  if (!opt) {
    request->send(404, "application/json", "{}\n");
    return;
  }

  String body = "";
  for (size_t i = 0; i < len; i++) {
      body += (char)data[i];
  }

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, body);
  if (error) {
    request->send(400, "application/json", "{\"error\":\"bad json\"}\n");
    return;
  }
  Serial.println("Received update for option " + name + ": " + body);
  if (opt->type == "range") {
    double parsedValue = 0.0;
    String serializedValue;

    if (!parseRangeValue(doc, "value", nullptr, parsedValue, serializedValue)) {
      request->send(400, "application/json", "{\"error\":\"no value\"}\n");
      return;
    }

    String errorMessage;
    if (!validateOptionValue(*opt, serializedValue, errorMessage)) {
      request->send(400, "application/json", "{\"error\":\"" + errorMessage + "\"}\n");
      return;
    }

    opt->value = serializedValue;
  } else if (opt->type == "number") {
    double parsedValue = 0.0;
    String serializedValue;

    if (!parseRangeValue(doc, "value", nullptr, parsedValue, serializedValue)) {
      request->send(400, "application/json", "{\"error\":\"no value\"}\n");
      return;
    }

    String errorMessage;
    if (!validateOptionValue(*opt, serializedValue, errorMessage)) {
      request->send(400, "application/json", "{\"error\":\"" + errorMessage + "\"}\n");
      return;
    }

    opt->value = serializedValue;
  } else {
    if (doc["value"].isNull()) {
      request->send(400, "application/json", "{\"error\":\"no value\"}\n");
      return;
    }
    opt->value = doc["value"].as<String>();
  }

  if (!persistOptionValue(*opt)) {
    request->send(500, "application/json", "{\"error\":\"failed to save value\"}\n");
    return;
  }

  String resp = "{\"" + name + "\": true}";
  Serial.println("Responding with update result: " + resp);
  request->send(200, "application/json", resp);
  return; 
}

void setupWebServer() {
  server.serveStatic("/dist", SPIFFS, "/dist");
  server.serveStatic("/assets", SPIFFS, "/dist/assets");

  // Serve index.html for root
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(SPIFFS, "/dist/index.html", "text/html");
  });


  server.on("/api/options", HTTP_GET, handleListOptions);
  server.on("^/api/options/([a-zA-Z0-9_-]+)$", HTTP_GET, handleGetOption);
  server.on("^/api/options/([a-zA-Z0-9_-]+)$", HTTP_POST, [](AsyncWebServerRequest *request){}, NULL, handlePostOption);

  server.begin();
}

void setup() {
  delay(1000);
  Serial.begin(115200);
  bool runConfigurationWindowOnThisBoot = shouldRunConfigurationWindow();
  if (!preferences.begin(OPTIONS_NAMESPACE, false)) {
    Serial.println("Ошибка инициализации NVS");
    return;
  }

  initializeOptionsStorage();
  loadOptionsFromStorage();
  pinMode(PUMP_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  setPumpState(false);
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(ssid, password);
  if (runConfigurationWindowOnThisBoot) { 
    if (!SPIFFS.begin(true)) {
      Serial.println("Ошибка монтирования SPIFFS");
      return;
    }

    Serial.println("\nWiFi AP поднят!");
    Serial.print("IP адрес: ");
    Serial.println(WiFi.softAPIP());
    setupWebServer();
    delay(1000);
    runConfigurationWindow();
  }
}

void loop() {
  runPumpCycle();
  enterConfiguredDeepSleep();
  delay(1000);
}
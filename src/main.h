
#pragma once
#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <Preferences.h>

struct Option {
  String name;
  String displayName;
  String type;
  String value;
  String minValue;
  String maxValue;
};

extern const char* ssid;
extern const char* password;
extern AsyncWebServer server;
extern Preferences preferences;
extern RTC_DATA_ATTR unsigned long configWindowCompletedAtMs;
extern Option options[];
extern const size_t optionsCount;

Option* findOption(const String& name);
bool parseRangeValue(const JsonDocument& doc, const char* key, const char* legacyKey,
                     double& parsedValue, String& serializedValue);

bool validateOptionValue(const Option& option, const String& candidateValue, String& errorMessage);
bool persistOptionValue(const Option& option);

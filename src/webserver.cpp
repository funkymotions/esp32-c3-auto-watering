#include "webserver.h"
#include "main.h"
#include <SPIFFS.h>
#include <ArduinoJson.h>

// --- Вспомогательные функции для web API ---
void appendOptionJson(JsonObject optionJson, const Option& option) {
  optionJson["name"] = option.name;
  optionJson["displayName"] = option.displayName;
  optionJson["type"] = option.type;
  if (option.type == "range") {
    optionJson["value"] = option.value.toFloat();
    optionJson["min"] = option.minValue.toFloat();
    optionJson["max"] = option.maxValue.toFloat();
  } else if (option.type == "number") {
    optionJson["value"] = option.value.toFloat();
    if (option.minValue.length() > 0) {
      optionJson["min"] = option.minValue.toFloat();
    }
    if (option.maxValue.length() > 0) {
      optionJson["max"] = option.maxValue.toFloat();
    }
  } else {
    optionJson["value"] = option.value;
  }
}

void handleListOptions(AsyncWebServerRequest* request) {
  JsonDocument doc;
  JsonArray optionsJson = doc["options"].to<JsonArray>();
  for (size_t i = 0; i < optionsCount; ++i) {
    JsonObject optionJson = optionsJson.add<JsonObject>();
    appendOptionJson(optionJson, options[i]);
  }
  String json;
  serializeJson(doc, json);
  Serial.println("Responding with options: " + json);
  request->send(200, "application/json", json);
}

void handleGetOption(AsyncWebServerRequest* request) {
  String name = request->pathArg(0);
  Option* opt = findOption(name);
  if (!opt) {
    request->send(404, "application/json", "{}\n");
    return;
  }
  JsonDocument doc;
  appendOptionJson(doc.to<JsonObject>(), *opt);
  String json;
  serializeJson(doc, json);
  Serial.println("Responding with option: " + json);
  request->send(200, "application/json", json);
}

void handlePostOption(AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index,
                      size_t total) {
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

  JsonDocument responseDoc;
  responseDoc[name] = true;
  String resp;
  serializeJson(responseDoc, resp);
  Serial.println("Responding with update result: " + resp);
  request->send(200, "application/json", resp);
  return;
}

void setupWebServer() {
  server.serveStatic("/dist", SPIFFS, "/dist");
  server.serveStatic("/assets", SPIFFS, "/dist/assets");

  // Serve index.html for root
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
    request->send(SPIFFS, "/dist/index.html", "text/html");
  });

  server.on("/api/options", HTTP_GET, handleListOptions);
  server.on("^/api/options/([a-zA-Z0-9_-]+)$", HTTP_GET, handleGetOption);
  server.on(
      "^/api/options/([a-zA-Z0-9_-]+)$", HTTP_POST, [](AsyncWebServerRequest* request) {}, NULL,
      handlePostOption);

  server.begin();
}

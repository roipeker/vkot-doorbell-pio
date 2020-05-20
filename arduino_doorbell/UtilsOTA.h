#ifndef _UTILS_OTA_H
#define _UTILS_OTA_H
#include <ArduinoOTA.h>
void setupOTA() {
  Serial.println("Using OTA!");
  ArduinoOTA.setHostname("esp_doorbell");
  ArduinoOTA.onStart([]() {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH) {
      type = "sketch";
    } else {
      type = "filesystem";
    }
    Serial.println("Start updating " + type);
  });

  ArduinoOTA.onEnd([]() { Serial.println("OTA Ended"); });

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("OTA Progress: %u%%\r", progress / (total / 100));
  });

  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) {
      Serial.println("OTA Auth failed.");
    } else if (error == OTA_BEGIN_ERROR) {
      Serial.println("OTA Begin failed.");
    } else if (error == OTA_CONNECT_ERROR) {
      Serial.println("OTA Connection failed.");
    } else if (error == OTA_RECEIVE_ERROR) {
      Serial.println("OTA Recieve failed.");
    } else if (error == OTA_END_ERROR) {
      Serial.println("OTA End upload failed.");
    }
  });
  ArduinoOTA.begin();
  Serial.println();
  Serial.println("OTA initialized");
}

void loopOTA() { ArduinoOTA.handle(); }
#endif
// constants.h defines the custom vars, create that file with your configuration

// database.h defines firebase credentials
// #define FIREBASE_HOST "https://NAMEHERE.firebaseio.com"
// #define FIREBASE_AUTH "000000000000000000000000"

#include <Arduino.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266WebServer.h>  //Local WebServer used to serve the configuration portal
#include <ESP8266WiFi.h>
#include <FirebaseESP8266.h>
#include <WiFiManager.h>  //https://github.com/tzapu/WiFiManager WiFi Configuration Magic
#include <jled.h>

#include "ESP8266Ping.h"
#include "database.h"
#include "constants.h"

#ifdef USE_OTA
#include "UtilsOTA.h"
#endif

#define DEFAULT_RELAY_DURATION_MS 4000          // 4 sec door opens.
#define FACTORY_RESET_PRESS_TIMEOUT_MS 10000    // 8 seconds pressed.
#define DEFAULT_PRESS_COUNT_ACTIVATE 5          // 5 rings, can't be < 2.
#define DEFAULT_PRESS_COUNT_TIMEOUT_MS 4000     // in 4 secs, to open door
#define DEFAULT_WIFI_AP_TIMEOUT 40              // Wifi 30 SECONDS to fail on setup(),
                                                // and do offline
#define PING_SERVER_INTERVAL_MS 60000           // only when activates

IPAddress PING_IP(1, 1, 1, 1);                  // The remote ip to ping

bool pingServerEnabled = false;                 // only when activates WIFI
uint16_t BUTTON_DEBOUNCE = 50;                  // 50 ms
bool isRelayOn = false;
bool isButtonPressed = false;
bool isWifiConnected = false;
uint8_t hasWiFiConnection = -1;                 // 0 - 1
uint8_t hasInternetConnection = -1;             // 0 - 1
uint8_t lastWifiStatus = -1;                    // check for WL_CONNECTED

// log connection info and network status (noise).
unsigned long buttonLastDebounceTime = 0;
unsigned long wifiLastDebounceTime = 0;

// firebase realtime database.
FirebaseData fbData;
FirebaseData fbPushData;
FirebaseData fbLogsData;

String devId = WiFi.macAddress();
String fbPathNetStats = "devices/" + devId + "/netstats";
String fbPathState = "devices/" + devId + "/state";
String fbPathLogsPush = "devices/" + devId + "/logs/push";
String fbPathLogsLogin = "devices/" + devId + "/logs/login";
String fbPathLogsReset = "devices/" + devId + "/logs/reset";
String fbPathLogsOpen = "devices/" + devId + "/logs/open";  // activate system.

unsigned long ms;
unsigned long relayStartTime = 0;
unsigned long factoryResetStartTime = 0;  // hits FACTORY_RESET_PRESS_TIMEOUT_MS
unsigned long lastPingServerStartTime = 0;
unsigned long pressCountStartTime = 0;  // millis when first press.

uint8_t pressCountActivateRelay = DEFAULT_PRESS_COUNT_ACTIVATE;
uint16_t relayTimeoutDelay = DEFAULT_RELAY_DURATION_MS;
uint16_t pressCountTimeout = DEFAULT_PRESS_COUNT_TIMEOUT_MS;

// global flag to change status of the push system activation.
bool systemEnabled = true;

uint8_t resetBlinkState = -1;  // factory reset LED bomb blink state.
uint8_t pressCount = 0;        // this is the actual counter.

// todo: improve led states.
JLed ledPrimary = JLed(PIN_STATUS_LED);
JLed ledSecondary = JLed(PIN_STATUS_LED);

void printResult(FirebaseData &data) {
  if (data.dataType() == "int")
    Serial.println(data.intData());
  else if (data.dataType() == "float")
    Serial.println(data.floatData(), 5);
  else if (data.dataType() == "double")
    printf("%.9lf\n", data.doubleData());
  else if (data.dataType() == "boolean")
    Serial.println(data.boolData() == 1 ? "true" : "false");
  else if (data.dataType() == "string")
    Serial.println(data.stringData());
  else if (data.dataType() == "json") {
    Serial.println();
    FirebaseJson &json = data.jsonObject();
    // Print all object data
    Serial.println("Pretty printed JSON data:");
    String jsonStr;
    json.toString(jsonStr, true);
    Serial.println(jsonStr);
    Serial.println();
    Serial.println("Iterate JSON data:");
    Serial.println();
    size_t len = json.iteratorBegin();
    String key, value = "";
    int type = 0;
    for (size_t i = 0; i < len; i++) {
      json.iteratorGet(i, type, key, value);
      Serial.print(i);
      Serial.print(", ");
      Serial.print("Type: ");
      Serial.print(type == FirebaseJson::JSON_OBJECT ? "object" : "array");
      if (type == FirebaseJson::JSON_OBJECT) {
        Serial.print(", Key: ");
        Serial.print(key);
      }
      Serial.print(", Value: ");
      Serial.println(value);
    }
    json.iteratorEnd();
  } else if (data.dataType() == "array") {
    Serial.println();
    // get array data from FirebaseData using FirebaseJsonArray object
    FirebaseJsonArray &arr = data.jsonArray();
    // Print all array values
    Serial.println("Pretty printed Array:");
    String arrStr;
    arr.toString(arrStr, true);
    Serial.println(arrStr);
    Serial.println();
    Serial.println("Iterate array values:");
    Serial.println();
    for (size_t i = 0; i < arr.size(); i++) {
      Serial.print(i);
      Serial.print(", Value: ");

      FirebaseJsonData &jsonData = data.jsonData();
      // Get the result data from FirebaseJsonArray object
      arr.get(jsonData, i);
      if (jsonData.typeNum == FirebaseJson::JSON_BOOL)
        Serial.println(jsonData.boolValue ? "true" : "false");
      else if (jsonData.typeNum == FirebaseJson::JSON_INT)
        Serial.println(jsonData.intValue);
      else if (jsonData.typeNum == FirebaseJson::JSON_DOUBLE)
        printf("%.9lf\n", jsonData.doubleValue);
      else if (jsonData.typeNum == FirebaseJson::JSON_STRING ||
               jsonData.typeNum == FirebaseJson::JSON_NULL ||
               jsonData.typeNum == FirebaseJson::JSON_OBJECT ||
               jsonData.typeNum == FirebaseJson::JSON_ARRAY)
        Serial.println(jsonData.stringValue);
    }
  }
}

void setRelayOnDuration(uint16_t duration) {
  relayTimeoutDelay = duration == 0 ? DEFAULT_RELAY_DURATION_MS : duration;
}

void setPressCountActivation(uint8_t count) {
  pressCountActivateRelay = count < 2 ? DEFAULT_PRESS_COUNT_ACTIVATE : count;
}

void setPressCountTimeout(uint16_t duration) {
  pressCountTimeout =
      duration == 0 ? DEFAULT_PRESS_COUNT_TIMEOUT_MS : pressCountTimeout;
}

void sendLogin() {
  Firebase.pushTimestamp(fbLogsData, fbPathLogsLogin);
  // update IP once.
  WiFiClient client;
  HTTPClient http;

  http.begin(client, "http://api.ipify.org/");
  http.GET();
  String ipPayload = http.getString();
  http.end();
  Serial.print("IP is: ");
  Serial.println(ipPayload);
  String gatewayIP = WiFi.gatewayIP().toString();
  String localIP = WiFi.localIP().toString();
  int channel = WiFi.channel();
  String ssid = WiFi.SSID();
  String bssid = WiFi.BSSIDstr();
  int rssi = WiFi.RSSI();
  // Serial.println();
  // Serial.print("gateway ip ");
  // Serial.println(gatewayIP);
  // Serial.print("localIP ");
  // Serial.println(localIP);
  // Serial.print("channel ");
  // Serial.println(channel);
  // Serial.print("ssid ");
  // Serial.println(ssid);
  // Serial.print("bssid ");
  // Serial.println(bssid);
  // Serial.print("RSSI ");
  // Serial.println(rssi);
  // Serial.println();
  
  FirebaseJson json1;

  json1.set("public_ip", ipPayload);
  json1.set("gateway_ip", gatewayIP);
  json1.set("local_ip", localIP);
  json1.set("channel", channel);
  json1.set("ssid", ssid);
  json1.set("bssid", bssid);
  json1.set("rssi", rssi);
  if (Firebase.set(fbPushData, fbPathNetStats, json1)) {
    Serial.println("network stats updated.");
  }
}

// logic
void sendPressNoti() { 
  Firebase.pushTimestamp(fbLogsData, fbPathLogsPush);
}

void turnRelay(bool flag) {
  if (isRelayOn == flag) return;
  isRelayOn = flag;
  Serial.print("Turn relay: ");
  Serial.println(isRelayOn);
  if (flag) {
    // make led stay on for this much.
    ledSecondary.Blink(relayTimeoutDelay, 10).Repeat(1);
  }
  if (PIN_RELAY > -1) {
    digitalWrite(PIN_RELAY, flag ? LOW : HIGH);
  }
}

void turnRelayRemote(bool flag) {
  // todo: check if we need to update time here.
  if (isRelayOn == flag) {
    return;
  }

  // reset counter to avoid overlapping of remote command.
  pressCountStartTime = 0;
  pressCount = 0;

  if (flag) {
    relayStartTime = ms;
  }
  turnRelay(flag);
}

void execFactoryReset() {
  Firebase.pushTimestamp(fbLogsData, fbPathLogsReset);
  Serial.println("Factory reset hit.");
  delay(500);
  WiFiManager wifiManager;
  wifiManager.resetSettings();
  delay(1000);
  ESP.restart();
}

void stopFactoryResetTimeout() {
  factoryResetStartTime = 0;
  resetBlinkState = -1;
}

void startFactoryResetTimeout(bool flag) {
  factoryResetStartTime = flag ? ms : 0;
  resetBlinkState = -1;
  ledSecondary.Stop();
}

void checkFactoryResetTimeout() {
  if (factoryResetStartTime == 0) return;

  int _diffTime = (factoryResetStartTime + FACTORY_RESET_PRESS_TIMEOUT_MS) - ms;
  int _resetBlinkState = -1;

  if (_diffTime < 2000) {
    _resetBlinkState = 2;
  } else if (_diffTime < 4000) {
    _resetBlinkState = 4;
  } else if (_diffTime < 6000) {
    _resetBlinkState = 5;
  }

  if (resetBlinkState != _resetBlinkState) {
    resetBlinkState = _resetBlinkState;
    ledSecondary.Stop();
    switch (resetBlinkState) {
      case 5:
        ledSecondary.Blink(300, 300).Forever();
        break;
      case 4:
        ledSecondary.Blink(150, 150).Forever();
        break;
      case 2:
        ledSecondary.Blink(50, 50).Forever();
        break;
      default:
        break;
    }
  }

  if (factoryResetStartTime + FACTORY_RESET_PRESS_TIMEOUT_MS <= ms) {
    factoryResetStartTime = 0;
    resetBlinkState = -1;
    ledSecondary.Stop();
    Serial.println("Boom! hard reset!");
    execFactoryReset();
  }
}

// manual delay of primary LED pattern (wifi on/off)
// todo: make it consistent in the system to avoid failures.
// double primLedPauseUntil = 0;
// void pausePrimaryLed(int nextTime) { primLedPauseUntil = ms + nextTime; }

void checkPressCountTimeout() {
  // only if system enabled
  if (!systemEnabled || pressCountStartTime == 0) return;
  bool isValidTime = ms < pressCountStartTime + pressCountTimeout;
  if (!isValidTime) {
    if (pressCount == pressCountActivateRelay) {
      Serial.println("Press valid!!!!");
      Serial.println();
      Serial.print("Press count hit. Activate relay for ");
      Serial.print(relayTimeoutDelay);
      Serial.print("ms.");
      Serial.println();
      turnRelayRemote(true);
      Firebase.pushTimestamp(fbLogsData, fbPathLogsOpen);
      // or use PatchData with JSON to be silent?
      // https://github.com/mobizt/Firebase-ESP8266#patch-data
      Firebase.setInt(fbPushData, fbPathState + "/on", 1);
    } else {
      Serial.println("Press count timed out. Reset counter.");
      ledSecondary.Blink(120, 120).Repeat(4);
    }
    pressCountStartTime = 0;
    pressCount = 0;
  }
}

void onButtonPressed(bool flag) {
  Serial.println(flag ? "Button pressed" : "Button released");

  // factory reset, only if relay is off.
  if (!isRelayOn) {
    startFactoryResetTimeout(flag);
  }

  if (!flag) {
    // system is not enabled.
    if (!systemEnabled) {
      return;
    }

    /// if relay is currently ON, skip this.
    if (isRelayOn) {
      return;
    }

    if (pressCountStartTime == 0) {
      pressCountStartTime = ms;
      pressCount = 0;
    }
    ++pressCount;
    if (pressCount > pressCountActivateRelay) {
      Serial.println();
      Serial.print("Press count overflow: ");
      Serial.print(pressCount);
      Serial.print("/");
      Serial.println(pressCountActivateRelay);
    }

  } else {
    // send notification only once if it's NOT running the detector.
    if (pressCountStartTime == 0) {
      sendPressNoti();
    }
  }
}

void readButtonState() {
  bool _pressed = digitalRead(PIN_BUTTON) == LOW;
  if (_pressed != isButtonPressed) {
    bool _debounce = false;
    if ((ms - buttonLastDebounceTime) <= BUTTON_DEBOUNCE) {
      _debounce = true;
    }
    buttonLastDebounceTime = ms;
    if (_debounce) return;
    isButtonPressed = _pressed;
    onButtonPressed(_pressed);
  }
}

void setupPins() {
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  pinMode(PIN_RELAY, OUTPUT);
  pinMode(PIN_STATUS_LED, OUTPUT);
  isRelayOn = true;
  turnRelay(false);
  if (LED_INV) {
    ledPrimary = ledPrimary.LowActive();
    ledSecondary = ledSecondary.LowActive();
  }
}

void wifiManConfigModeCallback(WiFiManager *myWiFiManager) {
  Serial.println("Entered config mode");
  Serial.println(WiFi.softAPIP());
  // stop firebase?
  // Serial.println(myWiFiManager->getConfigPortalSSID());
}

void setupWifiManager() {
  // locally.
  WiFiManager wifiManager;
  
  // String msg =
  //     "<p><strong>vkot</strong> is a cool and unique smart system!</p><br>Take "
  //     "note of your unique smartbell ID and link it to your account:";
  // String html =
  //     "<input type='text' "
  //     "onmousedown='this.style.outline=\"none\";this.style.opacity=\"0.5\"' "
  //     "onclick='this.style.outline=\"none\";this.style.opacity=\"1.0\";this."
  //     "select();this.setSelectionRange(0, "
  //     "99999);document.execCommand(\"copy\");var "
  //     "msg=document.getElementById(\"copy_msg\");msg.style.visibility="
  //     "\"visible\";this.setSelectionRange(0, 0);setTimeout(function "
  //     "(){msg.style.visibility=\"hidden\";},1000);' style='border: 1px solid "
  //     "black;border-radius: "
  //     "12px;padding:8px;text-align:center;font-size:28px;font-weight:bold;' "
  //     "value='" +
  //     devId +
  //     "' id='myid' readonly/><p id='copy_msg' "
  //     "style='visibility:hidden;width:120px;background-color: "
  //     "black;color:white;border-radius: "
  //     "6px;text-align:center;padding:10px;font-size: 12px;'>device id "
  //     "copied</p>";
  // WiFiManagerParameter custom_text(msg.c_str());
  // wifiManager.addParameter(&custom_text);
  // WiFiManagerParameter copy_text_html(html.c_str());
  // wifiManager.addParameter(&copy_text_html);
//  wifiManager.setAPStaticIPConfig(IPAddress(10,0,1,1), IPAddress(10,0,1,1), IPAddress(255,255,255,0));
//  wifiManager.setScanDispPerc(true);
  // wifiManager.setRemoveDuplicateAPs(false);
  // wifiManager.setWebPortalClientCheck(true);
  wifiManager.setConfigPortalBlocking(true);
  Serial.println("Wifi is saved:");
  Serial.print(wifiManager.getWiFiIsSaved());
  // wifiManager.setConfigPortalTimeout(60*60*2);
//  wifiManager.setAPCallback(wifiManConfigModeCallback);
  if (!wifiManager.autoConnect("VKOT-DOORBELL")) {
    Serial.println("Failed to connect to network/AP and hit timeout.");
    //   Reset and try again, or maybe put it to deep sleep?
    //   ESP.reset();
    delay(1000);
    Serial.println("Going on without WiFi :( ");
  }
}

void setupWifi() {
  Serial.print("device id=");
  Serial.println(devId);
  setupWifiManager();
}

void setupFirebase() {
  Serial.println("setting up firebase");
  Firebase.begin(FIREBASE_HOST, FIREBASE_AUTH);
  // Firebase.reconnectWiFi(true);
  if (!Firebase.beginStream(fbData, fbPathState)) {
    Serial.println("Could not begin stream, REASON: " + fbData.errorReason());
    Serial.println();
  }
}

void checkRelayOnTimer() {
  if (relayStartTime > 0 && ms >= relayStartTime + relayTimeoutDelay) {
    relayStartTime = 0;
    Firebase.setInt(fbPushData, fbPathState + "/on", 0);
    turnRelay(false);
  }
}

void sysEnabledBlink() {
  if (systemEnabled) {
    // TODO: implement led system.
    // ledController.Blink(100, 100).Repeat(3).DelayAfter(1000);
  }
}

void setConfigSysEnabled(bool flag) {
  systemEnabled = flag;
  sysEnabledBlink();
  Serial.println("System enabled: ");
  Serial.print(systemEnabled);
  Serial.println();
}

void checkCommand(int cmdId) {
  if (cmdId == 0) return;
  if (cmdId == 1) {
    // ping / pong, update to notify client.
  } else if (cmdId == 2) {
    if (Firebase.setInt(fbPushData, fbPathNetStats + "/rssi", WiFi.RSSI())) {
      Serial.println("Netstats RSSI updated.");
    }
  }
  Firebase.setInt(fbPushData, fbPathState + "/cmd", 0);
}

void readFirebaseStream() {
  Serial.println(fbData.dataType());
  Serial.println("Stream Data Available...");
  Serial.println("STREAM PATH: " + fbData.streamPath());
  Serial.println("EVENT PATH: " + fbData.dataPath());
  Serial.println("DATA TYPE: " + fbData.dataType());
  Serial.println("EVENT TYPE: " + fbData.eventType());

  printResult(fbData);

  if (fbData.dataType() == "json") {
    FirebaseJsonData jsonObj;
    FirebaseJson &json = fbData.jsonObject();

    json.get(jsonObj, "/on_timeout");
    setRelayOnDuration(jsonObj.intValue);

    json.get(jsonObj, "/on");
    if (jsonObj.success) {
      turnRelayRemote(jsonObj.intValue == 1);
    }

    json.get(jsonObj, "/press_count");
    if (jsonObj.success) {
      setPressCountActivation(jsonObj.intValue);
    }

    json.get(jsonObj, "/press_timeout");
    if (jsonObj.success) {
      setPressCountTimeout(jsonObj.intValue);
    }

    json.get(jsonObj, "/enabled");
    if (jsonObj.success) {
      setConfigSysEnabled(jsonObj.intValue == 1);
    }

    json.get(jsonObj, "/cmd");
    if (jsonObj.success) {
      checkCommand(jsonObj.intValue);
    }

  } else if (fbData.dataType() == "int") {
    String path = fbData.dataPath();
    if (path == "/on_timeout") {
      // how much time in milliseconds to maintain on when send on command.
      setRelayOnDuration(fbData.intData());
    } else if (path == "/on") {
      // we expect 0-1
      turnRelayRemote(fbData.intData() == 1);
    } else if (path == "/press_count") {
      setPressCountActivation(fbData.intData());
    } else if (path == "/press_timeout") {
      setPressCountTimeout(fbData.intData());
    } else if (path == "/enabled") {
      setConfigSysEnabled(fbData.intData() == 1);
    } else if (path == "/cmd") {
      checkCommand(fbData.intData());
    }
  }
}

void loopFirebase() {
  if (!hasInternetConnection) {
    return;
  }
  if (!Firebase.readStream(fbData)) {
    Serial.println("Can't read stream data, REASON: " + fbData.errorReason());
  }
  if (fbData.streamTimeout()) {
    Serial.println("Stream timeout, resume streaming...");
  }
  if (fbData.streamAvailable()) {
    readFirebaseStream();
  }
}

void setup() {
  Serial.begin(115200, SERIAL_8N1, SERIAL_TX_ONLY); //No RX
  delay(100);

#ifdef USE_OTA
  Serial.println("USING OTA.");
  setupOTA();
#else
  Serial.println("NOT using OTA.");
#endif

  setupPins();
  ledSecondary.On().Update();
  setupWifi();
  ledSecondary.Off().Update();
  // moved this into internet connectivity.
  // setupFirebase();
  // sendLogin();
  Serial.println("setup() finished.");
}

void checkPingServer() {
  int _hasInternet = Ping.ping(PING_IP, 1) ? 1 : 0;
  Serial.println();
  Serial.print("Ping DNS result=");
  Serial.print(_hasInternet ? "TRUE" : "FALSE");
  Serial.print(", avg time=");
  Serial.print(Ping.averageTime());
  Serial.println();
  if (hasInternetConnection != _hasInternet) {
    hasInternetConnection = _hasInternet;
    onInternetStatusChange();
  }
}

void onInternetStatusChange() {
  Serial.println();
  Serial.print("Internet status changed to: ");
  Serial.print(hasInternetConnection);
  Serial.println();

  // status not defined yet.
  if (hasInternetConnection == -1) return;

  if (hasInternetConnection == 1) {
    // send login.
    setupFirebase();
    sendLogin();
    ledPrimary.Breathe(3000).DelayAfter(400).Forever();
  } else {
    ledPrimary.Blink(80, 1000).Forever();
  }
}

void onWifiConnectionChange() {
  pingServerEnabled = isWifiConnected;
  Serial.println(isWifiConnected ? "WiFi Connected" : "WiFi Disconnected");
  // we have to ping server to know if we have internet.
  if (isWifiConnected) {
    lastPingServerStartTime = ms;
    checkPingServer();
  }
  // if (isWifiConnected) {
  //   // wait 500ms to make the next request.
  //   // lastPingServerStartTime = ms - 500;
  //   checkPingServer();
  // } else {
  //   checkPingServer();
  // }
}

void loopPingServer() {
  // only ping where's wifi connection.
  if (!pingServerEnabled) return;
  if (ms - lastPingServerStartTime >= PING_SERVER_INTERVAL_MS) {
    checkPingServer();
    lastPingServerStartTime = ms;
  }
}

void loopWifi() {
  // each 500ms check status.
  if (ms - wifiLastDebounceTime >= 500) {
    wifiLastDebounceTime = ms;
    int _status = WiFi.status();
    if (_status != lastWifiStatus) {
      // WiFi status changed.
      lastWifiStatus = _status;
      int _isConnected = _status == WL_CONNECTED ? 1 : 0;
      if (hasWiFiConnection != _isConnected) {
        hasWiFiConnection = _isConnected;
        isWifiConnected = hasWiFiConnection == 1;
        onWifiConnectionChange();
      }
#ifdef APP_DEBUG
      Serial.println();
      Serial.print("Wifi status changed to: ");
      Serial.print(_status);
#endif
    }
    // Serial.println(WiFi.RSSI());
    // Serial.println("--------");
  }
}

void loopLed() {
  if (ledSecondary.IsRunning()) {
    ledSecondary.Update();
  } else {
    // todo : related to delay on primaryLed. Not implemented.
    // if (ms < primLedPauseUntil) {
    //   return;
    // } else {
    // reset
    // if (primLedPauseUntil > 0) primLedPauseUntil = 0;
    // }
    ledPrimary.Update();
  }
}

void loop() {
  ms = millis();
  // Serial.print(".");
  if (ms < 0) ms = 0;
  loopWifi();
  loopPingServer();
  checkFactoryResetTimeout();
  checkRelayOnTimer();
  readButtonState();
  checkPressCountTimeout();
  if( isWifiConnected ){
    loopFirebase();
  }
  loopLed();
#ifdef USE_OTA
  loopOTA();
#endif
  delay(1);
}

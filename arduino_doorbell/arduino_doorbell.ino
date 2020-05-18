#include <Arduino.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266WebServer.h>  //Local WebServer used to serve the configuration portal
#include <ESP8266WiFi.h>
#include <FirebaseESP8266.h>
#include <WiFiManager.h>  //https://github.com/tzapu/WiFiManager WiFi Configuration Magic
#include <jled.h>
#include "constants.h"

// constants.h defines the custom vars
// #define FIREBASE_HOST "https://NAMEHERE.firebaseio.com"
// #define FIREBASE_AUTH "000000000000000000000000"
// #define PIN_RELAY 0
// #define PIN_STATUS_LED 1
// #define PIN_BUTTON 2
// #define LED_INV 0

#define IP_API_URL "http://api.ipify.org/"
#define DEFAULT_BUTTON_DURATION_MS 2000      // 2 sec on.
#define FACTORY_RESET_PRESS_TIMEOUT_MS 8000  // 8 seconds pressed.

bool isRelayOn = false;
bool isWifiOn = false;
bool isButtonPressed = false;

int buttonDebounceDelay = 900;  // 50 ms
volatile long buttonLastDebounceTime = 0;

// firebase realtime database.
FirebaseData fbData;
FirebaseData fbPushData;
FirebaseData fbLogsData;

String devId = WiFi.macAddress();
String fbPathConfig = "devices/" + devId + "/config";
String fbPathState = "devices/" + devId + "/state";
String fbPathLogsPush = "devices/" + devId + "/logs/push";
String fbPathLogsLogin = "devices/" + devId + "/logs/login";
String fbPathLogsReset = "devices/" + devId + "/logs/reset";
String fbPathLogsOpen = "devices/" + devId + "/logs/open";  // activate system.

unsigned long ms;
unsigned long relayStartTime = 0;
unsigned long factoryResetStartTime = 0;  // hits FACTORY_RESET_PRESS_TIMEOUT_MS

int relayTimeoutDelay = DEFAULT_BUTTON_DURATION_MS;

// global flag to change status of the push system activation!
bool systemEnabled = true;

int _currPressCount = 0;  // this is the actual counter.
int pressCountActivateRelay = 0;
int pressCountTimeout = 3000;           // ms
unsigned long pressCountStartTime = 0;  // millis when first press.

HTTPClient http;

// check JLED Later.
JLed ledController = JLed(PIN_STATUS_LED);

// JLed leds[] = {ledRef.Breathe(500).Repeat(3), ledRef.FadeOff(500).Repeat(3),
//                ledRef.Blink(100, 100).Repeat(3),
//                ledRef.FadeOn(500).Repeat(3), ledRef.Off()};
// JLedSequence sequence(JLedSequence::eMode::SEQUENCE, leds);

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

void setRelayOnDuration(int duration) {
  if (duration == 0) {
    duration = DEFAULT_BUTTON_DURATION_MS;
  }
  relayTimeoutDelay = duration;
}

void sendLogin() {
  Firebase.pushTimestamp(fbLogsData, fbPathLogsLogin);
  // update IP once.
  http.begin(IP_API_URL);
  http.GET();
  String ipPayload = http.getString();
  http.end();
  Serial.print("IP is: ");
  Serial.println(ipPayload);
  if (Firebase.setString(fbPushData, fbPathConfig + "/ip", ipPayload)) {
    Serial.println("IP updated");
  }
}

// logic
void sendPressNoti() {
  // Serial.println("sending noti...");
  // push timestamp to
  Firebase.pushTimestamp(fbLogsData, fbPathLogsPush);
}

void turnRelay(bool flag) {
  if (isRelayOn == flag) return;
  isRelayOn = flag;
  Serial.print("Turn relay: ");
  Serial.println(isRelayOn);
  digitalWrite(PIN_RELAY, flag ? LOW : HIGH);
}

void turnRelayRemote(bool flag) {
  // todo: check if we need to update time here.
  if (isRelayOn == flag) {
    return;
  }

  // reset counter to avoid overlapping of remote command.
  pressCountStartTime = 0;
  _currPressCount = 0;

  if (flag) {
    // start relay remotly.
    relayStartTime = ms;
  }
  turnRelay(flag);
}

void execFactoryReset() {
  return ;
  Firebase.pushTimestamp(fbLogsData, fbPathLogsReset);
  Serial.println("Factory reset hit.");
  delay(500);
  WiFiManager wifiManager;
  wifiManager.resetSettings();
  delay(1000);
  ESP.restart();
}

void startFactoryResetTimeout(bool flag) {
  factoryResetStartTime = flag ? ms : 0;
}

// int factoryResetBlinkStep = 0;

void checkFactoryResetTimeout() {
  if (factoryResetStartTime == 0) return;

  // int maxTime = factoryResetStartTime + FACTORY_RESET_PRESS_TIMEOUT_MS;
  // if (ms >= maxTime - 2000 && factoryResetBlinkStep != 1) {
  //   factoryResetBlinkStep = 1;
  //   Serial.println("2 sec to timeout!!!");
  //   ledController.Stop();
  //   ledController.Blink(100, 100).Forever();
  // } else if (ms >= factoryResetStartTime + FACTORY_RESET_PRESS_TIMEOUT_MS -
  //                      3000 &&
  //            factoryResetBlinkStep != 3) {
  //   factoryResetBlinkStep = 3;
  //   Serial.println("3 sec to timeout");
  //   ledController.Stop();
  //   ledController.FadeOff(200).Forever();
  // } else if (ms >= factoryResetStartTime + FACTORY_RESET_PRESS_TIMEOUT_MS -
  //                      4000 &&
  //            factoryResetBlinkStep != 4) {
  //   Serial.println("4 sec to timeout");
  //   factoryResetBlinkStep = 4;
  //   ledController.Stop();
  //   ledController.FadeOff(300).Forever();
  // }

  if (factoryResetStartTime + FACTORY_RESET_PRESS_TIMEOUT_MS <= ms) {
    factoryResetStartTime = 0;
    // factoryResetBlinkStep = 0;
    ledController.Reset().Stop().Off().Update();
    execFactoryReset();
  }
}

void checkPressCountTimeout() {
  // only if system enabled
  if (!systemEnabled || pressCountStartTime == 0) return;
  bool isValidTime = ms < pressCountStartTime + pressCountTimeout;
  if (!isValidTime) {
    pressCountStartTime = 0;
    _currPressCount = 0;
    Serial.println("Press count relay timed out. Reset counter.");
    ledController.FadeOff(300).Repeat(3);
    // apply delay to keep running the other pattern...
  }
}

void onButtonPressed(bool flag) {
  Serial.println(flag ? "Button pressed" : "Button released");
  ledController.Stop();
  startFactoryResetTimeout(flag);

  // turnRelay(flag);
  if (!flag) {
    // system is not enabled.
    if (!systemEnabled) {
      return;
    }
    if (pressCountStartTime == 0) {
      pressCountStartTime = ms;
      _currPressCount = 0;
    }
    if (++_currPressCount >= pressCountActivateRelay) {
      Serial.println("Press count hit. Activate relay for ");
      Serial.print(relayTimeoutDelay);
      Serial.print("ms.");
      Serial.println();
      turnRelayRemote(true);
      Firebase.pushTimestamp(fbLogsData, fbPathLogsOpen);
      // or use PatchData with JSON to be silent?
      // https://github.com/mobizt/Firebase-ESP8266#patch-data
      Firebase.setInt(fbPushData, fbPathState + "/on", 1);
      pressCountStartTime = 0;
      _currPressCount = 0;
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
    if (ms - buttonLastDebounceTime <= buttonDebounceDelay) _debounce = true;
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
    ledController = ledController.LowActive();
  }
  // on by default during setup();
  ledController.On().Update();
}

void wifiManConfigModeCallback(WiFiManager *myWiFiManager) {
  Serial.println("Entered config mode");
  Serial.println(WiFi.softAPIP());
  Serial.println(myWiFiManager->getConfigPortalSSID());
}

void setupWifiManager() {
  // locally.
  WiFiManager wifiManager;
  String msg =
      "<p><strong>vkot</strong> is a cool and unique smart system!</p><br>Take "
      "note of your unique smartbell ID and link it to your account:";
  String html =
      "<input type='text' "
      "onmousedown='this.style.outline=\"none\";this.style.opacity=\"0.5\"' "
      "onclick='this.style.outline=\"none\";this.style.opacity=\"1.0\";this."
      "select();this.setSelectionRange(0, "
      "99999);document.execCommand(\"copy\");var "
      "msg=document.getElementById(\"copy_msg\");msg.style.visibility="
      "\"visible\";this.setSelectionRange(0, 0);setTimeout(function "
      "(){msg.style.visibility=\"hidden\";},1000);' style='border: 1px solid "
      "black;border-radius: "
      "12px;padding:8px;text-align:center;font-size:28px;font-weight:bold;' "
      "value='" +
      devId +
      "' id='myid' readonly/><p id='copy_msg' "
      "style='visibility:hidden;width:120px;background-color: "
      "black;color:white;border-radius: "
      "6px;text-align:center;padding:10px;font-size: 12px;'>device id "
      "copied</p>";

  WiFiManagerParameter custom_text(msg.c_str());
  wifiManager.addParameter(&custom_text);

  WiFiManagerParameter copy_text_html(html.c_str());
  wifiManager.addParameter(&copy_text_html);

  //  wifiManager.setCustomHeadElement(
  //      "<style>html{filter: invert(100%); -webkit-filter: "
  //      "invert(100%);}</style>");

  // set callback that gets called when connecting to previous WiFi fails, and
  // enters Access Point mode
  wifiManager.setAPCallback(wifiManConfigModeCallback);

  if (!wifiManager.autoConnect("VKOT-DOORBELL")) {
    Serial.println("failed to connect and hit timeout");
    // reset and try again, or maybe put it to deep sleep
    ESP.reset();
    delay(1000);
  }

  // if you get here you have connected to the WiFi
  Serial.println("connected...yeey :)");
  Serial.print("Connected with IP: ");
  Serial.println(WiFi.localIP());
}

void setupWifi() {
  Serial.print("device id=");
  Serial.println(devId);
  setupWifiManager();
}

void setupFirebase() {
  Serial.println("setting up firebase");
  Firebase.begin(FIREBASE_HOST, FIREBASE_AUTH);
  Firebase.reconnectWiFi(true);
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

void setConfigCount(int val) {
  if (val <= 0) val = 1;
  pressCountActivateRelay = val;
}
void setConfigTimeout(int val) { pressCountTimeout = val; }
void setConfigSysEnabled(bool flag) {
  systemEnabled = flag;
  sysEnabledBlink();
  Serial.println("System enabled: ");
  Serial.print(systemEnabled);
  Serial.println();
}

void readFirebaseStream() {
  Serial.println(fbData.dataType());
  Serial.println("Stream Data Available...");
  Serial.println("STREAM PATH: " + fbData.streamPath());
  Serial.println("EVENT PATH: " + fbData.dataPath());
  Serial.println("DATA TYPE: " + fbData.dataType());
  Serial.println("EVENT TYPE: " + fbData.eventType());
  printResult(fbData);

  // this conflicts with the emulated "relay"...
  // use in production.
  // ledController.Breathe(140).Repeat(2);

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
      setConfigCount(jsonObj.intValue);
    }

    json.get(jsonObj, "/press_timeout");
    if (jsonObj.success) {
      setConfigTimeout(jsonObj.intValue);
    }

    json.get(jsonObj, "/enabled");
    if (jsonObj.success) {
      setConfigSysEnabled(jsonObj.intValue == 1);
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
      setConfigCount(fbData.intData());
    } else if (path == "/press_timeout") {
      setConfigTimeout(fbData.intData());
    } else if (path == "/enabled") {
      setConfigSysEnabled(fbData.intData() == 1);
    }
  }
}

void loopFirebase() {
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
  Serial.begin(115200, SERIAL_8N1, SERIAL_TX_ONLY);
  delay(100);
  setupPins();
  setupWifi();
  setupFirebase();
  sendLogin();

  ledController.Off().Update();
  Serial.println("setup() finished.");
}

void loop() {
  ms = millis();
  if (ms < 0) ms = 0;
  checkFactoryResetTimeout();
  checkRelayOnTimer();
  readButtonState();
  checkPressCountTimeout();
  loopFirebase();
  ledController.Update();
  delay(1);
  // sequence.Update();
}

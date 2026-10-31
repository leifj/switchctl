#include <Arduino.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <WiFiManager.h>
#include <ESPAsyncWebServer.h>
#include <SPIFFS.h>
#include <Update.h>
#include <ESPmDNS.h>
#include <TimerEvent.h>

#define FW_VERSION "1.1"

AsyncWebServer server(80);
#ifdef ENABLE
#define ENABLE_PIN 12
#define AUTO_OFF_TIME 10*60*1000
unsigned int autoOffPeriod = AUTO_OFF_TIME;
bool enabled = true;
TimerEvent autoOff;
#endif
int cur_pin;

static const int pins[] = {27,14,32,33,25,26};
static const int PIN_COUNT = sizeof(pins) / sizeof(pins[0]);

// --- OTA -----------------------------------------------------------------
// Custom handler instead of ESPAsyncHTTPUpdateServer. The library starts a
// filesystem update with SPIFFS.totalBytes(), which is smaller than the
// partition; the full-size spiffs.bin is then truncated and the filesystem is
// corrupted. Using UPDATE_SIZE_UNKNOWN makes Update use the partition size.
static bool otaAuthorized = false;
static bool otaFilesystem = false;
static bool otaFailed = false;
static String otaError;

static void otaFail() {
  otaFailed = true;
  otaError = Update.errorString();
  Serial.printf("OTA error: %s\n", otaError.c_str());
  Update.abort();
  if (otaFilesystem) {
    // Best effort: the image may be unusable after a partial write.
    SPIFFS.begin(false);
  }
}

static const char otaPage[] PROGMEM = R"rawliteral(
<!doctype html><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>switchctl update</title>
<style>body{font-family:system-ui,sans-serif;max-width:34rem;margin:3rem auto;padding:0 1rem}
fieldset{margin:1rem 0}button{margin-top:.5rem}</style>
<h1>switchctl update</h1>
<form method="post" action="/update?name=firmware" enctype="multipart/form-data">
<fieldset><legend>Firmware</legend><input type="file" name="firmware" accept=".bin" required><br><button>Upload firmware</button></fieldset></form>
<form method="post" action="/update?name=filesystem" enctype="multipart/form-data">
<fieldset><legend>Filesystem</legend><input type="file" name="filesystem" accept=".bin" required><br><button>Upload filesystem</button></fieldset></form>
<p><a href="/">Back to status</a></p>
)rawliteral";

static void otaRequest(AsyncWebServerRequest *request) {
  if (!otaAuthorized) {
    request->requestAuthentication();
    return;
  }
  if (otaFailed || Update.hasError()) {
    String msg = F("Update failed: ");
    msg += otaError.length() ? otaError : Update.errorString();
    request->send(500, "text/plain", msg);
    return;
  }
  request->send(200, "text/plain", F("Update Success! Rebooting..."));
  delay(1000);
  ESP.restart();
}

static void otaUpload(AsyncWebServerRequest *request, String filename,
                      size_t index, uint8_t *data, size_t len, bool final) {
  if (index == 0) {
    otaFailed = false;
    otaError = "";
    otaFilesystem = false;
    otaAuthorized = request->authenticate(OTA_USER, OTA_PASSWORD);
    if (!otaAuthorized) {
      return;
    }

    String kind = "firmware";
    if (request->hasParam("name")) {
      kind = request->getParam("name")->value();
    }
    otaFilesystem = (kind == "filesystem");

    if (otaFilesystem) {
      // Unmount before writing raw sectors: a mounted SPIFFS keeps stale
      // metadata in RAM and will not mount cleanly after the raw write.
      SPIFFS.end();
    }

    if (!Update.begin(UPDATE_SIZE_UNKNOWN, otaFilesystem ? U_SPIFFS : U_FLASH)) {
      otaFail();
      return;
    }
  }
  if (otaAuthorized && !otaFailed) {
    if (len && !Update.write(data, len)) {
      otaFail();
      return;
    }
    if (final && !Update.end(true)) {
      otaFail();
      return;
    }
  }
}

void addBool(JsonDocument *doc, String name, bool value) {
  if (value) {
    (*doc)[name].set("true");
  } else {
    (*doc)[name].set("false");
  }
}

void addString(JsonDocument *doc, String name, String value) {
    (*doc)[name].set(value);
}

#ifdef ENABLE
void disableSwitch() {
    digitalWrite(ENABLE_PIN,LOW);
    enabled = false;
}

void enableSwitch() {
    digitalWrite(ENABLE_PIN,HIGH);
    enabled = true;
    autoOff.set(autoOffPeriod,disableSwitch);
}
#endif

void off(bool disable) {
#ifdef ENABLE
  if (disable) {
    disableSwitch();
  }
#endif
  for (auto pin: pins) {
    digitalWrite(pin,HIGH);
  }
  cur_pin = -1;
}

void on(int pin) {
  if (pin < 0 || pin >= PIN_COUNT) {
    return;
  }
  off(false);
  Serial.printf("setting pad %d (%d) on\n",pins[pin],pin);
  digitalWrite(pins[pin],LOW);
  cur_pin = pin;
#ifdef ENABLE
  autoOff.set(autoOffPeriod,disableSwitch);
  if (!enabled) {
    enableSwitch();
  }
#endif
}

void getStatus(AsyncWebServerRequest *request) {
  StaticJsonDocument<1024> doc;
  Serial.println("Processing request...");
  addString(&doc,"pin",String(cur_pin));
#ifdef ENABLE
  addBool(&doc,"enabled",enabled);
#endif
  addString(&doc,"version",FW_VERSION);
  doc["uptime"] = millis() / 1000;
  doc["rssi"] = WiFi.RSSI();
  doc["heap"] = ESP.getFreeHeap();
  char buffer[1024];
  serializeJson(doc, buffer);

  request->send(200, "application/json", buffer);
}

void setupApi() {
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Methods", "GET, POST, PUT");
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Headers", "Content-Type");
  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *request){
    getStatus(request);
  });
  server.serveStatic("/", SPIFFS, "/").setDefaultFile("index.html");
  server.serveStatic("/s/", SPIFFS, "/");
  server.on("/api/on", HTTP_GET, [](AsyncWebServerRequest *request){
    if (!request->hasParam("pin")) {
      request->send(400, "text/plain", "missing pin");
      return;
    }
    int pin = request->getParam("pin")->value().toInt();
    if (pin < 0 || pin >= PIN_COUNT) {
      request->send(400, "text/plain", "pin out of range");
      return;
    }
#ifdef ENABLE
    if (request->hasParam("timer")) {
      autoOffPeriod = request->getParam("timer")->value().toInt();
    } else {
      autoOffPeriod = AUTO_OFF_TIME;
    }
#endif
    on(pin);
    getStatus(request);
  });
  server.on("/api/off", HTTP_GET, [](AsyncWebServerRequest *request){
    off(false);
    getStatus(request);
  });
#ifdef ENABLE
  server.on("/api/disable", HTTP_GET, [](AsyncWebServerRequest *request){
    off(true);
    getStatus(request);
  });
  server.on("/api/enable", HTTP_GET, [](AsyncWebServerRequest *request){
    if (request->hasParam("timer")) {
      autoOffPeriod = request->getParam("timer")->value().toInt();
    } else {
      autoOffPeriod = AUTO_OFF_TIME;
    }
    if (!enabled) {
      enableSwitch();
    }
    off(false);
    getStatus(request);
  });
  server.on("/api/ping", HTTP_GET, [](AsyncWebServerRequest *request){
    if (request->hasParam("timer")) {
      autoOffPeriod = request->getParam("timer")->value().toInt();
    } else {
      autoOffPeriod = AUTO_OFF_TIME;
    }
    if (!enabled) {
      enableSwitch();
    }
    getStatus(request);
  });
#endif
  // Registered after the static handlers: ESPAsyncWebServer checks handlers
  // newest-first, so these win over the serveStatic fallbacks.
  server.on("/favicon.ico", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(204);  // no favicon; stop browsers from re-fetching errors
  });
  server.onNotFound([](AsyncWebServerRequest *request) {
    request->send(404, "text/plain", "not found");
  });
  // Authenticated OTA at /update (firmware + filesystem)
  server.on("/update", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!request->authenticate(OTA_USER, OTA_PASSWORD)) {
      return request->requestAuthentication();
    }
    request->send_P(200, "text/html", otaPage);
  });
  server.on("/update", HTTP_POST, otaRequest, otaUpload);
  server.begin();
}

void setup() {
  // put your setup code here, to run once:

  Serial.begin(115200);
  // This delay gives the chance to wait for a Serial Monitor without blocking if none is found
  delay(1500);


  WiFi.mode(WIFI_STA); // explicitly set mode, esp defaults to STA+AP
  // it is a good practice to make sure your code sets wifi mode how you want it.

  //WiFiManager, Local intialization. Once its business is done, there is no need to keep it around
  WiFiManager wm;

  // reset settings - wipe stored credentials for testing
  // these are stored by the esp library
  // wm.resetSettings();

  // Automatically connect using saved credentials,
  // if connection fails, it starts an access point with the specified name ( "AutoConnectAP"),
  // if empty will auto generate SSID, if password is blank it will be anonymous AP (wm.autoConnect())
  // then goes into a blocking loop awaiting configuration and will return success result

  bool res;
  res = wm.autoConnect(); // auto generated AP name from chipid
  // res = wm.autoConnect("AutoConnectAP"); // anonymous ap
  // res = wm.autoConnect("AutoConnectAP","password"); // password protected ap

  if(!res) {
      Serial.println("Failed to connect");
      ESP.restart();
  }
  else {
      //if you get here you have connected to the WiFi
      Serial.println("Connected...yeey :)");
      // Post-connect ritual, same as the ATU-100 / ESPels: keep the radio
      // out of modem sleep so HTTP responses stay fast on a marginal link.
      WiFi.setHostname("switchctl");
      WiFi.setAutoReconnect(true);
      WiFi.persistent(true);
      WiFi.setSleep(false);
  }

  if(!SPIFFS.begin(true)){
    Serial.println("An Error has occurred while mounting SPIFFS");
    return;
  }

  if (!MDNS.begin("switchctl")) {   // Set the hostname to "switchctl.local"
    Serial.println("Error setting up MDNS responder!");
    delay(1000);
  } else {
    Serial.println("mDNS responder started");
  }
  MDNS.addService("http", "tcp", 80);
  setupApi();

#ifdef ENABLE
  gpio_pad_select_gpio(ENABLE_PIN);
  pinMode(ENABLE_PIN,OUTPUT);
  disableSwitch();
#endif
  for (auto pin: pins) {
    Serial.printf("initializing pin %d\n",pin);
    pinMode(pin,OUTPUT);
    digitalWrite(pin,HIGH);
  }
}

unsigned int previousMillis = 0;
unsigned const int interval = 30*1000;
void loop() {
  unsigned long currentMillis = millis();
  // if WiFi is down, try reconnecting
  if ((WiFi.status() != WL_CONNECTED) && (currentMillis - previousMillis >= interval)) {
    Serial.print(millis());
    Serial.println("Reconnecting to WiFi...");
    WiFi.disconnect();
    WiFi.reconnect();
    previousMillis = currentMillis;
  }
#ifdef ENABLE
  autoOff.update();
#endif
  delay(1000);
}

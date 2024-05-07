#include <Arduino.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <WiFiManager.h>
#include <ESPAsyncWebServer.h>
#include <SPIFFS.h>
#include <TimerEvent.h>


AsyncWebServer server(80);
#define ENABLE_PIN 12
#define AUTO_OFF_TIME 10*60*1000
unsigned int autoOffPeriod = AUTO_OFF_TIME;
int cur_pin;

static const int pins[] = {27,14,32,33,25,26};
bool enabled = false;
TimerEvent autoOff;

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

void disableSwitch() {
    digitalWrite(ENABLE_PIN,LOW);
    enabled = false;
}

void enableSwitch() {
    digitalWrite(ENABLE_PIN,HIGH);
    enabled = true;
    autoOff.set(autoOffPeriod,disableSwitch);
}

void off(bool disable) {
  if (disable) {
    disableSwitch();
  }
  for (auto pin: pins) {
    digitalWrite(pin,LOW);
  }
  cur_pin = -1;
}

void on(int pin) {
  off(false);
  Serial.printf("setting pad %d (%d) HIGH\n",pins[pin],pin);
  digitalWrite(pins[pin],HIGH);
  autoOff.set(autoOffPeriod,disableSwitch);
  cur_pin = pin;
  if (!enabled) {
    enableSwitch();
  }
}

void getStatus(AsyncWebServerRequest *request) {
  StaticJsonDocument<1024> doc;
  Serial.println("Processing request...");
  addString(&doc,"pin",String(cur_pin));
  addBool(&doc,"enabled",enabled);
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
  server.serveStatic("/static/", SPIFFS, "/");
  server.on("/api/on", HTTP_GET, [](AsyncWebServerRequest *request){
    if (request->hasParam("timer")) {
      autoOffPeriod = request->getParam("timer")->value().toInt();
    } else {
      autoOffPeriod = AUTO_OFF_TIME;
    }
    on(request->getParam("pin")->value().toInt());
    getStatus(request);
  });
  server.on("/api/off", HTTP_GET, [](AsyncWebServerRequest *request){
    off(false);
    getStatus(request);
  });
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
    enableSwitch();
    off(false);
    getStatus(request);
  });
  server.on("/api/ping", HTTP_GET, [](AsyncWebServerRequest *request){
    if (request->hasParam("timer")) {
      autoOffPeriod = request->getParam("timer")->value().toInt();
    } else {
      autoOffPeriod = AUTO_OFF_TIME;
    }
    enableSwitch();
    getStatus(request);
  });
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
  }

  if(!SPIFFS.begin(true)){
    Serial.println("An Error has occurred while mounting SPIFFS");
    return;
  }

  setupApi();

  gpio_pad_select_gpio(ENABLE_PIN);
  pinMode(ENABLE_PIN,OUTPUT);
  disableSwitch();
  for (auto pin: pins) {
    Serial.printf("initializing pin %d\n",pin);
    gpio_pad_select_gpio(pin);
    pinMode(pin,OUTPUT);
    digitalWrite(pin,LOW);
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
  autoOff.update();
  delay(1000);
}
#include <Arduino.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <HTTPClient.h>
#include <SPIFFS.h>
#include <FS.h>
#include <ArduinoJson.h>
#include <AsyncJson.h>
#include <FileHandler.hpp>
#include <Base64.h>
#include <ESP32Servo.h>
#include <Update.h>
#include <Preferences.h>
#include <MQTT.h>

#include "MQTTClient.h"
#include "WiFiClient.h"
#include "credentials.hpp"
#include "esp32-hal-log.h"
#include "esp_camera.h"
#include "time.h"

#define LED_BUILTIN     33

#define SERVO_PIN       14

#define Y2_GPIO_NUM     5
#define Y3_GPIO_NUM     18
#define Y4_GPIO_NUM     19
#define Y5_GPIO_NUM     21
#define Y6_GPIO_NUM     36
#define Y7_GPIO_NUM     39
#define Y8_GPIO_NUM     34
#define Y9_GPIO_NUM     35
#define XCLK_GPIO_NUM   0
#define PCLK_GPIO_NUM   22
#define VSYNC_GPIO_NUM  25
#define HREF_GPIO_NUM   23
#define SIOD_GPIO_NUM   26
#define SIOC_GPIO_NUM   27
#define PWDN_GPIO_NUM   32
#define RESET_GPIO_NUM  -1

#define PREF_OPEN_SERVO_IF_TIMEOUT_KEY  "openOnTimeout"
#define PREF_SERVO_OPEN_MS_KEY          "servoOpenMs"

#define PREF_WIFI_SSID_KEY "ssid"
#define PREF_WIFI_PASS_KEY "password"

#define PREF_AI_URL_KEY   "aiUrl"

#define PREF_MQTT_HOST_KEY "mqttHost"
#define PREF_MQTT_PORT_KEY "mqttPort"
#define PREF_MQTT_USERNAME "mqttUsername"
#define PREF_MQTT_PASSWORD "mqttPassword"

#define PREF_TZ_KEY "tz"

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
SemaphoreHandle_t feedingScheduleMutex, 
                  urlFileMutex,
                  mqttFileMutex, 
                  tzFileMutex,
                  wifiFileMutex,
                  notifyFileMutex,
                  servoConfigMutex;

Servo servo;
FileHandler fileHandler(SPIFFS);

const char* ntpServer = "pool.ntp.org";

DynamicJsonDocument feedingScheduleDoc(384);
DynamicJsonDocument urlListDoc(192);
DynamicJsonDocument httpPredictionResponseDoc(192);
DynamicJsonDocument servoConfigDoc(96);
DynamicJsonDocument tzDoc(64);
DynamicJsonDocument wifiCredentialsDoc(128);
DynamicJsonDocument weightDoc(48);

const char* FEEDING_SCHEDULE_FILE_PATH = "/feeding_schedule.json";
const char* URL_LIST_FILE_PATH = "/url_list.json";
const char* TZ_FILE_PATH = "/timezone.json";
const char* WIFI_CRED_FILE_PATH = "/wifi_cred.json";
const char* NOTIFY_FILE_PATH = "/notify_config.json";
const char* SERVO_CONFIG_FILE_PATH = "/servo_config.json";

unsigned long currentMillis = 0;

Preferences preferences;
WiFiClient wifiClient;
MQTTClient mqttClient;

void openServo(int delayMs) {
  log_d("Opening servo");
  for (int pos = 0; pos <= 180; pos++) {
    servo.write(pos);
  }

  log_d("Done opening servo");
  delay(delayMs);

  log_d("Closing servo");
  for (int pos = 180; pos >= 0; pos--) {
    servo.write(pos);
  }
  log_d("Done closing servo");
}

void printScannedWifi() {
  int numNetworks = WiFi.scanNetworks();
  for (int i = 0; i < numNetworks; ++i) {
    Serial.print(WiFi.SSID(i));
    Serial.printf(" | (%4d) | [%2d]\n", WiFi.RSSI(i), WiFi.channel(i));
  }
}

void messageReceived(String &topic, String &payload) {
  log_d("incoming: %s - %s", topic.c_str(), payload.c_str());

  // Note: Do not use the client in the callback to publish, subscribe or
  // unsubscribe as it may cause deadlocks when other things arrive while
  // sending and receiving acknowledgments. Instead, change a global variable,
  // or push to a queue and handle it in the loop after calling `client.loop()`.
}


void onEvent(AsyncWebSocket * server, AsyncWebSocketClient * client, AwsEventType type, void * arg, uint8_t *data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    client->printf("Hello client %u | There are currently %zu client(s)\n", client->id(), ws.count());
  } else if(type == WS_EVT_DISCONNECT){
    log_d("ws[%s][%u] disconnect: %u\n", server->url(), client->id());
  } else if(type == WS_EVT_DATA) {
    AwsFrameInfo * info = (AwsFrameInfo*)arg;

    if(info->final && info->index == 0 && info->len == len) {
      log_d("ws[%s][%u] %s-message[%llu]: ", server->url(), client->id(), (info->opcode == WS_TEXT) ? "text":"binary", info->len);

      if (info->opcode == WS_TEXT) {
        data[len] = 0;
        if (String((char*)data) == String("request_image")) {
          camera_fb_t* fb = esp_camera_fb_get();
    
          client->binary(fb->buf, fb->len);
          esp_camera_fb_return(fb);
        }
      }
    }
  }
}

void WiFiEvent(WiFiEvent_t event) {
  switch(event) {
    case ARDUINO_EVENT_WIFI_AP_START:
      WiFi.softAPsetHostname("ESP32CAM_PetBowlCam");
      WiFi.softAPenableIpV6();
      
      break;
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      WiFi.enableIpV6();

    // case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
    //   delay(1000);

    //   break;
    default:
      break;
  }
}

void initCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 2000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.grab_mode = CAMERA_GRAB_LATEST;
  config.fb_location = CAMERA_FB_IN_PSRAM;

  if (psramFound()) {
    config.frame_size = FRAMESIZE_UXGA;
    config.jpeg_quality = 10;
    config.fb_count = 2;
  } else {
    config.frame_size = FRAMESIZE_SVGA;
    config.jpeg_quality = 12;
    config.fb_count = 1;
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x", err);
    ESP.restart();
  }

  sensor_t* sensor = esp_camera_sensor_get();
  if (sensor->id.PID == OV3660_PID) {
    sensor->set_vflip(sensor, 1);
    sensor->set_brightness(sensor, 1);
    sensor->set_saturation(sensor, -2);
  }
  sensor->set_framesize(sensor, FRAMESIZE_240X240);
}

void initServo() {
  ESP32PWM::allocateTimer(0);
	ESP32PWM::allocateTimer(1);
	ESP32PWM::allocateTimer(2);
	ESP32PWM::allocateTimer(3);

  servo.setPeriodHertz(50);
  servo.attach(SERVO_PIN, 1000, 2000);
}

void initTimezone(const char* timezone) {
  configTime(0, 0, ntpServer);
  setenv("TZ", timezone, 1);
  tzset();
}

void initMutex() {
  feedingScheduleMutex = xSemaphoreCreateMutex();
  urlFileMutex = xSemaphoreCreateMutex();
  tzFileMutex = xSemaphoreCreateMutex();
  wifiFileMutex = xSemaphoreCreateMutex();
  notifyFileMutex = xSemaphoreCreateMutex();
  servoConfigMutex = xSemaphoreCreateMutex();
  mqttFileMutex = xSemaphoreCreateMutex();


  if (feedingScheduleMutex == nullptr || 
      urlFileMutex == nullptr         || 
      tzFileMutex == nullptr          ||
      wifiFileMutex == nullptr        ||
      notifyFileMutex == nullptr      ||
      servoConfigMutex == nullptr     ||
      mqttFileMutex == nullptr) {
    log_e("Error initializing mutex, restarting...");
    ESP.restart();
  }
}

void listDir() {
  File root = SPIFFS.open("/");
  if(!root){
      Serial.println("- failed to open directory");
      return;
  }
  if(!root.isDirectory()){
      Serial.println(" - not a directory");
      return;
  }

  File file = root.openNextFile();
  while(file){
    if(file.isDirectory()){
        Serial.print("  DIR : ");
        Serial.println(file.name());
    } else {
        Serial.print("  FILE: ");
        Serial.print(file.name());
        Serial.print("\tSIZE: ");
        Serial.println(file.size());
    }
    file = root.openNextFile();
  }
}

/* TODO: Add json file to store
*   - Weight sensor value to notify through MQTT
*   - Handle ai server output
*   - Use Preferences library to store simple key value data
*/
void setup() {

  Serial.begin(115200);

  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);

  initServo();
  initMutex();

  preferences.begin("app", false);
  
  if (!SPIFFS.begin(true)) {
    Serial.println("Error mounting spiffs");
    return;
  }

  if (!SPIFFS.exists(FEEDING_SCHEDULE_FILE_PATH)) {
    if (!fileHandler.createFile(FEEDING_SCHEDULE_FILE_PATH)) {
      log_e("Failed to create file content");
      return;
    }

    JsonArray feedingScheduleObj = feedingScheduleDoc.to<JsonArray>();

    if (!fileHandler.writeJson(FEEDING_SCHEDULE_FILE_PATH, feedingScheduleObj)) {
      log_e("Failed to write to file");
      return;
    }
  }

  if (!fileHandler.readJson(FEEDING_SCHEDULE_FILE_PATH, feedingScheduleDoc)) {
    log_e("Failed to read file content");
    return;
  }

  listDir();
  psramInit();

  byte* psdRamBuffer = (byte*)ps_malloc(1024 * 1024 * 4);

  // Init Camera
  initCamera();

  if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
    log_d("not enough space");
  }

  // Init WiFi
  WiFi.mode(WIFI_AP_STA);
  WiFi.onEvent(WiFiEvent);
  WiFi.softAP(AP_SSID, AP_PASS);

  printScannedWifi();

  if (preferences.getString(PREF_WIFI_SSID_KEY, "") != "") {
    WiFi.begin(preferences.getString(PREF_WIFI_SSID_KEY, "").c_str(), preferences.getString(PREF_WIFI_PASS_KEY, "").c_str());
  }

  // Init server

  // server.on("/photo", HTTP_GET, [](AsyncWebServerRequest *request) {
  //   camera_fb_t* fb = esp_camera_fb_get();
  //
  //   if (!fb) {
  //     request->send(500, "text/plain", "failed to capture photo\n");
  //     return;
  //   }
  //
  //   // request->send(200, "text/jpeg", );
  //   AsyncWebServerResponse *response = request->beginResponse_P(200, "image/jpeg", (const uint8_t*)fb->buf, fb->len);
  //   // response->addHeader("Content-Encoding", "gzip");
  //   response->addHeader("Content-Disposition", "inline");
  //   response->addHeader("Access-Control-Allow-Origin", "*");
  //   esp_camera_fb_return(fb);
  //
  //   request->send(response);
  // });

  server.on("/url", HTTP_GET, [](AsyncWebServerRequest* request) {
    char buffer[100];
    snprintf(buffer, 100, "{\"%s\":\"%s\"}", 
      PREF_AI_URL_KEY, 
      preferences.getString(PREF_AI_URL_KEY, "").c_str());

    request->send(200, "application/json", buffer);
  });

  server.on("/mqtt", HTTP_GET, [](AsyncWebServerRequest* request)  {
    char buffer[250];
    snprintf(buffer, 250, "{\"%s\":\"%s\",\"%s\":\"%d\",\"%s\":\"%s\",\"%s\":\"%s\"}", 
      PREF_MQTT_HOST_KEY,
      preferences.getString(PREF_MQTT_HOST_KEY, "").c_str(),
      PREF_MQTT_PORT_KEY,
      preferences.getUInt(PREF_MQTT_PORT_KEY, 1883),
      PREF_MQTT_USERNAME,
      preferences.getString(PREF_MQTT_USERNAME, "").c_str(),
      PREF_MQTT_PASSWORD,
      preferences.getString(PREF_MQTT_PASSWORD, "").c_str()
    );

    request->send(200, "application/json", buffer);
  });
  
  server.on("/mqtt", HTTP_POST, [](AsyncWebServerRequest* request) {
    if (!request->hasParam("host", true) || !request->hasParam("port", true)) {
      request->send(400, "application/json", "{\"message\": \"missing required field\"}");
      return;
    }

    if (xSemaphoreTake(mqttFileMutex, portMAX_DELAY) != pdTRUE) {
      request->send(500, "text/plain", "internal server error");

      return;
    }

    AsyncWebParameter* hostParam = request->getParam("host", true);
    if (hostParam->value() == "") {
      request->send(400, "application/json", "{\"message\": \"host field cannot be empty\"}");
      xSemaphoreGive(mqttFileMutex);

      return;
    }

    AsyncWebParameter* portParam = request->getParam("port", true);
    if (portParam->value() == "") {
      request->send(400, "application/json", "{\"message\": \"port field cannot be empty\"}");
      xSemaphoreGive(mqttFileMutex);

      return;
    }

    int port = portParam->value().toInt();
    if (port < 0) {
      request->send(400, "application/json", "{\"message\": \"port field must be a valid positive number\"}");
      xSemaphoreGive(mqttFileMutex);

      return;
    }

    if (preferences.putString(PREF_MQTT_HOST_KEY, hostParam->value()) == 0 ||
        preferences.putUInt(PREF_MQTT_PORT_KEY, port) == 0) {
      request->send(500, "application/json", "{\"message\": \"failed to store data\"}");
      xSemaphoreGive(mqttFileMutex);

      return;
    };

    if (request->hasParam("username", true) && request->hasParam("password", true)) {
      if (preferences.putString(PREF_MQTT_USERNAME, request->getParam("username", true)->value()) == 0 ||
          preferences.putString(PREF_MQTT_PASSWORD, request->getParam("password", true)->value()) == 0) {
        
        request->send(500, "application/json", "{\"message\": \"failed to username and password data\"}");
        xSemaphoreGive(mqttFileMutex);

        return;
      }
    }

    request->send(201);
    xSemaphoreGive(mqttFileMutex);
  });

  AsyncCallbackJsonWebHandler* urlListPostHandler = 
    new AsyncCallbackJsonWebHandler("/url", [](AsyncWebServerRequest* request, JsonVariant &json) {
  
    if (!json.containsKey("aiUrl")) {
      request->send(400, "application/json", "{\"message\": \"missing url field\"}");
      return;
    }

    if (xSemaphoreTake(urlFileMutex, portMAX_DELAY) != pdTRUE) {
      request->send(500, "text/plain", "max client allowed");

      return; 
    }

    if (preferences.putString(PREF_AI_URL_KEY, json["aiUrl"].as<const char*>()) == 0) {

      request->send(500, "application/json", "{\"message\": \"failed to store data\"}\n");
      xSemaphoreGive(urlFileMutex);

      return;
    }

    request->send(204);
    xSemaphoreGive(urlFileMutex);
  });

  server.on("/feeding_schedule", HTTP_GET, [](AsyncWebServerRequest* request) {
    request->send(SPIFFS, FEEDING_SCHEDULE_FILE_PATH, "application/json");
  });
  
  AsyncCallbackJsonWebHandler* feedingSchedulePostHandler = 
    new AsyncCallbackJsonWebHandler("/feeding_schedule", [](AsyncWebServerRequest* request, JsonVariant &json) {
    if (json.is<JsonArray>()) {
      request->send(400, "application/json", "{\"message\": \"invalid request\"}\n");
      return;
    }

    if (xSemaphoreTake(feedingScheduleMutex, portMAX_DELAY) != pdTRUE) {
      request->send(500, "text/plain", "internal server error");

      return;
    }

    if (!json.containsKey("hour") || !json.containsKey("minutes") || !json.containsKey("seconds")) {
      request->send(400, "application/json", "{\"message\": \"missing required field\"}\n");
      xSemaphoreGive(feedingScheduleMutex);      

      return;
    }

    FileHandler fileHandler(SPIFFS);

    if(!fileHandler.readJson(FEEDING_SCHEDULE_FILE_PATH, feedingScheduleDoc)) {
      request->send(500, "application/json", "{\"message\": \"failed reading a file\"}");
      xSemaphoreGive(feedingScheduleMutex);

      return;
    }

    size_t feedingScheduleSize = feedingScheduleDoc.as<JsonArray>().size();

    if (json.containsKey("id")) {
      int id = json["id"].as<int>();
      if (id < 1 || id > feedingScheduleSize) {
        request->send(200);
        xSemaphoreGive(feedingScheduleMutex);

        return;
      }

      feedingScheduleDoc[id - 1]["hour"] = json["hour"];
      feedingScheduleDoc[id - 1]["minutes"] = json["minutes"];
      feedingScheduleDoc[id - 1]["seconds"] = json["seconds"];
    } else {
      if (feedingScheduleSize > 5) {
        request->send(403, "application/json", "{\"message\": \"maximum capacity of feeding schedules\"}\n");
        xSemaphoreGive(feedingScheduleMutex);

        return;
      }

      if (!feedingScheduleDoc.add(json.as<JsonObject>())) {
        request->send(500, "application/json", "{\"message\": \"failed adding data to variable\"}\n");
        xSemaphoreGive(feedingScheduleMutex);      

        return;
      }
    }

    if(!fileHandler.writeJson(FEEDING_SCHEDULE_FILE_PATH, feedingScheduleDoc)) {
      request->send(500, "application/json", "{\"message\": \"failed writing to a file\"}\n");
      xSemaphoreGive(feedingScheduleMutex);

    }

    request->send(201);
    xSemaphoreGive(feedingScheduleMutex);
  });

  server.on("/feeding_schedule", HTTP_DELETE, [](AsyncWebServerRequest* request) {
    if (!request->hasParam("id")) {
      request->send(400, "application/json", "{\"message\": \"missing id field\"}");

      return;
    }

    if (xSemaphoreTake(feedingScheduleMutex, portMAX_DELAY) != pdTRUE) {
      request->send(500, "text/plain", "internal server error");
      xSemaphoreGive(feedingScheduleMutex);

      return;
    }

    FileHandler fileHandler(SPIFFS);

    if (!fileHandler.readJson(FEEDING_SCHEDULE_FILE_PATH, feedingScheduleDoc)){
      request->send(500, "application/json", "{\"message\": \"failed reading a file\"}");
      xSemaphoreGive(feedingScheduleMutex);

      return;
    }
    
    AsyncWebParameter* idParam = request->getParam("id");
    int id = idParam->value().toInt();
    if (id < 1 || id > feedingScheduleDoc.as<JsonArray>().size()) {
      request->send(200);
      xSemaphoreGive(feedingScheduleMutex);

      return;
    }

    feedingScheduleDoc.remove(id - 1);
    if (!fileHandler.writeJson(FEEDING_SCHEDULE_FILE_PATH, feedingScheduleDoc)) {
      request->send(500, "application/json", "{\"message\": \"failed writing to a file\"}");
      xSemaphoreGive(feedingScheduleMutex);

      return;
    }

    request->send(200);

    xSemaphoreGive(feedingScheduleMutex);
  });

  server.on("/tz", HTTP_GET, [](AsyncWebServerRequest* request) {
    char buffer[50];
    snprintf(buffer, 50, "{\"%s\":\"%s\"}", 
      PREF_TZ_KEY, 
      preferences.getString(PREF_TZ_KEY, "").c_str()
    );

    request->send(200, "application/json", buffer);
  });

  server.on("/tz", HTTP_POST, [](AsyncWebServerRequest* request) {
    if (!request->hasParam("tz", true)) {
      request->send(400, "application/json", "{\"message\": \"missing tz field\"}");
      return;
    }

    if (xSemaphoreTake(tzFileMutex, portMAX_DELAY) != pdTRUE) {
      request->send(500, "text/plain", "internal server error");

      return;
    }

    AsyncWebParameter* tzParam = request->getParam("tz", true);
    if (tzParam->value() == "") {
      request->send(400, "application/json", "{\"message\": \"tz field cannot be empty\"}");
      xSemaphoreGive(tzFileMutex);

      return;
    }

    if (preferences.putString(PREF_TZ_KEY, tzParam->value()) == 0) {
      request->send(500, "application/json", "{\"message\": \"failed to store data\"}");
      xSemaphoreGive(tzFileMutex);

      return;
    };

    setenv("TZ", tzParam->value().c_str(), 1);
    tzset();

    request->send(204);
    xSemaphoreGive(tzFileMutex);
  });

  server.on("/wifi", HTTP_GET, [](AsyncWebServerRequest* request) {
    char buffer[100];
    snprintf(buffer, 100, "{\"%s\":\"%s\",\"%s\":\"%s\"}", 
      PREF_WIFI_SSID_KEY, 
      preferences.getString(PREF_WIFI_SSID_KEY, "").c_str(),
      PREF_WIFI_PASS_KEY,
      preferences.getString(PREF_WIFI_PASS_KEY, "").c_str()
    );

    request->send(200, "application/json", buffer);
  });

  server.on("/wifi", HTTP_POST, [](AsyncWebServerRequest* request) {
    if (!request->hasParam("ssid", true) || !request->hasParam("password", true)) {
      request->send(400, "application/json", "{\"message\": \"missing required field\"}");
      return;
    }

    if (xSemaphoreTake(wifiFileMutex, portMAX_DELAY) != pdTRUE) {
      request->send(500, "text/plain", "internal server error");

      return;
    }

    AsyncWebParameter* ssidParam = request->getParam("ssid", true);
    AsyncWebParameter* passwordParam = request->getParam("password", true);

    if (preferences.putString(PREF_WIFI_SSID_KEY, ssidParam->value()) == 0 || preferences.putString(PREF_WIFI_PASS_KEY, passwordParam->value()) == 0) {
      request->send(500, "application/json", "{\"message\": \"failed to store data\"}\n");
      xSemaphoreGive(wifiFileMutex);

      return;
    }

    request->send(204);
    vTaskDelay(1000);

    WiFi.begin(preferences.getString(PREF_WIFI_SSID_KEY, "").c_str(), preferences.getString(PREF_WIFI_PASS_KEY, "").c_str());
    xSemaphoreGive(wifiFileMutex);
  });

  AsyncCallbackJsonWebHandler* wifiPostHandler = 
    new AsyncCallbackJsonWebHandler("/wifi", [](AsyncWebServerRequest* request, JsonVariant &json) {
    if (json.is<JsonArray>()) {
      request->send(400, "application/json", "{\"message\": \"invalid request\"}\n");
      return;
    }

    if (xSemaphoreTake(wifiFileMutex, portMAX_DELAY) != pdTRUE) {
      request->send(500, "text/plain", "internal server error");

      return;
    }

    if (!json.containsKey("ssid")) {
      request->send(400, "application/json", "{\"message\": \"missing ssid field\"}\n");
      xSemaphoreGive(wifiFileMutex);      

      return;
    }

    // FileHandler fileHandler(SPIFFS);

    // if(!fileHandler.readJson(WIFI_CRED_FILE_PATH, wifiCredentialsDoc)) {
    //   request->send(500, "application/json", "{\"message\": \"failed reading a file\"}");
    //   xSemaphoreGive(wifiFileMutex);

    //   return;
    // }

    // wifiCredentialsDoc["ssid"] = json["ssid"];
    // if (json.containsKey("password"))
    //   wifiCredentialsDoc["password"] = json["password"];
    
    // if (!fileHandler.writeJson(WIFI_CRED_FILE_PATH, wifiCredentialsDoc)) {
      // request->send(500, "application/json", "{\"message\": \"failed opening a file\"}\n");
      // xSemaphoreGive(wifiFileMutex);

      // return;
    // }

    if (preferences.putString(PREF_WIFI_SSID_KEY, json["ssid"].as<const char*>()) == 0) {
      request->send(500, "application/json", "{\"message\": \"failed to store ssid\"}\n");
      xSemaphoreGive(wifiFileMutex);

      return;
    }

    if (json.containsKey("password"))
      if (preferences.putString(PREF_WIFI_PASS_KEY, json["password"].as<const char*>()) == 0) {
        request->send(500, "application/json", "{\"message\": \"failed to store password\"}\n");
        xSemaphoreGive(wifiFileMutex);
  
        return;
      }


    request->send(204);
    WiFi.begin(wifiCredentialsDoc["ssid"].as<const char*>(), wifiCredentialsDoc["password"].as<const char*>());
    xSemaphoreGive(wifiFileMutex);
  });

  // server.on("/notify/setting", HTTP_GET, [](AsyncWebServerRequest* request) {
  //   request->send(SPIFFS, NOTIFY_FILE_PATH, "application/json");
  // });

  // server.on("/notify/setting", HTTP_POST, [](AsyncWebServerRequest* request) {
  //   if (!request->hasParam("weightToNotify", true)) {
  //     request->send(400, "application/json", "{\"message\": \"missing tz field\"}");
  //     return;
  //   }
  //
  //   if (xSemaphoreTake(notifyFileMutex, portMAX_DELAY) != pdTRUE) {
  //     request->send(500, "text/plain", "internal server error");;
  //
  //     return;
  //   }
  //
  //   FileHandler fileHandler(SPIFFS);
  //
  //   if (!fileHandler.readJson(NOTIFY_FILE_PATH, weightDoc)){
  //     request->send(500, "application/json", "{\"message\": \"failed reading a file\"}");
  //     xSemaphoreGive(notifyFileMutex);
  //
  //     return;
  //   }
  //
  //   AsyncWebParameter* weightToNotifyParam = request->getParam("weightToNotify", true);
  //   long weightToNotify = weightToNotifyParam->value().toInt();
  //   if (weightToNotify < 0) {
  //     request->send(400, "application/json", "{\"message\": \"value must be a positive number\"}");
  //     xSemaphoreGive(notifyFileMutex);
  //
  //     return;
  //   }
  //
  //   weightDoc["weightToNotify"] = weightToNotify;
  //
  //   if (!fileHandler.writeJson(NOTIFY_FILE_PATH, weightDoc)) {
  //     request->send(500, "application/json", "{\"message\": \"failed writing to a file\"}");
  //     xSemaphoreGive(notifyFileMutex);
  //
  //     return;
  //   }
  //
  //   request->send(204);
  //   xSemaphoreGive(notifyFileMutex);
  // });

  server.on("/servo/open", HTTP_POST, [](AsyncWebServerRequest* request) {
    if (xSemaphoreTake(servoConfigMutex, portMAX_DELAY) != pdTRUE) {
      request->send(500, "application/json", "{\"message\": \"servo is in use by the esp32\"}");

      return;
    }

    if (!fileHandler.readJson(SERVO_CONFIG_FILE_PATH, servoConfigDoc)) {
      request->send(500, "application/json", "{\"message\": \"failed to read from file\"}");
      xSemaphoreGive(servoConfigMutex);

      return;
    }

    // unsigned int delayMs = servoConfigDoc["servoOpenMs"].as<unsigned int>();

    openServo(servoConfigDoc["servoOpenMs"].as<int>());

    request->send(200, "application/json", "{\"message\": \"success\"}");
    xSemaphoreGive(servoConfigMutex);
  });

  server.on("/servo", HTTP_GET, [](AsyncWebServerRequest* request) {
    char buffer[60];
    snprintf(buffer, 60, "{\"%s\":%s,\"%s\":%d}", 
      PREF_OPEN_SERVO_IF_TIMEOUT_KEY, 
      preferences.getBool(PREF_OPEN_SERVO_IF_TIMEOUT_KEY, true) ? "true" : "false",
      PREF_SERVO_OPEN_MS_KEY,
      preferences.getUInt(PREF_SERVO_OPEN_MS_KEY, 0)
    );

    request->send(200, "application/json", buffer);
  });

  server.on("/servo", HTTP_POST, [](AsyncWebServerRequest* request) {
    if (!request->hasParam("shouldOpenIfTimeout", true) || !request->hasParam("servoOpenMs", true)) {
      request->send(400, "application/json", "{\"message\": \"missing required field\"}");
      return;
    }

    if (xSemaphoreTake(servoConfigMutex, portMAX_DELAY) != pdTRUE) {
      request->send(500, "text/plain", "internal server error");

      return;
    }

    AsyncWebParameter* shouldOpenIfTimeoutParam = request->getParam("shouldOpenIfTimeout", true);
    AsyncWebParameter* servoOpenMsParam = request->getParam("servoOpenMs", true);

    if (shouldOpenIfTimeoutParam->value() != "true" && shouldOpenIfTimeoutParam->value() != "false" ) {
      request->send(400, "application/json", "{\"message\": \"shouldOpenIfTimeout param should be either true or false\"}");
      xSemaphoreGive(servoConfigMutex);

      return;
    }

    bool shouldOpenIfTimeout = (shouldOpenIfTimeoutParam->value() == "true") ? true : false;
    long servoOpenMs = servoOpenMsParam->value().toInt();
    if (servoOpenMs < 0) {
      request->send(400, "application/json", "{\"message\": \"servoOpenMs parameter value should be a positive number\"}");
      xSemaphoreGive(servoConfigMutex);

      return;
    }

    if (preferences.putBool(PREF_OPEN_SERVO_IF_TIMEOUT_KEY, shouldOpenIfTimeout) == 0 ||
        preferences.putUInt(PREF_SERVO_OPEN_MS_KEY, servoOpenMs) == 0) {

      request->send(500, "application/json", "{\"message\": \"failed to store data\"}");
      xSemaphoreGive(servoConfigMutex);

      return;
    }

    request->send(204);
    xSemaphoreGive(servoConfigMutex);
  });

  server.on("/update", HTTP_POST, [](AsyncWebServerRequest* request) {
    AsyncWebServerResponse *response = request->beginResponse((Update.hasError()) ? 400 : 200, "text/plain", Update.hasError() ? String(Update.getError()).c_str() : "OK");
    response->addHeader("Connection", "close");
    response->addHeader("Access-Control-Allow-Origin", "*");

    request->send(response);

  }, [](AsyncWebServerRequest *request, const String& filename, size_t index, uint8_t *data, size_t len, bool final) {
    log_d("Handling file upload, filename = %s | index = %d | len = %d | final = %d", filename.c_str(), index, len, final);
    if(len){
      if (Update.write(data, len) != len) {
        return request->send(400, "text/plain", "Failed to write chunked data to free space");
      }
    }
        
    if (final) {
      if (!Update.end(true)) {
        log_d("/update: %s", String(Update.getError()).c_str());
      }

      log_i("Update successfully completed. Please reboot your device");
    }else{
      return;
    }
  });

  server.on("/reset", HTTP_POST, [](AsyncWebServerRequest* request) {
    request->send(204);

    vTaskDelay(1000);
    ESP.restart();
  });

  ws.onEvent(onEvent);

  server.addHandler(&ws);
  server.addHandler(feedingSchedulePostHandler);
  server.addHandler(urlListPostHandler);
  // server.addHandler(wifiPostHandler);

  server.begin();

  initTimezone(preferences.getString(PREF_TZ_KEY, "UTC0").c_str());

  while (WiFi.status() != WL_CONNECTED) {
    log_i("Could not connect to wifi, waiting for wifi connection");

    delay(3000);
  }

  if (preferences.getString(PREF_MQTT_HOST_KEY, "") != "") {
    mqttClient.begin(
      preferences.getString(PREF_MQTT_HOST_KEY, "").c_str(), 
      preferences.getUInt(PREF_MQTT_PORT_KEY, 1883), 
      wifiClient);

    mqttClient.setTimeout(10000);
    mqttClient.onMessage(messageReceived);


    if (preferences.getString(PREF_MQTT_USERNAME, "") == "") {
      if (!mqttClient.connect("petBowlCam")) {
        log_w("Error connecting to MQTT client");
      } else {
        if (!mqttClient.subscribe("petBowlCam/test")) {
          log_e("Error subscribing to queue");
        }
        if (!mqttClient.publish("petBowlCam/test", "testing", true, 1)) {
          log_e("Error publishing to queue");
        }
      }
    } else {
      if (!mqttClient.connect(
        "petBowlCam", 
        preferences.getString(PREF_MQTT_USERNAME, "").c_str(),
        preferences.getString(PREF_MQTT_PASSWORD, "").c_str())
      ) {
        log_w("Error connecting to MQTT client");
      } else {
        if (!mqttClient.subscribe("petBowlCam/test")) {
          log_e("Error subscribing to queue");
        }
        if (!mqttClient.publish("petBowlCam/test", "testing", true, 1)) {
          log_e("Error publishing to queue");
        }
      }
    }
  }


  digitalWrite(LED_BUILTIN, HIGH);

  log_d("test update upload from web 2");
}

void loop() {
  struct tm timeinfo;

  if (WiFi.status() != WL_CONNECTED) return;

  mqttClient.loop();

  if (xSemaphoreGetMutexHolder(feedingScheduleMutex) != nullptr || 
      xSemaphoreGetMutexHolder(urlFileMutex) != nullptr         ||
      xSemaphoreGetMutexHolder(tzFileMutex) != nullptr          || 
      xSemaphoreGetMutexHolder(wifiFileMutex) != nullptr        ||
      xSemaphoreGetMutexHolder(servoConfigMutex) != nullptr     ||
      xSemaphoreGetMutexHolder(mqttFileMutex) != nullptr) {
      
    return;
  }

  if(!getLocalTime(&timeinfo)){
    log_w("Failed to fetch local time");
    return;
  }

  JsonArray arrDoc = feedingScheduleDoc.as<JsonArray>();
  for (JsonVariant v : arrDoc) {
    if (v["hour"].as<int>() != timeinfo.tm_hour   || 
        v["minutes"].as<int>() != timeinfo.tm_min ||
        (millis() - currentMillis < 60000 && currentMillis != 0)) {
          continue;
    }

    log_i("Time matched: %d:%d | %d:%d", 
      v["hour"].as<int>(), 
      v["minutes"].as<int>(), 
      timeinfo.tm_hour, 
      timeinfo.tm_min
    );

    digitalWrite(LED_BUILTIN, LOW);

    String url = preferences.getString(PREF_AI_URL_KEY, "");
    if (url == nullptr || url == "") {
      log_w("AI URL is empty");        
      return;
    }

    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) {
      log_e("Failed at getting camera framebuffer");
      continue;
    }


    int encodedLength = Base64.encodedLength(fb->len);
    log_d("Length: %d", encodedLength);

    char* encodedString = (char*)malloc(encodedLength * sizeof(char) + 1);
    Base64.encode(encodedString, reinterpret_cast<char*>(fb->buf), fb->len);

    esp_camera_fb_return(fb);
    log_d("Deallocate camera framebuffer");

    String reqBody = "--camBoundary\r\nContent-Disposition: form-data; name=\"file_base64\"\r\n\r\n";
    reqBody += encodedString;
    reqBody += "\r\n--camBoundary--\r\n";

    free(encodedString);
    log_d("Successfully deallocate base64 variable");

    log_d("start request");

    int retries = 0;
    HTTPClient http;

    int httpCode = 0;

    http.begin(url.c_str());

    http.addHeader("Content-Type", "multipart/form-data; boundary=camBoundary");
    http.addHeader("Content-Length", String(reqBody.length()));
    http.addHeader("accept", "application/json");

    do {
      http.setTimeout(10000);
      http.useHTTP10(false);
      http.setReuse(false);

      httpCode = http.POST(reqBody);

      log_d("Status code: %d", httpCode);

      retries++;
    } while (retries <= 5 && httpCode != HTTP_CODE_OK);

    digitalWrite(LED_BUILTIN, HIGH);


    if (retries <= 5) {
      log_d("Getting response body");
      String payload = http.getString();

      deserializeJson(httpPredictionResponseDoc, payload);

      String responseOutput = "";
      serializeJson(httpPredictionResponseDoc, responseOutput);
      responseOutput += '\n';

      log_d("response: %s", responseOutput.c_str());

      ws.textAll(responseOutput.c_str());
      String topPrediction = httpPredictionResponseDoc["top"].as<String>();

      if (topPrediction == "unfinished_bowl" || topPrediction == "empty_bowl") {
        digitalWrite(LED_BUILTIN, LOW);

        openServo(preferences.getUInt(PREF_SERVO_OPEN_MS_KEY, 0));

        digitalWrite(LED_BUILTIN, HIGH);
      } else if (topPrediction == "floor") {
        log_d("prediction is floor, ignoring");
      }
    } else {
      ws.textAll("Failed to send payload");
      log_w("Failed to send payload");

      if (preferences.getBool(PREF_OPEN_SERVO_IF_TIMEOUT_KEY, true)) {  
        digitalWrite(LED_BUILTIN, LOW);
        openServo(preferences.getUInt(PREF_SERVO_OPEN_MS_KEY, 0));
        digitalWrite(LED_BUILTIN, HIGH);
      }
    }

    http.end();
    log_d("HTTPClient end() called");

    currentMillis = millis();
  }
}

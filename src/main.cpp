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

#include "credentials.hpp"
#include "freertos/FreeRTOS.h"
#include "esp_camera.h"
#include "esp_timer.h"
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

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
SemaphoreHandle_t endpointMutex, 
                  feedingScheduleMutex, 
                  urlFileMutex, 
                  tzFileMutex,
                  wifiFileMutex,
                  notifyFileMutex,
                  servoConfigMutex;

Servo servo;
FileHandler fileHandler(SPIFFS);

const char* ntpServer = "pool.ntp.org";
String nonce = "test";

DynamicJsonDocument feedingScheduleDoc(384);
DynamicJsonDocument urlListDoc(192);
DynamicJsonDocument httpPredictionResponseDoc(192);
DynamicJsonDocument servoConfigDoc(96);
DynamicJsonDocument tzDoc(64);
DynamicJsonDocument wifiCredentialsDoc(128);
DynamicJsonDocument weightDoc(48);

bool isProcessed = false;

const char* FEEDING_SCHEDULE_FILE_PATH = "/feeding_schedule.json";
const char* URL_LIST_FILE_PATH = "/url_list.json";
const char* TZ_FILE_PATH = "/timezone.json";
const char* WIFI_CRED_FILE_PATH = "/wifi_cred.json";
const char* NOTIFY_FILE_PATH = "/notify_config.json";
const char* SERVO_CONFIG_FILE_PATH = "/servo_config.json";

void printLocalTime();

void printScannedWifi() {
  int numNetworks = WiFi.scanNetworks();
  for (int i = 0; i < numNetworks; ++i) {
    Serial.print(WiFi.SSID(i));
    Serial.printf(" | (%4d) | [%2d]\n", WiFi.RSSI(i), WiFi.channel(i));
  }
}

void onEvent(AsyncWebSocket * server, AsyncWebSocketClient * client, AwsEventType type, void * arg, uint8_t *data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    client->printf("Hello client %u | There are currently %u client(s)\n", client->id(), ws.count());
    log_d("client %u connected | There are currently %u client(s)\n", client->id(), ws.count());
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

      log_d("AP IPv4: %s\n", WiFi.softAPIP());
      break;
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      WiFi.enableIpV6();
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP6:
      log_d("STA IPv6: %s", WiFi.localIPv6());
      break;
    case ARDUINO_EVENT_WIFI_AP_GOT_IP6:
      log_d("AP IPv6: %s", WiFi.softAPIPv6());
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      log_d("STA IPv4: %s", WiFi.localIP());
      break;
    default:
      break;
  }
}

void simpleTask(void *pvParameters) {
  (void) pvParameters;

  pinMode(LED_BUILTIN, OUTPUT);

  digitalWrite(LED_BUILTIN, HIGH);
  // for (int pos = 0; pos <= 180; pos++) {
  //   servo.write(pos);
  //   delay(15);
  // }
  vTaskDelay(2000 / portTICK_PERIOD_MS);

  digitalWrite(LED_BUILTIN, LOW);
  // for (int pos = 180; pos >= 0; pos--) {
  //   servo.write(pos); 
  // }
  vTaskDelay(2000 / portTICK_PERIOD_MS);

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
  sensor->set_framesize(sensor, FRAMESIZE_QVGA);
}

void initServo() {
  ESP32PWM::allocateTimer(0);
	ESP32PWM::allocateTimer(1);
	ESP32PWM::allocateTimer(2);
	ESP32PWM::allocateTimer(3);
  servo.setPeriodHertz(50);
  servo.attach(SERVO_PIN, 1000, 2000);
}

void initMutex() {
  endpointMutex = xSemaphoreCreateMutex();
  feedingScheduleMutex = xSemaphoreCreateMutex();
  urlFileMutex = xSemaphoreCreateMutex();
  tzFileMutex = xSemaphoreCreateMutex();
  wifiFileMutex = xSemaphoreCreateMutex();
  notifyFileMutex = xSemaphoreCreateMutex();
  servoConfigMutex = xSemaphoreCreateMutex();


  if (feedingScheduleMutex == nullptr || 
      urlFileMutex == nullptr         || 
      tzFileMutex == nullptr          ||
      wifiFileMutex == nullptr        ||
      notifyFileMutex == nullptr      ||
      servoConfigMutex == nullptr) {
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
*/
void setup() {

  Serial.begin(115200);

  initServo();
  initMutex();
  
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

  if (!SPIFFS.exists(URL_LIST_FILE_PATH)) {
    if (!fileHandler.createFile(URL_LIST_FILE_PATH)) {
      log_e("Failed to create file content");
      return;
    }

    JsonObject urlListObj = urlListDoc.to<JsonObject>();
    urlListObj["aiUrl"] = "";
    urlListObj["mqttUrl"] = "";

    if (!fileHandler.writeJson(URL_LIST_FILE_PATH, urlListObj)) {
      log_e("Failed to write to file");
      return;
    }
  }

  if (!fileHandler.readJson(URL_LIST_FILE_PATH, urlListDoc)) {
    log_e("Failed to read url file");
    return;
  }

  if (!SPIFFS.exists(TZ_FILE_PATH)) {
    if (!fileHandler.createFile(TZ_FILE_PATH)) {
      log_e("Failed to create file content");
      return;
    }

    JsonObject tzObj = tzDoc.to<JsonObject>();
    tzObj["tz"] = "UTC0";

    if (!fileHandler.writeJson(TZ_FILE_PATH, tzObj)) {
      log_e("Failed to write to file");
      return;
    }
  }

  if (!fileHandler.readJson(TZ_FILE_PATH, tzDoc)) {
    log_e("Failed to read url file");
    return;
  }

  if (!SPIFFS.exists(WIFI_CRED_FILE_PATH)) {
    if (!fileHandler.createFile(WIFI_CRED_FILE_PATH)) {
      log_e("Failed to create file content");
      return;
    }

    JsonObject wifiObj = wifiCredentialsDoc.to<JsonObject>();
    wifiObj["ssid"] = "";
    wifiObj["password"] = "";

    if (!fileHandler.writeJson(WIFI_CRED_FILE_PATH, wifiObj)) {
      log_e("Failed to write to file");
      return;
    }
  }

  if (!fileHandler.readJson(WIFI_CRED_FILE_PATH, wifiCredentialsDoc)) {
    log_e("Failed to read url file");
    return;
  }

  if (!SPIFFS.exists(NOTIFY_FILE_PATH)) {
    if (!fileHandler.createFile(NOTIFY_FILE_PATH)) {
      log_e("Failed to create file content");
      return;
    }

    JsonObject weightObj = weightDoc.to<JsonObject>();
    weightObj["weightToNotify"] = 0;

    if (!fileHandler.writeJson(NOTIFY_FILE_PATH, weightObj)) {
      log_e("Failed to write to file");
      return;
    }
  }

  if (!fileHandler.readJson(NOTIFY_FILE_PATH, weightDoc)) {
    log_e("Failed to read url file");
    return;
  }

  if (!SPIFFS.exists(SERVO_CONFIG_FILE_PATH)) {
    if (!fileHandler.createFile(SERVO_CONFIG_FILE_PATH)) {
      log_e("Failed to create file content");
      return;
    }

    JsonObject servoConfigObj = servoConfigDoc.to<JsonObject>();
    servoConfigDoc["servoOpenMs"] = 0;
    servoConfigDoc["shouldOpenIfTimeout"] = true;

    if (!fileHandler.writeJson(SERVO_CONFIG_FILE_PATH, servoConfigDoc)) {
      log_e("Failed to write to file");
      return;
    }
  }

  if (!fileHandler.readJson(SERVO_CONFIG_FILE_PATH, servoConfigDoc)) {
    log_e("Failed to read url file");
    return;
  }

  listDir();
  psramInit();

  byte* psdRamBuffer = (byte*)ps_malloc(1024 * 1024 * 4);

  // Init WiFi
  WiFi.mode(WIFI_AP_STA);
  WiFi.onEvent(WiFiEvent);
  WiFi.softAP(AP_SSID, AP_PASS);

  if (wifiCredentialsDoc["ssid"].as<String>() != "" && wifiCredentialsDoc.containsKey("password")) {
    WiFi.begin(wifiCredentialsDoc["ssid"].as<const char*>(), wifiCredentialsDoc["password"].as<const char*>());
  }

  // digitalWrite(LED_BUILTIN, HIGH);
  // printScannedWifi();

  // digitalWrite(LED_BUILTIN, LOW);

  // Init Camera
  initCamera();

  // Init server
  // server.on("/photo", HTTP_GET, [](AsyncWebServerRequest *request) {
  //   camera_fb_t* fb = esp_camera_fb_get();

  //   if (!fb) {
  //     request->send(500, "text/plain", "failed to capture photo\n");
  //     return;
  //   }

  //   // request->send(200, "text/jpeg", );
  //   AsyncWebServerResponse *response = request->beginResponse_P(200, "image/jpeg", (const uint8_t*)fb->buf, fb->len);
  //   // response->addHeader("Content-Encoding", "gzip");
  //   response->addHeader("Content-Disposition", "inline");
  //   response->addHeader("Access-Control-Allow-Origin", "*");
  //   esp_camera_fb_return(fb);

  //   request->send(response);
  // });

  server.on("/prediction", HTTP_POST, [](AsyncWebServerRequest* request) {
    if (xSemaphoreTake(endpointMutex, portMAX_DELAY) != pdTRUE) {
      request->send(500, "text/plain", "max client allowed");

      return;
    }

    if (request->hasParam("prediction", true) || request->hasParam("servoOpenSeconds", true) || request->hasParam("nonce", true)) {
      AsyncWebParameter* nonceParam = request->getParam("nonce", true);

      String tmp = nonceParam->value();


      if (tmp != nonce) {
        request->send(403, "text/plain", "Invalid nonce\n");
        xSemaphoreGive(endpointMutex);

        return;
      }


      AsyncWebParameter* predictionParam = request->getParam("prediction", true);
      AsyncWebParameter* servoOpenSecondsParam = request->getParam("servoOpenSeconds", true);

      nonce = "test1";
      // Open Servo code here
      char response[100];
      snprintf(response, 100, "{\"message\": \"%s\",\"newNonce\": \"%s\",\"servoOpenSeconds\": \"%s\"}\n", predictionParam->value(), nonce.c_str(), servoOpenSecondsParam->value());

      request->send(200, "application/json", response);
      xSemaphoreGive(endpointMutex);

      return;
    }

    xSemaphoreGive(endpointMutex);

    request->send(400, "application/json", "{\"message\": \"either one of prediction or servoOpenSeconds parameter is missing\"}\n");
  });

  server.on("/url", HTTP_GET, [](AsyncWebServerRequest* request) {
    request->send(SPIFFS, URL_LIST_FILE_PATH, "application/json");
  });

  AsyncCallbackJsonWebHandler* urlListPostHandler = 
    new AsyncCallbackJsonWebHandler("/url", [](AsyncWebServerRequest* request, JsonVariant &json) {
  
    if (!json.containsKey("aiUrl") || !json.containsKey("mqttUrl")) {
      request->send(400, "application/json", "{\"message\": \"missing url field\"}");
      return;
    }

    if (xSemaphoreTake(urlFileMutex, portMAX_DELAY) != pdTRUE) {
      request->send(500, "text/plain", "max client allowed");

      return; 
    }

    FileHandler fileHandler(SPIFFS);

    if(!fileHandler.readJson(URL_LIST_FILE_PATH, urlListDoc)) {
      request->send(500, "application/json", "{\"message\": \"failed reading a file\"}");
      xSemaphoreGive(urlFileMutex);

      return;
    }

    urlListDoc["aiUrl"] = json["aiUrl"].as<const char*>();
    urlListDoc["mqttUrl"] = json["mqttUrl"].as<const char*>();

    if (!fileHandler.writeJson(URL_LIST_FILE_PATH, urlListDoc)) {
      request->send(500, "application/json", "{\"message\": \"failed opening a file\"}\n");
      xSemaphoreGive(urlFileMutex);

      return;
    }

    request->send(204);
    xSemaphoreGive(urlFileMutex);
  });

  // server.on("/url", HTTP_PUT, [](AsyncWebServerRequest* request) {
  //   if (!request->hasParam("url", true)) {
  //     request->send(400, "application/json", "{\"message\": \"missing id field\"}");

  //     return;
  //   }

  //   if (xSemaphoreTake(urlFileMutex, portMAX_DELAY) != pdTRUE) {
  //     request->send(500, "text/plain", "max client allowed");

  //     return; 
  //   }

  //   urlListDoc["url"] = request->getParam("url", true)->value();

  //   // File file = SPIFFS.open(URL_LIST_FILE_PATH, FILE_WRITE);
  //   // if (!file) {
  //   //   request->send(500, "application/json", "{\"message\": \"failed opening a file\"}\n");
  //   //   file.close();
  //   //   xSemaphoreGive(urlFileMutex);

  //   //   return;
  //   // }

  //   // serializeJson(urlListDoc, file);
  //   // file.close();
    
  //   // request->send(204);
    
  //   // xSemaphoreGive(urlFileMutex);    
  // });

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
    request->send(SPIFFS, TZ_FILE_PATH, "application/json");
  });

  server.on("/tz", HTTP_POST, [](AsyncWebServerRequest* request) {
    if (!request->hasParam("tz", true)) {
      request->send(400, "application/json", "{\"message\": \"missing tz field\"}");
      return;
    }

    if (xSemaphoreTake(tzFileMutex, portMAX_DELAY) != pdTRUE) {
      request->send(500, "text/plain", "internal server error");
      xSemaphoreGive(tzFileMutex);

      return;
    }

    FileHandler fileHandler(SPIFFS);

    if (!fileHandler.readJson(TZ_FILE_PATH, tzDoc)){
      request->send(500, "application/json", "{\"message\": \"failed reading a file\"}");
      xSemaphoreGive(tzFileMutex);

      return;
    }

    AsyncWebParameter* tzParam = request->getParam("tz", true);
    if (tzParam->value() == "") {
      request->send(400, "application/json", "{\"message\": \"tz field cannot be empty\"}");
      xSemaphoreGive(tzFileMutex);

      return;
    }

    tzDoc["tz"] = tzParam->value();

    if (!fileHandler.writeJson(TZ_FILE_PATH, tzDoc)) {
      request->send(500, "application/json", "{\"message\": \"failed writing to a file\"}");
      xSemaphoreGive(tzFileMutex);

      return;
    }

    setenv("TZ", tzParam->value().c_str(), 1);
    tzset();

    request->send(204);
    xSemaphoreGive(tzFileMutex);
  });

  server.on("/wifi", HTTP_GET, [](AsyncWebServerRequest* request) {
    request->send(SPIFFS, WIFI_CRED_FILE_PATH, "application/json");
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

    FileHandler fileHandler(SPIFFS);

    if(!fileHandler.readJson(WIFI_CRED_FILE_PATH, wifiCredentialsDoc)) {
      request->send(500, "application/json", "{\"message\": \"failed reading a file\"}");
      xSemaphoreGive(wifiFileMutex);

      return;
    }

    wifiCredentialsDoc["ssid"] = json["ssid"];
    if (json.containsKey("password"))
      wifiCredentialsDoc["password"] = json["password"];
    
    if (!fileHandler.writeJson(WIFI_CRED_FILE_PATH, wifiCredentialsDoc)) {
      request->send(500, "application/json", "{\"message\": \"failed opening a file\"}\n");
      xSemaphoreGive(wifiFileMutex);

      return;
    }

    request->send(201);
    WiFi.begin(wifiCredentialsDoc["ssid"].as<const char*>(), wifiCredentialsDoc["password"].as<const char*>());
    xSemaphoreGive(wifiFileMutex);
  });

  server.on("/notify/setting", HTTP_GET, [](AsyncWebServerRequest* request) {
    request->send(SPIFFS, NOTIFY_FILE_PATH, "application/json");
  });

  server.on("/notify/setting", HTTP_POST, [](AsyncWebServerRequest* request) {
    if (!request->hasParam("weightToNotify", true)) {
      request->send(400, "application/json", "{\"message\": \"missing tz field\"}");
      return;
    }

    if (xSemaphoreTake(notifyFileMutex, portMAX_DELAY) != pdTRUE) {
      request->send(500, "text/plain", "internal server error");
      xSemaphoreGive(notifyFileMutex);

      return;
    }

    FileHandler fileHandler(SPIFFS);

    if (!fileHandler.readJson(NOTIFY_FILE_PATH, weightDoc)){
      request->send(500, "application/json", "{\"message\": \"failed reading a file\"}");
      xSemaphoreGive(notifyFileMutex);

      return;
    }

    AsyncWebParameter* weightToNotifyParam = request->getParam("weightToNotify", true);
    long weightToNotify = weightToNotifyParam->value().toInt();
    if (weightToNotify < 0) {
      request->send(400, "application/json", "{\"message\": \"value must be a positive number\"}");
      xSemaphoreGive(notifyFileMutex);

      return;
    }

    weightDoc["weightToNotify"] = weightToNotify;

    if (!fileHandler.writeJson(NOTIFY_FILE_PATH, weightDoc)) {
      request->send(500, "application/json", "{\"message\": \"failed writing to a file\"}");
      xSemaphoreGive(notifyFileMutex);

      return;
    }

    request->send(204);
    xSemaphoreGive(notifyFileMutex);
  });

  server.on("/servo", HTTP_GET, [](AsyncWebServerRequest* request) {
    request->send(SPIFFS, SERVO_CONFIG_FILE_PATH, "application/json");
  });

  server.on("/servo", HTTP_POST, [](AsyncWebServerRequest* request) {
    if (!request->hasParam("shouldOpenIfTimeout", true) || !request->hasParam("servoOpenMs", true)) {
      request->send(400, "application/json", "{\"message\": \"missing required field\"}");
      return;
    }

    if (xSemaphoreTake(servoConfigMutex, portMAX_DELAY) != pdTRUE) {
      request->send(500, "text/plain", "internal server error");
      xSemaphoreGive(servoConfigMutex);

      return;
    }

    FileHandler fileHandler(SPIFFS);

    if (!fileHandler.readJson(SERVO_CONFIG_FILE_PATH, servoConfigDoc)){
      request->send(500, "application/json", "{\"message\": \"failed reading a file\"}");
      xSemaphoreGive(servoConfigMutex);

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

    servoConfigDoc["shouldOpenIfTimeout"] = shouldOpenIfTimeout;
    servoConfigDoc["servoOpenMs"] = servoOpenMs;

    if (!fileHandler.writeJson(SERVO_CONFIG_FILE_PATH, servoConfigDoc)) {
      request->send(500, "application/json", "{\"message\": \"failed writing to a file\"}");
      xSemaphoreGive(servoConfigMutex);

      return;
    }

    request->send(204);
    xSemaphoreGive(servoConfigMutex);
  });

  ws.onEvent(onEvent);

  server.addHandler(&ws);
  server.addHandler(feedingSchedulePostHandler);
  server.addHandler(urlListPostHandler);
  server.addHandler(wifiPostHandler);

  server.begin();

  configTime(0, 0, ntpServer);
  setenv("TZ", tzDoc["tz"].as<const char*>(), 1);
  tzset();

  // BaseType_t xReturned = xTaskCreate(simpleTask, "simpleTask", 512, nullptr, 2, nullptr);
  // if (xReturned == pdPASS) log_d("Successfully create task");
  // else log_d("Failed creating task");
  // vTaskStartScheduler();
}

void printLocalTime() {
  struct tm timeinfo;
  if(!getLocalTime(&timeinfo)){
    Serial.println("Failed to obtain time");
    return;
  }
  Serial.println(&timeinfo, "%A, %B %d %Y %H:%M:%S");
  Serial.print("Day of week: ");
  Serial.println(&timeinfo, "%A");
  Serial.print("Month: ");
  Serial.println(&timeinfo, "%B");
  Serial.print("Day of Month: ");
  Serial.println(&timeinfo, "%d");
  Serial.print("Year: ");
  Serial.println(&timeinfo, "%Y");
  Serial.print("Hour: ");
  Serial.println(&timeinfo, "%H");
  Serial.print("Hour (12 hour format): ");
  Serial.println(&timeinfo, "%I");
  Serial.print("Minute: ");
  Serial.println(&timeinfo, "%M");
  Serial.print("Second: ");
  Serial.println(&timeinfo, "%S");

  Serial.println("Time variables");
  char timeHour[3];
  strftime(timeHour,3, "%H", &timeinfo);
  Serial.println(timeHour);
  char timeWeekDay[10];
  strftime(timeWeekDay,10, "%A", &timeinfo);
  Serial.println(timeWeekDay);
  Serial.println();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    return;
  }
  
  struct tm timeinfo;

  log_d("Getting local time");
  if(!getLocalTime(&timeinfo)){
    return;
  }

  log_d("Successfully fetch local time");

  if (xSemaphoreGetMutexHolder(feedingScheduleMutex) == nullptr && 
      xSemaphoreGetMutexHolder(urlFileMutex) == nullptr         &&
      xSemaphoreGetMutexHolder(tzFileMutex) == nullptr          && 
      xSemaphoreGetMutexHolder(wifiFileMutex) == nullptr        &&
      xSemaphoreGetMutexHolder(servoConfigMutex) == nullptr) {

    if (!fileHandler.readJson(FEEDING_SCHEDULE_FILE_PATH, feedingScheduleDoc)) {
      return;
    }

    if (!fileHandler.readJson(URL_LIST_FILE_PATH, urlListDoc)) {
      return;
    }

    if (!fileHandler.readJson(SERVO_CONFIG_FILE_PATH, servoConfigDoc)) {
      return;
    }

    JsonArray arrDoc = feedingScheduleDoc.as<JsonArray>();
    for (JsonVariant v : arrDoc) {
      log_d("compare time: %d:%d | %d:%d", v["hour"].as<int>(), v["minutes"].as<int>(), timeinfo.tm_hour, timeinfo.tm_min);

      if (v["hour"].as<int>() == timeinfo.tm_hour && v["minutes"].as<int>() == timeinfo.tm_min) {
        String url = urlListDoc["aiUrl"].as<String>();
        if (url == nullptr || url == "") {
          log_w("AI URL is empty");          
          return;
        }

        int retries = 0;
        HTTPClient http;

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

        // String reqBody = "--camBoundary\r\nContent-Disposition: form-data; name=\"file_base64\"; filename=\"bowl_cam.jpg\"\r\nContent-Type: image/jpeg\r\n\r\n";
        // reqBody += encodedString;
        // reqBody += "\r\n--camBoundary--\r\n";
        String reqBody = "--camBoundary\r\nContent-Disposition: form-data; name=\"file_base64\"\r\n\r\n";
        reqBody += encodedString;
        reqBody += "\r\n--camBoundary--\r\n";

        log_d("start POST request");

        http.setTimeout(10000);
        http.begin(url.c_str());
        http.addHeader("Content-Type", "multipart/form-data; boundary=camBoundary");
        http.addHeader("Content-Length", String(reqBody.length()));
        http.addHeader("accept", "application/json");

        int httpCode = http.POST(reqBody);
        log_d("Status code: %d", httpCode);

        if (httpCode != HTTP_CODE_OK) {
          while (retries < 5) {
            retries++;
            delay(500);

            httpCode = http.POST(reqBody);
            if (httpCode == HTTP_CODE_OK)
              break;
          }
        }

        if (retries < 5) {
          log_d("Getting response body");
          String payload = http.getString();
          log_d("length: %d", payload.length());

          deserializeJson(httpPredictionResponseDoc, payload);
          serializeJson(httpPredictionResponseDoc, Serial);

          if (httpPredictionResponseDoc["top"].as<String>() != "full_bowl" && httpPredictionResponseDoc["top"].as<String>() != "floor") {
            // TODO: Handle response and logic to open servo
            log_d("Opening servo");
            // for (int pos = 0; pos <= 180; pos++) {
            //   servo.write(pos);
            //   delay(15);
            // }
  
            delay(servoConfigDoc["servoOpenMs"].as<unsigned int>());
  
  
            log_d("Closing servo");
            // for (int pos = 180; pos >= 0; pos--) {
              // servo.write(pos);
              // delay(15);
            // }
          }
        } else {
          if (servoConfigDoc["shouldOpenIfTimeout"].as<bool>()) {
            log_d("Opening servo");
            // for (int pos = 0; pos <= 180; pos++) {
              // servo.write(pos);
              // delay(15);
            // }

            delay(servoConfigDoc["servoOpenMs"].as<unsigned int>());


            log_d("Closing servo");
            // for (int pos = 180; pos >= 0; pos--) {
              // servo.write(pos);
              // delay(15);
            // }
          }
        }

        http.end();
        log_d("HTTPClient end() called");

        free(encodedString);
        log_d("Successfully deallocate base64 variable");
      }
    }

    delay(60000);
  } else {
    delay(1000);
  }
}

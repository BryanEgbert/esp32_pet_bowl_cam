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
SemaphoreHandle_t endpointMutex, feedingScheduleMutex, urlFileMutex;

FileHandler fileHandler(SPIFFS);

const char* ntpServer = "pool.ntp.org";
String nonce = "test";

DynamicJsonDocument feedingScheduleDoc(1024);
static DynamicJsonDocument urlListDoc(128);
DynamicJsonDocument httpPredictionResponseDoc(192);

bool isProcessed = false;

const char* FEEDING_SCHEDULE_FILE_PATH = "/feeding_schedule.json";
const char* URL_LIST_FILE_PATH = "/url_list.json";

void printLocalTime();

void onEvent(AsyncWebSocket * server, AsyncWebSocketClient * client, AwsEventType type, void * arg, uint8_t *data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    client->printf("Hello client %u | There are currently %u client(s)\n", client->id(), ws.count());
    Serial.printf("Hello client %u | There are currently %u client(s)\n", client->id(), ws.count());
  } else if(type == WS_EVT_DISCONNECT){
    //client disconnected
    Serial.printf("ws[%s][%u] disconnect: %u\n", server->url(), client->id());
  } else if(type == WS_EVT_DATA) {
    AwsFrameInfo * info = (AwsFrameInfo*)arg;

    if(info->final && info->index == 0 && info->len == len) {
      Serial.printf("ws[%s][%u] %s-message[%llu]: ", server->url(), client->id(), (info->opcode == WS_TEXT)?"text":"binary", info->len);

      if (info->opcode == WS_TEXT) {
        data[len] = 0;
        // Serial.printf("%s\n", (char*)data);
        if (String((char*)data) == String("request_image")) {
          camera_fb_t* fb = esp_camera_fb_get();
    
          client->binary(fb->buf, fb->len);
          esp_camera_fb_return(fb);
        }
      } else {
        for(size_t i=0; i < info ->len; i++){
          Serial.printf("%02x ", data[i]);
        }
          Serial.print("\n");

          client->binary("I gotchu binary\n");
      }
    }
  }
}

void WiFiEvent(WiFiEvent_t event) {
  switch(event) {
    case ARDUINO_EVENT_WIFI_AP_START:
      WiFi.softAPsetHostname("ESP32CAM_PetBowlCam");
      WiFi.softAPenableIpV6();

      Serial.print("AP IPv4: ");
      Serial.println(WiFi.softAPIP());
      break;

    case ARDUINO_EVENT_WIFI_STA_START:
      WiFi.setHostname("RumahCs_KM");
      break;
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      //enable sta ipv6 here
      WiFi.enableIpV6();
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP6:
      Serial.print("STA IPv6: ");
      Serial.println(WiFi.localIPv6());
      break;
    case ARDUINO_EVENT_WIFI_AP_GOT_IP6:
      Serial.print("AP IPv6: ");
      Serial.println(WiFi.softAPIPv6());
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Serial.print("STA Connected | IPv4: ");
      Serial.println(WiFi.localIP());
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      Serial.println("WiFi Disconnected");
      break;
    default:
      break;
  }
}

void feedTask(void *pvParameters) {
  (void) pvParameters;
  struct tm timeinfo;

  pinMode(LED_BUILTIN, OUTPUT);

  digitalWrite(LED_BUILTIN, HIGH);

  while (true) {
    if(!getLocalTime(&timeinfo)){
      continue;
    }

    if (xSemaphoreGetMutexHolder(feedingScheduleMutex) == nullptr) {
      vTaskDelay(500 / portTICK_PERIOD_MS);
      digitalWrite(LED_BUILTIN, LOW);

      vTaskDelay(500 / portTICK_PERIOD_MS);
      digitalWrite(LED_BUILTIN, HIGH);

      JsonArray arrDoc = feedingScheduleDoc.as<JsonArray>();
      for (JsonVariant v : arrDoc) {
        if(v["status"].as<bool>() == true) {
          continue;
        }


        // if (v["hour"].as<int>() == timeinfo.tm_hour && v["minutes"].as<int>() == timeinfo.tm_min) {
        //   int retries = 0;
        //   HTTPClient http;

        //   // camera_fb_t* fb = esp_camera_fb_get();
        //   // if (!fb) {
        //   //   continue;
        //   // }

        //   http.begin("https://google.com");
        //   // int httpCode = http.POST(fb->buf, fb->len);
        //   int httpCode = http.GET();

        //   // if (httpCode != HTTP_CODE_OK) {
        //   //   retries++;
        //   //   log_d("retries: %d", retries);
        //   // }
        //   // while (retries < 5) {
        //   // }

        //   // esp_camera_fb_return(fb);
        //   http.end();
        // }
      }

      vTaskDelay(5000 / portTICK_PERIOD_MS);
    } else {
      vTaskDelay(1000 / portTICK_PERIOD_MS);
    }

  }
}

void setup() {

  Serial.begin(115200);

  endpointMutex = xSemaphoreCreateMutex();
  feedingScheduleMutex = xSemaphoreCreateMutex();
  urlFileMutex = xSemaphoreCreateMutex();
  
  if (!SPIFFS.begin(true)) {
    Serial.println("Error mounting spiffs");
    return;
  }

  SPIFFS.remove("/pet_bowl_cam.db");
  SPIFFS.remove("/feeding_schedule.json");
  SPIFFS.remove("/url_list.json");

  if (!fileHandler.createFile(FEEDING_SCHEDULE_FILE_PATH)) {
    // fileHandler.writeJson(FEEDING_SCHEDULE_FILE_PATH, feedingScheduleDoc);
    log_d("Failed to create file content");
    return;
  }

  JsonArray feedingScheduleObj = feedingScheduleDoc.to<JsonArray>();

  if (!fileHandler.writeJson(FEEDING_SCHEDULE_FILE_PATH, feedingScheduleObj)) {
    log_d("Failed to write to file");
    return;
  }

  if (!fileHandler.readJson(FEEDING_SCHEDULE_FILE_PATH, feedingScheduleDoc)) {
    log_d("Failed to read file content");
    return;
  }

  if (!fileHandler.createFile(URL_LIST_FILE_PATH)) {
    // urlListDoc.set("{}");
    // fileHandler.writeJson(URL_LIST_FILE_PATH, urlListDoc);
    log_d("Failed to create file content");
    return;
  }

  JsonObject urlListObj = urlListDoc.to<JsonObject>();
  urlListObj["url"] = "";

  if (!fileHandler.writeJson(URL_LIST_FILE_PATH, urlListObj)) {
    log_d("Failed to write to file");
    return;
  }

  if (!fileHandler.readJson(URL_LIST_FILE_PATH, urlListDoc)) {
    log_d("Failed to read url file");
    return;
  }
  
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

  psramInit();

  byte* psdRamBuffer = (byte*)ps_malloc(1024 * 1024 * 4);

  // Init WiFi
  WiFi.mode(WIFI_AP_STA);
  WiFi.onEvent(WiFiEvent);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  WiFi.softAP(AP_SSID, AP_PASS);

  digitalWrite(LED_BUILTIN, HIGH);

  while (WiFi.status() != WL_CONNECTED) {
    digitalWrite(LED_BUILTIN, LOW);
    delay(1500);

    digitalWrite(LED_BUILTIN, HIGH);
    delay(1500);
  }

  digitalWrite(LED_BUILTIN, LOW);

  // Init Camera
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
  
    if (!json.containsKey("url")) {
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

    urlListDoc["url"] = json["url"];

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

    if (json.containsKey("id")) {
      int id = json["id"].as<int>();
      if (id < 1 || id > feedingScheduleDoc.as<JsonArray>().size()) {
        request->send(200);
        xSemaphoreGive(feedingScheduleMutex);

        return;
      }
      
      if (!json.containsKey("status")) {
        json["status"] = 0;
      }

      feedingScheduleDoc[id - 1]["hour"] = json["hour"];
      feedingScheduleDoc[id - 1]["minutes"] = json["minutes"];
      feedingScheduleDoc[id - 1]["seconds"] = json["seconds"];
      feedingScheduleDoc[id - 1]["status"] = json["status"];

      feedingScheduleDoc.garbageCollect();
    } else {
      json["status"] = 0;

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

    // File file = SPIFFS.open(FEEDING_SCHEDULE_FILE_PATH, FILE_WRITE);
    // if (!file) {
    //   request->send(500, "application/json", "{\"message\": \"failed opening a file\"}\n");
    //   file.close();
    //   xSemaphoreGive(feedingScheduleMutex);

    //   return;
    // }

    // serializeJson(feedingScheduleDoc, file);

    // file.close();
    request->send(200);

    xSemaphoreGive(feedingScheduleMutex);
  });

  ws.onEvent(onEvent);

  server.addHandler(&ws);
  server.addHandler(feedingSchedulePostHandler);
  server.addHandler(urlListPostHandler);

  server.begin();

  configTime(0, 0, ntpServer);
  setenv("TZ", "WIB-7", 1);
  tzset();
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
  struct tm timeinfo;

  log_d("Getting local time");
  if(!getLocalTime(&timeinfo)){
    return;
  } else {
    log_d("Successfully fetch local time");
  }

  if (xSemaphoreGetMutexHolder(feedingScheduleMutex) == nullptr) {
    if (!fileHandler.readJson(FEEDING_SCHEDULE_FILE_PATH, feedingScheduleDoc)) {
      return;
    }

    if (!fileHandler.readJson(URL_LIST_FILE_PATH, urlListDoc)) {
      return;
    }

    JsonArray arrDoc = feedingScheduleDoc.as<JsonArray>();
    for (JsonVariant v : arrDoc) {
      if(v["status"].as<bool>() == true) {
        log_d("Status is true");
        continue;
      }

      log_d("compare time: %d:%d | %d:%d", v["hour"].as<int>(), v["minutes"].as<int>(), timeinfo.tm_hour, timeinfo.tm_min);
      if (v["hour"].as<int>() == timeinfo.tm_hour && v["minutes"].as<int>() == timeinfo.tm_min) {
        int retries = 0;
        
        HTTPClient http;
        const char* boundary = "camBoundary";

        camera_fb_t* fb = esp_camera_fb_get();
        if (!fb) {
          log_d("Failed at getting camera framebuffer");
          continue;
        }


        int encodedLength = Base64.encodedLength(fb->len);
        log_d("Length: %d", encodedLength);

        char* encodedString = (char*)malloc(encodedLength * sizeof(char) + 1);
        // String encodedString;
        Base64.encode(encodedString, reinterpret_cast<char*>(fb->buf), fb->len);

        esp_camera_fb_return(fb);
        log_d("Deallocate camera framebuffer");

        const char* url = urlListDoc["url"];
        log_d("url: %s", urlListDoc["url"].as<const char*>());

        String reqBody = "--camBoundary\r\nContent-Disposition: form-data; name=\"file\"; filename=\"bowl_cam.jpg\"\r\nContent-Type: image/jpeg\r\n\r\n";
        reqBody += encodedString;
        reqBody += "\r\n--camBoundary--\r\n";

        log_d("start POST request");

        http.setTimeout(1000);
        http.begin(urlListDoc["url"].as<const char*>());
        http.addHeader("Content-Type", "multipart/form-data; boundary=camBoundary");
        http.addHeader("accept", "application/json");

        int httpCode = http.POST(reqBody);
        log_d("Status code: %d", httpCode);

        if (httpCode != HTTP_CODE_OK) {
          while (retries < 5) {
            retries++;
            httpCode = http.POST(reqBody);
          }
        }

        log_d("Getting response body");
        String payload = http.getString();
        log_d("length: %d", payload.length());

        deserializeJson(httpPredictionResponseDoc, payload);
        serializeJson(httpPredictionResponseDoc, Serial);

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
  // // put your main code here, to run repeatedly:
  // digitalWrite(LED_BUILTIN, HIGH);

  // delay(2000);

  // // if(WiFi.status() == WL_CONNECTED) {
  // //   http.begin
  // // }

  // // printLocalTime();
  // digitalWrite(LED_BUILTIN, LOW);

  // delay(5000);

  // JsonArray arrDoc = feedingScheduleDoc.as<JsonArray>();
  // for (JsonVariant value : arrDoc) {
  //   // Access individual elements using the [] operator
  //   int hour = value["hour"];
  //   int minutes = value["minutes"];
  //   int seconds = value["seconds"];
  //   int status = value["status"];

  //   // Print the values using Serial.printf
  //   Serial.printf("Hour: %d, Minutes: %d, Seconds: %d, Status: %d\n", hour, minutes, seconds, status);
  // }

  // delay(5000);
}

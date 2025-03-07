/*********
  Based on Rui Santos ESP32 CAM Project:
  https://RandomNerdTutorials.com/esp32-cam-video-streaming-web-server-camera-home-assistant/
  
  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files.

  The above copyright notice and this permission notice shall be included in all
  copies or substantial portions of the Software.

  Adapted to XIAO ESP32S3 Sense by MJRovai 02May23
  
*********/

#include "esp_camera.h"
#include <WiFi.h>
#include "esp_timer.h"
#include "img_converters.h"
#include "Arduino.h"
#include "fb_gfx.h"
#include "soc/soc.h" //disable brownout problems
#include "soc/rtc_cntl_reg.h"  //disable brownout problems
#include "esp_http_server.h"
#include <ESPmDNS.h>
#include <string>

#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include <BLE2902.h>


//Replace with your network credentials

String ssid     = "esp32";
String password = "esp32esp32";
String host = "esp32";
IPAddress ip(192,168,1,200);     
IPAddress gateway(192,168,1,1);   
IPAddress subnet(255,255,255,0);

BLECharacteristic* pCharacteristic = NULL;
BLECharacteristic* pLedCharacteristic = NULL;
BLECharacteristic* pNameCharacteristic = NULL;
BLECharacteristic* pPwdCharacteristic = NULL;
BLECharacteristic* pSECharacteristic = NULL;

BLEServer* pServer = NULL;
bool deviceConnected = false;
bool oldDeviceConnected = false;

#define SERVICE_UUID        "19b10000-e8f2-537e-4f6c-d104768a1214"
#define CHARACTERISTIC_UUID "19b10001-e8f2-537e-4f6c-d104768a1214"
#define NAME_CHARACTERISTIC_UUID "19b10003-e8f2-537e-4f6c-d104768a1214"
#define PWD_CHARACTERISTIC_UUID "19b10004-e8f2-537e-4f6c-d104768a1214"
#define LED_CHARACTERISTIC_UUID "19b10002-e8f2-537e-4f6c-d104768a1214"
#define SE_CHARACTERISTIC_UUID "19b10005-e8f2-537e-4f6c-d104768a1214"

#define PART_BOUNDARY "123456789000000000000987654321"

#define CAMERA_MODEL_XIAO_ESP32S3 // Has PSRAM
#include "camera_pins.h"

static const char* _STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* _STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char* _STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

httpd_handle_t stream_httpd = NULL;

WiFiServer server(80);

static esp_err_t stream_handler(httpd_req_t *req){
  camera_fb_t * fb = NULL;
  esp_err_t res = ESP_OK;
  size_t _jpg_buf_len = 0;
  uint8_t * _jpg_buf = NULL;
  char * part_buf[64];

  res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
  if(res != ESP_OK){
    return res;
  }

  while(true){
    fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("Camera capture failed");
      res = ESP_FAIL;
    } else {
      if(fb->width > 400){
        if(fb->format != PIXFORMAT_JPEG){
          bool jpeg_converted = frame2jpg(fb, 80, &_jpg_buf, &_jpg_buf_len);
          esp_camera_fb_return(fb);
          fb = NULL;
          if(!jpeg_converted){
            Serial.println("JPEG compression failed");
            res = ESP_FAIL;
          }
        } else {
          _jpg_buf_len = fb->len;
          _jpg_buf = fb->buf;
        }
      }
    }
    if(res == ESP_OK){
      size_t hlen = snprintf((char *)part_buf, 64, _STREAM_PART, _jpg_buf_len);
      res = httpd_resp_send_chunk(req, (const char *)part_buf, hlen);
    }
    if(res == ESP_OK){
      res = httpd_resp_send_chunk(req, (const char *)_jpg_buf, _jpg_buf_len);
    }
    if(res == ESP_OK){
      res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
    }
    if(fb){
      esp_camera_fb_return(fb);
      fb = NULL;
      _jpg_buf = NULL;
    } else if(_jpg_buf){
      free(_jpg_buf);
      _jpg_buf = NULL;
    }
    if(res != ESP_OK){
      break;
    }
    //Serial.printf("MJPG: %uB\n",(uint32_t)(_jpg_buf_len));
  }
  return res;
}

void startCameraServer(){
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 81;

  httpd_uri_t index_uri = {
    .uri       = "/",
    .method    = HTTP_GET,
    .handler   = stream_handler,
    .user_ctx  = NULL
  };
  
  //Serial.printf("Starting web server on port: '%d'\n", config.server_port);
  if (httpd_start(&stream_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(stream_httpd, &index_uri);
  }
}

void setupmDNS(){
  int totalTry = 5;
  while (!MDNS.begin(host) && totalTry > 0) {
      Serial.println("*");
      delay(1000);
      totalTry--;
  }
  Serial.println("[Wifi] mDNS responder started");
  Serial.print("[Wifi] You can now connect to: http://");
  Serial.print(host);
  Serial.println(".local");
}

class MyServerCallbacks: public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) {
    deviceConnected = true;
  };

  void onDisconnect(BLEServer* pServer) {
    deviceConnected = false;
  }
};

void connectToWifi(){
  ssid = pNameCharacteristic->getValue().c_str();
  password = pPwdCharacteristic -> getValue().c_str();
  Serial.print("___wifi name: ");
  Serial.println(ssid);
  Serial.print("___wifi pwd: ");
  Serial.println(password);
  
  char _ssid[50];
  ssid.toCharArray(_ssid, 50);
  char _password[50];
  password.toCharArray(_password, 50);

  WiFi.begin(_ssid,_password);
  int totalTry = 5;
  while (WiFi.status() != WL_CONNECTED && totalTry > 0) {
    delay(500);
    Serial.print(".");
    totalTry--;
  }  

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("");
    Serial.println("WiFi connected");

    Serial.print("Camera Stream Ready! Go to: http://");
    Serial.print(WiFi.localIP());
    //server.begin();

    // Start streaming web server
    startCameraServer();

    // set the wifiState characteristic
    String myValue    = "connected";
    pLedCharacteristic -> setValue(myValue.c_str());
    Serial.print("Setting pLedCharacteristic to: " + myValue);

    // set new ip to the characteristic
    Serial.println("changing the characteristic of ip");
    pCharacteristic->setValue(WiFi.localIP().toString().c_str());
    pCharacteristic->notify();
  
  } else {
    Serial.println("");
    Serial.println("WiFi NOT connected");
    // set the wifiState characteristic
    String myValue    = "disconnected";
    pLedCharacteristic -> setValue(myValue.c_str());

    // set new ip to the characteristic
    Serial.println("changing the characteristic of ip");
    String myValue2    = "none";
    pCharacteristic->setValue(myValue2.c_str());
    pCharacteristic->notify();    
  }


};


class MyCharacteristicCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pLedCharacteristic) {
    String value = pLedCharacteristic->getValue().c_str();
    Serial.println("Characteristic event, written: " + value);
    
    if (value == "connecting"){
      Serial.println("try to connect to wifi ...");
      connectToWifi();
      delay(1000);

    } else {
      Serial.println( " not connecting ");
    }

    

    //}
  }
};

class MySECharacteristicCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pSECharacteristic) {
    String value = pSECharacteristic->getValue().c_str();
    Serial.println("SE Characteristic event, written: " + value);
    
    sensor_t * s = esp_camera_sensor_get();

    s->set_special_effect(s, 0);

    if (value == "1"){
      Serial.println("setting special effect 1");
      s->set_special_effect(s, 1);
    } else {
      Serial.println("setting special effect 0");
      s->set_special_effect(s, 0);
    }

  }
};



void setup() {

  // initialize digital pin LED_BUILTIN as an output.
  pinMode(LED_BUILTIN, OUTPUT);

  Serial.begin(115200);

  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0); //disable brownout detector
 
  //Serial.begin(115200);
  //while(!Serial); // When the serial monitor is turned on, the program starts to execute

  Serial.setDebugOutput(false);
  
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
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
//  config.frame_size = FRAMESIZE_UXGA;
  config.frame_size = FRAMESIZE_VGA;

  config.pixel_format = PIXFORMAT_JPEG; // for streaming
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.jpeg_quality = 12;
  config.fb_count = 1;
  
  
  // Camera init
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x", err);
    return;
  }
  // Wi-Fi connection

  pinMode(LED_BUILTIN, OUTPUT);      // set the LED pin mode
  digitalWrite(LED_BUILTIN, HIGH);  // set LED off

  delay(10);

  //WiFi.softAP(ssid, password);
  //WiFi.softAPConfig(ip, gateway, subnet);
  //WiFi.mode(WIFI_STA);
  //WiFi.config(ip, gateway, subnet);
 
  const char* myssid = "none";
  const char* mypass = "none";
  WiFi.begin(myssid,mypass);
 
  //WiFi.begin(ssid, password);
  //while (WiFi.status() != WL_CONNECTED) {
  //  delay(500);
  //  Serial.print(".");
  //}
  //Serial.println("");
  //Serial.println("WiFi connected");
  
  //Serial.print("Camera Stream Ready! Go to: http://");
  //Serial.print(WiFi.localIP());
  //Serial.println(WiFi.softAPIP());
  //setupmDNS();

  BLEDevice::init("ESP32");
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());
  BLEService *pService = pServer->createService(SERVICE_UUID);
  
  // Create BLE Characteristic
    pCharacteristic = pService->createCharacteristic(
                                         CHARACTERISTIC_UUID,
                                         BLECharacteristic::PROPERTY_READ   |
                                         BLECharacteristic::PROPERTY_WRITE  |
                                         BLECharacteristic::PROPERTY_NOTIFY |
                                         BLECharacteristic::PROPERTY_INDICATE
                                       );
  pCharacteristic->addDescriptor(new BLE2902());

  // Create the ON button Characteristic
  pLedCharacteristic = pService->createCharacteristic(
    LED_CHARACTERISTIC_UUID,
    BLECharacteristic::PROPERTY_WRITE |
    BLECharacteristic::PROPERTY_READ 
  );
  pLedCharacteristic->addDescriptor(new BLE2902());

  // Register the callback for the ON button characteristic
  pLedCharacteristic->setCallbacks(new MyCharacteristicCallbacks());

  // Create the wifiName  Characteristic
  pNameCharacteristic = pService->createCharacteristic(
    NAME_CHARACTERISTIC_UUID,
    BLECharacteristic::PROPERTY_WRITE
  );
  pNameCharacteristic->addDescriptor(new BLE2902());

  // Create the wifiPwd  Characteristic
  pPwdCharacteristic = pService->createCharacteristic(
    PWD_CHARACTERISTIC_UUID,
    BLECharacteristic::PROPERTY_WRITE
  );
  pPwdCharacteristic->addDescriptor(new BLE2902());

  // Create the special Effect Characteristic
  pSECharacteristic = pService->createCharacteristic(
    SE_CHARACTERISTIC_UUID,
    BLECharacteristic::PROPERTY_WRITE
  );
  pSECharacteristic->addDescriptor(new BLE2902());
  // Register the callback for the ON button characteristic
  pSECharacteristic->setCallbacks(new MySECharacteristicCallbacks());


  //pCharacteristic->setValue(String("Hello World says Neil").c_str());
  pService->start();
  // BLEAdvertising *pAdvertising = pServer->getAdvertising();  // this still is working for backward compatibility
  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(false);
  pAdvertising->setMinPreferred(0x0);  // set value to 0x00 to not advertise this parameter
  BLEDevice::startAdvertising();
  Serial.println("Characteristic defined! Now you can read it in your phone!");



  //pCharacteristic->setValue(WiFi.localIP().toString());
  //pCharacteristic->setValue(WiFi.localIP().toString().c_str());
  //pCharacteristic->setValue("ahoj");



}

void loop() {


  ssid = pNameCharacteristic->getValue().c_str();
  password = pPwdCharacteristic -> getValue().c_str();
  Serial.print("wifi name: ");
  Serial.println(ssid);
  Serial.print("wifi pwd: ");
  Serial.println(password);
  String value = pLedCharacteristic->getValue().c_str();
  Serial.print("pLedCharacteristic: ");
  Serial.println(value);
  String value2 = pCharacteristic->getValue().c_str();
  Serial.print("pCharacteristic: ");
  Serial.println(value2);
  String value3 = pSECharacteristic->getValue().c_str();
  Serial.print("pSECharacteristic: ");
  Serial.println(value3);
  
  digitalWrite(LED_BUILTIN, HIGH);  // turn the LED on (HIGH is the voltage level)
  delay(1500);                      // wait for a second
  digitalWrite(LED_BUILTIN, LOW);   // turn the LED off by making the voltage LOW
  delay(1500);                      // wait for a second
  


  // disconnecting
  if (!deviceConnected && oldDeviceConnected) {
    Serial.println("Device disconnected.");
    delay(500); // give the bluetooth stack the chance to get things ready
    pServer->startAdvertising(); // restart advertising
    Serial.println("Start advertising");
    oldDeviceConnected = deviceConnected;
  }
  // connecting
  if (deviceConnected && !oldDeviceConnected) {
    // do stuff here on connecting
    oldDeviceConnected = deviceConnected;
    Serial.println("Device Connected");
    delay(500); // give the bluetooth stack the chance to get things ready
  }

  
}
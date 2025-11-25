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
#include <HTTPClient.h>

#include "index_html_gz.h"


//Replace with your network credentials
String ssid     = "esp32";
String password = "";
String host = "esp32";
int access_point = 0;
String ipCam2 = "0";
String ipAP = "192.168.4.1";
//IPAddress ip(192,168,1,200);     
//PAddress gateway(192,168,1,1);   
//IPAddress subnet(255,255,255,0);
//IPAddress ip(192,168,0,1);     
//IPAddress gateway(192,168,0,1);   
//IPAddress subnet(255,255,255,0);

int timing = 0;

uint32_t delayValue = 1500;
uint32_t xPosition = 100;
uint32_t yPosition = 200;

#define PART_BOUNDARY "123456789000000000000987654321"

#define CAMERA_MODEL_XIAO_ESP32S3 // Has PSRAM
#include "camera_pins.h"

static const char* _STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* _STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char* _STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

httpd_handle_t stream_httpd = NULL;
httpd_handle_t camera_httpd = NULL;

WiFiServer server(80);

// Handler for "/ping"
static esp_err_t ping_handler(httpd_req_t *req)
{
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  const char *resp = "{\"status\":\"ok\",\"ping\":1}";
  return httpd_resp_send(req, resp, strlen(resp));
}

// Handler for Camera Control "/camera /camera?set=quality&value=30"
static esp_err_t camera_control_handler(httpd_req_t *req)
{
    // CORS headers
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    if (req->method == HTTP_OPTIONS) {
        httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, OPTIONS");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "*");
        return httpd_resp_send(req, NULL, 0);
    }

    sensor_t *s = esp_camera_sensor_get();
    if (!s) {
        return httpd_resp_send_500(req);
    }

    char query[256];
    char param[32];
    char value_str[16];
    int value = 0;
    bool do_set = false;

    // Read full query string
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {

        if (httpd_query_key_value(query, "set", param, sizeof(param)) == ESP_OK &&
            httpd_query_key_value(query, "value", value_str, sizeof(value_str)) == ESP_OK)
        {
            do_set = true;
            value = atoi(value_str);
        }
    }

    /* -------------------------
        SET SECTION
       ------------------------- */
    if (do_set)
    {
        if (strcmp(param, "quality") == 0) {
            if (value < 1) value = 1;
            if (value > 63) value = 63;
            s->set_quality(s, value);
        }
        else if (strcmp(param, "ae") == 0) {
            value = (value != 0) ? 1 : 0;
            s->set_aec2(s, value);
            s->set_aec_value(s, s->status.aec_value); // refresh exposure
        }
        else if (strcmp(param, "exposure") == 0) {
            if (s->status.aec == 0) { // only if AE disabled
                if (value < 0) value = 0;
                if (value > 1200) value = 1200;
                s->set_aec_value(s, value);
            }
        }
        else if (strcmp(param, "gain") == 0) {
            if (value < 0) value = 0;
            if (value > 6) value = 6; // typical max for gainceiling enum
            s->set_gainceiling(s, (gainceiling_t)value);
        }
        else if (strcmp(param, "brightness") == 0) {
            if (value < -2) value = -2;
            if (value > 2)  value = 2;
            s->set_brightness(s, value);
        }
        else if (strcmp(param, "contrast") == 0) {
            if (value < -2) value = -2;
            if (value > 2)  value = 2;
            s->set_contrast(s, value);
        }
        else if (strcmp(param, "saturation") == 0) {
            if (value < -2) value = -2;
            if (value > 2)  value = 2;
            s->set_saturation(s, value);
        }
        else if (strcmp(param, "framesize") == 0) {
          if (value >= 0 && value < FRAMESIZE_INVALID) {
              s->set_framesize(s, (framesize_t)value);
          }
        }
        else if (strcmp(param, "ip") == 0) {
            ipCam2 = String(value_str);
        }
        else {
            httpd_resp_set_type(req, "application/json");
            return httpd_resp_sendstr(req, "{\"error\":\"unknown_parameter\"}");
        }
    }

    
    /* -------------------------
        GET SECTION — JSON
       ------------------------- */
    int quality      = s->status.quality;
    int exposure     = s->status.aec_value;
    int auto_exp     = s->status.aec;
    int gain         = s->status.gainceiling;
    int brightness   = s->status.brightness;
    int contrast     = s->status.contrast;
    int saturation   = s->status.saturation;
    int framesize    = s->status.framesize;

    char json[256];
    snprintf(json, sizeof(json),
        "{\"quality\":%d,\"exposure\":%d,\"auto_exposure\":%d,"
        "\"gain\":%d,\"brightness\":%d,\"contrast\":%d,"
        "\"saturation\":%d,\"framesize\":%d, \"ipCam2\":\"%s\"}",
        quality, exposure, auto_exp,
        gain, brightness, contrast, saturation, framesize, ipCam2.c_str()
    );

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}


// Handler for "/index"
static esp_err_t index_handler(httpd_req_t *req) {
  esp_err_t res = ESP_OK;
  
  res = httpd_resp_set_type(req, "text/html");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Content-Encoding", "gzip");

  if(res != ESP_OK){
    return res;
  }

  return httpd_resp_send(req, (const char *)index_html_gz, index_html_gz_len);
 
}

// Handler for "/stream"
static esp_err_t stream_handler(httpd_req_t *req){
  camera_fb_t * fb = NULL;
  esp_err_t res = ESP_OK;
  size_t _jpg_buf_len = 0;
  uint8_t * _jpg_buf = NULL;
  char * part_buf[64];

  // stream the image
  
  res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
  if(res != ESP_OK){
    return res;
  }

  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "X-Framerate", "60");
  
  while(true){
    fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("Camera capture failed");
      res = ESP_FAIL;
    } else {
      if(fb->width > 200){
        if(fb->format != PIXFORMAT_JPEG){
          // add a lines to the image
          unsigned short imageWidth = fb->width;
          unsigned short imageHeight = fb->height;

          // horizontal line
          for(int i = 0; i < imageWidth ; i++){
            fb->buf[(i+yPosition*imageWidth)*2] = 255;  // white line
            fb->buf[(i+yPosition*imageWidth)*2+1] = 255;  // white line - second byte (for RGB)
          }
          // vertical line
          for(int i = 0; i < imageHeight ; i++){
            fb->buf[xPosition*2 +i*imageWidth*2] = 255;  // white line
            fb->buf[xPosition*2+i*imageWidth*2+1] = 255;  // white line -- second byte (for RGB)

          }

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
  config.server_port = 80;

  httpd_uri_t index_uri = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = index_handler,
    .user_ctx = NULL
    #ifdef CONFIG_HTTPD_WS_SUPPORT
    ,
    .is_websocket = true,
    .handle_ws_control_frames = false,
    .supported_subprotocol = NULL
    #endif
  };

  httpd_uri_t stream_uri = {
    .uri = "/stream",
    .method = HTTP_GET,
    .handler = stream_handler,
    .user_ctx = NULL
#ifdef CONFIG_HTTPD_WS_SUPPORT
    ,
    .is_websocket = true,
    .handle_ws_control_frames = false,
    .supported_subprotocol = NULL
#endif
  };

httpd_uri_t ping_uri = {
    .uri       = "/ping",
    .method    = HTTP_GET,
    .handler   = ping_handler,
    .user_ctx  = NULL
};

httpd_uri_t cam_ctrl = {
    .uri = "/camera",
    .method = HTTP_GET,
    .handler = camera_control_handler,
    .user_ctx = NULL
};


  Serial.printf("Starting web server on port: '%d'\n", config.server_port);
  if (httpd_start(&camera_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(camera_httpd, &index_uri);
    httpd_register_uri_handler(camera_httpd, &ping_uri);
    httpd_register_uri_handler(camera_httpd, &cam_ctrl);
  }
  
  config.server_port += 1;
  config.ctrl_port += 1;
  Serial.printf("Starting stream server on port: '%d'", config.server_port);
  if (httpd_start(&stream_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(stream_httpd, &stream_uri);
  };
};

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
};

void connectToWifi(){

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid,password);
  int totalTry = 10;
  while (WiFi.status() != WL_CONNECTED && totalTry > 0) {
    delay(500);
    Serial.print(".");
    totalTry--;
  }  

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("");
    Serial.println("WiFi connected");

    Serial.print("Camera Stream Ready! Go to: http://");
    ipCam2 = String(WiFi.localIP().toString());
    Serial.print(ipCam2);
    server.begin();
    delayValue = 300;
    access_point = 0;

    // send ip to the AP
    String url = "http://" + ipAP + "/camera?set=ip&value=" + ipCam2;
    Serial.print("sending IP to AP: ");
    Serial.println(url);


    // Send IP via GET request
    HTTPClient http;
    http.begin(url);

    int httpCode = http.GET();
    if (httpCode > 0) {
      Serial.printf("Server response code: %d\n", httpCode);
      Serial.println(http.getString());
    } else {
      Serial.printf("Failed to send request: %s\n", http.errorToString(httpCode).c_str());
    }
    http.end();

  } else {
    Serial.println("\n[*] Creating AP");
    WiFi.mode(WIFI_AP);
    //WiFi.softAPConfig(ip, gateway, subnet);
    WiFi.softAP(ssid, password);
    Serial.print("[+] AP Created with IP Gateway ");
    Serial.println(WiFi.softAPIP());
    delayValue = 1500;
    access_point= 1;
  }

  // Start streaming web server
  startCameraServer();
  setupmDNS();
};

void setup() {

  // initialize digital pin LED_BUILTIN as an output.
  pinMode(LED_BUILTIN, OUTPUT);

  Serial.begin(115200);

  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0); //disable brownout detector
 
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
  //config.frame_size = FRAMESIZE_UXGA;
  config.frame_size = FRAMESIZE_VGA;
  //config.frame_size =  FRAMESIZE_QVGA;


  config.pixel_format = PIXFORMAT_JPEG; // for streaming
  //config.pixel_format = PIXFORMAT_RGB565; // for image modification
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.jpeg_quality = 64; //high quality
  config.fb_count = 1;
  
  // Camera init
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x", err);
    return;
  };

  // Set initial Camera Parameters
  sensor_t *s = esp_camera_sensor_get();
  s->set_exposure_ctrl(s, 0);             // disable auto-exposure
  s->set_aec_value(s, 10);       // low exposure
  s->set_gainceiling(s, GAINCEILING_2X); // lowest gain
  s->set_brightness(s, 0);      // digital brightness offset
  s->set_contrast(s, 0);        // optional: increase darkness of shadows


  pinMode(LED_BUILTIN, OUTPUT);      // set the LED pin mode
  digitalWrite(LED_BUILTIN, HIGH);  // set LED off

  delay(10);


  // Wi-Fi connection
  connectToWifi();

}

void loop() {

  Serial.println("");
  Serial.print("wifi name: ");
  Serial.println(ssid);
  Serial.print("wifi pwd: ");
  Serial.println(password);
  Serial.print("timing: ");
  Serial.println(timing);
  timing += 1;

 if (access_point == 1) {
    Serial.print("[+] AP Created with IP Gateway ");
    Serial.println(WiFi.softAPIP());
 }
 else {
    Serial.print("[+] camera IP: ");
    Serial.println(ipCam2);
    Serial.print("WIFI strength: ");
    Serial.println (WiFi.RSSI());
 }

  digitalWrite(LED_BUILTIN, HIGH);  // turn the LED on (HIGH is the voltage level)
  delay(delayValue);                      // wait for a second
  digitalWrite(LED_BUILTIN, LOW);   // turn the LED off by making the voltage LOW
  delay(delayValue);                      // wait for a second
}

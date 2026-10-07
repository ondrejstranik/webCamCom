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
#include <atomic>
#include <HTTPClient.h>
#include "lwip/sockets.h"

#include "index_html_gz.h"
#include "fake_internet.h"
#include "ap_wifi.h"


//Replace with your network credentials
String ssid     = "esp32";
String password = "";
String host = "esp32";
int access_point = 0;
String ipAP = "192.168.4.1";

// all cameras run the same firmware: the first device that finds no "esp32" network
// creates it (access point, 192.168.4.1), every further device joins it as station.
// the LED board (ledCom firmware) follows the same rules, so it can be the access point too
#define WIFI_CHANNEL 6                  // fixed channel, so joining and scanning is fast
#define STA_CONNECT_TIMEOUT_MS 10000    // how long to look for an existing access point at boot
#define STA_LOST_RESTART_MS 30000       // station: restart (and re-decide the role) after this long without the access point
#define REGISTER_INTERVAL_MS 5000       // station: how often the own ip is sent to the access point
#define CAM_TIMEOUT_MS 15000            // access point: forget a station camera / LED board when it was not heard for this long
#define MAX_CAMERAS 5                   // access point camera (number 1) + up to 4 station cameras
#define MAX_LEDS 2                      // LED boards (ledCom firmware) that can register
#define MAX_WIFI_CLIENTS 8              // devices on the access point: station cameras + phones/computers
// access point: how often to look for a second access point with the same name.
// a duplicate can only appear when both cameras start together, and every scan
// pauses the access point shortly, so scan often only during the first minutes
#define AP_SCAN_INTERVAL_MS 10000
#define AP_SCAN_INTERVAL_LATE_MS 60000
#define AP_SCAN_FAST_PERIOD_MS 120000

// own camera number, shown by the LED blinking: 1 = access point,
// 2.. = assigned by the access point at registration, 0 = not known yet
static volatile int camNumber = 0;

// camera sensor of this board (OV2640 or OV3660 on the XIAO ESP32S3 Sense), set in setCamDefault()
static const char *sensorName = "unknown";

// access point: the devices that registered (station cameras and LED boards).
// slot i of a list is device number i + firstNumber.
// written by the http task (registration) and loop() (timeout), guarded by devMux
#define MAX_SLOTS 4
struct Slot {
  char ip[16];          // "" = slot free
  uint32_t lastSeen;    // millis() of the last registration
};
struct DeviceList {
  const char *name;     // for the serial log
  int firstNumber;
  int count;
  Slot slots[MAX_SLOTS];
};
static DeviceList cams = {"camera", 2, MAX_CAMERAS - 1, {}};
static DeviceList leds = {"LED", 1, MAX_LEDS, {}};
static portMUX_TYPE devMux = portMUX_INITIALIZER_UNLOCKED;

// returns the device number for ip (an ip keeps its number while it registers
// regularly), 0 when all slots are taken
static int registerDevice(DeviceList &list, const char *ip)
{
  int slot = -1;
  portENTER_CRITICAL(&devMux);
  int freeSlot = -1;
  for (int i = 0; i < list.count; i++) {
    if (strcmp(list.slots[i].ip, ip) == 0) { slot = i; break; }
    if (list.slots[i].ip[0] == 0 && freeSlot < 0) freeSlot = i;
  }
  if (slot < 0 && freeSlot >= 0) {
    slot = freeSlot;
    strlcpy(list.slots[slot].ip, ip, sizeof(list.slots[slot].ip));
  }
  if (slot >= 0) list.slots[slot].lastSeen = millis();
  portEXIT_CRITICAL(&devMux);
  return slot < 0 ? 0 : slot + list.firstNumber;
}

// free the slots of devices that stopped registering
static void expireDevices(DeviceList &list)
{
  uint32_t now = millis();
  for (int i = 0; i < list.count; i++) {
    bool expired = false;
    portENTER_CRITICAL(&devMux);
    if (list.slots[i].ip[0] != 0 && now - list.slots[i].lastSeen > CAM_TIMEOUT_MS) {
      list.slots[i].ip[0] = 0;
      expired = true;
    }
    portEXIT_CRITICAL(&devMux);
    if (expired) Serial.printf("%s %d lost\n", list.name, i + list.firstNumber);
  }
}

// JSON list of the registered devices: [{"n":2,"ip":"192.168.4.2"},...].
// selfIp: this device is part of the list too, with number firstNumber - 1
static void devicesJson(DeviceList &list, char *out, size_t size, const char *selfIp = NULL)
{
  Slot copy[MAX_SLOTS];
  portENTER_CRITICAL(&devMux);
  memcpy(copy, list.slots, sizeof(copy));
  portEXIT_CRITICAL(&devMux);

  size_t used = snprintf(out, size, "[");
  if (selfIp) used += snprintf(out + used, size - used, "{\"n\":%d,\"ip\":\"%s\"}", list.firstNumber - 1, selfIp);
  for (int i = 0; i < list.count && used < size; i++) {
    if (copy[i].ip[0] == 0) continue;
    used += snprintf(out + used, size - used, "%s{\"n\":%d,\"ip\":\"%s\"}",
                     used > 1 ? "," : "", i + list.firstNumber, copy[i].ip);
  }
  if (used < size) snprintf(out + used, size - used, "]");
}
//IPAddress ip(192,168,1,200);     
//PAddress gateway(192,168,1,1);   
//IPAddress subnet(255,255,255,0);
//IPAddress ip(192,168,0,1);     
//IPAddress gateway(192,168,0,1);   
//IPAddress subnet(255,255,255,0);

int timing = 0;
int allAuto = 1;   // 1 = automatic control ON, 0 = manual

uint32_t delayValue = 1500;
uint32_t xPosition = 100;
uint32_t yPosition = 200;

#define PART_BOUNDARY "123456789000000000000987654321"

#define CAMERA_MODEL_XIAO_ESP32S3 // Has PSRAM
#include "camera_pins.h"

#define STREAM_PORT 81
#define MAX_STREAM_CLIENTS 4       // simultaneous /stream viewers (phone, python, ...)
// camera clock; the test environment "xclk10" in platformio.ini builds with 10 MHz
// for a board whose image data gets corrupted at full speed (colored bands)
#ifndef XCLK_MHZ
#define XCLK_MHZ 20
#endif
#define MAX_FRAMESIZE FRAMESIZE_UXGA   // largest allowed frame size (1600x1200, the OV2640 maximum), e.g. for focusing
#define DEFAULT_FRAMESIZE FRAMESIZE_QVGA   // frame size after start (320x240), light on the WiFi
#define DEFAULT_QUALITY 10         // jpeg quality 1 (best) - 63 (worst)
#define DEFAULT_MAX_FPS 12         // frames per second sent to every viewer, limits the WiFi load
#define MAX_FPS_LIMIT 50           // the OV2640 delivers at most ~50 fps (up to 400x296, 20 MHz clock)

static volatile int maxFps = DEFAULT_MAX_FPS;

static const char* _STREAM_HEADER =
  "HTTP/1.1 200 OK\r\n"
  "Content-Type: multipart/x-mixed-replace;boundary=" PART_BOUNDARY "\r\n"
  "Access-Control-Allow-Origin: *\r\n"
  "Cache-Control: no-cache, no-store\r\n"
  "Connection: close\r\n"
  "\r\n";
// same frames, but a neutral content type for the web page, which reads the
// stream with fetch(): Safari splits multipart/x-mixed-replace itself and
// then fetch() does not get the stream
static const char* _STREAM_HEADER_RAW =
  "HTTP/1.1 200 OK\r\n"
  "Content-Type: application/octet-stream\r\n"
  "X-Content-Type-Options: nosniff\r\n"
  "Access-Control-Allow-Origin: *\r\n"
  "Cache-Control: no-cache, no-store\r\n"
  "Connection: close\r\n"
  "\r\n";
// answer to a CORS preflight request (some browsers send one before the stream)
static const char* _STREAM_OPTIONS =
  "HTTP/1.1 204 No Content\r\n"
  "Access-Control-Allow-Origin: *\r\n"
  "Access-Control-Allow-Methods: GET, OPTIONS\r\n"
  "Access-Control-Allow-Headers: *\r\n"
  "Access-Control-Max-Age: 600\r\n"
  "Content-Length: 0\r\n"
  "Connection: close\r\n"
  "\r\n";
static const char* _STREAM_PART = "--" PART_BOUNDARY "\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";
static const char* _STREAM_PART_END = "\r\n";

httpd_handle_t camera_httpd = NULL;

// latest JPEG frame, written by captureTask and read by every stream client
static SemaphoreHandle_t frameMutex = NULL;
static uint8_t *frameBuf = NULL;
static size_t frameLen = 0;
static size_t frameCap = 0;
static volatile uint32_t frameId = 0;      // incremented on every new frame, 0 = no frame yet
static std::atomic<int> streamClients{0};


// manual white balance: red / green / blue gain, 64 is about 1x.
// OV2640: the white balance block stays on, DSP register 0xC7 = 0x40 switches it
// from automatic to the gains in 0xCC (red), 0xCD (green), 0xCE (blue).
// set_reg() address = bank << 8 | register, bank 0 = DSP
static int wbR = 64, wbG = 64, wbB = 64;

// the white balance registers as the camera set them up at start (automatic
// white balance), written back when going from manual to automatic again.
// set_wb_mode(0) would write 0xC7 = 0x00, which is not necessarily the start value
static const int WB_REGS[4] = {0x0C7, 0x0CC, 0x0CD, 0x0CE};
static int wbAutoRegs[4] = {-1, -1, -1, -1};

static void printWbRegs(sensor_t *s, const char *when)
{
  if (s->id.PID != OV2640_PID) return;
  Serial.printf("white balance %s: C7=%02x CC=%02x CD=%02x CE=%02x\n", when,
                s->get_reg(s, WB_REGS[0], 0xFF), s->get_reg(s, WB_REGS[1], 0xFF),
                s->get_reg(s, WB_REGS[2], 0xFF), s->get_reg(s, WB_REGS[3], 0xFF));
}

// call once after the camera is set up, while the white balance is still automatic
static void saveAutoWb(sensor_t *s)
{
  if (s->id.PID != OV2640_PID) return;
  for (int i = 0; i < 4; i++) wbAutoRegs[i] = s->get_reg(s, WB_REGS[i], 0xFF);
  printWbRegs(s, "at start");
}

static void setAutoWb(sensor_t *s)
{
  s->set_whitebal(s, 1);
  if (s->id.PID != OV2640_PID || wbAutoRegs[0] < 0) {
    s->set_wb_mode(s, 0);
    return;
  }
  for (int i = 0; i < 4; i++) {
    if (wbAutoRegs[i] >= 0) s->set_reg(s, WB_REGS[i], 0xFF, wbAutoRegs[i]);
  }
  s->status.wb_mode = 0;
  printWbRegs(s, "back to auto");
}

static void setManualWb(sensor_t *s, int r, int g, int b)
{
  wbR = constrain(r, 0, 255);
  wbG = constrain(g, 0, 255);
  wbB = constrain(b, 0, 255);
  if (s->id.PID != OV2640_PID) {
    Serial.println("manual white balance gains are only implemented for the OV2640");
    return;
  }
  s->set_whitebal(s, 1);
  s->set_reg(s, 0x0C7, 0xFF, 0x40);
  s->set_reg(s, 0x0CC, 0xFF, wbR);
  s->set_reg(s, 0x0CD, 0xFF, wbG);
  s->set_reg(s, 0x0CE, 0xFF, wbB);
  printWbRegs(s, "manual");
}

// manual exposure in sensor lines (about 60 us each). The sensor limits it to
// about one frame: measured 332 lines up to 400x296, 686 up to 800x600, and
// about 1250 (estimated) above. set_aec_value() of the camera library stops at
// 1200, so on the OV2640 the exposure registers are written directly:
// sensor bank REG45[5:0] = bits 15..10, AEC = bits 9..2, REG04[1:0] = bits 1..0
#define MAX_EXPOSURE_LINES 1250

static void setExposureLines(sensor_t *s, int lines)
{
  lines = constrain(lines, 0, MAX_EXPOSURE_LINES);
  if (s->id.PID != OV2640_PID) {
    s->set_aec_value(s, lines);
    return;
  }
  s->set_reg(s, 0x145, 0x3F, (lines >> 10) & 0x3F);
  s->set_reg(s, 0x110, 0xFF, (lines >> 2) & 0xFF);
  s->set_reg(s, 0x104, 0x03, lines & 0x03);
  s->status.aec_value = lines;
}

void setAllAuto(int enable)
{
    allAuto = enable;

    sensor_t *s = esp_camera_sensor_get();
    if (!s) return;

    bool en = (enable != 0);   // convert to true/false

    // AUTO EXPOSURE
    s->set_exposure_ctrl(s, en);

    // AUTO GAIN CONTROL
    s->set_gain_ctrl(s, en);

    // WHITE BALANCE: automatic, or the manual red / green / blue gains
    if (en) {
      setAutoWb(s);
    } else {
      setManualWb(s, wbR, wbG, wbB);
    }

    // AEC2 ("AEC DSP") stays off: on the OV3660 it is the night mode, which
    // lowers the frame rate in low light (see setCamDefault)

    // OPTIONAL: reset brightness/contrast when auto enabled
    if (en) {
        s->set_brightness(s, 0);
        s->set_contrast(s, 0);
    }

    // When going manual: freeze current values
    if (!en) {
        setExposureLines(s, s->status.aec_value);
        s->set_agc_gain(s, s->status.agc_gain);
    }

    Serial.printf("allAuto = %d\n", en);
}


// Handler for "/ping"
static esp_err_t ping_handler(httpd_req_t *req)
{
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  const char *resp = "{\"status\":\"ok\",\"ping\":1}";
  return httpd_resp_send(req, resp, strlen(resp));
}

// Handler for "/register?ip=192.168.4.2[&type=led]" (access point only): a station
// camera or an LED board announces itself and gets its number back,
// {"number":2} (0 = no free slot)
static esp_err_t register_handler(httpd_req_t *req)
{
  char query[64];
  char ipStr[16];
  char type[8] = "";
  int number = 0;
  IPAddress ip;
  if (access_point == 1 &&
      httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
      httpd_query_key_value(query, "ip", ipStr, sizeof(ipStr)) == ESP_OK &&
      ip.fromString(ipStr)) {
    httpd_query_key_value(query, "type", type, sizeof(type));
    number = registerDevice(strcmp(type, "led") == 0 ? leds : cams, ipStr);
  }

  char resp[32];
  snprintf(resp, sizeof(resp), "{\"number\":%d}", number);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_sendstr(req, resp);
}

// Handler for "/devices" (access point): all cameras and LED boards of the network,
// {"cams":[{"n":1,"ip":"192.168.4.1"},...],"leds":[{"n":1,"ip":"192.168.4.3"},...]}.
// the web page reads it from 192.168.4.1, which is a camera or an LED board
static esp_err_t devices_handler(httpd_req_t *req)
{
  char camList[200];
  char ledList[100];
  devicesJson(cams, camList, sizeof(camList), access_point == 1 ? ipAP.c_str() : NULL);
  devicesJson(leds, ledList, sizeof(ledList));

  char json[320];
  snprintf(json, sizeof(json), "{\"cams\":%s,\"leds\":%s}", camList, ledList);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_sendstr(req, json);
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
                setExposureLines(s, value);
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
          if (value < 0) value = 0;
          if (value > MAX_FRAMESIZE) value = MAX_FRAMESIZE;
          if (value != s->status.framesize) {
              s->set_framesize(s, (framesize_t)value);
          }
        }
        else if (strcmp(param, "allAuto") == 0) {
          setAllAuto(value);
        }
        else if (strcmp(param, "agc") == 0) {          // auto gain on / off
          value = (value != 0) ? 1 : 0;
          if (!value) s->set_agc_gain(s, s->status.agc_gain);   // keep the current gain
          s->set_gain_ctrl(s, value);
        }
        else if (strcmp(param, "agc_gain") == 0) {     // manual gain 0..30, switches auto gain off
          if (value < 0) value = 0;
          if (value > 30) value = 30;
          s->set_gain_ctrl(s, 0);
          s->set_agc_gain(s, value);
        }
        else if (strcmp(param, "awb") == 0) {          // auto white balance on / off (off = manual gains)
          if (value) {
            setAutoWb(s);
          } else {
            setManualWb(s, wbR, wbG, wbB);
          }
        }
        else if (strcmp(param, "wb_mode") == 0) {      // 0 auto, 1 sunny, 2 cloudy, 3 office, 4 home
          if (value < 0) value = 0;
          if (value > 4) value = 4;
          if (value == 0) {
            setAutoWb(s);
          } else {
            s->set_whitebal(s, 1);                     // the presets need the white balance block on
            s->set_wb_mode(s, value);
          }
        }
        else if (strcmp(param, "wb_rgb") == 0) {       // manual white balance "red,green,blue" 0..255
          int r, g, b;
          if (sscanf(value_str, "%d,%d,%d", &r, &g, &b) == 3) setManualWb(s, r, g, b);
        }
        else if (strcmp(param, "fps") == 0) {
          if (value < 1) value = 1;
          if (value > MAX_FPS_LIMIT) value = MAX_FPS_LIMIT;
          maxFps = value;
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

    char json[400];
    snprintf(json, sizeof(json),
        "{\"quality\":%d,\"exposure\":%d,\"auto_exposure\":%d,"
        "\"gain\":%d,\"brightness\":%d,\"contrast\":%d,"
        "\"saturation\":%d,\"framesize\":%d,\"maxFramesize\":%d,"
        "\"allAuto\":%d,\"agc\":%d,\"agc_gain\":%d,\"awb\":%d,\"wb_mode\":%d,"
        "\"wb_r\":%d,\"wb_g\":%d,\"wb_b\":%d,\"number\":%d,\"streamClients\":%d,"
        "\"fps\":%d,\"maxFpsLimit\":%d,\"sensor\":\"%s\",\"ap\":%d,\"rssi\":%d}",
        quality, exposure, auto_exp,
        gain, brightness, contrast, saturation, framesize, MAX_FRAMESIZE,
        allAuto, s->status.agc, s->status.agc_gain, s->status.awb, s->status.wb_mode,
        wbR, wbG, wbB, camNumber, streamClients.load(),
        maxFps, MAX_FPS_LIMIT, sensorName,
        // WiFi signal of a station camera to the access point in dBm (0 = access point / not connected)
        access_point, (access_point == 0 && WiFi.status() == WL_CONNECTED) ? (int)WiFi.RSSI() : 0
    );

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
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

/* -------------------------
    STREAMING
    One capture task grabs frames from the camera and keeps the latest JPEG
    in frameBuf. Every viewer of http://<ip>:81/stream is served by its own
    task, so several viewers (phone, second tab, python) run in parallel and
    a slow or dead viewer never blocks the others.
   ------------------------- */

// draw a white cross into a RGB565 frame (only used when not streaming JPEG)
static void drawCross(camera_fb_t *fb)
{
  unsigned short imageWidth = fb->width;
  unsigned short imageHeight = fb->height;
  if (yPosition >= imageHeight || xPosition >= imageWidth) return;

  // horizontal line
  for(int i = 0; i < imageWidth ; i++){
    fb->buf[(i+yPosition*imageWidth)*2] = 255;
    fb->buf[(i+yPosition*imageWidth)*2+1] = 255;
  }
  // vertical line
  for(int i = 0; i < imageHeight ; i++){
    fb->buf[xPosition*2 +i*imageWidth*2] = 255;
    fb->buf[xPosition*2+i*imageWidth*2+1] = 255;
  }
}

static void captureTask(void *arg)
{
  while (true) {
    // do not capture when nobody is watching
    if (streamClients.load() == 0) {
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }

    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("Camera capture failed");
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    uint8_t *jpg = fb->buf;
    size_t len = fb->len;
    bool converted = false;
    if (fb->format != PIXFORMAT_JPEG) {
      drawCross(fb);
      converted = frame2jpg(fb, 80, &jpg, &len);
      if (!converted) {
        Serial.println("JPEG compression failed");
        esp_camera_fb_return(fb);
        continue;
      }
    }

    xSemaphoreTake(frameMutex, portMAX_DELAY);
    if (len > frameCap) {
      size_t newCap = len + len / 2;
      uint8_t *newBuf = (uint8_t *)heap_caps_realloc(frameBuf, newCap, MALLOC_CAP_SPIRAM);
      if (newBuf) {
        frameBuf = newBuf;
        frameCap = newCap;
      }
    }
    if (len <= frameCap) {
      memcpy(frameBuf, jpg, len);
      frameLen = len;
      frameId++;
      if (frameId == 0) frameId = 1;
    }
    xSemaphoreGive(frameMutex);

    if (converted) free(jpg);
    esp_camera_fb_return(fb);
  }
}

static bool sendAll(int sock, const uint8_t *data, size_t len)
{
  while (len > 0) {
    int n = send(sock, data, len, 0);
    if (n <= 0) return false;
    data += n;
    len -= n;
  }
  return true;
}

static bool sendStr(int sock, const char *str)
{
  return sendAll(sock, (const uint8_t *)str, strlen(str));
}

enum StreamRequest { REQ_INVALID, REQ_STREAM, REQ_STREAM_RAW, REQ_OPTIONS };

// read the HTTP request header and tell what it asks for:
// GET /stream (multipart), GET /stream?raw=1 (for the web page), OPTIONS (CORS preflight)
static StreamRequest readStreamRequest(int sock)
{
  char req[1024];
  size_t used = 0;
  while (used < sizeof(req) - 1) {
    int n = recv(sock, req + used, sizeof(req) - 1 - used, 0);
    if (n <= 0) return REQ_INVALID;
    used += n;
    req[used] = 0;
    if (strstr(req, "\r\n\r\n")) break;
  }

  // only the request line matters
  char *lineEnd = strstr(req, "\r\n");
  if (lineEnd) *lineEnd = 0;
  if (strncmp(req, "OPTIONS ", 8) == 0) return REQ_OPTIONS;
  if (strncmp(req, "GET /stream", 11) != 0) return REQ_INVALID;
  return strstr(req, "raw=1") ? REQ_STREAM_RAW : REQ_STREAM;
}

static void streamClientTask(void *arg)
{
  int sock = (int)(intptr_t)arg;
  uint8_t *buf = NULL;
  size_t cap = 0;
  uint32_t lastId = 0;
  TickType_t lastSend = 0;
  char part[96];

  StreamRequest request = readStreamRequest(sock);
  if (request == REQ_OPTIONS) {
    sendStr(sock, _STREAM_OPTIONS);
  } else if (request == REQ_INVALID) {
    sendStr(sock, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
  } else if (sendStr(sock, request == REQ_STREAM_RAW ? _STREAM_HEADER_RAW : _STREAM_HEADER)) {
    while (true) {
      // wait for a frame newer than the last one sent, but send not more than maxFps
      if (frameId == lastId ||
          xTaskGetTickCount() - lastSend < pdMS_TO_TICKS(1000 / maxFps)) {
        vTaskDelay(pdMS_TO_TICKS(5));
        continue;
      }
      lastSend = xTaskGetTickCount();

      // copy the latest frame, so the network send does not block the capture
      xSemaphoreTake(frameMutex, portMAX_DELAY);
      size_t len = frameLen;
      if (len > cap) {
        uint8_t *newBuf = (uint8_t *)heap_caps_realloc(buf, frameCap, MALLOC_CAP_SPIRAM);
        if (newBuf) {
          buf = newBuf;
          cap = frameCap;
        }
      }
      bool ok = (len <= cap);
      if (ok) memcpy(buf, frameBuf, len);
      lastId = frameId;
      xSemaphoreGive(frameMutex);
      if (!ok) break;

      snprintf(part, sizeof(part), _STREAM_PART, (unsigned)len);
      if (!sendStr(sock, part) ||
          !sendAll(sock, buf, len) ||
          !sendStr(sock, _STREAM_PART_END)) {
        break;   // viewer closed the connection or stopped reading
      }
    }
  }

  close(sock);
  free(buf);
  streamClients--;
  Serial.printf("stream client left, %d active\n", streamClients.load());
  vTaskDelete(NULL);
}

static void streamServerTask(void *arg)
{
  int listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
  int opt = 1;
  setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  struct sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(STREAM_PORT);

  if (listenSock < 0 ||
      bind(listenSock, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
      listen(listenSock, 4) != 0) {
    Serial.println("Stream server start failed");
    if (listenSock >= 0) close(listenSock);
    vTaskDelete(NULL);
    return;
  }

  while (true) {
    struct sockaddr_in source;
    socklen_t sourceLen = sizeof(source);
    int sock = accept(listenSock, (struct sockaddr *)&source, &sourceLen);
    if (sock < 0) {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    // a viewer that does not read for 3 s is dropped
    struct timeval timeout = {3, 0};
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    if (streamClients.load() >= MAX_STREAM_CLIENTS) {
      sendStr(sock, "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
      close(sock);
      continue;
    }

    streamClients++;
    if (xTaskCreate(streamClientTask, "streamClient", 4096, (void *)(intptr_t)sock, 4, NULL) != pdPASS) {
      streamClients--;
      close(sock);
      continue;
    }
    Serial.printf("stream client joined, %d active\n", streamClients.load());
  }
}

void startCameraServer(){
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.max_open_sockets = 5;
  config.lru_purge_enable = true;   // drop the oldest idle connection instead of refusing new ones
  config.max_uri_handlers = 16;     // own pages + the internet check pages of fake_internet.h

  httpd_uri_t index_uri = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = index_handler,
    .user_ctx = NULL
  };

httpd_uri_t ping_uri = {
    .uri       = "/ping",
    .method    = HTTP_GET,
    .handler   = ping_handler,
    .user_ctx  = NULL
};

httpd_uri_t register_uri = {
    .uri       = "/register",
    .method    = HTTP_GET,
    .handler   = register_handler,
    .user_ctx  = NULL
};

httpd_uri_t devices_uri = {
    .uri       = "/devices",
    .method    = HTTP_GET,
    .handler   = devices_handler,
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
    httpd_register_uri_handler(camera_httpd, &register_uri);
    httpd_register_uri_handler(camera_httpd, &devices_uri);
    httpd_register_uri_handler(camera_httpd, &cam_ctrl);
  }

  Serial.printf("Starting stream server on port: '%d'\n", STREAM_PORT);
  frameMutex = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(captureTask, "capture", 4096, NULL, 5, NULL, 1);
  xTaskCreate(streamServerTask, "streamServer", 4096, NULL, 5, NULL);
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

// station: tell the access point camera our ip and get our camera number back,
// repeated so it is renewed after a reconnect or a restart of the access point
void registerAtAP(){
  String ip = WiFi.localIP().toString();

  HTTPClient http;
  http.setConnectTimeout(1000);
  http.setTimeout(1000);
  http.begin("http://" + ipAP + "/register?ip=" + ip);
  int httpCode = http.GET();
  if (httpCode == 200) {
    int number = 0;
    if (sscanf(http.getString().c_str(), "{\"number\":%d", &number) == 1) {
      if (number != camNumber) Serial.printf("camera number: %d\n", number);
      camNumber = number;
    }
  } else {
    Serial.printf("registering at AP failed: %s\n", http.errorToString(httpCode).c_str());
  }
  http.end();
}

// access point: if two cameras became access point at the same time (both
// powered on together), the one with the higher MAC restarts and joins the other
void checkDuplicateAP(){
  int n = WiFi.scanNetworks(false, false, false, 120, WIFI_CHANNEL, ssid.c_str());
  uint8_t own[6];
  WiFi.softAPmacAddress(own);
  for (int i = 0; i < n; i++) {
    if (WiFi.SSID(i) == ssid && memcmp(own, WiFi.BSSID(i), 6) > 0) {
      Serial.printf("second access point %s found, restarting as station\n", WiFi.BSSIDstr(i).c_str());
      delay(100);
      ESP.restart();
    }
  }
  WiFi.scanDelete();
}

void connectToWifi(){

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  // WiFi power saving adds large latency/jitter to the video stream
  WiFi.setSleep(false);
  WiFi.setTxPower(WIFI_POWER_19_5dBm);

  // random wait, so two cameras powered at the same moment do not decide at the same time
  delay(esp_random() % 3000);

  WiFi.begin(ssid, password, WIFI_CHANNEL);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < STA_CONNECT_TIMEOUT_MS) {
    delay(250);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("");
    Serial.println("WiFi connected");
    access_point = 0;
    // the access point camera already uses "esp32", make the name unique with the MAC
    host = "esp32-" + WiFi.macAddress().substring(12, 14) + WiFi.macAddress().substring(15, 17);
    host.toLowerCase();
    WiFi.setAutoReconnect(true);

    registerAtAP();
    Serial.print("Camera Stream Ready! Go to: http://");
    Serial.println(WiFi.localIP());

  } else {
    Serial.println("\n[*] Creating AP");
    // stop the station from searching, it would switch the radio away from the AP channel
    WiFi.setAutoReconnect(false);
    WiFi.disconnect();
    // AP + STA, so the camera can scan for a second access point (see checkDuplicateAP)
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(ssid, password, WIFI_CHANNEL, 0, MAX_WIFI_CLIENTS);
    WiFi.setSleep(false);
    WiFi.setTxPower(WIFI_POWER_19_5dBm);
    Serial.print("[+] AP Created with IP Gateway ");
    Serial.println(WiFi.softAPIP());
    access_point= 1;
    camNumber = 1;
  }

  // Start streaming web server
  startCameraServer();
  // phones keep the traffic on the WiFi only when it seems to have internet
  if (access_point == 1) startFakeInternet(camera_httpd, WiFi.softAPIP());
  // signal of every device as received by the access point ({"ap":0} on a station)
  startApWifi(camera_httpd, &access_point);
  setupmDNS();
};

// called from loop(): keep the connection between the cameras alive
void maintainWifi(){
  static uint32_t lastConnected = millis();
  static uint32_t lastRegister = 0;
  static uint32_t lastScan = millis();
  uint32_t now = millis();

  if (access_point == 1) {
    expireDevices(cams);
    expireDevices(leds);
    uint32_t scanInterval = (now < AP_SCAN_FAST_PERIOD_MS) ? AP_SCAN_INTERVAL_MS : AP_SCAN_INTERVAL_LATE_MS;
    if (now - lastScan > scanInterval) {
      lastScan = now;
      checkDuplicateAP();
    }
  } else {
    if (WiFi.status() == WL_CONNECTED) {
      lastConnected = now;
      if (now - lastRegister > REGISTER_INTERVAL_MS) {
        lastRegister = now;
        registerAtAP();
      }
    } else if (now - lastConnected > STA_LOST_RESTART_MS) {
      // no access point any more: restart, then this camera becomes the access point if none is there
      Serial.println("access point lost, restarting");
      delay(100);
      ESP.restart();
    }
  }
}



void setCamDefault()
{
  // Set initial Camera Parameters
  sensor_t *s = esp_camera_sensor_get();
  s->set_gainceiling(s, GAINCEILING_2X); // lowest gain
  s->set_brightness(s, 0);      // digital brightness offset
  s->set_contrast(s, 0);        // optional: increase darkness of shadows
  s->set_saturation(s,0);
  s->set_quality(s, DEFAULT_QUALITY);
  s->set_framesize(s, DEFAULT_FRAMESIZE);
  // AEC2 ("AEC DSP") off: on the OV3660 it is the night mode, which lowers the
  // frame rate in low light; the normal auto exposure works without it
  s->set_aec2(s, 0);

  // which sensor this camera has (OV2640 or OV3660 on the XIAO ESP32S3 Sense)
  camera_sensor_info_t *info = esp_camera_sensor_get_info(&s->id);
  if (info) sensorName = info->name;
  Serial.printf("camera sensor: %s (PID 0x%04x)\n", sensorName, s->id.PID);

  // remember the automatic white balance setup, to restore it after manual mode
  saveAutoWb(s);
}


void setup() {

  // initialize digital pin LED_BUILTIN as an output.
  pinMode(LED_BUILTIN, OUTPUT);

  Serial.begin(115200);
  // shows whether a lost connection was a crash/brownout (restart) or a WiFi problem
  Serial.printf("reset reason: %d (1 power on, 3 software, 4 panic, 5-7 watchdog, 9 brownout)\n", esp_reset_reason());

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
  config.xclk_freq_hz = XCLK_MHZ * 1000000;
  // init with the largest frame size: the frame buffers (in PSRAM) are sized for it,
  // so every size up to MAX_FRAMESIZE can be chosen later; the working size
  // (DEFAULT_FRAMESIZE) is set afterwards in setCamDefault()
  config.frame_size = MAX_FRAMESIZE;


  config.pixel_format = PIXFORMAT_JPEG; // for streaming
  //config.pixel_format = PIXFORMAT_RGB565; // for image modification
  // two buffers: the sensor captures the next frame while the last one is sent
  config.grab_mode = CAMERA_GRAB_LATEST;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.jpeg_quality = DEFAULT_QUALITY;
  config.fb_count = 2;
  
  // Camera init
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x", err);
    return;
  };

  setCamDefault();

  pinMode(LED_BUILTIN, OUTPUT);      // set the LED pin mode
  digitalWrite(LED_BUILTIN, HIGH);  // set LED off

  delay(10);


  // Wi-Fi connection
  connectToWifi();

}

void loop() {

  Serial.println("");
  Serial.print("timing: ");
  Serial.println(timing);
  timing += 1;

  maintainWifi();

 if (access_point == 1) {
    Serial.print("[+] AP Created with IP Gateway ");
    Serial.println(WiFi.softAPIP());
  }
 else {
    Serial.print("[+] camera IP: ");
    Serial.println(WiFi.localIP());
    Serial.print("connected to wifi: ");
    Serial.println(ssid);
    Serial.print("WIFI strength: ");
    Serial.println (WiFi.RSSI());
  }
  Serial.printf("camera number: %d, sensor: %s\n", camNumber, sensorName);

  // the LED flashes the camera number (LOW = LED on),
  // one long flash = no number yet (not registered at the access point)
  int flashes = camNumber;
  if (flashes == 0) {
    digitalWrite(LED_BUILTIN, LOW);
    delay(1000);
    digitalWrite(LED_BUILTIN, HIGH);
  }
  for (int i = 0; i < flashes; i++) {
    if (i > 0) delay(200);
    digitalWrite(LED_BUILTIN, LOW);
    delay(200);
    digitalWrite(LED_BUILTIN, HIGH);
  }
  delay(delayValue);                // wait
}

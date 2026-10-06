#pragma once
/*********
  WiFi signal as received by the access point.

  "/wifi" on the access point lists the signal strength (RSSI in dBm) with which
  the access point receives every connected device (cameras, LED board, phones,
  computers), and the one of the device that asks ("you"), so the web page can
  show the link of the phone / computer it runs on.
  {"ap":1,"you":-55,"stations":[{"ip":"192.168.4.2","rssi":-61},...]}

  Used by webCamCom (cameras) and ledCom (LED board), only on the access point.
 *********/

#include <Arduino.h>
#include "esp_http_server.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_netif_sta_list.h"
#include "lwip/sockets.h"

// ipv4 address of the device that sent the request, 0 = unknown.
// the web server socket may be ipv6 with ipv4 mapped addresses (::ffff:a.b.c.d)
static uint32_t apWifiPeerIp(httpd_req_t *req)
{
  struct sockaddr_storage addr;
  socklen_t len = sizeof(addr);
  if (getpeername(httpd_req_to_sockfd(req), (struct sockaddr *)&addr, &len) != 0) return 0;
  if (addr.ss_family == AF_INET) {
    return ((struct sockaddr_in *)&addr)->sin_addr.s_addr;
  }
#if LWIP_IPV6
  if (addr.ss_family == AF_INET6) {
    const uint8_t *b = ((struct sockaddr_in6 *)&addr)->sin6_addr.s6_addr;
    uint32_t ip;
    memcpy(&ip, b + 12, 4);
    return ip;
  }
#endif
  return 0;
}

static esp_err_t apWifiHandler(httpd_req_t *req)
{
  const int *accessPoint = (const int *)req->user_ctx;
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  if (!*accessPoint) return httpd_resp_sendstr(req, "{\"ap\":0}");

  wifi_sta_list_t wifiList;
  esp_netif_sta_list_t ipList;
  if (esp_wifi_ap_get_sta_list(&wifiList) != ESP_OK ||
      esp_netif_get_sta_list(&wifiList, &ipList) != ESP_OK) {
    return httpd_resp_sendstr(req, "{\"ap\":1,\"you\":0,\"stations\":[]}");
  }

  uint32_t peer = apWifiPeerIp(req);
  int you = 0;
  char json[512];
  size_t used = snprintf(json, sizeof(json), "{\"ap\":1,\"stations\":[");
  bool first = true;
  for (int i = 0; i < ipList.num && i < wifiList.num && used < sizeof(json); i++) {
    uint32_t ip = ipList.sta[i].ip.addr;
    int rssi = wifiList.sta[i].rssi;
    if (ip == 0) continue;                 // no ip address yet
    if (ip == peer) you = rssi;
    const uint8_t *b = (const uint8_t *)&ip;
    used += snprintf(json + used, sizeof(json) - used, "%s{\"ip\":\"%u.%u.%u.%u\",\"rssi\":%d}",
                     first ? "" : ",", b[0], b[1], b[2], b[3], rssi);
    first = false;
  }
  if (used < sizeof(json)) snprintf(json + used, sizeof(json) - used, "],\"you\":%d}", you);
  return httpd_resp_sendstr(req, json);
}

// accessPoint: the firmware's access point flag (1 = this device created the network)
static void startApWifi(httpd_handle_t server, const int *accessPoint)
{
  httpd_uri_t uri = {
    .uri = "/wifi",
    .method = HTTP_GET,
    .handler = apWifiHandler,
    .user_ctx = (void *)accessPoint
  };
  httpd_register_uri_handler(server, &uri);
}

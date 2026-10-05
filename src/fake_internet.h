#pragma once
/*********
  Pretend that the "esp32" network has internet.

  Phones and computers check after joining a WiFi network whether it reaches
  the internet. Without internet, phones keep the WiFi but send the traffic
  over mobile data, then the page on 192.168.4.1 does not load (until mobile
  data is switched off) and streams can stop.

  The access point therefore answers these checks itself:
  - a small DNS server answers the names of the check servers with its own ip
    (and the special address Windows expects), every other name with
    "does not exist", so apps fail fast and do not load the ESP
  - the web server answers the check requests like the real servers

  Used by webCamCom (cameras) and ledCom (LED board), only on the access point.
  Android 10+ also checks over https, which cannot be faked, it may still show
  "limited connectivity", but keeps the WiFi for the page.
 *********/

#include <Arduino.h>
#include "esp_http_server.h"
#include "lwip/sockets.h"

// one check request answered by the web server
struct FakeInternetPage {
  const char *uri;
  const char *status;   // "204 No Content" or "200 OK"
  const char *type;
  const char *body;
};

static const FakeInternetPage fakeInternetPages[] = {
  // Android (connectivitycheck.gstatic.com, clients3.google.com, play.googleapis.com, ...)
  {"/generate_204", "204 No Content", "text/plain", ""},
  {"/gen_204",      "204 No Content", "text/plain", ""},
  // iPhone / Mac (captive.apple.com)
  {"/hotspot-detect.html",       "200 OK", "text/html", "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>"},
  {"/library/test/success.html", "200 OK", "text/html", "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>"},
  // Windows (www.msftconnecttest.com, www.msftncsi.com)
  {"/connecttest.txt", "200 OK", "text/plain", "Microsoft Connect Test"},
  {"/ncsi.txt",        "200 OK", "text/plain", "Microsoft NCSI"},
  // Firefox (detectportal.firefox.com)
  {"/success.txt",     "200 OK", "text/plain", "success\n"},
};

// names answered by the DNS server, ip 0.0.0.0 = the ip of the access point
struct FakeInternetName {
  const char *name;
  uint8_t ip[4];
};

static const FakeInternetName fakeInternetNames[] = {
  {"connectivitycheck.gstatic.com", {0, 0, 0, 0}},
  {"connectivitycheck.android.com", {0, 0, 0, 0}},
  {"clients1.google.com",           {0, 0, 0, 0}},
  {"clients3.google.com",           {0, 0, 0, 0}},
  {"www.google.com",                {0, 0, 0, 0}},
  {"play.googleapis.com",           {0, 0, 0, 0}},
  {"captive.apple.com",             {0, 0, 0, 0}},
  {"www.apple.com",                 {0, 0, 0, 0}},
  {"www.appleiphonecell.com",       {0, 0, 0, 0}},
  {"www.msftconnecttest.com",       {0, 0, 0, 0}},
  {"www.msftncsi.com",              {0, 0, 0, 0}},
  {"dns.msftncsi.com",              {131, 107, 255, 255}},   // Windows expects exactly this address
  {"detectportal.firefox.com",      {0, 0, 0, 0}},
};

static uint8_t fakeInternetOwnIp[4] = {192, 168, 4, 1};

static esp_err_t fakeInternetHandler(httpd_req_t *req)
{
  const FakeInternetPage *page = (const FakeInternetPage *)req->user_ctx;
  httpd_resp_set_status(req, page->status);
  httpd_resp_set_type(req, page->type);
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store");
  return httpd_resp_send(req, page->body, strlen(page->body));
}

// returns the answer for a DNS name, NULL = not one of ours
static const uint8_t *fakeInternetLookup(const char *name)
{
  for (const FakeInternetName &entry : fakeInternetNames) {
    if (strcasecmp(entry.name, name) == 0) {
      bool own = entry.ip[0] == 0 && entry.ip[1] == 0 && entry.ip[2] == 0 && entry.ip[3] == 0;
      return own ? fakeInternetOwnIp : entry.ip;
    }
  }
  return NULL;
}

// minimal DNS server on udp port 53: one question per request, A records only
static void fakeInternetDnsTask(void *arg)
{
  int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  struct sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(53);
  if (sock < 0 || bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    Serial.println("fake internet: DNS server start failed");
    if (sock >= 0) close(sock);
    vTaskDelete(NULL);
    return;
  }

  uint8_t buf[512 + 16];
  while (true) {
    struct sockaddr_in from;
    socklen_t fromLen = sizeof(from);
    int n = recvfrom(sock, buf, 512, 0, (struct sockaddr *)&from, &fromLen);
    // header: id, flags, question count, answer / authority / additional counts
    if (n < 12 || (buf[2] & 0x80) || buf[4] != 0 || buf[5] != 1) continue;

    // question name: labels "\x03www\x06google\x03com\x00" -> "www.google.com"
    char name[128];
    size_t nameLen = 0;
    int pos = 12;
    bool ok = true;
    while (pos < n && buf[pos] != 0) {
      int labelLen = buf[pos++];
      if (labelLen > 63 || pos + labelLen > n || nameLen + labelLen + 2 > sizeof(name)) { ok = false; break; }
      if (nameLen > 0) name[nameLen++] = '.';
      memcpy(name + nameLen, buf + pos, labelLen);
      nameLen += labelLen;
      pos += labelLen;
    }
    if (!ok || pos + 5 > n) continue;
    name[nameLen] = 0;
    pos++;                                       // end of name
    uint16_t qtype = (buf[pos] << 8) | buf[pos + 1];
    pos += 4;                                    // type + class
    int answerPos = pos;                         // additional records (EDNS) are dropped

    const uint8_t *ip = fakeInternetLookup(name);

    // turn the request into the response
    buf[2] = 0x80 | (buf[2] & 0x01);             // response, keep "recursion desired"
    buf[3] = 0x80;                               // recursion available, no error
    memset(buf + 6, 0, 6);                       // no answer / authority / additional yet
    if (!ip) {
      buf[3] |= 0x03;                            // name does not exist
    } else if (qtype == 1) {                     // A record; other types: empty answer
      buf[7] = 1;
      const uint8_t answer[] = {
        0xc0, 0x0c,                              // name: pointer to the question
        0x00, 0x01, 0x00, 0x01,                  // type A, class IN
        0x00, 0x00, 0x00, 0x3c,                  // ttl 60 s
        0x00, 0x04, ip[0], ip[1], ip[2], ip[3]
      };
      memcpy(buf + answerPos, answer, sizeof(answer));
      answerPos += sizeof(answer);
    }
    sendto(sock, buf, answerPos, 0, (struct sockaddr *)&from, fromLen);
  }
}

// call on the access point after the web server (port 80) is started
static void startFakeInternet(httpd_handle_t server, IPAddress ownIp)
{
  for (int i = 0; i < 4; i++) fakeInternetOwnIp[i] = ownIp[i];

  for (const FakeInternetPage &page : fakeInternetPages) {
    httpd_uri_t uri = {
      .uri = page.uri,
      .method = HTTP_GET,
      .handler = fakeInternetHandler,
      .user_ctx = (void *)&page
    };
    httpd_register_uri_handler(server, &uri);
  }

  xTaskCreate(fakeInternetDnsTask, "fakeDns", 4096, NULL, 3, NULL);
  Serial.println("fake internet: DNS server and check pages started");
}

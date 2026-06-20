// ===========================================================================
//  Türkamera — AI-Thinker ESP32-CAM
//
//  WiFi: WIFI_AP_STA
//   - Permanenter SoftAP "DoorCam-Setup" (immer sichtbar), optional mit Passwort
//     geschützt (im Web-Portal unter "Sicherheit" setzbar).
//   - STA verbindet sich mit dem Heim-WLAN; die Kamera ist als doorcam.local
//     erreichbar und liefert den MJPEG-Stream auf Port 81 (/stream).
//
//  Web-Portal (Captive Portal, Arduino-WebServer auf Port 80):
//   - Tab "Vorschau"   : Live-Bild
//   - Tab "WLAN"       : Heim-SSID + Passwort (mit Netzwerk-Scan)
//   - Tab "Sicherheit" : Hotspot-Passwort + Wiederholung (leer = offen, sonst >= 8)
//   - /led?on=0|1      : Front-LED (GPIO4) schalten  (vom Display-Portal genutzt)
//
//  Persistenz (Preferences-Namespace "doorcam"): homessid, homepass, appass
// ===========================================================================

#include "esp_camera.h"
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>

#include "camera_pins.h"

// In app_httpd.cpp definiert.
void startStreamServer();

static const char *MDNS_HOSTNAME = "doorcam";        // -> doorcam.local
static const char *AP_SSID       = "DoorCam-Setup";  // dauerhafter Hotspot
#define LAMP_PIN 4                                    // weiße Front-/Blitz-LED

WebServer   server(80);
DNSServer   dnsServer;
Preferences prefs;
IPAddress   apIP(192, 168, 4, 1);

String   homeSsid, homePass, apPass;
int      lampOn = 0;
uint32_t apRestartAt = 0;

// ---------------------------------------------------------------------------
// Kamera initialisieren (240x240 JPEG, 180°-Orientierung)
// ---------------------------------------------------------------------------
static bool initCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0       = Y2_GPIO_NUM;
  config.pin_d1       = Y3_GPIO_NUM;
  config.pin_d2       = Y4_GPIO_NUM;
  config.pin_d3       = Y5_GPIO_NUM;
  config.pin_d4       = Y6_GPIO_NUM;
  config.pin_d5       = Y7_GPIO_NUM;
  config.pin_d6       = Y8_GPIO_NUM;
  config.pin_d7       = Y9_GPIO_NUM;
  config.pin_xclk     = XCLK_GPIO_NUM;
  config.pin_pclk     = PCLK_GPIO_NUM;
  config.pin_vsync    = VSYNC_GPIO_NUM;
  config.pin_href     = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn     = PWDN_GPIO_NUM;
  config.pin_reset    = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.frame_size   = FRAMESIZE_240X240;
  config.pixel_format = PIXFORMAT_JPEG;
  config.grab_mode    = CAMERA_GRAB_LATEST;
  config.fb_location  = CAMERA_FB_IN_PSRAM;
  config.jpeg_quality = 12;
  config.fb_count     = 1;

  if (psramFound()) {
    config.fb_count  = 2;
    config.grab_mode = CAMERA_GRAB_LATEST;
  } else {
    config.fb_location = CAMERA_FB_IN_DRAM;
    config.fb_count    = 1;
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed: 0x%x\n", err);
    return false;
  }

  sensor_t *s = esp_camera_sensor_get();
  if (s) {
    // 180°-Rotation: beide Achsen spiegeln (vflip + hmirror).
    s->set_vflip(s, 0);
    s->set_hmirror(s, 1);
  }
  return true;
}

// ===========================================================================
//  Web-Portal
// ===========================================================================
void serviceNet() {
  dnsServer.processNextRequest();
  server.handleClient();
  if (apRestartAt && millis() >= apRestartAt) {
    apRestartAt = 0;
    WiFi.softAP(AP_SSID, apPass.length() ? apPass.c_str() : nullptr);
  }
}

String esc(const String &s) {
  String o;
  for (char c : s) {
    switch (c) {
      case '&': o += "&amp;"; break;
      case '<': o += "&lt;"; break;
      case '>': o += "&gt;"; break;
      case '"': o += "&quot;"; break;
      default:  o += c;
    }
  }
  return o;
}

String pageHtml() {
  String staInfo = (WiFi.status() == WL_CONNECTED)
                       ? ("connected (" + WiFi.localIP().toString() + ")")
                       : String("not connected");
  String secState = apPass.length() ? "protected" : "open";

  String h;
  h.reserve(4096);
  h += F("<!DOCTYPE html><html lang='en'><head><meta charset='utf-8'>"
         "<meta name='viewport' content='width=device-width,initial-scale=1'>"
         "<title>DoorCam</title><style>"
         "body{font-family:system-ui,sans-serif;background:#111;color:#eee;margin:0;padding:1rem}"
         "h2{margin:.2rem 0 1rem}"
         ".tabs{display:flex;gap:.3rem;margin-bottom:1rem}"
         ".tabs button{flex:1;padding:.6rem;border:0;border-radius:8px 8px 0 0;background:#222;color:#bbb;font-size:1rem}"
         ".tabs button.active{background:#0a84ff;color:#fff}"
         ".tab{display:none;background:#1c1c1e;padding:1rem;border-radius:0 8px 8px 8px}"
         ".tab.active{display:block}"
         "label{display:block;margin:.6rem 0 .2rem;font-size:.9rem;color:#bbb}"
         "input{width:100%;box-sizing:border-box;padding:.6rem;border-radius:8px;border:1px solid #444;background:#000;color:#fff;font-size:1rem}"
         "button.save{margin-top:1rem;width:100%;padding:.7rem;border:0;border-radius:8px;background:#0a84ff;color:#fff;font-size:1rem}"
         ".hint{font-size:.8rem;color:#888;margin-top:.4rem}"
         ".badge{font-size:.8rem;color:#0a84ff}"
         ".tip{font-size:.8rem;color:#888;margin-bottom:.8rem}"
         "img{width:100%;max-width:240px;display:block;margin:.5rem auto;border-radius:8px}"
         "</style></head><body><h2>DoorCam</h2>"
         "<div class='tip'>If a button does not respond, open this page in Safari "
         "&rarr; <b>http://192.168.4.1</b></div>"
         "<div class='tabs'>"
         "<button class='active' onclick=\"t(event,'vor')\">Preview</button>"
         "<button onclick=\"t(event,'wlan')\">WiFi</button>"
         "<button onclick=\"t(event,'sec')\">Security</button></div>");

  // Tab Preview
  h += F("<div id='vor' class='tab active'>"
         "<img id='cam' alt='Live image'>"
         "<div class='hint'>STA: ");
  h += staInfo;
  h += F("</div></div>");

  // Tab WiFi
  h += F("<div id='wlan' class='tab'><form method='POST' action='/savewifi'>"
         "<label>Home WiFi (SSID)</label>"
         "<input list='nets' name='ssid' value='");
  h += esc(homeSsid);
  h += F("'><datalist id='nets'></datalist>"
         "<label>WiFi password</label>"
         "<input type='password' name='pass' placeholder='(leave empty = unchanged)'>"
         "<button type='button' class='save' style='background:#333' onclick='scan()'>Scan networks</button>"
         "<button class='save' type='submit'>Save WiFi</button>"
         "<div class='hint'>After saving, the camera reconnects.</div></form></div>");

  // Tab Security
  h += F("<div id='sec' class='tab'><form method='POST' action='/savesec'>"
         "<label>Hotspot password</label>"
         "<input type='password' id='p1' name='appass' placeholder='empty = open hotspot'>"
         "<label>Repeat password</label>"
         "<input type='password' id='p2' name='appass2' placeholder='same as above'>"
         "<button class='save' type='submit'>Save password</button>"
         "<div class='hint'>Current status: <span class='badge'>");
  h += secState;
  h += F("</span>. Leave empty = open hotspot. Otherwise at least 8 characters. "
         "After saving, the hotspot restarts — please reconnect with the new "
         "password.</div></form></div>");

  h += F("<script>"
         "function t(e,id){document.querySelectorAll('.tab').forEach(x=>x.classList.remove('active'));"
         "document.querySelectorAll('.tabs button').forEach(x=>x.classList.remove('active'));"
         "document.getElementById(id).classList.add('active');e.target.classList.add('active');}"
         "document.getElementById('cam').src='http://'+location.hostname+':81/stream';"
         "function scan(){fetch('/scan').then(r=>r.json()).then(d=>{var dl=document.getElementById('nets');"
         "dl.innerHTML='';d.forEach(n=>{var o=document.createElement('option');o.value=n;dl.appendChild(o);});"
         "alert(d.length+' networks found — tap the field for the list');});}"
         "</script></body></html>");
  return h;
}

void handleRoot() { server.send(200, "text/html; charset=utf-8", pageHtml()); }

void sendInfo(const String &msg) {
  String h = F("<!DOCTYPE html><html><head><meta charset='utf-8'>"
               "<meta name='viewport' content='width=device-width,initial-scale=1'>"
               "<style>body{font-family:system-ui,sans-serif;background:#111;color:#eee;"
               "padding:2rem;text-align:center}a{color:#0a84ff}</style></head><body><p>");
  h += msg;
  h += F("</p><p><a href='/'>Back</a></p></body></html>");
  server.send(200, "text/html; charset=utf-8", h);
}

void handleScan() {
  int n = WiFi.scanNetworks();
  String j = "[";
  for (int i = 0; i < n; i++) {
    if (i) j += ",";
    String s = WiFi.SSID(i);
    s.replace("\\", "\\\\");
    s.replace("\"", "\\\"");
    j += "\"" + s + "\"";
  }
  j += "]";
  WiFi.scanDelete();
  server.send(200, "application/json", j);
}

void handleSaveWifi() {
  String ssid = server.arg("ssid");
  String pass = server.arg("pass");
  ssid.trim();
  if (ssid.length()) {
    homeSsid = ssid;
    homePass = pass;
    prefs.putString("homessid", homeSsid);
    prefs.putString("homepass", homePass);
    WiFi.begin(homeSsid.c_str(), homePass.c_str());
    sendInfo("WiFi saved. The camera is reconnecting…");
  } else {
    sendInfo("Please enter an SSID.");
  }
}

void handleSaveSec() {
  String p1 = server.arg("appass");
  String p2 = server.arg("appass2");
  if (p1 != p2) { sendInfo("Passwords do not match."); return; }
  if (p1.length() > 0 && p1.length() < 8) {
    sendInfo("The password must be empty (open) or at least 8 characters long.");
    return;
  }
  apPass = p1;
  prefs.putString("appass", apPass);
  if (apPass.length())
    sendInfo("Hotspot password set. The hotspot is restarting — please connect with the new password.");
  else
    sendInfo("Hotspot password removed (open hotspot). The hotspot is restarting.");
  apRestartAt = millis() + 1200;
}

// LED schalten (vom Display-Portal aufgerufen): /led?on=0|1 -> JSON {"on":N}
void handleLed() {
  if (server.hasArg("on")) {
    lampOn = server.arg("on").toInt() ? 1 : 0;
    digitalWrite(LAMP_PIN, lampOn ? HIGH : LOW);
  }
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", String("{\"on\":") + lampOn + "}");
}

// Bildeinstellungen aus Preferences auf den Sensor anwenden (Defaults neutral).
// br/co/sa = -2..2, fx = 0..6, awb = 0/1, q = 10..63 (kleiner = bessere Qualität)
void applyImageSettings() {
  sensor_t *s = esp_camera_sensor_get();
  if (!s) return;
  s->set_brightness(s, prefs.getInt("br", 0));
  s->set_contrast(s, prefs.getInt("co", 0));
  s->set_saturation(s, prefs.getInt("sa", 0));
  s->set_special_effect(s, prefs.getInt("fx", 0));
  int awb = prefs.getInt("awb", 1);
  s->set_whitebal(s, awb);
  s->set_awb_gain(s, awb);
  s->set_quality(s, prefs.getInt("q", 12));
}

// /imgset?br=&co=&sa=&fx=&awb=&q=  -> anwenden + speichern, aktuellen Stand als JSON
void handleImgSet() {
  sensor_t *s = esp_camera_sensor_get();
  if (s) {
    if (server.hasArg("br")) { int v = constrain((int)server.arg("br").toInt(), -2, 2); s->set_brightness(s, v); prefs.putInt("br", v); }
    if (server.hasArg("co")) { int v = constrain((int)server.arg("co").toInt(), -2, 2); s->set_contrast(s, v);   prefs.putInt("co", v); }
    if (server.hasArg("sa")) { int v = constrain((int)server.arg("sa").toInt(), -2, 2); s->set_saturation(s, v); prefs.putInt("sa", v); }
    if (server.hasArg("fx")) { int v = constrain((int)server.arg("fx").toInt(),  0, 6); s->set_special_effect(s, v); prefs.putInt("fx", v); }
    if (server.hasArg("awb")){ int v = server.arg("awb").toInt() ? 1 : 0; s->set_whitebal(s, v); s->set_awb_gain(s, v); prefs.putInt("awb", v); }
    if (server.hasArg("q"))  { int v = constrain((int)server.arg("q").toInt(), 10, 63); s->set_quality(s, v); prefs.putInt("q", v); }
  }
  String j = String("{\"br\":") + prefs.getInt("br", 0) +
             ",\"co\":" + prefs.getInt("co", 0) +
             ",\"sa\":" + prefs.getInt("sa", 0) +
             ",\"fx\":" + prefs.getInt("fx", 0) +
             ",\"awb\":" + prefs.getInt("awb", 1) +
             ",\"q\":" + prefs.getInt("q", 12) + "}";
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", j);
}

void startPortal() {
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
  WiFi.softAP(AP_SSID, apPass.length() ? apPass.c_str() : nullptr);
  dnsServer.start(53, "*", apIP);

  server.on("/", handleRoot);
  server.on("/scan", handleScan);
  server.on("/savewifi", HTTP_POST, handleSaveWifi);
  server.on("/savesec", HTTP_POST, handleSaveSec);
  server.on("/led", handleLed);
  server.on("/imgset", handleImgSet);
  server.onNotFound(handleRoot);
  server.begin();
}

// ===========================================================================
//  Setup / Loop
// ===========================================================================
void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(false);
  Serial.println("\n=== DoorCam starting ===");

  pinMode(LAMP_PIN, OUTPUT);
  digitalWrite(LAMP_PIN, LOW);

  if (!initCamera()) {
    Serial.println("Restarting in 3 s ...");
    delay(3000);
    ESP.restart();
  }

  prefs.begin("doorcam", false);
  homeSsid = prefs.getString("homessid", "");
  homePass = prefs.getString("homepass", "");
  apPass   = prefs.getString("appass", "");
  applyImageSettings();   // gespeicherte Bildeinstellungen auf den Sensor anwenden

  WiFi.mode(WIFI_AP_STA);
  WiFi.persistent(true);
  startPortal();
  Serial.printf("SoftAP active: SSID '%s' (%s), IP %s\n", AP_SSID,
                apPass.length() ? "password-protected" : "open",
                WiFi.softAPIP().toString().c_str());

  // STA: gespeicherte Zugangsdaten, sonst die im NVS hinterlegten (Migration
  // vom früheren WiFiManager-Setup) nutzen.
  if (homeSsid.length()) WiFi.begin(homeSsid.c_str(), homePass.c_str());
  else                   WiFi.begin();

  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
    serviceNet();
    delay(50);
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi connected, IP: ");
    Serial.println(WiFi.localIP());
    if (MDNS.begin(MDNS_HOSTNAME)) {
      MDNS.addService("http", "tcp", 80);
      Serial.printf("mDNS active: http://%s.local/\n", MDNS_HOSTNAME);
    }
  } else {
    Serial.println("STA not (yet) connected — hotspot 'DoorCam-Setup' ready.");
  }

  startStreamServer();

  Serial.printf("Stream:  http://%s:81/stream\n", WiFi.localIP().toString().c_str());
  Serial.printf("Portal:  http://%s.local/  (or http://192.168.4.1/ on the hotspot)\n", MDNS_HOSTNAME);
}

void loop() {
  serviceNet();
  delay(2);
}

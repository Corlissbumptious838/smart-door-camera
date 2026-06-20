// ===========================================================================
//  Büro-Display — ESP32-S3 + rundes GC9A01-TFT (240x240, SPI)
//
//  WiFi: WIFI_AP_STA
//   - Permanenter SoftAP "OfficeDisplay-Setup" (immer sichtbar), optional mit
//     Passwort geschützt (im Web-Portal unter "Sicherheit" setzbar).
//   - STA verbindet sich mit dem Heim-WLAN und holt den MJPEG-Stream der Kamera,
//     dekodiert die JPEG-Bilder (TJpg_Decoder) und zeigt sie 1:1 (240x240) per
//     TFT_eSPI auf dem runden Display.
//
//  Web-Portal (Captive Portal auf der AP-Seite, erreichbar auf Port 80):
//   - Tab "WLAN"       : Heim-SSID + Passwort (mit Netzwerk-Scan)
//   - Tab "Kamera"     : Stream-URL
//   - Tab "Sicherheit" : AP-Passwort + Wiederholung (leer = offen, sonst >= 8)
//
//  Persistenz (Preferences-Namespace "display"):
//   camurl, homessid, homepass, appass
// ===========================================================================

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <HTTPClient.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <TFT_eSPI.h>
#include <TJpg_Decoder.h>

static const char *AP_SSID       = "OfficeDisplay-Setup";
static const char *DEFAULT_URL   = "http://doorcam.local:81/stream";
static const size_t JPG_BUF_SIZE = 32 * 1024;   // reicht für ein 240x240-JPEG

TFT_eSPI    tft = TFT_eSPI();
Preferences prefs;
WebServer   server(80);
DNSServer   dnsServer;
IPAddress   apIP(192, 168, 4, 1);

// Gespeicherte Einstellungen
String camUrl, homeSsid, homePass, apPass;

// Bildeinstellungen (Spiegel der Kamera-Werte, für sofortige Formularanzeige)
int imgBr = 0, imgCo = 0, imgSa = 0, imgFx = 0, imgAwb = 1, imgQ = 12;

// Aus camUrl abgeleitet
String   streamHost;
String   streamPath = "/stream";
uint16_t streamPort = 81;

WiFiClient client;
uint8_t   *jpgBuf = nullptr;

int lightOn = 0;            // zuletzt an die Kamera gesendeter LED-Zustand

// Geplanter SoftAP-Neustart (nach Passwortänderung), damit die HTTP-Antwort
// noch zugestellt wird, bevor die AP-Clients getrennt werden.
uint32_t apRestartAt = 0;

uint32_t lastStatusMs = 0;   // Drosselung der Statusanzeige bei fehlendem WLAN

// ---------------------------------------------------------------------------
// TJpg_Decoder-Callback: dekodierten Bildblock aufs Display schieben
// ---------------------------------------------------------------------------
bool tftOutput(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap) {
  if (y >= tft.height()) return false;
  tft.pushImage(x, y, w, h, bitmap);
  return true;
}

// ---------------------------------------------------------------------------
// Statusmeldung mittig auf dem runden Display
// ---------------------------------------------------------------------------
void showStatus(const char *line1, const char *line2 = nullptr) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(line1, 120, line2 ? 105 : 120, 2);
  if (line2) tft.drawString(line2, 120, 130, 2);
}

// ---------------------------------------------------------------------------
// URL in Host / Port / Pfad zerlegen
// ---------------------------------------------------------------------------
void parseUrl(const String &url) {
  String u = url;
  u.trim();
  if (u.startsWith("http://"))  u = u.substring(7);
  if (u.startsWith("https://")) u = u.substring(8);

  int slash = u.indexOf('/');
  String hostPort = (slash >= 0) ? u.substring(0, slash) : u;
  streamPath       = (slash >= 0) ? u.substring(slash)   : "/stream";

  int colon = hostPort.indexOf(':');
  if (colon >= 0) {
    streamHost = hostPort.substring(0, colon);
    streamPort = (uint16_t)hostPort.substring(colon + 1).toInt();
  } else {
    streamHost = hostPort;
    streamPort = 81;
  }
  if (streamPort == 0) streamPort = 81;
}

// ---------------------------------------------------------------------------
// Host auflösen: .local über mDNS, sonst per DNS
// ---------------------------------------------------------------------------
IPAddress resolveHost() {
  if (streamHost.endsWith(".local")) {
    String name = streamHost.substring(0, streamHost.length() - 6);
    IPAddress ip = MDNS.queryHost(name, 3000);
    if (ip != IPAddress(0, 0, 0, 0)) return ip;
  }
  IPAddress ip;
  if (WiFi.hostByName(streamHost.c_str(), ip)) return ip;
  return IPAddress(0, 0, 0, 0);
}

// ---------------------------------------------------------------------------
// Web-Portal + DNS bedienen (häufig aufrufen, damit das Portal flüssig bleibt)
// ---------------------------------------------------------------------------
void serviceNet() {
  dnsServer.processNextRequest();
  server.handleClient();
  // Anstehender SoftAP-Neustart nach Passwortänderung.
  if (apRestartAt && millis() >= apRestartAt) {
    apRestartAt = 0;
    WiFi.softAP(AP_SSID, apPass.length() ? apPass.c_str() : nullptr);
  }
}

// Servicierte Wartepause (statt blockierendem delay), damit das Portal reagiert.
void waitServiced(uint32_t ms) {
  uint32_t t0 = millis();
  while (millis() - t0 < ms) {
    serviceNet();
    delay(5);
  }
}

// ---------------------------------------------------------------------------
// Eine Zeile vom Stream-Client lesen (CRLF-terminiert), mit Timeout
// ---------------------------------------------------------------------------
String readLine(uint32_t timeoutMs = 3000) {
  String line;
  uint32_t t0 = millis();
  while (millis() - t0 < timeoutMs) {
    if (!client.connected() && !client.available()) break;
    if (client.available()) {
      char c = (char)client.read();
      if (c == '\n') return line;
      if (c != '\r') line += c;
      t0 = millis();
    } else {
      serviceNet();   // Portal während des Wartens weiter bedienen
    }
  }
  return line;
}

// ---------------------------------------------------------------------------
// Verbindung zum MJPEG-Stream aufbauen und HTTP-Antwortheader überspringen
// ---------------------------------------------------------------------------
bool connectStream() {
  showStatus("Searching camera...", streamHost.c_str());
  IPAddress ip = resolveHost();
  if (ip == IPAddress(0, 0, 0, 0)) {
    showStatus("Camera not", "found");
    waitServiced(2000);
    return false;
  }
  if (!client.connect(ip, streamPort)) {
    showStatus("Connection", "failed");
    waitServiced(2000);
    return false;
  }
  client.printf("GET %s HTTP/1.1\r\n", streamPath.c_str());
  client.printf("Host: %s\r\n", streamHost.c_str());
  client.print("User-Agent: ESP32-OfficeDisplay\r\n");
  client.print("Connection: keep-alive\r\n\r\n");

  while (client.connected()) {
    String l = readLine();
    if (l.length() == 0) break;
  }
  return client.connected();
}

// ===========================================================================
//  Web-Portal
// ===========================================================================

// HTML-Escaping für Attributwerte (Anführungszeichen etc.)
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

// <option>-Liste für einen ganzzahligen Bereich lo..hi, "cur" vorausgewählt
String optRange(int lo, int hi, int cur) {
  String o;
  for (int v = lo; v <= hi; v++)
    o += "<option value='" + String(v) + "'" + (v == cur ? " selected" : "") + ">" +
         String(v) + "</option>";
  return o;
}
// einzelne <option> mit Beschriftung
String opt(int val, int cur, const char *label) {
  return "<option value='" + String(val) + "'" + (val == cur ? " selected" : "") + ">" +
         label + "</option>";
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
         "<title>OfficeDisplay</title><style>"
         "body{font-family:system-ui,sans-serif;background:#111;color:#eee;margin:0;padding:1rem}"
         "h2{margin:.2rem 0 1rem}"
         ".tabs{display:flex;gap:.3rem;margin-bottom:1rem}"
         ".tabs button{flex:1;padding:.6rem;border:0;border-radius:8px 8px 0 0;background:#222;color:#bbb;font-size:1rem}"
         ".tabs button.active{background:#0a84ff;color:#fff}"
         ".tab{display:none;background:#1c1c1e;padding:1rem;border-radius:0 8px 8px 8px}"
         ".tab.active{display:block}"
         "label{display:block;margin:.6rem 0 .2rem;font-size:.9rem;color:#bbb}"
         "input,select{width:100%;box-sizing:border-box;padding:.6rem;border-radius:8px;border:1px solid #444;background:#000;color:#fff;font-size:1rem}"
         "button.save{margin-top:1rem;width:100%;padding:.7rem;border:0;border-radius:8px;background:#0a84ff;color:#fff;font-size:1rem}"
         ".hint{font-size:.8rem;color:#888;margin-top:.4rem}"
         ".badge{font-size:.8rem;color:#0a84ff}"
         ".tip{font-size:.8rem;color:#888;margin-bottom:.8rem}"
         "</style></head><body><h2>OfficeDisplay</h2>"
         "<div class='tip'>If a button does not respond, open this page in Safari "
         "&rarr; <b>http://192.168.4.1</b></div>"
         "<div class='tabs'>"
         "<button class='active' onclick=\"t(event,'wlan')\">WiFi</button>"
         "<button onclick=\"t(event,'cam')\">Camera</button>"
         "<button onclick=\"t(event,'licht')\">Light</button>"
         "<button onclick=\"t(event,'bild')\">Image</button>"
         "<button onclick=\"t(event,'sec')\">Security</button></div>");

  // Tab WiFi
  h += F("<div id='wlan' class='tab active'><form method='POST' action='/savewifi'>"
         "<label>Home WiFi (SSID)</label>"
         "<input list='nets' name='ssid' value='");
  h += esc(homeSsid);
  h += F("'><datalist id='nets'></datalist>"
         "<label>WiFi password</label>"
         "<input type='password' name='pass' placeholder='(leave empty = unchanged)'>"
         "<button type='button' class='save' style='background:#333' onclick='scan()'>Scan networks</button>"
         "<button class='save' type='submit'>Save WiFi</button>"
         "<div class='hint'>STA status: ");
  h += staInfo;
  h += F("</div></form></div>");

  // Tab Camera
  h += F("<div id='cam' class='tab'><form method='POST' action='/savecam'>"
         "<label>Camera stream URL</label><input name='camurl' value='");
  h += esc(camUrl);
  h += F("'><button class='save' type='submit'>Save camera URL</button>"
         "<div class='hint'>e.g. http://doorcam.local:81/stream</div></form></div>");

  // Tab Light (links instead of JS -> captive-portal safe)
  h += F("<div id='licht' class='tab'>"
         "<p style='margin:.2rem 0'>Controls the camera's white front LED.</p>"
         "<div class='hint'>Current status: <span class='badge'>");
  h += (lightOn ? "ON" : "OFF");
  h += F("</span></div>"
         "<a class='save' style='display:block;text-align:center;text-decoration:none' "
         "href='/light?on=1'>Turn light on</a>"
         "<a class='save' style='display:block;text-align:center;text-decoration:none;background:#333' "
         "href='/light?on=0'>Turn light off</a>"
         "<div class='hint'>Note: the front LED is very bright.</div></div>");

  // Tab Image (camera sensor settings; <select> -> captive-portal safe)
  h += F("<div id='bild' class='tab'><form method='POST' action='/savebild'>"
         "<label>Brightness</label><select name='br'>");
  h += optRange(-2, 2, imgBr);
  h += F("</select><label>Contrast</label><select name='co'>");
  h += optRange(-2, 2, imgCo);
  h += F("</select><label>Saturation</label><select name='sa'>");
  h += optRange(-2, 2, imgSa);
  h += F("</select><label>Special effect</label><select name='fx'>");
  h += opt(0, imgFx, "None") + opt(1, imgFx, "Negative") + opt(2, imgFx, "Grayscale") +
       opt(3, imgFx, "Reddish") + opt(4, imgFx, "Greenish") + opt(5, imgFx, "Bluish") +
       opt(6, imgFx, "Sepia");
  h += F("</select><label>White balance</label><select name='awb'>");
  h += opt(1, imgAwb, "Automatic") + opt(0, imgAwb, "Off");
  h += F("</select><label>JPEG quality</label><select name='q'>");
  h += opt(10, imgQ, "High (10)") + opt(12, imgQ, "Standard (12)") + opt(18, imgQ, "Medium (18)") +
       opt(30, imgQ, "Low (30)") + opt(45, imgQ, "Very low (45)");
  h += F("</select>"
         "<button class='save' type='submit'>Save image</button>"
         "<div class='hint'>Applies system-wide on the camera and is stored there.</div>"
         "</form></div>");

  // Tab Security (no JS submit gate: captive-portal browsers often suppress
  // alert() and then block submission — validation runs server-side)
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

  // JS: Tab-Wechsel, Passwort-Check, Netzwerk-Scan
  h += F("<script>"
         "function t(e,id){document.querySelectorAll('.tab').forEach(x=>x.classList.remove('active'));"
         "document.querySelectorAll('.tabs button').forEach(x=>x.classList.remove('active'));"
         "document.getElementById(id).classList.add('active');e.target.classList.add('active');}"
         "function scan(){fetch('/scan').then(r=>r.json()).then(d=>{var dl=document.getElementById('nets');"
         "dl.innerHTML='';d.forEach(n=>{var o=document.createElement('option');o.value=n;dl.appendChild(o);});"
         "alert(d.length+' networks found — tap the field for the list');});}"
         "</script></body></html>");
  return h;
}

void handleRoot()    { server.send(200, "text/html; charset=utf-8", pageHtml()); }

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
    sendInfo("WiFi saved. The display is reconnecting…");
  } else {
    sendInfo("Please enter an SSID.");
  }
}

void handleSaveCam() {
  String u = server.arg("camurl");
  u.trim();
  if (u.length()) {
    camUrl = u;
    prefs.putString("camurl", camUrl);
    parseUrl(camUrl);
    if (client.connected()) client.stop();   // mit neuer URL frisch verbinden
    sendInfo("Camera URL saved.");
  } else {
    sendInfo("Please enter a URL.");
  }
}

void handleSaveSec() {
  String p1 = server.arg("appass");
  String p2 = server.arg("appass2");
  if (p1 != p2) {
    sendInfo("Passwords do not match.");
    return;
  }
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
  // SoftAP kurz nach dem Senden der Antwort mit neuem Passwort neu starten.
  apRestartAt = millis() + 1200;
}

// Licht der Kamera schalten: /light?on=0|1  -> ruft Kamera-Endpunkt /led auf
void handleLight() {
  int on = server.arg("on").toInt() ? 1 : 0;
  IPAddress ip = resolveHost();   // Kamera-Host aus der Stream-URL auflösen
  if (ip == IPAddress(0, 0, 0, 0)) {
    sendInfo("Camera not reachable.");
    return;
  }
  HTTPClient http;
  String url = "http://" + ip.toString() + "/led?on=" + String(on);
  http.setConnectTimeout(3000);
  http.begin(url);
  int code = http.GET();
  http.end();
  if (code == 200) {
    lightOn = on;
    sendInfo(on ? "Light turned on." : "Light turned off.");
  } else {
    sendInfo("Error switching the LED (HTTP " + String(code) + ").");
  }
}

// Bildeinstellungen speichern (S3-Mirror) + an die Kamera /imgset weiterreichen
void handleSaveBild() {
  imgBr  = constrain((int)server.arg("br").toInt(), -2, 2);
  imgCo  = constrain((int)server.arg("co").toInt(), -2, 2);
  imgSa  = constrain((int)server.arg("sa").toInt(), -2, 2);
  imgFx  = constrain((int)server.arg("fx").toInt(),  0, 6);
  imgAwb = server.arg("awb").toInt() ? 1 : 0;
  imgQ   = constrain((int)server.arg("q").toInt(), 10, 63);
  prefs.putInt("br", imgBr);  prefs.putInt("co", imgCo);  prefs.putInt("sa", imgSa);
  prefs.putInt("fx", imgFx);  prefs.putInt("awb", imgAwb); prefs.putInt("q", imgQ);

  IPAddress ip = resolveHost();
  if (ip == IPAddress(0, 0, 0, 0)) {
    sendInfo("Saved, but the camera is currently not reachable.");
    return;
  }
  HTTPClient http;
  String url = "http://" + ip.toString() + "/imgset?br=" + imgBr + "&co=" + imgCo +
               "&sa=" + imgSa + "&fx=" + imgFx + "&awb=" + imgAwb + "&q=" + imgQ;
  http.setConnectTimeout(3000);
  http.begin(url);
  int code = http.GET();
  http.end();
  if (code == 200) sendInfo("Image settings saved and applied to the camera.");
  else             sendInfo("Saved, but the camera reported an error (HTTP " + String(code) + ").");
}

void startPortal() {
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
  WiFi.softAP(AP_SSID, apPass.length() ? apPass.c_str() : nullptr);
  dnsServer.start(53, "*", apIP);   // Captive Portal: alles auf die AP-IP lenken

  server.on("/", handleRoot);
  server.on("/scan", handleScan);
  server.on("/savewifi", HTTP_POST, handleSaveWifi);
  server.on("/savecam", HTTP_POST, handleSaveCam);
  server.on("/savesec", HTTP_POST, handleSaveSec);
  server.on("/light", handleLight);
  server.on("/savebild", HTTP_POST, handleSaveBild);
  // Captive-Portal-Erkennung diverser Systeme + Catch-all -> Startseite
  server.onNotFound(handleRoot);
  server.begin();
}

// ===========================================================================
//  Setup / Loop
// ===========================================================================
void setup() {
  Serial.begin(115200);
  Serial.println("\n=== OfficeDisplay starting ===");

  // Einstellungen laden.
  prefs.begin("display", false);
  camUrl   = prefs.getString("camurl", DEFAULT_URL);
  homeSsid = prefs.getString("homessid", "");
  homePass = prefs.getString("homepass", "");
  apPass   = prefs.getString("appass", "");
  imgBr = prefs.getInt("br", 0);   imgCo = prefs.getInt("co", 0);   imgSa = prefs.getInt("sa", 0);
  imgFx = prefs.getInt("fx", 0);   imgAwb = prefs.getInt("awb", 1); imgQ = prefs.getInt("q", 12);

  // Display + Decoder.
  tft.init();
  tft.setRotation(0);
  showStatus("Starting...");

  jpgBuf = (uint8_t *)ps_malloc(JPG_BUF_SIZE);
  if (!jpgBuf) jpgBuf = (uint8_t *)malloc(JPG_BUF_SIZE);
  if (!jpgBuf) { showStatus("Out of memory", "Restarting..."); delay(2000); ESP.restart(); }

  TJpgDec.setJpgScale(1);
  TJpgDec.setSwapBytes(true);
  TJpgDec.setCallback(tftOutput);

  parseUrl(camUrl);

  // AP + STA gleichzeitig.
  WiFi.mode(WIFI_AP_STA);
  WiFi.persistent(true);
  startPortal();
  Serial.printf("SoftAP active: SSID '%s' (%s), IP %s\n", AP_SSID,
                apPass.length() ? "password-protected" : "open",
                WiFi.softAPIP().toString().c_str());

  // STA verbinden: gespeicherte Zugangsdaten, sonst die im NVS hinterlegten
  // (Migration vom früheren WiFiManager-Setup) nutzen.
  if (homeSsid.length()) WiFi.begin(homeSsid.c_str(), homePass.c_str());
  else                   WiFi.begin();
  showStatus("WiFi setup", "WiFi: OfficeDisplay-Setup");

  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
    serviceNet();
    delay(50);
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("STA connected, IP: "); Serial.println(WiFi.localIP());
    MDNS.begin("officedisplay");
    showStatus("WiFi connected", WiFi.localIP().toString().c_str());
    waitServiced(800);
  } else {
    Serial.println("STA not (yet) connected — portal ready.");
    showStatus("No WiFi", "AP: OfficeDisplay-Setup");
  }
}

void loop() {
  serviceNet();

  // Ohne STA-Verbindung: nur Portal bedienen, Hinweis anzeigen (gedrosselt).
  if (WiFi.status() != WL_CONNECTED) {
    if (client.connected()) client.stop();
    if (millis() - lastStatusMs > 3000) {
      lastStatusMs = millis();
      showStatus("No WiFi", "AP: OfficeDisplay-Setup");
    }
    delay(20);
    return;
  }

  // (Neu) mit dem Stream verbinden.
  if (!client.connected()) {
    if (!connectStream()) return;
  }

  // --- Multipart-Teil lesen: Header bis zur Leerzeile, Content-Length merken ---
  int contentLength = 0;
  while (client.connected()) {
    String l = readLine();
    if (l.length() == 0) {
      if (contentLength > 0) break;
      else continue;
    }
    String low = l;
    low.toLowerCase();
    if (low.startsWith("content-length:")) {
      contentLength = l.substring(l.indexOf(':') + 1).toInt();
    }
  }

  if (contentLength <= 0 || (size_t)contentLength > JPG_BUF_SIZE) {
    client.stop();
    return;
  }

  // --- JPEG-Bytes exakt einlesen ---
  int idx = 0;
  uint32_t t0 = millis();
  while (idx < contentLength && client.connected() && millis() - t0 < 4000) {
    if (client.available()) {
      int r = client.read(jpgBuf + idx, contentLength - idx);
      if (r > 0) { idx += r; t0 = millis(); }
    } else {
      serviceNet();
    }
  }

  if (idx == contentLength) {
    TJpgDec.drawJpg(0, 0, jpgBuf, (uint32_t)contentLength);
  } else {
    client.stop();
  }
}

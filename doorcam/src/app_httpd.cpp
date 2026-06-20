// MJPEG-Live-Stream der Türkamera auf Port 81 (/stream).
//
// Die Stream-Logik entspricht dem offiziellen CameraWebServer-Beispiel, ist
// hier aber auf das Nötigste reduziert. Die Konfigurationsseite, die LED-
// Steuerung (/led) und das Captive Portal laufen separat über einen
// Arduino-WebServer auf Port 80 (siehe main.cpp).

#include "esp_http_server.h"
#include "esp_camera.h"
#include <Arduino.h>

#define PART_BOUNDARY "123456789000000000000987654321"
static const char *STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char *STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

static httpd_handle_t stream_httpd = NULL;

// ---------------------------------------------------------------------------
// MJPEG-Stream-Handler (Port 81, /stream)
// ---------------------------------------------------------------------------
static esp_err_t stream_handler(httpd_req_t *req) {
  camera_fb_t *fb = NULL;
  esp_err_t res = ESP_OK;
  size_t jpg_len = 0;
  uint8_t *jpg_buf = NULL;
  char part_buf[64];

  res = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
  if (res != ESP_OK) return res;
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "X-Framerate", "30");

  while (true) {
    fb = esp_camera_fb_get();
    if (!fb) {
      log_e("Frame buffer konnte nicht geholt werden");
      res = ESP_FAIL;
    } else if (fb->format != PIXFORMAT_JPEG) {
      bool ok = frame2jpg(fb, 80, &jpg_buf, &jpg_len);
      esp_camera_fb_return(fb);
      fb = NULL;
      if (!ok) {
        log_e("JPEG-Konvertierung fehlgeschlagen");
        res = ESP_FAIL;
      }
    } else {
      jpg_len = fb->len;
      jpg_buf = fb->buf;
    }

    if (res == ESP_OK)
      res = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
    if (res == ESP_OK) {
      size_t hlen = snprintf(part_buf, sizeof(part_buf), STREAM_PART, jpg_len);
      res = httpd_resp_send_chunk(req, part_buf, hlen);
    }
    if (res == ESP_OK)
      res = httpd_resp_send_chunk(req, (const char *)jpg_buf, jpg_len);

    if (fb) {
      esp_camera_fb_return(fb);
      fb = NULL;
      jpg_buf = NULL;
    } else if (jpg_buf) {
      free(jpg_buf);
      jpg_buf = NULL;
    }

    if (res != ESP_OK) break;  // Client hat die Verbindung getrennt
  }
  return res;
}

// ---------------------------------------------------------------------------
// Stream-Server auf Port 81 starten
// ---------------------------------------------------------------------------
void startStreamServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 81;
  config.ctrl_port = 32081;
  httpd_uri_t stream_uri = {
      .uri = "/stream", .method = HTTP_GET, .handler = stream_handler, .user_ctx = NULL};
  if (httpd_start(&stream_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(stream_httpd, &stream_uri);
  }
}

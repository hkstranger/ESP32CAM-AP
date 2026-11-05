// ESP32-CAM 分階段測試：搵出邊一步崩潰
// STAGE 1 = 只測鏡頭 | 2 = 鏡頭 + WiFi AP | 3 = 鏡頭 + WiFi AP + 簡單網頁
#define STAGE 1

#include <WiFi.h>
#include "esp_camera.h"
#include "esp_http_server.h"

const char* AP_SSID = "ESP32CAM-Face";
const char* AP_PASS = "12345678";

// ---- AI Thinker 腳位 ----
#define PWDN_GPIO_NUM   32
#define RESET_GPIO_NUM  -1
#define XCLK_GPIO_NUM    0
#define SIOD_GPIO_NUM   26
#define SIOC_GPIO_NUM   27
#define Y9_GPIO_NUM     35
#define Y8_GPIO_NUM     34
#define Y7_GPIO_NUM     39
#define Y6_GPIO_NUM     36
#define Y5_GPIO_NUM     21
#define Y4_GPIO_NUM     19
#define Y3_GPIO_NUM     18
#define Y2_GPIO_NUM      5
#define VSYNC_GPIO_NUM  25
#define HREF_GPIO_NUM   23
#define PCLK_GPIO_NUM   22

bool cameraOk = false;

esp_err_t initCamera() {
  camera_config_t c = {};
  c.ledc_channel = LEDC_CHANNEL_0;
  c.ledc_timer   = LEDC_TIMER_0;
  c.pin_d0 = Y2_GPIO_NUM; c.pin_d1 = Y3_GPIO_NUM;
  c.pin_d2 = Y4_GPIO_NUM; c.pin_d3 = Y5_GPIO_NUM;
  c.pin_d4 = Y6_GPIO_NUM; c.pin_d5 = Y7_GPIO_NUM;
  c.pin_d6 = Y8_GPIO_NUM; c.pin_d7 = Y9_GPIO_NUM;
  c.pin_xclk = XCLK_GPIO_NUM;
  c.pin_pclk = PCLK_GPIO_NUM;
  c.pin_vsync = VSYNC_GPIO_NUM;
  c.pin_href = HREF_GPIO_NUM;
  c.pin_sccb_sda = SIOD_GPIO_NUM;
  c.pin_sccb_scl = SIOC_GPIO_NUM;
  c.pin_pwdn = PWDN_GPIO_NUM;
  c.pin_reset = RESET_GPIO_NUM;
  c.xclk_freq_hz = 20000000;
  c.pixel_format = PIXFORMAT_JPEG;
  c.frame_size   = FRAMESIZE_QVGA;      // 先用細畫面，最穩
  c.jpeg_quality = 12;
  c.fb_count     = 1;
  c.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;
  if (psramFound()) {
    c.fb_location = CAMERA_FB_IN_PSRAM;
  } else {
    c.fb_location = CAMERA_FB_IN_DRAM;
  }
  return esp_camera_init(&c);
}

static esp_err_t hello_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/plain");
  return httpd_resp_send(req, "ESP32-CAM OK", HTTPD_RESP_USE_STRLEN);
}

void setup() {
  Serial.begin(115200);
  delay(2000);                           // 等 Serial Monitor 接上
  Serial.println();
  Serial.println("[0] 啟動，STAGE = " + String(STAGE));
  Serial.printf("[1] PSRAM: %s，大小 %u，可用 heap %u\n",
                psramFound() ? "有" : "冇",
                (unsigned)ESP.getPsramSize(),
                (unsigned)ESP.getFreeHeap());

  Serial.println("[2] 初始化鏡頭…");
  esp_err_t err = initCamera();
  Serial.printf("[3] esp_camera_init 結果：0x%x (%s)\n", (unsigned)err, esp_err_to_name(err));
  cameraOk = (err == ESP_OK);
  if (!cameraOk) {
    Serial.println("    鏡頭失敗：檢查排線、電源、Board 設定。");
  }

#if STAGE >= 2
  Serial.println("[4] 啟動 WiFi AP…");
  WiFi.mode(WIFI_AP);
  bool ok = WiFi.softAP(AP_SSID, AP_PASS, 1, 0, 2);
  Serial.printf("[5] softAP：%s，IP = ", ok ? "成功" : "失敗");
  Serial.println(WiFi.softAPIP());
#endif

#if STAGE >= 3
  Serial.println("[6] 啟動網頁伺服器…");
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  httpd_handle_t srv = NULL;
  esp_err_t e = httpd_start(&srv, &cfg);
  Serial.printf("[7] httpd_start：0x%x (%s)\n", (unsigned)e, esp_err_to_name(e));
  if (e == ESP_OK) {
    httpd_uri_t u = {};
    u.uri = "/";
    u.method = HTTP_GET;
    u.handler = hello_handler;
    u.user_ctx = NULL;
    httpd_register_uri_handler(srv, &u);
    Serial.println("[8] 完成，手機開 http://192.168.4.1/");
  }
#endif

  Serial.println("[完] setup 完成，冇崩潰");
}

void loop() {
  if (cameraOk) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb) {
      Serial.printf("影相成功：%ux%u，%u bytes\n",
                    (unsigned)fb->width, (unsigned)fb->height, (unsigned)fb->len);
      esp_camera_fb_return(fb);
    } else {
      Serial.println("影相失敗（fb_get 回傳空）");
    }
  }
  delay(2000);
}

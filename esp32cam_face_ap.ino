// ESP32-CAM 人臉偵測 + 綠框 + 網頁即時影像（AP 模式：ESP32 自己做 WiFi）
// 板：AI Thinker ESP32-CAM | ESP32 core 3.0.7 | PSRAM Enabled | Huge APP (3MB No OTA)
// 手機連 WiFi「ESP32CAM-Face」，瀏覽器開 http://192.168.4.1/

#include <WiFi.h>
#include "esp_camera.h"
#include "esp_http_server.h"
#include "img_converters.h"
#include "human_face_detect_msr01.hpp"
#include "human_face_detect_mnp01.hpp"

// ---- WiFi 設定 ----
const char* AP_SSID = "ESP32CAM-Face";
const char* AP_PASS = "12345678";      // 至少 8 個字元

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

#define LED_PIN          33     // 板上紅燈（LOW = 亮）
#define OUTPUT_PIN       13     // 可接繼電器／蜂鳴器（HIGH = 有人）
#define HOLD_MS        3000     // 偵測到後保持 3 秒
#define JPEG_QUALITY     20     // 數字越細畫質越好（0-63）
#define BOX_THICK         3     // 框線粗幼

HumanFaceDetectMSR01 s1(0.1F, 0.5F, 10, 0.2F);
HumanFaceDetectMNP01 s2(0.5F, 0.3F, 5);

httpd_handle_t pageServer = NULL;
httpd_handle_t streamServer = NULL;

SemaphoreHandle_t mtx;
uint8_t *jpgBuf = NULL;
size_t jpgLen = 0;
uint32_t frameId = 0;

volatile int faceCount = 0;
volatile bool personPresent = false;
unsigned long lastSeen = 0;

// ================= 鏡頭 =================
bool initCamera() {
  camera_config_t c;
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
  c.pixel_format = PIXFORMAT_RGB565;    // 人臉偵測要 RGB565
  c.frame_size   = FRAMESIZE_QVGA;      // 320x240
  c.fb_count     = 1;
  c.fb_location  = CAMERA_FB_IN_PSRAM;
  c.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;
  return esp_camera_init(&c) == ESP_OK;
}

// ================= 畫綠框（RGB565）=================
// 如果框顏色唔係綠色，將 0x07 同 0xE0 對調
static inline void putGreen(uint8_t *buf, int w, int h, int x, int y) {
  if (x < 0 || y < 0 || x >= w || y >= h) return;
  int i = (y * w + x) * 2;
  buf[i] = 0x07;
  buf[i + 1] = 0xE0;
}

void drawBox(uint8_t *buf, int w, int h, int x1, int y1, int x2, int y2) {
  for (int t = 0; t < BOX_THICK; t++) {
    for (int x = x1; x <= x2; x++) {
      putGreen(buf, w, h, x, y1 + t);
      putGreen(buf, w, h, x, y2 - t);
    }
    for (int y = y1; y <= y2; y++) {
      putGreen(buf, w, h, x1 + t, y);
      putGreen(buf, w, h, x2 - t, y);
    }
  }
}

// ================= 網頁 =================
static const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32-CAM 人臉偵測</title>
<style>
body{margin:0;background:#111;color:#eee;font-family:sans-serif;text-align:center}
img{width:100%;max-width:640px;display:block;margin:0 auto;background:#000}
#st{font-size:22px;padding:12px}
.on{color:#4caf50}.off{color:#999}
</style></head><body>
<img id="v">
<div id="st" class="off">連線中…</div>
<script>
document.getElementById('v').src='http://'+location.hostname+':81/stream';
async function poll(){
  try{
    const r=await fetch('/status');const j=await r.json();
    const s=document.getElementById('st');
    if(j.person){s.textContent='有人（人臉數：'+j.faces+'）';s.className='on';}
    else{s.textContent='冇人';s.className='off';}
  }catch(e){}
  setTimeout(poll,500);
}
poll();
</script></body></html>
)rawliteral";

static esp_err_t index_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t status_handler(httpd_req_t *req) {
  char out[64];
  snprintf(out, sizeof(out), "{\"person\":%s,\"faces\":%d}",
           personPresent ? "true" : "false", (int)faceCount);
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t stream_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "multipart/x-mixed-replace;boundary=frame");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  uint32_t lastId = 0;

  while (true) {
    uint8_t *copy = NULL;
    size_t len = 0;

    xSemaphoreTake(mtx, portMAX_DELAY);
    if (jpgLen && frameId != lastId) {
      copy = (uint8_t *)malloc(jpgLen);
      if (copy) {
        memcpy(copy, jpgBuf, jpgLen);
        len = jpgLen;
        lastId = frameId;
      }
    }
    xSemaphoreGive(mtx);

    if (!copy) { vTaskDelay(10 / portTICK_PERIOD_MS); continue; }

    char hdr[96];
    int n = snprintf(hdr, sizeof(hdr),
      "\r\n--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
      (unsigned)len);
    esp_err_t r = httpd_resp_send_chunk(req, hdr, n);
    if (r == ESP_OK) r = httpd_resp_send_chunk(req, (const char *)copy, len);
    free(copy);
    if (r != ESP_OK) break;   // 瀏覽器關咗就離開
  }
  return ESP_OK;
}

void startServers() {
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.server_port = 80;
  httpd_uri_t idx  = { "/",       HTTP_GET, index_handler,  NULL };
  httpd_uri_t stat = { "/status", HTTP_GET, status_handler, NULL };
  if (httpd_start(&pageServer, &cfg) == ESP_OK) {
    httpd_register_uri_handler(pageServer, &idx);
    httpd_register_uri_handler(pageServer, &stat);
  }

  cfg.server_port = 81;
  cfg.ctrl_port += 1;
  httpd_uri_t str = { "/stream", HTTP_GET, stream_handler, NULL };
  if (httpd_start(&streamServer, &cfg) == ESP_OK) {
    httpd_register_uri_handler(streamServer, &str);
  }
}

// ================= setup / loop =================
void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);    digitalWrite(LED_PIN, HIGH);
  pinMode(OUTPUT_PIN, OUTPUT); digitalWrite(OUTPUT_PIN, LOW);
  mtx = xSemaphoreCreateMutex();

  if (!psramFound()) {
    Serial.println("搵唔到 PSRAM，請喺 Tools 開 PSRAM");
    while (true) delay(1000);
  }
  if (!initCamera()) {
    Serial.println("鏡頭初始化失敗，檢查排線同電源");
    while (true) delay(1000);
  }

  // ---- AP 模式：ESP32 自己做 WiFi ----
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS, 1, 0, 2);   // 頻道1、唔隱藏、最多2部裝置
  Serial.print("手機連 WiFi「");
  Serial.print(AP_SSID);
  Serial.print("」，然後開： http://");
  Serial.println(WiFi.softAPIP());          // 通常係 192.168.4.1

  startServers();
}

void loop() {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) { delay(50); return; }

  int w = fb->width, h = fb->height;

  // 人臉偵測（兩階段）
  std::list<dl::detect::result_t> &cand =
      s1.infer((uint16_t *)fb->buf, {h, w, 3});
  std::list<dl::detect::result_t> &res =
      s2.infer((uint16_t *)fb->buf, {h, w, 3}, cand);

  int count = 0;
  for (auto &r : res) {
    int x1 = max(0, r.box[0]);
    int y1 = max(0, r.box[1]);
    int x2 = min(w - 1, r.box[2]);
    int y2 = min(h - 1, r.box[3]);
    drawBox(fb->buf, w, h, x1, y1, x2, y2);
    count++;
  }
  faceCount = count;

  // 狀態、LED、輸出腳位
  if (count > 0) lastSeen = millis();
  bool present = (lastSeen != 0) && (millis() - lastSeen < HOLD_MS);
  if (present != personPresent) {
    personPresent = present;
    digitalWrite(LED_PIN, present ? LOW : HIGH);
    digitalWrite(OUTPUT_PIN, present ? HIGH : LOW);
    Serial.println(present ? ">>> 有人" : ">>> 冇人");
  }

  // 轉 JPEG 俾網頁用
  uint8_t *out = NULL;
  size_t outLen = 0;
  bool ok = fmt2jpg(fb->buf, fb->len, w, h, PIXFORMAT_RGB565,
                    JPEG_QUALITY, &out, &outLen);
  esp_camera_fb_return(fb);

  if (ok && out) {
    xSemaphoreTake(mtx, portMAX_DELAY);
    free(jpgBuf);
    jpgBuf = out;
    jpgLen = outLen;
    frameId++;
    xSemaphoreGive(mtx);
  }
}

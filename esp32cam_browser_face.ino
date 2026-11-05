// ESP32-CAM 即時影像（AP 模式）+ 手機瀏覽器做人臉偵測並畫綠框
// 適用 ESP32 core 3.3.x（唔使 esp-dl，唔使降版本）
// 板：AI Thinker ESP32-CAM（或 ESP32 Dev Module + PSRAM Enabled）
// 手機連 WiFi「ESP32CAM-Face」，Chrome 開 http://192.168.4.1/

#include <WiFi.h>
#include "esp_camera.h"
#include "esp_http_server.h"

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

httpd_handle_t server = NULL;

bool initCamera() {
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
  c.frame_size   = FRAMESIZE_VGA;       // 640x480
  c.jpeg_quality = 14;                  // 數字越細畫質越好
  c.fb_count     = 2;
  c.fb_location  = CAMERA_FB_IN_PSRAM;
  c.grab_mode    = CAMERA_GRAB_LATEST;
  if (!psramFound()) {                  // 冇 PSRAM 就降規格
    c.frame_size = FRAMESIZE_QVGA;
    c.fb_count = 1;
    c.fb_location = CAMERA_FB_IN_DRAM;
  }
  return esp_camera_init(&c) == ESP_OK;
}

static const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32-CAM 人臉偵測</title>
<style>
body{margin:0;background:#111;color:#eee;font-family:sans-serif;text-align:center}
canvas{width:100%;max-width:640px;display:block;margin:0 auto;background:#000}
#st{font-size:22px;padding:12px}
.on{color:#4caf50}.off{color:#999}.warn{color:#ffb300;font-size:16px}
</style></head><body>
<canvas id="cv" width="640" height="480"></canvas>
<div id="st" class="off">連線中…</div>
<script>
const cv=document.getElementById('cv'),ctx=cv.getContext('2d'),st=document.getElementById('st');
let det=null;
if('FaceDetector' in window){
  try{det=new FaceDetector({fastMode:true,maxDetectedFaces:5});}catch(e){det=null;}
}
if(!det){st.className='warn';st.textContent='呢個瀏覽器唔支援人臉偵測，只顯示影像。請用手機 Chrome。';}
let busy=false;
async function loop(){
  if(busy)return; busy=true;
  try{
    const r=await fetch('/capture?'+Date.now(),{cache:'no-store'});
    const bmp=await createImageBitmap(await r.blob());
    cv.width=bmp.width;cv.height=bmp.height;
    ctx.drawImage(bmp,0,0);
    if(det){
      const faces=await det.detect(bmp);
      ctx.strokeStyle='#00ff00';ctx.lineWidth=3;
      for(const f of faces){
        const b=f.boundingBox,s=Math.max(b.width,b.height);
        ctx.strokeRect(b.x+b.width/2-s/2,b.y+b.height/2-s/2,s,s);
      }
      if(faces.length){st.className='on';st.textContent='有人（人臉數：'+faces.length+'）';}
      else{st.className='off';st.textContent='冇人';}
    }
    bmp.close();
  }catch(e){}
  busy=false;
  setTimeout(loop,0);
}
loop();
</script></body></html>
)rawliteral";

static esp_err_t index_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t capture_handler(httpd_req_t *req) {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  esp_err_t r = httpd_resp_send(req, (const char *)fb->buf, fb->len);
  esp_camera_fb_return(fb);
  return r;
}

void startServer() {
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.server_port = 80;
  httpd_uri_t idx = { "/",        HTTP_GET, index_handler,   NULL };
  httpd_uri_t cap = { "/capture", HTTP_GET, capture_handler, NULL };
  if (httpd_start(&server, &cfg) == ESP_OK) {
    httpd_register_uri_handler(server, &idx);
    httpd_register_uri_handler(server, &cap);
  }
}

void setup() {
  Serial.begin(115200);
  if (!initCamera()) {
    Serial.println("鏡頭初始化失敗，檢查排線、腳位同電源");
    while (true) delay(1000);
  }
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS, 1, 0, 2);
  Serial.print("連 WiFi「");
  Serial.print(AP_SSID);
  Serial.print("」，開 http://");
  Serial.println(WiFi.softAPIP());
  startServer();
}

void loop() {
  delay(1000);
}


#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/i2c_master.h"

#include "esp_log.h"
#include "esp_err.h"

#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"

#include "esp_http_server.h"

//================= CONFIG =================
#define WIFI_SSID   "Aum"       // <-- แก้ให้ตรง Wi-Fi ของเรา
#define WIFI_PASS   "12345678"   // <-- แก้ให้ตรง Wi-Fi ของเรา

#define I2C_MASTER_PORT      0
#define I2C_MASTER_SDA_IO    25
#define I2C_MASTER_SCL_IO    26
#define I2C_MASTER_FREQ_HZ   100000

#define DS3231_ADDR          0x68

#define READ_INTERVAL_MS     5000

static const char *TAG = "APP";

//================= I2C Handles ===============
static i2c_master_bus_handle_t bus = NULL;
static i2c_master_dev_handle_t ds3231_dev = NULL;

//================= IP String =================
static char ip_str[16] = "Not connected";

//================= Utils: BCD <-> DEC =========
static inline uint8_t bcd_to_dec(uint8_t val) { return ((val >> 4) * 10) + (val & 0x0F); }
static inline uint8_t dec_to_bcd(uint8_t val) { return ((val / 10) << 4) | (val % 10); }

//================= DS3231 MODEL ==============
typedef struct {
    uint8_t hour;   // 0-23
    uint8_t min;    // 0-59
    uint8_t sec;    // 0-59
    uint8_t day;    // 1-31
    uint8_t month;  // 1-12
    uint8_t year;   // 0-99   (20YY)
} rtc_time_t;

//================= I2C INIT ==================
static void i2c_init(void)
{
    i2c_master_bus_config_t bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_MASTER_PORT,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &bus));

    i2c_device_config_t ds_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = DS3231_ADDR,
        .scl_speed_hz   = I2C_MASTER_FREQ_HZ,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &ds_cfg, &ds3231_dev));

    ESP_LOGI(TAG, "I2C init done (SDA=%d, SCL=%d)", I2C_MASTER_SDA_IO, I2C_MASTER_SCL_IO);
}

// (ทางเลือก) i2c scan ช่วยดีบัก hardware
static void i2c_scan(void)
{
    ESP_LOGI(TAG, "I2C scanning...");
    for (uint8_t addr = 0x03; addr < 0x78; addr++) {
        // API ตัวนี้มีใน IDF 5.x
        esp_err_t err = i2c_master_probe(bus, addr, pdMS_TO_TICKS(20));
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Found device at 0x%02X", addr);
        }
    }
}

//================= DS3231: Read/Write =========
static esp_err_t ds3231_get_time(rtc_time_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;

    uint8_t reg = 0x00;
    uint8_t data[7] = {0};

    esp_err_t err = i2c_master_transmit_receive(ds3231_dev, &reg, 1, data, 7, -1);
    if (err != ESP_OK) return err;

    out->sec   = bcd_to_dec(data[0] & 0x7F);
    out->min   = bcd_to_dec(data[1] & 0x7F);
    // hour reg (24h mode bit6=0)
    out->hour  = bcd_to_dec(data[2] & 0x3F);
    out->day   = bcd_to_dec(data[4] & 0x3F);
    out->month = bcd_to_dec(data[5] & 0x1F);
    out->year  = bcd_to_dec(data[6]);

    return ESP_OK;
}

static esp_err_t ds3231_set_time(const rtc_time_t *t)
{
    if (!t) return ESP_ERR_INVALID_ARG;
    // validate
    if (t->hour > 23 || t->min > 59 || t->sec > 59 ||
        t->day == 0 || t->day > 31 || t->month == 0 || t->month > 12 || t->year > 99) {
        return ESP_ERR_INVALID_ARG;
    }

    // weekday ใส่ 1 แบบ placeholder (ไม่ได้ใช้)
    uint8_t payload[8];
    payload[0] = 0x00;                   // start at seconds register
    payload[1] = dec_to_bcd(t->sec);
    payload[2] = dec_to_bcd(t->min);
    payload[3] = dec_to_bcd(t->hour) & 0x3F; // บังคับ 24h mode bit6=0
    payload[4] = 1;                          // weekday (ไม่ใช้)
    payload[5] = dec_to_bcd(t->day);
    payload[6] = dec_to_bcd(t->month);
    payload[7] = dec_to_bcd(t->year);

    return i2c_master_transmit(ds3231_dev, payload, sizeof(payload), -1);
}

//================= Wi-Fi (STA) ================
static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "Disconnected. Reconnecting...");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        esp_ip4addr_ntoa(&event->ip_info.ip, ip_str, sizeof(ip_str));
        ESP_LOGI(TAG, "Got IP: %s", ip_str);
    }
}

static void wifi_init_sta(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler, NULL, NULL));

    wifi_config_t wc = {0};
    strncpy((char *)wc.sta.ssid, WIFI_SSID, sizeof(wc.sta.ssid));
    strncpy((char *)wc.sta.password, WIFI_PASS, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to Wi-Fi...");
}

//================= HTTP helpers =================
// URL decode in-place (สำหรับ x-www-form-urlencoded)
static void url_decode_inplace(char *s)
{
    char *src = s, *dst = s;
    while (*src) {
        if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else if (*src == '%' && isxdigit((unsigned char)src[1]) && isxdigit((unsigned char)src[2])) {
            char hex[3] = { src[1], src[2], 0 };
            *dst++ = (char) strtol(hex, NULL, 16);
            src += 3;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

// ดึงค่า int (0-255) จาก key=... ใน form body (เช่น "hour=12&min=34")
static bool form_get_u8(const char *body, const char *key, uint8_t *out)
{
    if (!body || !key || !out) return false;

    size_t keylen = strlen(key);
    const char *p = body;
    while ((p = strstr(p, key)) != NULL) {
        if ((p == body || *(p-1) == '&') && p[keylen] == '=') {
            p += keylen + 1;
            // คัดลอกเลขจนกว่าจะเจอ & หรือจบสตริง
            char num[6] = {0};
            int i = 0;
            while (*p && *p != '&' && i < (int)sizeof(num)-1) {
                num[i++] = *p++;
            }
            // แปลงเป็นเลข
            int v = atoi(num);
            if (v < 0) v = 0;
            if (v > 255) v = 255;
            *out = (uint8_t)v;
            return true;
        }
        p += keylen;
    }
    return false;
}

//================= HTML ========================
static const char *HTML_PAGE =
"<!DOCTYPE html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width, initial-scale=1'>"
"<title>ESP32 DS3231</title>"
"<style>"
"body{font-family:system-ui,Arial;background:#f6f7fb;margin:0;padding:24px;display:flex;justify-content:center;}"
".card{width:100%;max-width:720px;background:white;border-radius:16px;box-shadow:0 10px 30px rgba(0,0,0,.08);padding:24px;}"
"h1{margin:0 0 12px 0;font-size:22px}"
".sub{color:#666;margin-bottom:18px}"
".row{display:flex;gap:12px;flex-wrap:wrap;margin:12px 0}"
".field{flex:1 1 100px;min-width:110px}"
"label{font-size:12px;color:#555;display:block;margin-bottom:6px}"
"input{width:100%;padding:10px 12px;border:1px solid #dfe3eb;border-radius:10px;font-size:14px;outline:none}"
"input:focus{border-color:#4c8bf5;box-shadow:0 0 0 3px rgba(76,139,245,.15)}"
".btn{padding:12px 16px;border-radius:12px;border:0;background:#4c8bf5;color:white;font-weight:600;cursor:pointer}"
".btn:active{transform:translateY(1px)}"
".pill{display:inline-block;background:#eef3ff;color:#2d55bd;padding:6px 10px;border-radius:999px;font-size:12px}"
".grid{display:grid;grid-template-columns:1fr 1fr;gap:16px}"
"@media(max-width:560px){.grid{grid-template-columns:1fr}}"
".ok{color:#2e7d32}.err{color:#c62828}"
"</style></head><body>"
"<div class='card'>"
"<h1>ESP32 DS3231 Control</h1>"
"<div class='sub'>IP: <span class='pill' id='ip'>%s</span></div>"
"<div class='grid'>"
" <div>"
"   <h3>Current Time</h3>"
"   <div id='now' style='font-size:28px;font-weight:700'>--:--:--</div>"
"   <div id='date' style='color:#555;margin-top:6px'>--/--/----</div>"
" </div>"
" <div>"
"   <h3>Set Time</h3>"
"   <div class='row'>"
"     <div class='field'><label>Hour (0-23)</label><input id='hour' type='number' min='0' max='23' placeholder='HH'></div>"
"     <div class='field'><label>Minute (0-59)</label><input id='min'  type='number' min='0' max='59' placeholder='MM'></div>"
"     <div class='field'><label>Second (0-59)</label><input id='sec'  type='number' min='0' max='59' placeholder='SS'></div>"
"   </div>"
"   <div class='row'>"
"     <div class='field'><label>Day (1-31)</label><input id='day'   type='number' min='1' max='31' placeholder='DD'></div>"
"     <div class='field'><label>Month (1-12)</label><input id='month' type='number' min='1' max='12' placeholder='MM'></div>"
"     <div class='field'><label>Year (00-99)</label><input id='year' type='number' min='0' max='99'  placeholder='YY'></div>"
"   </div>"
"   <button class='btn' id='btnSet'>Set Time</button>"
"   <div id='msg' style='margin-top:10px;font-size:13px'></div>"
" </div>"
"</div>"
"</div>"
"<script>"
"async function refresh(){"
" try{"
"  const r=await fetch('/api/time');"
"  if(!r.ok) throw new Error('HTTP '+r.status);"
"  const j=await r.json();"
"  document.getElementById('now').textContent=j.time;"
"  document.getElementById('date').textContent=j.date;"
" }catch(e){console.log(e);}"
"}"
"setInterval(refresh,1000);refresh();"
"document.getElementById('btnSet').addEventListener('click',async()=>{"
" const q=new URLSearchParams();"
" const v=(id)=>document.getElementById(id).value.trim();"
" q.set('hour', v('hour')); q.set('min', v('min')); q.set('sec', v('sec'));"
" q.set('day', v('day')); q.set('month', v('month')); q.set('year', v('year'));"
" const res=await fetch('/api/set_time',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:q.toString()});"
" const t=await res.text();"
" const msg=document.getElementById('msg');"
" msg.textContent=t;"
" msg.className=res.ok?'ok':'err';"
"});"
"</script>"
"</body></html>";

//================= HTTP Handlers ===============
static esp_err_t root_get_handler(httpd_req_t *req)
{
    // ใส่ IP ลง HTML แล้วค่อยส่ง (กัน buffer ไม่พอ: ใช้ heap)
    size_t need = strlen(HTML_PAGE) + strlen(ip_str) + 64;
    char *page = (char *)malloc(need);
    if (!page) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    snprintf(page, need, HTML_PAGE, ip_str);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, page);
    free(page);
    return ESP_OK;
}

static esp_err_t api_time_handler(httpd_req_t *req)
{
    rtc_time_t t;
    if (ds3231_get_time(&t) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "I2C read failed");
        return ESP_FAIL;
    }
    char buf[64];
    // date as dd/mm/20yy
    char date[16];
    snprintf(date, sizeof(date), "%02u/%02u/20%02u", t.day, t.month, t.year);
    snprintf(buf, sizeof(buf), "{\"time\":\"%02u:%02u:%02u\",\"date\":\"%s\"}", t.hour, t.min, t.sec, date);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

static esp_err_t api_set_time_handler(httpd_req_t *req)
{
    // อ่าน body ทั้งหมดตาม content_len
    int total = req->content_len;
    if (total <= 0 || total > 1024) { // กัน body ใหญ่เกินไป
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body size");
        return ESP_FAIL;
    }
    char *body = (char *)malloc(total + 1);
    if (!body) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    int received = 0;
    while (received < total) {
        int r = httpd_req_recv(req, body + received, total - received);
        if (r <= 0) {
            free(body);
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        received += r;
    }
    body[received] = '\0';
    url_decode_inplace(body);

    uint8_t hour=0,min=0,sec=0,day=0,month=0,year=0;
    bool ok = true;
    ok &= form_get_u8(body, "hour",  &hour);
    ok &= form_get_u8(body, "min",   &min);
    ok &= form_get_u8(body, "sec",   &sec);
    ok &= form_get_u8(body, "day",   &day);
    ok &= form_get_u8(body, "month", &month);
    ok &= form_get_u8(body, "year",  &year);

    free(body);

    // validate ช่วงตัวเลข
    if (!ok || hour>23 || min>59 || sec>59 || day<1 || day>31 || month<1 || month>12 || year>99) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid time fields");
        return ESP_FAIL;
    }

    rtc_time_t t = { .hour=hour, .min=min, .sec=sec, .day=day, .month=month, .year=year };
    esp_err_t err = ds3231_set_time(&t);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "I2C write failed");
        return ESP_FAIL;
    }

    httpd_resp_sendstr(req, "Time set successfully!");
    return ESP_OK;
}

static httpd_handle_t start_webserver(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    // ปรับเพิ่มเพื่อเลี่ยง 431 (header/uri ยาว)

    config.lru_purge_enable   = true;   // ไล่ purge ถ้าหน่วยความจำไม่พอ
    config.stack_size         = 8192;   // กัน stack handler เล็กเกิน

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        return NULL;
    }

    httpd_uri_t root = {
        .uri      = "/",
        .method   = HTTP_GET,
        .handler  = root_get_handler,
        .user_ctx = NULL
    };
    httpd_uri_t timeapi = {
        .uri      = "/api/time",
        .method   = HTTP_GET,
        .handler  = api_time_handler,
        .user_ctx = NULL
    };
    httpd_uri_t setapi = {
        .uri      = "/api/set_time",
        .method   = HTTP_POST,
        .handler  = api_set_time_handler,
        .user_ctx = NULL
    };

    httpd_register_uri_handler(server, &root);
    httpd_register_uri_handler(server, &timeapi);
    httpd_register_uri_handler(server, &setapi);

    ESP_LOGI(TAG, "HTTP server started");
    return server;
}

//================= Task: print every 5s =========
static void ds3231_task(void *arg)
{
    while (1) {
        rtc_time_t t;
        esp_err_t err = ds3231_get_time(&t);
        if (err == ESP_OK) {
            printf("DS3231 Time: %02u:%02u:%02u Date: %02u/%02u/20%02u\n",
                   t.hour, t.min, t.sec, t.day, t.month, t.year);
        } else {
            ESP_LOGE(TAG, "I2C read error: %s", esp_err_to_name(err));
        }
        vTaskDelay(pdMS_TO_TICKS(READ_INTERVAL_MS));
    }
}

//================= MAIN ========================
void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    i2c_init();
    // (ทางเลือก) ช่วยเช็กว่าเห็น 0x68 หรือไม่
    // i2c_scan();

    wifi_init_sta();
    start_webserver();

    xTaskCreatePinnedToCore(ds3231_task, "ds3231_task", 4096, NULL, 5, NULL, 1);
}

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "esp_chip_info.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "driver/gpio.h"

// index.html/style.css/htmx.min.js.gz as C byte arrays, generated via
// `xxd -i` from the files sitting alongside this one — see
// src/web_assets.h. Simpler and more portable than ESP-IDF's
// EMBED_FILES/EMBED_TXTFILES linker-symbol mechanism, and sidesteps a couple
// of real bugs in this PlatformIO release's CMake<->SCons integration for
// espidf file embedding.
#include "web_assets.h"

// TODO: fill in your WiFi network credentials before flashing
// Must be a 2.4GHz network — the ESP32 radio does not support 5GHz.
#define WIFI_SSID ""
#define WIFI_PASSWORD ""
#define WIFI_MAX_RETRY 10

// GPIO2 drives the onboard blue LED on the esp32doit-devkit-v1 board.
#define LED_PIN GPIO_NUM_2

// Upper bound on user-requested blink counts (see blink_get_handler below).
#define BLINK_MAX_COUNT 10

static const char *TAG = "hello-world";

// FreeRTOS event group used to block app_main() until WiFi either connects
// or gives up. The WiFi driver runs in its own background tasks and reports
// results asynchronously through the event system below — there's no
// blocking connect() call like on a desktop OS.
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

static int s_retry_count = 0;

// Invoked by the default event loop task whenever a WiFi/IP event fires.
// This runs in that task, not in app_main() — that's why results are
// reported back via the event group instead of a return value.
static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
    if (s_retry_count < WIFI_MAX_RETRY) {
      gpio_set_level(LED_PIN, !gpio_get_level(LED_PIN)); // blink LED once per retry
      esp_wifi_connect();
      s_retry_count++;
      ESP_LOGI(TAG, "retrying WiFi connection (%d/%d)", s_retry_count, WIFI_MAX_RETRY);
    } else {
      xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
    }
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    ESP_LOGI(TAG, "connected! IP address: " IPSTR, IP2STR(&event->ip_info.ip));
    s_retry_count = 0;
    xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
  }
}

// Logs every nearby WiFi network — useful for confirming the exact SSID
// spelling/case and that it's actually visible (2.4GHz range). Requires the
// WiFi driver to already be started (esp_wifi_start()).
static void wifi_scan(void) {
  ESP_LOGI(TAG, "Scanning for WiFi networks...");
  wifi_scan_config_t scan_config = {.show_hidden = true};
  ESP_ERROR_CHECK(esp_wifi_scan_start(&scan_config, true)); // true = block until the scan completes

  uint16_t ap_count = 0;
  esp_wifi_scan_get_ap_num(&ap_count);
  wifi_ap_record_t *ap_records = calloc(ap_count, sizeof(wifi_ap_record_t));
  esp_wifi_scan_get_ap_records(&ap_count, ap_records);

  for (int i = 0; i < ap_count; i++) {
    ESP_LOGI(TAG, "%2d: %-32s RSSI=%d %s", i, (char *)ap_records[i].ssid, ap_records[i].rssi,
             ap_records[i].authmode == WIFI_AUTH_OPEN ? "OPEN" : "");
  }
  free(ap_records);
}

// Brings up the WiFi driver in station mode, connects, and blocks until we
// either get an IP or exhaust our retries. Returns true on success.
static bool wifi_connect(void) {
  s_wifi_event_group = xEventGroupCreate();

  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_create_default_wifi_sta();

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_start()); // driver must be running before we can scan

  wifi_scan();

  // Register handlers only after the scan, so the driver's own
  // WIFI_EVENT_STA_START (fired by esp_wifi_start above) doesn't race with
  // wifi_config being set below — the first connect attempt is triggered
  // manually further down instead.
  esp_event_handler_instance_t instance_any_id;
  esp_event_handler_instance_t instance_got_ip;
  ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL,
                                                        &instance_any_id));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL,
                                                        &instance_got_ip));

  wifi_config_t wifi_config = {
      .sta =
          {
              .ssid = WIFI_SSID,
              .password = WIFI_PASSWORD,
              .threshold.authmode = WIFI_AUTH_WPA2_PSK,
          },
  };
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

  ESP_LOGI(TAG, "Connecting to WiFi...");
  esp_wifi_connect();

  // Block here until wifi_event_handler() reports success or failure —
  // equivalent to the Arduino version's `while (WiFi.status() != WL_CONNECTED)` poll loop,
  // but driven by events instead of busy-waiting.
  EventBits_t bits =
      xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, portMAX_DELAY);

  return (bits & WIFI_CONNECTED_BIT) != 0;
}

static esp_err_t root_get_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  httpd_resp_send(req, (const char *)index_html, index_html_len);
  return ESP_OK;
}

static esp_err_t style_get_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/css");
  httpd_resp_send(req, (const char *)style_css, style_css_len);
  return ESP_OK;
}

// htmx.min.js.gz was gzip-compressed once, up front, when it was vendored.
// The ESP32 never decompresses it — it just serves the raw compressed bytes
// with a header telling the browser to do the decompression itself.
static esp_err_t htmx_get_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "application/javascript");
  httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
  httpd_resp_send(req, (const char *)htmx_min_js_gz, htmx_min_js_gz_len);
  return ESP_OK;
}

// Each of the handlers below backs one status card in index.html and
// returns just the value text as a plain HTML fragment — htmx swaps it
// directly into the card's target element, so there's no JSON parsing (or
// even a full page reload) involved on the client side. Uptime/heap/tasks
// are triggered by their card's "Refresh" button (plus once on page load);
// flashing the LED marks those as visible, user-triggered events. WiFi has
// no button — it polls continuously (hx-trigger="every 2s" in the HTML) —
// so it skips the LED flash, which would otherwise just be constant noise
// rather than a meaningful signal.

// Shared tail for the four handlers below: sends buf/len as an HTML
// fragment, optionally flashing the LED first to mark a user-triggered
// request (see the comment above for which handlers want that).
static esp_err_t send_text_response(httpd_req_t *req, const char *buf, int len, bool flash_led) {
  if (flash_led) {
    gpio_set_level(LED_PIN, 1);
  }
  httpd_resp_set_type(req, "text/html");
  httpd_resp_send(req, buf, len);
  if (flash_led) {
    vTaskDelay(pdMS_TO_TICKS(100)); // keep the LED on just long enough to see it blink
    gpio_set_level(LED_PIN, 0);
  }
  return ESP_OK;
}

static esp_err_t uptime_get_handler(httpd_req_t *req) {
  int64_t uptime_seconds = esp_timer_get_time() / 1000000;
  char buf[32];
  int len = snprintf(buf, sizeof(buf), "%lld s", uptime_seconds);
  return send_text_response(req, buf, len, true);
}

static esp_err_t heap_get_handler(httpd_req_t *req) {
  uint32_t free_heap = esp_get_free_heap_size();
  char buf[32];
  int len = snprintf(buf, sizeof(buf), "%lu bytes", (unsigned long)free_heap);
  return send_text_response(req, buf, len, true);
}

static esp_err_t wifi_get_handler(httpd_req_t *req) {
  wifi_ap_record_t ap_info = {0};
  esp_wifi_sta_get_ap_info(&ap_info); // same struct used by wifi_scan() above
  char buf[32];
  int len = snprintf(buf, sizeof(buf), "%d dBm", ap_info.rssi);
  return send_text_response(req, buf, len, false);
}

static esp_err_t tasks_get_handler(httpd_req_t *req) {
  UBaseType_t task_count = uxTaskGetNumberOfTasks(); // one per driver/event-loop/httpd worker, plus app_main's own
  char buf[32];
  int len = snprintf(buf, sizeof(buf), "%u tasks", (unsigned int)task_count);
  return send_text_response(req, buf, len, true);
}

// Backs the "Blink LED" card. Takes a `count` query param (from the number
// input in index.html, capped client-side at BLINK_MAX_COUNT) and blinks the
// LED that many times, clamping server-side too since query params are
// user-controlled input. Blinks the LED directly instead of going through
// send_text_response's single-flash behavior, since this needs N on/off
// cycles rather than one.
static esp_err_t blink_get_handler(httpd_req_t *req) {
  int count = 1;
  char query[16];
  char val[8];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
      httpd_query_key_value(query, "count", val, sizeof(val)) == ESP_OK) {
    count = atoi(val);
  }
  if (count < 1) {
    count = 1;
  } else if (count > BLINK_MAX_COUNT) {
    count = BLINK_MAX_COUNT;
  }

  for (int i = 0; i < count; i++) {
    gpio_set_level(LED_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(150));
    gpio_set_level(LED_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(150));
  }

  char buf[32];
  int len = snprintf(buf, sizeof(buf), "Blinked %d time%s", count, count == 1 ? "" : "s");
  return send_text_response(req, buf, len, false);
}

// Backs the "Dice roll" card. esp_random() draws from the ESP32's hardware
// RNG (seeded by RF/thermal noise), not a software PRNG, so this is a real
// hardware feature demo rather than just math.
static esp_err_t dice_get_handler(httpd_req_t *req) {
  int roll = (esp_random() % 6) + 1;
  char buf[8];
  int len = snprintf(buf, sizeof(buf), "%d", roll);
  return send_text_response(req, buf, len, true);
}

static const char *chip_model_name(esp_chip_model_t model) {
  switch (model) {
  case CHIP_ESP32:
    return "ESP32";
  case CHIP_ESP32S2:
    return "ESP32-S2";
  case CHIP_ESP32S3:
    return "ESP32-S3";
  case CHIP_ESP32C3:
    return "ESP32-C3";
  default:
    return "unknown chip";
  }
}

// Backs the chip-info banner above the grid. Static hardware facts (model,
// revision, core count, radio features) read via esp_chip_info() — unlike
// the cards below, this never changes at runtime, so it loads once on page
// load with no refresh button and no LED flash.
static esp_err_t chipinfo_get_handler(httpd_req_t *req) {
  esp_chip_info_t info;
  esp_chip_info(&info);
  char buf[96];
  int len = snprintf(buf, sizeof(buf), "%s rev v%d.%d &middot; %d core%s &middot; %s%s%s", chip_model_name(info.model),
                      info.revision / 100, info.revision % 100, info.cores, info.cores == 1 ? "" : "s",
                      (info.features & CHIP_FEATURE_WIFI_BGN) ? "WiFi" : "", (info.features & CHIP_FEATURE_BT) ? "+BT" : "",
                      (info.features & CHIP_FEATURE_BLE) ? "+BLE" : "");
  return send_text_response(req, buf, len, false);
}

// To add a future route: write a handler function and append an entry here
// — start_webserver() below registers whatever is in this table.
static const httpd_uri_t routes[] = {
    {.uri = "/", .method = HTTP_GET, .handler = root_get_handler},
    {.uri = "/style.css", .method = HTTP_GET, .handler = style_get_handler},
    {.uri = "/htmx.min.js", .method = HTTP_GET, .handler = htmx_get_handler},
    {.uri = "/api/uptime", .method = HTTP_GET, .handler = uptime_get_handler},
    {.uri = "/api/heap", .method = HTTP_GET, .handler = heap_get_handler},
    {.uri = "/api/wifi", .method = HTTP_GET, .handler = wifi_get_handler},
    {.uri = "/api/tasks", .method = HTTP_GET, .handler = tasks_get_handler},
    {.uri = "/api/blink", .method = HTTP_GET, .handler = blink_get_handler},
    {.uri = "/api/dice", .method = HTTP_GET, .handler = dice_get_handler},
    {.uri = "/api/chipinfo", .method = HTTP_GET, .handler = chipinfo_get_handler},
};

// Starts the HTTP server and registers every route in the table above.
static void start_webserver(void) {
  httpd_handle_t server = NULL;
  httpd_config_t config = HTTPD_DEFAULT_CONFIG(); // listens on TCP port 80 by default
  config.max_uri_handlers = sizeof(routes) / sizeof(routes[0]); // default of 8 is too small for our route table

  ESP_ERROR_CHECK(httpd_start(&server, &config));
  for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &routes[i]));
  }
  ESP_LOGI(TAG, "HTTP server started");
}

// Entry point on ESP-IDF (there's no setup()/loop() here — app_main runs
// once, in its own FreeRTOS task, and can return: the WiFi driver and HTTP
// server keep running in their own background tasks after it does).
void app_main(void) {
  // WiFi needs NVS to store calibration data; initialize it first,
  // reformatting if a previous, incompatible layout is found.
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  gpio_reset_pin(LED_PIN);
  gpio_set_direction(LED_PIN, GPIO_MODE_OUTPUT);

  if (!wifi_connect()) {
    // Gave up after WIFI_MAX_RETRY attempts. Common causes: wrong
    // SSID/password, or a 5GHz-only network the ESP32 can't see.
    ESP_LOGE(TAG, "Failed to connect to WiFi");
    gpio_set_level(LED_PIN, 1); // solid LED = connection failed (visible even without Serial open)
    return;                     // skip starting the HTTP server — there's no network to serve on
  }
  gpio_set_level(LED_PIN, 0); // LED off = connected successfully

  start_webserver();
}

#include "web_server.h"

#include <unistd.h>

#include "cJSON.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mdns.h"
#include "nvs_flash.h"
#include "secrets.h"
#include "usb_driver.h"

#define MDNS_HOST_NAME "amp"  // amp.local
#define MAX_CLIENTS 7         // Should Max(CONFIG_LWIP_MAX_SOCKETS-3)

typedef struct {
  uint8_t preset_a;
  uint8_t preset_b;
  uint32_t min_time_s;
  uint32_t max_time_s;
} ab_test_config_t;

typedef struct {
  bool is_running;
  bool is_finished;
} ab_test_state_t;

static const char *TAG_WEB = "WEB_SERVER";
static httpd_handle_t server = NULL;
static int client_fds[MAX_CLIENTS] = {0};
static char filter_name[FILTER_NAME_MAX_LEN];

static volatile bool test_mode_enabled = false;
static ab_test_state_t ab_test_state = {0};
static ab_test_config_t ab_test_config = {0};
static SemaphoreHandle_t ab_test_mutex;
static StaticSemaphore_t ab_test_mutex_buffer;
static TaskHandle_t ab_test_task_handle = NULL;

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");
extern const uint8_t index_css_start[] asm("_binary_index_css_start");
extern const uint8_t index_css_end[] asm("_binary_index_css_end");
extern const uint8_t index_js_start[] asm("_binary_index_js_start");
extern const uint8_t index_js_end[] asm("_binary_index_js_end");
extern const uint8_t favicon_ico_start[] asm("_binary_favicon_ico_start");
extern const uint8_t favicon_ico_end[] asm("_binary_favicon_ico_end");

void send_to_clients(cJSON *root) {
  char *json_string = cJSON_PrintUnformatted(root);
  // Log the JSON string
  ESP_LOGI(TAG_WEB, "%s", json_string);

  httpd_ws_frame_t ws_pkt;
  memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
  ws_pkt.payload = (uint8_t *)json_string;
  ws_pkt.len = strlen(json_string);
  ws_pkt.type = HTTPD_WS_TYPE_TEXT;
  for (int i = 0; i < MAX_CLIENTS; i++) {
    if (client_fds[i] != 0) {
      httpd_ws_send_frame_async(server, client_fds[i], &ws_pkt);
    }
  }
  free(json_string);
}

void notify_state_changed(const state_t *state) {
  if (!server) return;

  cJSON *root = cJSON_CreateObject();
  // cJSON_AddBoolToObject(root, "test_mode_enabled", test_mode_enabled);

  // Amp state
  if (state != NULL) {
    cJSON *amp_state_json = cJSON_CreateObject();
    cJSON_AddNumberToObject(amp_state_json, "preset", state->preset);
    cJSON_AddNumberToObject(amp_state_json, "volume_db", state->volume_db);
    cJSON_AddBoolToObject(amp_state_json, "is_muted", state->is_muted);
    cJSON_AddNumberToObject(amp_state_json, "current_source",
                            state->current_source);
    cJSON *source_array = cJSON_CreateArray();
    for (int i = 0; i < 3; i++) {
      cJSON_AddItemToArray(source_array,
                           cJSON_CreateNumber(state->preset_source[i]));
    }
    cJSON_AddItemToObject(amp_state_json, "preset_source", source_array);
    cJSON *eq_on_array = cJSON_CreateArray();
    for (int i = 0; i < 3; i++) {
      cJSON_AddItemToArray(eq_on_array, cJSON_CreateBool(state->is_eq_on[i]));
    }
    cJSON_AddItemToObject(amp_state_json, "eq_on", eq_on_array);
    get_filter_name(&filter_name[0]);
    cJSON_AddStringToObject(amp_state_json, "filter_name", filter_name);
    cJSON_AddItemToObject(root, "amp_state", amp_state_json);
  }

  // A/B Test Status
  if (test_mode_enabled) {
    cJSON *ab_test_json = cJSON_CreateObject();
    xSemaphoreTake(ab_test_mutex, portMAX_DELAY);
    cJSON_AddBoolToObject(ab_test_json, "is_running", ab_test_state.is_running);
    cJSON_AddBoolToObject(ab_test_json, "is_finished",
                          ab_test_state.is_finished);
    cJSON_AddNumberToObject(ab_test_json, "preset_a", ab_test_config.preset_a);
    cJSON_AddNumberToObject(ab_test_json, "preset_b", ab_test_config.preset_b);
    xSemaphoreGive(ab_test_mutex);
    cJSON_AddItemToObject(root, "ab_test", ab_test_json);
  }

  send_to_clients(root);
  cJSON_Delete(root);
}

void enable_test_mode(bool enable) {
  test_mode_enabled = enable;
  // propagate mode to other clients
  notify_state_changed(NULL);
}

static void ab_test_task(void *arg) {
  ESP_LOGI(TAG_WEB, "A/B Test Task gestartet.");
  TickType_t test_start_time = xTaskGetTickCount();
  TickType_t next_switch_time = test_start_time;

  bool switch_preset = false;
  control_action_t switch_preset_cmd;
  switch_preset_cmd.action = ACTION_SET_PRESET;

  control_action_t mute_cmd;
  mute_cmd.action = ACTION_SET_MUTE;

  xSemaphoreTake(ab_test_mutex, portMAX_DELAY);

  memset(&ab_test_state, 0, sizeof(ab_test_state_t));
  ab_test_state.is_running = true;

  // Select inital preset to start with
  switch_preset_cmd.value = (esp_random() % 2 == 0) ? ab_test_config.preset_a
                                                    : ab_test_config.preset_b;
  xSemaphoreGive(ab_test_mutex);
  enable_test_mode(true);

  enqueue_command(switch_preset_cmd);

  while (1) {
    if (switch_preset) {
      // Unmute and switch, mute is used to hide switching between presets with
      // and without FIR
      mute_cmd.value = 0;
      enqueue_command(mute_cmd);
      enqueue_command(switch_preset_cmd);
      switch_preset = false;
    }

    if (xSemaphoreTake(ab_test_mutex, pdMS_TO_TICKS(50)) != pdTRUE) continue;
    // Check if test should stop
    if (!ab_test_state.is_running) {
      ab_test_state.is_finished = true;
      ESP_LOGI(TAG_WEB, "A/B Test beendet.");
      xSemaphoreGive(ab_test_mutex);
      notify_state_changed(NULL);
      break;
    }

    // Maybe switch presets
    if (xTaskGetTickCount() >= next_switch_time) {
      switch_preset_cmd.value = (esp_random() % 2 == 0)
                                    ? ab_test_config.preset_a
                                    : ab_test_config.preset_b;
      switch_preset = true;

      uint32_t delay_s = ab_test_config.min_time_s +
                         (esp_random() % (ab_test_config.max_time_s -
                                          ab_test_config.min_time_s + 1));
      next_switch_time = xTaskGetTickCount() + pdMS_TO_TICKS(delay_s * 1000);
      ESP_LOGI(TAG_WEB, "A/B: Change to preset %d. Next change in  %lx s",
               switch_preset_cmd.value, delay_s);
    }

    xSemaphoreGive(ab_test_mutex);
    if (switch_preset) {
      // Muting to mask switching between presets with and without FIR, actual
      // switch on the next iteration.
      mute_cmd.value = 1;
      enqueue_command(mute_cmd);
    }
    vTaskDelay(pdMS_TO_TICKS(1000));  // Check every second
  }
  ESP_LOGI(TAG_WEB, "A/B Test Task beendet.");
  ab_test_task_handle = NULL;
  vTaskDelete(NULL);
}

void start_ab_test(ab_test_config_t *cfg) {
  if (xSemaphoreTake(ab_test_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
  if (ab_test_task_handle != NULL) {
    ESP_LOGI(TAG_WEB, "A/B Test already running");
    xSemaphoreGive(ab_test_mutex);
    return;
  }
  memcpy(&ab_test_config, cfg, sizeof(ab_test_config_t));
  xSemaphoreGive(ab_test_mutex);
  ESP_LOGI(TAG_WEB, "Starting ab test");
  BaseType_t task_created = xTaskCreatePinnedToCore(
      ab_test_task, "ab_test_task", 4096, NULL, 3, &ab_test_task_handle, 1);
  assert(task_created == pdTRUE);
}

void reset_test(void) {
  xSemaphoreTake(ab_test_mutex, portMAX_DELAY);
  memset(&ab_test_state, 0, sizeof(ab_test_state_t));
  memset(&ab_test_config, 0, sizeof(ab_test_config_t));
  xSemaphoreGive(ab_test_mutex);
  notify_state_changed(NULL);
}


void stop_ab_test(void) {
  ESP_LOGI(TAG_WEB, "Stopping ab test");
  xSemaphoreTake(ab_test_mutex, portMAX_DELAY);
  ab_test_state.is_running = false;
  ab_test_state.is_finished = true;
  xSemaphoreGive(ab_test_mutex);
  notify_state_changed(NULL);
}

static void send_state() {
  state_t current_state;
  get_state(&current_state);
  notify_state_changed(&current_state);
}

static esp_err_t websocket_handler(httpd_req_t *req) {
  if (req->method == HTTP_GET) {
    int sockfd = httpd_req_to_sockfd(req);
    int free_slot = -1;

    for (int i = 0; i < MAX_CLIENTS; i++) {
      if (client_fds[i] == 0) {
        free_slot = i;
        break;
      }
    }

    if (free_slot == -1) {
      ESP_LOGE(
          TAG_WEB,
          "Maximum number of clients (%d) reached. Rejecting new connection.",
          MAX_CLIENTS);
      close(sockfd);
      return ESP_FAIL;
    }

    ESP_LOGI(TAG_WEB,
             "New client connected, socket fd: %d, assigned to slot %d", sockfd,
             free_slot);
    client_fds[free_slot] = sockfd;

    // sendState();

    return ESP_OK;
  }
  // if HTTP_POST
  httpd_ws_frame_t ws_pkt;
  memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
  ws_pkt.type = HTTPD_WS_TYPE_TEXT;
  esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
  if (ret != ESP_OK) return ret;
  uint8_t *buf = calloc(1, ws_pkt.len + 1);
  ws_pkt.payload = buf;
  ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
  if (ret != ESP_OK) {
    free(buf);
    return ret;
  }
  cJSON *root = cJSON_Parse((char *)ws_pkt.payload);
  if (root) {
    cJSON *action_json = cJSON_GetObjectItem(root, "action");
    cJSON *value_json = cJSON_GetObjectItem(root, "value");
    if (action_json && value_json) {
      control_action_t cmd;

      if (strcmp(action_json->valuestring, "get_state") == 0) {
        send_state();
      } else if (strcmp(action_json->valuestring, "disable_test_mode") == 0) {
        enable_test_mode(false);
      } else if (strcmp(action_json->valuestring, "start_test") == 0 &&
                 value_json) {
        ab_test_config_t cfg;
        cfg.preset_a = cJSON_GetObjectItem(value_json, "preset_a")->valueint;
        cfg.preset_b = cJSON_GetObjectItem(value_json, "preset_b")->valueint;
        cfg.min_time_s = cJSON_GetObjectItem(value_json, "min_time")->valueint;
        cfg.max_time_s = cJSON_GetObjectItem(value_json, "max_time")->valueint;
        start_ab_test(&cfg);
      } else if (strcmp(action_json->valuestring, "stop_test") == 0) {
        stop_ab_test();
      } else if (strcmp(action_json->valuestring, "reset_test") == 0) {
        reset_test();
      } else if (strcmp(action_json->valuestring, "set_preset") == 0) {
        cmd.action = ACTION_SET_PRESET;
        cmd.value = (int8_t)value_json->valueint;
        enqueue_command(cmd);
      } else if (strcmp(action_json->valuestring, "set_volume") == 0) {
        cmd.action = ACTION_SET_VOLUME;
        cmd.value = (int8_t)value_json->valueint;
        enqueue_command(cmd);
      } else if (strcmp(action_json->valuestring, "set_mute") == 0) {
        cmd.action = ACTION_SET_MUTE;
        cmd.value = (int8_t)cJSON_IsTrue(value_json);
        enqueue_command(cmd);
      } else if (strcmp(action_json->valuestring, "set_eq_p1") == 0) {
        cmd.action = ACTION_SET_EQ_P1;
        cmd.value = (int8_t)cJSON_IsTrue(value_json);
        enqueue_command(cmd);
      } else if (strcmp(action_json->valuestring, "set_eq_p2") == 0) {
        cmd.action = ACTION_SET_EQ_P2;
        cmd.value = (int8_t)cJSON_IsTrue(value_json);
        enqueue_command(cmd);
      } else if (strcmp(action_json->valuestring, "set_eq_p3") == 0) {
        cmd.action = ACTION_SET_EQ_P3;
        cmd.value = (int8_t)cJSON_IsTrue(value_json);
        enqueue_command(cmd);
      } else if (strcmp(action_json->valuestring, "set_source_p1") == 0) {
        cmd.action = ACTION_SET_SOURCE_P1;
        cmd.value = (int8_t)value_json->valueint;
        enqueue_command(cmd);
      } else if (strcmp(action_json->valuestring, "set_source_p2") == 0) {
        cmd.action = ACTION_SET_SOURCE_P2;
        cmd.value = (int8_t)value_json->valueint;
        enqueue_command(cmd);
      } else if (strcmp(action_json->valuestring, "set_source_p3") == 0) {
        cmd.action = ACTION_SET_SOURCE_P3;
        cmd.value = (int8_t)value_json->valueint;
        enqueue_command(cmd);
      } else {
        ESP_LOGE(TAG_WEB, "Invaild command received: %s",
                 action_json->valuestring);
      }
    }
    cJSON_Delete(root);
  }
  free(buf);
  return ret;
}

static esp_err_t favicon_get_handler(httpd_req_t *req) {
  ESP_LOGI(TAG_WEB, "Serving favicon");
  httpd_resp_set_type(req, "image/x-icon");
  return httpd_resp_send(req, (const char *)favicon_ico_start,
                         favicon_ico_end - favicon_ico_start - 1);
}

static esp_err_t root_get_handler(httpd_req_t *req) {
  int sockfd = httpd_req_to_sockfd(req);
  ESP_LOGI(TAG_WEB, "Root html handler called. Client #%d connected", sockfd);
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, (const char *)index_html_start,
                         index_html_end - index_html_start - 1);
}
static esp_err_t root_css_get_handler(httpd_req_t *req) {
  int sockfd = httpd_req_to_sockfd(req);
  ESP_LOGI(TAG_WEB, "Root css handler called. Client #%d connected", sockfd);
  httpd_resp_set_type(req, "text/css");
  return httpd_resp_send(req, (const char *)index_css_start,
                         index_css_end - index_css_start - 1);
}
static esp_err_t root_js_get_handler(httpd_req_t *req) {
  int sockfd = httpd_req_to_sockfd(req);
  ESP_LOGI(TAG_WEB, "Root js handler called. Client #%d connected", sockfd);
  ESP_LOGI(TAG_WEB, "Sending index.js");
  httpd_resp_set_type(req, "application/javascript");
  return httpd_resp_send(req, (const char *)index_js_start,
                         index_js_end - index_js_start - 1);
}

static void client_disconnect_handler(void *arg, int sockfd) {
  ESP_LOGI(TAG_WEB, "Client #%d disconnected", sockfd);
  close(sockfd);
  for (int i = 0; i < MAX_CLIENTS; i++) {
    if (client_fds[i] == sockfd) {
      client_fds[i] = 0;  // Remove client from list
      break;
    }
  }
}

static httpd_handle_t start_webserver(void) {
  httpd_handle_t server_handle = NULL;
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.max_open_sockets = MAX_CLIENTS;
  config.uri_match_fn = httpd_uri_match_wildcard;
  config.close_fn = client_disconnect_handler;
  config.lru_purge_enable = true;
  // config.enable_so_linger = true;
  config.linger_timeout = 1;

  if (httpd_start(&server_handle, &config) == ESP_OK) {
    // web socket
    httpd_uri_t ws_uri = {.uri = "/ws",
                          .method = HTTP_GET,
                          .handler = websocket_handler,
                          .is_websocket = true};
    httpd_register_uri_handler(server_handle, &ws_uri);

    httpd_uri_t favicon_uri = {.uri = "/favicon.ico",
                               .method = HTTP_GET,
                               .handler = favicon_get_handler,
                               .user_ctx = NULL};
    httpd_register_uri_handler(server_handle, &favicon_uri);

    // root
    httpd_uri_t css_root = {.uri = "/index.css",
                            .method = HTTP_GET,
                            .handler = root_css_get_handler};
    httpd_register_uri_handler(server_handle, &css_root);
    httpd_uri_t js_root = {
        .uri = "/index.js", .method = HTTP_GET, .handler = root_js_get_handler};
    httpd_register_uri_handler(server_handle, &js_root);
    httpd_uri_t root_uri = {
        .uri = "/", .method = HTTP_GET, .handler = root_get_handler};
    httpd_register_uri_handler(server_handle, &root_uri);
  }
  return server_handle;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
  if (event_id == WIFI_EVENT_STA_START)
    esp_wifi_connect();
  else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
    esp_wifi_connect();
  } else if (event_id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t *e = (ip_event_got_ip_t *)event_data;
    ESP_LOGI(TAG_WEB, "got ip:" IPSTR, IP2STR(&e->ip_info.ip));
    if (server == NULL) server = start_webserver();
  }
}

void start_mdns_service() {
  // initialize mDNS service
  esp_err_t err = mdns_init();
  if (err) {
    ESP_LOGE(TAG_WEB, "MDNS Init failed: %d", err);
    return;
  }

  // set hostname
  mdns_hostname_set(MDNS_HOST_NAME);
  // set default instance
  mdns_instance_name_set("USB based amp control");
  ESP_LOGI(TAG_WEB, "MDNS started, address: %s.local", MDNS_HOST_NAME);
}

void web_server_task(void *arg) {
  ab_test_mutex = xSemaphoreCreateMutexStatic(&ab_test_mutex_buffer);
  memset(&ab_test_state, 0, sizeof(ab_test_state_t));

  ESP_ERROR_CHECK(nvs_flash_init());
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_create_default_wifi_sta();
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                             &wifi_event_handler, NULL));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                             &wifi_event_handler, NULL));
  wifi_config_t wifi_config = {};
  strcpy((char *)wifi_config.sta.ssid, WIFI_SSID);
  strcpy((char *)wifi_config.sta.password, WIFI_PASS);
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
  ESP_ERROR_CHECK(esp_wifi_start());
  start_mdns_service();
  vTaskDelete(NULL);
}
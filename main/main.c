#include <assert.h>

#include "usb_driver.h"
#include "web_server.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usb/usb_host.h"

#define USB_LIB_TASK_PRIORITY 2
#define CLASS_TASK_PRIORITY 3
#define TRIGGER_TASK_PRIORITY 3
#define WEB_SERVER_TASK_PRIORITY 4

// *** IO PIN CONFIGURATION ***
#define TRIGGER_PIN_PRESET_1 GPIO_NUM_4
#define TRIGGER_PIN_PRESET_2 GPIO_NUM_5
#define TRIGGER_PIN_PRESET_3 GPIO_NUM_6
#define RELAY_PIN GPIO_NUM_14

static const char *TAG = "TRIGGER_TASK";

static bool is_amp_powered_on(void) { return (bool)gpio_get_level(RELAY_PIN); }

static void turn_on_relay(void) { gpio_set_level(RELAY_PIN, 1); }
static void turn_off_relay(void) { gpio_set_level(RELAY_PIN, 0); }

static void trigger_monitor_task(void *arg) {
  ESP_LOGI(TAG, "Trigger-Monitor-Task started.");

  const uint64_t trigger_pin_mask = (1ULL << TRIGGER_PIN_PRESET_1) |
                                    (1ULL << TRIGGER_PIN_PRESET_2) |
                                    (1ULL << TRIGGER_PIN_PRESET_3);
  gpio_config_t trigger_io_conf = {
      .pin_bit_mask = trigger_pin_mask,
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  gpio_config(&trigger_io_conf);

  gpio_config_t relay_io_conf = {
      .pin_bit_mask = (1ULL << RELAY_PIN),
      .mode = GPIO_MODE_INPUT_OUTPUT,
  };
  gpio_config(&relay_io_conf);
  turn_off_relay();

  // TODO: reset to actual state if we're not on ab compare mode
  uint8_t current_preset = 0;
  TickType_t no_trigger_start_time = 0;
  TickType_t last_power_off_time = -10001;
  const TickType_t POWER_OFF_COOLDOWN_TICKS = pdMS_TO_TICKS(10000);
  const TickType_t POWER_OFF_DELAY_TICKS = pdMS_TO_TICKS(10000);

  while (1) {
    vTaskDelay(pdMS_TO_TICKS(100));

    int8_t new_preset = 0;
    if (!gpio_get_level(TRIGGER_PIN_PRESET_1))
      new_preset = 1;
    else if (!gpio_get_level(TRIGGER_PIN_PRESET_2))
      new_preset = 2;
    else if (!gpio_get_level(TRIGGER_PIN_PRESET_3))
      new_preset = 3;

    if (new_preset == current_preset) {
      no_trigger_start_time = 0;
      continue;
    }

    if (new_preset == 0) {
      // Shutdown
      if (!is_amp_powered_on()) {
        ESP_LOGW(TAG, "Already off fix internal state");
        current_preset = 0;
        continue;
      }
      if (no_trigger_start_time == 0) {
        ESP_LOGI(TAG, "No trigger present. Starting power off sequence.");
        no_trigger_start_time = xTaskGetTickCount();
        continue;
      }
      if ((xTaskGetTickCount() - no_trigger_start_time) >
          POWER_OFF_DELAY_TICKS) {
        ESP_LOGI(TAG, "Turning off amp");
        turn_off_relay();
        current_preset = 0;
        last_power_off_time = xTaskGetTickCount();
        no_trigger_start_time = 0;
      }
      continue;
    } else {
      if (!is_amp_powered_on()) {
        // Turn on amp first
        TickType_t time_since_last_off =
            xTaskGetTickCount() - last_power_off_time;
        if (time_since_last_off > POWER_OFF_COOLDOWN_TICKS) {
          ESP_LOGI(TAG, "Turning on AMP. Trigger %d active.", new_preset);
          turn_on_relay();

          ESP_LOGI(TAG, "Wait for USB-Connection...");
          int wait_cycles = 100;
          while (!is_device_connected() && wait_cycles > 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            wait_cycles--;
          }

          // Check if connected or timeout occured.
          if (is_device_connected()) {
            ESP_LOGI(TAG, "AMP connected.");
          } else {
            ESP_LOGE(TAG, "AMP not detected via USB!");
            // continue;
          }
        } else {
          ESP_LOGW(TAG, "Turn on not allowed still in cooldown.");
          continue;
        }
      }
      ESP_LOGI(TAG, "Set Preset %d.", new_preset);
      control_action_t cmd = {.action = ACTION_SET_PRESET,
                              .value = new_preset};
      enqueue_command(cmd);
      current_preset = new_preset;
    }
  }
}

// TODO move to usb lib
static void usb_host_lib_task(void *arg) {
  SemaphoreHandle_t installed = (SemaphoreHandle_t)arg;
  ESP_LOGI("USB_HOST_TASK", "Installing USB Host Library");
  usb_host_config_t host_config = {.skip_phy_setup = false,
                                   .intr_flags = ESP_INTR_FLAG_LEVEL1};
  ESP_ERROR_CHECK(usb_host_install(&host_config));

  // Signal that the host library is installed
  xSemaphoreGive(installed);
  vTaskDelay(10);  // Short delay to let client task spin up

  while (1) {
    uint32_t event_flags;
    ESP_ERROR_CHECK(usb_host_lib_handle_events(portMAX_DELAY, &event_flags));
    if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
      ESP_LOGI(TAG, "No more clients");
    }
    if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
      ESP_LOGI(TAG, "No more devices");
    }
  }
}

void app_main(void) {
  ESP_LOGI("APP_MAIN", "Starting app main...");
  SemaphoreHandle_t host_lib_installed = xSemaphoreCreateBinary();
  SemaphoreHandle_t hypex_state_updated = xSemaphoreCreateBinary();

  TaskHandle_t host_lib_task_hdl, class_driver_task_hdl, trigger_task_hdl,
      web_server_task_hdl;
  BaseType_t task_created;

  // Create host lib task
  task_created = xTaskCreatePinnedToCore(
      usb_host_lib_task, "usb_host", 4096, (void *)host_lib_installed,
      USB_LIB_TASK_PRIORITY, &host_lib_task_hdl, 1);
  assert(task_created == pdTRUE);
  xSemaphoreTake(host_lib_installed, portMAX_DELAY);
  // Create client task
  task_created = xTaskCreatePinnedToCore(
      usb_driver_task, "driver", 4096, (void *)hypex_state_updated,
      CLASS_TASK_PRIORITY, &class_driver_task_hdl, 1);
  assert(task_created == pdTRUE);
  // Create trigger monitor task
  task_created = xTaskCreatePinnedToCore(
      trigger_monitor_task, "trigger_monitor", 4096, NULL,
      TRIGGER_TASK_PRIORITY, &trigger_task_hdl, 0);
  assert(task_created == pdTRUE);
  // Create web server task
  task_created = xTaskCreatePinnedToCore(web_server_task, "web_server_task",
                                         4096, NULL, WEB_SERVER_TASK_PRIORITY,
                                         &web_server_task_hdl, 0);
  assert(task_created == pdTRUE);
  while (1) {
    if (xSemaphoreTake(hypex_state_updated, portMAX_DELAY) == pdTRUE) {
      ESP_LOGI(TAG, "New data inform web server");
      state_t current_state;
      get_state(&current_state);
      // TODO should I use a queue here?
      notify_state_changed(&current_state);
    }
  }
}

#include "usb_driver.h"

#include <stdbool.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usb/usb_host.h"

// #define TEST_MODE

// Uncomment if volume should not be reset upon preset change.
#define PRESET_CHANGE_RESET_VOLUME_DB -3.0f

#define MAX_VOLUME 18
#define MIN_VOLUME -99

#define DEFAULT_MUTE_STATE 0  // 1 for MUTE

#define CLIENT_NUM_EVENT_MSG 5
#define PACKET_SIZE 64

#define COMMAND_QUEUE_LENGTH 10

static const char *TAG = "CLASS-DRIVER";
static SemaphoreHandle_t hypex_state_updated;

// Transfer semaphore
static StaticSemaphore_t usb_out_transfer_sem_buffer;
static SemaphoreHandle_t usb_out_transfer_sem;
static StaticSemaphore_t poll_callback_sem_buffer;
static SemaphoreHandle_t poll_callback_pending;

// Data new structure
static SemaphoreHandle_t state_cache_mutex;
static StaticSemaphore_t state_cache_mutex_buffer;
static uint8_t state_cache[PACKET_SIZE] = {0x00};
static SemaphoreHandle_t filter_name_mutex;
static StaticSemaphore_t filter_name_mutex_buffer;
static char filter_name[FILTER_NAME_MAX_LEN];

#define ACTION_OPEN_DEV (1 << 0)
#define ACTION_TRANSFER (1 << 1)
#define ACTION_CLOSE_DEV (1 << 2)
#define ACTION_POLL (1 << 3)
#define ACTION_GET_STATE (1 << 4)
#define ACTION_GET_FILTER_NAME (1 << 5)

typedef struct {
  uint32_t actions;
  uint8_t dev_addr;
  usb_host_client_handle_t client_hdl;
  usb_device_handle_t dev_hdl;
  usb_transfer_t *out_transfer;
  usb_transfer_t *in_transfer;
} class_driver_t;

static const char *TAG_DRIVER = "DRIVER";
static volatile bool device_is_connected = false;

static StaticQueue_t command_queue_buffer;
uint8_t ucQueueStorageArea[COMMAND_QUEUE_LENGTH * sizeof(control_action_t)];
QueueHandle_t command_queue;

bool is_device_connected(void) { return device_is_connected; }

static void cache_hypex_state_buffer(uint8_t *data) {
  ESP_LOGI(TAG_DRIVER, "********** Received state data **********");
  ESP_LOG_BUFFER_HEX(TAG_DRIVER, data, PACKET_SIZE);
  bool state_changed = false;
  if (xSemaphoreTake(state_cache_mutex, portMAX_DELAY) == pdTRUE) {
    if (memcmp(data, state_cache, 64) != 0) {
      state_changed = true;
      memcpy(state_cache, data, 64);
    }
    xSemaphoreGive(state_cache_mutex);
  }
  if (state_changed) {
    xSemaphoreGive(hypex_state_updated);
  }
}

void read_hypex_state_buffer(uint8_t *data) {
  // Set packages for the current settings are only first 32 bytes.
  // FYI: DIM state of display is not in the first 32!
  memset(data, 0x00, PACKET_SIZE);
  if (xSemaphoreTake(state_cache_mutex, portMAX_DELAY) == pdTRUE) {
    memcpy(data, state_cache, 32);
    xSemaphoreGive(state_cache_mutex);
  }
  // The amp is responding the current source here but if we set it here the
  // command is rejected.
  data[1] = 0x00;
  // Always 0x00 in request different in response ¯\_(ツ)_/¯
  data[5] = 0x00;
  data[23] = 0x00;
  data[26] = 0x00;
}

void get_state(state_t *state) {
  if (xSemaphoreTake(state_cache_mutex, portMAX_DELAY) == pdTRUE) {
    state->preset = state_cache[2];
    int16_t v = (state_cache[4] << 8) | state_cache[3];
    state->volume_db = (float)v / 100.0f;
    state->is_muted = (state_cache[6] & 0x80) ? true : false;
    state->current_source = (input_source_t)(state_cache[50]);
    state->preset_source[0] = (input_source_t)(state_cache[12] & 0x0F);
    state->preset_source[1] = (input_source_t)(state_cache[13] & 0x0F);
    state->preset_source[2] = (input_source_t)(state_cache[14] & 0x0F);
    state->is_eq_on[0] = (state_cache[12] & 0x10) ? true : false;
    state->is_eq_on[1] = (state_cache[13] & 0x10) ? true : false;
    state->is_eq_on[2] = (state_cache[14] & 0x10) ? true : false;
    xSemaphoreGive(state_cache_mutex);
  }
}

void get_filter_name(char *name) {
  xSemaphoreTake(filter_name_mutex, portMAX_DELAY);
  strcpy(name, filter_name);
  xSemaphoreGive(filter_name_mutex);
}

static void cache_filter_name(const uint8_t *data) {
  xSemaphoreTake(filter_name_mutex, portMAX_DELAY);
  strncpy(filter_name, (const char *)&data[2], FILTER_NAME_MAX_LEN - 1);
  filter_name[FILTER_NAME_MAX_LEN - 1] = '\0';
  xSemaphoreGive(filter_name_mutex);
}

static void clear_caches(void) {
  xSemaphoreTake(state_cache_mutex, portMAX_DELAY);
  memset(state_cache, 0x00, PACKET_SIZE);
  xSemaphoreGive(state_cache_mutex);
  xSemaphoreTake(filter_name_mutex, portMAX_DELAY);
  memset(filter_name, 0x00, PACKET_SIZE);
  xSemaphoreGive(filter_name_mutex);
}

static void set_volume_in_packet(uint8_t *paket, int8_t db_value) {
  int16_t volume_value = (int16_t)(db_value * 100.0f);
  paket[3] = volume_value & 0xFF;
  paket[4] = (volume_value >> 8) & 0xFF;
}

static void in_transfer_callback(usb_transfer_t *transfer) {
  ESP_LOGI(TAG_DRIVER, "Received IN transfer callback");
  if (transfer->status == USB_TRANSFER_STATUS_COMPLETED) {
    if (transfer->actual_num_bytes > 0) {
      if (transfer->data_buffer[0] == 0x05) {
        ESP_LOGI(TAG_DRIVER, "Received state data.");
        cache_hypex_state_buffer(transfer->data_buffer);
      } else if (transfer->data_buffer[0] == 0x03) {
        ESP_LOGI(TAG_DRIVER, "Received filter name data.");

        cache_filter_name(transfer->data_buffer);
      } else {
        ESP_LOGI(TAG_DRIVER, "Unkown data package.");
        ESP_LOG_BUFFER_HEX(TAG_DRIVER, transfer->data_buffer,
                           transfer->actual_num_bytes);
      }
    }
  } else {
    ESP_LOGW(TAG_DRIVER, "Command error status: %d.", transfer->status);
  }
  xSemaphoreGive(poll_callback_pending);
}

static void out_transfer_callback(usb_transfer_t *transfer) {
  ESP_LOGI(TAG_DRIVER, "Received OUT transfer callback");
  if (transfer->status == USB_TRANSFER_STATUS_COMPLETED) {
    ESP_LOGI(TAG_DRIVER,
             "Ack for sending (%d bytes):", transfer->actual_num_bytes);
  } else {
    ESP_LOGW(TAG_DRIVER, "Command error status: %d.", transfer->status);
  }
  xSemaphoreGive(usb_out_transfer_sem);
}

static esp_err_t send_single_command(class_driver_t *driver_obj) {
  ESP_LOGI(TAG_DRIVER, "Sending data:");
  ESP_LOG_BUFFER_HEX(TAG_DRIVER, driver_obj->out_transfer->data_buffer,
                     PACKET_SIZE);
#ifdef TEST_MODE
  cache_hypex_state_buffer(driver_obj->out_transfer->data_buffer);
#endif  // TEST_MODE

  if (!device_is_connected) {  // TODO remove as I now have the
                               // class_driver_t *driver_obj?
    ESP_LOGE(TAG_DRIVER, "Not connected.");
    // TODO have own error class?
    return ESP_ERR_NOT_FOUND;
  }

  esp_err_t err = usb_host_transfer_submit(driver_obj->out_transfer);
  if (err != ESP_OK) {
    ESP_LOGE(TAG_DRIVER, "Transfer failed.");
    return err;
  }

  // TODO maybe don't sleep?
  // Sleep until transfer completed
  // if (xSemaphoreTake(usb_out_transfer_sem, portMAX_DELAY) != pdTRUE) {
  //   ESP_LOGE(TAG_DRIVER, "*** Transfer timed out:");
  //   ESP_LOG_BUFFER_HEX(TAG_DRIVER, hid_paket, paket_size);
  //   ESP_LOGE(TAG_DRIVER, " *** Transfer timed out");

  //   xSemaphoreGive(usb_out_transfer_sem);
  //   return ESP_ERR_TIMEOUT;
  // }
  return ESP_OK;
}

void enqueue_command(control_action_t command) {
  // Validation
  switch (command.action) {
    case ACTION_SET_PRESET:
      if (command.value < 1 || command.value > 3) {
        ESP_LOGE(TAG, "Invalid preset value %d. Must be between 1 and 3.",
                 command.value);
        return;
      }
      break;
    case ACTION_SET_VOLUME:
      if (command.value < MIN_VOLUME || command.value > MAX_VOLUME) {
        ESP_LOGE(TAG, "Invalid volume value %d. Must be between %d and %d.",
                 command.value, MIN_VOLUME, MAX_VOLUME);
        return;
      }
      break;
    case ACTION_SET_SOURCE_P1:
    case ACTION_SET_SOURCE_P2:
    case ACTION_SET_SOURCE_P3:
      // SOURCE_SCAN   = 0,
      // SOURCE_XLR    = 1,
      // SOURCE_RCA    = 2,
      // SOURCE_SPDIF  = 4,
      // SOURCE_AES    = 5,
      // SOURCE_OPT    = 6,
      // SOURCTE_EXT   = 7
      if (command.value < 0 || command.value > 7 || command.value == 3) {
        ESP_LOGE(TAG, "Invalid source value %d.", command.value);
        return;
      }
      break;
    case ACTION_SET_MUTE:
    case ACTION_SET_EQ_P1:
    case ACTION_SET_EQ_P2:
    case ACTION_SET_EQ_P3:
      // No validation needed
      break;
  }

  BaseType_t status = xQueueSend(command_queue, &command, portMAX_DELAY);
  if (status != pdPASS) {
    ESP_LOGE(TAG_DRIVER, "Failed to add command %d to the queue.",
             command.action);
  } else {
    ESP_LOGI(TAG_DRIVER, "Added command %d to the queue.", command.action);
  }
}

static void set_preset(class_driver_t *driver_obj, int8_t preset) {
  read_hypex_state_buffer(driver_obj->out_transfer->data_buffer);

  driver_obj->out_transfer->data_buffer[2] = preset;
#ifdef PRESET_CHANGE_RESET_VOLUME_DB
  set_volume_in_packet(driver_obj->out_transfer->data_buffer,
                       PRESET_CHANGE_RESET_VOLUME_DB);
#endif  // PRESET_CHANGE_RESET_VOLUME_DB
  send_single_command(driver_obj);
}

static void set_volume(class_driver_t *driver_obj, float volume_db) {
  read_hypex_state_buffer(driver_obj->out_transfer->data_buffer);

  set_volume_in_packet(driver_obj->out_transfer->data_buffer, volume_db);
  send_single_command(driver_obj);
}

static void set_mute(class_driver_t *driver_obj, bool mute) {
  read_hypex_state_buffer(driver_obj->out_transfer->data_buffer);

  // Mute bit is (Byte 6, Bit 7)
  uint8_t mask = (1 << 7);
  if (mute) {
    driver_obj->out_transfer->data_buffer[6] |= mask;
  } else {
    driver_obj->out_transfer->data_buffer[6] &= ~mask;
  }

  send_single_command(driver_obj);
}

static void set_source(class_driver_t *driver_obj, preset_t preset,
                       uint8_t preset_source) {
  assert(preset >= 1 && preset <= 3);
  read_hypex_state_buffer(driver_obj->out_transfer->data_buffer);
  driver_obj->out_transfer->data_buffer[11 + (uint8_t)preset] =
      (driver_obj->out_transfer->data_buffer[11 + (uint8_t)preset] & 0xF0) |
      (uint8_t)(preset_source & 0x0F);

  send_single_command(driver_obj);
}

static void set_eq(class_driver_t *driver_obj, preset_t preset, bool enable) {
  assert(preset >= 1 && preset <= 3);
  read_hypex_state_buffer(driver_obj->out_transfer->data_buffer);

  uint8_t mask = (1 << 4);

  if (enable) {
    driver_obj->out_transfer->data_buffer[11 + (uint8_t)preset] |= mask;
  } else {
    driver_obj->out_transfer->data_buffer[11 + (uint8_t)preset] &= ~mask;
  }

  send_single_command(driver_obj);
}

static void action_execute_command(class_driver_t *driver_obj) {
  control_action_t command;
  BaseType_t status = xQueueReceive(command_queue, &command, portMAX_DELAY);
  if (status != pdPASS) {
    ESP_LOGE(TAG_DRIVER, "Failed to get command from queue");
    return;
  }

  ESP_LOGI(TAG_DRIVER,
           "************** Executing command from queue **************");
  ESP_LOGI(TAG_DRIVER, "Command: %d with value: %d", command.action,
           command.value);

  switch (command.action) {
    case ACTION_SET_PRESET:
      set_preset(driver_obj, command.value);
      break;
    case ACTION_SET_VOLUME:
      set_volume(driver_obj, command.value);
      break;
    case ACTION_SET_SOURCE_P1:
      set_source(driver_obj, PRESET_1, command.value);
      break;
    case ACTION_SET_SOURCE_P2:
      set_source(driver_obj, PRESET_2, command.value);
      break;
    case ACTION_SET_SOURCE_P3:
      set_source(driver_obj, PRESET_3, command.value);
      break;
    case ACTION_SET_MUTE:
      set_mute(driver_obj, command.value);
      break;
    case ACTION_SET_EQ_P1:
      set_eq(driver_obj, PRESET_1, command.value);
      break;
    case ACTION_SET_EQ_P2:
      set_eq(driver_obj, PRESET_2, command.value);
      break;
    case ACTION_SET_EQ_P3:
      set_eq(driver_obj, PRESET_3, command.value);
      break;
  }
  ESP_LOGI(TAG_DRIVER,
           "************* Finished executing command from queue *************");
}

static void client_event_cb(const usb_host_client_event_msg_t *event_msg,
                            void *arg) {
  // This function is called from within usb_host_client_handle_events().
  // Do not block and try to keep it short
  class_driver_t *driver_obj = (class_driver_t *)arg;
  switch (event_msg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
      driver_obj->actions = ACTION_OPEN_DEV;
      driver_obj->dev_addr = event_msg->new_dev.address;
      break;
    case USB_HOST_CLIENT_EVENT_DEV_GONE:
      driver_obj->actions = ACTION_CLOSE_DEV;
      break;
    default:
      break;
  }
}

static void action_request_initial_state(class_driver_t *driver_obj) {
  ESP_LOGI(TAG, "Requesting initial state");
  memset(driver_obj->out_transfer->data_buffer, 0x00, PACKET_SIZE);
  driver_obj->out_transfer->data_buffer[0] = 0x06;
  driver_obj->out_transfer->data_buffer[1] = 0x02;
  send_single_command(driver_obj);
}

static void action_request_filter_name(class_driver_t *driver_obj) {
  memset(driver_obj->out_transfer->data_buffer, 0x00, PACKET_SIZE);
  driver_obj->out_transfer->data_buffer[0] = 0x03;
  driver_obj->out_transfer->data_buffer[1] = 0x08;
  send_single_command(driver_obj);
}

static void action_open_dev(class_driver_t *driver_obj) {
  ESP_LOGI(TAG, "Opening device at address %d", driver_obj->dev_addr);
  ESP_ERROR_CHECK(usb_host_device_open(
      driver_obj->client_hdl, driver_obj->dev_addr, &driver_obj->dev_hdl));
  ESP_ERROR_CHECK(usb_host_interface_claim(driver_obj->client_hdl,
                                           driver_obj->dev_hdl, 0, 0));
  driver_obj->in_transfer->device_handle = driver_obj->dev_hdl;
  driver_obj->out_transfer->device_handle = driver_obj->dev_hdl;
  device_is_connected = true;
}

static void action_close_dev(class_driver_t *driver_obj) {
  device_is_connected = false;
  // Remove pending commands
  xQueueReset(command_queue);
  // Close device
  ESP_LOGI(TAG, "Closing device at address %d", driver_obj->dev_addr);
  usb_host_interface_release(driver_obj->client_hdl, driver_obj->dev_hdl, 0);

  usb_host_device_close(driver_obj->client_hdl, driver_obj->dev_hdl);
  clear_caches();
  driver_obj->dev_hdl = NULL;
  driver_obj->dev_addr = 0;
  driver_obj->actions = 0;
  xSemaphoreGive(hypex_state_updated);
}

void usb_driver_task(void *arg) {
  ESP_LOGI(TAG_DRIVER, "  ************** Staring USB driver **************");
  // Initialize static structures
  command_queue =
      xQueueCreateStatic(COMMAND_QUEUE_LENGTH, sizeof(control_action_t),
                         ucQueueStorageArea, &command_queue_buffer);
  if (command_queue == 0) {
    ESP_LOGE(TAG_DRIVER, "Failed to create xQueue.");
  }
  state_cache_mutex = xSemaphoreCreateMutexStatic(&state_cache_mutex_buffer);
  filter_name_mutex = xSemaphoreCreateMutexStatic(&filter_name_mutex_buffer);
  usb_out_transfer_sem =
      xSemaphoreCreateBinaryStatic(&usb_out_transfer_sem_buffer);
  poll_callback_pending =
      xSemaphoreCreateBinaryStatic(&poll_callback_sem_buffer);

  // Negate the semaphore so we can ensure we only have one pending in
  // connection
  xSemaphoreGive(poll_callback_pending);

  hypex_state_updated = (SemaphoreHandle_t)arg;

  class_driver_t driver_obj = {0};

  // DO NOT SUBMIT testing only
  driver_obj.actions = ACTION_TRANSFER;

  usb_host_client_config_t client_config = {
      .is_synchronous = false,
      .max_num_event_msg = CLIENT_NUM_EVENT_MSG,
      .async =
          {
              .client_event_callback = client_event_cb,
              .callback_arg = (void *)&driver_obj,
          },
  };
  ESP_ERROR_CHECK(
      usb_host_client_register(&client_config, &driver_obj.client_hdl));
  // OUT transfer
  ESP_ERROR_CHECK(
      usb_host_transfer_alloc(PACKET_SIZE, 0, &driver_obj.out_transfer));
  driver_obj.out_transfer->bEndpointAddress = 0x01;  // OUT
  driver_obj.out_transfer->callback = out_transfer_callback;
  driver_obj.out_transfer->context = NULL;
  driver_obj.out_transfer->num_bytes = PACKET_SIZE;

  // IN transfer
  ESP_ERROR_CHECK(
      usb_host_transfer_alloc(PACKET_SIZE, 0, &driver_obj.in_transfer));
  driver_obj.in_transfer->bEndpointAddress = 0x81;  // IN
  driver_obj.in_transfer->callback = in_transfer_callback;
  driver_obj.in_transfer->context = NULL;
  driver_obj.in_transfer->num_bytes = PACKET_SIZE;

  while (1) {
    usb_host_client_handle_events(driver_obj.client_hdl, 100);

    // Only one action before polling
    if (driver_obj.actions & ACTION_OPEN_DEV) {
      action_open_dev(&driver_obj);
      driver_obj.actions = ACTION_GET_STATE | ACTION_POLL;
    } else if (driver_obj.actions & ACTION_CLOSE_DEV) {
      action_close_dev(&driver_obj);
      driver_obj.actions = 0;
      // break;
    } else if (driver_obj.actions & ACTION_GET_STATE) {
      action_request_initial_state(&driver_obj);
      driver_obj.actions = ACTION_GET_FILTER_NAME | ACTION_POLL;
    } else if (driver_obj.actions & ACTION_GET_FILTER_NAME) {
      action_request_filter_name(&driver_obj);
      driver_obj.actions = ACTION_TRANSFER | ACTION_POLL;
    } else if (driver_obj.actions & ACTION_TRANSFER &&
               uxQueueMessagesWaiting(command_queue) > 0) {
      ESP_LOGI(TAG, "Messages waiting %d",
               uxQueueMessagesWaiting(command_queue));

      action_execute_command(&driver_obj);
    }

    // Always poll if initalized and no pending poll
    if (driver_obj.actions & ACTION_POLL) {
      if (xSemaphoreTake(poll_callback_pending, 0) != pdTRUE) {
        continue;
      }
      if (usb_host_transfer_submit(driver_obj.in_transfer) != ESP_OK) {
        ESP_LOGE(TAG_DRIVER, "Polling failed.");
        xSemaphoreGive(poll_callback_pending);
      }
    }
  }
  usb_host_transfer_free(driver_obj.out_transfer);
  usb_host_transfer_free(driver_obj.in_transfer);
  usb_host_client_deregister(driver_obj.client_hdl);
}

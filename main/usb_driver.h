#ifndef USB_DRIVER_H
#define USB_DRIVER_H

#include <stdbool.h>
#include <stdint.h>

#include <esp_err.h>

#define FILTER_NAME_MAX_LEN 64

typedef enum {
  SOURCE_SCAN = 0,
  SOURCE_XLR = 1,
  SOURCE_RCA = 2,
  SOURCE_SPDIF = 4,
  SOURCE_AES = 5,
  SOURCE_OPT = 6,
  SOURCE_EXT = 7
} input_source_t;

typedef enum {
  PRESET_1 = 1,
  PRESET_2 = 2,
  PRESET_3 = 3
} preset_t;

typedef struct {
  preset_t preset;
  float volume_db;
  bool is_muted;
  input_source_t current_source;
  input_source_t preset_source[3];
  bool is_eq_on[3];
} state_t;

typedef enum {
  ACTION_SET_PRESET,
  ACTION_SET_VOLUME,
  ACTION_SET_SOURCE_P1,
  ACTION_SET_SOURCE_P2,
  ACTION_SET_SOURCE_P3,
  ACTION_SET_MUTE,
  ACTION_SET_EQ_P1,
  ACTION_SET_EQ_P2,
  ACTION_SET_EQ_P3,
} control_action_type_t;

typedef struct {
  control_action_type_t action;
  int8_t value;
} control_action_t;

void usb_driver_task(void *arg);

void enqueue_command(control_action_t command);

void get_state(state_t *state);
void get_filter_name(char *name);
bool is_device_connected(void);

#endif  // USB_DRIVER_H
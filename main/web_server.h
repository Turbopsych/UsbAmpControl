#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include "usb_driver.h"


void web_server_task(void *arg);
void notify_state_changed(const state_t *state);

#endif  // WEB_SERVER_H
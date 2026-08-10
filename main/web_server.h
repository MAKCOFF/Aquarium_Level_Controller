#pragma once

#include <stdbool.h>
#include "esp_http_server.h"

typedef enum { 
    STATE_IDLE,
    STATE_WAITING,
    STATE_PUMPING,
    STATE_ERROR,
    STATE_DISABLED,
    STATE_TURNING_OFF
} system_state_t;

typedef enum {
    VALVE_CLOSED,
    VALVE_OPENING,
    VALVE_OPEN,
    VALVE_CLOSING,
    VALVE_ERROR
} valve_state_t;

extern system_state_t current_state;
extern valve_state_t valve_state;
extern bool mosfet_fault;

void start_webserver(void);

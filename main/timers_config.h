#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"

// --- ДЕФОЛТНЫЕ ТАЙМИНГИ ---
#define DEFAULT_WAIT_MS           20000
#define DEFAULT_SAFETY_MS         30000
#define DEFAULT_STOP_MS           5000
#define DEFAULT_BUZZER_MS         60000
#define DEFAULT_VALVE_DELAY_MS    5000
#define DEFAULT_VALVE_TIMEOUT_MS  60000
#define DEFAULT_VALVE_CLOSE_DELAY_MS 10000
#define BUZZER_TOGGLE_MS          3000

// === СТРУКТУРА ДЛЯ ХРАНЕНИЯ ВСЕХ ТАЙМЕРОВ ===
typedef struct {
    int wait_ms;
    int safety_ms;
    int stop_ms;
    int buzzer_ms;
    int valve_delay_ms;
    int valve_timeout_ms;
    int valve_close_delay_ms;
} timer_config_t;

extern timer_config_t timers;

// Глобальные переменные насоса
extern int t_wait_ms;
extern int t_safety_ms;
extern int t_stop_ms;
extern int t_buzzer_ms;

// Глобальные переменные клапана
extern int t_valve_delay_ms;
extern int t_valve_timeout_ms;
extern int t_valve_close_delay_ms;

extern TimerHandle_t pump_timer;
extern TimerHandle_t safety_timer;
extern TimerHandle_t alarm_timer;
extern TimerHandle_t buzzer_toggle_timer;
extern TimerHandle_t stop_timer;
extern TimerHandle_t valve_delay_timer;
extern TimerHandle_t valve_timeout_timer;
extern TimerHandle_t valve_close_delay_timer;

typedef void (*timer_cb_t)(TimerHandle_t);

typedef struct {
    timer_cb_t pump_cb;
    timer_cb_t safety_cb;
    timer_cb_t alarm_cb;
    timer_cb_t buzzer_toggle_cb;
    timer_cb_t stop_cb;
    timer_cb_t valve_delay_cb;
    timer_cb_t valve_timeout_cb;
    timer_cb_t valve_close_delay_cb;
} timer_callbacks_t;

void save_timers_to_nvs(void);
void load_timers_from_nvs(void);
void restart_timers(const timer_callbacks_t *cbs);
void create_timers(const timer_callbacks_t *cbs);
void sync_timers_from_struct(void);
void sync_timers_to_struct(void);

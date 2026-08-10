#include <stdio.h>
#include <string.h>
#include "timers_config.h"
#include "nvs_flash.h"
#include "nvs.h"

// Глобальная структура с таймерами
timer_config_t timers;

// Глобальные переменные насоса
int t_wait_ms = DEFAULT_WAIT_MS;
int t_safety_ms = DEFAULT_SAFETY_MS;
int t_stop_ms = DEFAULT_STOP_MS;
int t_buzzer_ms = DEFAULT_BUZZER_MS;

// Глобальные переменные клапана
int t_valve_delay_ms = DEFAULT_VALVE_DELAY_MS;
int t_valve_timeout_ms = DEFAULT_VALVE_TIMEOUT_MS;
int t_valve_close_delay_ms = DEFAULT_VALVE_CLOSE_DELAY_MS;

TimerHandle_t pump_timer;
TimerHandle_t safety_timer;
TimerHandle_t alarm_timer;
TimerHandle_t stop_timer;
TimerHandle_t valve_delay_timer;
TimerHandle_t valve_timeout_timer;
TimerHandle_t valve_close_delay_timer;

static const timer_callbacks_t *active_cbs;

// === СИНХРОНИЗАЦИЯ МЕЖДУ СТРУКТУРОЙ И ГЛОБАЛЬНЫМИ ПЕРЕМЕННЫМИ ===
void sync_timers_from_struct(void) {
    t_wait_ms = timers.wait_ms;
    t_safety_ms = timers.safety_ms;
    t_stop_ms = timers.stop_ms;
    t_buzzer_ms = timers.buzzer_ms;
    t_valve_delay_ms = timers.valve_delay_ms;
    t_valve_timeout_ms = timers.valve_timeout_ms;
    t_valve_close_delay_ms = timers.valve_close_delay_ms;
}

void sync_timers_to_struct(void) {
    timers.wait_ms = t_wait_ms;
    timers.safety_ms = t_safety_ms;
    timers.stop_ms = t_stop_ms;
    timers.buzzer_ms = t_buzzer_ms;
    timers.valve_delay_ms = t_valve_delay_ms;
    timers.valve_timeout_ms = t_valve_timeout_ms;
    timers.valve_close_delay_ms = t_valve_close_delay_ms;
}

// === NVS (СОХРАНЕНИЕ ЧЕРЕЗ BLOB) ===
void save_timers_to_nvs(void) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &nvs_handle);
    if (err == ESP_OK) {
        sync_timers_to_struct();
        
        err = nvs_set_blob(nvs_handle, "timers_blob", &timers, sizeof(timer_config_t));
        if (err == ESP_OK) {
            printf("[NVS] Timers saved (%d bytes): wait=%d, safety=%d, stop=%d, buzzer=%d, valve_delay=%d, valve_timeout=%d, valve_close=%d\n",
                   sizeof(timer_config_t), timers.wait_ms, timers.safety_ms, timers.stop_ms, timers.buzzer_ms,
                   timers.valve_delay_ms, timers.valve_timeout_ms, timers.valve_close_delay_ms);
        } else {
            printf("[NVS] Failed to save timers blob, err=%d\n", err);
        }
        
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    } else {
        printf("[NVS] Failed to open namespace for saving, err=%d\n", err);
    }
}

void load_timers_from_nvs(void) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &nvs_handle);
    
    // Установка дефолтных значений
    timers.wait_ms = DEFAULT_WAIT_MS;
    timers.safety_ms = DEFAULT_SAFETY_MS;
    timers.stop_ms = DEFAULT_STOP_MS;
    timers.buzzer_ms = DEFAULT_BUZZER_MS;
    timers.valve_delay_ms = DEFAULT_VALVE_DELAY_MS;
    timers.valve_timeout_ms = DEFAULT_VALVE_TIMEOUT_MS;
    timers.valve_close_delay_ms = DEFAULT_VALVE_CLOSE_DELAY_MS;
    
    if (err == ESP_OK) {
        size_t size = sizeof(timer_config_t);
        err = nvs_get_blob(nvs_handle, "timers_blob", &timers, &size);
        
        if (err == ESP_OK && size == sizeof(timer_config_t)) {
            printf("[NVS] Timers loaded (%d bytes): wait=%d, safety=%d, stop=%d, buzzer=%d, valve_delay=%d, valve_timeout=%d, valve_close=%d\n",
                   size, timers.wait_ms, timers.safety_ms, timers.stop_ms, timers.buzzer_ms,
                   timers.valve_delay_ms, timers.valve_timeout_ms, timers.valve_close_delay_ms);
        } else {
            printf("[NVS] No saved timers blob (err=%d), using defaults\n", err);
        }
        nvs_close(nvs_handle);
    } else {
        printf("[NVS] Failed to open namespace for loading (err=%d), using defaults\n", err);
    }
    
    sync_timers_from_struct();
}

void create_timers(const timer_callbacks_t *cbs) {
    active_cbs = cbs;
    
    pump_timer   = xTimerCreate("WaitT", pdMS_TO_TICKS(t_wait_ms),   pdFALSE, 0, cbs->pump_cb);
    safety_timer = xTimerCreate("SafeT", pdMS_TO_TICKS(t_safety_ms), pdFALSE, 0, cbs->safety_cb);
    stop_timer   = xTimerCreate("OT", pdMS_TO_TICKS(t_stop_ms),      pdFALSE, 0, cbs->stop_cb);
    alarm_timer  = xTimerCreate("AlrmT", pdMS_TO_TICKS(t_buzzer_ms), pdFALSE, 0, cbs->alarm_cb);
    valve_delay_timer = xTimerCreate("VlvDly", pdMS_TO_TICKS(t_valve_delay_ms), pdFALSE, 0, cbs->valve_delay_cb);
    valve_timeout_timer = xTimerCreate("VlvTout", pdMS_TO_TICKS(t_valve_timeout_ms), pdFALSE, 0, cbs->valve_timeout_cb);
    valve_close_delay_timer = xTimerCreate("VlvCls", pdMS_TO_TICKS(t_valve_close_delay_ms), pdFALSE, 0, cbs->valve_close_delay_cb);
    
    if (!pump_timer || !safety_timer || !stop_timer || !alarm_timer ||
        !valve_delay_timer || !valve_timeout_timer || !valve_close_delay_timer) {
        printf("[ERROR] Failed to create timers!\n");
    } else {
        printf("[TIMERS] All timers created successfully\n");
    }
}

void restart_timers(const timer_callbacks_t *cbs) {
    if (pump_timer) xTimerDelete(pump_timer, 0);
    if (safety_timer) xTimerDelete(safety_timer, 0);
    if (stop_timer) xTimerDelete(stop_timer, 0);
    if (alarm_timer) xTimerDelete(alarm_timer, 0);
    if (valve_delay_timer) xTimerDelete(valve_delay_timer, 0);
    if (valve_timeout_timer) xTimerDelete(valve_timeout_timer, 0);
    if (valve_close_delay_timer) xTimerDelete(valve_close_delay_timer, 0);
    
    create_timers(cbs);
    
    printf("[TIMERS] Restarted with: wait=%d, safety=%d, stop=%d, buzzer=%d, valve_delay=%d, valve_timeout=%d, valve_close=%d\n",
           t_wait_ms, t_safety_ms, t_stop_ms, t_buzzer_ms,
           t_valve_delay_ms, t_valve_timeout_ms, t_valve_close_delay_ms);
}

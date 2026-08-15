//#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "driver/gpio.h"
#include "nvs_flash.h"
#include "esp_mac.h"
#include "timers_config.h"
#include "web_server.h"
#include "wifi_init.h"

// --- ПИНЫ ---
#define PIN_PUMP          25   // Основной насос
#define PIN_VALVE         33   // Клапан подпитки
#define PIN_SENS_1        4    // Верхний уровень (дисплей)
#define PIN_SENS_2        5    // Нижний уровень (самп)
#define PIN_VALVE_SENS    26   // Датчик уровня для клапана
#define PIN_BUZZER        32
#define PIN_RESET_BUTTON  16
#define PIN_SENS_ALARM    27

// --- СВЕТОДИОДЫ (активный LOW - управление минусом) ---
#define PIN_LED_SENS_1       17   // Зеленый: статус датчика SENS_1 (пин 4)
#define PIN_LED_SENS_2       18   // Зеленый: статус датчика SENS_2 (пин 5)
#define PIN_LED_VALVE_ERROR  19   // Красный: ошибка клапана (VALVE_ERROR)
#define PIN_LED_PUMP_ERROR   21   // Красный: ошибка насоса (STATE_ERROR)

// --- ЗАЩИТА ОТ ПРОБИТОГО МОСФЕТА ---
#define PIN_MOSFET_PUMP      13   // Факт открытия мосфета насоса (LOW = открыт)
#define PIN_MOSFET_VALVE     14   // Факт открытия мосфета клапана (LOW = открыт)
#define PIN_RELAY_12V        23   // Реле 12В (LOW = включено, HIGH = отключено)

// === УПРАВЛЕНИЕ ЗУММЕРОМ (прерывистый сигнал 3с/3с) ===
static bool buzzer_active = false;
static bool buzzer_level = false;

static void buzzer_toggle_callback(TimerHandle_t xTimer) {
    if (buzzer_active) {
        buzzer_level = !buzzer_level;
        gpio_set_level(PIN_BUZZER, buzzer_level ? 1 : 0);
    }
}

void alarm_buzzer_on(void) {
    buzzer_active = true;
    buzzer_level = true;
    gpio_set_level(PIN_BUZZER, 1);
    xTimerStart(alarm_timer, 0);
    xTimerStart(buzzer_toggle_timer, 0);
}

void alarm_buzzer_off(void) {
    buzzer_active = false;
    buzzer_level = false;
    gpio_set_level(PIN_BUZZER, 0);
    xTimerStop(alarm_timer, 0);
    xTimerStop(buzzer_toggle_timer, 0);
}

// === КОЛЛБЭКИ НАСОСА ===
static void pump_timer_callback(TimerHandle_t xTimer) {
    printf("[PUMP] Start delay expired. Starting pump.\n");
    if (current_state == STATE_WAITING) {
        current_state = STATE_PUMPING;
        gpio_set_level(PIN_PUMP, 1);
        xTimerStart(safety_timer, 0);
    }
}

static void safety_timeout_callback(TimerHandle_t xTimer) {
    printf("[PUMP] ALARM: Pump ran %d ms without result!\n", t_safety_ms);
    gpio_set_level(PIN_PUMP, 0);
    current_state = STATE_ERROR;
    alarm_buzzer_on();
}

static void stop_alarm_callback(TimerHandle_t xTimer) {
    alarm_buzzer_off();
    printf("[BUZZER] Turned off.\n");
}

static void stop_timer_callback(TimerHandle_t xTimer) {
    printf("[PUMP] Stop delay (%d ms) completed - turning off pump\n", t_stop_ms);
    gpio_set_level(PIN_PUMP, 0);
    if (current_state == STATE_TURNING_OFF) {
        current_state = STATE_IDLE;
    }
}

// === КОЛЛБЭКИ КЛАПАНА ===
static void valve_delay_callback(TimerHandle_t xTimer) {
    printf("[VALVE] Delay expired. Opening valve.\n");
    if (valve_state == VALVE_OPENING) {
        valve_state = VALVE_OPEN;
        gpio_set_level(PIN_VALVE, 1);
        xTimerStart(valve_timeout_timer, 0);
    }
}

static void valve_timeout_callback(TimerHandle_t xTimer) {
    printf("[VALVE] TIMEOUT! Valve open for %d ms without filling! Valve ERROR.\n", t_valve_timeout_ms);
    gpio_set_level(PIN_VALVE, 0);
    valve_state = VALVE_ERROR;
    alarm_buzzer_on();
}

static void valve_close_delay_callback(TimerHandle_t xTimer) {
    printf("[VALVE] Close delay expired. Closing valve.\n");
    if (valve_state == VALVE_CLOSING) {
        gpio_set_level(PIN_VALVE, 0);
        valve_state = VALVE_CLOSED;
        printf("[VALVE] Valve closed.\n");
    }
}

// Структура коллбэков для передачи в timers_config
timer_callbacks_t main_timer_callbacks = {
    .pump_cb = pump_timer_callback,
    .safety_cb = safety_timeout_callback,
    .alarm_cb = stop_alarm_callback,
    .buzzer_toggle_cb = buzzer_toggle_callback,
    .stop_cb = stop_timer_callback,
    .valve_delay_cb = valve_delay_callback,
    .valve_timeout_cb = valve_timeout_callback,
    .valve_close_delay_cb = valve_close_delay_callback,
};

// === ПЕРЕМЕННЫЕ ЗАЩИТЫ МОСФЕТОВ ===
bool mosfet_fault = false;  // Флаг: обнаружен пробитый мосфет

void app_main(void) {
    // ============================================
    // 1. ИНИЦИАЛИЗАЦИЯ NVS
    // ============================================
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    // ============================================
    // 2. ЗАГРУЗКА ТАЙМЕРОВ ИЗ NVS
    // ============================================
    load_timers_from_nvs();
    
    // ============================================
    // 3. ВЫВОД ЗАГРУЖЕННЫХ ЗНАЧЕНИЙ
    // ============================================
    printf("\n========== FINAL TIMER VALUES ==========\n");
    printf("PUMP: wait=%d, safety=%d, stop=%d, buzzer=%d\n", 
           t_wait_ms, t_safety_ms, t_stop_ms, t_buzzer_ms);
    printf("VALVE: delay=%d, timeout=%d, close_delay=%d\n", 
           t_valve_delay_ms, t_valve_timeout_ms, t_valve_close_delay_ms);
    printf("========================================\n\n");
    
    // ============================================
    // 4. WI-FI ИНИЦИАЛИЗАЦИЯ
    // ============================================
    wifi_init();
    
    // ============================================
    // 5. НАСТРОЙКА GPIO
    // ============================================
    // Силовые выходы (насос, клапан, зуммер)
    gpio_config_t out = {
        .mode = GPIO_MODE_OUTPUT, 
        .pin_bit_mask = (1ULL<<PIN_PUMP) | (1ULL<<PIN_VALVE) | (1ULL<<PIN_BUZZER),
        .pull_down_en = 1
    };
    gpio_config(&out);
    
    // Светодиоды (управление минусом, с подтяжкой к питанию)
    gpio_config_t out_led = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL<<PIN_LED_SENS_1) | (1ULL<<PIN_LED_SENS_2) |
                        (1ULL<<PIN_LED_VALVE_ERROR) | (1ULL<<PIN_LED_PUMP_ERROR),
        .pull_up_en = 1,
        .pull_down_en = 0
    };
    gpio_config(&out_led);
    
    // Инициализация светодиодов (выключены = HIGH)
    gpio_set_level(PIN_LED_SENS_1, 1);
    gpio_set_level(PIN_LED_SENS_2, 1);
    gpio_set_level(PIN_LED_VALVE_ERROR, 1);
    gpio_set_level(PIN_LED_PUMP_ERROR, 1);
    
    gpio_config_t in_pd = {.mode = GPIO_MODE_INPUT, .pin_bit_mask = (1ULL<<PIN_SENS_1)|(1ULL<<PIN_SENS_2), .pull_down_en=1, .pull_up_en=0};
    gpio_config(&in_pd);
    
    gpio_config_t in_pu = {.mode = GPIO_MODE_INPUT, .pin_bit_mask = (1ULL<<PIN_SENS_ALARM)|(1ULL<<PIN_RESET_BUTTON)|(1ULL<<PIN_VALVE_SENS), .pull_up_en=1, .pull_down_en=0};
    gpio_config(&in_pu);
    
    // Входы мосфетов (LOW = открыт, с подтяжкой вверх)
    gpio_config_t in_mosfet = {.mode = GPIO_MODE_INPUT, .pin_bit_mask = (1ULL<<PIN_MOSFET_PUMP)|(1ULL<<PIN_MOSFET_VALVE), .pull_up_en=1, .pull_down_en=0};
    gpio_config(&in_mosfet);
    
    // Реле 12В (LOW = включено, по умолчанию включено)
    gpio_config_t out_relay = {.mode = GPIO_MODE_OUTPUT, .pin_bit_mask = (1ULL<<PIN_RELAY_12V), .pull_up_en=1, .pull_down_en=0};
    gpio_config(&out_relay);
    gpio_set_level(PIN_RELAY_12V, 0);  // Включаем реле (LOW = включено)
    
    // ============================================
    // 6. СОЗДАНИЕ ТАЙМЕРОВ (ПОСЛЕ ЗАГРУЗКИ NVS!)
    // ============================================
    create_timers(&main_timer_callbacks);
    
    // ============================================
    // 7. ВЫВОД MAC АДРЕСА
    // ============================================
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    printf("\n========================================\n");
    printf("[MAC] ESP32 MAC: %02X:%02X:%02X:%02X:%02X:%02X\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    printf("========================================\n\n");
    
    // ============================================
    // 8. ОСНОВНОЙ ЦИКЛ
    // ============================================
    int last_s1 = -1, last_s2 = -1, last_valve_sens = -1, last_alarm = -1;
    int s1 = 0;
    int s2 = 0;
    int valve_sens = 0;
    int alarm = 0;
    int btn_raw = 0;
    bool water_normal = true;
    bool overflow_alarm = false;
    bool tank_empty = false;
    
    uint32_t last_button_press = 0;
    
    // Переменные для проверки мосфетов
    int mosfet_pump_raw = 0;
    int mosfet_valve_raw = 0;
    
    while (1) {
        s1 = !gpio_get_level(PIN_SENS_1);
        s2 = !gpio_get_level(PIN_SENS_2);
        valve_sens = gpio_get_level(PIN_VALVE_SENS);
        alarm = gpio_get_level(PIN_SENS_ALARM);
        btn_raw = gpio_get_level(PIN_RESET_BUTTON);
        
        // Чтение датчиков мосфетов (LOW = открыт)
        mosfet_pump_raw = gpio_get_level(PIN_MOSFET_PUMP);
        mosfet_valve_raw = gpio_get_level(PIN_MOSFET_VALVE);
        
        water_normal = (s1 == 1 || s2 == 1);
        overflow_alarm = (alarm == 1);
        tank_empty = (valve_sens == 0);
        
        // === ОБНОВЛЕНИЕ СВЕТОДИОДОВ (активный LOW) ===
        gpio_set_level(PIN_LED_SENS_1, !s1);
        gpio_set_level(PIN_LED_SENS_2, !s2);
        gpio_set_level(PIN_LED_VALVE_ERROR, !(valve_state == VALVE_ERROR || mosfet_fault));
        gpio_set_level(PIN_LED_PUMP_ERROR, !(current_state == STATE_ERROR || mosfet_fault));
        
        // === ПРОВЕРКА МОСФЕТОВ (защита от пробоя) ===
        // Если мосфет LOW (открыт) а команда на насос/клапан не подана (HIGH) — пробит
        if (!mosfet_fault) {
            bool pump_mosfet_bad = (mosfet_pump_raw == 0 && gpio_get_level(PIN_PUMP) == 0);
            bool valve_mosfet_bad = (mosfet_valve_raw == 0 && gpio_get_level(PIN_VALVE) == 0);
            
            if (pump_mosfet_bad || valve_mosfet_bad) {
                printf("[MOSFET] FAULT! PumpMosfet=%d ValveMosfet=%d (LOW=open) | PumpCmd=%d ValveCmd=%d\n",
                       mosfet_pump_raw, mosfet_valve_raw,
                       gpio_get_level(PIN_PUMP), gpio_get_level(PIN_VALVE));
                
                mosfet_fault = true;
                
                // Отключаем всё
                gpio_set_level(PIN_PUMP, 0);
                gpio_set_level(PIN_VALVE, 0);
                gpio_set_level(PIN_RELAY_12V, 1);  // Отключаем реле 12В (HIGH = отключено)
                
                // Останавливаем все таймеры
                xTimerStop(pump_timer, 0);
                xTimerStop(safety_timer, 0);
                xTimerStop(stop_timer, 0);
                xTimerStop(valve_delay_timer, 0);
                xTimerStop(valve_timeout_timer, 0);
                xTimerStop(valve_close_delay_timer, 0);
                
                current_state = STATE_ERROR;
                valve_state = VALVE_ERROR;
                
                // Зажигаем оба красных светодиода (LOW = включено)
                gpio_set_level(PIN_LED_VALVE_ERROR, 0);
                gpio_set_level(PIN_LED_PUMP_ERROR, 0);
                
                // Зуммер прерывистый (3с/3с) на заданное время
                alarm_buzzer_on();
            }
        }
        
        if (s1 != last_s1 || s2 != last_s2 || valve_sens != last_valve_sens || alarm != last_alarm) {
            printf("[LOG] S1=%d S2=%d ValveSens=%d Alarm=%d | Water=%d Overflow=%d TankEmpty=%d | State=%d Valve=%d | MosfetP=%d MosfetV=%d\n", 
                   s1, s2, valve_sens, alarm, water_normal, overflow_alarm, tank_empty, current_state, valve_state,
                   mosfet_pump_raw, mosfet_valve_raw);
            last_s1 = s1; last_s2 = s2; last_valve_sens = valve_sens; last_alarm = alarm;
        }
        
        // === КНОПКА СБРОСА ===
        uint32_t current_time = xTaskGetTickCount();
        if (btn_raw == 0 && (current_time - last_button_press) > pdMS_TO_TICKS(200)) {
            last_button_press = current_time;
            printf("[BUTTON] Reset pressed\n");
            
            gpio_set_level(PIN_PUMP, 0);
            gpio_set_level(PIN_VALVE, 0);
            alarm_buzzer_off();
            gpio_set_level(PIN_LED_VALVE_ERROR, 1);
            gpio_set_level(PIN_LED_PUMP_ERROR, 1);
            
            xTimerStop(pump_timer, 0);
            xTimerStop(safety_timer, 0);
            xTimerStop(stop_timer, 0);
            xTimerStop(valve_delay_timer, 0);
            xTimerStop(valve_timeout_timer, 0);
            xTimerStop(valve_close_delay_timer, 0);
            
            current_state = STATE_IDLE;
            valve_state = VALVE_CLOSED;
            mosfet_fault = false;
            gpio_set_level(PIN_RELAY_12V, 0);  // Включаем реле обратно (LOW = включено)
            
            for(int i = 0; i < 50; i++) {
                if(gpio_get_level(PIN_RESET_BUTTON) == 1) break;
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }
        
        // === АВТОМАТ КЛАПАНА ===
        if (current_state != STATE_DISABLED && !overflow_alarm) {
            switch (valve_state) {
                case VALVE_CLOSED:
                    if (tank_empty) {
                        printf("[VALVE] Tank empty, starting open delay (%d ms)\n", t_valve_delay_ms);
                        valve_state = VALVE_OPENING;
                        xTimerStart(valve_delay_timer, 0);
                    }
                    break;
                    
                case VALVE_OPENING:
                    if (!tank_empty) {
                        printf("[VALVE] Tank filled during opening delay, cancelling\n");
                        xTimerStop(valve_delay_timer, 0);
                        valve_state = VALVE_CLOSED;
                    }
                    break;
                    
                case VALVE_OPEN:
                    if (!tank_empty) {
                        printf("[VALVE] Tank filled, starting close delay (%d ms)\n", t_valve_close_delay_ms);
                        xTimerStop(valve_timeout_timer, 0);
                        valve_state = VALVE_CLOSING;
                        xTimerStart(valve_close_delay_timer, 0);
                    }
                    break;
                    
                case VALVE_CLOSING:
                    if (tank_empty) {
                        printf("[VALVE] Tank empty during close delay, cancelling close\n");
                        xTimerStop(valve_close_delay_timer, 0);
                        valve_state = VALVE_OPEN;
                        xTimerStart(valve_timeout_timer, 0);
                    }
                    break;
                    
                case VALVE_ERROR:
                    break;
            }
        } else {
            if (valve_state != VALVE_CLOSED && valve_state != VALVE_ERROR) {
                printf("[VALVE] System disabled/alarm, closing valve\n");
                gpio_set_level(PIN_VALVE, 0);
                xTimerStop(valve_delay_timer, 0);
                xTimerStop(valve_timeout_timer, 0);
                xTimerStop(valve_close_delay_timer, 0);
                valve_state = VALVE_CLOSED;
            }
        }
        
        // === АВТОМАТ НАСОСА ===
        if (overflow_alarm && current_state != STATE_ERROR && current_state != STATE_DISABLED) {
            printf("[!!!] OVERFLOW ALARM!\n");
            gpio_set_level(PIN_PUMP, 0);
            xTimerStop(pump_timer, 0);
            xTimerStop(safety_timer, 0);
            xTimerStop(stop_timer, 0);
            current_state = STATE_ERROR;
            alarm_buzzer_on();
        }
        
        switch (current_state) {
            case STATE_IDLE:
                if (!water_normal && !overflow_alarm && current_state != STATE_DISABLED) {
                    printf("[PUMP] No water -> WAITING\n");
                    current_state = STATE_WAITING;
                    xTimerStart(pump_timer, 0);
                }
                break;
            case STATE_WAITING:
                if (water_normal || overflow_alarm) {
                    printf("[PUMP] Water detected -> IDLE\n");
                    xTimerStop(pump_timer, 0);
                    current_state = STATE_IDLE;
                }
                break;
            case STATE_PUMPING:
                if (water_normal) {
                    printf("[PUMP] Water detected -> Stop timer (%d ms)\n", t_stop_ms);
                    xTimerStop(safety_timer, 0);
                    xTimerStart(stop_timer, 0);
                    current_state = STATE_TURNING_OFF;
                }
                break;
            case STATE_TURNING_OFF:
                if (!water_normal && !overflow_alarm) {
                    printf("[PUMP] Water lost -> Continue pumping\n");
                    xTimerStop(stop_timer, 0);
                    xTimerStart(safety_timer, 0);
                    current_state = STATE_PUMPING;
                } else if (overflow_alarm) {
                    xTimerStop(stop_timer, 0);
                    gpio_set_level(PIN_PUMP, 0);
                    current_state = STATE_ERROR;
                    alarm_buzzer_on();
                }
                break;
            case STATE_ERROR:
            case STATE_DISABLED:
                gpio_set_level(PIN_PUMP, 0);
                break;
        }
        
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

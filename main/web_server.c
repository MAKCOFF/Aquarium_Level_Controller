#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "web_server.h"
#include "timers_config.h"
#include "driver/gpio.h"
#include "esp_http_server.h"

// --- ПИНЫ ---
#define PIN_PUMP          25   // Основной насос
#define PIN_VALVE         33   // Клапан подпитки
#define PIN_SENS_1        4    // Верхний уровень (дисплей)
#define PIN_SENS_2        5    // Нижний уровень (самп)
#define PIN_VALVE_SENS    26   // Датчик уровня для клапана
#define PIN_BUZZER        32
#define PIN_SENS_ALARM    27

// --- СВЕТОДИОДЫ (активный LOW) ---
#define PIN_LED_VALVE_ERROR  19   // Красный: ошибка клапана (VALVE_ERROR)
#define PIN_LED_PUMP_ERROR   21   // Красный: ошибка насоса (STATE_ERROR)

// --- ЗАЩИТА ОТ ПРОБИТОГО МОСФЕТА ---
#define PIN_RELAY_12V        23   // Реле 12В (LOW = включено, HIGH = отключено)

system_state_t current_state = STATE_IDLE;
valve_state_t valve_state = VALVE_CLOSED;

// --- JSON API ---
static esp_err_t status_json_handler(httpd_req_t *req) {
    char resp[896];
    const char* st_names[] = {"NORMAL", "WAITING", "PUMPING", "ERROR", "DISABLED", "TURNING OFF"};
    const char* valve_names[] = {"CLOSED", "OPENING", "OPEN", "CLOSING", "ERROR"};
    
    int s1 = !gpio_get_level(PIN_SENS_1);
    int s2 = !gpio_get_level(PIN_SENS_2);
    int valve_sens = gpio_get_level(PIN_VALVE_SENS);
    int alarm = !gpio_get_level(PIN_SENS_ALARM);
    
    snprintf(resp, sizeof(resp),
             "{\"state\":%d,\"state_name\":\"%s\",\"valve_state\":%d,\"valve_state_name\":\"%s\","
             "\"s1\":%d,\"s2\":%d,\"valve_sens\":%d,\"alarm\":%d,\"mosfet_fault\":%d,"
             "\"timers\":{\"wait\":%d,\"safety\":%d,\"stop\":%d,\"buzzer\":%d,"
             "\"valve_delay\":%d,\"valve_timeout\":%d,\"valve_close_delay\":%d}}",
             current_state, st_names[current_state], 
             valve_state, valve_names[valve_state],
             s1, s2, valve_sens, alarm, mosfet_fault ? 1 : 0,
             t_wait_ms, t_safety_ms, t_stop_ms, t_buzzer_ms,
             t_valve_delay_ms, t_valve_timeout_ms, t_valve_close_delay_ms);
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, strlen(resp));
    return ESP_OK;
}

static esp_err_t set_timer_handler(httpd_req_t *req) {
    char buf[250];
    int ret = httpd_req_recv(req, buf, sizeof(buf)-1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid data");
        return ESP_FAIL;
    }
    buf[ret] = '\0';
    
    char timer_name[25] = {0};
    int value = 0;
    sscanf(buf, "{\"timer\":\"%[^\"]\",\"value\":%d}", timer_name, &value);
    
    // ==== ИНДИВИДУАЛЬНЫЕ ОГРАНИЧЕНИЯ ====
    if (strcmp(timer_name, "wait") == 0) {
        // Задержка запуска насоса: 5-60 сек
        if (value < 5000) value = 5000;
        if (value > 60000) value = 60000;
    }
    else if (strcmp(timer_name, "safety") == 0) {
        // Защитный таймер насоса: 10-60 сек
        if (value < 10000) value = 10000;
        if (value > 60000) value = 60000;
    }
    else if (strcmp(timer_name, "stop") == 0) {
        // Задержка остановки насоса: 1-30 сек
        if (value < 1000) value = 1000;
        if (value > 20000) value = 20000;
    }
    else if (strcmp(timer_name, "buzzer") == 0) {
        // Длительность звука: 10-120 сек
        if (value < 10000) value = 10000;
        if (value > 120000) value = 120000;
    }
    else if (strcmp(timer_name, "valve_delay") == 0) {
        // Задержка открытия клапана: 1-30 сек
        if (value < 1000) value = 1000;
        if (value > 30000) value = 30000;
    }
    else if (strcmp(timer_name, "valve_timeout") == 0) {
        // Таймаут работы клапана: 10-600 сек
        if (value < 10000) value = 10000;
        if (value > 600000) value = 600000;
    }
    else if (strcmp(timer_name, "valve_close_delay") == 0) {
        // Задержка закрытия клапана: 1-20 сек
        if (value < 1000) value = 1000;
        if (value > 20000) value = 20000;
    }
    // ============================
    
    if (strcmp(timer_name, "wait") == 0) t_wait_ms = value;
    else if (strcmp(timer_name, "safety") == 0) t_safety_ms = value;
    else if (strcmp(timer_name, "stop") == 0) t_stop_ms = value;
    else if (strcmp(timer_name, "buzzer") == 0) t_buzzer_ms = value;
    else if (strcmp(timer_name, "valve_delay") == 0) t_valve_delay_ms = value;
    else if (strcmp(timer_name, "valve_timeout") == 0) t_valve_timeout_ms = value;
    else if (strcmp(timer_name, "valve_close_delay") == 0) t_valve_close_delay_ms = value;
    
    save_timers_to_nvs();
    extern timer_callbacks_t main_timer_callbacks;
    restart_timers(&main_timer_callbacks);
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// --- ХЕНДЛЕРЫ ---
static esp_err_t off_handler(httpd_req_t *req) {
    printf("[WEB] Reset/Auto\n");
    gpio_set_level(PIN_PUMP, 0);
    gpio_set_level(PIN_VALVE, 0);
    alarm_buzzer_off();
    gpio_set_level(PIN_LED_VALVE_ERROR, 1);
    gpio_set_level(PIN_LED_PUMP_ERROR, 1);
    gpio_set_level(PIN_RELAY_12V, 0);  // Включаем реле обратно (LOW = включено)
    xTimerStop(pump_timer, 0);
    xTimerStop(safety_timer, 0);
    xTimerStop(stop_timer, 0);
    xTimerStop(valve_delay_timer, 0);
    xTimerStop(valve_timeout_timer, 0);
    xTimerStop(valve_close_delay_timer, 0);
    current_state = STATE_IDLE;
    valve_state = VALVE_CLOSED;
    mosfet_fault = false;
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t disable_handler(httpd_req_t *req) {
    printf("[WEB] Disable all\n");
    gpio_set_level(PIN_PUMP, 0);
    gpio_set_level(PIN_VALVE, 0);
    alarm_buzzer_off();
    gpio_set_level(PIN_LED_VALVE_ERROR, 1);
    gpio_set_level(PIN_LED_PUMP_ERROR, 1);
    gpio_set_level(PIN_RELAY_12V, 0);  // Включаем реле обратно (LOW = включено)
    xTimerStop(pump_timer, 0);
    xTimerStop(safety_timer, 0);
    xTimerStop(stop_timer, 0);
    xTimerStop(valve_delay_timer, 0);
    xTimerStop(valve_timeout_timer, 0);
    xTimerStop(valve_close_delay_timer, 0);
    current_state = STATE_DISABLED;
    valve_state = VALVE_CLOSED;
    mosfet_fault = false;
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// --- ВЕБ СТРАНИЦА ---
static const char HTML_PAGE[] = 
    "<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1'>"
    "<title>Контроллер уровня воды</title><style>"
    "body{font-family:Arial;text-align:center;margin:20px;background:#f0f0f0;}"
    ".container{max-width:700px;margin:auto;background:white;padding:20px;border-radius:10px;}"
    "h1{color:#333;font-size:24px;}h2{color:#555;font-size:18px;margin:15px 0 10px 0;border-bottom:1px solid #ddd;}"
    ".state{font-size:24px;margin:20px 0;}.state div{padding:20px;border-radius:10px;font-weight:bold;}"
    ".sensors{display:flex;justify-content:space-around;margin:20px 0;flex-wrap:wrap;gap:10px;}"
    ".sensor{padding:15px;border-radius:10px;flex:1;min-width:100px;text-align:center;}"
    ".sensor h3{margin:0;font-size:16px;}.sensor p{margin:8px 0 0 0;font-size:12px;color:#666;}"
    ".sensor.active{background:#d4edda;border:2px solid #28a745;}"
    ".sensor.inactive{background:#e0e0e0;border:2px solid #999;}"
    ".sensor.alarm{background:#f8d7da;border:2px solid #dc3545;}"
    ".button{padding:12px 25px;margin:5px;font-size:16px;cursor:pointer;border:none;border-radius:5px;}"
    ".btn-reset{background:#4CAF50;color:white;}.btn-disable{background:#ff9800;color:white;}"
    ".timers{margin:20px 0;padding:15px;background:#e8e8e8;border-radius:10px;text-align:left;}"
    ".timer-group{margin-bottom:20px;padding:10px;background:#f0f0f0;border-radius:8px;}"
    ".timer-group h4{margin:0 0 10px 0;color:#333;}"
    ".timer-item{margin:12px 0;display:flex;align-items:center;justify-content:space-between;flex-wrap:wrap;}"
    ".timer-item label{width:180px;font-weight:bold;}"
    ".timer-item input{width:100px;padding:8px;border:1px solid #ccc;border-radius:4px;text-align:center;}"
    ".timer-desc{font-size:11px;color:#888;width:100%;margin-top:5px;}"
    ".save-btn{background:#2196F3;color:white;padding:10px;margin-top:15px;width:100%;border:none;border-radius:5px;cursor:pointer;}"
    ".status-msg{margin-top:10px;padding:8px;border-radius:5px;display:none;}"
    ".status-msg.success{background:#4CAF50;color:white;display:block;}"
    "</style></head><body>"
    "<div class='container'><h1>Контроллер долива воды</h1>"
    "<div class='state' id='stateDiv'>Загрузка...</div>"
    
    "<h2>Насос</h2>"
    "<div class='sensors'>"
    "<div class='sensor' id='displaySensor'><h3>Аквариум</h3><p>Датчик уровня</p></div>"
    "<div class='sensor' id='waterSensor'><h3>Самп</h3><p>Датчик уровня</p></div>"
    "<div class='sensor' id='alarmSensor'><h3>Самп</h3><p>Датчик перелива</p></div>"
    "</div>"
    
    "<h2>Клапан подпитки</h2>"
    "<div class='sensors'>"
    "<div class='sensor' id='valveSensor'><h3>Ёмкость подпитки</h3><p>Датчик уровня</p></div>"
    "<div class='sensor' id='valveStateSensor'><h3>Состояние клапана</h3><p>Положение</p></div>"
    "</div>"
    
    "<h2>Защита мосфетов</h2>"
    "<div class='sensors'>"
    "<div class='sensor' id='mosfetSensor'><h3>Мосфеты</h3><p>Состояние</p></div>"
    "<div class='sensor' id='relaySensor'><h3>Реле 12В</h3><p>Питание</p></div>"
    "</div>"
    
    "<div class='buttons'>"
    "<button class='button btn-reset' onclick='resetSystem()'>Сброс / АВТО</button>"
    "<button class='button btn-disable' onclick='disableSystem()'>Отключить насос и клапан</button>"
    "</div>"
    
    "<div class='timers'><h3>Настройки таймеров</h3>"
    "<div class='timer-group'><h4>Таймеры насоса</h4>"
    "<div class='timer-item'><label>Задержка запуска:</label><input type='number' id='waitInput' min='5' max='60' step='1'><span>сек</span>"
    "<div class='timer-desc'>Задержка перед запуском насоса после падения уровня воды</div></div>"
    "<div class='timer-item'><label>Защитный таймер:</label><input type='number' id='safetyInput' min='10' max='120' step='1'><span>сек</span>"
    "<div class='timer-desc'>Максимальное время работы насоса, если уровень не поднялся</div></div>"
    "<div class='timer-item'><label>Задержка остановки:</label><input type='number' id='stopInput' min='1' max='30' step='1'><span>сек</span>"
    "<div class='timer-desc'>Дополнительное время подкачки после достижения уровня</div></div>"
    "<div class='timer-item'><label>Длительность звука:</label><input type='number' id='buzzerInput' min='10' max='120' step='1'><span>сек</span>"
    "<div class='timer-desc'>Как долго пищит зуммер при ошибке</div></div></div>"
    
    "<div class='timer-group'><h4>Таймеры клапана</h4>"
    "<div class='timer-item'><label>Задержка открытия:</label><input type='number' id='valveDelayInput' min='1' max='30' step='1'><span>сек</span>"
    "<div class='timer-desc'>Задержка перед открытием клапана после опустошения ёмкости</div></div>"
    "<div class='timer-item'><label>Таймаут работы:</label><input type='number' id='valveTimeoutInput' min='5' max='120' step='1'><span>сек</span>"
    "<div class='timer-desc'>Максимальное время открытия клапана (защита)</div></div>"
    "<div class='timer-item'><label>Задержка закрытия:</label><input type='number' id='valveCloseDelayInput' min='1' max='60' step='1'><span>сек</span>"
    "<div class='timer-desc'>Задержка перед закрытием клапана после наполнения ёмкости</div></div></div>"
    
    "<button class='save-btn' onclick='saveTimers()'>Сохранить настройки</button>"
    "<div id='statusMsg' class='status-msg'></div></div></div>"
    
    "<script>"
    "let updating=false,pendingUpdate=false;"
    "function updateStatus(){if(updating){pendingUpdate=true;return;}updating=true;"
    "fetch('/status.json').then(r=>r.json()).then(data=>{"
    "const colors={0:'#4CAF50',1:'#FF9800',2:'#2196F3',3:'#f44336',4:'#9E9E9E',5:'#9C27B0'};"
    "const stateText={0:'НОРМА',1:'ОЖИДАНИЕ',2:'НАСОС РАБОТАЕТ',3:'ОШИБКА',4:'ОТКЛЮЧЕНО',5:'ВЫКЛЮЧЕНИЕ'};"
    "document.getElementById('stateDiv').innerHTML=`<div style='background:${colors[data.state]};color:white;'>${stateText[data.state]}</div>`;"
    "document.getElementById('displaySensor').className=data.s1?'sensor active':'sensor inactive';"
    "document.getElementById('waterSensor').className=data.s2?'sensor active':'sensor inactive';"
    "document.getElementById('alarmSensor').className=!data.alarm?'sensor alarm':'sensor inactive';"
    "document.getElementById('valveSensor').className=data.valve_sens?'sensor active':'sensor inactive';"
    "const valveColors = {0:'#999', 1:'#FF9800', 2:'#2196F3', 3:'#FF9800', 4:'#f44336'};"
    "const valveText = {0:'ЗАКРЫТ', 1:'ОТКРЫТИЕ', 2:'ОТКРЫТ', 3:'ЗАКРЫТИЕ', 4:'ОШИБКА'};"
    "document.getElementById('valveStateSensor').innerHTML=`<h3>Состояние клапана</h3><p style='background:${valveColors[data.valve_state]};padding:5px;border-radius:5px;color:white;'>${valveText[data.valve_state]}</p>`;"
    "let mosfetColor=data.mosfet_fault?'#f44336':'#4CAF50';let mosfetText=data.mosfet_fault?'АВАРИЯ':'НОРМА';"
    "document.getElementById('mosfetSensor').className=data.mosfet_fault?'sensor alarm':'sensor active';"
    "document.getElementById('mosfetSensor').innerHTML=`<h3>Мосфеты</h3><p style='background:${mosfetColor};padding:5px;border-radius:5px;color:white;'>${mosfetText}</p>`;"
    "let relayColor=data.mosfet_fault?'#f44336':'#4CAF50';let relayText=data.mosfet_fault?'ОТКЛЮЧЕНО':'ВКЛЮЧЕНО';"
    "document.getElementById('relaySensor').className=data.mosfet_fault?'sensor alarm':'sensor active';"
    "document.getElementById('relaySensor').innerHTML=`<h3>Реле 12В</h3><p style='background:${relayColor};padding:5px;border-radius:5px;color:white;'>${relayText}</p>`;"
    "if(!document.activeElement||document.activeElement.id!=='waitInput')document.getElementById('waitInput').value=data.timers.wait/1000;"
    "if(!document.activeElement||document.activeElement.id!=='safetyInput')document.getElementById('safetyInput').value=data.timers.safety/1000;"
    "if(!document.activeElement||document.activeElement.id!=='stopInput')document.getElementById('stopInput').value=data.timers.stop/1000;"
    "if(!document.activeElement||document.activeElement.id!=='buzzerInput')document.getElementById('buzzerInput').value=data.timers.buzzer/1000;"
    "if(!document.activeElement||document.activeElement.id!=='valveDelayInput')document.getElementById('valveDelayInput').value=data.timers.valve_delay/1000;"
    "if(!document.activeElement||document.activeElement.id!=='valveTimeoutInput')document.getElementById('valveTimeoutInput').value=data.timers.valve_timeout/1000;"
    "if(!document.activeElement||document.activeElement.id!=='valveCloseDelayInput')document.getElementById('valveCloseDelayInput').value=data.timers.valve_close_delay/1000;"
    "}).catch(err=>console.log('Ошибка:',err)).finally(()=>{updating=false;if(pendingUpdate){pendingUpdate=false;updateStatus();}});}"
    "function showMessage(text){let msg=document.getElementById('statusMsg');msg.className='status-msg success';msg.innerHTML=text;setTimeout(()=>msg.className='status-msg',3000);}"
    "function saveTimers(){"
    "let waitVal=document.getElementById('waitInput').value*1000;"
    "let safetyVal=document.getElementById('safetyInput').value*1000;"
    "let stopVal=document.getElementById('stopInput').value*1000;"
    "let buzzerVal=document.getElementById('buzzerInput').value*1000;"
    "let valveDelayVal=document.getElementById('valveDelayInput').value*1000;"
    "let valveTimeoutVal=document.getElementById('valveTimeoutInput').value*1000;"
    "let valveCloseDelayVal=document.getElementById('valveCloseDelayInput').value*1000;"
    "Promise.all(["
    "fetch('/set_timer',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({timer:'wait',value:waitVal})}),"
    "fetch('/set_timer',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({timer:'safety',value:safetyVal})}),"
    "fetch('/set_timer',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({timer:'stop',value:stopVal})}),"
    "fetch('/set_timer',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({timer:'buzzer',value:buzzerVal})}),"
    "fetch('/set_timer',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({timer:'valve_delay',value:valveDelayVal})}),"
    "fetch('/set_timer',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({timer:'valve_timeout',value:valveTimeoutVal})}),"
    "fetch('/set_timer',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({timer:'valve_close_delay',value:valveCloseDelayVal})})"
    "]).then(()=>showMessage('Настройки сохранены!')).catch(()=>showMessage('Ошибка сохранения!'));}"
    "function resetSystem(){fetch('/off').then(()=>setTimeout(updateStatus,500));}"
    "function disableSystem(){fetch('/disable').then(()=>setTimeout(updateStatus,500));}"
    "setInterval(updateStatus,1000);updateStatus();"
    "</script></body></html>";

static esp_err_t get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, HTML_PAGE, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

void start_webserver(void) {
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    config.lru_purge_enable = true;
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t uri_get = {.uri = "/", .method = HTTP_GET, .handler = get_handler};
        httpd_register_uri_handler(server, &uri_get);
        httpd_uri_t uri_status = {.uri = "/status.json", .method = HTTP_GET, .handler = status_json_handler};
        httpd_register_uri_handler(server, &uri_status);
        httpd_uri_t uri_set = {.uri = "/set_timer", .method = HTTP_POST, .handler = set_timer_handler};
        httpd_register_uri_handler(server, &uri_set);
        httpd_uri_t uri_off = {.uri = "/off", .method = HTTP_GET, .handler = off_handler};
        httpd_register_uri_handler(server, &uri_off);
        httpd_uri_t uri_disable = {.uri = "/disable", .method = HTTP_GET, .handler = disable_handler};
        httpd_register_uri_handler(server, &uri_disable);
        printf("[WEB] HTTP server started\n");
    }
}

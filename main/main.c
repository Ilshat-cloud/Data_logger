#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "driver/gpio.h"

#include "inc/wifi.h"
#include "inc/storage.h"

/* The examples use WiFi configuration that you can set via project configuration menu.

   If you'd rather not, just change the below entries to strings with
   the config you want - ie #define EXAMPLE_WIFI_SSID "mywifissid"
*/
QueueHandle_t log_queue = NULL;
extern volatile bool is_logging_active;
void led_init(void);
void discrete_inputs_init(void);
static void IRAM_ATTR gpio_isr_handler(void* arg);
static inline uint16_t read_disc_pins_mask(void) ;
void boot_button_init(void) ;

#define REG_READ(_r) ({                                                                                                \
            (*(volatile uint32_t *)(_r));                                                                              \
        })

void app_main(void)
{
    //Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    led_init();
    wifi_init_softap();
    vTaskDelay(1000 / portTICK_PERIOD_MS); // Ждем секунду, чтобы WiFi успел подняться
    ret = sd_card_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG2, "Ошибка инициализации SD-карты: %s", esp_err_to_name(ret));
        return;
    }
    xTaskCreate(tcp_server_task, "tcp_server", 20000, NULL, 6, NULL);

    // 1. Создаем очередь на 2000 элементов
    log_queue = xQueueCreate(QUEUE_SIZE, sizeof(log_item_t));
    if (log_queue == NULL) {
        ESP_LOGE(TAG2, "Ошибка: не удалось создать очередь логов!");
        return;
    }

    // 2. Создаем задачу сбора данных (стек 2048 байт, приоритет 5 - повыше)
    xTaskCreate(collect_data_task, "collect_data_task", 2048, NULL, 5, NULL);

    // 3. Создаем задачу записи на SD-карту (стек 20000 байт, т.к. работа с файлами требует больше памяти, приоритет 3)
    xTaskCreate(sd_writer_task, "sd_writer_task", 30000, NULL, 3, NULL);
    gpio_set_level(PIN_LED_1, 1);
    discrete_inputs_init();
    boot_button_init(); 
}


void led_init(void) {
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,       // Прерывания не нужны
        .mode = GPIO_MODE_OUTPUT,             // Режим выхода
        .pin_bit_mask = (1ULL << PIN_LED_1) | (1ULL << PIN_LED_2), // Маска пинов
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
    };
    gpio_config(&io_conf);
    gpio_set_level(PIN_LED_1, 0);
    gpio_set_level(PIN_LED_2, 0);
}

// Вспомогательная функция для чтения маски 5 пинов в единое 32-битное число
static inline uint16_t read_disc_pins_mask(void) {
    uint16_t mask = 0;
    if (gpio_get_level(PIN_DISC_0))  mask |= (1 << 0);
    if (gpio_get_level(PIN_DISC_1))  mask |= (1 << 1);
    if (gpio_get_level(PIN_DISC_2))  mask |= (1 << 2);
    if (gpio_get_level(PIN_DISC_3))  mask |= (1 << 3);
    return mask;
}

// ==========================================
// ОБРАБОТЧИК ПРЕРЫВАНИЙ (ISR)
// ==========================================
static void IRAM_ATTR gpio_isr_handler(void* arg) {
    log_item_t item;
    item.timestamp = (uint32_t)(xTaskGetTickCountFromISR() * portTICK_PERIOD_MS);
    item.pin_states = read_disc_pins_mask();

    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xQueueSendFromISR(log_queue, &item, &xHigherPriorityTaskWoken);

    if (xHigherPriorityTaskWoken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}
// ==========================================
// ИНИЦИАЛИЗАЦИЯ ПИНОВ И ПРЕРЫВАНИЙ
// ==========================================
void discrete_inputs_init(void) {
    // Маска всех используемых пинов
    uint64_t pin_mask = (1ULL << PIN_DISC_0)  |
                        (1ULL << PIN_DISC_1)  |
                        (1ULL << PIN_DISC_2)  |
                        (1ULL << PIN_DISC_3) ;

    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_ANYEDGE,     // Ловим и фронт, и спад (весь дребезг)
        .mode = GPIO_MODE_INPUT,            // Режим входа
        .pin_bit_mask = pin_mask,           // Применяем маску пинов
        .pull_up_en = GPIO_PULLUP_ENABLE,   // Подтяжка к питанию (для кнопок/контактов на GND)
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    
    // Применяем конфигурацию к пинам
    gpio_config(&io_conf);

    // Устанавливаем глобальный сервис обработки прерываний GPIO (если еще не был установлен)
    gpio_install_isr_service(0);

    // Привязываем общий обработчик к каждому пину индивидуально
    gpio_isr_handler_add(PIN_DISC_0,  gpio_isr_handler, (void*)PIN_DISC_0);
    gpio_isr_handler_add(PIN_DISC_1,  gpio_isr_handler, (void*)PIN_DISC_1);
    gpio_isr_handler_add(PIN_DISC_2,  gpio_isr_handler, (void*)PIN_DISC_2);
    gpio_isr_handler_add(PIN_DISC_3,  gpio_isr_handler, (void*)PIN_DISC_3);
}

void boot_button_init(void) {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << PIN_I2C_SCL),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,     // Кнопка подтянута к питанию (в покое 1)
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE         // Без прерываний, опросим в задаче
    };
    gpio_config(&io_conf);
}
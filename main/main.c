#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "inc/wifi.h"
#include "inc/storage.h"

/* The examples use WiFi configuration that you can set via project configuration menu.

   If you'd rather not, just change the below entries to strings with
   the config you want - ie #define EXAMPLE_WIFI_SSID "mywifissid"
*/
QueueHandle_t log_queue = NULL;


void app_main(void)
{
    //Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

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


}



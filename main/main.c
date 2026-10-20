#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "sdmmc_cmd.h"
#include <dirent.h>

#include "inc/wifi.h"

static const char *TAG2 = "SD_FATFS";

// Назначение пинов SPI для ESP32-C3
#define PIN_NUM_MISO 5
#define PIN_NUM_MOSI 6
#define PIN_NUM_CLK  4
#define PIN_NUM_CS   7

#define MOUNT_POINT "/sdcard"

/* The examples use WiFi configuration that you can set via project configuration menu.

   If you'd rather not, just change the below entries to strings with
   the config you want - ie #define EXAMPLE_WIFI_SSID "mywifissid"
*/
typedef struct {
    uint32_t timestamp;
    uint32_t pin_states;
} log_item_t;

#define QUEUE_SIZE 2000
#define BATCH_SIZE 1000
QueueHandle_t log_queue = NULL;

esp_err_t sd_card(void);

// --- 1. Задача сбора данных (вызывается каждую 1 мс, например через таймер или высокоприоритетную задачу) ---
void collect_data_task(void *pvParameters) {

    while (1) {
        log_item_t item;
        item.timestamp = xTaskGetTickCount() ; // Время в мс от старта
        item.pin_states = 666;

        // Пытаемся закинуть в очередь. Если очередь забита (например, SD-карта зависла),
        // используем таймаут 0, чтобы не заблокировать прерывание/опрос в 1 мс.
        if (xQueueSend(log_queue, &item, 0) != pdPASS) {
            // Очередь переполнена! Можно инкрементировать счетчик пропущенных точек
            vTaskDelay(10); // Немного подождем, чтобы флшка успела отработать
        }

        vTaskDelay(1);
    }
}

// --- 2. Фоновая задача записи на SD-карту ---
void sd_writer_task(void *pvParameters) {
    static log_item_t batch[BATCH_SIZE];

    ESP_LOGI(TAG2, "Фоновый писатель запущен, ждем накопления %d элементов...", BATCH_SIZE);

    while (1) {
        // Проверяем: накопилось ли в очереди достаточное количество данных?
        if (uxQueueMessagesWaiting(log_queue) >= BATCH_SIZE) {
            
            // Выгребаем ровно BATCH_SIZE элементов из очереди
            for (int i = 0; i < BATCH_SIZE; i++) {
                xQueueReceive(log_queue, &batch[i], 0);
            }

            // Открываем файл под каждую запись
            FILE *f = fopen("/sdcard/datalog.bin", "ab");
            if (f == NULL) {
                ESP_LOGE(TAG2, "Не удалось открыть файл для записи!");
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }

            // Записываем пачку
            size_t written = fwrite(batch, sizeof(log_item_t), BATCH_SIZE, f);
            
            // Сбрасываем кэш и закрываем файл, чтобы размер сразу обновился на флешке!
            fflush(f);
            fclose(f);

            ESP_LOGI(TAG2, "Записана пачка из %d элементов (%d байт) и файл закрыт", written, BATCH_SIZE * sizeof(log_item_t));
        } else {
            // Если данных мало — спим 50 мс
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
}

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
    ESP_ERROR_CHECK(sd_card());

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


esp_err_t sd_card(void){
    esp_err_t ret;
    ESP_LOGI(TAG2, "Инициализация SD карты через SPI...");
    // 1. Настройка точек монтирования FATFS
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false, // Не форматировать карту при ошибке
        .max_files = 5,                  // Макс. кол-во одновременно открытых файлов
        .allocation_unit_size = 16 * 1024
    };

    sdmmc_card_t *card;
    const char mount_point[] = MOUNT_POINT;

    // 2. Инициализация SPI шины (SPI2_HOST)
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST; // В ESP32-C3 используется SPI2

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = PIN_NUM_MISO,
        .sclk_io_num = PIN_NUM_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };

    ret = spi_bus_initialize(host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG2, "Ошибка инициализации шины SPI: %s", esp_err_to_name(ret));
        return ret;
    }

    // 3. Настройка слота SD SPI
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = PIN_NUM_CS;
    slot_config.host_id = host.slot;

    // 4. Монтирование файловой системы FATFS
    ret = esp_vfs_fat_sdspi_mount(mount_point, &host, &slot_config, &mount_config, &card);

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG2, "Не удалось смонтировать FATFS. Проверьте карту.");
        } else {
            ESP_LOGE(TAG2, "Ошибка подключения SD-карты (%s).", esp_err_to_name(ret));
        }
        return ret;
    }

    ESP_LOGI(TAG2, "SD-карта успешно смонтирована!");
    sdmmc_card_print_info(stdout, card); // Вывести информацию о карте
    ESP_LOGI(TAG2, "Содержимое карты памяти (/sdcard):");
    // Открываем корневую папку монтирования
    DIR *dir = opendir(MOUNT_POINT);
    if (dir == NULL) {
        ESP_LOGE(TAG2, "Не удалось открыть каталог для чтения!");
    }else{
        struct dirent *entry;
        int file_count = 0;

        // Читаем элементы по очереди
        while ((entry = readdir(dir)) != NULL) {
            file_count++;
            // entry->d_name — имя файла или папки
            // entry->d_type может подсказать тип (DT_DIR для папки, DT_REG для файла)
            if (entry->d_type == DT_DIR) {
                ESP_LOGI(TAG2, "  [DIR]  %s", entry->d_name);
            } else {
                ESP_LOGI(TAG2, "  [FILE] %s", entry->d_name);
            }
        }

        // Обязательно закрываем директорию после чтения, чтобы не было утечки памяти/дескрипторов
        closedir(dir);

        ESP_LOGI(TAG2, "Всего элементов: %d", file_count);  
    }

    // --- Проверка записи в файл ---
    FILE *f = fopen(MOUNT_POINT "/test.txt", "w");
    if (f == NULL) {
        ESP_LOGE(TAG2, "Не удалось создать файл!");
        return ret;
    }
    fprintf(f, "ESP32-C3 Logger Test\n");
    fclose(f);
    ESP_LOGI(TAG2, "Файл успешно записан!");

    // Отмонтирование (при необходимости)
    // esp_vfs_fat_sdcard_unmount(mount_point, card);
    return ESP_OK;

}
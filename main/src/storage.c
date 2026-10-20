#include "inc/storage.h"

#include "esp_vfs_fat.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "sdmmc_cmd.h"
#include <dirent.h>

extern QueueHandle_t log_queue;
sdmmc_card_t *card=NULL;
volatile bool is_logging_active = false;
char active_filename[256] = "/sdcard/datalog.bin";

// --- 1. Задача сбора данных (вызывается каждую 1 мс, например через таймер или высокоприоритетную задачу) ---
void collect_data_task(void *pvParameters) {

    while (1) {
        if (!is_logging_active) {
            
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
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

        if (!is_logging_active) {
            
            vTaskDelay(pdMS_TO_TICKS(100));
            xQueueReset(log_queue);
            continue;
        }

        // Проверяем: накопилось ли в очереди достаточное количество данных?
        if (uxQueueMessagesWaiting(log_queue) >= BATCH_SIZE) {
            
            // Выгребаем ровно BATCH_SIZE элементов из очереди
            for (int i = 0; i < BATCH_SIZE; i++) {
                xQueueReceive(log_queue, &batch[i], 0);
            }

            // Открываем файл под каждую запись
            FILE *f = fopen(active_filename, "ab");
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

esp_err_t sd_card_init(void){
    esp_err_t ret;
    ESP_LOGI(TAG2, "Инициализация SD карты через SPI...");
    // 1. Настройка точек монтирования FATFS
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false, // Не форматировать карту при ошибке
        .max_files = 5,                  // Макс. кол-во одновременно открытых файлов
        .allocation_unit_size = 16 * 1024
    };

    
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
    return ESP_OK;

}

void sd_card_status_update(){
    if (card != NULL) {
        sdmmc_card_print_info(stdout, card);
    } else {
        ESP_LOGE(TAG2, "SD-карта не инициализирована!");
    }
}


int sd_get_file_list_str(char *out_buf, size_t max_len) {
    DIR *dir = opendir(MOUNT_POINT);
    if (dir == NULL) {
        return snprintf(out_buf, max_len, "ERROR: Cannot open directory\n");
    }

    struct dirent *entry;
    int file_count = 0;
    size_t offset = 0;

    // Записываем заголовок ответа
    offset += snprintf(out_buf + offset, max_len - offset, "--- FILES START ---\n");

    // Читаем элементы по очереди
    while ((entry = readdir(dir)) != NULL) {
        file_count++;
        
        // Проверяем, чтобы не выйти за рамки буфера
        if (offset >= max_len - 64) {
            break; 
        }

        if (entry->d_type == DT_DIR) {
            offset += snprintf(out_buf + offset, max_len - offset, "[DIR]  %s\n", entry->d_name);
        } else {
            offset += snprintf(out_buf + offset, max_len - offset, "[FILE] %s\n", entry->d_name);
        }
    }

    // Закрываем директорию
    closedir(dir);

    // Дописываем итог
    offset += snprintf(out_buf + offset, max_len - offset, "Total elements: %d\n--- FILES END ---\n", file_count);
    
    return (int)offset; // Возвращаем реальную длину сформированной строки
}


FILE* sd_bin_file_open(const char *filename) {
    char full_path[64];
    // Если в пути нет префикса /sdcard/, добавляем его автоматически
    if (strncmp(filename, "/sdcard/", 8) != 0) {
        snprintf(full_path, sizeof(full_path), "/sdcard/%s", filename);
    } else {
        snprintf(full_path, sizeof(full_path), "%s", filename);
    }

    FILE *f = fopen(full_path, "rb");
    if (f == NULL) {
        ESP_LOGE("STORAGE", "Не удалось открыть файл %s для чтения", full_path);
    }
    return f;
}

// 2. Чтение порции данных (куска) из файла
size_t sd_bin_file_read(FILE *f, void *buffer, size_t size) {
    if (f == NULL) return 0;
    return fread(buffer, 1, size, f);
}

// 3. Закрытие файла
void sd_bin_file_close(FILE *f) {
    if (f != NULL) {
        fclose(f);
    }
}
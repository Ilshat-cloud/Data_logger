#ifndef STORAGE_H
#define STORAGE_H

#include <stdio.h>
#include <string.h>
#include "esp_err.h"


// Назначение пинов SPI для ESP32-C3
#define PIN_NUM_MISO 5
#define PIN_NUM_MOSI 6
#define PIN_NUM_CLK  4
#define PIN_NUM_CS   7

#define MOUNT_POINT "/sdcard"

static const char *TAG2 = "SD_FATFS";
#define QUEUE_SIZE 2000
#define BATCH_SIZE 1000


typedef struct {
    uint32_t timestamp;
    uint32_t pin_states;
} log_item_t;

esp_err_t sd_card_init(void);
void sd_writer_task(void *pvParameters);
void collect_data_task(void *pvParameters);
void sd_card_status_update(void);
int sd_get_file_list_str(char *out_buf, size_t max_len);

#endif // STORAGE_H
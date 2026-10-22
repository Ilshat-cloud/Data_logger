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

// 2. Дискретные входы (EXTI с прерываниями по фронту) — СВОБОДНО
#define PIN_DISC_0         0
#define PIN_DISC_1         1
#define PIN_DISC_2         2
#define PIN_DISC_3         3

// 3. I2C шина (Будущий 16-битный АЦП) — РЕЗЕРВ
#define PIN_I2C_SDA        8        //будет тут CS от АЦП, а SCL будет кнопкой BOOT
#define PIN_I2C_SCL        9        //он же и boot 

// 4. CAN / TWAI шина (Будущая автомобильная шина) — РЕЗЕРВ
#define PIN_CAN_TX         18
#define PIN_CAN_RX         19

// 5. Внутренняя периферия (для справки, не трогаем)
#define PIN_LED_1          12 // Встроенный светодиод
#define PIN_LED_2          13 // Встроенный светодиод
#define PIN_JTAG_TMS       10 // Аппаратный отладчик

#define MOUNT_POINT "/sdcard"

static const char *TAG2 = "SD_FATFS";
#define QUEUE_SIZE 8000
#define BATCH_SIZE 1000


typedef struct {
    uint32_t timestamp;  // Время события в мс
    uint32_t pin_states; // Битовая маска всех пинов (биты 0, 1, 2, 3, 11)
} log_item_t;

esp_err_t sd_card_init(void);
void sd_writer_task(void *pvParameters);
void collect_data_task(void *pvParameters);
void sd_card_status_update(void);
int sd_get_file_list_str(char *out_buf, size_t max_len);

#endif // STORAGE_H
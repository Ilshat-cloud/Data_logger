#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "inc/wifi.h"
#include "esp_netif.h"
#include <sys/param.h>
#include <sys/socket.h>
#include <netdb.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_err.h"
#include "sdmmc_cmd.h"

static const char *TAG = "wifi softAP";
#define EXAMPLE_ESP_WIFI_SSID      CONFIG_ESP_WIFI_SSID
#define EXAMPLE_ESP_WIFI_PASS      CONFIG_ESP_WIFI_PASSWORD
#define EXAMPLE_ESP_WIFI_CHANNEL   CONFIG_ESP_WIFI_CHANNEL
#define EXAMPLE_MAX_STA_CONN       CONFIG_ESP_MAX_STA_CONN

#if CONFIG_ESP_GTK_REKEYING_ENABLE
#define EXAMPLE_GTK_REKEY_INTERVAL CONFIG_ESP_GTK_REKEY_INTERVAL
#else
#define EXAMPLE_GTK_REKEY_INTERVAL 0
#endif

const int WIFI_CONNECTED_BIT = BIT0;
static int8_t client_status=0;                     //Status для wifi AP

extern volatile bool is_logging_active;
extern sdmmc_card_t *card;
extern char active_filename[256];
static void sd_send_file_list(int sock);
static void sd_send_status(int sock);

extern int sd_get_file_list_str(char *out_buf, size_t max_len);

extern FILE* sd_bin_file_open(const char *filename);
extern size_t sd_bin_file_read(FILE *f, void *buffer, size_t size);
extern void sd_bin_file_close(FILE *f);

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                    int32_t event_id, void* event_data)
{
    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
        ESP_LOGI(TAG, "station "MACSTR" join, AID=%d",
                 MAC2STR(event->mac), event->aid);
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
        ESP_LOGI(TAG, "station "MACSTR" leave, AID=%d, reason=%d",
                 MAC2STR(event->mac), event->aid, event->reason);
    }
}

void wifi_init_softap(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));
                                                        


    wifi_config_t wifi_config = {
        .ap = {
            .ssid = EXAMPLE_ESP_WIFI_SSID,
            .ssid_len = strlen(EXAMPLE_ESP_WIFI_SSID),
            .channel = EXAMPLE_ESP_WIFI_CHANNEL,
            .password = EXAMPLE_ESP_WIFI_PASS,
            .max_connection = EXAMPLE_MAX_STA_CONN,
#ifdef CONFIG_ESP_WIFI_SOFTAP_SAE_SUPPORT
            .authmode = WIFI_AUTH_OPEN,
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
#else /* CONFIG_ESP_WIFI_SOFTAP_SAE_SUPPORT */
            .authmode = WIFI_AUTH_WPA2_PSK,
#endif
            .pmf_cfg = {
                    .required = true,
            },
#ifdef CONFIG_ESP_WIFI_BSS_MAX_IDLE_SUPPORT
            .bss_max_idle_cfg = {
                .period = WIFI_AP_DEFAULT_MAX_IDLE_PERIOD,
                .protected_keep_alive = 1,
            },
#endif
            .gtk_rekey_interval = EXAMPLE_GTK_REKEY_INTERVAL,
        },
    };
    if (strlen(EXAMPLE_ESP_WIFI_PASS) == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init_softap finished. SSID:%s password:%s channel:%d",
             EXAMPLE_ESP_WIFI_SSID, EXAMPLE_ESP_WIFI_PASS, EXAMPLE_ESP_WIFI_CHANNEL);
}

void tcp_server_task(void *pvParameters)
{
    int listen_sock, client_sock;
    struct sockaddr_in server_addr, client_addr;
    socklen_t addr_len = sizeof(client_addr);

    listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (listen_sock < 0) {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        client_status=-2;
        vTaskDelete(NULL);
        return;
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    if (bind(listen_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
        close(listen_sock);
        client_status=-3;
        vTaskDelete(NULL);
        return;
    }

    if (listen(listen_sock, 1) < 0) {
        ESP_LOGE(TAG, "Error during listen: errno %d", errno);
        close(listen_sock);
        client_status=-4;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "TCP server listening on port %d", SERVER_PORT);


    while (1) {
        client_sock = accept(listen_sock, (struct sockaddr *)&client_addr, &addr_len);
        
         if (client_sock < 0) {
            ESP_LOGE(TAG, "Unable to accept connection: errno %d", errno);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        client_status=1;
        ESP_LOGI(TAG, "Client connected");
        char rx_buffer[128];
        int len;
        struct timeval timeout;
        timeout.tv_sec = 50;
        timeout.tv_usec = 0;
        setsockopt(client_sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
         while (1) {
            // --- 1. Прием данных от клиента ---
            len = recv(client_sock, rx_buffer, sizeof(rx_buffer) - 1, 0);
            if (len > 0) {

                rx_buffer[len] = 0; // null terminate
                rx_buffer[strcspn(rx_buffer, "\r\n")] = 0;
                ESP_LOGI(TAG, "Received: %s", rx_buffer);
                client_status=1;
                // --- ОБРАБОТКА КОМАНД ---
                if (strncmp(rx_buffer, "StartWrite", 10) == 0) {
                    // Ожидаем формат: "StartWrite filename.bin"
                    char *name_ptr = rx_buffer + 10;
                    while (*name_ptr == ' ' || *name_ptr == '=') name_ptr++; // Пропускаем пробелы

                    if (strlen(name_ptr) > 0) {
                        // Если имя передано, формируем путь на SD-карте
                        snprintf(active_filename, sizeof(active_filename), "/sdcard/%s", name_ptr);
                    } else {
                        // Если имя не передано, оставляем дефолтное
                        strcpy(active_filename, "/sdcard/datalog.bin");
                    }

                    is_logging_active = true;
                    ESP_LOGI(TAG, "-> START RECORDING to file: %s", active_filename);
                    
                    const char *resp = "OK: START_WRITE\n";
                    send(client_sock, resp, strlen(resp), 0);
                } 
                else if (strcmp(rx_buffer, "StopWrite") == 0) {
                    is_logging_active = false;
                    ESP_LOGI(TAG, "-> STOP RECORDING");
                    
                    const char *resp = "OK: STOP_WRITE\n";
                    send(client_sock, resp, strlen(resp), 0);
                } 
                else if (strcmp(rx_buffer, "LS") == 0) {
                    ESP_LOGI(TAG, "-> COMMAND LS requested");
                    sd_send_file_list(client_sock);
                }
                else if (strcmp(rx_buffer, "Status") == 0) {
                    ESP_LOGI(TAG, "-> COMMAND Status requested");
                    sd_send_status(client_sock);
                }
                else if (strncmp(rx_buffer, "Download", 8) == 0) {
                    // Ожидаем формат: "Download datalog.bin" или просто "Download"
                    char *name_ptr = rx_buffer + 8;
                    while (*name_ptr == ' ' || *name_ptr == '=') name_ptr++; // Пробелы

                    // Если имя не передали — используем active_filename по умолчанию
                    if (strlen(name_ptr) == 0) {
                        name_ptr = active_filename;
                    }

                    ESP_LOGI(TAG, "-> COMMAND Download requested for file: %s", name_ptr);

                    // 1. Принудительно останавливаем запись на время скачивания
                    bool was_logging = is_logging_active; // Сохраняем состояние, если понадобится вернуть
                    is_logging_active = false;
                    vTaskDelay(pdMS_TO_TICKS(150)); // Ждем, пока писатель закроет файлы

                    // 2. Открываем файл для чтения
                    FILE *f = sd_bin_file_open(name_ptr);
                    if (f == NULL) {
                        const char *err = "ERROR: File not found\n";
                        send(client_sock, err, strlen(err), 0);
                        // Если запись была активна до этого, а файл не нашли — можно вернуть флаг обратно
                        if (was_logging) is_logging_active = true; 
                        continue;
                    }

                    // Отправляем клиенту сигнал, что файл найден и начинается передача потока
                    const char *start_msg = "OK: STREAM_START\n";
                    send(client_sock, start_msg, strlen(start_msg), 0);

                    // 3. Буфер для передачи (4 КБ)
                    uint8_t *tx_buffer = malloc(CHUNK_SIZE);
                    if (tx_buffer != NULL) {
                        size_t bytes_read;
                        while ((bytes_read = sd_bin_file_read(f, tx_buffer, CHUNK_SIZE)) > 0) {
                            int sent = send(client_sock, tx_buffer, bytes_read, 0);
                            if (sent < 0) {
                                ESP_LOGE(TAG, "Ошибка отправки данных клиенту (сокет разорван)");
                                break;
                            }
                        }
                        free(tx_buffer);
                    } else {
                        ESP_LOGE(TAG, "Не удалось выделить память под буфер передачи!");
                    }

                    // 4. Закрываем файл
                    sd_bin_file_close(f);
                    ESP_LOGI(TAG, "Файл успешно выгружен клиенту по TCP");

                    // Небольшая задержка перед возможным возобновлением работы
                    vTaskDelay(pdMS_TO_TICKS(100));
                }
                else
                {
                    const char *resp = "ERROR: UNKNOWN_COMMAND\n";
                    send(client_sock, resp, strlen(resp), 0);
                }

            } else if (len == 0) {
                client_status=-1;
                ESP_LOGI(TAG, "Client disconnected");
                break;
            } else if (errno != EWOULDBLOCK && errno != EAGAIN){
                client_status=-1;
                ESP_LOGE(TAG, "recv failed: errno %d", errno);
                break;
            }

            vTaskDelay(pdMS_TO_TICKS(10)); // небольшая пауза
        }
        ESP_LOGI(TAG, "Client disconnected");
        close(client_sock);
    }

    vTaskDelete(NULL);
}


void sd_send_file_list(int sock) {
    char response_buf[512];
    int len = sd_get_file_list_str(response_buf, sizeof(response_buf));
    send(sock, response_buf, len, 0);
}

// Функция отправки статуса и информации о флешке
static void sd_send_status(int sock) {
    char status_buf[256];
    int len = 0;

    if (card != NULL) {
        // Вычисляем размер в мегабайтах из полей структуры CSD карты
        uint64_t total_bytes = ((uint64_t)card->csd.capacity) * card->csd.sector_size;
        uint32_t total_mb = (uint32_t)(total_bytes / (1024 * 1024));

        len = snprintf(status_buf, sizeof(status_buf),
            "STATUS:\n"
            "Card Name: %s\n"
            "Total Size: %lu MB\n"
            "Sector Size: %u\n"
            "Logging Active: %s\n"
            "Active File: %s\n",
            card->cid.name,
            total_mb,
            card->csd.sector_size,
            is_logging_active ? "YES" : "NO",
            active_filename);
    } else {
        len = snprintf(status_buf, sizeof(status_buf),
            "STATUS:\nCard not inserted or not initialized!\nLogging Active: %s\n",
            is_logging_active ? "YES" : "NO");
    }

    send(sock, status_buf, len, 0);
}
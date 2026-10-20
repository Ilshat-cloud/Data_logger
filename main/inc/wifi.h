#ifndef WIFI_H
#define WIFI_H

#define SERVER_IP       "192.168.4.1"
#define MAXIMUM_RETRY   5000
#define SERVER_PORT     3333
#define RECONNECT_DELAY_MS 5000
#define CHUNK_SIZE 4096

void wifi_init_softap(void);
void tcp_server_task(void *pvParameters);



#endif // WIFI_H
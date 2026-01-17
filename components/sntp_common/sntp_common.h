#include <time.h>

typedef void (*sntp_callback_type)(struct timeval *);
void cmn_sntp_init(sntp_callback_type callback);
esp_err_t esp_netif_sntp_sync_wait(TickType_t tout);
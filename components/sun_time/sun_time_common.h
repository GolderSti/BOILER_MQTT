typedef void (*stc_callback_type)(int, int); //out parameter is time of SunSet / SunRise in hours from 00:00

int32_t GetLocalTime();
uint64_t stc_GetStartTime();
int stc_GetSunriseTime();
int stc_GetSunsetTime();
void sun_time_init(stc_callback_type stc_callback);
esp_err_t stc_sync_wait(TickType_t tout);
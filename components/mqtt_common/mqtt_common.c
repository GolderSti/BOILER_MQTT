#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_system.h"
#include "esp_log.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include "freertos/event_groups.h"
#include <freertos/semphr.h> 
#include "esp_netif.h"
#include "mqtt_client.h"
#include "mqtt_common.h"
#include "wifi_common.h"

#define QUEUE_LENGTH 5
#define MQTT_BROKER_MAX_WAIT pdMS_TO_TICKS(30000) //If no broker skip for next data
#define WiFi_SLEEP_DELAY pdMS_TO_TICKS(5000) //Keep WiFi on till next MQTT message
static const char *MQTT_TAG = "/ESP32";               //Topic preamble

static const char *TAG = "MQTTCMN";
static const char *MQTT_LAST_WILL = "OFFLINE";          //If client disconnected for 2 minutes broker send this
static const char *MQTT_ON_CONNECTED = "ONLINE";        //Message on connection established

QueueHandle_t xMQTT_Queue;
static esp_mqtt_client_handle_t g_client;

static int iSubscribedTopicsCnt = 0; // Начальный размер массива
SubscibedHandler *xSubscribedTopics = NULL;   //array for iSubscribedTopics
static SemaphoreHandle_t xSubscribedTopicsMutex = NULL; //

//Event group for MQTT Events
EventGroupHandle_t mqtt_state_event_group;
#define MQTT_CONNECTED_BITS BIT(0)
#define MQTT_CONTINIOUS_WORK

#define MAX_MESSAGE_LENGTH 214
#define WF_STATUS 0
#define WF_NEIGHBOR 1
#define WF_ROUMING 2
static const char *pcTopics[] = {
                                [WF_STATUS]="/WiFi/Status",                  
                                [WF_NEIGHBOR]="/WiFi/Neighbors",              
                                [WF_ROUMING]="/WiFi/Rouming",                    
                                };
#define CONTROL_TOPICS_COUNT 3
static const char *pcControlTopics[] = {
                                        "/WiFi/Status/Get",
                                        "/WiFi/Neighbors/Get",
                                        "/WiFi/Rouming/Set",                    
                                    };

/*!
 * @brief Convert String to int32 via strtol() and checks some errors
 *
 * @param[in] pcMessage         : input string
 *
 * @retval -1 if  there are error
 * @retval >=0 in other cases 
 */
static int32_t cmn_String_to_int32(const char *pcMessage) {
    char *endptr;
    uint32_t uState;

    // pcTmp=malloc(sizeof(pcMessage));
    // memcpy(pcTmp, pcMessage, sizeof(pcMessage));

    uState=strtol(pcMessage, &endptr, 10);
    if (endptr==pcMessage)
    {
        ESP_LOGW(TAG,"\t[MQTT MSG ERR]\t%s",pcMessage);
        return -1;
    } else if (*endptr != '\0')
    {
        ESP_LOGW(TAG,"\t[MQTT MSG ERR]\t%c", *endptr);
        return -1;
    } else 
    {
        return uState;
    }
}


/**
 * @brief Универсальная функция отправки MQTT сообщений
 * @param pcTopic Строка с топиком
 * @param uMsg Числовое сообщение для отправки
 */
void mqtt_common_send_message(const char *pcTopic, uint32_t uMsg)
{
    char pTopic[strlen(pcTopic) + 1];  // Mem alloc for Topic string
    char pMessage[MAX_MESSAGE_LENGTH];
    
    strcpy(pTopic, pcTopic);  
    // Форматируем сообщение
    snprintf(pMessage, MAX_MESSAGE_LENGTH, "%lu", uMsg);
    
    // Отправляем через существующую функцию публикации
    mqtt_Message_Publish(pTopic, pMessage);
}

/*!
 *  @brief Send pMessage and pointer to "MQTT_TAG + pTopic" Via queue to Main MQTT task to publish 
 *
 *  @details 
 *  Send pMessage and pointer to "MQTT_TAG + pTopic" Via queue to Main MQTT task to publish. 
 *  Free it in case of error, otherwise it is freed in MQTT task
 * 
 *  @param[in] pTopic    : Topic
 *  @param[in] pMessage  : Message
 *  @param[in] bool      : TAG
 *
 */
void mqtt_Message_Publish_TAG(const char * pTopic, const char* pMessage, bool bTag)
{
    MQTT_Queue_Data_t xDataToSend;
    BaseType_t xStatus;

    if (pTopic==NULL)
    {
        ESP_LOGW(TAG,"\tTrying to publish to NULL topic.");
        return;
    }

    if (pMessage==NULL)
    {
        ESP_LOGW(TAG,"\tTrying to publish to NULL message.");
        return;
    }
    const char *pTAG;
    if (bTag)
    {
        pTAG=MQTT_TAG;
    }else
    {
        pTAG="";
    }
    
    
    
    xDataToSend.pTopic=malloc(strlen(pTopic) + strlen(pTAG) + 1);
    if (xDataToSend.pTopic != NULL) {
        strcpy(xDataToSend.pTopic,pTAG);
        strcat(xDataToSend.pTopic,pTopic);
        // xDataToSend.pTopic=pTopic;
        xDataToSend.pMessage=malloc(strlen(pMessage)+1);
        if(xDataToSend.pMessage != NULL){
            strcpy(xDataToSend.pMessage, pMessage);
            xStatus = xQueueSendToBack(xMQTT_Queue, &xDataToSend, 0 );
            switch (xStatus)
            {
                // pdPASS = 0,                                    /*!< Function execution successful */
                case pdPASS:
                    //Free resources in queue processing function
                    ESP_LOGD(TAG,"\tMessage sended to MQTT Queue");
                    break;
                default:
                    //Have an error. Free resources here, becouse thay can't be freed in queue processing function
                    free(xDataToSend.pTopic);
                    free(xDataToSend.pMessage);
                    ESP_LOGE(TAG,"\tMQTT Queue Error [%d] : Bufer is full\r\n", xStatus);
                    break;
            }
        }else
        {
            ESP_LOGE(TAG,"\tNot enouth memory for Message String");
            free(xDataToSend.pTopic);
        }
    }else
    {
        ESP_LOGE(TAG,"\tNot enouth memory for Topic String");
    }
    
}

/*!
 *  @brief Send pMessage and pointer to "MQTT_TAG + pTopic" Via queue to Main MQTT task to publish 
 *
 *  @details 
 *  Send pMessage and pointer to "MQTT_TAG + pTopic" Via queue to Main MQTT task to publish. 
 *  Free it in case of error, otherwise it is freed in MQTT task
 * 
 *  @param[in] pTopic    : Topic
 *  @param[in] pMessage  : Message
 *
 */
void mqtt_Message_Publish(const char * pTopic, const char* pMessage)
{
    mqtt_Message_Publish_TAG(pTopic, pMessage, pdTRUE);
}

/*!
 * @brief Run when MQTT client receive MSG in //WiFi/Status/Get topic
 *
 * @param[in] pcMessage         : contain data that was received
 *
 */
static void MQTT_Get_Info_Callback(const char *pcMessage) {
    int32_t uState;
    char pMessage[MAX_MESSAGE_LENGTH];
    
    uState = cmn_String_to_int32(pcMessage);
    if (uState == 1) {
        wifi_status_info_t status_info;
        if (WFc_GetCurrentStatus(&status_info) == ESP_OK) {
            // Форматируем MAC-адрес устройства в строку
            char mac_str[18]; // 6 байт по 2 символа + 5 двоеточий + нуль-терминатор
            snprintf(mac_str, sizeof(mac_str), 
                     "%02x:%02x:%02x:%02x:%02x:%02x",
                     status_info.sta_mac[0], status_info.sta_mac[1],
                     status_info.sta_mac[2], status_info.sta_mac[3],
                     status_info.sta_mac[4], status_info.sta_mac[5]);

            // Форматируем MAC-адрес AP в строку
            char bssid_str[18]; // 6 байт по 2 символа + 5 двоеточий + нуль-терминатор
            snprintf(bssid_str, sizeof(bssid_str), 
                     "%02x:%02x:%02x:%02x:%02x:%02x",
                     status_info.bssid[0], status_info.bssid[1],
                     status_info.bssid[2], status_info.bssid[3],
                     status_info.bssid[4], status_info.bssid[5]);
            
            // Форматируем IP-адреса
            char ip_str[16], gw_str[16], mask_str[16];
            snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&status_info.ip_addr));
            snprintf(gw_str, sizeof(gw_str), IPSTR, IP2STR(&status_info.gateway));
            snprintf(mask_str, sizeof(mask_str), IPSTR, IP2STR(&status_info.netmask));
            
            // Конвертируем тип аутентификации в строку
            const char *auth_str;
            switch (status_info.auth_mode) {
                case WIFI_AUTH_OPEN:
                    auth_str = "OPEN";
                    break;
                case WIFI_AUTH_WEP:
                    auth_str = "WEP";
                    break;
                case WIFI_AUTH_WPA_PSK:
                    auth_str = "WPA_PSK";
                    break;
                case WIFI_AUTH_WPA2_PSK:
                    auth_str = "WPA2_PSK";
                    break;
                case WIFI_AUTH_WPA_WPA2_PSK:
                    auth_str = "WPA_WPA2_PSK";
                    break;
                case WIFI_AUTH_WPA2_ENTERPRISE:
                    auth_str = "WPA2_ENTERPRISE";
                    break;
                case WIFI_AUTH_WPA3_PSK:
                    auth_str = "WPA3_PSK";
                    break;
                case WIFI_AUTH_WPA2_WPA3_PSK:
                    auth_str = "WPA2_WPA3_PSK";
                    break;
                // case WIFI_AUTH_WAPI_PSK:
                //     auth_str = "WAPI_PSK";
                //     break;
                default:
                    auth_str = "UNKNOWN";
                    break;
            }
            
            // Формируем JSON-строку
            snprintf(pMessage, MAX_MESSAGE_LENGTH,
                     "{\"MAC\": \"%s\",\"RSSI\": %d,\"bSSID\": \"%s\",\"SSID\": \"%s\","
                     "\"IP\": \"%s\",\"GW\": \"%s\",\"Mask\": \"%s\","
                     "\"Auth\": \"%s\"}",
                     mac_str,
                     status_info.rssi,
                     bssid_str,
                     status_info.ssid,
                     ip_str,
                     gw_str,
                     mask_str,
                     auth_str);
            
            // Отправляем сообщение в MQTT
            mqtt_Message_Publish(pcTopics[WF_STATUS], pMessage);
            ESP_LOGI(TAG, "WiFi status published: %s", pMessage);
        } else {
            ESP_LOGE(TAG, "Failed to get WiFi status");
        }
    } else {
        ESP_LOGW(TAG, "[/WiFi/Status/Get] Invalid state: %d", uState);
    }
}

/*!
 * @brief Run when MQTT client receive MSG in /WiFi/Neighbors/Get topic
 *
 * @param[in] pcMessage         : contain data that was received
 *
 */
static void MQTT_Get_Neighbors_Callback(const char *pcMessage) {
    int32_t uState;
    char pMessage[MAX_MESSAGE_LENGTH];
    
    uState = cmn_String_to_int32(pcMessage);
    if (uState == 1) {
        wifi_neighbor_scan_result_t scan_result;
        
        // Выполняем синхронное сканирование соседних AP
        esp_err_t ret = WFc_ScanNeighborAPs(&scan_result, false);
        
        if (ret == ESP_OK && scan_result.scan_status == ESP_OK) {
            ESP_LOGI(TAG, "Found %d neighbor APs", scan_result.ap_count);
            
            // Формируем начало JSON строки
            int pos = 0;
            pos = snprintf(pMessage, MAX_MESSAGE_LENGTH, "{");
            
            // Обрабатываем до 7 AP или меньше, если их меньше
            int ap_limit = (scan_result.ap_count < 7) ? scan_result.ap_count : 7;
            
            for (int i = 0; i < ap_limit && pos < MAX_MESSAGE_LENGTH - 50; i++) {
                // Форматируем MAC-адрес
                char bssid_str[18];
                snprintf(bssid_str, sizeof(bssid_str), 
                         "%02x:%02x:%02x:%02x:%02x:%02x",
                         scan_result.aps[i].bssid[0],
                         scan_result.aps[i].bssid[1],
                         scan_result.aps[i].bssid[2],
                         scan_result.aps[i].bssid[3],
                         scan_result.aps[i].bssid[4],
                         scan_result.aps[i].bssid[5]);
                
                // Добавляем запятую между элементами, кроме первого
                if (i > 0) {
                    pos += snprintf(pMessage + pos, MAX_MESSAGE_LENGTH - pos, ",");
                }
                
                // Добавляем элемент в JSON
                pos += snprintf(pMessage + pos, MAX_MESSAGE_LENGTH - pos, 
                               "\"%d\":\"%s\"", i + 1, bssid_str);
            }
            
            // Завершаем JSON строку
            if (pos < MAX_MESSAGE_LENGTH - 2) {
                pos += snprintf(pMessage + pos, MAX_MESSAGE_LENGTH - pos, "}");
            }
            
            // Отправляем сообщение в MQTT
            mqtt_Message_Publish(pcTopics[WF_NEIGHBOR], pMessage);
            ESP_LOGI(TAG, "Neighbor APs published: %s", pMessage);
            
        } else {
            ESP_LOGE(TAG, "Failed to scan neighbor APs: ret=%d, scan_status=%d", 
                     ret, scan_result.scan_status);
            
            // Отправляем пустой JSON в случае ошибки
            snprintf(pMessage, MAX_MESSAGE_LENGTH, "{}");
            mqtt_Message_Publish(pcTopics[WF_NEIGHBOR], pMessage);
        }
    } else {
        ESP_LOGW(TAG, "[MQTT Get Neighbors] Invalid state: %d", uState);
    }
}

/*!
 * @brief Run when MQTT client receive MSG in /WiFi/Rouming/Set topic
 *
 * @param[in] pcMessage         : contain data that was received
 *
 */
static void MQTT_Set_Rouming_Callback(const char *pcMessage) {
//
}

/*!
 *  @brief Re-subscribe to all registered topics after reconnection
 * 
 */
static void mqtt_Resubscribe_All_Topics(void) {
    if (xSubscribedTopicsMutex) {
        xSemaphoreTake(xSubscribedTopicsMutex, portMAX_DELAY);
    }
    
    for (int i = 0; i < iSubscribedTopicsCnt; i++) {
        int msg_id = esp_mqtt_client_subscribe(g_client, xSubscribedTopics[i].pcTopic, 0);
        if (msg_id == -1) {
            ESP_LOGE(TAG, "\tBroker rejected subscription for %s", xSubscribedTopics[i].pcTopic);
        } else {
            ESP_LOGD(TAG, "\t[Re-subscribed]\tTopic:%s; msg_id=%d", xSubscribedTopics[i].pcTopic, msg_id);
        }
    }
    
    if (xSubscribedTopicsMutex) {
        xSemaphoreGive(xSubscribedTopicsMutex);
    }
}

/*!
 *  @brief Subsribes to pcTopic and add CallBack to run when it fires 
 * 
 *  @details
 *  If MQTT is connected, subscribe immediately. Otherwise store topic 
 *  for subscription when connection is established.
 * 
 *  @param[in] pcTopic     : Topic to subsribe to
 *  @param[in] callback    : Callback that will be fired when message in this topic will be received
 *
 */
void mqtt_Topic_Subsribe(const char *pcTopic, SubscibedCallback callback) {
    if (xSubscribedTopicsMutex == NULL) {
        xSubscribedTopicsMutex = xSemaphoreCreateMutex();
        if (xSubscribedTopicsMutex == NULL) {
            ESP_LOGE(TAG, "\tFailed to create mutex for subscribed topics");
            return;
        }
    }
    
    // Захватываем мьютекс для безопасного доступа к массиву
    if (xSemaphoreTake(xSubscribedTopicsMutex, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "\tFailed to take mutex");
        return;
    }
    
    // Увеличиваем массив для хранения топиков и их callback'ов
    SubscibedHandler *tmp = realloc(xSubscribedTopics, (iSubscribedTopicsCnt + 1) * sizeof(SubscibedHandler));
    if (tmp == NULL) {
        ESP_LOGE(TAG, "\tNot enough memory for realloc xSubscribedTopics");
        xSemaphoreGive(xSubscribedTopicsMutex);
        return;
    }
    
    xSubscribedTopics = tmp;
    
    // Выделяем память для нового топика
    xSubscribedTopics[iSubscribedTopicsCnt].pcTopic = malloc(strlen(pcTopic) + strlen(MQTT_TAG) + 1);
    if (xSubscribedTopics[iSubscribedTopicsCnt].pcTopic == NULL) {
        ESP_LOGE(TAG, "\tNot enough memory for Topic String");
        // Уменьшаем размер массива обратно
        tmp = realloc(xSubscribedTopics, iSubscribedTopicsCnt * sizeof(SubscibedHandler));
        if (tmp != NULL) {
            xSubscribedTopics = tmp;
        }
        xSemaphoreGive(xSubscribedTopicsMutex);
        return;
    }
    
    // Формируем полное имя топика
    strcpy(xSubscribedTopics[iSubscribedTopicsCnt].pcTopic, MQTT_TAG);
    strcat(xSubscribedTopics[iSubscribedTopicsCnt].pcTopic, pcTopic);
    xSubscribedTopics[iSubscribedTopicsCnt].callback = callback;
    
    // Проверяем, подключены ли мы к брокеру
    EventBits_t uxBits = xEventGroupGetBits(mqtt_state_event_group);
    bool is_connected = ((uxBits & MQTT_CONNECTED_BITS) != 0);
    
    if (is_connected && g_client != NULL) {
        // Если подключены - подписываемся сразу
        int msg_id = esp_mqtt_client_subscribe(g_client, xSubscribedTopics[iSubscribedTopicsCnt].pcTopic, 0);
        if (msg_id == -1) {
            ESP_LOGE(TAG, "\tBroker rejected subscription for %s", xSubscribedTopics[iSubscribedTopicsCnt].pcTopic);
            // Освобождаем память и удаляем элемент
            free(xSubscribedTopics[iSubscribedTopicsCnt].pcTopic);
            // Уменьшаем размер массива
            tmp = realloc(xSubscribedTopics, iSubscribedTopicsCnt * sizeof(SubscibedHandler));
            if (tmp != NULL) {
                xSubscribedTopics = tmp;
            }
        } else {
            ESP_LOGD(TAG, "\t[Subscribed]\tTopic:%s; msg_id=%d", xSubscribedTopics[iSubscribedTopicsCnt].pcTopic, msg_id);
            iSubscribedTopicsCnt++;
        }
    } else {
        // Если не подключены - просто сохраняем топик, подпишемся позже
        ESP_LOGD(TAG, "\t[Topic stored]\tTopic:%s (will subscribe when connected)", 
                xSubscribedTopics[iSubscribedTopicsCnt].pcTopic);
        iSubscribedTopicsCnt++;
    }
    
    xSemaphoreGive(xSubscribedTopicsMutex);
}

/*!
 *  @brief Unsubscribe from topic and remove it from array
 * 
 *  @param[in] pcTopic     : Topic to unsubscribe from
 *
 */
void mqtt_Topic_Unsubscribe(const char *pcTopic) {
    if (xSubscribedTopicsMutex == NULL || xSubscribedTopics == NULL) {
        return;
    }
    
    if (xSemaphoreTake(xSubscribedTopicsMutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    
    char full_topic[strlen(pcTopic) + strlen(MQTT_TAG) + 1];
    strcpy(full_topic, MQTT_TAG);
    strcat(full_topic, pcTopic);
    
    // Ищем топик в массиве
    int index_to_remove = -1;
    for (int i = 0; i < iSubscribedTopicsCnt; i++) {
        if (strcmp(xSubscribedTopics[i].pcTopic, full_topic) == 0) {
            index_to_remove = i;
            break;
        }
    }
    
    if (index_to_remove != -1) {
        // Отписываемся от топика, если подключены
        EventBits_t uxBits = xEventGroupGetBits(mqtt_state_event_group);
        bool is_connected = ((uxBits & MQTT_CONNECTED_BITS) != 0);
        
        if (is_connected && g_client != NULL) {
            esp_mqtt_client_unsubscribe(g_client, xSubscribedTopics[index_to_remove].pcTopic);
        }
        
        // Освобождаем память
        free(xSubscribedTopics[index_to_remove].pcTopic);
        
        // Сдвигаем остальные элементы
        for (int i = index_to_remove; i < iSubscribedTopicsCnt - 1; i++) {
            xSubscribedTopics[i] = xSubscribedTopics[i + 1];
        }
        
        iSubscribedTopicsCnt--;
        
        // Уменьшаем размер массива
        SubscibedHandler *tmp = realloc(xSubscribedTopics, iSubscribedTopicsCnt * sizeof(SubscibedHandler));
        if (tmp != NULL || iSubscribedTopicsCnt == 0) {
            xSubscribedTopics = tmp;
        }
        
        ESP_LOGD(TAG, "\t[Unsubscribed]\tTopic:%s", full_topic);
    }
    
    xSemaphoreGive(xSubscribedTopicsMutex);
}

/*!
 *  @brief Callback for all MQTT events 
 * 
 *  @details
 *  Call registered CallBack in case of resiving data in subsribed topics
 *  Set and Clear MQTT_CONNECTED_BITS
 *  Log other events to COM
 * 
 *  @param[in] event    : MQTT event handl
 *
 */
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {

    int msg_id;
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    ESP_LOGD(TAG, "\t[MQTT_cb]\tEvent dispatched from event loop base=%s, event_id=%d", base, event_id);
    // esp_mqtt_client_handle_t client = event->client;
    // your_context_t *context = event->context;
    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
            msg_id = esp_mqtt_client_publish(g_client, MQTT_TAG, MQTT_ON_CONNECTED, 0, 1, 1);
            ESP_LOGI(TAG, "\t[ONLINE]\t\"%s\" publish successful, msg_id=%d", MQTT_ON_CONNECTED, msg_id);

            xEventGroupSetBits(mqtt_state_event_group, MQTT_CONNECTED_BITS);
            ESP_LOGI(TAG, "\tCONNECTED to Brocker");
            mqtt_Resubscribe_All_Topics();

            break;
        case MQTT_EVENT_DISCONNECTED:
            xEventGroupClearBits(mqtt_state_event_group, MQTT_CONNECTED_BITS);
            ESP_LOGI(TAG, "\tDISCONNECTED from Broker");
            break;

        case MQTT_EVENT_SUBSCRIBED:
            ESP_LOGD(TAG, "\tSUBSCRIBED, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_UNSUBSCRIBED:
            ESP_LOGD(TAG, "\tUNSUBSCRIBED, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_PUBLISHED:
            ESP_LOGD(TAG, "\tMessage PUBLISHED, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_DATA:
            ESP_LOGD(TAG, "\tMessage Received");
            char *pcTopic;
            pcTopic=malloc(event->topic_len+1);
            if (pcTopic!=NULL)
            {
                char *pcMessage;
                pcMessage=malloc(event->data_len+1);
                if (pcMessage!=NULL)
                {
                    memcpy(pcTopic,event->topic, event->topic_len);
                    pcTopic[event->topic_len]=0;
                    ESP_LOGD(TAG, "\t[MQTT get topic]\t%s",pcTopic);
                    memcpy(pcMessage,event->data, event->data_len);
                    pcMessage[event->data_len]=0;
                    ESP_LOGD(TAG, "\t[MQTT get data ]\t%s",pcMessage);
                    //check all topics and call callback functions 
                    int32_t uTopicNumber=0;
                    for (uTopicNumber = 0; uTopicNumber < iSubscribedTopicsCnt; uTopicNumber++) {
                        if (strcmp(xSubscribedTopics[uTopicNumber].pcTopic, pcTopic) == 0) {
                            xSubscribedTopics[uTopicNumber].callback(pcMessage);  // Вызов callback s
                            ESP_LOGD(TAG, "\t[Callback #]\t%d",uTopicNumber);
                        }
                    }
                    free(pcMessage);
                }                
                free(pcTopic);                
            }  
            break;         
        case MQTT_EVENT_ERROR:
            ESP_LOGW(TAG, "\t[MQTT_cb]\tMQTT_EVENT_ERROR");
            break;
        default:
            ESP_LOGI(TAG, "\t[MQTT_cb]\tOther event id:%d", event->event_id);
            break;
    }
}

/*!
 *  @brief MQTT Main Task 
 * 
 *  @details
 *  Wait data in MQTT Queue, wait for broker connecton, send message wait for some time and then in case if of WiFi need to be turn off do it 
 * 
 *  @param[in] pvParameters    : doesn't use
 *
 */
void mqtt_maintask(void *pvParameters)
{
    MQTT_Queue_Data_t xReceivedData;
    EventBits_t uxBits;

#ifndef MQTT_CONTINIOUS_WORK
    //Wait while we will be connected to the broker
    uxBits = xEventGroupWaitBits(mqtt_state_event_group, MQTT_CONNECTED_BITS, false, false, portMAX_DELAY);
    if ((uxBits & MQTT_CONNECTED_BITS)!=0)
    {//if we don't want WiFi to work continiously, stop mqtt and wifi 
        esp_mqtt_client_stop(g_client);

        WFc_Stop();
    }
#endif

    while (1)//mqtt_needed
    {
        ESP_LOGD(TAG, "\tWait for DATA_READY_FOR_MQTT");
        if (xQueueReceive( xMQTT_Queue, &xReceivedData, portMAX_DELAY )==pdPASS)
        {            
            #ifndef MQTT_CONTINIOUS_WORK
            WFc_Start();
            if (WFc_ConnectionWait(MQTT_BROKER_MAX_WAIT)==ESP_OK)
            {           
                esp_mqtt_client_start(g_client);
            }
            #endif

            //check if we hav connection to Brocker
            uxBits = xEventGroupWaitBits(mqtt_state_event_group, MQTT_CONNECTED_BITS, false, false, MQTT_BROKER_MAX_WAIT);
            if ((uxBits & MQTT_CONNECTED_BITS)!=0)
            {
                //connected
                ESP_LOGD(TAG, "\t[WIFI_MQTT]\tGot broker"); 
                ESP_LOGV(TAG, "\t[WIFI_MQTT]\tTrying to publish");
                int msg_id;
                do
                {                      
                    //main procedure
                    ESP_LOGI(TAG,"\tSending to:\t%s",xReceivedData.pTopic);
                    msg_id = esp_mqtt_client_publish(g_client, xReceivedData.pTopic, xReceivedData.pMessage, 0, 1, 1);                    
                    free(xReceivedData.pTopic);
                    free(xReceivedData.pMessage);
                    if (msg_id==-1)
                    {
                        ESP_LOGW(TAG,"\tError puplish message to MQTT");
                    }else
                    {
                        ESP_LOGD(TAG, "\tMessage sended succsess, msg_id=%d", msg_id);
                    }
                } while (xQueueReceive(xMQTT_Queue, &xReceivedData, WiFi_SLEEP_DELAY) == pdPASS);
            }
            else
            {
                //no mqtt server
                // u8g2_DrawStrln(&u8g2, "MQTT no broker");
                ESP_LOGI(TAG, "\t[MainTask]\tNo Broker"); 
                xQueueReset(xMQTT_Queue);
            }
            //messaged have been sended
            //need to stop wifi!
            #ifndef MQTT_CONTINIOUS_WORK
            esp_mqtt_client_stop(g_client);

            WFc_Stop();
            #endif
        }
        else
        {
            ESP_LOGE(TAG,"\t[xQueueReceive]\tFAIL");
        }
        
    }
      
    ESP_LOGI(TAG, "\t[WIFI        ]\tStop MQTT"); 
    esp_mqtt_client_stop(g_client);
    esp_mqtt_client_destroy(g_client);

    WFc_Stop();
    vTaskDelete(NULL);
}

/*!
 *  @brief Initialize MQTT Client 
 * 
 *
 */
void mqtt_init()
{
    if (xMQTT_Queue==NULL)
    {
        xMQTT_Queue = xQueueCreate( 18, sizeof( MQTT_Queue_Data_t ) );
        mqtt_state_event_group = xEventGroupCreate();
        WFc_Start();
        WFc_ConnectionWait(portMAX_DELAY);
        //come here only if we had wifi connected
        //init MQTT client and connect to brocker
        esp_mqtt_client_config_t mqtt_cfg = {
            .broker.address.uri = CONFIG_BROKER_URL,
            
            .session.last_will.topic = MQTT_TAG,
            .session.last_will.msg = MQTT_LAST_WILL,
            .session.last_will.qos = 1,                            /*!< LWT message qos */
            .session.last_will.retain = 1,                         /*!< LWT retained message flag */
            .session.last_will.msg_len = strlen(MQTT_LAST_WILL),   /*!< LWT message length */
        };

        g_client = esp_mqtt_client_init(&mqtt_cfg);
        esp_mqtt_client_register_event(g_client, ESP_EVENT_ANY_ID, mqtt_event_handler, g_client);

        ESP_LOGI(TAG, "\tMQTT Starting...");
        esp_mqtt_client_start(g_client);
        ESP_LOGD(TAG, "\tStarted. Free memory:%d bytes", esp_get_free_heap_size());
        // ESP_LOGI(TAG, "\t[WIFI    MQTT]\tClient started. wait broker"); 
        mqtt_Topic_Subsribe(pcControlTopics[0],&MQTT_Get_Info_Callback);
        mqtt_Topic_Subsribe(pcControlTopics[1],&MQTT_Get_Neighbors_Callback);
        mqtt_Topic_Subsribe(pcControlTopics[2],&MQTT_Set_Rouming_Callback);

        xTaskCreate(mqtt_maintask, "mqtt_maintask", configMINIMAL_STACK_SIZE * 3, NULL, 5, NULL);
    }else
    {
        ESP_LOGI(TAG,"MQTT have been alreay initialized");
    }
    
    
}

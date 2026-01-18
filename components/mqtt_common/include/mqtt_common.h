
// Subsribed topics callback
typedef void (*SubscibedCallback)(const char *response);

// MQTT Topics to Subsribe and it's response functions
typedef struct {
    char *pcTopic;             // Subscribed Topic
    SubscibedCallback callback; // Subscribed Topic Callback
} SubscibedHandler;


typedef struct
{
    char * pMessage;
    char * pTopic;
} MQTT_Queue_Data_t;

void mqtt_Message_Publish(const char * pTopic, const char* pMessage);
void mqtt_Message_Publish_TAG(const char * pTopic, const char* pMessage, bool bTag);
void mqtt_init();
void mqtt_Topic_Subsribe(const char *pcTopic, SubscibedCallback callback);
void mqtt_common_send_message(const char *pcTopic, uint32_t uMsg);
void mqtt_Topic_Unsubscribe(const char *pcTopic);

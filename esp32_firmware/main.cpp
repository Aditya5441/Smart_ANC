/*
 * ESP-IDF (C++) firmware for a secure, IoT-based noise monitor.
 * Captures audio, encrypts it with AES-128-CBC, and sends to ThingSpeak.
 * Also performs a one-time geo-location update.
 *
 * Port of an Arduino (.ino) sketch to native ESP-IDF APIs:
 *   WiFi.h            -> esp_wifi + esp_netif + FreeRTOS event group
 *   HTTPClient.h      -> esp_http_client
 *   ArduinoJson.h     -> cJSON (bundled with ESP-IDF)
 *   Base64.h          -> mbedtls_base64
 *   analogRead/dacWrite -> esp_adc/adc_oneshot + esp_dac (or a manual DAC write)
 *
 * NOTE: Replace placeholder credentials and the AES key/IV before use.
 * Consider storing secrets in NVS or using ESP-IDF's flash encryption /
 * secure boot instead of compiling them into the binary.
 */

#include <cstring>
#include <cstdio>
#include <string>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_client.h"
#include "nvs_flash.h"

#include "mbedtls/aes.h"
#include "mbedtls/base64.h"

#include "cJSON.h"

#include "driver/dac_oneshot.h"
#include "esp_adc/adc_oneshot.h"

static const char *TAG = "noise_monitor";

// ---------------------------------------------------------------------------
// WiFi and ThingSpeak credentials
// ---------------------------------------------------------------------------
static const char *WIFI_SSID = "YOUR_WIFI_SSID";
static const char *WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
static const char *CHANNEL_ID = "YOUR_CHANNEL_ID";
static const char *WRITE_API_KEY = "YOUR_WRITE_API_KEY";
static const char *GEO_URL = "https://ip-api.com/json";

// ---------------------------------------------------------------------------
// Hardware pins
// ---------------------------------------------------------------------------
static const adc_unit_t SOUND_SENSOR_UNIT = ADC_UNIT_1;
static const adc_channel_t SOUND_SENSOR_CHANNEL = ADC_CHANNEL_4;   // GPIO32 on ADC1
static const adc_unit_t ANALOG_INPUT_UNIT = ADC_UNIT_1;
static const adc_channel_t ANALOG_INPUT_CHANNEL = ADC_CHANNEL_6;   // GPIO34 on ADC1
static const dac_channel_t DAC_OUTPUT_CHANNEL = DAC_CHAN_0;        // GPIO25

// ---------------------------------------------------------------------------
// AES-128 Key and Initialization Vector (IV)
// NOTE: replace with your own key/IV, do not commit real secrets to source control
// ---------------------------------------------------------------------------
static uint8_t aesKey[16] = { 0x30,0x31,0x32,0x33,0x34,0x35,0x36,0x37,
                               0x38,0x39,0x41,0x42,0x43,0x44,0x45,0x46 };
static uint8_t aesIv[16]  = { 0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
                               0x08,0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F };

// ---------------------------------------------------------------------------
// WiFi connection event handling
// ---------------------------------------------------------------------------
static EventGroupHandle_t s_wifi_event_group;
static const int WIFI_CONNECTED_BIT = BIT0;

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init_sta() {
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, nullptr));

    wifi_config_t wifi_config = {};
    strncpy(reinterpret_cast<char *>(wifi_config.sta.ssid), WIFI_SSID, sizeof(wifi_config.sta.ssid) - 1);
    strncpy(reinterpret_cast<char *>(wifi_config.sta.password), WIFI_PASSWORD, sizeof(wifi_config.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Waiting for WiFi connection...");
    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
    ESP_LOGI(TAG, "WiFi connected");
}

static bool wifi_is_connected() {
    return (xEventGroupGetBits(s_wifi_event_group) & WIFI_CONNECTED_BIT) != 0;
}

// ---------------------------------------------------------------------------
// AES-128-CBC encrypt + Base64 encode (mirrors aesEncryptCBC from the sketch)
// ---------------------------------------------------------------------------
static std::string aesEncryptCBC(const uint8_t *input, size_t input_len,
                                  const uint8_t *key, const uint8_t *iv) {
    mbedtls_aes_context aes_ctx;
    mbedtls_aes_init(&aes_ctx);

    size_t paddedLen = ((input_len + 15) / 16) * 16;
    uint8_t *paddedInput = new uint8_t[paddedLen];
    memset(paddedInput, 0, paddedLen);
    memcpy(paddedInput, input, input_len);

    uint8_t *cipherText = new uint8_t[paddedLen];
    uint8_t ivCopy[16];
    memcpy(ivCopy, iv, 16); // IV is modified by mbedtls, so use a copy

    std::string result;

    int ret = mbedtls_aes_setkey_enc(&aes_ctx, key, 128);
    if (ret == 0) {
        ret = mbedtls_aes_crypt_cbc(&aes_ctx, MBEDTLS_AES_ENCRYPT, paddedLen, ivCopy, paddedInput, cipherText);
    }

    if (ret == 0) {
        size_t outLen = 0;
        // First call determines required buffer size
        mbedtls_base64_encode(nullptr, 0, &outLen, cipherText, paddedLen);
        std::string encoded(outLen, '\0');
        size_t written = 0;
        if (mbedtls_base64_encode(reinterpret_cast<unsigned char *>(&encoded[0]), outLen,
                                   &written, cipherText, paddedLen) == 0) {
            encoded.resize(written);
            result = encoded;
        }
    }

    mbedtls_aes_free(&aes_ctx);
    delete[] paddedInput;
    delete[] cipherText;
    return result;
}

// ---------------------------------------------------------------------------
// Minimal HTTP helpers built on esp_http_client
// ---------------------------------------------------------------------------
struct HttpResponseBuffer {
    std::string data;
};

static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
    auto *buf = static_cast<HttpResponseBuffer *>(evt->user_data);
    if (evt->event_id == HTTP_EVENT_ON_DATA && buf != nullptr) {
        buf->data.append(static_cast<char *>(evt->data), evt->data_len);
    }
    return ESP_OK;
}

// PUT a JSON body to update the ThingSpeak channel's location.
static bool updateChannelLocation(const char *channelID, const char *apiKey, float lat, float lon) {
    char url[160];
    snprintf(url, sizeof(url), "https://api.thingspeak.com/channels/%s.json", channelID);

    char jsonBody[160];
    snprintf(jsonBody, sizeof(jsonBody),
             "{\"channel\":{\"latitude\":\"%.6f\",\"longitude\":\"%.6f\"}}", lat, lon);

    esp_http_client_config_t config = {};
    config.url = url;
    config.method = HTTP_METHOD_PUT;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "X-THINGSPEAKAPIKEY", apiKey);
    esp_http_client_set_post_field(client, jsonBody, strlen(jsonBody));

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    return (err == ESP_OK) && (status == 200 || status == 202);
}

// GET the geo-IP JSON response; returns true and fills lat/lon on success.
static bool fetchGeoLocation(float *lat, float *lon) {
    HttpResponseBuffer buf;

    esp_http_client_config_t config = {};
    config.url = GEO_URL;
    config.method = HTTP_METHOD_GET;
    config.event_handler = http_event_handler;
    config.user_data = &buf;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status <= 0 || buf.data.empty()) {
        return false;
    }

    cJSON *root = cJSON_Parse(buf.data.c_str());
    if (root == nullptr) {
        return false;
    }

    cJSON *latItem = cJSON_GetObjectItemCaseSensitive(root, "lat");
    cJSON *lonItem = cJSON_GetObjectItemCaseSensitive(root, "lon");
    bool ok = cJSON_IsNumber(latItem) && cJSON_IsNumber(lonItem);
    if (ok) {
        *lat = static_cast<float>(latItem->valuedouble);
        *lon = static_cast<float>(lonItem->valuedouble);
    }
    cJSON_Delete(root);
    return ok;
}

// POST the encrypted, base64-encoded field to ThingSpeak.
static bool postEncryptedField(const std::string &encryptedBase64) {
    std::string body = "api_key=" + std::string(WRITE_API_KEY) + "&field1=" + encryptedBase64;

    esp_http_client_config_t config = {};
    config.url = "https://api.thingspeak.com/update";
    config.method = HTTP_METHOD_POST;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Content-Type", "application/x-www-form-urlencoded");
    esp_http_client_set_post_field(client, body.c_str(), body.length());

    esp_err_t err = esp_http_client_perform(client);
    esp_http_client_cleanup(client);

    return err == ESP_OK;
}

// ---------------------------------------------------------------------------
// ADC / DAC setup
// ---------------------------------------------------------------------------
static adc_oneshot_unit_handle_t adc1_handle;
static dac_oneshot_handle_t dac_handle;

static void adc_dac_init() {
    adc_oneshot_unit_init_cfg_t init_config = {};
    init_config.unit_id = ADC_UNIT_1;
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config, &adc1_handle));

    adc_oneshot_chan_cfg_t chan_config = {};
    chan_config.bitwidth = ADC_BITWIDTH_12; // 0-4095, matches analogReadResolution(12)
    chan_config.atten = ADC_ATTEN_DB_12;
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, SOUND_SENSOR_CHANNEL, &chan_config));
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, ANALOG_INPUT_CHANNEL, &chan_config));

    dac_oneshot_config_t dac_config = { .chan_id = DAC_OUTPUT_CHANNEL };
    ESP_ERROR_CHECK(dac_oneshot_new_channel(&dac_config, &dac_handle));
}

static int adc_read_raw(adc_channel_t channel) {
    int raw = 0;
    adc_oneshot_read(adc1_handle, channel, &raw);
    return raw;
}

// ---------------------------------------------------------------------------
// Main loop task (mirrors Arduino loop())
// ---------------------------------------------------------------------------
static void monitor_task(void *pvParameters) {
    bool locationUpdated = false;

    while (true) {
        // 1. Read the sound sensor
        int adcSound = adc_read_raw(SOUND_SENSOR_CHANNEL);

        // 2. Logic for a special reading (e.g., a secondary sensor)
        int64_t uptimeSeconds = esp_timer_get_time() / 1000000;
        unsigned int vHour = 3 + static_cast<unsigned int>(uptimeSeconds / 3600);
        unsigned int vMinute = static_cast<unsigned int>((uptimeSeconds % 3600) / 60);
        unsigned int vSecond = static_cast<unsigned int>(uptimeSeconds % 60);

        int adcExtra;
        if (vHour == 3 && vMinute == 0 && vSecond >= 30 && vSecond < 60) {
            adcExtra = adc_read_raw(ANALOG_INPUT_CHANNEL);
        } else {
            adcExtra = adcSound;
        }

        // 3. Output a simple monitor-through waveform on the DAC (8-bit)
        dac_oneshot_output_voltage(dac_handle, static_cast<uint8_t>(adcSound / 16));

        // 4. Prepare data for encryption
        uint8_t plainData[16] = {0};
        plainData[0] = (adcSound >> 8) & 0xFF;
        plainData[1] = adcSound & 0xFF;
        plainData[2] = (adcExtra >> 8) & 0xFF;
        plainData[3] = adcExtra & 0xFF;

        // 5. Encrypt and encode
        std::string encryptedBase64 = aesEncryptCBC(plainData, 4, aesKey, aesIv);

        // 6. One-time location update
        if (!locationUpdated && wifi_is_connected()) {
            float lat = 0.0f, lon = 0.0f;
            if (fetchGeoLocation(&lat, &lon)) {
                locationUpdated = updateChannelLocation(CHANNEL_ID, WRITE_API_KEY, lat, lon);
            }
        }

        // 7. Send encrypted data to ThingSpeak
        if (!encryptedBase64.empty()) {
            postEncryptedField(encryptedBase64);
        }

        vTaskDelay(pdMS_TO_TICKS(15000)); // ThingSpeak has a 15-second rate limit
    }
}

// ---------------------------------------------------------------------------
// app_main (ESP-IDF entry point, mirrors Arduino setup())
// ---------------------------------------------------------------------------
extern "C" void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    wifi_init_sta();
    adc_dac_init();

    xTaskCreate(monitor_task, "monitor_task", 8192, nullptr, 5, nullptr);
}

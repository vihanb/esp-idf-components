#include "WiFi.hpp"
#include <iostream>

#include <esp_log.h>
#include <esp_wifi.h>

#include <wifi_provisioning/manager.h>
#include <wifi_provisioning/scheme_ble.h>

#include "qrcode.hpp"

using namespace qrcode;

static const char *TAG = "WiFi";

WiFi::WiFi(const std::string &serviceName):
    serviceName(serviceName),
    popSeed(esp_random()) {}

WiFi::~WiFi() {
    ESP_LOGI(TAG, "Stopping WiFi module.");

    esp_wifi_deinit();
    esp_netif_deinit();
}

void WiFi::init() {
    // Initialize network service
    ESP_ERROR_CHECK(esp_netif_init());

    // Setup Wi-Fi handlers
    // TODO: potential leak, not unregistering handlers.
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, &WiFi::eventHandler, this));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &WiFi::eventHandler, this));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &WiFi::eventHandler, this));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_GOT_IP6, &WiFi::eventHandler, this));

    // Initialize Wi-Fi including netif with default config
    // TODO: potential leak, never saved.
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Initialize the provisioning manager
    wifi_prov_mgr_config_t config = {
        // Use BLE provisioning
        .scheme = wifi_prov_scheme_ble,

        // Free both BLE and Bluetooth (BTDM) memory as we don't use it.
        .scheme_event_handler = WIFI_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM,

        // (deprecated)
        .app_event_handler = WIFI_PROV_EVENT_HANDLER_NONE        
    };

    // Initialize the provisioning manager.
    ESP_ERROR_CHECK(wifi_prov_mgr_init(config));
    ESP_LOGI(TAG, "Successfully initialized WiFi module.");
}

void WiFi::start() {
    // Check if already provisioned
    bool provisioned = false;
    ESP_ERROR_CHECK(wifi_prov_mgr_is_provisioned(&provisioned));

    if (!provisioned) {
        ESP_LOGI(TAG, "Not provisioned, beginning provisioning.");
        wifi_prov_security_t security = WIFI_PROV_SECURITY_1;
        const char *service_key = NULL;

        ESP_ERROR_CHECK(wifi_prov_mgr_start_provisioning(
            security,
            pop().c_str(),
            deviceName().c_str(), service_key));
        
        printQRCode();

        // Wait for provisioning to finish.
        ESP_LOGI(TAG, "Finished provisioning.");

        wifi_prov_mgr_wait();
    } else {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_start());
    }

    wifi_prov_mgr_deinit();

    ESP_LOGI(TAG, "Waiting for WiFi to connect...");
    wifiEventGroup.waitForAll(BIT0);
    ESP_LOGI(TAG, "Successfully started WiFi module.");
}

void WiFi::handler(esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_STA_GOT_IP) {
            ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
            ESP_LOGI(TAG, "Connected with IPv4 Address: " IPSTR, IP2STR(&event->ip_info.ip));
            wifiEventGroup.setBits(BIT0);
        }
        if (event_id == IP_EVENT_GOT_IP6) {
            ip_event_got_ip6_t *event = (ip_event_got_ip6_t *)event_data;
            ESP_LOGI(TAG, "Connected with IPv6 Address: " IPV6STR, IPV62STR(event->ip6_info.ip));
            wifiEventGroup.setBits(BIT0);
        }

        if (event_id == IP_EVENT_STA_LOST_IP) {
            ESP_LOGD(TAG, "Lost IP Address.");
        }
    }

    if (event_base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_STA_CONNECTED) {
            ESP_LOGI(TAG, "Connection established to WiFi AP.");
        }

        if (event_id == WIFI_EVENT_STA_START || event_id == WIFI_EVENT_STA_DISCONNECTED) {
            ESP_LOGI(TAG, "Connecting to Wi-Fi AP...");
            esp_wifi_connect();
        }
    }
}

void WiFi::eventHandler(void *data, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    static_cast<WiFi *>(data)->handler(event_base, event_id, event_data);
}

std::string WiFi::deviceName() {
    // 8-bytes for type safety, but is likely only 6.
    uint8_t mac[8];
    esp_efuse_mac_get_default(mac);

    uint8_t byte1 = mac[0] ^ mac[1] ^ mac[2];
    uint8_t byte2 = mac[3] ^ mac[4] ^ mac[5];

    char uniqueId[5];
    snprintf(uniqueId, sizeof(uniqueId), "%02x%02x", byte1, byte2);

    return serviceName + " " + uniqueId;
}

std::string WiFi::pop() {
    char uniqueId[9];
    snprintf(uniqueId, sizeof(uniqueId), "%08x", popSeed);

    return std::string(uniqueId);
}

void WiFi::printQRCode() {
    std::string payload = R"({"ver":"v1","name":")" + deviceName()
                    + R"(","pop":")" + pop()
                    + R"(","transport":"ble"})";

    QrCode qrcode = QrCode::encodeText(
        payload.c_str(),
        QrCode::Ecc::LOW
    );

    qrcode.print();

    ESP_LOGI(TAG, "Scan the above QR code to provision '%s' with POP '%s'.", deviceName().c_str(), pop().c_str());
}
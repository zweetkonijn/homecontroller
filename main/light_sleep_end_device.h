/*
 * Zigbee Remote Controller - 13 knoppen met light sleep
 */

#pragma once

// ─── Zigbee netwerk instellingen ─────────────────────────────────────────────
#define ESP_ZIGBEE_PRIMARY_CHANNEL_MASK   ((1U << 11))  // Kanaal 11 = jouw Z2M kanaal
#define ESP_ZIGBEE_SECONDARY_CHANNEL_MASK (0x07FFF800U)

// ─── Endpoint ────────────────────────────────────────────────────────────────
#define REMOTE_ENDPOINT  1

// ─── Apparaat info ───────────────────────────────────────────────────────────
#define ESP_ZIGBEE_STORAGE_PARTITION_NAME "zb_storage"
#define ESP_MANUFACTURER_NAME  "\x07""Laurenz"
#define ESP_MODEL_IDENTIFIER   "\x08""RemoteV1"

// ─── Knoppen ─────────────────────────────────────────────────────────────────
#define BUTTON_COUNT  13
#define DEBOUNCE_MS   50

// GPIO8 (boot knop) en GPIO0 weggelaten — beide zijn strapping pinnen
static const int BUTTON_PINS[BUTTON_COUNT] = {
    2, 3, 4, 5, 11, 12, 13, 14, 9, 23, 24, 10, 1
};

// ─── Zigbee configuratie macros ───────────────────────────────────────────────
#define ESP_ZIGBEE_ZED_CONFIG()                          \
    {                                                    \
        .device_type = EZB_NWK_DEVICE_TYPE_END_DEVICE,  \
        .install_code_policy = false,                    \
        .zed_config = {                                  \
            .ed_timeout = EZB_NWK_ED_TIMEOUT_64MIN,      \
            .keep_alive = 10000,                         \
        },                                               \
    }

#if CONFIG_SOC_IEEE802154_SUPPORTED
#define ESP_ZIGBEE_PLATFORM_CONFIG()                                  \
    {                                                                 \
        .storage_partition_name = ESP_ZIGBEE_STORAGE_PARTITION_NAME,  \
        .radio_config = {                                             \
            .radio_mode = ESP_ZIGBEE_RADIO_MODE_NATIVE,               \
        },                                                            \
    }
#endif

#define ESP_ZIGBEE_DEFAULT_CONFIG()                       \
    {                                                     \
        .device_config = ESP_ZIGBEE_ZED_CONFIG(),         \
        .platform_config = ESP_ZIGBEE_PLATFORM_CONFIG(),  \
    };

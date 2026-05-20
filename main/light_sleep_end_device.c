/*
 * Zigbee Remote Controller - 13 knoppen met light sleep
 * Gebaseerd op Espressif light_sleep_end_device voorbeeld (SDK v2.x)
 */

#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "driver/gpio.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

#ifdef CONFIG_PM_ENABLE
#include "esp_pm.h"
#endif

#include "alarm_timer.h"
#include "esp_zigbee.h"
#include "ezbee/zha.h"
#include "ezbee/af.h"
#include "ezbee/nwk.h"
#include "ezbee/zcl/zcl_reporting.h"
#include "ezbee/zcl/zcl_desc.h"
#include "ezbee/zcl/zcl_general_cmd.h"
#include "ezbee/zcl/zcl_type.h"
#include "ezbee/zcl/cluster/analog_input_desc.h"
#include "ezbee/zdo/zdo_bind_mgmt.h"

#include "light_sleep_end_device.h"

static const char *TAG = "ZB_REMOTE";

// ─── Globale variabelen ───────────────────────────────────────────────────────

static bool s_zb_connected  = false;
static bool s_binding_done  = false;

// Opgeslagen IEEE adres van de coordinator voor binding
static ezb_extaddr_t s_coordinator_ieee = {0};

// Knop staten voor polling
typedef enum { BTN_IDLE, BTN_DEBOUNCE, BTN_PRESSED, BTN_RELEASE } btn_state_t;
static btn_state_t s_btn_state[BUTTON_COUNT];
static uint32_t    s_btn_timer[BUTTON_COUNT];

// Storage voor presentValue attribuut
static float s_present_value = 0.0f;

// ─── GPIO initialisatie ───────────────────────────────────────────────────────

static void buttons_init(void)
{
    gpio_config_t io_conf = {
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };

    for (int i = 0; i < BUTTON_COUNT; i++) {
        io_conf.pin_bit_mask = (1ULL << BUTTON_PINS[i]);
        gpio_config(&io_conf);
        gpio_wakeup_enable((gpio_num_t)BUTTON_PINS[i], GPIO_INTR_LOW_LEVEL);
        s_btn_state[i] = BTN_IDLE;
        s_btn_timer[i] = 0;
    }

    esp_sleep_enable_gpio_wakeup();
    ESP_LOGI(TAG, "%d knoppen geconfigureerd (polling modus)", BUTTON_COUNT);
}

// ─── Analog input rapporteren ─────────────────────────────────────────────────

static void report_analog_value(float value)
{
    // Stap 1: Update de lokale ZCL attribuut tabel
    s_present_value = value;
    ezb_zcl_status_t set_status = ezb_zcl_set_attr_value(
        REMOTE_ENDPOINT,
        EZB_ZCL_CLUSTER_ID_ANALOG_INPUT,
        EZB_ZCL_CLUSTER_SERVER,
        EZB_ZCL_ATTR_ANALOG_INPUT_PRESENT_VALUE_ID,
        EZB_ZCL_STD_MANUF_CODE,
        &value,
        false
    );

    if (set_status != EZB_ZCL_STATUS_SUCCESS) {
        ESP_LOGE(TAG, "set_attr_value mislukt: 0x%02x", set_status);
        return;
    }

    // Stap 2: Stuur rapport via binding naar coordinator (0x0000)
    ezb_zcl_report_attr_cmd_t report_cmd = {
        .cmd_ctrl = {
            .dst_addr   = EZB_ADDRESS_SHORT(0x0000),
            .dst_ep     = 1,
            .src_ep     = REMOTE_ENDPOINT,
            .cluster_id = EZB_ZCL_CLUSTER_ID_ANALOG_INPUT,
            .manuf_code = EZB_ZCL_STD_MANUF_CODE,
            .fc = {
                .manuf_specific  = 0,
                .direction       = EZB_ZCL_CMD_DIRECTION_TO_SRV,
                .dis_default_rsp = 1,
            },
            .cnf_ctx = { .cb = NULL, .user_ctx = NULL },
        },
        .payload = {
            .attr_id = EZB_ZCL_ATTR_ANALOG_INPUT_PRESENT_VALUE_ID,
        },
    };

    ezb_err_t err = ezb_zcl_report_attr_cmd_req(&report_cmd);
    if (err != EZB_ERR_NONE) {
        ESP_LOGE(TAG, "report_attr mislukt: 0x%04x", err);
    } else {
        ESP_LOGI(TAG, "Analog input: %.0f verstuurd", value);
    }
}

// ─── Knop polling task ────────────────────────────────────────────────────────

static void button_task(void *pvParameters)
{
    while (1) {
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);

        for (int i = 0; i < BUTTON_COUNT; i++) {
            bool is_low = (gpio_get_level((gpio_num_t)BUTTON_PINS[i]) == 0);

            switch (s_btn_state[i]) {

                case BTN_IDLE:
                    if (is_low) {
                        s_btn_timer[i] = now;
                        s_btn_state[i] = BTN_DEBOUNCE;
                    }
                    break;

                case BTN_DEBOUNCE:
                    if (!is_low) {
                        s_btn_state[i] = BTN_IDLE;
                    } else if ((now - s_btn_timer[i]) >= DEBOUNCE_MS) {
                        int knop = i + 1;
                        ESP_LOGI(TAG, "Knop %d ingedrukt (GPIO%d)", knop, BUTTON_PINS[i]);
                        if (s_zb_connected && s_binding_done) {
                            esp_zigbee_lock_acquire(portMAX_DELAY);
                            report_analog_value((float)knop);
                            esp_zigbee_lock_release();
                        } else {
                            ESP_LOGW(TAG, "Knop %d: niet verbonden of binding niet klaar", knop);
                        }
                        s_btn_state[i] = BTN_PRESSED;
                    }
                    break;

                case BTN_PRESSED:
                    if (!is_low) {
                        s_btn_timer[i] = now;
                        s_btn_state[i] = BTN_RELEASE;
                    }
                    break;

                case BTN_RELEASE:
                    if (is_low) {
                        s_btn_state[i] = BTN_PRESSED;
                    } else if ((now - s_btn_timer[i]) >= DEBOUNCE_MS) {
                        int knop = i + 1;
                        ESP_LOGI(TAG, "Knop %d losgelaten", knop);
                        if (s_zb_connected && s_binding_done) {
                            esp_zigbee_lock_acquire(portMAX_DELAY);
                            report_analog_value(0.0f);
                            esp_zigbee_lock_release();
                        }
                        s_btn_state[i] = BTN_IDLE;
                    }
                    break;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ─── Zigbee endpoint aanmaken ─────────────────────────────────────────────────

static esp_err_t create_remote_endpoint(void)
{
    ezb_af_device_desc_t dev_desc = ezb_af_create_device_desc();
    if (dev_desc == EZB_INVALID_AF_DEVICE_DESC) {
        return ESP_FAIL;
    }

    ezb_af_ep_config_t ep_cfg = {
        .ep_id              = REMOTE_ENDPOINT,
        .app_profile_id     = EZB_AF_HA_PROFILE_ID,
        .app_device_id      = 0x0000,
        .app_device_version = 0,
    };

    ezb_af_ep_desc_t ep_desc = ezb_af_create_endpoint_desc(&ep_cfg);
    if (ep_desc == EZB_INVALID_AF_EP_DESC) {
        return ESP_FAIL;
    }

    // Basic cluster
    ezb_zcl_cluster_desc_t basic_desc = ezb_zcl_basic_create_cluster_desc(NULL, EZB_ZCL_CLUSTER_SERVER);
    ezb_zcl_basic_cluster_desc_add_attr(basic_desc, EZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID, (void *)ESP_MANUFACTURER_NAME);
    ezb_zcl_basic_cluster_desc_add_attr(basic_desc, EZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID,  (void *)ESP_MODEL_IDENTIFIER);
    ESP_ERROR_CHECK(ezb_af_endpoint_add_cluster_desc(ep_desc, basic_desc));

    // Analog Input cluster met presentValue gemarkeerd als READ + REPORTING
    ezb_zcl_analog_input_cluster_server_config_t ai_cfg = {
        .out_of_service = false,
        .present_value  = 0.0f,
        .status_flags   = EZB_ZCL_ANALOG_INPUT_STATUS_FLAGS_DEFAULT_VALUE,
    };
    ezb_zcl_cluster_desc_t ai_desc = ezb_zcl_analog_input_create_cluster_desc(&ai_cfg, EZB_ZCL_CLUSTER_SERVER);

    // Maak presentValue attribuut aan met reporting access
    ezb_zcl_attr_desc_t pv_attr = ezb_zcl_create_attr_desc(
        EZB_ZCL_ATTR_ANALOG_INPUT_PRESENT_VALUE_ID,
        EZB_ZCL_ATTR_TYPE_SINGLE,
        EZB_ZCL_ATTR_ACCESS_READ | EZB_ZCL_ATTR_ACCESS_REPORTING,
        EZB_ZCL_STD_MANUF_CODE,
        &s_present_value
    );
    if (pv_attr != EZB_INVALID_ZCL_ATTR_DESC) {
        ezb_zcl_cluster_add_attr_desc(ai_desc, pv_attr);
    }

    ESP_ERROR_CHECK(ezb_af_endpoint_add_cluster_desc(ep_desc, ai_desc));
    ESP_ERROR_CHECK(ezb_af_device_add_endpoint_desc(dev_desc, ep_desc));
    ESP_ERROR_CHECK(ezb_af_device_desc_register(dev_desc));

    ESP_LOGI(TAG, "Endpoint %d geregistreerd", REMOTE_ENDPOINT);
    return ESP_OK;
}

// ─── Binding aanmaken naar coordinator ───────────────────────────────────────

static void bind_result_cb(const ezb_zdp_bind_req_result_t *result, void *user_ctx)
{
    if (result->error == EZB_ERR_NONE && result->rsp &&
        result->rsp->status == 0x00) {
        ESP_LOGI(TAG, "Binding naar coordinator succesvol!");
        s_binding_done = true;

        // Reporting info instellen na succesvolle binding
        ezb_zcl_reporting_info_t rep_info = ezb_zcl_reporting_info_find(
            REMOTE_ENDPOINT,
            EZB_ZCL_CLUSTER_ID_ANALOG_INPUT,
            EZB_ZCL_CLUSTER_SERVER,
            EZB_ZCL_ATTR_ANALOG_INPUT_PRESENT_VALUE_ID,
            0xFFFF
        );
        if (rep_info != NULL) {
            ezb_zcl_reporting_info_update(rep_info, 0, 300, NULL);
            ESP_LOGI(TAG, "Reporting geconfigureerd");
        }
    } else {
        ESP_LOGW(TAG, "Binding mislukt (err: 0x%04x), rapportage werkt mogelijk toch",
                 result->error);
        // Zelfs zonder binding proberen we te rapporteren
        s_binding_done = true;
    }
}

static void create_binding(void)
{
    // Haal het IEEE adres van de coordinator op (short address 0x0000)
    ezb_err_t ret = ezb_address_extended_by_short(0x0000, &s_coordinator_ieee);
    if (ret != EZB_ERR_NONE) {
        ESP_LOGW(TAG, "Kon IEEE adres van coordinator niet ophalen (0x%04x), binding overslaan", ret);
        s_binding_done = true;
        return;
    }

    // Haal ons eigen IEEE adres op
    ezb_extaddr_t our_ieee;
    ezb_nwk_get_extended_address(&our_ieee);

    ezb_zdo_bind_req_t bind_req = {
        .dst_nwk_addr = 0x0000,  // stuur binding request naar onszelf
        .field = {
            .src_addr     = our_ieee,
            .src_ep       = REMOTE_ENDPOINT,
            .cluster_id   = EZB_ZCL_CLUSTER_ID_ANALOG_INPUT,
            .dst_addr_mode = EZB_ADDR_MODE_EXT,
            .dst_ep       = 1,
        },
        .cb       = bind_result_cb,
        .user_ctx = NULL,
    };

    // Zet het coordinator IEEE adres in het dst_addr veld
    ezb_eui64_copy(&bind_req.field.dst_addr.extended_addr, &s_coordinator_ieee);

    ret = ezb_zdo_bind_req(&bind_req);
    if (ret != EZB_ERR_NONE) {
        ESP_LOGW(TAG, "Binding request mislukt: 0x%04x, rapportage toch proberen", ret);
        s_binding_done = true;
    } else {
        ESP_LOGI(TAG, "Binding request verstuurd naar coordinator");
    }
}

// ─── Zigbee signaal handler helpers ──────────────────────────────────────────

static void esp_zigbee_alarm_bdb_commissioning(alarm_timer_arg_t arg)
{
    esp_zigbee_lock_acquire(portMAX_DELAY);
    (void)ezb_bdb_start_top_level_commissioning(arg);
    esp_zigbee_lock_release();
}

static void set_rx_off(alarm_timer_arg_t arg)
{
    ezb_nwk_set_rx_on_when_idle(false);
    ESP_LOGI(TAG, "Radio naar sleep modus");
}

// ─── Zigbee signaal handler ───────────────────────────────────────────────────

static bool esp_zigbee_app_signal_handler(const ezb_app_signal_t *app_signal)
{
    ezb_app_signal_type_t signal_type = ezb_app_signal_get_type(app_signal);

    switch (signal_type) {

        case EZB_ZDO_SIGNAL_SKIP_STARTUP:
            ESP_LOGI(TAG, "Zigbee stack geinitialiseerd");
            ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_INITIALIZATION);
            break;

        case EZB_BDB_SIGNAL_DEVICE_FIRST_START:
        case EZB_BDB_SIGNAL_DEVICE_REBOOT: {
            ezb_bdb_comm_status_t status =
                *((ezb_bdb_comm_status_t *)ezb_app_signal_get_params(app_signal));
            if (status == EZB_BDB_STATUS_SUCCESS) {
                ESP_LOGI(TAG, "Apparaat opgestart in %s modus",
                         ezb_bdb_is_factory_new() ? "factory-reset" : "normale");
                if (ezb_bdb_is_factory_new()) {
                    ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_NETWORK_STEERING);
                } else {
                    s_zb_connected = true;
                    ESP_LOGI(TAG, "Herverbonden — binding aanmaken");
                    ezb_nwk_set_rx_on_when_idle(true);
                    create_binding();
                    alarm_timer_schedule(set_rx_off, 0, 60000);
                }
            } else {
                ESP_LOGW(TAG, "Opstart mislukt (0x%02x), opnieuw proberen...", status);
                alarm_timer_schedule(esp_zigbee_alarm_bdb_commissioning,
                                     EZB_BDB_MODE_INITIALIZATION, 1000);
            }
        } break;

        case EZB_BDB_SIGNAL_STEERING: {
            ezb_bdb_comm_status_t status =
                *((ezb_bdb_comm_status_t *)ezb_app_signal_get_params(app_signal));
            if (status == EZB_BDB_STATUS_SUCCESS) {
                ESP_LOGI(TAG,
                    "Verbonden! PAN ID: 0x%04hx, Kanaal: %d, Adres: 0x%04hx",
                    ezb_nwk_get_panid(),
                    ezb_nwk_get_current_channel(),
                    ezb_nwk_get_short_address()
                );
                s_zb_connected = true;
                ESP_LOGI(TAG, "Binding aanmaken naar coordinator");
                create_binding();
                // 60s wakker voor Z2M interview + binding
                alarm_timer_schedule(set_rx_off, 0, 60000);
            } else {
                ESP_LOGW(TAG, "Netwerk niet gevonden (0x%02x), opnieuw zoeken...", status);
                alarm_timer_schedule(esp_zigbee_alarm_bdb_commissioning,
                                     EZB_BDB_MODE_NETWORK_STEERING, 1000);
            }
        } break;

        case EZB_ZDO_SIGNAL_LEAVE:
            s_zb_connected = false;
            s_binding_done = false;
            ESP_LOGW(TAG, "Netwerk verlaten, opnieuw verbinden...");
            alarm_timer_schedule(esp_zigbee_alarm_bdb_commissioning,
                                 EZB_BDB_MODE_NETWORK_STEERING, 1000);
            break;

        default:
            ESP_LOGD(TAG, "Zigbee signaal: %s (0x%02x)",
                     ezb_app_signal_to_string(signal_type), signal_type);
            break;
    }
    return true;
}

// ─── Power management ─────────────────────────────────────────────────────────

#ifdef CONFIG_PM_ENABLE
static esp_err_t esp_pm_light_sleep_config(void)
{
#if CONFIG_FREERTOS_USE_TICKLESS_IDLE
    int cur_cpu_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
    esp_pm_config_t pm_config = {
        .max_freq_mhz       = cur_cpu_freq_mhz,
        .min_freq_mhz       = cur_cpu_freq_mhz,
        .light_sleep_enable = true,
    };
    esp_err_t rc = esp_pm_configure(&pm_config);
    if (rc == ESP_OK) {
        ESP_LOGI(TAG, "Power management: light sleep actief");
    }
    return rc;
#else
    return ESP_OK;
#endif
}
#endif

// ─── Zigbee commissioning setup ──────────────────────────────────────────────

static esp_err_t esp_zigbee_setup_commissioning(void)
{
    ezb_aps_secur_enable_distributed_security(false);
    ESP_ERROR_CHECK(ezb_bdb_set_primary_channel_set(ESP_ZIGBEE_PRIMARY_CHANNEL_MASK));
    ESP_ERROR_CHECK(ezb_bdb_set_secondary_channel_set(ESP_ZIGBEE_SECONDARY_CHANNEL_MASK));
    ESP_ERROR_CHECK(ezb_app_signal_add_handler(esp_zigbee_app_signal_handler));
    ezb_nwk_set_rx_on_when_idle(true);
    return ESP_OK;
}

// ─── Zigbee main task ─────────────────────────────────────────────────────────

static void esp_zigbee_stack_main_task(void *pvParameters)
{
    esp_zigbee_config_t config = ESP_ZIGBEE_DEFAULT_CONFIG();

    ESP_ERROR_CHECK(esp_zigbee_init(&config));
    ESP_ERROR_CHECK(esp_zigbee_setup_commissioning());
    ESP_ERROR_CHECK(create_remote_endpoint());
    ESP_ERROR_CHECK(esp_zigbee_start(false));

    esp_zigbee_launch_mainloop();

    esp_zigbee_deinit();
    vTaskDelete(NULL);
}

// ─── app_main ────────────────────────────────────────────────────────────────

void app_main(void)
{
    ESP_LOGI(TAG, "=== Zigbee Remote opstarten ===");

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(nvs_flash_init_partition(ESP_ZIGBEE_STORAGE_PARTITION_NAME));

#ifdef CONFIG_PM_ENABLE
    ESP_ERROR_CHECK(esp_pm_light_sleep_config());
#endif

    buttons_init();

    xTaskCreate(button_task, "button_task", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "Zigbee stack starten...");
    xTaskCreate(esp_zigbee_stack_main_task, "Zigbee_main", 4096, NULL, 5, NULL);
}

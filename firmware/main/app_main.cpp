/*
 * SmartButton — Matter over Thread, Generic Switch, ICD LIT.
 *
 * Le firmware ne pilote pas la mise en veille au cas par cas : c'est le
 * framework esp_pm (tickless idle + light sleep automatique) combiné à l'ICD
 * Manager de CHIP qui s'en charge. Il faut seulement l'*autoriser* une fois,
 * par esp_pm_configure() au démarrage (app_enable_light_sleep). Forcer
 * manuellement esp_light_sleep_start() casserait la pile OpenThread.
 * Voir docs/02-energie.md.
 */

#include "app_priv.h"

#include <esp_err.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <esp_pm.h>

#include <esp_matter.h>
#include <esp_matter_console.h>

#include <common_macros.h>

#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
#include <platform/ESP32/OpenthreadLauncher.h>
#include <esp_openthread.h>
#include <esp_openthread_lock.h>
#include <openthread/thread.h>

/* ESP-IDF ne fournit pas ces macros : chaque exemple les définit lui-même.
 * Reprises telles quelles de esp-matter/examples/icd_app/main/app_priv.h. */
#define ESP_OPENTHREAD_DEFAULT_RADIO_CONFIG()                                           \
    {                                                                                   \
        .radio_mode = RADIO_MODE_NATIVE,                                                \
    }

#define ESP_OPENTHREAD_DEFAULT_HOST_CONFIG()                                            \
    {                                                                                   \
        .host_connection_mode = HOST_CONNECTION_MODE_NONE,                              \
    }

#define ESP_OPENTHREAD_DEFAULT_PORT_CONFIG()                                            \
    {                                                                                   \
        .storage_partition_name = "nvs", .netif_queue_size = 10, .task_queue_size = 10, \
    }
#endif

static const char *TAG = "smartbutton";

uint16_t g_switch_endpoint_id = 0;

using namespace esp_matter;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

/* ------------------------------------------------------------------------ */
/* Callbacks Matter                                                          */
/* ------------------------------------------------------------------------ */

static void app_event_cb(const ChipDeviceEvent *event, intptr_t)
{
    switch (event->Type) {
    case chip::DeviceLayer::DeviceEventType::kCommissioningComplete:
        ESP_LOGI(TAG, "commissioning terminé");
        app_led_blink(1000);
        break;
    case chip::DeviceLayer::DeviceEventType::kCommissioningWindowOpened:
        ESP_LOGI(TAG, "fenêtre de commissioning ouverte");
        break;
    case chip::DeviceLayer::DeviceEventType::kCommissioningWindowClosed:
        ESP_LOGI(TAG, "fenêtre de commissioning fermée");
        break;
    case chip::DeviceLayer::DeviceEventType::kFailSafeTimerExpired:
        ESP_LOGW(TAG, "commissioning échoué (fail-safe expiré)");
        break;
    case chip::DeviceLayer::DeviceEventType::kThreadConnectivityChange:
        ESP_LOGI(TAG, "connectivité Thread modifiée");
        break;
    default:
        break;
    }
}

/* Sans ça, la période de data poll reste plafonnée au child timeout par
 * défaut (240 s) quelle que soit la valeur d'ICD_SLOW_POLL_INTERVAL_MS.
 * Voir le commentaire de APP_THREAD_CHILD_TIMEOUT_S dans app_priv.h. */
static void app_set_thread_child_timeout(void)
{
#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
    if (!esp_openthread_lock_acquire(pdMS_TO_TICKS(1000))) {
        ESP_LOGE(TAG, "verrou OpenThread indisponible, child timeout inchangé");
        return;
    }
    otInstance *instance = esp_openthread_get_instance();
    if (instance != nullptr) {
        otThreadSetChildTimeout(instance, APP_THREAD_CHILD_TIMEOUT_S);
        ESP_LOGI(TAG, "child timeout Thread = %d s (défaut 240)",
                 APP_THREAD_CHILD_TIMEOUT_S);
    } else {
        ESP_LOGE(TAG, "instance OpenThread nulle, child timeout inchangé");
    }
    esp_openthread_lock_release();
#endif
}

/* PIÈGE : CONFIG_PM_ENABLE et CONFIG_FREERTOS_USE_TICKLESS_IDLE ne suffisent
 * pas. Sans cet appel, light_sleep_enable reste à false et la puce ne dort
 * JAMAIS, sans le moindre message d'erreur (même CONFIG_PM_DFS_INIT_AUTO ne
 * règle que la fréquence). Schéma repris d'esp-matter/examples/icd_app.
 * En profil dev (PM_ENABLE=n), la fonction ne fait rien. */
static void app_enable_light_sleep(void)
{
#if CONFIG_PM_ENABLE
    esp_pm_config_t pm_config = {
        .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .min_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
#if CONFIG_FREERTOS_USE_TICKLESS_IDLE
        .light_sleep_enable = true,
#endif
    };
    esp_err_t err = esp_pm_configure(&pm_config);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "light sleep automatique activé");
    } else {
        ESP_LOGE(TAG, "esp_pm_configure: %s — la puce ne dormira pas", esp_err_to_name(err));
    }
#else
    ESP_LOGW(TAG, "PM désactivé (profil dev) : pas de light sleep");
#endif
}

static esp_err_t app_identification_cb(identification::callback_type_t type,
                                       uint16_t endpoint_id, uint8_t effect_id,
                                       uint8_t effect_variant, void *)
{
    ESP_LOGI(TAG, "identify: type=%u ep=%u effect=%u", type, endpoint_id, effect_id);
    app_led_blink(300);
    return ESP_OK;
}

static esp_err_t app_attribute_update_cb(attribute::callback_type_t type,
                                         uint16_t endpoint_id, uint32_t cluster_id,
                                         uint32_t attribute_id, esp_matter_attr_val_t *,
                                         void *)
{
    /* Un Generic Switch n'a pas d'attribut inscriptible par le contrôleur.
     * Ce hook n'est là que pour la traçabilité — retourner ESP_OK. */
    return ESP_OK;
}

/* ------------------------------------------------------------------------ */

extern "C" void app_main()
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    app_enable_light_sleep();

    /* --- Arbre d'objets Matter --- */

    node::config_t node_config;
    node_t *node = node::create(&node_config, app_attribute_update_cb,
                                app_identification_cb);
    ABORT_APP_ON_FAILURE(node != nullptr, ESP_LOGE(TAG, "création du node échouée"));

    /* La feature de base est passée à la création ; les autres s'ajoutent
     * ensuite sur le cluster (docs/01-architecture.md §2).
     *
     * MS  : bouton momentané  -> InitialPress
     * MSR : relâchement       -> ShortRelease
     * MSL : appui long        -> LongPress / LongRelease
     * MSM : multi-clic        -> MultiPressOngoing / MultiPressComplete
     *
     * On n'active PAS ActionSwitch (AS) : c'est une feature Matter 1.4 qui
     * supprime les événements intermédiaires. Moins de trafic radio, mais
     * support contrôleur plus incertain. À réévaluer après l'étape 1.
     */
    generic_switch::config_t switch_config;
    switch_config.switch_cluster.number_of_positions = 2;
    switch_config.switch_cluster.current_position = 0;
    switch_config.switch_cluster.feature_flags =
        cluster::switch_cluster::feature::momentary_switch::get_id();

    endpoint_t *ep = generic_switch::create(node, &switch_config, ENDPOINT_FLAG_NONE, nullptr);
    ABORT_APP_ON_FAILURE(ep != nullptr, ESP_LOGE(TAG, "création de l'endpoint échouée"));

    g_switch_endpoint_id = endpoint::get_id(ep);
    ESP_LOGI(TAG, "generic switch sur endpoint %u", g_switch_endpoint_id);

    cluster_t *sw = cluster::get(ep, Switch::Id);
    ABORT_APP_ON_FAILURE(sw != nullptr, ESP_LOGE(TAG, "cluster Switch introuvable"));

    cluster::switch_cluster::feature::momentary_switch_release::add(sw);
    cluster::switch_cluster::feature::momentary_switch_long_press::add(sw);

    cluster::switch_cluster::feature::momentary_switch_multi_press::config_t msm;
    msm.multi_press_max = APP_MULTI_PRESS_MAX;
    cluster::switch_cluster::feature::momentary_switch_multi_press::add(sw, &msm);

#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
    esp_openthread_platform_config_t ot_config = {
        .radio_config = ESP_OPENTHREAD_DEFAULT_RADIO_CONFIG(),
        .host_config = ESP_OPENTHREAD_DEFAULT_HOST_CONFIG(),
        .port_config = ESP_OPENTHREAD_DEFAULT_PORT_CONFIG(),
    };
    set_openthread_platform_config(&ot_config);
#endif

    ESP_ERROR_CHECK(esp_matter::start(app_event_cb));

    /* Après start() : la pile OpenThread est initialisée. */
    app_set_thread_child_timeout();

#if CONFIG_ENABLE_CHIP_SHELL
    esp_matter::console::diagnostics_register_commands();
    esp_matter::console::factoryreset_register_commands();
    esp_matter::console::init();
#endif

    ESP_ERROR_CHECK(app_button_init());

    ESP_LOGI(TAG, "prêt");
}

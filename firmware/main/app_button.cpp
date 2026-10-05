/*
 * Traduction des gestes physiques en événements du cluster Matter Switch.
 *
 *   appui             -> InitialPress
 *   relâchement court -> ShortRelease  puis MultiPressComplete(n)
 *   maintien > 1 s    -> LongPress     puis LongRelease
 *   n-ième appui      -> MultiPressOngoing(n)
 *
 * Les callbacks iot_button s'exécutent dans la tâche « button », pas dans la
 * tâche CHIP. Tout appel à l'API Matter passe donc par ScheduleLambda(), qui
 * réordonnance le travail sur la tâche CHIP — même schéma que
 * esp-matter/examples/generic_switch.
 */

#include "app_priv.h"

#include <esp_log.h>
#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/timers.h>

#include <esp_sleep.h>
#include <soc/soc_caps.h>

#include <iot_button.h>
#include <button_gpio.h>

#include <esp_matter.h>
#include <app/server/Server.h>
#include <app/server/CommissioningWindowManager.h>

#include <app/clusters/switch-server/CodegenIntegration.h>

#if CHIP_CONFIG_ENABLE_ICD_SERVER
#include <app/icd/server/ICDConfigurationData.h>
#include <app/icd/server/ICDNotifier.h>
#endif

static const char *TAG = "app_button";

using namespace esp_matter;
using namespace esp_matter::cluster;
using namespace chip::app::Clusters;

static button_handle_t s_btn = nullptr;

/* Un appui long déclenche aussi BUTTON_PRESS_UP au relâchement : ce drapeau
 * choisit entre LongRelease et ShortRelease. */
static bool s_long_press_active = false;

static constexpr uint8_t kIdlePosition = 0;
static constexpr uint8_t kPressedPosition = 1;

/* ------------------------------------------------------------------------ */
/* Helpers Matter                                                            */
/* ------------------------------------------------------------------------ */

/* Le cluster Switch est « code-driven » dans esp-matter : son état vit dans
 * l'objet SwitchCluster, pas dans le stockage d'attributs esp-matter.
 * attribute::update() y est refusé (ESP_ERR_NOT_SUPPORTED) et les helpers
 * send_*() n'émettent que l'événement, sans toucher à CurrentPosition. */
static void set_current_position(uint8_t position)
{
    SwitchCluster *cluster = Switch::FindClusterOnEndpoint(g_switch_endpoint_id);
    if (cluster == nullptr) {
        ESP_LOGE(TAG, "cluster Switch introuvable sur l'endpoint %u", g_switch_endpoint_id);
        return;
    }
    CHIP_ERROR err = cluster->SetCurrentPosition(position);
    if (err != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "SetCurrentPosition: %" CHIP_ERROR_FORMAT, err.Format());
    }
}

/* Diagnostic : le mode ICD effectif décide de l'autonomie. En SIT (aucun
 * client enregistré par le contrôleur), CHIP plafonne le slow poll à 15 s
 * quelle que soit la config (ICDConfigurationData::GetSlowPollingInterval).
 * Seul un passage en LIT donne les longs intervalles du profil sleepy. */
static void log_icd_state(void)
{
#if CHIP_CONFIG_ENABLE_ICD_SERVER
    chip::ICDConfigurationData &icd = chip::ICDConfigurationData::GetInstance();
    const bool lit = icd.GetICDMode() == chip::ICDConfigurationData::ICDMode::LIT;
    ESP_LOGI(TAG, "ICD : mode %s, slow poll effectif %lu ms, idle %lu s",
             lit ? "LIT" : "SIT",
             static_cast<unsigned long>(icd.GetSlowPollingInterval().count()),
             static_cast<unsigned long>(icd.GetIdleModeDuration().count()));
#endif
}

/* Feature UAT : toute action utilisateur doit pouvoir sortir l'appareil du
 * mode veille. Voir docs/01-architecture.md §3. */
static void icd_request_active_mode(void)
{
#if CHIP_CONFIG_ENABLE_ICD_SERVER
    chip::DeviceLayer::SystemLayer().ScheduleLambda([]() {
        chip::app::ICDNotifier::GetInstance().NotifyNetworkActivityNotification();
    });
#endif
}

/* ------------------------------------------------------------------------ */
/* Callbacks bouton                                                          */
/* ------------------------------------------------------------------------ */

static void on_press_down(void *, void *)
{
    ESP_LOGI(TAG, "press down");
    icd_request_active_mode();
    app_led_blink(30);
    s_long_press_active = false;

    chip::DeviceLayer::SystemLayer().ScheduleLambda([]() {
        set_current_position(kPressedPosition);
        switch_cluster::event::send_initial_press(g_switch_endpoint_id, kPressedPosition);
        log_icd_state();
    });
}

static void on_press_up(void *, void *)
{
    const bool was_long = s_long_press_active;
    s_long_press_active = false;
    ESP_LOGI(TAG, "press up (long=%d)", was_long);

    chip::DeviceLayer::SystemLayer().ScheduleLambda([was_long]() {
        set_current_position(kIdlePosition);
        if (was_long) {
            switch_cluster::event::send_long_release(g_switch_endpoint_id, kPressedPosition);
        } else {
            switch_cluster::event::send_short_release(g_switch_endpoint_id, kPressedPosition);
        }
    });
}

static void on_long_press_start(void *, void *)
{
    ESP_LOGI(TAG, "long press");
    s_long_press_active = true;

    chip::DeviceLayer::SystemLayer().ScheduleLambda([]() {
        switch_cluster::event::send_long_press(g_switch_endpoint_id, kPressedPosition);
    });
}

static void on_press_repeat(void *arg, void *)
{
    const uint8_t count = iot_button_get_repeat((button_handle_t)arg);
    ESP_LOGI(TAG, "repeat %u", count);

    chip::DeviceLayer::SystemLayer().ScheduleLambda([count]() {
        set_current_position(kPressedPosition);
        switch_cluster::event::send_multi_press_ongoing(g_switch_endpoint_id,
                                                        kPressedPosition, count);
    });
}

static void on_press_repeat_done(void *arg, void *)
{
    uint8_t count = iot_button_get_repeat((button_handle_t)arg);
    /* La spec veut 0 quand le compte dépasse MultiPressMax. */
    if (count > APP_MULTI_PRESS_MAX) {
        count = 0;
    }
    ESP_LOGI(TAG, "multi-press complete: %u", count);

    chip::DeviceLayer::SystemLayer().ScheduleLambda([count]() {
        set_current_position(kIdlePosition);
        switch_cluster::event::send_multi_press_complete(g_switch_endpoint_id,
                                                         kPressedPosition, count);
    });
}

/* --- Réveil du bouton en light sleep --------------------------------------
 *
 * Contournement d'un défaut du composant espressif/button (constaté en 4.2.0,
 * toujours présent sur master en octobre 2026), qui ne se manifeste qu'avec
 * CONFIG_PM_POWER_DOWN_PERIPHERAL_IN_LIGHT_SLEEP :
 *
 *   - à l'init, il arme le réveil EXT1 sur la broche (seul réveil effectif
 *     quand les périphériques sont éteints pendant le light sleep) ;
 *   - en sortant de son mode basse conso après un appui, il le DÉSARME
 *     (button_gpio_enable_gpio_wakeup, branche disable) ;
 *   - en y revenant, il ne réarme que le réveil GPIO, inopérant dans ce mode.
 *
 * Symptôme observé sur la SuperMini (06/10/2026) : le premier appui marche,
 * puis les appuis ne sont plus vus que si la puce est déjà éveillée pour autre
 * chose (fenêtre active après un appui, poll Thread). On réarme donc EXT1 à
 * chaque retour en basse conso, via le callback prévu par le composant. */
static void on_button_enter_power_save(void *)
{
#if CONFIG_PM_POWER_DOWN_PERIPHERAL_IN_LIGHT_SLEEP && SOC_PM_SUPPORT_EXT1_WAKEUP
    esp_err_t err = esp_sleep_enable_ext1_wakeup_io(
        1ULL << APP_BUTTON_GPIO,
        APP_BUTTON_ACTIVE_LEVEL ? ESP_EXT1_WAKEUP_ANY_HIGH : ESP_EXT1_WAKEUP_ANY_LOW);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "réarmement du réveil EXT1 : %s", esp_err_to_name(err));
    }
#endif
}

/* --- Gestes système ----------------------------------------------------- */

static void on_hold_commission(void *, void *)
{
    ESP_LOGW(TAG, "hold 10s -> réouverture fenêtre de commissioning");
    app_led_blink(500);
    chip::DeviceLayer::SystemLayer().ScheduleLambda([]() {
        CHIP_ERROR err = chip::Server::GetInstance().GetCommissioningWindowManager()
                             .OpenBasicCommissioningWindow();
        if (err != CHIP_NO_ERROR) {
            ESP_LOGE(TAG, "OpenBasicCommissioningWindow: %" CHIP_ERROR_FORMAT, err.Format());
        }
    });
}

static void on_hold_factory_reset(void *, void *)
{
    ESP_LOGW(TAG, "hold 20s -> FACTORY RESET");
    app_led_blink(2000);
    chip::DeviceLayer::SystemLayer().ScheduleLambda([]() {
        chip::Server::GetInstance().ScheduleFactoryReset();
    });
}

/* ------------------------------------------------------------------------ */

esp_err_t app_button_init(void)
{
    esp_err_t led_err = app_led_init();
    if (led_err != ESP_OK) {
        /* Une LED en panne ne doit pas empêcher le bouton de fonctionner. */
        ESP_LOGW(TAG, "LED indisponible : %s", esp_err_to_name(led_err));
    }

    button_config_t btn_cfg = {};
    btn_cfg.long_press_time = APP_LONG_PRESS_MS;
    btn_cfg.short_press_time = APP_SHORT_PRESS_MS;

    button_gpio_config_t gpio_cfg = {};
    gpio_cfg.gpio_num = APP_BUTTON_GPIO;
    gpio_cfg.active_level = APP_BUTTON_ACTIVE_LEVEL;
    /* Autorise le light sleep et évite de maintenir une pull-up en
     * permanence. Voir docs/03-hardware.md §4. */
    gpio_cfg.enable_power_save = true;

    esp_err_t err = iot_button_new_gpio_device(&btn_cfg, &gpio_cfg, &s_btn);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "iot_button_new_gpio_device: %s", esp_err_to_name(err));
        return err;
    }

    button_power_save_config_t ps_cfg = {};
    ps_cfg.enter_power_save_cb = on_button_enter_power_save;
    err = iot_button_register_power_save_cb(&ps_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "iot_button_register_power_save_cb: %s", esp_err_to_name(err));
        return err;
    }

    iot_button_register_cb(s_btn, BUTTON_PRESS_DOWN,        nullptr, on_press_down,        nullptr);
    iot_button_register_cb(s_btn, BUTTON_PRESS_UP,          nullptr, on_press_up,          nullptr);
    iot_button_register_cb(s_btn, BUTTON_LONG_PRESS_START,  nullptr, on_long_press_start,  nullptr);
    /* Le 1er argument des callbacks est le button_handle_t, d'où le nullptr
     * en usr_data : on_press_repeat* lisent le compteur via `arg`. */
    iot_button_register_cb(s_btn, BUTTON_PRESS_REPEAT,      nullptr, on_press_repeat,      nullptr);
    iot_button_register_cb(s_btn, BUTTON_PRESS_REPEAT_DONE, nullptr, on_press_repeat_done, nullptr);

    /* press_time est un uint16_t : 20 000 ms passe, pas au-delà de 65 535. */
    button_event_args_t hold_commission = {};
    hold_commission.long_press.press_time = APP_HOLD_COMMISSION_MS;
    iot_button_register_cb(s_btn, BUTTON_LONG_PRESS_START, &hold_commission,
                           on_hold_commission, nullptr);

    button_event_args_t hold_factory = {};
    hold_factory.long_press.press_time = APP_HOLD_FACTORY_MS;
    iot_button_register_cb(s_btn, BUTTON_LONG_PRESS_START, &hold_factory,
                           on_hold_factory_reset, nullptr);

    ESP_LOGI(TAG, "bouton initialisé sur GPIO%d", APP_BUTTON_GPIO);
    return ESP_OK;
}

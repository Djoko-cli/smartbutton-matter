/*
 * Niveau de pile, publié dans le cluster Matter Power Source (feature Battery).
 *
 * Maison affiche le pourcentage (BatPercentRemaining) et alerte quand la pile
 * faiblit (BatChargeLevel / BatReplacementNeeded).
 *
 * Source de tension (menuconfig > SmartButton > Pile) :
 *   - simulée : carte de dev alimentée en USB, rien à mesurer ;
 *   - ADC     : PCB final, la pile à travers un pont diviseur.
 *     ⚠️ Branche compilée mais pas encore testée sur matériel.
 *
 * La mesure est rare (toutes les 6 h par défaut) et un rapport radio n'est
 * émis que si une valeur change : coût négligeable dans le budget énergie.
 */

#include "app_priv.h"

#include <esp_log.h>
#include <esp_timer.h>

#include <esp_matter.h>

#if CONFIG_APP_BATTERY_ADC
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include <esp_adc/adc_oneshot.h>
#endif

static const char *TAG = "app_battery";

using namespace esp_matter;
using namespace chip::app::Clusters;

static uint16_t s_endpoint_id = 0;
static esp_timer_handle_t s_timer = nullptr;

/* Dernières valeurs publiées : on n'appelle attribute::update() (et donc on
 * ne provoque un rapport radio) que si quelque chose a changé. */
static int s_last_half_percent = -1;
static int s_last_level = -1;

/* ------------------------------------------------------------------------ */
/* Courbe de décharge                                                        */
/* ------------------------------------------------------------------------ */

/* Pile lithium / dioxyde de manganèse (CR2450) sous faible charge : ~3,0 V
 * neuve, long plateau entre 2,9 et 2,7 V, puis chute. Le boost du PCB permet
 * de l'exploiter jusqu'à ~2,0 V (docs/03-hardware.md §1).
 *
 * Table approximative, à recaler sur une vraie décharge mesurée. */
struct curve_point {
    uint16_t mv;
    uint8_t percent;
};

static constexpr curve_point kCurve[] = {
    {3000, 100}, {2900, 90}, {2800, 75}, {2700, 55}, {2600, 35},
    {2500, 20},  {2400, 10}, {2200, 3},  {2000, 0},
};

static uint8_t percent_from_mv(uint32_t mv)
{
    if (mv >= kCurve[0].mv) {
        return 100;
    }
    for (size_t i = 1; i < sizeof(kCurve) / sizeof(kCurve[0]); i++) {
        if (mv >= kCurve[i].mv) {
            const curve_point &hi = kCurve[i - 1];
            const curve_point &lo = kCurve[i];
            return lo.percent + (mv - lo.mv) * (hi.percent - lo.percent) / (hi.mv - lo.mv);
        }
    }
    return 0;
}

/* BatChargeLevelEnum : 0 = OK, 1 = Warning, 2 = Critical. */
static uint8_t level_from_percent(uint8_t percent)
{
    if (percent <= 5) {
        return 2;
    }
    if (percent <= 20) {
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------------ */
/* Mesure                                                                    */
/* ------------------------------------------------------------------------ */

#if CONFIG_APP_BATTERY_ADC

static esp_err_t read_battery_mv(uint32_t *out_mv)
{
    adc_unit_t unit;
    adc_channel_t channel;
    esp_err_t err = adc_oneshot_io_to_channel(CONFIG_APP_BATTERY_ADC_GPIO, &unit, &channel);
    if (err != ESP_OK) {
        return err;
    }

    adc_oneshot_unit_handle_t adc = nullptr;
    adc_oneshot_unit_init_cfg_t unit_cfg = {};
    unit_cfg.unit_id = unit;
    err = adc_oneshot_new_unit(&unit_cfg, &adc);
    if (err != ESP_OK) {
        return err;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {};
    chan_cfg.atten = ADC_ATTEN_DB_12;
    chan_cfg.bitwidth = ADC_BITWIDTH_DEFAULT;

    adc_cali_handle_t cali = nullptr;
    adc_cali_curve_fitting_config_t cali_cfg = {};
    cali_cfg.unit_id = unit;
    cali_cfg.chan = channel;
    cali_cfg.atten = chan_cfg.atten;
    cali_cfg.bitwidth = chan_cfg.bitwidth;

    int adc_mv = 0;
    err = adc_oneshot_config_channel(adc, channel, &chan_cfg);
    if (err == ESP_OK) {
        err = adc_cali_create_scheme_curve_fitting(&cali_cfg, &cali);
    }
    if (err == ESP_OK) {
        /* Moyenne de quelques lectures pour lisser le bruit. */
        int sum = 0;
        const int n = 8;
        for (int i = 0; i < n && err == ESP_OK; i++) {
            int v = 0;
            err = adc_oneshot_get_calibrated_result(adc, cali, channel, &v);
            sum += v;
        }
        adc_mv = sum / n;
    }

    if (cali) {
        adc_cali_delete_scheme_curve_fitting(cali);
    }
    adc_oneshot_del_unit(adc);

    if (err == ESP_OK) {
        *out_mv = static_cast<uint32_t>(adc_mv) * CONFIG_APP_BATTERY_DIVIDER_X1000 / 1000;
    }
    return err;
}

#else /* CONFIG_APP_BATTERY_SIMULATED */

static esp_err_t read_battery_mv(uint32_t *out_mv)
{
    *out_mv = CONFIG_APP_BATTERY_SIM_MV;
    return ESP_OK;
}

#endif

/* ------------------------------------------------------------------------ */
/* Publication Matter                                                        */
/* ------------------------------------------------------------------------ */

static void update_attr(uint32_t attribute_id, esp_matter_attr_val_t val)
{
    esp_err_t err = attribute::update(s_endpoint_id, PowerSource::Id, attribute_id, &val);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PowerSource 0x%04lx : %s", static_cast<unsigned long>(attribute_id),
                 esp_err_to_name(err));
    }
}

/* Tourne sur la tâche CHIP (ScheduleLambda). */
static void publish(uint32_t mv)
{
    const uint8_t percent = percent_from_mv(mv);
    const int half_percent = percent * 2; /* l'attribut est en demi-pourcents */
    const int level = level_from_percent(percent);

    if (half_percent == s_last_half_percent && level == s_last_level) {
        return;
    }

    update_attr(PowerSource::Attributes::BatVoltage::Id,
                esp_matter_nullable_uint32(nullable<uint32_t>(mv)));
    update_attr(PowerSource::Attributes::BatPercentRemaining::Id,
                esp_matter_nullable_uint8(nullable<uint8_t>(static_cast<uint8_t>(half_percent))));
    update_attr(PowerSource::Attributes::BatChargeLevel::Id, esp_matter_enum8(level));
    update_attr(PowerSource::Attributes::BatReplacementNeeded::Id, esp_matter_bool(level == 2));

    s_last_half_percent = half_percent;
    s_last_level = level;
    ESP_LOGI(TAG, "pile : %lu mV -> %u %%, niveau %s", static_cast<unsigned long>(mv), percent,
             level == 0 ? "OK" : (level == 1 ? "faible" : "critique"));
}

static void measure_and_publish(void)
{
    uint32_t mv = 0;
    esp_err_t err = read_battery_mv(&mv);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mesure de la pile : %s", esp_err_to_name(err));
        return;
    }
    chip::DeviceLayer::SystemLayer().ScheduleLambda([mv]() { publish(mv); });
}

static void timer_cb(void *)
{
    measure_and_publish();
}

/* ------------------------------------------------------------------------ */

esp_err_t app_battery_init(uint16_t endpoint_id)
{
    s_endpoint_id = endpoint_id;

    esp_timer_create_args_t args = {};
    args.callback = timer_cb;
    args.name = "battery";
    esp_err_t err = esp_timer_create(&args, &s_timer);
    if (err != ESP_OK) {
        return err;
    }

    measure_and_publish();
    return esp_timer_start_periodic(s_timer,
                                    static_cast<uint64_t>(CONFIG_APP_BATTERY_PERIOD_MIN) * 60ULL * 1000000ULL);
}

/*
 * LED de l'appareil. Le comportement dépend de CONFIG_APP_LED_MODE
 * (menuconfig > SmartButton, voir Kconfig.projbuild) :
 *
 *   APP_LED_SIMPLE        PCB final : LED éteinte, flash bref sur événement.
 *   APP_LED_WS2812_WHITE  SuperMini : blanc fixe, posé une fois au démarrage.
 *   APP_LED_NONE          rien.
 */

#include "app_priv.h"

#include <esp_check.h>
#include <esp_log.h>
#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/timers.h>

#if CONFIG_APP_LED_WS2812_WHITE
#include <led_strip.h>
#endif

static const char *TAG = "app_led";

#if CONFIG_APP_LED_SIMPLE

static TimerHandle_t s_led_timer = nullptr;

static void led_off_cb(TimerHandle_t)
{
    gpio_set_level(static_cast<gpio_num_t>(CONFIG_APP_LED_GPIO), !APP_LED_ACTIVE_LEVEL);
}

esp_err_t app_led_init(void)
{
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = 1ULL << CONFIG_APP_LED_GPIO;
    cfg.mode = GPIO_MODE_OUTPUT;
    ESP_RETURN_ON_ERROR(gpio_config(&cfg), TAG, "gpio_config");
    gpio_set_level(static_cast<gpio_num_t>(CONFIG_APP_LED_GPIO), !APP_LED_ACTIVE_LEVEL);

    s_led_timer = xTimerCreate("led", pdMS_TO_TICKS(50), pdFALSE, nullptr, led_off_cb);
    return s_led_timer ? ESP_OK : ESP_ERR_NO_MEM;
}

void app_led_blink(uint32_t on_ms)
{
    if (s_led_timer == nullptr) {
        return;
    }
    gpio_set_level(static_cast<gpio_num_t>(CONFIG_APP_LED_GPIO), APP_LED_ACTIVE_LEVEL);
    xTimerChangePeriod(s_led_timer, pdMS_TO_TICKS(on_ms), 0);
    xTimerStart(s_led_timer, 0);
}

#elif CONFIG_APP_LED_WS2812_WHITE

/* La WS2812 mémorise sa couleur tant qu'elle est alimentée : on l'envoie une
 * fois, puis on libère le canal RMT. La ligne de données est ensuite tenue à 0,
 * y compris pendant le light sleep (gpio_sleep_sel_dis), sinon elle flotterait
 * (CONFIG_PM_SLP_DISABLE_GPIO) et le bruit pourrait changer la couleur. */
esp_err_t app_led_init(void)
{
    led_strip_config_t strip_cfg = {};
    strip_cfg.strip_gpio_num = CONFIG_APP_LED_GPIO;
    strip_cfg.max_leds = 1;
    strip_cfg.led_model = LED_MODEL_WS2812;
    strip_cfg.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;

    led_strip_rmt_config_t rmt_cfg = {};
    rmt_cfg.resolution_hz = 10 * 1000 * 1000;

    led_strip_handle_t strip = nullptr;
    ESP_RETURN_ON_ERROR(led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &strip), TAG, "led_strip");

    const uint32_t v = CONFIG_APP_LED_WS2812_LEVEL;
    esp_err_t err = led_strip_set_pixel(strip, 0, v, v, v);
    if (err == ESP_OK) {
        err = led_strip_refresh(strip);
    }
    led_strip_del(strip);
    ESP_RETURN_ON_ERROR(err, TAG, "envoi de la couleur");

    const gpio_num_t pin = static_cast<gpio_num_t>(CONFIG_APP_LED_GPIO);
    gpio_set_direction(pin, GPIO_MODE_OUTPUT);
    gpio_set_level(pin, 0);
    gpio_sleep_sel_dis(pin);

    ESP_LOGI(TAG, "WS2812 en blanc fixe (%lu/255)", static_cast<unsigned long>(v));
    return ESP_OK;
}

void app_led_blink(uint32_t) {}

#else /* CONFIG_APP_LED_NONE */

esp_err_t app_led_init(void) { return ESP_OK; }
void app_led_blink(uint32_t) {}

#endif

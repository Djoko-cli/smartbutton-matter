#pragma once

#include <esp_err.h>
#include "sdkconfig.h"
#include <stdint.h>

/* ------------------------------------------------------------------------ */
/* Brochage — à aligner sur le PCB (docs/03-hardware.md)                     */
/* ------------------------------------------------------------------------ */

/* Switch tactile entre GPIO et GND. Pull-up interne, activée seulement
 * pendant l'échantillonnage par le driver (enable_power_save).
 *
 * GPIO9 = bouton BOOT des devkits C6 et H2 : pratique pour prototyper sans
 * câblage. MAIS c'est une broche de strapping — la maintenir à 0 pendant le
 * reset fait entrer la puce en mode download. Sur le PCB final, choisir une
 * GPIO ordinaire capable de réveiller depuis le light sleep. */
#define APP_BUTTON_GPIO         GPIO_NUM_9
#define APP_BUTTON_ACTIVE_LEVEL 0

/* LED : type et GPIO réglés dans menuconfig > SmartButton (Kconfig.projbuild).
 * Niveau actif de la LED simple du PCB final : */
#define APP_LED_ACTIVE_LEVEL    1

/* ------------------------------------------------------------------------ */
/* Timings des gestes                                                        */
/* ------------------------------------------------------------------------ */

/* Fenêtre entre un relâchement et l'appui suivant pour compter un multi-clic.
 * 180 ms s'est révélé trop court à l'usage (05/10/2026) : une double pression
 * « naturelle » laisse ~150-300 ms entre les deux appuis. Contrepartie : la
 * pression simple n'est confirmée (MultiPressComplete) qu'à la fin de cette
 * fenêtre, donc elle déclenche d'autant plus tard dans Maison. */
#define APP_SHORT_PRESS_MS       300
#define APP_LONG_PRESS_MS       1000   /* seuil LongPress                     */
#define APP_MULTI_PRESS_MAX        3   /* simple / double / triple, comme Hue */

/* Gestes système : maintien prolongé (docs/01-architecture.md §4) */
#define APP_HOLD_COMMISSION_MS  10000  /* rouvre la fenêtre de commissioning  */
#define APP_HOLD_FACTORY_MS     20000  /* factory reset                       */

/* ------------------------------------------------------------------------ */
/* Thread                                                                    */
/* ------------------------------------------------------------------------ */

/* Child timeout Thread, en secondes. Défaut OpenThread : 240 s.
 *
 * CRITIQUE pour l'autonomie. OpenThread calcule la période de data poll ainsi
 * (DataPollSender::CalculatePollPeriod) :
 *
 *     période = Min(child_timeout - marge, ICD_SLOW_POLL_INTERVAL_MS)
 *
 * Autrement dit, régler ICD_SLOW_POLL_INTERVAL_MS à 30 min **sans toucher au
 * child timeout** ne sert à rien : l'appareil pollera quand même toutes les
 * ~240 s. Il faut lever les deux.
 *
 * 3600 s laisse une marge de 2× sur le slow poll de 1800 s : l'appareil reste
 * enfant de son parent même s'il rate un poll.
 *
 * ⚠️ À valider avec ton border router : rien ne garantit qu'un parent accepte
 *    une valeur aussi longue, ni qu'il conserve l'entrée dans sa child table. */
#define APP_THREAD_CHILD_TIMEOUT_S  3600

/* ------------------------------------------------------------------------ */

/* Renseigné par app_main() une fois l'endpoint Generic Switch créé. */
extern uint16_t g_switch_endpoint_id;

/* Initialise le bouton et la LED. À appeler après esp_matter::start(). */
esp_err_t app_button_init(void);

/* Pile (app_battery.cpp) : mesure périodique publiée dans le cluster Power
 * Source de l'endpoint donné. À appeler après esp_matter::start(). */
esp_err_t app_battery_init(uint16_t endpoint_id);

/* Statistiques d'énergie (app_power_stats.cpp) : synthèse périodique du
 * temps éveillé et des réveils. Sans effet si CONFIG_APP_POWER_STATS=n. */
esp_err_t app_power_stats_init(void);

/* LED (app_led.cpp). app_led_blink est sans effet hors LED simple. */
esp_err_t app_led_init(void);
void app_led_blink(uint32_t on_ms);

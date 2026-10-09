/*
 * Statistiques d'énergie sans instrument de mesure (CONFIG_APP_POWER_STATS).
 *
 * ESP-IDF tient, avec CONFIG_PM_PROFILING, le temps passé dans chaque mode de
 * gestion d'énergie (dont le light sleep) et le nombre de mises en light
 * sleep. Toutes les APP_POWER_STATS_PERIOD_S secondes, on en tire une ligne :
 *
 *   énergie (300 s) : veille 99,95 %, 21 réveils, éveil moyen 7,1 ms ;
 *                     estimation 0,142 mC/réveil, ~38 µA en moyenne
 *
 * Le temps éveillé et le nombre de réveils sont MESURÉS par la puce. La charge
 * et le courant moyen sont une ESTIMATION : temps × courants de référence
 * réglables (menuconfig > SmartButton > Statistiques d'énergie). Ça suffit à
 * situer l'H2 entre « poll bon marché » (~0,1 mC) et « poll cher » (~5 mC),
 * en attendant une vraie mesure au PPK2.
 *
 * Les compteurs ne sont exposés que par esp_pm_dump_locks() : on capture son
 * texte en mémoire et on le relit.
 */

#include "app_priv.h"

#if CONFIG_APP_POWER_STATS

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <esp_log.h>
#include <esp_pm.h>
#include <esp_timer.h>

static const char *TAG = "energie";

struct pm_snapshot {
    int64_t uptime_us;
    int64_t sleep_us;
    uint32_t sleep_count;
    uint32_t reject_count;
};

static esp_timer_handle_t s_timer = nullptr;
static pm_snapshot s_prev = {};
static bool s_have_prev = false;

/* Relit la sortie texte de esp_pm_dump_locks(). Format (pm_locks.c,
 * pm_impl.c, ESP-IDF 5.5) :
 *   Time since bootup: 123456 us
 *   SLEEP     0  M        1234567     99%      <- "%-3uM" : largeur fixe, donc
 *                                                  une espace avant le M
 *   light_sleep_counts:42  light_sleep_reject_counts:3 */
static bool take_snapshot(pm_snapshot *out)
{
    char *buf = nullptr;
    size_t len = 0;
    FILE *f = open_memstream(&buf, &len);
    if (f == nullptr) {
        return false;
    }
    esp_pm_dump_locks(f);
    fclose(f);

    bool got_uptime = false, got_sleep = false, got_counts = false;
    *out = {};
    char *save = nullptr;
    for (char *line = strtok_r(buf, "\n", &save); line != nullptr; line = strtok_r(nullptr, "\n", &save)) {
        long long v = 0;
        unsigned long c = 0, r = 0;
        if (sscanf(line, "Time since bootup: %lld us", &v) == 1) {
            out->uptime_us = v;
            got_uptime = true;
        } else if (strncmp(line, "SLEEP", 5) == 0) {
            /* « %*u M » accepte « 96M » comme « 96 M ». */
            if (sscanf(line, "SLEEP %*u M %lld", &v) == 1) {
                out->sleep_us = v;
                got_sleep = true;
            } else {
                ESP_LOGW(TAG, "ligne SLEEP non reconnue : '%s'", line);
            }
        } else if (sscanf(line, "light_sleep_counts:%lu light_sleep_reject_counts:%lu", &c, &r) == 2) {
            out->sleep_count = c;
            out->reject_count = r;
            got_counts = true;
        }
    }
    free(buf);
    if (!(got_uptime && got_sleep && got_counts)) {
        ESP_LOGW(TAG, "relevé incomplet : durée %d, veille %d, compteurs %d",
                 got_uptime, got_sleep, got_counts);
    }
    return got_uptime && got_sleep && got_counts;
}

static void report(const pm_snapshot &cur, const pm_snapshot &prev)
{
    const int64_t span_us = cur.uptime_us - prev.uptime_us;
    const int64_t sleep_us = cur.sleep_us - prev.sleep_us;
    const int64_t awake_us = span_us - sleep_us;
    const uint32_t wakes = cur.sleep_count - prev.sleep_count;
    const uint32_t rejects = cur.reject_count - prev.reject_count;
    if (span_us <= 0 || wakes == 0) {
        ESP_LOGI(TAG, "aucune mise en veille sur la période (span %" PRId64 " ms)", span_us / 1000);
        return;
    }

    const double sleep_pct = 100.0 * static_cast<double>(sleep_us) / static_cast<double>(span_us);
    const double awake_per_wake_ms = static_cast<double>(awake_us) / wakes / 1000.0;

    /* Estimation : charge éveillée par réveil, et courant moyen. */
    const double active_ma = CONFIG_APP_POWER_STATS_ACTIVE_UA / 1000.0;
    const double mc_per_wake = active_ma * awake_per_wake_ms / 1000.0;
    const double avg_ua = (static_cast<double>(awake_us) * CONFIG_APP_POWER_STATS_ACTIVE_UA +
                           static_cast<double>(sleep_us) * CONFIG_APP_POWER_STATS_SLEEP_UA) /
                          static_cast<double>(span_us);

    ESP_LOGI(TAG, "%" PRId64 " s : veille %.2f %%, %" PRIu32 " réveils (%" PRIu32 " refusés), "
             "éveil moyen %.2f ms",
             span_us / 1000000, sleep_pct, wakes, rejects, awake_per_wake_ms);
    ESP_LOGI(TAG, "  estimation à %.0f mA éveillé / %d µA en veille : %.3f mC par réveil, "
             "~%.0f µA en moyenne",
             active_ma, CONFIG_APP_POWER_STATS_SLEEP_UA, mc_per_wake, avg_ua);
}

static void timer_cb(void *)
{
    pm_snapshot cur;
    if (!take_snapshot(&cur)) {
        ESP_LOGW(TAG, "compteurs de gestion d'énergie illisibles");
        return;
    }
    if (s_have_prev) {
        report(cur, s_prev);
    }
    s_prev = cur;
    s_have_prev = true;
}

esp_err_t app_power_stats_init(void)
{
    esp_timer_create_args_t args = {};
    args.callback = timer_cb;
    args.name = "power_stats";
    esp_err_t err = esp_timer_create(&args, &s_timer);
    if (err != ESP_OK) {
        return err;
    }
    /* Premier relevé tout de suite : il sert de référence, pour que la
     * première synthèse n'englobe pas le démarrage. */
    timer_cb(nullptr);
    return esp_timer_start_periodic(s_timer,
                                    static_cast<uint64_t>(CONFIG_APP_POWER_STATS_PERIOD_S) * 1000000ULL);
}

#else

esp_err_t app_power_stats_init(void) { return ESP_OK; }

#endif

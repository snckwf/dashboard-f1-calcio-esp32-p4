/* ============================================================================
 *  sport_p4
 *  Formula 1 e calcio su Waveshare ESP32-P4-WIFI6-Touch-LCD-7B
 *
 *  F1      LIVE (con gomme), MAPPA del circuito in tempo reale, PILOTI, COSTRUTTORI,
 *          ULTIMA GARA  -  fonti: livetiming.formula1.com + api.jolpi.ca
 *  CALCIO  Serie A (con classifica), Nations League, Champions League,
 *          Europa League, Europei, Mondiali  -  fonte: ESPN
 *          Risultati in tempo reale: avviso + suono per gol, rossi e fischi.
 *          Tocco su una partita: cronaca, statistiche e formazioni.
 *
 *  Nessuna chiave API, nessun account.
 *
 *  Configurazione da browser: http://sport-p4.local (volume, luminosita',
 *  standby, avvisi, competizioni mostrate).
 *
 *  Il layer hardware (BoardLvgl + Audio + lv_conf + i due file di conf del
 *  pannello) e' preso identico da bitaxe_dashboard_p4 / meteo_p4.
 *
 *  Librerie necessarie (Gestore librerie di Arduino IDE):
 *    - ESP32_Display_Panel   (espressif)
 *    - ESP32_IO_Expander     (dipendenza della precedente)
 *    - esp-lib-utils         (dipendenza della precedente)
 *    - lvgl 9.x
 *    - ArduinoJson 7.x
 *
 *  Scheda: "ESP32P4 Dev Module", PSRAM Enabled, Flash 16MB,
 *  Partition Scheme "16M Flash (3MB APP/9.9MB FATFS)".
 * ========================================================================== */
#include <Arduino.h>
#include <WiFi.h>
#include <time.h>
#include "esp_heap_caps.h"

#include "AppConfig.h"
#include "Audio.h"
#include "BoardLvgl.h"
#include "Clients.h"
#include "DataModel.h"
#include "Notify.h"
#include "Ui.h"
#include "Impostazioni.h"

/* L'RSSI passa da una chiamata RPC verso il C6 (ESP-Hosted): se il C6 e'
 * occupato la risposta tarda e la chiamata blocca per secondi. Lo si legge
 * quindi di rado e si tiene l'ultimo valore. */
static void aggiornaStatoWifi()
{
    static int8_t   rssi = 0;
    static uint32_t ultimoRssi = 0;
    char text[80];
    if (WiFi.status() == WL_CONNECTED) {
        if (ultimoRssi == 0 || millis() - ultimoRssi >= 60000) {
            rssi = (int8_t)WiFi.RSSI();
            ultimoRssi = millis() | 1;
        }
        const IPAddress ip = WiFi.localIP();
        if (rssi != 0) snprintf(text, sizeof(text), "%s  %u.%u.%u.%u  %d dBm", WIFI_SSID, ip[0], ip[1], ip[2], ip[3], rssi);
        else           snprintf(text, sizeof(text), "%s  %u.%u.%u.%u", WIFI_SSID, ip[0], ip[1], ip[2], ip[3]);
    } else {
        snprintf(text, sizeof(text), "connessione a %s", WIFI_SSID);
    }
    setWifiStatusText(text);
}

static void connettiWifi()
{
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    Serial.printf("Connessione a %s\n", WIFI_SSID);
    const uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 30000) {
        setWifiStatusText("connessione WiFi...");
        delay(500);
        Serial.print('.');
    }
    Serial.println();
    aggiornaStatoWifi();
    Serial.printf("WiFi: %s\n", getWifiStatusText());

    /* orario locale con cambio ora legale automatico (serve anche per le date ESPN) */
    configTzTime(TZ_ITALIA, "it.pool.ntp.org", "pool.ntp.org", "time.google.com");
    if (WiFi.status() == WL_CONNECTED) {
        struct tm t;
        for (uint8_t i = 0; i < 20 && !getLocalTime(&t, 500); ++i) delay(100);
    }
}

/* tiene d'occhio il WiFi e aggiorna il testo nell'header */
static void wifiTask(void *arg)
{
    (void)arg;
    while (true) {
        aggiornaStatoWifi();
        if (WiFi.status() != WL_CONNECTED) {
            WiFi.reconnect();
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

void setup()
{
    Serial.begin(115200);
    delay(300);
    Serial.println();
    Serial.println("=== sport_p4 ===");

    initDataModel();
    impostazioniCarica();
    notifyInit();

    if (initBoardLvgl()) {
        const unsigned primaUi = (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        if (lvglLock(-1)) {
            uiCreate();
            lvglUnlock();
        }
        Serial.printf("[MEM] RAM interna libera: %u prima della UI, %u dopo\n", primaUi,
                      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        Serial.printf("Touch: %s\n", boardTouchStatus());
    } else {
        Serial.println("Init display/LVGL fallito");
    }

    /* Audio: se il codec non risponde lo sketch prosegue muto, senza bloccarsi */
    audioSetVolume(imp().volume);
    if (audioBegin()) {
        audioPlay(SND_STARTUP);
    } else {
        Serial.println("Audio non disponibile, avvisi solo a schermo");
    }

    impostazioniApplica();       /* luminosita' e standby salvati */

    connettiWifi();

    xTaskCreatePinnedToCore(wifiTask,   "wifi",   4096,  nullptr, 1, nullptr, 0);
    xTaskCreatePinnedToCore(calcioTask, "calcio", 16384, nullptr, 1, nullptr, 0);
    xTaskCreatePinnedToCore(f1Task,     "f1",     12288, nullptr, 1, nullptr, 0);
    /* 32 KB: TLS del WebSocket + download HTTPS della sagoma del circuito */
    xTaskCreatePinnedToCore(f1LiveTask, "f1live", 32768, nullptr, 2, nullptr, 0);
    webBegin();
}

void loop()
{
    /* comandi da seriale (115200): d = prova suoni, m = suoni avvisi on/off,
     * g = finto gol in Nations League, f = finto avviso F1 (prova del risveglio) */
    while (Serial.available() > 0) {
        const int c = Serial.read();
        if (c == 'g') {
            notifySchermata(1, 1);
            notifica(NOT_GOL, SND_GOL, "PROVA: GOL Italia!   Italia 1-0 Norvegia   23'   NATIONS");
        } else if (c == 'f') {
            notifySchermata(0, 0);
            notifica(NOT_F1, SND_F1_PASSAGGIO, "PROVA: F1  Nuovo leader");
        } else if (c == 'd') {
            Serial.println("[AUDIO] prova suoni");
            notifyDemoSuoni();
        } else if (c == 'm') {
            notifySetSuoni(!notifySuoni());
            Serial.printf("[AUDIO] suoni avvisi %s\n", notifySuoni() ? "ON" : "OFF");
        }
    }

    static uint32_t ultimo = 0;
    if (millis() - ultimo >= 60000) {
        ultimo = millis();
        Serial.printf("[MEM] interna libera %u, PSRAM libera %u\n",
                      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                      (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    }
    delay(1000);
}

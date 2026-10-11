/* ============================================================================
 *  sport_p4 - configurazione
 *  Scheda: Waveshare ESP32-P4-WIFI6-Touch-LCD-7B (1024x600, EK79007, GT911)
 *
 *  La sezione hardware e' identica a bitaxe_dashboard_p4 / meteo_p4: e' il
 *  layer gia' validato su questa scheda, non va toccato.
 * ========================================================================== */
#pragma once

#include <Arduino.h>

/* ---- Schermo ------------------------------------------------------------- */
static constexpr uint16_t SCREEN_WIDTH  = 1024;
static constexpr uint16_t SCREEN_HEIGHT = 600;

/* ---- WiFi ---------------------------------------------------------------- */
/* Inserisci qui la tua rete WiFi (attenzione a maiuscole/minuscole). */
static constexpr char WIFI_SSID[]     = "LA_TUA_RETE";
static constexpr char WIFI_PASSWORD[] = "LA_TUA_PASSWORD";

/* Fuso orario Italia con cambio ora legale automatico */
static constexpr char TZ_ITALIA[] = "CET-1CEST,M3.5.0,M10.5.0/3";

static constexpr uint32_t WEB_HTTP_TIMEOUT_MS = 12000;

/* ---- Calcio (ESPN, nessuna chiave API) ------------------------------------ */
static constexpr uint8_t CALCIO_COMP_COUNT = 6;
static const char *const CALCIO_SLUG[CALCIO_COMP_COUNT] = {
    "ita.1", "uefa.nations", "uefa.champions", "uefa.europa", "uefa.euro", "fifa.world",
};
static const char *const CALCIO_TAB[CALCIO_COMP_COUNT] = {
    "SERIE A", "NATIONS", "CHAMPIONS", "EUROPA L.", "EUROPEI", "MONDIALI",
};
static const char *const CALCIO_NOME[CALCIO_COMP_COUNT] = {
    "Serie A", "UEFA Nations League", "UEFA Champions League",
    "UEFA Europa League", "Campionato Europeo UEFA", "Campionato Mondiale FIFA",
};
/* Finestra delle partite mostrate: da 2 giorni fa a 7 giorni avanti
 * (una settimana intera copre anche le soste per le nazionali).
 * Se la finestra e' vuota (Europei/Mondiali fuori stagione) si mostra la
 * giornata che ESPN considera "corrente": di solito l'ultima giocata.       */
static constexpr int8_t   CALCIO_GIORNI_PRIMA       = 2;
static constexpr int8_t   CALCIO_GIORNI_DOPO        = 7;
static constexpr uint32_t CALCIO_LISTA_VISIBILE_MS  = 600000;   /* 10 min  */
static constexpr uint32_t CALCIO_LISTA_ALTRE_MS     = 3600000;  /* 60 min  */
static constexpr uint32_t CALCIO_LIVE_IDLE_MS       = 60000;    /* 60 s    */
static constexpr uint32_t CALCIO_LIVE_ATTIVO_MS     = 20000;    /* 20 s con partite in corso */
static constexpr uint32_t CALCIO_CLASSIFICA_MS      = 1800000;  /* 30 min  */
/* per quanti ms il punteggio resta evidenziato dopo un gol */
static constexpr uint32_t CALCIO_GOL_EVIDENZA_MS    = 120000;

/* ---- Formula 1 ------------------------------------------------------------ */
/* Jolpica (ex Ergast): classifiche, ultima gara, calendario.
 * livetiming.formula1.com (SignalR Core, senza login): tempo reale.         */
static constexpr uint32_t F1_JOLPICA_MS        = 1800000;  /* 30 min          */
static constexpr uint32_t F1_JOLPICA_POSTGARA_MS = 600000; /* 10 min dopo una gara */
static constexpr uint32_t F1_LIVE_CONTROLLO_MS = 600000;   /* fuori sessione: ogni 10 min */
static constexpr uint32_t F1_LIVE_RETRY_MS     = 30000;    /* in sessione: ritenta ogni 30 s */
static constexpr uint32_t F1_LIVE_MARGINE_S    = 1800;     /* finestra sessione +-30 min */
static constexpr uint32_t F1_DELTA_POS_MS      = 15000;    /* frecce sorpasso visibili 15 s */

/* ---- Notifiche ------------------------------------------------------------ */
static constexpr uint32_t NOTIFICA_DURATA_MS = 20000;

/* ---- Risparmio schermo --------------------------------------------------- */
/* 0 = schermo sempre acceso.
 * Risveglio: un tocco (serve solo a riaccendere, non preme nulla) oppure un
 * avviso (gol, fischio, bandiera...), che porta anche alla schermata giusta. */
static constexpr uint32_t SCREEN_TIMEOUT_MS = 120000;   /* standby dopo 2 minuti */

/* ---- Display: pin e timing (dallo sketch gia' validato) ------------------ */
static constexpr int  LCD_RST_IO      = 33;
static constexpr int  LCD_BL_IO       = 32;
static constexpr bool LCD_BL_ON_LEVEL = 0;      /* retroilluminazione ATTIVA BASSA */
static constexpr int  TOUCH_SDA_IO    = 7;
static constexpr int  TOUCH_SCL_IO    = 8;
static constexpr int  TOUCH_I2C_NUM   = 1;
static constexpr int  TOUCH_RST_IO    = -1;
static constexpr int  TOUCH_INT_IO    = -1;

static constexpr int LCD_DSI_PHY_LDO_ID     = 3;
static constexpr int LCD_DSI_LANE_NUM       = 2;
static constexpr int LCD_DSI_LANE_RATE_MBPS = 1000;
static constexpr int LCD_DPI_CLOCK_MHZ      = 52;
static constexpr int LCD_HSYNC_PULSE_WIDTH  = 10;
static constexpr int LCD_HSYNC_BACK_PORCH   = 160;
static constexpr int LCD_HSYNC_FRONT_PORCH  = 160;
static constexpr int LCD_VSYNC_PULSE_WIDTH  = 1;
static constexpr int LCD_VSYNC_BACK_PORCH   = 23;
static constexpr int LCD_VSYNC_FRONT_PORCH  = 12;

/* ---- Orientamento -------------------------------------------------------- */
/* Il pannello e' montato capovolto: l'immagine va ruotata di 180 gradi in
 * software. Il touch, di conseguenza, va lasciato cosi' com'e'.             */
static constexpr bool DISPLAY_ROTATE_180 = true;
static constexpr bool TOUCH_MIRROR_X     = false;
static constexpr bool TOUCH_MIRROR_Y     = false;
static constexpr bool TOUCH_SWAP_XY      = false;

/* 1 = mostra le coordinate del tocco nell'header (diagnostica) */
#define SHOW_TOUCH_DEBUG 0

/* ---- Audio (codec ES8311 + altoparlante in dotazione) -------------------- */
/* 75 = 0 dB sul registro di volume del DAC. Sopra si satura. */
static constexpr uint8_t AUDIO_VOLUME     = 75;
static constexpr bool    AUDIO_TOUCH_BEEP = true;

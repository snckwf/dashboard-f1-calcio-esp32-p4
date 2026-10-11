/* ============================================================================
 *  Audio - jingle chiptune sullo speaker della scheda (codec ES8311)
 *
 *  Onde quadre con duty cycle variabile, come i canali pulse del NES.
 *  Nessun file audio: tutto sintetizzato, poche centinaia di byte di tabelle.
 * ========================================================================== */
#pragma once

#include <Arduino.h>

enum SoundEvent : uint8_t {
    SND_TOUCH = 0,     /* blip secco a ogni tocco                            */
    SND_MINER_DOWN,    /* discesa "power down": un miner non risponde piu'   */
    SND_TEMP_ALARM,    /* sirena a due toni: temperatura critica             */
    SND_BEST_SHARE,    /* arpeggio ascendente: nuovo record di best share    */
    SND_BLOCK_FOUND,   /* fanfara di vittoria: blocco trovato                */
    SND_STARTUP,       /* accordo breve all'avvio, conferma che l'audio va   */

    /* ---- suoni sintetizzati in tempo reale (sport_p4) ---- */
    SND_F1_PASSAGGIO,        /* rombo di una monoposto che passa (effetto Doppler) */
    SND_F1_PARTENZA,         /* 5 luci, spegnimento, motore che sale di giri       */
    SND_FISCHIO_INIZIO,      /* fischio lungo dell'arbitro                         */
    SND_FISCHIO_INTERVALLO,  /* due fischi                                         */
    SND_FISCHIO_FINE,        /* tre fischi: corto, corto, lungo                    */
    SND_GOL,                 /* sirena da stadio + boato della folla               */
    SND_ESPULSIONE,          /* fischio deciso + tre note discendenti (rosso)      */
    SND_COUNT
};

/* i suoni da qui in poi non sono jingle a note ma generatori procedurali */
static constexpr uint8_t SND_PRIMO_SINTETIZZATO = SND_F1_PASSAGGIO;

/* Inizializza codec e I2S. Ritorna false se il codec non risponde:
 * in quel caso audioPlay() diventa una no-op e il resto continua a funzionare. */
bool audioBegin();

/* Non bloccante: accoda il jingle e torna subito. Chiamabile da qualsiasi task. */
void audioPlay(SoundEvent event);

bool audioReady();

/* 0..100. Agisce sul registro di volume del DAC. */
void audioSetVolume(uint8_t percent);
uint8_t audioGetVolume();

/* Diagnostica leggibile, da stampare su seriale */
const char *audioStatus();

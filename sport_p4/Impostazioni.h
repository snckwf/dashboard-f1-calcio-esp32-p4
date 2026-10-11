/* ============================================================================
 *  Impostazioni - valori regolabili dalla pagina web, salvati in NVS
 *  (namespace "sport_p4"), quindi sopravvivono a riavvii e aggiornamenti.
 * ========================================================================== */
#pragma once

#include <Arduino.h>

struct Impostazioni {
    uint8_t  volume;        /* 0..100, 75 = 0 dB sul DAC                   */
    uint8_t  luminosita;    /* 5..100 %                                     */
    uint16_t standbyMin;    /* minuti senza tocchi prima dello standby, 0 = mai */
    bool     suoni;         /* suoni degli avvisi                           */
    bool     avvisiCalcio;  /* banner + suono per gol, fischi...            */
    bool     avvisiF1;      /* banner + suono per eventi F1                 */
    bool     avvisiRossi;   /* avviso per i cartellini rossi                */
    uint8_t  compAttive;    /* bit 0..5 = competizioni mostrate             */
};

/* carica da NVS e applica volume, luminosita' e standby */
void          impostazioniCarica();
void          impostazioniSalva();
Impostazioni &imp();
/* applica all'hardware i valori correnti (volume, luminosita', standby) */
void          impostazioniApplica();
/* true se la competizione c e' attiva */
bool          compAttiva(uint8_t c);

/* server web di configurazione (porta 80, http://sport-p4.local) */
void          webBegin();

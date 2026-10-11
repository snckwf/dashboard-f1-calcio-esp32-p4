/* ============================================================================
 *  Notify - coda di avvisi mostrati nel banner sotto l'header
 *  (gol, bandiere F1, cambio leader...). Chiamabile da qualsiasi task.
 * ========================================================================== */
#pragma once

#include <Arduino.h>
#include "Audio.h"

enum NotificaTipo : uint8_t {
    NOT_INFO = 0,     /* blu    */
    NOT_GOL,          /* giallo */
    NOT_F1,           /* rosso  */
    NOT_OK,           /* verde  */
};

void notifyInit();
/* suono = SND_COUNT per nessun suono */
void notifica(NotificaTipo tipo, SoundEvent suono, const char *fmt, ...);

/* Per il banner: true se c'e' un avviso attivo da mostrare adesso */
bool notificaCorrente(char *testo, size_t n, NotificaTipo *tipo);

/* Suoni degli avvisi on/off (memorizzato in NVS, sopravvive al riavvio).
 * Il bip del tocco non dipende da questa impostazione. */
void notifySetSuoni(bool on);
bool notifySuoni();

/* Fa sentire in fila tutti i suoni degli avvisi */
void notifyDemoSuoni();

/* Schermata a cui porta il prossimo avviso se lo schermo e' in standby.
 * modo 0 = F1 (sub = scheda), modo 1 = calcio (sub = competizione).
 * Va chiamata subito prima di notifica(). */
void notifySchermata(uint8_t modo, uint8_t sub);

/* Per la UI: true se dall'ultima chiamata e' arrivato un avviso.
 * modo = 255 se l'avviso non indica una schermata. */
bool notifyPrendiSveglia(uint8_t *modo, uint8_t *sub);

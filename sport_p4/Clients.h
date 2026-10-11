/* ============================================================================
 *  Clients - task di rete
 *
 *    calcioTask   ESPN site.api.espn.com   (risultati, live, classifica)
 *    f1Task       api.jolpi.ca             (classifiche, ultima gara, calendario)
 *    f1LiveTask   livetiming.formula1.com  (SignalR Core via WebSocket, tempo reale)
 * ========================================================================== */
#pragma once

#include <Arduino.h>

/* GET HTTPS, corpo in out. Ritorna il codice HTTP (o negativo se errore). */
int  httpGet(const String &url, String &out);

void calcioTask(void *arg);
void f1Task(void *arg);
void f1LiveTask(void *arg);

/* true se il WiFi e' connesso e l'orologio e' sincronizzato */
bool reteOk();

/* una sola apertura di connessione HTTPS/TLS alla volta */
bool reteLock(uint32_t timeoutMs);
void reteUnlock();

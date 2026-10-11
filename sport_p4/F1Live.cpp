/* ============================================================================
 *  F1Live - tempo reale dal live timing ufficiale (livetiming.formula1.com)
 *
 *  Protocollo: SignalR Core su WebSocket, formato JSON. Non serve login per i
 *  topic usati qui (verificato il 04/10/2026):
 *    1. POST /signalrcore/negotiate?negotiateVersion=1  -> connectionToken + cookie AWSALB
 *    2. GET  /signalrcore?id=<token>  con Upgrade: websocket e lo stesso cookie
 *    3. {"protocol":"json","version":1}<0x1E>
 *    4. Subscribe ai topic -> risposta type 3 con lo stato completo,
 *       poi messaggi type 1 target "feed" con le sole differenze.
 *
 *  Il client WebSocket e' scritto qui sopra NetworkClientSecure: cosi' non ci
 *  sono limiti di dimensione sui messaggi (lo stato iniziale supera i 40 KB) e
 *  il buffer sta in PSRAM.
 *
 *  Fuori dalle sessioni ci si collega ogni 10 minuti solo per leggere lo stato
 *  e poi si chiude; durante la finestra di una sessione si resta collegati.
 * ========================================================================== */
#include <ArduinoJson.h>
#include "PsramJson.h"
#include <NetworkClientSecure.h>
#include <WiFi.h>

#include "AppConfig.h"
#include "Clients.h"
#include "DataModel.h"
#include "Notify.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "mbedtls/base64.h"
#include "miniz.h"            /* tinfl in ROM: Position.z e' compresso con deflate */
#include <Preferences.h>

static const char HOST[] = "livetiming.formula1.com";
static const char TOPIC_SUBSCRIBE[] =
    "{\"type\":1,\"target\":\"Subscribe\",\"invocationId\":\"0\",\"arguments\":[["
    "\"SessionInfo\",\"SessionStatus\",\"LapCount\",\"TrackStatus\","
    "\"DriverList\",\"TimingData\",\"RaceControlMessages\","
    "\"TimingAppData\",\"Position.z\"]]}\x1e";

static constexpr size_t MSG_CAP = 512 * 1024;

static NetworkClientSecure s_cli;
static char    *s_msg = nullptr;
static size_t   s_len = 0;
static bool     s_overflow = false;
static bool     s_connesso = false;
static char     s_cookie[512];
static char     s_token[128];
static char     s_leader[4] = "";

/* sagoma del circuito: caricamento/salvataggio in NVS fatti dal task, fuori dal lock */
static volatile uint16_t s_trkDaCaricare = 0;
static volatile bool     s_trkDaSalvare  = false;
static char              s_rifNum[4]     = "";     /* monoposto che "disegna" la pista */
static uint8_t          *s_b64   = nullptr;        /* buffer base64 decodificato  */
static char             *s_infl  = nullptr;        /* buffer JSON decompresso     */
static constexpr size_t  B64_CAP  = 16 * 1024;
static constexpr size_t  INFL_CAP = 64 * 1024;

/* ===========================================================================
 *  Socket: lettura con timeout
 * ========================================================================= */
static bool leggiEsatti(NetworkClientSecure &c, uint8_t *buf, size_t n, uint32_t timeoutMs)
{
    size_t got = 0;
    uint32_t t0 = millis();
    while (got < n) {
        const int disp = c.available();
        if (disp > 0) {
            const int r = c.read(buf + got, n - got);
            if (r > 0) { got += (size_t)r; t0 = millis(); continue; }
        } else if (!c.connected()) {
            return false;
        }
        if (millis() - t0 > timeoutMs) return false;
        vTaskDelay(1);
    }
    return true;
}

static bool leggiRiga(NetworkClientSecure &c, char *buf, size_t n, uint32_t timeoutMs)
{
    size_t i = 0;
    uint32_t t0 = millis();
    while (true) {
        if (c.available() > 0) {
            const int ch = c.read();
            if (ch < 0) continue;
            t0 = millis();
            if (ch == '\n') {
                if (i > 0 && buf[i - 1] == '\r') --i;
                buf[i] = 0;
                return true;
            }
            if (i + 1 < n) buf[i++] = (char)ch;
        } else {
            if (!c.connected()) { buf[i] = 0; return i > 0; }
            if (millis() - t0 > timeoutMs) return false;
            vTaskDelay(1);
        }
    }
}

/* ===========================================================================
 *  1. negotiate
 * ========================================================================= */
static void urlEncode(const char *in, char *out, size_t n)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (; *in && o + 4 < n; ++in) {
        const char c = *in;
        if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out[o++] = c;
        } else {
            out[o++] = '%';
            out[o++] = hex[((uint8_t)c) >> 4];
            out[o++] = hex[((uint8_t)c) & 0x0F];
        }
    }
    out[o] = 0;
}

static bool negozia()
{
    NetworkClientSecure c;
    c.setInsecure();
    c.setTimeout(10);
    if (!c.connect(HOST, 443)) {
        Serial.println("[F1LIVE] negotiate: connessione TLS fallita");
        return false;
    }
    c.print("POST /signalrcore/negotiate?negotiateVersion=1 HTTP/1.1\r\n"
            "Host: livetiming.formula1.com\r\n"
            "User-Agent: BestHTTP\r\n"
            "Accept: application/json\r\n"
            "Content-Length: 0\r\n"
            "Connection: close\r\n\r\n");

    char riga[600];
    if (!leggiRiga(c, riga, sizeof(riga), 10000)) { c.stop(); return false; }
    const char *sp = strchr(riga, ' ');
    const int status = sp ? atoi(sp + 1) : 0;
    if (status != 200) {
        Serial.printf("[F1LIVE] negotiate HTTP %d\n", status);
        c.stop();
        return false;
    }

    s_cookie[0] = 0;
    long clen = -1;
    bool chunked = false;
    while (leggiRiga(c, riga, sizeof(riga), 10000)) {
        if (riga[0] == 0) break;
        if (strncasecmp(riga, "set-cookie:", 11) == 0) {
            const char *v = riga + 11;
            while (*v == ' ') ++v;
            const size_t l = strcspn(v, ";");
            const size_t cur = strlen(s_cookie);
            if (cur + l + 3 < sizeof(s_cookie)) {
                if (cur > 0) strcat(s_cookie, "; ");
                strncat(s_cookie, v, l);
            }
        } else if (strncasecmp(riga, "content-length:", 15) == 0) {
            clen = atol(riga + 15);
        } else if (strncasecmp(riga, "transfer-encoding:", 18) == 0 && strstr(riga, "chunked")) {
            chunked = true;
        }
    }

    String body;
    if (chunked) {
        while (leggiRiga(c, riga, sizeof(riga), 10000)) {
            const long sz = strtol(riga, nullptr, 16);
            if (sz <= 0) break;
            uint8_t tmp[256];
            long resto = sz;
            while (resto > 0) {
                const size_t blocco = resto > (long)sizeof(tmp) ? sizeof(tmp) : (size_t)resto;
                if (!leggiEsatti(c, tmp, blocco, 10000)) { resto = -1; break; }
                body.concat((const char *)tmp, blocco);
                resto -= (long)blocco;
            }
            if (resto < 0) break;
            leggiRiga(c, riga, sizeof(riga), 5000);   /* CRLF dopo il blocco */
        }
    } else {
        const uint32_t t0 = millis();
        while ((clen < 0 || (long)body.length() < clen) && millis() - t0 < 10000) {
            if (c.available() > 0) {
                const int ch = c.read();
                if (ch >= 0) body += (char)ch;
            } else if (!c.connected()) {
                break;
            } else {
                vTaskDelay(1);
            }
        }
    }
    c.stop();

    JsonDocument doc(PSRAM_JSON);
    if (deserializeJson(doc, body, DeserializationOption::NestingLimit(40))) {
        Serial.println("[F1LIVE] negotiate: JSON non valido");
        return false;
    }
    const char *tok = doc["connectionToken"] | (doc["connectionId"] | "");
    if (tok[0] == 0) return false;
    snprintf(s_token, sizeof(s_token), "%s", tok);
    return true;
}

/* ===========================================================================
 *  2. WebSocket
 * ========================================================================= */
static bool inviaFrame(uint8_t opcode, const uint8_t *data, size_t len)
{
    uint8_t hdr[14];
    size_t h = 0;
    hdr[h++] = 0x80 | opcode;
    if (len < 126) {
        hdr[h++] = 0x80 | (uint8_t)len;
    } else if (len < 65536) {
        hdr[h++] = 0x80 | 126;
        hdr[h++] = (uint8_t)(len >> 8);
        hdr[h++] = (uint8_t)(len & 0xFF);
    } else {
        hdr[h++] = 0x80 | 127;
        for (int i = 7; i >= 0; --i) hdr[h++] = (uint8_t)(((uint64_t)len >> (8 * i)) & 0xFF);
    }
    const uint32_t r = esp_random();
    uint8_t mask[4];
    memcpy(mask, &r, 4);
    memcpy(hdr + h, mask, 4);
    h += 4;

    uint8_t *buf = static_cast<uint8_t *>(malloc(h + len));
    if (buf == nullptr) return false;
    memcpy(buf, hdr, h);
    for (size_t i = 0; i < len; ++i) buf[h + i] = data[i] ^ mask[i & 3];
    const size_t scritti = s_cli.write(buf, h + len);
    free(buf);
    return scritti == h + len;
}

static bool inviaTesto(const char *s)
{
    return inviaFrame(0x1, reinterpret_cast<const uint8_t *>(s), strlen(s));
}

static void chiudi()
{
    if (s_connesso) Serial.println("[F1LIVE] disconnesso");
    s_cli.stop();
    s_connesso = false;
    s_len = 0;
    if (dataLock(500)) {
        if (g_f1live->connesso) { g_f1live->connesso = false; g_f1live->versione++; }
        dataUnlock();
    }
}

static bool connettiInterno();

static bool connetti()
{
    if (!reteLock(60000)) return false;
    const bool ok = connettiInterno();
    reteUnlock();
    if (!ok) Serial.printf("[F1LIVE] RAM interna libera %u\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    return ok;
}

static bool connettiInterno()
{
    if (!negozia()) return false;

    s_cli.setInsecure();
    s_cli.setTimeout(10);
    if (!s_cli.connect(HOST, 443)) {
        Serial.println("[F1LIVE] websocket: connessione TLS fallita");
        return false;
    }

    uint8_t chiave[16];
    esp_fill_random(chiave, sizeof(chiave));
    unsigned char chiave64[32];
    size_t l64 = 0;
    mbedtls_base64_encode(chiave64, sizeof(chiave64), &l64, chiave, sizeof(chiave));
    chiave64[l64] = 0;

    char tok[256];
    urlEncode(s_token, tok, sizeof(tok));

    String req;
    req.reserve(900);
    req += "GET /signalrcore?id=";
    req += tok;
    req += " HTTP/1.1\r\nHost: livetiming.formula1.com\r\n"
           "User-Agent: BestHTTP\r\n"
           "Upgrade: websocket\r\nConnection: Upgrade\r\n"
           "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: ";
    req += (const char *)chiave64;
    req += "\r\n";
    if (s_cookie[0] != 0) {
        req += "Cookie: ";
        req += s_cookie;
        req += "\r\n";
    }
    req += "\r\n";
    s_cli.print(req);

    char riga[300];
    if (!leggiRiga(s_cli, riga, sizeof(riga), 10000) || strstr(riga, " 101") == nullptr) {
        Serial.printf("[F1LIVE] upgrade rifiutato: %s\n", riga);
        s_cli.stop();
        return false;
    }
    while (leggiRiga(s_cli, riga, sizeof(riga), 10000)) {
        if (riga[0] == 0) break;
    }

    s_connesso = true;
    s_len = 0;
    s_overflow = false;
    if (!inviaTesto("{\"protocol\":\"json\",\"version\":1}\x1e") || !inviaTesto(TOPIC_SUBSCRIBE)) {
        chiudi();
        return false;
    }
    if (dataLock(500)) { g_f1live->connesso = true; g_f1live->versione++; dataUnlock(); }
    Serial.println("[F1LIVE] collegato al live timing");
    return true;
}

/* 1 = messaggio completo in s_msg, 0 = niente di nuovo, -1 = connessione chiusa */
static int leggiFrame()
{
    if (s_cli.available() <= 0) return s_cli.connected() ? 0 : -1;

    uint8_t h[2];
    if (!leggiEsatti(s_cli, h, 2, 5000)) return -1;
    const bool fin = (h[0] & 0x80) != 0;
    const uint8_t op = h[0] & 0x0F;
    const bool masked = (h[1] & 0x80) != 0;
    uint64_t len = h[1] & 0x7F;
    if (len == 126) {
        uint8_t e[2];
        if (!leggiEsatti(s_cli, e, 2, 5000)) return -1;
        len = ((uint64_t)e[0] << 8) | e[1];
    } else if (len == 127) {
        uint8_t e[8];
        if (!leggiEsatti(s_cli, e, 8, 5000)) return -1;
        len = 0;
        for (int i = 0; i < 8; ++i) len = (len << 8) | e[i];
    }
    uint8_t mask[4] = { 0, 0, 0, 0 };
    if (masked && !leggiEsatti(s_cli, mask, 4, 5000)) return -1;

    if (op == 0x8) return -1;                        /* close */
    if (op == 0x9 || op == 0xA) {                    /* ping / pong */
        uint8_t p[125];
        const size_t n = len > sizeof(p) ? sizeof(p) : (size_t)len;
        if (!leggiEsatti(s_cli, p, n, 5000)) return -1;
        if (op == 0x9) inviaFrame(0xA, p, n);
        return 0;
    }

    if (op == 0x1 || op == 0x2) { s_len = 0; s_overflow = false; }

    if (s_overflow || s_len + len + 1 > MSG_CAP) {
        /* troppo grande: lo scarto senza perdere la sincronia dei frame */
        s_overflow = true;
        uint8_t tmp[512];
        uint64_t resto = len;
        while (resto > 0) {
            const size_t b = resto > sizeof(tmp) ? sizeof(tmp) : (size_t)resto;
            if (!leggiEsatti(s_cli, tmp, b, 10000)) return -1;
            resto -= b;
        }
    } else {
        if (!leggiEsatti(s_cli, reinterpret_cast<uint8_t *>(s_msg + s_len), (size_t)len, 15000)) return -1;
        if (masked) for (uint64_t i = 0; i < len; ++i) s_msg[s_len + i] ^= mask[i & 3];
        s_len += (size_t)len;
    }

    if (!fin) return 0;
    if (s_overflow) {
        Serial.println("[F1LIVE] messaggio troppo grande, scartato");
        s_overflow = false;
        s_len = 0;
        return 0;
    }
    s_msg[s_len] = 0;
    return 1;
}

/* ===========================================================================
 *  3. Applicazione dei dati (con dataLock preso)
 * ========================================================================= */
static void nomeSessione(const char *en, char *out, size_t n)
{
    if (strcmp(en, "Race") == 0)                    snprintf(out, n, "Gara");
    else if (strcmp(en, "Qualifying") == 0)         snprintf(out, n, "Qualifiche");
    else if (strncmp(en, "Sprint Q", 8) == 0 ||
             strncmp(en, "Sprint S", 8) == 0)       snprintf(out, n, "Sprint Q.");
    else if (strncmp(en, "Practice ", 9) == 0)      snprintf(out, n, "Libere %s", en + 9);
    else                                            asciiFold(en, out, n);
}

static F1LivePilota *pilota(F1Live &L, const char *num, bool crea)
{
    for (uint8_t i = 0; i < L.n; ++i) {
        if (strcmp(L.p[i].num, num) == 0) return &L.p[i];
    }
    if (!crea || L.n >= 24 || num[0] == '_') return nullptr;
    F1LivePilota &p = L.p[L.n++];
    memset(&p, 0, sizeof(p));
    snprintf(p.num, sizeof(p.num), "%s", num);
    snprintf(p.tla, sizeof(p.tla), "%s", num);
    p.colore = 0x8B949E;
    return &p;
}

static void ordinaPiloti(F1Live &L)
{
    for (uint8_t i = 1; i < L.n; ++i) {
        F1LivePilota x = L.p[i];
        const uint8_t kx = x.pos == 0 ? 255 : x.pos;
        int j = i - 1;
        while (j >= 0 && (L.p[j].pos == 0 ? 255 : L.p[j].pos) > kx) { L.p[j + 1] = L.p[j]; --j; }
        L.p[j + 1] = x;
    }
}

static bool garaInCorso(const F1Live &L)
{
    return strcmp(L.tipo, "Race") == 0 && strcmp(L.stato, "Started") == 0;
}

static void copiaValore(JsonVariant v, char *out, size_t n)
{
    if (v.isNull()) return;
    if (v.is<JsonObject>()) {
        if (v["Value"].isNull()) return;
        asciiFold(v["Value"] | "", out, n);
    } else if (v.is<const char *>()) {
        asciiFold(v.as<const char *>(), out, n);
    }
}

static void applicaSessionInfo(F1Live &L, JsonObject o)
{
    if (!o["Meeting"]["Name"].isNull()) asciiFold(o["Meeting"]["Name"] | "", L.meeting, sizeof(L.meeting));
    if (!o["Name"].isNull())            nomeSessione(o["Name"] | "", L.sessione, sizeof(L.sessione));
    if (!o["Type"].isNull())            asciiFold(o["Type"] | "", L.tipo, sizeof(L.tipo));
    const long off = parseOffset(o["GmtOffset"] | "00:00:00");
    if (!o["StartDate"].isNull()) L.inizio = parseIsoUtc(o["StartDate"] | "") - off;
    if (!o["EndDate"].isNull())   L.fine   = parseIsoUtc(o["EndDate"] | "") - off;
    if (!o["Meeting"]["Circuit"]["Key"].isNull()) {
        L.circuito = (uint16_t)(o["Meeting"]["Circuit"]["Key"] | 0);
        if (L.circuito != 0 && g_trk->chiave != L.circuito) s_trkDaCaricare = L.circuito;
    }
}

static void applicaStato(F1Live &L, JsonObject o, bool snapshot)
{
    if (o["Status"].isNull()) return;
    char nuovo[16];
    asciiFold(o["Status"] | "", nuovo, sizeof(nuovo));
    if (!snapshot && strcmp(nuovo, L.stato) != 0) {
        if (strcmp(nuovo, "Started") == 0) {
            notifySchermata(0, 0), notifica(NOT_F1, strcmp(L.tipo, "Race") == 0 ? SND_F1_PARTENZA : SND_F1_PASSAGGIO,
                     "F1  %s - %s: semaforo verde!", L.meeting, L.sessione);
        } else if (strcmp(nuovo, "Finished") == 0) {
            if (strcmp(L.tipo, "Race") == 0 && L.n > 0 && L.p[0].pos == 1) {
                notifySchermata(0, 0), notifica(NOT_F1, SND_BLOCK_FOUND, "F1  Bandiera a scacchi: vince %s (%s)",
                         L.p[0].cognome[0] ? L.p[0].cognome : L.p[0].tla, L.p[0].team);
            } else {
                notifySchermata(0, 0), notifica(NOT_F1, SND_COUNT, "F1  Fine %s", L.sessione);
            }
        } else if (strcmp(nuovo, "Aborted") == 0) {
            notifySchermata(0, 0), notifica(NOT_F1, SND_TEMP_ALARM, "F1  Sessione sospesa - BANDIERA ROSSA");
        }
    }
    snprintf(L.stato, sizeof(L.stato), "%s", nuovo);
}

static void applicaPista(F1Live &L, JsonObject o, bool snapshot)
{
    if (o["Status"].isNull()) return;
    const uint8_t nuovo = (uint8_t)atoi(o["Status"] | "1");
    if (!snapshot && nuovo != L.pista) {
        switch (nuovo) {
            case 4: notifySchermata(0, 0), notifica(NOT_F1, SND_TEMP_ALARM, "F1  SAFETY CAR in pista - giro %u", L.giro); break;
            case 5: notifySchermata(0, 0), notifica(NOT_F1, SND_TEMP_ALARM, "F1  BANDIERA ROSSA - giro %u", L.giro); break;
            case 6: notifySchermata(0, 0), notifica(NOT_F1, SND_TEMP_ALARM, "F1  VIRTUAL SAFETY CAR - giro %u", L.giro); break;
            case 1:
                if (L.pista >= 4) notifySchermata(0, 0), notifica(NOT_OK, SND_F1_PASSAGGIO, "F1  Bandiera verde, si riparte");
                break;
            default: break;
        }
    }
    L.pista = nuovo;
}

static void applicaPiloti(F1Live &L, JsonObject o)
{
    for (JsonPair kv : o) {
        if (!kv.value().is<JsonObject>()) continue;
        JsonObject d = kv.value().as<JsonObject>();
        F1LivePilota *p = pilota(L, kv.key().c_str(), true);
        if (p == nullptr) continue;
        if (!d["Tla"].isNull())      asciiFold(d["Tla"] | "", p->tla, sizeof(p->tla));
        if (!d["LastName"].isNull()) asciiFold(d["LastName"] | "", p->cognome, sizeof(p->cognome));
        if (!d["TeamName"].isNull()) asciiFold(d["TeamName"] | "", p->team, sizeof(p->team));
        if (!d["TeamColour"].isNull()) p->colore = (uint32_t)strtoul(d["TeamColour"] | "8B949E", nullptr, 16);
        if (!d["Line"].isNull() && p->pos == 0) p->pos = (uint8_t)(d["Line"] | 0);
    }
    ordinaPiloti(L);
}

static void applicaTiming(F1Live &L, JsonObject o, bool snapshot)
{
    JsonObject lines = o["Lines"];
    if (lines.isNull()) return;
    const bool gara = garaInCorso(L);

    for (JsonPair kv : lines) {
        if (!kv.value().is<JsonObject>()) continue;
        JsonObject d = kv.value().as<JsonObject>();
        F1LivePilota *p = pilota(L, kv.key().c_str(), true);
        if (p == nullptr) continue;

        int nuovaPos = -1;
        if (!d["Position"].isNull())  nuovaPos = atoi(d["Position"] | "0");
        else if (!d["Line"].isNull()) nuovaPos = d["Line"] | 0;
        if (nuovaPos > 0 && nuovaPos != p->pos) {
            if (!snapshot && gara && p->pos > 0) {
                p->delta = (int8_t)((int)p->pos - nuovaPos);
                p->deltaMs = millis() | 1;
            }
            p->pos = (uint8_t)nuovaPos;
        }

        copiaValore(d["GapToLeader"], p->gap, sizeof(p->gap));
        copiaValore(d["IntervalToPositionAhead"], p->intervallo, sizeof(p->intervallo));
        copiaValore(d["LastLapTime"], p->ultimo, sizeof(p->ultimo));
        if (!d["NumberOfLaps"].isNull())     p->giri  = (uint8_t)(d["NumberOfLaps"] | 0);
        if (!d["NumberOfPitStops"].isNull()) p->pit   = (uint8_t)(d["NumberOfPitStops"] | 0);
        if (!d["InPit"].isNull())            p->inPit = d["InPit"] | false;
        if (!d["Retired"].isNull())          p->ritirato = d["Retired"] | false;
        if (!d["Stopped"].isNull())          p->fermo = d["Stopped"] | false;

        /* giro migliore: BestLapTime (prove, gara) oppure BestLapTimes (qualifiche) */
        if (!d["BestLapTime"].isNull()) {
            char t[12] = "";
            copiaValore(d["BestLapTime"], t, sizeof(t));
            const uint32_t ms = tempoGiroMs(t);
            if (ms > 0) { snprintf(p->migliore, sizeof(p->migliore), "%s", t); p->miglioreMs = ms; }
        }
        JsonVariant blt = d["BestLapTimes"];
        if (!blt.isNull()) {
            auto prova = [&](JsonVariant v) {
                const char *s = v["Value"] | "";
                const uint32_t ms = tempoGiroMs(s);
                if (ms > 0 && (p->miglioreMs == 0 || ms < p->miglioreMs)) {
                    asciiFold(s, p->migliore, sizeof(p->migliore));
                    p->miglioreMs = ms;
                }
            };
            if (blt.is<JsonArray>())       for (JsonVariant v : blt.as<JsonArray>()) prova(v);
            else if (blt.is<JsonObject>()) for (JsonPair q : blt.as<JsonObject>()) prova(q.value());
        }
    }
    ordinaPiloti(L);

    /* cambio di leader in gara (dal secondo giro: alla partenza e' normale) */
    bool nuovoLeader = false;
    if (L.n > 0 && L.p[0].pos == 1) {
        if (!snapshot && gara && L.giro >= 2 && s_leader[0] != 0 && strcmp(s_leader, L.p[0].num) != 0) {
            notifySchermata(0, 0), notifica(NOT_F1, SND_F1_PASSAGGIO, "F1  Nuovo leader: %s %s - giro %u/%u",
                     L.p[0].tla, L.p[0].cognome, L.giro, L.giriTot);
            nuovoLeader = true;
        }
        snprintf(s_leader, sizeof(s_leader), "%s", L.p[0].num);
    }

    /* sorpasso per il 2. o 3. posto: rombo di passaggio, al massimo uno ogni
     * 30 s per non trasformare il pannello in una pista nei primi giri */
    static char     s_podio[2][4] = { "", "" };
    static uint32_t s_ultimoPodioMs = 0;
    if (L.n >= 3 && L.p[1].pos == 2 && L.p[2].pos == 3) {
        const bool cambiato = strcmp(s_podio[0], L.p[1].num) != 0 || strcmp(s_podio[1], L.p[2].num) != 0;
        if (cambiato && !snapshot && gara && !nuovoLeader && L.giro >= 2 && s_podio[0][0] != 0 &&
            (s_ultimoPodioMs == 0 || millis() - s_ultimoPodioMs >= 30000)) {
            const bool secondo = strcmp(s_podio[0], L.p[1].num) != 0;
            const F1LivePilota &chi = secondo ? L.p[1] : L.p[2];
            notifySchermata(0, 0), notifica(NOT_F1, SND_F1_PASSAGGIO, "F1  %s %s sale in P%u - giro %u/%u   (podio: %s %s %s)",
                     chi.tla, chi.cognome, secondo ? 2 : 3, L.giro, L.giriTot,
                     L.p[0].tla, L.p[1].tla, L.p[2].tla);
            s_ultimoPodioMs = millis() | 1;
        }
        snprintf(s_podio[0], sizeof(s_podio[0]), "%s", L.p[1].num);
        snprintf(s_podio[1], sizeof(s_podio[1]), "%s", L.p[2].num);
    }
}

static void applicaMessaggi(F1Live &L, JsonVariant v)
{
    JsonVariant m = v["Messages"];
    /* JsonObject si puo' riassegnare (e' un riferimento), JsonVariant no */
    JsonObject ultimo;
    if (m.is<JsonArray>()) {
        JsonArray a = m.as<JsonArray>();
        if (a.size() > 0) ultimo = a[a.size() - 1].as<JsonObject>();
    } else if (m.is<JsonObject>()) {
        for (JsonPair q : m.as<JsonObject>()) ultimo = q.value().as<JsonObject>();
    }
    if (ultimo.isNull()) return;
    char testo[80];
    asciiFold(ultimo["Message"] | "", testo, sizeof(testo));
    const int giro = ultimo["Lap"] | 0;
    if (giro > 0) snprintf(L.messaggio, sizeof(L.messaggio), "G%d  %s", giro, testo);
    else          snprintf(L.messaggio, sizeof(L.messaggio), "%s", testo);
}

/* ---- gomme (TimingAppData) ---------------------------------------------- */
static char letteraGomma(const char *c)
{
    if (c == nullptr) return '?';
    if (strcmp(c, "SOFT") == 0)         return 'S';
    if (strcmp(c, "MEDIUM") == 0)       return 'M';
    if (strcmp(c, "HARD") == 0)         return 'H';
    if (strcmp(c, "INTERMEDIATE") == 0) return 'I';
    if (strcmp(c, "WET") == 0)          return 'W';
    return '?';
}

static void applicaStint(F1LivePilota *p, int idx, JsonVariant o)
{
    if (idx < 0 || !o.is<JsonObject>()) return;
    if (p->gomma != 0 && idx < p->stint) return;          /* stint vecchio */
    if (idx > p->stint || p->gomma == 0) {
        p->stint = (uint8_t)idx;
        if (p->gomma == 0) p->gomma = '?';
    }
    if (!o["Compound"].isNull())  p->gomma = letteraGomma(o["Compound"] | "");
    if (!o["TotalLaps"].isNull()) p->giriGomma = (uint8_t)(o["TotalLaps"] | 0);
    if (!o["New"].isNull())       p->gommaNuova = strcmp(o["New"] | "", "true") == 0;
}

static void applicaGomme(F1Live &L, JsonObject o)
{
    JsonObject lines = o["Lines"];
    if (lines.isNull()) return;
    for (JsonPair kv : lines) {
        if (!kv.value().is<JsonObject>()) continue;
        F1LivePilota *p = pilota(L, kv.key().c_str(), true);
        if (p == nullptr) continue;
        JsonVariant st = kv.value()["Stints"];
        if (st.is<JsonArray>()) {
            int i = 0;
            for (JsonVariant s : st.as<JsonArray>()) applicaStint(p, i++, s);
        } else if (st.is<JsonObject>()) {
            for (JsonPair q : st.as<JsonObject>()) applicaStint(p, atoi(q.key().c_str()), q.value());
        }
    }
}

/* ---- posizioni in pista (Position.z: base64 di un JSON compresso deflate) -- */
static void aggiungiPuntoPista(int16_t x, int16_t y)
{
    Tracciato &T = *g_trk;
    if (T.completo || T.chiave == 0) return;
    if (T.n > 0) {
        const int32_t dx = x - T.x[T.n - 1], dy = y - T.y[T.n - 1];
        const int32_t d2 = dx * dx + dy * dy;
        if (d2 > 3000 * 3000) { T.n = 0; return; }           /* salto: ricomincia */
        if (d2 < 120 * 120) return;                           /* troppo vicino   */
        if (T.n > 150) {
            const int32_t ex = x - T.x[0], ey = y - T.y[0];
            if (ex * ex + ey * ey < 300 * 300) {               /* giro chiuso      */
                T.completo = true;
                T.versione++;
                s_trkDaSalvare = true;
                Serial.printf("[F1LIVE] sagoma del circuito %u completata: %u punti\n", T.chiave, T.n);
                return;
            }
        }
    }
    T.x[T.n] = x;
    T.y[T.n] = y;
    T.n++;
    T.versione++;
    if (T.n >= TRK_MAX) { T.completo = true; s_trkDaSalvare = true; }
}

static void applicaPosizioni(F1Live &L, const char *b64)
{
    if (b64 == nullptr || s_b64 == nullptr || s_infl == nullptr) return;
    size_t nb = 0;
    if (mbedtls_base64_decode(s_b64, B64_CAP, &nb, reinterpret_cast<const unsigned char *>(b64), strlen(b64)) != 0) return;
    const size_t ni = tinfl_decompress_mem_to_mem(s_infl, INFL_CAP - 1, s_b64, nb, 0);
    if (ni == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED || ni == 0) return;
    s_infl[ni] = 0;

    JsonDocument doc(PSRAM_JSON);
    if (deserializeJson(doc, s_infl, ni)) return;
    JsonArray campioni = doc["Position"];
    if (campioni.isNull() || campioni.size() == 0) return;

    /* la monoposto di riferimento per la sagoma: il leader, se e' in pista */
    if (s_rifNum[0] == 0 && L.n > 0) snprintf(s_rifNum, sizeof(s_rifNum), "%s", L.p[0].num);

    for (JsonObject c : campioni) {
        JsonObject e = c["Entries"];
        if (s_rifNum[0] != 0 && !g_trk->completo) {
            JsonObject r = e[s_rifNum];
            if (!r.isNull() && strcmp(r["Status"] | "", "OnTrack") == 0) {
                const int16_t x = (int16_t)(r["X"] | 0), y = (int16_t)(r["Y"] | 0);
                if (x != 0 || y != 0) aggiungiPuntoPista(x, y);
            }
        }
    }
    /* per lo schermo basta l'ultimo campione */
    JsonObject ultimo = campioni[campioni.size() - 1]["Entries"];
    for (JsonPair kv : ultimo) {
        F1LivePilota *p = pilota(L, kv.key().c_str(), false);
        if (p == nullptr) continue;
        JsonObject e = kv.value();
        p->x = (int16_t)(e["X"] | 0);
        p->y = (int16_t)(e["Y"] | 0);
        p->inPista = strcmp(e["Status"] | "", "OnTrack") == 0 && (p->x != 0 || p->y != 0);
    }
    L.posVersione++;
}

/* NVS: una chiave per circuito, blob = n (uint16) + x[n] + y[n] */
static void caricaTracciato(uint16_t chiave)
{
    char k[12];
    snprintf(k, sizeof(k), "c%u", chiave);
    static int16_t buf[1 + 2 * TRK_MAX];
    size_t letti = 0;
    Preferences p;
    if (p.begin("sport_trk", true)) {
        letti = p.getBytes(k, buf, sizeof(buf));
        p.end();
    }
    if (dataLock(1000)) {
        Tracciato &T = *g_trk;
        const uint32_t ver = T.versione;
        memset(&T, 0, sizeof(T));
        T.versione = ver;
        T.chiave = chiave;
        const uint16_t n = letti >= 2 ? (uint16_t)buf[0] : 0;
        if (n > 0 && n <= TRK_MAX && letti >= (size_t)(1 + 2 * n) * 2) {
            memcpy(T.x, buf + 1, n * 2);
            memcpy(T.y, buf + 1 + n, n * 2);
            T.n = n;
            T.completo = true;
        }
        T.versione++;
        s_rifNum[0] = 0;
        dataUnlock();
        Serial.printf("[F1LIVE] circuito %u: sagoma %s (%u punti)\n", chiave, T.completo ? "caricata da NVS" : "da costruire", T.n);
    }
}

static void salvaTracciato()
{
    static int16_t buf[1 + 2 * TRK_MAX];
    uint16_t chiave = 0, n = 0;
    if (!dataLock(1000)) return;
    chiave = g_trk->chiave;
    n = g_trk->n;
    buf[0] = (int16_t)n;
    memcpy(buf + 1, g_trk->x, n * 2);
    memcpy(buf + 1 + n, g_trk->y, n * 2);
    dataUnlock();
    if (chiave == 0 || n == 0) return;
    char k[12];
    snprintf(k, sizeof(k), "c%u", chiave);
    Preferences p;
    if (p.begin("sport_trk", false)) {
        p.putBytes(k, buf, (size_t)(1 + 2 * n) * 2);
        p.end();
        Serial.printf("[F1LIVE] sagoma del circuito %u salvata in NVS\n", chiave);
    }
}

static void applica(const char *topic, JsonVariant v, bool snapshot)
{
    F1Live &L = *g_f1live;
    if (strcmp(topic, "SessionInfo") == 0)          applicaSessionInfo(L, v.as<JsonObject>());
    else if (strcmp(topic, "SessionStatus") == 0)   applicaStato(L, v.as<JsonObject>(), snapshot);
    else if (strcmp(topic, "TrackStatus") == 0)     applicaPista(L, v.as<JsonObject>(), snapshot);
    else if (strcmp(topic, "DriverList") == 0)      applicaPiloti(L, v.as<JsonObject>());
    else if (strcmp(topic, "TimingData") == 0)      applicaTiming(L, v.as<JsonObject>(), snapshot);
    else if (strcmp(topic, "RaceControlMessages") == 0) applicaMessaggi(L, v);
    else if (strcmp(topic, "TimingAppData") == 0)   applicaGomme(L, v.as<JsonObject>());
    else if (strcmp(topic, "Position.z") == 0)      applicaPosizioni(L, v.as<const char *>());
    else if (strcmp(topic, "LapCount") == 0) {
        if (!v["CurrentLap"].isNull()) L.giro    = (uint8_t)(v["CurrentLap"] | 0);
        if (!v["TotalLaps"].isNull())  L.giriTot = (uint8_t)(v["TotalLaps"] | 0);
    }
}

/* Divide il messaggio WebSocket nei record SignalR (separatore 0x1E) */
static bool processa()
{
    char *p = s_msg;
    char *fine = s_msg + s_len;
    while (p < fine) {
        char *sep = static_cast<char *>(memchr(p, 0x1E, fine - p));
        const size_t len = sep ? (size_t)(sep - p) : (size_t)(fine - p);
        if (len > 2) {
            JsonDocument doc(PSRAM_JSON);
            if (!deserializeJson(doc, p, len, DeserializationOption::NestingLimit(40))) {
                const int tipo = doc["type"] | 0;
                if (tipo == 7) return false;           /* close dal server */
                if (tipo == 3 && strcmp(doc["invocationId"] | "", "0") == 0) {
                    JsonObject res = doc["result"];
                    if (!res.isNull() && dataLock(1000)) {
                        /* stato completo: si riparte da zero */
                        F1Live &L = *g_f1live;
                        const bool conn = L.connesso;
                        const uint32_t ver = L.versione;
                        memset(&L, 0, sizeof(L));
                        L.connesso = conn;
                        L.versione = ver;
                        s_leader[0] = 0;
                        /* l'ordine conta: prima la sessione, poi piloti e tempi */
                        static const char *const ORDINE[] = {
                            "SessionInfo", "SessionStatus", "LapCount", "TrackStatus",
                            "DriverList", "TimingData", "RaceControlMessages",
                            "TimingAppData", "Position.z",
                        };
                        for (const char *t : ORDINE) {
                            if (!res[t].isNull()) applica(t, res[t], true);
                        }
                        L.versione++;
                        L.aggMs = millis();
                        Serial.printf("[F1LIVE] stato: %s - %s (%s), %u piloti, giro %u/%u\n",
                                      L.meeting, L.sessione, L.stato, L.n, L.giro, L.giriTot);
                        dataUnlock();
                    }
                } else if (tipo == 1 && strcmp(doc["target"] | "", "feed") == 0) {
                    JsonArray a = doc["arguments"];
                    const char *topic = a[0] | "";
                    if (topic[0] != 0 && dataLock(1000)) {
                        applica(topic, a[1], false);
                        g_f1live->versione++;
                        g_f1live->aggMs = millis();
                        dataUnlock();
                    }
                }
            }
        }
        if (sep == nullptr) break;
        p = sep + 1;
    }
    return true;
}

/* ===========================================================================
 *  4. Quando restare collegati
 * ========================================================================= */
static bool inFinestra()
{
    const time_t ora = time(nullptr);
    bool si = false;
    if (dataLock(200)) {
        const F1Live &L = *g_f1live;
        if (strcmp(L.stato, "Started") == 0 || strcmp(L.stato, "Aborted") == 0) si = true;
        if (L.inizio > 0 && L.fine > 0 &&
            ora >= L.inizio - (time_t)F1_LIVE_MARGINE_S && ora <= L.fine + (time_t)F1_LIVE_MARGINE_S) si = true;
        for (uint8_t i = 0; i < g_f1->nSessioni && !si; ++i) {
            const F1Sessione &s = g_f1->sessioni[i];
            const time_t durata = strcmp(s.nome, "GARA") == 0 ? 3 * 3600 : 90 * 60;
            if (ora >= s.inizio - (time_t)F1_LIVE_MARGINE_S && ora <= s.inizio + durata + (time_t)F1_LIVE_MARGINE_S) si = true;
        }
        dataUnlock();
    }
    return si;
}

void f1LiveTask(void *arg)
{
    (void)arg;
    s_msg = static_cast<char *>(heap_caps_malloc(MSG_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (s_msg == nullptr) {
        Serial.println("[F1LIVE] buffer PSRAM non disponibile, live disattivato");
        vTaskDelete(nullptr);
        return;
    }
    s_b64  = static_cast<uint8_t *>(heap_caps_malloc(B64_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    s_infl = static_cast<char *>(heap_caps_malloc(INFL_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));

    uint32_t ultimoTentativo = 0;
    uint32_t attesa = 0;
    uint32_t ultimoPing = 0;
    uint32_t ultimoMsg = 0;
    uint32_t connessoDa = 0;

    /* lascia partire prima calcio e Jolpica */
    vTaskDelay(pdMS_TO_TICKS(15000));

    while (true) {
        if (!reteOk()) {
            if (s_connesso) chiudi();
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        if (!s_connesso) {
            if (ultimoTentativo == 0 || millis() - ultimoTentativo >= attesa) {
                ultimoTentativo = millis();
                if (connetti()) {
                    connessoDa = ultimoMsg = ultimoPing = millis();
                } else {
                    attesa = inFinestra() ? F1_LIVE_RETRY_MS : F1_LIVE_CONTROLLO_MS;
                    Serial.printf("[F1LIVE] connessione fallita, riprovo fra %lu s\n", (unsigned long)(attesa / 1000));
                }
            }
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        /* svuota quello che e' arrivato */
        int giri = 0;
        while (giri++ < 50) {
            const int r = leggiFrame();
            if (r == 0) break;
            if (r < 0 || !processa()) { chiudi(); break; }
            ultimoMsg = millis();
            s_len = 0;
        }
        if (!s_connesso) {
            ultimoTentativo = millis();
            attesa = inFinestra() ? 5000 : F1_LIVE_CONTROLLO_MS;
            continue;
        }

        if (millis() - ultimoPing >= 15000) {
            inviaTesto("{\"type\":6}\x1e");
            ultimoPing = millis();
        }
        static uint32_t ultimoLog = 0;
        if (millis() - ultimoLog >= 60000) {
            ultimoLog = millis();
            if (dataLock(200)) {
                const F1Live &L = *g_f1live;
                Serial.printf("[F1LIVE] %s %s: giro %u/%u, pista %u, leader %s, aggiornamenti %lu\n",
                              L.sessione, L.stato, L.giro, L.giriTot, L.pista,
                              L.n > 0 ? L.p[0].tla : "-", (unsigned long)L.versione);
                dataUnlock();
            }
        }
        if (millis() - ultimoMsg >= 60000) {
            Serial.println("[F1LIVE] nessun messaggio da 60 s, riconnetto");
            chiudi();
            ultimoTentativo = millis();
            attesa = 2000;
            continue;
        }
        /* fuori sessione: letto lo stato si chiude e si ricontrolla piu' tardi */
        if (millis() - connessoDa >= 45000 && !inFinestra()) {
            chiudi();
            ultimoTentativo = millis();
            attesa = F1_LIVE_CONTROLLO_MS;
            continue;
        }
        if (s_trkDaCaricare != 0) {
            const uint16_t k = s_trkDaCaricare;
            s_trkDaCaricare = 0;
            caricaTracciato(k);
        }
        if (s_trkDaSalvare) {
            s_trkDaSalvare = false;
            salvaTracciato();
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

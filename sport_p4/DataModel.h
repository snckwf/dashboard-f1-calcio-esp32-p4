/* ============================================================================
 *  DataModel - stato condiviso tra i task di rete e il task LVGL
 *  Tutto in RAM (le strutture grandi in PSRAM), nessuna scrittura su flash.
 *  Ogni blocco ha un contatore "versione": la UI ridisegna solo se cambia.
 * ========================================================================== */
#pragma once

#include <Arduino.h>
#include <time.h>
#include "AppConfig.h"

/* =========================== CALCIO ====================================== */
#define CALCIO_MAX_PARTITE 64

struct Partita {
    char     id[16];
    time_t   inizio;          /* UTC                                       */
    char     casa[28];
    char     ospite[28];
    int8_t   golCasa;         /* -1 = non disponibile                       */
    int8_t   golOspite;
    int8_t   rigCasa;         /* rigori, -1 se non ci sono                  */
    int8_t   rigOspite;
    char     stato;           /* 'p' da giocare, 'i' in corso, 'f' finita, 'x' rinviata */
    char     minuto[12];      /* "67'", "INT"                               */
    char     dettaglio[16];   /* "FINALE", "D.T.S.", "D.RIG."               */
    char     nota[48];
    uint32_t golMs;           /* millis() dell'ultimo cambio punteggio, 0 = mai */
    uint8_t  rossiCasa;       /* cartellini rossi (da "details" dello scoreboard) */
    uint8_t  rossiOspite;
    char     ultimoRosso[40]; /* "A. Rossi 67'"                                */
    char     marcatore[40];   /* ultimo gol: "L. Retegui 23'"                  */
};

struct Competizione {
    uint16_t n;
    Partita  p[CALCIO_MAX_PARTITE];
    uint32_t versione;
    uint32_t aggMs;           /* millis() dell'ultimo caricamento lista     */
    bool     caricata;
    bool     ripiego;         /* finestra vuota: mostrata la giornata ESPN  */
    uint8_t  live;            /* partite in corso                           */
    char     errore[40];
};

/* ---- scheda dettaglio di una partita (summary ESPN) -------------------- */
struct EventoPartita {
    char minuto[10];
    char tipo;               /* G gol, R rigore, A autogol, Y giallo, X rosso, S sostituzione */
    bool casa;
    char nome[28];
    char nome2[28];          /* sostituzione: chi esce */
};

struct Giocatore {
    char num[4];
    char nome[26];
    char ruolo[6];
    bool titolare;
    bool entrato;
    bool uscito;
};

#define DET_MAX_EVENTI 40
#define DET_MAX_STAT   12
#define DET_MAX_GIOC   26

struct DettaglioPartita {
    char     id[16];
    uint8_t  comp;
    bool     pronto;
    char     errore[32];
    uint32_t versione;
    uint32_t aggMs;
    char     casa[28];
    char     ospite[28];
    int8_t   golCasa, golOspite;
    char     stato;
    char     minuto[12];
    char     dettaglio[24];
    time_t   inizio;
    char     stadio[48];
    uint32_t spettatori;
    uint8_t  nEv;
    EventoPartita ev[DET_MAX_EVENTI];
    uint8_t  nStat;
    char     statNome[DET_MAX_STAT][22];
    char     statCasa[DET_MAX_STAT][8];
    char     statOsp[DET_MAX_STAT][8];
    char     moduloCasa[10];
    char     moduloOsp[10];
    uint8_t  nCasa, nOsp;
    Giocatore gCasa[DET_MAX_GIOC];
    Giocatore gOsp[DET_MAX_GIOC];
};

struct RigaClassifica {
    uint8_t pos;
    char    squadra[28];
    uint8_t g, v, n, p;
    int16_t dr;
    int16_t pt;
};

struct Classifica {
    uint8_t        n;
    RigaClassifica r[24];
    uint32_t       versione;
    bool           ok;
};

/* =========================== FORMULA 1 =================================== */
struct F1PilotaClass {
    uint8_t  pos;
    char     codice[4];
    char     cognome[24];
    char     team[24];
    uint32_t colore;
    float    punti;
    uint8_t  vittorie;
};

struct F1TeamClass {
    uint8_t  pos;
    char     nome[24];
    uint32_t colore;
    float    punti;
    uint8_t  vittorie;
};

struct F1Risultato {
    uint8_t  pos;
    char     codice[4];
    char     cognome[24];
    char     team[24];
    uint32_t colore;
    uint8_t  griglia;
    uint8_t  giri;
    char     tempo[20];       /* "1:38:02.339", "+0.196", "+1 Lap", "Ritirato" */
    float    punti;
    bool     giroVeloce;
};

struct F1Sessione {
    char   nome[16];
    time_t inizio;            /* UTC */
};

struct F1Dati {
    char          stagione[6];
    uint8_t       round;
    uint8_t       nPiloti;
    F1PilotaClass piloti[26];
    uint8_t       nTeam;
    F1TeamClass   team[12];

    char          garaNome[48];
    char          garaData[12];
    uint8_t       nRis;
    F1Risultato   ris[26];

    char          prossimoNome[48];
    char          prossimoLuogo[48];
    uint8_t       nSessioni;
    F1Sessione    sessioni[6];

    uint32_t      versione;
    bool          ok;
};

struct F1LivePilota {
    char     num[4];
    char     tla[4];
    char     cognome[20];
    char     team[24];
    uint32_t colore;
    uint8_t  pos;
    char     gap[14];
    char     intervallo[14];
    char     ultimo[12];
    char     migliore[12];
    uint32_t miglioreMs;      /* ms del giro migliore, 0 = nessuno */
    uint8_t  giri;
    uint8_t  pit;
    bool     inPit;
    bool     ritirato;
    bool     fermo;
    int8_t   delta;           /* posizioni guadagnate (+) o perse (-) */
    uint32_t deltaMs;
    /* gomme (TimingAppData) */
    uint8_t  stint;           /* indice dello stint corrente               */
    char     gomma;           /* S M H I W, '?' sconosciuta, 0 = nessun dato */
    uint8_t  giriGomma;
    bool     gommaNuova;
    /* posizione in pista (Position.z), coordinate F1 in decimi di metro */
    int16_t  x, y;
    bool     inPista;
};

/* sagoma del circuito, costruita dal percorso di una monoposto e salvata in NVS */
#define TRK_MAX 800
struct Tracciato {
    uint16_t chiave;          /* Meeting.Circuit.Key                        */
    uint16_t n;
    bool     completo;
    int16_t  x[TRK_MAX];
    int16_t  y[TRK_MAX];
    uint32_t versione;
};

struct F1Live {
    bool         connesso;
    char         meeting[48];
    char         sessione[24];
    char         tipo[16];      /* "Race", "Qualifying", "Practice" */
    char         stato[16];     /* "Started", "Finished", "Inactive", "Aborted" */
    uint8_t      giro;
    uint8_t      giriTot;
    uint8_t      pista;         /* 1 verde 2 gialla 4 SC 5 rossa 6 VSC 7 fine VSC */
    char         messaggio[96]; /* ultimo messaggio della direzione gara */
    time_t       inizio;        /* UTC */
    time_t       fine;
    uint8_t      n;
    F1LivePilota p[24];
    uint16_t     circuito;      /* Meeting.Circuit.Key */
    uint32_t     posVersione;   /* cambia a ogni aggiornamento delle posizioni */
    uint32_t     versione;
    uint32_t     aggMs;
};

/* =========================== ISTANZE ===================================== */
extern Competizione *g_comp;          /* [CALCIO_COMP_COUNT] in PSRAM */
extern Classifica    g_classSerieA;
extern F1Dati       *g_f1;
extern F1Live       *g_f1live;
extern Tracciato    *g_trk;
extern DettaglioPartita *g_det;

/* dettaglio richiesto dalla UI (id evento ESPN + competizione) */
extern volatile bool    g_detRichiesta;
extern volatile bool    g_detAperto;
extern char             g_detId[16];
extern volatile uint8_t g_detComp;

/* Competizione mostrata a schermo: il task calcio le da' la precedenza */
extern volatile uint8_t g_compVisibile;
extern volatile bool    g_richiestaLista;

void initDataModel();
bool dataLock(int timeoutMs);
void dataUnlock();

void        setWifiStatusText(const char *text);
const char *getWifiStatusText();

/* =========================== HELPER ====================================== */
/* UTF-8 -> ASCII: i font Montserrat di LVGL hanno solo i caratteri ASCII */
void   asciiFold(const char *in, char *out, size_t n);
/* "2026-10-10T13:00Z", "2026-10-10T13:00:00Z", "2026-10-04T15:00:00" -> epoch UTC */
time_t parseIsoUtc(const char *iso);
/* "08:00:00" o "-04:00:00" -> secondi */
long   parseOffset(const char *s);
/* Nome italiano di nazionali e squadre (in ingresso il nome ESPN gia' in ASCII) */
const char *nomeItaliano(const char *eng);
/* "Sab 10/10" in ora italiana */
void   fmtGiorno(time_t t, char *out, size_t n);
/* "20:45" in ora italiana */
void   fmtOra(time_t t, char *out, size_t n);
/* chiave del giorno locale AAAAMMGG */
uint32_t chiaveGiorno(time_t t);
/* "1:35.631" -> 95631 ms, 0 se non valido */
uint32_t tempoGiroMs(const char *s);
/* colore del team da id/nome (Jolpica o F1) */
uint32_t coloreTeam(const char *idONome);
/* true se l'orologio di sistema e' gia' sincronizzato via NTP */
bool   orologioValido();

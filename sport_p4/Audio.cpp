/* ============================================================================
 *  Audio - implementazione
 *
 *  Catena: ESP32-P4  --I2S-->  ES8311  -->  amplificatore  -->  speaker
 *  Il codec sta sullo stesso bus I2C del touch (GPIO 7/8, porta 1) a 0x18.
 *
 *  La sequenza di registri viene dal driver ufficiale Espressif esp_codec_dev
 *  (device/es8311/es8311.c), non da ricostruzioni: reset, clock manager,
 *  formato I2S 16 bit, accensione DAC, volume.
 * ========================================================================== */
#include "Audio.h"
#include "AppConfig.h"

#include <math.h>
#include "driver/i2s_std.h"
#include "driver/i2c.h"      /* ESP32_Display_Panel apre il bus col driver legacy */
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

/* ---- parametri audio ------------------------------------------------------ */
static constexpr int      AUDIO_RATE     = 32000;
static constexpr int      AUDIO_MCLK_DIV = 256;          /* MCLK = 8.192 MHz  */
static constexpr uint8_t  ES8311_ADDR    = 0x18;
static constexpr int      I2S_MCLK_IO    = 13;
static constexpr int      I2S_BCLK_IO    = 12;
static constexpr int      I2S_LRCK_IO    = 10;
static constexpr int      I2S_DOUT_IO    = 9;
static constexpr int      PA_ENABLE_IO   = 53;
static constexpr size_t   CHUNK_FRAMES   = 256;

/* Ampiezza del segnale sui 16 bit disponibili (fondo scala 32767).
 * A 22000, con il volume del DAC in guadagno positivo, lo stadio di uscita
 * saturava. A 20000 restano circa 4 dB di margine e, tenendo il DAC a 0 dB,
 * la catena resta pulita. Oltre 22000 si torna a distorcere. */
static constexpr int16_t  WAVE_AMPLITUDE = 20000;

/* Filtro passa-basso a un polo sull'uscita. Oltre a togliere le ultime asprezze,
 * imita il comportamento degli altoparlantini delle TV su cui giravano davvero
 * le console 8 bit: il quadrato "tondo" che ricordiamo non era mai a spigolo
 * vivo. Alzando la frequenza il suono torna piu' tagliente. */
static constexpr float LOWPASS_HZ = 3200.0f;

/* ---- registri ES8311 (nomi dal datasheet) --------------------------------- */
#define R_RESET   0x00
#define R_CLK1    0x01
#define R_CLK2    0x02
#define R_CLK3    0x03
#define R_CLK4    0x04
#define R_CLK5    0x05
#define R_CLK6    0x06
#define R_CLK7    0x07
#define R_CLK8    0x08
#define R_SDPIN   0x09
#define R_SDPOUT  0x0A
#define R_SYS0D   0x0D
#define R_SYS0E   0x0E
#define R_SYS12   0x12
#define R_SYS14   0x14
#define R_ADC15   0x15
#define R_ADC17   0x17
#define R_DACMUTE 0x31
#define R_DACVOL  0x32
#define R_DAC37   0x37
#define R_GP45    0x45
#define R_CHIPID1 0xFD
#define R_CHIPID2 0xFE

/* ---- stato ---------------------------------------------------------------- */
static i2s_chan_handle_t s_tx = nullptr;
static bool      s_ready = false;
static uint8_t   s_volume = 75;
static QueueHandle_t s_queue = nullptr;
static char      s_status[80] = "audio non inizializzato";

/* ============================================================ accesso I2C ===
 * Il bus e' gia' aperto da ESP32_Display_Panel per il touch GT911: la libreria
 * usa il driver legacy (i2c_param_config + i2c_driver_install), quindi noi ci
 * limitiamo a fare transazioni sulla stessa porta, senza reinizializzare nulla. */
static bool i2cWrite(uint8_t reg, uint8_t value)
{
    const uint8_t buf[2] = { reg, value };
    return i2c_master_write_to_device((i2c_port_t)TOUCH_I2C_NUM, ES8311_ADDR,
                                      buf, 2, pdMS_TO_TICKS(200)) == ESP_OK;
}

static bool i2cRead(uint8_t reg, uint8_t *value)
{
    return i2c_master_write_read_device((i2c_port_t)TOUCH_I2C_NUM, ES8311_ADDR,
                                        &reg, 1, value, 1, pdMS_TO_TICKS(200)) == ESP_OK;
}

/* Legge l'ID del chip: 0x83 / 0x11. E' anche il test che dice se il bus va. */
static bool codecProbe()
{
    uint8_t id1 = 0, id2 = 0;
    if (!i2cRead(R_CHIPID1, &id1)) return false;
    if (!i2cRead(R_CHIPID2, &id2)) return false;
    return (id1 == 0x83 && id2 == 0x11);
}

static bool openCodecBus()
{
    if (codecProbe()) {
        snprintf(s_status, sizeof(s_status), "ES8311 trovato a 0x%02X su I2C%d",
                 ES8311_ADDR, TOUCH_I2C_NUM);
        return true;
    }
    snprintf(s_status, sizeof(s_status), "ES8311 non risponde a 0x%02X su I2C%d",
             ES8311_ADDR, TOUCH_I2C_NUM);
    return false;
}

/* ============================================================ init codec ==== */
static void codecSetVolumeReg(uint8_t percent)
{
    /* REG32 e' una scala logaritmica: 0x00 = muto, ogni passo vale 0,5 dB,
     * 0xBF (191) = 0 dB, 0xFF (255) = circa +32 dB.
     *
     * La versione precedente mappava 0..100% su 0..0xBF: al 70% finiva a 133,
     * cioe' 58 passi sotto lo zero = -29 dB. Sommati ai -11 dB dell'ampiezza
     * del campione facevano -40 dB, da cui il suono appena percettibile.
     *
     * Ora la scala copre tutto il registro: 75% = 0 dB, 100% = massimo. */
    if (percent > 100) percent = 100;
    const uint8_t reg = (uint8_t)(((uint16_t)percent * 0xFF) / 100);
    i2cWrite(R_DACVOL, reg);
}

static bool codecInit()
{
    /* reset */
    if (!i2cWrite(R_RESET, 0x1F)) return false;
    delay(20);
    i2cWrite(R_RESET, 0x00);
    delay(10);

    /* slave mode, MCLK dall'ESP32, non invertito */
    i2cWrite(R_RESET, 0x80);
    i2cWrite(R_CLK1,  0x3F);

    /* --- divisori di clock per MCLK 8.192 MHz e 32 kHz ---------------------
     * Riga della tabella ufficiale: {8192000, 32000, pre_div 1, pre_multi 1,
     * adc_div 1, dac_div 1, fs_mode 0, lrck_h 0x00, lrck_l 0xFF, bclk_div 4,
     * adc_osr 0x10, dac_osr 0x10}. Applico le stesse formule del driver. */
    uint8_t r;
    if (i2cRead(R_CLK2, &r)) i2cWrite(R_CLK2, (uint8_t)((r & 0x07) | (0 << 5) | (0 << 3)));
    i2cWrite(R_CLK5, 0x00);                                   /* (adc_div-1)<<4 | (dac_div-1) */
    if (i2cRead(R_CLK3, &r)) i2cWrite(R_CLK3, (uint8_t)((r & 0x80) | (0 << 6) | 0x10));
    if (i2cRead(R_CLK4, &r)) i2cWrite(R_CLK4, (uint8_t)((r & 0x80) | 0x10));
    if (i2cRead(R_CLK7, &r)) i2cWrite(R_CLK7, (uint8_t)((r & 0xC0) | 0x00));
    i2cWrite(R_CLK8, 0xFF);
    if (i2cRead(R_CLK6, &r)) i2cWrite(R_CLK6, (uint8_t)((r & 0xE0) | (4 - 1)));

    /* formato I2S standard, 16 bit, uscita non mutata */
    i2cWrite(R_SDPIN,  0x0C);
    i2cWrite(R_SDPOUT, 0x0C);

    /* accensione */
    i2cWrite(R_ADC17,   0xBF);
    i2cWrite(R_SYS0E,   0x02);
    i2cWrite(R_SYS12,   0x00);   /* abilita il DAC */
    i2cWrite(R_SYS14,   0x1A);
    i2cWrite(R_SYS0D,   0x01);
    i2cWrite(R_ADC15,   0x40);
    i2cWrite(R_DAC37,   0x08);
    i2cWrite(R_GP45,    0x00);
    i2cWrite(R_DACMUTE, 0x00);   /* togli il mute del DAC */
    codecSetVolumeReg(s_volume);
    return true;
}

static bool i2sInit()
{
    i2s_chan_config_t chanCfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chanCfg.dma_desc_num  = 4;
    chanCfg.dma_frame_num = CHUNK_FRAMES;
    chanCfg.auto_clear    = true;
    if (i2s_new_channel(&chanCfg, &s_tx, nullptr) != ESP_OK) return false;

    i2s_std_config_t std = {};
    std.clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_RATE);
    std.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    std.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    std.gpio_cfg.mclk = (gpio_num_t)I2S_MCLK_IO;
    std.gpio_cfg.bclk = (gpio_num_t)I2S_BCLK_IO;
    std.gpio_cfg.ws   = (gpio_num_t)I2S_LRCK_IO;
    std.gpio_cfg.dout = (gpio_num_t)I2S_DOUT_IO;
    std.gpio_cfg.din  = I2S_GPIO_UNUSED;
    std.gpio_cfg.invert_flags.mclk_inv = false;
    std.gpio_cfg.invert_flags.bclk_inv = false;
    std.gpio_cfg.invert_flags.ws_inv   = false;

    if (i2s_channel_init_std_mode(s_tx, &std) != ESP_OK) return false;
    return i2s_channel_enable(s_tx) == ESP_OK;
}

/* ============================================================ sintesi ======= */
/* note in Hz, quarta e quinta ottava: la scala che usavano i giochi 8 bit */
#define N_REST 0
#define N_C3   131
#define N_E3   165
#define N_G3   196
#define N_A3   220
#define N_C4   262
#define N_D4   294
#define N_E4   330
#define N_F4   349
#define N_G4   392
#define N_A4   440
#define N_B4   494
#define N_C5   523
#define N_D5   587
#define N_E5   659
#define N_F5   698
#define N_G5   784
#define N_A5   880
#define N_B5   988
#define N_C6  1047

enum NoteFx : uint8_t { FX_NONE = 0, FX_SLIDE_DOWN, FX_SLIDE_UP, FX_VIBRATO };

struct Note {
    uint16_t freq;    /* Hz, 0 = pausa                                       */
    uint16_t ms;      /* durata                                              */
    uint8_t  duty;    /* brillantezza: 12 = sinusoide pura (la piu' dolce),
                       * 25 = con seconda armonica, 50 = anche terza         */
    uint8_t  fx;
};

/* --- tocco: un "ping" morbido, sinusoide pura --- */
static const Note J_TOUCH[] = {
    { N_A4, 55, 12, FX_NONE },
};

/* --- miner caduto: discesa calda che si spegne --- */
static const Note J_MINER_DOWN[] = {
    { N_G4, 110, 25, FX_NONE },
    { N_E4, 110, 25, FX_NONE },
    { N_C4, 130, 25, FX_NONE },
    { N_G3, 190, 12, FX_NONE },
    { N_E3, 420, 12, FX_SLIDE_DOWN },
};

/* --- temperatura critica: due toni alternati, insistenti ma non striduli --- */
static const Note J_TEMP[] = {
    { N_A4, 130, 25, FX_NONE }, { N_E4, 130, 25, FX_NONE }, { N_REST, 60, 12, FX_NONE },
    { N_A4, 130, 25, FX_NONE }, { N_E4, 130, 25, FX_NONE }, { N_REST, 60, 12, FX_NONE },
    { N_A4, 150, 25, FX_NONE }, { N_E4, 260, 25, FX_NONE },
};

/* --- nuova best share: arpeggio ascendente, delicato --- */
static const Note J_BEST[] = {
    { N_C4,  70, 12, FX_NONE },
    { N_E4,  70, 12, FX_NONE },
    { N_G4,  70, 12, FX_NONE },
    { N_C5, 300, 25, FX_VIBRATO },
};

/* --- blocco trovato: rincorsa e fanfara, il momento che aspetti da sempre --- */
static const Note J_BLOCK[] = {
    { N_C4,  90, 12, FX_NONE }, { N_E4,  90, 12, FX_NONE },
    { N_G4,  90, 12, FX_NONE }, { N_C5,  90, 12, FX_NONE },
    { N_E5,  90, 25, FX_NONE }, { N_G5,  90, 25, FX_NONE },
    { N_C6, 260, 25, FX_NONE },
    { N_REST, 90, 12, FX_NONE },
    { N_G5, 170, 25, FX_NONE },
    { N_E5, 170, 25, FX_NONE },
    { N_C5, 170, 25, FX_NONE },
    { N_G4, 150, 25, FX_NONE },
    { N_C5, 900, 25, FX_VIBRATO },
};

/* --- avvio: arpeggio breve --- */
static const Note J_STARTUP[] = {
    { N_C4, 120, 12, FX_NONE },
    { N_E4, 120, 12, FX_NONE },
    { N_G4, 120, 12, FX_NONE },
    { N_C5, 380, 25, FX_VIBRATO },
};

struct Jingle { const Note *notes; uint8_t count; };
static const Jingle JINGLES[SND_PRIMO_SINTETIZZATO] = {
    { J_TOUCH,      sizeof(J_TOUCH)      / sizeof(Note) },
    { J_MINER_DOWN, sizeof(J_MINER_DOWN) / sizeof(Note) },
    { J_TEMP,       sizeof(J_TEMP)       / sizeof(Note) },
    { J_BEST,       sizeof(J_BEST)       / sizeof(Note) },
    { J_BLOCK,      sizeof(J_BLOCK)      / sizeof(Note) },
    { J_STARTUP,    sizeof(J_STARTUP)    / sizeof(Note) },
};

/* ---------------------------------------------------------------- PolyBLEP --
 * Un'onda quadra costruita a forza bruta ("sopra la soglia = +1, sotto = -1")
 * ha due discontinuita' per periodo. Una discontinuita' contiene armoniche fino
 * a frequenza infinita: tutto cio' che supera la meta' della frequenza di
 * campionamento si ripiega su righe NON armoniche, ed e' quello che l'orecchio
 * sente come "metallico".
 *
 * PolyBLEP corregge il problema alla radice: sostituisce ogni salto netto con
 * un raccordo polinomiale lungo un campione, che equivale a togliere le
 * armoniche oltre Nyquist. Costa due moltiplicazioni per campione.            */
static inline float polyBlep(float t, float dt)
{
    if (t < dt) {
        t /= dt;
        return t + t - t * t - 1.0f;
    }
    if (t > 1.0f - dt) {
        t = (t - 1.0f) / dt;
        return t * t + t + t + 1.0f;
    }
    return 0.0f;
}

/* stato del passa-basso, azzerato all'inizio di ogni jingle */
static float s_lpState = 0.0f;
static float s_lpAlpha = 0.0f;

/* Genera una nota e la manda all'I2S.
 * L'inviluppo (attacco 2 ms, rilascio 8 ms) serve a evitare il "tac" alle
 * estremita': un'onda quadra troncata di netto fa click sull'altoparlante. */
static void renderNote(const Note &n)
{
    const uint32_t totalFrames = (uint32_t)AUDIO_RATE * n.ms / 1000;
    if (totalFrames == 0) return;

    uint32_t attack  = (uint32_t)AUDIO_RATE * 12 / 1000;
    uint32_t release = (uint32_t)AUDIO_RATE * 15 / 1000;
    /* su note cortissime attacco e rilascio non possono superare la nota */
    if (attack + release > totalFrames) {
        attack  = totalFrames / 3;
        release = totalFrames / 3;
    }
    static int16_t buf[CHUNK_FRAMES * 2];

    float phase = 0.0f;
    uint32_t done = 0;

    while (done < totalFrames) {
        const uint32_t frames = min<uint32_t>(CHUNK_FRAMES, totalFrames - done);

        for (uint32_t i = 0; i < frames; ++i) {
            const uint32_t pos = done + i;
            int16_t sample = 0;

            if (n.freq > 0) {
                float f = (float)n.freq;
                const float t = (float)pos / (float)AUDIO_RATE;
                if (n.fx == FX_SLIDE_DOWN) f *= (1.0f - 0.35f * (float)pos / (float)totalFrames);
                else if (n.fx == FX_SLIDE_UP) f *= (1.0f + 0.25f * (float)pos / (float)totalFrames);
                else if (n.fx == FX_VIBRATO) f *= (1.0f + 0.02f * sinf(2.0f * (float)M_PI * 6.0f * t));

                const float dt = f / (float)AUDIO_RATE;
                phase += dt;
                if (phase >= 1.0f) phase -= 1.0f;

                /* Sintesi additiva: fondamentale piu' poche armoniche deboli.
                 * L'onda quadra ha TUTTE le armoniche dispari a piena ampiezza
                 * (1/n), ed e' questo a renderla aspra. Qui la fondamentale
                 * domina e le armoniche servono solo a dare corpo, non morso.
                 * Il campo "duty" ora regola quanto e' brillante il timbro. */
                const float w = 2.0f * (float)M_PI * phase;
                float v = sinf(w);
                float norm = 1.0f;
                if (n.duty >= 25) { v += 0.20f * sinf(2.0f * w); norm += 0.20f; }
                if (n.duty >= 50) { v += 0.10f * sinf(3.0f * w); norm += 0.10f; }
                v /= norm;

                /* Inviluppo morbido: attacco lento (12 ms invece di 2) perche'
                 * un attacco secco viene percepito come "tac" metallico, e
                 * decadimento esponenziale che ammorbidisce la coda della nota. */
                float env = expf(-1.4f * (float)pos / (float)totalFrames);
                if (pos < attack) env *= (float)pos / (float)attack;
                else if (pos > totalFrames - release && totalFrames > release) {
                    env *= (float)(totalFrames - pos) / (float)release;
                }

                /* passa-basso a un polo: arrotonda gli spigoli residui */
                const float target = v * env * (float)WAVE_AMPLITUDE;
                s_lpState += s_lpAlpha * (target - s_lpState);

                float out = s_lpState;
                if (out >  32000.0f) out =  32000.0f;
                if (out < -32000.0f) out = -32000.0f;
                sample = (int16_t)out;
            } else {
                /* pausa: lasciamo decadere il filtro invece di azzerare di
                 * colpo, altrimenti il salto a zero e' esso stesso un click */
                s_lpState += s_lpAlpha * (0.0f - s_lpState);
                sample = (int16_t)s_lpState;
            }

            buf[i * 2 + 0] = sample;
            buf[i * 2 + 1] = sample;
        }

        size_t written = 0;
        i2s_channel_write(s_tx, buf, frames * 2 * sizeof(int16_t), &written, portMAX_DELAY);
        done += frames;
    }
}

/* ============================================== suoni procedurali ==========
 *  Non sono campioni registrati: ogni suono e' calcolato campione per campione
 *  (sintesi additiva + rumore filtrato), quindi non occupano flash.
 *  Tutti passano dallo stesso passa-basso a 3200 Hz dei jingle.
 * ========================================================================= */
static constexpr float DUE_PI = 2.0f * (float)M_PI;

/* rumore bianco veloce (xorshift32), uscita -1..+1 */
static uint32_t s_rng = 0x9E3779B9u;
static inline float rumore()
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return (float)(int32_t)s_rng * (1.0f / 2147483648.0f);
}

/* filtro passa-banda a variabili di stato (Chamberlin) */
struct PassaBanda {
    float lp = 0.0f, bp = 0.0f, F = 0.1f, q = 1.0f;
    void imposta(float hz, float smorzamento)
    {
        F = 2.0f * sinf((float)M_PI * hz / (float)AUDIO_RATE);
        q = smorzamento;
    }
    float passo(float x)
    {
        const float hp = x - lp - q * bp;
        bp += F * hp;
        lp += F * bp;
        return bp;
    }
};

/* Voce di motore: fondamentale + armoniche con peso 1/k (dente di sega
 * addolcito) e una sottoarmonica a meta' frequenza, che da' il "borbottio"
 * del ciclo motore. "brillantezza" 0..1 pesa le armoniche alte. */
struct Motore {
    float fase = 0.0f;
    float faseSub = 0.0f;
    float campione(float f, float brillantezza)
    {
        fase += f / (float)AUDIO_RATE;
        if (fase >= 1.0f) fase -= 1.0f;
        faseSub += 0.5f * f / (float)AUDIO_RATE;
        if (faseSub >= 1.0f) faseSub -= 1.0f;
        const float w = DUE_PI * fase;
        float v = sinf(w);
        float norm = 1.0f;
        for (int k = 2; k <= 6; ++k) {
            const float a = brillantezza / (float)k;
            v += a * sinf((float)k * w);
            norm += a;
        }
        v /= norm;
        return 0.8f * v + 0.2f * sinf(DUE_PI * faseSub);
    }
};

/* Genera "secondi" di audio chiamando gen(t) per ogni campione (valore -1..1).
 * Rampa di 8 ms all'inizio e alla fine contro i click. */
template <typename F>
static void renderGen(float secondi, F gen)
{
    const uint32_t totale = (uint32_t)(secondi * (float)AUDIO_RATE);
    static int16_t buf[CHUNK_FRAMES * 2];
    const float rampa = 0.008f;
    uint32_t fatto = 0;
    while (fatto < totale) {
        const uint32_t frames = min<uint32_t>(CHUNK_FRAMES, totale - fatto);
        for (uint32_t i = 0; i < frames; ++i) {
            const uint32_t pos = fatto + i;
            const float t = (float)pos / (float)AUDIO_RATE;
            const float resto = (float)(totale - pos) / (float)AUDIO_RATE;
            float v = gen(t);
            if (t < rampa)     v *= t / rampa;
            if (resto < rampa) v *= resto / rampa;
            s_lpState += s_lpAlpha * (v * (float)WAVE_AMPLITUDE - s_lpState);
            float out = s_lpState;
            if (out >  32000.0f) out =  32000.0f;
            if (out < -32000.0f) out = -32000.0f;
            buf[i * 2 + 0] = (int16_t)out;
            buf[i * 2 + 1] = (int16_t)out;
        }
        size_t scritti = 0;
        i2s_channel_write(s_tx, buf, frames * 2 * sizeof(int16_t), &scritti, portMAX_DELAY);
        fatto += frames;
    }
}

static void pausa(uint16_t ms)
{
    const Note n = { N_REST, ms, 12, FX_NONE };
    renderNote(n);
}

/* ---- F1: monoposto che passa -------------------------------------------
 * Arriva da lontano (volume basso, tono alto per effetto Doppler), passa
 * davanti (volume massimo, timbro piu' brillante) e si allontana (tono che
 * cala di circa una terza, volume che si spegne).                          */
static void suonaPassaggio()
{
    Motore m;
    const float durata = 2.4f;
    const float tc = 1.05f;           /* istante del passaggio */
    renderGen(durata, [&](float t) {
        const float x = t - tc;
        const float f = 365.0f * (1.0f + 0.16f * tanhf(-x / 0.10f));   /* ~423 -> ~307 Hz */
        const float vicino = 1.0f / (1.0f + (x / 0.20f) * (x / 0.20f));
        const float amp = 0.06f + 0.94f * vicino;
        float v = m.campione(f, 0.35f + 0.65f * vicino);
        v += 0.22f * vicino * rumore();                       /* aria e gomme */
        v *= 1.0f + 0.10f * sinf(DUE_PI * 31.0f * t);         /* vibrazione   */
        float fade = 1.0f;
        if (t < 0.5f) fade *= t / 0.5f;
        if (t > durata - 0.7f) fade *= (durata - t) / 0.7f;
        return 0.85f * amp * v * fade;
    });
}

/* ---- F1: partenza --------------------------------------------------------
 * Cinque luci che si accendono (bip gravi), spegnimento, poi il motore che
 * sale di giri con due cambi marcia.                                        */
static void suonaPartenza()
{
    const Note luce = { N_C4, 90, 12, FX_NONE };
    for (int i = 0; i < 5; ++i) {
        renderNote(luce);
        pausa(230);
    }
    pausa(450);
    Motore m;
    const float durata = 2.3f;
    renderGen(durata, [&](float t) {
        float f;
        if (t < 0.8f)      f = 190.0f + 330.0f * (t / 0.8f);            /* prima   */
        else if (t < 1.5f) f = 330.0f + 220.0f * ((t - 0.8f) / 0.7f);   /* seconda */
        else               f = 390.0f + 160.0f * ((t - 1.5f) / 0.8f);   /* terza   */
        float amp = t < 0.12f ? t / 0.12f : 1.0f;
        if (fabsf(t - 0.8f) < 0.045f || fabsf(t - 1.5f) < 0.045f) amp *= 0.35f;   /* cambio */
        if (t > durata - 0.6f) amp *= (durata - t) / 0.6f;
        const float v = m.campione(f, 0.75f) + 0.15f * rumore();
        return 0.8f * amp * v;
    });
}

/* ---- calcio: fischio dell'arbitro ----------------------------------------
 * Fischietto a pallina: tono intorno ai 2,1 kHz modulato a ~28 Hz dalla
 * pallina che gira nella camera, piu' un filo di soffio.                    */
static void fischio(float durata)
{
    float fase = 0.0f;
    renderGen(durata, [&](float t) {
        const float mod = sinf(DUE_PI * 28.0f * t);
        const float f = 2150.0f * (1.0f + 0.012f * mod);
        fase += f / (float)AUDIO_RATE;
        if (fase >= 1.0f) fase -= 1.0f;
        const float trillo = 0.55f + 0.45f * mod;
        float v = (sinf(DUE_PI * fase) + 0.15f * sinf(2.0f * DUE_PI * fase)) / 1.15f;
        v = v * trillo + 0.06f * rumore();
        float env = t < 0.025f ? t / 0.025f : 1.0f;
        if (t > durata - 0.05f) env *= (durata - t) / 0.05f;
        return 0.8f * v * env;
    });
}

/* ---- calcio: gol ---------------------------------------------------------
 * Sirena da stadio (accordo maggiore di tre toni ricchi) e boato della folla
 * (rumore filtrato attorno ai 900 Hz che esplode e poi si spegne a ondate). */
static void suonaGol()
{
    float f1 = 0.0f, f2 = 0.0f, f3 = 0.0f;
    PassaBanda folla;
    folla.imposta(900.0f, 0.8f);
    auto voce = [](float &fase, float hz) {
        fase += hz / (float)AUDIO_RATE;
        if (fase >= 1.0f) fase -= 1.0f;
        const float w = DUE_PI * fase;
        return (sinf(w) + 0.5f * sinf(2.0f * w) + 0.33f * sinf(3.0f * w) + 0.25f * sinf(4.0f * w)) / 2.08f;
    };
    const float durata = 3.6f;
    renderGen(durata, [&](float t) {
        float sirena = 0.0f;
        if (t < 1.6f) {
            float e = t < 0.04f ? t / 0.04f : 1.0f;
            if (t > 1.3f) e *= (1.6f - t) / 0.3f;
            sirena = e * (voce(f1, 277.0f) + voce(f2, 349.0f) + voce(f3, 415.0f)) / 3.0f;
        }
        const float n = folla.passo(rumore());
        float ef = t < 0.5f ? t / 0.5f : expf(-(t - 0.5f) / 1.4f);
        ef *= 1.0f + 0.25f * sinf(DUE_PI * 2.3f * t);
        return 0.6f * sirena + 1.5f * ef * n;
    });
}

static void suonaProcedurale(SoundEvent e)
{
    switch (e) {
        case SND_F1_PASSAGGIO: suonaPassaggio(); break;
        case SND_F1_PARTENZA:  suonaPartenza();  break;
        case SND_FISCHIO_INIZIO:
            fischio(1.1f);
            break;
        case SND_FISCHIO_INTERVALLO:
            fischio(0.35f); pausa(180); fischio(0.9f);
            break;
        case SND_FISCHIO_FINE:
            fischio(0.35f); pausa(180); fischio(0.35f); pausa(180); fischio(1.3f);
            break;
        case SND_GOL: suonaGol(); break;
        case SND_ESPULSIONE: {
            fischio(0.6f);
            pausa(120);
            const Note giu[] = { { N_E4, 160, 25, FX_NONE }, { N_C4, 160, 25, FX_NONE }, { N_G3, 420, 12, FX_SLIDE_DOWN } };
            for (const Note &n : giu) renderNote(n);
            break;
        }
        default: break;
    }
}

static void audioTask(void *arg)
{
    (void)arg;
    SoundEvent e;
    while (true) {
        if (xQueueReceive(s_queue, &e, portMAX_DELAY) == pdTRUE) {
            if (e >= SND_COUNT) continue;
            s_lpState = 0.0f;               /* filtro pulito a ogni jingle */
            if (e >= SND_PRIMO_SINTETIZZATO) {
                suonaProcedurale(e);
            } else {
                const Jingle &j = JINGLES[e];
                for (uint8_t i = 0; i < j.count; ++i) {
                    renderNote(j.notes[i]);
                }
            }
            /* mezzo frame di silenzio: chiude la coda DMA senza ronzio */
            static int16_t silence[64] = { 0 };
            size_t w = 0;
            i2s_channel_write(s_tx, silence, sizeof(silence), &w, pdMS_TO_TICKS(50));
        }
    }
}

/* ============================================================ API =========== */
bool audioBegin()
{
    if (!openCodecBus()) {
        Serial.printf("Audio: %s\n", s_status);
        return false;
    }

    /* amplificatore */
    gpio_config_t pa = {};
    pa.pin_bit_mask = 1ULL << PA_ENABLE_IO;
    pa.mode = GPIO_MODE_OUTPUT;
    gpio_config(&pa);
    gpio_set_level((gpio_num_t)PA_ENABLE_IO, 1);

    if (!i2sInit()) {
        snprintf(s_status, sizeof(s_status), "I2S non inizializzato");
        Serial.printf("Audio: %s\n", s_status);
        return false;
    }
    if (!codecInit()) {
        snprintf(s_status, sizeof(s_status), "init ES8311 fallita");
        Serial.printf("Audio: %s\n", s_status);
        return false;
    }

    /* coefficiente del passa-basso a un polo: alpha = 1 - e^(-2*pi*fc/fs) */
    s_lpAlpha = 1.0f - expf(-2.0f * (float)M_PI * LOWPASS_HZ / (float)AUDIO_RATE);
    s_lpState = 0.0f;

    s_queue = xQueueCreate(10, sizeof(SoundEvent));
    if (s_queue == nullptr) return false;
    /* priorita' 4, sopra LVGL (3): i suoni sintetizzati si calcolano in tempo
     * reale e un ridisegno a schermo intero non deve lasciare buchi nell'audio */
    if (xTaskCreatePinnedToCore(audioTask, "audio", 6144, nullptr, 4, nullptr, 1) != pdPASS) {
        return false;
    }

    s_ready = true;
    Serial.printf("Audio: %s\n", s_status);
    return true;
}

void audioPlay(SoundEvent event)
{
    if (!s_ready || s_queue == nullptr) return;
    /* se la coda e' piena lasciamo perdere: meglio saltare un suono che
     * bloccare il task che lo ha richiesto (spesso e' quello di LVGL) */
    xQueueSend(s_queue, &event, 0);
}

bool audioReady() { return s_ready; }

void audioSetVolume(uint8_t percent)
{
    s_volume = (percent > 100) ? 100 : percent;
    if (s_ready) codecSetVolumeReg(s_volume);
}

uint8_t audioGetVolume() { return s_volume; }

const char *audioStatus() { return s_status; }

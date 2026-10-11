/* ============================================================================
 *  WebConfig - pagina di configurazione su http://<ip>/ e http://sport-p4.local
 *
 *    GET  /             pagina
 *    GET  /api/stato    impostazioni + stato della scheda (JSON)
 *    POST /api/imposta  chiave=valore&...  (salva=1 per scrivere in NVS)
 *    POST /api/prova    s=gol|rosso|inizio|intervallo|fine|passaggio|partenza|tutti
 *    POST /api/riavvia
 *
 *  Gli slider mandano i valori mentre li muovi (salva=0, effetto immediato)
 *  e li salvano quando li rilasci (salva=1): niente scritture continue in NVS.
 * ========================================================================== */
#include <ESPmDNS.h>
#include <WebServer.h>
#include <WiFi.h>

#include "AppConfig.h"
#include "Audio.h"
#include "BoardLvgl.h"
#include "DataModel.h"
#include "Impostazioni.h"
#include "Notify.h"
#include "esp_heap_caps.h"

static WebServer s_server(80);

static const char PAGINA[] PROGMEM = R"HTML(<!doctype html>
<html lang="it"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Sport P4</title>
<style>
:root{--bg:#f6f8f8;--card:#fff;--ink:#16181d;--mut:#5b6470;--acc:#0f766e;--bor:#dfe5e5}
@media (prefers-color-scheme:dark){:root{--bg:#0d1117;--card:#161b22;--ink:#e6edf3;--mut:#8b949e;--acc:#2dd4bf;--bor:#30363d}}
*{box-sizing:border-box}body{margin:0;font:16px/1.45 system-ui,Segoe UI,sans-serif;background:var(--bg);color:var(--ink)}
main{max-width:720px;margin:0 auto;padding:16px}
h1{font-size:22px;margin:4px 0 2px}h2{font-size:14px;letter-spacing:.06em;text-transform:uppercase;color:var(--mut);margin:0 0 12px}
.sub{color:var(--mut);margin:0 0 16px}
section{background:var(--card);border:1px solid var(--bor);border-radius:12px;padding:16px;margin-bottom:14px}
.riga{display:flex;align-items:center;justify-content:space-between;gap:12px;padding:8px 0;border-top:1px solid var(--bor)}
.riga:first-of-type{border-top:0}
input[type=range]{width:100%;accent-color:var(--acc)}
.val{min-width:48px;text-align:right;font-variant-numeric:tabular-nums;color:var(--acc);font-weight:600}
select{font:inherit;padding:6px 8px;border-radius:8px;border:1px solid var(--bor);background:var(--card);color:var(--ink)}
.sw{position:relative;width:46px;height:26px;flex:none}.sw input{opacity:0;width:0;height:0}
.sw span{position:absolute;inset:0;background:var(--bor);border-radius:26px;transition:.2s;cursor:pointer}
.sw span:before{content:"";position:absolute;width:20px;height:20px;left:3px;top:3px;background:#fff;border-radius:50%;transition:.2s}
.sw input:checked+span{background:var(--acc)}.sw input:checked+span:before{transform:translateX(20px)}
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(150px,1fr));gap:8px}
.grid label{display:flex;gap:8px;align-items:center;padding:8px 10px;border:1px solid var(--bor);border-radius:8px;cursor:pointer}
.grid input{accent-color:var(--acc);width:18px;height:18px}
button{font:inherit;padding:8px 12px;border-radius:8px;border:1px solid var(--bor);background:var(--card);color:var(--ink);cursor:pointer}
button:hover{border-color:var(--acc)}.btns{display:flex;flex-wrap:wrap;gap:8px}
dl{display:grid;grid-template-columns:max-content 1fr;gap:4px 16px;margin:0}dt{color:var(--mut)}dd{margin:0}
#msg{position:fixed;left:50%;bottom:16px;transform:translateX(-50%);background:var(--acc);color:#fff;padding:8px 14px;border-radius:8px;opacity:0;transition:.3s}
</style></head><body><main>
<h1>Sport P4</h1><p class="sub">Formula 1 e calcio sul display ESP32-P4 &mdash; impostazioni</p>

<section><h2>Schermo e audio</h2>
<div class="riga"><span>Volume</span><span class="val" id="v_volume"></span></div>
<input type="range" id="volume" min="0" max="100" step="1">
<div class="riga"><span>Luminosit&agrave;</span><span class="val" id="v_luce"></span></div>
<input type="range" id="luce" min="5" max="100" step="1">
<div class="riga"><span>Standby dello schermo</span>
<select id="standby"><option value="0">mai</option><option value="1">1 minuto</option><option value="2">2 minuti</option>
<option value="5">5 minuti</option><option value="10">10 minuti</option><option value="30">30 minuti</option></select></div>
</section>

<section><h2>Avvisi</h2>
<div class="riga"><span>Suoni degli avvisi</span><label class="sw"><input type="checkbox" id="suoni"><span></span></label></div>
<div class="riga"><span>Avvisi calcio (gol, fischi)</span><label class="sw"><input type="checkbox" id="avvCalcio"><span></span></label></div>
<div class="riga"><span>Avvisi cartellini rossi</span><label class="sw"><input type="checkbox" id="avvRossi"><span></span></label></div>
<div class="riga"><span>Avvisi Formula 1</span><label class="sw"><input type="checkbox" id="avvF1"><span></span></label></div>
</section>

<section><h2>Competizioni mostrate</h2><div class="grid" id="comp"></div></section>

<section><h2>Prova suoni</h2><div class="btns">
<button data-s="gol">Gol</button><button data-s="rosso">Espulsione</button><button data-s="inizio">Fischio d'inizio</button>
<button data-s="intervallo">Intervallo</button><button data-s="fine">Fine partita</button>
<button data-s="partenza">Partenza F1</button><button data-s="passaggio">Passaggio F1</button><button data-s="tutti">Tutti</button>
</div></section>

<section><h2>Stato</h2><dl id="info"></dl>
<div class="btns" style="margin-top:12px"><button id="riavvia">Riavvia la scheda</button></div></section>
</main><div id="msg"></div>
<script>
const $=id=>document.getElementById(id);
const COMP=["Serie A","Nations League","Champions League","Europa League","Europei","Mondiali"];
let comp=63;
function msg(t){const m=$("msg");m.textContent=t;m.style.opacity=1;clearTimeout(m._t);m._t=setTimeout(()=>m.style.opacity=0,1500)}
function invia(o,salva){const b=new URLSearchParams(o);b.set("salva",salva?1:0);
 return fetch("/api/imposta",{method:"POST",body:b}).then(r=>{if(salva)msg(r.ok?"Salvato":"Errore")})}
function slider(id,key,suff){const s=$(id),v=$("v_"+id);
 s.oninput=()=>{v.textContent=s.value+suff;invia({[key]:s.value},0)};
 s.onchange=()=>invia({[key]:s.value},1)}
slider("volume","volume","");slider("luce","luce","%");
$("standby").onchange=e=>invia({standby:e.target.value},1);
for(const k of ["suoni","avvCalcio","avvRossi","avvF1"])$(k).onchange=e=>invia({[k]:e.target.checked?1:0},1);
document.querySelectorAll("[data-s]").forEach(b=>b.onclick=()=>fetch("/api/prova",{method:"POST",body:new URLSearchParams({s:b.dataset.s})}).then(()=>msg("Suono inviato")));
$("riavvia").onclick=()=>{if(confirm("Riavviare la scheda?"))fetch("/api/riavvia",{method:"POST"}).then(()=>msg("Riavvio..."))};
function disegnaComp(){const g=$("comp");g.innerHTML="";COMP.forEach((n,i)=>{const l=document.createElement("label");
 const c=document.createElement("input");c.type="checkbox";c.checked=!!(comp&(1<<i));
 c.onchange=()=>{let nv=c.checked?comp|(1<<i):comp&~(1<<i);if(!nv){c.checked=true;msg("Almeno una");return}comp=nv;invia({comp:comp},1)};
 l.append(c,document.createTextNode(n));g.append(l)})}
function stato(primo){fetch("/api/stato").then(r=>r.json()).then(j=>{
 if(primo){$("volume").value=j.volume;$("v_volume").textContent=j.volume;$("luce").value=j.luce;$("v_luce").textContent=j.luce+"%";
  $("standby").value=j.standby;for(const k of ["suoni","avvCalcio","avvRossi","avvF1"])$(k).checked=j[k];comp=j.comp;disegnaComp()}
 const i=j.info;$("info").innerHTML=Object.entries(i).map(([k,v])=>"<dt>"+k+"</dt><dd>"+v+"</dd>").join("")})}
stato(true);setInterval(()=>stato(false),5000);
</script></body></html>)HTML";

static bool argBool(const char *k, bool attuale)
{
    if (!s_server.hasArg(k)) return attuale;
    const String v = s_server.arg(k);
    return v == "1" || v == "true" || v == "on";
}

static void inviaStato()
{
    Impostazioni &I = imp();
    char f1[96] = "-";
    uint8_t live = 0;
    if (dataLock(200)) {
        const F1Live &L = *g_f1live;
        if (L.meeting[0]) snprintf(f1, sizeof(f1), "%s - %s (%s)%s", L.meeting, L.sessione, L.stato, L.connesso ? ", collegato" : "");
        for (uint8_t c = 0; c < CALCIO_COMP_COUNT; ++c) live += g_comp[c].live;
        dataUnlock();
    }
    for (char *p = f1; *p; ++p) if (*p == '"' || *p == '\\') *p = '\'';
    const uint32_t s = millis() / 1000;
    char out[900];
    snprintf(out, sizeof(out),
             "{\"volume\":%u,\"luce\":%u,\"standby\":%u,\"suoni\":%s,\"avvCalcio\":%s,\"avvRossi\":%s,\"avvF1\":%s,\"comp\":%u,"
             "\"info\":{\"Indirizzo\":\"%s  (sport-p4.local)\",\"Acceso da\":\"%lu h %02lu min\",\"Schermo\":\"%s\","
             "\"RAM interna libera\":\"%u KB\",\"PSRAM libera\":\"%u KB\",\"Partite in corso\":\"%u\",\"F1\":\"%s\"}}",
             I.volume, I.luminosita, I.standbyMin, I.suoni ? "true" : "false", I.avvisiCalcio ? "true" : "false",
             I.avvisiRossi ? "true" : "false", I.avvisiF1 ? "true" : "false", I.compAttive,
             WiFi.localIP().toString().c_str(), (unsigned long)(s / 3600), (unsigned long)((s / 60) % 60),
             boardIsScreenOn() ? "acceso" : "standby",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024), live, f1);
    s_server.sendHeader("Cache-Control", "no-store");
    s_server.send(200, "application/json", out);
}

static void imposta()
{
    Impostazioni &I = imp();
    if (s_server.hasArg("volume")) {
        I.volume = (uint8_t)constrain(s_server.arg("volume").toInt(), 0, 100);
        audioSetVolume(I.volume);
    }
    if (s_server.hasArg("luce")) {
        I.luminosita = (uint8_t)constrain(s_server.arg("luce").toInt(), 5, 100);
        if (lvglLock(500)) { boardSetBrightness(I.luminosita); boardResetScreenTimeout(); lvglUnlock(); }
    }
    if (s_server.hasArg("standby")) {
        I.standbyMin = (uint16_t)constrain(s_server.arg("standby").toInt(), 0, 600);
        if (lvglLock(500)) { boardSetScreenTimeoutMs((uint32_t)I.standbyMin * 60000UL); lvglUnlock(); }
    }
    I.suoni        = argBool("suoni", I.suoni);
    I.avvisiCalcio = argBool("avvCalcio", I.avvisiCalcio);
    I.avvisiRossi  = argBool("avvRossi", I.avvisiRossi);
    I.avvisiF1     = argBool("avvF1", I.avvisiF1);
    if (s_server.hasArg("comp")) {
        const uint8_t c = (uint8_t)(s_server.arg("comp").toInt() & 0x3F);
        if (c != 0) I.compAttive = c;
    }
    if (s_server.arg("salva") == "1") {
        impostazioniSalva();
        Serial.println("[WEB] impostazioni salvate");
    }
    s_server.send(200, "text/plain", "ok");
}

static void prova()
{
    const String s = s_server.arg("s");
    if (s == "gol")             audioPlay(SND_GOL);
    else if (s == "rosso")      audioPlay(SND_ESPULSIONE);
    else if (s == "inizio")     audioPlay(SND_FISCHIO_INIZIO);
    else if (s == "intervallo") audioPlay(SND_FISCHIO_INTERVALLO);
    else if (s == "fine")       audioPlay(SND_FISCHIO_FINE);
    else if (s == "partenza")   audioPlay(SND_F1_PARTENZA);
    else if (s == "passaggio")  audioPlay(SND_F1_PASSAGGIO);
    else if (s == "tutti")      notifyDemoSuoni();
    s_server.send(200, "text/plain", "ok");
}

static void webTask(void *arg)
{
    (void)arg;
    /* aspetta il Wi-Fi */
    while (WiFi.status() != WL_CONNECTED) vTaskDelay(pdMS_TO_TICKS(1000));

    s_server.on("/", HTTP_GET, []() { s_server.send_P(200, "text/html; charset=utf-8", PAGINA); });
    s_server.on("/api/stato", HTTP_GET, inviaStato);
    s_server.on("/api/imposta", HTTP_POST, imposta);
    s_server.on("/api/prova", HTTP_POST, prova);
    s_server.on("/api/riavvia", HTTP_POST, []() {
        s_server.send(200, "text/plain", "ok");
        delay(500);
        ESP.restart();
    });
    s_server.onNotFound([]() { s_server.send(404, "text/plain", "non trovato"); });
    s_server.begin();
    if (MDNS.begin("sport-p4")) MDNS.addService("http", "tcp", 80);
    Serial.printf("[WEB] configurazione su http://%s/  (http://sport-p4.local/)\n", WiFi.localIP().toString().c_str());

    while (true) {
        s_server.handleClient();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

void webBegin()
{
    xTaskCreatePinnedToCore(webTask, "web", 8192, nullptr, 1, nullptr, 0);
}

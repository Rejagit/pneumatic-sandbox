#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <NetworkUdp.h>
#include <ArduinoOTA.h>

// --- KONFIGURASI WIFI: ESP32 MEMBUAT HOTSPOT SENDIRI (MODE AP) ---
// Sambungkan HP atau laptop ke WiFi ini, lalu buka http://192.168.4.1 di browser.
const char* apSsid = "Lengan-Robot";
const char* apPassword = "12345678";  // minimal 8 karakter
const int apChannel = 1;               // channel WiFi 1 sampai 13
const int apMaxClient = 4;             // jumlah perangkat yang boleh tersambung bersamaan

IPAddress local_IP(192, 168, 4, 1);
IPAddress gateway(192, 168, 4, 1);
IPAddress subnet(255, 255, 255, 0);

// --- KONFIGURASI OTA (UPLOAD PROGRAM LEWAT WIFI) ---
const char* otaHostname = "lengan-robot";  // nama yang muncul di Arduino IDE
const char* otaPassword = "admin";         // password diminta saat upload, silakan ganti
uint32_t last_ota_time = 0;

// --- PIN RELAY LENGAN ROBOT PNEUMATIK (JOYSTICK) ---
// Relay 1 = Naik        (GPIO 4)    Relay 2 = Turun       (GPIO 5)
// Relay 3 = Buka capit  (GPIO 6)    Relay 4 = Putar kiri  (GPIO 7)
// Relay 5 = Putar kanan (GPIO 15)   Relay 6 = Mundur      (GPIO 8)
// Relay 7 = Maju        (GPIO 9)
const int relayPins[] = {4, 5, 6, 7, 15, 8, 9};
const char* namaRelay[] = {"Naik", "Turun", "Buka capit", "Putar kiri", "Putar kanan", "Mundur", "Maju"};
const int NUM_RELAY = sizeof(relayPins) / sizeof(relayPins[0]);

// Pasangan relay yang tidak boleh menyala bersamaan (-1 = tidak berpasangan)
const int pasangan[] = {1, 0, -1, 4, 3, 6, 5};

// Status tiap relay (mati di awal)
bool relayStatus[NUM_RELAY] = {false};

// Pengaman mode tahan: relay mati sendiri bila sinyal dari browser berhenti
// (WiFi putus, layar HP mati, tab ditutup).
unsigned long holdUntil[NUM_RELAY] = {0};
const unsigned long HOLD_MS = 700;

// Logika Relay (Sesuaikan jika relay Anda Active High)
#define RELAY_ON LOW
#define RELAY_OFF HIGH

// --- MODE AUTO: PINDAHKAN BOTOL DARI KIRI KE KANAN ---
// Semua waktu dalam milidetik. Sesuaikan dengan kecepatan silinder Anda:
// waktu gerak harus cukup lama agar silinder sampai ujung langkah.
const unsigned int T_PUTAR = 1500;  // putar kiri / kanan
const unsigned int T_VERT = 1000;   // naik / turun
const unsigned int T_HORIZ = 1000;  // maju / mundur
const unsigned int T_CAPIT = 700;   // capit membuka atau menutup
const unsigned int T_JEDA = 300;    // jeda antar langkah

// Index relay (relay 1 = 0)
enum { R_NAIK = 0, R_TURUN = 1, R_CAPIT = 2, R_KIRI = 3, R_KANAN = 4, R_MUNDUR = 5, R_MAJU = 6 };
// GERAK: relay menyala selama waktu langkah lalu mati. BUKA / TUTUP: capit tetap pada posisinya.
enum Aksi { GERAK, BUKA, TUTUP };
struct Langkah { Aksi aksi; int relay; unsigned int ms; const char* nama; };

// Urutan langkah, ubah sesuai kebutuhan
const Langkah URUTAN[] = {
  // Posisi awal: capit tertutup, naik, mundur, putar kiri
  {TUTUP, 0,        T_CAPIT,       "Tutup capit"},
  {GERAK, R_NAIK,   T_VERT,        "Naik"},
  {GERAK, R_MUNDUR, T_HORIZ,       "Mundur"},
  {GERAK, R_KIRI,   T_PUTAR,       "Putar kiri"},
  // Ambil botol di sisi kiri
  {BUKA,  0,        T_CAPIT,       "Buka capit"},
  {GERAK, R_MAJU,   T_HORIZ,       "Maju"},
  {GERAK, R_TURUN,  T_VERT,        "Turun"},
  {TUTUP, 0,        T_CAPIT + 300, "Cengkeram botol"},
  {GERAK, R_NAIK,   T_VERT,        "Angkat botol"},
  {GERAK, R_MUNDUR, T_HORIZ,       "Mundur"},
  // Pindah ke sisi kanan
  {GERAK, R_KANAN,  T_PUTAR,       "Putar kanan"},
  {GERAK, R_MAJU,   T_HORIZ,       "Maju"},
  {GERAK, R_TURUN,  T_VERT,        "Turun"},
  {BUKA,  0,        T_CAPIT,       "Lepas botol"},
  // Kembali ke posisi awal
  {GERAK, R_NAIK,   T_VERT,        "Naik"},
  {GERAK, R_MUNDUR, T_HORIZ,       "Mundur"},
  {TUTUP, 0,        T_CAPIT,       "Tutup capit"},
  {GERAK, R_KIRI,   T_PUTAR,       "Putar kiri"},
};
const int JUMLAH_LANGKAH = sizeof(URUTAN) / sizeof(URUTAN[0]);

bool autoJalan = false;    // true saat urutan sedang berjalan
bool autoSelesai = false;  // true setelah urutan selesai sampai langkah terakhir
int autoIdx = 0;           // langkah yang sedang dijalankan
int autoFase = 0;          // 0 = kirim perintah, 1 = menunggu, 2 = jeda
unsigned long autoT0 = 0;

WebServer server(80);

// --- HALAMAN JOYSTICK (disimpan di flash, tidak memakan RAM) ---
const char INDEX_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="id">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,user-scalable=no">
<meta name="color-scheme" content="light">
<meta name="theme-color" content="#EEF2F6">
<link rel="icon" href="data:,">
<title>Lengan Robot &ndash; PT MULTI SPUNINDO JAYA</title>
<style>
:root{color-scheme:light;--bg:#EEF2F6;--ink:#14202D;--mut:#5E6B7A;--line:#DCE3EB;--navy:#0B4F9E;--air:#0A6CD6;--hi:#3B97F5;--air2:rgba(10,108,214,.18);--bad:#B3261E;--track:#9AA7B5}
*{box-sizing:border-box}
body{margin:0;min-height:100vh;color:var(--ink);font:16px/1.5 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;background:var(--bg) radial-gradient(#CBD5E1 1px,transparent 1.2px) 0 0/20px 20px}
main{max-width:460px;margin:0 auto;padding:36px 18px 56px}
button{font-family:inherit;font-size:14px;font-weight:600;line-height:1;cursor:pointer}

header{text-align:center;margin-bottom:26px}
.mark{display:grid;place-items:center;width:48px;height:48px;margin:0 auto 14px;border-radius:15px;background:linear-gradient(145deg,var(--hi),var(--air));box-shadow:0 10px 22px -8px rgba(10,108,214,.55),inset 0 1px 0 rgba(255,255,255,.35)}
.mark svg{width:26px;height:26px;fill:none;stroke:#fff;stroke-width:1.8;stroke-linecap:round;stroke-linejoin:round}
h1{margin:0;font:600 clamp(34px,9vw,46px)/1.05 "Iowan Old Style","Palatino Linotype",Palatino,Georgia,serif;letter-spacing:-.015em}
header p{display:flex;align-items:center;justify-content:center;gap:10px;margin:12px 0 0;font-size:12px;font-weight:600;letter-spacing:.14em;color:var(--navy)}
header p::before,header p::after{content:"";width:22px;height:1px;background:currentColor;opacity:.35}

.card{padding:22px 22px 24px;border-radius:30px;background:#fff;border:1px solid rgba(20,32,45,.06);box-shadow:0 1px 2px rgba(20,40,70,.05),0 24px 48px -24px rgba(20,40,70,.28)}
.sum{display:flex;align-items:center;gap:10px;margin:0 0 14px;padding:9px 14px;border-radius:99px;background:#F1F5F9;color:var(--mut);font-size:14px;font-weight:500;transition:background .3s,color .3s}
.dot{flex:none;width:8px;height:8px;border-radius:50%;background:var(--track)}
.ok .sum{color:var(--ink)}
.ok .dot{background:#22A06B;box-shadow:0 0 0 4px rgba(34,160,107,.16)}
.lost .sum{background:#FDECEA;color:var(--bad)}
.lost .dot{background:var(--bad)}

/* Jalur udara: mengalir selama ada valve yang aktif */
.pipe{height:4px;border-radius:2px;background:var(--line);margin-bottom:26px}
.pipe.on{background:linear-gradient(90deg,var(--air) 55%,var(--air2) 55%) 0 0/20px 100%;animation:flow .8s linear infinite}
@keyframes flow{to{background-position:20px 0}}

.wait .pad,.wait .all,.lost .pad,.lost .all{opacity:.45;pointer-events:none}
.pad{display:grid;grid-template-columns:repeat(3,1fr);gap:14px;max-width:340px;margin:0 auto;transition:opacity .3s;-webkit-user-select:none;user-select:none;-webkit-touch-callout:none}
.pad button{align-self:start;aspect-ratio:1;display:flex;flex-direction:column;align-items:center;justify-content:center;gap:6px;padding:0;border-radius:26px;border:1px solid var(--line);background:linear-gradient(180deg,#fff,#F0F4F8);color:var(--ink);box-shadow:0 4px 0 #D3DBE4,0 12px 18px -10px rgba(20,40,70,.35);touch-action:none;-webkit-tap-highlight-color:transparent;transition:transform .1s,box-shadow .1s,background .15s,color .15s,border-color .15s}
.pad svg{width:34%;height:auto;fill:none;stroke:currentColor;stroke-width:2.2;stroke-linecap:round;stroke-linejoin:round}
.pad span{font-size:12px;font-weight:500;color:var(--mut)}
.pad .cv svg{color:var(--air)}
.pad .on{background:linear-gradient(180deg,var(--hi),var(--air));border-color:var(--air);color:#fff;box-shadow:0 1px 0 #0A5CB5,0 6px 14px -6px rgba(10,108,214,.6);transform:translateY(3px)}
.pad .on span,.pad .cv.on svg{color:#fff}

.hint{margin:24px auto 16px;max-width:300px;text-align:center;font-size:13px;line-height:1.5;color:var(--mut)}
.all{display:flex;align-items:center;justify-content:center;gap:8px;width:100%;padding:15px;border-radius:18px;border:1px solid #F1C4BF;background:#FDF0EE;color:var(--bad);font-size:15px;transition:background .2s,color .2s,border-color .2s}
.all svg{width:14px;height:14px}
.all:hover,.all:active{background:var(--bad);border-color:var(--bad);color:#fff}
.pad button:focus-visible,.all:focus-visible{outline:2px solid var(--air);outline-offset:3px}
.tabs{display:grid;grid-template-columns:1fr 1fr;gap:4px;padding:4px;margin-bottom:22px;border-radius:16px;background:#F1F5F9}
.tabs button{padding:11px;border:0;border-radius:12px;background:none;color:var(--mut);transition:background .2s,color .2s,box-shadow .2s}
.tabs .act{background:#fff;color:var(--ink);box-shadow:0 1px 3px rgba(20,40,70,.15)}
.run .pad{opacity:.45;pointer-events:none}
.wait .go,.lost .go{opacity:.45;pointer-events:none}

#va h2{margin:0;text-align:center;font:600 22px/1.2 "Iowan Old Style","Palatino Linotype",Palatino,Georgia,serif}
#va .hint{margin:8px auto 22px}
.prog{height:8px;border-radius:99px;background:#E6ECF2;overflow:hidden}
.prog i{display:block;height:100%;width:0;border-radius:99px;background:linear-gradient(90deg,var(--hi),var(--air));transition:width .4s}
.cur{margin:12px 0 18px;text-align:center;font-size:14px;font-weight:500}
.go{width:100%;padding:16px;border:0;border-radius:18px;background:linear-gradient(180deg,var(--hi),var(--air));color:#fff;font-size:16px;box-shadow:0 4px 0 #0A5CB5,0 12px 18px -10px rgba(10,108,214,.6);transition:transform .1s,box-shadow .1s}
.go:active{transform:translateY(3px);box-shadow:0 1px 0 #0A5CB5}
.go.stop{background:linear-gradient(180deg,#E5584E,var(--bad));box-shadow:0 4px 0 #8E1C16,0 12px 18px -10px rgba(179,38,30,.6)}
.go.stop:active{box-shadow:0 1px 0 #8E1C16}
details{margin:18px 0 0;font-size:14px;color:var(--mut)}
summary{cursor:pointer;text-align:center;padding:6px}
#sl{margin:10px 0 0;padding:0;list-style:none;counter-reset:s}
#sl li{counter-increment:s;display:flex;align-items:center;gap:10px;padding:6px 8px;border-radius:10px}
#sl li::before{content:counter(s);flex:none;display:grid;place-items:center;width:22px;height:22px;border-radius:50%;background:#EEF2F6;font-size:12px;font-weight:600}
#sl .c{background:#EAF3FE;color:var(--ink);font-weight:600}
#sl .c::before{background:var(--air);color:#fff}
#sl .d{opacity:.55}
#sl .d::before{content:"\2713";background:#22A06B;color:#fff}
.tabs button:focus-visible,.go:focus-visible{outline:2px solid var(--air);outline-offset:3px}
@media(prefers-reduced-motion:reduce){.pipe.on{animation:none}*{transition:none!important}}
</style>
</head>
<body class="wait">
<main>
  <header>
    <div class="mark"><svg viewBox="0 0 24 24"><path d="M4 20h7M7.5 20v-7l8-7"/><circle cx="7.5" cy="13" r="1.6"/><circle cx="15.5" cy="6" r="1.6"/><path d="M17 5l3.5-1M17 7l3 2"/></svg></div>
    <h1>Lengan Robot</h1>
    <p>PT MULTI SPUNINDO JAYA</p>
  </header>
  <section class="card">
    <p class="sum"><i class="dot"></i><span id="msg">Menghubungkan ke ESP32</span></p>
    <div class="pipe" id="pipe"></div>
    <div class="tabs"><button id="tm" class="act" onclick="mode(0)">Manual</button><button id="ta" onclick="mode(1)">Auto</button></div>
    <div id="vm">
      <div class="pad" id="pad" oncontextmenu="return false"></div>
      <p class="hint">Tahan tombol arah untuk bergerak, lepas untuk berhenti. Tombol capit: tekan untuk buka atau tutup.</p>
    </div>
    <div id="va" hidden>
      <h2>Pindahkan botol</h2>
      <p class="hint">Mengambil botol di sisi kiri, memindahkannya ke sisi kanan, lalu kembali ke posisi awal. Pastikan area lengan bebas sebelum memulai.</p>
      <div class="prog"><i id="bar"></i></div>
      <p class="cur" id="cur">Siap dijalankan</p>
      <button class="go" id="go" onclick="autoBtn()">Mulai auto</button>
      <details><summary>Lihat urutan langkah</summary><ol id="sl"></ol></details>
    </div>
    <button class="all" onclick="allOff()"><svg viewBox="0 0 24 24"><rect x="5" y="5" width="14" height="14" rx="2.5" fill="currentColor"/></svg>Stop semua</button>
  </section>
</main>
<script>
const $=s=>document.querySelector(s);
// [relay (mulai 0), label, ikon, putaran ikon, kolom, baris]
const BT=[[5,'Mundur','v',180,1,1],[0,'Naik','a',0,2,1],[6,'Maju','v',0,3,1],
          [3,'Putar kiri','a',270,1,2],[2,'Capit','o',0,2,2],[4,'Putar kanan','a',90,3,2],
          [1,'Turun','a',180,2,3]];
const IC={a:'<path d="M12 19V5M5 12l7-7 7 7"/>',v:'<path d="M9 5l7 7-7 7"/>',o:'<circle cx="12" cy="12" r="7"/>'};
const PR=[1,0,-1,4,3,6,5];   // pasangan yang tidak boleh aktif bersamaan
const RO=['Naik','Turun','Capit terbuka','Putar kiri','Putar kanan','Mundur','Maju'];
let st=[],ok=null,seq=0,au=-1,dn=0,names=[];const held={},beat={};

function build(){
  $('#pad').innerHTML=BT.map(([i,t,k,d,c,r])=>{
    const h=i==2?`onclick="tog(2)"`:`onpointerdown="press(${i},event)" onpointerup="rel(${i})" onpointercancel="rel(${i})" onlostpointercapture="rel(${i})" onblur="rel(${i})" onkeydown="key(${i},event,1)" onkeyup="key(${i},event,0)"`;
    return `<button id="b${i}" class="${k=='v'?'cv':''}" style="grid-column:${c};grid-row:${r}" aria-label="${t}" ${h}><svg viewBox="0 0 24 24" style="transform:rotate(${d}deg)">${IC[k]}</svg><span>${t}</span></button>`;
  }).join('');
}

function paint(){
  const a=[];
  BT.forEach(([i])=>{
    const o=!!st[i],b=$('#b'+i);
    b.classList.toggle('on',o);b.setAttribute('aria-pressed',o);
    if(o)a.push(RO[i]);
  });
  const run=au>=0,n=names.length||1;
  document.body.className=(ok===false?'lost':ok?'ok':'wait')+(run?' run':'');
  $('#pipe').classList.toggle('on',a.length>0);
  $('#msg').textContent=ok===null?'Menghubungkan ke ESP32':ok?(run?'Mode auto berjalan':a.length?a.join(', '):'Semua valve tertutup'):'Koneksi terputus. Memeriksa ulang otomatis.';
  $('#bar').style.width=(run?(au+1)/n*100:dn?100:0)+'%';
  $('#cur').textContent=run?`Langkah ${au+1} dari ${n}: ${names[au]||''}`:dn?'Selesai. Botol sudah dipindahkan.':'Siap dijalankan';
  const g=$('#go');g.textContent=run?'Hentikan auto':'Mulai auto';g.classList.toggle('stop',run);
  names.forEach((_,i)=>{$('#s'+i).className=run?(i<au?'d':i==au?'c':''):dn?'d':''});
}

// Hanya respons terbaru yang dipakai, jadi tampilan tidak berkedip
async function api(u){
  const k=++seq;let d,good=true;
  try{d=await(await fetch(u,{cache:'no-store'})).json()}catch(e){good=false}
  if(k!==seq)return;
  ok=good;
  if(ok){st=d.r;au=d.a;dn=d.d}
  paint();
}

// Tahan tombol = relay menyala. Selama ditahan, browser mengirim sinyal tiap 250 ms.
function press(i,e){
  if(e.pointerId!=null)e.currentTarget.setPointerCapture(e.pointerId);
  if(held[i])return;
  const p=PR[i];
  if(p>=0&&held[p])drop(p);
  held[i]=1;st[i]=1;if(p>=0)st[p]=0;
  paint();
  const send=()=>api(`/set?id=${i}&state=1&hold=1`);
  send();beat[i]=setInterval(send,250);
}
function drop(i){clearInterval(beat[i]);held[i]=0}
function rel(i){
  if(!held[i])return;
  drop(i);st[i]=0;paint();
  api(`/set?id=${i}&state=0`);
}
function key(i,e,down){
  if(e.key!==' '&&e.key!=='Enter')return;
  e.preventDefault();
  down?(e.repeat||press(i,e)):rel(i);
}

// Capit: tekan sekali untuk buka, tekan lagi untuk tutup
function tog(i){
  const v=st[i]?0:1;
  st[i]=v;paint();
  api(`/set?id=${i}&state=${v}`);
}
function allOff(){
  for(let i=0;i<7;i++)drop(i);
  st=[];au=-1;dn=0;paint();
  api('/alloff');
}
document.onvisibilitychange=()=>{if(document.hidden)for(let i=0;i<7;i++)rel(i)};

// Mode auto
function mode(m){
  $('#vm').hidden=!!m;$('#va').hidden=!m;
  $('#tm').classList.toggle('act',!m);$('#ta').classList.toggle('act',!!m);
}
function autoBtn(){
  if(au>=0)return allOff();
  for(let i=0;i<7;i++)drop(i);
  mode(1);api('/auto');
}
async function steps(){
  try{
    names=await(await fetch('/steps')).json();
    $('#sl').innerHTML=names.map((t,i)=>`<li id="s${i}">${t}</li>`).join('');
    paint();
  }catch(e){setTimeout(steps,2000)}
}

build();paint();steps();
// Polling lebih rapat selama mode auto berjalan
(function tick(){api('/status');setTimeout(tick,au>=0?500:2000)})();
</script>
</body>
</html>)rawliteral";

// Kirim status relay dan mode auto dalam format JSON, contoh: {"r":[0,1,0,0,0,0,0],"a":-1,"d":0}
void sendStatus() {
  String json = "{\"r\":[";
  for (int i = 0; i < NUM_RELAY; i++) {
    if (i) json += ',';
    json += relayStatus[i] ? '1' : '0';
  }
  json += "],\"a\":";
  json += autoJalan ? autoIdx : -1;
  json += ",\"d\":";
  json += autoSelesai ? 1 : 0;
  json += '}';
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", json);
}

void setRelay(int id, bool on) {
  // Interlock: arah berlawanan dimatikan dulu, tidak pernah menyala bersamaan
  int p = pasangan[id];
  if (on && p >= 0 && relayStatus[p]) {
    relayStatus[p] = false;
    holdUntil[p] = 0;
    digitalWrite(relayPins[p], RELAY_OFF);
    Serial.printf("Relay %d (%s) -> OFF\n", p + 1, namaRelay[p]);
    delay(50);
  }
  if (!on) holdUntil[id] = 0;
  if (relayStatus[id] != on) {
    Serial.printf("Relay %d (%s) -> %s\n", id + 1, namaRelay[id], on ? "ON" : "OFF");
  }
  relayStatus[id] = on;
  digitalWrite(relayPins[id], on ? RELAY_ON : RELAY_OFF);
}

void semuaMati() {
  for (int i = 0; i < NUM_RELAY; i++) setRelay(i, false);
}

void autoStart() {
  semuaMati();
  autoJalan = true;
  autoSelesai = false;
  autoIdx = 0;
  autoFase = 0;
  Serial.println("Mode auto: mulai");
}

// Dipanggil terus dari loop(), jadi web server tetap responsif
void autoTick() {
  if (!autoJalan) return;
  const Langkah& L = URUTAN[autoIdx];
  unsigned long now = millis();
  if (autoFase == 0) {
    Serial.printf("Auto %d/%d: %s\n", autoIdx + 1, JUMLAH_LANGKAH, L.nama);
    if (L.aksi == GERAK) setRelay(L.relay, true);
    else setRelay(R_CAPIT, L.aksi == BUKA);
    autoT0 = now;
    autoFase = 1;
  } else if (autoFase == 1) {
    if (now - autoT0 >= L.ms) {
      if (L.aksi == GERAK) setRelay(L.relay, false);
      autoT0 = now;
      autoFase = 2;
    }
  } else if (now - autoT0 >= T_JEDA) {
    autoIdx++;
    autoFase = 0;
    if (autoIdx >= JUMLAH_LANGKAH) {
      autoJalan = false;
      autoSelesai = true;
      Serial.println("Mode auto: selesai");
    }
  }
}

void handleRoot() {
  server.send_P(200, "text/html", INDEX_HTML);
}

void handleRelay() {
  // Kontrol manual dikunci selama mode auto berjalan
  if (!autoJalan && server.hasArg("id") && server.hasArg("state")) {
    int id = server.arg("id").toInt();
    int state = server.arg("state").toInt();

    // Validasi ID relay
    if (id >= 0 && id < NUM_RELAY) {
      setRelay(id, state == 1);
      // Mode tahan: perpanjang waktu hidup relay selama sinyal masih datang
      if (state == 1 && server.hasArg("hold")) holdUntil[id] = millis() + HOLD_MS;
    }
  }
  sendStatus();
}

void handleAllOff() {
  autoJalan = false;  // hentikan mode auto bila sedang berjalan
  autoSelesai = false;
  semuaMati();
  sendStatus();
}

void handleAuto() {
  if (!autoJalan) autoStart();
  sendStatus();
}

// Daftar nama langkah, dipakai halaman web untuk menampilkan progres
void sendSteps() {
  String json = "[";
  for (int i = 0; i < JUMLAH_LANGKAH; i++) {
    if (i) json += ',';
    json += '"';
    json += URUTAN[i].nama;
    json += '"';
  }
  json += ']';
  server.send(200, "application/json", json);
}

// --- OTA: upload program lewat WiFi ---
void setupOTA() {
  ArduinoOTA.setHostname(otaHostname);
  ArduinoOTA.setPassword(otaPassword);

  ArduinoOTA
    .onStart([]() {
      // Pengaman: selama upload, loop() berhenti. Matikan semua relay dan hentikan
      // mode auto dulu, supaya tidak ada valve yang tertinggal menyala.
      autoJalan = false;
      autoSelesai = false;
      semuaMati();
      Serial.println(ArduinoOTA.getCommand() == U_FLASH ? "Mulai update: sketch" : "Mulai update: filesystem");
    })
    .onEnd([]() {
      Serial.println("\nUpdate selesai, ESP32 restart");
    })
    .onProgress([](unsigned int progress, unsigned int total) {
      if (millis() - last_ota_time > 500) {
        Serial.printf("Progress: %u%%\n", (progress * 100) / total);
        last_ota_time = millis();
      }
    })
    .onError([](ota_error_t error) {
      Serial.printf("Error[%u]: ", error);
      if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
      else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
      else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
      else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
      else if (error == OTA_END_ERROR) Serial.println("End Failed");
    });

  ArduinoOTA.begin();
  Serial.println("OTA siap");
}

void setup() {
  Serial.begin(115200);

  // Inisialisasi semua pin relay sebagai output
  for (int i = 0; i < NUM_RELAY; i++) {
    pinMode(relayPins[i], OUTPUT);
    digitalWrite(relayPins[i], RELAY_OFF);
  }

  Serial.println("\nMembuat hotspot...");
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(local_IP, gateway, subnet);
  if (!WiFi.softAP(apSsid, apPassword, apChannel, 0, apMaxClient)) {
    Serial.println("Gagal membuat hotspot!");
  }

  Serial.print("Hotspot aktif: ");
  Serial.println(apSsid);
  Serial.print("Buka Browser: http://");
  Serial.println(WiFi.softAPIP());

  setupOTA();

  server.on("/", handleRoot);
  server.on("/set", handleRelay);
  server.on("/status", sendStatus);
  server.on("/alloff", handleAllOff);
  server.on("/auto", handleAuto);
  server.on("/steps", sendSteps);
  server.onNotFound([]() { server.send(404, "text/plain", "Tidak ditemukan"); });

  server.begin();
}

void loop() {
  ArduinoOTA.handle();
  server.handleClient();
  autoTick();

  // Pengaman: matikan relay mode tahan yang sinyalnya sudah berhenti datang
  for (int i = 0; i < NUM_RELAY; i++) {
    if (holdUntil[i] && (long)(millis() - holdUntil[i]) >= 0) {
      setRelay(i, false);
    }
  }
}

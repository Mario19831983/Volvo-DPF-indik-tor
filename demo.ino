#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <SPI.h>
#include <ACAN2515.h>
#include <math.h>

// ===================== ESP32-C3 Super Mini pins =====================
static const byte PIN_SCK  = 4;
static const byte PIN_MISO = 5;
static const byte PIN_MOSI = 6;
static const byte PIN_CS   = 7;
static const byte PIN_INT  = 3;

// ===================== WiFi AP =====================
String apSsid = "MY_VOLVO";
String apPass = "12345678";
static const int AP_CHANNEL  = 1;
static const int AP_MAX_CONN = 4;

WebServer server(80);
Preferences prefs;

// ===================== SPI + CAN =====================
SPIClass spi(FSPI);
ACAN2515 can(PIN_CS, spi, PIN_INT);

static const uint32_t SPI_CLOCK    = 125000;
static const uint32_t MCP2515_XTAL = 8000000;
static const uint32_t CAN_BITRATE  = 500000;

// ===================== UDS IDs =====================
static const uint32_t UDS_REQ_ID  = 0x7E0;
static const uint32_t UDS_RESP_LO = 0x7E8;
static const uint32_t UDS_RESP_HI = 0x7EF;

// ===================== DIDs =====================
static const uint16_t DID_RPM      = 0xF40C; // raw / 4 = rpm
static const uint16_t DID_EXT_TEMP = 0xF446; // raw - 40 = °C

// ===================== Polling =====================
static const uint32_t MIN_GAP_MS      = 120;
static const uint32_t RESP_TIMEOUT_MS = 350;
static const uint16_t POLL_DIDS[] = { DID_RPM, DID_EXT_TEMP };
static const uint8_t  POLL_N = sizeof(POLL_DIDS) / sizeof(POLL_DIDS[0]);

static uint8_t  pollIdx = 0;
static bool     awaitingResp = false;
static uint16_t awaitingDid  = 0;
static uint32_t sentAtMs     = 0;
static uint32_t lastSendMs   = 0;

// ===================== RX counters =====================
static volatile uint32_t rxTotal = 0;
static volatile uint32_t txTotal = 0;
static volatile uint32_t errTotal = 0;

static uint32_t lastRateMs = 0;
static uint32_t lastRxSnap = 0;
static uint32_t rxPerSec   = 0;

// ===================== Values =====================
static float    engineRpm = NAN;
static uint16_t engineRpmRaw = 0;
static uint32_t engineRpmMs = 0;

static float    extTempC = NAN;
static uint8_t  extTempRaw = 0;
static uint32_t extTempMs = 0;

// ===================== Helpers =====================
static String htmlEscape(const String &s) {
  String o;
  o.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '&') o += "&amp;";
    else if (c == '<') o += "&lt;";
    else if (c == '>') o += "&gt;";
    else if (c == '"') o += "&quot;";
    else if (c == '\'') o += "&#39;";
    else o += c;
  }
  return o;
}

static bool parseUdsSF62(const CANMessage &msg, uint16_t &did, uint8_t &d0, uint8_t &d1, uint8_t &dataLen) {
  dataLen = 0;
  if (msg.ext) return false;
  if (msg.len != 8) return false;

  const uint8_t pci = msg.data[0];
  if ((pci & 0xF0) != 0x00) return false;
  const uint8_t plen = (pci & 0x0F);
  if (plen < 3) return false;

  const uint8_t sid = msg.data[1];
  if (sid != 0x62) return false;

  did = ((uint16_t)msg.data[2] << 8) | msg.data[3];
  dataLen = plen - 3;
  d0 = msg.data[4];
  d1 = msg.data[5];
  return true;
}

// ===================== CAN ISR =====================
void IRAM_ATTR onMcpInt() { can.isr(); }

// ===================== CAN pump =====================
static void pumpCAN() {
  CANMessage msg;
  while (can.receive(msg)) {
    rxTotal++;

    if (!msg.ext && msg.id >= UDS_RESP_LO && msg.id <= UDS_RESP_HI) {
      uint16_t did;
      uint8_t a, b, n;
      if (parseUdsSF62(msg, did, a, b, n)) {

        if (did == DID_RPM && n >= 2) {
          const uint16_t rawU16 = ((uint16_t)a << 8) | b;
          engineRpmRaw = rawU16;
          engineRpm    = rawU16 / 4.0f;
          engineRpmMs  = millis();
        }
        else if (did == DID_EXT_TEMP && n >= 1) {
          extTempRaw = a;
          extTempC   = ((int)extTempRaw) - 40;
          extTempMs  = millis();
        }

        if (awaitingResp && did == awaitingDid) {
          awaitingResp = false;
          pollIdx = (pollIdx + 1) % POLL_N;
        }
      }
    }
  }
}

// ===================== UDS request send =====================
static bool udsReadDid(uint16_t did) {
  CANMessage m;
  m.id = UDS_REQ_ID;
  m.ext = false;
  m.rtr = false;
  m.len = 8;
  m.data[0] = 0x03;
  m.data[1] = 0x22;
  m.data[2] = (uint8_t)(did >> 8);
  m.data[3] = (uint8_t)(did & 0xFF);
  for (int i=4;i<8;i++) m.data[i] = 0x00;

  const bool ok = can.tryToSend(m);
  if (ok) txTotal++;
  else    errTotal++;
  return ok;
}

// ===================== Web: /status =====================
static void handleStatus() {
  const uint32_t now = millis();
  auto ageOrNull = [&](uint32_t t)->String {
    if (t == 0) return "null";
    return String((uint32_t)(now - t));
  };

  String j = "{";
  j += "\"rx\":" + String((uint32_t)rxTotal) + ",";
  j += "\"tx\":" + String((uint32_t)txTotal) + ",";
  j += "\"err\":" + String((uint32_t)errTotal) + ",";
  j += "\"rxps\":" + String(rxPerSec) + ",";
  j += "\"sta\":" + String(WiFi.softAPgetStationNum()) + ",";

  j += "\"rpm\":" + String(isnan(engineRpm) ? "null" : String(engineRpm,0)) + ",";
  j += "\"rpmAge\":" + ageOrNull(engineRpmMs) + ",";

  j += "\"extTempC\":" + String(isnan(extTempC) ? "null" : String(extTempC,1)) + ",";
  j += "\"extTempAge\":" + ageOrNull(extTempMs) + ",";

  j += "\"apSsid\":\"" + htmlEscape(apSsid) + "\"";
  j += "}";

  server.send(200, "application/json", j);
}

// ===================== Web: /setap =====================
static void handleSetAp() {
  if (!server.hasArg("ssid") || !server.hasArg("pass")) {
    server.send(400, "text/plain; charset=utf-8", "Missing ssid or pass");
    return;
  }

  String newSsid = server.arg("ssid");
  String newPass = server.arg("pass");

  newSsid.trim();
  newPass.trim();

  if (newSsid.length() < 1 || newSsid.length() > 31) {
    server.send(400, "text/plain; charset=utf-8", "SSID must be 1 to 31 characters");
    return;
  }

  if (newPass.length() < 8 || newPass.length() > 63) {
    server.send(400, "text/plain; charset=utf-8", "Password must be 8 to 63 characters");
    return;
  }

  prefs.putString("apSsid", newSsid);
  prefs.putString("apPass", newPass);

  server.send(200, "text/plain; charset=utf-8", "AP saved, restarting...");
  delay(800);
  ESP.restart();
}

// ===================== Web: /manifest.json =====================
static void handleManifest() {
  String j = R"JSON(
{
  "name": "MY VOLVO",
  "short_name": "VOLVO",
  "start_url": "/phone",
  "display": "standalone",
  "background_color": "#0b0f14",
  "theme_color": "#0b0f14"
}
)JSON";
  server.send(200, "application/manifest+json; charset=utf-8", j);
}

// ===================== UI (Main) =====================
static String mainHtml() {
  String h;
  h += "<!doctype html><html><head><meta charset='utf-8'/>";
  h += "<meta name='viewport' content='width=device-width, initial-scale=1'/>";
  h += "<title>Volvo Monitor</title>";
  h += R"HTML(
<style>
body{margin:0;background:#0b0f14;color:#e6edf3;font-family:system-ui,-apple-system,Segoe UI,Roboto,Arial}
.wrap{max-width:820px;margin:14px auto;padding:0 12px}
.card{border:1px solid #1f2a3a;border-radius:16px;background:#0f1622;padding:14px;margin:10px 0}
.k{color:#9fb0c0;font-size:12px;text-transform:uppercase;letter-spacing:.6px}
.v{font-size:34px;font-weight:900;margin-top:6px}
.sub{margin-top:8px;color:#9fb0c0;font-family:ui-monospace,Menlo,Consolas,monospace;font-size:12px;white-space:pre-wrap}
.row{display:grid;grid-template-columns:1fr 1fr;gap:10px}
input{width:100%;font-size:16px;padding:10px;border-radius:12px;border:1px solid #243244;background:#0b0f14;color:#e6edf3;box-sizing:border-box}
button{width:100%;font-size:18px;padding:12px;border:0;border-radius:12px;background:#49b3ff;color:#001018;font-weight:800}
.badge{display:inline-flex;align-items:center;gap:8px;padding:6px 10px;border-radius:999px;border:1px solid rgba(73,179,255,.25);background:rgba(73,179,255,.08);font-size:12px}
.dot{width:10px;height:10px;border-radius:50%}
.dot.ok{background:#33d17a} .dot.warn{background:#ffcc66}
</style>
</head><body><div class='wrap'>
<div class='card'>
  <div style='display:flex;justify-content:space-between;align-items:center;gap:10px;flex-wrap:wrap'>
    <div>
      <div class='k'>MY VOLVO</div>
      <div style='font-size:16px;font-weight:800'>VOLVO XC60 • LIVE</div>
    </div>
    <div style='display:flex;align-items:center;gap:10px'>
      <button type='button' onclick='location.href="/phone"' style='width:auto;padding:10px 16px;font-size:16px'>Phone</button>
      <div class='badge'><span class='dot warn' id='dot'></span><span id='alive'>Waiting</span></div>
    </div>
  </div>
</div>

<div class='row'>
  <div class='card'>
    <div class='k'>Engine RPM</div>
    <div class='v' id='rpm'>--</div>
    <div class='sub' id='rpmAge'>--</div>
  </div>

  <div class='card'>
    <div class='k'>Outside Temperature</div>
    <div class='v' id='ext'>--</div>
    <div class='sub' id='extAge'>--</div>
  </div>
</div>

<div class='card'>
  <div class='k'>AP Settings</div>

  <div class='row'>
    <div>
      <div class='sub'>SSID</div>
      <input id='apSsid' type='text' maxlength='31'>
    </div>
    <div>
      <div class='sub'>Password</div>
      <input id='apPass' type='text' minlength='8' maxlength='63'>
    </div>
  </div>

  <div class='sub'>Password must be at least 8 characters. The device will restart after saving.</div>
  <div style='height:10px'></div>
  <button onclick='saveAp()'>Save AP Settings</button>
  <div class='sub' id='apMsg'></div>
</div>

<div class='card'>
  <div class='k'>Bus</div>
  <div class='sub' id='bus'>--</div>
</div>

<script>
let autoRefresh = true;

function age(ms){
  if(ms===null) return '--';
  return (ms/1000).toFixed(2)+' s';
}

async function tick(){
  if(!autoRefresh) return;

  try{
    const r = await fetch('/status',{cache:'no-store'});
    const j = await r.json();

    document.getElementById('dot').className='dot ok';
    document.getElementById('alive').textContent='Live';

    document.getElementById('rpm').textContent = (j.rpm==null)?'--':(Number(j.rpm).toFixed(0)+' rpm');
    document.getElementById('rpmAge').textContent = 'age: ' + age(j.rpmAge);

    document.getElementById('ext').textContent = (j.extTempC==null)?'--':(Number(j.extTempC).toFixed(1)+' °C');
    document.getElementById('extAge').textContent = 'age: ' + age(j.extTempAge);

    if(document.activeElement !== document.getElementById('apSsid')) {
      document.getElementById('apSsid').value = j.apSsid || '';
    }

    document.getElementById('bus').textContent =
      'RX: '+j.rx+'  TX: '+j.tx+'  ERR: '+j.err+'  RX/s: '+j.rxps+'  STA: '+j.sta;

  } catch(e) {
    document.getElementById('dot').className='dot warn';
    document.getElementById('alive').textContent='Waiting';
  }
}

function stopRefresh(){
  autoRefresh = false;
  document.getElementById('alive').textContent='Edit mode';
}

function startRefresh(){
  autoRefresh = true;
  document.getElementById('alive').textContent='Live';
  tick();
}

async function saveAp(){
  const ssid = document.getElementById('apSsid').value.trim();
  const pass = document.getElementById('apPass').value.trim();

  if(ssid.length < 1 || ssid.length > 31){
    document.getElementById('apMsg').textContent = 'SSID must be 1 to 31 characters';
    return;
  }

  if(pass.length < 8){
    document.getElementById('apMsg').textContent = 'Password must be at least 8 characters';
    return;
  }

  const qs = new URLSearchParams({ ssid, pass });
  const r = await fetch('/setap?'+qs.toString());
  document.getElementById('apMsg').textContent = await r.text();
}

window.addEventListener("load", () => {
  const inputs = document.querySelectorAll("input");
  inputs.forEach(el => {
    el.addEventListener("focus", stopRefresh);
    el.addEventListener("change", stopRefresh);
  });
});

setInterval(tick, 2000);
tick();
</script>
</div></body></html>
)HTML";
  return h;
}

// ===================== UI (Phone) =====================
static String phoneHtml() {
  String h;
  h += "<!doctype html><html><head><meta charset='utf-8'/>";
  h += "<meta name='viewport' content='width=device-width, initial-scale=1, viewport-fit=cover, user-scalable=no'/>";
  h += "<meta name='apple-mobile-web-app-capable' content='yes'/>";
  h += "<meta name='apple-mobile-web-app-status-bar-style' content='black-translucent'/>";
  h += "<meta name='apple-mobile-web-app-title' content='MY VOLVO'/>";
  h += "<meta name='theme-color' content='#0b0f14'/>";
  h += "<link rel='manifest' href='/manifest.json'/>";
  h += "<title>Volvo Phone</title>";
  h += R"HTML(
<style>
html,body{
  margin:0;
  padding:0;
  background:#0b0f14;
  color:#e6edf3;
  font-family:system-ui,-apple-system,Segoe UI,Roboto,Arial;
  min-height:100%;
  overscroll-behavior:none;
}
body{
  padding-top:env(safe-area-inset-top);
  padding-bottom:env(safe-area-inset-bottom);
  padding-left:env(safe-area-inset-left);
  padding-right:env(safe-area-inset-right);
}
.wrap{
  max-width:720px;
  margin:0 auto;
  padding:10px 12px 18px 12px;
}
.card{border:1px solid #1f2a3a;border-radius:16px;background:#0f1622;padding:14px;margin:10px 0}
.k{color:#9fb0c0;font-size:12px;text-transform:uppercase;letter-spacing:.6px}
.v{font-size:34px;font-weight:900;margin-top:6px}
.sub{margin-top:8px;color:#9fb0c0;font-family:ui-monospace,Menlo,Consolas,monospace;font-size:12px;white-space:pre-wrap}
.badge{display:inline-flex;align-items:center;gap:8px;padding:6px 10px;border-radius:999px;border:1px solid rgba(73,179,255,.25);background:rgba(73,179,255,.08);font-size:12px}
.dot{width:10px;height:10px;border-radius:50%}
.dot.ok{background:#33d17a} .dot.warn{background:#ffcc66}
.btn{
  display:inline-block;
  width:100%;
  padding:12px 12px;
  border-radius:12px;
  background:#49b3ff;
  color:#001018;
  font-weight:800;
  border:0;
  font-size:16px;
}
</style>
</head><body><div class='wrap'>

<div class='card'>
  <div style='display:flex;justify-content:space-between;align-items:center'>
    <div class='k'>Engine RPM</div>
    <div class='badge'><span class='dot warn' id='dot'></span><span id='alive'>Waiting</span></div>
  </div>
  <div class='v' id='rpm'>--</div>
  <div class='sub' id='rpmAge'>--</div>
</div>

<div class='card'>
  <div class='k'>Outside Temperature</div>
  <div class='v' id='ext'>--</div>
  <div class='sub' id='extAge'>--</div>
</div>

<div class='card'>
  <button class='btn' onclick='location.href="/main"'>Back</button>
</div>

<script>
function age(ms){
  if(ms===null) return '--';
  return (ms/1000).toFixed(2)+' s';
}

async function tick(){
  try{
    const r = await fetch('/status',{cache:'no-store'});
    const j = await r.json();

    document.getElementById('dot').className='dot ok';
    document.getElementById('alive').textContent='Live';

    document.getElementById('rpm').textContent =
      (j.rpm==null)?'--':(Number(j.rpm).toFixed(0)+' rpm');
    document.getElementById('rpmAge').textContent =
      'age: ' + age(j.rpmAge);

    document.getElementById('ext').textContent =
      (j.extTempC==null)?'--':(Number(j.extTempC).toFixed(1)+' °C');
    document.getElementById('extAge').textContent =
      'age: ' + age(j.extTempAge);

  } catch(e) {
    document.getElementById('dot').className='dot warn';
    document.getElementById('alive').textContent='Waiting';
  }
}

window.addEventListener('load', () => {
  tick();
});

setInterval(tick, 1000);
</script>
</div></body></html>
)HTML";
  return h;
}

static void handleRoot() {
  server.sendHeader("Location", "/main", true);
  server.send(302, "text/plain", "");
}

static void handleMain() {
  server.send(200, "text/html; charset=utf-8", mainHtml());
}

static void handlePhone() {
  server.send(200, "text/html; charset=utf-8", phoneHtml());
}

// ===================== Setup / Loop =====================
void setup() {
  Serial.begin(115200);
  delay(300);

  prefs.begin("dpfmon", false);

  apSsid = prefs.getString("apSsid", apSsid);
  apPass = prefs.getString("apPass", apPass);

  pinMode(PIN_INT, INPUT_PULLUP);

  spi.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CS);
  spi.setFrequency(SPI_CLOCK);
  spi.setDataMode(SPI_MODE0);

  Serial.println("ESP32-C3 + ACAN2515 monitor starting...");

  ACAN2515Settings settings(MCP2515_XTAL, CAN_BITRATE);
  settings.mRequestedMode = ACAN2515Settings::NormalMode;

  const uint16_t errorCode = can.begin(settings, [] { can.isr(); });
  if (errorCode == 0) {
    Serial.println("CAN init OK (NORMAL MODE)");
  } else {
    Serial.print("CAN init ERROR: ");
    Serial.println(errorCode);
    while (true) delay(1000);
  }

  attachInterrupt(digitalPinToInterrupt(PIN_INT), onMcpInt, FALLING);

  WiFi.persistent(false);
  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);

  bool apOk = WiFi.softAP(apSsid.c_str(), apPass.c_str(), AP_CHANNEL, 0, AP_MAX_CONN);
  delay(200);

  Serial.print("softAP ok: ");
  Serial.println(apOk ? "YES" : "NO");
  Serial.print("AP SSID: ");
  Serial.println(WiFi.softAPSSID());
  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());
  Serial.println("Main:  http://192.168.4.1/main");
  Serial.println("Phone: http://192.168.4.1/phone");

  server.on("/", handleRoot);
  server.on("/main", handleMain);
  server.on("/phone", handlePhone);
  server.on("/status", handleStatus);
  server.on("/setap", handleSetAp);
  server.on("/manifest.json", handleManifest);
  server.begin();
}

void loop() {
  server.handleClient();
  pumpCAN();

  const uint32_t now = millis();

  if (awaitingResp && (now - sentAtMs >= RESP_TIMEOUT_MS)) {
    awaitingResp = false;
    pollIdx = (pollIdx + 1) % POLL_N;
  }

  if (!awaitingResp && (now - lastSendMs >= MIN_GAP_MS)) {
    const uint16_t did = POLL_DIDS[pollIdx];
    const bool ok = udsReadDid(did);
    lastSendMs = now;
    if (ok) {
      awaitingResp = true;
      awaitingDid  = did;
      sentAtMs     = now;
    }
  }

  if (now - lastRateMs >= 1000) {
    lastRateMs = now;
    uint32_t cur = (uint32_t)rxTotal;
    rxPerSec = cur - lastRxSnap;
    lastRxSnap = cur;
  }
}
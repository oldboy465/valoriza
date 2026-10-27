/*
 * ============================================================================
 * PROJETO: SALA PROJETO VALORIZA - SERVIDOR DE FREQUENCIA E PONTO ELETRONICO
 * INSTITUICAO: UNIVERSIDADE ESTADUAL DO MARANHAO (UEMA)
 * AUTOR: Philipe Sampaio Lima
 * HARDWARE: ESP32 DevKit V1 + Display OLED 0.96" SSD1306 I2C (128x64 Bicolor)
 * VERSAO: 2.2 (Sincronismo de Relógio do Dispositivo + Efeito Chuva e Sleep)
 * ============================================================================
 */

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <esp_wifi.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <time.h>
#include <sys/time.h>

// ----------------------------------------------------------------------------
// CONFIGURACOES DE HARDWARE & OLED
// ----------------------------------------------------------------------------
#define SCREEN_WIDTH      128
#define SCREEN_HEIGHT     64
#define OLED_RESET        -1
#define OLED_I2C_ADDRESS  0x3C
#define PIN_SDA           21
#define PIN_SCL           22

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ----------------------------------------------------------------------------
// REDE WI-FI E PORTAL CATIVO
// ----------------------------------------------------------------------------
const char* AP_SSID = "atendimento-esp32";
const IPAddress AP_IP(192, 168, 4, 1);
const IPAddress AP_NETMASK(255, 255, 255, 0);
const byte DNS_PORT = 53;

DNSServer dnsServer;
WebServer server(80);

const char* ADMIN_PASSWORD = "465465";

// ----------------------------------------------------------------------------
// GEOMETRIA DA AREA DE ASSINATURA (FAIXA AZUL: 128 x 48)
// ----------------------------------------------------------------------------
#define CANVAS_W 128
#define CANVAS_H 48
#define CANVAS_BYTES (CANVAS_W * CANVAS_H / 8) // 768 bytes

// ----------------------------------------------------------------------------
// BANCO DE DADOS EM MEMORIA (ESTRUTURA RLE COMPACTA)
// ----------------------------------------------------------------------------
#define MAX_RECORDS 60

struct AttendanceRecord {
  uint16_t id;
  char name[48];
  char timestamp[24];
  uint16_t rleSize;
  uint8_t* rleData;
};

AttendanceRecord* records[MAX_RECORDS];
uint16_t recordCount = 0;
bool clockSynced = false;

// Controle da Rotina
bool routineActive = false;
char routineOpeningTime[24] = "FECHADO";

// Buffer de desenho ativo
uint8_t liveCanvas[CANVAS_BYTES];
char currentSignerName[48] = "";

// ----------------------------------------------------------------------------
// CONTROLE DE OCIOSIDADE & GERENCIAMENTO DE ENERGIA DO DISPLAY
// ----------------------------------------------------------------------------
uint32_t lastActivityTime = 0;
bool displaySleeping = false;

// Efeito Chuva (Screensaver de 30s)
#define NUM_DROPS 24
struct RainDrop {
  int8_t x;
  int8_t y;
  uint8_t speed;
  uint8_t len;
};
RainDrop drops[NUM_DROPS];

void initRain() {
  for (int i = 0; i < NUM_DROPS; i++) {
    drops[i].x = random(0, SCREEN_WIDTH);
    drops[i].y = random(-20, SCREEN_HEIGHT);
    drops[i].speed = random(2, 5);
    drops[i].len = random(3, 8);
  }
}

// ----------------------------------------------------------------------------
// MAQUINA DE ESTADOS DO OLED E CARROSSEL
// ----------------------------------------------------------------------------
enum OledMode {
  MODE_CAROUSEL,
  MODE_DRAWING,
  MODE_ANIMATION_WHIP,
  MODE_FLASH_PHILIPE,
  MODE_RAIN
};
OledMode currentOledMode = MODE_CAROUSEL;

uint8_t carouselIndex = 0;
uint32_t lastCarouselTick = 0;
uint32_t animStartTime = 0;
uint32_t lastOledFrameTime = 0;

// Faixa Amarela Superior (y = 0 a 15)
float marqueeX = SCREEN_WIDTH;
char marqueeMsg[160] = "UEMA - PROJETO VALORIZA // valoriza.local // ";

// ----------------------------------------------------------------------------
// DESPERTAR E RESET DE ATIVIDADE
// ----------------------------------------------------------------------------
void resetActivity() {
  lastActivityTime = millis();
  if (displaySleeping) {
    displaySleeping = false;
    display.ssd1306_command(SSD1306_DISPLAYON);
  }
  if (currentOledMode == MODE_RAIN) {
    currentOledMode = MODE_CAROUSEL;
    lastCarouselTick = millis();
  }
}

// ----------------------------------------------------------------------------
// COMPRESSAO RLE (BYTE-LEVEL RUN-LENGTH ENCODING)
// ----------------------------------------------------------------------------
uint16_t compressRLE(const uint8_t* in, uint16_t inLen, uint8_t* out) {
  uint16_t outIdx = 0;
  uint16_t i = 0;
  while (i < inLen) {
    uint8_t val = in[i];
    uint8_t count = 1;
    while ((i + 1) < inLen && in[i + 1] == val && count < 255) {
      count++;
      i++;
    }
    out[outIdx++] = count;
    out[outIdx++] = val;
    i++;
  }
  return outIdx;
}

// ----------------------------------------------------------------------------
// SINCRONIZADOR DE RELOGIO RTC A PARTIR DO CLIENTE (ISO 8601)
// ----------------------------------------------------------------------------
void syncRtcFromISO(const String& iso) {
  if (iso.length() >= 19) {
    int yr = iso.substring(0, 4).toInt();
    int mo = iso.substring(5, 7).toInt();
    int dy = iso.substring(8, 10).toInt();
    int hr = iso.substring(11, 13).toInt();
    int mn = iso.substring(14, 16).toInt();
    int sc = iso.substring(17, 19).toInt();

    struct tm tm_in;
    tm_in.tm_year = yr - 1900;
    tm_in.tm_mon = mo - 1;
    tm_in.tm_mday = dy;
    tm_in.tm_hour = hr;
    tm_in.tm_min = mn;
    tm_in.tm_sec = sc;
    tm_in.tm_isdst = 0;

    time_t t = mktime(&tm_in);
    struct timeval nowTv = { .tv_sec = t, .tv_usec = 0 };
    settimeofday(&nowTv, NULL);
    clockSynced = true;
  }
}

// ----------------------------------------------------------------------------
// INTERFACE WEB EMBARCADA
// ----------------------------------------------------------------------------
const char PAGE_INDEX[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html lang="pt-BR">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no">
<meta name="mobile-web-app-capable" content="yes">
<meta name="apple-mobile-web-app-capable" content="yes">
<title>UEMA - Projeto Valoriza</title>
<style>
:root {
  --uema-navy: #002B49;
  --uema-blue: #0055A5;
  --uema-light: #EBF3FA;
  --uema-accent: #D9822B;
  --uema-green: #2E7D32;
  --uema-red: #C62828;
  --text-dark: #1C252C;
  --bg-gray: #F4F6F9;
  --border-color: #CCD6E0;
}
html, body {
  overscroll-behavior: none !important;
  overscroll-behavior-y: none !important;
  -webkit-overscroll-behavior: none !important;
  background-color: var(--bg-gray);
  color: var(--text-dark);
  font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
  margin: 0;
  padding: 0;
  padding-bottom: 30px;
  touch-action: pan-y;
}
* { box-sizing: border-box; -webkit-tap-highlight-color: transparent; }
header {
  background: linear-gradient(135deg, var(--uema-navy), var(--uema-blue));
  color: white;
  padding: 14px 18px;
  box-shadow: 0 2px 6px rgba(0,0,0,0.15);
  display: flex;
  justify-content: space-between;
  align-items: center;
}
.header-titles h1 { font-size: 16px; font-weight: 700; text-transform: uppercase; margin: 0; }
.header-titles p { font-size: 11px; opacity: 0.85; margin: 2px 0 0 0; }
.btn-admin-nav {
  background: rgba(255,255,255,0.18);
  border: 1px solid rgba(255,255,255,0.4);
  color: white;
  padding: 6px 12px;
  font-size: 11px;
  font-weight: 600;
  border-radius: 4px;
  cursor: pointer;
}
.container { max-width: 480px; margin: 12px auto; padding: 0 12px; }
.card {
  background: white;
  border-radius: 8px;
  border: 1px solid var(--border-color);
  padding: 16px;
  margin-bottom: 12px;
  box-shadow: 0 1px 3px rgba(0,0,0,0.05);
}
.card-title {
  font-size: 13px;
  font-weight: 700;
  color: var(--uema-navy);
  border-bottom: 2px solid var(--uema-light);
  padding-bottom: 8px;
  margin-bottom: 12px;
  text-transform: uppercase;
  display: flex;
  justify-content: space-between;
}
.alert-closed {
  background: #FFF3CD;
  color: #856404;
  border: 1px solid #FFEEBA;
  padding: 14px;
  border-radius: 6px;
  font-size: 13px;
  font-weight: 600;
  text-align: center;
  line-height: 1.4;
}
label { font-size: 13px; font-weight: 600; color: #4A5568; margin-bottom: 6px; display: block; }
input[type="text"], input[type="password"] {
  width: 100%;
  padding: 12px;
  font-size: 15px;
  border: 1.5px solid var(--border-color);
  border-radius: 6px;
  outline: none;
}
input[type="text"]:focus, input[type="password"]:focus {
  border-color: var(--uema-blue);
  box-shadow: 0 0 0 3px rgba(0,85,165,0.15);
}

.canvas-wrapper {
  position: relative;
  width: 100%;
  border: 2px dashed var(--uema-blue);
  border-radius: 6px;
  background: #FFFFFF;
  touch-action: none !important;
  overscroll-behavior: none !important;
  overscroll-behavior-y: none !important;
  -webkit-overscroll-behavior: none !important;
  user-select: none !important;
  -webkit-user-select: none !important;
  overflow: hidden;
  margin-top: 6px;
}
canvas {
  display: block;
  width: 100%;
  height: 160px;
  background: #FFFFFF;
  cursor: crosshair;
  touch-action: none !important;
  overscroll-behavior: none !important;
  overscroll-behavior-y: none !important;
  -webkit-overscroll-behavior: none !important;
}
.canvas-hint {
  position: absolute;
  top: 50%;
  left: 50%;
  transform: translate(-50%, -50%);
  color: #A0AEC0;
  font-size: 13px;
  pointer-events: none;
  font-weight: 500;
}
.btn {
  width: 100%;
  padding: 12px;
  font-size: 13px;
  font-weight: 700;
  border-radius: 6px;
  border: none;
  cursor: pointer;
  text-transform: uppercase;
  letter-spacing: 0.5px;
  transition: 0.15s;
}
.btn-primary { background: var(--uema-blue); color: white; margin-top: 10px; }
.btn-secondary { background: white; border: 1px solid var(--border-color); color: var(--text-dark); margin-top: 8px; }
.btn-success { background: var(--uema-green); color: white; margin-top: 8px; }
.btn-danger { background: var(--uema-red); color: white; margin-top: 8px; }
.btn-warning { background: var(--uema-accent); color: white; margin-top: 8px; }

.table-records {
  width: 100%;
  border-collapse: collapse;
  font-size: 12px;
  margin-top: 8px;
}
.table-records th {
  background: var(--uema-light);
  color: var(--uema-navy);
  text-align: left;
  padding: 8px;
  border-bottom: 2px solid var(--border-color);
}
.table-records td {
  padding: 8px;
  border-bottom: 1px solid #E2E8F0;
}
.btn-del-sm {
  background: var(--uema-red);
  color: white;
  border: none;
  padding: 4px 8px;
  border-radius: 4px;
  font-size: 11px;
  cursor: pointer;
}
.footer-text { text-align: center; font-size: 11px; color: #718096; margin-top: 20px; line-height: 1.5; }
#toast {
  position: fixed;
  bottom: 20px;
  left: 50%;
  transform: translateX(-50%);
  background: #1C252C;
  color: white;
  padding: 10px 18px;
  border-radius: 20px;
  font-size: 13px;
  opacity: 0;
  transition: 0.3s;
  pointer-events: none;
  z-index: 999;
}
</style>
</head>
<body>
<header>
  <div class="header-titles">
    <h1>PROJETO VALORIZA</h1>
    <p>Universidade Estadual do Maranhão - UEMA</p>
  </div>
  <button class="btn-admin-nav" onclick="toggleAdminPanel()">Área do Atendente</button>
</header>

<div class="container">
  <!-- PAINEL ADMINISTRATIVO -->
  <div class="card" id="admin-panel" style="display:none; border: 2px solid var(--uema-accent);">
    <div class="card-title">
      <span>Controle do Atendente</span>
      <span style="color:var(--uema-accent)" id="adm-session-tag">Autenticação</span>
    </div>

    <div id="admin-auth">
      <label for="admin-pwd">Código de Segurança:</label>
      <input type="password" id="admin-pwd" autocomplete="off" placeholder="Digite a senha...">
      <button class="btn btn-primary" onclick="loginAdmin()">Acessar Painel</button>
    </div>

    <div id="admin-view" style="display:none;">
      <div style="background:var(--uema-light); padding:10px; border-radius:6px; margin-bottom:10px; font-size:12px;">
        <div><b>Status da Rotina:</b> <span id="adm-rot-status">--</span></div>
        <div><b>Horário Abertura:</b> <span id="adm-rot-time">--</span></div>
        <div><b>Assinaturas Registradas:</b> <span id="adm-count">0</span> / 60</div>
      </div>

      <!-- BOTAO DICOTOMICO -->
      <div id="adm-routine-action-container">
        <button id="btn-routine-start" class="btn btn-success" style="display:none;" onclick="adminSetRoutine(1)">Iniciar Rotina</button>
        <button id="btn-routine-stop" class="btn btn-warning" style="display:none;" onclick="adminSetRoutine(0)">Encerrar Rotina</button>
      </div>

      <button class="btn btn-primary" style="background:#2E7D32; margin-top:8px;" onclick="exportJSON()">Exportar Dados (JSON RLE)</button>
      <button class="btn btn-danger" onclick="clearEntireDB()">Limpar Banco de Dados</button>

      <div style="margin-top:14px;">
        <div style="font-weight:700; font-size:13px; margin-bottom:6px; color:var(--uema-navy);">Frequências de Hoje:</div>
        <div style="max-height:180px; overflow-y:auto; border:1px solid #E2E8F0; border-radius:4px;">
          <table class="table-records">
            <thead>
              <tr>
                <th>#</th>
                <th>Nome</th>
                <th>Hora</th>
                <th>Ação</th>
              </tr>
            </thead>
            <tbody id="records-table-body">
              <tr><td colspan="4" style="text-align:center;">Carregando...</td></tr>
            </tbody>
          </table>
        </div>
      </div>

      <div style="display:grid; grid-template-columns:1fr 1fr; gap:8px; margin-top:8px;">
        <button class="btn btn-secondary" onclick="hideAdminPanel()">Ocultar Painel</button>
        <button class="btn btn-secondary" style="color:var(--uema-red);" onclick="logoutAdmin()">Encerrar Sessão</button>
      </div>
    </div>
  </div>

  <!-- PAINEL DO USUARIO -->
  <div class="card" id="user-panel">
    <div class="card-title">
      <span>Registro de Frequência</span>
      <span id="usr-tag-id" style="color:var(--uema-accent)">#--</span>
    </div>

    <div id="box-closed" class="alert-closed" style="display:none;">
      <b>ATENDIMENTO ENCERRADO NO MOMENTO</b><br>
      Aguarde o atendente iniciar a rotina para assinar a frequência.
    </div>

    <div id="box-open">
      <label for="att-name">Nome Completo do Usuário:</label>
      <input type="text" id="att-name" placeholder="Digite seu nome aqui..." autocomplete="off">

      <label style="margin-top:12px;">Assinatura Digital:</label>
      <div class="canvas-wrapper" id="canvas-wrapper">
        <div class="canvas-hint" id="canvas-hint">Assine com o dedo aqui</div>
        <canvas id="sign-pad"></canvas>
      </div>

      <button class="btn btn-secondary" onclick="clearPad()">Limpar Traço</button>
      <button class="btn btn-primary" onclick="confirmAttendance()">Confirmar Presença</button>
    </div>
  </div>

  <div class="footer-text">
    Acesso direto: <b>valoriza.local</b> ou <b>192.168.4.1</b><br>
    Desenvolvido por Philipe Sampaio Lima<br>
    Terminal Autônomo UEMA - Otimizado
  </div>
</div>

<div id="toast">Notificação</div>

<script>
const canvas = document.getElementById('sign-pad');
const wrapper = document.getElementById('canvas-wrapper');
const ctx = canvas.getContext('2d');
const hint = document.getElementById('canvas-hint');

let drawing = false;
let strokesQueue = [];
let lastSend = 0;
let strokeTimer = null;
let lastPingTime = 0;

function resizeCanvas() {
  canvas.width = 128;
  canvas.height = 48;
}
window.addEventListener('resize', resizeCanvas);
resizeCanvas();

function showToast(msg) {
  const t = document.getElementById('toast');
  t.innerText = msg;
  t.style.opacity = '1';
  setTimeout(() => t.style.opacity = '0', 2500);
}

function wakeESP32() {
  const now = Date.now();
  if (now - lastPingTime > 2000) {
    lastPingTime = now;
    fetch('/api/wake', {method:'POST'}).catch(()=>{});
  }
}

// ============================================================================
// BLOQUEIO TOTAL DE TECLADO E ANTI-PULL-TO-REFRESH
// ============================================================================
function dismissKeyboard() {
  const nameInput = document.getElementById('att-name');
  if (nameInput) nameInput.blur();
  if (document.activeElement) document.activeElement.blur();
}

function preventTouchScroll(e) {
  dismissKeyboard();
  wakeESP32();
  if (e.cancelable) e.preventDefault();
  e.stopPropagation();
}

['touchstart', 'touchmove', 'touchend', 'touchcancel'].forEach(evt => {
  wrapper.addEventListener(evt, preventTouchScroll, { passive: false });
  canvas.addEventListener(evt, preventTouchScroll, { passive: false });
});

document.addEventListener('touchmove', function(e) {
  if (e.target === canvas || e.target === wrapper || wrapper.contains(e.target)) {
    if (e.cancelable) e.preventDefault();
  }
}, { passive: false });

function getPos(e) {
  const r = canvas.getBoundingClientRect();
  const clientX = e.touches ? e.touches[0].clientX : e.clientX;
  const clientY = e.touches ? e.touches[0].clientY : e.clientY;
  return {
    x: Math.round(((clientX - r.left) / r.width) * 128),
    y: Math.round(((clientY - r.top) / r.height) * 48)
  };
}

function startDraw(e) {
  preventTouchScroll(e);
  drawing = true;
  hint.style.display = 'none';
  clearTimeout(strokeTimer);
  const p = getPos(e);
  ctx.beginPath();
  ctx.moveTo(p.x, p.y);
  addStrokePoint(p.x, p.y, 1);
}

function moveDraw(e) {
  preventTouchScroll(e);
  if (!drawing) return;
  const p = getPos(e);
  ctx.lineWidth = 1.5;
  ctx.lineCap = 'round';
  ctx.strokeStyle = '#0055A5';
  ctx.lineTo(p.x, p.y);
  ctx.stroke();
  addStrokePoint(p.x, p.y, 0);

  const now = Date.now();
  if (now - lastSend > 40 && strokesQueue.length > 0) {
    sendStrokes();
    lastSend = now;
  }
}

function stopDraw(e) {
  preventTouchScroll(e);
  if (!drawing) return;
  drawing = false;
  sendStrokes();

  clearTimeout(strokeTimer);
  strokeTimer = setTimeout(() => {
    const n = document.getElementById('att-name').value.trim() || "USUARIO";
    fetch('/api/finish-stroke?name=' + encodeURIComponent(n), {method:'POST'}).catch(()=>{});
  }, 2500);
}

canvas.addEventListener('touchstart', startDraw, {passive: false});
canvas.addEventListener('touchmove', moveDraw, {passive: false});
canvas.addEventListener('touchend', stopDraw, {passive: false});
canvas.addEventListener('mousedown', startDraw);
canvas.addEventListener('mousemove', moveDraw);
canvas.addEventListener('mouseup', stopDraw);

function addStrokePoint(x, y, isStart) {
  strokesQueue.push({x: Math.max(0, Math.min(127, x)), y: Math.max(0, Math.min(47, y)), s: isStart});
}

function sendStrokes() {
  if (strokesQueue.length === 0) return;
  const payload = JSON.stringify(strokesQueue);
  strokesQueue = [];
  fetch('/api/draw-stroke', {
    method: 'POST',
    headers: {'Content-Type': 'application/json'},
    body: payload
  }).catch(()=>{});
}

function clearPad() {
  ctx.clearRect(0, 0, canvas.width, canvas.height);
  hint.style.display = 'block';
  strokesQueue = [];
  fetch('/api/clear-canvas', {method:'POST'}).catch(()=>{});
}

// ============================================================================
// CONFIRMACAO COM TIMESTAMP REAL DO DISPOSITIVO
// ============================================================================
function confirmAttendance() {
  const nameInput = document.getElementById('att-name');
  const nameVal = nameInput.value.trim();
  if (!nameVal) {
    showToast('Informe o seu nome antes de confirmar.');
    nameInput.focus();
    return;
  }

  // Captura o momento exato do aparelho do usuario
  const now = new Date();
  const pad = (n) => String(n).padStart(2, '0');
  const timeStr = `${now.getFullYear()}-${pad(now.getMonth()+1)}-${pad(now.getDate())} ${pad(now.getHours())}:${pad(now.getMinutes())}:${pad(now.getSeconds())}`;
  const isoStr = now.toISOString();

  const formData = new URLSearchParams();
  formData.append('name', nameVal);
  formData.append('time', timeStr);
  formData.append('iso', isoStr);

  fetch('/api/commit-attendance', {
    method: 'POST',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body: formData.toString()
  })
  .then(r => r.json())
  .then(data => {
    if (data.status === 'OK') {
      showToast('Presença confirmada: #' + data.id);
      nameInput.value = '';
      clearPad();
      fetchStatus();
    } else {
      showToast(data.msg || 'Erro ao registrar.');
    }
  })
  .catch(() => showToast('Falha na comunicação.'));
}

// ============================================================================
// DICOTOMIA DA ROTINA
// ============================================================================
function updateRoutineButtons(isOpen) {
  const btnStart = document.getElementById('btn-routine-start');
  const btnStop = document.getElementById('btn-routine-stop');
  if (isOpen) {
    btnStart.style.display = 'none';
    btnStop.style.display = 'block';
  } else {
    btnStart.style.display = 'block';
    btnStop.style.display = 'none';
  }
}

function fetchStatus() {
  fetch('/api/status')
    .then(r => r.json())
    .then(d => {
      document.getElementById('usr-tag-id').innerText = '#' + (d.count + 1);
      const isOpen = (d.routine === 1);
      document.getElementById('box-open').style.display = isOpen ? 'block' : 'none';
      document.getElementById('box-closed').style.display = isOpen ? 'none' : 'block';

      document.getElementById('adm-rot-status').innerText = isOpen ? 'ABERTA' : 'ENCERRADA';
      document.getElementById('adm-rot-status').style.color = isOpen ? 'var(--uema-green)' : 'var(--uema-red)';
      document.getElementById('adm-rot-time').innerText = d.opening_time || 'FECHADO';
      document.getElementById('adm-count').innerText = d.count;

      updateRoutineButtons(isOpen);
    }).catch(()=>{});
}

// ============================================================================
// CONTROLE DO ADMINISTRADOR
// ============================================================================
function getStoredPwd() {
  return sessionStorage.getItem('uema_admin_token') || '';
}

function setStoredPwd(pwd) {
  sessionStorage.setItem('uema_admin_token', pwd);
  sessionStorage.setItem('uema_admin_active_view', '1');
}

function clearStoredPwd() {
  sessionStorage.removeItem('uema_admin_token');
  sessionStorage.removeItem('uema_admin_active_view');
}

function toggleAdminPanel() {
  wakeESP32();
  const p = document.getElementById('admin-panel');
  if (p.style.display === 'none') {
    p.style.display = 'block';
    sessionStorage.setItem('uema_admin_active_view', '1');
    p.scrollIntoView({ behavior: 'smooth' });
    checkActiveSession();
  } else {
    p.style.display = 'none';
    sessionStorage.removeItem('uema_admin_active_view');
  }
}

function hideAdminPanel() {
  document.getElementById('admin-panel').style.display = 'none';
  sessionStorage.removeItem('uema_admin_active_view');
}

function checkActiveSession() {
  const token = getStoredPwd();
  if (token) {
    document.getElementById('admin-panel').style.display = 'block';
    fetch('/api/admin-auth?pwd=' + encodeURIComponent(token), {method:'POST'})
      .then(r => r.text())
      .then(res => {
        if (res === 'OK') {
          showAdminView();
          loadAdminRecords();
        } else {
          clearStoredPwd();
          showAuthView();
        }
      }).catch(() => {
        showAdminView();
      });
  } else {
    showAuthView();
  }
}

function showAdminView() {
  document.getElementById('admin-panel').style.display = 'block';
  document.getElementById('admin-auth').style.display = 'none';
  document.getElementById('admin-view').style.display = 'block';
  document.getElementById('adm-session-tag').innerText = "Conectado";
  document.getElementById('adm-session-tag').style.color = "var(--uema-green)";
}

function showAuthView() {
  document.getElementById('admin-auth').style.display = 'block';
  document.getElementById('admin-view').style.display = 'none';
  document.getElementById('adm-session-tag').innerText = "Autenticação";
  document.getElementById('adm-session-tag').style.color = "var(--uema-accent)";
  document.getElementById('admin-pwd').value = '';
}

function logoutAdmin() {
  clearStoredPwd();
  hideAdminPanel();
  showAuthView();
  showToast('Sessão encerrada.');
}

function loginAdmin() {
  const p = document.getElementById('admin-pwd').value.trim();
  if (!p) {
    showToast('Informe a senha.');
    return;
  }
  fetch('/api/admin-auth?pwd=' + encodeURIComponent(p), {method:'POST'})
    .then(r => r.text())
    .then(res => {
      if (res === 'OK') {
        setStoredPwd(p);
        showAdminView();
        loadAdminRecords();
        showToast('Acesso concedido.');
      } else {
        showToast('Senha incorreta.');
      }
    }).catch(() => showToast('Falha na comunicação.'));
}

function adminSetRoutine(val) {
  const p = getStoredPwd();
  if (!p) { checkActiveSession(); return; }

  const now = new Date();
  const timeStr = now.toLocaleTimeString();
  const isoStr = now.toISOString();

  fetch('/api/routine?pwd=' + encodeURIComponent(p) + '&active=' + val + '&time=' + encodeURIComponent(timeStr) + '&iso=' + encodeURIComponent(isoStr), {method:'POST'})
    .then(r => r.text())
    .then(res => {
      if (res === 'OK') {
        showToast(val ? 'Rotina iniciada!' : 'Rotina encerrada.');
        fetchStatus();
      } else {
        showToast('Erro: Sessão inválida.');
        checkActiveSession();
      }
    }).catch(() => showToast('Falha ao comunicar com o servidor.'));
}

function loadAdminRecords() {
  const p = getStoredPwd();
  if (!p) return;

  fetch('/api/records-list?pwd=' + encodeURIComponent(p))
    .then(r => r.json())
    .then(list => {
      const tbody = document.getElementById('records-table-body');
      tbody.innerHTML = '';
      if (!list || list.length === 0) {
        tbody.innerHTML = '<tr><td colspan="4" style="text-align:center;">Nenhum registro.</td></tr>';
        return;
      }
      list.forEach(item => {
        const tr = document.createElement('tr');
        
        const tdId = document.createElement('td');
        tdId.textContent = '#' + item.id;
        
        const tdName = document.createElement('td');
        const bName = document.createElement('b');
        bName.textContent = item.name;
        tdName.appendChild(bName);
        
        const tdTime = document.createElement('td');
        tdTime.textContent = item.time;
        
        const tdAction = document.createElement('td');
        const btnDel = document.createElement('button');
        btnDel.className = 'btn-del-sm';
        btnDel.textContent = 'Excluir';
        btnDel.onclick = () => deleteSingleRecord(item.id);
        tdAction.appendChild(btnDel);

        tr.appendChild(tdId);
        tr.appendChild(tdName);
        tr.appendChild(tdTime);
        tr.appendChild(tdAction);
        tbody.appendChild(tr);
      });
    }).catch(()=>{});
}

function deleteSingleRecord(id) {
  if (confirm('Deseja excluir a assinatura #' + id + '?')) {
    const p = getStoredPwd();
    fetch('/api/delete-record?pwd=' + encodeURIComponent(p) + '&id=' + id, {method:'POST'})
      .then(r => r.text())
      .then(res => {
        if (res === 'DELETED') {
          showToast('Registro #' + id + ' removido.');
          loadAdminRecords();
          fetchStatus();
        } else {
          showToast('Falha ao excluir registro.');
        }
      });
  }
}

function clearEntireDB() {
  if (confirm('ATENÇÃO: Deseja apagar TODAS as frequências gravadas na ESP32?')) {
    const p = getStoredPwd();
    fetch('/api/clear-db?pwd=' + encodeURIComponent(p), {method:'POST'})
      .then(r => r.text())
      .then(res => {
        if (res === 'CLEARED') {
          showToast('Banco de dados zerado.');
          loadAdminRecords();
          fetchStatus();
        }
      });
  }
}

function exportJSON() {
  const p = getStoredPwd();
  if (!p) { checkActiveSession(); return; }
  window.location.href = '/api/export-json?pwd=' + encodeURIComponent(p);
}

window.onload = function() {
  fetchStatus();
  setInterval(fetchStatus, 3000);
  if (getStoredPwd()) {
    document.getElementById('admin-panel').style.display = 'block';
    checkActiveSession();
  }
};
</script>
</body>
</html>
)rawhtml";

// ----------------------------------------------------------------------------
// ROTINAS DE DESENHO VETORIAL NO BUFFER LOCAL
// ----------------------------------------------------------------------------
void setCanvasPixel(int x, int y, uint8_t val) {
  if (x < 0 || x >= CANVAS_W || y < 0 || y >= CANVAS_H) return;
  int byteIdx = x + (y / 8) * CANVAS_W;
  int bit = y % 8;
  if (val) liveCanvas[byteIdx] |= (1 << bit);
  else liveCanvas[byteIdx] &= ~(1 << bit);
}

void drawLineOnCanvas(int x0, int y0, int x1, int y1) {
  int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
  int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
  int err = dx + dy, e2;
  while (true) {
    setCanvasPixel(x0, y0, 1);
    if (x0 == x1 && y0 == y1) break;
    e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
}

// ----------------------------------------------------------------------------
// HANDLERS HTTP REST
// ----------------------------------------------------------------------------
void sendCORSHeaders() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
}

void handleOptions() {
  sendCORSHeaders();
  server.send(204);
}

void handleRoot() {
  sendCORSHeaders();
  server.send_P(200, "text/html", PAGE_INDEX);
}

void handleWake() {
  resetActivity();
  sendCORSHeaders();
  server.send(200, "text/plain", "WAKE_OK");
}

void handleAdminAuth() {
  resetActivity();
  sendCORSHeaders();
  if (server.arg("pwd") == ADMIN_PASSWORD) {
    server.send(200, "text/plain", "OK");
  } else {
    server.send(403, "text/plain", "DENIED");
  }
}

int prevX = -1, prevY = -1;
void handleDrawStroke() {
  resetActivity();
  if (!routineActive) {
    sendCORSHeaders();
    server.send(403, "text/plain", "Rotina encerrada");
    return;
  }

  if (!server.hasArg("plain")) {
    sendCORSHeaders();
    server.send(400, "text/plain", "Sem dados");
    return;
  }
  String body = server.arg("plain");
  currentOledMode = MODE_DRAWING;

  int idx = 0;
  while ((idx = body.indexOf('{', idx)) != -1) {
    int end = body.indexOf('}', idx);
    if (end == -1) break;
    String chunk = body.substring(idx + 1, end);
    int xPos = chunk.indexOf("\"x\":");
    int yPos = chunk.indexOf("\"y\":");
    int sPos = chunk.indexOf("\"s\":");
    if (xPos != -1 && yPos != -1) {
      int x = chunk.substring(xPos + 4, chunk.indexOf(',', xPos)).toInt();
      int y = chunk.substring(yPos + 4, chunk.indexOf(',', yPos)).toInt();
      int s = (sPos != -1) ? chunk.substring(sPos + 4).toInt() : 0;

      x = constrain(x, 0, CANVAS_W - 1);
      y = constrain(y, 0, CANVAS_H - 1);

      if (s == 1 || prevX == -1) {
        setCanvasPixel(x, y, 1);
      } else {
        drawLineOnCanvas(prevX, prevY, x, y);
      }
      prevX = x;
      prevY = y;
    }
    idx = end + 1;
  }
  sendCORSHeaders();
  server.send(200, "text/plain", "OK");
}

void handleClearCanvas() {
  resetActivity();
  memset(liveCanvas, 0, sizeof(liveCanvas));
  prevX = -1;
  prevY = -1;
  currentOledMode = MODE_CAROUSEL;
  sendCORSHeaders();
  server.send(200, "text/plain", "CLEARED");
}

void handleFinishStroke() {
  resetActivity();
  if (server.hasArg("name")) {
    String n = server.arg("name");
    strncpy(currentSignerName, n.c_str(), sizeof(currentSignerName) - 1);
    currentSignerName[sizeof(currentSignerName) - 1] = '\0';
  } else {
    strcpy(currentSignerName, "USUARIO");
  }
  currentOledMode = MODE_ANIMATION_WHIP;
  animStartTime = millis();
  sendCORSHeaders();
  server.send(200, "text/plain", "ANIM_START");
}

void handleCommitAttendance() {
  resetActivity();
  sendCORSHeaders();

  if (!routineActive) {
    server.send(200, "application/json", "{\"status\":\"ERROR\",\"msg\":\"Atendimento fechado.\"}");
    return;
  }

  if (recordCount >= MAX_RECORDS) {
    server.send(200, "application/json", "{\"status\":\"ERROR\",\"msg\":\"Limite de 60 atendimentos atingido.\"}");
    return;
  }

  String name = "";
  if (server.hasArg("name")) {
    name = server.arg("name");
  } else if (server.hasArg("plain")) {
    String body = server.arg("plain");
    int pos = body.indexOf("\"name\":");
    if (pos != -1) {
      int start = body.indexOf('"', pos + 7) + 1;
      int end = body.indexOf('"', start);
      if (start > 0 && end > start) name = body.substring(start, end);
    }
  }

  name.trim();
  if (name.length() == 0) name = "USUARIO";

  // Sincroniza RTC com o ISO do aparelho, se fornecido
  if (server.hasArg("iso")) {
    syncRtcFromISO(server.arg("iso"));
  }

  static uint8_t rleTemp[CANVAS_BYTES * 2];
  uint16_t compressedLen = compressRLE(liveCanvas, CANVAS_BYTES, rleTemp);

  AttendanceRecord* r = (AttendanceRecord*)malloc(sizeof(AttendanceRecord));
  if (!r) {
    server.send(200, "application/json", "{\"status\":\"ERROR\",\"msg\":\"Memoria cheia no ESP32.\"}");
    return;
  }

  r->rleData = (uint8_t*)malloc(compressedLen);
  if (!r->rleData) {
    free(r);
    server.send(200, "application/json", "{\"status\":\"ERROR\",\"msg\":\"Memoria insuficiente para assinatura.\"}");
    return;
  }

  memcpy(r->rleData, rleTemp, compressedLen);
  r->rleSize = compressedLen;
  r->id = recordCount + 1;
  strncpy(r->name, name.c_str(), sizeof(r->name) - 1);
  r->name[sizeof(r->name) - 1] = '\0';

  // Grava o timestamp exato do dispositivo se presente, ou do RTC interno
  if (server.hasArg("time") && server.arg("time").length() > 0) {
    String tStr = server.arg("time");
    strncpy(r->timestamp, tStr.c_str(), sizeof(r->timestamp) - 1);
    r->timestamp[sizeof(r->timestamp) - 1] = '\0';
  } else {
    time_t now = time(nullptr);
    struct tm* timeinfo = localtime(&now);
    if (clockSynced) {
      strftime(r->timestamp, sizeof(r->timestamp), "%Y-%m-%d %H:%M:%S", timeinfo);
    } else {
      snprintf(r->timestamp, sizeof(r->timestamp), "REG+%lu s", (unsigned long)(millis() / 1000));
    }
  }

  records[recordCount] = r;
  recordCount++;

  char res[128];
  snprintf(res, sizeof(res), "{\"status\":\"OK\",\"id\":%d}", r->id);
  server.send(200, "application/json", res);

  snprintf(marqueeMsg, sizeof(marqueeMsg), "ASSINATURA #%02d: %s [%s] // ", r->id, r->name, r->timestamp);
  marqueeX = SCREEN_WIDTH;

  memset(liveCanvas, 0, sizeof(liveCanvas));
  prevX = -1;
  prevY = -1;
  currentOledMode = MODE_CAROUSEL;
}

void handleStatus() {
  sendCORSHeaders();
  char buf[200];
  snprintf(buf, sizeof(buf), "{\"count\":%d,\"max\":%d,\"routine\":%d,\"opening_time\":\"%s\"}",
    recordCount, MAX_RECORDS, routineActive ? 1 : 0, routineOpeningTime);
  server.send(200, "application/json", buf);
}

void handleRoutine() {
  resetActivity();
  sendCORSHeaders();
  if (server.arg("pwd") != ADMIN_PASSWORD) {
    server.send(403, "text/plain", "Senha incorreta");
    return;
  }

  if (server.hasArg("active")) {
    routineActive = (server.arg("active") == "1");

    if (server.hasArg("time")) {
      String tStr = server.arg("time");
      strncpy(routineOpeningTime, tStr.c_str(), sizeof(routineOpeningTime) - 1);
      routineOpeningTime[sizeof(routineOpeningTime) - 1] = '\0';
    }

    if (server.hasArg("iso")) {
      syncRtcFromISO(server.arg("iso"));
    }

    if (routineActive) {
      snprintf(marqueeMsg, sizeof(marqueeMsg), "UEMA - VALORIZA // ROTINA ABERTA AS %s // ", routineOpeningTime);
    } else {
      strcpy(routineOpeningTime, "FECHADO");
      snprintf(marqueeMsg, sizeof(marqueeMsg), "UEMA - VALORIZA // ROTINA ENCERRADA // AGUARDE // ");
    }
    marqueeX = SCREEN_WIDTH;
    currentOledMode = MODE_CAROUSEL;
    server.send(200, "text/plain", "OK");
    return;
  }
  server.send(400, "text/plain", "Parametro invalido");
}

void handleRecordsList() {
  resetActivity();
  sendCORSHeaders();
  if (server.arg("pwd") != ADMIN_PASSWORD) {
    server.send(403, "text/plain", "Senha incorreta");
    return;
  }

  String json = "[";
  for (size_t i = 0; i < recordCount; i++) {
    if (records[i]) {
      json += "{\"id\":" + String(records[i]->id) + ",";
      json += "\"name\":\"" + String(records[i]->name) + "\",";
      json += "\"time\":\"" + String(records[i]->timestamp) + "\"}";
      if (i < recordCount - 1) json += ",";
    }
  }
  json += "]";
  server.send(200, "application/json", json);
}

void handleDeleteRecord() {
  resetActivity();
  sendCORSHeaders();
  if (server.arg("pwd") != ADMIN_PASSWORD) {
    server.send(403, "text/plain", "Senha incorreta");
    return;
  }

  if (server.hasArg("id")) {
    uint16_t delId = server.arg("id").toInt();
    int foundIdx = -1;
    for (size_t i = 0; i < recordCount; i++) {
      if (records[i] && records[i]->id == delId) {
        foundIdx = i;
        break;
      }
    }

    if (foundIdx != -1) {
      if (records[foundIdx]->rleData) {
        free(records[foundIdx]->rleData);
      }
      free(records[foundIdx]);

      for (size_t i = foundIdx; i < recordCount - 1; i++) {
        records[i] = records[i + 1];
      }
      records[recordCount - 1] = nullptr;
      recordCount--;
      server.send(200, "text/plain", "DELETED");
      return;
    }
  }
  server.send(400, "text/plain", "ID nao encontrado");
}

void handleExportJSON() {
  resetActivity();
  if (server.arg("pwd") != ADMIN_PASSWORD) {
    server.send(403, "text/plain", "Senha incorreta");
    return;
  }

  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.sendHeader("Content-Disposition", "attachment; filename=\"uema_valoriza_frequencias.json\"");
  server.send(200, "application/json", "[\n");

  for (size_t i = 0; i < recordCount; i++) {
    if (!records[i]) continue;
    AttendanceRecord* r = records[i];

    String chunk = "  {\n";
    chunk += "    \"id\": " + String(r->id) + ",\n";
    chunk += "    \"nome\": \"" + String(r->name) + "\",\n";
    chunk += "    \"data_hora\": \"" + String(r->timestamp) + "\",\n";
    chunk += "    \"largura\": " + String(CANVAS_W) + ",\n";
    chunk += "    \"altura\": " + String(CANVAS_H) + ",\n";
    chunk += "    \"rle_hex\": \"";
    server.sendContent(chunk);

    String hexLine = "";
    hexLine.reserve(128);
    for (int b = 0; b < r->rleSize; b++) {
      char hb[3];
      sprintf(hb, "%02X", r->rleData[b]);
      hexLine += hb;
      if (hexLine.length() >= 120) {
        server.sendContent(hexLine);
        hexLine = "";
      }
    }
    if (hexLine.length() > 0) server.sendContent(hexLine);

    String endChunk = "\"\n  }";
    if (i < recordCount - 1) endChunk += ",";
    endChunk += "\n";
    server.sendContent(endChunk);
  }

  server.sendContent("]\n");
}

void handleClearDB() {
  resetActivity();
  if (server.arg("pwd") != ADMIN_PASSWORD) {
    server.send(403, "text/plain", "Senha incorreta");
    return;
  }
  for (size_t i = 0; i < recordCount; i++) {
    if (records[i]) {
      if (records[i]->rleData) free(records[i]->rleData);
      free(records[i]);
      records[i] = nullptr;
    }
  }
  recordCount = 0;
  memset(liveCanvas, 0, sizeof(liveCanvas));
  strcpy(marqueeMsg, "UEMA - PROJETO VALORIZA // BANCO ZERADO // ");
  marqueeX = SCREEN_WIDTH;
  currentOledMode = MODE_CAROUSEL;
  sendCORSHeaders();
  server.send(200, "text/plain", "CLEARED");
}

void handleCaptiveRedirect() {
  server.sendHeader("Location", "http://192.168.4.1/", true);
  server.send(302, "text/plain", "Redirecionando");
}

// ----------------------------------------------------------------------------
// RENDERIZADOR DO EFEITO CHUVA (30 SEGUNDOS)
// ----------------------------------------------------------------------------
void updateAndRenderRain() {
  display.clearDisplay();
  for (int i = 0; i < NUM_DROPS; i++) {
    display.drawLine(drops[i].x, drops[i].y, drops[i].x, drops[i].y + drops[i].len, SSD1306_WHITE);
    drops[i].y += drops[i].speed;
    if (drops[i].y > SCREEN_HEIGHT) {
      drops[i].y = random(-10, 0);
      drops[i].x = random(0, SCREEN_WIDTH);
      drops[i].speed = random(2, 5);
      drops[i].len = random(3, 8);
    }
  }
  display.display();
}

// ----------------------------------------------------------------------------
// RENDERIZACAO OLED (CARROSSEL, CHUVA E SLEEP)
// ----------------------------------------------------------------------------
void renderOledDisplay() {
  uint32_t now = millis();
  uint32_t idle = now - lastActivityTime;

  // 1. SLEEP (1 MINUTO SEM QUALQUER INTERACAO)
  if (idle >= 60000) {
    if (!displaySleeping) {
      displaySleeping = true;
      display.clearDisplay();
      display.display();
      display.ssd1306_command(SSD1306_DISPLAYOFF);
    }
    return;
  }

  // 2. CHUVA (30 SEGUNDOS SEM INTERACAO)
  if (idle >= 30000) {
    if (currentOledMode != MODE_DRAWING && currentOledMode != MODE_ANIMATION_WHIP && currentOledMode != MODE_FLASH_PHILIPE) {
      currentOledMode = MODE_RAIN;
    }
  }

  if (currentOledMode == MODE_RAIN) {
    updateAndRenderRain();
    return;
  }

  display.clearDisplay();
  display.setTextWrap(false);
  display.setTextColor(SSD1306_WHITE);

  // Faixa Amarela Superior (y = 0 a 15)
  display.setTextSize(1);
  display.setCursor((int)marqueeX, 4);
  display.print(marqueeMsg);
  marqueeX -= 1.6f;
  if (marqueeX < -(int)(strlen(marqueeMsg) * 6)) {
    marqueeX = SCREEN_WIDTH;
  }

  display.drawFastHLine(0, 15, SCREEN_WIDTH, SSD1306_WHITE);

  // Faixa Azul Inferior (y = 16 a 63)
  switch (currentOledMode) {
    case MODE_CAROUSEL: {
      if (now - lastCarouselTick >= 4000) {
        carouselIndex = (carouselIndex + 1) % 3;
        lastCarouselTick = now;
      }

      if (carouselIndex == 0) {
        display.setTextSize(1);
        display.setCursor(6, 20);
        display.print("[CONECTE NO WI-FI]");
        display.setCursor(4, 34);
        display.print("HTTP://VALORIZA.LOCAL");
        display.setCursor(4, 48);
        display.print("OU IP: 192.168.4.1");
      } 
      else if (carouselIndex == 1) {
        display.setTextSize(1);
        display.setCursor(6, 20);
        display.print("[TOTAL DE PRESENCAS]");
        display.setTextSize(2);
        display.setCursor(16, 33);
        display.printf("%02d / %d", recordCount, MAX_RECORDS);
        display.setTextSize(1);
        display.setCursor(4, 52);
        display.printf("VAGAS LIVRES: %d", (MAX_RECORDS - recordCount));
      } 
      else if (carouselIndex == 2) {
        display.setTextSize(1);
        display.setCursor(6, 20);
        display.print("[ROTINA DO DIA]");
        display.setCursor(4, 34);
        if (routineActive) {
          display.print("STATUS: ABERTO");
          display.setCursor(4, 48);
          display.printf("INICIO: %s", routineOpeningTime);
        } else {
          display.print("STATUS: FECHADO");
          display.setCursor(4, 48);
          display.print("AGUARDE O ATENDENTE");
        }
      }
      break;
    }

    case MODE_DRAWING: {
      for (int x = 0; x < CANVAS_W; x++) {
        for (int y = 0; y < CANVAS_H; y++) {
          int byteIdx = x + (y / 8) * CANVAS_W;
          int bit = y % 8;
          if (liveCanvas[byteIdx] & (1 << bit)) {
            display.drawPixel(x, y + 16, SSD1306_WHITE);
          }
        }
      }
      break;
    }

    case MODE_ANIMATION_WHIP: {
      uint32_t elapsed = now - animStartTime;
      if (elapsed < 2400) {
        float angle = (elapsed / 50.0f);
        int offsetX = (int)(sin(angle) * (14.0f * (1.0f - (elapsed / 2400.0f))));
        
        display.setTextSize(1);
        display.setCursor(18 + offsetX, 20);
        display.print("ASSINATURA DE:");

        int nameLen = strlen(currentSignerName);
        if (nameLen <= 10) {
          int textW = nameLen * 12;
          int posX = constrain((SCREEN_WIDTH - textW) / 2 + offsetX, 0, SCREEN_WIDTH - textW);
          display.setTextSize(2);
          display.setCursor(posX, 36);
          display.print(currentSignerName);
        } else {
          int textW = nameLen * 6;
          int posX = constrain((SCREEN_WIDTH - textW) / 2 + offsetX, 0, SCREEN_WIDTH - textW);
          display.setTextSize(1);
          display.setCursor(posX, 38);
          display.print(currentSignerName);
        }
      } else {
        currentOledMode = MODE_FLASH_PHILIPE;
        animStartTime = now;
      }
      break;
    }

    case MODE_FLASH_PHILIPE: {
      uint32_t elapsed = now - animStartTime;
      if (elapsed < 500) {
        display.fillRect(0, 16, SCREEN_WIDTH, 48, SSD1306_WHITE);
        display.setTextColor(SSD1306_BLACK, SSD1306_WHITE);
        display.setTextSize(1);
        display.setCursor(16, 24);
        display.print("DESENVOLVIDO POR");
        display.setTextSize(2);
        display.setCursor(22, 40);
        display.print("PHILIPE");
      } else {
        currentOledMode = MODE_CAROUSEL;
        lastCarouselTick = millis();
      }
      break;
    }

    default:
      break;
  }

  display.display();
}

// ----------------------------------------------------------------------------
// SETUP INICIAL
// ----------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\n[BOOT] Iniciando Servidor Valoriza UEMA (v2.2)...");

  for (int i = 0; i < MAX_RECORDS; i++) {
    records[i] = nullptr;
  }
  memset(liveCanvas, 0, sizeof(liveCanvas));
  initRain();

  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(400000);

  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_I2C_ADDRESS)) {
    Serial.println("[FALHA] Display OLED nao respondeu.");
  } else {
    display.clearDisplay();
    display.setTextWrap(false);
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(16, 20);
    display.print("UEMA - VALORIZA");
    display.setCursor(10, 36);
    display.print("SISTEMA ON");
    display.display();
    
    // TELA INICIAL: EXATAMENTE 5 SEGUNDOS DE EXIBICAO
    delay(5000);
  }

  WiFi.persistent(false);
  WiFi.disconnect(true);
  delay(50);
  WiFi.mode(WIFI_AP);

  WiFi.setSleep(false);
  esp_wifi_set_ps(WIFI_PS_NONE);

  WiFi.softAPConfig(AP_IP, AP_IP, AP_NETMASK);
  WiFi.softAP(AP_SSID, NULL, 1, 0, 8);
  Serial.printf("[REDE] SSID: %s | IP: 192.168.4.1\n", AP_SSID);

  dnsServer.start(DNS_PORT, "*", AP_IP);

  if (MDNS.begin("valoriza")) {
    MDNS.addService("http", "tcp", 80);
    Serial.println("[MDNS] Disponivel em: http://valoriza.local");
  }

  // Rotas HTTP
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/wake", HTTP_POST, handleWake);
  server.on("/api/admin-auth", HTTP_POST, handleAdminAuth);
  server.on("/api/draw-stroke", HTTP_POST, handleDrawStroke);
  server.on("/api/clear-canvas", HTTP_POST, handleClearCanvas);
  server.on("/api/finish-stroke", HTTP_POST, handleFinishStroke);
  server.on("/api/commit-attendance", HTTP_POST, handleCommitAttendance);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/routine", HTTP_POST, handleRoutine);
  server.on("/api/records-list", HTTP_GET, handleRecordsList);
  server.on("/api/delete-record", HTTP_POST, handleDeleteRecord);
  server.on("/api/export-json", HTTP_GET, handleExportJSON);
  server.on("/api/clear-db", HTTP_POST, handleClearDB);

  // Pre-flight OPTIONS
  server.on("/api/wake", HTTP_OPTIONS, handleOptions);
  server.on("/api/commit-attendance", HTTP_OPTIONS, handleOptions);
  server.on("/api/draw-stroke", HTTP_OPTIONS, handleOptions);
  server.on("/api/finish-stroke", HTTP_OPTIONS, handleOptions);
  server.on("/api/routine", HTTP_OPTIONS, handleOptions);
  server.on("/api/delete-record", HTTP_OPTIONS, handleOptions);
  server.on("/api/admin-auth", HTTP_OPTIONS, handleOptions);

  // Redirecionamentos de Portal Cativo
  server.on("/generate_204", handleCaptiveRedirect);
  server.on("/gen_204", handleCaptiveRedirect);
  server.on("/hotspot-detect.html", handleCaptiveRedirect);
  server.on("/ncsi.txt", handleCaptiveRedirect);
  server.on("/connecttest.txt", handleCaptiveRedirect);
  server.onNotFound(handleCaptiveRedirect);

  server.begin();
  lastCarouselTick = millis();
  lastActivityTime = millis();
  Serial.println("[OK] Servidor pronto.");
}

// ----------------------------------------------------------------------------
// LOOP PRINCIPAL
// ----------------------------------------------------------------------------
void loop() {
  dnsServer.processNextRequest();
  server.handleClient();

  uint32_t now = millis();
  if (now - lastOledFrameTime >= 40) {
    lastOledFrameTime = now;
    renderOledDisplay();
  }

  yield();
}
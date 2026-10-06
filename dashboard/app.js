/* Sentinel-X — dashboard de supervision
 * - Client MQTT 3.1.1 sur WebSocket (wss://<serveur>/mqtt, via nginx), sans librairie externe
 * - Graphiques temps réel sur <canvas>
 * - Fonctionne hors Internet (aucun CDN)
 */
'use strict';

/* ===================== Configuration ===================== */
const CFG = {
  group: 'g02',                 // numéro de groupe (doit correspondre à l'ACL Mosquitto)
  wsPath: '/mqtt',              // proxifié par nginx vers mosquitto:9001
  camUrl: '/video/stream',      // flux MJPEG du script IA (proxifié par nginx)
  maxPoints: 150,               // points gardés sur les graphiques (~5 min à 2 s)
  staleMs: 15000,               // boîtier considéré hors ligne sans mesure depuis 15 s
  keepalive: 30,                // secondes
  // Seuils d'AFFICHAGE uniquement (la vraie détection d'anomalies est faite par l'IA)
  temp: { warn: 32, crit: 40 },
  gaz:  { warn: 1200, crit: 2000 }   // valeur brute ADC ESP32 (0-4095) : à calibrer sur place
};
const T = {
  telemetry: `sentinelx/${CFG.group}/telemetry`,
  alerts:    `sentinelx/${CFG.group}/alerts`,
  status:    `sentinelx/${CFG.group}/status`,
  cmd:       `sentinelx/${CFG.group}/cmd`,
  all:       `sentinelx/${CFG.group}/#`
};

/* ===================== Utilitaires DOM ===================== */
const $ = (id) => document.getElementById(id);
const fmtTime = (d = new Date()) => d.toLocaleTimeString('fr-FR', { hour12: false });
let toastTimer;
function toast(msg) {
  const t = $('toast'); t.textContent = msg; t.hidden = false;
  clearTimeout(toastTimer); toastTimer = setTimeout(() => (t.hidden = true), 2500);
}
function cssVar(name) { return getComputedStyle(document.documentElement).getPropertyValue(name).trim(); }

/* ===================== Client MQTT minimal (3.1.1 / WebSocket) ===================== */
class MiniMqtt {
  constructor(url, { username, password, clientId, keepalive = 30 }) {
    Object.assign(this, { url, username, password, clientId, keepalive });
    this.buf = new Uint8Array(0);
    this.packetId = 1;
    this.handlers = { connect: [], message: [], close: [], error: [] };
    this.enc = new TextEncoder(); this.dec = new TextDecoder();
  }
  on(ev, fn) { this.handlers[ev].push(fn); return this; }
  emit(ev, ...a) { this.handlers[ev].forEach((fn) => fn(...a)); }

  connect() {
    this.ws = new WebSocket(this.url, ['mqtt']);
    this.ws.binaryType = 'arraybuffer';
    this.ws.onopen = () => this.send(this.connectPacket());
    this.ws.onmessage = (e) => this.feed(new Uint8Array(e.data));
    this.ws.onerror = () => this.emit('error', 'Connexion WebSocket impossible');
    this.ws.onclose = () => { clearInterval(this.ping); this.emit('close'); };
  }
  end() { try { this.send(Uint8Array.of(0xe0, 0x00)); this.ws.close(); } catch (_) {} }

  /* --- encodage --- */
  str(s) { const b = this.enc.encode(s); return [b.length >> 8, b.length & 255, ...b]; }
  remLen(n) { const out = []; do { let d = n % 128; n = Math.floor(n / 128); if (n > 0) d |= 128; out.push(d); } while (n > 0); return out; }
  packet(type, body) { return Uint8Array.from([type, ...this.remLen(body.length), ...body]); }
  connectPacket() {
    let flags = 0x02;                       // clean session
    if (this.username) flags |= 0x80;
    if (this.password) flags |= 0x40;
    const body = [...this.str('MQTT'), 4, flags, this.keepalive >> 8, this.keepalive & 255, ...this.str(this.clientId)];
    if (this.username) body.push(...this.str(this.username));
    if (this.password) body.push(...this.str(this.password));
    return this.packet(0x10, body);
  }
  subscribe(topic) {
    const id = this.packetId++;
    this.send(this.packet(0x82, [id >> 8, id & 255, ...this.str(topic), 0]));
  }
  publish(topic, payload) {
    const p = typeof payload === 'string' ? this.enc.encode(payload) : payload;
    this.send(this.packet(0x30, [...this.str(topic), ...p]));
  }
  send(bytes) { if (this.ws && this.ws.readyState === 1) this.ws.send(bytes); }

  /* --- décodage (gère paquets fragmentés ou groupés) --- */
  feed(chunk) {
    const merged = new Uint8Array(this.buf.length + chunk.length);
    merged.set(this.buf); merged.set(chunk, this.buf.length);
    this.buf = merged;
    for (;;) {
      if (this.buf.length < 2) return;
      let mult = 1, len = 0, i = 1, byte;
      do {
        if (i >= this.buf.length) return;
        byte = this.buf[i++]; len += (byte & 127) * mult; mult *= 128;
      } while (byte & 128);
      if (this.buf.length < i + len) return;
      this.handle(this.buf[0], this.buf.subarray(i, i + len));
      this.buf = this.buf.slice(i + len);
    }
  }
  handle(h, body) {
    const type = h >> 4;
    if (type === 2) {                                   // CONNACK
      const rc = body[1];
      if (rc === 0) {
        this.ping = setInterval(() => this.send(Uint8Array.of(0xc0, 0x00)), this.keepalive * 500);
        this.emit('connect');
      } else {
        const why = { 1: 'version refusée', 2: 'identifiant refusé', 3: 'broker indisponible', 4: 'utilisateur ou mot de passe incorrect', 5: 'accès non autorisé' }[rc] || `code ${rc}`;
        this.emit('error', why); this.ws.close();
      }
    } else if (type === 3) {                            // PUBLISH
      const qos = (h >> 1) & 3;
      const tl = (body[0] << 8) | body[1];
      const topic = this.dec.decode(body.subarray(2, 2 + tl));
      let off = 2 + tl;
      if (qos > 0) {                                    // acquitter si QoS 1
        const id = [body[off], body[off + 1]]; off += 2;
        if (qos === 1) this.send(Uint8Array.of(0x40, 0x02, ...id));
      }
      this.emit('message', topic, this.dec.decode(body.subarray(off)), !!(h & 1));
    }
    // SUBACK (9) et PINGRESP (13) : rien à faire
  }
}

/* ===================== Graphique temps réel (canvas) ===================== */
class LiveChart {
  constructor(canvas, series) {
    this.c = canvas; this.ctx = canvas.getContext('2d');
    this.series = series.map((s) => ({ ...s, data: [] }));   // {name, color, axis:'left'|'right'}
    this.labels = [];
    new ResizeObserver(() => this.draw()).observe(canvas);
  }
  push(label, values) {
    this.labels.push(label);
    this.series.forEach((s, i) => s.data.push(values[i]));
    if (this.labels.length > CFG.maxPoints) { this.labels.shift(); this.series.forEach((s) => s.data.shift()); }
    this.draw();
  }
  range(axis) {
    const vals = this.series.filter((s) => s.axis === axis).flatMap((s) => s.data).filter((v) => v != null && !isNaN(v));
    if (!vals.length) return null;
    let lo = Math.min(...vals), hi = Math.max(...vals);
    const pad = (hi - lo) * 0.15 || Math.abs(hi) * 0.1 || 1;
    return [lo - pad, hi + pad];
  }
  draw() {
    const dpr = window.devicePixelRatio || 1;
    const w = this.c.clientWidth, h = this.c.clientHeight;
    if (!w) return;
    this.c.width = w * dpr; this.c.height = h * dpr;
    const ctx = this.ctx; ctx.setTransform(dpr, 0, 0, dpr, 0, 0); ctx.clearRect(0, 0, w, h);
    const hasRight = this.series.some((s) => s.axis === 'right');
    const P = { l: 46, r: hasRight ? 46 : 12, t: 10, b: 22 };
    const pw = w - P.l - P.r, ph = h - P.t - P.b;
    const grid = cssVar('--line'), muted = cssVar('--muted');
    ctx.font = '11px ui-monospace, Consolas, monospace';

    const ranges = { left: this.range('left'), right: this.range('right') };
    // grille + graduations
    ctx.strokeStyle = grid; ctx.lineWidth = 1; ctx.fillStyle = muted;
    for (let i = 0; i <= 4; i++) {
      const y = P.t + (ph * i) / 4;
      ctx.beginPath(); ctx.moveTo(P.l, y); ctx.lineTo(P.l + pw, y); ctx.stroke();
      ['left', 'right'].forEach((ax) => {
        const r = ranges[ax]; if (!r) return;
        const v = r[1] - ((r[1] - r[0]) * i) / 4;
        ctx.textAlign = ax === 'left' ? 'right' : 'left';
        ctx.fillText(v.toFixed(Math.abs(r[1] - r[0]) < 2 ? 2 : Math.abs(r[1] - r[0]) < 20 ? 1 : 0), ax === 'left' ? P.l - 6 : P.l + pw + 6, y + 4);
      });
    }
    const n = this.labels.length;
    if (n === 0) {
      ctx.textAlign = 'center'; ctx.fillText('En attente de mesures…', P.l + pw / 2, P.t + ph / 2); return;
    }
    const span = Math.max(n, 30) - 1;                 // les premiers points s'étalent, puis défilent
    const x = (i) => P.l + (pw * i) / span;
    // étiquettes de temps (début / dernier point)
    ctx.textAlign = 'left'; ctx.fillText(this.labels[0], P.l, h - 6);
    if (n > 20) { ctx.textAlign = 'right'; ctx.fillText(this.labels[n - 1], Math.min(x(n - 1), P.l + pw), h - 6); }
    this.series.forEach((s) => {
      const r = ranges[s.axis]; if (!r) return;
      const y = (v) => P.t + ph - ((v - r[0]) / (r[1] - r[0])) * ph;
      const color = cssVar(s.color);
      // aire
      ctx.beginPath(); let started = false, lastX = 0;
      s.data.forEach((v, i) => { if (v == null) return; const px = x(i), py = y(v); started ? ctx.lineTo(px, py) : (ctx.moveTo(px, py), started = true); lastX = px; });
      if (!started) return;
      ctx.strokeStyle = color; ctx.lineWidth = 2; ctx.stroke();
      ctx.lineTo(lastX, P.t + ph); ctx.lineTo(x(s.data.findIndex((v) => v != null)), P.t + ph); ctx.closePath();
      ctx.globalAlpha = 0.12; ctx.fillStyle = color; ctx.fill(); ctx.globalAlpha = 1;
      // dernier point
      const li = s.data.length - 1, lv = s.data[li];
      if (lv != null) { ctx.beginPath(); ctx.arc(x(li), y(lv), 3.5, 0, Math.PI * 2); ctx.fillStyle = color; ctx.fill(); }
    });
  }
}

/* ===================== État de l'application ===================== */
const state = { client: null, sim: null, lastSeen: 0, alerts: 0, boxStatus: null, connected: false };
const chartTH = new LiveChart($('chartTH'), [
  { name: 'Température', color: '--c1', axis: 'left' },
  { name: 'Humidité', color: '--c2', axis: 'right' }
]);
const chartGaz = new LiveChart($('chartGaz'), [{ name: 'Gaz', color: '--c3', axis: 'left' }]);
$('groupLbl').textContent = CFG.group.toUpperCase();

/* ===================== Journal ===================== */
function log(kind, label, msg) {
  const li = document.createElement('li'); li.className = kind;
  const t = document.createElement('time'); t.textContent = fmtTime();
  const k = document.createElement('span'); k.className = 'k'; k.textContent = label;
  const m = document.createElement('span'); m.className = 'msg'; m.textContent = msg;   // textContent : pas d'injection HTML
  li.append(t, k, m);
  const ol = $('log'); ol.prepend(li);
  while (ol.children.length > 200) ol.lastChild.remove();
}
$('clearLog').onclick = () => ($('log').innerHTML = '');

/* ===================== Traitement des messages ===================== */
function level(v, th) { return v >= th.crit ? 'crit' : v >= th.warn ? 'warn' : 'ok'; }
function setTile(id, cls) { const el = $(id); el.classList.remove('ok', 'warn', 'crit'); if (cls) el.classList.add(cls); }
const num = (v) => (v == null || v === '' || isNaN(+v) ? null : +v);

function onTelemetry(raw) {
  let d; try { d = JSON.parse(raw); } catch { log('warn', 'FORMAT', `Mesure illisible : ${raw.slice(0, 80)}`); return; }
  const t = num(d.t ?? d.temp ?? d.temperature), h = num(d.h ?? d.hum ?? d.humidity),
        g = num(d.gaz ?? d.gas ?? d.mq2), pir = d.pir ?? d.motion;
  state.lastSeen = Date.now();
  if (t != null) { $('vTemp').textContent = t.toFixed(1); setTile('tileTemp', level(t, CFG.temp)); }
  if (h != null) { $('vHum').textContent = h.toFixed(0); setTile('tileHum', 'ok'); }
  if (g != null) { $('vGaz').textContent = Math.round(g); setTile('tileGaz', level(g, CFG.gaz)); }
  if (pir != null) {
    const on = pir === 1 || pir === true || pir === '1';
    $('vPir').textContent = on ? 'DÉTECTÉ' : 'Aucun';
    setTile('tilePir', on ? 'warn' : 'ok');
  }
  const lbl = fmtTime();
  chartTH.push(lbl, [t, h]);
  chartGaz.push(lbl, [g]);
}

function onAlert(raw) {
  let a; try { a = JSON.parse(raw); } catch { a = { type: 'alerte', msg: raw }; }
  const sev = String(a.niveau ?? a.level ?? 'critique').toLowerCase();
  const kind = /crit|high|haut/.test(sev) ? 'crit' : /warn|moy|attention/.test(sev) ? 'warn' : 'info';
  const type = String(a.type ?? 'alerte').toUpperCase();
  const detail = a.msg ?? a.message ?? (a.conf != null ? `confiance ${Math.round(a.conf * 100)} %` : sev);
  state.alerts++; $('vAlerts').textContent = state.alerts; setTile('tileAlerts', kind === 'info' ? 'ok' : kind);
  log(kind, type, detail);
  if (kind === 'crit') {
    const b = $('alertBanner'); b.textContent = `⚠ ALERTE ${type} — ${detail} (${fmtTime()})`; b.hidden = false;
    $('brandDot').className = 'dot alarm';
    clearTimeout(state.bannerTimer);
    state.bannerTimer = setTimeout(() => { b.hidden = true; $('brandDot').className = state.connected ? 'dot on' : 'dot'; }, 15000);
  }
}

function onStatus(raw, retained) {
  state.boxStatus = raw.trim().toLowerCase();
  if (!retained) log(state.boxStatus === 'online' ? 'ok' : 'warn', 'BOÎTIER', `État : ${state.boxStatus}`);
}

function route(topic, payload, retained) {
  if (topic === T.telemetry) onTelemetry(payload);
  else if (topic === T.alerts) onAlert(payload);
  else if (topic === T.status) onStatus(payload, retained);
}

/* ===================== Indicateurs de santé (chaque seconde) ===================== */
function pill(id, text, cls) { const p = $(id); p.textContent = text; p.className = 'pill' + (cls ? ' ' + cls : ''); }
setInterval(() => {
  if ($('app').hidden) return;
  if (state.sim) pill('pillBroker', 'Broker : simulation', 'warn');
  else pill('pillBroker', state.connected ? 'Broker : connecté (TLS)' : 'Broker : déconnecté', state.connected ? 'ok' : 'crit');

  const age = state.lastSeen ? Date.now() - state.lastSeen : Infinity;
  const fresh = age < CFG.staleMs;
  const offline = state.boxStatus === 'offline' || !fresh;
  pill('pillBox', offline ? 'Boîtier : hors ligne' : 'Boîtier : en ligne', offline ? 'crit' : 'ok');
  pill('pillLast', state.lastSeen ? `Dernière mesure : il y a ${Math.round(age / 1000)} s` : 'Dernière mesure : —', fresh ? '' : 'warn');
}, 1000);

/* ===================== Commandes ===================== */
document.querySelectorAll('[data-cmd]').forEach((btn) => {
  btn.addEventListener('click', () => {
    const payload = btn.dataset.cmd;
    if (state.sim) { log('info', 'COMMANDE', `${payload} (simulation, non envoyée)`); toast('Simulation : commande non envoyée'); return; }
    if (!state.connected) { toast('Non connecté au broker'); return; }
    state.client.publish(T.cmd, payload);
    log('info', 'COMMANDE', payload);
    toast('Commande envoyée au boîtier');
  });
});

/* ===================== Webcam (flux MJPEG de l'IA) ===================== */
function startCam() {
  const img = $('cam');
  img.onload = () => { img.classList.add('live'); $('camOff').hidden = true; $('camTag').textContent = 'en direct'; $('camTag').className = 'tag ok'; };
  img.onerror = () => {
    img.classList.remove('live'); $('camOff').hidden = false; $('camTag').textContent = 'hors ligne'; $('camTag').className = 'tag';
    setTimeout(() => (img.src = `${CFG.camUrl}?t=${Date.now()}`), 5000);
  };
  img.src = `${CFG.camUrl}?t=${Date.now()}`;
}

/* ===================== Connexion ===================== */
function showApp() { $('login').hidden = true; $('app').hidden = false; chartTH.draw(); chartGaz.draw(); startCam(); }

/* Historique : précharge les graphiques et le journal depuis l'API (si elle tourne) */
async function loadHistory() {
  try {
    const [rm, ra] = await Promise.all([fetch('/api/v1/mesures?limit=' + CFG.maxPoints), fetch('/api/v1/alerts?limit=20')]);
    if (rm.ok) {
      const rows = await rm.json();
      rows.forEach((r) => {
        const lbl = fmtTime(new Date(r.ts));
        chartTH.push(lbl, [num(r.t), num(r.h)]);
        chartGaz.push(lbl, [num(r.gaz)]);
      });
      if (rows.length) log('info', 'HISTORIQUE', `${rows.length} mesures chargées depuis la base`);
    }
    if (ra.ok) {
      const rows = await ra.json();
      rows.slice().reverse().forEach((a) => {
        const k = /crit/.test(a.niveau) ? 'crit' : /att/.test(a.niveau) ? 'warn' : 'info';
        log(k, String(a.type).toUpperCase(), `${a.msg ?? ''} (${new Date(a.ts).toLocaleString('fr-FR')})`);
      });
    }
  } catch (_) { /* API absente : le dashboard fonctionne quand même en direct */ }
}

function connect(user, pass) {
  const proto = location.protocol === 'https:' ? 'wss' : 'ws';
  const url = `${proto}://${location.host}${CFG.wsPath}`;
  const client = new MiniMqtt(url, {
    username: user, password: pass,
    clientId: `dashboard-${Math.random().toString(16).slice(2, 10)}`, keepalive: CFG.keepalive
  });
  let firstConnect = true, userClosed = false;
  $('loginBtn').disabled = true; $('loginErr').hidden = true;

  client.on('connect', () => {
    state.connected = true; state.client = client;
    client.subscribe(T.all);
    $('brandDot').className = 'dot on';
    if (firstConnect) { showApp(); loadHistory(); log('ok', 'BROKER', `Connecté à ${url} en tant que « ${user} »`); firstConnect = false; }
    else log('ok', 'BROKER', 'Reconnecté');
  });
  client.on('message', route);
  client.on('error', (why) => {
    if (firstConnect) { $('loginErr').textContent = `Échec : ${why}`; $('loginErr').hidden = false; $('loginBtn').disabled = false; userClosed = true; }
    else log('crit', 'BROKER', why);
  });
  client.on('close', () => {
    state.connected = false; $('brandDot').className = 'dot';
    if (userClosed || firstConnect) { $('loginBtn').disabled = false; return; }
    log('warn', 'BROKER', 'Connexion perdue, nouvelle tentative dans 3 s');
    setTimeout(() => { if (!userClosed) client.connect(); }, 3000);
  });
  client.stop = () => { userClosed = true; client.end(); };
  client.connect();
  state.client = client;
}

$('loginForm').addEventListener('submit', (e) => {
  e.preventDefault();
  connect($('user').value.trim(), $('pass').value);
  $('pass').value = '';            // le mot de passe n'est gardé nulle part
});

$('logoutBtn').onclick = () => {
  if (state.client && state.client.stop) state.client.stop();
  if (state.sim) { clearInterval(state.sim); state.sim = null; $('simBanner').hidden = true; }
  location.reload();
};

/* ===================== Mode simulation (test de l'interface sans boîtier) ===================== */
$('demoBtn').onclick = () => {
  $('simBanner').hidden = false; showApp();
  log('warn', 'SIMULATION', 'Données fictives générées dans le navigateur');
  let t = 23.5, h = 46, g = 450, k = 0;
  state.boxStatus = 'online';
  state.sim = setInterval(() => {
    k++;
    t += (Math.random() - 0.45) * 0.3 + (k > 60 && k < 90 ? 0.35 : 0);   // montée lente de température
    h += (Math.random() - 0.5) * 0.8;
    g += (Math.random() - 0.5) * 40 + (k > 70 && k < 90 ? 60 : 0);       // micro-déviation de gaz
    const pir = Math.random() < 0.08 ? 1 : 0;
    route(T.telemetry, JSON.stringify({ t: +t.toFixed(1), h: +h.toFixed(0), gaz: Math.max(0, Math.round(g)), pir }), false);
    if (k === 85) route(T.alerts, JSON.stringify({ type: 'anomalie', niveau: 'critique', msg: 'Corrélation hausse température + gaz (Isolation Forest)' }), false);
    if (pir && Math.random() < 0.3) route(T.alerts, JSON.stringify({ type: 'intrus', niveau: 'attention', conf: 0.87 }), false);
    if (k > 120) { k = 0; t = 23.5; g = 450; }
  }, 2000);
};

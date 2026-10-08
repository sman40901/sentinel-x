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
  // Valeur brute de l'ADC du boîtier. ATTENTION : l'ESP8266 est en 10 bits
  // (0-1023), pas en 12 bits (0-4095) comme l'ancien ESP32. Les seuils
  // ci-dessous ont été divisés par 4 en conséquence — à recalibrer sur place.
  // Le signal fiable reste gaz_d (écart en volts par rapport à la baseline),
  // que le firmware publie à côté de gaz.
  gaz:  { warn: 300, crit: 500 }
};
const T = {
  telemetry: `sentinelx/${CFG.group}/telemetry`,
  state:     `sentinelx/${CFG.group}/state`,       // état complet, retenu
  test:      `sentinelx/${CFG.group}/test`,        // avancement de l'auto-test
  testmeta:  `sentinelx/${CFG.group}/testmeta`,    // liste des étapes
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
  else if (topic === T.state) onState(payload);
  else if (topic === T.test) onTest(payload);
  else if (topic === T.testmeta) onTestMeta(payload);
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
  let t = 23.5, h = 46, g = 210, k = 0;   // g sur l'échelle ADC 0-1023 de l'ESP8266
  state.boxStatus = 'online';
  state.sim = setInterval(() => {
    k++;
    t += (Math.random() - 0.45) * 0.3 + (k > 60 && k < 90 ? 0.35 : 0);   // montée lente de température
    h += (Math.random() - 0.5) * 0.8;
    g += (Math.random() - 0.5) * 10 + (k > 70 && k < 90 ? 15 : 0);       // micro-déviation de gaz
    const pir = Math.random() < 0.08 ? 1 : 0;
    const pres = Math.max(0, Math.round(2 + (Math.random() - 0.5) * 4 + (k > 70 && k < 95 ? 7 : 0)));
    route(T.telemetry, JSON.stringify({
      t: +t.toFixed(1), h: +h.toFixed(0), gaz: Math.max(0, Math.round(g)), pir,
      pres, assoc: Math.min(pres, 1), sniff: Math.max(0, pres - 1),
      state: pres >= 10 || pir ? 'red' : pres >= 3 ? 'yellow' : 'green'
    }), false);
    if (k === 85) route(T.alerts, JSON.stringify({ type: 'anomalie', niveau: 'critique', msg: 'Corrélation hausse température + gaz (Isolation Forest)' }), false);
    if (pir && Math.random() < 0.3) route(T.alerts, JSON.stringify({ type: 'intrus', niveau: 'attention', conf: 0.87 }), false);
    // Un état cohérent, pour que toute l'interface soit démontrable sans boîtier.
    const st = k > 95 ? 'alarm' : k > 88 ? 'entry' : pres >= 6 ? 'armed' : 'armed';
    const led = st === 'alarm' ? [0, 0, 3] : st === 'entry' ? [0, 3, 0]
              : pres >= 6 ? [0, 2, 0] : [1, 0, 0];
    route(T.state, JSON.stringify({
      st, left: st === 'entry' ? 95 - k + 25 : st === 'alarm' ? 120 - k : 0,
      cause: st === 'alarm' ? 'EXERCICE : alarme simulee'
           : st === 'entry' ? 'Mouvement confirme (PIR)' : '',
      warn: pres >= 6, warnWhy: pres >= 6 ? `Presence WiFi inhabituelle (+${pres - 3} appareils)` : '',
      alarms: k > 95 ? 1 : 0, healthy: true,
      led, buzz: st === 'alarm' ? 4 : st === 'entry' ? 6 : 0,
      maint: { on: false, nonce: 'simulation', usable: false, locked: 0, fails: 0, grants: 0, denials: 0 },
      manual: false, manLed: [false, false, false], manBuzz: false, forced: -1,
      muted: false, hasBuzzer: true, dhtOk: true, pirRaw: pir, pirOk: !!pir, pirWarm: 0,
      gas: { raw: Math.round(g), v: g * 3.3 / 1023 * 2, base: 1.3, delta: 0.02, rise: 0.01,
             warn: false, crit: false, fast: false, ceil: 6.6, warming: false },
      imu: { ok: true, who: 112, tilt: 0.4, shock: 0.01, spin: 0, tamper: false, ref: true, warnDeg: 8 },
      pres: { n: pres, rnd: Math.max(0, pres - 1), stable: Math.min(1, pres), rssi: -58,
              amb: 3.0, excess: pres - 3, streak: pres >= 6 ? 2 : 0, windows: 9,
              learn: false, warn: pres >= 6, on: true, next: 40, frames: pres * 7 },
      test: false, heap: 33000, up: k * 2, rssi: -61, drops: 0, build: 'simulation'
    }), false);
    if (k > 120) { k = 0; t = 23.5; g = 210; }
  }, 2000);
};

/* =====================================================================
 * État du boîtier, commandes, maintenance et auto-test
 *
 * Le boîtier n'héberge plus de page : il publie son état sur MQTT et
 * reçoit ses ordres sur le topic cmd. Tout ce qui suit parle ce langage.
 * ===================================================================== */

const BOX = { state: null, test: null, meta: null };

/* ---------- envoi d'une commande ---------- */
function cmd(obj) {
  if (state.sim) { log('info', 'COMMANDE', `${JSON.stringify(obj)} (simulation)`); return false; }
  if (!state.connected) { toast('Non connecté au broker'); return false; }
  state.client.publish(T.cmd, JSON.stringify(obj));
  return true;
}

/* ---------- maintenance : preuve du code sans jamais l'envoyer ----------
 *
 * Le boîtier publie un nonce aléatoire ; on renvoie sha256(nonce + ":" + code).
 * Le code ne quitte donc jamais le navigateur, et le nonce étant à usage
 * unique, rejouer une réponse interceptée ne sert à rien.
 *
 * crypto.subtle n'existe qu'en contexte sécurisé : le dashboard doit être
 * servi en HTTPS (c'est le cas via nginx). En HTTP simple, on le dit au lieu
 * d'échouer silencieusement.
 */
async function sha256hex(str) {
  const buf = await crypto.subtle.digest('SHA-256', new TextEncoder().encode(str));
  return [...new Uint8Array(buf)].map((b) => b.toString(16).padStart(2, '0')).join('');
}

$('maintForm').addEventListener('submit', async (e) => {
  e.preventDefault();
  const pin = $('maintPin').value;
  $('maintPin').value = '';                       // jamais conservé
  if (!BOX.state) { toast('État du boîtier inconnu'); return; }
  if (BOX.state.maint && BOX.state.maint.on) { cmd({ maint: 'off' }); return; }
  if (!window.isSecureContext || !crypto.subtle) {
    toast('Maintenance impossible en HTTP : ouvrez le dashboard en HTTPS');
    return;
  }
  if (!pin) { toast('Entrez le code'); return; }
  const nonce = BOX.state.maint ? BOX.state.maint.nonce : '';
  if (!nonce) { toast('Pas de défi reçu du boîtier'); return; }
  cmd({ maint: await sha256hex(`${nonce}:${pin}`) });
});

$('armBtn').onclick = () => cmd({ arm: 'now' });

/* ---------- boutons de pilotage ---------- */
for (const b of $('segMode').querySelectorAll('[data-mode]')) b.onclick = () => cmd({ mode: b.dataset.mode });
for (const b of $('segForce').querySelectorAll('[data-force]')) b.onclick = () => cmd({ force: b.dataset.force });
for (const b of document.querySelectorAll('[data-led]')) {
  b.onclick = () => {
    const i = { vert: 0, jaune: 1, rouge: 2 }[b.dataset.led];
    const on = BOX.state && BOX.state.manLed ? BOX.state.manLed[i] : false;
    cmd({ [b.dataset.led]: on ? 0 : 1 });
  };
}
for (const b of document.querySelectorAll('[data-drill]')) {
  b.onclick = () => { if (cmd({ drill: b.dataset.drill })) toast('Exercice lancé'); };
}
for (const b of document.querySelectorAll('[data-rebase]')) {
  b.onclick = () => { if (cmd({ rebase: b.dataset.rebase })) toast('Référence reprise'); };
}
$('bzToggle').onclick = () => cmd({ buzzer: BOX.state && BOX.state.manBuzz ? 0 : 1 });
$('bzBeep').onclick   = () => cmd({ beep: 300 });
$('allOff').onclick   = () => cmd({ alloff: 1 });
$('muteBtn').onclick  = () => cmd({ mute: BOX.state && BOX.state.muted ? 0 : 1 });
$('sniffBtn').onclick = () => cmd({ sniff: BOX.state && BOX.state.pres && BOX.state.pres.on ? 0 : 1 });

/* ---------- auto-test ---------- */
const tCmd = (action, extra = {}) => cmd(Object.assign({ test: action }, extra));
$('tStart').onclick = () => tCmd('start');
$('tPause').onclick = () => tCmd('toggle');
$('tStop').onclick  = () => tCmd('stop');
$('tPrev').onclick  = () => tCmd('prev');
$('tNext').onclick  = () => tCmd('next');
$('tLoop').onclick  = () => tCmd('loop', { flag: BOX.test && BOX.test.loop ? 0 : 1 });
$('tWait').onclick  = () => tCmd('wait', { flag: BOX.test && BOX.test.wait ? 0 : 1 });
$('tYes').onclick   = () => BOX.test && tCmd('confirm', { step: BOX.test.step, flag: 1 });
$('tNo').onclick    = () => BOX.test && tCmd('confirm', { step: BOX.test.step, flag: 0 });
$('tList').addEventListener('click', (e) => {
  const yn = e.target.closest('button[data-c]');
  if (yn) { tCmd('confirm', { step: +yn.dataset.c, flag: +yn.dataset.ok }); return; }
  const li = e.target.closest('li');
  if (li) tCmd('goto', { step: +li.dataset.i });
});

/* ---------- rendu de l'état ---------- */
const ST_LABEL = { armed: 'ARMÉ', exit: 'SORTIE EN COURS', entry: 'INTRUSION — IDENTIFIEZ-VOUS',
                   alarm: 'ALARME', maintenance: 'MAINTENANCE' };
const press = (el, on) => el.setAttribute('aria-pressed', on ? 'true' : 'false');

function onState(raw) {
  let d; try { d = JSON.parse(raw); } catch { return; }
  BOX.state = d;
  state.lastSeen = Date.now();

  $('stateBar').dataset.st = d.st;
  $('stName').textContent = ST_LABEL[d.st] || d.st;
  const cnt = $('stCount');
  cnt.hidden = !d.left;
  cnt.textContent = d.left ? `${d.left} s` : '';
  $('stWhy').textContent = d.warn && d.warnWhy ? d.warnWhy
    : d.cause ? d.cause
    : d.healthy ? 'Aucune anomalie.' : 'En service, mais un capteur ne répond pas.';

  // Les voyants reprennent le rythme réel, pas un niveau échantillonné.
  [['bG', 0], ['bY', 1], ['bR', 2]].forEach(([id, i]) => {
    const el = $(id);
    el.className = `bulb ${'gyr'[i]} p${d.led[i]}` + (d.led[i] ? ' on' : '');
  });
  $('bZ').className = `spk p${d.buzz}` + (d.buzz ? ' on' : '');
  $('bZt').textContent = !d.hasBuzzer ? 'absent'
    : { 0: 'silencieux', 1: 'actif', 4: 'alarme', 5: 'chirp', 6: 'décompte' }[d.buzz] || '—';

  // Maintenance
  const m = d.maint || {};
  const open = !!m.on;
  $('maintTag').textContent = open ? 'déverrouillé' : m.locked ? `verrouillé ${m.locked}s` : 'verrouillé';
  $('maintTag').className = 'tag ' + (open ? 'open' : 'lock');
  $('maintBtn').textContent = open ? 'Verrouiller' : 'Déverrouiller';
  $('maintPin').hidden = open;
  $('maintPin').disabled = open || !!m.locked;
  $('unlocked').hidden = !open;
  $('testPanel').hidden = !open;
  $('maintHint').textContent = !m.usable
    ? "Aucun code n'est configuré sur le boîtier : la maintenance à distance est refusée (MAINT_PIN dans secrets.h)."
    : m.locked ? `Trop d'essais : réessayez dans ${m.locked} s.`
    : open ? "Les alarmes sont coupées et les commandes acceptées. Le boîtier se ré-arme seul."
    : "Le boîtier refuse toute commande tant qu'il n'est pas en maintenance.";

  // Pilotage
  for (const b of $('segMode').querySelectorAll('[data-mode]')) {
    press(b, (b.dataset.mode === 'manual') === !!d.manual);
  }
  const fname = { '-1': 'none', 1: 'armed', 2: 'entry', 3: 'alarm' }[String(d.forced)] || 'none';
  for (const b of $('segForce').querySelectorAll('[data-force]')) press(b, b.dataset.force === fname);
  document.querySelectorAll('[data-led]').forEach((b) => {
    press(b, !!(d.manLed && d.manLed[{ vert: 0, jaune: 1, rouge: 2 }[b.dataset.led]]));
  });
  press($('bzToggle'), !!d.manBuzz);
  press($('muteBtn'), !!d.muted);
  press($('sniffBtn'), !!(d.pres && d.pres.on));

  // Tuiles
  if (d.imu && d.imu.ok) {
    $('vTamper').textContent = d.imu.tilt == null ? '—' : d.imu.tilt.toFixed(1);
    $('sTamper').textContent = d.imu.ref
      ? `seuil ${d.imu.warnDeg}° · choc ${d.imu.shock == null ? '—' : d.imu.shock.toFixed(2)} g`
      : 'référence en cours';
    setTile('tileTamper', d.imu.tamper ? 'crit' : 'ok');
  } else {
    $('vTamper').textContent = '—';
    $('sTamper').textContent = 'MPU-6500 absent';
    setTile('tileTamper', 'warn');
  }

  if (d.pres) {
    $('vPres').textContent = d.pres.n;
    $('sPres').textContent = d.pres.learn
      ? `apprentissage ${d.pres.windows}/5`
      : `habituel ${d.pres.ambient != null ? d.pres.ambient.toFixed(1) : (d.pres.amb || 0).toFixed(1)} · écart ${(d.pres.excess >= 0 ? '+' : '')}${d.pres.excess.toFixed(1)}`;
    setTile('tilePres', d.pres.warn ? 'warn' : 'ok');
  } else {
    $('vPres').textContent = '—';
    $('sPres').textContent = 'sniffer désactivé';
  }

  if (d.gas) {
    $('sGaz').textContent = d.gas.warming
      ? `${d.gas.v.toFixed(2)} V · préchauffage`
      : `${d.gas.v.toFixed(2)} V · écart ${(d.gas.delta >= 0 ? '+' : '')}${(d.gas.delta || 0).toFixed(2)} V · marge ${(d.gas.ceil - d.gas.base).toFixed(2)} V`;
    setTile('tileGaz', d.gas.crit || d.gas.fast ? 'crit' : d.gas.warn ? 'warn' : 'ok');
  }
  if (!d.dhtOk) { setTile('tileTemp', 'warn'); $('sTemp').textContent = 'DHT22 — aucune réponse'; }
}

/* ---------- auto-test : rendu ---------- */
const RES = ['', 'running', 'pass', 'fail', 'info', 'look', 'skip'];
const RLBL = ['—', 'EN COURS', 'OK', 'ÉCHEC', 'INFO', 'À VÉRIFIER', 'SAUTÉ'];
let lastSteps = '';

function onTestMeta(raw) { try { BOX.meta = JSON.parse(raw); paintTest(); } catch {} }
function onTest(raw) { try { BOX.test = JSON.parse(raw); paintTest(); } catch {} }

function paintTest() {
  const t = BOX.test, M = BOX.meta;
  if (!t || !M) return;
  const S = M.steps, i = t.step, st = S[i] || {};

  $('tStart').textContent = t.active ? 'Redémarrer' : 'Démarrer';
  $('tPause').textContent = t.paused ? 'Reprendre' : 'Pause';
  for (const id of ['tPause', 'tStop', 'tPrev', 'tNext']) $(id).disabled = !t.active;
  press($('tLoop'), t.loop); press($('tWait'), t.wait);

  const frac = t.active ? Math.min(1, t.elapsed / Math.max(1, t.dur)) : 0;
  $('tBar').style.width = (t.active ? (i + frac) / t.total * 100 : t.done ? 100 : 0) + '%';
  $('tPos').textContent = t.active ? `Étape ${i + 1} sur ${t.total}${t.loop ? ` · passe ${t.passes + 1}` : ''}`
    : t.done ? 'Terminé' : 'Inactif';
  $('tName').textContent = t.active ? st.n : (t.done ? `${t.passes} passe(s) terminée(s)` : 'Appuyez sur Démarrer');
  $('tHint').textContent = t.active ? st.h : '';
  $('tLeft').textContent = !t.active ? '' : t.paused ? 'En pause'
    : t.hold ? 'En attente de votre réponse…'
    : `${Math.max(0, (t.dur - t.elapsed) / 1000).toFixed(1)} s restantes`;
  $('tAsk').hidden = !(t.active && st.v && (t.res[i] === 1 || t.res[i] === 5));

  const c = [0, 0, 0, 0, 0, 0, 0];
  t.res.forEach((r) => c[r]++);
  $('tSum').innerHTML = (c[2] ? `<span class="chip pass">${c[2]} OK</span>` : '')
    + (c[3] ? `<span class="chip fail">${c[3]} échec</span>` : '')
    + (c[5] ? `<span class="chip look">${c[5]} à vérifier</span>` : '')
    + (c[6] ? `<span class="chip">${c[6]} sauté</span>` : '');

  // Reconstruit seulement si quelque chose a changé : sinon un clic peut être
  // perdu parce que la liste est remplacée sous le doigt.
  let h = '';
  S.forEach((x, k) => {
    const r = t.res[k];
    h += `<li data-i="${k}" class="${t.active && k === i ? 'cur' : ''}">`
      + `<span class="chip ${RES[r]}">${RLBL[r]}</span>`
      + `<span><b>${k + 1}. ${esc(x.n)}</b><small>${esc(t.det[k] || x.h)}</small></span>`
      + (x.v && r === 5 ? `<span class="yn"><button class="btn yes" data-c="${k}" data-ok="1">Oui</button>`
          + `<button class="btn no" data-c="${k}" data-ok="0">Non</button></span>` : '<span></span>')
      + '</li>';
  });
  if (h !== lastSteps) { $('tList').innerHTML = h; lastSteps = h; }
}

function esc(s) {
  return String(s).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
}

/* =====================================================================
 * Reconnaissance faciale : personnes autorisées et captures
 *
 * Le service vision expose son API derrière /video/ (proxifié par nginx).
 * L'ajout et la suppression sont réservés à la maintenance : décider qui est
 * autorisé est un contrôle de sécurité, pas un réglage d'affichage.
 * ===================================================================== */

const VAPI = '/video/api';
let faceBusy = false;

function maintOpen() {
  return !!(BOX.state && BOX.state.maint && BOX.state.maint.on);
}

async function vget(path) {
  const r = await fetch(VAPI + path, { cache: 'no-store' });
  if (!r.ok) throw new Error(`HTTP ${r.status}`);
  return r.json();
}

/* ---------- personnes autorisées ---------- */
async function loadPeople() {
  const ul = $('people');
  try {
    const { people } = await vget('/faces');
    $('faceTag').textContent = people.length
      ? `${people.length} autorisée${people.length > 1 ? 's' : ''}` : 'aucune';
    $('faceTag').className = 'tag' + (people.length ? ' ok' : '');
    ul.innerHTML = people.length ? people.map((p) => `
      <li>
        <img src="${VAPI}/faces/thumb?id=${encodeURIComponent(p.id)}" alt="" loading="lazy">
        <span class="nm">${esc(p.name)}</span>
        <span class="meta">${p.samples} vue${p.samples > 1 ? 's' : ''}</span>
        <button class="btn danger" data-del="${esc(p.id)}" ${maintOpen() ? '' : 'disabled'}>Retirer</button>
      </li>`).join('')
      : '<li class="empty">Personne enregistrée. Tout visage sera signalé comme inconnu.</li>';
  } catch (e) {
    $('faceTag').textContent = 'service hors ligne';
    $('faceTag').className = 'tag lock';
    ul.innerHTML = '<li class="empty">Service vision injoignable (conteneur « stream » démarré ?)</li>';
  }
}

$('faceForm').addEventListener('submit', async (e) => {
  e.preventDefault();
  if (faceBusy) return;
  if (!maintOpen()) { toast('Passez en maintenance pour modifier les autorisations'); return; }
  const name = $('faceName').value.trim();
  if (!name) { toast('Entrez un nom'); return; }

  faceBusy = true;
  $('faceAdd').disabled = true;
  $('faceAdd').textContent = 'Lecture du visage…';
  try {
    const r = await fetch(`${VAPI}/faces?name=${encodeURIComponent(name)}`, { method: 'POST' });
    const j = await r.json();
    if (!r.ok || j.ok === false) { toast(j.err || `Échec (HTTP ${r.status})`); }
    else { toast(`${name} enregistré`); $('faceName').value = ''; loadPeople(); }
  } catch (err) {
    toast('Service vision injoignable');
  } finally {
    faceBusy = false;
    $('faceAdd').disabled = false;
    $('faceAdd').textContent = 'Enregistrer le visage';
  }
});

$('people').addEventListener('click', async (e) => {
  const b = e.target.closest('button[data-del]');
  if (!b) return;
  if (!maintOpen()) { toast('Passez en maintenance pour modifier les autorisations'); return; }
  const li = b.closest('li');
  const who = li ? li.querySelector('.nm').textContent : 'cette personne';
  if (!confirm(`Retirer ${who} des personnes autorisées ?`)) return;
  try {
    const r = await fetch(`${VAPI}/faces?id=${encodeURIComponent(b.dataset.del)}`, { method: 'DELETE' });
    if (r.ok) { toast('Retiré'); loadPeople(); } else { toast('Échec de la suppression'); }
  } catch { toast('Service vision injoignable'); }
});

/* ---------- captures ---------- */
const SHOT_LABEL = { detection: 'détection', alarme: 'ALARME',
                     identification: 'identifié', 'visage-inconnu': 'inconnu' };

async function loadShots() {
  const box = $('shots');
  try {
    const { events } = await vget('/events');
    $('shotTag').textContent = events.length ? `${events.length}` : 'aucune';
    box.innerHTML = events.length ? events.map((ev) => {
      const t = new Date(ev.ts * 1000).toLocaleTimeString('fr-FR', { hour12: false });
      return `<figure class="${esc(ev.reason)}" data-file="${esc(ev.file)}" data-detail="${esc(ev.detail || '')}" data-t="${t}">
          <img src="${VAPI}/events/img?file=${encodeURIComponent(ev.file)}" alt="" loading="lazy">
          <figcaption>${SHOT_LABEL[ev.reason] || esc(ev.reason)} ${t}</figcaption>
        </figure>`;
    }).join('') : '<p class="empty">Aucune capture.</p>';
  } catch {
    $('shotTag').textContent = 'hors ligne';
    box.innerHTML = '<p class="empty">Service vision injoignable.</p>';
  }
}

$('shots').addEventListener('click', (e) => {
  const fig = e.target.closest('figure');
  if (!fig) return;
  const box = document.createElement('div');
  box.id = 'lightbox';
  box.innerHTML = `<div><img src="${VAPI}/events/img?file=${encodeURIComponent(fig.dataset.file)}" alt="">
      <p>${esc(fig.dataset.t)} — ${esc(fig.dataset.detail || fig.className)}</p></div>`;
  box.onclick = () => box.remove();
  document.body.appendChild(box);
});

// Rafraîchissement : les personnes changent rarement, les captures souvent.
setInterval(() => { if (!$('app').hidden) loadShots(); }, 5000);
setInterval(() => { if (!$('app').hidden) loadPeople(); }, 15000);
loadPeople(); loadShots();

/* =====================================================================
 * Réglages de la caméra — appliqués à chaud
 *
 * Évite d'avoir à rebuilder l'image ou éditer un fichier pour ajuster un
 * seuil : tout se règle ici, et prend effet immédiatement.
 * ===================================================================== */

const CFG_META = {
  detect_confidence: {
    label: 'Seuil de détection',
    why: "À partir de quand une forme est considérée comme un visage. Trop haut : les profils et les visages mal éclairés sont manqués. Trop bas : n'importe quelle forme passe pour un visage.",
    step: 0.01,
  },
  match_threshold: {
    label: 'Seuil de reconnaissance',
    why: "À partir de quand un visage est reconnu comme une personne enregistrée. Trop bas : des inconnus passent pour autorisés.",
    step: 0.005,
  },
  known_grace_seconds: {
    label: 'Délai de grâce',
    why: "Une fois reconnue, une personne reste considérée présente pendant ce temps même si elle tourne la tête — un profil ne ressemble pas à une vue de face. À 0, chaque mouvement de tête relance un décompte d'alarme.",
    step: 1,
  },
  unknown_confirm: {
    label: 'Relevés avant « inconnu »',
    why: "Nombre de relevés consécutifs sans visage connu avant de déclarer un inconnu. Lisse les images ratées.",
    step: 1,
  },
  check_every_n: {
    label: 'Analyser 1 image sur',
    why: "Plus bas = plus réactif mais plus de charge processeur.",
    step: 1,
  },
  require_liveness: {
    label: 'Exiger la preuve de vivacité',
    why: "À 1, un visage reconnu ne vaut autorisation qu'après avoir tourné la tête : une photo imprimée de la bonne personne est refusée. À 0, la vivacité est seulement mesurée et affichée, sans rien bloquer — à utiliser si le défi gêne une démonstration.",
    step: 1,
  },
  turn_required: {
    label: 'Amplitude de rotation exigée',
    why: "Asymétrie du nez à atteindre de chaque côté. Mesuré : une photo qu'on agite plafonne vers 0,13 ; un vrai visage tourné de 30° atteint 0,40. Descendre sous 0,20 laisse passer des photos.",
    step: 0.01,
  },
  challenge_seconds: {
    label: 'Temps pour le défi',
    why: "Durée accordée pour tourner la tête. Doit rester ≤ au délai d'identification du boîtier (5 s), sinon l'alarme sonne pendant que la personne obéit encore.",
    step: 0.5,
  },
  hold_seconds: {
    label: 'Vivacité acquise pendant',
    why: "Une fois le défi réussi, la personne reste « vivante » ce temps-là sans devoir recommencer.",
    step: 5,
  },
  min_iod_px: {
    label: 'Taille minimale du visage',
    why: "Écart entre les yeux, en pixels, sous lequel on ne tranche pas (on affiche « approchez-vous »). Mesuré : à 45 px, 5 % des attaques passent ; à 75 px, 0,2 %. C'est le réglage qui améliore sécurité ET confort en même temps.",
    step: 5,
  },
  smooth: {
    label: 'Lissage du défi',
    why: "Nombre de relevés dans la médiane glissante. Plus haut : moins sensible au bruit, mais la médiane suit moins bien un mouvement rapide.",
    step: 1,
  },
  sustain: {
    label: 'Relevés à maintenir',
    why: "Combien de relevés consécutifs la rotation doit tenir. Mesuré : à 3, les faux refus d'un vrai visage passent de 0,6 % à 6 % sans réduire les attaques. Laisser à 1.",
    step: 1,
  },
};
const CFG_DEFAULTS = { detect_confidence: 0.60, match_threshold: 0.363,
                       known_grace_seconds: 20, unknown_confirm: 3, check_every_n: 5 };
let cfgLimits = {};

async function loadSettings() {
  const box = $('settings');
  try {
    const { settings, limits } = await vget('/config');
    cfgLimits = limits;
    const open = maintOpen();
    $('cfgTag').textContent = open ? 'modifiable' : 'maintenance requise';
    $('cfgTag').className = 'tag ' + (open ? 'open' : 'lock');
    box.innerHTML = Object.entries(settings).map(([k, v]) => {
      const m = CFG_META[k] || { label: k, why: '', step: 0.01 };
      const [lo, hi] = limits[k] || [0, 1];
      return `<div class="setting" data-k="${k}">
          <label for="s-${k}">${esc(m.label)}</label>
          <p class="why">${esc(m.why)}</p>
          <div class="ctl">
            <input type="range" id="s-${k}" min="${lo}" max="${hi}" step="${m.step}"
                   value="${v}" ${open ? '' : 'disabled'}>
            <output for="s-${k}">${v}</output>
          </div>
        </div>`;
    }).join('');
  } catch {
    $('cfgTag').textContent = 'hors ligne';
    $('cfgTag').className = 'tag lock';
    box.innerHTML = '<p class="empty">Service vision injoignable.</p>';
  }
}

let cfgTimer;
$('settings').addEventListener('input', (e) => {
  const inp = e.target;
  if (inp.type !== 'range') return;
  const card = inp.closest('.setting');
  card.querySelector('output').textContent = inp.value;
  card.classList.add('changed');
  // Debounce : un glissement de curseur ne doit pas envoyer 40 requêtes.
  clearTimeout(cfgTimer);
  cfgTimer = setTimeout(async () => {
    try {
      const r = await fetch(`${VAPI}/config?${card.dataset.k}=${encodeURIComponent(inp.value)}`,
                            { method: 'POST' });
      const j = await r.json();
      if (!r.ok || j.ok === false) { toast(j.err || 'Réglage refusé'); }
      else { card.classList.remove('changed'); toast(`${CFG_META[card.dataset.k].label} : ${inp.value}`); }
    } catch { toast('Service vision injoignable'); }
  }, 400);
});

$('cfgReset').onclick = async () => {
  if (!maintOpen()) { toast('Passez en maintenance'); return; }
  const qs = Object.entries(CFG_DEFAULTS).map(([k, v]) => `${k}=${v}`).join('&');
  try {
    const r = await fetch(`${VAPI}/config?${qs}`, { method: 'POST' });
    if (r.ok) { toast('Valeurs par défaut restaurées'); loadSettings(); }
  } catch { toast('Service vision injoignable'); }
};

setInterval(() => { if (!$('app').hidden) loadSettings(); }, 20000);

/* =====================================================================
 * VIVACITÉ
 *
 * La consigne du défi s'affiche aussi SUR l'image de la caméra, parce que
 * la personne qui s'identifie regarde la caméra et pas cet écran. Ici c'est
 * pour l'opérateur : il voit où en est le défi et peut le relancer.
 * ===================================================================== */
async function loadLiveness() {
  const box = $('liveBox');
  try {
    const d = await vget('/liveness');
    const on = !!d.settings.require_liveness;
    $('liveTag').textContent = on ? 'exigée' : 'mesure seule';
    $('liveTag').className = 'tag ' + (on ? 'ok' : '');
    if (!d.tracks.length) {
      box.innerHTML = '<p class="empty">Aucun visage suivi.</p>';
      return;
    }
    box.innerHTML = d.tracks.map((t) => {
      const c = t.challenge;
      const etat = t.etat === 'vivant' ? '<b class="ok">vivante</b>'
                 : t.etat === 'echec' ? '<b class="err">défi échoué</b>'
                 : '<b>en attente</b>';
      const loin = t.trop_loin
        ? `<p class="why">Trop loin : ${t.inter_oculaire} px entre les yeux, il en faut ${d.settings.min_iod_px}. Approchez-vous.</p>`
        : '';
      const prog = c
        ? `<p class="why">${esc(c.consigne)} — atteint ${c.atteint[0]} / ${c.atteint[1]}, il faut ±${c.requis}${c.restant ? ` (${c.restant} s)` : ''}</p>`
        : '';
      const raison = c && c.raison ? `<p class="why">${esc(c.raison)}</p>` : '';
      return `<div class="setting">
        <label>Visage suivi — ${etat}${t.tient_jusqua ? ` (encore ${t.tient_jusqua} s)` : ''}</label>
        ${loin}${prog}${raison}
        <p class="why">Netteté ${t.nettete ?? '—'} · moiré ${t.moire ?? '—'} <em>(indicatifs, non décisifs)</em></p>
      </div>`;
    }).join('');
  } catch {
    $('liveTag').textContent = 'hors ligne';
    $('liveTag').className = 'tag lock';
    box.innerHTML = '<p class="empty">Service vision injoignable.</p>';
  }
}

$('liveRetry').addEventListener('click', async () => {
  try {
    await fetch(`${VAPI}/liveness/retry`, { method: 'POST' });
    toast('Nouveau défi demandé');
    loadLiveness();
  } catch { toast('Service vision injoignable'); }
});

setInterval(() => { if (!$('app').hidden) loadLiveness(); }, 1000);
loadSettings();

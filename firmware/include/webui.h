// The dashboard the board serves itself, held in PROGMEM so it costs no heap
// until a browser asks for it.
//
// NO EXTERNAL RESOURCES. Not one. Clients attached to the Sentinel-X AP have no
// route to the internet, so a CDN font or script does not degrade — it hangs the
// page load until the browser gives up. Everything is inline.
//
// This is the board's own console: a threat banner, the three physical LEDs
// mirrored on screen, and the raw sensor/presence figures. The full supervision
// dashboard with the webcam feed is the separate one in ../dashboard, served by
// nginx off the server PC.
#pragma once

#include <Arduino.h>

static const char DASHBOARD_HTML[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<title>Sentinel-X Console</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
:root{
--bg:#0A0E14;--panel:#121820;--edge:#1E2733;--ink:#D4DCE6;--dim:#6B7A8C;
--grn:#2ECC71;--ylw:#F5C518;--red:#FF4B4B;--cyn:#38BDF8;
--mono:ui-monospace,SFMono-Regular,"SF Mono",Menlo,Consolas,monospace;
}
body{background:var(--bg);color:var(--ink);font-family:var(--mono);font-size:14px;line-height:1.5;padding:14px;-webkit-font-smoothing:antialiased}
.wrap{max-width:900px;margin:0 auto}
header{display:flex;align-items:baseline;gap:10px;flex-wrap:wrap;margin-bottom:14px}
h1{font-size:15px;letter-spacing:.18em;text-transform:uppercase;font-weight:700}
.dot{width:7px;height:7px;border-radius:50%;background:var(--dim);display:inline-block}
.dot.on{background:var(--grn);box-shadow:0 0 8px var(--grn)}
.meta{color:var(--dim);font-size:11.5px;margin-left:auto}

/* threat banner */
.banner{border:1px solid var(--edge);border-left-width:4px;border-radius:4px;padding:14px 16px;margin-bottom:14px;background:var(--panel);transition:border-color .25s}
.banner .state{font-size:22px;font-weight:700;letter-spacing:.06em}
.banner .why{color:var(--dim);font-size:12px;margin-top:4px;min-height:1.5em}
.banner[data-s="green"]{border-left-color:var(--grn)}
.banner[data-s="green"] .state{color:var(--grn)}
.banner[data-s="yellow"]{border-left-color:var(--ylw)}
.banner[data-s="yellow"] .state{color:var(--ylw)}
.banner[data-s="red"]{border-left-color:var(--red)}
.banner[data-s="red"] .state{color:var(--red)}
.banner[data-s="red"]{animation:pulse 1.1s ease-in-out infinite}
@keyframes pulse{50%{background:#1A1012}}

/* physical LED mirror */
.leds{display:flex;gap:18px;margin-top:12px;padding-top:12px;border-top:1px solid var(--edge)}
.led{display:flex;align-items:center;gap:7px;font-size:11px;color:var(--dim);text-transform:uppercase;letter-spacing:.08em}
.bulb{width:12px;height:12px;border-radius:50%;background:#1B2430;border:1px solid var(--edge)}
.led[data-on="1"]{color:var(--ink)}
.led.g[data-on="1"] .bulb{background:var(--grn);box-shadow:0 0 10px var(--grn)}
.led.y[data-on="1"] .bulb{background:var(--ylw);box-shadow:0 0 10px var(--ylw)}
.led.r[data-on="1"] .bulb{background:var(--red);box-shadow:0 0 10px var(--red)}

/* tiles */
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(168px,1fr));gap:10px}
.tile{background:var(--panel);border:1px solid var(--edge);border-radius:4px;padding:11px 13px}
.tile h2{font-size:10.5px;font-weight:600;color:var(--dim);text-transform:uppercase;letter-spacing:.11em;margin-bottom:6px}
.v{font-size:25px;font-weight:600;letter-spacing:-.01em}
.v u{font-size:12px;font-weight:400;color:var(--dim);text-decoration:none;margin-left:3px}
.pin{color:var(--dim);font-size:10.5px;margin-top:5px}
.flag{display:inline-block;font-size:10px;padding:1px 6px;border-radius:3px;margin-top:6px;background:#1B2430;color:var(--dim);text-transform:uppercase;letter-spacing:.07em}
.flag.warm{background:#2A2410;color:var(--ylw)}
.flag.bad{background:#2A1416;color:var(--red)}
.flag.ok{background:#102A1C;color:var(--grn)}
.t-red{color:var(--red)}.t-ylw{color:var(--ylw)}.t-grn{color:var(--grn)}.t-dim{color:var(--dim)}

/* presence */
.pres{background:var(--panel);border:1px solid var(--edge);border-radius:4px;padding:13px;margin-top:10px}
.bar{height:5px;background:#1B2430;border-radius:3px;overflow:hidden;margin:9px 0}
.bar i{display:block;height:100%;width:0;background:var(--cyn);transition:width .4s,background .4s}
.split{display:flex;gap:16px;flex-wrap:wrap;font-size:11.5px;color:var(--dim)}
.split b{color:var(--ink);font-weight:600}
.note{color:var(--dim);font-size:11px;margin-top:9px;border-top:1px solid var(--edge);padding-top:9px}
footer{color:var(--dim);font-size:11px;margin-top:14px;display:flex;gap:14px;flex-wrap:wrap}
</style>
</head>
<body>
<div class="wrap">

<header>
  <span class="dot" id="live"></span>
  <h1>Sentinel-X</h1>
  <span class="meta" id="meta">connecting</span>
</header>

<div class="banner" id="banner" data-s="green">
  <div class="state" id="state">...</div>
  <div class="why" id="why"></div>
  <div class="leds">
    <span class="led g" id="Lg" data-on="0"><i class="bulb"></i>green · validation</span>
    <span class="led y" id="Ly" data-on="0"><i class="bulb"></i>yellow · early warning</span>
    <span class="led r" id="Lr" data-on="0"><i class="bulb"></i>red · intruder</span>
  </div>
</div>

<div class="grid">
  <div class="tile">
    <h2>Temperature</h2>
    <div class="v" id="t">--<u>&deg;C</u></div>
    <div class="pin">DHT22 &middot; D2 / GPIO4</div>
    <div id="tf"></div>
  </div>
  <div class="tile">
    <h2>Humidity</h2>
    <div class="v" id="h">--<u>%RH</u></div>
    <div class="pin">DHT22 &middot; same data line</div>
  </div>
  <div class="tile">
    <h2>Motion</h2>
    <div class="v" id="pir">--</div>
    <div class="pin">HC-SR501 &middot; D1 / GPIO5</div>
    <div id="pirf"></div>
  </div>
  <div class="tile">
    <h2>Gas</h2>
    <div class="v" id="gas">--<u>V</u></div>
    <div class="pin" id="gasd">A0 &middot; 68k+68k divider</div>
    <div id="gasf"></div>
  </div>
  <div class="tile">
    <h2>Tamper</h2>
    <div class="v" id="tilt">--<u>&deg;</u></div>
    <div class="pin" id="imup">MPU-6500 &middot; D6/D7</div>
    <div id="imuf"></div>
  </div>
  <div class="tile">
    <h2>Uplink</h2>
    <div class="v" id="mq">--</div>
    <div class="pin" id="mqp">MQTT broker</div>
  </div>
</div>

<div class="pres">
  <h2 style="font-size:10.5px;font-weight:600;color:var(--dim);text-transform:uppercase;letter-spacing:.11em">
    WiFi presence &middot; score <span id="ps" style="color:var(--ink)">--</span>
  </h2>
  <div class="bar"><i id="pbar"></i></div>
  <div class="split">
    <span>joined <b id="pa">--</b></span>
    <span>sniffed <b id="pt">--</b></span>
    <span>randomized <b id="pr">--</b></span>
    <span>stable <b id="pst">--</b></span>
    <span>strongest <b id="prs">--</b> dBm</span>
    <span>frames <b id="pf">--</b></span>
  </div>
  <div class="note" id="pnote"></div>
</div>

<footer>
  <span>uptime <b id="up">--</b></span>
  <span>heap <b id="heap">--</b></span>
  <span>ap <b id="ip">--</b></span>
  <span><a href="/api/readings" style="color:var(--cyn)">/api/readings</a></span>
</footer>

</div>
<script>
var $=function(i){return document.getElementById(i)};
var misses=0;

function n(v,d,u){return v===null||v===undefined?'--':(typeof v==='number'?v.toFixed(d):v)+(u?'<u>'+u+'</u>':'')}
function secs(s){var h=Math.floor(s/3600),m=Math.floor(s%3600/60);return h?h+'h '+m+'m':(m?m+'m '+(s%60)+'s':s+'s')}
function flag(el,cls,txt){el.innerHTML=txt?'<span class="flag '+cls+'">'+txt+'</span>':''}

function paint(d){
  misses=0;
  $('live').className='dot on';
  $('meta').textContent=d.mode==='apsta'?'AP + station':'AP only';

  // threat state
  var b=$('banner');
  b.dataset.s=d.threat;
  $('state').textContent={green:'ALL CLEAR',yellow:'EARLY WARNING',red:'INTRUDER'}[d.threat]||d.threat;
  $('why').textContent=d.reason||'No anomaly on any channel.';
  $('Lg').dataset.on=d.led.green?1:0;
  $('Ly').dataset.on=d.led.yellow?1:0;
  $('Lr').dataset.on=d.led.red?1:0;

  // climate
  $('t').innerHTML=n(d.tempC,1,'&deg;C');
  $('h').innerHTML=n(d.humidity,0,'%RH');
  flag($('tf'),'bad',d.tempC===null?'sensor silent':'');

  // motion
  $('pir').innerHTML=d.motion?'<span class="t-red">DETECTED</span>':'<span class="t-dim">clear</span>';
  flag($('pirf'),'warm',d.pirWarming?'warming up':'');

  // gas
  $('gas').innerHTML=n(d.gasVolts,2,'V');
  $('gasd').innerHTML=d.gasDelta===null?'baseline not set yet':'&Delta; '+d.gasDelta.toFixed(2)+' V vs baseline &middot; raw '+d.gasRaw+'/1023';
  flag($('gasf'),d.gasWarming?'warm':(d.gasCrit?'bad':(d.gasWarn?'warm':'')),
       d.gasWarming?'heater warming':(d.gasCrit?'critical':(d.gasWarn?'elevated':'')));

  // tamper
  if(d.imuOk){
    $('tilt').innerHTML='p '+n(d.pitch,0,'')+' / r '+n(d.roll,0,'&deg;');
    $('imup').innerHTML='MPU-6500 &middot; '+d.imuMag.toFixed(2)+' g';
    flag($('imuf'),d.tampered?'bad':'ok',d.tampered?'tampered':'level');
  }else{
    $('tilt').innerHTML='<span class="t-dim">n/a</span>';
    $('imup').textContent='not responding on D6/D7';
    flag($('imuf'),'bad','absent');
  }

  // uplink
  if(!d.mqttEnabled){$('mq').innerHTML='<span class="t-dim">off</span>';$('mqp').textContent='built standalone';}
  else{
    $('mq').innerHTML=d.mqttUp?'<span class="t-grn">ONLINE</span>':'<span class="t-red">DOWN</span>';
    $('mqp').textContent=(d.mqttTls?'TLS ':'plain ')+d.mqttHost+':'+d.mqttPort+(d.mqttUp?'':' · state '+d.mqttState);
  }

  // presence
  var p=d.presence;
  $('ps').textContent=p.score;
  $('pa').textContent=p.associated;
  $('pt').textContent=p.sniffed;
  $('pr').textContent=p.randomized;
  $('pst').textContent=p.stable;
  $('prs').textContent=p.strongestRssi||'--';
  $('pf').textContent=p.frames;
  var pct=Math.min(100,p.score/p.critScore*100);
  var bar=$('pbar');bar.style.width=pct+'%';
  bar.style.background=p.score>=p.critScore?'var(--red)':(p.score>=p.warnScore?'var(--ylw)':'var(--cyn)');
  $('pnote').textContent=p.sniffer
    ?'Randomized addresses inflate the sniffed count, so treat it as an activity level rather than a headcount. Next sniff window in '+secs(p.nextSniffS)+'; the AP drops for '+(p.windowMs/1000)+'s each time.'
    :'Sniffer disabled — only devices joined to this access point are counted.';

  // footer
  $('up').textContent=secs(d.uptime);
  $('heap').textContent=(d.heap/1024).toFixed(1)+' kB'+(d.heap<8192?' (LOW)':'');
  $('heap').className=d.heap<8192?'t-red':'';
  $('ip').textContent=d.apIp;
}

function poll(){
  fetch('/api/readings',{cache:'no-store'})
    .then(function(r){return r.json()}).then(paint)
    .catch(function(){
      // A sniff window takes the AP down, so a couple of misses is expected.
      if(++misses>2){$('live').className='dot';$('meta').textContent='link lost (sniff window?)'}
    });
}
poll();setInterval(poll,2000);
</script>
</body>
</html>)HTML";

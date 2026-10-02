#pragma once

// Web pages of the WiFi-Cam-Proxy (included by http.cpp only).
// No external resources (fonts, CDNs): the device usually has no internet access.

// --- Shared head, style and navigation -------------------------------------------
#define PAGE_STYLE                                                                              \
  "<meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"  \
  "<meta name='color-scheme' content='dark'><link rel='stylesheet' href='/style.css'>"

// Shared style sheet (/style.css), loaded by every page
static const char STYLE_CSS[] = R"CSS(
:root{--bg:#0e1015;--card:#171a21;--card2:#1e222b;--line:#2a2f3a;--text:#e7e9ee;--muted:#8b93a5;--accent:#4c9dff;--ok:#3fb950;--warn:#d6a03a;--bad:#f2665c;--r:12px}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--text);font:15px/1.5 system-ui,-apple-system,'Segoe UI',Roboto,sans-serif}
a{color:var(--accent);text-decoration:none}
a:hover{text-decoration:underline}
header{position:sticky;top:0;z-index:5;display:flex;flex-wrap:wrap;align-items:center;gap:4px 16px;padding:10px 16px;background:rgba(14,16,21,.92);border-bottom:1px solid var(--line);backdrop-filter:blur(6px)}
.brand{font-weight:650;letter-spacing:.2px;display:flex;align-items:center;gap:8px}
.brand i{width:9px;height:9px;border-radius:50%;background:var(--ok);box-shadow:0 0 8px var(--ok)}
nav{display:flex;flex-wrap:wrap;gap:2px}
nav a{color:var(--muted);padding:5px 10px;border-radius:8px}
nav a:hover{color:var(--text);background:var(--card2);text-decoration:none}
nav a.on{color:var(--text);background:var(--card2)}
main{max-width:640px;margin:0 auto;padding:16px}
main.wide{max-width:960px}
h1{font-size:1.35rem;margin:6px 0 14px}
h2{font-size:1.02rem;margin:0 0 10px}
.card{background:var(--card);border:1px solid var(--line);border-radius:var(--r);padding:16px;margin:0 0 14px}
.muted{color:var(--muted)}
.small{font-size:.85rem}
.ok{color:var(--ok)}
.warnc{color:var(--warn)}
.bad{color:var(--bad)}
.note{border-radius:var(--r);padding:10px 14px;margin:0 0 14px;border:1px solid #6b5420;background:#2b2412}
.btn,button{font:inherit;color:var(--text);background:var(--card2);border:1px solid var(--line);border-radius:9px;padding:7px 14px;cursor:pointer;display:inline-flex;align-items:center;gap:6px;margin:3px 2px}
.btn:hover,button:hover{border-color:#3d4452;text-decoration:none}
button:disabled{opacity:.45;cursor:default}
.primary{background:var(--accent);border-color:var(--accent);color:#06101f;font-weight:600}
input,select{font:inherit;color:var(--text);background:var(--bg);border:1px solid var(--line);border-radius:9px;padding:8px 10px;margin:4px 0}
input:focus,select:focus{outline:2px solid var(--accent);outline-offset:-1px}
.field{width:100%}
.switch{display:inline-flex;align-items:center;gap:8px;margin:3px 8px 3px 2px;cursor:pointer;white-space:nowrap}
.switch input{appearance:none;width:34px;height:20px;margin:0;padding:0;border-radius:20px;background:var(--line);position:relative;cursor:pointer;border:0}
.switch input:before{content:'';position:absolute;top:3px;left:3px;width:14px;height:14px;border-radius:50%;background:#cfd3dc;transition:.15s}
.switch input:checked{background:var(--accent)}
.switch input:checked:before{left:17px;background:#fff}
progress{width:100%;height:8px;accent-color:var(--accent)}
dl.kv{display:grid;grid-template-columns:max-content 1fr;gap:6px 16px;margin:0}
dl.kv dt{color:var(--muted)}
dl.kv dd{margin:0;overflow-wrap:anywhere}
table{border-collapse:collapse;width:100%}
td,th{border-bottom:1px solid var(--line);padding:8px 6px;text-align:left}
th{color:var(--muted);font-weight:500;font-size:.85rem}
.badge{display:inline-block;padding:1px 9px;border-radius:20px;background:var(--card2);border:1px solid var(--line);font-size:.85rem;color:var(--muted)}
img.round{border-radius:50%}
code{background:var(--card2);padding:1px 5px;border-radius:5px}
)CSS";

#define PAGE_HEAD(title) "<!doctype html><html lang='en'><head><title>" title "</title>" PAGE_STYLE

// Navigation; the link of the current page is highlighted
#define PAGE_NAV                                                                                \
  "<header><span class='brand'><i></i>WiFi-Cam</span><nav>"                                    \
  "<a href='/'>Live</a><a href='/cameras'>Cameras</a>"                                          \
  "<a href='/calibrate' id='calLink'>Calibrate</a><a href='/update'>Status</a>"                 \
  "<a href='/wifi-setup'>Wi-Fi</a></nav></header>"                                              \
  "<script>document.querySelectorAll('nav a').forEach(a=>{"                                    \
  "if(a.getAttribute('href')===location.pathname)a.className='on'})</script>"

// Shared helpers (/app.js): orientation maths for start page and calibration,
// rendering of key/value lists
static const char APP_JS[] = R"JS(
const $=id=>document.getElementById(id);
const esc=t=>String(t).replace(/[&<>"']/g,c=>'&#'+c.charCodeAt(0)+';');
// key/value list: rows=[[key, html value, css class], ...], falsy rows are skipped
function kv(el,rows){el.innerHTML=rows.filter(Boolean).map(r=>'<dt>'+r[0]+'</dt><dd'+(r[2]?' class='+r[2]:'')+'>'+r[1]+'</dd>').join('')}
const norm=a=>((a%360)+540)%360-180;   // angle to -180..180
const store={get(k,d){try{const v=localStorage.getItem(k);return v===null?d:JSON.parse(v)}catch(e){return d}},
  set(k,v){try{localStorage.setItem(k,JSON.stringify(v))}catch(e){}}};

// Calibration (stored on the device): ellipse (offset/scale per axis), optional
// correction table from quarter turns, zero point, smoothing
// base=-90: the camera image is mounted rotated by 90° in the probe and is always
// rotated like this (determined on the device). zero = sensor angle in the normal
// position of the probe; orientation correction additionally compensates deviations.
const DEFAULT_CAL={v:2,base:-90,ox:0,oy:0,sx:1,sy:1,zero:0,smooth:0.6,pts:null};
async function loadCal(){
  try{
    const r=await fetch('/calibration',{cache:'no-store'}), c=await r.json();
    // format 1 had no base rotation, the zero point contained it (-90)
    if(!c.v&&typeof c.zero==='number'){c.zero+=90;c.v=2}
    return Object.assign({},DEFAULT_CAL,c);
  }catch(e){return Object.assign({},DEFAULT_CAL)}
}
async function saveCal(c){
  const r=await fetch('/calibration',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(c)});
  if(!r.ok)throw new Error(await r.text());
}

// raw values -> sensor angle in degrees
function sensorAngle(x,y,c){return Math.atan2((x-c.ox)/c.sx,(y-c.oy)/c.sy)*180/Math.PI}

// correction table pts=[[sensor angle, target angle 0/90/180/270], ...]
function tableInfo(pts){
  if(!pts||pts.length<3)return null;
  const s0=pts[0][0], k=norm(pts[1][0]-s0)>=0?1:-1;
  const u=pts.map(p=>((k*(p[0]-s0))%360+360)%360);
  for(let i=1;i<u.length;i++)if(!(u[i]>u[i-1]))return null;  // must increase in order
  return {s0,k,u,t:pts.map(p=>p[1])};
}
function correct(a,c){
  const ti=tableInfo(c.pts); if(!ti)return a;
  const v=((ti.k*(a-ti.s0))%360+360)%360, U=[...ti.u,360], T=[...ti.t,360];
  let i=0; while(i<U.length-2&&v>U[i+1])i++;
  return ti.s0+ti.k*(T[i]+(v-U[i])/(U[i+1]-U[i])*(T[i+1]-T[i]));
}
// orientation relative to the zero point; the image is rotated by its negative
function probeAngle(x,y,c){return norm(correct(sensorAngle(x,y,c),c)-c.zero)}
// image rotation: always the base rotation, with orientation correction also -angle
function imageRotation(on,sm,c){return c.base-(on&&sm.have?probeAngle(sm.x,sm.y,c):0)}

// smooths the vector instead of the angle (no jump at 180/-180)
class Smoother{
  constructor(){this.x=0;this.y=0;this.have=false}
  add(a,c){
    // probe points steeply up/down -> roll angle undefined, keep the last value
    if(Math.hypot((a.x-c.ox)/c.sx,(a.y-c.oy)/c.sy)<0.25)return false;
    const s=this.have?c.smooth:0;
    this.x=this.x*s+a.x*(1-s); this.y=this.y*s+a.y*(1-s); this.have=true;
    return true;
  }
}
// rotates an element always the shortest way; optionally with zoom and pan
// view={z,px,py}. Calling it without an angle only re-applies view.
function rotator(el,view){
  let shown=0;view=view||{z:1,px:0,py:0};
  return t=>{if(t!==undefined)shown+=norm(t-shown);
    el.style.transform='translate('+view.px+'px,'+view.py+'px) rotate('+shown.toFixed(1)+'deg) scale('+view.z+')'};
}

// camera info (/cameras.json): name, battery, LED, orientation sensor
async function camInfo(){try{return await (await fetch('/cameras.json',{cache:'no-store'})).json()}catch(e){return null}}
function batteryText(c){return c.battery<0?'':'Battery '+c.battery+' %'+(c.charging===1?' (charging?)':'')}

// sensor values from the device (server-sent events), reconnects by itself
function orientation(onSample,onState){
  const go=()=>{
    const es=new EventSource('/orientation');
    es.onopen=()=>onState&&onState(true);
    es.onmessage=e=>onSample(JSON.parse(e.data));
    es.onerror=()=>{onState&&onState(false);es.close();setTimeout(go,2000)};
  };
  go();
}
// adds the OTA password header if one was entered
function otaHeaders(h){h=h||{};const p=$('pw');if(p&&p.value)h['X-OTA-Password']=p.value;return h}
)JS";

static const char INDEX_HTML[] = PAGE_HEAD("WiFi-Cam")
    R"HTML(<style>
#view{display:block;width:max-content;max-width:100%;margin:0 auto;overflow:hidden;line-height:0;
 cursor:zoom-in;border-radius:var(--r);background:#000;box-shadow:0 10px 30px rgba(0,0,0,.45)}
#view.z{cursor:grab;touch-action:none}
#wrap{display:inline-block;transition:transform .12s linear}
#img{max-width:100%;max-height:72vh;min-width:240px;min-height:240px;background:#000}
.camline{display:flex;flex-wrap:wrap;justify-content:space-between;align-items:center;gap:8px;margin:0 0 12px}
.toolbar{display:flex;flex-wrap:wrap;align-items:center;justify-content:center;gap:4px;margin:14px 0 4px}
#ledBtn.on{background:#f5c518;border-color:#f5c518;color:#111}
</style></head><body>)HTML" PAGE_NAV R"HTML(<main class='wide'>
<p id='choose' class='note' hidden>Several cameras found. Please pick one under <a href='/cameras'>Cameras</a>.</p>
<div class='camline'><span id='cam' class='muted'>…</span><span id='bat' class='badge' hidden></span></div>
<div id='view' title='Double-click: zoom'><div id='wrap'><img id='img' src='/stream' alt=''></div></div>
<div class='toolbar'>
<button id='zoom' title='Zoom in, then drag the image with the mouse or a finger'>2&times;</button>
<span id='ori'><label class='switch'><input type='checkbox' id='on'>Correct orientation</label>
<label class='switch'><input type='checkbox' id='round'>Round</label>
<button id='zero'>Current position = up</button></span>
<button id='ledBtn' title='Camera LED on/off' hidden>&#128161; LED</button>
<input type='range' id='ledLvl' min='0' max='100' title='LED brightness (0 = off)' hidden>
<a class='btn' href='/snapshot' download='snapshot.jpg'>&#128247; Snapshot</a>
</div>
<p id='ledMsg' class='muted small' style='text-align:center'></p>
</main>
<script src='/app.js'></script><script>
let cal=Object.assign({},DEFAULT_CAL), hasOri=true;
const view={z:1,px:0,py:0}, sm=new Smoother(), rot=rotator($('wrap'),view);
// zoom 2x: pan the crop by dragging, at most up to the image edge
function setZoom(z){view.z=z;view.px=view.py=0;$('zoom').innerHTML=z>1?'1&times;':'2&times;';$('view').className=z>1?'z':'';rot()}
function clampPan(){
  const mx=(view.z-1)*$('img').clientWidth/2, my=(view.z-1)*$('img').clientHeight/2;
  view.px=Math.max(-mx,Math.min(mx,view.px));view.py=Math.max(-my,Math.min(my,view.py));
}
$('zoom').onclick=()=>setZoom(view.z>1?1:2);
$('view').ondblclick=()=>setZoom(view.z>1?1:2);
let drag=null;
$('view').onpointerdown=e=>{if(view.z>1){drag={x:e.clientX-view.px,y:e.clientY-view.py};$('view').setPointerCapture(e.pointerId);$('wrap').style.transition='none'}};
$('view').onpointermove=e=>{if(drag){view.px=e.clientX-drag.x;view.py=e.clientY-drag.y;clampPan();rot()}};
$('view').onpointerup=$('view').onpointercancel=()=>{drag=null;$('wrap').style.transition=''};
// round crop: display only (the image arrives square), off by default
$('round').checked=store.get('round',false);
function applyRound(){$('img').className=hasOri&&$('round').checked?'round':''}
$('round').onchange=()=>{store.set('round',$('round').checked);applyRound()};
applyRound();
$('on').checked=store.get('on',false);
// without an orientation sensor (e.g. microscope): image unrotated, orientation controls off
function apply(){rot(hasOri?imageRotation($('on').checked,sm,cal):0)}
$('on').onchange=()=>{store.set('on',$('on').checked);apply()};
$('zero').onclick=async()=>{
  if(!sm.have)return;
  cal.zero=correct(sensorAngle(sm.x,sm.y,cal),cal);
  $('on').checked=true;store.set('on',true);apply();
  try{await saveCal(cal)}catch(e){alert('Saving failed: '+e.message)}
};
// LED (i4season command 0x0A, JHCMD 20 02). Shows the state confirmed by the camera
// (JHCMD does not confirm: the state that was sent).
let ledOn=false;
function applyLed(){$('ledBtn').className=ledOn?'on':'';$('ledBtn').title='LED '+(ledOn?'on – click to switch off':'off – click to switch on')}
$('ledBtn').onclick=async()=>{
  const want=!ledOn;
  try{
    const r=await fetch('/led/'+(want?'1':'0'),{method:'POST'});
    $('ledMsg').textContent=r.ok?'LED '+(want?'on':'off')+' sent…':'Error: '+await r.text();
    setTimeout(info,1200);
  }catch(e){$('ledMsg').textContent='Not reachable'}
};
// brightness (dimmable cameras): sent right away while dragging; while a request is
// on its way only the newest value is kept and sent afterwards (no backlog)
let lvlSending=false,lvlNext=null,lvlBusy=false,lvlIdle=0;
async function sendLvl(v){
  if(lvlSending){lvlNext=v;return}
  lvlSending=true;
  try{await fetch('/led/level/'+v,{method:'POST'});ledOn=+v>0;applyLed()}catch(e){}
  lvlSending=false;
  if(lvlNext!==null){const n=lvlNext;lvlNext=null;sendLvl(n)}
}
$('ledLvl').oninput=()=>{lvlBusy=true;clearTimeout(lvlIdle);lvlIdle=setTimeout(()=>lvlBusy=false,1500);
  sendLvl($('ledLvl').value)};
async function info(){
  const c=await camInfo(); if(!c)return;
  $('choose').hidden=c.state!=='choose';
  const name=c.ssid||(c.state==='choose'?'none chosen':'looking for camera…');
  $('cam').textContent=[name,c.product].filter(Boolean).join(' · ');
  $('bat').hidden=c.battery<0;$('bat').textContent=batteryText(c);
  // orientation sensor: i4season reports it in the video header; before the first video
  // data (width 0) the otoscope view stays. MaxSee microscopes have none.
  if(c.state==='connected'){
    const ori=c.orientation||(c.proto==='i4season'&&!c.width);
    if(ori!==hasOri){hasOri=ori;applyRound();$('ori').hidden=!ori;$('calLink').hidden=!ori;apply()}
    if(ori)startOri();
  }
  $('ledBtn').hidden=!c.led_supported;
  $('ledLvl').hidden=!c.led_dimmable;ledFast=!!c.led_dimmable;
  showLed(c.led,c.led_level,c.led_dimmable);
}
// LED state from /cameras.json or /led: button and (dimmable) slider; off = slider at 0
// (the firmware keeps the last level for "LED on")
function showLed(led,level,dimmable){
  if(dimmable&&!lvlBusy)$('ledLvl').value=led===0?0:level;
  if(led>=0&&(led===1)!==ledOn){ledOn=led===1;applyLed()}
}
async function ledPoll(){
  try{const l=await (await fetch('/led',{cache:'no-store'})).json();showLed(l.led,l.level,true)}catch(e){}
}
applyLed();
apply();
// status every 5 s; with a dimmable LED the small /led every second in between, so the
// light button on the device shows up quickly
let ledFast=false;
info();setInterval(info,5000);
setInterval(()=>{if(ledFast&&!document.hidden)ledPoll()},1000);
loadCal().then(c=>{cal=c;apply()});
// The orientation stream (a client task with a 6 KB stack on the device) only once the
// camera turns out to have a sensor: the microscopes have none, and with 720p frames the
// heap is short.
let oriStarted=false;
function startOri(){if(!oriStarted){oriStarted=true;orientation(a=>{if(sm.add(a,cal))apply()})}}
</script></body></html>)HTML";

static const char CALIBRATE_HTML[] = PAGE_HEAD("Calibration")
    R"HTML(<style>
.row{display:flex;flex-wrap:wrap;gap:16px;justify-content:center;align-items:center}
#wrap{transition:transform .12s linear}#wrap img{width:200px;height:200px;background:#000;border-radius:var(--r)}
#wrap img.round{border-radius:50%}
#live[src='']{visibility:hidden}
canvas{background:var(--bg);border-radius:var(--r);max-width:100%;display:block;margin:10px auto}
#cov{display:flex;gap:2px;margin:10px 0}#cov div{flex:1;height:10px;border-radius:2px;background:var(--line)}
#cov div.on{background:var(--ok)}
.big{font-size:1.1rem;margin:8px 0}
td,th{text-align:right}
input[type=range]{width:100%;accent-color:var(--accent);padding:0;border:0}
</style></head><body>)HTML" PAGE_NAV R"HTML(<main>
<h1>Calibrate orientation</h1>
<p id='conn' class='bad'>Waiting for sensor data… (otoscope on and connected?)</p>

<div class='card'><h2>Live</h2>
<div class='row'>
 <div id='wrap'><img id='live' alt=''></div>
 <svg id='dial' width='200' height='200' viewBox='-100 -100 200 200'>
  <circle r='90' fill='none' stroke='#2a2f3a' stroke-width='2'/>
  <g stroke='#3d4452' stroke-width='2'><line y1='-90' y2='-78'/><line x1='90' x2='78'/><line y1='90' y2='78'/><line x1='-90' x2='-78'/></g>
  <text y='-62' fill='#8b93a5' text-anchor='middle' font-size='14'>up</text>
  <line id='needle' y2='-80' stroke='#4c9dff' stroke-width='5' stroke-linecap='round'/>
  <circle r='6' fill='#4c9dff'/>
 </svg>
</div>
<label class='switch'><input type='checkbox' id='liveOn'>Show live image</label>
<dl class='kv small' id='vals'><dt>raw</dt><dd>–</dd></dl>
<p class='muted small'>The needle shows where the side is that was up at the zero point. The
image on the left is rotated with the calibration you are editing here.</p>
</div>

<div class='card'><h2>1. Record a circle</h2>
<p>Hold the probe as level as possible and <b>slowly turn it once all the way around its
long axis</b>. The green fields show which angles have been covered. The live image is off
during recording so the Wi-Fi only has to carry sensor data.</p>
<button id='recBtn' class='primary'>Start recording</button>
<div id='cov'></div>
<canvas id='plot' width='260' height='260'></canvas>
<p id='recStat' class='small'></p>
<p id='fitRes'></p>
<button id='fitUse' disabled>Apply result</button>
</div>

<div class='card'><h2>2. Check quarter turns</h2>
<p>Mark one side of the probe (e.g. the button). Then step by step <b>one quarter turn
further each time, always in the same direction</b>, and at each step hold still and
click “Capture”.</p>
<p class='big' id='qStep'></p>
<button id='qTake' class='primary'>Capture</button> <button id='qReset'>Start over</button>
<table id='qTab'></table>
<p id='qRes'></p>
<button id='qUse' disabled>Apply as correction</button>
<button id='qClear'>Remove correction</button>
</div>

<div class='card'><h2>3. Zero point and smoothing</h2>
<p>The image is always rotated by −90° (mounting of the camera). The zero point is the
normal position of the probe in which no additional correction is needed (default 0°).
For fine-tuning, hold the probe so the image is the right way up, then:</p>
<button id='zero'>Current position = up</button>
<p>Smoothing: <b id='smV'></b> <span class='muted small'>(left = direct/jittery, right = calm/sluggish)</span></p>
<input type='range' id='sm' min='0' max='0.9' step='0.05'>
</div>

<div class='card'><h2>Save</h2>
<button id='save' class='primary'>Save on device</button> <button id='reset'>Reset everything</button>
<p id='msg'></p>
</div>
</main>
<script src='/app.js'></script><script>
let W=Object.assign({},DEFAULT_CAL), dirty=false;
const sm=new Smoother(), rot=rotator($('wrap'));
let last=null, recent=[];           // latest raw values
let rec=false, pts=[], recT=[];     // circle recording (points, timestamps)
let fit=null, q=[];                 // ellipse result, quarter turns
const Q_TEXT=['Mark pointing UP','¼ further (mark RIGHT or sideways)',
  '¼ further (mark DOWN)','¼ further (mark LEFT or other side)'];

// live image only when needed: without the stream more Wi-Fi bandwidth is left for sensor data
function live(){$('live').src=($('liveOn').checked&&!rec)?'/stream?'+Date.now():''}
$('live').className=store.get('round',false)?'round':'';  // same as on the start page
$('liveOn').checked=store.get('calLive',false);
$('liveOn').onchange=()=>{store.set('calLive',$('liveOn').checked);live()};
live();

function changed(){dirty=true;$('msg').textContent='Unsaved changes.';$('msg').className='bad';render()}

// --- Live ---
function render(){
  if(sm.have){
    const l=probeAngle(sm.x,sm.y,W);
    rot(imageRotation(true,sm,W));
    $('needle').setAttribute('transform','rotate('+l.toFixed(1)+')');
  }else rot(W.base);
  if(last){
    const g=Math.hypot(last.x,last.y,last.z);
    kv($('vals'),[['raw','x '+last.x+' · y '+last.y+' · z '+last.z+' · |g| '+g.toFixed(0)],
      ['sensor angle',sensorAngle(last.x,last.y,W).toFixed(1)+'°'],
      ['corrected',correct(sensorAngle(last.x,last.y,W),W).toFixed(1)+'°'],
      ['orientation',(sm.have?probeAngle(sm.x,sm.y,W).toFixed(1):'–')+'° (to zero point)']]);
  }
  $('smV').textContent=W.smooth.toFixed(2); $('sm').value=W.smooth;
  renderQ();
}
orientation(a=>{
  last=a; recent.push(a); if(recent.length>8)recent.shift();
  $('conn').textContent='Sensor connected'; $('conn').className='ok';
  if(rec){pts.push([a.x,a.y]);recT.push(performance.now());drawPlot()}
  sm.add(a,W); render();
},ok=>{if(!ok){$('conn').textContent='Connection lost, reconnecting…';$('conn').className='bad'}});

// --- 1. Circle ---
for(let i=0;i<36;i++)$('cov').appendChild(document.createElement('div'));
function coverage(){
  if(pts.length<5)return 0;
  let mnx=1e9,mxx=-1e9,mny=1e9,mxy=-1e9;
  for(const[x,y]of pts){mnx=Math.min(mnx,x);mxx=Math.max(mxx,x);mny=Math.min(mny,y);mxy=Math.max(mxy,y)}
  const cx=(mnx+mxx)/2,cy=(mny+mxy)/2,on=new Array(36).fill(false);
  for(const[x,y]of pts)on[Math.floor((Math.atan2(x-cx,y-cy)*180/Math.PI+180)/10)%36]=true;
  $('cov').childNodes.forEach((d,i)=>d.className=on[i]?'on':'');
  return on.filter(Boolean).length;
}
function drawPlot(){
  const cv=$('plot'),g=cv.getContext('2d'),S=cv.width/2,k=S/150;
  g.clearRect(0,0,cv.width,cv.height);
  g.strokeStyle='#2a2f3a';g.beginPath();g.moveTo(0,S);g.lineTo(2*S,S);g.moveTo(S,0);g.lineTo(S,2*S);g.stroke();
  g.fillStyle='#4c9dff';
  for(const[x,y]of pts)g.fillRect(S+x*k-1,S-y*k-1,2,2);
  if(fit){
    g.strokeStyle='#3fb950';g.lineWidth=2;g.beginPath();
    g.ellipse(S+fit.ox*k,S-fit.oy*k,fit.sx*k,fit.sy*k,0,0,2*Math.PI);g.stroke();g.lineWidth=1;
  }
  const n=coverage();
  if(recT.length>1){
    let gap=0;for(let i=1;i<recT.length;i++)gap=Math.max(gap,recT[i]-recT[i-1]);
    const rate=(recT.length-1)/((recT[recT.length-1]-recT[0])/1000);
    $('recStat').innerHTML='Data rate '+rate.toFixed(1)+' values/s (target ~17) &middot; longest pause '+
      '<span class='+(gap>300?'bad':'ok')+'>'+gap.toFixed(0)+' ms</span>'+
      (gap>300?' – gaps in the circle then come from Wi-Fi dropouts, not from the sensor.':'');
  }
  if(rec&&n>=34){stopRec()}
}
// axis-parallel ellipse A x² + B y² + C x + D y = 1 (least squares)
function fitEllipse(P){
  const M=[0,1,2,3].map(()=>[0,0,0,0]),v=[0,0,0,0];
  for(const[x,y]of P){const r=[x*x,y*y,x,y];for(let i=0;i<4;i++){v[i]+=r[i];for(let j=0;j<4;j++)M[i][j]+=r[i]*r[j]}}
  for(let c=0;c<4;c++){   // Gauss with pivoting
    let p=c;for(let r=c+1;r<4;r++)if(Math.abs(M[r][c])>Math.abs(M[p][c]))p=r;
    if(Math.abs(M[p][c])<1e-12)return null;
    [M[c],M[p]]=[M[p],M[c]];[v[c],v[p]]=[v[p],v[c]];
    for(let r=0;r<4;r++)if(r!==c){const f=M[r][c]/M[c][c];for(let j=c;j<4;j++)M[r][j]-=f*M[c][j];v[r]-=f*v[c]}
  }
  const[A,B,C,D]=[0,1,2,3].map(i=>v[i]/M[i][i]);
  if(!(A>0&&B>0))return null;
  const ox=-C/(2*A),oy=-D/(2*B),R=1+C*C/(4*A)+D*D/(4*B);
  if(!(R>0))return null;
  const f={ox,oy,sx:Math.sqrt(R/A),sy:Math.sqrt(R/B)};
  let e=0;for(const[x,y]of P)e+=(Math.hypot((x-f.ox)/f.sx,(y-f.oy)/f.sy)-1)**2;
  f.rms=Math.sqrt(e/P.length);
  return f;
}
function stopRec(){
  rec=false;$('recBtn').textContent='Record again';live();
  fit=fitEllipse(pts);
  if(!fit||coverage()<30){
    $('fitRes').innerHTML='<span class=bad>Too little coverage or no sensible circle. Please one full turn, slowly.</span>';
    fit=null;$('fitUse').disabled=true;
  }else{
    $('fitRes').innerHTML='Centre x '+fit.ox.toFixed(1)+', y '+fit.oy.toFixed(1)+
      ' &middot; radius x '+fit.sx.toFixed(1)+', y '+fit.sy.toFixed(1)+
      ' &middot; deviation '+(fit.rms*100).toFixed(1)+' %'+
      (fit.rms<0.08?' <span class=ok>(good)</span>':' <span class=bad>(noisy – maybe repeat)</span>');
    $('fitUse').disabled=false;
  }
  drawPlot();
}
$('recBtn').onclick=()=>{
  if(rec)return stopRec();
  pts=[];recT=[];fit=null;rec=true;$('fitUse').disabled=true;$('fitRes').textContent='';
  $('recStat').textContent='';$('recBtn').textContent='Stop recording';live();drawPlot();
};
$('fitUse').onclick=()=>{
  Object.assign(W,{ox:fit.ox,oy:fit.oy,sx:fit.sx,sy:fit.sy});
  // the table refers to the old ellipse -> redo it
  W.pts=null;q=[];
  $('fitRes').innerHTML+='<br><span class=ok>Applied. Redo step 2 if needed, adjust the zero point if necessary.</span>';
  changed();
};

// --- 2. Quarter turns ---
function avgAngle(){
  let x=0,y=0;for(const a of recent){x+=(a.x-W.ox)/W.sx;y+=(a.y-W.oy)/W.sy}
  return Math.atan2(x,y)*180/Math.PI;
}
function renderQ(){
  $('qStep').textContent=q.length<4?'Step '+(q.length+1)+'/4: '+Q_TEXT[q.length]:'Done.';
  $('qTake').disabled=q.length>=4||recent.length<4;
  let h='<tr><th>Step</th><th>Sensor</th><th>Distance to previous</th><th>Error</th></tr>';
  let maxErr=0;
  q.forEach((s,i)=>{
    const d=i?Math.abs(norm(s-q[i-1])):null, err=d===null?null:d-90;
    if(err!==null)maxErr=Math.max(maxErr,Math.abs(err));
    h+='<tr><td>'+(i+1)+'</td><td>'+s.toFixed(1)+'°</td><td>'+(d===null?'–':d.toFixed(1)+'°')+
       '</td><td class='+(err===null?'':(Math.abs(err)<5?'ok':'bad'))+'>'+(err===null?'–':(err>0?'+':'')+err.toFixed(1)+'°')+'</td></tr>';
  });
  if(q.length===4){
    const d=Math.abs(norm(q[0]-q[3]));
    h+='<tr><td>4→1</td><td></td><td>'+d.toFixed(1)+'°</td><td>'+(d-90>0?'+':'')+(d-90).toFixed(1)+'°</td></tr>';
    maxErr=Math.max(maxErr,Math.abs(d-90));
  }
  $('qTab').innerHTML=h;
  const ti=q.length===4?tableInfo(q.map((s,i)=>[s,i*90])):null;
  $('qUse').disabled=!ti;
  $('qRes').innerHTML=q.length<4?'':(!ti?'<span class=bad>Order does not fit (direction changed?). Start over.</span>':
    'Largest deviation '+maxErr.toFixed(1)+'°. '+(maxErr<5?'<span class=ok>That is already good, a correction is hardly needed.</span>':
    '<span class=bad>“Apply as correction” compensates for this.</span>'));
  $('qClear').disabled=!W.pts;
}
$('qTake').onclick=()=>{q.push(avgAngle());renderQ()};
$('qReset').onclick=()=>{q=[];renderQ()};
$('qUse').onclick=()=>{W.pts=q.map((s,i)=>[s,i*90]);changed();
  $('qRes').innerHTML+='<br><span class=ok>Applied.</span>'};
$('qClear').onclick=()=>{W.pts=null;changed()};

// --- 3. Zero point, smoothing ---
$('zero').onclick=()=>{if(!sm.have)return;W.zero=correct(sensorAngle(sm.x,sm.y,W),W);changed()};
$('sm').oninput=()=>{W.smooth=parseFloat($('sm').value);changed()};

// --- Save ---
$('save').onclick=async()=>{
  try{await saveCal(W);dirty=false;$('msg').textContent='Saved.';$('msg').className='ok'}
  catch(e){$('msg').textContent='Saving failed: '+e.message;$('msg').className='bad'}
};
$('reset').onclick=()=>{if(confirm('Reset the calibration to factory defaults?')){W=Object.assign({},DEFAULT_CAL);q=[];fit=null;changed()}};
addEventListener('beforeunload',e=>{if(dirty){e.preventDefault();e.returnValue=''}});

loadCal().then(c=>{W=c;render();drawPlot()});
render();drawPlot();
</script></body></html>)HTML";

static const char UPDATE_HTML[] = PAGE_HEAD("Status & update")
    R"HTML(</head><body>)HTML" PAGE_NAV R"HTML(<main>
<h1>Status &amp; update</h1>
<p id='rescue' class='note' hidden><b>Rescue mode:</b> Ethernet has no IP, Wi-Fi is on the home
network or the own access point instead of the camera. When Ethernet comes back, the
device restarts by itself. <a href='/wifi-setup'>Set up home Wi-Fi</a></p>
<div class='card'><h2>Device</h2><dl class='kv' id='st'><dt>Status</dt><dd>loading…</dd></dl></div>
<div class='card'><h2>Video</h2><dl class='kv' id='vid'></dl></div>
<div class='card'><h2>Wi-Fi to the camera</h2>
<p class='muted small'>Takes effect immediately, the device briefly reconnects. Afterwards compare
the lost packets above.</p>
<p><button class='wm' data-m='bgn'>b/g/n</button> fast, aggregates packets<br>
<button class='wm' data-m='bg'>b/g</button> every packet on its own (default)<br>
<button class='wm' data-m='b'>b only</button> slow, most robust with a weak signal</p>
</div>
<div class='card'><h2>Ethernet</h2>
<p><button class='eth' data-v='1'>10 Mbit</button> required on the ZB-GW03 (Wi-Fi disturbs the clock otherwise)<br>
<button class='eth' data-v='0'>100 Mbit</button> for boards with their own oscillator (WT32-ETH01)</p>
</div>
<div class='card'><h2>Firmware update</h2>
<p class='muted small'>Choose the file <code>.pio/build/&lt;board&gt;/firmware.bin</code>, e.g.
<code>zb-gw03</code> (not <code>firmware.factory.bin</code>).</p>
<input type='file' id='f' accept='.bin' class='field'>
<input type='password' id='pw' placeholder='OTA password (if set)' class='field'>
<p><button id='go' class='primary'>Flash</button> <button id='rs'>Restart</button></p>
<progress id='p' max='100' value='0'></progress>
<p id='msg'></p>
</div>
</main><script src='/app.js'></script><script>
async function status(){
  try{
    const s=await (await fetch('/status',{cache:'no-store'})).json();
    $('rescue').hidden=s.mode!=='rescue';
    const eth=s.eth_ip?esc(s.eth_ip)+(s.eth_speed?' · '+s.eth_speed+' Mbit':'')+(s.eth10?' (10 Mbit set)':'')
      :'no IP, '+(!s.eth_begin?'init failed':s.eth_link?'link up':'no link');
    kv($('st'),[
      ['Mode',s.mode==='rescue'?'rescue':'normal',s.mode==='rescue'?'warnc':'ok'],
      ['Ethernet',eth,s.eth_ip?'':'bad'],
      s.ap&&['Setup AP',esc(s.ap)+' (192.168.4.1)'],
      ['Wi-Fi',(s.wifi_connected?esc(s.wifi_ssid)+' · '+s.wifi_rssi+' dBm':'disconnected')+' · mode '+s.wifi_mode,
        !s.wifi_connected?'bad':s.wifi_rssi<-70?'warnc':''],
      ['Camera',(s.cam_proto||'none')+(s.battery>=0?' · battery '+s.battery+' %':'')],
      ['Heap',s.free_heap+' bytes free (min '+s.min_heap+')'],
      ['Uptime',Math.floor(s.uptime_s/3600)+' h '+Math.floor(s.uptime_s%3600/60)+' min'],
      ['Version',esc(s.version)],
      ['Commit',esc(s.commit||'–')],
      ['Reset',esc(s.reset_reason)+' (boot #'+s.boot_count+')'],
      s.last_crash&&['Crash',esc(s.last_crash),'bad']]);
    kv($('vid'),[
      ['Frame rate',s.fps.toFixed(1)+' fps',s.fps<5?'warnc':'ok'],
      ['Frames',s.frames+' received, '+s.dropped+' dropped'],
      ['Dropped',s.drop_nomem+' memory · '+s.drop_toobig+' too large · '+s.drop_incomplete+' incomplete'],
      ['Packets lost',s.packets_lost+' (shown damaged: '+s.damaged+')'],
      ['Largest frame',(s.max_frame/1024).toFixed(1)+' KB'],
      ['Stalls',s.stalls_loss+' after packet loss, '+s.stalls_clean+' without'+
        (s.clean_stall_times.length?' (at '+s.clean_stall_times.join(', ')+' s)':'')],
      ['Viewers',s.stream_clients]]);
    return true;
  }catch(e){kv($('st'),[['Status','not reachable','bad']]);return false}
}
function waitReboot(){
  let n=0;
  const t=setInterval(async()=>{
    if(++n>3&&await status()){clearInterval(t);$('msg').textContent='Device is running again.'}
  },2000);
}
$('go').onclick=async()=>{
  const file=$('f').files[0];
  if(!file){$('msg').textContent='No file chosen.';return}
  const head=new Uint8Array(await file.slice(0,1).arrayBuffer());
  if(head[0]!==0xE9){$('msg').textContent='This is not an ESP32 app (first byte is not 0xE9).';return}
  const x=new XMLHttpRequest();
  x.open('POST','/update');
  x.setRequestHeader('Content-Type','application/octet-stream');
  if($('pw').value)x.setRequestHeader('X-OTA-Password',$('pw').value);
  x.upload.onprogress=e=>{if(e.lengthComputable)$('p').value=e.loaded*100/e.total};
  x.onload=()=>{$('msg').textContent=x.status+': '+x.responseText;if(x.status===200)waitReboot()};
  x.onerror=()=>{$('msg').textContent='Transfer aborted.'};
  $('msg').textContent='Uploading…';
  x.send(file);
};
$('rs').onclick=async()=>{
  const r=await fetch('/restart',{method:'POST',headers:otaHeaders()});
  $('msg').textContent=r.status+': '+await r.text();if(r.ok)waitReboot();
};
document.querySelectorAll('.eth').forEach(b=>b.onclick=async()=>{
  const r=await fetch('/eth10/'+b.dataset.v,{method:'POST',headers:otaHeaders()});
  $('msg').textContent=r.status+': '+await r.text();
});
document.querySelectorAll('.wm').forEach(b=>b.onclick=async()=>{
  const r=await fetch('/wifi/'+b.dataset.m,{method:'POST',headers:otaHeaders()});
  $('msg').textContent=r.status+': '+await r.text();status();
});
status();setInterval(status,3000);
</script></body></html>)HTML";

// Choose camera: recognised cameras (SSID patterns) and all other networks from the scan
static const char CAMERAS_HTML[] = PAGE_HEAD("Cameras")
    R"HTML(<style>.cur{color:var(--ok);font-weight:600}td:last-child{text-align:right}
.sig{display:inline-flex;gap:2px;align-items:flex-end;height:12px;margin-right:6px;vertical-align:-1px}
.sig i{width:3px;background:var(--line);border-radius:1px}.sig i.on{background:var(--accent)}
.net td:nth-child(2){white-space:nowrap}
@media(max-width:560px){.net tr{display:flex;flex-wrap:wrap;align-items:center;column-gap:10px;padding:6px 0;
border-bottom:1px solid var(--line)}.net td{border:0;padding:2px}.net td:first-child{flex-basis:100%}
.net td:nth-child(4){margin-left:auto}}</style>
</head><body>)HTML" PAGE_NAV R"HTML(<main>
<h1>Cameras</h1>
<div class='card'><dl class='kv' id='st'><dt>State</dt><dd>loading…</dd></dl></div>
<div class='card'><h2>Recognised cameras</h2><table id='rec' class='net'></table>
<p><button id='scan'>&#8635; Rescan</button> <button id='forget'>Clear selection (automatic)</button></p>
<p><label><input type='checkbox' id='autoscan'> Automatic scan</label></p>
<p class='muted small'>The device remembers the chosen camera and reconnects to it at startup. If it
is off, the device takes another recognised camera if exactly one is in range. Scanning while the
image is running makes it stutter briefly. Without automatic scan the device only reconnects to
the remembered camera and scans only on “Rescan”.</p></div>
<div class='card'><h2>Other networks</h2>
<p class='muted small'>Unknown camera? Try it here with protocol “automatic” (192.168.29.1 →
JHCMD, otherwise by name, else i4season). No image? Pick another protocol and reconnect.</p>
<table id='oth' class='net'></table></div>
<div class='card'><h2>Connection options</h2>
<input type='password' id='wpw' class='field' placeholder='Wi-Fi password of the camera (usually empty)'>
<input type='password' id='pw' class='field' placeholder='OTA password (if set)'>
<p id='msg'></p></div>
</main><script src='/app.js'></script><script>
const STATE={connected:'connected',connecting:'connecting…',scanning:'scanning…',choose:'several found, please choose',searching:'no camera found, still searching',restart:'reconnecting…'};
// signal strength as 4 bars
function sig(r){const n=r>-55?4:r>-65?3:r>-75?2:1;let h='<span class=sig title="'+r+' dBm">';
  for(let i=1;i<=4;i++)h+='<i class='+(i<=n?'on':'')+' style="height:'+(i*3)+'px"></i>';return h+'</span>'}
// protocol per network: the choice survives the 4 s refresh; default for the active
// camera is its current protocol, otherwise "automatic". The list comes from the
// firmware (cameras.json "protocols": [key, name]).
let PROTOS=[['auto','automatic'],['i4season','i4season'],['jhcmd','JHCMD']];const chosen={};
function protoOf(n,c){return chosen[n.ssid]||(n.ssid===c.ssid&&c.proto?c.proto:'auto')}
function row(n,c){
  const cur=n.ssid===c.ssid&&c.state==='connected', p=protoOf(n,c);
  return '<tr><td class='+(cur?'cur':'')+'>'+esc(n.ssid)+(n.ssid===c.preferred?' ★':'')+
    (cur?' <span class="ok small">active</span>':'')+'</td><td>'+sig(n.rssi)+
    '<span class="muted small">'+n.rssi+' dBm</span></td><td class="muted small">'+(n.open?'open':'&#128274;')+
    '</td><td><select data-p="'+esc(n.ssid)+'">'+PROTOS.map(([k,l])=>'<option value='+k+(k===p?' selected':'')+
    ' title="'+esc(l)+'">'+(k==='auto'?'automatic':k)+'</option>').join('')+'</select></td><td><button data-s="'+esc(n.ssid)+'">'+(cur?'Reconnect':'Connect')+
    '</button></td></tr>';
}
async function load(){
  const c=await camInfo();
  if(!c){kv($('st'),[['State','not reachable','bad']]);return}
  kv($('st'),[['State',STATE[c.state]||c.state,c.state==='connected'?'ok':c.state==='choose'?'warnc':''],
    ['Camera',(c.ssid?esc(c.ssid):'–')+(c.proto?' <span class=badge>'+c.proto+'</span>':'')],
    ['Device',esc([c.vendor,c.product,c.firmware].filter(Boolean).join(' ')||'–')],
    c.width&&['Image',c.width+'×'+c.height+' <span class="muted small">(as reported by the camera)</span>'],
    c.battery>=0&&['Battery',c.battery+' %'],
    ['Remembered',c.preferred?esc(c.preferred):'– (automatic)'],
    ['Automatic scan',c.autoscan?'on':'off'],
    ['Last scan',c.scan_age_s<0?'none yet':c.scan_age_s+' s ago']]);
  $('autoscan').checked=!!c.autoscan;
  if(c.protocols&&c.protocols.length)PROTOS=c.protocols;
  // do not rebuild the tables while a protocol list is open (it would close)
  if(document.activeElement&&document.activeElement.matches('select[data-p]'))return;
  const rec=c.networks.filter(n=>n.proto), oth=c.networks.filter(n=>!n.proto);
  $('rec').innerHTML=rec.length?rec.map(n=>row(n,c)).join(''):'<tr><td class=muted>none in range</td></tr>';
  $('oth').innerHTML=oth.map(n=>row(n,c)).join('')||'<tr><td class=muted>none</td></tr>';
  document.querySelectorAll('button[data-s]').forEach(b=>b.onclick=()=>select(b.dataset.s));
  document.querySelectorAll('select[data-p]').forEach(s=>s.onchange=()=>{chosen[s.dataset.p]=s.value;s.blur()});
}
async function post(url,body){
  const r=await fetch(url,{method:'POST',headers:otaHeaders({'Content-Type':'application/x-www-form-urlencoded'}),body});
  $('msg').textContent=r.status+': '+await r.text();setTimeout(load,1500);
}
function select(ssid){
  const s=[...document.querySelectorAll('select[data-p]')].find(e=>e.dataset.p===ssid);
  post('/cameras/select',new URLSearchParams({ssid,pass:$('wpw').value,proto:s?s.value:'auto'}).toString())}
$('forget').onclick=()=>post('/cameras/select','ssid=');
$('autoscan').onchange=e=>post('/cameras/autoscan/'+(e.target.checked?1:0),'');
$('scan').onclick=async()=>{await fetch('/cameras/scan',{method:'POST'});$('msg').textContent='Scanning…';setTimeout(load,4000)};
load();setInterval(load,4000);
</script></body></html>)HTML";

// Set up the home Wi-Fi for rescue mode (also via the own access point)
static const char WIFI_SETUP_HTML[] = PAGE_HEAD("Wi-Fi setup")
    R"HTML(<style>tr.n{cursor:pointer}tr.n:hover td{background:var(--card2)}</style>
</head><body>)HTML" PAGE_NAV R"HTML(<main>
<h1>Home Wi-Fi</h1>
<div class='card'><dl class='kv' id='st'><dt>Mode</dt><dd>loading…</dd></dl>
<p class='muted small'>If Ethernet has no connection, the device switches to this Wi-Fi so the web UI and
updates stay reachable. If that fails too, it opens its own access point
(<code>WiFi-Cam-…</code>), through which you get here.</p></div>
<div class='card'><h2>Networks in range</h2>
<p><button id='scan'>&#8635; Scan</button> <span id='scanMsg' class='muted small'></span></p>
<table id='nets'></table></div>
<div class='card'><h2>Credentials</h2>
<input id='ssid' class='field' placeholder='Wi-Fi name (SSID)' maxlength='32'>
<input type='password' id='pass' class='field' placeholder='Wi-Fi password' maxlength='64'>
<input type='password' id='pw' class='field' placeholder='OTA password (if set)'>
<p><button id='save' class='primary'>Save and connect</button> <button id='clear'>Delete</button></p>
<p id='msg'></p></div>
</main><script src='/app.js'></script><script>
async function load(){
  try{
    const s=await (await fetch('/status',{cache:'no-store'})).json();
    kv($('st'),[['Mode',s.mode==='rescue'?'rescue':'normal (Ethernet '+(s.eth_ip?esc(s.eth_ip):'without IP')+')',s.mode==='rescue'?'warnc':'ok'],
      ['Home Wi-Fi',s.home_ssid?esc(s.home_ssid):'– not set up'],
      s.mode==='rescue'&&['Wi-Fi',s.wifi_connected?esc(s.wifi_ssid)+', IP '+esc(s.wifi_ip):'not connected'],
      s.ap&&['Setup AP',esc(s.ap)+' (192.168.4.1)']]);
  }catch(e){kv($('st'),[['Mode','not reachable','bad']])}
  try{
    const c=await (await fetch('/cameras.json',{cache:'no-store'})).json();
    $('nets').innerHTML=c.networks.map(n=>'<tr class=n data-s="'+esc(n.ssid)+'"><td>'+esc(n.ssid)+'</td><td class="muted small">'+n.rssi+
      ' dBm</td><td>'+(n.open?'open':'&#128274;')+'</td></tr>').join('')||'<tr><td class=muted>no scan yet</td></tr>';
    document.querySelectorAll('tr.n').forEach(r=>r.onclick=()=>{$('ssid').value=r.dataset.s;$('pass').focus()});
  }catch(e){}
}
async function save(ssid,pass){
  try{
    const r=await fetch('/wifi-setup',{method:'POST',headers:otaHeaders({'Content-Type':'application/x-www-form-urlencoded'}),
      body:new URLSearchParams({ssid,pass}).toString()});
    $('msg').textContent=r.status+': '+await r.text();
  }catch(e){$('msg').textContent='Connection lost - the device may be switching Wi-Fi.'}
  setTimeout(load,3000);
}
$('save').onclick=()=>save($('ssid').value,$('pass').value);
$('clear').onclick=()=>save('','');
$('scan').onclick=async()=>{await fetch('/cameras/scan',{method:'POST'});$('scanMsg').textContent='scanning…';
  setTimeout(()=>{$('scanMsg').textContent='';load()},5000)};
load();setInterval(load,5000);
</script></body></html>)HTML";

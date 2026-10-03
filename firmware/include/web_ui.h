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
nav .ro{display:none}
.rescue nav .ro{display:block}
.rescue nav .nr{display:none}
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
.danger{background:var(--bad);border-color:var(--bad);color:#1a0605;font-weight:600}
.danger:hover{border-color:#ff948b;background:#f77d74}
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
pre{background:var(--bg);border:1px solid var(--line);border-radius:9px;padding:10px;margin:8px 0 0;overflow:auto;max-height:55vh;font-size:.78rem;line-height:1.35}
pre:empty{display:none}
h3{font-size:.92rem;margin:16px 0 6px;color:var(--muted);font-weight:600}
#auth{margin-left:auto}
button.sel{border-color:var(--accent);color:var(--accent)}
.opts{display:grid;grid-template-columns:max-content 1fr;gap:6px 12px;align-items:center;margin:6px 0}
.opts button{margin:0}
footer{max-width:640px;margin:0 auto;padding:4px 16px 24px;text-align:center}
)CSS";

#define PAGE_HEAD(title) "<!doctype html><html lang='en'><head><title>" title "</title>" PAGE_STYLE

// Navigation; the link of the current page is highlighted. "logged in": an OTA password is
// kept for this browser session (app.js, auth), a click forgets it.
#define PAGE_NAV                                                                                \
  "<header><span class='brand'><i></i>WiFi-Cam</span><nav>"                                    \
  "<a class='nr' href='/'>Live</a><a class='nr' href='/cameras'>Cameras</a><a class='nr' href='/settings'>Settings</a>" \
  "<a class='nr' href='/info'>Status</a><a href='/update'>Update</a><a class='ro' href='/wifi-setup'>Home Wi-Fi</a></nav>" \
  "<a id='auth' class='small muted' href='#' hidden title='Forget the OTA password'>&#128275; logged in</a></header>" \
  "<script>document.querySelectorAll('nav a').forEach(a=>{"                                    \
  "if(a.getAttribute('href')===location.pathname)a.classList.add('on')})</script>"

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
// zero = sensor angle in the normal position of the probe; orientation correction
// compensates deviations from it. The fixed rotation of the image (otoscope: -90°, the
// camera sits turned in the probe) comes from the device per camera model
// (/cameras.json "rotation"); "base" in older stored calibrations is ignored.
const DEFAULT_CAL={v:2,ox:0,oy:0,sx:1,sy:1,zero:0,smooth:0.6,pts:null};
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
// image rotation: always the camera's rotation (base), with orientation correction also -angle
function imageRotation(on,sm,c,base){return base-(on&&sm.have?probeAngle(sm.x,sm.y,c):0)}

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
// OTA password: asked for once when the device answers 401, then kept for this browser
// session (sessionStorage) and sent with every change
const auth={get(){try{return sessionStorage.getItem('ota')||''}catch(e){return ''}},
  set(p){try{p?sessionStorage.setItem('ota',p):sessionStorage.removeItem('ota')}catch(e){}showAuth()}};
function showAuth(){const a=$('auth');if(a)a.hidden=!auth.get()}
function otaHeaders(h){h=h||{};const p=auth.get();if(p)h['X-OTA-Password']=p;return h}
// POST a change; on 401 ask for the password and send once more. -> {ok,status,text}
async function post(url,body,type){
  for(let i=0;i<2;i++){
    let r;
    try{r=await fetch(url,{method:'POST',headers:otaHeaders(type?{'Content-Type':type}:{}),body:body||''})}
    catch(e){return {ok:false,status:0,text:'Device not reachable'}}
    const text=await r.text();
    if(r.status===401&&i===0){
      const p=prompt(auth.get()?'Wrong password. OTA password:':'OTA password:');
      if(p===null)return {ok:false,status:401,text};
      auth.set(p);continue;
    }
    return {ok:r.ok,status:r.status,text};
  }
}
document.addEventListener('DOMContentLoaded',()=>{showAuth();const a=$('auth');
  if(a)a.onclick=e=>{e.preventDefault();auth.set('')}});

// MJPEG over fetch instead of <img src=stream>: every frame is shown as its own image,
// so a stall or the end of the stream is noticed (an <img> just keeps the last frame).
// The device sends /live as application/octet-stream: Safari fails fetch() on
// multipart/x-mixed-replace. Reconnects by itself. onState(text): '' = frames arrive, else
// why not; onFrame() once per frame shown. Returns stop().
function mjpeg(img,url,onState,onFrame){
  let ctl=null,stopped=false,last=0,timer=0,prev='',wait=2000;
  const find=(b,from)=>{for(let i=from;i+3<b.length;i++)if(b[i]===13&&b[i+1]===10&&b[i+2]===13&&b[i+3]===10)return i;return -1};
  const show=jpg=>{
    const u=URL.createObjectURL(new Blob([jpg],{type:'image/jpeg'}));
    img.onload=()=>{if(prev&&prev!==u)URL.revokeObjectURL(prev);prev=u};
    last=Date.now();onState('');
    if(img.dataset.frozen){URL.revokeObjectURL(u);return}
    img.src=u;if(onFrame)onFrame();
  };
  async function run(){
    ctl=new AbortController();
    try{
      const r=await fetch(url+'?'+Date.now(),{signal:ctl.signal,cache:'no-store'});
      if(!r.ok){onState(await r.text());wait=5000;throw 'status'}  // e.g. switched off: ask less often
      wait=2000;
      const rd=r.body.getReader();let buf=new Uint8Array(0);
      for(;;){
        const {done,value}=await rd.read();if(done)break;
        const b=new Uint8Array(buf.length+value.length);b.set(buf);b.set(value,buf.length);buf=b;
        for(;;){  // part: headers up to a blank line, then Content-Length bytes of JPEG
          const h=find(buf,0);if(h<0)break;
          const m=/Content-Length:\s*(\d+)/i.exec(new TextDecoder().decode(buf.subarray(0,h)));
          if(!m){buf=buf.slice(h+4);continue}
          const n=+m[1];if(buf.length<h+4+n)break;
          show(buf.slice(h+4,h+4+n));buf=buf.slice(h+4+n);
        }
      }
      if(!stopped)onState('Stream ended, reconnecting…');
    }catch(e){if(!stopped&&e!=='status')onState('Connection interrupted, reconnecting…')}
    if(!stopped)timer=setTimeout(run,wait);
  }
  // no frame for 5 s: start over (a stalled connection does not end by itself)
  const wd=setInterval(()=>{if(ctl&&last&&Date.now()-last>5000){last=0;onState('No image for 5 s, reconnecting…');ctl.abort()}},1000);
  run();
  return ()=>{stopped=true;clearTimeout(timer);clearInterval(wd);if(ctl)ctl.abort()};
}
async function settings(){try{return await (await fetch('/settings.json',{cache:'no-store'})).json()}catch(e){return null}}
// Footer on every page: repository and commit of the running firmware
fetch('/status',{cache:'no-store'}).then(r=>r.json()).then(s=>{
  const c=String(s.commit||''),repo='https://github.com/HeikoGr/wifi-cam-proxy';
  document.body.insertAdjacentHTML('beforeend','<footer class="small muted"><a href="'+repo+'">GitHub</a> · '+
    (/^[0-9a-f]{7,}$/.test(c)?'<a href="'+repo+'/commit/'+c+'">'+c+'</a>':esc(c||'–'))+'</footer>')}).catch(()=>{});
)JS";

static const char INDEX_HTML[] = PAGE_HEAD("WiFi-Cam")
    R"HTML(<style>
#view{display:block;width:max-content;max-width:100%;margin:0 auto;overflow:hidden;line-height:0;
 cursor:zoom-in;border-radius:var(--r);background:#000;box-shadow:0 10px 30px rgba(0,0,0,.45)}
#view.z{cursor:grab;touch-action:none}
#view.round{background:none;box-shadow:none}
#wrap{display:inline-block;transition:transform .12s linear}
#img{max-width:100%;max-height:72vh;min-width:240px;min-height:240px;background:#000}
.camline{display:flex;flex-wrap:wrap;justify-content:space-between;align-items:center;gap:8px;margin:0 0 12px}
.toolbar{display:flex;flex-wrap:wrap;align-items:center;justify-content:center;gap:4px;margin:14px 0 4px}
#ledBtn.on,#freeze.on{background:#f5c518;border-color:#f5c518;color:#111}
#view{position:relative}
#vstate{position:absolute;inset:0;display:flex;align-items:center;justify-content:center;padding:20px;
 text-align:center;line-height:1.5;color:var(--muted);background:rgba(14,16,21,.72)}
#vstate[hidden]{display:none}
</style></head><body>)HTML" PAGE_NAV R"HTML(<main class='wide'>
<p id='choose' class='note' hidden>Several cameras found. Please pick one under <a href='/cameras'>Cameras</a>.</p>
<p id='camOff' class='note' hidden>The connection to the camera is switched off. <a href='/settings'>Settings</a></p>
<div class='camline'><span id='cam' class='muted'>…</span><span><span id='fps' class='badge' title='Frames per second shown here (the camera and the frame rate limit in the settings set the rate)' hidden></span> <span id='bat' class='badge' hidden></span></span></div>
<div id='view' title='Double-click: zoom'><div id='wrap'><img id='img' alt=''></div><div id='vstate'>Connecting…</div></div>
<div class='toolbar'>
<button id='zoom' title='Zoom in, then drag the image with the mouse or a finger'>2&times;</button>
<button id='freeze' title='Hold the current image (also the photo button of the camera)'>&#10074;&#10074; Freeze</button>
<span id='ori'><label class='switch'><input type='checkbox' id='on'>Correct orientation</label>
<label class='switch'><input type='checkbox' id='round'>Round</label>
<button id='zero'>Current position = up</button>
<a class='btn' href='/calibrate'>Calibrate</a></span>
<button id='ledBtn' title='Camera LED on/off' hidden>&#128161; LED</button>
<input type='range' id='ledLvl' min='0' max='100' title='LED brightness (0 = off)' hidden>
<a class='btn' href='/snapshot' download='snapshot.jpg'>&#128247; Snapshot</a>
<a class='btn' id='vlc' href='/stream.m3u' title='Open the stream in VLC (playlist file); address and Home Assistant under Settings' hidden>&#9654; VLC</a>
</div>
<p id='ledMsg' class='muted small' style='text-align:center'></p>
</main>
<script src='/app.js'></script><script>
let cal=Object.assign({},DEFAULT_CAL), hasOri=true, camRot=0;  // camRot: /cameras.json rotation
const view={z:1,px:0,py:0}, sm=new Smoother(), rot=rotator($('wrap'),view);
// zoom 2x: pan the crop by dragging, at most up to the image edge
// steps 1x, 2x, 4x (zoom buttons of the camera: up and down)
function setZoom(z){view.z=z;view.px=view.py=0;$('zoom').innerHTML=(z>=4?1:z*2)+'&times;';$('view').classList.toggle('z',z>1);rot()}
function stepZoom(d){setZoom(Math.max(1,Math.min(4,d>0?view.z*2:view.z/2)))}
function toggleFreeze(){const f=!$('img').dataset.frozen;if(f)$('img').dataset.frozen='1';else delete $('img').dataset.frozen;
  $('freeze').innerHTML=f?'&#9654; Resume':'&#10074;&#10074; Freeze';$('freeze').className=f?'on':'';if(!f)apply()}
$('freeze').onclick=toggleFreeze;
function clampPan(){
  const mx=(view.z-1)*$('img').clientWidth/2, my=(view.z-1)*$('img').clientHeight/2;
  view.px=Math.max(-mx,Math.min(mx,view.px));view.py=Math.max(-my,Math.min(my,view.py));
}
$('zoom').onclick=()=>setZoom(view.z>=4?1:view.z*2);
$('view').ondblclick=()=>setZoom(view.z>=4?1:view.z*2);
let drag=null;
$('view').onpointerdown=e=>{if(view.z>1){drag={x:e.clientX-view.px,y:e.clientY-view.py};$('view').setPointerCapture(e.pointerId);$('wrap').style.transition='none'}};
$('view').onpointermove=e=>{if(drag){view.px=e.clientX-drag.x;view.py=e.clientY-drag.y;clampPan();rot()}};
$('view').onpointerup=$('view').onpointercancel=()=>{drag=null;$('wrap').style.transition=''};
// round crop: display only (the image arrives square), off by default
$('round').checked=store.get('round',false);
function applyRound(){const r=hasOri&&$('round').checked;$('img').className=r?'round':'';$('view').classList.toggle('round',r)}
$('round').onchange=()=>{store.set('round',$('round').checked);applyRound()};
applyRound();
$('on').checked=store.get('on',false);
// without an orientation sensor (e.g. microscope): image unrotated, orientation controls off
function apply(){if($('img').dataset.frozen)return;rot(hasOri?imageRotation($('on').checked,sm,cal,camRot):camRot)}
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
  $('camOff').hidden=c.enabled!==false;
  const name=c.ssid||(c.state==='choose'?'none chosen':'looking for camera…');
  $('cam').textContent=[name,c.product].filter(Boolean).join(' · ');
  $('bat').hidden=c.battery<0;$('bat').textContent=batteryText(c);
  // orientation sensor: i4season reports it in the video header; before the first video
  // data (width 0) the otoscope view stays. MaxSee microscopes have none.
  if(c.state==='connected'){
    const ori=c.orientation||(c.proto==='i4season'&&!c.width);
    if(ori!==hasOri){hasOri=ori;applyRound();$('ori').hidden=!ori;apply()}
    if(c.rotation!==undefined&&c.rotation!==camRot){camRot=c.rotation;apply()}
    if(ori)startOri();
  }
  $('ledBtn').hidden=!c.led_supported;
  $('ledLvl').hidden=!c.led_dimmable;ledFast=!!c.led_dimmable||!!c.buttons;
  showLed(c.led,c.led_level,c.led_dimmable);
}
// LED state from /cameras.json or /led: button and (dimmable) slider; off = slider at 0
// (the firmware keeps the last level for "LED on")
function showLed(led,level,dimmable){
  if(dimmable&&!lvlBusy)$('ledLvl').value=led===0?0:level;
  if(led>=0&&(led===1)!==ledOn){ledOn=led===1;applyLed()}
}
async function ledPoll(){
  try{
    const l=await (await fetch('/led',{cache:'no-store'})).json();showLed(l.led,l.level,true);
    // camera buttons (CamKey): 2 zoom in, 3 zoom out, 1 photo = freeze; the first answer only sets the count
    if(keySeq!==null&&l.seq!==keySeq){if(l.key===2)stepZoom(1);else if(l.key===3)stepZoom(-1);else if(l.key===1)toggleFreeze()}
    keySeq=l.seq;
  }catch(e){}
}
let keySeq=null;
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
// live image; the text over it says why there is none (switched off, interrupted, ...)
// fps: frames shown in this browser over the last 2 s
let frames=0,fpsSince=Date.now();
mjpeg($('img'),'/live',t=>{$('vstate').hidden=!t;$('vstate').textContent=t;if(t)$('fps').hidden=true},()=>frames++);
setInterval(()=>{
  const now=Date.now(),f=frames/((now-fpsSince)/1000);frames=0;fpsSince=now;
  $('fps').hidden=!(f>0);$('fps').textContent=f.toFixed(f<10?1:0)+' fps';
},2000);
settings().then(s=>{if(s)$('vlc').hidden=!s.external});
</script></body></html>)HTML";

static const char CALIBRATE_HTML[] = PAGE_HEAD("Calibration")
    R"HTML(<style>
.row{display:flex;flex-wrap:wrap;gap:16px;justify-content:center;align-items:center}
#wrap{transition:transform .12s linear}#wrap img{width:200px;height:200px;background:#000;border-radius:var(--r)}
#wrap img.round{border-radius:50%}
#live:not([src]){visibility:hidden}
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
let W=Object.assign({},DEFAULT_CAL), dirty=false, camRot=-90;  // camRot: /cameras.json rotation
camInfo().then(c=>{if(c&&c.rotation!==undefined){camRot=c.rotation;render()}});
const sm=new Smoother(), rot=rotator($('wrap'));
let last=null, recent=[];           // latest raw values
let rec=false, pts=[], recT=[];     // circle recording (points, timestamps)
let fit=null, q=[];                 // ellipse result, quarter turns
const Q_TEXT=['Mark pointing UP','¼ further (mark RIGHT or sideways)',
  '¼ further (mark DOWN)','¼ further (mark LEFT or other side)'];

// live image only when needed: without the stream more Wi-Fi bandwidth is left for sensor data
let stopLive=null;
function live(){
  if(stopLive){stopLive();stopLive=null;$('live').removeAttribute('src')}
  if($('liveOn').checked&&!rec)stopLive=mjpeg($('live'),'/live',()=>{});
}
$('live').className=store.get('round',false)?'round':'';  // same as on the start page
$('liveOn').checked=store.get('calLive',false);
$('liveOn').onchange=()=>{store.set('calLive',$('liveOn').checked);live()};
live();

function changed(){dirty=true;$('msg').textContent='Unsaved changes.';$('msg').className='bad';render()}

// --- Live ---
function render(){
  if(sm.have){
    const l=probeAngle(sm.x,sm.y,W);
    rot(imageRotation(true,sm,W,camRot));
    $('needle').setAttribute('transform','rotate('+l.toFixed(1)+')');
  }else rot(camRot);
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

// Settings: everything that can be switched or set, stored on the device (NVS)
static const char SETTINGS_HTML[] = PAGE_HEAD("Settings")
    R"HTML(</head><body>)HTML" PAGE_NAV R"HTML(<main>
<h1>Settings</h1>
<div class='card'><h2>Camera</h2>
<label class='switch'><input type='checkbox' id='camera' data-url='/camera/enabled/'>Connection to the camera</label>
<p class='muted small'>Off: the device leaves the camera Wi-Fi (e.g. so the vendor app can connect) and
stays away from it, also after a restart. Ethernet and this page stay reachable.</p>
<label class='switch'><input type='checkbox' id='autoscan' data-url='/cameras/autoscan/'>Automatic scan</label>
<p class='muted small'>Off: the device only reconnects to the remembered camera and scans only on
“Rescan” under <a href='/cameras'>Cameras</a>.</p></div>

<div class='card'><h2>Video</h2>
<label class='switch'><input type='checkbox' id='live' data-url='/stream/live/'>Live view in the browser</label><br>
<label class='switch'><input type='checkbox' id='external' data-url='/stream/external/'>Stream for VLC and Home Assistant</label>
<p>Frame rate per viewer for images larger than 640×480: <select id='fps'></select></p>
<p class='muted small'>Both use the same image. Fewer frames per second mean less load on Ethernet and
memory; on the ZB-GW03 (10 Mbit) 5 fps is the default, more stutters with 720p microscopes.
Smaller images (otoscope) always come at the rate the camera delivers.
With frames above 48 KB (720p) only one viewer is served: the newest one wins.</p>
<h3>VLC, Home Assistant and other programs</h3>
<dl class='kv' id='urls'></dl>
<p><a class='btn' href='/stream.m3u'>&#9654; Open in VLC</a> <button id='copy'>Copy stream address</button></p>
<pre id='ha'></pre></div>

<div class='card'><h2>Wi-Fi to the camera</h2>
<div class='opts'>
<button class='wm' data-m='bgn'>b/g/n</button><span>fast, aggregates packets (one lost part holds up the block)</span>
<button class='wm' data-m='bg'>b/g</button><span>every packet on its own (default)</span>
<button class='wm' data-m='b'>b only</button><span>slow, most robust with a weak signal</span></div>
<p class='muted small'>Takes effect immediately, the device briefly reconnects. Then compare the lost
packets under <a href='/info'>Status</a>.</p>
<h3>Transmit power</h3>
<p><input type='range' id='tx' min='8' max='84' step='4'> <b id='txv'></b> <button id='txSet'>Apply</button></p>
<p class='muted small'>Default 11 dBm. On the ZB-GW03 high power disturbs the Ethernet clock that the
ESP32 generates itself (lost Ethernet packets).</p></div>

<div class='card'><h2>Ethernet</h2>
<div class='opts'>
<button class='eth' data-v='1'>10 Mbit</button><span>required on the ZB-GW03 (Wi-Fi disturbs the clock otherwise)</span>
<button class='eth' data-v='0'>100 Mbit</button><span>for boards with their own oscillator (WT32-ETH01)</span></div></div>

<div class='card'><h2>Home Wi-Fi for rescue mode</h2>
<dl class='kv' id='home'></dl>
<p class='muted small'>If Ethernet has no connection, the device joins this Wi-Fi so the web UI stays
reachable, else it opens its own access point.</p>
<p><a class='btn' href='/wifi-setup'>Set up home Wi-Fi</a></p></div>
<p id='msg'></p>
</main><script src='/app.js'></script><script>
const FPS=[0,1,2,5,10,15,20,25,30];
let S=null;
async function load(){
  S=await settings();
  if(!S){$('msg').textContent='Device not reachable';return}
  for(const id of ['camera','autoscan','live','external'])$(id).checked=!!S[id];
  $('fps').innerHTML=FPS.filter(f=>f<=S.max_fps_limit).map(f=>'<option value='+f+(f===S.max_fps?' selected':'')+'>'+
    (f?'at most '+f+' fps':'as the camera delivers')+'</option>').join('');
  document.querySelectorAll('.wm').forEach(b=>b.classList.toggle('sel',b.dataset.m===S.wifi_mode));
  document.querySelectorAll('.eth').forEach(b=>b.classList.toggle('sel',(b.dataset.v==='1')===S.eth10));
  if(document.activeElement!==$('tx')){$('tx').value=S.wifi_tx;txText()}
  const host=location.host, url='http://'+host+'/stream';
  kv($('urls'),[['Stream (MJPEG)','<code>'+esc(url)+'</code>'+(S.external?'':' <span class=warnc>switched off</span>')],
    ['Single frame','<code>http://'+esc(host)+'/snapshot</code>'],
    host!==S.hostname+'.local'&&['By name','<code>http://'+esc(S.hostname)+'.local/stream</code>']]);
  $('ha').textContent='# Home Assistant, configuration.yaml\ncamera:\n  - platform: mjpeg\n    name: WiFi-Cam\n'+
    '    mjpeg_url: '+url+'\n    still_image_url: http://'+host+'/snapshot\n';
  kv($('home'),[['Home Wi-Fi',S.home_ssid?esc(S.home_ssid):'– not set up']]);
}
function txText(){$('txv').textContent=($('tx').value/4).toFixed(1)+' dBm'}
async function change(url,what){
  const r=await post(url);
  $('msg').textContent=(what?what+': ':'')+r.text;$('msg').className=r.ok?'ok':'bad';
  setTimeout(load,r.ok?600:0);
}
document.querySelectorAll('input[data-url]').forEach(c=>c.onchange=()=>change(c.dataset.url+(c.checked?1:0)));
$('fps').onchange=()=>change('/stream/fps/'+$('fps').value);
document.querySelectorAll('.wm').forEach(b=>b.onclick=()=>change('/wifi/'+b.dataset.m,'Wi-Fi mode'));
document.querySelectorAll('.eth').forEach(b=>b.onclick=()=>change('/eth10/'+b.dataset.v,'Ethernet'));
$('tx').oninput=txText;
$('txSet').onclick=()=>change('/wifi/tx/'+$('tx').value,'Transmit power');
$('copy').onclick=async()=>{const u='http://'+location.host+'/stream';
  try{await navigator.clipboard.writeText(u);$('msg').textContent='Copied: '+u}catch(e){prompt('Stream address:',u)}};
load();
</script></body></html>)HTML";

// Status: counters and state of device, network and video; diagnostics
static const char INFO_HTML[] = PAGE_HEAD("Status")
    R"HTML(</head><body>)HTML" PAGE_NAV R"HTML(<main>
<h1>Status</h1>
<div class='card'><h2>Device</h2><dl class='kv' id='st'><dt>Status</dt><dd>loading…</dd></dl></div>
<div class='card'><h2>Network</h2><dl class='kv' id='net'></dl></div>
<div class='card'><h2>Video</h2><dl class='kv' id='vid'></dl></div>
<div class='card'><h2>Diagnostics</h2>
<p class='muted small'>For new cameras and errors: what the camera sent, a whole frame as it arrived,
the vendor app's commands.</p>
<p><button id='dLog'>Session log</button> <button id='dRaw'>Raw capture of one frame</button></p>
<h3>Sniffer</h3>
<p class='muted small'>Records the traffic between the vendor app and the camera (not the video).
The device leaves the camera Wi-Fi meanwhile: start, connect the app, use it, then show and stop.</p>
<p>Channel <input type='number' id='ch' min='0' max='14' value='0' style='width:5em'> <span class='muted small'>0 = that of the camera</span></p>
<p><button id='sStart'>Start</button> <button id='sShow'>Show recording</button> <button id='sStop'>Stop</button></p>
<h3>Send bytes to the camera</h3>
<p>Port <input type='number' id='sp' min='1' max='65535' style='width:7em'>
hex <input id='sx' placeholder='4a48434d442002' style='width:14em'> <button id='sSend'>Send</button></p>
<pre id='out'></pre></div>
</main><script src='/app.js'></script><script>
async function status(){
  try{
    const s=await (await fetch('/status',{cache:'no-store'})).json();
    const eth=s.eth_ip?esc(s.eth_ip)+(s.eth_speed?' · '+s.eth_speed+' Mbit'+(s.eth_full_duplex?'':' half duplex'):'')
      :'no IP, '+(!s.eth_begin?'init failed':s.eth_link?'link up':'no link');
    kv($('st'),[
      ['Mode',s.mode==='rescue'?'rescue':'normal',s.mode==='rescue'?'warnc':'ok'],
      ['Camera',(s.cam_proto||'none')+(s.battery>=0?' · battery '+s.battery+' %':'')],
      ['Memory',s.free_heap+' bytes free (lowest '+s.min_heap+', IRAM '+s.iram_heap+')',s.min_heap<8000?'warnc':''],
      s.cpu_load&&s.cpu_load[0]>=0&&['CPU load','core 0 '+s.cpu_load[0]+' % (Wi-Fi, network) · core 1 '+s.cpu_load[1]+' % (video, web)',
        Math.max(...s.cpu_load)>85?'warnc':''],
      ['Uptime',Math.floor(s.uptime_s/3600)+' h '+Math.floor(s.uptime_s%3600/60)+' min'],
      ['Firmware',esc(s.version)+' · '+esc(s.commit||'–')],
      ['Last reset',esc(s.reset_reason)+' (boot #'+s.boot_count+')'],
      s.last_crash&&['Crash',esc(s.last_crash),'bad']]);
    kv($('net'),[
      ['Ethernet',eth,s.eth_ip?'':'bad'],
      ['Wi-Fi',(s.wifi_connected?esc(s.wifi_ssid)+' · '+s.wifi_rssi+' dBm':'disconnected')+' · '+s.wifi_mode+' · '+s.wifi_tx_dbm.toFixed(1)+' dBm',
        !s.wifi_connected?'bad':s.wifi_rssi<-70?'warnc':''],
      s.ap&&['Setup AP',esc(s.ap)+' (192.168.4.1)'],
      ['Connections',s.stream_clients+' stream · '+s.sse_clients+' orientation · '+s.client_tasks+' in total']]);
    kv($('vid'),[
      ['Frame rate',s.fps.toFixed(1)+' fps received',s.fps<5?'warnc':'ok'],
      ['Frames',s.frames+' received, '+s.dropped+' dropped'],
      ['Dropped',s.drop_nomem+' memory · '+s.drop_toobig+' too large · '+s.drop_incomplete+' incomplete'+
        (s.released?' · stored frame given up '+s.released+'×':'')],
      ['Packets lost',s.packets_lost+' (shown damaged: '+s.damaged+')'],
      ['Largest frame',(s.max_frame/1024).toFixed(1)+' KB'],
      ['Handshakes',s.handshakes+(s.keepalives?' · '+s.keepalives+' heartbeats':'')],
      ['Stalls',s.stalls_loss+' after packet loss, '+s.stalls_clean+' without'+
        (s.clean_stall_times.length?' (at '+s.clean_stall_times.join(', ')+' s)':'')]]);
  }catch(e){kv($('st'),[['Status','not reachable','bad']])}
}
const out=t=>{$('out').textContent=t};
async function getText(url){try{const r=await fetch(url,{cache:'no-store'});return await r.text()}catch(e){return 'Device not reachable'}}
$('dLog').onclick=async()=>out(await getText('/camdiag'));
$('dRaw').onclick=async()=>{
  out('Capture requested, waiting for the next frame…');
  for(let i=0;i<6;i++){
    const r=await fetch('/camdiag/raw',{cache:'no-store'}).catch(()=>null);
    if(r&&r.status===200){
      const b=await r.blob(),a=document.createElement('a');
      a.href=URL.createObjectURL(b);a.download='raw-frame.bin';a.click();
      return out('Downloaded raw-frame.bin, '+b.size+' bytes (all UDP packets of one frame with headers).');
    }
    await new Promise(f=>setTimeout(f,1000));
  }
  out('No frame captured: is the camera sending?');
};
$('sStart').onclick=async()=>{const c=+$('ch').value;out((await post('/sniff/start'+(c?'/'+c:''))).text)};
$('sShow').onclick=async()=>out(await getText('/sniff'));
$('sStop').onclick=async()=>out((await post('/sniff/stop')).text);
$('sSend').onclick=async()=>out((await post('/camdiag/send/'+(+$('sp').value)+'/'+$('sx').value.replace(/[^0-9a-f]/gi,''))).text);
status();setInterval(status,3000);
</script></body></html>)HTML";

// Firmware update and restart
static const char UPDATE_HTML[] = PAGE_HEAD("Update")
    R"HTML(</head><body>)HTML" PAGE_NAV R"HTML(<main>
<h1>Firmware update</h1>
<div class='card'><dl class='kv' id='st'><dt>Firmware</dt><dd>loading…</dd></dl></div>
<div class='card'><h2>Install</h2>
<p class='muted small'>Choose the file <code>.pio/build/&lt;board&gt;/firmware.bin</code>, e.g.
<code>zb-gw03</code> (not <code>firmware.factory.bin</code>). The image stops briefly while the flash
is written; afterwards the device restarts.</p>
<input type='file' id='f' accept='.bin' class='field'>
<p><button id='go' class='primary'>Flash</button> <button id='rs'>Restart only</button></p>
<progress id='p' max='100' value='0'></progress>
<p id='msg'></p></div>
<div class='card'><h2>Factory reset</h2>
<p class='muted small'>Erases everything the device stored: orientation calibration, remembered camera,
home Wi-Fi for rescue mode, switches and settings. Then it restarts with the defaults. The firmware
stays.</p>
<p><button id='fr' class='danger'>Erase settings and restart</button></p></div>
</main><script src='/app.js'></script><script>
async function status(){
  try{
    const s=await (await fetch('/status',{cache:'no-store'})).json();
    const rs=s.mode==='rescue';
    document.body.classList.toggle('rescue',rs);
    kv($('st'),[['Firmware',esc(s.version)],['Commit',esc(s.commit||'–')],
      ['Uptime',Math.floor(s.uptime_s/3600)+' h '+Math.floor(s.uptime_s%3600/60)+' min'],
      rs&&['Mode','rescue: camera idle, restarts by itself when Ethernet is back','warnc'],
      rs&&['Ethernet',s.eth_ip?esc(s.eth_ip):'no IP, '+(!s.eth_begin?'init failed':s.eth_link?'link up':'no link')],
      rs&&['Wi-Fi',s.wifi_connected?esc(s.wifi_ssid)+', IP '+esc(s.wifi_ip):s.ap?'access point '+esc(s.ap)+' (192.168.4.1)':'not connected'],
      s.last_crash&&['Crash',esc(s.last_crash),'bad']]);
    return true;
  }catch(e){kv($('st'),[['Firmware','not reachable','bad']]);return false}
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
  const a=await post('/auth');  // asks for the OTA password if one is set
  if(!a.ok){$('msg').textContent=a.text;return}
  const x=new XMLHttpRequest();
  x.open('POST','/update');
  x.setRequestHeader('Content-Type','application/octet-stream');
  const h=otaHeaders();for(const k in h)x.setRequestHeader(k,h[k]);
  x.upload.onprogress=e=>{if(e.lengthComputable)$('p').value=e.loaded*100/e.total};
  x.onload=()=>{$('msg').textContent=x.status+': '+x.responseText;if(x.status===200)waitReboot()};
  x.onerror=()=>{$('msg').textContent='Transfer aborted.'};
  $('msg').textContent='Uploading…';
  x.send(file);
};
$('rs').onclick=async()=>{
  if(!confirm('Restart the device?'))return;
  const r=await post('/restart');
  $('msg').textContent=r.text;if(r.ok)waitReboot();
};
$('fr').onclick=async()=>{
  if(!confirm('Erase ALL stored settings (calibration, camera, home Wi-Fi, switches) and restart?'))return;
  const r=await post('/factory-reset');
  $('msg').textContent=r.text;if(r.ok)waitReboot();
};
status();
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
<p class='muted small'>The device remembers the chosen camera and reconnects to it at startup. If it
is off, the device takes another recognised camera if exactly one is in range. Scanning while the
image is running makes it stutter briefly. Automatic scan and the connection itself can be switched
off under <a href='/settings'>Settings</a>.</p></div>
<div class='card'><h2>Other networks</h2>
<p class='muted small'>Unknown camera? Try it here with protocol “automatic”: the device asks the
camera which protocol it speaks. No image? Pick another protocol and reconnect.</p>
<table id='oth' class='net'></table>
<input type='password' id='wpw' class='field' placeholder='Wi-Fi password of the camera (usually empty)'>
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
    ['Connection',c.enabled===false?'switched off':'on',c.enabled===false?'warnc':''],
    ['Automatic scan',c.autoscan?'on':'off'],
    ['Last scan',c.scan_age_s<0?'none yet':c.scan_age_s+' s ago']]);
  if(c.protocols&&c.protocols.length)PROTOS=c.protocols;
  // do not rebuild the tables while a protocol list is open (it would close)
  if(document.activeElement&&document.activeElement.matches('select[data-p]'))return;
  const rec=c.networks.filter(n=>n.proto), oth=c.networks.filter(n=>!n.proto);
  $('rec').innerHTML=rec.length?rec.map(n=>row(n,c)).join(''):'<tr><td class=muted>none in range</td></tr>';
  $('oth').innerHTML=oth.map(n=>row(n,c)).join('')||'<tr><td class=muted>none</td></tr>';
  document.querySelectorAll('button[data-s]').forEach(b=>b.onclick=()=>select(b.dataset.s));
  document.querySelectorAll('select[data-p]').forEach(s=>s.onchange=()=>{chosen[s.dataset.p]=s.value;s.blur()});
}
async function send(url,body){
  const r=await post(url,body,'application/x-www-form-urlencoded');
  $('msg').textContent=r.text;setTimeout(load,1500);
}
function select(ssid){
  const s=[...document.querySelectorAll('select[data-p]')].find(e=>e.dataset.p===ssid);
  send('/cameras/select',new URLSearchParams({ssid,pass:$('wpw').value,proto:s?s.value:'auto'}).toString())}
$('forget').onclick=()=>send('/cameras/select','ssid=');
$('scan').onclick=async()=>{await post('/cameras/scan');$('msg').textContent='Scanning…';setTimeout(load,4000)};
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
<p><button id='save' class='primary'>Save and connect</button> <button id='clear'>Delete</button></p>
<p id='msg'></p></div>
</main><script src='/app.js'></script><script>
async function load(){
  try{
    const s=await (await fetch('/status',{cache:'no-store'})).json();
    document.body.classList.toggle('rescue',s.mode==='rescue');
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
  const r=await post('/wifi-setup',new URLSearchParams({ssid,pass}).toString(),'application/x-www-form-urlencoded');
  $('msg').textContent=r.status?r.text:'Connection lost - the device may be switching Wi-Fi.';
  setTimeout(load,3000);
}
$('save').onclick=()=>save($('ssid').value,$('pass').value);
$('clear').onclick=()=>save('','');
$('scan').onclick=async()=>{await post('/cameras/scan');$('scanMsg').textContent='scanning…';
  setTimeout(()=>{$('scanMsg').textContent='';load()},5000)};
load();setInterval(load,5000);
</script></body></html>)HTML";

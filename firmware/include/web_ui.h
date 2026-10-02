#pragma once

// Web pages of the WiFi-Cam-Proxy (included by main.cpp only)

// --- Web pages ------------------------------------------------------------------
#define PAGE_STYLE                                                                           \
  "<meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>" \
  "<style>body{background:#111;color:#ddd;font-family:sans-serif;text-align:center;"         \
  "margin:0 16px}a{color:#8cf}img{max-width:95vw;max-height:80vh}img.round{border-radius:50%}"         \
  ".box{max-width:480px;margin:0 auto;text-align:left}"                                      \
  "button,input{font-size:1rem;margin:4px 0}progress{width:100%}"                            \
  "pre{background:#1b1b1b;padding:8px;border-radius:6px;white-space:pre-wrap}"               \
  ".warn{background:#5a3b00;padding:8px;border-radius:6px}</style>"

// Shared orientation maths for start page and calibration (/app.js)
static const char APP_JS[] = R"JS(
const $=id=>document.getElementById(id);
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
function lageAngle(x,y,c){return norm(correct(sensorAngle(x,y,c),c)-c.zero)}
// image rotation: always the base rotation, with orientation correction also -angle
function imageRotation(on,sm,c){return c.base-(on&&sm.have?lageAngle(sm.x,sm.y,c):0)}

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
)JS";

static const char INDEX_HTML[] = "<!doctype html><html><head><title>WiFi-Cam</title>" PAGE_STYLE
    R"HTML(<style>#wrap{display:inline-block;transition:transform .12s linear}
#view{display:inline-block;overflow:hidden;line-height:0;cursor:zoom-in}#view.z{cursor:grab;touch-action:none}
label{margin:0 8px;white-space:nowrap}.ctl{margin:8px 0}
#ledBtn{font-size:1.2rem;padding:4px 10px;border-radius:6px;border:none;cursor:pointer;background:#333;color:#ddd}
#ledBtn.on{background:#f5c518;color:#111}#cam{color:#888;font-size:.9rem}</style>
</head><body><h3>Live</h3>
<p id='choose' class='warn' hidden>Several cameras found. Please pick one under <a href='/cameras'>Choose camera</a>.</p>
<p id='cam'>…</p>
<div id='view' title='Double-click: zoom'><div id='wrap'><img id='img' src='/stream'></div></div>
<div class='ctl'><button id='zoom' title='Zoom in, then drag the image with the mouse or a finger'>2&times;</button>
<span id='ori'><label><input type='checkbox' id='on'> Correct orientation</label>
<label><input type='checkbox' id='round'> Round</label>
<button id='zero'>Current position = up</button></span>
<button id='ledBtn' title='Camera LED on/off' hidden>&#128261;</button></div>
<p id='ledMsg' style='font-size:.8rem;color:#888'></p>
<p><a href='/snapshot' download='snapshot.jpg'>Save snapshot</a> &middot;
<a href='/cameras'>Choose camera</a> &middot;
<a href='/calibrate' id='calLink'>Calibrate</a> &middot; <a href='/update'>Status &amp; update</a></p>
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
// LED (i4season command 0x0A). Shows the state confirmed by the camera.
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
async function info(){
  const c=await camInfo(); if(!c)return;
  $('choose').hidden=c.state!=='choose';
  const name=c.ssid||(c.state==='choose'?'none chosen':'looking for camera…');
  $('cam').textContent=[name,c.product,batteryText(c)].filter(Boolean).join(' · ');
  // orientation sensor: i4season reports it in the video header; before the first video
  // data (width 0) the otoscope view stays. MaxSee microscopes have none.
  if(c.state==='connected'){
    const ori=c.orientation||(c.proto==='i4season'&&!c.width);
    if(ori!==hasOri){hasOri=ori;applyRound();$('ori').hidden=!ori;$('calLink').hidden=!ori;apply()}
  }
  $('ledBtn').hidden=!c.led_supported;
  if(c.led>=0&&(c.led===1)!==ledOn){ledOn=c.led===1;applyLed()}
}
applyLed();
apply();
info();setInterval(info,5000);
loadCal().then(c=>{cal=c;apply()});
orientation(a=>{if(sm.add(a,cal))apply()});
</script></body></html>)HTML";

static const char CALIBRATE_HTML[] = "<!doctype html><html><head><title>Orientation calibration</title>" PAGE_STYLE
    R"HTML(<style>
.box{max-width:560px}.card{background:#1b1b1b;border-radius:8px;padding:10px 12px;margin:12px 0}
.card h4{margin:4px 0 8px}.row{display:flex;flex-wrap:wrap;gap:12px;justify-content:center;align-items:center}
#wrap{transition:transform .12s linear}#wrap img{width:200px;height:200px}
canvas{background:#111;border-radius:6px;max-width:100%}
#cov{display:flex;gap:1px;margin:6px 0}#cov div{flex:1;height:10px;background:#333}#cov div.on{background:#4a4}
.big{font-size:1.2rem;color:#fff;margin:6px 0}.ok{color:#6c6}.bad{color:#e86}
table{border-collapse:collapse;width:100%}td,th{border-bottom:1px solid #333;padding:3px 6px;text-align:right}
input[type=range]{width:100%}
</style></head><body><div class='box'>
<h3>Calibrate orientation</h3>
<p id='conn' class='bad'>Waiting for sensor data… (otoscope on and connected?)</p>

<div class='card'><h4>Live</h4>
<div class='row'>
 <div id='wrap'><img id='live' alt=''></div>
 <svg id='dial' width='200' height='200' viewBox='-100 -100 200 200'>
  <circle r='90' fill='none' stroke='#555' stroke-width='2'/>
  <g stroke='#777' stroke-width='2'><line y1='-90' y2='-78'/><line x1='90' x2='78'/><line y1='90' y2='78'/><line x1='-90' x2='-78'/></g>
  <text y='-62' fill='#888' text-anchor='middle' font-size='14'>up</text>
  <line id='needle' y2='-80' stroke='#8cf' stroke-width='5' stroke-linecap='round'/>
  <circle r='6' fill='#8cf'/>
 </svg>
</div>
<label><input type='checkbox' id='liveOn'> Show live image</label>
<pre id='vals'>–</pre>
<p>The needle shows where the side is that was up at the zero point. The image on the
left is rotated with the calibration you are editing here.</p>
</div>

<div class='card'><h4>1. Record a circle</h4>
<p>Hold the probe as level as possible and <b>slowly turn it once all the way around its
long axis</b>. The green fields show which angles have been covered. The live image is off
during recording so the Wi-Fi only has to carry sensor data.</p>
<button id='recBtn'>Start recording</button>
<div id='cov'></div>
<canvas id='plot' width='260' height='260'></canvas>
<p id='recStat'></p>
<p id='fitRes'></p>
<button id='fitUse' disabled>Apply result</button>
</div>

<div class='card'><h4>2. Check quarter turns</h4>
<p>Mark one side of the probe (e.g. the button). Then step by step <b>one quarter turn
further each time, always in the same direction</b>, and at each step hold still and
click “Capture”.</p>
<p class='big' id='qStep'></p>
<button id='qTake'>Capture</button> <button id='qReset'>Start over</button>
<table id='qTab'></table>
<p id='qRes'></p>
<button id='qUse' disabled>Apply as correction</button>
<button id='qClear'>Remove correction</button>
</div>

<div class='card'><h4>3. Zero point and smoothing</h4>
<p>The image is always rotated by −90° (mounting of the camera). The zero point is the
normal position of the probe in which no additional correction is needed (default 0°).
For fine-tuning, hold the probe so the image is the right way up, then:</p>
<button id='zero'>Current position = up</button>
<p>Smoothing: <span id='smV'></span> (left = direct/jittery, right = calm/sluggish)</p>
<input type='range' id='sm' min='0' max='0.9' step='0.05'>
</div>

<div class='card'><h4>Save</h4>
<button id='save'>Save on device</button> <button id='reset'>Reset everything</button>
<p id='msg'></p>
<p><a href='/'>To the live image</a> &middot; <a href='/update'>Status &amp; update</a></p>
</div>
</div>
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
    const l=lageAngle(sm.x,sm.y,W);
    rot(imageRotation(true,sm,W));
    $('needle').setAttribute('transform','rotate('+l.toFixed(1)+')');
  }else rot(W.base);
  if(last){
    const g=Math.hypot(last.x,last.y,last.z);
    $('vals').textContent='raw  x '+last.x+'  y '+last.y+'  z '+last.z+'  |g| '+g.toFixed(0)+
      '\nSensor angle '+sensorAngle(last.x,last.y,W).toFixed(1)+'°'+
      '   corrected '+correct(sensorAngle(last.x,last.y,W),W).toFixed(1)+'°'+
      '\nOrientation (to zero point) '+(sm.have?lageAngle(sm.x,sm.y,W).toFixed(1):'–')+'°';
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
  g.strokeStyle='#333';g.beginPath();g.moveTo(0,S);g.lineTo(2*S,S);g.moveTo(S,0);g.lineTo(S,2*S);g.stroke();
  g.fillStyle='#8cf';
  for(const[x,y]of pts)g.fillRect(S+x*k-1,S-y*k-1,2,2);
  if(fit){
    g.strokeStyle='#6c6';g.lineWidth=2;g.beginPath();
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

static const char UPDATE_HTML[] = "<!doctype html><html><head><title>WiFi-Cam update</title>"
    PAGE_STYLE R"(</head><body><div class='box'>
<h3>WiFi-Cam-Proxy</h3>
<p id='rescue' class='warn' hidden>Rescue mode: Ethernet has no IP, Wi-Fi is on the home
network or the own access point instead of the camera. When Ethernet comes back, the
device restarts by itself. <a href='/wifi-setup'>Set up home Wi-Fi</a></p>
<pre id='st'>loading status…</pre>
<h4>Wi-Fi to the camera</h4>
<p>Takes effect immediately, the device briefly reconnects. Afterwards compare the lost
packets above.</p>
<p><button class='wm' data-m='bgn'>b/g/n</button> fast, aggregates packets<br>
<button class='wm' data-m='bg'>b/g</button> every packet on its own (default)<br>
<button class='wm' data-m='b'>b only</button> slow, most robust with a weak signal</p>
<h4>Ethernet</h4>
<p><button class='eth' data-v='1'>10 Mbit</button> required on the ZB-GW03 (Wi-Fi disturbs the clock otherwise)<br>
<button class='eth' data-v='0'>100 Mbit</button> for boards with their own oscillator (WT32-ETH01)</p>
<h4>Firmware update</h4>
<p>Choose the file <code>.pio/build/&lt;board&gt;/firmware.bin</code>, e.g. <code>zb-gw03</code>
(not <code>firmware.factory.bin</code>).</p>
<input type='file' id='f' accept='.bin'><br>
<input type='password' id='pw' placeholder='OTA password (if set)'><br>
<button id='go'>Flash</button> <button id='rs'>Restart</button>
<progress id='p' max='100' value='0'></progress>
<p id='msg'></p>
<p><a href='/'>To the live image</a> &middot; <a href='/cameras'>Choose camera</a> &middot;
<a href='/wifi-setup'>Home Wi-Fi (rescue)</a></p>
</div><script>
const $=id=>document.getElementById(id);
async function status(){
  try{
    const s=await (await fetch('/status',{cache:'no-store'})).json();
    $('rescue').hidden=s.mode!=='rescue';
    $('st').textContent=
      'Version:   '+s.version+'\nReset:     '+s.reset_reason+' (boot #'+s.boot_count+')'+'\nMode:      '+s.mode+
      '\nEthernet:  '+(s.eth_ip||('no IP, '+(!s.eth_begin?'init failed':s.eth_link?'link up':'no link')))+
      (s.eth_speed?', '+s.eth_speed+' Mbit':'')+(s.eth10?' (10 Mbit set)':'')+
      (s.ap?'\nSetup AP:  '+s.ap+' (192.168.4.1)':'')+
      '\nWi-Fi:     '+(s.wifi_connected?s.wifi_ssid+' ('+s.wifi_rssi+' dBm)':'disconnected')+', mode '+s.wifi_mode+
      '\nCamera:    '+(s.cam_proto||'none')+(s.battery>=0?', battery '+s.battery+' %':'')+
      '\nVideo:     '+s.fps.toFixed(1)+' fps, '+s.frames+' frames, '+s.dropped+' dropped'+
      '\n           (memory '+s.drop_nomem+', too large '+s.drop_toobig+', incomplete '+s.drop_incomplete+
      ', packets lost '+s.packets_lost+', shown damaged '+s.damaged+', largest frame '+s.max_frame+' B)'+
      '\nStalls:    '+s.stalls_loss+' after packet loss, '+s.stalls_clean+' without'+
      (s.clean_stall_times.length?' (at '+s.clean_stall_times.join(', ')+' s)':'')+
      ' · keepalives '+s.keepalives+
      '\nViewers:   '+s.stream_clients+'\nHeap:      '+s.free_heap+' bytes free'+
      '\nUptime:    '+s.uptime_s+' s'+
      (s.last_crash?'\nCrash:     '+s.last_crash:'');
    return true;
  }catch(e){$('st').textContent='not reachable';return false}
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
  const h={};if($('pw').value)h['X-OTA-Password']=$('pw').value;
  const r=await fetch('/restart',{method:'POST',headers:h});
  $('msg').textContent=r.status+': '+await r.text();if(r.ok)waitReboot();
};
document.querySelectorAll('.eth').forEach(b=>b.onclick=async()=>{
  const h={};if($('pw').value)h['X-OTA-Password']=$('pw').value;
  const r=await fetch('/eth10/'+b.dataset.v,{method:'POST',headers:h});
  $('msg').textContent=r.status+': '+await r.text();
});
document.querySelectorAll('.wm').forEach(b=>b.onclick=async()=>{
  const h={};if($('pw').value)h['X-OTA-Password']=$('pw').value;
  const r=await fetch('/wifi/'+b.dataset.m,{method:'POST',headers:h});
  $('msg').textContent=r.status+': '+await r.text();status();
});
status();setInterval(status,3000);
</script></body></html>)";

// Choose camera: recognised cameras (SSID patterns) and all other networks from the scan
static const char CAMERAS_HTML[] = "<!doctype html><html><head><title>Choose camera</title>" PAGE_STYLE
    R"HTML(<style>table{border-collapse:collapse;width:100%}td,th{border-bottom:1px solid #333;padding:4px 6px;text-align:left}
.cur{color:#6c6}.dim{color:#777}select,input[type=password]{font-size:1rem}</style>
</head><body><div class='box'>
<h3>Choose camera</h3>
<pre id='st'>loading…</pre>
<p>Cameras are recognised by their Wi-Fi name. The device remembers the chosen camera
and reconnects to it at startup. If it is off, the device takes another recognised
camera if exactly one is in range.</p>
<p><button id='scan'>Rescan</button> <button id='forget'>Clear selection (automatic)</button></p>
<p class='dim'>Scanning while the image is running makes it stutter briefly.</p>
<h4>Recognised cameras</h4><table id='rec'></table>
<h4>Other networks</h4>
<p class='dim'>Unknown camera? Try it here with protocol “automatic” (192.168.29.1 →
MaxSee, otherwise i4season).</p>
<table id='oth'></table>
<p>Protocol <select id='proto'><option value='auto'>automatic</option>
<option value='i4season'>i4season (Soulear, MS5, MAX-VIEW)</option>
<option value='jhcmd'>MaxSee/JoyHonest (JHCMD)</option></select></p>
<p><input type='password' id='wpw' placeholder='Wi-Fi password of the camera (usually empty)'><br>
<input type='password' id='pw' placeholder='OTA password (if set)'></p>
<p id='msg'></p>
<p><a href='/'>To the live image</a> &middot; <a href='/update'>Status &amp; update</a></p>
</div><script src='/app.js'></script><script>
const STATE={connected:'connected',connecting:'connecting…',scanning:'scanning…',choose:'several found, please choose',searching:'no camera found, still searching',restart:'reconnecting…'};
const esc=t=>String(t).replace(/[&<>"']/g,c=>'&#'+c.charCodeAt(0)+';');
function row(n,c){
  const cur=n.ssid===c.ssid&&c.state==='connected';
  return '<tr><td class='+(cur?'cur':'')+'>'+esc(n.ssid)+(n.ssid===c.preferred?' ★':'')+'</td><td>'+n.rssi+' dBm</td><td>'+
    (n.open?'open':'password')+'</td><td>'+(n.proto||'–')+'</td><td>'+
    (cur?'active':'<button data-s="'+esc(n.ssid)+'">Connect</button>')+'</td></tr>';
}
async function load(){
  const c=await camInfo();
  if(!c){$('st').textContent='not reachable';return}
  $('st').textContent='State:    '+(STATE[c.state]||c.state)+
    '\nCamera:   '+(c.ssid||'–')+(c.proto?' ('+c.proto+')':'')+
    '\nDevice:   '+([c.vendor,c.product,c.firmware].filter(Boolean).join(' ')||'–')+
    (c.width?'\nImage:    '+c.width+'×'+c.height+' (as reported by the camera)':'')+
    (c.battery>=0?'\nBattery:  '+c.battery+' %':'')+
    '\nRemembered: '+(c.preferred||'– (automatic)')+
    '\nScan:     '+(c.scan_age_s<0?'none yet':c.scan_age_s+' s ago');
  const rec=c.networks.filter(n=>n.proto), oth=c.networks.filter(n=>!n.proto);
  $('rec').innerHTML=rec.length?rec.map(n=>row(n,c)).join(''):'<tr><td class=dim>none</td></tr>';
  $('oth').innerHTML=oth.map(n=>row(n,c)).join('');
  document.querySelectorAll('button[data-s]').forEach(b=>b.onclick=()=>select(b.dataset.s));
}
async function post(url,body){
  const h={'Content-Type':'application/x-www-form-urlencoded'};if($('pw').value)h['X-OTA-Password']=$('pw').value;
  const r=await fetch(url,{method:'POST',headers:h,body});
  $('msg').textContent=r.status+': '+await r.text();setTimeout(load,1500);
}
function select(ssid){post('/cameras/select',new URLSearchParams({ssid,pass:$('wpw').value,proto:$('proto').value}).toString())}
$('forget').onclick=()=>post('/cameras/select','ssid=');
$('scan').onclick=async()=>{await fetch('/cameras/scan',{method:'POST'});$('msg').textContent='Scanning…';setTimeout(load,4000)};
load();setInterval(load,4000);
</script></body></html>)HTML";

// Set up the home Wi-Fi for rescue mode (also via the own access point)
static const char WIFI_SETUP_HTML[] = "<!doctype html><html><head><title>Wi-Fi setup</title>" PAGE_STYLE
    R"HTML(<style>table{border-collapse:collapse;width:100%}td{border-bottom:1px solid #333;padding:6px}
tr.n{cursor:pointer}tr.n:hover{background:#222}.dim{color:#777}input{width:100%;box-sizing:border-box}</style>
</head><body><div class='box'>
<h3>Set up home Wi-Fi</h3>
<pre id='st'>loading…</pre>
<p>If Ethernet has no connection, the device switches to this Wi-Fi so the web UI and
updates stay reachable. If that fails too, it opens its own access point
(<code>WiFi-Cam-…</code>), through which you get here.</p>
<h4>Networks in range</h4>
<p><button id='scan'>Scan</button> <span id='scanMsg' class='dim'></span></p>
<table id='nets'></table>
<p><input id='ssid' placeholder='Wi-Fi name (SSID)' maxlength='32'></p>
<p><input type='password' id='pass' placeholder='Wi-Fi password' maxlength='64'></p>
<p><input type='password' id='pw' placeholder='OTA password (if set)'></p>
<p><button id='save'>Save and connect</button> <button id='clear'>Delete</button></p>
<p id='msg'></p>
<p><a href='/'>To the live image</a> &middot; <a href='/update'>Status &amp; update</a></p>
</div><script>
const $=id=>document.getElementById(id);
const esc=t=>String(t).replace(/[&<>"']/g,c=>'&#'+c.charCodeAt(0)+';');
async function load(){
  try{
    const s=await (await fetch('/status',{cache:'no-store'})).json();
    $('st').textContent='Mode:      '+(s.mode==='rescue'?'rescue':'normal (Ethernet '+(s.eth_ip||'without IP')+')')+
      '\nHome Wi-Fi: '+(s.home_ssid||'– not set up')+
      (s.mode==='rescue'?'\nWi-Fi:     '+(s.wifi_connected?s.wifi_ssid+', IP '+s.wifi_ip:'not connected'):'')+
      (s.ap?'\nSetup AP:  '+s.ap+' (192.168.4.1)':'');
  }catch(e){$('st').textContent='not reachable'}
  try{
    const c=await (await fetch('/cameras.json',{cache:'no-store'})).json();
    $('nets').innerHTML=c.networks.map(n=>'<tr class=n data-s="'+esc(n.ssid)+'"><td>'+esc(n.ssid)+'</td><td>'+n.rssi+
      ' dBm</td><td>'+(n.open?'open':'&#128274;')+'</td></tr>').join('')||'<tr><td class=dim>no scan yet</td></tr>';
    document.querySelectorAll('tr.n').forEach(r=>r.onclick=()=>{$('ssid').value=r.dataset.s;$('pass').focus()});
  }catch(e){}
}
async function save(ssid,pass){
  const h={'Content-Type':'application/x-www-form-urlencoded'};if($('pw').value)h['X-OTA-Password']=$('pw').value;
  try{
    const r=await fetch('/wifi-setup',{method:'POST',headers:h,body:new URLSearchParams({ssid,pass}).toString()});
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

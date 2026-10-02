#pragma once

// Webseiten der Otoskop-Bridge (nur von main.cpp eingebunden)

// --- Webseiten ------------------------------------------------------------------
#define PAGE_STYLE                                                                           \
  "<meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>" \
  "<style>body{background:#111;color:#ddd;font-family:sans-serif;text-align:center;"         \
  "margin:0 16px}a{color:#8cf}img{max-width:95vw;max-height:80vh;border-radius:50%}"         \
  ".box{max-width:480px;margin:0 auto;text-align:left}"                                      \
  "button,input{font-size:1rem;margin:4px 0}progress{width:100%}"                            \
  "pre{background:#1b1b1b;padding:8px;border-radius:6px;white-space:pre-wrap}"               \
  ".warn{background:#5a3b00;padding:8px;border-radius:6px}</style>"

// Gemeinsame Lage-Berechnung für Startseite und Kalibrierung (/app.js)
static const char APP_JS[] = R"JS(
const $=id=>document.getElementById(id);
const norm=a=>((a%360)+540)%360-180;   // Winkel auf -180..180
const store={get(k,d){try{const v=localStorage.getItem(k);return v===null?d:JSON.parse(v)}catch(e){return d}},
  set(k,v){try{localStorage.setItem(k,JSON.stringify(v))}catch(e){}}};

// Kalibrierung (auf dem Gerät gespeichert): Ellipse (Versatz/Skalierung pro Achse),
// optionale Korrekturtabelle aus Vierteldrehungen, Nullpunkt, Glättung
// base=-90: Das Kamerabild ist im Stift um 90° gedreht eingebaut und wird immer so
// gedreht (am Gerät ermittelt). zero = Sensorwinkel in der Normallage des Stifts;
// nur Abweichungen davon gleicht die Lagekorrektur zusätzlich aus.
const DEFAULT_CAL={v:2,base:-90,ox:0,oy:0,sx:1,sy:1,zero:0,smooth:0.6,pts:null};
async function loadCal(){
  try{
    const r=await fetch('/calibration',{cache:'no-store'}), c=await r.json();
    // Format 1 kannte keine Grunddrehung, der Nullpunkt enthielt sie (-90)
    if(!c.v&&typeof c.zero==='number'){c.zero+=90;c.v=2}
    return Object.assign({},DEFAULT_CAL,c);
  }catch(e){return Object.assign({},DEFAULT_CAL)}
}
async function saveCal(c){
  const r=await fetch('/calibration',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(c)});
  if(!r.ok)throw new Error(await r.text());
}

// Rohwerte -> Winkel des Sensors in Grad
function sensorAngle(x,y,c){return Math.atan2((x-c.ox)/c.sx,(y-c.oy)/c.sy)*180/Math.PI}

// Korrekturtabelle pts=[[Sensorwinkel, Sollwinkel 0/90/180/270], ...]
function tableInfo(pts){
  if(!pts||pts.length<3)return null;
  const s0=pts[0][0], k=norm(pts[1][0]-s0)>=0?1:-1;
  const u=pts.map(p=>((k*(p[0]-s0))%360+360)%360);
  for(let i=1;i<u.length;i++)if(!(u[i]>u[i-1]))return null;  // muss der Reihe nach ansteigen
  return {s0,k,u,t:pts.map(p=>p[1])};
}
function correct(a,c){
  const ti=tableInfo(c.pts); if(!ti)return a;
  const v=((ti.k*(a-ti.s0))%360+360)%360, U=[...ti.u,360], T=[...ti.t,360];
  let i=0; while(i<U.length-2&&v>U[i+1])i++;
  return ti.s0+ti.k*(T[i]+(v-U[i])/(U[i+1]-U[i])*(T[i+1]-T[i]));
}
// Lage relativ zum Nullpunkt; das Bild wird um das Negative davon gedreht
function lageAngle(x,y,c){return norm(correct(sensorAngle(x,y,c),c)-c.zero)}
// Drehung des Bildes: immer die Grunddrehung, mit Lagekorrektur zusätzlich -Lage
function imageRotation(on,sm,c){return c.base-(on&&sm.have?lageAngle(sm.x,sm.y,c):0)}

// Glättet den Vektor statt des Winkels (kein Sprung bei 180/-180)
class Smoother{
  constructor(){this.x=0;this.y=0;this.have=false}
  add(a,c){
    // Stift zeigt steil nach oben/unten -> Rollwinkel unbestimmt, letzten Wert behalten
    if(Math.hypot((a.x-c.ox)/c.sx,(a.y-c.oy)/c.sy)<0.25)return false;
    const s=this.have?c.smooth:0;
    this.x=this.x*s+a.x*(1-s); this.y=this.y*s+a.y*(1-s); this.have=true;
    return true;
  }
}
// Dreht ein Element immer den kürzesten Weg
function rotator(el){let shown=0;return t=>{shown+=norm(t-shown);el.style.transform='rotate('+shown.toFixed(1)+'deg)'}}

// Sensorwerte vom Gerät (Server-Sent Events), verbindet sich selbst neu
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

static const char INDEX_HTML[] = "<!doctype html><html><head><title>Otoskop</title>" PAGE_STYLE
    R"HTML(<style>#wrap{display:inline-block;transition:transform .12s linear}
label{margin:0 8px;white-space:nowrap}.ctl{margin:8px 0}
#ledBtn{font-size:1.2rem;padding:4px 10px;border-radius:6px;border:none;cursor:pointer;background:#333;color:#ddd}
#ledBtn.on{background:#f5c518;color:#111}</style>
</head><body><h3>Otoskop Live</h3>
<div id='wrap'><img src='/stream'></div>
<div class='ctl'><label><input type='checkbox' id='on'> Lage korrigieren</label>
<button id='zero'>Aktuelle Lage = oben</button>
<button id='ledBtn' title='Otoskop-LED ein/aus'>&#128261;</button></div>
<p id='ledMsg' style='font-size:.8rem;color:#888'></p>
<p><a href='/snapshot' download='otoskop.jpg'>Snapshot speichern</a> &middot;
<a href='/calibrate'>Kalibrieren</a> &middot; <a href='/update'>Status &amp; Update</a></p>
<script src='/app.js'></script><script>
let cal=Object.assign({},DEFAULT_CAL);
const sm=new Smoother(), rot=rotator($('wrap'));
$('on').checked=store.get('on',false);
function apply(){rot(imageRotation($('on').checked,sm,cal))}
$('on').onchange=()=>{store.set('on',$('on').checked);apply()};
$('zero').onclick=async()=>{
  if(!sm.have)return;
  cal.zero=correct(sensorAngle(sm.x,sm.y,cal),cal);
  $('on').checked=true;store.set('on',true);apply();
  try{await saveCal(cal)}catch(e){alert('Speichern fehlgeschlagen: '+e.message)}
};
// LED-Steuerung (SetLed 0x0A, laut i4season-Protokoll; am Gerät bisher ungetestet)
let ledOn=store.get('led',false);
function applyLed(){$('ledBtn').className=ledOn?'on':'';$('ledBtn').title='LED '+(ledOn?'an – klicken zum Ausschalten':'aus – klicken zum Einschalten')}
$('ledBtn').onclick=async()=>{
  ledOn=!ledOn;store.set('led',ledOn);applyLed();
  try{
    const r=await fetch('/led/'+(ledOn?'1':'0'),{method:'POST'});
    const t=await r.text();
    if(!r.ok){$('ledMsg').textContent='Fehler: '+t;ledOn=!ledOn;store.set('led',ledOn);applyLed()}
    else{$('ledMsg').textContent='LED '+(ledOn?'an':'aus')+(t.includes('ungetestet')?'(Protokoll ungetestet)':'')}
  }catch(e){$('ledMsg').textContent='Nicht erreichbar';ledOn=!ledOn;store.set('led',ledOn);applyLed()}
};
applyLed();
apply();
loadCal().then(c=>{cal=c;apply()});
orientation(a=>{if(sm.add(a,cal))apply()});
</script></body></html>)HTML";

static const char CALIBRATE_HTML[] = "<!doctype html><html><head><title>Otoskop Kalibrierung</title>" PAGE_STYLE
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
<h3>Lage kalibrieren</h3>
<p id='conn' class='bad'>Warte auf Sensordaten… (Otoskop an und verbunden?)</p>

<div class='card'><h4>Live</h4>
<div class='row'>
 <div id='wrap'><img id='live' alt=''></div>
 <svg id='dial' width='200' height='200' viewBox='-100 -100 200 200'>
  <circle r='90' fill='none' stroke='#555' stroke-width='2'/>
  <g stroke='#777' stroke-width='2'><line y1='-90' y2='-78'/><line x1='90' x2='78'/><line y1='90' y2='78'/><line x1='-90' x2='-78'/></g>
  <text y='-62' fill='#888' text-anchor='middle' font-size='14'>oben</text>
  <line id='needle' y2='-80' stroke='#8cf' stroke-width='5' stroke-linecap='round'/>
  <circle r='6' fill='#8cf'/>
 </svg>
</div>
<label><input type='checkbox' id='liveOn'> Livebild anzeigen</label>
<pre id='vals'>–</pre>
<p>Der Zeiger zeigt, wo die Seite ist, die beim Nullpunkt oben war. Das Bild links wird
mit der Kalibrierung gedreht, die du hier gerade bearbeitest.</p>
</div>

<div class='card'><h4>1. Kreis aufzeichnen</h4>
<p>Stift möglichst waagerecht halten und <b>langsam einmal ganz um die Längsachse drehen</b>.
Die grünen Felder zeigen, welche Winkel schon erfasst sind. Das Livebild ist während der
Aufzeichnung aus, damit das WLAN nur Sensordaten tragen muss.</p>
<button id='recBtn'>Aufzeichnung starten</button>
<div id='cov'></div>
<canvas id='plot' width='260' height='260'></canvas>
<p id='recStat'></p>
<p id='fitRes'></p>
<button id='fitUse' disabled>Ergebnis übernehmen</button>
</div>

<div class='card'><h4>2. Vierteldrehungen prüfen</h4>
<p>Markiere dir eine Seite des Stifts (z.B. den Knopf). Dann Schritt für Schritt jeweils
<b>eine Vierteldrehung weiter, immer in dieselbe Richtung</b>, und bei jedem Schritt
ruhig halten und auf „Aufnehmen“ klicken.</p>
<p class='big' id='qStep'></p>
<button id='qTake'>Aufnehmen</button> <button id='qReset'>Neu beginnen</button>
<table id='qTab'></table>
<p id='qRes'></p>
<button id='qUse' disabled>Als Korrektur übernehmen</button>
<button id='qClear'>Korrektur entfernen</button>
</div>

<div class='card'><h4>3. Nullpunkt und Glättung</h4>
<p>Das Bild wird immer um −90° gedreht (Einbaulage der Kamera). Der Nullpunkt ist die
Normallage des Stifts, in der keine zusätzliche Korrektur nötig ist (Standard 0°). Zum
Feinjustieren den Stift so halten, dass das Bild richtig herum steht, dann:</p>
<button id='zero'>Aktuelle Lage = oben</button>
<p>Glättung: <span id='smV'></span> (links = direkt/zappelig, rechts = ruhig/träge)</p>
<input type='range' id='sm' min='0' max='0.9' step='0.05'>
</div>

<div class='card'><h4>Speichern</h4>
<button id='save'>Auf Gerät speichern</button> <button id='reset'>Alles zurücksetzen</button>
<p id='msg'></p>
<p><a href='/'>Zum Live-Bild</a> &middot; <a href='/update'>Status &amp; Update</a></p>
</div>
</div>
<script src='/app.js'></script><script>
let W=Object.assign({},DEFAULT_CAL), dirty=false;
const sm=new Smoother(), rot=rotator($('wrap'));
let last=null, recent=[];           // letzte Rohwerte
let rec=false, pts=[], recT=[];     // Kreis-Aufzeichnung (Punkte, Zeitstempel)
let fit=null, q=[];                 // Ellipsen-Ergebnis, Vierteldrehungen
const Q_TEXT=['Markierung nach OBEN','¼ weiter (Markierung RECHTS bzw. seitlich)',
  '¼ weiter (Markierung UNTEN)','¼ weiter (Markierung LINKS bzw. andere Seite)'];

// Livebild nur bei Bedarf: ohne Stream bleibt mehr WLAN-Bandbreite für die Sensordaten
function live(){$('live').src=($('liveOn').checked&&!rec)?'/stream?'+Date.now():''}
$('liveOn').checked=store.get('calLive',false);
$('liveOn').onchange=()=>{store.set('calLive',$('liveOn').checked);live()};
live();

function changed(){dirty=true;$('msg').textContent='Ungespeicherte Änderungen.';$('msg').className='bad';render()}

// --- Live ---
function render(){
  if(sm.have){
    const l=lageAngle(sm.x,sm.y,W);
    rot(imageRotation(true,sm,W));
    $('needle').setAttribute('transform','rotate('+l.toFixed(1)+')');
  }else rot(W.base);
  if(last){
    const g=Math.hypot(last.x,last.y,last.z);
    $('vals').textContent='roh  x '+last.x+'  y '+last.y+'  z '+last.z+'  |g| '+g.toFixed(0)+
      '\nSensorwinkel '+sensorAngle(last.x,last.y,W).toFixed(1)+'°'+
      '   korrigiert '+correct(sensorAngle(last.x,last.y,W),W).toFixed(1)+'°'+
      '\nLage (zum Nullpunkt) '+(sm.have?lageAngle(sm.x,sm.y,W).toFixed(1):'–')+'°';
  }
  $('smV').textContent=W.smooth.toFixed(2); $('sm').value=W.smooth;
  renderQ();
}
orientation(a=>{
  last=a; recent.push(a); if(recent.length>8)recent.shift();
  $('conn').textContent='Sensor verbunden'; $('conn').className='ok';
  if(rec){pts.push([a.x,a.y]);recT.push(performance.now());drawPlot()}
  sm.add(a,W); render();
},ok=>{if(!ok){$('conn').textContent='Verbindung unterbrochen, verbinde neu…';$('conn').className='bad'}});

// --- 1. Kreis ---
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
    $('recStat').innerHTML='Datenrate '+rate.toFixed(1)+' Werte/s (Soll ~17) &middot; längste Pause '+
      '<span class='+(gap>300?'bad':'ok')+'>'+gap.toFixed(0)+' ms</span>'+
      (gap>300?' – Lücken im Kreis kommen dann von Aussetzern im WLAN, nicht vom Sensor.':'');
  }
  if(rec&&n>=34){stopRec()}
}
// Achsparallele Ellipse A x² + B y² + C x + D y = 1 (kleinste Quadrate)
function fitEllipse(P){
  const M=[0,1,2,3].map(()=>[0,0,0,0]),v=[0,0,0,0];
  for(const[x,y]of P){const r=[x*x,y*y,x,y];for(let i=0;i<4;i++){v[i]+=r[i];for(let j=0;j<4;j++)M[i][j]+=r[i]*r[j]}}
  for(let c=0;c<4;c++){   // Gauß mit Pivotsuche
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
  rec=false;$('recBtn').textContent='Neu aufzeichnen';live();
  fit=fitEllipse(pts);
  if(!fit||coverage()<30){
    $('fitRes').innerHTML='<span class=bad>Zu wenig Abdeckung oder kein sinnvoller Kreis. Bitte eine ganze Umdrehung, langsam.</span>';
    fit=null;$('fitUse').disabled=true;
  }else{
    $('fitRes').innerHTML='Mitte x '+fit.ox.toFixed(1)+', y '+fit.oy.toFixed(1)+
      ' &middot; Radius x '+fit.sx.toFixed(1)+', y '+fit.sy.toFixed(1)+
      ' &middot; Abweichung '+(fit.rms*100).toFixed(1)+' %'+
      (fit.rms<0.08?' <span class=ok>(gut)</span>':' <span class=bad>(unruhig – evtl. wiederholen)</span>');
    $('fitUse').disabled=false;
  }
  drawPlot();
}
$('recBtn').onclick=()=>{
  if(rec)return stopRec();
  pts=[];recT=[];fit=null;rec=true;$('fitUse').disabled=true;$('fitRes').textContent='';
  $('recStat').textContent='';$('recBtn').textContent='Aufzeichnung beenden';live();drawPlot();
};
$('fitUse').onclick=()=>{
  Object.assign(W,{ox:fit.ox,oy:fit.oy,sx:fit.sx,sy:fit.sy});
  // Die Tabelle bezieht sich auf die alte Ellipse -> neu machen
  W.pts=null;q=[];
  $('fitRes').innerHTML+='<br><span class=ok>Übernommen. Schritt 2 bei Bedarf neu machen, Nullpunkt ggf. nachjustieren.</span>';
  changed();
};

// --- 2. Vierteldrehungen ---
function avgAngle(){
  let x=0,y=0;for(const a of recent){x+=(a.x-W.ox)/W.sx;y+=(a.y-W.oy)/W.sy}
  return Math.atan2(x,y)*180/Math.PI;
}
function renderQ(){
  $('qStep').textContent=q.length<4?'Schritt '+(q.length+1)+'/4: '+Q_TEXT[q.length]:'Fertig.';
  $('qTake').disabled=q.length>=4||recent.length<4;
  let h='<tr><th>Schritt</th><th>Sensor</th><th>Abstand zum vorigen</th><th>Fehler</th></tr>';
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
  $('qRes').innerHTML=q.length<4?'':(!ti?'<span class=bad>Reihenfolge passt nicht (Richtung gewechselt?). Neu beginnen.</span>':
    'Größte Abweichung '+maxErr.toFixed(1)+'°. '+(maxErr<5?'<span class=ok>Das ist schon gut, eine Korrektur ist kaum nötig.</span>':
    '<span class=bad>Mit „Als Korrektur übernehmen“ wird das ausgeglichen.</span>'));
  $('qClear').disabled=!W.pts;
}
$('qTake').onclick=()=>{q.push(avgAngle());renderQ()};
$('qReset').onclick=()=>{q=[];renderQ()};
$('qUse').onclick=()=>{W.pts=q.map((s,i)=>[s,i*90]);changed();
  $('qRes').innerHTML+='<br><span class=ok>Übernommen.</span>'};
$('qClear').onclick=()=>{W.pts=null;changed()};

// --- 3. Nullpunkt, Glättung ---
$('zero').onclick=()=>{if(!sm.have)return;W.zero=correct(sensorAngle(sm.x,sm.y,W),W);changed()};
$('sm').oninput=()=>{W.smooth=parseFloat($('sm').value);changed()};

// --- Speichern ---
$('save').onclick=async()=>{
  try{await saveCal(W);dirty=false;$('msg').textContent='Gespeichert.';$('msg').className='ok'}
  catch(e){$('msg').textContent='Speichern fehlgeschlagen: '+e.message;$('msg').className='bad'}
};
$('reset').onclick=()=>{if(confirm('Kalibrierung auf Werkseinstellung zurücksetzen?')){W=Object.assign({},DEFAULT_CAL);q=[];fit=null;changed()}};
addEventListener('beforeunload',e=>{if(dirty){e.preventDefault();e.returnValue=''}});

loadCal().then(c=>{W=c;render();drawPlot()});
render();drawPlot();
</script></body></html>)HTML";

static const char UPDATE_HTML[] = "<!doctype html><html><head><title>Otoskop Update</title>"
    PAGE_STYLE R"(</head><body><div class='box'>
<h3>Otoskop-Bridge</h3>
<p id='rescue' class='warn' hidden>Notfall-Modus: Ethernet hat keine IP, das WLAN hängt im
Heimnetz statt am Otoskop. Kommt Ethernet zurück, startet das Gerät von selbst neu.</p>
<pre id='st'>lade Status…</pre>
<h4>WLAN zum Otoskop</h4>
<p>Wirkt sofort, das Gerät verbindet sich kurz neu. Vergleiche danach in
<a href='/log'>/log</a> die Spalte <code>lost</code>.</p>
<p><button class='wm' data-m='bgn'>b/g/n</button> schnell, bündelt Pakete<br>
<button class='wm' data-m='bg'>b/g</button> jedes Paket einzeln (Standard)<br>
<button class='wm' data-m='b'>nur b</button> langsam, am robustesten bei schwachem Signal</p>
<h4>Firmware-Update</h4>
<p>Datei <code>.pio/build/zb-gw03/firmware.bin</code> wählen (nicht <code>firmware.factory.bin</code>).</p>
<input type='file' id='f' accept='.bin'><br>
<input type='password' id='pw' placeholder='OTA-Passwort (falls gesetzt)'><br>
<button id='go'>Flashen</button> <button id='rs'>Neustart</button>
<progress id='p' max='100' value='0'></progress>
<p id='msg'></p>
<p><a href='/'>Zum Live-Bild</a> &middot; <a href='/log'>Protokoll vor dem letzten Neustart</a></p>
</div><script>
const $=id=>document.getElementById(id);
async function status(){
  try{
    const s=await (await fetch('/status',{cache:'no-store'})).json();
    $('rescue').hidden=s.mode!=='rescue';
    $('st').textContent=
      'Version:   '+s.version+'\nReset:     '+s.reset_reason+' (Boot #'+s.boot_count+')'+'\nModus:     '+s.mode+
      '\nEthernet:  '+(s.eth_ip||('keine IP, '+(!s.eth_begin?'Init fehlgeschlagen':s.eth_link?'Link da':'kein Link')))+
      '\nWLAN:      '+(s.wifi_connected?s.wifi_ssid+' ('+s.wifi_rssi+' dBm)':'getrennt')+', Modus '+s.wifi_mode+
      '\nVideo:     '+s.fps.toFixed(1)+' fps, '+s.frames+' Bilder, '+s.dropped+' verworfen'+
      '\n           (Speicher '+s.drop_nomem+', zu groß '+s.drop_toobig+', unvollständig '+s.drop_incomplete+
      ', Pakete verloren '+s.packets_lost+', beschädigt angezeigt '+s.damaged+', größtes Bild '+s.max_frame+' B)'+
      '\nStillstand: '+s.stalls_loss+' nach Paketverlust, '+s.stalls_clean+' ohne'+
      (s.clean_stall_times.length?' (bei '+s.clean_stall_times.join(', ')+' s)':'')+
      ' · Lebenszeichen '+s.keepalives+
      '\nZuschauer: '+s.stream_clients+'\nHeap:      '+s.free_heap+' Byte frei'+
      '\nLaufzeit:  '+s.uptime_s+' s'+
      (s.last_crash?'\nAbsturz:   '+s.last_crash:'');
    return true;
  }catch(e){$('st').textContent='nicht erreichbar';return false}
}
function waitReboot(){
  let n=0;
  const t=setInterval(async()=>{
    if(++n>3&&await status()){clearInterval(t);$('msg').textContent='Gerät läuft wieder.'}
  },2000);
}
$('go').onclick=async()=>{
  const file=$('f').files[0];
  if(!file){$('msg').textContent='Keine Datei gewählt.';return}
  const head=new Uint8Array(await file.slice(0,1).arrayBuffer());
  if(head[0]!==0xE9){$('msg').textContent='Das ist keine ESP32-App (erstes Byte nicht 0xE9).';return}
  const x=new XMLHttpRequest();
  x.open('POST','/update');
  x.setRequestHeader('Content-Type','application/octet-stream');
  if($('pw').value)x.setRequestHeader('X-OTA-Password',$('pw').value);
  x.upload.onprogress=e=>{if(e.lengthComputable)$('p').value=e.loaded*100/e.total};
  x.onload=()=>{$('msg').textContent=x.status+': '+x.responseText;if(x.status===200)waitReboot()};
  x.onerror=()=>{$('msg').textContent='Übertragung abgebrochen.'};
  $('msg').textContent='Lade hoch…';
  x.send(file);
};
$('rs').onclick=async()=>{
  const h={};if($('pw').value)h['X-OTA-Password']=$('pw').value;
  const r=await fetch('/restart',{method:'POST',headers:h});
  $('msg').textContent=r.status+': '+await r.text();if(r.ok)waitReboot();
};
document.querySelectorAll('.wm').forEach(b=>b.onclick=async()=>{
  const h={};if($('pw').value)h['X-OTA-Password']=$('pw').value;
  const r=await fetch('/wifi/'+b.dataset.m,{method:'POST',headers:h});
  $('msg').textContent=r.status+': '+await r.text();status();
});
status();setInterval(status,3000);
</script></body></html>)";


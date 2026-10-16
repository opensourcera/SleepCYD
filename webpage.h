// The report page the device serves on the home network (night mode). Plain HTML + JS, reads /report.json and /site.
#pragma once
static const char WEB_PAGE[] PROGMEM = R"HTML(<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Sleep night</title>
<style>:root{--bg:#f5f6fa;--card:#fff;--ink:#1b1e28;--mute:#6b7186;--line:#e2e5ee;--acc:#0b79d0;--red:#d64045;--amb:#d98a00;--grn:#1f9d55}
@media(prefers-color-scheme:dark){:root{--bg:#12141b;--card:#1b1e28;--ink:#eceef5;--mute:#98a0b8;--line:#2c3040;--acc:#4aa8f0}}
body{margin:0;background:var(--bg);color:var(--ink);font:16px/1.45 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif}main{max-width:720px;margin:0 auto;padding:18px 16px 40px}
h1{font-size:22px;margin:0 0 2px}.sub{color:var(--mute);font-size:14px;margin-bottom:14px}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(140px,1fr));gap:10px;margin-bottom:12px}
.c{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:12px 14px}.c h3{margin:0 0 4px;font-size:13px;font-weight:600;color:var(--mute)}.big{font-size:28px;font-weight:700}.u{font-size:14px;color:var(--mute);margin-left:4px}
canvas{width:100%;height:120px;display:block}.card{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:12px 14px;margin-bottom:12px}.card h3{margin:0 0 8px;font-size:15px}
a.b{display:inline-block;background:var(--acc);color:#fff;text-decoration:none;padding:10px 16px;border-radius:10px;font-weight:600;margin:4px 8px 4px 0}.disc{color:var(--mute);font-size:13px}.leg{font-size:12px;color:var(--mute)}.leg i{display:inline-block;width:10px;height:10px;border-radius:2px;margin:0 4px 0 10px;vertical-align:middle}</style></head>
<body><main><h1 id="t">Sleep night</h1><div class="sub" id="s">Loading the report from the device...</div>
<div class="grid" id="g"></div>
<div class="card"><h3>SpO2 per minute, %</h3><canvas id="c1"></canvas></div>
<div class="card"><h3>Pulse per minute, bpm</h3><canvas id="c2"></canvas></div>
<div class="card"><h3>Body position</h3><canvas id="c3" style="height:40px"></canvas><div class="leg" id="l3"></div></div>
<div class="card" id="links"></div>
<p class="disc">Numbers come from the raw WatchPAT signals with the device's own estimates (pulse from the PAT wave, SpO2 from the two optical channels with a generic formula, oxygen drops of 3% for 10 s or more). Not a clinical result.</p></main>
<script>
const css=v=>getComputedStyle(document.documentElement).getPropertyValue(v).trim();const v=(x,d=0)=>x==null?'--':(+x).toFixed(d);
function card(h,b,u,n){return '<div class="c"><h3>'+h+'</h3><div class="big">'+b+(u?'<span class="u">'+u+'</span>':'')+'</div>'+(n?'<div class="leg">'+n+'</div>':'')+'</div>'}
function line(id,a,lo,hi,col,band){const c=document.getElementById(id),r=c.getBoundingClientRect();c.width=r.width*devicePixelRatio;c.height=r.height*devicePixelRatio;const x=c.getContext('2d');x.scale(devicePixelRatio,devicePixelRatio);const w=r.width,h=r.height,L=30,B=16;
x.font='11px system-ui';x.fillStyle=css('--mute');x.strokeStyle=css('--line');for(let k=0;k<=2;k++){const yv=lo+(hi-lo)*k/2,y=h-B-(h-B-6)*(yv-lo)/(hi-lo);x.beginPath();x.moveTo(L,y);x.lineTo(w,y);x.stroke();x.fillText(yv,2,y+4)}
if(band){const y=h-B-(h-B-6)*(band-lo)/(hi-lo);x.fillStyle='rgba(214,64,69,.12)';x.fillRect(L,y,w-L,h-B-y)}
x.strokeStyle=col;x.lineWidth=1.6;x.beginPath();let on=false;a.forEach((p,i)=>{if(!p){on=false;return}const X=L+(w-L)*i/Math.max(1,a.length-1),Y=h-B-(h-B-6)*(Math.min(hi,Math.max(lo,p))-lo)/(hi-lo);on?x.lineTo(X,Y):x.moveTo(X,Y);on=true});x.stroke();
x.fillStyle=css('--mute');const hrs=Math.floor(a.length/60);for(let hh=0;hh<=hrs;hh++){const X=L+(w-L)*hh*60/Math.max(1,a.length-1);x.fillText(hh+'h',X-6,h-3)}}
const PC=['#6b7186','#9aa0b4','#4aa8f0','#0b79d0','#1f9d55','#d98a00'];
function strip(a,names){const c=document.getElementById('c3'),r=c.getBoundingClientRect();c.width=r.width*devicePixelRatio;c.height=r.height*devicePixelRatio;const x=c.getContext('2d');x.scale(devicePixelRatio,devicePixelRatio);const w=r.width,h=r.height;x.fillStyle=css('--line');x.fillRect(0,0,w,h);
a.forEach((p,i)=>{if(!p)return;x.fillStyle=PC[p-1];x.fillRect(w*i/a.length,0,Math.max(1,w/a.length+0.5),h)});document.getElementById('l3').innerHTML=names.map((n,i)=>'<i style="background:'+PC[i]+'"></i>'+n).join('')}
fetch('/report.json').then(r=>r.json()).then(D=>{const m=D.per_minute||{};const hrs=(D.duration_s/3600).toFixed(1);
document.getElementById('t').textContent=(D.patient_name||'Sleep night')+(D.patient_dob?'  (born '+D.patient_dob+')':'');document.getElementById('s').textContent=hrs+' h recorded  -  '+D.file+'  -  '+(D.bytes/1048576).toFixed(1)+' MB';
const pp=D.position_pct||{};document.getElementById('g').innerHTML=card('Recorded',hrs,'h')+(D.pahi!=null?card('Apnea index (pAHI)',v(D.pahi,1),'/h','<b style="color:'+(D.severity=='normal'?'var(--grn)':D.severity=='mild'?'var(--amb)':'var(--red)')+'">'+D.severity+'</b>  pRDI '+v(D.prdi,1)+'  (apneas '+D.apneas+', hypopneas '+D.hypopneas+', RERA '+D.reras+')'):'')+card('SpO2 average',v(D.spo2&&D.spo2.mean,1),'%','lowest '+v(D.spo2&&D.spo2.min)+' %')+card('Pulse average',v(D.pulse&&D.pulse.mean),'bpm',v(D.pulse&&D.pulse.min)+' to '+v(D.pulse&&D.pulse.max))
+card('Oxygen drops',D.desaturations,'','ODI '+v(D.odi3,1)+' per hour')+card('On the back',v(pp.supine),'%','left '+v(pp.left)+'  right '+v(pp.right)+'  front '+v(pp.prone))+(D.breathing>0?card('Breathing',v(D.breathing),'/min','last reading'):'');
line('c1',m.spo2||[],70,100,css('--red'),90);line('c2',m.pulse||[],40,140,css('--acc'));strip(m.position||[],D.position_codes||[]);
let L='<h3>Files</h3><a class="b" href="/data.dat">Download the recording ('+(D.bytes/1048576).toFixed(1)+' MB)</a><a class="b" href="/report.json">report.json</a>';
fetch('/site').then(r=>r.text()).then(u=>{if(u&&u.startsWith('http'))L+='<a class="b" style="background:var(--grn)" href="'+u+'">Open the full report on the site</a>';document.getElementById('links').innerHTML=L}).catch(()=>{document.getElementById('links').innerHTML=L})
}).catch(e=>{document.getElementById('s').textContent='Could not load /report.json: '+e})
</script></body></html>)HTML";

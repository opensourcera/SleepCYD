"""Tabbed night report from a WatchPAT recording (.dat), with apnea parameters and an optional upload to a website API.

usage:
  python SleepCYD/sleep_report.py E:\\SLEEP\\watchpat_s0019.dat                      -> watchpat_s0019_report.html + .json next to the file
  python SleepCYD/sleep_report.py night.dat --start "2026-10-15 22:40" --name "Name"   real clock times on the charts
  python SleepCYD/sleep_report.py night.dat --post https://example.com/api/sleep --token SECRET [--with-series]

The numbers come from the analysis code of the WatchPAT ONE BLE client project (watchpat_analysis.py, folder given by the
WATCHPAT_DIR environment variable), replayed packet by packet, so they match its dashboard. They are estimates computed from the raw signals, not a clinical score.
The HTML file is self-contained (no network needed), works on a phone, and follows light/dark mode.
"""
import argparse, json, math, os, sys, time, urllib.request, urllib.error
from collections import Counter

WATCHPAT_DIR = os.environ.get('WATCHPAT_DIR', r'C:\code\sleep\watchpat')
sys.path.insert(0, WATCHPAT_DIR)
import numpy as np
import watchpat_analysis as wa
from watchpat_ble import parse_data_packet, read_dat_file


def normalize(raw):
    return raw[24:] if len(raw) >= 24 and raw[:2] == b'\xBB\xBB' else raw


def analyse(path):
    buf = wa.SensorBuffers(full_session_history=True)
    pos_sec, metric = [], []
    n = 0
    for idx, raw in enumerate(read_dat_file(path)):
        pkt = parse_data_packet(normalize(raw), idx)
        if pkt.motion is not None:
            pos_sec.append(wa.POSITION_LABELS.get(pkt.motion.body_position, pkt.motion.body_position))
        else:
            pos_sec.append(pos_sec[-1] if pos_sec else '?')
        metric.append(pkt.metric.value if pkt.metric is not None else (metric[-1] if metric else 0))
        buf.feed(pkt, now=float(idx + 1))
        n = idx + 1
    if n:
        with buf.lock:
            buf._last_derive_time = 0.0
            buf._update_derived(now=float(n + 1), record_history=False)
    return buf, pos_sec, metric, n


def per_minute(times, values, minutes, agg=np.nanmean):
    out = []
    t = np.asarray(times, dtype=float); v = np.asarray(values, dtype=float)
    for m in range(minutes):
        sel = v[(t >= m * 60) & (t < (m + 1) * 60)]
        sel = sel[~np.isnan(sel)] if sel.size else sel
        out.append(None if not sel.size else round(float(agg(sel)), 1))
    return out


def build(path, start=None, name=''):
    buf, pos_sec, metric, n = analyse(path)
    if n < 30:
        sys.exit(f'{path}: only {n} packets, too short for a report')
    secs = n
    minutes = max(1, math.ceil(secs / 60))
    hours = secs / 3600.0
    snap = buf.snapshot()

    spo2_t, spo2_v = snap['spo2_full_times'], snap['spo2_full_history']
    hr_t, hr_v = snap['hr_full_times'], snap['hr_full_history']
    spo2_valid = spo2_v[~np.isnan(spo2_v)] if len(spo2_v) else np.array([])
    hr_valid = hr_v[~np.isnan(hr_v)] if len(hr_v) else np.array([])
    pat_counts = Counter(ev[4] for ev in snap['pat_events'])
    n_apnea, n_hyp, n_rera = pat_counts.get(wa.EVT_APNEA, 0), pat_counts.get(wa.EVT_HYPOPNEA, 0), pat_counts.get(wa.EVT_RERA, 0)
    desats = snap['apnea_events']                       # (start s, nadir) of oxygen drops >= 3 % for >= 10 s
    central = snap['central_events']
    odi = len(desats) / hours if hours else 0

    t90 = float((spo2_valid < 90).sum()) if spo2_valid.size else 0.0   # seconds (one sample per second)
    t88 = float((spo2_valid < 88).sum()) if spo2_valid.size else 0.0
    stage_pct = snap['sleep_stage_percentages']
    stage_labels = snap['sleep_stage_labels']

    pos_counts = Counter(pos_sec)
    pos_pct = {k: round(100.0 * v / len(pos_sec), 1) for k, v in pos_counts.items()} if pos_sec else {}

    # events with the position the patient was in
    def pos_at(t):
        i = min(int(t), len(pos_sec) - 1)
        return pos_sec[i] if pos_sec else '?'
    events = []
    for ev in snap['pat_events']:
        events.append({'t': round(float(ev[0]), 1), 'type': ev[4], 'pat_ratio': round(float(ev[1]), 2), 'hr_rise': round(float(ev[2]), 1),
                       'spo2_drop': round(float(ev[3]), 1), 'pos': pos_at(ev[0])})
    for t, nadir in desats:
        events.append({'t': round(float(t), 1), 'type': 'DESAT', 'nadir': round(float(nadir), 1), 'pos': pos_at(t)})
    for t, drop in central:
        events.append({'t': round(float(t), 1), 'type': 'CENTRAL', 'drop': round(float(drop), 1), 'pos': pos_at(t)})
    events.sort(key=lambda e: e['t'])

    # events per hour of recording
    per_hour = []
    for h in range(max(1, math.ceil(hours))):
        lo, hi = h * 3600, (h + 1) * 3600
        span = max(1.0, min(hi, secs) - lo) / 3600.0
        c = Counter(e['type'] for e in events if lo <= e['t'] < hi)
        per_hour.append({'hour': h + 1, 'apnea': c['APNEA'], 'hypopnea': c['HYPOPNEA'], 'rera': c['RERA'], 'desat': c['DESAT'], 'central': c['CENTRAL'],
                         'index': round((c['APNEA'] + c['HYPOPNEA']) / span, 1), 'span_h': round(span, 2)})

    # AHI by position (events per hour spent in that position)
    pos_hours = {k: v / 3600.0 for k, v in pos_counts.items()}
    by_pos = {}
    for k, hrs in pos_hours.items():
        c = sum(1 for e in events if e['type'] in ('APNEA', 'HYPOPNEA') and e['pos'] == k)
        by_pos[k] = {'hours': round(hrs, 2), 'events': c, 'index': round(c / hrs, 1) if hrs >= 0.05 else None}

    pahi = float(buf.pahi_estimate) if buf.pahi_estimate >= 0 else (n_apnea + n_hyp) / hours
    severity = 'normal' if pahi < 5 else 'mild' if pahi < 15 else 'moderate' if pahi < 30 else 'severe'

    mot_t, mot_v = snap['motion_level_times'], snap['motion_level_history']
    series = {
        'minutes': minutes,
        'spo2': per_minute(spo2_t, spo2_v, minutes),
        'spo2_min': per_minute(spo2_t, spo2_v, minutes, np.nanmin),
        'hr': per_minute(hr_t, hr_v, minutes),
        'motion': per_minute(mot_t, mot_v, minutes) if len(mot_t) else [],
        'resp': per_minute(np.arange(len(snap['resp_rate_history'])), np.where(snap['resp_rate_history'] > 0, snap['resp_rate_history'], np.nan), minutes) if len(snap['resp_rate_history']) else [],
        'stage': [],
        'position': [],
    }
    # stage and position per minute (most frequent label in the minute)
    st_t, st_l = snap['sleep_stage_times'], stage_labels
    for m in range(minutes):
        sel = [st_l[i] for i in range(min(len(st_l), len(st_t))) if m * 60 <= st_t[i] < (m + 1) * 60]
        series['stage'].append(Counter(sel).most_common(1)[0][0] if sel else None)
        ps = pos_sec[m * 60:(m + 1) * 60]
        series['position'].append(Counter(ps).most_common(1)[0][0] if ps else None)

    summary = {
        'name': name, 'source': os.path.basename(path), 'start': start, 'generated': time.strftime('%Y-%m-%d %H:%M:%S'),
        'duration_s': secs, 'duration_h': round(hours, 2), 'packets': n,
        'spo2': {'mean': round(float(spo2_valid.mean()), 1) if spo2_valid.size else None, 'min': round(float(spo2_valid.min()), 1) if spo2_valid.size else None,
                 'max': round(float(spo2_valid.max()), 1) if spo2_valid.size else None, 't_below_90_min': round(t90 / 60, 1), 't_below_88_min': round(t88 / 60, 1),
                 'pct_below_90': round(100 * t90 / spo2_valid.size, 1) if spo2_valid.size else None, 'valid_pct': round(100.0 * spo2_valid.size / max(1, len(spo2_v)), 1)},
        'pulse': {'mean': round(float(hr_valid.mean()), 1) if hr_valid.size else None, 'min': round(float(hr_valid.min()), 1) if hr_valid.size else None,
                  'max': round(float(hr_valid.max()), 1) if hr_valid.size else None},
        'apnea': {'ahi_odi_based': round(float(buf.ahi_estimate), 1) if buf.ahi_estimate >= 0 else round(odi, 1), 'pahi': round(pahi, 1),
                  'prdi': round(float(buf.rdi_estimate), 1) if buf.rdi_estimate >= 0 else round((n_apnea + n_hyp + n_rera) / hours, 1),
                  'odi3': round(odi, 1), 'apneas': n_apnea, 'hypopneas': n_hyp, 'reras': n_rera, 'desaturations': len(desats), 'central': len(central),
                  'severity': severity, 'by_position': by_pos, 'per_hour': per_hour},
        'stages_pct': {k: round(v, 1) for k, v in stage_pct.items()},
        'position_pct': pos_pct,
        'metric_mean': round(float(np.mean(metric)), 1) if metric else None,
    }
    return {'summary': summary, 'series': series, 'events': events}


PAGE = r'''<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Sleep report</title>
<style>
:root{--bg:#f5f6fa;--card:#fff;--ink:#1b1e28;--mute:#6b7186;--line:#e2e5ee;--acc:#0b79d0;--ok:#1f9d55;--warn:#d98a00;--bad:#d64045;--s1:#0b79d0;--s2:#8e5bd6;--s3:#e07a2f}
@media (prefers-color-scheme:dark){:root{--bg:#12141b;--card:#1b1e28;--ink:#eceef5;--mute:#98a0b8;--line:#2c3040;--acc:#4aa8f0;--ok:#43c47a;--warn:#f0b13a;--bad:#f06b70;--s1:#4aa8f0;--s2:#b48cf0;--s3:#f0a060}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--ink);font:16px/1.45 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif}
header{padding:18px 16px 6px;max-width:980px;margin:0 auto}h1{margin:0;font-size:22px}.sub{color:var(--mute);font-size:14px}
nav{position:sticky;top:0;background:var(--bg);z-index:5;border-bottom:1px solid var(--line)}nav div{display:flex;gap:4px;max-width:980px;margin:0 auto;padding:6px 12px;overflow-x:auto}
nav button{border:0;background:transparent;color:var(--mute);font:inherit;font-weight:600;padding:10px 14px;border-radius:10px;cursor:pointer;white-space:nowrap}
nav button[aria-selected=true]{background:var(--card);color:var(--acc);box-shadow:0 1px 0 var(--line)}
main{max-width:980px;margin:0 auto;padding:14px 16px 40px}section{display:none}section.on{display:block}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:10px;margin-bottom:14px}
.card{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:14px 14px 12px}.card h3{margin:0 0 8px;font-size:13px;font-weight:600;color:var(--mute);text-transform:uppercase;letter-spacing:.04em}
.big{font-size:30px;font-weight:700;line-height:1.1}.unit{font-size:14px;font-weight:500;color:var(--mute);margin-left:3px}.note{font-size:13px;color:var(--mute);margin-top:4px}
.pill{display:inline-block;padding:2px 10px;border-radius:99px;font-size:13px;font-weight:700;color:#fff}.normal{background:var(--ok)}.mild{background:var(--warn)}.moderate{background:#e0662f}.severe{background:var(--bad)}
canvas{width:100%;height:210px;display:block}table{width:100%;border-collapse:collapse;font-size:14px}th,td{padding:8px 6px;border-bottom:1px solid var(--line);text-align:left}th{color:var(--mute);font-weight:600;font-size:12px;text-transform:uppercase}
td.n,th.n{text-align:right;font-variant-numeric:tabular-nums}.legend{display:flex;flex-wrap:wrap;gap:12px;font-size:13px;color:var(--mute);margin-top:8px}.legend i{display:inline-block;width:11px;height:11px;border-radius:3px;margin-right:5px;vertical-align:-1px}
.disc{font-size:12.5px;color:var(--mute);margin-top:18px;border-top:1px solid var(--line);padding-top:12px}.scroll{overflow-x:auto;max-height:360px;overflow-y:auto}
</style></head><body>
<header><h1 id="title">Sleep report</h1><div class="sub" id="sub"></div></header>
<nav><div role="tablist" id="tabs"></div></nav><main id="main"></main>
<script>
const D=__DATA__;const S=D.summary,SR=D.series,EV=D.events,A=S.apnea;
const css=n=>getComputedStyle(document.documentElement).getPropertyValue(n).trim();
const $=(h)=>{const t=document.createElement('template');t.innerHTML=h.trim();return t.content.firstChild};
const fmtT=m=>{if(!S.start)return Math.floor(m/60)+'h '+String(Math.round(m%60)).padStart(2,'0')+'m';const d=new Date(S.start.replace(' ','T'));d.setMinutes(d.getMinutes()+m);return d.toLocaleTimeString([],{hour:'2-digit',minute:'2-digit'})};
const v=(x,d=1,u='')=>x==null?'--':(typeof x==='number'?x.toFixed(d):x)+u;
document.getElementById('title').textContent='Sleep report'+(S.name?' - '+S.name:'');
document.getElementById('sub').textContent=(S.start?S.start+' - ':'')+S.duration_h+' h recorded - '+S.source;
const tabs=[['Summary','sum'],['Oxygen','oxy'],['Pulse & breathing','pul'],['Apnea','apn'],['Sleep & position','slp'],['Events','evt']];
const tabsEl=document.getElementById('tabs'),main=document.getElementById('main'),secs={};
tabs.forEach(([t,id],i)=>{const b=$('<button role="tab" aria-selected="'+(i==0)+'">'+t+'</button>');b.onclick=()=>show(id);tabsEl.append(b);const s=$('<section id="'+id+'"></section>');main.append(s);secs[id]=s});
function show(id){tabs.forEach(([t,k],i)=>{secs[k].classList.toggle('on',k==id);tabsEl.children[i].setAttribute('aria-selected',k==id)});draw[id]&&draw[id]()}
function card(h,body,cls=''){return '<div class="card '+cls+'"><h3>'+h+'</h3>'+body+'</div>'}
function stat(h,val,unit,note){return card(h,'<div class="big">'+val+'<span class="unit">'+(unit||'')+'</span></div>'+(note?'<div class="note">'+note+'</div>':''))}
const draw={};
// ---- chart helper: line series on a canvas, with optional threshold lines, bands and event markers
function chart(cv,series,o={}){const dpr=devicePixelRatio||1,w=cv.clientWidth,h=cv.clientHeight;cv.width=w*dpr;cv.height=h*dpr;const c=cv.getContext('2d');c.scale(dpr,dpr);c.clearRect(0,0,w,h);
const L=38,R=8,T=8,B=22,n=o.n||series[0].d.length;let lo=o.min,hi=o.max;
if(lo==null||hi==null){let a=[];series.forEach(s=>s.d.forEach(x=>{if(x!=null)a.push(x)}));lo=lo??Math.min(...a);hi=hi??Math.max(...a);if(lo==hi){lo-=1;hi+=1}}
const X=i=>L+(w-L-R)*i/Math.max(1,n-1),Y=y=>T+(h-T-B)*(1-(y-lo)/(hi-lo));
c.font='11px system-ui';c.fillStyle=css('--mute');c.strokeStyle=css('--line');c.lineWidth=1;
for(let k=0;k<=4;k++){const y=lo+(hi-lo)*k/4;c.beginPath();c.moveTo(L,Y(y));c.lineTo(w-R,Y(y));c.stroke();c.fillText(y.toFixed(o.dec??0),2,Y(y)+4)}
const ticks=Math.min(6,Math.max(2,Math.floor(w/90)));for(let k=0;k<=ticks;k++){const i=(n-1)*k/ticks;c.fillText(fmtT(i),X(i)-14,h-6)}
(o.bands||[]).forEach(b=>{c.fillStyle=b.c;c.globalAlpha=.12;c.fillRect(L,Y(b.to),w-L-R,Y(b.from)-Y(b.to));c.globalAlpha=1});
(o.lines||[]).forEach(l=>{c.strokeStyle=css(l.c);c.setLineDash([5,4]);c.beginPath();c.moveTo(L,Y(l.y));c.lineTo(w-R,Y(l.y));c.stroke();c.setLineDash([]);c.fillStyle=css(l.c);c.fillText(l.t,L+4,Y(l.y)-3)});
(o.marks||[]).forEach(m=>{c.strokeStyle=css(m.c);c.globalAlpha=.55;c.lineWidth=2;const x=X(m.t/60);c.beginPath();c.moveTo(x,T);c.lineTo(x,h-B);c.stroke();c.globalAlpha=1;c.lineWidth=1});
series.forEach(s=>{c.strokeStyle=css(s.c);c.lineWidth=s.w||1.8;c.beginPath();let pen=false;s.d.forEach((y,i)=>{if(y==null){pen=false;return}pen?c.lineTo(X(i),Y(y)):c.moveTo(X(i),Y(y));pen=true});c.stroke()})}
function legend(items){return '<div class="legend">'+items.map(([c,t])=>'<span><i style="background:var('+c+')"></i>'+t+'</span>').join('')+'</div>'}
// ---- Summary
secs.sum.innerHTML='<div class="grid">'+stat('Duration',S.duration_h,' h')+stat('SpO2 average',v(S.spo2.mean),' %','lowest '+v(S.spo2.min)+' %')+stat('Pulse average',v(S.pulse.mean,0),' bpm',v(S.pulse.min,0)+' to '+v(S.pulse.max,0))
 +card('Apnea index (pAHI)','<div class="big">'+v(A.pahi)+'<span class="unit">/ h</span></div><div class="note"><span class="pill '+A.severity+'">'+A.severity+'</span> estimate</div>')+'</div>'
 +'<div class="grid">'+stat('Oxygen drops (ODI 3%)',v(A.odi3),' / h',A.desaturations+' drops')+stat('Time below 90%',v(S.spo2.t_below_90_min),' min',v(S.spo2.pct_below_90)+' % of valid time')
 +stat('Sleep stages','Light '+v(S.stages_pct.Light,0)+'%','','Deep '+v(S.stages_pct.Deep,0)+'% - REM '+v(S.stages_pct.REM,0)+'% - Awake '+v(S.stages_pct.Awake,0)+'%')+'</div>'
 +'<div class="card"><h3>Night at a glance</h3><canvas id="c_glance"></canvas>'+legend([['--s1','SpO2 % (per minute)'],['--bad','below 90%']])+'</div>'
 +'<p class="disc">Estimates derived from the raw WatchPAT signals with open analysis code. They are not a clinical score and do not replace the manufacturer software or a physician\'s interpretation.</p>';
draw.sum=()=>{const cv=document.getElementById('c_glance');chart(cv,[{d:SR.spo2,c:'--s1'}],{min:80,max:100,bands:[{from:80,to:90,c:'#d64045'}]})};
// ---- Oxygen
secs.oxy.innerHTML='<div class="grid">'+stat('Average',v(S.spo2.mean),' %')+stat('Lowest',v(S.spo2.min),' %')+stat('Below 90%',v(S.spo2.t_below_90_min),' min')+stat('Below 88%',v(S.spo2.t_below_88_min),' min')+stat('Valid signal',v(S.spo2.valid_pct,0),' %','share of the night with a usable reading')+'</div>'
 +'<div class="card"><h3>SpO2 through the night (per minute)</h3><canvas id="c_oxy"></canvas>'+legend([['--s1','average'],['--s2','lowest in minute'],['--bad','oxygen drop']])+'</div>';
draw.oxy=()=>chart(document.getElementById('c_oxy'),[{d:SR.spo2_min,c:'--s2',w:1},{d:SR.spo2,c:'--s1'}],{min:80,max:100,bands:[{from:80,to:90,c:'#d64045'}],lines:[{y:90,c:'--bad',t:'90'}],marks:EV.filter(e=>e.type=='DESAT').map(e=>({t:e.t,c:'--bad'}))});
// ---- Pulse & breathing
secs.pul.innerHTML='<div class="grid">'+stat('Pulse average',v(S.pulse.mean,0),' bpm')+stat('Lowest',v(S.pulse.min,0),' bpm')+stat('Highest',v(S.pulse.max,0),' bpm')+'</div>'
 +'<div class="card"><h3>Pulse (per minute)</h3><canvas id="c_hr"></canvas></div><div class="card" style="margin-top:12px"><h3>Breathing rate (per minute, from the chest sensor)</h3><canvas id="c_rs"></canvas></div>';
draw.pul=()=>{chart(document.getElementById('c_hr'),[{d:SR.hr,c:'--s3'}],{});SR.resp.length?chart(document.getElementById('c_rs'),[{d:SR.resp,c:'--s2'}],{min:4,max:30}):0};
// ---- Apnea
const rows=A.per_hour.map(h=>'<tr><td>Hour '+h.hour+'</td><td class="n">'+h.apnea+'</td><td class="n">'+h.hypopnea+'</td><td class="n">'+h.rera+'</td><td class="n">'+h.desat+'</td><td class="n"><b>'+h.index+'</b></td></tr>').join('');
const pr=Object.entries(A.by_position).map(([k,p])=>'<tr><td>'+k+'</td><td class="n">'+p.hours+' h</td><td class="n">'+p.events+'</td><td class="n"><b>'+v(p.index)+'</b></td></tr>').join('');
secs.apn.innerHTML='<div class="grid">'+card('pAHI (apnea + hypopnea / h)','<div class="big">'+v(A.pahi)+'</div><div class="note"><span class="pill '+A.severity+'">'+A.severity+'</span></div>')+stat('pRDI (with RERA)',v(A.prdi),' / h')+stat('AHI (oxygen-drop based)',v(A.ahi_odi_based),' / h')+stat('ODI 3%',v(A.odi3),' / h')+'</div>'
 +'<div class="grid">'+stat('Apneas',A.apneas)+stat('Hypopneas',A.hypopneas)+stat('RERAs',A.reras)+stat('Central-type drops',A.central,'','oxygen drop with no breathing-effort change')+'</div>'
 +'<div class="card"><h3>Events per hour of the night</h3><canvas id="c_ph"></canvas>'+legend([['--bad','apnea'],['--warn','hypopnea'],['--s2','RERA']])+'</div>'
 +'<div class="card" style="margin-top:12px"><h3>By hour</h3><table><tr><th></th><th class="n">Apnea</th><th class="n">Hypopnea</th><th class="n">RERA</th><th class="n">O2 drops</th><th class="n">Index / h</th></tr>'+rows+'</table></div>'
 +'<div class="card" style="margin-top:12px"><h3>By body position</h3><table><tr><th></th><th class="n">Time</th><th class="n">Events</th><th class="n">Index / h</th></tr>'+pr+'</table></div>'
 +'<p class="disc">pAHI counts PAT-attenuation events with a matching oxygen drop (apnea 4% or more, hypopnea 3%); pRDI adds RERA events (arousal by pulse rise). Severity bands (under 5 normal, 5 to 15 mild, 15 to 30 moderate, 30 and above severe) are the usual adult AHI cut-offs applied to this estimate. Not a diagnosis.</p>';
draw.apn=()=>{const cv=document.getElementById('c_ph'),dpr=devicePixelRatio||1,w=cv.clientWidth,h=cv.clientHeight;cv.width=w*dpr;cv.height=h*dpr;const c=cv.getContext('2d');c.scale(dpr,dpr);c.clearRect(0,0,w,h);
 const hs=A.per_hour,mx=Math.max(3,...hs.map(x=>x.apnea+x.hypopnea+x.rera)),L=30,B=22,bw=(w-L-8)/hs.length;c.font='11px system-ui';c.fillStyle=css('--mute');
 hs.forEach((x,i)=>{let y=h-B;[['apnea','--bad'],['hypopnea','--warn'],['rera','--s2']].forEach(([k,col])=>{const ht=(h-B-8)*x[k]/mx;c.fillStyle=css(col);c.fillRect(L+i*bw+4,y-ht,bw-8,ht);y-=ht});c.fillStyle=css('--mute');c.fillText('h'+x.hour,L+i*bw+bw/2-8,h-6)});
 for(let k=0;k<=mx;k+=Math.ceil(mx/4)){const y=h-B-(h-B-8)*k/mx;c.fillText(k,4,y+4)}};
// ---- Sleep & position
const st=S.stages_pct,ps=S.position_pct;
secs.slp.innerHTML='<div class="grid">'+['Awake','Light','Deep','REM'].map(k=>stat(k,v(st[k],0),' %')).join('')+'</div>'
 +'<div class="card"><h3>Sleep stages (estimated)</h3><canvas id="c_st"></canvas></div>'
 +'<div class="card" style="margin-top:12px"><h3>Body position</h3><canvas id="c_ps"></canvas><div class="legend" id="pl"></div></div>'
 +'<div class="card" style="margin-top:12px"><h3>Movement (per minute)</h3><canvas id="c_mv"></canvas></div>';
draw.slp=()=>{const lv={Awake:3,REM:2,Light:1,Deep:0},cv=document.getElementById('c_st');chart(cv,[{d:SR.stage.map(s=>s==null?null:lv[s]),c:'--s1',w:2}],{min:-.2,max:3.2,dec:0});
 const c=cv.getContext('2d');const dpr=devicePixelRatio||1;c.save();c.setTransform(dpr,0,0,dpr,0,0);c.fillStyle=css('--mute');c.font='11px system-ui';cv.parentNode.querySelector('.legend')||0;c.restore();
 const cols={Supine:'--s1',Left:'--s2',Right:'--s3',Prone:'--bad',Upright:'--warn'};const pcv=document.getElementById('c_ps');const w=pcv.clientWidth,h=pcv.clientHeight,d2=devicePixelRatio||1;pcv.width=w*d2;pcv.height=h*d2;const x=pcv.getContext('2d');x.scale(d2,d2);
 const n=SR.position.length;SR.position.forEach((p,i)=>{if(!p)return;x.fillStyle=css(cols[p]||'--mute');x.fillRect(8+(w-16)*i/n,20,Math.max(1,(w-16)/n+.5),h-50)});x.fillStyle=css('--mute');x.font='11px system-ui';for(let k=0;k<=5;k++){const i=n*k/5;x.fillText(fmtT(i),8+(w-16)*k/5-12,h-8)}
 document.getElementById('pl').innerHTML=Object.entries(ps).map(([k,pc])=>'<span><i style="background:var('+(cols[k]||'--mute')+')"></i>'+k+' '+pc+'%</span>').join('');
 chart(document.getElementById('c_mv'),[{d:SR.motion,c:'--s3'}],{min:0})};
// ---- Events
secs.evt.innerHTML='<div class="card"><h3>'+EV.length+' events</h3><div class="scroll"><table><tr><th>Time</th><th>Type</th><th>Position</th><th class="n">O2 drop</th><th class="n">Pulse rise</th></tr>'+EV.map(e=>'<tr><td>'+fmtT(e.t/60)+'</td><td>'+e.type+'</td><td>'+e.pos+'</td><td class="n">'+(e.spo2_drop!=null?e.spo2_drop+' %':e.nadir!=null?'to '+e.nadir+' %':e.drop!=null?e.drop+' %':'')+'</td><td class="n">'+(e.hr_rise!=null?e.hr_rise+' bpm':'')+'</td></tr>').join('')+'</table></div></div>';
addEventListener('resize',()=>{const on=tabs.find(([t,k])=>secs[k].classList.contains('on'));on&&draw[on[1]]&&draw[on[1]]()});
show('sum');
</script></body></html>'''


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('dat'); ap.add_argument('-o', '--out'); ap.add_argument('--start', help='recording start, "YYYY-MM-DD HH:MM", for real clock times')
    ap.add_argument('--name', default=''); ap.add_argument('--post', help='API URL to POST the JSON summary to'); ap.add_argument('--token', help='bearer token for --post')
    ap.add_argument('--with-series', action='store_true', help='include per-minute series and the event list in the POST body')
    a = ap.parse_args()
    data = build(a.dat, a.start, a.name)
    base = a.out or os.path.splitext(a.dat)[0] + '_report'
    with open(base + '.json', 'w', encoding='utf-8') as f: json.dump(data, f, indent=1)
    with open(base + '.html', 'w', encoding='utf-8') as f: f.write(PAGE.replace('__DATA__', json.dumps(data)))
    s = data['summary']; ap_ = s['apnea']
    print(f"{base}.html  ({s['duration_h']} h, pAHI {ap_['pahi']} {ap_['severity']}, ODI {ap_['odi3']}, SpO2 mean {s['spo2']['mean']} min {s['spo2']['min']})")
    if a.post:
        body = json.dumps(data if a.with_series else {'summary': data['summary']}).encode()
        req = urllib.request.Request(a.post, data=body, headers={'Content-Type': 'application/json', **({'Authorization': 'Bearer ' + a.token} if a.token else {})})
        try:
            with urllib.request.urlopen(req, timeout=30) as r: print('POST', a.post, '->', r.status, r.read(300).decode('utf-8', 'replace'))
        except urllib.error.HTTPError as e: print('POST failed:', e.code, e.read(300).decode('utf-8', 'replace')); sys.exit(1)
        except Exception as e: print('POST failed:', e); sys.exit(1)


if __name__ == '__main__':
    main()

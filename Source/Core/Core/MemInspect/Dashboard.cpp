// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The dashboard, as a string.
//
// It is split into several literals and joined at first use rather than written
// as one. MSVC refuses a single string literal over 16380 bytes, and adjacent
// concatenation only moves the limit rather than removing it - joining at
// runtime has neither problem and costs one allocation for the life of the
// process.
//
// Everything the page needs is in here: no CDN, no external stylesheet, no
// fonts. A dashboard that cannot render without the internet would be useless
// on a machine whose whole point is a private LAN.

#include "Core/MemInspect/Dashboard.h"

#include <string>

namespace MemInspect::Web
{
namespace
{
constexpr std::string_view STYLE = R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>MKWii Memory Inspector</title>
<style>
  :root{
    --bg:#0e1116; --panel:#161b22; --panel2:#1c2230; --line:#2a3242;
    --tx:#e6edf3; --dim:#8b949e; --accent:#58a6ff;
    --ok:#3fb950; --warn:#d29922; --bad:#f85149; --flash:#f0b72f;
  }
  *{box-sizing:border-box}
  body{margin:0;background:var(--bg);color:var(--tx);
       font:14px/1.5 ui-sans-serif,system-ui,"Segoe UI",sans-serif}
  header{display:flex;align-items:center;gap:16px;flex-wrap:wrap;
         padding:12px 20px;background:var(--panel);border-bottom:1px solid var(--line)}
  h1{font-size:15px;margin:0;font-weight:650;letter-spacing:.2px}
  .sub{color:var(--dim);font-size:12px}
  .pill{padding:3px 10px;border-radius:999px;font-size:12px;font-weight:600;
        border:1px solid var(--line);background:var(--panel2)}
  .pill.ok{color:var(--ok);border-color:#1f4b2c}
  .pill.warn{color:var(--warn);border-color:#5c4a12}
  .pill.bad{color:var(--bad);border-color:#5c2124}
  main{padding:20px;max-width:1200px;margin:0 auto}

  .banner{padding:22px;border-radius:12px;text-align:center;margin-bottom:22px;
          border:1px solid var(--line);background:var(--panel);transition:.15s}
  .banner .big{font-size:34px;font-weight:750;letter-spacing:1px}
  .banner.race{background:linear-gradient(180deg,#0f2e18,#0d1f13);border-color:#1f6f36}
  .banner.race .big{color:#4ade80}
  .banner.menu{background:linear-gradient(180deg,#2a1f08,#1d1708);border-color:#7a5a12}
  .banner.menu .big{color:#f0b72f}
  .banner.off .big{color:var(--dim)}

  h2{font-size:12px;text-transform:uppercase;letter-spacing:.8px;color:var(--dim);
     margin:26px 0 10px;font-weight:650}
  .grid{display:grid;gap:12px;grid-template-columns:repeat(auto-fill,minmax(255px,1fr))}
  .card{background:var(--panel);border:1px solid var(--line);border-radius:10px;
        padding:12px 14px;position:relative;transition:border-color .12s,background .12s}
  .card.flash{border-color:var(--flash);background:#1e1c10}
  .card .lab{font-size:12px;color:var(--dim);display:flex;justify-content:space-between;gap:8px}
  .card .val{font-size:24px;font-weight:700;margin:5px 0 2px;
             font-family:ui-monospace,"Cascadia Code",Consolas,monospace;word-break:break-all}
  .card .val.null{color:var(--bad);font-size:16px;font-weight:600}
  .card .meta{font-size:11px;color:var(--dim);
              font-family:ui-monospace,Consolas,monospace}
  .card .note{font-size:11px;color:#6e7681;margin-top:6px;line-height:1.4}
  .card .x{position:absolute;top:8px;right:9px;cursor:pointer;color:#5a626d;
           border:0;background:none;font-size:15px;padding:0 3px}
  .card .x:hover{color:var(--bad)}
  svg.spark{width:100%;height:26px;margin-top:6px;display:block}

  form.add,.panel{display:flex;gap:8px;flex-wrap:wrap;align-items:flex-end;
           background:var(--panel);border:1px solid var(--line);
           border-radius:10px;padding:14px;margin-top:10px}
  label{font-size:11px;color:var(--dim);display:block;margin-bottom:3px}
  input,select{background:var(--panel2);color:var(--tx);border:1px solid var(--line);
               border-radius:7px;padding:7px 9px;font:13px ui-monospace,Consolas,monospace}
  input:focus,select:focus{outline:1px solid var(--accent);border-color:var(--accent)}
  button.go{background:var(--accent);color:#06121f;border:0;border-radius:7px;
            padding:8px 16px;font-weight:650;cursor:pointer;font-size:13px}
  button.go:hover{filter:brightness(1.1)}
  button.go.warn{background:var(--warn)}
  .err{color:var(--bad);font-size:12px;margin-top:8px}
  .ok{color:var(--ok);font-size:12px;margin-top:8px}

  .maprow{display:flex;gap:16px;align-items:flex-start;margin-bottom:22px;
          flex-wrap:wrap}
  .mapbox{position:relative;flex:1 1 560px;min-width:320px;background:var(--panel);
          border:1px solid var(--line);border-radius:12px;overflow:hidden}
  .mapbox canvas{display:block;width:100%;height:auto}
  .maphead{display:flex;align-items:center;gap:10px;padding:9px 14px;
           border-bottom:1px solid var(--line);background:var(--panel2)}
  .maphead h2{margin:0;font-size:13px;font-weight:650}
  .btn{background:var(--panel);color:var(--tx);border:1px solid var(--line);
       border-radius:6px;padding:4px 10px;font-size:12px;cursor:pointer}
  .btn:hover{border-color:var(--accent)}
  .btn.on{border-color:var(--accent);color:var(--accent)}
  .standings{flex:0 0 240px;background:var(--panel);border:1px solid var(--line);
             border-radius:12px;overflow:hidden}
  .standings .maphead{border-bottom:1px solid var(--line)}
  .standings ol{margin:0;padding:6px 0;list-style:none}
  .standings li{display:flex;align-items:center;gap:8px;padding:4px 12px;
                font-size:12.5px;flex-wrap:wrap}
  .standings .dot{width:10px;height:10px;border-radius:50%;flex:0 0 auto}
  .standings .who{flex:1;color:var(--dim)}
  .standings .me{color:var(--tx);font-weight:650}
  .standings .kmh{font-variant-numeric:tabular-nums;color:var(--dim)}
  .standings .item{flex:0 0 100%;padding-left:18px;font-size:11px;
                   color:var(--dim);margin-top:-2px}
  .badges{display:flex;gap:3px;flex:0 0 auto}
  .badge{font-size:9px;font-weight:700;letter-spacing:.3px;padding:1px 4px;
         border-radius:3px;color:#06121f;text-transform:uppercase}
  .legend{display:flex;flex-wrap:wrap;gap:6px;padding:6px 12px;
          border-top:1px solid var(--line)}
  code{background:var(--panel2);padding:1px 5px;border-radius:4px;font-size:12px}
</style>
</head>
)HTML";

constexpr std::string_view BODY = R"HTML(<body>
<header>
  <h1>MKWii Memory Inspector</h1>
  <span class="sub" id="gameNote">&nbsp;</span>
  <span style="flex:1"></span>
  <span class="pill" id="rate">-- Hz</span>
  <span class="pill" id="hook">connecting</span>
</header>

<main>
  <div class="banner off" id="banner">
    <div class="big" id="bannerText">WAITING FOR A GAME</div>
    <div class="sub" id="bannerSub">boot Mario Kart and this fills in</div>
  </div>

  <div class="maprow">
    <div class="mapbox">
      <div class="maphead">
        <h2>Track map</h2>
        <span class="sub" id="mapInfo">no race</span>
        <span style="flex:1"></span>
        <button class="btn on" id="btnTrack" type="button">Track outline</button>
        <button class="btn on" id="btnTrail" type="button">Trails</button>
        <button class="btn" id="btnReset" type="button">Reset</button>
      </div>
      <canvas id="map" width="1120" height="760"></canvas>
    </div>
    <div class="standings">
      <div class="maphead"><h2>Standings</h2></div>
      <ol id="stand"></ol>
      <div class="legend" id="legend"></div>
    </div>
  </div>

  <h2>Inject</h2>
  <div class="panel">
    <div><label>Racer</label><select id="iRacer"></select></div>
    <div><label>Item</label><select id="iItem"></select></div>
    <div><label>Count</label><input id="iCount" size="3" placeholder="auto"></div>
    <button class="go" id="btnGive" type="button">Give item</button>
    <span style="width:18px"></span>
    <div><label>State</label><select id="iState"></select></div>
    <div><label>Frames (60/s)</label><input id="iFrames" size="5" placeholder="default"></div>
    <button class="go" id="btnState" type="button">Set state</button>
    <button class="go warn" id="btnClear" type="button">Clear states</button>
    <div style="flex-basis:100%" class="sub">
      The item slot is read when the item button is pressed, so one write is
      enough. Writing a state only genuinely works for <code>star</code>:
      the rest set the flag and the timer but never change the kart, because the
      game does the visible part in its activation routine.
    </div>
  </div>
  <div class="err" id="writeErr"></div>

  <div class="maphead" style="border:1px solid var(--line);border-radius:8px;
       margin:22px 0 10px">
    <h2>Raw values</h2>
    <span style="flex:1"></span>
    <button class="btn" id="btnRacers" type="button">Show per-racer numbers</button>
  </div>

  <div id="groups"></div>

  <h2>Add a live watch</h2>
  <form class="add" id="addForm">
    <div><label>Address (hex)</label><input id="fAddr" placeholder="80444804" size="12" required></div>
    <div><label>Type</label><select id="fKind"></select></div>
    <div><label>Label (optional)</label><input id="fLabel" placeholder="My value" size="18"></div>
    <button class="go" type="submit">Watch</button>
    <div style="flex-basis:100%" class="sub">
      Kept across runs in <code>User\Config\MemInspectWatches.txt</code>.
      For a permanent entry add a <code>Watch</code> to
      <code>Source/Core/Core/MemInspect/Registry.cpp</code>.
    </div>
  </form>
  <div class="err" id="err"></div>
</main>
)HTML";

constexpr std::string_view SCRIPT_CARDS = R"HTML(<script>
let META = null;
const HIST = {};          // key -> recent raw values
const LAST = {};          // key -> last value (change detection)
const FLASH = {};         // key -> timestamp of last change
const HMAX = 90;

function esc(s){
  return String(s).replace(/[&<>"']/g, c =>
    ({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;","'":"&#39;"}[c]));
}

function fmtVal(w, raw){
  if (raw === null || raw === undefined) return {t:"unreadable", cls:"null"};
  if (w.enum && w.enum[String(raw)]) return {t:w.enum[String(raw)], cls:""};
  if (w.kind === "f32" || w.kind === "f64") return {t:Number(raw).toFixed(4), cls:""};
  if (w.fmt === "hex" || w.kind === "ptr")
    return {t:"0x"+((raw>>>0).toString(16).toUpperCase().padStart(8,"0")), cls:""};
  return {t:String(raw), cls:""};
}

function spark(key){
  const h = HIST[key] || [];
  const nums = h.filter(v => typeof v === "number");
  if (nums.length < 3) return "";
  const mn = Math.min(...nums), mx = Math.max(...nums);
  if (mx === mn) return "";
  const pts = nums.map((v,i) =>
    `${(i/(nums.length-1)*100).toFixed(2)},${(22-((v-mn)/(mx-mn))*20).toFixed(2)}`
  ).join(" ");
  return `<svg class="spark" viewBox="0 0 100 26" preserveAspectRatio="none">
    <polyline points="${pts}" fill="none" stroke="#58a6ff" stroke-width="1.2"
      vector-effect="non-scaling-stroke"/></svg>`;
}

function build(){
  const groups = {};
  META.watches.forEach(w => (groups[w.group] = groups[w.group] || []).push(w));
  const order = Object.keys(groups).sort((a,b) =>
    (a==="Scene"?0:a==="Live Watches"?2:1) - (b==="Scene"?0:b==="Live Watches"?2:1)
    || a.localeCompare(b));
  // The 12 per-racer groups are 96 cards of raw floats - the map is the point,
  // so they start hidden behind a toggle.
  document.getElementById("groups").innerHTML = order.map(g => `
    <section class="${/^Racer \d+$/.test(g) ? "racergrp" : ""}">
    <h2>${esc(g)}</h2>
    <div class="grid">${groups[g].map(w => `
      <div class="card" id="c_${w.key}">
        ${w.user ? `<button class="x" data-k="${w.key}" title="remove">&times;</button>` : ""}
        <div class="lab"><span>${esc(w.label)}</span><span>${esc(w.kind)}</span></div>
        <div class="val" id="v_${w.key}">--</div>
        <div class="meta" id="m_${w.key}">0x${w.addr.toString(16).toUpperCase()}</div>
        <div id="s_${w.key}"></div>
        ${w.note ? `<div class="note">${esc(w.note)}</div>` : ""}
      </div>`).join("")}</div></section>`).join("");

  document.querySelectorAll(".x").forEach(b => b.onclick = async () => {
    await fetch("/api/watch/" + b.dataset.k, {method:"DELETE"});
    await loadMeta();
  });
  applyRacerVis();
}

let SHOW_RACERS = false;
function applyRacerVis(){
  document.querySelectorAll(".racergrp").forEach(
    e => e.style.display = SHOW_RACERS ? "" : "none");
  const b = document.getElementById("btnRacers");
  b.textContent = SHOW_RACERS ? "Hide per-racer numbers"
                              : "Show per-racer numbers";
  b.classList.toggle("on", SHOW_RACERS);
}

document.getElementById("btnRacers").onclick = () => {
  SHOW_RACERS = !SHOW_RACERS; applyRacerVis();
};
document.getElementById("btnReset").onclick = () => mapReset();
document.getElementById("btnTrack").onclick = e => {
  MAP.showTrack = !MAP.showTrack;
  e.target.classList.toggle("on", MAP.showTrack);
};
document.getElementById("btnTrail").onclick = e => {
  MAP.showTrail = !MAP.showTrail;
  e.target.classList.toggle("on", MAP.showTrail);
};

async function loadMeta(){
  META = await (await fetch("/api/meta")).json();
  document.getElementById("gameNote").textContent =
    META.game_id + " · " + META.game_note;
  const sel = document.getElementById("fKind");
  if (!sel.options.length)
    sel.innerHTML = META.kinds.map(k =>
      `<option ${k==="u32"?"selected":""}>${k}</option>`).join("");
  const racer = document.getElementById("iRacer");
  if (!racer.options.length){
    racer.innerHTML = Array.from({length:12}, (_,k) =>
      `<option value="${k}">Racer ${k}</option>`).join("");
    document.getElementById("iItem").innerHTML = META.items.map(i =>
      `<option value="${i.id}" ${i.id===9?"selected":""}>${esc(i.name)}` +
      `${i.risk ? " ⚠" : ""}</option>`).join("");
    document.getElementById("iState").innerHTML = META.states.map(s =>
      `<option>${esc(s)}</option>`).join("");
  }
  build();
}
)HTML";

constexpr std::string_view SCRIPT_MAP = R"HTML(
// ---------------------------------------------------------------------------
// Track map. Plots every racer on the XZ plane (Y is vertical in MKWii, so a
// top-down view drops it). Positions come from the Mtx34 translation column and
// the heading from its forward COLUMN - row 2 is the transpose and points
// sideways.
const MAXR = 12;
const TRAIL = 110;            // per-racer trail length, in samples
const CLOUD = 9000;           // accumulated points that draw the track outline
const COLORS = ["#58a6ff","#f85149","#3fb950","#d29922","#c678dd","#56b6c2",
                "#ff8f40","#e06c9f","#8bd450","#6272a4","#e5c07b","#a0a8b4"];
const MAP = {cloud:[], trails:[], view:null, showTrack:true, showTrail:true};

// Order matters: the first match decides the halo colour when several states
// are active at once.
const STATES = [
  {key:"star",    tag:"STAR",  col:"#ffd93d", halo:"#ffd93d", scale:1.35},
  {key:"mega",    tag:"MEGA",  col:"#ff8f40", halo:"#ff8f40", scale:1.8},
  {key:"bullet",  tag:"BILL",  col:"#e8eef7", halo:"#ffffff", scale:1.3},
  {key:"shocked", tag:"SHOCK", col:"#7aa2ff", halo:"#7aa2ff", scale:0.55},
  {key:"crushed", tag:"CRUSH", col:"#e06c9f", halo:"#e06c9f", scale:0.6},
  {key:"inked",   tag:"INK",   col:"#8b7bd8", halo:"#3a2f6b", scale:1.0},
  {key:"has_tc",  tag:"TC",    col:"#8bd450", halo:"#8bd450", scale:1.0},
];
for (let k=0;k<MAXR;k++) MAP.trails.push([]);

function mapReset(){
  MAP.cloud.length = 0;
  MAP.trails.forEach(t => t.length = 0);
  MAP.view = null;
}

function racers(s){
  const out = [];
  for (let k=0;k<MAXR;k++){
    const g = n => s.values[`r${k}_${n}`]?.raw;
    const x = g("pos_x"), z = g("pos_z"), y = g("pos_y");
    if (typeof x !== "number" || typeof z !== "number") continue;
    if (!isFinite(x) || !isFinite(z)) continue;
    // RaceConfig slot k and kart-array entry k are the same racer. Names,
    // states and items are all resolved server-side, because none of them can
    // be expressed as a single address.
    const info = (s.racers || []).find(r => r.index === k);
    const st = (s.states || []).find(r => r.index === k);
    const it = (s.items || []).find(r => r.index === k);
    out.push({k, x, z, y, fx: g("fwd_x"), fz: g("fwd_z"),
              place: g("place"),
              human: info ? info.local : (st ? st.local : g("controller") === 1),
              name: info ? info.name : null,
              source: info ? info.source : null,
              st, active: st ? st.active : [],
              item: it && !it.empty ? it : null});
  }
  return out;
}

// Fit the view to the accumulated cloud, not just the current frame, so the map
// does not lurch every time the pack spreads out or bunches up.
function fitView(pts, w, h){
  if (!pts.length) return MAP.view;
  let x0=Infinity,x1=-Infinity,z0=Infinity,z1=-Infinity;
  for (const p of pts){
    if (p[0]<x0)x0=p[0]; if (p[0]>x1)x1=p[0];
    if (p[1]<z0)z0=p[1]; if (p[1]>z1)z1=p[1];
  }
  const pad = 0.06*Math.max(x1-x0, z1-z0, 1);
  x0-=pad; x1+=pad; z0-=pad; z1+=pad;
  const sc = Math.min(w/Math.max(x1-x0,1), h/Math.max(z1-z0,1));
  const v = {sc, cx:(x0+x1)/2, cz:(z0+z1)/2, w, h};
  if (!MAP.view) return v;
  const a = 0.06, o = MAP.view;      // ease toward it so it never jitters
  return {sc:o.sc+(v.sc-o.sc)*a, cx:o.cx+(v.cx-o.cx)*a,
          cz:o.cz+(v.cz-o.cz)*a, w, h};
}

function drawMap(s){
  const cv = document.getElementById("map");
  const ctx = cv.getContext("2d");
  const W = cv.width, H = cv.height;
  ctx.clearRect(0,0,W,H);

  const rs = racers(s);
  const info = document.getElementById("mapInfo");
  if (!rs.length){
    info.textContent = "no race — the kart manager is null in menus";
    ctx.fillStyle = "#8b949e"; ctx.font = "16px system-ui";
    ctx.textAlign = "center";
    ctx.fillText("waiting for a race", W/2, H/2);
    ctx.textAlign = "start";
    document.getElementById("stand").innerHTML = "";
    document.getElementById("legend").innerHTML = "";
    return;
  }
  info.textContent = `${rs.length} racers`;

  for (const r of rs){
    MAP.cloud.push([r.x, r.z]);
    const t = MAP.trails[r.k];
    t.push([r.x, r.z]);
    if (t.length > TRAIL) t.shift();
  }
  while (MAP.cloud.length > CLOUD) MAP.cloud.shift();

  MAP.view = fitView(MAP.cloud, W, H);
  const v = MAP.view;
  const X = x => W/2 + (x - v.cx)*v.sc;
  const Z = z => H/2 + (z - v.cz)*v.sc;   // MKWii +Z points down the screen

  if (MAP.showTrack){
    ctx.fillStyle = "rgba(120,140,170,0.16)";
    for (const p of MAP.cloud) ctx.fillRect(X(p[0])-1, Z(p[1])-1, 2, 2);
  }

  if (MAP.showTrail){
    for (const r of rs){
      const t = MAP.trails[r.k];
      if (t.length < 2) continue;
      ctx.strokeStyle = COLORS[r.k % COLORS.length] + "66";
      ctx.lineWidth = r.human ? 2.5 : 1.5;
      ctx.beginPath();
      ctx.moveTo(X(t[0][0]), Z(t[0][1]));
      for (let i=1;i<t.length;i++) ctx.lineTo(X(t[i][0]), Z(t[i][1]));
      ctx.stroke();
    }
  }

  for (const r of rs){
    const px = X(r.x), pz = Z(r.z);
    const col = COLORS[r.k % COLORS.length];
    const base = r.human ? 9.5 : 7;
    // Mega grows the dot, shocked and crushed shrink it - so the map shows the
    // state at a glance without reading any label.
    const sdef = STATES.find(d => r.active.includes(d.key));
    const rad = base * (sdef ? sdef.scale : 1);

    if (typeof r.fx === "number" && typeof r.fz === "number"){
      const n = Math.hypot(r.fx, r.fz) || 1;
      ctx.strokeStyle = col; ctx.lineWidth = 2.5;
      ctx.beginPath(); ctx.moveTo(px, pz);
      ctx.lineTo(px + r.fx/n*rad*2.8, pz + r.fz/n*rad*2.8);
      ctx.stroke();
    }
    if (sdef){
      const pulse = 0.55 + 0.45*Math.sin(Date.now()/140);
      const g = ctx.createRadialGradient(px, pz, rad, px, pz, rad*2.4);
      g.addColorStop(0, sdef.halo + "cc");
      g.addColorStop(1, sdef.halo + "00");
      ctx.globalAlpha = 0.35 + 0.35*pulse;
      ctx.fillStyle = g;
      ctx.beginPath(); ctx.arc(px, pz, rad*2.4, 0, 6.2832); ctx.fill();
      ctx.globalAlpha = 1;
    }
    ctx.beginPath(); ctx.arc(px, pz, rad, 0, 6.2832);
    ctx.fillStyle = col; ctx.fill();
    if (sdef){
      ctx.strokeStyle = sdef.col; ctx.lineWidth = 3;
      ctx.beginPath(); ctx.arc(px, pz, rad + 2.5, 0, 6.2832); ctx.stroke();
    }
    if (r.human){ ctx.strokeStyle = "#fff"; ctx.lineWidth = 2.5;
      ctx.beginPath(); ctx.arc(px, pz, rad, 0, 6.2832); ctx.stroke(); }

    ctx.fillStyle = "#06121f";
    ctx.font = `700 ${r.human ? 12 : 10}px system-ui`;
    ctx.textAlign = "center"; ctx.textBaseline = "middle";
    ctx.fillText(String(r.place ?? r.k), px, pz);

    if (r.name){
      ctx.font = `${r.human ? 700 : 500} 12px system-ui`;
      ctx.textBaseline = "bottom";
      ctx.lineWidth = 3; ctx.strokeStyle = "rgba(0,0,0,0.75)";
      ctx.strokeText(r.name, px, pz - rad - 3);
      ctx.fillStyle = r.human ? "#fff" : col;
      ctx.fillText(r.name, px, pz - rad - 3);
    }
    ctx.textAlign = "start"; ctx.textBaseline = "alphabetic";
  }

  drawStandings(rs);
}
)HTML";

constexpr std::string_view SCRIPT_RENDER = R"HTML(
// Speed comes from differentiating position: there is no velocity field that
// reads correctly for every racer, because bikes and karts allocate differently
// and shift every fixed offset by 4 bytes.
function drawStandings(rs){
  const rows = rs.slice().sort((a, b) => (a.place ?? 99) - (b.place ?? 99));
  document.getElementById("stand").innerHTML = rows.map(r => {
    const t = MAP.trails[r.k];
    let kmh = "--";
    if (t.length >= 6){
      const a = t[t.length-6], b = t[t.length-1];
      // 5 samples at the ~20 Hz poll rate; 38 units/s per km/h, calibrated
      // against the HUD speedometer.
      const ups = Math.hypot(b[0]-a[0], b[1]-a[1]) / (5/20);
      kmh = (ups/38).toFixed(0);
    }
    const col = COLORS[r.k % COLORS.length];
    const who = r.name ? (r.human ? r.name + " (you)" : r.name)
                       : (r.human ? "YOU" : "CPU " + r.k);
    const tips = [];
    if (r.source) tips.push(`name via ${r.source}`);
    if (r.st){
      tips.push(`star ${r.st.star_timer} shock ${r.st.shock_timer} `
              + `crush ${r.st.crush_timer} mega ${r.st.mega_timer} (frames)`);
      // consistent=false means the two independent representations disagree,
      // which is a pointer-chain fault rather than an odd racer.
      if (!r.st.consistent) tips.push("BIT/TIMER MISMATCH - chain may have drifted");
    }
    const tip = tips.length ? ` title="${esc(tips.join(" | "))}"` : "";

    // Timers are in frames at 60fps; show seconds, which is what a human can
    // judge against what is on screen.
    const badges = r.active.map(key => {
      const d = STATES.find(x => x.key === key);
      if (!d) return "";
      const tk = {star:"star_timer", shocked:"shock_timer",
                  crushed:"crush_timer", mega:"mega_timer",
                  inked:"ink_timer"}[key];
      const f = tk && r.st ? r.st[tk] : 0;
      const secs = f > 0 ? ` ${(f/60).toFixed(1)}s` : "";
      return `<span class="badge" style="background:${d.col}">${d.tag}${secs}</span>`;
    }).join("");

    const item = r.item
      ? `<div class="item">${esc(r.item.name)}`
        + `${r.item.count > 1 ? " &times;" + r.item.count : ""}</div>`
      : "";

    return `<li${tip}><span class="dot" style="background:${col}"></span>`
         + `<span class="who ${r.human ? "me" : ""}">${r.place ?? "-"}. ${esc(who)}`
         + `</span><span class="badges">${badges}</span>`
         + `<span class="kmh">${kmh} km/h</span>${item}</li>`;
  }).join("");

  const seen = new Set(rs.flatMap(r => r.active));
  document.getElementById("legend").innerHTML = STATES
    .filter(d => seen.has(d.key))
    .map(d => `<span class="badge" style="background:${d.col}">${d.tag}</span>`)
    .join("");
}

function render(s){
  const hookEl = document.getElementById("hook");
  const wrongGame = s.hooked && s.game && META && s.game !== META.game_id;
  hookEl.textContent = s.hooked ? (s.game || "hooked") : "no game running";
  hookEl.className = "pill " + (!s.hooked ? "bad" : wrongGame ? "warn" : "ok");
  hookEl.title = wrongGame
    ? `every address here was derived against ${META.game_id}; this is ${s.game}`
    : "";
  document.getElementById("rate").textContent = (s.hz||0).toFixed(0) + " Hz";

  const b = document.getElementById("banner");
  const bt = document.getElementById("bannerText");
  const bs = document.getElementById("bannerSub");
  if (!s.hooked){
    b.className = "banner off"; bt.textContent = "WAITING FOR A GAME";
    bs.textContent = "boot Mario Kart and this fills in";
  } else if (s.derived.has_control === true){
    b.className = "banner race"; bt.textContent = "DRIVING";
    bs.textContent = "player has control — past GO, before the finish line";
  } else if (s.derived.in_race === true){
    const pr = s.values.race_progress?.raw;
    b.className = "banner menu";
    bt.textContent = pr >= 7 ? "FINISHED — NO CONTROL"
                   : "IN RACE SCENE — NO CONTROL";
    bs.textContent = `race_progress = ${pr ?? "unreadable"}`;
  } else if (s.derived.in_race === false){
    b.className = "banner menu"; bt.textContent = "IN MENUS";
    bs.textContent = "scene_id = " + (s.values.scene_id?.raw ?? "?");
  } else {
    b.className = "banner off"; bt.textContent = "UNKNOWN"; bs.textContent = "";
  }

  drawMap(s);

  const now = performance.now();
  (META ? META.watches : []).forEach(w => {
    const e = s.values[w.key];
    if (!e) return;
    const raw = e.raw;
    if (LAST[w.key] !== raw){ FLASH[w.key] = now; LAST[w.key] = raw; }
    (HIST[w.key] = HIST[w.key] || []).push(raw);
    if (HIST[w.key].length > HMAX) HIST[w.key].shift();

    const f = fmtVal(w, raw);
    const ve = document.getElementById("v_" + w.key);
    if (ve){ ve.textContent = f.t; ve.className = "val " + f.cls; }
    const me = document.getElementById("m_" + w.key);
    if (me) me.textContent =
      (e.addr !== null && e.addr !== undefined
        ? "0x" + e.addr.toString(16).toUpperCase() : "unresolved")
      + (typeof raw === "number" && !w.enum[String(raw)]
        ? "  •  0x" + ((raw>>>0).toString(16).toUpperCase()) : "");
    const se = document.getElementById("s_" + w.key);
    if (se) se.innerHTML = spark(w.key);
    const ce = document.getElementById("c_" + w.key);
    if (ce) ce.classList.toggle("flash", now - (FLASH[w.key]||0) < 350);
  });
}

async function post(url, body){
  const err = document.getElementById("writeErr");
  err.textContent = "";
  err.className = "err";
  try {
    const r = await fetch(url, {method:"POST",
      headers:{"Content-Type":"application/json"}, body: JSON.stringify(body)});
    const j = await r.json();
    if (j.error){ err.textContent = j.error; return null; }
    return j;
  } catch (e) {
    err.textContent = String(e);
    return null;
  }
}

document.getElementById("btnGive").onclick = async () => {
  const count = document.getElementById("iCount").value.trim();
  const j = await post("/api/item", {
    racer: Number(document.getElementById("iRacer").value),
    id: Number(document.getElementById("iItem").value),
    count: count === "" ? undefined : Number(count)});
  if (j){
    const err = document.getElementById("writeErr");
    err.className = "ok";
    err.textContent = `gave ${j.name}`;
  }
};

document.getElementById("btnState").onclick = async () => {
  const frames = document.getElementById("iFrames").value.trim();
  const name = document.getElementById("iState").value;
  const j = await post("/api/state", {
    racer: Number(document.getElementById("iRacer").value),
    state: name, on: true,
    frames: frames === "" ? undefined : Number(frames)});
  if (j){
    const err = document.getElementById("writeErr");
    err.className = j.effective ? "ok" : "err";
    err.textContent = j.effective
      ? `set ${name}`
      : `${name} flag and timer written, but the kart will not change - the `
        + `game does the visible part in its activation routine`;
  }
};

document.getElementById("btnClear").onclick = () =>
  post("/api/state", {racer: Number(document.getElementById("iRacer").value),
                      clear: true});

document.getElementById("addForm").onsubmit = async ev => {
  ev.preventDefault();
  const err = document.getElementById("err"); err.textContent = "";
  const r = await fetch("/api/watch", {
    method:"POST", headers:{"Content-Type":"application/json"},
    body: JSON.stringify({addr:fAddr.value, kind:fKind.value, label:fLabel.value})
  });
  const j = await r.json();
  if (j.error){ err.textContent = j.error; return; }
  fAddr.value = ""; fLabel.value = "";
  await loadMeta();
};

loadMeta().then(() => {
  const es = new EventSource("/events");
  es.onmessage = m => render(JSON.parse(m.data));
  es.onerror = () => {
    document.getElementById("hook").textContent = "stream lost";
    document.getElementById("hook").className = "pill bad";
  };
});
</script>
</body>
</html>
)HTML";
}  // namespace

std::string_view DashboardHtml()
{
  static const std::string page = [] {
    std::string out;
    out.reserve(STYLE.size() + BODY.size() + SCRIPT_CARDS.size() + SCRIPT_MAP.size() +
                SCRIPT_RENDER.size());
    out += STYLE;
    out += BODY;
    out += SCRIPT_CARDS;
    out += SCRIPT_MAP;
    out += SCRIPT_RENDER;
    return out;
  }();
  return page;
}
}  // namespace MemInspect::Web

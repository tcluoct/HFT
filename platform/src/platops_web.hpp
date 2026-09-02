#pragma once
// platops_web.hpp — the ops console page, embedded as one self-contained
// string (no CDN, no external assets: the colo box has no internet). Served
// at GET /; polls /api/state each second and POSTs operator lines to
// /api/cmd as "<shard>\n<command>". Destructive actions confirm first.

namespace platops {

inline const char* kWebPage = R"__(<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>platops</title>
<style>
:root{--bg:#101418;--card:#171d24;--line:#242d37;--fg:#cdd6e0;--dim:#7a8794;
--green:#3fb26f;--red:#e05c5c;--amber:#e0a23c;--blue:#4d9fdc;font-size:14px}
*{box-sizing:border-box;margin:0}
body{background:var(--bg);color:var(--fg);font:13px/1.45 Consolas,Menlo,monospace;padding:14px}
h1{font-size:16px;letter-spacing:1px} h2{font-size:13px;color:var(--dim);margin:10px 0 4px}
.top{display:flex;gap:14px;align-items:baseline;margin-bottom:12px}
.top .clock{color:var(--dim)} .top .conn{margin-left:auto;font-size:12px}
.card{background:var(--card);border:1px solid var(--line);border-radius:8px;
padding:12px;margin-bottom:14px}
.head{display:flex;gap:8px;align-items:center;flex-wrap:wrap}
.head b{font-size:15px}
.pill{padding:1px 8px;border-radius:9px;font-size:11px;border:1px solid var(--line)}
.pill.live{color:var(--green);border-color:var(--green)}
.pill.sealed{color:var(--blue);border-color:var(--blue)}
.pill.waiting,.pill.no-sidecar{color:var(--dim)}
.pill.warn{color:var(--amber);border-color:var(--amber)}
.pill.bad{color:var(--red);border-color:var(--red)}
.alerts{margin:6px 0} .alerts span{color:#14181d;background:var(--amber);border-radius:4px;
padding:1px 8px;margin-right:6px;font-weight:bold}
table{border-collapse:collapse;width:100%;margin:2px 0 8px}
th{color:var(--dim);text-align:right;font-weight:normal;padding:2px 8px;
border-bottom:1px solid var(--line)}
td{text-align:right;padding:2px 8px;border-bottom:1px solid #1c232b;white-space:nowrap}
th:first-child,td:first-child{text-align:left}
.num.pos{color:var(--green)} .num.neg{color:var(--red)}
button{background:#202932;color:var(--fg);border:1px solid var(--line);border-radius:4px;
padding:1px 8px;cursor:pointer;font:inherit;font-size:11px}
button:hover{border-color:var(--blue)} button.danger:hover{border-color:var(--red);color:var(--red)}
input,select{background:#0d1116;color:var(--fg);border:1px solid var(--line);
border-radius:4px;padding:2px 6px;font:inherit;width:90px}
.row{display:flex;gap:6px;align-items:center;flex-wrap:wrap;margin:4px 0}
.cols{display:flex;gap:16px;flex-wrap:wrap} .cols>div{flex:1;min-width:340px}
.feed{max-height:180px;overflow-y:auto}
.dim{color:var(--dim)}
</style></head><body>
<div class="top"><h1>PLATOPS</h1><span class="clock" id="clock">--:--:--</span>
<span class="dim">journal-mirror ops console — every action is journaled &amp; risk-checked</span>
<span class="conn" id="conn">connecting…</span></div>
<div id="shards"></div>
<script>
"use strict";
const $=(id)=>document.getElementById(id);
const esc=(s)=>String(s).replace(/[&<>"']/g,c=>({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;","'":"&#39;"}[c]));
const cny=(v)=>{const n=+v;const c=n>0?"pos":n<0?"neg":"";return `<span class="num ${c}">${n.toFixed(2)}</span>`};
let firstBuild=true;

async function post(shard,cmd,confirmMsg){
  if(confirmMsg&&!window.confirm(confirmMsg+"\n\n["+shard+"] "+cmd))return;
  try{
    const r=await fetch("/api/cmd",{method:"POST",body:shard+"\n"+cmd});
    if(!r.ok)alert("command failed: "+await r.text());
  }catch(e){alert("command failed: "+e)}
}
window.ops={post};

function pill(s){return `<span class="pill ${esc(s)}">${esc(s)}</span>`}

function shardSkeleton(sh){
  const n=esc(sh.name);
  return `<div class="card" id="card-${n}">
  <div class="head"><b>${n}</b><span id="st-${n}"></span>
   <span style="margin-left:auto"></span>
   <button onclick="ops.post('${n}','status')">status→log</button>
   <button onclick="ops.post('${n}','arm')">arm</button>
   <button onclick="ops.post('${n}','disarm')">disarm</button>
   <button class="danger" onclick="ops.post('${n}','halt','HALT the whole site? Resting orders are cancelled.')">HALT</button>
   <button class="danger" onclick="ops.post('${n}','flatten','FLATTEN the whole site? Positions will be closed out.')">FLATTEN</button>
  </div>
  <div class="alerts" id="al-${n}"></div>
  <h2>strategies</h2><div id="strat-${n}"></div>
  <div class="cols">
   <div><h2>positions</h2><div id="pos-${n}"></div>
        <h2>open orders</h2><div id="ord-${n}"></div>
        <h2>manual order (risk-checked, journaled)</h2>
        <div class="row">
         <input id="mo-id-${n}" placeholder="strat id" size="6">
         <input id="mo-sym-${n}" placeholder="symbol">
         <select id="mo-side-${n}"><option>buy</option><option>sell</option></select>
         <select id="mo-off-${n}"><option>open</option><option>closetoday</option><option>close</option></select>
         <input id="mo-px-${n}" placeholder="px CNY">
         <input id="mo-q-${n}" placeholder="lots" size="4">
         <button class="danger" onclick="manualOrder('${n}')">send</button>
        </div></div>
   <div><h2>fills</h2><div class="feed" id="fills-${n}"></div>
        <h2>risk denies</h2><div class="feed" id="deny-${n}"></div>
        <h2>risk</h2><div id="risk-${n}"></div>
        <h2>operator audit</h2><div class="feed" id="ops-${n}"></div></div>
  </div></div>`;
}

function manualOrder(n){
  const v=(x)=>$( x+"-"+n).value.trim();
  const id=v("mo-id"),sym=v("mo-sym"),side=v("mo-side"),off=v("mo-off"),px=v("mo-px"),q=v("mo-q");
  if(!id||!sym||!px||!q){alert("fill strat id / symbol / px / lots");return}
  post(n,`order ${id} ${sym} ${side} ${off} ${px} ${q}`,
       `Send MANUAL ${side.toUpperCase()} ${off} ${sym} ${q} lot(s) @ ${px} under strategy ${id}?`);
}
window.manualOrder=manualOrder;

function tuneParam(n,id){
  const key=window.prompt("parameter key (e.g. lots, take_ticks, stop_ticks, timeout_ms, imb_gate_e4, max_entries)");
  if(!key)return;
  const val=window.prompt("integer value for "+key);
  if(val===null||val.trim()==="")return;
  post(n,`param ${id} ${key.trim()} ${val.trim()}`,`Update strategy ${id}: ${key} = ${val}?`);
}
window.tuneParam=tuneParam;

function transferPos(n,sym,strat){
  const side=window.prompt("which book: long or short","long");if(!side)return;
  const to=window.prompt("transfer TO strategy id");if(!to)return;
  const q=window.prompt("lots to move");if(!q)return;
  post(n,`transfer ${sym} ${strat} ${to.trim()} ${side.trim()} ${q.trim()}`,
       `Move ${q} ${side} lot(s) of ${sym} from strategy ${strat} to ${to}? (bookkeeping only)`);
}
window.transferPos=transferPos;

function render(data){
  $("clock").textContent=data.now;
  const box=$("shards");
  if(firstBuild){box.innerHTML=data.shards.map(shardSkeleton).join("");firstBuild=false}
  for(const sh of data.shards){
    const n=esc(sh.name);
    if(!$("card-"+n)){box.insertAdjacentHTML("beforeend",shardSkeleton(sh))}
    const md=sh.status!=="live"?"md -":sh.md_age_ms<0?"no md"
      :sh.md_age_ms<2000?"md "+sh.md_age_ms+"ms":"md "+(sh.md_age_ms/1000|0)+"s";
    const mdBad=sh.status==="live"&&sh.md_age_ms>5000&&sh.phase==="Continuous";
    $("st-"+n).innerHTML=pill(sh.status)+" "+pill(esc(sh.phase))
      +` <span class="pill ${mdBad?"bad":""}">${md}</span>`
      +` <span class="pill">day ${sh.day}</span><span class="pill">${sh.envelopes} env</span>`
      +(sh.risk.any_armed?' <span class="pill bad">KILL ARMED</span>':"");
    $("al-"+n).innerHTML=sh.alerts.map(a=>`<span>${esc(a)}</span>`).join("");
    $("strat-"+n).innerHTML=`<table><tr><th>strategy</th><th>type</th><th>state</th>
      <th>realized</th><th>unrealized</th><th>fees</th><th>net</th><th>actions</th></tr>`+
      sh.strategies.map(s=>{
        const net=(+s.realized)+(+s.unrealized);
        return `<tr><td>${s.id} ${esc(s.name)}</td><td>${esc(s.type)}</td>
        <td>${s.halted?'<span class="pill bad">halted</span>':'<span class="pill live">run</span>'}</td>
        <td>${cny(s.realized)}</td><td>${cny(s.unrealized)}</td><td class="dim">${(+s.fees).toFixed(2)}</td><td>${cny(net)}</td>
        <td><button onclick="tuneParam('${n}',${s.id})">tune</button>
            <button onclick="ops.post('${n}','disarm ${s.id}')">run</button>
            <button class="danger" onclick="ops.post('${n}','halt ${s.id}','Halt strategy ${s.id}?')">halt</button>
            <button class="danger" onclick="ops.post('${n}','flatten ${s.id}','Flatten strategy ${s.id} positions?')">flatten</button>
        </td></tr>`}).join("")+"</table>";
    $("pos-"+n).innerHTML=`<table><tr><th>strat</th><th>symbol</th><th>Lyd</th><th>Ltd</th>
      <th>Syd</th><th>Std</th><th>net</th><th>cost L/S</th><th>mark</th><th>unrl</th><th></th></tr>`+
      sh.positions.map(p=>`<tr><td>${p.strategy}</td><td>${esc(p.symbol)}</td>
        <td>${p.long_yd}</td><td>${p.long_td}</td><td>${p.short_yd}</td><td>${p.short_td}</td>
        <td>${p.net}</td><td class="dim">${p.long_cost}/${p.short_cost}</td><td>${p.mark}</td>
        <td>${cny(p.unrealized)}</td>
        <td><button onclick="transferPos('${n}','${esc(p.symbol)}',${p.strategy})">transfer</button></td>
        </tr>`).join("")+"</table>";
    $("ord-"+n).innerHTML=sh.orders.length?`<table><tr><th>id</th><th>symbol</th><th>strat</th>
      <th>side</th><th>off</th><th>px</th><th>leaves</th><th>state</th><th>age</th><th></th></tr>`+
      sh.orders.map(o=>`<tr><td class="dim">${o.cl_id}</td><td>${esc(o.symbol)}</td>
        <td>${o.strategy}</td><td>${o.side}</td><td>${o.offset}</td><td>${o.px}</td>
        <td>${o.leaves}/${o.qty}</td><td>${o.state}</td><td>${o.age_s}s</td>
        <td><button class="danger" onclick="ops.post('${n}','cancel 0x${o.cl_id}','Cancel order 0x${o.cl_id}?')">cancel</button></td>
        </tr>`).join("")+"</table>":'<span class="dim">none</span>';
    $("fills-"+n).innerHTML=sh.fills.length?`<table>`+
      sh.fills.map(f=>`<tr><td class="dim">${f.ts}</td><td>${esc(f.symbol)}</td><td>s${f.strategy}</td>
        <td>${f.side} ${f.offset}</td><td>${f.px}</td><td>x${f.qty}</td></tr>`).join("")+"</table>"
      :'<span class="dim">none</span>';
    $("deny-"+n).innerHTML=sh.denies.length?`<table>`+
      sh.denies.map(d=>`<tr><td class="dim">${d.ts}</td><td>${esc(d.symbol)}</td><td>s${d.strategy}</td>
        <td class="num neg">${esc(d.check)}</td><td>${d.cancel?"cancel":"order"}</td></tr>`).join("")+"</table>"
      :'<span class="dim">none</span>';
    const mu=+sh.risk.margin_used,ml=+sh.risk.margin_limit;
    $("risk-"+n).innerHTML=`pass ${sh.risk.pass} / deny ${sh.risk.deny}
      &nbsp; margin ${mu.toFixed(0)} / ${ml.toFixed(0)} CNY
      (${ml>0?(100*mu/ml).toFixed(1):"0"}%)<br>`+
      Object.entries(sh.risk.checks).map(([k,v])=>`<span class="pill warn">${esc(k)}: ${v}</span>`).join(" ");
    $("ops-"+n).innerHTML=sh.ops.length?`<table>`+
      sh.ops.map(r=>`<tr><td class="dim">${r.ts}</td><td>${esc(r.text)}</td></tr>`).join("")+"</table>"
      :'<span class="dim">none</span>';
  }
}

async function tick(){
  try{
    const r=await fetch("/api/state");
    render(await r.json());
    $("conn").textContent="live";$("conn").style.color="var(--green)";
  }catch(e){
    $("conn").textContent="DISCONNECTED";$("conn").style.color="var(--red)";
  }
  setTimeout(tick,1000);
}
tick();
</script></body></html>
)__";

}  // namespace platops

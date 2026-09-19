#include "http/api_server.hpp"

#include <sys/stat.h>
#include <sys/wait.h>
#include <csignal>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include <httplib.h>

#include "aes67/daemon_client.hpp"
#include "engine.hpp"
#include "log.hpp"
#include "service.hpp"
#include "version.hpp"

namespace aes67_srt {
namespace {

bool directory_exists(const std::string& path) {
  if (path.empty()) {
    return false;
  }
  struct stat info {};
  if (stat(path.c_str(), &info) != 0) {
    return false;
  }
  return (info.st_mode & S_IFDIR) != 0;
}

void reply_json(httplib::Response& response, const nlohmann::json& body,
                int status = 200) {
  response.status = status;
  response.set_content(body.dump(), "application/json");
}

void reply_text(httplib::Response& response, int status,
                const std::string& message) {
  response.status = status;
  response.set_content(message, "text/plain");
}

/** Parse a JSON request body, setting |error| rather than throwing. */
nlohmann::json parse_body(const httplib::Request& request, std::string* error) {
  if (request.body.empty()) {
    *error = "empty request body";
    return nlohmann::json::object();
  }
  try {
    return nlohmann::json::parse(request.body);
  } catch (const std::exception& ex) {
    *error = std::string("invalid JSON: ") + ex.what();
    return nlohmann::json::object();
  }
}

/**
 * Write a configuration file by replace, not by truncate.
 *
 * A half-written file is an appliance that will not start, so the new document
 * goes to a sibling and a rename (atomic within a filesystem) puts it in place.
 */
bool write_atomically(const std::string& path, const std::string& text,
                      std::string* error) {
  const std::string temporary = path + ".tmp";
  bool wrote_temporary = false;
  {
    std::ofstream out(temporary, std::ios::trunc);
    if (out.is_open()) {
      out << text;
      wrote_temporary = out.good();
    }
  }
  if (wrote_temporary && std::rename(temporary.c_str(), path.c_str()) == 0) {
    return true;
  }
  // The write-and-rename needs the *directory* writable, and a hardened systemd
  // unit bind-mounts the configuration file itself instead. That case cannot even
  // create the temporary, so fall through to a plain truncating write: a
  // configuration change the operator asked for must still land, and the
  // alternative is a page that refuses every save on an installed appliance.
  std::remove(temporary.c_str());
  std::ofstream direct(path, std::ios::trunc);
  if (!direct.is_open()) {
    *error = "cannot write " + path;
    return false;
  }
  direct << text;
  if (!direct.good()) {
    *error = "error writing " + path;
    return false;
  }
  return true;
}

/**
 * The page served until the built web UI exists.
 *
 * It is not a placeholder that says "not implemented": it polls `/api/status` and
 * renders exactly the same preflight the real page will, so an appliance with no
 * daemon and no link still loads a page and says what is wrong — which is one of
 * this ticket's acceptance criteria, and a behaviour worth being able to exercise
 * before any front-end toolchain is involved.
 */
const char* k_fallback_page = R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta name="color-scheme" content="light dark">
<title>aes67-srt</title>
<style>
 :root {
   --bg:#f5f6f8; --surface:#ffffff; --border:#dde1e6; --text:#191d21; --muted:#606a74;
   --ok:#157f3b; --bad:#b42318; --ok-bg:#e7f6ec; --bad-bg:#fdecea; --radius:8px;
   --mono: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;
 }
 @media (prefers-color-scheme: dark) {
   :root { --bg:#0f1214; --surface:#171b1f; --border:#2a3037; --text:#e8ecf0;
           --muted:#9aa4ad; --ok:#5bd07b; --bad:#ff7a70; --ok-bg:#16301f; --bad-bg:#3a1c1a; }
 }
 * { box-sizing:border-box; }
 body { margin:0; background:var(--bg); color:var(--text);
        font:15px/1.5 system-ui, -apple-system, "Segoe UI", Roboto, sans-serif; }
 header { display:flex; align-items:baseline; gap:.75rem; flex-wrap:wrap;
          padding:.9rem 1.25rem; border-bottom:1px solid var(--border); background:var(--surface); }
 header .brand { font-weight:650; letter-spacing:-.01em; }
 header .meta { color:var(--muted); font-size:.85rem; overflow-wrap:anywhere; }
 main { max-width:64rem; margin:0 auto; padding:1.25rem; display:grid; gap:1rem;
        grid-template-columns:repeat(auto-fit, minmax(21rem, 1fr)); }
 .card { background:var(--surface); border:1px solid var(--border);
         border-radius:var(--radius); padding:1rem 1.1rem; }
 .card h2 { font-size:.78rem; text-transform:uppercase; letter-spacing:.06em;
            color:var(--muted); margin:0 0 .7rem; font-weight:600; }
 .wide { grid-column:1 / -1; }
 .pill { display:inline-block; padding:.05rem .5rem; border-radius:999px; font-size:.78rem; font-weight:600; }
 .pill.ok { background:var(--ok-bg); color:var(--ok); }
 .pill.bad { background:var(--bad-bg); color:var(--bad); }
 .row { display:grid; grid-template-columns:5.5rem 3.2rem 1fr; gap:.6rem;
        align-items:baseline; padding:.35rem 0; border-top:1px solid var(--border); }
 .row:first-child { border-top:0; }
 .row .name { font-weight:600; }
 .row .detail { color:var(--muted); font-size:.88rem; overflow-wrap:anywhere; }
 .figures { display:grid; grid-template-columns:1fr auto; gap:.3rem .75rem; margin:0; }
 .figures dt { color:var(--muted); }
 .figures dd { margin:0; font-family:var(--mono); font-variant-numeric:tabular-nums; text-align:right; }
 .controls { display:flex; gap:.5rem; align-items:center; flex-wrap:wrap; }
 label { color:var(--muted); font-size:.9rem; }
 input, select, button { font:inherit; color:var(--text); background:var(--surface);
                         border:1px solid var(--border); border-radius:6px; padding:.3rem .45rem; }
 input[type=number] { width:6rem; font-family:var(--mono); }
 button { cursor:pointer; }
 button:hover { border-color:var(--muted); }
 button:focus-visible, input:focus-visible, select:focus-visible { outline:2px solid var(--ok); outline-offset:1px; }
 .msg { font-size:.88rem; }
 .msg.ok { color:var(--ok); } .msg.bad { color:var(--bad); }
 .form { display:grid; grid-template-columns:repeat(auto-fit, minmax(11rem, 1fr)); gap:.6rem; }
 .form .actions { grid-column:1 / -1; display:flex; gap:.6rem; align-items:center; flex-wrap:wrap; }
 .banner { border-radius:var(--radius); padding:.65rem .9rem; font-weight:500; }
 .banner.bad { background:var(--bad-bg); color:var(--bad); }
 .sub { font-size:.9rem; margin:.5rem 0 .2rem; }
 .sub:first-child { margin-top:0; }
 .mini { padding:.1rem .5rem; font-size:.82rem; }
 .list { display:flex; flex-wrap:wrap; gap:.4rem; }
 .tag { border:1px solid var(--border); border-radius:999px; padding:.1rem .55rem; font-size:.82rem; }
 footer { max-width:64rem; margin:0 auto; padding:0 1.25rem 2rem; color:var(--muted); font-size:.8rem; }
</style>
</head>
<body>
<header>
  <span class="brand">aes67-srt</span>
  <span class="meta" id="meta">connecting&hellip;</span>
  <span class="pill" id="runpill" style="margin-left:auto">&hellip;</span>
</header>
<main>
  <div id="banner" class="banner bad wide" hidden></div>
  <section class="card">
    <h2>Preflight</h2>
    <div id="checks"></div>
  </section>
  <section class="card wide">
    <h2>Service</h2>
    <div class="controls">
      <span id="enginestate" class="pill">?</span>
      <button id="startengine" type="button">Start</button>
      <button id="stopengine" type="button">Stop</button>
      <button id="restartengine" type="button">Restart</button>
      <button id="restartprocess" type="button">Restart process</button>
      <span class="msg" id="enginemsg" role="status"></span>
    </div>
    <p class="msg">Stop and Start the audio without stopping this page. Restart applies a
    saved configuration change.</p>
  </section>
  <section class="card">
    <h2>Live</h2>
    <dl class="figures" id="figures"></dl>
  </section>
  <section class="card wide">
    <h2>A/V alignment</h2>
    <div class="controls">
      <label for="delay">Delay (ms)</label>
      <input id="delay" type="number" min="0" max="5000" step="0.1">
      <button id="setdelay" type="button">Set delay</button>
      <button id="trigger" type="button">Trigger test signal</button>
      <span class="msg" id="controlmsg" role="status"></span>
    </div>
  </section>
  <section class="card wide">
    <h2>Configuration</h2>
    <form class="form" id="linkform" onsubmit="return false;">
      <label>Mode<br><select id="c_mode"><option>caller</option><option>listener</option><option>rendezvous</option><option>loopback</option></select></label>
      <label>Role<br><select id="c_role"><option>tx</option><option>rx</option><option>duplex</option></select></label>
      <label>Peer<br><input id="c_peer" placeholder="host:port"></label>
      <label>Local port<br><input id="c_local_port" type="number"></label>
      <label>Latency (ms)<br><input id="c_latency_ms" type="number"></label>
      <label>Codec<br><select id="c_codec_mode"><option value="pcm_l24">Lossless (PCM L24)</option><option value="opus">Compressed (Opus)</option></select></label>
      <label>Bitrate/ch (Opus)<br><input id="c_bitrate" type="number" step="1000" min="6000" max="256000" value="128000"></label>
      <label>Frame (samples)<br><input id="c_period_frames" type="number" min="48"></label>
      <label>Blocks<br><input id="c_blocks" type="number" min="1" max="8"></label>
      <label>Passphrase<br><input id="c_passphrase" type="password"></label>
      <div class="actions">
        <button id="savelink" type="button">Save configuration</button>
        <span class="msg" id="configmsg" role="status"></span>
      </div>
    </form>
    <div class="sub">Blocks, mapping and levels</div>
    <div id="blocks"></div>
    <p class="msg">A change to the link, the mapping or a level needs a restart; the A/V delay does not.</p>
  </section>
  <section class="card wide">
    <h2>AES67 daemon</h2>
    <div id="aes67"><span class="msg">loading&hellip;</span></div>
    <div class="controls" style="margin-top:.8rem">
      <label for="a_block">Block</label>
      <input id="a_block" type="number" min="0" value="0" style="width:4rem">
      <label for="a_source">to</label>
      <input id="a_source" list="discovered" placeholder="discovered name or self">
      <datalist id="discovered"></datalist>
      <button id="subscribe" type="button">Subscribe sink</button>
      <button id="unsubscribe" type="button">Unsubscribe</button>
      <button id="publish" type="button">Publish sources</button>
      <button id="daemonstart" type="button">Start daemon</button>
      <button id="daemonstop" type="button">Stop daemon</button>
      <button id="daemonrestart" type="button">Restart daemon</button>
      <span class="msg" id="aes67msg" role="status"></span>
    </div>
  </section>
</main>
<footer>Configuration is validated before it is written; a field that needs a restart says so.</footer>
<script>
const $ = (id) => document.getElementById(id);
const pill = (ok) => '<span class="pill ' + (ok ? 'ok' : 'bad') + '">' + (ok ? 'ok' : 'FAIL') + '</span>';
function banner(text) { const b = $('banner'); if (!text) { b.hidden = true; return; } b.hidden = false; b.textContent = text; }

async function poll() {
  try {
    const s = await (await fetch('/api/status')).json();
    $('meta').textContent = 'v' + s.version + ' · ' + s.build + ' · ' + s.role + ' ' + s.mode + (s.peer ? ' → ' + s.peer : '');
    $('runpill').className = 'pill ' + (s.engine.running ? 'ok' : 'bad');
    $('runpill').textContent = s.engine.running ? 'running' : 'stopped';
    const state = s.engine.state || (s.engine.running ? 'running' : 'stopped');
    $('enginestate').className = 'pill ' + (state === 'running' ? 'ok' : (state === 'failed' ? 'bad' : ''));
    $('enginestate').textContent = state;
    if (s.engine.error) { $('enginemsg').className = 'msg bad'; $('enginemsg').textContent = s.engine.error; }

    const p = s.preflight || { ok: false, checks: [] };
    $('checks').innerHTML = p.checks.map((c) =>
      '<div class="row"><span class="name">' + c.name + '</span>' + pill(c.ok) +
      '<span class="detail">' + c.detail + '</span></div>').join('');

    const e = s.engine;
    const figures = [
      ['playout delay', e.delay_ms.toFixed(1) + ' ms'],
      ['A/V offset', e.egress_delay_ms.toFixed(1) + ' ms'],
      ['total A/V delay', e.av_delay_ms.toFixed(1) + ' ms'],
      ['clock correction', e.clock_offset_ppm.toFixed(2) + ' ppm'],
      ['clock ratio', e.clock_ratio.toFixed(9)],
      ['frames sent', e.frames_sent],
      ['frames received', e.frames_received],
      ['frames refused', e.frames_refused],
      ['frames concealed', e.frames_concealed],
      ['silence periods', e.silence_periods]
    ];
    if (s.link && s.link.available) {
      figures.push(['RTT', s.link.rtt_ms.toFixed(1) + ' ms'],
                   ['receive rate', s.link.receive_rate_mbps.toFixed(1) + ' Mbps'],
                   ['packets lost', s.link.packets_lost],
                   ['retransmitted', s.link.packets_retransmitted],
                   ['packets dropped', s.link.packets_dropped]);
    }
    $('figures').innerHTML = figures.map(([k, v]) => '<dt>' + k + '</dt><dd>' + v + '</dd>').join('');

    const notices = [];
    if (!p.ok) notices.push('Preflight failed: audio will not flow until every check passes.');
    if (s.pending_restart && s.pending_restart.length) notices.push('Saved, waiting for a restart: ' + s.pending_restart.join(', '));
    banner(notices.join(' '));

    const d = $('delay');
    if (document.activeElement !== d) d.value = e.egress_delay_ms;
  } catch (err) {
    $('meta').textContent = 'unreachable';
    $('runpill').className = 'pill bad';
    $('runpill').textContent = 'no connection';
    banner('Cannot reach the appliance: ' + err);
  }
}

$('setdelay').onclick = async () => {
  const value = parseFloat($('delay').value);
  const r = await fetch('/api/egress/delay', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ delay_ms: value }) });
  const m = $('controlmsg');
  if (r.ok) { m.className = 'msg ok'; m.textContent = 'delay set to ' + value + ' ms'; }
  else { m.className = 'msg bad'; m.textContent = await r.text(); }
};
$('trigger').onclick = async () => {
  const r = await fetch('/api/egress/test-signal', { method: 'POST' });
  const m = $('controlmsg');
  m.className = r.ok ? 'msg ok' : 'msg bad';
  m.textContent = r.ok ? 'impulse fired' : await r.text();
};

let currentConfig = null;
async function engineAction(path, oktext) {
  const m = $('enginemsg'); m.className = 'msg'; m.textContent = 'working…';
  const r = await fetch(path, { method: 'POST' });
  m.className = r.ok ? 'msg ok' : 'msg bad';
  m.textContent = r.ok ? oktext : await r.text();
  poll();
}
$('startengine').onclick = () => engineAction('/api/engine/start', 'started');
$('stopengine').onclick = () => engineAction('/api/engine/stop', 'stopped');
$('restartengine').onclick = () => engineAction('/api/engine/restart', 'restarting');
$('restartprocess').onclick = async () => {
  const m = $('enginemsg'); m.className = 'msg'; m.textContent = 'restarting the process…';
  await fetch('/api/process/restart', { method: 'POST' }).catch(() => {});
  m.className = 'msg'; m.textContent = 'restarting; the page will reconnect';
};
// The mode control sets every block at once — the operator thinks "lossless or
// compressed", not "block 3's payload type". The per-block selects stay for the
// advanced case, and are what is saved.
function applyCodecMode(codec) {
  const bitrate = $('c_bitrate').value;
  document.querySelectorAll('[data-codec]').forEach((sel) => { sel.value = codec; });
  document.querySelectorAll('[data-bitrate]').forEach((inp) => { inp.value = bitrate; });
  $('c_period_frames').value = codec === 'opus' ? 960 : 48;
}
$('c_codec_mode').onchange = () => applyCodecMode($('c_codec_mode').value);
$('c_bitrate').onchange = () => {
  const value = $('c_bitrate').value;
  document.querySelectorAll('[data-bitrate]').forEach((inp) => { inp.value = value; });
};

async function loadConfig() {
  try {
    currentConfig = await (await fetch('/api/config')).json();
    const l = currentConfig.link;
    $('c_mode').value = l.mode; $('c_role').value = l.role;
    $('c_peer').value = l.peer || ''; $('c_local_port').value = l.local_port;
    $('c_latency_ms').value = l.latency_ms; $('c_blocks').value = l.blocks;
    $('c_passphrase').value = l.passphrase || '';
    $('c_period_frames').value = currentConfig.audio.period_frames;
    renderBlocks();
    const blocks = currentConfig.blocks || [];
    const allOpus = blocks.length > 0 &&
                    blocks.every((b) => (b.codec || 'pcm_l24') === 'opus');
    $('c_codec_mode').value = allOpus ? 'opus' : 'pcm_l24';
    $('c_bitrate').value = (blocks[0] && blocks[0].bitrate_bps_per_channel) || 128000;
  } catch (e) { /* the status poll reports an unreachable appliance */ }
}
function renderBlocks() {
  $('blocks').innerHTML = (currentConfig.blocks || []).map((b, i) =>
    '<div class="row"><span class="name">block ' + b.index + '</span>' +
    '<span class="detail"><label>channels <input data-channels="' + i + '" value="' +
      (b.channels || []).join(',') + '" size="20" title="comma-separated device channels, eight of them"></label></span>' +
    '<span class="detail"><label>codec <select data-codec="' + i + '">' +
      ['pcm_l24', 'pcm_l16', 'opus'].map((c) =>
        '<option' + ((b.codec || 'pcm_l24') === c ? ' selected' : '') + '>' + c + '</option>').join('') +
    '</select></label> ' +
    '<label>bitrate/ch <input type="number" step="1000" min="6000" max="256000" data-bitrate="' + i + '" value="' + (b.bitrate_bps_per_channel || 128000) + '" title="Opus only; 6000..256000 bit/s per channel"></label> ' +
    '<label>gain dB <input type="number" step="0.1" data-gain="' + i + '" value="' + b.gain_db + '"></label> ' +
    '<label><input type="checkbox" data-mute="' + i + '"' + (b.mute ? ' checked' : '') + '> mute</label></span></div>').join('');
}
// Changing the block count regenerates the array rather than leaving a block list
// that no longer covers the channels, which validation would refuse without saying
// how to fix it. Existing blocks are kept; new ones get the conventional mapping.
$('c_blocks').onchange = () => {
  if (!currentConfig) return;
  const wanted = parseInt($('c_blocks').value, 10);
  const old = currentConfig.blocks || [];
  const next = [];
  for (let i = 0; i < wanted; i++) {
    if (old[i]) { next.push(old[i]); continue; }
    const channels = [];
    for (let c = 0; c < 8; c++) channels.push(i * 8 + c);
    next.push({ index: i, channels, gain_db: 0.0, mute: false, codec: 'pcm_l24', bitrate_bps_per_channel: 128000 });
  }
  currentConfig.blocks = next;
  renderBlocks();
};
$('savelink').onclick = async () => {
  if (!currentConfig) await loadConfig();
  if (!currentConfig) return;
  const n = (id) => parseInt($(id).value, 10);
  currentConfig.link.mode = $('c_mode').value;
  currentConfig.link.role = $('c_role').value;
  currentConfig.link.peer = $('c_peer').value;
  currentConfig.link.local_port = n('c_local_port');
  currentConfig.link.latency_ms = n('c_latency_ms');
  currentConfig.link.blocks = n('c_blocks');
  currentConfig.link.passphrase = $('c_passphrase').value;
  currentConfig.audio.period_frames = n('c_period_frames');
  (currentConfig.blocks || []).forEach((b, i) => {
    const gain = document.querySelector('[data-gain="' + i + '"]');
    const mute = document.querySelector('[data-mute="' + i + '"]');
    const codec = document.querySelector('[data-codec="' + i + '"]');
    const bitrate = document.querySelector('[data-bitrate="' + i + '"]');
    const channels = document.querySelector('[data-channels="' + i + '"]');
    if (gain) b.gain_db = parseFloat(gain.value);
    if (mute) b.mute = mute.checked;
    if (codec) b.codec = codec.value;
    if (bitrate) b.bitrate_bps_per_channel = parseInt(bitrate.value, 10);
    if (channels) b.channels = channels.value.split(',').map((x) => parseInt(x.trim(), 10)).filter((x) => !isNaN(x));
  });
  const r = await fetch('/api/config', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(currentConfig) });
  const m = $('configmsg');
  if (!r.ok) { m.className = 'msg bad'; m.textContent = await r.text(); return; }
  const res = await r.json();
  const restart = res.restart_required || [];
  m.className = restart.length ? 'msg bad' : 'msg ok';
  m.textContent = restart.length
    ? 'saved to ' + res.path + ' — restart to apply: ' + restart.join(', ')
    : 'saved to ' + res.path;
};

async function loadAes67() {
  const root = $('aes67');
  try {
    const a = await (await fetch('/api/aes67/status')).json();
    if (!a.reachable) {
      root.innerHTML = '<p class="msg bad">daemon unreachable' + (a.error ? ': ' + a.error : '') + '</p>';
      $('discovered').innerHTML = '';
      return;
    }
    const sources = a.sources || [], sinks = a.sinks || [], discovered = a.discovered || [];
    let html = '';
    html += '<div class="sub">' + sources.length + ' sources published</div>';
    html += sources.length
      ? '<div class="list">' + sources.map((s) => '<span class="tag">' + (s.name || ('source ' + s.id)) + '</span>').join('') + '</div>'
      : '<p class="msg">none</p>';
    html += '<div class="sub">' + sinks.length + ' sinks subscribed</div>';
    html += sinks.length
      ? sinks.map((s) => '<div class="row"><span class="name">block ' + s.id + '</span><span class="pill ' +
          (s.receiving ? 'ok' : 'bad') + '">' + (s.receiving ? 'receiving' : (s.in_use ? 'no RTP' : 'no stream')) +
          '</span><span class="detail"></span></div>').join('')
      : '<p class="msg">none</p>';
    html += '<div class="sub">' + discovered.length + ' discovered</div>';
    html += discovered.length
      ? discovered.map((d) => '<div class="row"><span class="name">' + (d.name || '(unnamed)') +
          '</span><span class="detail">' + (d.address || '') + '</span>' +
          '<button class="mini" type="button" onclick="subscribeTo(this.dataset.name)" data-name="' +
          (d.name || '') + '">use</button></div>').join('')
      : '<p class="msg">none</p>';
    root.innerHTML = html;
    $('discovered').innerHTML = discovered.map((d) => '<option value="' + (d.name || '') + '">').join('');
  } catch (e) {
    root.innerHTML = '<p class="msg bad">' + e + '</p>';
  }
}
function subscribeTo(name) { $('a_source').value = name; }
async function aes67Action(path, body, oktext) {
  const m = $('aes67msg');
  const r = await fetch(path, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) });
  m.className = r.ok ? 'msg ok' : 'msg bad';
  m.textContent = r.ok ? oktext : await r.text();
  loadAes67();
}
$('subscribe').onclick = () => aes67Action('/api/aes67/subscribe',
  { block: parseInt($('a_block').value, 10), source: $('a_source').value }, 'sink subscribed');
$('unsubscribe').onclick = () => aes67Action('/api/aes67/unsubscribe',
  { block: parseInt($('a_block').value, 10) }, 'sink removed');
async function daemonAction(verb) {
  const m = $('aes67msg'); m.className = 'msg'; m.textContent = 'working…';
  const r = await fetch('/api/daemon/' + verb, { method: 'POST' });
  m.className = r.ok ? 'msg ok' : 'msg bad';
  m.textContent = r.ok ? ('daemon ' + verb + ' ok') : await r.text();
  loadAes67();
}
$('daemonstart').onclick = () => daemonAction('start');
$('daemonstop').onclick = () => daemonAction('stop');
$('daemonrestart').onclick = () => daemonAction('restart');
$('publish').onclick = async () => {
  const m = $('aes67msg');
  const r = await fetch('/api/aes67/publish', { method: 'POST' });
  m.className = r.ok ? 'msg ok' : 'msg bad';
  m.textContent = r.ok ? 'sources published' : await r.text();
  loadAes67();
};

poll();
setInterval(poll, 1000);
loadConfig();
loadAes67();
setInterval(loadAes67, 5000);
</script>
</body>
</html>
)HTML";

}  // namespace

Engine* ApiServer::engine() {
  return service_ != nullptr ? service_->engine() : nullptr;
}

ApiServer::ApiServer(Config* config, Service* service, daemon::DaemonClient* daemon,
                     std::string webui_dir, std::string config_path)
    : config_(config),
      service_(service),
      daemon_(daemon),
      webui_dir_(std::move(webui_dir)),
      config_path_(std::move(config_path)) {}

ApiServer::~ApiServer() {
  stop();
}

Config ApiServer::config_snapshot() {
  std::lock_guard<std::mutex> lock(config_mutex_);
  return config_ != nullptr ? *config_ : Config{};
}

nlohmann::json ApiServer::build_preflight() {
  const EngineStatus status =
      engine() != nullptr ? engine()->status() : EngineStatus{};
  const Config config = config_snapshot();

  const bool device_open = engine() != nullptr && engine()->backend() != nullptr &&
                           engine()->backend()->is_open();
  const bool link_open = status.link_open;

  nlohmann::json checks = nlohmann::json::array();
  bool ok = device_open && link_open;

  // The daemon and PTP checks belong to the appliance. The macOS endpoint carries
  // no AES67, no PTP and no daemon (ADR 0004), so with no daemon client they are
  // *absent* rather than reported as failures a site could not fix. On the
  // appliance they lead, because an unlocked slave is the failure that looks like
  // success.
  if (daemon_ != nullptr) {
    daemon::PtpStatus ptp;
    std::string daemon_error;
    bool daemon_ok = false;
    {
      std::lock_guard<std::mutex> lock(daemon_mutex_);
      daemon_ok = daemon_->get_ptp_status(&ptp, &daemon_error);
    }
    const bool ptp_locked = daemon_ok && ptp.status == "locked";
    ok = ok && daemon_ok && ptp_locked;
    checks.push_back(
        {{"name", "ptp"},
         {"ok", ptp_locked},
         {"detail", !daemon_ok ? std::string("unknown: the daemon is unreachable")
                               : (ptp_locked ? std::string("locked")
                                             : "not locked: " + ptp.status)}});
    checks.push_back({{"name", "daemon"},
                      {"ok", daemon_ok},
                      {"detail", daemon_ok ? daemon_->endpoint() : daemon_error}});
  }

  checks.push_back({{"name", "device"},
                    {"ok", device_open},
                    {"detail", engine() != nullptr && engine()->backend() != nullptr
                                   ? engine()->backend()->detail()
                                   : std::string("no audio backend")}});
  checks.push_back({{"name", "link"},
                    {"ok", link_open},
                    {"detail", link_open ? config.link.mode + " " + config.link.role
                                         : std::string("not open")}});

  return {{"ok", ok}, {"delay_ms", status.delay_ms}, {"checks", checks}};
}

nlohmann::json ApiServer::build_status() {
  const EngineStatus status =
      engine() != nullptr ? engine()->status() : EngineStatus{};

  nlohmann::json engine;
  engine["running"] = status.running;
  engine["link_open"] = status.link_open;
  engine["frames_sent"] = status.frames_sent;
  engine["frames_received"] = status.frames_received;
  engine["frames_refused"] = status.frames_refused;
  engine["frames_concealed"] = status.frames_concealed;
  engine["silence_periods"] = status.silence_periods;
  engine["delay_ms"] = status.delay_ms;
  engine["delay_fraction"] = status.delay_fraction;
  engine["egress_delay_ms"] = status.egress_delay_ms;
  engine["av_delay_ms"] = status.av_delay_ms;
  engine["clock_offset_ppm"] = status.clock_offset_ppm;
  engine["clock_ratio"] = status.clock_ratio;
  engine["test_signal_channel"] = status.test_signal_channel;
  engine["state"] = service_ != nullptr ? service_->state() : "stopped";
  engine["error"] = service_ != nullptr ? service_->last_error() : "";

  nlohmann::json link;
  link["available"] = status.link_stats_available;
  if (status.link_stats_available) {
    const transport::LinkStats& stats = status.link_stats;
    link["rtt_ms"] = stats.rtt_ms;
    link["bandwidth_mbps"] = stats.bandwidth_mbps;
    link["receive_rate_mbps"] = stats.receive_rate_mbps;
    link["receive_buffer_ms"] = stats.receive_buffer_ms;
    link["negotiated_latency_ms"] = stats.negotiated_latency_ms;
    link["send_buffer_ms"] = stats.send_buffer_ms;
    link["packets_received"] = stats.packets_received;
    link["packets_lost"] = stats.packets_lost;
    link["packets_retransmitted"] = stats.packets_retransmitted;
    link["packets_dropped"] = stats.packets_dropped;
  }

  const Config config = config_snapshot();
  nlohmann::json document;
  document["name"] = "aes67-srt";
  document["version"] = version_string();
  document["build"] = build_info();
  document["role"] = config.link.role;
  document["mode"] = config.link.mode;
  document["peer"] = config.link.peer;
  document["blocks"] = config.blocks.size();
  document["channels"] = config.audio.channels;
  {
    std::lock_guard<std::mutex> lock(config_mutex_);
    // A configuration POST that needs a restart says so here, rather than the
    // change appearing to have taken effect.
    document["pending_restart"] = pending_restart_;
    document["config_path"] = config_path_;
  }
  document["preflight"] = build_preflight();
  document["engine"] = engine;
  document["link"] = link;
  return document;
}

void ApiServer::register_routes() {
  httplib::Server* svr = server_.get();

  svr->Get("/api/version",
           [](const httplib::Request&, httplib::Response& response) {
             reply_json(response, {{"name", "aes67-srt"},
                                   {"version", version_string()},
                                   {"build", build_info()}});
           });

  svr->Get("/api/status",
           [this](const httplib::Request&, httplib::Response& response) {
             reply_json(response, build_status());
           });

  svr->Get(
      "/api/log", [](const httplib::Request& request, httplib::Response& response) {
        int lines = 200;
        if (request.has_param("lines")) {
          lines = std::atoi(request.get_param_value("lines").c_str());
          if (lines <= 0 || lines > 500) {
            lines = 500;
          }
        }
        reply_json(response, {{"lines", log().tail(static_cast<size_t>(lines))}});
      });

  // ---- configuration (ticket 14, issue #15) ------------------------------
  // The running document, and a whole-document replace. A change is validated
  // before anything is written or applied, and a field that only takes effect on a
  // restart is reported rather than silently deferred.
  svr->Get(
      "/api/config", [this](const httplib::Request&, httplib::Response& response) {
        reply_json(response, nlohmann::json::parse(config_snapshot().to_json()));
      });

  svr->Post("/api/config", [this](const httplib::Request& request,
                                  httplib::Response& response) {
    if (config_ == nullptr) {
      reply_text(response, 503, "no configuration");
      return;
    }
    // Validate the whole document first: a refusal names the offending
    // field and changes nothing.
    Config parsed;
    std::string reason;
    if (!parse_config(request.body, &parsed, &reason)) {
      reply_text(response, 400, reason);
      return;
    }

    const Config running = config_snapshot();
    const nlohmann::json wanted = nlohmann::json::parse(parsed.to_json());
    const nlohmann::json current = nlohmann::json::parse(running.to_json());

    std::vector<std::string> applied;
    std::vector<std::string> restart;
    static const char* k_sections[] = {"audio",    "link",   "aes67_daemon",
                                       "egress",   "blocks", "http_addr",
                                       "http_port"};
    for (const char* section : k_sections) {
      if (!wanted.contains(section) || !current.contains(section)) {
        continue;
      }
      if (wanted[section] == current[section]) {
        continue;
      }
      if (std::string(section) == "egress" &&
          parsed.egress.test_signal_channel == running.egress.test_signal_channel) {
        // Only the A/V offset moved, and that one is live.
        applied.push_back("egress.delay_ms");
        continue;
      }
      restart.push_back(section);
    }

    // Apply the live field before writing, so a value the engine refuses
    // never reaches the file. The running config takes the new offset so
    // that file and running state agree on the field that did apply.
    if (!applied.empty()) {
      if (engine() == nullptr ||
          !engine()->set_egress_delay_ms(parsed.egress.delay_ms, &reason)) {
        reply_text(response, 400, reason);
        return;
      }
    }

    if (!config_path_.empty()) {
      if (!write_atomically(config_path_, parsed.to_json(), &reason)) {
        reply_text(response, 500, reason);
        return;
      }
    }

    {
      std::lock_guard<std::mutex> lock(config_mutex_);
      if (!applied.empty()) {
        config_->egress.delay_ms = parsed.egress.delay_ms;
      }
      pending_restart_ = restart;
    }
    log().write(LogLevel::info,
                "control: configuration saved" +
                    (restart.empty() ? std::string()
                                     : " (" + std::to_string(restart.size()) +
                                           " section(s) need a restart)"));
    reply_json(response, {{"ok", true},
                          {"applied", applied},
                          {"restart_required", restart},
                          {"path", config_path_}});
  });

  // ---- AES67: sources, sinks and what is on the wire (ticket 14) ---------
  // The daemon owns discovery, subscription and PTP (decision 11); this is the
  // window onto it. Sources are ours, one per block; sinks are what we subscribed;
  // `discovered` is what the daemon heard on the network, with the SDP a sink
  // needs.
  svr->Get("/api/aes67/status", [this](const httplib::Request&,
                                       httplib::Response& response) {
    nlohmann::json out;
    out["endpoint"] = daemon_ != nullptr ? daemon_->endpoint() : "";
    if (daemon_ == nullptr) {
      reply_json(response, out);
      return;
    }
    std::lock_guard<std::mutex> lock(daemon_mutex_);
    std::string error;
    daemon::PtpStatus ptp;
    out["reachable"] = daemon_->get_ptp_status(&ptp, &error);
    if (out["reachable"].get<bool>()) {
      out["ptp"] = {
          {"status", ptp.status}, {"gmid", ptp.gmid}, {"jitter", ptp.jitter}};
    } else {
      out["error"] = error;
    }

    nlohmann::json sources = nlohmann::json::array();
    daemon::json raw_sources;
    if (daemon_->get_sources(&raw_sources, &error) &&
        raw_sources.contains("sources")) {
      for (const daemon::json& entry : raw_sources["sources"]) {
        sources.push_back({{"id", entry.value("id", -1)},
                           {"name", entry.value("name", std::string())},
                           {"codec", entry.value("codec", std::string())}});
      }
    }
    out["sources"] = sources;

    nlohmann::json sinks = nlohmann::json::array();
    daemon::json raw_sinks;
    if (daemon_->get_sinks(&raw_sinks, &error) && raw_sinks.contains("sinks")) {
      for (const daemon::json& entry : raw_sinks["sinks"]) {
        const int id = entry.value("id", -1);
        daemon::SinkStatus status;
        const bool known = daemon_->get_sink_status(id, &status, &error);
        sinks.push_back({{"id", id},
                         {"in_use", known && status.in_use},
                         {"receiving", known && status.receiving_rtp_packet}});
      }
    }
    out["sinks"] = sinks;

    nlohmann::json discovered = nlohmann::json::array();
    daemon::json raw_discovered;
    if (daemon_->browse_sources("all", &raw_discovered, &error) &&
        raw_discovered.contains("remote_sources")) {
      for (const daemon::json& entry : raw_discovered["remote_sources"]) {
        discovered.push_back({{"name", entry.value("name", std::string())},
                              {"address", entry.value("address", std::string())},
                              {"id", entry.value("id", std::string())},
                              {"sdp", entry.value("sdp", std::string())}});
      }
    }
    out["discovered"] = discovered;
    reply_json(response, out);
  });

  // Subscribe one block's sink to a discovered announcement, or to our own source
  // ("self"). This is the per-site mapping the operator owns, and it is the same
  // `make_block_sink` the commissioning path uses, so the two cannot drift.
  svr->Post("/api/aes67/subscribe", [this](const httplib::Request& request,
                                           httplib::Response& response) {
    if (daemon_ == nullptr) {
      reply_text(response, 503, "no daemon client");
      return;
    }
    std::string error;
    const nlohmann::json body = parse_body(request, &error);
    if (!error.empty()) {
      reply_text(response, 400, error);
      return;
    }
    if (!body.contains("block") || !body["block"].is_number_integer()) {
      reply_text(response, 400, "block: expected a block index");
      return;
    }
    const std::string source = body.value("source", std::string());
    if (source.empty()) {
      reply_text(response, 400, "source: expected a discovered name, or \"self\"");
      return;
    }
    const Config config = config_snapshot();
    const int block_index = body["block"].get<int>();
    if (block_index < 0 || block_index >= static_cast<int>(config.blocks.size())) {
      reply_text(response, 400,
                 "block: expected 0.." + std::to_string(config.blocks.size() - 1));
      return;
    }

    std::lock_guard<std::mutex> lock(daemon_mutex_);
    std::string sdp;
    if (source == "self") {
      if (!daemon_->get_source_sdp(block_index, &sdp, &error)) {
        reply_text(response, 502, error);
        return;
      }
    } else {
      daemon::json discovered;
      if (!daemon_->browse_sources("all", &discovered, &error)) {
        reply_text(response, 502, error);
        return;
      }
      const daemon::json list =
          discovered.value("remote_sources", daemon::json::array());
      for (const daemon::json& entry : list) {
        if (entry.value("name", std::string()) == source) {
          sdp = entry.value("sdp", std::string());
          break;
        }
      }
      if (sdp.empty()) {
        reply_text(response, 400,
                   "source: nothing discovered is named \"" + source + "\"");
        return;
      }
    }
    if (sdp.empty()) {
      reply_text(response, 400, "source: \"" + source + "\" announces no SDP");
      return;
    }

    daemon::json sink;
    if (!daemon::make_block_sink(config.blocks[block_index], sdp, &sink, &error)) {
      reply_text(response, 400, error);
      return;
    }
    if (!daemon_->put_sink(block_index, sink, &error)) {
      reply_text(response, 502, error);
      return;
    }
    log().write(LogLevel::info, "control: subscribed block " +
                                    std::to_string(block_index) + " to \"" +
                                    source + "\"");
    reply_json(response,
               {{"ok", true}, {"block", block_index}, {"source", source}});
  });

  svr->Post("/api/aes67/unsubscribe",
            [this](const httplib::Request& request, httplib::Response& response) {
              if (daemon_ == nullptr) {
                reply_text(response, 503, "no daemon client");
                return;
              }
              std::string error;
              const nlohmann::json body = parse_body(request, &error);
              if (!body.contains("block") || !body["block"].is_number_integer()) {
                reply_text(response, 400, "block: expected a block index");
                return;
              }
              const int block_index = body["block"].get<int>();
              std::lock_guard<std::mutex> lock(daemon_mutex_);
              if (!daemon_->delete_sink(block_index, &error)) {
                reply_text(response, 502, error);
                return;
              }
              log().write(LogLevel::info, "control: unsubscribed block " +
                                              std::to_string(block_index));
              reply_json(response, {{"ok", true}, {"block", block_index}});
            });

  // (Re)publish one source per block. Commissioning does this at start; the button
  // exists for a daemon that was restarted underneath a running appliance, which
  // forgets everything it was told.
  svr->Post("/api/aes67/publish", [this](const httplib::Request&,
                                         httplib::Response& response) {
    if (daemon_ == nullptr) {
      reply_text(response, 503, "no daemon client");
      return;
    }
    const Config config = config_snapshot();
    std::lock_guard<std::mutex> lock(daemon_mutex_);
    std::string error;
    size_t published = 0;
    for (size_t index = 0; index < config.blocks.size(); ++index) {
      if (!daemon_->put_source(
              static_cast<int>(index),
              daemon::make_block_source(config, config.blocks[index]), &error)) {
        reply_text(response, 502, error);
        return;
      }
      ++published;
    }
    log().write(LogLevel::info,
                "control: published " + std::to_string(published) + " sources");
    reply_json(response, {{"ok", true}, {"sources", published}});
  });

  // ---- A/V alignment, live (ticket 14, issue #15) ------------------------
  // The offset moves while audio runs, through the delay line's crossfade, and the
  // impulse is something to line the audio up against. Both are refused with the
  // field named, and neither blocks: the engine posts the request and the receive
  // loop applies it on its next period.
  svr->Post("/api/egress/delay", [this](const httplib::Request& request,
                                        httplib::Response& response) {
    std::string error;
    const nlohmann::json body = parse_body(request, &error);
    if (!error.empty()) {
      reply_text(response, 400, error);
      return;
    }
    if (!body.contains("delay_ms") || !body["delay_ms"].is_number()) {
      reply_text(response, 400, "egress.delay_ms: expected a number");
      return;
    }
    if (engine() == nullptr) {
      reply_text(response, 503, "the engine is not running");
      return;
    }
    if (!engine()->set_egress_delay_ms(body["delay_ms"].get<double>(), &error)) {
      reply_text(response, 400, error);
      return;
    }
    reply_json(response, {{"ok", true}, {"delay_ms", engine()->egress_delay_ms()}});
  });

  svr->Post("/api/egress/test-signal",
            [this](const httplib::Request&, httplib::Response& response) {
              std::string error;
              if (engine() == nullptr) {
                reply_text(response, 503, "the engine is not running");
                return;
              }
              if (!engine()->trigger_test_signal(&error)) {
                reply_text(response, 400, error);
                return;
              }
              reply_json(response, {{"ok", true}});
            });

  // ---- the engine's lifecycle (issue #33) --------------------------------
  // Start, stop and restart of the audio from the page. Restart is what applies a
  // saved configuration change, so the "restart to apply" message has somewhere
  // to go. The process (and this page) stays up throughout.
  const auto lifecycle = [this](httplib::Response& response, const char* action) {
    if (service_ == nullptr) {
      reply_text(response, 503, "no engine service on this build");
      return;
    }
    std::string error;
    if (std::string(action) == "stop") {
      service_->stop();
    } else if (std::string(action) == "start") {
      if (!service_->start(&error)) {
        reply_text(response, 400, error);
        return;
      }
    } else if (!service_->restart(&error)) {
      reply_text(response, 400, error);
      return;
    }
    reply_json(response, {{"ok", true}, {"state", service_->state()}});
  };
  svr->Post("/api/engine/start",
            [lifecycle](const httplib::Request&, httplib::Response& response) {
              lifecycle(response, "start");
            });
  svr->Post("/api/engine/stop",
            [lifecycle](const httplib::Request&, httplib::Response& response) {
              lifecycle(response, "stop");
            });
  svr->Post("/api/engine/restart",
            [lifecycle](const httplib::Request&, httplib::Response& response) {
              lifecycle(response, "restart");
            });

  // Restart the *process* — needed to pick up a new binary, and distinct from an
  // engine restart, which keeps the process (and this page) up. The binary cannot
  // restart itself and keep serving, so it asks to stop: both supervisors relaunch
  // on exit (`launchd` KeepAlive, `systemd` Restart=always) and the page
  // reconnects.
  svr->Post("/api/process/restart", [](const httplib::Request&,
                                       httplib::Response& response) {
    reply_json(response, {{"ok", true},
                          {"detail", "stopping; the supervisor will restart it"}});
    // After the response is written: a raised signal stops this process, so it
    // must not happen before the reply leaves. A short delay on a detached thread
    // does that without blocking the server.
    std::thread([] {
      std::this_thread::sleep_for(std::chrono::milliseconds(300));
      std::raise(SIGTERM);
    }).detach();
  });

  // ---- the AES67 daemon's lifecycle (issue #33) --------------------------
  // A separate systemd service, so this needs a privilege the appliance user does
  // not have by default. install-daemon.sh installs a polkit rule scoped to
  // aes67-daemon.service; without it systemctl refuses and we report exactly that
  // rather than pretending. The command is fixed — the verb is one of three and
  // the unit is ours — so nothing here reaches a shell from the request.
  const auto daemon_lifecycle = [this](httplib::Response& response,
                                       const char* action) {
    if (daemon_ == nullptr) {
      reply_text(response, 404, "this build has no AES67 daemon to control");
      return;
    }
    const std::string verb(action);
    if (verb != "start" && verb != "stop" && verb != "restart") {
      reply_text(response, 400, "unknown daemon action");
      return;
    }
    const std::string command = "systemctl " + verb + " aes67-daemon 2>&1";
    FILE* pipe = popen(command.c_str(), "r");
    if (pipe == nullptr) {
      reply_text(response, 500, "cannot run systemctl");
      return;
    }
    std::string output;
    char buffer[256];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
      output += buffer;
    }
    const int status = pclose(pipe);
    const int code = status == -1 ? -1 : WEXITSTATUS(status);
    if (code != 0) {
      reply_text(response, 403,
                 output.empty() ? "systemctl refused (is the polkit rule "
                                  "installed?)"
                                : output);
      return;
    }
    reply_json(response, {{"ok", true}, {"action", verb}, {"output", output}});
  };
  svr->Post("/api/daemon/start", [daemon_lifecycle](const httplib::Request&,
                                                    httplib::Response& response) {
    daemon_lifecycle(response, "start");
  });
  svr->Post("/api/daemon/stop", [daemon_lifecycle](const httplib::Request&,
                                                   httplib::Response& response) {
    daemon_lifecycle(response, "stop");
  });
  svr->Post("/api/daemon/restart", [daemon_lifecycle](const httplib::Request&,
                                                      httplib::Response& response) {
    daemon_lifecycle(response, "restart");
  });

  // ---- the UI ------------------------------------------------------------
  // The built front end wins when it is there; otherwise the built-in page still
  // loads and says what is wrong. Either way the appliance serves its own UI.
  if (directory_exists(webui_dir_)) {
    svr->set_mount_point("/", webui_dir_);
    const std::string index = webui_dir_ + "/index.html";
    svr->Get("/(.*)",
             [index](const httplib::Request& request, httplib::Response& response) {
               if (request.path.rfind("/api/", 0) == 0) {
                 reply_text(response, 404, "unknown API endpoint " + request.path);
                 return;
               }
               std::ifstream input(index, std::ios::binary);
               if (!input.is_open()) {
                 reply_text(response, 500, "cannot read " + index);
                 return;
               }
               std::stringstream buffer;
               buffer << input.rdbuf();
               response.status = 200;
               response.set_content(buffer.str(), "text/html");
             });
    log().write(LogLevel::info, "control: serving the web UI from " + webui_dir_);
  } else {
    svr->Get("/(.*)",
             [](const httplib::Request& request, httplib::Response& response) {
               if (request.path.rfind("/api/", 0) == 0) {
                 reply_text(response, 404, "unknown API endpoint " + request.path);
                 return;
               }
               response.status = 200;
               response.set_content(k_fallback_page, "text/html");
             });
    log().write(LogLevel::info,
                webui_dir_.empty()
                    ? std::string("control: no web UI directory configured; "
                                  "serving the built-in status page")
                    : "control: no web UI at '" + webui_dir_ +
                          "'; serving the built-in status page");
  }

  svr->set_error_handler([](const httplib::Request&, httplib::Response& response) {
    if (response.status == 404 && response.body.empty()) {
      reply_text(response, 404, "not found");
    }
  });

  svr->set_exception_handler([](const httplib::Request& request,
                                httplib::Response& response,
                                std::exception_ptr error) {
    std::string message = "internal error";
    try {
      std::rethrow_exception(error);
    } catch (const std::exception& ex) {
      message = std::string("internal error: ") + ex.what();
    } catch (...) {
      message = "internal error: unknown exception";
    }
    log().write(LogLevel::error,
                "control: " + request.method + " " + request.path + ": " + message);
    reply_text(response, 500, message);
  });
}

bool ApiServer::start(std::string* error) {
  if (running_) {
    return true;
  }
  server_ = std::make_unique<httplib::Server>();
  register_routes();

  const std::string address = (config_ != nullptr && !config_->http_addr.empty())
                                  ? config_->http_addr
                                  : "0.0.0.0";
  const int port = (config_ != nullptr) ? config_->http_port : 8082;
  if (!server_->bind_to_port(address, port)) {
    if (error != nullptr) {
      *error = "control: cannot bind " + address + ":" + std::to_string(port) +
               " (is another instance already running?)";
    }
    server_.reset();
    return false;
  }

  running_ = true;
  thread_ = std::thread([this, address, port] {
    log().write(LogLevel::info, "control: REST API listening on http://" + address +
                                    ":" + std::to_string(port));
    server_->listen_after_bind();
    running_ = false;
  });
  return true;
}

void ApiServer::stop() {
  if (!running_ && !thread_.joinable()) {
    return;
  }
  if (server_) {
    server_->stop();
  }
  if (thread_.joinable()) {
    thread_.join();
  }
  server_.reset();
  running_ = false;
}

}  // namespace aes67_srt

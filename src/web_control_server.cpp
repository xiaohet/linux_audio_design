#include "web_control_server.h"

#include "audio_config.h"

#include <arpa/inet.h>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <poll.h>
#include <sstream>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>

namespace {
std::string json_string(const std::string& value) {
    std::string result = "\"";
    const char* hex = "0123456789abcdef";
    for(unsigned char c : value) {
        if(c == '"' || c == '\\') { result += '\\'; result += c; }
        else if(c < 32) { result += "\\u00"; result += hex[c >> 4]; result += hex[c & 15]; }
        else result += c;
    }
    return result + '"';
}
std::string url_decode(const std::string& value) {
    std::string result;
    for(size_t i=0; i<value.size(); ++i) {
        if(value[i]=='+') result+=' ';
        else if(value[i]=='%') {
            if(i+2>=value.size()) throw std::runtime_error("Invalid URL encoding");
            const auto digits=value.substr(i+1,2);
            if(digits.find_first_not_of("0123456789abcdefABCDEF")!=std::string::npos)
                throw std::runtime_error("Invalid URL encoding");
            const char c=static_cast<char>(std::stoul(digits,nullptr,16));
            if(!c) throw std::runtime_error("Invalid filename");
            result+=c; i+=2;
        } else result+=value[i];
    }
    return result;
}
}

WebControlServer::WebControlServer(unsigned int port, Processor& processor, AudioTransport& transport)
    : port_(port), processor_(processor), transport_(transport) {
    if(port_ == 0) return;

    listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if(listener_ < 0) throw std::runtime_error("Cannot create web control socket");

    int reuse = 1;
    setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(static_cast<uint16_t>(port_));
    if(::bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        const std::string message = "Cannot bind web control port " +
                                    std::to_string(port_) + ": " + std::strerror(errno);
        ::close(listener_);
        listener_ = -1;
        throw std::runtime_error(message);
    }
    if(::listen(listener_, 8) < 0) {
        ::close(listener_);
        listener_ = -1;
        throw std::runtime_error("Cannot listen on web control port");
    }

    worker_ = std::thread(&WebControlServer::serve, this);
    std::cout << "Browser controls: http://raspberrypi.local:" << port_ << '\n';
}

WebControlServer::~WebControlServer() {
    stop_ = true;
    if(listener_ >= 0) ::close(listener_);
    if(worker_.joinable()) worker_.join();
}

const char* WebControlServer::page() {
    return R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Pi Audio Control</title>
<style>
:root{color-scheme:dark;--bg:#111410;--panel:#1b211b;--line:#344033;--text:#f1f4ec;--muted:#a9b3a5;--accent:#b8f34a;--accent2:#67d7c4}
*{box-sizing:border-box}body{margin:0;min-height:100vh;background:radial-gradient(circle at 80% 0,#26351f 0,transparent 38%),var(--bg);color:var(--text);font:16px/1.45 system-ui,sans-serif}
main{width:min(850px,calc(100% - 24px));margin:auto;padding:22px 0 34px}
header{display:flex;justify-content:space-between;align-items:flex-start;gap:20px;margin-bottom:24px}
h1{font-size:clamp(2rem,8vw,4.6rem);line-height:.9;letter-spacing:-.06em;margin:0}.eyebrow{color:var(--accent);font-size:.75rem;font-weight:800;letter-spacing:.18em;text-transform:uppercase;margin-bottom:12px}
.status{border:1px solid var(--line);border-radius:99px;padding:8px 13px;color:var(--muted);white-space:nowrap}.status.online{color:var(--accent);border-color:#607d36}
.console{display:grid;grid-template-columns:minmax(500px,1fr) 112px;grid-template-rows:auto auto;gap:10px}.card{background:color-mix(in srgb,var(--panel) 92%,transparent);border:1px solid var(--line);border-radius:15px;padding:11px;box-shadow:0 14px 40px #0004}.top-row{grid-column:1;grid-row:1;display:grid;grid-template-columns:minmax(0,1.15fr) minmax(190px,.85fr);gap:10px}
.row{display:flex;align-items:baseline;justify-content:space-between;gap:18px;margin-bottom:15px}.label{font-weight:750}.value{font:700 1.3rem ui-monospace,monospace;color:var(--accent)}
input[type=range]{width:100%;height:34px;margin:0;accent-color:var(--accent);cursor:pointer}small{display:block;color:var(--muted);margin-top:8px}
select{width:100%;background:#111610;color:var(--text);border:1px solid var(--line);border-radius:10px;padding:12px;font:inherit}
.controls-card{display:grid;gap:8px}.controls-card select{padding:7px;font-size:.82rem}.control-row .row{margin-bottom:1px}.control-row .label{font-size:.75rem}.control-row .value{font-size:.8rem}.control-row input{height:20px}.mix-ends{display:flex;justify-content:space-between;color:var(--muted);font-size:.62rem;margin-top:-5px}.routing-block .label{display:block;margin-bottom:4px;font-size:.75rem}.controls-card small{font-size:.61rem;line-height:1.25;margin-top:3px}
.compressor{display:grid;grid-template-columns:1fr;align-content:start;gap:5px}.compressor-title{display:flex;justify-content:space-between;align-items:baseline;margin-bottom:1px}.compressor-title small{margin:0;color:var(--accent2);font-size:.62rem}.comp-control .row{margin-bottom:-2px}.comp-control .label{font-size:.68rem}.comp-control .value{font-size:.68rem}.comp-control input{height:17px}
.vertical-slider{writing-mode:vertical-lr;direction:rtl;width:20px!important;height:175px!important;flex:1}.eq-card{grid-column:1;grid-row:2;height:245px;display:grid;grid-template-columns:repeat(7,1fr);grid-template-rows:35px 1fr;gap:2px}.eq-title{display:flex;grid-column:1/8;justify-content:space-between;align-items:baseline;margin-bottom:1px}.eq-control{display:flex;flex-direction:column;align-items:center;min-width:0}.eq-control .label{font-size:.66rem}.eq-control .value{font-size:.65rem;white-space:nowrap;margin:1px 0 5px}
.utility{display:none}.peak-card{grid-column:2;grid-row:1/3;display:flex;flex-direction:column;align-items:center}.peak-card .row{width:100%;flex-direction:column;align-items:center;gap:1px;text-align:center;margin-bottom:8px}.peak-card .label{font-size:.72rem}.peak-card .value{font-size:.72rem}.meter-stack{display:flex;flex:1;min-height:285px;max-height:500px;align-items:stretch;gap:6px}.meter{width:24px;background:#0c100c;border:1px solid var(--line);border-radius:5px;overflow:hidden;display:flex;align-items:flex-end}.meter span{display:block;width:100%;height:0;background:linear-gradient(0deg,var(--accent2) 0 72%,#f4d35e 86%,#ff5c5c 100%);transition:height 70ms linear}
.scale{display:flex;flex-direction:column;justify-content:space-between;color:var(--muted);font:600 .68rem ui-monospace,monospace;padding:1px 0}
footer{color:var(--muted);font-size:.82rem;margin-top:18px;text-align:center}
.transport{display:flex;gap:8px;align-items:center;flex-wrap:wrap;max-width:470px;margin-top:25px}.transport select{flex:1;min-width:100px;width:170px;padding:8px;font-size:.8rem}.transport button{background:var(--accent);color:#111610;border:0;border-radius:8px;padding:9px 12px;font-weight:750;cursor:pointer}.transport button:disabled,.transport select:disabled{opacity:.4;cursor:default}.transport small{flex-basis:100%;margin:0;font-size:.68rem}.header-left{flex-shrink:0}header{gap:12px;align-items:center;flex-wrap:wrap;margin-bottom:12px}.status{font-size:.72rem;padding:5px 8px}
.header-tools{display:flex;align-items:center;gap:8px;align-self:flex-start;margin-left:auto}.icon-button{display:inline-flex;align-items:center;justify-content:center;width:40px;height:40px;padding:8px!important;background:var(--panel);color:var(--text);border:1px solid var(--line);border-radius:10px;cursor:pointer}.icon-button svg{width:22px;height:22px;fill:currentColor}.icon-button:focus-visible,button:focus-visible{outline:2px solid var(--accent2);outline-offset:3px}#record[aria-pressed=true]{background:#ff6767}
dialog{width:min(480px,calc(100% - 24px));max-height:90vh;overflow:auto;color:var(--text);background:var(--panel);border:1px solid var(--line);border-radius:15px;padding:22px;box-shadow:0 20px 80px #0009}dialog::backdrop{background:#0009}dialog h2{margin:0 0 18px}dialog label{display:block;margin:14px 0 5px;font-size:.85rem;font-weight:700}dialog input[type=text]{width:100%;background:var(--bg);color:var(--text);border:1px solid var(--line);border-radius:10px;padding:10px;font:inherit}dialog select{padding:10px;font-size:.85rem}.dialog-actions,.directory-actions{display:flex;justify-content:flex-end;gap:8px;margin-top:12px}dialog button{padding:9px 13px;border:1px solid var(--line);border-radius:8px;background:var(--bg);color:var(--text);cursor:pointer}dialog button[type=submit]{background:var(--accent);color:#111610;font-weight:750}dialog p,dialog small{font-size:.78rem;color:var(--muted)}#settingsError{color:#ff9090;min-height:1.2em}#directoryBrowser{border:1px solid var(--line);border-radius:10px;padding:10px;margin-top:12px}#directoryLocation{overflow-wrap:anywhere;margin:4px 0 8px}#directoryEntries{display:grid;gap:5px;max-height:150px;overflow:auto}#directoryEntries button{text-align:left;overflow-wrap:anywhere}button:disabled{opacity:.45;cursor:default}
@media(max-width:700px){main{width:min(640px,calc(100% - 12px))}.console{grid-template-columns:minmax(430px,1fr) 94px;overflow-x:auto}.top-row{grid-template-columns:minmax(230px,1.1fr) minmax(180px,.9fr)}.vertical-slider{height:155px!important}.eq-card{height:220px}.meter-stack{min-height:260px}}
</style>
</head>
<body><main>
<header><div class="header-left"><div class="eyebrow">Raspberry Pi · USB Audio</div><h2>Sound shaping.</h2></div><div class="card transport" aria-label="Recording and playback"><button id="record" class="icon-button" aria-label="Start recording" title="Start recording" aria-pressed="false" disabled><svg aria-hidden="true" viewBox="0 0 24 24"><circle cx="12" cy="12" r="7"/></svg></button><select id="wavFiles" aria-label="WAV file" disabled></select><button id="play" class="icon-button" aria-label="Play selected WAV" title="Play selected WAV" disabled><svg aria-hidden="true" viewBox="0 0 24 24"><path d="M7 4v16l15-8z"/></svg></button><small id="mediaStatus" role="status">Loading WAV files…</small></div><div class="header-tools"><div id="status" class="status">Connecting…</div><button id="settingsButton" class="icon-button" aria-label="Recording and playback settings" title="Settings"><svg aria-hidden="true" viewBox="0 0 24 24"><path d="M9 2h6l.7 3 2 .9 2.8-.9 3 5.2-2.2 2.1v2.3l2.2 2.1-3 5.2-2.8-.9-2 .9-.7 3H9l-.7-3-2-.9-2.8.9-3-5.2 2.2-2.1v-2.3L.5 10.2l3-5.2 2.8.9 2-.9z" transform="translate(1 0) scale(.92)"/><circle cx="12" cy="12" r="3.5" fill="var(--panel)"/></svg></button></div></header>
<dialog id="settingsDialog" aria-labelledby="settingsTitle">
  <form id="settingsForm">
    <h2 id="settingsTitle">Recording &amp; playback</h2>
    <label for="recordSource">Record audio from</label>
    <select id="recordSource"><option value="output">Output · after effects and gain</option><option value="input">Input · raw capture before routing and effects</option></select>
    <label for="playbackMode">Play WAV files</label>
    <select id="playbackMode"><option value="direct">Direct · original audio</option><option value="effects">Through effects · current sound shaping</option></select>
    <label for="recordDirectory">Recording directory on Raspberry Pi</label>
    <input id="recordDirectory" type="text" required autocomplete="off" spellcheck="false" aria-describedby="directoryHelp">
    <small id="directoryHelp">WAV files are saved and listed here. Relative paths start at the project folder. Missing folders are created when you save.</small>
    <div class="directory-actions"><button id="browseDirectory" type="button">Browse folders…</button></div>
    <div id="directoryBrowser" hidden><small id="directoryLocation"></small><div id="directoryEntries"></div><div class="directory-actions"><button id="directoryUp" type="button">Up one folder</button></div></div>
    <p>Saved changes apply to the next recording or playback. An active session keeps its original settings. Settings reset when the audio program restarts.</p>
    <p id="settingsError" role="alert"></p>
    <div class="dialog-actions"><button id="cancelSettings" type="button">Cancel</button><button id="saveSettings" type="submit">Save settings</button></div>
  </form>
</dialog>
<section class="console">
  <div class="top-row">
  <div class="card controls-card">
    <div class="routing-block">
      <span class="label">Input routing</span>
      <select id="routing" aria-label="Input routing">
        <option value="input2">Input 2 to both speakers</option>
        <option value="input1">Input 1 to both speakers</option>
        <option value="mix">Mix inputs to both speakers</option>
        <option value="stereo">Preserve stereo channels</option>
      </select>
    </div>
    <div class="control-row">
    <div class="row"><span class="label">Output gain</span><span id="gainValue" class="value">−1.9 dB</span></div>
    <input id="gain" type="range" min="-60" max="12" step="0.1" value="-1.9" aria-label="Output gain in decibels">
    </div>
	    <div class="control-row">
	      <div class="row"><span class="label">Dry / wet</span><span id="dryWetValue" class="value">100% wet</span></div>
	      <input id="dryWet" type="range" min="0" max="100" step="1" value="100" aria-label="Dry and wet effects mix">
	      <div class="mix-ends"><span>Dry</span><span>Wet</span></div>
	    </div>
	    <div class="control-row" id="noiseBlock">
	      <div class="row"><span class="label">Noise suppression</span><span id="noiseValue" class="value">Off</span></div>
	      <input id="noise" type="range" min="0" max="100" step="1" value="0" aria-label="DeepFilterNet noise suppression strength">
	      <div class="mix-ends"><span>Off</span><span>Full</span></div>
	      <small id="dfStatus">DeepFilterNet status loading...</small>
	    </div>
  </div>
    <div class="card compressor">
      <div class="compressor-title"><span class="label">Compressor</span><small id="compReduction">0.0 dB reduction</small></div>
      <div class="comp-control"><div class="row"><span class="label">Threshold</span><span id="compThresholdValue" class="value">-18 dB</span></div><input id="compThreshold" type="range" min="-60" max="0" step="0.5" value="-18" aria-label="Compressor threshold in decibels"></div>
      <div class="comp-control"><div class="row"><span class="label">Ratio</span><span id="compRatioValue" class="value">1.0:1</span></div><input id="compRatio" type="range" min="1" max="20" step="0.1" value="1" aria-label="Compressor ratio"></div>
      <div class="comp-control"><div class="row"><span class="label">Attack</span><span id="compAttackValue" class="value">10 ms</span></div><input id="compAttack" type="range" min="0.1" max="200" step="0.1" value="10" aria-label="Compressor attack in milliseconds"></div>
      <div class="comp-control"><div class="row"><span class="label">Release</span><span id="compReleaseValue" class="value">100 ms</span></div><input id="compRelease" type="range" min="10" max="2000" step="1" value="100" aria-label="Compressor release in milliseconds"></div>
      <div class="comp-control"><div class="row"><span class="label">Makeup</span><span id="compMakeupValue" class="value">0.0 dB</span></div><input id="compMakeup" type="range" min="0" max="24" step="0.1" value="0" aria-label="Compressor makeup gain in decibels"></div>
    </div>
  </div>
  <div class="card eq-card">
    <div class="eq-title"><span class="label">EQ</span></div>
    <div class="eq-control">
      <span class="label">80 Hz</span><span id="eq0Value" class="value">0.0 dB</span>
      <input id="eq0" class="vertical-slider eq-gain" type="range" min="-18" max="18" step="0.1" value="0" aria-label="80 Hz gain">
    </div>
    <div class="eq-control">
      <span class="label">160 Hz</span><span id="eq1Value" class="value">0.0 dB</span>
      <input id="eq1" class="vertical-slider eq-gain" type="range" min="-18" max="18" step="0.1" value="0" aria-label="160 Hz gain">
    </div>
    <div class="eq-control">
      <span class="label">320 Hz</span><span id="eq2Value" class="value">0.0 dB</span>
      <input id="eq2" class="vertical-slider eq-gain" type="range" min="-18" max="18" step="0.1" value="0" aria-label="320 Hz gain">
    </div>
    <div class="eq-control">
      <span class="label">640 Hz</span><span id="eq3Value" class="value">0.0 dB</span>
      <input id="eq3" class="vertical-slider eq-gain" type="range" min="-18" max="18" step="0.1" value="0" aria-label="640 Hz gain">
    </div>
    <div class="eq-control">
      <span class="label">1.28 kHz</span><span id="eq4Value" class="value">0.0 dB</span>
      <input id="eq4" class="vertical-slider eq-gain" type="range" min="-18" max="18" step="0.1" value="0" aria-label="1280 Hz gain">
    </div>
    <div class="eq-control">
      <span class="label">2.56 kHz</span><span id="eq5Value" class="value">0.0 dB</span>
      <input id="eq5" class="vertical-slider eq-gain" type="range" min="-18" max="18" step="0.1" value="0" aria-label="2560 Hz gain">
    </div>
    <div class="eq-control">
      <span class="label">5.12 kHz</span><span id="eq6Value" class="value">0.0 dB</span>
      <input id="eq6" class="vertical-slider eq-gain" type="range" min="-18" max="18" step="0.1" value="0" aria-label="5120 Hz gain">
    </div>
  </div>
  <div class="utility">
    <div class="card routing-panel routing-block">
      <span class="label">Input routing</span>
      <select id="routing-unused" aria-label="Input routing">
        <option value="input2">Input 2 → both speakers</option>
        <option value="input1">Input 1 → both speakers</option>
        <option value="mix">Mix inputs → both speakers</option>
        <option value="stereo">Preserve stereo channels</option>
      </select>
    </div>
  </div>
  <div class="card peak-card">
    <div class="row"><span class="label">Output peak</span><span id="peakValue" class="value">&lt;-60dBFS</span></div>
    <div class="meter-stack">
      <div class="meter" role="meter" aria-label="Output peak level" aria-valuemin="-60" aria-valuemax="0" aria-valuenow="-60"><span id="peakBar"></span></div>
      <div class="scale"><span>0</span><span>−6</span><span>−18</span><span>−36</span><span>−60</span></div>
    </div>
  </div>
</section>
<footer>Controls update the running audio engine immediately · raspberrypi.local</footer>
</main>
<script>
const $=id=>document.getElementById(id);
let media={recording:false,playing:false},mediaPending=false,mediaActionError="";
function showMedia(s){media=s;const label=s.recording?'Stop recording':'Start recording';$('record').innerHTML='<svg aria-hidden="true" viewBox="0 0 24 24">'+(s.recording?'<rect x="5" y="5" width="14" height="14" rx="2"/>':'<circle cx="12" cy="12" r="7"/>')+'</svg>';$('record').title=label;$('record').setAttribute('aria-label',label);$('record').setAttribute('aria-pressed',String(!!s.recording));$('record').disabled=mediaPending||s.playing;$('play').disabled=mediaPending||s.recording||s.playing||!$('wavFiles').value;$('wavFiles').disabled=mediaPending||s.recording||s.playing;$('mediaStatus').textContent=mediaActionError||s.mediaError||(s.recording?'Recording '+(s.activeRecordInput?'input':'output')+' · '+s.mediaFile:s.playing?'Playing '+(s.activePlaybackEffects?'through effects':'direct')+' · '+s.mediaFile:'Ready · WAV playback mixes with live input')}
async function loadFiles(){try{const r=await fetch('/api/files');if(!r.ok)throw Error('Cannot list WAV files');const names=await r.json(),selected=$('wavFiles').value;$('wavFiles').replaceChildren(...names.map(name=>new Option(name,name)));if(names.includes(selected))$('wavFiles').value=selected;showMedia(media)}catch(e){$('mediaStatus').textContent=e.message}}
async function transport(action){mediaActionError="";mediaPending=true;showMedia(media);try{const q=new URLSearchParams({action,file:$('wavFiles').value}),r=await fetch('/api/transport?'+q),s=await r.json();if(!r.ok)throw Error(s.error);mediaPending=false;showMedia(s);await loadFiles()}catch(e){mediaPending=false;mediaActionError=e.message;showMedia(media)}}
$('record').onclick=()=>transport(media.recording?'stop':'record');$('play').onclick=()=>transport('play');$('wavFiles').onchange=()=>showMedia(media);
let directoryParent='';
$('settingsButton').onclick=async()=>{const dialog=$('settingsDialog');$('settingsError').textContent='';$('directoryBrowser').hidden=true;$('saveSettings').disabled=true;dialog.showModal();try{const r=await fetch('/api/settings');const s=await r.json();if(!r.ok)throw Error(s.error||'Cannot load settings');$('recordSource').value=s.settings.recordSource;$('playbackMode').value=s.settings.playbackMode;$('recordDirectory').value=s.settings.directory;$('saveSettings').disabled=false}catch(e){$('settingsError').textContent=e.message}};
$('cancelSettings').onclick=()=>$('settingsDialog').close();
async function browseDirectory(path){$('settingsError').textContent='';try{const r=await fetch('/api/directories?'+new URLSearchParams({path})),s=await r.json();if(!r.ok)throw Error(s.error);directoryParent=s.parent;$('recordDirectory').value=s.directory;$('directoryLocation').textContent=s.directory;$('directoryBrowser').hidden=false;$('directoryUp').disabled=!s.parent||s.parent===s.directory;const entries=s.children.map(name=>{const button=document.createElement('button');button.type='button';button.textContent=name;button.onclick=()=>browseDirectory(s.directory+'/'+name);return button});if(!entries.length){const empty=document.createElement('small');empty.textContent='No subfolders';entries.push(empty)}$('directoryEntries').replaceChildren(...entries)}catch(e){$('settingsError').textContent=e.message}}
$('browseDirectory').onclick=()=>browseDirectory($('recordDirectory').value);$('directoryUp').onclick=()=>browseDirectory(directoryParent);
$('settingsForm').onsubmit=async event=>{event.preventDefault();$('saveSettings').disabled=true;$('settingsError').textContent='';try{const q=new URLSearchParams({recordSource:$('recordSource').value,playbackMode:$('playbackMode').value,directory:$('recordDirectory').value}),r=await fetch('/api/settings?'+q),s=await r.json();if(!r.ok)throw Error(s.error);mediaActionError='';showMedia(s);await loadFiles();$('settingsDialog').close()}catch(e){$('settingsError').textContent=e.message}finally{$('saveSettings').disabled=false}};
	const gain=$('gain'),dryWet=$('dryWet'),noise=$('noise'),eqGains=[0,1,2,3,4,5,6].map(i=>$('eq'+i)),compThreshold=$('compThreshold'),compRatio=$('compRatio'),compAttack=$('compAttack'),compRelease=$('compRelease'),compMakeup=$('compMakeup'),routing=$('routing'),status=$('status'),peakBar=$('peakBar'),peakValue=$('peakValue');
let timer;
	function labels(){$('gainValue').textContent=(+gain.value).toFixed(1)+' dB';$('dryWetValue').textContent=Math.round(+dryWet.value)+'% wet';$('noiseValue').textContent=+noise.value===0?'Off':Math.round(+noise.value)+'%';eqGains.forEach((control,i)=>$('eq'+i+'Value').textContent=(+control.value).toFixed(1)+' dB');$('compThresholdValue').textContent=(+compThreshold.value).toFixed(1)+' dB';$('compRatioValue').textContent=(+compRatio.value).toFixed(1)+':1';$('compAttackValue').textContent=(+compAttack.value).toFixed(1)+' ms';$('compReleaseValue').textContent=Math.round(+compRelease.value)+' ms';$('compMakeupValue').textContent=(+compMakeup.value).toFixed(1)+' dB'}
	function showPeak(linear){const db=linear>0?20*Math.log10(linear):-120,p=Math.max(0,Math.min(100,(db+60)/60*100));peakBar.style.height=p+'%';peakValue.textContent=db<=-60?'<-60dBFS':db.toFixed(1)+'dBFS';peakBar.parentElement.setAttribute('aria-valuenow',Math.max(-60,db).toFixed(1))}
	function showDf(s){$('dfStatus').textContent=s.deepFilterAvailable?'DF '+s.deepFilterMeanMs.toFixed(2)+' ms avg · '+s.deepFilterDeadlineMisses+' late · '+s.deepFilterOutputUnderruns+' fallback periods · '+s.deepFilterStaleOutputSamples+' stale samples dropped':'DeepFilterNet unavailable'}
async function send(){
  clearTimeout(timer);
	  const values={gainDb:gain.value,dryWet:(+dryWet.value/100),noiseSuppression:(+noise.value/100),compThreshold:compThreshold.value,compRatio:compRatio.value,compAttack:compAttack.value,compRelease:compRelease.value,compMakeup:compMakeup.value,routing:routing.value};eqGains.forEach((control,i)=>values['eq'+i]=control.value);const q=new URLSearchParams(values);
  try{const r=await fetch('/api/set?'+q);if(!r.ok)throw Error();status.textContent='Live';status.className='status online'}catch{status.textContent='Disconnected';status.className='status'}
}
function changed(){labels();clearTimeout(timer);timer=setTimeout(send,45)}
	[gain,dryWet,noise,compThreshold,compRatio,compAttack,compRelease,compMakeup,...eqGains].forEach(x=>x.addEventListener('input',changed));routing.addEventListener('change',send);
async function load(){
	  try{const s=await(await fetch('/api/state')).json();gain.value=s.gainDb;dryWet.value=s.dryWet*100;noise.value=s.noiseSuppression*100;noise.disabled=!s.deepFilterAvailable;$('noiseBlock').style.opacity=s.deepFilterAvailable?'1':'.45';eqGains.forEach((control,i)=>control.value=s.eqGains[i]);compThreshold.value=s.compThreshold;compRatio.value=s.compRatio;compAttack.value=s.compAttack;compRelease.value=s.compRelease;compMakeup.value=s.compMakeup;routing.value=s.routing;showPeak(s.peak);showDf(s);$('compReduction').textContent=(-s.compReduction).toFixed(1)+' dB reduction';labels();status.textContent='Live';status.className='status online'}
  catch{status.textContent='Disconnected';status.className='status'}
}
		async function meter(){try{const s=await(await fetch('/api/state')).json();showMedia(s);gain.value=s.gainDb;dryWet.value=s.dryWet*100;$('gainValue').textContent=(+gain.value).toFixed(1)+' dB';$('dryWetValue').textContent=Math.round(+dryWet.value)+'% wet';showPeak(s.peak);$('compReduction').textContent=(-s.compReduction).toFixed(1)+' dB reduction'}catch{$('record').disabled=true;$('play').disabled=true}}
load();loadFiles();setInterval(loadFiles,5000);setInterval(load,5000);setInterval(meter,90);
</script></body></html>)HTML";
}

std::map<std::string, std::string>
WebControlServer::query_parameters(const std::string& target) {
    std::map<std::string, std::string> result;
    const size_t question = target.find('?');
    if(question == std::string::npos) return result;
    std::istringstream pairs(target.substr(question + 1));
    std::string pair;
    while(std::getline(pairs, pair, '&')) {
        const size_t equals = pair.find('=');
        if(equals != std::string::npos)
            result[pair.substr(0, equals)] = url_decode(pair.substr(equals + 1));
    }
    return result;
}

std::string WebControlServer::state_json() const {
    const auto state = processor_.snapshot();
    const float gainDb = state.gain > 0 ? 20.0f * std::log10(state.gain) : -120.0f;
    std::ostringstream json;
    const auto media = transport_.state();
    json << "{\"recording\":" << (media.recording ? "true" : "false")
         << ",\"playing\":" << (media.playing ? "true" : "false")
         << ",\"mediaFile\":" << json_string(media.file)
         << ",\"mediaError\":" << json_string(media.error)
         << ",\"activeRecordInput\":" << (media.activeRecordInput ? "true" : "false")
         << ",\"activePlaybackEffects\":" << (media.activePlaybackEffects ? "true" : "false")
         << ",\"activeDirectory\":" << json_string(media.activeDirectory)
         << ",\"settings\":{\"recordSource\":" << json_string(media.settings.recordInput ? "input" : "output")
         << ",\"playbackMode\":" << json_string(media.settings.playbackEffects ? "effects" : "direct")
         << ",\"directory\":" << json_string(media.settings.directory.string()) << '}'
         << ",\"gainDb\":" << gainDb << ",\"eqGains\":[";
    for(size_t band = 0; band < Processor::EqBandCount; ++band) {
        if(band > 0) json << ',';
        json << state.eqGainsDb[band];
    }
    json << "],\"peak\":" << state.peak
         << ",\"input1Peak\":" << state.input1Peak
         << ",\"input2Peak\":" << state.input2Peak
         << ",\"gateDb\":" << state.noiseGateDb
         << ",\"gateOpen\":" << (state.gateOpen ? "true" : "false")
         << ",\"dryWet\":" << state.dryWet
         << ",\"compThreshold\":" << state.compressorThresholdDb
         << ",\"compRatio\":" << state.compressorRatio
         << ",\"compAttack\":" << state.compressorAttackMs
         << ",\"compRelease\":" << state.compressorReleaseMs
         << ",\"compMakeup\":" << state.compressorMakeupDb
	         << ",\"compReduction\":" << state.compressorGainReductionDb
	         << ",\"deepFilterAvailable\":"
	         << (state.deepFilterAvailable ? "true" : "false")
	         << ",\"noiseSuppression\":" << state.noiseSuppression
	         << ",\"deepFilterMeanMs\":" << state.deepFilterMeanMs
	         << ",\"deepFilterMaximumMs\":" << state.deepFilterMaximumMs
	         << ",\"deepFilterFrames\":" << state.deepFilterFrames
	         << ",\"deepFilterDeadlineMisses\":"
	         << state.deepFilterDeadlineMisses
	         << ",\"deepFilterInputOverruns\":"
	         << state.deepFilterInputOverruns
	         << ",\"deepFilterOutputUnderruns\":"
	         << state.deepFilterOutputUnderruns
	         << ",\"deepFilterStaleOutputSamples\":"
	         << state.deepFilterStaleOutputSamples
         << ",\"routing\":\"" << routing_name(state.routing) << "\"}";
    return json.str();
}

void WebControlServer::apply(const std::map<std::string, std::string>& parameters) {
    auto value = parameters.find("gainDb");
    if(value != parameters.end()) {
        const float gainDb = std::stof(value->second);
        if(gainDb < -120.0f || gainDb > 20.0f) throw std::runtime_error("gain");
        processor_.set_gain_db(gainDb);
    }
    for(size_t band = 0; band < Processor::EqBandCount; ++band) {
        value = parameters.find("eq" + std::to_string(band));
        if(value != parameters.end()) {
            const float gain = std::stof(value->second);
            if(gain < -18.0f || gain > 18.0f)
                throw std::runtime_error("eq");
            processor_.set_eq_gain_db(band, gain);
        }
    }
    value = parameters.find("routing");
    if(value != parameters.end()) processor_.set_routing(parse_routing(value->second));
    value = parameters.find("gateDb");
    if(value != parameters.end()) {
        const float threshold = std::stof(value->second);
        if(threshold < -120.0f || threshold > -10.0f)
            throw std::runtime_error("gate");
        processor_.set_noise_gate_db(threshold);
    }
	    value = parameters.find("dryWet");
    if(value != parameters.end()) {
        const float dryWet = std::stof(value->second);
        if(dryWet < 0.0f || dryWet > 1.0f)
            throw std::runtime_error("dryWet");
	        processor_.set_dry_wet(dryWet);
	    }
	    value = parameters.find("noiseSuppression");
	    if(value != parameters.end()) {
	        const float strength = std::stof(value->second);
	        if(strength < 0.0f || strength > 1.0f)
	            throw std::runtime_error("noiseSuppression");
	        processor_.set_noise_suppression(strength);
	    }
    const auto state = processor_.snapshot();
    float threshold = state.compressorThresholdDb;
    float ratio = state.compressorRatio;
    float attack = state.compressorAttackMs;
    float release = state.compressorReleaseMs;
    float makeup = state.compressorMakeupDb;
    bool compressorChanged = false;
    const auto readCompressor = [&](const char* name, float& target,
                                    float minimum, float maximum) {
        const auto parameter = parameters.find(name);
        if(parameter == parameters.end()) return;
        target = std::stof(parameter->second);
        if(!std::isfinite(target) || target < minimum || target > maximum)
            throw std::runtime_error(name);
        compressorChanged = true;
    };
    readCompressor("compThreshold", threshold, -60.0f, 0.0f);
    readCompressor("compRatio", ratio, 1.0f, 20.0f);
    readCompressor("compAttack", attack, 0.1f, 200.0f);
    readCompressor("compRelease", release, 10.0f, 2000.0f);
    readCompressor("compMakeup", makeup, 0.0f, 24.0f);
    if(compressorChanged)
        processor_.set_compressor(threshold, ratio, attack, release, makeup);
}

void WebControlServer::send_response(int client, const char* status,
                                     const char* contentType,
                                     const std::string& body) {
    std::ostringstream response;
    response << "HTTP/1.1 " << status << "\r\n"
             << "Content-Type: " << contentType << "\r\n"
             << "Content-Length: " << body.size() << "\r\n"
             << "Cache-Control: no-store\r\n"
             << "Connection: close\r\n"
             << "X-Content-Type-Options: nosniff\r\n\r\n"
             << body;
    const std::string data = response.str();
    size_t sent = 0;
    while(sent < data.size()) {
        const ssize_t result =
            ::send(client, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if(result <= 0) break;
        sent += static_cast<size_t>(result);
    }
}

void WebControlServer::handle(int client) {
    timeval timeout{1, 0};
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    std::array<char, 8192> buffer{};
    const ssize_t count = ::recv(client, buffer.data(), buffer.size() - 1, 0);
    if(count <= 0) return;

    std::istringstream request(std::string(buffer.data(), static_cast<size_t>(count)));
    std::string method;
    std::string target;
    std::string version;
    request >> method >> target >> version;
    if(method != "GET") {
        send_response(client, "405 Method Not Allowed", "text/plain", "GET required");
    } else if(target == "/") {
        send_response(client, "200 OK", "text/html; charset=utf-8", page());
    } else if(target == "/api/state") {
        send_response(client, "200 OK", "application/json", state_json());
    } else if(target == "/api/settings" || target.rfind("/api/settings?", 0) == 0 || target.rfind("/api/directories?", 0) == 0) {
        try {
            if(target.rfind("/api/directories?", 0) == 0) {
                const auto parameters = query_parameters(target);
                const auto directories = transport_.directories(parameters.at("path"));
                std::string body = "{\"directory\":"+json_string(directories.directory)+",\"parent\":"+json_string(directories.parent)+",\"children\":[";
                bool first=true;
                for(const auto& child : directories.children) {
                    if(!first) body+=',';
                    first=false; body+=json_string(child);
                }
                send_response(client,"200 OK","application/json",body+"]}");
            } else {
                if(target != "/api/settings") {
                    const auto parameters = query_parameters(target);
                    const auto source = parameters.at("recordSource"), mode = parameters.at("playbackMode");
                    if((source!="input" && source!="output") || (mode!="direct" && mode!="effects"))
                        throw std::runtime_error("Invalid recording source or playback mode");
                    transport_.set_settings({source=="input",mode=="effects",parameters.at("directory")});
                }
                send_response(client,"200 OK","application/json",state_json());
            }
        } catch(const std::exception& error) {
            send_response(client,"400 Bad Request","application/json","{\"error\":"+json_string(error.what())+"}");
        }
    } else if(target == "/api/files" || target.rfind("/api/transport?", 0) == 0) {
        try {
            if(target == "/api/files") {
                std::string body = "[";
                for(const auto& name : transport_.files()) {
                    if(body.size()>1) body += ',';
                    body += json_string(name);
                }
                send_response(client,"200 OK","application/json",body+"]");
            } else {
                const auto parameters=query_parameters(target);
                const auto action=parameters.at("action");
                if(action=="record") transport_.record();
                else if(action=="stop") transport_.stop_recording();
                else if(action=="play") transport_.play(parameters.at("file"));
                else if(action=="playstop") transport_.stop_playback();
                else throw std::runtime_error("Unknown transport action");
                send_response(client,"200 OK","application/json",state_json());
            }
        } catch(const std::exception& error) {
            send_response(client,"400 Bad Request","application/json","{\"error\":"+json_string(error.what())+"}");
        }
    } else if(target.rfind("/api/set?", 0) == 0) {
        try {
            apply(query_parameters(target));
            send_response(client, "200 OK", "application/json", state_json());
        } catch(...) {
            send_response(client, "400 Bad Request", "application/json",
                          "{\"error\":\"Invalid control value\"}");
        }
    } else {
        send_response(client, "404 Not Found", "text/plain", "Not found");
    }
}

void WebControlServer::serve() {
    while(!stop_) {
        pollfd descriptor{listener_, POLLIN, 0};
        const int ready = poll(&descriptor, 1, 250);
        if(ready <= 0 || !(descriptor.revents & POLLIN)) continue;
        const int client = ::accept(listener_, nullptr, nullptr);
        if(client < 0) continue;
        handle(client);
        ::close(client);
    }
}

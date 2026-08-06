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

WebControlServer::WebControlServer(unsigned int port, Processor& processor)
    : port_(port), processor_(processor) {
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
main{width:min(900px,calc(100% - 28px));margin:auto;padding:30px 0 44px}
header{display:flex;justify-content:space-between;align-items:flex-start;gap:20px;margin-bottom:24px}
h1{font-size:clamp(2rem,8vw,4.6rem);line-height:.9;letter-spacing:-.06em;margin:0}.eyebrow{color:var(--accent);font-size:.75rem;font-weight:800;letter-spacing:.18em;text-transform:uppercase;margin-bottom:12px}
.status{border:1px solid var(--line);border-radius:99px;padding:8px 13px;color:var(--muted);white-space:nowrap}.status.online{color:var(--accent);border-color:#607d36}
.console{display:grid;grid-template-columns:minmax(520px,1fr) 140px;grid-template-rows:auto auto;gap:12px}.card{background:color-mix(in srgb,var(--panel) 92%,transparent);border:1px solid var(--line);border-radius:18px;padding:14px;box-shadow:0 18px 50px #0004}
.row{display:flex;align-items:baseline;justify-content:space-between;gap:18px;margin-bottom:15px}.label{font-weight:750}.value{font:700 1.3rem ui-monospace,monospace;color:var(--accent)}
input[type=range]{width:100%;height:34px;margin:0;accent-color:var(--accent);cursor:pointer}small{display:block;color:var(--muted);margin-top:8px}
select{width:100%;background:#111610;color:var(--text);border:1px solid var(--line);border-radius:10px;padding:12px;font:inherit}
.controls-card{display:grid;gap:13px}.control-row .row{margin-bottom:4px}.control-row input{height:26px}.mix-ends{display:flex;justify-content:space-between;color:var(--muted);font-size:.68rem;margin-top:-4px}.routing-block .label{display:block;margin-bottom:7px}
.compressor{border-top:1px solid var(--line);padding-top:11px;display:grid;grid-template-columns:repeat(2,1fr);gap:8px 14px}.compressor-title{grid-column:1/-1;display:flex;justify-content:space-between;align-items:baseline}.compressor-title small{margin:0;color:var(--accent2)}.comp-control .row{margin-bottom:0}.comp-control .label{font-size:.72rem}.comp-control .value{font-size:.75rem}.comp-control input{height:22px}
.vertical-slider{writing-mode:vertical-lr;direction:rtl;width:24px!important;height:205px!important;flex:1}.eq-card{height:285px;display:grid;grid-template-columns:repeat(7,1fr);gap:3px}.eq-control{display:flex;flex-direction:column;align-items:center;min-width:0}.eq-control .label{font-size:.7rem}.eq-control .value{font-size:.7rem;white-space:nowrap;margin:2px 0 7px}
.utility{display:none}.peak-card{grid-column:2;grid-row:1/3;display:flex;flex-direction:column;align-items:center}.peak-card .row{width:100%;flex-direction:column;align-items:center;gap:2px;text-align:center}.meter-stack{display:flex;flex:1;min-height:360px;align-items:stretch;gap:8px}.meter{width:30px;background:#0c100c;border:1px solid var(--line);border-radius:5px;overflow:hidden;display:flex;align-items:flex-end}.meter span{display:block;width:100%;height:0;background:linear-gradient(0deg,var(--accent2) 0 72%,#f4d35e 86%,#ff5c5c 100%);transition:height 70ms linear}
.scale{display:flex;flex-direction:column;justify-content:space-between;color:var(--muted);font:600 .68rem ui-monospace,monospace;padding:1px 0}
footer{color:var(--muted);font-size:.82rem;margin-top:18px;text-align:center}
@media(max-width:700px){main{width:min(640px,calc(100% - 16px))}.console{grid-template-columns:minmax(450px,1fr) 120px;overflow-x:auto}.vertical-slider{height:180px!important}.eq-card{height:255px}.meter-stack{min-height:330px}}
</style>
</head>
<body><main>
<header><div><div class="eyebrow">Raspberry Pi · USB Audio</div><h1>Sound<br>shaping.</h1></div><div id="status" class="status">Connecting…</div></header>
<section class="console">
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
	    </div>
    <div class="compressor">
      <div class="compressor-title"><span class="label">Compressor</span><small id="compReduction">0.0 dB reduction</small></div>
      <div class="comp-control"><div class="row"><span class="label">Threshold</span><span id="compThresholdValue" class="value">-18 dB</span></div><input id="compThreshold" type="range" min="-60" max="0" step="0.5" value="-18" aria-label="Compressor threshold in decibels"></div>
      <div class="comp-control"><div class="row"><span class="label">Ratio</span><span id="compRatioValue" class="value">1.0:1</span></div><input id="compRatio" type="range" min="1" max="20" step="0.1" value="1" aria-label="Compressor ratio"></div>
      <div class="comp-control"><div class="row"><span class="label">Attack</span><span id="compAttackValue" class="value">10 ms</span></div><input id="compAttack" type="range" min="0.1" max="200" step="0.1" value="10" aria-label="Compressor attack in milliseconds"></div>
      <div class="comp-control"><div class="row"><span class="label">Release</span><span id="compReleaseValue" class="value">100 ms</span></div><input id="compRelease" type="range" min="10" max="2000" step="1" value="100" aria-label="Compressor release in milliseconds"></div>
      <div class="comp-control"><div class="row"><span class="label">Makeup</span><span id="compMakeupValue" class="value">0.0 dB</span></div><input id="compMakeup" type="range" min="0" max="24" step="0.1" value="0" aria-label="Compressor makeup gain in decibels"></div>
    </div>
  </div>
  <div class="card eq-card">
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
	const gain=$('gain'),dryWet=$('dryWet'),noise=$('noise'),eqGains=[0,1,2,3,4,5,6].map(i=>$('eq'+i)),compThreshold=$('compThreshold'),compRatio=$('compRatio'),compAttack=$('compAttack'),compRelease=$('compRelease'),compMakeup=$('compMakeup'),routing=$('routing'),status=$('status'),peakBar=$('peakBar'),peakValue=$('peakValue');
let timer;
	function labels(){$('gainValue').textContent=(+gain.value).toFixed(1)+' dB';$('dryWetValue').textContent=Math.round(+dryWet.value)+'% wet';$('noiseValue').textContent=+noise.value===0?'Off':Math.round(+noise.value)+'%';eqGains.forEach((control,i)=>$('eq'+i+'Value').textContent=(+control.value).toFixed(1)+' dB');$('compThresholdValue').textContent=(+compThreshold.value).toFixed(1)+' dB';$('compRatioValue').textContent=(+compRatio.value).toFixed(1)+':1';$('compAttackValue').textContent=(+compAttack.value).toFixed(1)+' ms';$('compReleaseValue').textContent=Math.round(+compRelease.value)+' ms';$('compMakeupValue').textContent=(+compMakeup.value).toFixed(1)+' dB'}
function showPeak(linear){const db=linear>0?20*Math.log10(linear):-120,p=Math.max(0,Math.min(100,(db+60)/60*100));peakBar.style.height=p+'%';peakValue.textContent=db<=-60?'<-60dBFS':db.toFixed(1)+'dBFS';peakBar.parentElement.setAttribute('aria-valuenow',Math.max(-60,db).toFixed(1))}
async function send(){
  clearTimeout(timer);
	  const values={gainDb:gain.value,dryWet:(+dryWet.value/100),noiseSuppression:(+noise.value/100),compThreshold:compThreshold.value,compRatio:compRatio.value,compAttack:compAttack.value,compRelease:compRelease.value,compMakeup:compMakeup.value,routing:routing.value};eqGains.forEach((control,i)=>values['eq'+i]=control.value);const q=new URLSearchParams(values);
  try{const r=await fetch('/api/set?'+q);if(!r.ok)throw Error();status.textContent='Live';status.className='status online'}catch{status.textContent='Disconnected';status.className='status'}
}
function changed(){labels();clearTimeout(timer);timer=setTimeout(send,45)}
	[gain,dryWet,noise,compThreshold,compRatio,compAttack,compRelease,compMakeup,...eqGains].forEach(x=>x.addEventListener('input',changed));routing.addEventListener('change',send);
async function load(){
	  try{const s=await(await fetch('/api/state')).json();gain.value=s.gainDb;dryWet.value=s.dryWet*100;noise.value=s.noiseSuppression*100;noise.disabled=!s.deepFilterAvailable;$('noiseBlock').style.opacity=s.deepFilterAvailable?'1':'.45';eqGains.forEach((control,i)=>control.value=s.eqGains[i]);compThreshold.value=s.compThreshold;compRatio.value=s.compRatio;compAttack.value=s.compAttack;compRelease.value=s.compRelease;compMakeup.value=s.compMakeup;routing.value=s.routing;showPeak(s.peak);$('compReduction').textContent=(-s.compReduction).toFixed(1)+' dB reduction';labels();status.textContent='Live';status.className='status online'}
  catch{status.textContent='Disconnected';status.className='status'}
}
async function meter(){try{const s=await(await fetch('/api/state')).json();showPeak(s.peak);$('compReduction').textContent=(-s.compReduction).toFixed(1)+' dB reduction'}catch{}}
load();setInterval(load,5000);setInterval(meter,90);
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
            result[pair.substr(0, equals)] = pair.substr(equals + 1);
    }
    return result;
}

std::string WebControlServer::state_json() const {
    const auto state = processor_.snapshot();
    const float gainDb = state.gain > 0 ? 20.0f * std::log10(state.gain) : -120.0f;
    std::ostringstream json;
    json << "{\"gainDb\":" << gainDb << ",\"eqGains\":[";
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

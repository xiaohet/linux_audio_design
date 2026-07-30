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

WebControlServer::WebControlServer(unsigned int port, unsigned int sampleRate,
                                   Processor& processor)
    : port_(port), sampleRate_(sampleRate), processor_(processor) {
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
main{width:min(1120px,calc(100% - 28px));margin:auto;padding:34px 0 50px}
header{display:flex;justify-content:space-between;align-items:flex-start;gap:20px;margin-bottom:24px}
h1{font-size:clamp(2rem,8vw,4.6rem);line-height:.9;letter-spacing:-.06em;margin:0}.eyebrow{color:var(--accent);font-size:.75rem;font-weight:800;letter-spacing:.18em;text-transform:uppercase;margin-bottom:12px}
.status{border:1px solid var(--line);border-radius:99px;padding:8px 13px;color:var(--muted);white-space:nowrap}.status.online{color:var(--accent);border-color:#607d36}
.console{display:grid;grid-template-columns:repeat(3,108px) 170px 150px;justify-content:center;gap:12px}.card{background:color-mix(in srgb,var(--panel) 92%,transparent);border:1px solid var(--line);border-radius:18px;padding:14px;box-shadow:0 18px 50px #0004}
.row{display:flex;align-items:baseline;justify-content:space-between;gap:18px;margin-bottom:15px}.label{font-weight:750}.value{font:700 1.3rem ui-monospace,monospace;color:var(--accent)}
input[type=range]{width:100%;height:34px;margin:0;accent-color:var(--accent);cursor:pointer}small{display:block;color:var(--muted);margin-top:8px}
select{width:100%;background:#111610;color:var(--text);border:1px solid var(--line);border-radius:10px;padding:12px;font:inherit}
.fader{height:430px;display:flex;flex-direction:column;align-items:center}.fader .row,.peak-card .row{width:100%;flex-direction:column;align-items:center;gap:2px;text-align:center}.vertical-slider{writing-mode:vertical-lr;direction:rtl;width:34px!important;height:305px!important;flex:1}
.utility{height:430px;display:flex;flex-direction:column;justify-content:space-between;gap:12px}.routing-block .label{display:block;margin-bottom:12px}.bypass-panel,.routing-panel{width:100%}.routing-panel{margin-top:auto}
.peak-card{height:430px;display:flex;flex-direction:column;align-items:center}.peak-card .row{width:100%}.meter-stack{display:flex;flex:1;min-height:0;align-items:stretch;gap:8px}.meter{width:30px;background:#0c100c;border:1px solid var(--line);border-radius:5px;overflow:hidden;display:flex;align-items:flex-end}.meter span{display:block;width:100%;height:0;background:linear-gradient(0deg,var(--accent2) 0 72%,#f4d35e 86%,#ff5c5c 100%);transition:height 70ms linear}
.scale{display:flex;flex-direction:column;justify-content:space-between;color:var(--muted);font:600 .68rem ui-monospace,monospace;padding:1px 0}
.bypass{width:100%;border:1px solid #607d36;background:#172014;color:var(--accent);border-radius:12px;padding:14px 18px;font:800 .86rem system-ui,sans-serif;letter-spacing:.1em;text-transform:uppercase;cursor:pointer}.bypass.active{background:#ffcb69;color:#241b08;border-color:#ffcb69}
footer{color:var(--muted);font-size:.82rem;margin-top:18px;text-align:center}
@media(max-width:780px){.console{grid-template-columns:repeat(3,minmax(92px,1fr))}.utility{grid-column:span 2}.peak-card{grid-column:span 1}.fader,.utility,.peak-card{height:390px}.vertical-slider{height:270px!important}}
</style>
</head>
<body><main>
<header><div><div class="eyebrow">Raspberry Pi · USB Audio</div><h1>Sound<br>shaping.</h1></div><div id="status" class="status">Connecting…</div></header>
<section class="console">
  <div class="card fader">
    <div class="row"><span class="label">Output gain</span><span id="gainValue" class="value">−1.9 dB</span></div>
    <input id="gain" class="vertical-slider" type="range" min="-60" max="12" step="0.1" value="-1.9" aria-label="Output gain in decibels">
  </div>
  <div class="card fader">
    <div class="row"><span class="label">Low-pass</span><span id="lpValue" class="value">Off</span></div>
    <input id="lp" class="vertical-slider" type="range" min="0" max="20000" step="50" value="0" aria-label="Low-pass cutoff">
  </div>
  <div class="card fader">
    <div class="row"><span class="label">High-pass</span><span id="hpValue" class="value">Off</span></div>
    <input id="hp" class="vertical-slider" type="range" min="0" max="5000" step="10" value="0" aria-label="High-pass cutoff">
  </div>
  <div class="utility">
    <div class="card bypass-panel">
      <button id="bypass" class="bypass" type="button" aria-pressed="false">BYPASS OFF</button>
    </div>
    <div class="card routing-panel routing-block">
      <span class="label">Input routing</span>
      <select id="routing" aria-label="Input routing">
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
const gain=$('gain'),hp=$('hp'),lp=$('lp'),routing=$('routing'),status=$('status'),bypass=$('bypass'),peakBar=$('peakBar'),peakValue=$('peakValue');
let timer,bypassed=false,bypassVersion=0;
const hz=v=>+v===0?'Off':(+v>=1000?(+v/1000).toFixed(+v%1000?1:0)+' kHz':v+' Hz');
function labels(){ $('gainValue').textContent=(+gain.value).toFixed(1)+' dB';$('hpValue').textContent=hz(hp.value);$('lpValue').textContent=hz(lp.value) }
function showBypass(value){bypassed=!!value;bypass.classList.toggle('active',bypassed);bypass.setAttribute('aria-pressed',bypassed);bypass.textContent=bypassed?'BYPASS ON':'BYPASS OFF'}
function showPeak(linear){const db=linear>0?20*Math.log10(linear):-120,p=Math.max(0,Math.min(100,(db+60)/60*100));peakBar.style.height=p+'%';peakValue.textContent=db<=-60?'<-60dBFS':db.toFixed(1)+'dBFS';peakBar.parentElement.setAttribute('aria-valuenow',Math.max(-60,db).toFixed(1))}
async function send(){
  clearTimeout(timer);
  const q=new URLSearchParams({gainDb:gain.value,highpass:hp.value,lowpass:lp.value,routing:routing.value});
  try{const r=await fetch('/api/set?'+q);if(!r.ok)throw Error();status.textContent='Live';status.className='status online'}catch{status.textContent='Disconnected';status.className='status'}
}
function changed(){labels();clearTimeout(timer);timer=setTimeout(send,45)}
[gain,hp,lp].forEach(x=>x.addEventListener('input',changed));routing.addEventListener('change',send);
async function setBypass(next){const version=++bypassVersion;showBypass(next);try{const r=await fetch('/api/set?bypass='+(next?'1':'0'));if(!r.ok)throw Error();const s=await r.json();if(version===bypassVersion)showBypass(s.bypass)}catch{if(version===bypassVersion)showBypass(!next)}}
bypass.addEventListener('click',()=>setBypass(!bypassed));
async function load(){
  const version=bypassVersion;
  try{const s=await(await fetch('/api/state')).json();gain.value=s.gainDb;hp.value=s.highpass;lp.value=s.lowpass;routing.value=s.routing;if(version===bypassVersion)showBypass(s.bypass);showPeak(s.peak);labels();status.textContent='Live';status.className='status online'}
  catch{status.textContent='Disconnected';status.className='status'}
}
async function meter(){try{const s=await(await fetch('/api/state')).json();showPeak(s.peak)}catch{}}
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
    json << "{\"gainDb\":" << gainDb
         << ",\"lowpass\":" << state.lowPassHz
         << ",\"highpass\":" << state.highPassHz
         << ",\"peak\":" << state.peak
         << ",\"input1Peak\":" << state.input1Peak
         << ",\"input2Peak\":" << state.input2Peak
         << ",\"gateDb\":" << state.noiseGateDb
         << ",\"gateOpen\":" << (state.gateOpen ? "true" : "false")
         << ",\"bypass\":" << (state.bypass ? "true" : "false")
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
    value = parameters.find("lowpass");
    if(value != parameters.end()) {
        const float cutoff = std::stof(value->second);
        if(cutoff < 0 || cutoff >= sampleRate_ * 0.5f)
            throw std::runtime_error("lowpass");
        processor_.set_low_pass(cutoff);
    }
    value = parameters.find("highpass");
    if(value != parameters.end()) {
        const float cutoff = std::stof(value->second);
        if(cutoff < 0 || cutoff >= sampleRate_ * 0.5f)
            throw std::runtime_error("highpass");
        processor_.set_high_pass(cutoff);
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
    value = parameters.find("bypass");
    if(value != parameters.end()) {
        if(value->second != "0" && value->second != "1")
            throw std::runtime_error("bypass");
        processor_.set_bypass(value->second == "1");
    }
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

#include "ui.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <iostream>
#include <sstream>

#include "runtime/database.h"

namespace {
volatile sig_atomic_t stopped = 0;
void stop_server(int) { stopped = 1; }

const char UI[] = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Wake artifact triage</title><style>
:root{color-scheme:dark;--bg:#0b1020;--panel:#121a2d;--line:#27324b;--text:#e6edf7;--muted:#96a2b8;--ok:#4ade80;--bad:#fb7185;--accent:#60a5fa}*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);font:14px ui-sans-serif,system-ui,sans-serif}header{padding:22px 28px;border-bottom:1px solid var(--line);display:flex;align-items:center;gap:18px}h1{font-size:19px;margin:0}header span,.muted{color:var(--muted)}main{display:grid;grid-template-columns:minmax(360px,42%) 1fr;height:calc(100vh - 68px)}aside{border-right:1px solid var(--line);overflow:auto}.tools{padding:14px;position:sticky;top:0;background:var(--bg);display:flex;gap:8px}input,select{background:var(--panel);border:1px solid var(--line);border-radius:7px;color:var(--text);padding:9px 10px}input{flex:1}.job{padding:13px 16px;border-top:1px solid var(--line);cursor:pointer}.job:hover,.job.on{background:var(--panel)}.row{display:flex;justify-content:space-between;gap:12px}.label{overflow:hidden;text-overflow:ellipsis;white-space:nowrap}.bad{color:var(--bad)}.ok{color:var(--ok)}section{overflow:auto;padding:24px}h2{margin:0 0 8px;font-size:18px}h3{font-size:13px;color:var(--muted);text-transform:uppercase;margin:24px 0 8px}.card,pre{background:var(--panel);border:1px solid var(--line);border-radius:8px;padding:13px}pre{white-space:pre-wrap;word-break:break-word;max-height:320px;overflow:auto}.files{margin:0;padding-left:20px}.files li{padding:3px 0}code{color:#bfdbfe}.empty{color:var(--muted);padding:30px}@media(max-width:760px){main{display:block;height:auto}aside{border-right:0;max-height:48vh}section{padding:18px}}
</style></head><body><header><h1>Wake artifact triage</h1><span id="summary">Loading…</span></header><main><aside><div class="tools"><input id="q" placeholder="Search jobs or artifacts"><select id="state"><option value="all">All</option><option value="failed">Failed</option><option value="passed">Passed</option></select></div><div id="jobs"></div></aside><section id="detail"><div class="empty">Select a job to inspect its artifacts and logs.</div></section></main><script>
let data=[], selected=null; const esc=s=>String(s??'').replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
function files(j){return j.outputs||j.output_files||[]}; function name(f){return typeof f==='string'?f:(f.path||'')};
function status(j){return j.usage?.status??j.status??0};
function render(){const q=document.querySelector('#q').value.toLowerCase(), st=document.querySelector('#state').value;const shown=data.filter(j=>{const s=status(j);return(st==='all'||(st==='failed'?s!==0:s===0))&&(!q||JSON.stringify(j).toLowerCase().includes(q))});document.querySelector('#summary').textContent=`${shown.length} of ${data.length} jobs`;document.querySelector('#jobs').innerHTML=shown.map(j=>`<div class="job ${selected===j.job?'on':''}" data-id="${j.job}"><div class="row"><b class="label">${esc(j.label||'(unlabelled)')}</b><span class="${status(j)?'bad':'ok'}">${status(j)?'failed':'passed'}</span></div><div class="row muted"><span>#${j.job}</span><span>${files(j).length} artifact${files(j).length===1?'':'s'}</span></div></div>`).join('')||'<div class="empty">No matching jobs.</div>';document.querySelectorAll('.job').forEach(e=>e.onclick=()=>show(+e.dataset.id))}
function show(id){selected=id;render();const j=data.find(x=>x.job===id);if(!j)return;const outs=files(j);document.querySelector('#detail').innerHTML=`<h2>${esc(j.label||'(unlabelled)')}</h2><div class="muted">Job #${j.job} · exit ${status(j)}</div><h3>Command</h3><div class="card"><code>${esc((j.commandline||[]).join(' '))}</code></div><h3>Artifacts (${outs.length})</h3><div class="card">${outs.length?`<ul class="files">${outs.map(f=>`<li><code>${esc(name(f))}</code>${f.type?` <span class="muted">${esc(f.type)}</span>`:''}</li>`).join('')}</ul>`:'<span class="muted">No recorded outputs</span>'}</div><h3>Standard output</h3><pre>${esc(j.stdout)||'<span class="muted">Empty</span>'}</pre><h3>Standard error</h3><pre>${esc(j.stderr)||'<span class="muted">Empty</span>'}</pre>`}
document.querySelector('#q').oninput=render;document.querySelector('#state').onchange=render;fetch('/api/jobs').then(r=>{if(!r.ok)throw Error(r.statusText);return r.json()}).then(x=>{data=x;render()}).catch(e=>document.querySelector('#jobs').innerHTML=`<div class="empty bad">${esc(e)}</div>`);setInterval(()=>fetch('/api/jobs').then(r=>r.json()).then(x=>{data=x;render()}),5000);
</script></body></html>)HTML";

bool send_all(int fd, const std::string &data) {
  size_t sent = 0;
  while (sent < data.size()) {
    ssize_t n = send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
    if (n <= 0) return false;
    sent += static_cast<size_t>(n);
  }
  return true;
}

void reply(int fd, const char *status, const char *type, const std::string &body) {
  std::ostringstream out;
  out << "HTTP/1.1 " << status << "\r\nContent-Type: " << type
      << "\r\nContent-Length: " << body.size()
      << "\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff"
         "\r\nContent-Security-Policy: default-src 'self' 'unsafe-inline'\r\nConnection: close\r\n\r\n"
      << body;
  send_all(fd, out.str());
}

std::string jobs_json(Database &db) {
  MatchingQueryFilters filters;
  auto jobs = db.matching(std::move(filters));
  JAST root(JSON_ARRAY);
  for (const auto &job : jobs) root.add("", job.to_structured_json());
  std::ostringstream out;
  out << root;
  return out.str();
}
}  // namespace

int serve_ui(Database &db, const std::string &address, const std::string &port) {
  addrinfo hints = {};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;
  addrinfo *addresses = nullptr;
  int gai = getaddrinfo(address.empty() ? nullptr : address.c_str(), port.c_str(), &hints, &addresses);
  if (gai != 0) {
    std::cerr << "wake ui: " << gai_strerror(gai) << std::endl;
    return 1;
  }
  int server = -1;
  for (addrinfo *a = addresses; a; a = a->ai_next) {
    server = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
    if (server < 0) continue;
    int yes = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    if (bind(server, a->ai_addr, a->ai_addrlen) == 0 && listen(server, 32) == 0) break;
    close(server);
    server = -1;
  }
  freeaddrinfo(addresses);
  if (server < 0) {
    std::cerr << "wake ui: cannot listen on " << address << ':' << port << ": " << strerror(errno)
              << std::endl;
    return 1;
  }
  signal(SIGINT, stop_server);
  signal(SIGTERM, stop_server);
  std::cout << "Wake UI listening on http://" << address << ':' << port << std::endl;
  if (address != "127.0.0.1" && address != "localhost" && address != "::1")
    std::cerr << "warning: Wake UI is remotely accessible and has no authentication" << std::endl;
  while (!stopped) {
    int client = accept(server, nullptr, nullptr);
    if (client < 0) {
      if (errno == EINTR) continue;
      std::cerr << "wake ui: accept: " << strerror(errno) << std::endl;
      break;
    }
    char request[8193] = {};
    ssize_t n = recv(client, request, sizeof(request) - 1, 0);
    std::string path;
    if (n > 0) {
      std::istringstream line(std::string(request, static_cast<size_t>(n)));
      std::string method, version;
      line >> method >> path >> version;
      if (method != "GET")
        reply(client, "405 Method Not Allowed", "text/plain; charset=utf-8", "GET only\n");
      else if (path == "/" || path == "/index.html")
        reply(client, "200 OK", "text/html; charset=utf-8", UI);
      else if (path == "/api/jobs")
        reply(client, "200 OK", "application/json; charset=utf-8", jobs_json(db));
      else if (path == "/healthz")
        reply(client, "200 OK", "text/plain; charset=utf-8", "ok\n");
      else
        reply(client, "404 Not Found", "text/plain; charset=utf-8", "not found\n");
    }
    close(client);
  }
  close(server);
  return 0;
}

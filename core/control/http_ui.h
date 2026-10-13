// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_CONTROL_HTTP_UI_H_
#define BESS_CONTROL_HTTP_UI_H_

#include <string_view>

namespace bess::control {

// GET / (D-125): one static page, compiled in, that reads the daemon only
// through the HTTP JSON API (POST /v2/GetPipeline, POST /v2/ListMetrics,
// GET /v2/events) -- no state or logic of its own.
inline constexpr std::string_view kHttpIndexPage = R"html(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>bessd</title>
<style>
body { font-family: sans-serif; margin: 1.5em; }
table { border-collapse: collapse; margin-bottom: 1.5em; }
th, td { border: 1px solid #ccc; padding: 0.2em 0.6em; text-align: left; font-size: 0.9em; }
td.n { text-align: right; font-family: monospace; }
#events { font-family: monospace; font-size: 0.85em; white-space: pre; max-height: 20em; overflow: auto; }
.err { color: #b00; }
</style>
</head>
<body>
<h1>bessd</h1>
<p>Epoch <span id="epoch">?</span>, generation <span id="generation">?</span>.
<button id="refresh">Refresh</button></p>
<h2>Modules</h2>
<table><thead><tr><th>Name</th><th>Class</th></tr></thead><tbody id="modules"></tbody></table>
<h2>Metrics</h2>
<table><thead><tr><th>Name</th><th>Labels</th><th>Value</th></tr></thead><tbody id="metrics"></tbody></table>
<h2>Events</h2>
<div id="events"></div>
<script>
"use strict";
async function call(method, body) {
  const r = await fetch("/v2/" + method, {method: "POST", headers: {"Content-Type": "application/json"},
                                          body: JSON.stringify(body || {})});
  const answer = await r.json();
  if (!r.ok) throw new Error(method + ": " + answer.message);
  return answer;
}
function row(tbody, cells, numeric) {
  const tr = tbody.insertRow();
  cells.forEach((text, i) => {
    const td = tr.insertCell();
    td.textContent = text;
    if (numeric === i) td.className = "n";
  });
}
function fail(tbody, e) {
  tbody.replaceChildren();
  const td = tbody.insertRow().insertCell();
  td.colSpan = 3;
  td.className = "err";
  td.textContent = String(e);
}
async function refresh() {
  const modules = document.getElementById("modules");
  const metrics = document.getElementById("metrics");
  try {
    const p = await call("GetPipeline");
    document.getElementById("epoch").textContent = p.daemonEpoch || "0";
    document.getElementById("generation").textContent = p.generation || "0";
    modules.replaceChildren();
    ((p.pipeline || {}).modules || []).forEach(m => row(modules, [m.name, m.mclass]));
  } catch (e) { fail(modules, e); }
  try {
    const m = await call("ListMetrics");
    metrics.replaceChildren();
    (m.samples || []).forEach(s => {
      const labels = Object.entries(s.labels || {}).sort().map(([k, v]) => k + "=" + v).join(", ");
      row(metrics, [s.name, labels, String(s.value || 0)], 2);
    });
  } catch (e) { fail(metrics, e); }
}
document.getElementById("refresh").onclick = refresh;
refresh();
const log = document.getElementById("events");
new EventSource("/v2/events").onmessage = (m) => {
  const e = JSON.parse(m.data);
  log.textContent = [e.sequence || "-", e.type, e.source, JSON.stringify(e.fields || {})].join(" ") +
                    "\n" + log.textContent;
  if (e.type !== "bess.start" && e.type !== "bess.progress") refresh();
};
</script>
</body>
</html>
)html";

}  // namespace bess::control

#endif  // BESS_CONTROL_HTTP_UI_H_

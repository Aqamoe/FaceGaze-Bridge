#include "web/panel.hpp"

#include "base/log.hpp"
#include "params/avatar_config.hpp"
#include "params/catalog.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

namespace fgb::web {
namespace {

constexpr char kTag[] = "panel";

// 面板页面。刻意写成单文件：不引前端框架、不要构建步骤、不要 CDN。
// 刷新用 500 ms 轮询而不是 WebSocket——本机回环上这点开销无所谓，代码少一半。
constexpr char kIndexHtml[] = R"HTML(<!doctype html>
<html lang="zh"><head><meta charset="utf-8">
<title>FaceGaze Bridge</title>
<meta name="viewport" content="width=device-width,initial-scale=1">
<style>
:root{--bg:#14161a;--panel:#1c1f26;--line:#2a2f39;--fg:#e6e9ef;--dim:#8b93a7;--acc:#4da3ff;--ok:#3ddc84;--warn:#ffb020}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);font:14px/1.5 -apple-system,"Segoe UI",system-ui,sans-serif}
header{padding:16px 20px;border-bottom:1px solid var(--line);display:flex;align-items:center;gap:12px}
h1{font-size:16px;margin:0;font-weight:600}
.badge{font-size:12px;padding:2px 8px;border-radius:10px;background:#243042;color:var(--acc)}
main{padding:20px;display:grid;gap:16px;grid-template-columns:repeat(auto-fit,minmax(320px,1fr))}
section{background:var(--panel);border:1px solid var(--line);border-radius:10px;padding:16px}
section h2{font-size:13px;margin:0 0 12px;color:var(--dim);font-weight:600;letter-spacing:.04em;text-transform:uppercase}
.row{display:flex;justify-content:space-between;padding:4px 0;border-bottom:1px dashed #232833}
.row:last-child{border-bottom:0}
.row span:first-child{color:var(--dim)}
.k{color:var(--fg);font-variant-numeric:tabular-nums}
.bar{height:6px;background:#232833;border-radius:3px;overflow:hidden;margin-top:4px}
.bar i{display:block;height:100%;background:var(--acc);width:0}
label{display:flex;align-items:center;gap:8px;margin:8px 0}
input[type=range]{flex:1}
button{background:#243042;color:var(--fg);border:1px solid var(--line);border-radius:6px;padding:6px 12px;cursor:pointer}
button:hover{border-color:var(--acc)}
table{width:100%;border-collapse:collapse;font-size:13px}
td{padding:2px 0;font-variant-numeric:tabular-nums}
td:last-child{text-align:right;color:var(--dim)}
.muted{color:var(--dim);font-size:12px}
</style></head><body>
<header><h1>FaceGaze Bridge</h1><span class="badge" id="status">连接中…</span></header>
<main>
  <section><h2>运行状态</h2><div id="stats"></div></section>
  <section><h2>头像</h2><div id="avatar"></div></section>
  <section><h2>滤波</h2>
    <label><input type="checkbox" id="fEnabled"> 启用 One Euro 滤波</label>
    <label>minCutoff <input type="range" id="fCutoff" min="0.1" max="5" step="0.1"><b id="fCutoffV"></b></label>
    <label>beta <input type="range" id="fBeta" min="0" max="2" step="0.05"><b id="fBetaV"></b></label>
    <div class="muted">默认关闭，与 VRCFT 一致。调大 minCutoff 更跟手，调小更稳。</div>
  </section>
  <section><h2>实时参数</h2><div id="params"></div></section>
</main>
<script>
const $=id=>document.getElementById(id);
function row(k,v){return `<div class="row"><span>${k}</span><span class="k">${v}</span></div>`}
async function tick(){
  try{
    const s=await (await fetch('/api/status')).json();
    $('status').textContent = s.source_ok ? '运行中' : '无数据';
    $('status').style.color = s.source_ok ? 'var(--ok)' : 'var(--warn)';
    $('stats').innerHTML =
      row('输入', s.source) + row('帧率', s.fps.toFixed(1)+' fps') +
      row('累计帧', s.frames_in) + row('tick', s.ticks) + row('迟到 tick', s.late_ticks) +
      row('OSC 消息', s.messages) + row('已发送', (s.bytes/1024).toFixed(1)+' KB');
    $('avatar').innerHTML =
      row('名称', s.avatar_name || '—') + row('ID', (s.avatar_id||'—').slice(0,20)) +
      row('参数', s.param_count + ' / ' + s.target_count + ' 条目标') +
      row('二进制', s.binary_count + ' 个') + row('参数集', s.params_mode) +
      row('眼追', s.eye_mode);
    if(s.filter){
      $('fEnabled').checked=s.filter.enabled;
      if(document.activeElement!==$('fCutoff')){$('fCutoff').value=s.filter.min_cutoff}
      if(document.activeElement!==$('fBeta')){$('fBeta').value=s.filter.beta}
      $('fCutoffV').textContent=(+$('fCutoff').value).toFixed(1);
      $('fBetaV').textContent=(+$('fBeta').value).toFixed(2);
    }else{
      $('fEnabled').disabled=true;
      $('fCutoff').disabled=true;$('fBeta').disabled=true;
      $('fCutoffV').textContent='—';$('fBetaV').textContent='—';
    }
    const p=await (await fetch('/api/params')).json();
    const rows=p.params.slice(0,28).map(x=>
      `<div class="row"><span>${x.name}</span><span class="k">${x.value.toFixed(3)}</span></div>`);
    $('params').innerHTML = rows.join('') || '<div class="muted">没有参数被绑定</div>';
  }catch(e){ $('status').textContent='面板已断开'; $('status').style.color='var(--warn)'; }
}
async function pushFilter(){
  const body={enabled:$('fEnabled').checked,
              min_cutoff:+$('fCutoff').value, beta:+$('fBeta').value};
  await fetch('/api/filter',{method:'POST',headers:{'Content-Type':'application/json'},
                             body:JSON.stringify(body)});
}
['fEnabled','fCutoff','fBeta'].forEach(id=>{
  $(id).addEventListener('input',()=>{
    $('fCutoffV').textContent=(+$('fCutoff').value).toFixed(1);
    $('fBetaV').textContent=(+$('fBeta').value).toFixed(2);
  });
  $(id).addEventListener('change',pushFilter);
});
tick(); setInterval(tick,500);
</script></body></html>)HTML";

struct Response {
    int status = 200;
    std::string body;
    std::string content_type = "application/json";
};

std::string make_http_response(const Response& response) {
    std::string out;
    out.reserve(response.body.size() + 160);
    const char* reason = response.status == 200 ? "OK" : (response.status == 404 ? "Not Found"
                                                                               : "Bad Request");
    char header[256];
    std::snprintf(header, sizeof(header),
                  "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                  "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
                  response.status, reason, response.content_type.c_str(), response.body.size());
    out.append(header);
    out.append(response.body);
    return out;
}

} // namespace

Panel::Panel(PanelContext context) : ctx_(std::move(context)) {}

bool Panel::start(std::uint16_t port, std::string& error) { return listener_.listen(port, error); }

void Panel::stop() { listener_.close(); }

namespace {

Response route(PanelContext& ctx, const HttpRequest& request) {
    Response response;

    if (request.is_get() && (request.path == "/" || request.path == "/index.html")) {
        response.body = kIndexHtml;
        response.content_type = "text/html; charset=utf-8";
        return response;
    }

    if (request.is_get() && request.path == "/api/status") {
        std::string json;
        JsonWriter writer{json};
        writer.obj_begin();

        const EngineStats stats = ctx.engine != nullptr ? ctx.engine->stats() : EngineStats{};
        const bool source_ok    = stats.frames_in > 0;

        writer.key("source").str(ctx.source_name);
        writer.key("source_ok").boolean(source_ok);
        writer.key("frames_in").integer(static_cast<long long>(stats.frames_in));
        writer.key("ticks").integer(static_cast<long long>(stats.ticks));
        writer.key("late_ticks").integer(static_cast<long long>(stats.late_ticks));

        // 帧率用"两次请求之间新增的帧数 / 间隔"估算，够用且不用额外埋点
        static std::uint64_t last_frames = 0;
        static auto last_time            = std::chrono::steady_clock::now();
        const auto now                   = std::chrono::steady_clock::now();
        const double dt = std::chrono::duration<double>(now - last_time).count();
        const std::uint64_t delta = stats.frames_in >= last_frames ? stats.frames_in - last_frames : 0;
        const double fps = dt > 0.05 ? static_cast<double>(delta) / dt : 0.0;
        last_frames      = stats.frames_in;
        last_time        = now;
        writer.key("fps").num(fps, 1);

        if (ctx.sink != nullptr) {
            const SinkStats s = ctx.sink->stats();
            writer.key("messages").integer(static_cast<long long>(s.messages));
            writer.key("bytes").integer(static_cast<long long>(s.bytes));
        } else {
            writer.key("messages").integer(0);
            writer.key("bytes").integer(0);
        }

        writer.key("avatar_name").str(ctx.avatar_name);
        writer.key("avatar_id").str(ctx.avatar_id);
        writer.key("param_count").integer(static_cast<long long>(ctx.param_count));
        writer.key("target_count").integer(static_cast<long long>(ctx.target_count));
        writer.key("binary_count").integer(static_cast<long long>(ctx.binary_count));
        writer.key("params_mode").str(ctx.params_mode);
        writer.key("eye_mode").str(ctx.eye_mode);

        writer.key("filter");
        if (ctx.filter != nullptr) {
            writer.obj_begin();
            writer.key("enabled").boolean(ctx.filter->enabled());
            writer.key("min_cutoff").num(ctx.filter->min_cutoff(), 2);
            writer.key("beta").num(ctx.filter->beta(), 2);
            writer.obj_end();
        } else {
            writer.obj_begin();
            writer.obj_end();
        }

        writer.obj_end();
        response.body = std::move(json);
        return response;
    }

    if (request.is_get() && request.path == "/api/params") {
        const auto groups       = params::groups();
        const FaceFrame frame   = ctx.engine != nullptr ? ctx.engine->latest()
                                                        : FaceFrame::neutral();
        const params::EvalContext eval{frame, 30.0f};

        std::string json;
        JsonWriter writer{json};
        writer.obj_begin();
        writer.key("params").arr_begin();
        for (const params::Group& group : groups) {
            for (const params::ParamEntry& entry : group.entries) {
                writer.obj_begin();
                std::string name{group.name_prefix};
                name += entry.name;
                writer.key("name").str(name);
                writer.key("value").num(entry.eval(eval), 3);
                writer.obj_end();
            }
        }
        writer.arr_end();
        writer.obj_end();
        response.body = std::move(json);
        return response;
    }

    if (request.is_post() && request.path == "/api/filter") {
        if (ctx.filter == nullptr) {
            response.status = 400;
            response.body   = R"({"ok":false,"error":"filter not enabled"})";
            return response;
        }
        bool enabled = ctx.filter->enabled();
        double cutoff = ctx.filter->min_cutoff();
        double beta   = ctx.filter->beta();
        body_bool(request.body, "enabled", enabled);
        body_number(request.body, "min_cutoff", cutoff);
        body_number(request.body, "beta", beta);
        ctx.filter->set_enabled(enabled);
        ctx.filter->set_live_params(static_cast<float>(cutoff), static_cast<float>(beta));
        response.body = R"({"ok":true})";
        return response;
    }

    if (request.is_post() && request.path == "/api/quit") {
        if (ctx.quit != nullptr) {
            ctx.quit->store(true, std::memory_order_release);
        }
        response.body = R"({"ok":true})";
        return response;
    }

    response.status = 404;
    response.body   = R"({"ok":false,"error":"not found"})";
    return response;
}

} // namespace

bool Panel::serve_once(std::uint16_t timeout_ms) {
    const std::intptr_t conn = listener_.accept(timeout_ms);
    if (conn < 0) {
        return false;
    }
    char buffer[8192];
    const int n = net::TcpListener::recv_request(conn, buffer, sizeof(buffer));
    if (n <= 0) {
        net::TcpListener::close_conn(conn);
        return false;
    }

    HttpRequest request;
    Response response;
    if (!parse_request(buffer, static_cast<std::size_t>(n), request)) {
        response.status = 400;
        response.body   = R"({"ok":false,"error":"bad request"})";
    } else {
        response = route(ctx_, request);
    }

    const std::string raw = make_http_response(response);
    net::TcpListener::send_all(conn, raw.data(), raw.size());
    net::TcpListener::close_conn(conn);
    return true;
}

void Panel::run(std::atomic<bool>& stop_flag) {
    FGB_INFO(kTag, "控制面板：http://127.0.0.1:%u/", static_cast<unsigned>(listener_.port()));
    while (!stop_flag.load(std::memory_order_acquire)) {
        serve_once(100);
    }
}

} // namespace fgb::web

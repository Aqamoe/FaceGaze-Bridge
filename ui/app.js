// FaceGaze Bridge 控制面板。
//
// 与引擎的唯一通道是 WebSocket（/ws），消息格式见 src/web/ipc.cpp：
//   hello   {protocol, version, port}
//   catalog {names[], sent[], count}   参数名表，连接建立时发一次
//   status  {...指标, filter{...}}      10 Hz
//   params  {values[]}                 10 Hz，与 catalog 顺序一一对应
//   log     {level, tag, message}
//   ack     {command, ok, error?}      命令回执
//
// 页面不引框架、不打包：改一行 CSS 刷新就能看到。等 UI 复杂到需要构建时再上 TS/Vite，
// 现在引一套构建链只会让"改个滑杆"变成"先 npm i"。
"use strict";

const $ = (id) => document.getElementById(id);

// ---------- 本地化 ----------
//
// 文案在 ui/i18n.js，这里只做查表与"缺键回落到中文"。
// 语言记在 localStorage：WebView2 的用户数据目录会保留，所以选过一次就一直生效。
const DICTS = window.FGB_I18N || { zh: {} };
const LANGS = window.FGB_LANGS || [{ code: "zh", label: "简体中文" }];
const FALLBACK_LANG = "zh";

let lang = initial_lang();

function initial_lang() {
  const stored = localStorage.getItem("fgb.lang");
  if (stored && DICTS[stored]) {
    return stored;
  }
  const nav = (navigator.language || "zh").toLowerCase();
  let guess = "en";
  if (nav.startsWith("zh")) {
    guess = "zh";
  } else if (nav.startsWith("ja")) {
    guess = "ja";
  }
  return DICTS[guess] ? guess : FALLBACK_LANG;
}

function t(key, vars) {
  let text = (DICTS[lang] || {})[key];
  if (text === undefined) {
    if (lang !== FALLBACK_LANG) {
      console.warn(`[i18n] ${lang} 缺少文案: ${key}`);
    }
    text = (DICTS[FALLBACK_LANG] || {})[key];
  }
  if (text === undefined) {
    return key;
  }
  if (vars) {
    for (const [name, value] of Object.entries(vars)) {
      text = text.split(`{${name}}`).join(String(value));
    }
  }
  return text;
}

function locale_tag() {
  return lang === "zh" ? "zh-CN" : lang === "ja" ? "ja-JP" : "en-US";
}

// 静态文案走 data-i18n；动态文案在各 render_* 里用 t()。
function apply_l10n() {
  document.documentElement.lang = lang;
  document.querySelectorAll("[data-i18n]").forEach((node) => {
    node.textContent = t(node.dataset.i18n);
  });
  document.querySelectorAll("[data-i18n-placeholder]").forEach((node) => {
    node.placeholder = t(node.dataset.i18nPlaceholder);
  });
  $("brand-version").textContent = state.version ? `v${state.version}` : t("app.subtitle");

  // 页面标题、pill、模块卡片等都是动态生成的，切语言后要重画一遍
  update_page_header();
  if (state.status) {
    render_status(state.status);
  }
  if (state.view === "modules") {
    render_modules();
  }
  if (state.view === "logs") {
    rerender_logs();
  }
  set_link(state.connected ? true : null, state.connected ? t("link.connected") : t("link.connecting"));
}

function set_lang(code) {
  if (!DICTS[code]) {
    return;
  }
  lang = code;
  localStorage.setItem("fgb.lang", code);
  apply_l10n();
  // 下拉框自己也要跟上，否则从代码/探针切换后界面会显示"选的是中文"
  const select = $("lang");
  if (select) {
    select.value = code;
  }
}

const state = {
  connected: false,
  version: "",
  names: [],
  sent: [],
  values: [],
  cells: [],
  mini: [],
  logs: [],
  modules: null,
  status: null,
  source_rows: [],
  view: "status",
  retry: 0,
  timer: null,
  socket: null,
};

const PAGE_META = {
  status: ["page.status.title", "page.status.sub"],
  modules: ["page.modules.title", "page.modules.sub"],
  params: ["page.params.title", "page.params.sub"],
  output: ["page.output.title", "page.output.sub"],
  logs: ["page.logs.title", "page.logs.sub"],
  settings: ["page.settings.title", "page.settings.sub"],
};


// ---------- WebSocket ----------

function ws_url() {
  const host = location.hostname || "127.0.0.1";
  const port = location.port || "8787";
  return `ws://${host}:${port}/ws`;
}

function connect() {
  clearTimeout(state.timer);
  let socket;
  try {
    socket = new WebSocket(ws_url());
  } catch (error) {
    schedule_reconnect();
    return;
  }
  state.socket = socket;

  socket.onopen = () => {
    state.connected = true;
    state.retry = 0;
    set_link(true, t("link.connected"));
  };

  socket.onclose = () => {
    state.connected = false;
    set_link(false, t("link.disconnected"));
    schedule_reconnect();
  };

  socket.onerror = () => {
    if (!state.connected) {
      set_link(null, t("link.failed"));
    }
  };

  socket.onmessage = (event) => {
    let message;
    try {
      message = JSON.parse(event.data);
    } catch (error) {
      return;
    }
    handle(message);
  };
}

function schedule_reconnect() {
  state.retry = Math.min(state.retry + 1, 6);
  const delay = Math.min(400 * Math.pow(1.7, state.retry), 5000);
  clearTimeout(state.timer);
  state.timer = setTimeout(connect, delay);
}

function send(payload) {
  if (state.socket && state.socket.readyState === WebSocket.OPEN) {
    state.socket.send(JSON.stringify(payload));
    return true;
  }
  return false;
}

function set_link(up, text) {
  const led = $("link-state").querySelector(".led");
  led.className = "led" + (up === true ? " on" : up === false ? " off" : "");
  $("link-text").textContent = text;
  $("link-detail").textContent = ws_url();
  $("s-link").textContent = ws_url();
}

function handle(message) {
  switch (message.type) {
    case "hello":
      state.version = message.version || "";
      $("brand-version").textContent = state.version ? `v${state.version}` : t("app.subtitle");
      break;
    case "catalog":
      build_catalog(message.names || [], message.sent || []);
      break;
    case "status":
      render_status(message);
      break;
    case "modules":
      state.modules = message;
      if (state.view === "modules") {
        render_modules();
      }
      break;
    case "params":
      render_params(message.values || []);
      break;
    case "log":
      push_log(message);
      break;
    case "ack":
      if (message.command === "reload_avatar" && message.ok) {
        push_log({ level: "info", tag: "ui", message: t("log.avatar_reloaded") });
      }
      break;
    default:
      break;
  }
}

// ---------- 渲染：状态 ----------

function render_status(status) {
  state.status = status; // 留给调试与探针读最新一帧
  $("m-fps").textContent = (status.fps || 0).toFixed(1);
  $("m-frames").textContent = fmt_int(status.frames_in);
  $("m-ticks").textContent = fmt_int(status.ticks);
  $("m-late").textContent = fmt_int(status.late_ticks);
  $("m-messages").textContent = fmt_int(status.messages);
  $("m-bytes").textContent = ((status.bytes || 0) / 1024).toFixed(1);
  $("m-source").textContent = t(status.source_ok ? "status.fps.hint.running" : "status.fps.hint.idle", {
    source: status.source || "—",
  });

  $("pill-source").textContent = status.source ? `${t("pill.input")} · ${status.source}` : `${t("pill.input")} · —`;
  $("pill-source").className = "pill" + (status.source_ok ? " accent" : "");
  $("pill-eye").textContent = `${t("pill.eye")} · ${status.eye_mode || "—"}`;

  $("a-name").textContent = status.avatar_name || t("status.avatar.missing");
  $("a-id").textContent = (status.avatar_id || "—").slice(0, 24);
  $("a-bound").textContent = status.bound_to_avatar
    ? t("status.avatar.bound.avatar")
    : t("status.avatar.bound.prefix");
  $("a-params").textContent = `${status.param_count || 0} / ${status.matched || 0}`;
  $("a-targets").textContent = status.target_count || 0;
  $("a-binary").textContent = status.binary_count || 0;

  $("o-mode").textContent = status.params_mode || "—";
  $("o-eye").textContent = status.eye_mode || "—";
  $("o-targets").textContent = status.target_count || 0;
  $("o-matched").textContent = `${status.matched || 0}${t("output.matched.suffix")}`;
  $("o-binary").textContent = status.binary_count || 0;
  const vmc_out = status.vmc_out || {};
  $("o-vmc").textContent = vmc_out.enabled
    ? `${vmc_out.target} · ${fmt_int(vmc_out.messages)}`
    : t("output.vmc.off");
  $("c-source").textContent = `${status.source || "—"} · ${(status.fps || 0).toFixed(1)} fps`;
  $("s-clients").textContent = status.clients || 0;

  if (status.module && state.view === "modules") {
    sync_active_module(status.module);
  }
  render_sources(status.sources || []);

  const filter = status.filter || {};
  const enabled = $("f-enabled");
  const cutoff = $("f-cutoff");
  const beta = $("f-beta");
  const available = !!filter.available;

  enabled.disabled = !available;
  cutoff.disabled = !available || !filter.enabled;
  beta.disabled = !available || !filter.enabled;

  if (available) {
    if (document.activeElement !== enabled) {
      enabled.checked = !!filter.enabled;
    }
    if (document.activeElement !== cutoff) {
      cutoff.value = filter.min_cutoff;
    }
    if (document.activeElement !== beta) {
      beta.value = filter.beta;
    }
    $("f-cutoff-v").textContent = Number(cutoff.value).toFixed(1);
    $("f-beta-v").textContent = Number(beta.value).toFixed(2);
  } else {
    $("f-cutoff-v").textContent = "—";
    $("f-beta-v").textContent = "—";
  }
}

// ---------- 渲染：参数 ----------

function build_catalog(names, sent) {
  state.names = names;
  state.sent = sent;
  state.values = new Array(names.length).fill(0);

  const heatmap = $("heatmap");
  heatmap.textContent = "";
  state.cells = names.map((name, index) => {
    const cell = document.createElement("div");
    cell.className = "cell";
    cell.dataset.name = name.toLowerCase();
    cell.dataset.index = String(index);
    cell.title = name;

    const label = document.createElement("span");
    label.className = "cell-name";
    label.textContent = short_name(name);
    const value = document.createElement("span");
    value.className = "cell-val";
    value.textContent = "0.000";

    cell.append(label, value);
    heatmap.append(cell);
    return { element: cell, value, last: -1 };
  });

  const mini = $("mini-heat");
  mini.textContent = "";
  state.mini = [];
  const step = Math.max(1, Math.floor(names.length / 96));
  for (let i = 0; i < names.length; i += step) {
    const bar = document.createElement("i");
    bar.title = names[i];
    mini.append(bar);
    state.mini.push({ element: bar, index: i, last: -1 });
  }

  apply_param_filter();
}

function short_name(name) {
  return name.startsWith("v2/") ? name.slice(3) : name;
}

function render_params(values) {
  state.values = values;

  if (state.view === "params") {
    for (let i = 0; i < state.cells.length && i < values.length; ++i) {
      const cell = state.cells[i];
      const value = values[i];
      if (Math.abs(value - cell.last) < 0.004) {
        continue;
      }
      cell.last = value;
      cell.element.style.setProperty("--v", Math.min(1, Math.abs(value)).toFixed(3));
      cell.value.textContent = value.toFixed(3);
    }
  }

  if (state.view === "status") {
    for (const bar of state.mini) {
      const value = values[bar.index] || 0;
      const rounded = Math.round(Math.abs(value) * 50) / 50;
      if (rounded === bar.last) {
        continue;
      }
      bar.last = rounded;
      bar.element.style.setProperty("--v", Math.min(1, rounded).toFixed(2));
    }
  }
}

function apply_param_filter() {
  const text = $("param-search").value.trim().toLowerCase();
  const only_sent = $("param-only-sent").checked;
  let visible = 0;

  state.cells.forEach((cell, index) => {
    const matches_text = !text || cell.element.dataset.name.includes(text);
    const matches_sent = !only_sent || state.sent[index];
    const show = matches_text && matches_sent;
    cell.element.hidden = !show;
    cell.element.classList.toggle("dim", !state.sent[index]);
    if (show) {
      visible += 1;
    }
  });

  $("param-count").textContent = t("params.count", { visible, total: state.cells.length });
  $("param-empty").hidden = visible !== 0;
}

// ---------- 渲染：输入源 ----------

// 10 Hz 更新，所以只在"源的数量变了"时重建 DOM，其它时候只改文字与 class
function render_sources(sources) {
  const list = $("source-list");
  if (!list) {
    return;
  }
  if (!sources.length) {
    list.textContent = "";
    list.dataset.count = "0";
    state.source_rows = [];
    list.append(el("div", "muted", t("status.sources.empty")));
    return;
  }

  if (list.dataset.count !== String(sources.length)) {
    list.textContent = "";
    list.dataset.count = String(sources.length);
    state.source_rows = sources.map((source) => {
      const row = el("div", "source-row");
      const name = el("b", "source-name", source.name);
      const meta = el("span", "muted source-meta");
      const chips = el("span", "source-chips");
      const chip = (key) => {
        const node = el("span", "source-chip", t(`channel.${key}`));
        chips.append(node);
        return node;
      };
      const made = {
        row,
        meta,
        expression: chip("expression"),
        eyes: chip("eyes"),
        head: chip("head"),
      };
      row.append(name, meta, chips);
      list.append(row);
      return made;
    });
  }

  sources.forEach((source, index) => {
    const row = state.source_rows[index];
    if (!row) {
      return;
    }
    row.meta.textContent =
      `${t("status.sources.frames", { count: fmt_int(source.frames) })} · ` +
      t("status.sources.age", { ms: Math.round(source.age_ms) });
    row.row.classList.toggle("stale", !source.fresh);
    for (const key of ["expression", "eyes", "head"]) {
      const chip = row[key];
      chip.classList.toggle("on", !!source[key]);
      chip.classList.toggle("has", !source[key] && !!source[`has_${key}`]);
    }
  });
}

// ---------- 渲染：模块 ----------

// 状态 → (文案 key, chip 样式)。文案在 i18n.js 的 state.* 下。
const MODULE_STATE_STYLES = {
  active: "ok",
  starting: "warn",
  error: "err",
  stopped: "err",
  broken: "err",
  installed: "",
  none: "",
  external: "",
};

function el(tag, className, text) {
  const node = document.createElement(tag);
  if (className) {
    node.className = className;
  }
  if (text !== undefined) {
    node.textContent = text;
  }
  return node;
}

function format_state(key) {
  const style = MODULE_STATE_STYLES[key] !== undefined ? MODULE_STATE_STYLES[key] : "";
  return { label: t(`state.${key}`), cls: style };
}

function render_modules() {
  const data = state.modules;
  const items = (data && data.items) || [];
  const list = $("module-list");
  list.textContent = "";

  $("mod-config").textContent = (data && data.config) || t("modules.unpersisted");
  $("mod-config").title = (data && data.config) || "";

  if (!items.length) {
    const empty = el("div", "empty");
    empty.append(el("div", "", t("modules.empty")));
    empty.append(el("div", "hint", t("modules.empty.hint")));
    if (data && data.roots) {
      for (const root of data.roots) {
        empty.append(el("div", "module-meta", `[${root.origin}] ${root.path}`));
      }
    }
    list.append(empty);
    return;
  }

  for (const item of items) {
    list.append(module_card(item, data));
  }
}

function module_card(item, data) {
  const card = el("article", "module-card");
  card.dataset.id = item.id;
  if (item.active) {
    card.classList.add("active");
  }
  if (item.broken) {
    card.classList.add("broken");
  }

  const head = el("div", "module-head");
  head.append(el("strong", "", item.name || item.id));
  if (item.version) {
    head.append(el("span", "module-ver", `v${item.version}`));
  }
  if (item.author) {
    head.append(el("span", "module-author", item.author));
  }
  const chip_state = format_state(item.state);
  const chip = el("span", `chip ${chip_state.cls}`.trim(), chip_state.label);
  chip.dataset.role = "chip";
  head.append(chip);
  card.append(head);

  if (item.description) {
    card.append(el("p", "module-desc", item.description));
  }

  const meta = el("div", "module-meta");
  meta.append(el("span", "", `${t("modules.id")}  ${item.id}`));
  meta.append(el("span", "", `${t("modules.dll")} ${item.dll || t("modules.none_found")}`));
  card.append(meta);

  const error = el("div", "module-error");
  error.dataset.role = "error";
  error.hidden = true;
  card.append(error);

  const actions = el("div", "module-actions");
  if (item.active) {
    actions.append(el("span", "muted", t("modules.current")));
  } else if (item.broken) {
    actions.append(el("span", "muted", t("modules.broken.hint")));
  } else {
    const button = el("button", "btn", t("modules.set"));
    button.addEventListener("click", () => send({ type: "set_module", id: item.id }));
    actions.append(button);
  }
  card.append(actions);

  return card;
}

// 运行态在 status 里（10 Hz），模块列表只在变化时推一次：
// 这里只更新"当前那个"的 chip 与错误行，避免整页重建抢走按钮焦点。
function sync_active_module(module) {
  if (!module || !state.modules) {
    return;
  }
  const card = document.querySelector(`.module-card[data-id="${CSS.escape(module.id || "")}"]`);
  if (!card) {
    return;
  }
  const chip = card.querySelector('[data-role="chip"]');
  // 变量名不能叫 state：会遮蔽上面那个全局 state，而全局的那行 const 更晚执行，
  // 于是最前面的 `!state.modules` 直接 TDZ 报错（"Cannot access 'state' before initialization"）。
  const chip_state = format_state(module.state);
  if (chip) {
    chip.className = `chip ${chip_state.cls}`.trim();
    chip.textContent = chip_state.label;
  }
  const error = card.querySelector('[data-role="error"]');
  if (error) {
    error.hidden = !module.error;
    // 模块抛出来的经常是整段 .NET 栈：卡片里只放前几行，全文挂到 title 上
    error.textContent = (module.error || "").slice(0, 400);
    error.title = module.error || "";
  }
}

// ---------- 日志 ----------

const LEVEL_ORDER = { trace: 0, debug: 1, info: 2, warn: 3, error: 4 };

function push_log(entry) {
  state.logs.push({ ...entry, time: new Date() });
  if (state.logs.length > 600) {
    state.logs.shift();
  }
  if (state.view === "logs" && pass_level(entry.level)) {
    append_log_line(entry, new Date());
  }
}

function pass_level(level) {
  const min = $("log-level").value;
  return (LEVEL_ORDER[level] ?? 2) >= (LEVEL_ORDER[min] ?? 2);
}

function append_log_line(entry, time) {
  const box = $("logs");
  const line = document.createElement("div");
  line.className = `log-line log-level-${entry.level || "info"}`;

  // 不能叫 t：会把全局的翻译函数 t() 遮住，后面所有文案都变成 "t is not a function"
  const time_span = document.createElement("span");
  time_span.className = "t";
  time_span.textContent = time.toLocaleTimeString(locale_tag(), { hour12: false });
  const lv = document.createElement("span");
  lv.className = "lv";
  lv.textContent = (entry.level || "info").toUpperCase();
  const tg = document.createElement("span");
  tg.className = "tg";
  tg.textContent = entry.tag || "-";
  const msg = document.createElement("span");
  msg.textContent = entry.message || "";

  line.append(time_span, lv, tg, msg);
  box.append(line);

  while (box.childElementCount > 600) {
    box.firstElementChild.remove();
  }
  if ($("log-follow").checked) {
    box.scrollTop = box.scrollHeight;
  }
}

function rerender_logs() {
  const box = $("logs");
  box.textContent = "";
  for (const entry of state.logs) {
    if (pass_level(entry.level)) {
      append_log_line(entry, entry.time);
    }
  }
}

// ---------- 交互 ----------

function update_page_header() {
  const [title_key, subtitle_key] = PAGE_META[state.view] || ["", ""];
  $("page-title").textContent = title_key ? t(title_key) : "";
  $("page-subtitle").textContent = subtitle_key ? t(subtitle_key) : "";
}

function switch_page(page) {
  state.view = page;
  document.querySelectorAll(".nav-item").forEach((item) => {
    item.classList.toggle("active", item.dataset.page === page);
  });
  document.querySelectorAll(".page").forEach((item) => {
    item.classList.toggle("active", item.dataset.page === page);
  });
  update_page_header();

  if (page === "logs") {
    rerender_logs();
  }
  if (page === "modules") {
    render_modules();
  }
  if (page === "params") {
    apply_param_filter();
    render_params(state.values);
  }
  if (page === "status") {
    render_params(state.values);
  }
}

function push_filter() {
  send({
    type: "set_filter",
    enabled: $("f-enabled").checked,
    min_cutoff: Number($("f-cutoff").value),
    beta: Number($("f-beta").value),
  });
}

function wire() {
  document.querySelectorAll(".nav-item").forEach((item) => {
    item.addEventListener("click", () => switch_page(item.dataset.page));
  });

  $("param-search").addEventListener("input", apply_param_filter);
  $("param-only-sent").addEventListener("change", apply_param_filter);

  $("f-enabled").addEventListener("change", push_filter);
  $("f-cutoff").addEventListener("input", () => {
    $("f-cutoff-v").textContent = Number($("f-cutoff").value).toFixed(1);
  });
  $("f-beta").addEventListener("input", () => {
    $("f-beta-v").textContent = Number($("f-beta").value).toFixed(2);
  });
  $("f-cutoff").addEventListener("change", push_filter);
  $("f-beta").addEventListener("change", push_filter);

  $("o-reload").addEventListener("click", () => {
    send({ type: "reload_avatar" });
  });

  $("mod-rescan").addEventListener("click", () => {
    send({ type: "scan_modules" });
  });

  $("log-level").addEventListener("change", rerender_logs);
  $("log-clear").addEventListener("click", () => {
    state.logs = [];
    $("logs").textContent = "";
  });

  $("s-level").addEventListener("change", () => {
    send({ type: "set_log_level", level: $("s-level").value });
  });

  $("s-quit").addEventListener("click", () => {
    if (window.confirm(t("confirm.quit"))) {
      send({ type: "quit" });
    }
  });

  // 语言选择器：选项来自 i18n.js 的 FGB_LANGS，选完立刻整页重画
  const lang_select = $("lang");
  for (const item of LANGS) {
    const option = document.createElement("option");
    option.value = item.code;
    option.textContent = item.label;
    lang_select.append(option);
  }
  lang_select.value = lang;
  lang_select.addEventListener("change", () => {
    set_lang(lang_select.value);
  });

  window.addEventListener("beforeunload", () => {
    if (state.socket) {
      state.socket.close();
    }
  });
}

function fmt_int(value) {
  return (value || 0).toLocaleString(locale_tag());
}

apply_l10n();
wire();
connect();

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

let toast_timer = null;
function show_toast(text, kind) {
  const node = $("toast");
  node.textContent = text;
  node.className = "toast" + (kind === "error" ? " error" : "");
  node.hidden = false;
  clearTimeout(toast_timer);
  toast_timer = setTimeout(() => {
    node.hidden = true;
  }, 4200);
}

let modal_resolve = null;
function ask_confirm({ title, message, confirmText, danger = true }) {
  return new Promise((resolve) => {
    if (modal_resolve) {
      modal_resolve(false); // 连续触发时先取消上一个，避免 Promise 悬空
    }
    modal_resolve = resolve;
    $("modal-title").textContent = title || t("common.confirm");
    $("modal-message").textContent = message || "";
    $("modal-ok").textContent = confirmText || t("common.confirm");
    $("modal-cancel").textContent = t("common.cancel");
    $("modal-ok").className = "btn" + (danger ? " danger" : "");
    $("modal").hidden = false;
    requestAnimationFrame(() => $("modal-ok").focus());
  });
}

function close_modal(result) {
  if (!modal_resolve) {
    $("modal").hidden = true;
    return;
  }
  $("modal").hidden = true;
  const resolve = modal_resolve;
  modal_resolve = null;
  resolve(result);
}

let restart_timer = null;
function set_restarting(active) {
  const banner = $("restart-banner");
  banner.hidden = !active;
  clearTimeout(restart_timer);
  if (active) {
    restart_timer = setTimeout(() => {
      banner.hidden = true;
    }, 30000);
  }
}

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
  if (state.view === "modules" && state.modules) {
    render_modules();
  }
  if (state.view === "library") {
    render_library();
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
  library: null,
  import_selection: new Set(),
  expanded_modules: new Set(),
  library_tab: "vrcft",
  restarting: false,
  status: null,
  source_rows: [],
  running_rows: [],
  view: "status",
  retry: 0,
  timer: null,
  socket: null,
};

const PAGE_META = {
  status: ["page.status.title", "page.status.sub"],
  modules: ["page.modules.title", "page.modules.sub"],
  library: ["page.library.title", "page.library.sub"],
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
    state.restarting = false;
    set_restarting(false);
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
      } else if (state.view === "library") {
        render_library();
      }
      break;
    case "library": {
      const prev = (state.library && state.library.status) || {};
      const prev_phase = prev.phase;
      const prev_id = prev.install_id;
      state.library = message;
      const next = message.status || {};
      const finished = prev_phase === "downloading" || prev_phase === "installing";
      const entry = (message.items || []).find((item) => item.id === prev_id);
      if (finished && next.phase === "idle") {
        show_toast(t("modules.library.install_ok", { name: entry ? entry.name : prev_id }));
      }
      if (finished && next.phase === "error") {
        show_toast(t("modules.library.install_failed", {
          name: entry ? entry.name : prev_id,
          error: next.error || "?",
        }), "error");
      }
      if (state.view === "library") {
        render_library();
      }
      break;
    }
    case "params":
      render_params(message.values || []);
      break;
    case "log":
      push_log(message);
      break;
    case "ack":
      if (message.ok && message.restarting) {
        set_restarting(true);
      }
      if (!message.ok && message.error) {
        show_toast(`${message.command}: ${message.error}`, "error");
        push_log({ level: "warn", tag: "ui", message: `${message.command}: ${message.error}` });
      } else if (message.ok && message.message) {
        show_toast(message.message);
        if (message.command === "import_modules") {
          state.import_selection.clear();
        }
      } else if (message.command === "reload_avatar" && message.ok) {
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
  const shm_out = status.shm_out || {};
  $("o-shm").textContent = shm_out.enabled
    ? `${shm_out.name} · ${fmt_int(shm_out.messages)}`
    : t("output.shm.off");
  $("c-source").textContent = `${status.source || "—"} · ${(status.fps || 0).toFixed(1)} fps`;
  $("s-clients").textContent = status.clients || 0;

  if (state.view === "modules") {
    render_running_modules(status.modules);
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
  return { text: t(`state.${key}`), cls: style };
}

function render_modules() {
  const data = state.modules;
  const items = (data && data.items) || [];
  const list = $("module-list");
  list.textContent = "";

  $("mod-store").textContent = (data && data.store) || "—";
  $("mod-store").title = (data && data.store) || "";
  const installed_ids = new Set(items.map((item) => item.id));
  for (const id of [...state.expanded_modules]) {
    if (!installed_ids.has(id)) {
      state.expanded_modules.delete(id);
    }
  }
  $("mod-installed-count").textContent = t("modules.installed.count", { count: items.length });
  $("mod-picking").hidden = !(data && data.picking);
  $("mod-restart-hint").hidden = !(data && data.restart_needed);

  const runtime = (data && data.runtime) || {};
  const pill = $("mod-runtime");
  if (runtime.available) {
    pill.textContent = `${t("modules.runtime")} · ${t("modules.runtime." + (runtime.origin || "unknown"))}`;
    pill.className = "pill ok";
    pill.title = runtime.path || "";
  } else {
    pill.textContent = t("modules.runtime.missing");
    pill.className = "pill warn";
    pill.title = "";
  }

  if (!items.length) {
    const empty = el("div", "empty");
    empty.append(el("div", "", t("modules.empty")));
    empty.append(el("div", "hint", t("modules.empty.hint")));
    list.append(empty);
  } else {
    for (const item of items) {
      list.append(installed_card(item));
    }
  }

  render_running_modules(state.status ? state.status.modules : []);
  render_importable((data && data.importable) || []);
}

function fmt_mb(bytes) {
  if (!bytes) {
    return "—";
  }
  return `${(bytes / 1048576).toFixed(1)} MB`;
}

function source_label(source) {
  if (!source) {
    return "—";
  }
  const key = `modules.source.${source}`;
  const text = t(key);
  return text === key ? source : text;
}

function module_chip(item) {
  if (item.broken) {
    return { text: t("state.broken"), cls: "err" };
  }
  if (!item.enabled) {
    return { text: t("state.disabled"), cls: "" };
  }
  if (item.running) {
    return format_state(item.state);
  }
  if (item.active) {
    return { text: t("state.selected"), cls: "ok" };
  }
  return { text: t("state.installed"), cls: "" };
}

function installed_card(item) {
  const card = el("article", "module-card");
  card.dataset.id = item.id;
  card.tabIndex = 0;
  if (item.active) {
    card.classList.add("active");
  }
  if (item.broken) {
    card.classList.add("broken");
  }
  if (state.expanded_modules.has(item.id)) {
    card.classList.add("expanded");
  }

  const head = el("div", "module-head");
  head.append(el("strong", "", item.name || item.id));
  if (item.version) {
    head.append(el("span", "module-ver", `v${item.version}`));
  }
  if (item.author) {
    head.append(el("span", "module-author", item.author));
  }
  head.append(el("span", "chip", item.kind === "native"
    ? t("modules.kind.native") : t("modules.kind.vrcft")));
  const chip_state = module_chip(item);
  const chip = el("span", `chip ${chip_state.cls}`.trim(), chip_state.text);
  chip.dataset.role = "chip";
  head.append(chip);
  head.append(el("span", "chevron", "▾"));
  card.append(head);

  if (item.description) {
    card.append(el("p", "module-main-desc", item.description));
  }

  const live = ((state.status && state.status.modules) || [])
    .find((entry) => entry.module_id === item.id);
  const details = el("div", "module-details");
  const meta = el("div", "module-meta");
  meta.append(el("span", "", `${t("modules.id")}  ${item.id}`));
  meta.append(el("span", "", `${t("modules.source")} ${source_label(item.source)}`));
  meta.append(el("span", "", `${t("modules.installed_at")} ${item.installed_at || "—"}`));
  meta.append(el("span", "", `${t("modules.size")} ${fmt_mb(item.bytes)}`));
  if (item.dll) {
    meta.append(el("span", "", item.dll));
  }
  details.append(meta);

  const error = el("div", "module-error");
  error.dataset.role = "error";
  error.hidden = !live || !live.error;
  if (live && live.error) {
    error.textContent = live.error.slice(0, 400);
    error.title = live.error;
  }
  details.append(error);
  card.append(details);

  const actions = el("div", "module-actions");

  const toggle = el("label", "mod-toggle");
  const checkbox = document.createElement("input");
  checkbox.type = "checkbox";
  checkbox.checked = !!item.enabled;
  checkbox.addEventListener("change", () => {
    send({ type: "set_module_enabled", id: item.id, enabled: checkbox.checked });
  });
  toggle.append(checkbox, el("span", "", t("modules.enabled")));
  actions.append(toggle);

  if (!item.active) {
    const set_button = el("button", "btn ghost small", t("modules.set_active"));
    set_button.addEventListener("click", () => send({ type: "set_module", id: item.id }));
    actions.append(set_button);
  } else {
    actions.append(el("span", "muted", t("modules.current")));
  }

  const remove = el("button", "btn danger small", t("modules.delete"));
  remove.addEventListener("click", async () => {
    const running = !!(item.running || live);
    const text = running
      ? t("modules.delete_confirm_running", { name: item.name || item.id })
      : t("modules.delete_confirm", { name: item.name || item.id });
    if (await ask_confirm({ title: t("confirm.delete_title"), message: text, danger: true })) {
      send({ type: "delete_module", id: item.id });
    }
  });
  actions.append(remove);
  card.append(actions);

  const toggle_card = (event) => {
    if (event.type === "click" && event.target.closest("button, input, label, a, select")) {
      return;
    }
    if (event.type === "keydown" && event.key !== "Enter" && event.key !== " ") {
      return;
    }
    event.preventDefault();
    const open = !card.classList.contains("expanded");
    card.classList.toggle("expanded", open);
    if (open) {
      state.expanded_modules.add(item.id);
    } else {
      state.expanded_modules.delete(item.id);
    }
  };
  card.addEventListener("click", toggle_card);
  card.addEventListener("keydown", toggle_card);
  return card;
}

function render_importable(items) {
  const list = $("importable-list");
  const head = $("importable-head");
  const hint = $("importable-hint");
  const available = new Set(items.map((item) => item.id));
  for (const id of [...state.import_selection]) {
    if (!available.has(id)) {
      state.import_selection.delete(id);
    }
  }

  list.textContent = "";
  head.hidden = !items.length;
  hint.hidden = !items.length;
  $("importable-count").textContent = items.length
    ? t("modules.importable.count", { count: items.length })
    : "";

  for (const item of items) {
    const card = el("label", "import-card");
    const check = document.createElement("input");
    check.type = "checkbox";
    check.className = "import-check";
    check.checked = state.import_selection.has(item.id);
    check.addEventListener("change", () => {
      if (check.checked) {
        state.import_selection.add(item.id);
      } else {
        state.import_selection.delete(item.id);
      }
      update_import_selection_ui();
    });

    const body = el("div", "import-body");
    const header = el("div", "module-head");
    header.append(el("strong", "", item.name || item.id));
    if (item.version) {
      header.append(el("span", "module-ver", `v${item.version}`));
    }
    if (item.author) {
      header.append(el("span", "module-author", item.author));
    }
    header.append(el("span", "chip", item.origin || "VRCFT"));
    body.append(header);

    if (item.description) {
      body.append(el("p", "module-desc", item.description));
    }
    const meta = el("div", "module-meta");
    meta.append(el("span", "", `${t("modules.id")}  ${item.id}`));
    meta.append(el("span", "", item.dir || ""));
    body.append(meta);
    if (item.broken) {
      body.append(el("div", "module-error", t("state.broken")));
    }

    card.append(check, body);
    list.append(card);
  }
  update_import_selection_ui();
}

function update_import_selection_ui() {
  const items = (state.modules && state.modules.importable) || [];
  const total = items.length;
  const selected = state.import_selection.size;
  const button = $("mod-import-selected");
  button.disabled = selected === 0;
  button.textContent = selected
    ? t("modules.import_selected_count", { count: selected })
    : t("modules.import_selected");
  const all = $("import-select-all");
  all.checked = total > 0 && selected === total;
  all.indeterminate = selected > 0 && selected < total;
}

function parse_version(text) {
  const match = String(text || "").trim().replace(/^v/i, "").match(/^(\d+(?:\.\d+)*)/);
  return match ? match[1].split(".").map(Number) : null;
}

// 返回 -1/0/1；任一方无法解析时返回 null（当作"已安装"，避免永远提示更新）
function compare_versions(a, b) {
  const left = parse_version(a);
  const right = parse_version(b);
  if (!left || !right) {
    return null;
  }
  const count = Math.max(left.length, right.length);
  for (let i = 0; i < count; ++i) {
    const x = left[i] || 0;
    const y = right[i] || 0;
    if (x !== y) {
      return x < y ? -1 : 1;
    }
  }
  return 0;
}

// install / update / installed
function library_relation(installed, item) {
  if (!installed) {
    return "install";
  }
  const cmp = compare_versions(installed.version, item.version);
  if (cmp === null) {
    // 旧安装可能没有版本号（如 "local"）。如果库条目有版本，允许更新一次把元数据补齐；
    // 两边都无法解析时才当作已安装，避免永远提示更新。
    return parse_version(item.version) ? "update" : "installed";
  }
  return cmp >= 0 ? "installed" : "update";
}

function rating_stars(value) {
  const full = Math.round(value || 0);
  return "★".repeat(Math.max(0, Math.min(5, full))) + "☆".repeat(Math.max(0, 5 - full));
}

function library_card(item, installed, status) {
  const card = el("article", "lib-card");
  card.dataset.id = item.id;

  const head = el("div", "module-head");
  head.append(el("strong", "", item.name || item.id));
  head.append(el("span", "module-ver", `v${item.version || "?"}`));
  if (item.author) {
    head.append(el("span", "module-author", item.author));
  }
  const relation = library_relation(installed, item);
  if (relation !== "install") {
    const tag = el("span", `chip ${relation === "update" ? "warn" : "ok"}`.trim(),
      relation === "update" ? t("modules.library.update_available") : t("modules.library.installed"));
    head.append(tag);
  }
  card.append(head);

  if (item.description) {
    card.append(el("p", "lib-desc", item.description));
  }

  const meta = el("div", "lib-meta");
  meta.append(el("span", "rating", `${rating_stars(item.rating)} ${(item.rating || 0).toFixed(1)}`));
  meta.append(el("span", "", t("modules.library.downloads", { count: fmt_int(item.downloads) })));
  if (item.updated) {
    meta.append(el("span", "", item.updated.slice(0, 10)));
  }
  card.append(meta);

  const actions = el("div", "lib-actions");
  const installing = status && status.install_id === item.id &&
    (status.phase === "downloading" || status.phase === "installing");
  const button = el("button", "btn small", "");
  const state_text = el("span", "muted", "");
  const progress = el("div", "progress");
  const bar = document.createElement("i");
  progress.append(bar);
  progress.hidden = true;

  if (installing) {
    button.hidden = true;
    progress.hidden = false;
    state_text.dataset.role = "lib-state";
    actions.append(progress, state_text);
  } else {
    button.textContent = relation === "installed" ? t("modules.library.installed") :
      (relation === "update" ? t("modules.library.update") : t("modules.library.install"));
    button.disabled = relation === "installed" || (status && (status.phase === "downloading" ||
      status.phase === "installing"));
    button.addEventListener("click", async () => {
      const question = t("modules.library.confirm", { name: item.name || item.id });
      if (await ask_confirm({ title: t("confirm.install_title"), message: question, danger: true })) {
        send({ type: "install_library", id: item.id });
      }
    });
    actions.append(button);
  }
  card.append(actions);
  update_library_card_progress(card, status, item.id);
  return card;
}

function update_library_card_progress(card, status, id) {
  const bar = card.querySelector(".progress i");
  const label = card.querySelector("[data-role=lib-state]");
  if (!bar || !status || status.install_id !== id) {
    return;
  }
  const percent = status.total > 0 ? Math.min(100, (status.received / status.total) * 100) : 0;
  bar.parentElement.hidden = false;
  bar.style.setProperty("--p", `${percent.toFixed(1)}%`);
  if (label) {
    label.textContent = status.phase === "installing"
      ? t("modules.library.installing")
      : t("modules.library.downloading", {
          percent: percent.toFixed(0),
          received: (status.received / 1048576).toFixed(1),
          total: status.total ? (status.total / 1048576).toFixed(1) : "?",
        });
  }
}

function render_native_library() {
  const list = $("native-library-list");
  const status_el = $("lib-status");
  const filter = ($("lib-search").value || "").trim().toLowerCase();
  const all_items = ((state.modules && state.modules.items) || [])
    .filter((item) => item.kind === "native");
  const items = all_items.filter((item) => {
    if (!filter) {
      return true;
    }
    return `${item.name} ${item.author} ${item.description}`.toLowerCase().includes(filter);
  });

  status_el.textContent = t("modules.library.native_count", { count: all_items.length });
  status_el.title = "";
  list.textContent = "";
  if (!items.length) {
    list.append(el("div", "empty", all_items.length
      ? t("modules.library.no_match") : t("modules.library.native_empty")));
    return;
  }

  const live = new Map(((state.status && state.status.modules) || [])
    .map((entry) => [entry.module_id, entry]));
  for (const item of items) {
    const card = el("article", "lib-card");
    const head = el("div", "module-head");
    head.append(el("strong", "", item.name || item.id));
    if (item.version) {
      head.append(el("span", "module-ver", `v${item.version}`));
    }
    if (item.author) {
      head.append(el("span", "module-author", item.author));
    }
    const runtime_entry = live.get(item.id);
    const chip_state = item.broken
      ? { text: t("state.broken"), cls: "err" }
      : (runtime_entry ? format_state(runtime_entry.state) : module_chip(item));
    const chip = el("span", `chip ${chip_state.cls}`.trim(), chip_state.text);
    head.append(chip);
    card.append(head);

    if (item.description) {
      card.append(el("p", "lib-desc", item.description));
    }
    const meta = el("div", "lib-meta");
    meta.append(el("span", "", `${t("modules.source")} ${source_label(item.source)}`));
    meta.append(el("span", "", `${t("modules.installed_at")} ${item.installed_at || "—"}`));
    meta.append(el("span", "", `${t("modules.size")} ${fmt_mb(item.bytes)}`));
    card.append(meta);

    const actions = el("div", "lib-actions");
    const manage = el("button", "btn small", t("modules.library.open_modules"));
    manage.addEventListener("click", () => switch_page("modules"));
    actions.append(manage);
    card.append(actions);
    list.append(card);
  }
}

function render_library() {
  const list = $("library-list");
  if (!list) {
    return;
  }

  const native = state.library_tab === "native";
  document.querySelectorAll("#lib-tabs .tab").forEach((tab) => {
    tab.classList.toggle("active", tab.dataset.libTab === state.library_tab);
  });
  $("lib-refresh").hidden = native;
  $("lib-native-install").hidden = !native;
  list.hidden = native;
  $("native-library-list").hidden = !native;
  $("library-hint").hidden = native;
  $("native-library-hint").hidden = !native;
  if (native) {
    render_native_library();
    return;
  }

  const lib = state.library;
  const status_el = $("lib-status");
  if (!lib) {
    status_el.textContent = t("modules.library.not_loaded");
    if (!list.childElementCount) {
      list.append(el("div", "empty", t("modules.library.not_loaded")));
    }
    return;
  }

  const status = lib.status || {};
  if (status.phase === "downloading" || status.phase === "installing") {
    const percent = status.total > 0 ? Math.min(100, (status.received / status.total) * 100) : 0;
    status_el.textContent = status.phase === "installing"
      ? t("modules.library.installing")
      : t("modules.library.downloading", {
          percent: percent.toFixed(0),
          received: (status.received / 1048576).toFixed(1),
          total: status.total ? (status.total / 1048576).toFixed(1) : "?",
        });
  } else if (status.phase === "error") {
    status_el.textContent = t("modules.library.error", { error: status.error || "?" });
  } else if (status.refreshing) {
    status_el.textContent = t("modules.library.refreshing");
  } else if (status.refresh_error) {
    status_el.textContent = t("modules.library.error", { error: status.refresh_error });
  } else if (status.stale) {
    status_el.textContent = t("modules.library.stale", { time: status.fetched_at || "—" });
  } else {
    status_el.textContent = t("modules.library.updated", { time: status.fetched_at || "—" });
  }
  status_el.title = status.error || status.refresh_error || "";
  $("lib-refresh").disabled = !!status.refreshing;

  const filter = ($("lib-search").value || "").trim().toLowerCase();
  const all_items = lib.items || [];
  const items = all_items.filter((item) => {
    if (!filter) {
      return true;
    }
    return `${item.name} ${item.author} ${item.description}`.toLowerCase().includes(filter);
  });
  const installed = new Map(((state.modules && state.modules.items) || [])
    .map((m) => [String(m.id).toLowerCase(), m]));

  const signature = items.map((item) => item.id).join(",") + "|" + status.phase + "|" +
    (status.install_id || "") + "|" + filter;
  if (list.dataset.signature !== signature) {
    list.dataset.signature = signature;
    list.textContent = "";
    if (!items.length) {
      list.append(el("div", "empty", all_items.length ? t("modules.library.no_match") :
        t("modules.library.empty")));
    } else {
      for (const item of items) {
        list.append(library_card(item, installed.get(String(item.id).toLowerCase()), status));
      }
    }
  } else {
    for (const item of items) {
      const card = list.querySelector(`.lib-card[data-id="${CSS.escape(item.id)}"]`);
      if (card) {
        update_library_card_progress(card, status, item.id);
      }
    }
  }
}

// 运行中的实例：多实例时"谁在跑、哪一路坏了"要分开看，所以单独一块，
// 而不是把状态塞进"磁盘上的模块"卡片里（那两者可能不一致：改了选择还没重启）。
function render_running_modules(modules) {
  const list = $("running-list");
  if (!list) {
    return;
  }
  const running = modules || [];

  if (!running.length) {
    list.textContent = "";
    list.dataset.count = "0";
    state.running_rows = [];
    list.append(el("div", "muted", t("modules.running.empty")));
    return;
  }

  if (list.dataset.count !== String(running.length)) {
    list.textContent = "";
    list.dataset.count = String(running.length);
    state.running_rows = running.map((item) => {
      const row = el("div", "running-row");
      const name = el("b", "running-name", item.name);
      const module_name = el("span", "running-module", item.module_name || item.module_id);
      const chip = el("span", "chip", "");
      row.append(name, module_name, chip);
      list.append(row);

      const extra = el("div", "running-extra");
      list.append(extra);
      return { row, chip, extra };
    });
  }

  running.forEach((item, index) => {
    const ui = state.running_rows[index];
    if (!ui) {
      return;
    }
    const chip_state = format_state(item.state);
    ui.chip.className = `chip ${chip_state.cls}`.trim();
    ui.chip.textContent = chip_state.text;

    const parts = [t("modules.restarts", { count: item.restarts || 0 })];
    if (item.error) {
      // 模块抛出来的经常是整段 .NET 栈：只放前几行，全文挂 title
      parts.push(item.error.slice(0, 160));
    }
    ui.extra.textContent = parts.join(" · ");
    ui.extra.title = item.error || "";
  });
}

// 卡片上的 chip 也反映运行态（"已安装"和"运行中"不该混淆）
function running_state_of(module_id) {
  const running = (state.status && state.status.modules) || [];
  const hit = running.find((item) => item.module_id === module_id);
  return hit ? hit.state : null;
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
  if (page === "library") {
    render_library();
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
  $("mod-install").addEventListener("click", () => {
    send({ type: "install_module_archive" });
  });
  $("mod-library").addEventListener("click", () => {
    switch_page("library");
  });
  $("mod-open").addEventListener("click", () => {
    send({ type: "open_modules_dir" });
  });
  $("mod-restart").addEventListener("click", async () => {
    if (await ask_confirm({
      title: t("modules.restart"),
      message: t("modules.restart_confirm"),
      danger: false,
    })) {
      send({ type: "restart_engine" });
    }
  });
  $("import-select-all").addEventListener("change", () => {
    const items = (state.modules && state.modules.importable) || [];
    const all = $("import-select-all").checked;
    state.import_selection.clear();
    if (all) {
      for (const item of items) {
        state.import_selection.add(item.id);
      }
    }
    if (state.modules) {
      render_importable(items);
    }
  });
  $("mod-import-selected").addEventListener("click", () => {
    if (!state.import_selection.size) {
      return;
    }
    send({ type: "import_modules", ids: [...state.import_selection].join(",") });
  });
  $("lib-refresh").addEventListener("click", () => {
    send({ type: "refresh_library" });
  });
  $("lib-search").addEventListener("input", () => {
    render_library();
  });
  document.querySelectorAll("#lib-tabs .tab").forEach((tab) => {
    tab.addEventListener("click", () => {
      state.library_tab = tab.dataset.libTab === "native" ? "native" : "vrcft";
      render_library();
    });
  });
  $("lib-native-install").addEventListener("click", () => {
    send({ type: "install_module_archive" });
  });

  $("log-level").addEventListener("change", rerender_logs);
  $("log-clear").addEventListener("click", () => {
    state.logs = [];
    $("logs").textContent = "";
  });

  $("s-level").addEventListener("change", () => {
    send({ type: "set_log_level", level: $("s-level").value });
  });

  $("s-quit").addEventListener("click", async () => {
    if (await ask_confirm({
      title: t("settings.engine.stop"),
      message: t("confirm.quit"),
      danger: true,
    })) {
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

  $("modal-ok").addEventListener("click", () => close_modal(true));
  $("modal-cancel").addEventListener("click", () => close_modal(false));
  $("modal").addEventListener("click", (event) => {
    if (event.target === $("modal")) {
      close_modal(false);
    }
  });
  document.addEventListener("keydown", (event) => {
    if ($("modal").hidden) {
      return;
    }
    if (event.key === "Escape") {
      event.preventDefault();
      close_modal(false);
    } else if (event.key === "Enter") {
      event.preventDefault();
      close_modal(true);
    }
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

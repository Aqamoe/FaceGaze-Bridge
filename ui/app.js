// FaceGaze Bridge 控制面板。
//
// 与引擎的唯一通道是 WebSocket（/ws），消息格式见 src/web/ipc.cpp：
//   hello   {protocol, version, port}
//   catalog {names[], sent[], count}   参数名表，连接建立时发一次
//   status  {...指标, filter{...}}      10 Hz
//   params  {values[]}                 10 Hz，与 catalog 顺序一一对应
//   calibration {shapes[], eye, dirty}  连接时 / 任何校准改动后
//   preview {raw[], out[], eye, seq}   30 Hz，只在校准页打开时推
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
  document.querySelectorAll("[data-i18n-title]").forEach((node) => {
    node.title = t(node.dataset.i18nTitle);
  });
  document.querySelectorAll("[data-i18n-aria-label]").forEach((node) => {
    node.setAttribute("aria-label", t(node.dataset.i18nAriaLabel));
  });
  $("brand-version").textContent = state.version ? `v${state.version}` : t("app.subtitle");
  $("about-version").textContent = state.version ? `v${state.version}` : "—";

  // 页面标题、pill、模块卡片等都是动态生成的，切语言后要重画一遍
  update_page_header();
  if (state.status) {
    render_status(state.status);
  }
  // 预设下拉的「自定义」是构建时写进去的，切语言要重建一次
  if (state.output_profiles) {
    render_output_profiles();
  }
  // 开关状态上挂着翻译文案（默认开启/已关闭），切语言时即使没连引擎也要刷新
  sync_output_form();
  if (state.view === "modules" && state.modules) {
    render_modules();
  }
  if (state.view === "library") {
    render_library();
  }
  if (state.view === "calibration") {
    render_calibration();
  }
  if (state.view === "settings") {
    render_steamvr();
  }
  update_face_ui();
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
  // 提示条的按钮文案是 JS 填的（随状态变），data-i18n 管不到，得重画一次
  render_update();
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
  calibration: null,
  preview: null,
  cal_rows: [],
  cal_groups: [],
  cal_curves_sig: "",
  cal_preview_on: false,
  shape_labels: null,   // ui/shapes.json：协议名 → 人话名称/分组/说明
  shape_labels_pending: false,
  curve_pop: null,      // 曲线编辑浮层（单例，打开时指向某一行）
  steamvr: null,
  steamvr_busy: false,
  face_map: null,
  face_mode: "idle", // idle | loading | 3d | 2d | error
  face_error: "",
  face_loading: false,
  face_renderer: null,
  face_fallback: null,
  face_selected: null,
  import_selection: new Set(),
  expanded_modules: new Set(),
  expanded_library: new Set(),
  library_tab: "vrcft",
  restarting: false,
  outputs_dirty: false,
  pending_outputs_restart: false,
  output_profiles: { profiles: [], active: "", dir: "" },
  profile_pending: null, // {action, name}：ack 回来时用本地化文案提示
  update_info: null,
  update: null,              // 最近一次 update 快照（引擎是唯一事实来源）
  update_manual: false,      // 这次检查是用户点的：失败要说话，自动检查失败就安静
  update_dismissed: false,   // 点了「稍后」：本次会话不再显示提示条
  update_tag_seen: "",       // 换到新版本后要重新提醒一次
  status: null,
  gaze: null,
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
  calibration: ["page.calibration.title", "page.calibration.sub"],
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
    // 引擎重启后新连接是干净的：校准页还开着就重新订阅预览
    if (state.view === "calibration") {
      state.cal_preview_on = false;
      set_cal_preview(true);
    }
    if (state.view === "settings") {
      request_steamvr_refresh();
    }
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
      $("about-version").textContent = state.version ? `v${state.version}` : "—";
      $("u-current").textContent = state.version || "—";
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
      state.library = message;
      // 安装/更新的结果由服务端的 notice 消息广播（下载线程只有它知道成败），
      // 这里不再拿相位变化去猜，免得两处判断各说一套、或者一次装完弹两个 toast。
      if (state.view === "library") {
        render_library();
      }
      break;
    }
    case "notice":
      // 模块删除/安装/更新这类会失败也会耗时的操作，成功失败都要有一句人话提醒
      if (message.message) {
        show_toast(message.message, message.level === "error" ? "error" : undefined);
      }
      break;
    case "params":
      render_params(message.values || []);
      break;
    case "gaze":
      state.gaze = message;
      if (state.view === "status") {
        render_gaze(message);
      }
      break;
    case "calibration":
      state.calibration = message;
      if (state.view === "calibration") {
        render_calibration();
      }
      break;
    case "preview":
      state.preview = message;
      if (state.view === "calibration") {
        render_preview(message);
      }
      break;
    case "steamvr":
      handle_steamvr(message);
      break;
    case "outputs_profiles":
      state.output_profiles = {
        profiles: message.profiles || [],
        active: message.active || "",
        dir: message.dir || "",
      };
      render_output_profiles();
      break;
    case "update":
      apply_update(message);
      break;
    case "diagnostics":
      $("diag-busy").hidden = true;
      $("diag-export").disabled = false;
      if (message.ok) {
        $("diag-path").textContent = message.path || "";
        show_toast(t("settings.diag.done", { path: message.path || "" }));
      } else {
        show_toast(t("settings.diag.failed", { error: message.error || "?" }), "error");
      }
      break;
    case "log":
      push_log(message);
      break;
    case "ack":
      if (message.ok && message.restarting) {
        set_restarting(true);
      }
      if (!message.ok && message.error) {
        // 服务端已经把失败原因写成人话（modules.message）；有就用它，
        // 别把 "delete_module: ..." 这种命令名甩给用户
        show_toast(message.message || `${message.command}: ${message.error}`, "error");
        push_log({ level: "warn", tag: "ui", message: `${message.command}: ${message.error}` });
        if ((message.command || "").includes("steamvr")) {
          state.steamvr_busy = false;
          render_steamvr();
        }
        if (message.command === "set_outputs" || message.command === "apply_output_profile") {
          state.pending_outputs_restart = false;
        }
        if (message.command === "export_diagnostics") {
          $("diag-busy").hidden = true;
          $("diag-export").disabled = false;
        }
        if (message.command === "check_updates") {
          $("u-busy").hidden = true;
          $("u-check").disabled = false;
        }
        state.profile_pending = null;
      } else if (message.command === "set_outputs") {
        set_outputs_dirty(false);
        show_toast(t("output.saved"));
        if (state.pending_outputs_restart) {
          state.pending_outputs_restart = false;
          send({ type: "restart_engine" });
        }
      } else if (message.command === "apply_output_profile") {
        set_outputs_dirty(false);
        const pending = state.profile_pending;
        state.profile_pending = null;
        show_toast(t("output.profile.applied", { name: pending ? pending.name : "" }));
        if (state.pending_outputs_restart) {
          state.pending_outputs_restart = false;
          send({ type: "restart_engine" });
        }
      } else if (message.command === "save_output_profile") {
        const pending = state.profile_pending;
        state.profile_pending = null;
        show_toast(t("output.profile.saved", { name: pending ? pending.name : "" }));
        if (pending && state.output_profiles.profiles.includes(pending.name)) {
          $("o-profile-select").value = pending.name;
          refresh_profile_buttons();
        }
      } else if (message.command === "delete_output_profile") {
        const pending = state.profile_pending;
        state.profile_pending = null;
        show_toast(t("output.profile.deleted", { name: pending ? pending.name : "" }));
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

function fmt_vec(v, digits) {
  return `(${v.map((x) => Number(x).toFixed(digits)).join(",")})`;
}

// 逐眼一行：来源给的是 2D 点就显示点，否则显示方向 + 该眼自己的俯仰/偏航。
// 为什么优先点：只做 2D 眼追的模块（如 PICO Connect 私有通道）没有方向模型，
// 方向字段是模块填的占位 -Z，照着显示会让两眼看起来完全一样，把真正的差别藏起来。
function fmt_eye(valid, dir, pitch, yaw, point, viewport) {
  if (!valid) {
    return "—";
  }
  if (point) {
    // viewport = 该眼视口归一化（ADR-0033）；否则是来源自己的原始坐标，带 ~ 区分
    const mark = viewport ? "2D" : "2D~";
    return `${mark} (${Number(point[0]).toFixed(2)},${Number(point[1]).toFixed(2)})`;
  }
  if (!dir) {
    return "—";
  }
  const text = fmt_vec(dir, 2);
  if (pitch === undefined || pitch === null || yaw === undefined || yaw === null) {
    return text; // 旧引擎只有方向，角度缺字段时不要显示假的 0.0°
  }
  return `${text} ${Number(pitch).toFixed(1)}°/${Number(yaw).toFixed(1)}°`;
}

// 逐眼来源：不同源或时间差 > 50 ms 时标黄，这类情况合并方向会悄悄偏一边
function eye_source_text(gaze) {
  const id = (v) => (Number(v) > 0 ? `#${Number(v)}` : "—");
  const age = (ts) => {
    const delta = Number(gaze.ts_ns) - Number(ts || 0);
    return gaze.ts_ns && ts && delta > 0 ? Math.round(delta / 1e6) : 0;
  };
  return (
    `L ${id(gaze.left_source)} ${age(gaze.left_capture_ts_ns)} ms / ` +
    `R ${id(gaze.right_source)} ${age(gaze.right_capture_ts_ns)} ms`
  );
}

// 有 2D 点就按来源的屏幕坐标画（[0,1]，原点左上），否则按俯仰/偏航映射。
// 两种坐标不能混着比：点没有角度，角度也反推不出点（缺 FOV/内参）。
function place_gaze_dot(el, valid, pitch, yaw, point) {
  if (!valid) {
    el.hidden = true;
    return;
  }
  let x;
  let y;
  if (point) {
    x = Number(point[0]) * 100;
    y = Number(point[1]) * 100;
  } else if (pitch !== undefined && pitch !== null && yaw !== undefined && yaw !== null) {
    x = 50 + (Number(yaw) / 30) * 50;
    y = 50 - (Number(pitch) / 20) * 50;
  } else {
    el.hidden = true;
    return;
  }
  el.style.left = `${Math.max(0, Math.min(100, x))}%`;
  el.style.top = `${Math.max(0, Math.min(100, y))}%`;
  el.hidden = false;
}

function render_gaze(gaze) {
  // valid = 有可用的合并方向；2D 点是否有效是另一件事（2D-only 源 valid 为假）
  const has_dir = !!(gaze && gaze.valid);
  const eye_point = (side) => {
    if (!gaze) {
      return false;
    }
    const per_eye = gaze[`${side}_point_valid`];
    return per_eye === undefined ? !!gaze.point2d_valid : !!per_eye; // 旧引擎只有全局位
  };
  const left_point = eye_point("left");
  const right_point = eye_point("right");
  const has_point = left_point || right_point;
  // 旧引擎没有 point_space 字段：当时约定就是"由来源定义"，按原始坐标处理更安全
  const point_viewport = !!(gaze && gaze.point_space === "viewport");
  const has_data = has_dir || has_point;
  const left_dir = has_dir && !!gaze.left_valid;
  const right_dir = has_dir && !!gaze.right_valid;
  const left_valid = left_dir || left_point;
  const right_valid = right_dir || right_point;

  $("g-dir").textContent = has_dir
    ? `(${gaze.dir_local.map((v) => Number(v).toFixed(2)).join(", ")})`
    : "—";
  $("g-eye-left").textContent = fmt_eye(
    left_valid, left_dir ? gaze.left_dir_local : null,
    gaze && gaze.left_pitch_deg, gaze && gaze.left_yaw_deg,
    left_point ? gaze.left_point_2d : null, point_viewport);
  $("g-eye-right").textContent = fmt_eye(
    right_valid, right_dir ? gaze.right_dir_local : null,
    gaze && gaze.right_pitch_deg, gaze && gaze.right_yaw_deg,
    right_point ? gaze.right_point_2d : null, point_viewport);
  $("g-pitch-yaw").textContent = has_dir
    ? `${Number(gaze.pitch_deg).toFixed(1)}° / ${Number(gaze.yaw_deg).toFixed(1)}°`
    : "—";
  $("g-vergence").textContent = has_dir && gaze.vergence_distance > 0
    ? `${Number(gaze.vergence_distance).toFixed(2)} m`
    : "—";
  // 没有方向就没有扫视/注视可算：只有 2D 点时显示 —，而不是"无眼追"或"注视中"
  $("g-fixation").textContent = has_dir
    ? (gaze.fixation
        ? t("status.gaze.fixating", { ms: Math.round(gaze.fixation_ms || 0) })
        : t("status.gaze.saccade_state"))
    : (has_point ? "—" : t("status.gaze.none"));
  $("g-saccade").textContent = has_dir
    ? `${Number(gaze.saccade_vel_deg_s || 0).toFixed(0)} °/s`
    : "—";
  $("g-confidence").textContent = has_data
    ? `${Math.round((gaze.confidence || 0) * 100)}%`
    : "—";
  $("g-source").textContent = has_data
    ? `${gaze.source || t("status.gaze.eye_fallback")} · ${Math.round(gaze.source_age_ms || 0)} ms`
    : "—";
  const eye_source = $("g-eye-source");
  eye_source.textContent = has_data ? eye_source_text(gaze) : "—";
  eye_source.classList.toggle(
    "warn",
    has_data &&
      (Number(gaze.left_source || 0) !== Number(gaze.right_source || 0) ||
        Math.abs(
          Number(gaze.left_capture_ts_ns || 0) - Number(gaze.right_capture_ts_ns || 0)) >
          50 * 1e6));
  $("g-head").textContent = gaze && gaze.head_pose
    ? `p (${gaze.head_pose.p.map((v) => Number(v).toFixed(2)).join(", ")})`
    : "—";

  place_gaze_dot($("g-dot-left"), left_valid,
    gaze && gaze.left_pitch_deg, gaze && gaze.left_yaw_deg,
    left_point ? gaze.left_point_2d : null);
  place_gaze_dot($("g-dot-right"), right_valid,
    gaze && gaze.right_pitch_deg, gaze && gaze.right_yaw_deg,
    right_point ? gaze.right_point_2d : null);
}

function sync_output_form() {
  const vrchat = $("o-vrchat-on").checked;
  const vmc = $("o-vmc-on").checked;
  const shm = $("o-shm-on").checked;
  const ws = $("o-ws-on").checked;
  const udp = $("o-udp-on").checked;
  $("o-vrchat").disabled = !vrchat;
  $("o-params-mode").disabled = !vrchat;
  $("o-eye-mode").disabled = !vrchat;
  $("o-vmc-target").disabled = !vmc;
  $("o-shm-name").disabled = !shm;
  $("o-ws-port").disabled = !ws;
  $("o-ws-rate").disabled = !ws;
  $("o-ws-all").disabled = !ws;
  $("o-udp-target").disabled = !udp;
  $("o-udp-rate").disabled = !udp;
  // 关闭的通道整卡变暗，配置项虽然仍可见，但一眼能看出这路没开
  $("oc-osc").classList.toggle("off", !vrchat);
  $("oc-vmc").classList.toggle("off", !vmc);
  $("oc-shm").classList.toggle("off", !shm);
  $("oc-ws").classList.toggle("off", !ws);
  $("oc-udp").classList.toggle("off", !udp);

  const osc_state = $("o-vrchat-state");
  osc_state.textContent = vrchat ? t("output.osc.on") : t("output.osc.off");
  osc_state.classList.toggle("on", vrchat);

  // 步进按钮跟随输入框：输入被通道开关禁用时也不能点
  document.querySelectorAll(".num-field").forEach((field) => {
    const input = field.querySelector("input");
    field.querySelectorAll(".num-step").forEach((button) => {
      button.disabled = input.disabled;
    });
  });
}

function set_outputs_dirty(dirty) {
  state.outputs_dirty = dirty;
  $("o-dirty").hidden = !dirty;
}

function apply_outputs(outputs) {
  if (!outputs) {
    return;
  }
  // 用户正在改表单时不要拿 10 Hz 的状态推送覆盖输入
  if (!state.outputs_dirty) {
    $("o-vrchat").value = outputs.vrchat || "";
    // 旧引擎没有这个字段：缺省按开启处理，和引擎默认值一致
    $("o-vrchat-on").checked = outputs.vrchat_enabled !== false;
    $("o-eye-mode").value = outputs.eye_mode || "parameters";
    $("o-params-mode").value = outputs.params_mode || "v2";
    $("o-vmc-on").checked = !!outputs.vmc_enabled;
    $("o-vmc-target").value = outputs.vmc_target || "";
    $("o-shm-on").checked = !!outputs.shm_enabled;
    $("o-shm-name").value = outputs.shm_name || "";
    $("o-ws-on").checked = !!outputs.ws_enabled;
    $("o-ws-port").value = outputs.ws_port || 9101;
    $("o-ws-rate").value = outputs.ws_rate ?? 30;
    $("o-ws-all").checked = !!outputs.ws_all;
    $("o-udp-on").checked = !!outputs.udp_enabled;
    $("o-udp-target").value = outputs.udp_target || "";
    $("o-udp-rate").value = outputs.udp_rate ?? 0;
  }
  sync_output_form();
}

function collect_outputs() {
  return {
    type: "set_outputs",
    vrchat: $("o-vrchat").value.trim(),
    vrchat_enabled: $("o-vrchat-on").checked,
    eye_mode: $("o-eye-mode").value,
    params_mode: $("o-params-mode").value,
    vmc_enabled: $("o-vmc-on").checked,
    vmc_target: $("o-vmc-target").value.trim(),
    shm_enabled: $("o-shm-on").checked,
    shm_name: $("o-shm-name").value.trim(),
    ws_enabled: $("o-ws-on").checked,
    ws_port: Number($("o-ws-port").value),
    ws_rate: Number($("o-ws-rate").value),
    ws_all: $("o-ws-all").checked,
    udp_enabled: $("o-udp-on").checked,
    udp_target: $("o-udp-target").value.trim(),
    udp_rate: Number($("o-udp-rate").value),
  };
}

function save_outputs(restart) {
  state.pending_outputs_restart = !!restart;
  set_outputs_dirty(true);
  send(collect_outputs());
}

// ---------- 输出预设 ----------

function refresh_profile_buttons() {
  const name = $("o-profile-select").value;
  $("o-profile-delete").disabled = !name;
  $("o-profile-apply").disabled = !name;
}

function render_output_profiles() {
  const select = $("o-profile-select");
  const had_options = select.options.length > 0;
  const keep = select.value;
  const profiles = state.output_profiles.profiles || [];
  const active = state.output_profiles.active || "";

  select.innerHTML = "";
  const custom = document.createElement("option");
  custom.value = "";
  custom.textContent = t("output.profile.custom");
  select.appendChild(custom);
  for (const name of profiles) {
    const option = document.createElement("option");
    option.value = name;
    option.textContent = name;
    select.appendChild(option);
  }

  if (had_options && (keep === "" || profiles.includes(keep))) {
    select.value = keep;
  } else if (active && profiles.includes(active)) {
    select.value = active;
  } else {
    select.value = "";
  }
  select.title = state.output_profiles.dir || "";
  refresh_profile_buttons();
}

function save_output_profile() {
  const name = $("o-profile-name").value.trim();
  if (!name) {
    show_toast(t("output.profile.name_required"), "error");
    $("o-profile-name").focus();
    return;
  }
  state.profile_pending = { action: "save", name };
  const payload = collect_outputs();
  payload.type = "save_output_profile";
  payload.name = name;
  send(payload);
}

async function apply_output_profile() {
  const name = $("o-profile-select").value;
  if (!name) {
    show_toast(t("output.profile.pick"), "error");
    return;
  }
  if (!(await ask_confirm({
    title: t("output.profile.apply"),
    message: t("output.profile.apply_confirm", { name }),
    danger: false,
  }))) {
    return;
  }
  state.profile_pending = { action: "apply", name };
  state.pending_outputs_restart = true;
  set_outputs_dirty(true);
  send({ type: "apply_output_profile", name });
}

async function delete_output_profile() {
  const name = $("o-profile-select").value;
  if (!name) {
    show_toast(t("output.profile.pick"), "error");
    return;
  }
  if (!(await ask_confirm({
    title: t("output.profile.delete"),
    message: t("output.profile.delete_confirm", { name }),
    danger: true,
  }))) {
    return;
  }
  state.profile_pending = { action: "delete", name };
  send({ type: "delete_output_profile", name });
}

function render_status(status) {
  state.status = status; // 留给调试与探针读最新一帧
  apply_outputs(status.outputs);
  $("m-fps").textContent = (status.fps || 0).toFixed(1);
  $("m-frames").textContent = fmt_int(status.frames_in);
  $("m-ticks").textContent = fmt_int(status.ticks);
  $("m-late").textContent = fmt_int(status.late_ticks);
  const work = status.tick_work || {};
  const fmt_us = (value) => {
    const us = Number(value || 0);
    return us >= 1000 ? `${(us / 1000).toFixed(2)} ms` : `${us.toFixed(us < 10 ? 1 : 0)} µs`;
  };
  $("p-tick").textContent = work.samples ? fmt_us(work.avg_us) : "—";
  $("p-p95").textContent = work.samples ? fmt_us(work.p95_us) : "—";
  const process = status.process || {};
  $("p-cpu").textContent = process.cpu_percent === undefined
    ? "—" : `${Number(process.cpu_percent).toFixed(0)}%`;
  $("p-mem").textContent = process.working_set_mb === undefined
    ? "—" : `${Number(process.working_set_mb).toFixed(1)} MB`;
  $("p-handles").textContent = process.handles === undefined ? "—" : fmt_int(process.handles);
  $("p-resumes").textContent = fmt_int(status.resumes || 0);
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
  $("a-source").textContent = status.avatar_source
    ? t(`status.avatar.source.${status.avatar_source}`)
    : "—";
  $("a-params").textContent = `${status.param_count || 0} / ${status.matched || 0}`;
  $("a-targets").textContent = status.target_count || 0;
  $("a-binary").textContent = status.binary_count || 0;

  $("o-mode").textContent = status.params_mode || "—";
  $("o-eye").textContent = status.eye_mode || "—";
  $("o-targets").textContent = status.target_count || 0;
  $("o-matched").textContent = `${status.matched || 0}${t("output.matched.suffix")}`;
  $("o-binary").textContent = status.binary_count || 0;
  const vmc_out = status.vmc_out || {};
  const vmc_state = $("o-vmc");
  vmc_state.textContent = vmc_out.enabled
    ? `${vmc_out.target} · ${fmt_int(vmc_out.messages)}`
    : t("output.vmc.off");
  vmc_state.classList.toggle("on", !!vmc_out.enabled);
  const shm_out = status.shm_out || {};
  const shm_state = $("o-shm");
  shm_state.textContent = shm_out.enabled
    ? `${shm_out.name} · ${fmt_int(shm_out.messages)}`
    : t("output.shm.off");
  shm_state.classList.toggle("on", !!shm_out.enabled);
  const ws_out = status.ws_out || {};
  const ws_broken = !!ws_out.enabled && ws_out.listening === false;
  const ws_state = $("o-ws");
  ws_state.textContent = ws_out.enabled
    ? (ws_broken
        ? `${ws_out.target} · ${t("output.ws.failed")}`
        : `${ws_out.target} · ${fmt_int(ws_out.messages)} · ${ws_out.clients || 0} ${t("output.clients")}`)
    : t("output.ws.off");
  ws_state.classList.toggle("on", !!ws_out.enabled && !ws_broken);
  ws_state.classList.toggle("err", ws_broken);
  const udp_out = status.udp_out || {};
  const udp_state = $("o-udp");
  udp_state.textContent = udp_out.enabled
    ? `${udp_out.target} · ${fmt_int(udp_out.messages)}`
    : t("output.udp.off");
  udp_state.classList.toggle("on", !!udp_out.enabled);
  $("c-source").textContent = `${status.source || "—"} · ${(status.fps || 0).toFixed(1)} fps`;
  $("s-clients").textContent = status.clients || 0;
  if (state.gaze) {
    render_gaze(state.gaze);
  }

  if (state.view === "modules") {
    render_running_modules(status.modules);
    // 实例行的「停止/启动」要跟着实时状态翻转，不能只在 modules 消息时更新
    update_instance_controls();
    // 卡片上的状态 chip 同理：模块跑起来后 modules 消息不会重推
    update_module_chips(status.modules);
  }
  render_sources(status.sources || []);
  render_role_hint();

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
    paint_range(cutoff);
    paint_range(beta);
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
        gaze: chip("gaze"),
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
    for (const key of ["expression", "eyes", "gaze", "head"]) {
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

// 与 sdk/fgb_module_api.h 的 FGB_CAP_* 对齐（ABI v2 是唯一 ABI，ADR-0034）
const CAP_EYES = 0x1;
const CAP_EXPRESSION = 0x2;
const CAP_HEAD = 0x4;
const CAP_GAZE = 0x8;
const ROLE_KEYS = ["expression", "eyes", "gaze", "head"];
const ROLE_CAPS = {
  expression: CAP_EXPRESSION,
  eyes: CAP_EYES,
  gaze: CAP_GAZE,
  head: CAP_HEAD,
};

function capabilities_of(item) {
  return Number((item && (item.capabilities || item.declared_capabilities)) || 0);
}

function capability_chips(item) {
  const caps = capabilities_of(item);
  const chips = [];
  for (const key of ROLE_KEYS) {
    if (caps & ROLE_CAPS[key]) {
      chips.push(el("span", "chip cap", t(`channel.${key}`)));
    }
  }
  return chips;
}

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

  render_instance_section(data || {});
  render_running_modules(state.status ? state.status.modules : []);
  render_importable((data && data.importable) || []);
}

// 模块图标：清单带 ModuleIcon/icon.*（或 DLL 内嵌图标）时由 /module-icon/<id> 提供。
// 没有图标才渲染首字母头像；有图时不画底色，半透明 PNG 才能直接融进卡片背景。
function module_icon(item, label) {
  const wrap = el("span", "module-icon");
  const text = String(label || (item && (item.name || item.id)) || "?").trim();
  wrap.append(el("span", "module-icon-fallback", (text[0] || "?").toUpperCase()));
  if (item && item.icon) {
    const img = document.createElement("img");
    img.alt = "";
    img.decoding = "async";
    img.src = `/module-icon/${encodeURIComponent(item.id)}` +
      (item.version ? `?v=${encodeURIComponent(item.version)}` : "");
    img.addEventListener("load", () => wrap.classList.add("has-icon"));
    img.addEventListener("error", () => img.remove());
    wrap.append(img);
  }
  return wrap;
}

function library_chevron() {
  const chevron = el("span", "chevron");
  chevron.innerHTML = '<svg viewBox="0 0 16 16" aria-hidden="true"><path d="M4 6l4 4 4-4"/></svg>';
  return chevron;
}

function toggle_expanded(card, set, id, event) {
  if (event.type === "click" && event.target.closest("button, input, a, select, label")) {
    return;
  }
  if (event.type === "keydown" && event.key !== "Enter" && event.key !== " ") {
    return;
  }
  event.preventDefault();
  const open = !card.classList.contains("expanded");
  card.classList.toggle("expanded", open);
  if (open) {
    set.add(id);
  } else {
    set.delete(id);
  }
}

// 使用说明与模块页：链接不能写成 <a href>，否则 WebView2 会自己导航走、界面直接丢，
// 统一交给引擎用系统默认浏览器打开。
function module_details_block(usage, page) {
  const fragment = document.createDocumentFragment();
  if (usage) {
    const section = el("div", "module-detail");
    section.append(el("div", "module-detail-label", t("modules.usage")));
    section.append(el("p", "module-detail-text", usage));
    fragment.append(section);
  }
  if (page) {
    const section = el("div", "module-detail");
    section.append(el("div", "module-detail-label", t("modules.page")));
    const link = el("button", "module-page-link", page);
    link.title = t("modules.page_open");
    link.addEventListener("click", (event) => {
      event.stopPropagation();
      send({ type: "open_module_page", url: page });
    });
    section.append(link);
    fragment.append(section);
  }
  return fragment;
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
  // 运行态以 1Hz 的 status 为准：modules 消息只在配置变化时推，模块从
  // 「初始化中」变「运行中」不会触发它，照抄 item.state 会让卡片停在旧文案。
  const live = running_state_of(item.id);
  if (live) {
    return format_state(live);
  }
  if (item.running && !state.status) {
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

  const head = el("div", "module-head module-head-card");
  head.append(module_icon(item));
  const title = el("div", "module-title");
  title.append(el("strong", "", item.name || item.id));
  if (item.version) {
    title.append(el("span", "module-ver", `v${item.version}`));
  }
  if (item.author) {
    title.append(el("span", "module-author", item.author));
  }
  const badges = el("div", "module-badges");
  badges.append(el("span", "chip", item.kind === "native"
    ? t("modules.kind.native") : t("modules.kind.vrcft")));
  for (const node of capability_chips(item)) {
    badges.append(node);
  }
  if (item.host_compatible === false) {
    // 清单声明的 ABI/宿主版本和当前对不上：不用等到启动失败才发现
    const warn = el("span", "chip err", t("modules.incompatible"));
    warn.title = t("modules.incompatible.hint");
    badges.append(warn);
  }
  const chip_state = module_chip(item);
  const chip = el("span", `chip ${chip_state.cls}`.trim(), chip_state.text);
  chip.dataset.role = "chip";
  badges.append(chip);
  const chevron = el("span", "chevron");
  chevron.innerHTML = '<svg viewBox="0 0 16 16" aria-hidden="true"><path d="M4 6l4 4 4-4"/></svg>';
  head.append(title, badges, chevron);
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
  details.append(module_details_block(item.usage, item.page));

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

  const toggle = el("label", "switch mod-toggle");
  const checkbox = document.createElement("input");
  checkbox.type = "checkbox";
  checkbox.checked = !!item.enabled;
  checkbox.addEventListener("change", () => {
    send({ type: "set_module_enabled", id: item.id, enabled: checkbox.checked });
  });
  toggle.append(checkbox, el("span", "", t("modules.enabled")));
  actions.append(toggle);

  const buttons = el("div", "module-buttons");
  // 一旦配置了 [instances]，启动内容以实例列表为准；active 只是旧版单模块回退，
  // 再显示"设为启动"会和实例列表互相打架。
  const has_instances = ((state.modules && state.modules.instances) || []).length > 0;
  if (!has_instances) {
    if (!item.active) {
      const set_button = el("button", "btn ghost small", t("modules.set_active"));
      set_button.addEventListener("click", () => send({ type: "set_module", id: item.id }));
      buttons.append(set_button);
    } else {
      buttons.append(el("span", "module-current", t("modules.current")));
    }
  }

  const add_instance = el("button", "btn ghost small", t("modules.add_instance"));
  add_instance.addEventListener("click", () => {
    const instances = (state.modules && state.modules.instances) || [];
    // 原生模块默认进独立进程（崩溃隔离 + 可热替换 DLL）；VRCFT 模块本来就跑在
    // ModuleProcess.exe 里，这个字段对它没有意义，带 false 只是为了显式。
    send({
      type: "add_instance",
      id: item.id,
      name: default_instance_name(item, instances),
      host_process: item.kind === "native",
    });
  });
  buttons.append(add_instance);

  const remove = el("button", "btn danger small", t("modules.delete"));
  remove.addEventListener("click", async () => {
    // 只有实例真的在跑（DLL 被锁）才需要提示"会先停实例"；停着的模块直接删
    const running = module_running(item.id);
    const text = running
      ? t("modules.delete_confirm_running", { name: item.name || item.id })
      : t("modules.delete_confirm", { name: item.name || item.id });
    if (await ask_confirm({ title: t("confirm.delete_title"), message: text, danger: true })) {
      send({ type: "delete_module", id: item.id });
    }
  });
  buttons.append(remove);
  actions.append(buttons);
  card.append(actions);

  const toggle_card = (event) => toggle_expanded(card, state.expanded_modules, item.id, event);
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

// 这个模块现在是不是真的被某个实例加载着（DLL 被锁）。
// 卡片上的 running 只在 modules 消息时刷新，实时状态要看 1 Hz 的 status。
function module_running(id) {
  const live = running_state_of(id);
  if (live) {
    return live === "active" || live === "starting" || live === "error";
  }
  const item = ((state.modules && state.modules.items) || []).find((entry) => entry.id === id);
  return !!(item && item.running);
}

// 安装/更新前的确认文案。更新正在跑的模块时，弹窗必须写明"会先停实例、装完自动拉起来"：
// 用户点确定就是授权后台去停，而不是装到一半再让他自己去实例行点「停止」。
function install_question(item, kind, update, version) {
  const name = item.name || item.id;
  const target = version || item.version || (item.latest && item.latest.version) || "";
  let text;
  if (update && target) {
    text = t("modules.library.confirm_update", { name, version: target });
  } else if (kind === "native") {
    text = t("modules.native.confirm", { name, version: target });
  } else {
    text = t("modules.library.confirm", { name });
  }
  if (module_running(item.id)) {
    text += " " + t("modules.library.auto_stop");
  }
  return text;
}

function rating_stars(value) {
  const full = Math.round(value || 0);
  return "★".repeat(Math.max(0, Math.min(5, full))) + "☆".repeat(Math.max(0, 5 - full));
}

function library_card(item, installed, status) {
  const card = el("article", "lib-card");
  card.dataset.id = item.id;
  card.tabIndex = 0;
  if (state.expanded_library.has(item.id)) {
    card.classList.add("expanded");
  }

  const head = el("div", "module-head");
  head.append(module_icon(installed || item, item.name));
  head.append(el("strong", "", item.name || item.id));
  head.append(el("span", "module-ver", `v${item.version || "?"}`));
  if (item.author) {
    head.append(el("span", "module-author", item.author));
  }
  const relation = library_relation(installed, item);
  if (relation !== "install") {
    const badges = el("div", "module-badges");
    badges.append(el("span", `chip ${relation === "update" ? "warn" : "ok"}`.trim(),
      relation === "update" ? t("modules.library.update_available") : t("modules.library.installed")));
    head.append(badges);
  }
  head.append(library_chevron());
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

  const details = el("div", "lib-details");
  details.append(module_details_block(item.usage, item.page));
  card.append(details);

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
      const question = install_question(item, "vrcft", relation === "update");
      if (await ask_confirm({ title: t("confirm.install_title"), message: question, danger: true })) {
        send({ type: "install_library", id: item.id });
      }
    });
    actions.append(button);
  }
  card.append(actions);
  update_library_card_progress(card, status, item.id);

  const toggle_card = (event) => toggle_expanded(card, state.expanded_library, item.id, event);
  card.addEventListener("click", toggle_card);
  card.addEventListener("keydown", toggle_card);
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

// ---------- 原生模块库（fgb-registry，registry schema v2） ----------

function native_snapshot() {
  return (state.library && state.library.native) || null;
}

// 注册表允许 i18n 覆盖 summary/description/usage，键是 BCP-47 语言标签；
// 缺省回落到顶层同名字段（schema 这么定义），所以这里只负责挑最合适的一条。
function native_text(item, field) {
  const map = (item && item.i18n) || {};
  const candidates = lang === "zh"
    ? ["zh", "zh-CN", "zh-Hans", "zh-Hans-CN"]
    : (lang === "ja" ? ["ja", "ja-JP"] : ["en", "en-US", "en-GB"]);
  for (const key of candidates) {
    const text = map[key] && map[key][field];
    if (text) {
      return text;
    }
  }
  for (const key of Object.keys(map)) {
    if (key.split("-")[0] === lang && map[key][field]) {
      return map[key][field];
    }
  }
  return (item && item[field]) || "";
}

// 库条目的图标是远程 https（registry 的 icon 字段）；已安装的模块优先用本地
// /module-icon/<id>，这样离线时也有图。加载失败就退回首字母头像。
function native_icon(item, installed) {
  const wrap = el("span", "module-icon");
  const text = String((item && item.name) || (item && item.id) || "?").trim();
  wrap.append(el("span", "module-icon-fallback", (text[0] || "?").toUpperCase()));
  let src = "";
  if (item.icon && /^https:\/\//i.test(item.icon)) {
    src = item.icon;
  } else if (installed) {
    src = `/module-icon/${encodeURIComponent(item.id)}`;
  }
  if (!src) {
    return wrap;
  }
  const img = document.createElement("img");
  img.alt = "";
  img.decoding = "async";
  img.referrerPolicy = "no-referrer";
  img.src = src;
  img.addEventListener("load", () => wrap.classList.add("has-icon"));
  img.addEventListener("error", () => img.remove());
  wrap.append(img);
  return wrap;
}

function native_relation(installed, item) {
  if (!installed || !item.latest || !item.latest.version) {
    return "install";
  }
  const cmp = compare_versions(installed.version, item.latest.version);
  // 版本号读不出来的旧安装当作已装，避免永远挂一个"更新"按钮
  return cmp === null ? "installed" : (cmp < 0 ? "update" : "installed");
}

function native_incompatible_text(item) {
  const requires = item.requires || {};
  const abi = requires.abi || (item.latest && item.latest.abi_version) || 0;
  switch (item.incompatible_code) {
  case "abi":
    return t("modules.native.incompatible.abi", { abi });
  case "host_version":
    return t("modules.native.incompatible.host_version", { version: requires.min_host_version || "?" });
  case "os":
    return t("modules.native.incompatible.os");
  case "arch":
    return t("modules.native.incompatible.arch");
  default:
    return item.incompatible_reason || t("modules.native.incompatible.unknown");
  }
}

function native_detail_block(label, text) {
  const section = el("div", "module-detail");
  section.append(el("div", "module-detail-label", label));
  section.append(el("p", "module-detail-text", text));
  return section;
}

function native_library_card(item, installed, status) {
  const card = el("article", "lib-card");
  card.dataset.id = item.id;
  card.tabIndex = 0;
  if (state.expanded_library.has(item.id)) {
    card.classList.add("expanded");
  }

  const relation = native_relation(installed, item);
  const head = el("div", "module-head");
  head.append(native_icon(item, installed));
  head.append(el("strong", "", item.name || item.id));
  const shown_version = installed && installed.version ? installed.version : item.latest.version;
  if (shown_version) {
    head.append(el("span", "module-ver", `v${shown_version}`));
  }
  if (item.author) {
    head.append(el("span", "module-author", item.author));
  }
  // 徽标要成组紧跟在标题后面：.chip 默认 margin-left:auto，逐个塞进 head 会被
  // flex 推到各处（"已审核"飘中间、"已安装"贴到右边），看着像坏掉了。
  const badges = el("div", "module-badges");
  if (item.verified) {
    badges.append(el("span", "chip ok", t("modules.native.verified")));
  }
  if (relation === "update") {
    badges.append(el("span", "chip warn", t("modules.library.update_available")));
  } else if (relation === "installed") {
    badges.append(el("span", "chip ok", t("modules.library.installed")));
  }
  if (item.latest.channel === "beta") {
    badges.append(el("span", "chip warn", t("modules.native.beta")));
  }
  if (item.archived) {
    badges.append(el("span", "chip", t("modules.native.archived")));
  }
  if (badges.childElementCount) {
    head.append(badges);
  }
  head.append(library_chevron());
  card.append(head);

  const summary = native_text(item, "summary");
  if (summary) {
    card.append(el("p", "lib-desc", summary));
  }

  const meta = el("div", "lib-meta");
  const capabilities = item.capabilities || [];
  if (capabilities.length) {
    // 能力标签自己成组，避免 .chip 的 margin-left:auto 把整行拆散
    const caps = el("span", "lib-caps");
    for (const capability of capabilities) {
      caps.append(el("span", "chip cap", t(`channel.${capability}`)));
    }
    meta.append(caps);
  }
  const downloads = (item.stats && item.stats.downloads) || 0;
  meta.append(el("span", "", t("modules.library.downloads", { count: fmt_int(downloads) })));
  if (item.updated) {
    meta.append(el("span", "", item.updated.slice(0, 10)));
  }
  if (item.license) {
    meta.append(el("span", "", item.license));
  }
  card.append(meta);

  const details = el("div", "lib-details");
  const description = native_text(item, "description");
  if (description) {
    details.append(native_detail_block(t("modules.native.description"), description));
  }
  details.append(module_details_block(native_text(item, "usage"), item.homepage));
  if (item.latest.notes) {
    details.append(native_detail_block(t("modules.native.release_notes"), item.latest.notes));
  }

  // 版本列表：注册表按版本倒序给，用来降级/回滚（升级走上面的「更新」按钮）
  const versions = item.versions || [];
  if (versions.length > 1) {
    const row = el("div", "module-detail");
    row.append(el("div", "module-detail-label", t("modules.native.pick_version")));
    const select = document.createElement("select");
    select.className = "native-version-select";
    for (const version of versions) {
      const option = document.createElement("option");
      option.value = version.version;
      option.textContent = version.channel === "beta"
        ? `${version.version} (beta)`
        : version.version;
      select.append(option);
    }
    select.value = item.latest.version;
    const install = el("button", "btn small", t("modules.native.install_version"));
    const percent = el("span", "muted", "");
    install.addEventListener("click", async () => {
      const version = select.value;
      if (!version) {
        return;
      }
      // 版本下拉是"装回/切到某个版本"，可能还是降级；这里按安装说，
      // 不写"更新到"，否则用户会以为版本号只能往上走
      const question = install_question(item, "native", false, version);
      if (await ask_confirm({
        title: t("confirm.install_title"),
        message: question,
        danger: true,
      })) {
        send({ type: "install_native_library", id: item.id, version });
      }
    });
    const option_row = el("div", "native-version-row");
    option_row.append(select, install, percent);
    row.append(option_row);
    details.append(row);
  }
  card.append(details);

  const actions = el("div", "lib-actions");
  const busy_other = status && (status.phase === "downloading" || status.phase === "installing") &&
    status.install_id !== item.id;
  const installing = status && status.install_id === item.id &&
    (status.phase === "downloading" || status.phase === "installing");
  if (installing) {
    const progress = el("div", "progress");
    progress.append(document.createElement("i"));
    const label = el("span", "muted", "");
    label.dataset.role = "lib-state";
    actions.append(progress, label);
  } else if (!item.compatible) {
    actions.append(el("span", "chip err", native_incompatible_text(item)));
  } else if (relation === "installed") {
    // 头部徽标已经写了"已安装"，这里再放一个灰按钮只是噪音：换成版本提示，
    // 想换版本可以展开用上面的版本下拉。
    actions.append(el("span", "muted", t("modules.native.up_to_date", {
      version: (installed && installed.version) || item.latest.version,
    })));
  } else {
    const button = el("button", "btn small", "");
    button.textContent = relation === "update" ? t("modules.library.update")
                                               : t("modules.library.install");
    button.disabled = !!busy_other;
    button.addEventListener("click", async () => {
      const question = install_question(item, "native", relation === "update", item.latest.version);
      if (await ask_confirm({
        title: t("confirm.install_title"),
        message: question,
        danger: true,
      })) {
        send({ type: "install_native_library", id: item.id, version: item.latest.version });
      }
    });
    actions.append(button);
    if (item.archived) {
      actions.append(el("span", "muted", t("modules.native.archived_hint")));
    }
  }
  card.append(actions);
  update_library_card_progress(card, status, item.id);

  const toggle_card = (event) => toggle_expanded(card, state.expanded_library, item.id, event);
  card.addEventListener("click", toggle_card);
  card.addEventListener("keydown", toggle_card);
  return card;
}

// 已装但库里没有的原生模块（本地 zip 装的、库条目被撤下的）也要能看见，
// 否则用户在「原生模块」页找不到自己刚装的东西。
function native_installed_card(item) {
  const card = el("article", "lib-card");
  card.dataset.id = item.id;
  card.tabIndex = 0;
  if (state.expanded_library.has(item.id)) {
    card.classList.add("expanded");
  }
  const head = el("div", "module-head");
  head.append(module_icon(item));
  head.append(el("strong", "", item.name || item.id));
  if (item.version) {
    head.append(el("span", "module-ver", `v${item.version}`));
  }
  if (item.author) {
    head.append(el("span", "module-author", item.author));
  }
  const chip_state = item.broken ? { text: t("state.broken"), cls: "err" } : module_chip(item);
  head.append(el("span", `chip ${chip_state.cls}`.trim(), chip_state.text));
  head.append(library_chevron());
  card.append(head);
  if (item.description) {
    card.append(el("p", "lib-desc", item.description));
  }
  const meta = el("div", "lib-meta");
  meta.append(el("span", "", `${t("modules.source")} ${source_label(item.source)}`));
  meta.append(el("span", "", `${t("modules.installed_at")} ${item.installed_at || "—"}`));
  meta.append(el("span", "", `${t("modules.size")} ${fmt_mb(item.bytes)}`));
  card.append(meta);

  const details = el("div", "lib-details");
  details.append(module_details_block(item.usage, item.page));
  card.append(details);

  const actions = el("div", "lib-actions");
  const manage = el("button", "btn small", t("modules.library.open_modules"));
  manage.addEventListener("click", () => switch_page("modules"));
  actions.append(manage);
  card.append(actions);

  const toggle_card = (event) => toggle_expanded(card, state.expanded_library, item.id, event);
  card.addEventListener("click", toggle_card);
  card.addEventListener("keydown", toggle_card);
  return card;
}

function render_native_library() {
  const list = $("native-library-list");
  const status_el = $("lib-status");
  const filter = ($("lib-search").value || "").trim().toLowerCase();
  const show_beta = $("lib-native-beta").checked;
  const native = native_snapshot();
  const items = (native && native.items) || [];
  const registry_status = (native && native.status) || {};

  const installed_all = ((state.modules && state.modules.items) || [])
    .filter((item) => item.kind === "native");
  const installed_by_id = new Map(installed_all.map((item) => [String(item.id).toLowerCase(), item]));
  const entry_ids = new Set(items.map((item) => String(item.id).toLowerCase()));
  const local_only = installed_all.filter((item) => !entry_ids.has(String(item.id).toLowerCase()));

  if (!native) {
    status_el.textContent = t("modules.native.not_loaded");
    status_el.title = "";
    list.textContent = "";
    list.append(el("div", "empty", t("modules.native.not_loaded")));
    return;
  }

  if (registry_status.phase === "downloading" || registry_status.phase === "installing") {
    const percent = registry_status.total > 0
      ? Math.min(100, (registry_status.received / registry_status.total) * 100)
      : 0;
    status_el.textContent = registry_status.phase === "installing"
      ? t("modules.library.installing")
      : t("modules.library.downloading", {
          percent: percent.toFixed(0),
          received: (registry_status.received / 1048576).toFixed(1),
          total: registry_status.total ? (registry_status.total / 1048576).toFixed(1) : "?",
        });
  } else if (registry_status.phase === "error") {
    status_el.textContent = t("modules.library.error", { error: registry_status.error || "?" });
  } else if (registry_status.refreshing) {
    status_el.textContent = t("modules.library.refreshing");
  } else if (registry_status.refresh_error) {
    status_el.textContent = t("modules.library.error", { error: registry_status.refresh_error });
  } else if (registry_status.stale) {
    status_el.textContent = t("modules.library.stale", { time: registry_status.fetched_at || "—" });
  } else {
    status_el.textContent = t("modules.native.count", {
      count: items.length,
      installed: installed_all.length,
    });
  }
  status_el.title = registry_status.error || registry_status.refresh_error || "";
  $("lib-refresh").disabled = !!registry_status.refreshing;

  const matches = (item) => {
    const text = `${item.name} ${item.author} ${native_text(item, "summary")} ` +
      `${native_text(item, "description")} ${(item.tags || []).join(" ")}`.toLowerCase();
    return !filter || text.includes(filter);
  };
  const visible = items.filter((item) => {
    if (item.latest.channel === "beta" && !show_beta) {
      return false;
    }
    return matches(item);
  });
  const local_visible = local_only.filter((item) => {
    const text = `${item.name} ${item.author} ${item.description}`.toLowerCase();
    return !filter || text.includes(filter);
  });

  list.textContent = "";
  if (!visible.length && !local_visible.length) {
    list.append(el("div", "empty", items.length || local_only.length
      ? t("modules.library.no_match") : t("modules.native.empty")));
    return;
  }
  for (const item of visible) {
    const installed = installed_by_id.get(String(item.id).toLowerCase());
    list.append(native_library_card(item, installed, registry_status));
  }
  for (const item of local_visible) {
    list.append(native_installed_card(item));
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
  $("lib-refresh").hidden = false;
  $("lib-native-beta-wrap").hidden = !native;
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
function default_instance_name(item, instances) {
  const raw = (item.name || item.id || "module").trim();
  const base = raw.replace(/[=\[\]\r\n#;]/g, "-").slice(0, 40) || "module";
  let name = base;
  let suffix = 2;
  const taken = new Set((instances || []).map((instance) => String(instance.name).toLowerCase()));
  while (taken.has(name.toLowerCase())) {
    name = `${base}-${suffix++}`;
  }
  return name;
}

function role_options(role) {
  const options = new Map();
  const items = (state.modules && state.modules.items) || [];
  const instances = (state.modules && state.modules.instances) || [];
  const running = (state.status && state.status.modules) || [];
  const bit = ROLE_CAPS[role];
  const add = (value, label, caps) => {
    if (!value || options.has(value)) {
      return;
    }
    if (caps && bit && !(caps & bit)) {
      return;
    }
    options.set(value, label);
  };

  for (const instance of instances) {
    const item = items.find((entry) => entry.id === instance.module_id);
    add(instance.module_id, `${instance.name} · ${instance.module_name || instance.module_id}`,
        capabilities_of(item));
  }
  for (const entry of running) {
    add(entry.module_id, `${entry.name} · ${entry.module_name || entry.module_id}`,
        Number(entry.capabilities || 0));
  }
  return options;
}

// 实例行里「停止/启动」按钮的字面值必须跟着实时状态走，
// 否则点完停止按钮还是"停止"，看起来像没生效（modules 消息只在配置变化时推）。
function instance_running_entry(instance) {
  const running = (state.status && state.status.modules) || [];
  return running.find((entry) =>
    entry.module_id === instance.module_id &&
    (entry.name === instance.name || entry.name.endsWith(`:${instance.name}`)));
}

function instance_is_live(instance) {
  const entry = instance_running_entry(instance);
  return !!entry && entry.state !== "stopped";
}

function update_instance_controls() {
  for (const controls of Object.values(state.instance_controls || {})) {
    const entry = instance_running_entry(controls.instance);
    // 新增实例只在引擎启动时加载：没重启引擎前它不在运行列表里，点启动/重启
    // 只会收到 "unknown instance"。这里直接禁用，并把原因写在 tooltip 上。
    const known = !!state.status && state.connected;
    const loaded = !!entry;
    const live = loaded && entry.state !== "stopped";
    controls.toggle.textContent = live ? t("modules.instance.stop") : t("modules.instance.start");
    controls.toggle.dataset.role = live ? "stop" : "start";
    const hint = known && !loaded ? t("modules.instance.not_loaded_hint") : "";
    controls.toggle.title = hint;
    controls.restart.title = hint;
    controls.toggle.disabled = !!controls.busy || (known && !loaded);
    controls.restart.disabled = !!controls.busy || (known && !loaded);
    if (controls.pending) {
      controls.pending.hidden = !(known && !loaded);
    }
  }
}

// 停止/重启要等宿主退出+重新就绪，几百毫秒内再点一次只会重复排队；
// 先禁用一小会儿，状态消息回来后仍会按真实状态刷新文字。
function lock_instance_controls(name) {
  const controls = (state.instance_controls || {})[name];
  if (!controls) {
    return;
  }
  controls.busy = true;
  update_instance_controls();
  setTimeout(() => {
    const latest = (state.instance_controls || {})[name];
    if (latest) {
      latest.busy = false;
      update_instance_controls();
    }
  }, 2500);
}

function render_instance_section(data) {
  const list = $("instance-list");
  const grid = $("role-grid");
  if (!list || !grid) {
    return;
  }
  const instances = data.instances || [];
  state.instance_controls = {};
  list.textContent = "";
  if (!instances.length) {
    list.append(el("div", "muted", t("modules.instances.empty")));
  } else {
    for (const instance of instances) {
      const row = el("div", "instance-row");
      row.append(el("b", "instance-name", instance.name));
      row.append(el("span", "instance-module", instance.module_name || instance.module_id));
      // 「独立进程」只对原生模块有意义；VRCFT 模块永远跑在 ModuleProcess.exe 里，
      // 给它一个开关只会让人以为能切回进程内。
      if (instance.kind === "native") {
        const host = el("label", "switch instance-host");
        host.title = t("modules.instance.host_hint");
        const host_checkbox = document.createElement("input");
        host_checkbox.type = "checkbox";
        host_checkbox.checked = !!instance.host_process;
        host_checkbox.addEventListener("change", () => {
          send({
            type: "set_instance_host",
            name: instance.name,
            host_process: host_checkbox.checked,
          });
        });
        host.append(host_checkbox, el("span", "", t("modules.instance.host")));
        row.append(host);
      } else {
        row.append(el("span", "muted instance-host-fixed", t("modules.instance.vrcft_host")));
      }
      if (!instance.enabled) {
        row.append(el("span", "chip err", t("modules.instance.unavailable")));
      }
      // 配置里有、引擎还没加载（刚添加）：告诉用户差一步重启引擎
      const pending = el("span", "chip warn", t("modules.instance.not_loaded"));
      pending.hidden = true;
      row.append(pending);

      const toggle = el("button", "btn ghost small", t("modules.instance.start"));
      toggle.dataset.role = "start";
      // 点击时再算 live：渲染到点击之间模块可能已经崩溃/自愈了
      toggle.addEventListener("click", () => {
        const live = instance_is_live(instance);
        send({ type: live ? "stop_instance" : "start_instance", name: instance.name });
        lock_instance_controls(instance.name);
      });
      const restart = el("button", "btn ghost small", t("modules.instance.restart"));
      restart.dataset.role = "restart";
      restart.addEventListener("click", () => {
        send({ type: "restart_instance", name: instance.name });
        lock_instance_controls(instance.name);
      });
      row.append(toggle, restart);
      state.instance_controls[instance.name] = { instance, toggle, restart, pending, busy: false };
      const remove = el("button", "btn danger small", t("modules.instance.remove"));
      remove.addEventListener("click", async () => {
        const text = t("modules.instance.remove_confirm", { name: instance.name });
        if (await ask_confirm({
          title: t("modules.instance.remove_title"),
          message: text,
          danger: true,
        })) {
          send({ type: "remove_instance", name: instance.name });
        }
      });
      row.append(remove);
      list.append(row);
    }
    update_instance_controls();
  }

  const roles = data.roles || {};
  grid.textContent = "";
  for (const role of ROLE_KEYS) {
    const row = el("div", "role-row");
    row.append(el("span", "role-label", t(`role.${role}`)));
    const select = document.createElement("select");
    select.className = "select role-select";
    select.dataset.role = role;
    const auto = document.createElement("option");
    auto.value = "";
    auto.textContent = t("modules.roles.auto");
    select.append(auto);

    const values = role_options(role);
    for (const [value, label] of values) {
      const option = document.createElement("option");
      option.value = value;
      option.textContent = label;
      select.append(option);
    }
    const saved = roles[role] || "";
    if (saved && !values.has(saved)) {
      // 配置指向的模块没有在本次运行里出现：仍显示出来，让用户知道配置没丢。
      const option = document.createElement("option");
      option.value = saved;
      option.textContent = `${saved} · ${t("modules.roles.not_running")}`;
      select.append(option);
    }
    select.value = saved;
    select.addEventListener("change", () => {
      const payload = { type: "set_module_roles" };
      for (const key of ROLE_KEYS) {
        const node = grid.querySelector(`.role-select[data-role="${key}"]`);
        payload[key] = node ? node.value : "";
      }
      send(payload);
    });
    row.append(select);
    grid.append(row);
  }
}

function render_role_hint() {
  const hint = $("role-hint");
  if (!hint) {
    return;
  }
  const status = state.status;
  if (!status || !status.frames_in) {
    hint.hidden = true;
    return;
  }
  const sources = status.sources || [];
  const missing = [];
  for (const role of ROLE_KEYS) {
    if (!sources.some((source) => source[role])) {
      missing.push(t(`role.${role}`));
    }
  }
  if (!missing.length) {
    hint.hidden = true;
    return;
  }
  hint.hidden = false;
  hint.className = "role-hint warn";
  hint.textContent = t("modules.roles.missing", { roles: missing.join(" / ") });
}

function render_running_modules(modules) {
  const list = $("running-list");
  if (!list) {
    return;
  }
  const running = modules || [];

  if (!running.length) {
    list.textContent = "";
    delete list.dataset.signature;
    state.running_rows = [];
    list.append(el("div", "muted", t("modules.running.empty")));
    return;
  }

  // 按实例身份而不是行数判断是否重建：删掉一个再换成另一个时行数不变，
  // 只看数量会把旧实例名留在界面上，chip 却已经来自新实例。
  const signature = running.map((item) => `${item.name}\u0000${item.module_id}`).join("\u0001");
  if (list.dataset.signature !== signature) {
    list.dataset.signature = signature;
    list.textContent = "";
    state.running_rows = running.map(() => {
      const row = el("div", "running-row");
      const name = el("b", "running-name", "");
      const module_name = el("span", "running-module", "");
      const chip = el("span", "chip", "");
      row.append(name, module_name, chip);
      list.append(row);

      const extra = el("div", "running-extra");
      list.append(extra);
      return { row, name, module_name, chip, extra };
    });
  }

  running.forEach((item, index) => {
    const ui = state.running_rows[index];
    if (!ui) {
      return;
    }
    ui.name.textContent = item.name;
    ui.module_name.textContent = item.module_name || item.module_id;
    const chip_state = format_state(item.state);
    ui.chip.className = `chip ${chip_state.cls}`.trim();
    ui.chip.textContent = chip_state.text;

    const parts = [];
    if (item.host_process) {
      parts.push(t("modules.running.host"));
    }
    parts.push(t("modules.restarts", { count: item.restarts || 0 }));
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

// 不重建 DOM 地刷新已装模块卡片的 chip 与错误行：modules 消息只在配置变化时推，
// 而"初始化中 → 运行中"、模块自愈清错都只能从 1Hz 的 status 里看到。
function update_module_chips(modules) {
  const list = $("module-list");
  const items = (state.modules && state.modules.items) || [];
  if (!list || !items.length) {
    return;
  }
  const live = modules || [];
  for (const card of list.querySelectorAll(".module-card")) {
    const item = items.find((entry) => entry.id === card.dataset.id);
    if (!item) {
      continue;
    }
    const chip_state = module_chip(item);
    const chip = card.querySelector("[data-role=chip]");
    if (chip) {
      chip.className = `chip ${chip_state.cls}`.trim();
      chip.textContent = chip_state.text;
    }
    const entry = live.find((row) => row.module_id === item.id);
    const message = entry && entry.error ? entry.error : "";
    const error = card.querySelector("[data-role=error]");
    if (error) {
      error.hidden = !message;
      error.textContent = message.slice(0, 400);
      error.title = message;
    }
  }
}

// ---------- 渲染：校准 ----------
//
// 两条消息分工：calibration 是"档案"（低频，连接时与任何改动后），preview 是"实时数值"
// （30 Hz）。preview 只改数字、不重建 DOM——重建会把正在拖的滑杆从手指底下换掉。

function face_regions(name) {
  if (!name || !state.face_map || !state.face_map.shape_regions) {
    return [];
  }
  return state.face_map.shape_regions[name] || [];
}

function face_shape_names() {
  return ((state.calibration && state.calibration.shapes) || []).map((shape) => shape.name);
}

function update_face_ui() {
  const pill = $("cal-face-state");
  if (!pill) {
    return;
  }
  const mode = state.face_mode;
  pill.textContent = t(`calibration.preview.state.${mode}`);
  pill.className = "pill" +
    (mode === "3d" ? " accent" : "") +
    (mode === "2d" || mode === "error" ? " warn" : "");

  const has3d = mode === "3d" && !!state.face_renderer;
  const has2d = mode === "2d" && !!state.face_fallback;
  $("cal-canvas").hidden = !has3d;
  $("cal-fallback").hidden = !has2d;
  $("cal-stage-empty").hidden = has3d || has2d;

  if (!has3d && !has2d) {
    $("cal-stage-empty").textContent = mode === "error"
      ? t("calibration.preview.error", { error: state.face_error || "?" })
      : t("calibration.preview.empty");
  }

  const detail = $("cal-face-detail");
  if (mode === "error") {
    detail.textContent = t("calibration.preview.error", { error: state.face_error || "?" });
    return;
  }
  const selected = state.face_selected;
  const regions = face_regions(selected);
  if (selected) {
    detail.textContent = regions.length
      ? t("calibration.preview.regions", { regions: regions.join(" · ") })
      : t("calibration.preview.numeric_only_detail");
    return;
  }
  detail.textContent = mode === "2d"
    ? t("calibration.preview.fallback")
    : t("calibration.preview.hint");
}

function select_face_shape(name) {
  state.face_selected = name || null;
  for (const row of state.cal_rows) {
    row.element.classList.toggle("selected", row.name === name);
  }
  const regions = face_regions(name);
  if (state.face_renderer) {
    state.face_renderer.set_highlight(regions);
  }
  if (state.face_fallback) {
    state.face_fallback.set_highlight(regions);
  }
  update_face_ui();
}

async function ensure_face_loaded() {
  if (state.face_loading || state.face_mode === "loading" ||
      state.face_mode === "3d" || state.face_mode === "2d") {
    return;
  }
  state.face_loading = true;
  state.face_mode = "loading";
  state.face_error = "";
  update_face_ui();
  try {
    const map_response = await fetch("face/face_map.json", { cache: "force-cache" });
    if (!map_response.ok) {
      throw new Error(`face_map.json HTTP ${map_response.status}`);
    }
    const map = await map_response.json();
    // 模型文件名以映射表为准：换资产时只改 face_map.model，页面不用动
    const model_name = map.model || "head_v1.fgbface";
    const model_response = await fetch(`face/${model_name}`, { cache: "force-cache" });
    if (!model_response.ok) {
      throw new Error(`${model_name} HTTP ${model_response.status}`);
    }
    const buffer = await model_response.arrayBuffer();
    state.face_map = map;

    // 2D 降级先建好：WebGL2 初始化失败时直接切过去，校准功能不受影响
    state.face_fallback = new window.FGBFace.Fallback(
      document.querySelector("#cal-fallback svg"));
    state.face_fallback.set_highlight(face_regions(state.face_selected));

    try {
      const model = window.FGBFace.parse_model(buffer);
      const renderer = new window.FGBFace.Renderer($("cal-canvas"), model, map);
      renderer.set_shape_indices(face_shape_names());
      renderer.set_eye(state.preview && state.preview.eye);
      renderer.set_values(state.preview ? state.preview.out : []);
      renderer.set_highlight(face_regions(state.face_selected));
      state.face_renderer = renderer;
      state.face_mode = "3d";
      if (state.view === "calibration") {
        renderer.start();
      }
    } catch (error) {
      state.face_mode = "2d";
      state.face_error = String((error && error.message) || error);
    }
  } catch (error) {
    state.face_mode = "error";
    state.face_error = String((error && error.message) || error);
  } finally {
    state.face_loading = false;
    update_face_ui();
    // 模型映射到了之后，行里要出现「仅数值」标记
    if (state.calibration && state.view === "calibration") {
      render_calibration();
    }
  }
}

// 校准：形状名称、分组、曲线
//
// shapes.json 是"人话"来源：协议名（不可改）→ 中文名/英文名/分组/一句话说明。
// 它和 face_map.json 是两份数据：前者描述"这个参数是什么"，后者描述"它在模型上动哪里"。
// 换模型资产时只动后者，名字不该跟着变。

const CAL_MIN_SPAN = 0.02;
const CAL_STEP_BAND = 0.02;
const CAL_CURVE_PRESETS = [
  ["linear", "calibration.curve.linear"],
  ["ease_in", "calibration.curve.ease_in"],
  ["ease_out", "calibration.curve.ease_out"],
  ["smooth", "calibration.curve.smooth"],
  ["step", "calibration.curve.step"],
];

function cal_clamp(v) {
  return Math.max(0, Math.min(1, v));
}

function shape_info(name) {
  const table = state.shape_labels && state.shape_labels.shapes;
  return (table && table[name]) || null;
}

function shape_label(name) {
  const info = shape_info(name);
  if (!info) {
    return name;
  }
  return (lang === "zh" ? info.zh : info.en) || name;
}

function shape_tip(name) {
  const info = shape_info(name);
  if (!info || lang !== "zh") {
    return "";
  }
  return info.tip || "";
}

function shape_group_id(name) {
  const info = shape_info(name);
  return (info && info.group) || "";
}

function shape_groups() {
  const table = state.shape_labels;
  if (!table || !Array.isArray(table.groups)) {
    return [{ id: "", zh: "", en: "" }];
  }
  return table.groups;
}

// 曲线求值必须和后端 apply_curve 一致：缩略图和实际输出不同步，
// 用户就会觉得"我看到的不是我在用的"。
// 单调三次 Hermite 的切线（Fritsch–Carlson），与后端 curve_tangents 逐行对应。
// 表情映射必须连续：折线在控制点有折角，普通样条又会过冲，两者放到脸上都是"抽"。
function cal_curve_tangents(points) {
  const n = points.length;
  const slope = [];
  for (let i = 0; i < n - 1; i += 1) {
    const dx = points[i + 1][0] - points[i][0];
    slope[i] = dx > 1e-6 ? (points[i + 1][1] - points[i][1]) / dx : 0;
  }
  const tangent = new Array(n).fill(0);
  if (n === 2) {
    tangent[0] = slope[0];
    tangent[1] = slope[0];
    return tangent;
  }
  tangent[0] = slope[0];
  tangent[n - 1] = slope[n - 2];
  for (let i = 1; i < n - 1; i += 1) {
    tangent[i] = 0.5 * (slope[i - 1] + slope[i]);
  }
  for (let i = 0; i < n - 1; i += 1) {
    if (Math.abs(slope[i]) < 1e-6) {
      tangent[i] = 0;
      tangent[i + 1] = 0;
      continue;
    }
    const a = tangent[i] / slope[i];
    const b = tangent[i + 1] / slope[i];
    const s = a * a + b * b;
    if (s > 9) {
      const tau = 3 / Math.sqrt(s);
      tangent[i] = tau * a * slope[i];
      tangent[i + 1] = tau * b * slope[i];
    }
  }
  return tangent;
}

function cal_hermite(y0, y1, m0, m1, dx, u) {
  const u2 = u * u;
  const u3 = u2 * u;
  return (2 * u3 - 3 * u2 + 1) * y0 + (u3 - 2 * u2 + u) * dx * m0 +
    (-2 * u3 + 3 * u2) * y1 + (u3 - u2) * dx * m1;
}

function cal_curve_value(spec, t) {
  t = cal_clamp(t);
  const name = spec.curve || "linear";
  if (name.indexOf("custom:") === 0) {
    const points = spec.points;
    if (!points || points.length < 2) {
      return t;
    }
    if (t <= points[0][0]) {
      return points[0][1];
    }
    const tangent = cal_curve_tangents(points);
    for (let i = 1; i < points.length; i += 1) {
      if (t <= points[i][0]) {
        const dx = points[i][0] - points[i - 1][0];
        if (dx <= 1e-6) {
          return points[i][1];
        }
        const u = (t - points[i - 1][0]) / dx;
        return cal_clamp(cal_hermite(points[i - 1][1], points[i][1],
          tangent[i - 1], tangent[i], dx, u));
      }
    }
    return points[points.length - 1][1];
  }
  if (name === "ease_in") {
    return t * t;
  }
  if (name === "ease_out") {
    return t * (2 - t);
  }
  if (name === "smooth") {
    return t * t * (3 - 2 * t);
  }
  if (name === "step") {
    const threshold = cal_clamp(spec.param === undefined ? 0.5 : spec.param);
    if (t <= threshold - CAL_STEP_BAND) {
      return 0;
    }
    if (t >= threshold + CAL_STEP_BAND) {
      return 1;
    }
    return (t - (threshold - CAL_STEP_BAND)) / (2 * CAL_STEP_BAND);
  }
  return t;
}

function cal_curve_points(spec, samples) {
  const out = [];
  for (let i = 0; i <= samples; i += 1) {
    const t = i / samples;
    out.push([t, cal_curve_value(spec, t)]);
  }
  return out;
}

// 采样点 → SVG path。pad 给描边留位置，否则 0 和 1 两端会被裁掉半条线。
// 采样密度按"缩略图看不出来折角"定：32 段足够让 28×18 的小图和 140×140 的画布都顺滑。
function cal_curve_path(spec, w, h, pad) {
  const points = cal_curve_points(spec, 32);
  return points.map(([x, y], i) => {
    const px = pad + x * (w - pad * 2);
    const py = h - pad - y * (h - pad * 2);
    return `${i === 0 ? "M" : "L"}${px.toFixed(2)} ${py.toFixed(2)}`;
  }).join(" ");
}

function cal_curve_icon(spec) {
  const w = 28;
  const h = 18;
  return `<svg viewBox="0 0 ${w} ${h}" aria-hidden="true"><path d="${cal_curve_path(spec, w, h, 2)}" fill="none" stroke="currentColor" stroke-width="1.4" stroke-linecap="round" stroke-linejoin="round"/></svg>`;
}

// "custom:2" → 2；预设返回 -1
function cal_custom_slot(curve) {
  return curve && curve.indexOf("custom:") === 0 ? Number(curve.slice(7)) : -1;
}

function cal_parse_points(text) {
  if (!text) {
    return [];
  }
  return text.split(/\s+/).filter(Boolean).map((token) => {
    const [x, y] = token.split(",");
    return [Number(x) || 0, Number(y) || 0];
  });
}

function cal_points_text(points) {
  return points.map(([x, y]) => `${x.toFixed(3)},${y.toFixed(3)}`).join(" ");
}

// 行的曲线描述：把 "custom:N" 展开成实际控制点，画图和求值都只认这个对象。
// 正在编辑的那一行优先用浮层里的草稿点：服务端广播回来之前，缩略图也得跟着手走。
function row_curve_spec(row) {
  const slot = cal_custom_slot(row.curve);
  const pop = state.curve_pop;
  if (slot >= 0 && pop && pop.row === row && pop.slot === slot && pop.points.length >= 2) {
    return { curve: row.curve, param: row.curve_param, points: pop.points };
  }
  const text = slot >= 0 && state.calibration
    ? ((state.calibration.curves || [])[slot] || "") : "";
  return { curve: row.curve, param: row.curve_param, points: cal_parse_points(text) };
}

function cal_apply_filter() {
  const text = ($("cal-search").value || "").trim().toLowerCase();
  const only_active = $("cal-only-active").checked;
  let visible = 0;
  for (const group of state.cal_groups) {
    let shown = 0;
    for (const row of group.items) {
      const show = (!text || row.haystack.includes(text)) &&
        (!only_active || row.check.checked);
      row.element.hidden = !show;
      if (show) {
        shown += 1;
      }
    }
    group.element.hidden = shown === 0;
    group.count.textContent = String(shown);
    visible += shown;
  }
  $("cal-empty").hidden = visible !== 0;
}

function build_cal_rows(shapes) {
  const list = $("cal-list");
  list.textContent = "";
  state.cal_rows = [];
  state.cal_groups = [];

  // 先按分组桶装，再按 shapes.json 里的分组顺序输出：88 行平铺找不到东西
  const buckets = new Map();
  for (const group of shape_groups()) {
    buckets.set(group.id, []);
  }
  shapes.forEach((shape, index) => {
    const gid = shape_group_id(shape.name);
    if (!buckets.has(gid)) {
      buckets.set(gid, []);
    }
    buckets.get(gid).push({ shape, index });
  });

  for (const group of shape_groups()) {
    const items = buckets.get(group.id) || [];
    if (!items.length) {
      continue;
    }
    const details = document.createElement("details");
    details.className = "cal-group";
    details.open = true;
    const summary = document.createElement("summary");
    const count = el("span", "cal-group-count", "0");
    summary.append(el("b", "", (lang === "zh" ? group.zh : group.en) || group.id), count);
    details.append(summary);
    const body = el("div", "cal-group-body");
    details.append(body);
    list.append(details);

    const group_rows = [];
    for (const entry of items) {
      const row = build_cal_row(entry.shape, entry.index);
      body.append(row.element);
      state.cal_rows.push(row);
      group_rows.push(row);
    }
    state.cal_groups.push({ element: details, count, items: group_rows });
  }

  // 顺序按分组排了，但 index 仍是协议顺序：preview 的 raw/out 数组按协议顺序发
  state.cal_rows.sort((a, b) => a.index - b.index);
}

function build_cal_row(shape, index) {
  const row = el("div", "cal-row");
  row.dataset.name = shape.name.toLowerCase();
  row.dataset.index = String(index);

  const ui = {
    element: row,
    index,
    name: shape.name,
    check: null,
    raw: el("span", "cal-raw", "—"),
    out: el("span", "cal-out", "—"),
    lo: Number(shape.lo),
    hi: Number(shape.hi),
    curve: shape.curve || "linear",
    curve_param: Number(shape.curve_param === undefined ? 0.5 : shape.curve_param),
    lo_input: null,
    hi_input: null,
    span: null,
    thumb_lo: null,
    thumb_hi: null,
    curve_btn: null,
    dragging: false,
    drag: null,
    timer: null,
    haystack: "",
  };

  const head = el("div", "cal-head");
  const name_label = el("label", "cal-name");
  const check = document.createElement("input");
  check.type = "checkbox";
  check.checked = !!shape.active;
  ui.check = check;

  const names = el("span", "cal-names");
  names.append(el("b", "cal-zh", shape_label(shape.name)));
  // 说明放在协议名后面同一行：它原来单独占一列，被右边的「原/出」挤到只剩省略号
  const sub = el("span", "cal-en");
  sub.append(el("span", "cal-en-name", shape.name));
  const tip = shape_tip(shape.name);
  if (tip) {
    sub.append(el("span", "cal-tip", `· ${tip}`));
  }
  names.append(sub);
  name_label.append(check, names);
  name_label.title = tip ? `${shape_label(shape.name)}｜${tip}` : shape.name;

  head.append(name_label);
  head.append(ui.raw, ui.out);

  if (state.face_map) {
    const regions = face_regions(shape.name);
    if (regions.length) {
      head.append(el("span", "cal-model-region", regions.slice(0, 2).join(" · ")));
    } else {
      // 没有模型映射就必须标出来：否则用户调了半天看不到变化，会以为坏了
      head.append(el("span", "cal-model-none", t("calibration.preview.numeric_only")));
    }
  }

  const curve_btn = el("button", "cal-curve-btn");
  curve_btn.type = "button";
  curve_btn.addEventListener("click", (event) => {
    event.stopPropagation();
    open_curve_popover(ui, curve_btn);
  });
  ui.curve_btn = curve_btn;
  head.append(curve_btn);

  row.append(head);
  row.append(build_dual_range(ui));
  row.addEventListener("click", () => select_face_shape(shape.name));

  const info = shape_info(shape.name);
  ui.haystack = [
    shape.name,
    shape_label(shape.name),
    (info && info.en) || "",
    (info && info.zh) || "",
    tip,
    shape_group_id(shape.name),
  ].join(" ").toLowerCase();

  check.addEventListener("change", () => push_cal_shape(ui));
  paint_curve_icon(ui);
  render_dual_range(ui);
  return ui;
}

// 两端各一个数字框，中间一条轨道两个拇指。
//
// 【为什么不用两个 input[type=range] 叠起来】原生 range 做不出"拖中间那一段整体平移"，
// 而平移恰恰是这个界面最常用的动作：调完 lo 往往要同步移 hi。自绘的代价是要自己处理
// 键盘与命中判定，但换来的是可控的手感。
function build_dual_range(ui) {
  const wrap = el("div", "cal-slider");

  const make_input = (which) => {
    const input = document.createElement("input");
    input.type = "number";
    input.className = "cal-num";
    input.min = "0";
    input.max = "1";
    input.step = "0.005";
    input.value = Number(which === "lo" ? ui.lo : ui.hi).toFixed(3);
    input.addEventListener("change", () => {
      const value = cal_clamp(Number(input.value) || 0);
      if (ui.check && !ui.check.checked) {
        ui.check.checked = true; // 手填了数就是想让它生效，别让它静默不工作
      }
      if (which === "lo") {
        ui.lo = Math.max(0, Math.min(value, ui.hi - CAL_MIN_SPAN));
      } else {
        ui.hi = Math.min(1, Math.max(value, ui.lo + CAL_MIN_SPAN));
      }
      render_dual_range(ui);
      push_cal_shape(ui);
    });
    return input;
  };
  ui.lo_input = make_input("lo");
  ui.hi_input = make_input("hi");

  const track = el("div", "cal-track");
  const rail = el("div", "cal-rail");
  const span = el("div", "cal-span");
  ui.span = span;
  ui.thumb_lo = el("div", "cal-thumb lo");
  ui.thumb_hi = el("div", "cal-thumb hi");
  for (const [thumb, which] of [[ui.thumb_lo, "lo"], [ui.thumb_hi, "hi"]]) {
    thumb.tabIndex = 0;
    thumb.setAttribute("role", "slider");
    thumb.setAttribute("aria-label", `${ui.name} ${which}`);
    thumb.addEventListener("keydown", (event) => {
      const big = event.shiftKey ? 0.05 : 0.005;
      let delta = 0;
      if (event.key === "ArrowLeft" || event.key === "ArrowDown") {
        delta = -big;
      } else if (event.key === "ArrowRight" || event.key === "ArrowUp") {
        delta = big;
      } else if (event.key === "Home") {
        delta = -1;
      } else if (event.key === "End") {
        delta = 1;
      } else {
        return;
      }
      event.preventDefault();
      nudge_dual_range(ui, which, delta);
    });
  }
  track.append(rail, span, ui.thumb_lo, ui.thumb_hi);
  track.addEventListener("pointerdown", (event) => begin_drag(ui, event));
  track.addEventListener("pointermove", (event) => move_drag(ui, event));
  track.addEventListener("pointerup", (event) => end_drag(ui, event));
  track.addEventListener("pointercancel", (event) => end_drag(ui, event));

  wrap.append(ui.lo_input, track, ui.hi_input, build_capture_buttons(ui));
  return wrap;
}

function build_capture_buttons(ui) {
  const box = el("div", "cal-capture");
  for (const which of ["lo", "hi"]) {
    const button = el("button", "btn tiny", which === "lo"
      ? t("calibration.capture.lo") : t("calibration.capture.hi"));
    button.type = "button";
    button.title = t("calibration.capture.hint", { end: which });
    button.addEventListener("click", (event) => {
      event.stopPropagation();
      send({ type: "capture_calibration", shape: ui.name, which });
    });
    box.append(button);
  }
  return box;
}

function track_value(track, client_x) {
  const rect = track.getBoundingClientRect();
  return cal_clamp((client_x - rect.left) / Math.max(1, rect.width));
}

function begin_drag(ui, event) {
  if (event.button !== 0) {
    return;
  }
  const track = event.currentTarget;
  const value = track_value(track, event.clientX);
  const px = 14 / Math.max(1, track.getBoundingClientRect().width);
  const d_lo = Math.abs(value - ui.lo);
  const d_hi = Math.abs(value - ui.hi);
  let mode;
  if (d_lo <= px) {
    mode = "lo";
  } else if (d_hi <= px) {
    mode = "hi";
  } else if (value > ui.lo && value < ui.hi) {
    mode = "pan"; // 落在中间段：整段平移，跨度不变
  } else {
    mode = d_lo < d_hi ? "lo" : "hi";
  }
  ui.drag = {
    mode,
    pointer: event.pointerId,
    start_value: value,
    start_lo: ui.lo,
    start_hi: ui.hi,
  };
  ui.dragging = true;
  track.setPointerCapture(event.pointerId);
  if (ui.check && !ui.check.checked) {
    ui.check.checked = true; // 开始拖就是在调这个形状，顺手启用，省一次点击
  }
  event.preventDefault();
  move_drag(ui, event);
}

function move_drag(ui, event) {
  if (!ui.drag || event.pointerId !== ui.drag.pointer) {
    return;
  }
  const value = track_value(event.currentTarget, event.clientX);
  const drag = ui.drag;
  if (drag.mode === "lo") {
    ui.lo = Math.max(0, Math.min(value, ui.hi - CAL_MIN_SPAN));
  } else if (drag.mode === "hi") {
    ui.hi = Math.min(1, Math.max(value, ui.lo + CAL_MIN_SPAN));
  } else {
    const delta = value - drag.start_value;
    let lo = drag.start_lo + delta;
    let hi = drag.start_hi + delta;
    if (lo < 0) {
      hi -= lo;
      lo = 0;
    }
    if (hi > 1) {
      lo -= hi - 1;
      hi = 1;
    }
    ui.lo = Math.max(0, lo);
    ui.hi = Math.min(1, hi);
  }
  render_dual_range(ui);
  queue_cal_shape(ui);
}

function end_drag(ui, event) {
  if (!ui.drag || event.pointerId !== ui.drag.pointer) {
    return;
  }
  ui.drag = null;
  ui.dragging = false;
  push_cal_shape(ui);
}

function nudge_dual_range(ui, which, delta) {
  if (delta <= -1 || delta >= 1) {
    if (which === "lo") {
      ui.lo = 0;
    } else {
      ui.hi = 1;
    }
  } else if (which === "lo") {
    ui.lo = Math.max(0, Math.min(ui.lo + delta, ui.hi - CAL_MIN_SPAN));
  } else {
    ui.hi = Math.min(1, Math.max(ui.hi + delta, ui.lo + CAL_MIN_SPAN));
  }
  render_dual_range(ui);
  queue_cal_shape(ui);
}

function render_dual_range(ui) {
  if (!ui.span) {
    return;
  }
  const lo = cal_clamp(ui.lo);
  const hi = cal_clamp(ui.hi);
  ui.span.style.left = `${lo * 100}%`;
  ui.span.style.width = `${Math.max(0, hi - lo) * 100}%`;
  ui.thumb_lo.style.left = `${lo * 100}%`;
  ui.thumb_hi.style.left = `${hi * 100}%`;
  ui.thumb_lo.setAttribute("aria-valuenow", lo.toFixed(3));
  ui.thumb_hi.setAttribute("aria-valuenow", hi.toFixed(3));
  if (document.activeElement !== ui.lo_input) {
    ui.lo_input.value = lo.toFixed(3);
  }
  if (document.activeElement !== ui.hi_input) {
    ui.hi_input.value = hi.toFixed(3);
  }
  // 跨度就是灵敏度：贴得太近时标黄，提示这个形状几乎成了开关
  ui.element.classList.toggle("narrow", hi - lo < 0.08);
}

function paint_curve_icon(ui) {
  if (!ui.curve_btn) {
    return;
  }
  ui.curve_btn.innerHTML = cal_curve_icon(row_curve_spec(ui));
  const slot = cal_custom_slot(ui.curve);
  ui.curve_btn.classList.toggle("custom", slot >= 0);
  const preset = CAL_CURVE_PRESETS.find((entry) => entry[0] === ui.curve);
  ui.curve_btn.title = slot >= 0
    ? t("calibration.curve.custom_title", { n: slot + 1 })
    : t(preset ? preset[1] : "calibration.curve.linear");
}

// 拖动时 60 ms 一次：再快只会让引擎把同一份约 2 KB 的档案广播十几次，滑杆本身也不会更顺
function queue_cal_shape(ui) {
  if (!ui || ui.timer) {
    return;
  }
  ui.timer = setTimeout(() => {
    ui.timer = null;
    push_cal_shape(ui);
  }, 60);
}

function push_cal_shape(ui) {
  if (!ui) {
    return;
  }
  clearTimeout(ui.timer);
  ui.timer = null;
  send({
    type: "set_calibration_shape",
    name: ui.name,
    lo: Number(ui.lo),
    hi: Number(ui.hi),
    active: ui.check.checked,
    curve: ui.curve,
    curve_param: Number(ui.curve_param),
  });
}

function paint_range(node) {
  const min = Number(node.min || 0);
  const max = Number(node.max || 100);
  const value = Number(node.value || 0);
  const pct = max > min ? ((value - min) / (max - min)) * 100 : 0;
  node.style.setProperty("--fill", `${Math.max(0, Math.min(100, pct))}%`);
}

function sync_slider(id, value, digits) {
  const node = $(id);
  if (!node || value === undefined || value === null) {
    return;
  }
  // 用户正在拖这一条时不要把值抢回去，否则滑杆会"弹"
  if (document.activeElement !== node) {
    node.value = value;
  }
  paint_range(node);
  const label = $(`${id}-v`);
  if (label) {
    label.textContent = Number(node.value).toFixed(digits);
  }
}

function render_calibration() {
  const data = state.calibration;
  if (!data) {
    return;
  }
  const available = !!data.available;
  for (const id of ["cal-save", "cal-reload", "cal-reset", "cal-eye-active",
    "cal-eye-gain-x", "cal-eye-gain-y", "cal-eye-center-x", "cal-eye-center-y",
    "cal-eye-capture"]) {
    $(id).disabled = !available;
  }

  $("cal-path").textContent = data.path || "—";
  $("cal-path").title = data.loaded ? t("calibration.loaded") : t("calibration.not_saved");
  $("cal-dirty").hidden = !data.dirty;

  const shapes = data.shapes || [];
  // 语言也进签名：切语言后按钮上的「取当前」得跟着换；face_map 到了要补「仅数值」
  const signature = `${lang}|${state.face_map ? "m" : "n"}|${shapes.length}|${shapes.map((s) => s.name).join(",")}`;
  const list = $("cal-list");
  if (list.dataset.signature !== signature) {
    build_cal_rows(shapes);
    list.dataset.signature = signature;
  }

  // 逐行同步：拖拽中的行不要被广播抢回数值，否则滑杆会在手指底下弹
  for (const row of state.cal_rows) {
    const shape = shapes[row.index];
    if (!shape) {
      continue;
    }
    if (!row.dragging) {
      row.lo = Number(shape.lo);
      row.hi = Number(shape.hi);
      render_dual_range(row);
    }
    if (document.activeElement !== row.check) {
      row.check.checked = !!shape.active;
    }
    row.element.classList.toggle("inactive", !shape.active);

    const curve = shape.curve || "linear";
    const param = Number(shape.curve_param === undefined ? 0.5 : shape.curve_param);
    if (row.curve !== curve || row.curve_param !== param) {
      row.curve = curve;
      row.curve_param = param;
      paint_curve_icon(row);
      refresh_curve_popover(row);
    }
  }

  // 自定义曲线被改写时，所有指向该槽的缩略图都要重画（它们共享一条曲线）
  const curves_sig = (data.curves || []).join("|");
  if (curves_sig !== state.cal_curves_sig) {
    state.cal_curves_sig = curves_sig;
    for (const row of state.cal_rows) {
      if (cal_custom_slot(row.curve) >= 0) {
        paint_curve_icon(row);
      }
    }
    refresh_curve_popover();
  }

  cal_apply_filter();

  if (state.face_renderer) {
    state.face_renderer.set_shape_indices(face_shape_names());
  }

  const eye = data.eye || {};
  if (document.activeElement !== $("cal-eye-active")) {
    $("cal-eye-active").checked = !!eye.active;
  }
  sync_slider("cal-eye-gain-x", eye.gain_x, 2);
  sync_slider("cal-eye-gain-y", eye.gain_y, 2);
  sync_slider("cal-eye-center-x", eye.center_x, 1);
  sync_slider("cal-eye-center-y", eye.center_y, 1);
  if (state.preview) {
    render_preview(state.preview);
  }
  update_face_ui();
}

function render_preview(message) {
  const raw = message.raw || [];
  const out = message.out || [];
  for (const row of state.cal_rows) {
    if (row.index >= raw.length) {
      continue;
    }
    row.raw.textContent = `${t("calibration.raw")} ${Number(raw[row.index]).toFixed(3)}`;
    row.out.textContent = `${t("calibration.out")} ${Number(out[row.index]).toFixed(3)}`;
  }
  const eye = message.eye || {};
  $("cal-eye-live").textContent = eye.valid
    ? `x ${Number(eye.x).toFixed(1)}°  y ${Number(eye.y).toFixed(1)}°`
    : t("calibration.eye.no_data");

  // 模型看的是"实际发出去的输出值"（校准 + 滤波之后），不是原始值
  if (state.face_renderer) {
    state.face_renderer.set_eye(eye);
    state.face_renderer.set_values(out);
  }
  update_curve_cursor(message);
}

// ---------- 校准：曲线编辑浮层 ----------
//
// 单例浮层而不是新页面：调曲线要看"当前这一帧的原始值落在曲线的哪一段"，
// 离开列表就看不到上下文了。
const CC_SIZE = 168;
const CC_PAD = 14;

function cc_x(x) {
  return CC_PAD + x * (CC_SIZE - CC_PAD * 2);
}

function cc_y(y) {
  return CC_SIZE - CC_PAD - y * (CC_SIZE - CC_PAD * 2);
}

function mirror_shape_name(name) {
  if (name.endsWith("Right")) {
    return `${name.slice(0, -5)}Left`;
  }
  if (name.endsWith("Left")) {
    return `${name.slice(0, -4)}Right`;
  }
  return "";
}

function ensure_curve_pop() {
  if (state.curve_pop) {
    return state.curve_pop;
  }
  const root = el("div", "cal-curve-pop");
  root.hidden = true;
  root.innerHTML = `
    <div class="cc-head"><b class="cc-title"></b><span class="cc-sub"></span></div>
    <div class="cc-grid"></div>
    <div class="cc-custom" hidden>
      <svg class="cc-canvas" viewBox="0 0 ${CC_SIZE} ${CC_SIZE}" role="img"></svg>
      <div class="cc-tools">
        <button type="button" class="btn tiny" data-act="add"></button>
        <button type="button" class="btn tiny" data-act="remove"></button>
      </div>
    </div>    <div class="cc-foot">
      <span class="cc-live"></span>
      <button type="button" class="btn tiny" data-act="mirror"></button>
    </div>`;
  document.body.append(root);

  const pop = {
    root,
    title: root.querySelector(".cc-title"),
    sub: root.querySelector(".cc-sub"),
    grid: root.querySelector(".cc-grid"),
    custom: root.querySelector(".cc-custom"),
    canvas: root.querySelector(".cc-canvas"),
    live: root.querySelector(".cc-live"),
    mirror: root.querySelector("[data-act=mirror]"),
    row: null,
    anchor: null,
    points: [],
    slot: -1,
    timer: null,
  };

  pop.grid.textContent = "";
  for (const [id, key] of CAL_CURVE_PRESETS) {
    const card = el("button", "cc-card");
    card.type = "button";
    card.dataset.curve = id;
    card.innerHTML = `<span class="cc-icon">${cal_curve_icon({ curve: id })}</span>`;
    card.append(el("span", "cc-name", t(key)));
    card.addEventListener("click", () => {
      if (!pop.row) {
        return;
      }
      pop.row.curve = id;
      paint_curve_icon(pop.row);
      push_cal_shape(pop.row);
      refresh_curve_popover();
    });
    pop.grid.append(card);
  }
  const custom = el("button", "cc-card");
  custom.type = "button";
  custom.dataset.curve = "custom";
  // 自定义也给个形状：一条手绘感的 S，表示的正是"你自己定"的那类曲线
  custom.innerHTML = `<span class="cc-icon">${
    cal_curve_icon({ curve: "custom:0", points: [[0, 0], [0.28, 0.12], [0.72, 0.88], [1, 1]] })
  }</span>`;
  custom.append(el("span", "cc-name", t("calibration.curve.custom")));
  custom.addEventListener("click", () => {
    if (!pop.row) {
      return;
    }
    use_custom_curve(pop.row);
  });
  pop.grid.append(custom);

  root.querySelector("[data-act=add]").textContent = t("calibration.curve.add_point");
  root.querySelector("[data-act=remove]").textContent = t("calibration.curve.remove_point");
  root.querySelector("[data-act=add]").addEventListener("click", () => {
    if (pop.points.length >= 8) {
      return;
    }
    // 插在最宽的一段中间：用户想加点的位置几乎总是"不够细"的那一段
    let best = 0;
    for (let i = 1; i < pop.points.length; i += 1) {
      if (pop.points[i][0] - pop.points[i - 1][0] >
          pop.points[best][0] - pop.points[best - 1 < 0 ? 0 : best - 1][0]) {
        best = i;
      }
    }
    const a = pop.points[Math.max(0, best - 1)];
    const b = pop.points[best];
    pop.points.splice(best, 0, [(a[0] + b[0]) / 2, (a[1] + b[1]) / 2]);
    commit_custom_curve(pop);
  });
  root.querySelector("[data-act=remove]").addEventListener("click", () => {
    if (pop.points.length <= 2) {
      return;
    }
    pop.points.splice(Math.floor(pop.points.length / 2), 1);
    commit_custom_curve(pop);
  });
  pop.mirror.addEventListener("click", apply_curve_to_mirror);

  // 点外部关闭。用 pointerdown 而不是 click：拖完控制点松手在浮层外时，
  // click 会落在 body 上，把刚打开的面板关掉。
  document.addEventListener("pointerdown", (event) => {
    if (root.hidden || !pop.row) {
      return;
    }
    if (root.contains(event.target)) {
      return;
    }
    if (pop.anchor && pop.anchor.contains(event.target)) {
      return;
    }
    close_curve_popover();
  });
  document.addEventListener("keydown", (event) => {
    if (event.key === "Escape" && !root.hidden) {
      close_curve_popover();
    }
  });

  bind_custom_canvas(pop);
  state.curve_pop = pop;
  return pop;
}

function open_curve_popover(ui, anchor) {
  const pop = ensure_curve_pop();
  pop.row = ui;
  pop.anchor = anchor;
  pop.root.hidden = false;
  pop.title.textContent = `${shape_label(ui.name)} · ${ui.name}`;
  refresh_curve_popover();
  update_curve_cursor(state.preview);

  const rect = anchor.getBoundingClientRect();
  const width = pop.root.offsetWidth || 328;
  const height = pop.root.offsetHeight || 300;
  let left = rect.right - width;
  left = Math.max(12, Math.min(left, window.innerWidth - width - 12));
  let top = rect.bottom + 8;
  if (top + height > window.innerHeight - 12) {
    top = Math.max(12, rect.top - height - 8);
  }
  pop.root.style.left = `${left}px`;
  pop.root.style.top = `${top}px`;
}

function close_curve_popover() {
  const pop = state.curve_pop;
  if (!pop) {
    return;
  }
  pop.root.hidden = true;
  pop.row = null;
  pop.anchor = null;
}

// 只重画选中态和自定义画布：每次拖动都重建整块 DOM 会把正在拖的圆点换掉
function refresh_curve_popover(row) {
  const pop = state.curve_pop;
  if (!pop || pop.root.hidden || !pop.row) {
    return;
  }
  if (row && row !== pop.row) {
    return;
  }
  const ui = pop.row;
  const slot = cal_custom_slot(ui.curve);
  for (const card of pop.grid.querySelectorAll(".cc-card")) {
    const id = card.dataset.curve;
    const active = id === "custom" ? slot >= 0 : id === ui.curve;
    card.classList.toggle("active", active);
  }
  pop.custom.hidden = slot < 0;
  pop.sub.textContent = slot >= 0
    ? t("calibration.curve.custom_hint")
    : t("calibration.curve.preset_hint");

  const mirror = mirror_shape_name(ui.name);
  pop.mirror.hidden = !mirror;
  if (mirror) {
    pop.mirror.textContent = t("calibration.curve.mirror", { name: shape_label(mirror) });
  }

  if (slot >= 0) {
    // 只在"换了槽"时重新读服务端数据：拖点过程中服务端会广播旧值，
    // 每次都用它覆盖本地点会让拇指在手指底下弹回去
    if (pop.slot !== slot) {
      const text = (state.calibration && (state.calibration.curves || [])[slot]) || "";
      pop.points = cal_parse_points(text);
      if (pop.points.length < 2) {
        pop.points = cal_curve_points(row_curve_spec(ui), 4);
      }
      pop.slot = slot;
    }
    pop.custom.hidden = false;
    draw_custom_canvas(pop);
  } else {
    pop.slot = -1;
    pop.custom.hidden = true;
  }
  reposition_curve_pop();
}

// 展开自定义画布后浮层会变高：不重新定位就会顶出视口底部
function reposition_curve_pop() {
  const pop = state.curve_pop;
  if (!pop || pop.root.hidden || !pop.anchor) {
    return;
  }
  const rect = pop.anchor.getBoundingClientRect();
  const height = pop.root.offsetHeight;
  let top = rect.bottom + 8;
  if (top + height > window.innerHeight - 12) {
    top = Math.max(12, window.innerHeight - height - 12);
  }
  pop.root.style.top = `${top}px`;
}

// 切到自定义时，用当前曲线的形状做起点：视觉上不跳变，用户只在需要的地方改
function use_custom_curve(ui) {
  const pop = state.curve_pop;
  let slot = cal_custom_slot(ui.curve);
  if (slot < 0) {
    slot = 0;
    const used = new Set();
    for (const row of state.cal_rows) {
      const s = cal_custom_slot(row.curve);
      if (s >= 0) {
        used.add(s);
      }
    }
    for (let i = 0; i < 8; i += 1) {
      if (!used.has(i)) {
        slot = i;
        break;
      }
    }
  }
  pop.points = cal_curve_points(row_curve_spec(ui), 4);
  pop.slot = slot;
  ui.curve = `custom:${slot}`;
  ui.curve_param = 0.5;
  commit_custom_curve(pop);
  refresh_curve_popover();
}

function commit_custom_curve(pop) {
  const points = pop.points.slice().sort((a, b) => a[0] - b[0]);
  // 端点锚定：这里也做一次，保证发出去的东西和后端 sanitize 后一致
  points[0] = [0, 0];
  points[points.length - 1] = [1, 1];
  pop.points = points;
  draw_custom_canvas(pop);
  if (pop.row) {
    paint_curve_icon(pop.row);
  }
  send({
    type: "set_calibration_curve",
    slot: pop.slot,
    points: cal_points_text(points),
  });
  if (pop.row) {
    push_cal_shape(pop.row);
  }
}

function draw_custom_canvas(pop) {
  const points = pop.points;
  const grid = [0.25, 0.5, 0.75].map((t) => {
    const x = cc_x(t).toFixed(1);
    const y = cc_y(t).toFixed(1);
    return `<line x1="${x}" y1="${CC_PAD}" x2="${x}" y2="${CC_SIZE - CC_PAD}" class="cc-grid-line"/>` +
      `<line x1="${CC_PAD}" y1="${y}" x2="${CC_SIZE - CC_PAD}" y2="${y}" class="cc-grid-line"/>`;
  }).join("");
  const smooth = cal_curve_points({ curve: `custom:${pop.slot < 0 ? 0 : pop.slot}`, points }, 48)
    .map(([x, y], i) => `${i === 0 ? "M" : "L"}${cc_x(x).toFixed(1)} ${cc_y(y).toFixed(1)}`)
    .join(" ");
  const dots = points.map(([x, y], i) =>
    `<circle class="cc-dot${i === 0 || i === points.length - 1 ? " locked" : ""}" data-i="${i}" cx="${cc_x(x).toFixed(1)}" cy="${cc_y(y).toFixed(1)}" r="5"/>`).join("");
  pop.canvas.innerHTML = `
    <rect class="cc-frame" x="${CC_PAD}" y="${CC_PAD}" width="${CC_SIZE - CC_PAD * 2}" height="${CC_SIZE - CC_PAD * 2}" rx="4"/>
    ${grid}
    <path class="cc-ref" d="M${cc_x(0)} ${cc_y(0)} L${cc_x(1)} ${cc_y(1)}"/>
    <path class="cc-line" d="${smooth}"/>
    ${dots}`;
}

function bind_custom_canvas(pop) {
  let drag = null;
  pop.canvas.addEventListener("pointerdown", (event) => {
    const dot = event.target.closest ? event.target.closest(".cc-dot") : null;
    if (!dot || dot.classList.contains("locked")) {
      return;
    }
    drag = Number(dot.dataset.i);
    pop.canvas.setPointerCapture(event.pointerId);
    event.preventDefault();
  });
  pop.canvas.addEventListener("pointermove", (event) => {
    if (drag === null) {
      return;
    }
    const rect = pop.canvas.getBoundingClientRect();
    const x = cal_clamp((event.clientX - rect.left) / Math.max(1, rect.width));
    const y = cal_clamp(1 - (event.clientY - rect.top) / Math.max(1, rect.height));
    // 中间点只能在自己的邻居之间移动：x 一旦交叉，曲线就不再是函数
    const prev = pop.points[drag - 1];
    const next = pop.points[drag + 1];
    pop.points[drag] = [
      Math.max(prev[0] + 0.02, Math.min(x, next[0] - 0.02)),
      y,
    ];
    draw_custom_canvas(pop);
    clearTimeout(pop.timer);
    pop.timer = setTimeout(() => commit_custom_curve(pop), 80);
  });
  const stop = (event) => {
    if (drag === null) {
      return;
    }
    drag = null;
    pop.canvas.releasePointerCapture?.(event.pointerId);
    commit_custom_curve(pop);
  };
  pop.canvas.addEventListener("pointerup", stop);
  pop.canvas.addEventListener("pointercancel", stop);
}

function apply_curve_to_mirror() {
  const pop = state.curve_pop;
  if (!pop || !pop.row) {
    return;
  }
  const target = mirror_shape_name(pop.row.name);
  if (!target) {
    return;
  }
  // 复制的是"整组手感"：端点 + 曲线。左右脸用不同参数几乎没有正当理由。
  send({
    type: "set_calibration_shape",
    name: target,
    lo: Number(pop.row.lo),
    hi: Number(pop.row.hi),
    active: true,
    curve: pop.row.curve,
    curve_param: Number(pop.row.curve_param),
  });
}

// 实时游标：让用户看到"这一帧的原始值落在曲线的哪一段"，比任何说明都直观
function update_curve_cursor(message) {
  const pop = state.curve_pop;
  if (!pop || pop.root.hidden || !pop.row || !message) {
    return;
  }
  const index = pop.row.index;
  const raw = Number((message.raw || [])[index]);
  const out = Number((message.out || [])[index]);
  if (!Number.isFinite(raw)) {
    pop.live.textContent = t("calibration.curve.no_data");
    return;
  }
  const span = Math.max(1e-4, pop.row.hi - pop.row.lo);
  const t_value = cal_clamp((raw - pop.row.lo) / span);
  pop.live.textContent = t("calibration.curve.cursor", {
    raw: raw.toFixed(3),
    t: t_value.toFixed(3),
    out: Number.isFinite(out) ? out.toFixed(3) : "—",
  });
}

// 预览是"订阅制"：页面关掉就停，引擎热路径回到零额外开销（ADR-0021）
// 名称表是"锦上添花"：拿不到就退回协议名，校准本身照常可用。
// 失败只 warn 不弹错——用户不该因为一个翻译文件没读到就没法调表情。
async function ensure_labels_loaded() {
  if (state.shape_labels || state.shape_labels_pending) {
    return;
  }
  state.shape_labels_pending = true;
  try {
    const response = await fetch("shapes.json", { cache: "force-cache" });
    if (!response.ok) {
      throw new Error(`shapes.json HTTP ${response.status}`);
    }
    state.shape_labels = await response.json();
    // 名字和分组都变了，列表必须重建：signature 清掉即可，下一次渲染会自己重建
    const list = $("cal-list");
    if (list) {
      list.dataset.signature = "";
    }
    if (state.view === "calibration") {
      render_calibration();
    }
  } catch (error) {
    console.warn("shapes.json 加载失败，退回协议名", error);
  } finally {
    state.shape_labels_pending = false;
  }
}

function set_cal_preview(on) {
  if (state.cal_preview_on === on) {
    return;
  }
  state.cal_preview_on = on;
  send({ type: "set_preview", enabled: on });
  if (on) {
    ensure_face_loaded();
    ensure_labels_loaded();
  }
  if (state.face_renderer) {
    if (on) {
      state.face_renderer.start();
    } else {
      state.face_renderer.stop();
    }
  }
  update_face_ui();
}

function push_cal_eye() {
  send({
    type: "set_calibration_eye",
    active: $("cal-eye-active").checked,
    gain_x: Number($("cal-eye-gain-x").value),
    gain_y: Number($("cal-eye-gain-y").value),
    center_x: Number($("cal-eye-center-x").value),
    center_y: Number($("cal-eye-center-y").value),
  });
}

let cal_eye_timer = null;
function queue_cal_eye() {
  if (cal_eye_timer) {
    return;
  }
  cal_eye_timer = setTimeout(() => {
    cal_eye_timer = null;
    push_cal_eye();
  }, 60);
}

// ---------- 渲染：SteamVR ----------
//
// 引擎侧所有 OpenVR 调用都在后台线程完成：这里看到的 busy 只是快照。
// 命令发出去后先本地置 busy，收到下一次 steamvr 消息再以服务端为准。

function sv_reason_text(reason) {
  const key = `settings.steamvr.reason.${reason || "not_checked"}`;
  const text = t(key);
  return text === key ? (reason || "?") : text;
}

function sv_failure_text(data) {
  return t("settings.steamvr.failed", { reason: sv_reason_text(data.reason) });
}

function handle_steamvr(message) {
  const previous = state.steamvr;
  state.steamvr = message;
  state.steamvr_busy = !!message.busy;

  // 动作完成：busy 从 true 回到 false 时给一次 toast；刷新不打扰
  const action = message.action || "refresh";
  if (previous && previous.busy && !message.busy && action !== "refresh") {
    if (!message.ok) {
      show_toast(sv_failure_text(message), "error");
    } else if (action === "enable") {
      show_toast(t("settings.steamvr.enabled"));
    } else if (action === "disable") {
      show_toast(t("settings.steamvr.disabled"));
    } else if (action === "remove") {
      show_toast(t("settings.steamvr.removed"));
    }
  }
  if (state.view === "settings") {
    render_steamvr();
  }
}

function render_steamvr() {
  const data = state.steamvr || {
    available: false,
    state: "unknown",
    reason: "not_checked",
    busy: false,
    registered: false,
  };
  const available = !!data.available;
  const busy = state.steamvr_busy || !!data.busy;
  const state_id = data.state || "unknown";

  const pill = $("sv-state");
  pill.textContent = t(`settings.steamvr.state.${state_id}`);
  pill.className = "pill" +
    (state_id === "on" ? " accent" : "") +
    (state_id === "error" || state_id === "not_running" || state_id === "not_installed" ? " warn" : "");

  $("sv-autolaunch").checked = !!data.autolaunch;
  // 只有 on/off 才是"SteamVR 在跑、可以改"的状态；not_running 时开关禁用并提示先启动
  $("sv-autolaunch").disabled =
    !available || busy || (state_id !== "on" && state_id !== "off");
  $("sv-refresh").disabled = !available || busy;
  $("sv-remove").disabled = !available || busy || !data.registered;
  $("sv-busy").hidden = !busy;

  $("sv-runtime").textContent = data.runtime_dir || "—";
  $("sv-runtime").title = data.runtime_dir || "";
  $("sv-manifest").textContent = data.manifest_path || "—";
  $("sv-manifest").title = data.manifest_path || "";
  $("sv-binary").textContent = data.binary_path || "—";
  $("sv-binary").title = data.binary_path || "";

  const detail = $("sv-detail");
  if (!available) {
    detail.textContent = t("settings.steamvr.unavailable");
  } else if (busy) {
    detail.textContent = t("settings.steamvr.working");
  } else {
    const reason = sv_reason_text(data.reason);
    const extra = data.detail && data.reason !== "ok" ? ` · ${data.detail}` : "";
    detail.textContent = `${reason}${extra}`;
  }
}

function request_steamvr_refresh() {
  if (!state.connected) {
    return;
  }
  if (state.steamvr && state.steamvr.available === false) {
    return;
  }
  state.steamvr_busy = true;
  send({ type: "refresh_steamvr" });
  render_steamvr();
}

function set_steamvr_autolaunch(enabled) {
  state.steamvr_busy = true;
  if (!send({ type: "set_steamvr_autolaunch", enabled })) {
    state.steamvr_busy = false;
    show_toast(t("link.failed"), "error");
  }
  render_steamvr();
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

async function switch_page(page) {
  if (state.view === page) {
    return;
  }
  // 校准是"改了立即生效、保存才落盘"：走之前问一句，别让用户以为已经存下来了
  if (state.view === "calibration" && state.calibration && state.calibration.dirty) {
    const leave = await ask_confirm({
      title: t("calibration.unsaved"),
      message: t("calibration.leave_confirm"),
      confirmText: t("calibration.leave_ok"),
      danger: false,
    });
    if (!leave) {
      return;
    }
  }

  state.view = page;
  document.querySelectorAll(".nav-item").forEach((item) => {
    item.classList.toggle("active", item.dataset.page === page);
  });
  document.querySelectorAll(".page").forEach((item) => {
    item.classList.toggle("active", item.dataset.page === page);
  });
  update_page_header();
  set_cal_preview(page === "calibration");

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
  if (page === "calibration") {
    render_calibration();
  }
  if (page === "status") {
    render_params(state.values);
  }
  if (page === "library") {
    render_library();
  }
  if (page === "settings") {
    render_steamvr();
    request_steamvr_refresh();
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

  // 静态滑杆先画一次进度；校准页的行是动态建的，在 make_range 里画
  document.querySelectorAll('input[type="range"]').forEach((node) => {
    paint_range(node);
    node.addEventListener("input", () => paint_range(node));
  });

  // 数字输入的步进按钮：原生 spinner 在暗底上太突兀，换成右侧自绘的上下箭头
  document.querySelectorAll(".num-step").forEach((button) => {
    button.addEventListener("click", () => {
      const field = button.closest(".num-field");
      const input = field ? field.querySelector("input") : null;
      if (!input || input.disabled) {
        return;
      }
      const step = Number(input.step) || 1;
      const min = input.min === "" ? -Infinity : Number(input.min);
      const max = input.max === "" ? Infinity : Number(input.max);
      const current = input.value === "" ? 0 : Number(input.value);
      const next = Math.min(max, Math.max(min, current + step * Number(button.dataset.step || 1)));
      input.value = String(next);
      // 走一遍与手动输入相同的通知，outputs_dirty 与联动禁用才会跟上
      input.dispatchEvent(new Event("input", { bubbles: true }));
      input.dispatchEvent(new Event("change", { bubbles: true }));
    });
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

  $("sv-refresh").addEventListener("click", request_steamvr_refresh);
  $("sv-autolaunch").addEventListener("change", () => {
    if ($("sv-autolaunch").disabled) {
      return;
    }
    set_steamvr_autolaunch($("sv-autolaunch").checked);
  });
  $("sv-remove").addEventListener("click", async () => {
    if (!(await ask_confirm({
      title: t("settings.steamvr.remove"),
      message: t("settings.steamvr.remove_confirm"),
      danger: true,
    }))) {
      return;
    }
    state.steamvr_busy = true;
    send({ type: "remove_steamvr_manifest" });
    render_steamvr();
  });

  $("cal-search").addEventListener("input", cal_apply_filter);
  $("cal-only-active").addEventListener("change", cal_apply_filter);

  $("cal-view-reset").addEventListener("click", () => {
    if (state.face_renderer) {
      state.face_renderer.reset();
    }
  });
  document.querySelectorAll(".cal-presets [data-view]").forEach((button) => {
    button.addEventListener("click", () => {
      if (state.face_renderer) {
        state.face_renderer.set_view(button.dataset.view);
      }
    });
  });

  $("cal-save").addEventListener("click", () => {
    send({ type: "save_calibration" });
  });
  $("cal-reload").addEventListener("click", async () => {
    const dirty = state.calibration && state.calibration.dirty;
    if (dirty && !(await ask_confirm({
      title: t("calibration.reload"),
      message: t("calibration.reload_confirm"),
      danger: true,
    }))) {
      return;
    }
    send({ type: "reload_calibration" });
  });
  $("cal-reset").addEventListener("click", async () => {
    if (await ask_confirm({
      title: t("calibration.reset"),
      message: t("calibration.reset_confirm"),
      danger: true,
    })) {
      send({ type: "reset_calibration" });
    }
  });

  for (const id of ["cal-eye-gain-x", "cal-eye-gain-y", "cal-eye-center-x", "cal-eye-center-y"]) {
    const digits = id.includes("center") ? 1 : 2;
    $(id).addEventListener("input", () => {
      $(`${id}-v`).textContent = Number($(id).value).toFixed(digits);
      queue_cal_eye();
    });
    $(id).addEventListener("change", push_cal_eye);
  }
  $("cal-eye-active").addEventListener("change", push_cal_eye);
  $("cal-eye-capture").addEventListener("click", () => {
    send({ type: "capture_calibration", shape: "", which: "center" });
  });

  $("o-reload").addEventListener("click", () => {
    send({ type: "reload_avatar" });
  });
  for (const id of [
    "o-vrchat", "o-vrchat-on", "o-eye-mode", "o-params-mode", "o-vmc-on", "o-vmc-target",
    "o-shm-on", "o-shm-name", "o-ws-on", "o-ws-port", "o-ws-rate", "o-ws-all",
    "o-udp-on", "o-udp-target", "o-udp-rate",
  ]) {
    const element = $(id);
    element.addEventListener("input", () => {
      set_outputs_dirty(true);
    });
    element.addEventListener("change", () => {
      set_outputs_dirty(true);
      sync_output_form();
    });
  }
  $("o-save").addEventListener("click", () => save_outputs(false));
  $("o-save-restart").addEventListener("click", async () => {
    if (await ask_confirm({
      title: t("output.save_restart"),
      message: t("output.save_restart_confirm"),
      danger: false,
    })) {
      save_outputs(true);
    }
  });
  $("o-profile-select").addEventListener("change", () => {
    $("o-profile-name").value = $("o-profile-select").value;
    refresh_profile_buttons();
  });
  $("u-check").addEventListener("click", () => {
    $("u-busy").hidden = false;
    $("u-check").disabled = true;
    $("u-open").disabled = true;
    state.update_manual = true;
    $("u-status").textContent = t("settings.update.checking");
    send({ type: "check_updates" });
  });
  $("ub-primary").addEventListener("click", () => update_primary_action());
  $("ub-later").addEventListener("click", () => {
    // 只是"这次别烦我"：下载/安装状态照旧，引擎那边不受影响
    state.update_dismissed = true;
    render_update();
  });
  $("ub-skip").addEventListener("click", () => skip_update_version());
  $("u-skip").addEventListener("click", () => skip_update_version());
  $("u-install").addEventListener("click", () => install_update());
  $("u-open").addEventListener("click", () => {
    const info = state.update || state.update_info;
    if (info && info.url) {
      send({ type: "open_update_page", url: info.url });
    }
  });
  // 关于卡片的外链：引擎侧只放行作者的 GitHub 页面，外部站点不会被内嵌打开
  document.querySelectorAll("[data-open-url]").forEach((button) => {
    button.addEventListener("click", () => {
      send({ type: "open_update_page", url: button.dataset.openUrl });
    });
  });
  $("diag-export").addEventListener("click", () => {
    $("diag-busy").hidden = false;
    $("diag-export").disabled = true;
    send({ type: "export_diagnostics" });
  });
  $("o-profile-save").addEventListener("click", save_output_profile);
  $("o-profile-apply").addEventListener("click", apply_output_profile);
  $("o-profile-delete").addEventListener("click", delete_output_profile);

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
    send({ type: state.library_tab === "native" ? "refresh_native_library" : "refresh_library" });
  });
  $("lib-search").addEventListener("input", () => {
    render_library();
  });
  $("lib-native-beta").addEventListener("change", () => {
    render_native_library();
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

// ---------- 自动更新（ADR-0036）----------
// 状态只由引擎给（idle/checking/available/downloading/ready/installing/failed），
// 页面不猜"该不该提示"：它只把快照画出来，并把用户的选择发回引擎。
function update_percent(info) {
  const total = (info && info.progress && info.progress.total) || 0;
  const got = (info && info.progress && info.progress.received) || 0;
  if (!total || total <= 0) {
    return null;
  }
  return Math.max(0, Math.min(100, Math.round((got / total) * 100)));
}

function apply_update(info) {
  const stage = (info && info.state) || "idle";
  // 换了新版本：上次点过的「稍后」不再作数
  if (info && info.latest && info.latest !== state.update_tag_seen) {
    state.update_tag_seen = info.latest;
    state.update_dismissed = false;
  }
  const previous = state.update ? state.update.state : "";
  state.update = info;

  if (stage === "failed" && state.update_manual) {
    show_toast(t("settings.update.failed", { error: (info && info.error) || "?" }), "error");
  }
  if (stage === "ready" && previous !== "ready") {
    show_toast(t("update.ready_toast", { latest: (info && info.latest) || "" }));
  }
  if (stage === "installing" && previous !== "installing") {
    show_toast(t("update.installing_toast"));
  }
  if (stage !== "checking") {
    state.update_manual = false;
  }
  render_update();
}

function render_update() {
  const info = state.update;
  const stage = (info && info.state) || "idle";
  const percent = update_percent(info);

  // ---- 设置页 ----
  $("u-current").textContent = (info && info.current) || "—";
  $("u-latest").textContent = (info && info.latest) || "—";
  $("u-busy").hidden = stage !== "checking";
  $("u-check").disabled = stage === "checking" || stage === "downloading";
  $("u-progress").hidden = stage !== "downloading";
  if (stage === "downloading" && percent !== null) {
    $("u-progress-fill").style.setProperty("--p", `${percent}%`);
  }
  $("u-install").hidden = stage !== "ready";
  $("u-skip").hidden = !(info && info.available && stage !== "installing");
  $("u-open").disabled = !(info && info.url && info.available);

  let status = "—";
  if (stage === "checking") {
    status = t("settings.update.checking");
  } else if (stage === "downloading") {
    status = t("settings.update.downloading", { percent: percent === null ? "?" : percent });
  } else if (stage === "ready") {
    status = t("settings.update.ready", { latest: (info && info.latest) || "" });
  } else if (stage === "installing") {
    status = t("update.text.installing");
  } else if (stage === "failed") {
    status = t("settings.update.failed", { error: (info && info.error) || "?" });
  } else if (stage === "available" || (info && info.available)) {
    status = t("settings.update.available", { latest: (info && info.latest) || "" });
    if (info && info.error) {
      status += ` · ${info.error}`;
    }
  } else if (info && info.latest) {
    status = t("settings.update.latest_state");
  }
  $("u-status").textContent = status;

  // ---- 顶部提示条 ----
  // checking/failed/idle 不打扰：失败只写进设置页（自动检查失败时弹条会很烦人）
  const show_bar = !!info &&
    ["available", "downloading", "ready", "installing"].includes(stage) &&
    (stage === "installing" || !state.update_dismissed);
  $("update-bar").hidden = !show_bar;
  if (!show_bar) {
    return;
  }

  const latest = (info && info.latest) || "";
  $("ub-badge").textContent = t(`update.badge.${stage}`);
  let text = "";
  if (stage === "available") {
    text = t("update.text.available", { latest });
    if (info.error) {
      text += ` · ${info.error}`;
    }
  } else if (stage === "downloading") {
    text = t("update.text.downloading", {
      latest,
      percent: percent === null ? "…" : percent,
    });
  } else if (stage === "ready") {
    text = t("update.text.ready", { latest });
  } else if (stage === "installing") {
    text = t("update.text.installing");
  }
  $("ub-text").textContent = text;

  $("ub-progress").hidden = stage !== "downloading";
  if (stage === "downloading" && percent !== null) {
    $("ub-progress-fill").style.setProperty("--p", `${percent}%`);
  }

  const primary = $("ub-primary");
  primary.hidden = !(stage === "ready" || stage === "available");
  primary.dataset.action = stage === "ready" ? "install" : "page";
  primary.textContent = stage === "ready" ? t("update.action.install") : t("update.action.download");
  $("ub-skip").hidden = stage !== "ready" && stage !== "available";
  $("ub-later").hidden = stage === "installing";
}

function update_primary_action() {
  const action = $("ub-primary").dataset.action;
  if (action === "install") {
    install_update();
    return;
  }
  const info = state.update;
  if (info && info.url) {
    send({ type: "open_update_page", url: info.url });
  }
}

async function install_update() {
  const info = state.update;
  if (!info) {
    return;
  }
  const confirmed = await ask_confirm({
    title: t("update.action.install"),
    message: t("update.install_confirm", { latest: info.latest || "" }),
    confirmText: t("update.action.install"),
    danger: false,
  });
  if (!confirmed) {
    return;
  }
  send({ type: "install_update" });
}

function skip_update_version() {
  const info = state.update;
  if (!info) {
    return;
  }
  send({ type: "skip_update_version" });
  show_toast(t("update.skipped_toast", { latest: info.latest || "" }));
  state.update_dismissed = true;
  render_update();
}

function fmt_int(value) {
  return (value || 0).toLocaleString(locale_tag());
}

apply_l10n();
wire();
sync_output_form();
connect();

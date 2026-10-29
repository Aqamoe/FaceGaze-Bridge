// 校准页的 3D 面部预览：.fgbface 加载 + WebGL2 渲染 + 轨道相机。
//
// 只做校准需要的部分：morph、区域高亮、可旋转的通用人头。不是头像预览器。
// 不引 Three.js（项目规则），网格/相机/光照都是这里自己算的；CPU morph 每帧
// 只回写位置缓冲（7566 顶点 ≈ 90 KB），比引入整套引擎便宜得多。
"use strict";

(function () {
  const FGBFace = {};

  function half_to_float(h) {
    const sign = h & 0x8000 ? -1 : 1;
    const exp = (h >> 10) & 0x1f;
    const frac = h & 0x3ff;
    if (exp === 0) {
      return sign * Math.pow(2, -14) * (frac / 1024);
    }
    if (exp === 31) {
      return frac ? NaN : sign * Infinity;
    }
    return sign * Math.pow(2, exp - 15) * (1 + frac / 1024);
  }

  function parse_model(buffer) {
    const view = new DataView(buffer);
    let offset = 0;
    const tag = String.fromCharCode(view.getUint8(0), view.getUint8(1),
                                    view.getUint8(2), view.getUint8(3));
    if (tag !== "FGBF") {
      throw new Error("不是 .fgbface 文件（magic 不匹配）");
    }
    const version = view.getUint32(4, true);
    const flags = view.getUint32(8, true);
    const vertex_count = view.getUint32(12, true);
    const index_count = view.getUint32(16, true);
    const morph_count = view.getUint16(20, true);
    const region_count = view.getUint8(22);
    offset = 24;
    if (version !== 1 || vertex_count === 0 || morph_count === 0) {
      throw new Error(`不支持的 .fgbface 版本或空模型（v${version}）`);
    }

    const positions = new Float32Array(vertex_count * 3);
    for (let i = 0; i < positions.length; i += 1) {
      positions[i] = view.getFloat32(offset, true);
      offset += 4;
    }
    const normals = new Float32Array(vertex_count * 3);
    for (let i = 0; i < normals.length; i += 1) {
      normals[i] = view.getInt8(offset) / 127.0;
      offset += 1;
    }
    const indices = new Uint32Array(index_count);
    for (let i = 0; i < indices.length; i += 1) {
      indices[i] = view.getUint32(offset, true);
      offset += 4;
    }
    const regions = new Uint32Array(vertex_count);
    for (let i = 0; i < vertex_count; i += 1) {
      regions[i] = view.getUint32(offset, true);
      offset += 4;
    }
    const morph_names = [];
    for (let i = 0; i < morph_count; i += 1) {
      const length = view.getUint8(offset);
      offset += 1;
      const bytes = new Uint8Array(buffer, offset, length);
      morph_names.push(new TextDecoder().decode(bytes));
      offset += length;
    }
    const morph_deltas = [];
    for (let k = 0; k < morph_count; k += 1) {
      const delta = new Float32Array(vertex_count * 3);
      for (let i = 0; i < delta.length; i += 1) {
        delta[i] = half_to_float(view.getUint16(offset, true));
        offset += 2;
      }
      morph_deltas.push(delta);
    }
    return {
      version,
      flags,
      vertex_count,
      index_count,
      region_count,
      positions,
      normals,
      indices,
      regions,
      morph_names,
      morph_deltas,
    };
  }

  function mask_of(region_names, regions) {
    let mask = 0;
    for (const name of region_names || []) {
      const index = regions.indexOf(name);
      if (index >= 0) {
        mask = (mask | (1 << index)) >>> 0;
      }
    }
    return mask;
  }

  // ---------- 矩阵（列主序，直接喂给 uniformMatrix4fv） ----------

  function perspective(fovy, aspect, near, far) {
    const f = 1 / Math.tan(fovy / 2);
    const nf = 1 / (near - far);
    return new Float32Array([
      f / aspect, 0, 0, 0,
      0, f, 0, 0,
      0, 0, (far + near) * nf, -1,
      0, 0, 2 * far * near * nf, 0,
    ]);
  }

  function normalize3(v) {
    const n = Math.hypot(v[0], v[1], v[2]) || 1;
    return [v[0] / n, v[1] / n, v[2] / n];
  }

  function cross3(a, b) {
    return [
      a[1] * b[2] - a[2] * b[1],
      a[2] * b[0] - a[0] * b[2],
      a[0] * b[1] - a[1] * b[0],
    ];
  }

  function dot3(a, b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
  }

  function look_at(eye, target, up) {
    const z = normalize3([eye[0] - target[0], eye[1] - target[1], eye[2] - target[2]]);
    const x = normalize3(cross3(up, z));
    const y = cross3(z, x);
    return new Float32Array([
      x[0], y[0], z[0], 0,
      x[1], y[1], z[1], 0,
      x[2], y[2], z[2], 0,
      -dot3(x, eye), -dot3(y, eye), -dot3(z, eye), 1,
    ]);
  }

  function multiply4(a, b) {
    const out = new Float32Array(16);
    for (let c = 0; c < 4; c += 1) {
      for (let r = 0; r < 4; r += 1) {
        out[c * 4 + r] =
          a[r] * b[c * 4] + a[4 + r] * b[c * 4 + 1] +
          a[8 + r] * b[c * 4 + 2] + a[12 + r] * b[c * 4 + 3];
      }
    }
    return out;
  }

  const VERTEX_SHADER = `#version 300 es
layout(location=0) in vec3 a_pos;
layout(location=1) in vec3 a_normal;
layout(location=2) in uint a_region;
uniform mat4 u_mvp;
out vec3 v_normal;
out vec3 v_world;
flat out uint v_region;
void main() {
  v_region = a_region;
  v_normal = a_normal;
  v_world = a_pos;
  gl_Position = u_mvp * vec4(a_pos, 1.0);
}`;

  const FRAGMENT_SHADER = `#version 300 es
precision highp float;
in vec3 v_normal;
in vec3 v_world;
flat in uint v_region;
uniform uint u_highlight;
uniform uint u_mouth;
uniform uint u_lip;
uniform uint u_brow;
uniform uint u_eye_white;
uniform uint u_iris;
uniform uint u_nostril;
uniform uint u_pupil;
uniform float u_highlight_amt;
uniform vec3 u_eye;
out vec4 fragColor;

vec3 base_color() {
  vec3 c = vec3(0.82, 0.66, 0.58);
  if ((v_region & u_brow) != 0u)      c = vec3(0.18, 0.14, 0.12);
  if ((v_region & u_nostril) != 0u)   c = vec3(0.24, 0.10, 0.10);
  if ((v_region & u_lip) != 0u)       c = vec3(0.66, 0.34, 0.34);
  if ((v_region & u_eye_white) != 0u) c = vec3(0.93, 0.92, 0.90);
  if ((v_region & u_iris) != 0u)      c = vec3(0.22, 0.31, 0.35);
  if ((v_region & u_pupil) != 0u)     c = vec3(0.035, 0.035, 0.045);
  if ((v_region & u_mouth) != 0u)     c = vec3(0.13, 0.035, 0.045);
  return c;
}

void main() {
  vec3 n = normalize(v_normal);
  vec3 view_dir = normalize(u_eye - v_world);

  vec3 key_dir = normalize(vec3(0.45, 0.62, 0.75));
  vec3 fill_dir = normalize(vec3(-0.70, 0.15, 0.45));
  vec3 rim_dir = normalize(vec3(-0.15, -0.35, -0.85));

  float key = clamp(dot(n, key_dir) * 0.5 + 0.5, 0.0, 1.0);
  float fill = clamp(dot(n, fill_dir) * 0.5 + 0.5, 0.0, 1.0);
  float rim = clamp(dot(n, rim_dir) * 0.5 + 0.5, 0.0, 1.0);

  vec3 color = base_color();
  color *= 0.30 + 0.76 * key;
  color += vec3(0.16, 0.10, 0.09) * fill * 0.42;
  color += vec3(0.20, 0.12, 0.10) * rim * 0.30;

  // 很轻的次表面红晕：鼻尖/耳廓/嘴唇边缘不至于像塑料
  float fres = pow(1.0 - max(dot(n, view_dir), 0.0), 3.0);
  color += vec3(0.20, 0.05, 0.03) * fres * 0.28;

  if ((v_region & u_highlight) != 0u) {
    color = mix(color, vec3(0.20, 0.56, 0.96), 0.46 * u_highlight_amt);
    color += vec3(0.10, 0.28, 0.55) * fres * u_highlight_amt;
  }
  fragColor = vec4(color, 1.0);
}`;

  function compile(gl, type, source) {
    const shader = gl.createShader(type);
    gl.shaderSource(shader, source);
    gl.compileShader(shader);
    if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS)) {
      const log = gl.getShaderInfoLog(shader);
      gl.deleteShader(shader);
      throw new Error(`着色器编译失败：${log}`);
    }
    return shader;
  }

  class Renderer {
    constructor(canvas, model, face_map) {
      this.canvas = canvas;
      this.model = model;
      this.map = face_map;
      this.regions = face_map.regions || [];
      this.morph_index = new Map(model.morph_names.map((name, i) => [name, i]));
      this.shape_weights = new Map();
      for (const morph of face_map.morphs || []) {
        const index = this.morph_index.get(morph.name);
        if (index === undefined) {
          continue;
        }
        for (const [shape, weight] of Object.entries(morph.shapes || {})) {
          if (!this.shape_weights.has(shape)) {
            this.shape_weights.set(shape, []);
          }
          this.shape_weights.get(shape).push({ index, weight });
        }
      }
      this.shape_index = new Map();
      this.target = new Float32Array(model.morph_names.length);
      this.shown = new Float32Array(model.morph_names.length);
      this.weights_dirty = true;
      this.highlight_mask = 0;
      this.highlight_target = 0;
      this.highlight_amt = 0;
      this.morphed = new Float32Array(model.positions.length);
      this.morphed.set(model.positions);

      this.yaw = 0.16;
      this.pitch = 0.03;
      this.distance = 3.5;
      this.target_y = -0.25;
      this.running = false;
      this.frame = 0;
      this.last_time = 0;
      this.dragging = null;
      this.resize_observer = null;

      this.masks = {
        mouth: mask_of(["mouthInterior"], this.regions),
        lip: mask_of(["lipColorUpper", "lipColorLower"], this.regions),
        brow: mask_of(["eyebrowL", "eyebrowR"], this.regions),
        nostril: mask_of(["nostrilL", "nostrilR"], this.regions),
        eye_white: mask_of(["eyeWhiteL", "eyeWhiteR"], this.regions),
        iris: mask_of(["irisL", "irisR"], this.regions),
        pupil: mask_of(["pupilL", "pupilR"], this.regions),
      };

      const gl = canvas.getContext("webgl2", {
        alpha: true,
        antialias: true,
        premultipliedAlpha: false,
        preserveDrawingBuffer: false,
      });
      if (!gl) {
        throw new Error("WebGL2 不可用");
      }
      this.gl = gl;
      gl.disable(gl.CULL_FACE); // 程序化网格的绕序不保证一致，深度测试足够
      gl.enable(gl.DEPTH_TEST);
      gl.depthFunc(gl.LEQUAL);
      gl.clearColor(0, 0, 0, 0);

      const program = gl.createProgram();
      gl.attachShader(program, compile(gl, gl.VERTEX_SHADER, VERTEX_SHADER));
      gl.attachShader(program, compile(gl, gl.FRAGMENT_SHADER, FRAGMENT_SHADER));
      gl.linkProgram(program);
      if (!gl.getProgramParameter(program, gl.LINK_STATUS)) {
        throw new Error(`着色器链接失败：${gl.getProgramInfoLog(program)}`);
      }
      this.program = program;
      this.uniform = {
        mvp: gl.getUniformLocation(program, "u_mvp"),
        highlight: gl.getUniformLocation(program, "u_highlight"),
        highlight_amt: gl.getUniformLocation(program, "u_highlight_amt"),
        eye: gl.getUniformLocation(program, "u_eye"),
        mouth: gl.getUniformLocation(program, "u_mouth"),
        lip: gl.getUniformLocation(program, "u_lip"),
        brow: gl.getUniformLocation(program, "u_brow"),
        eye_white: gl.getUniformLocation(program, "u_eye_white"),
        iris: gl.getUniformLocation(program, "u_iris"),
        nostril: gl.getUniformLocation(program, "u_nostril"),
        pupil: gl.getUniformLocation(program, "u_pupil"),
      };

      this.vao = gl.createVertexArray();
      gl.bindVertexArray(this.vao);

      this.pos_buffer = gl.createBuffer();
      gl.bindBuffer(gl.ARRAY_BUFFER, this.pos_buffer);
      gl.bufferData(gl.ARRAY_BUFFER, this.morphed, gl.DYNAMIC_DRAW);
      gl.enableVertexAttribArray(0);
      gl.vertexAttribPointer(0, 3, gl.FLOAT, false, 0, 0);

      this.nrm_buffer = gl.createBuffer();
      gl.bindBuffer(gl.ARRAY_BUFFER, this.nrm_buffer);
      gl.bufferData(gl.ARRAY_BUFFER, model.normals, gl.STATIC_DRAW);
      gl.enableVertexAttribArray(1);
      gl.vertexAttribPointer(1, 3, gl.FLOAT, false, 0, 0);

      this.reg_buffer = gl.createBuffer();
      gl.bindBuffer(gl.ARRAY_BUFFER, this.reg_buffer);
      gl.bufferData(gl.ARRAY_BUFFER, model.regions, gl.STATIC_DRAW);
      gl.enableVertexAttribArray(2);
      gl.vertexAttribIPointer(2, 1, gl.UNSIGNED_INT, 0, 0);

      this.ibo = gl.createBuffer();
      gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, this.ibo);
      gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, model.indices, gl.STATIC_DRAW);
      gl.bindVertexArray(null);

      this.attach_controls();
      this.resize();
    }

    set_shape_indices(names) {
      this.shape_index = new Map((names || []).map((name, i) => [name, i]));
    }

    set_values(out_values) {
      const values = out_values || [];
      this.target.fill(0);
      for (const [shape, entries] of this.shape_weights) {
        const index = this.shape_index.get(shape);
        if (index === undefined) {
          continue;
        }
        const value = Number(values[index]) || 0;
        if (value === 0) {
          continue;
        }
        for (const entry of entries) {
          this.target[entry.index] += value * entry.weight;
        }
      }
      // 眼追的睁眼度直接驱动眨眼：即使没有形状数据，闭眼也能看到眼皮合上
      for (const [side, index] of [["L", "eyeBlinkL"], ["R", "eyeBlinkR"]]) {
        const morph = this.morph_index.get(index);
        if (morph !== undefined) {
          const openness = this.eye_open && this.eye_open[side];
          if (openness !== undefined) {
            this.target[morph] = Math.max(this.target[morph], 1 - Number(openness));
          }
        }
      }
      for (let i = 0; i < this.target.length; i += 1) {
        this.target[i] = Math.min(1, Math.max(0, this.target[i]));
      }
      this.weights_dirty = true;
    }

    set_eye(eye) {
      this.eye_open = eye && eye.valid ? { L: eye.open_l, R: eye.open_r } : null;
    }

    set_highlight(region_names) {
      this.highlight_target = mask_of(region_names, this.regions);
    }

    set_view(name) {
      if (name === "front") {
        this.yaw = 0;
        this.pitch = 0;
      } else if (name === "side") {
        this.yaw = Math.PI / 2;
        this.pitch = 0;
      } else if (name === "three-quarter") {
        this.yaw = 0.78;
        this.pitch = 0.03;
      }
      this.distance = 3.5;
    }

    reset() {
      this.yaw = 0.16;
      this.pitch = 0.03;
      this.distance = 3.5;
    }

    attach_controls() {
      const canvas = this.canvas;
      canvas.addEventListener("pointerdown", (event) => {
        if (event.button !== 0) {
          return;
        }
        canvas.setPointerCapture(event.pointerId);
        this.dragging = { x: event.clientX, y: event.clientY };
        canvas.classList.add("grabbing");
      });
      canvas.addEventListener("pointermove", (event) => {
        if (!this.dragging) {
          return;
        }
        const dx = event.clientX - this.dragging.x;
        const dy = event.clientY - this.dragging.y;
        this.dragging = { x: event.clientX, y: event.clientY };
        this.yaw -= dx * 0.009;
        const limit = (80 * Math.PI) / 180;
        this.pitch = Math.min(limit, Math.max(-limit, this.pitch + dy * 0.008));
      });
      const stop = () => {
        this.dragging = null;
        canvas.classList.remove("grabbing");
      };
      canvas.addEventListener("pointerup", stop);
      canvas.addEventListener("pointercancel", stop);
      canvas.addEventListener("wheel", (event) => {
        event.preventDefault();
        this.distance = Math.min(6.5, Math.max(2.0, this.distance * Math.exp(event.deltaY * 0.001)));
      }, { passive: false });
      canvas.addEventListener("dblclick", () => this.reset());
    }

    resize() {
      const dpr = Math.min(window.devicePixelRatio || 1, 2);
      const width = Math.max(1, Math.round(this.canvas.clientWidth * dpr));
      const height = Math.max(1, Math.round(this.canvas.clientHeight * dpr));
      if (this.canvas.width !== width || this.canvas.height !== height) {
        this.canvas.width = width;
        this.canvas.height = height;
      }
    }

    start() {
      if (this.running) {
        return;
      }
      this.running = true;
      this.last_time = 0;
      this.resize();
      const loop = (now) => {
        if (!this.running) {
          return;
        }
        const dt = this.last_time ? Math.min(0.1, (now - this.last_time) / 1000) : 0;
        this.last_time = now;
        this.resize();
        this.step(dt);
        this.draw();
        this.frame = requestAnimationFrame(loop);
      };
      this.frame = requestAnimationFrame(loop);
    }

    stop() {
      this.running = false;
      if (this.frame) {
        cancelAnimationFrame(this.frame);
        this.frame = 0;
      }
    }

    step(dt) {
      const k = dt > 0 ? Math.min(1, dt * 16) : 1;
      let changed = false;
      for (let i = 0; i < this.target.length; i += 1) {
        const delta = this.target[i] - this.shown[i];
        if (Math.abs(delta) > 0.0005) {
          this.shown[i] += delta * k;
          changed = true;
        } else {
          this.shown[i] = this.target[i];
        }
      }
      if (changed || this.weights_dirty) {
        const base = this.model.positions;
        const morphs = this.model.morph_deltas;
        const out = this.morphed;
        out.set(base);
        for (let m = 0; m < this.shown.length; m += 1) {
          const w = this.shown[m];
          if (w === 0) {
            continue;
          }
          const delta = morphs[m];
          for (let i = 0; i < out.length; i += 1) {
            out[i] += delta[i] * w;
          }
        }
        const gl = this.gl;
        gl.bindBuffer(gl.ARRAY_BUFFER, this.pos_buffer);
        gl.bufferSubData(gl.ARRAY_BUFFER, 0, out);
        this.weights_dirty = false;
      }
      const want = this.highlight_target !== 0 ? 1 : 0;
      this.highlight_amt += (want - this.highlight_amt) * k;
      if (Math.abs(want - this.highlight_amt) < 0.01) {
        this.highlight_amt = want;
      }
      if (this.highlight_target !== 0) {
        this.highlight_mask = this.highlight_target;
      }
    }

    draw() {
      const gl = this.gl;
      const width = this.canvas.width;
      const height = this.canvas.height;
      if (width <= 1 || height <= 1) {
        return;
      }
      gl.viewport(0, 0, width, height);
      gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);

      const cp = Math.cos(this.pitch);
      const eye = [
        Math.sin(this.yaw) * cp * this.distance,
        this.target_y + Math.sin(this.pitch) * this.distance,
        Math.cos(this.yaw) * cp * this.distance,
      ];
      const target = [0, this.target_y, 0];
      const view = look_at(eye, target, [0, 1, 0]);
      const proj = perspective((38 * Math.PI) / 180, width / height, 0.1, 50);
      const mvp = multiply4(proj, view);

      gl.useProgram(this.program);
      gl.bindVertexArray(this.vao);
      gl.uniformMatrix4fv(this.uniform.mvp, false, mvp);
      gl.uniform1ui(this.uniform.highlight, this.highlight_mask >>> 0);
      gl.uniform1f(this.uniform.highlight_amt, this.highlight_amt);
      gl.uniform3f(this.uniform.eye, eye[0], eye[1], eye[2]);
      gl.uniform1ui(this.uniform.mouth, this.masks.mouth >>> 0);
      gl.uniform1ui(this.uniform.lip, this.masks.lip >>> 0);
      gl.uniform1ui(this.uniform.brow, this.masks.brow >>> 0);
      gl.uniform1ui(this.uniform.eye_white, this.masks.eye_white >>> 0);
      gl.uniform1ui(this.uniform.iris, this.masks.iris >>> 0);
      gl.uniform1ui(this.uniform.nostril, this.masks.nostril >>> 0);
      gl.uniform1ui(this.uniform.pupil, this.masks.pupil >>> 0);
      gl.drawElements(gl.TRIANGLES, this.model.index_count, gl.UNSIGNED_INT, 0);
      gl.bindVertexArray(null);
    }
  }

  class Fallback {
    constructor(svg) {
      this.svg = svg;
      this.regions = new Map();
      for (const node of svg.querySelectorAll("[data-region]")) {
        const name = node.dataset.region;
        if (!this.regions.has(name)) {
          this.regions.set(name, []);
        }
        this.regions.get(name).push(node);
      }
      this.target = new Set();
    }

    set_highlight(region_names) {
      const next = new Set(region_names || []);
      for (const [name, nodes] of this.regions) {
        const on = next.has(name);
        for (const node of nodes) {
          node.classList.toggle("hl", on);
        }
      }
      this.target = next;
    }
  }

  FGBFace.parse_model = parse_model;
  FGBFace.Renderer = Renderer;
  FGBFace.Fallback = Fallback;
  FGBFace.mask_of = mask_of;
  window.FGBFace = FGBFace;
})();

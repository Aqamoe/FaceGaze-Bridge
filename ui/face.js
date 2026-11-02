// 校准页的 3D 面部预览：.fgbface 加载 + WebGL2 渲染 + 轨道相机。
//
// 只做校准需要的部分：morph、区域高亮、可旋转的通用人头。不是头像预览器。
// 不引 Three.js（项目规则），网格/相机/光照都是这里自己算的。
//
// v3 资产（tools/bake_gnm_head.py 烘焙的 GNM 中性头）带来三件事：
//   1. 位移是稀疏存储的——下颌 morph 只动 5k 个顶点，每帧不必扫全网格；
//   2. 基色是逐顶点的（v3 前是逐顶点材质 id）——眉毛/唇线在网格上没有几何分界，
//      用材质 id 只能切出锯齿边，逐顶点颜色由光栅化插值成柔和过渡；
//      材质 id 仍保留，只用来区分高光强度（唇亮、巩膜亮、皮肤哑光）；
//   3. morph 之后重算法线（节流），强变形时光照不会塌。
"use strict";

(function () {
  const FGBFace = {};

  // 材质名 → 高光强度。id 来自资产头部，face_map.json 里的 materials 顺序与之一致；
  // 未知材质按皮肤处理，换资产不会白屏。颜色在 v3 里是逐顶点的（见 colors）。
  const MATERIAL_STYLE = {
    skin: { color: [0.87, 0.71, 0.63], spec: 0.10 },
    lip: { color: [0.76, 0.50, 0.47], spec: 0.30 },
    brow: { color: [0.30, 0.22, 0.18], spec: 0.05 },
    sclera: { color: [0.95, 0.95, 0.93], spec: 0.85 },
    iris: { color: [0.26, 0.34, 0.37], spec: 0.65 },
    pupil: { color: [0.04, 0.04, 0.05], spec: 0.20 },
    mouth: { color: [0.17, 0.05, 0.06], spec: 0.25 },
    teeth: { color: [0.95, 0.93, 0.89], spec: 0.55 },
    gums: { color: [0.78, 0.45, 0.45], spec: 0.25 },
    tongue: { color: [0.70, 0.38, 0.40], spec: 0.25 },
    nostril: { color: [0.32, 0.15, 0.15], spec: 0.05 },
  };
  const MATERIAL_COLORS = Object.values(MATERIAL_STYLE).map((style) => style.color);
  const MAX_MATERIALS = 16;

  function parse_model(buffer) {
    const view = new DataView(buffer);
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
    const material_count = view.getUint8(23);
    if (version !== 2 && version !== 3) {
      throw new Error(`不支持的 .fgbface 版本 v${version}（需要 v2/v3）`);
    }
    if (vertex_count === 0 || morph_count === 0) {
      throw new Error("空模型");
    }
    let offset = 24;

    const positions = new Float32Array(buffer.slice(offset, offset + vertex_count * 12));
    offset += vertex_count * 12;
    const normals = new Float32Array(vertex_count * 3);
    for (let i = 0; i < normals.length; i += 1) {
      normals[i] = view.getInt8(offset) / 127.0;
      offset += 1;
    }
    const indices = new Uint16Array(buffer.slice(offset, offset + index_count * 2));
    offset += index_count * 2;
    const regions = new Uint32Array(buffer.slice(offset, offset + vertex_count * 4));
    offset += vertex_count * 4;
    const materials = new Uint8Array(buffer.slice(offset, offset + vertex_count));
    offset += vertex_count;

    // v3：逐顶点基色。v2 没有这一块，用材质调色板顶替，保证旧资产也能显示。
    let colors;
    if (version >= 3) {
      colors = new Uint8Array(buffer.slice(offset, offset + vertex_count * 3));
      offset += vertex_count * 3;
    } else {
      colors = new Uint8Array(vertex_count * 3);
      for (let i = 0; i < vertex_count; i += 1) {
        const rgb = MATERIAL_COLORS[materials[i]] || MATERIAL_COLORS[0];
        colors[i * 3] = Math.round(rgb[0] * 255);
        colors[i * 3 + 1] = Math.round(rgb[1] * 255);
        colors[i * 3 + 2] = Math.round(rgb[2] * 255);
      }
    }

    const morph_names = [];
    for (let i = 0; i < morph_count; i += 1) {
      const length = view.getUint8(offset);
      offset += 1;
      const bytes = new Uint8Array(buffer, offset, length);
      morph_names.push(new TextDecoder().decode(bytes));
      offset += length;
    }

    // 稀疏位移：每个 morph 一段 f32 scale + u32 count + u16 顶点索引 + i16 三分量
    const morph_deltas = [];
    for (let k = 0; k < morph_count; k += 1) {
      const scale = view.getFloat32(offset, true);
      const count = view.getUint32(offset + 4, true);
      offset += 8;
      const index = new Uint16Array(buffer.slice(offset, offset + count * 2));
      offset += count * 2;
      const value = new Int16Array(buffer.slice(offset, offset + count * 6));
      offset += count * 6;
      morph_deltas.push({ scale, index, value });
    }

    return {
      version,
      flags,
      vertex_count,
      index_count,
      region_count,
      material_count,
      positions,
      normals,
      indices,
      regions,
      materials,
      colors,
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
layout(location=3) in uint a_mat;
layout(location=4) in vec3 a_color;
uniform mat4 u_mvp;
out vec3 v_normal;
out vec3 v_world;
out vec3 v_color;
flat out uint v_region;
flat out uint v_mat;
void main() {
  v_region = a_region;
  v_mat = a_mat;
  v_normal = a_normal;
  v_world = a_pos;
  // 基色逐顶点插值：眉毛、唇线在网格上没有几何分界，只有这样才能平滑过渡
  v_color = a_color;
  gl_Position = u_mvp * vec4(a_pos, 1.0);
}`;

  const FRAGMENT_SHADER = `#version 300 es
precision highp float;
in vec3 v_normal;
in vec3 v_world;
in vec3 v_color;
flat in uint v_region;
flat in uint v_mat;
uniform uint u_highlight;
uniform float u_highlight_amt;
uniform vec3 u_eye;
uniform float u_mat_spec[${MAX_MATERIALS}];
out vec4 fragColor;

void main() {
  vec3 n = normalize(v_normal);
  vec3 view_dir = normalize(u_eye - v_world);
  int mat = int(v_mat);

  vec3 key_dir = normalize(vec3(0.42, 0.58, 0.78));
  vec3 fill_dir = normalize(vec3(-0.72, 0.12, 0.42));
  vec3 rim_dir = normalize(vec3(-0.15, -0.32, -0.86));

  float key = clamp(dot(n, key_dir) * 0.5 + 0.5, 0.0, 1.0);
  float fill = clamp(dot(n, fill_dir) * 0.5 + 0.5, 0.0, 1.0);
  float rim = clamp(dot(n, rim_dir) * 0.5 + 0.5, 0.0, 1.0);

  vec3 color = v_color;
  color *= 0.30 + 0.78 * key;
  color += vec3(0.16, 0.10, 0.09) * fill * 0.40;
  color += vec3(0.20, 0.12, 0.10) * rim * 0.26;

  // 很轻的次表面红晕 + 一点高光：扫描头的皮肤不至于像塑料，眼睛也有湿润感
  float fres = pow(1.0 - max(dot(n, view_dir), 0.0), 3.0);
  color += vec3(0.20, 0.05, 0.03) * fres * 0.26;
  vec3 half_dir = normalize(key_dir + view_dir);
  float spec = pow(max(dot(n, half_dir), 0.0), 42.0);
  color += vec3(1.0, 0.97, 0.94) * spec * u_mat_spec[mat];

  if ((v_region & u_highlight) != 0u) {
    color = mix(color, vec3(0.20, 0.56, 0.96), 0.42 * u_highlight_amt);
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

  // 相机与模型尺寸配套：GNM 头高约 2.0 个模型单位，中心在原点
  const CAMERA = { yaw: 0.16, pitch: 0.03, distance: 3.75, target_y: -0.04 };
  const PITCH_LIMIT = (80 * Math.PI) / 180;

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
      this.morphed = new Float32Array(model.positions);
      this.normals = new Float32Array(model.normals);
      this.normal_dirty = true;
      this.normal_frame = -100;

      this.yaw = CAMERA.yaw;
      this.pitch = CAMERA.pitch;
      this.distance = CAMERA.distance;
      this.target_y = CAMERA.target_y;
      this.running = false;
      this.frame = 0;
      this.last_time = 0;
      this.dragging = null;

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
        mat_spec: gl.getUniformLocation(program, "u_mat_spec[0]"),
      };
      this.build_material_table(face_map);

      this.vao = gl.createVertexArray();
      gl.bindVertexArray(this.vao);

      this.pos_buffer = gl.createBuffer();
      gl.bindBuffer(gl.ARRAY_BUFFER, this.pos_buffer);
      gl.bufferData(gl.ARRAY_BUFFER, this.morphed, gl.DYNAMIC_DRAW);
      gl.enableVertexAttribArray(0);
      gl.vertexAttribPointer(0, 3, gl.FLOAT, false, 0, 0);

      this.nrm_buffer = gl.createBuffer();
      gl.bindBuffer(gl.ARRAY_BUFFER, this.nrm_buffer);
      gl.bufferData(gl.ARRAY_BUFFER, this.normals, gl.DYNAMIC_DRAW);
      gl.enableVertexAttribArray(1);
      gl.vertexAttribPointer(1, 3, gl.FLOAT, false, 0, 0);

      this.reg_buffer = gl.createBuffer();
      gl.bindBuffer(gl.ARRAY_BUFFER, this.reg_buffer);
      gl.bufferData(gl.ARRAY_BUFFER, model.regions, gl.STATIC_DRAW);
      gl.enableVertexAttribArray(2);
      gl.vertexAttribIPointer(2, 1, gl.UNSIGNED_INT, 0, 0);

      this.mat_buffer = gl.createBuffer();
      gl.bindBuffer(gl.ARRAY_BUFFER, this.mat_buffer);
      gl.bufferData(gl.ARRAY_BUFFER, model.materials, gl.STATIC_DRAW);
      gl.enableVertexAttribArray(3);
      gl.vertexAttribIPointer(3, 1, gl.UNSIGNED_BYTE, 0, 0);

      this.color_buffer = gl.createBuffer();
      gl.bindBuffer(gl.ARRAY_BUFFER, this.color_buffer);
      gl.bufferData(gl.ARRAY_BUFFER, model.colors, gl.STATIC_DRAW);
      gl.enableVertexAttribArray(4);
      // normalized=true：u8 归一化到 0..1，省 3/4 显存，也省一次 JS 侧转换
      gl.vertexAttribPointer(4, 3, gl.UNSIGNED_BYTE, true, 0, 0);

      this.ibo = gl.createBuffer();
      gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, this.ibo);
      gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, model.indices, gl.STATIC_DRAW);
      gl.bindVertexArray(null);

      this.attach_controls();
      this.resize();
    }

    build_material_table(face_map) {
      const names = face_map.materials || [];
      const specs = new Float32Array(MAX_MATERIALS);
      for (let i = 0; i < MAX_MATERIALS; i += 1) {
        specs[i] = (MATERIAL_STYLE[names[i]] || MATERIAL_STYLE.skin).spec;
      }
      this.material_specs = specs;
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
      for (const [side, morph_name] of [["L", "eyeBlinkL"], ["R", "eyeBlinkR"]]) {
        const morph = this.morph_index.get(morph_name);
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
      this.distance = CAMERA.distance;
    }

    reset() {
      this.yaw = CAMERA.yaw;
      this.pitch = CAMERA.pitch;
      this.distance = CAMERA.distance;
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
        this.pitch = Math.min(PITCH_LIMIT, Math.max(-PITCH_LIMIT, this.pitch + dy * 0.008));
      });
      const stop = () => {
        this.dragging = null;
        canvas.classList.remove("grabbing");
      };
      canvas.addEventListener("pointerup", stop);
      canvas.addEventListener("pointercancel", stop);
      canvas.addEventListener("wheel", (event) => {
        event.preventDefault();
        this.distance = Math.min(6.2, Math.max(2.2, this.distance * Math.exp(event.deltaY * 0.001)));
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
        this.frame_counter = (this.frame_counter || 0) + 1;
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
        this.apply_morphs();
        this.weights_dirty = false;
        // 法线重算比形变本身贵：每两帧最多一次，形变停下来后再补一帧
        const counter = this.frame_counter || 0;
        if (counter - this.normal_frame >= 2 || !changed) {
          this.recompute_normals();
          this.normal_frame = counter;
        } else {
          this.normal_dirty = true;
        }
      } else if (this.normal_dirty) {
        this.recompute_normals();
        this.normal_dirty = false;
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

    apply_morphs() {
      const out = this.morphed;
      out.set(this.model.positions);
      const morphs = this.model.morph_deltas;
      for (let m = 0; m < this.shown.length; m += 1) {
        const w = this.shown[m];
        if (w <= 0) {
          continue;
        }
        const delta = morphs[m];
        const scale = delta.scale * w;
        const index = delta.index;
        const value = delta.value;
        for (let i = 0; i < index.length; i += 1) {
          const v = index[i] * 3;
          const q = i * 3;
          out[v] += value[q] * scale;
          out[v + 1] += value[q + 1] * scale;
          out[v + 2] += value[q + 2] * scale;
        }
      }
      const gl = this.gl;
      gl.bindBuffer(gl.ARRAY_BUFFER, this.pos_buffer);
      gl.bufferSubData(gl.ARRAY_BUFFER, 0, out);
    }

    recompute_normals() {
      const pos = this.morphed;
      const idx = this.model.indices;
      const nrm = this.normals;
      nrm.fill(0);
      for (let i = 0; i < idx.length; i += 3) {
        const a = idx[i] * 3;
        const b = idx[i + 1] * 3;
        const c = idx[i + 2] * 3;
        const abx = pos[b] - pos[a];
        const aby = pos[b + 1] - pos[a + 1];
        const abz = pos[b + 2] - pos[a + 2];
        const acx = pos[c] - pos[a];
        const acy = pos[c + 1] - pos[a + 1];
        const acz = pos[c + 2] - pos[a + 2];
        const nx = aby * acz - abz * acy;
        const ny = abz * acx - abx * acz;
        const nz = abx * acy - aby * acx;
        nrm[a] += nx; nrm[a + 1] += ny; nrm[a + 2] += nz;
        nrm[b] += nx; nrm[b + 1] += ny; nrm[b + 2] += nz;
        nrm[c] += nx; nrm[c + 1] += ny; nrm[c + 2] += nz;
      }
      for (let i = 0; i < nrm.length; i += 3) {
        const len = Math.hypot(nrm[i], nrm[i + 1], nrm[i + 2]) || 1;
        nrm[i] /= len;
        nrm[i + 1] /= len;
        nrm[i + 2] /= len;
      }
      const gl = this.gl;
      gl.bindBuffer(gl.ARRAY_BUFFER, this.nrm_buffer);
      gl.bufferSubData(gl.ARRAY_BUFFER, 0, nrm);
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
      gl.uniform1fv(this.uniform.mat_spec, this.material_specs);
      gl.drawElements(gl.TRIANGLES, this.model.index_count, gl.UNSIGNED_SHORT, 0);
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

#include "sinks/ws_json_sink.hpp"

#include "base/json.hpp"
#include "base/log.hpp"
#include "core/model/shape.hpp"

#include <utility>

namespace fgb {
namespace {

constexpr char kTag[] = "ws-out";

} // namespace

WsJsonSink::WsJsonSink(WsJsonConfig config) : config_(std::move(config)) {}

bool WsJsonSink::start() {
    // port 0 表示让系统分配：探针/单测用，CLI 会把真实端口打进日志
    server_.on_http = [](const net::ws::HttpHead& head, std::string& body,
                         std::string& content_type) {
        if (head.path != "/" && head.path != "/healthz") {
            return false;
        }
        body         = R"({"service":"fgb-ws-json","version":1})";
        content_type = "application/json";
        return true;
    };

    std::string error;
    if (!server_.listen(config_.port, error, true)) {
        // 端口被占只关这一路：不能因为一个可选输出让整个引擎起不来
        FGB_ERROR(kTag, "WebSocket JSON 监听失败：%s（引擎继续运行，WS 输出关闭）",
                  error.c_str());
        return true;
    }
    started_.store(true);
    if (config_.min_interval_ns == 0) {
        FGB_INFO(kTag, "WebSocket JSON 输出 → ws://127.0.0.1:%u/ws（每 tick，%s）",
                 static_cast<unsigned>(server_.port()),
                 config_.all_shapes ? "全量形状" : "只发活跃形状");
    } else {
        FGB_INFO(kTag, "WebSocket JSON 输出 → ws://127.0.0.1:%u/ws（%.1f Hz，%s）",
                 static_cast<unsigned>(server_.port()),
                 1e9 / static_cast<double>(config_.min_interval_ns),
                 config_.all_shapes ? "全量形状" : "只发活跃形状");
    }
    return true;
}

void WsJsonSink::stop() noexcept {
    started_.store(false);
    server_.close();
}

std::string WsJsonSink::build_json(const FaceFrame& frame, const GazeState& gaze,
                                   std::uint64_t now_ns, std::uint64_t seq,
                                   std::string_view source, bool all_shapes, float epsilon) {
    std::string out;
    out.reserve(4096);
    json::JsonWriter writer{out};

    writer.obj_begin();
    writer.key("t").integer(static_cast<long long>(now_ns / 1'000'000ULL));
    writer.key("seq").integer(static_cast<long long>(seq));
    writer.key("src").str(source);

    writer.key("eye").obj_begin();
    const auto write_eye = [&writer](std::string_view name, const EyeState& eye) {
        writer.key(name).obj_begin();
        writer.key("open").num(eye.openness, 3);
        writer.key("gaze").arr_begin();
        writer.num(eye.gazeDirLocal.x, 3);
        writer.num(eye.gazeDirLocal.y, 3);
        writer.num(eye.gazeDirLocal.z, 3);
        writer.arr_end();
        writer.key("pupil").num(eye.pupilDiameterMm, 2);
        writer.key("ok").integer(eye.valid ? 1 : 0);
        writer.obj_end();
    };
    write_eye("left", frame.eyes.left);
    write_eye("right", frame.eyes.right);
    writer.key("vergence").num(frame.eyes.hasVergence ? frame.eyes.vergenceDistanceM
                                                      : gaze.vergence_m,
                               3);
    writer.obj_end(); // eye

    // 渲染注视（ADR-0030 v1.1）：与 eye 分开，avatar 眼睛仍看上面的 eye。
    writer.key("gaze").obj_begin();
    writer.key("ok").integer(gaze.valid ? 1 : 0);
    const auto write_gaze_eye = [&writer](std::string_view name, bool valid, Vec3 dir,
                                          Vec2 point, bool point_valid) {
        writer.key(name).obj_begin();
        writer.key("ok").integer(valid ? 1 : 0);
        writer.key("dir").arr_begin();
        writer.num(dir.x, 3);
        writer.num(dir.y, 3);
        writer.num(dir.z, 3);
        writer.arr_end();
        writer.key("point_ok").integer(point_valid ? 1 : 0);
        writer.key("point").arr_begin();
        writer.num(point.x, 4);
        writer.num(point.y, 4);
        writer.arr_end();
        writer.obj_end();
    };
    write_gaze_eye("left", gaze.left_valid, gaze.left_dir_local, gaze.left_point_2d,
                   gaze.point2d_valid);
    write_gaze_eye("right", gaze.right_valid, gaze.right_dir_local, gaze.right_point_2d,
                   gaze.point2d_valid);
    writer.key("dir").arr_begin();
    writer.num(gaze.dir_local.x, 3);
    writer.num(gaze.dir_local.y, 3);
    writer.num(gaze.dir_local.z, 3);
    writer.arr_end();
    writer.key("ts_ns").integer(static_cast<long long>(gaze.capture_ts_ns));
    writer.key("src").integer(static_cast<long long>(gaze.source));
    writer.key("mask").integer(static_cast<long long>(gaze.source_mask));
    // 点坐标空间（ADR-0033）：不设这个字段的旧消费端按 source 处理
    writer.key("point_space").str(gaze.point_viewport ? "viewport" : "source");
    writer.obj_end();

    if (frame.hasHeadPose) {
        writer.key("head").obj_begin();
        writer.key("q").arr_begin();
        writer.num(frame.headRotLocal.x, 3);
        writer.num(frame.headRotLocal.y, 3);
        writer.num(frame.headRotLocal.z, 3);
        writer.num(frame.headRotLocal.w, 3);
        writer.arr_end();
        writer.key("p").arr_begin();
        writer.num(frame.headPosLocal.x, 3);
        writer.num(frame.headPosLocal.y, 3);
        writer.num(frame.headPosLocal.z, 3);
        writer.arr_end();
        writer.obj_end();
    }

    writer.key("shapes").obj_begin();
    for (std::size_t i = 0; i < kShapeCount; ++i) {
        const float value = frame.shapes[i];
        if (!all_shapes && (frame.shapeValid[i] == 0 || value <= epsilon)) {
            continue;
        }
        writer.key(shape_name(static_cast<Shape>(i))).num(value, 3);
    }
    writer.obj_end();

    writer.obj_end();
    return out;
}

void WsJsonSink::submit(const FaceFrame& frame, const GazeState& gaze, std::uint64_t now_ns) {
    if (!started_.load()) {
        return;
    }

    // 每 tick 收一轮 accept/帧，保证客户端连上后下一个节流点就能收到数据
    server_.poll(0);
    if (server_.ready_count() == 0) {
        ++stats_.suppressed;
        return;
    }
    if (!throttle_.due(now_ns, config_.min_interval_ns)) {
        ++stats_.suppressed;
        return;
    }

    const std::string text = build_json(frame, gaze, now_ns, ++seq_, config_.source_name,
                                        config_.all_shapes, config_.shape_epsilon);
    const std::size_t delivered = server_.broadcast(text);
    if (delivered == 0) {
        ++stats_.suppressed;
        return;
    }
    stats_.messages += delivered;
    stats_.bytes += text.size() * delivered;
}

} // namespace fgb

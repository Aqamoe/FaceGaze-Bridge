// 本地控制面板。
//
// 为什么是"浏览器 + HTTP"而不是原生窗口：
//   1. 零依赖。不用引 Qt/WinUI/D3D，CI 上不用装图形栈
//   2. 我能完整自测——原生窗口我看不到也点不了，只能麻烦人肉验证
//   3. 界面上要迭代（滑杆、曲线、日志），HTML 改起来比 C++ 快一个数量级
//
// 只绑 127.0.0.1，且只处理一个请求就关连接。这不是服务器，是个"本机遥控器"。
#pragma once

#include "core/process/one_euro.hpp"
#include "engine/engine.hpp"
#include "net/tcp_listener.hpp"
#include "sinks/vrchat_sink.hpp"
#include "web/json.hpp"

#include <atomic>
#include <cstdint>
#include <string>

namespace fgb::web {

struct PanelContext {
    Engine* engine       = nullptr;
    VrchatSink* sink     = nullptr; // 可能为空（没配 OSC 输出）
    FaceFilter* filter   = nullptr; // 可能为空（未启用滤波）

    std::string source_name;
    std::string avatar_name;
    std::string avatar_id;
    std::string params_mode;
    std::string eye_mode;
    std::size_t param_count  = 0;
    std::size_t target_count = 0;
    std::size_t binary_count = 0;

    std::atomic<bool>* quit = nullptr;
};

class Panel {
public:
    explicit Panel(PanelContext context);

    bool start(std::uint16_t port, std::string& error);
    void stop();

    // 处理至多一个连接（测试用），返回是否处理了请求
    bool serve_once(std::uint16_t timeout_ms);

    // 阻塞循环，直到 stop_flag 置位或收到 /api/quit
    void run(std::atomic<bool>& stop_flag);

private:
    // 返回 HTTP 响应体；status_code 为 200 或 4xx
    std::string handle(const HttpRequest& request, int& status_code);

    PanelContext ctx_;
    net::TcpListener listener_;
};

} // namespace fgb::web

// 日志去重闸门（ADR-0046）：上游模块每帧报同一句话时，把日志折叠成
// "首条 + 一条带计数的结算"，而不是原样每帧落一行。
#include "base/log.hpp"
#include "base/log_gate.hpp"
#include "base/log_rotate.hpp"
#include "sources/native_source.hpp"

#include "test_framework.hpp"

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace fs = std::filesystem;
using fgb::log::RepetitionGate;
using fgb::log::repetition_key;

namespace {

constexpr std::uint64_t kSecond = 1'000'000'000ULL;

fs::path exe_dir() {
    wchar_t buffer[MAX_PATH]{};
    const DWORD length = ::GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    return length > 0 ? fs::path{buffer}.parent_path() : fs::current_path();
}

} // namespace

FGB_TEST(log_gate_key_ignores_changing_numbers) {
    // 模块日志里"累计 N 条""已 N 秒"每次都变，抹平数字才认得出是同一类
    FGB_CHECK(repetition_key("上报了 16 条") == repetition_key("上报了 6101176 条"));
    FGB_CHECK(repetition_key("已 10 秒没有数据") == repetition_key("已 18446744073 秒没有数据"));
    FGB_CHECK(repetition_key("地址 /sl/x/x") != repetition_key("地址 /sl/x/y"));
    // 连续数字只抹成一个 #，不是逐位替换
    FGB_CHECK(repetition_key("count=12345") == "count=#");
    FGB_CHECK(repetition_key("no digits") == "no digits");
}

FGB_TEST(log_gate_folds_inside_window) {
    RepetitionGate gate{10 * kSecond};
    const auto first = gate.offer("msg 1", 0);
    FGB_CHECK(first.folded.empty());
    FGB_CHECK(first.line == "msg 1");

    for (int i = 2; i <= 100; ++i) {
        const auto again = gate.offer("msg " + std::to_string(i), kSecond);
        FGB_CHECK(again.none());
    }
    // 窗口没到期不结算
    FGB_CHECK(gate.drain_expired(10 * kSecond - 1).empty());

    const auto folded = gate.drain_expired(10 * kSecond);
    FGB_CHECK(folded.size() == 1);
    FGB_CHECK(folded[0].find("99") != std::string::npos);
    FGB_CHECK(folded[0].find("msg 100") != std::string::npos);
    // 结算过就不再重复吐
    FGB_CHECK(gate.drain_expired(10 * kSecond).empty());
}

FGB_TEST(log_gate_expiry_emits_summary_then_line) {
    RepetitionGate gate{kSecond};
    gate.offer("err 1", 0);
    for (int i = 0; i < 5; ++i) {
        gate.offer("err 2", 100);
    }

    const auto out = gate.offer("err 9", 2 * kSecond); // 窗口到期：先结算，再放行
    FGB_CHECK(!out.folded.empty());
    FGB_CHECK(out.line == "err 9");

    // 新窗口从这条重新起算
    FGB_CHECK(gate.offer("err 10", 2 * kSecond + 1).none());
}

FGB_TEST(log_gate_flush_and_reset) {
    RepetitionGate gate{10 * kSecond};
    gate.offer("state 1", 0);
    gate.offer("state 2", kSecond);

    const auto pending = gate.flush(); // 状态恢复/停止时无条件结算
    FGB_CHECK(pending.size() == 1);
    FGB_CHECK(gate.flush().empty());

    gate.reset();
    const auto after = gate.offer("state 3", kSecond);
    FGB_CHECK(after.line == "state 3");
}

FGB_TEST(log_gate_keeps_distinct_messages_apart) {
    RepetitionGate gate{10 * kSecond};
    FGB_CHECK(!gate.offer("alpha 1", 0).line.empty());
    FGB_CHECK(!gate.offer("beta 1", 0).line.empty());
    FGB_CHECK(gate.offer("alpha 2", 0).none());
    FGB_CHECK(gate.offer("beta 2", 0).none());
}

FGB_TEST(log_gate_evicts_oldest_bucket) {
    // 键抹掉了数字，所以这里的 8 类必须靠文字区分（k0/k1 都会被认成 "k#"）
    const char* names[] = {"alpha", "beta", "gamma", "delta", "epsilon", "zeta", "eta", "theta"};
    RepetitionGate gate{10 * kSecond};
    for (const char* name : names) {
        const std::string text = std::string{name} + " 1";
        FGB_CHECK(!gate.offer(text, 0).line.empty());
    }
    for (const char* name : names) {
        const std::string text = std::string{name} + " 2";
        FGB_CHECK(gate.offer(text, kSecond).none());
    }
    // 第 9 类把窗口起点最早的桶顶掉，它欠下的条数必须结清，不能静默丢
    const auto evicted = gate.offer("iota 1", 2 * kSecond);
    FGB_CHECK(evicted.line == "iota 1");
    FGB_CHECK(!evicted.folded.empty());
    FGB_CHECK(evicted.folded.find("alpha 2") != std::string::npos);
}

// 端到端：夹具模块每帧刷 8 条日志 + 1 条错误，宿主必须把它们折掉
FGB_TEST(log_gate_folds_module_log_spam) {
    fgb::NativeSourceConfig config;
    config.dll          = exe_dir() / "fgb-test-log-spam.dll";
    config.module_id    = "com.fgbbridge.test.logspam";
    config.module_name  = "Log Spam Test";
    config.display_name = "logspam";
    config.tick_hz      = 100;

    fgb::NativeModuleSource source{config};
    FGB_CHECK(source.start());

    int lines  = 0;
    int folded = 0;
    fgb::log::set_callback([&](fgb::log::Level, std::string_view, std::string_view message) {
        if (message.find("LOGSPAM") == std::string_view::npos) {
            return;
        }
        ++lines;
        if (message.find("已折叠") != std::string_view::npos) {
            ++folded;
        }
    });

    fgb::FaceFrame frame;
    for (int i = 0; i < 200; ++i) {
        source.poll(frame);
    }
    fgb::log::set_callback({});
    source.stop();

    // 200 帧 = 1600 条日志 + 200 条错误；不折叠就是 1800 行
    FGB_CHECK(lines > 0);
    FGB_CHECK(lines <= 4);
    // 200 帧只用了几毫秒，落在同一个 10 秒窗口里，还不该有结算行
    FGB_CHECK(folded == 0);
}

// engine.log 滚动：超限滚成 .1（只留一份），没超限不动，文件不在也不报错
FGB_TEST(log_rotate_moves_oversized_file_aside) {
    const fs::path dir = fs::temp_directory_path() / "fgb-log-rotate-test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    const fs::path log = dir / "engine.log";

    {
        std::ofstream out(log, std::ios::binary);
        out << std::string(4096, 'x');
    }
    FGB_CHECK(!fgb::rotate_log_file(log, 8 * 1024)); // 没超限：不动
    FGB_CHECK(fs::exists(log, ec));

    FGB_CHECK(fgb::rotate_log_file(log, 1024));
    FGB_CHECK(!fs::exists(log, ec));
    FGB_CHECK(fs::file_size(dir / "engine.log.1", ec) == 4096);

    // 再滚一次：.1 被替换而不是越堆越多
    {
        std::ofstream out(log, std::ios::binary);
        out << std::string(2048, 'y');
    }
    FGB_CHECK(fgb::rotate_log_file(log, 1024));
    FGB_CHECK(fs::file_size(dir / "engine.log.1", ec) == 2048);

    // 文件不存在：返回 false，不抛异常
    FGB_CHECK(!fgb::rotate_log_file(dir / "nope.log", 1024));

    fs::remove_all(dir, ec);
}

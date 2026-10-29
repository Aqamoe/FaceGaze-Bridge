// 测试用崩溃子进程：装好 handler 后故意写空指针。
// 由 tools/probe_crash.py 启动，检查崩溃目录里真的出现 .dmp。
#include "base/crash.hpp"

int main(int argc, char** argv) {
    if (argc < 2) {
        return 2;
    }
    fgb::crash::install(argv[1], "crash-child");
    volatile int* pointer = nullptr;
    *pointer               = 1;
    return 0;
}

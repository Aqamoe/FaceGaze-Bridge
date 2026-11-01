#pragma once
#include <functional>
int RunVrssProvider(int argc,wchar_t** argv,
    const std::function<bool()>& keepRunning = {},
    const std::function<void()>& prepareProfile = {});

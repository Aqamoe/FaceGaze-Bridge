// 系统代理的字符串翻译（不联网）：WinHTTP 的默认代理与 IE/WinINET 的开关注册表项
// 互不相通，下载能不能走代理全靠这里。真实注册表读取不在这里测。
#include "net/proxy_config.hpp"

#include "test_framework.hpp"

#include <string>

using namespace fgb;

FGB_TEST(proxy_winhttp_single_host_covers_all_schemes) {
    FGB_CHECK(net::proxy_for_winhttp("127.0.0.1:7890") == "127.0.0.1:7890");
    FGB_CHECK(net::proxy_for_winhttp(" 127.0.0.1:7890 ") == "127.0.0.1:7890");
    FGB_CHECK(net::proxy_for_winhttp("").empty());
    FGB_CHECK(net::proxy_for_winhttp("   ").empty());
}

FGB_TEST(proxy_winhttp_fills_missing_https) {
    // 只填 http= 是 IE 里最常见的形式；不补 https= 的话所有 HTTPS 请求都会直连
    FGB_CHECK(net::proxy_for_winhttp("http=127.0.0.1:7890") ==
              "http=127.0.0.1:7890;https=127.0.0.1:7890");
    FGB_CHECK(net::proxy_for_winhttp("https=127.0.0.1:7890") ==
              "http=127.0.0.1:7890;https=127.0.0.1:7890");
    FGB_CHECK(net::proxy_for_winhttp("http=127.0.0.1:7890;https=127.0.0.1:7891") ==
              "http=127.0.0.1:7890;https=127.0.0.1:7891");
    // 键名大小写不敏感
    FGB_CHECK(net::proxy_for_winhttp("HTTP=127.0.0.1:7890;HTTPS=127.0.0.1:7891") ==
              "http=127.0.0.1:7890;https=127.0.0.1:7891");
    // 空值不能把已填的那一项覆盖掉
    FGB_CHECK(net::proxy_for_winhttp("http=;https=127.0.0.1:7890") ==
              "http=127.0.0.1:7890;https=127.0.0.1:7890");
}

FGB_TEST(proxy_winhttp_drops_socks) {
    // WinHTTP 不支持 SOCKS：只配了 socks 宁可直连，也别把 SOCKS 端口当 HTTP 代理用
    FGB_CHECK(net::proxy_for_winhttp("socks=127.0.0.1:7890").empty());
    FGB_CHECK(net::proxy_for_winhttp("socks=127.0.0.1:7890;http=127.0.0.1:7891") ==
              "http=127.0.0.1:7891;https=127.0.0.1:7891");
    FGB_CHECK(net::proxy_for_winhttp("ftp=127.0.0.1:7890").empty());
}

FGB_TEST(proxy_winhttp_strips_scheme_prefix) {
    FGB_CHECK(net::proxy_for_winhttp("http://127.0.0.1:7890") == "127.0.0.1:7890");
    FGB_CHECK(net::proxy_for_winhttp("http=http://127.0.0.1:7890;https=https://127.0.0.1:7890") ==
              "http=127.0.0.1:7890;https=127.0.0.1:7890");
}

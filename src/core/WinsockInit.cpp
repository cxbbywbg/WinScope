#include "core/WinsockInit.h"

#include <winsock2.h>

namespace ws {

void ensureWinsock()
{
    // 只做一次,失败也不重试 —— 初始化不了的话后面自然会拿到空地址,
    // 至少不会每次采样都白跑一遍 WSAStartup。
    static const int rc = []() {
        WSADATA data{};
        return WSAStartup(MAKEWORD(2, 2), &data);
    }();
    (void)rc;
}

} // namespace ws

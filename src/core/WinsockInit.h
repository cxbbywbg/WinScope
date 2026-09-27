#pragma once

// Winsock 初始化。
//
// InetNtop / WSAAddressToString 这类函数必须先有 WSAStartup,否则会失败得很安静:
// WSAAddressToString 返回 WSANOTINITIALISED(10093),我们的封装把它当成「没有地址」,
// 界面上就变成 IPv4 一列全是「—」,还查不出是哪一层出的问题。
//
// 内部是函数内静态量,第一次调用时初始化,之后直接返回;线程安全。

namespace ws {

void ensureWinsock();

} // namespace ws

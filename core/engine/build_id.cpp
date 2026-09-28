#include "build_id.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

std::string ExeSha256()
{
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return {};
    DWORD size = GetFileSize(f, nullptr);
    std::vector<unsigned char> data(size);
    DWORD read = 0;
    ReadFile(f, data.data(), size, &read, nullptr);
    CloseHandle(f);

    unsigned char hash[32];
    if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, data.data(), read, hash, sizeof(hash)) != 0)
        return {};

    static const char hex[] = "0123456789abcdef";
    std::string out;
    for (unsigned char b : hash) {
        out += hex[b >> 4];
        out += hex[b & 15];
    }
    return out;
}

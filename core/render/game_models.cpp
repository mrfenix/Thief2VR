#include "game_models.h"

#include "../log.h"

#include <windows.h>
#include <objidl.h>
#include <shlwapi.h>
#include <gdiplus.h>

#include <cctype>
#include <cstdint>
#include <cstring>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shlwapi.lib")

namespace game_models {
namespace {

// --- Inflate (RFC 1951), after Mark Adler's puff.c ------------------------------------
struct Inflater {
    const unsigned char* in;
    size_t in_len, in_pos = 0;
    unsigned bit_buf = 0;
    int bit_cnt = 0;
    std::vector<unsigned char> out;
    bool error = false;

    int Bits(int need)
    {
        long val = bit_buf;
        while (bit_cnt < need) {
            if (in_pos >= in_len) {
                error = true;
                return 0;
            }
            val |= (long)in[in_pos++] << bit_cnt;
            bit_cnt += 8;
        }
        bit_buf = (unsigned)(val >> need);
        bit_cnt -= need;
        return (int)(val & ((1L << need) - 1));
    }

    struct Huffman {
        short count[16];
        short symbol[288];
    };

    int Decode(const Huffman& h)
    {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len < 16; ++len) {
            code |= Bits(1);
            int count = h.count[len];
            if (code - count < first)
                return h.symbol[index + (code - first)];
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
            if (error)
                return -1;
        }
        error = true;
        return -1;
    }

    static bool Construct(Huffman& h, const short* length, int n)
    {
        memset(h.count, 0, sizeof(h.count));
        for (int s = 0; s < n; ++s)
            h.count[length[s]]++;
        if (h.count[0] == n)
            return true;
        int left = 1;
        for (int len = 1; len < 16; ++len) {
            left <<= 1;
            left -= h.count[len];
            if (left < 0)
                return false;
        }
        short offs[16];
        offs[1] = 0;
        for (int len = 1; len < 15; ++len)
            offs[len + 1] = offs[len] + h.count[len];
        for (int s = 0; s < n; ++s)
            if (length[s])
                h.symbol[offs[length[s]]++] = (short)s;
        return true;
    }

    bool Codes(const Huffman& lencode, const Huffman& distcode)
    {
        static const short lbase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                        31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
        static const short lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
        static const short dbase[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                        193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
        static const short dext[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
        for (;;) {
            int symbol = Decode(lencode);
            if (symbol < 0 || error)
                return false;
            if (symbol < 256) {
                out.push_back((unsigned char)symbol);
            } else if (symbol == 256) {
                return true;
            } else {
                symbol -= 257;
                if (symbol >= 29)
                    return false;
                int len = lbase[symbol] + Bits(lext[symbol]);
                symbol = Decode(distcode);
                if (symbol < 0 || symbol >= 30)
                    return false;
                size_t dist = dbase[symbol] + Bits(dext[symbol]);
                if (dist > out.size() || error)
                    return false;
                size_t from = out.size() - dist;
                for (int i = 0; i < len; ++i)
                    out.push_back(out[from + i]);
            }
        }
    }

    bool Stored()
    {
        bit_buf = 0;
        bit_cnt = 0;
        if (in_pos + 4 > in_len)
            return false;
        unsigned len = in[in_pos] | in[in_pos + 1] << 8;
        in_pos += 4;
        if (in_pos + len > in_len)
            return false;
        out.insert(out.end(), in + in_pos, in + in_pos + len);
        in_pos += len;
        return true;
    }

    bool Fixed()
    {
        static Huffman lencode, distcode;
        static bool made;
        if (!made) {
            short lengths[288];
            int s = 0;
            for (; s < 144; ++s) lengths[s] = 8;
            for (; s < 256; ++s) lengths[s] = 9;
            for (; s < 280; ++s) lengths[s] = 7;
            for (; s < 288; ++s) lengths[s] = 8;
            Construct(lencode, lengths, 288);
            for (s = 0; s < 30; ++s) lengths[s] = 5;
            Construct(distcode, lengths, 30);
            made = true;
        }
        return Codes(lencode, distcode);
    }

    bool Dynamic()
    {
        static const short order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
        short lengths[320];
        int nlen = Bits(5) + 257, ndist = Bits(5) + 1, ncode = Bits(4) + 4;
        if (nlen > 286 || ndist > 30 || error)
            return false;
        int index = 0;
        for (; index < ncode; ++index)
            lengths[order[index]] = (short)Bits(3);
        for (; index < 19; ++index)
            lengths[order[index]] = 0;
        Huffman lencode, distcode;
        if (!Construct(lencode, lengths, 19))
            return false;
        index = 0;
        while (index < nlen + ndist) {
            int symbol = Decode(lencode);
            if (symbol < 0)
                return false;
            if (symbol < 16) {
                lengths[index++] = (short)symbol;
            } else {
                short len = 0;
                if (symbol == 16) {
                    if (index == 0)
                        return false;
                    len = lengths[index - 1];
                    symbol = 3 + Bits(2);
                } else if (symbol == 17) {
                    symbol = 3 + Bits(3);
                } else {
                    symbol = 11 + Bits(7);
                }
                if (index + symbol > nlen + ndist)
                    return false;
                while (symbol--)
                    lengths[index++] = len;
            }
        }
        if (lengths[256] == 0)
            return false;
        if (!Construct(lencode, lengths, nlen) || !Construct(distcode, lengths + nlen, ndist))
            return false;
        return Codes(lencode, distcode);
    }

    bool Run()
    {
        int last;
        do {
            last = Bits(1);
            int type = Bits(2);
            bool ok = type == 0 ? Stored() : type == 1 ? Fixed() : type == 2 ? Dynamic() : false;
            if (!ok || error)
                return false;
        } while (!last);
        return true;
    }
};

// --- Zip archive (the game's .crf files) -------------------------------------------------
std::string GameDir()
{
    char path[MAX_PATH];
    if (GetEnvironmentVariableA("T2VR_GAME_DIR", path, MAX_PATH))  // tests
        return std::string(path) + "\\";
    DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
    while (n > 0 && path[n - 1] != '\\' && path[n - 1] != '/')
        --n;
    path[n] = 0;
    return path;
}

bool IEquals(const std::string& a, const std::string& b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i]))
            return false;
    return true;
}

// Reads one entry (case-insensitive name, '/' separators) from a zip file,
// reading only its directory and the entry itself (the archives are large).
bool ReadZipEntry(const std::string& zip_path, const std::string& name, std::vector<unsigned char>& data)
{
    HANDLE f = CreateFileA(zip_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return false;
    auto read_at = [&](DWORD offset, DWORD size, std::vector<unsigned char>& buf) {
        buf.resize(size);
        DWORD got = 0;
        return SetFilePointer(f, (LONG)offset, nullptr, FILE_BEGIN) != INVALID_SET_FILE_POINTER &&
               ReadFile(f, buf.data(), size, &got, nullptr) && got == size;
    };
    auto u16 = [](const unsigned char* b) { return (unsigned)(b[0] | b[1] << 8); };
    auto u32 = [](const unsigned char* b) { return (unsigned)(b[0] | b[1] << 8 | b[2] << 16 | (unsigned)b[3] << 24); };
    bool ok = false;
    DWORD size = GetFileSize(f, nullptr);
    std::vector<unsigned char> tail, dir, local;
    DWORD tail_len = size < 65557 ? size : 65557;  // end of central directory + max comment
    if (size >= 22 && read_at(size - tail_len, tail_len, tail)) {
        size_t e = tail_len - 22;
        while (e > 0 && u32(&tail[e]) != 0x06054b50)
            --e;
        if (u32(&tail[e]) == 0x06054b50) {
            unsigned entries = u16(&tail[e + 10]), dir_size = u32(&tail[e + 12]), dir_off = u32(&tail[e + 16]);
            if (read_at(dir_off, dir_size, dir)) {
                size_t p = 0;
                for (unsigned i = 0; i < entries && p + 46 <= dir.size() && !ok; ++i) {
                    const unsigned char* c = &dir[p];
                    if (u32(c) != 0x02014b50)
                        break;
                    unsigned method = u16(c + 10), csize = u32(c + 20), usize = u32(c + 24);
                    unsigned nlen = u16(c + 28), xlen = u16(c + 30), clen = u16(c + 32), local_off = u32(c + 42);
                    std::string entry(reinterpret_cast<const char*>(c + 46), nlen);
                    p += 46 + nlen + xlen + clen;
                    if (!IEquals(entry, name))
                        continue;
                    std::vector<unsigned char> header;
                    if (!read_at(local_off, 30, header) || u32(header.data()) != 0x04034b50)
                        break;
                    DWORD start = local_off + 30 + u16(&header[26]) + u16(&header[28]);
                    if (!read_at(start, csize, local))
                        break;
                    if (method == 0) {
                        data = std::move(local);
                        ok = true;
                    } else if (method == 8) {
                        Inflater inf{local.data(), local.size()};
                        inf.out.reserve(usize);
                        if (inf.Run() && inf.out.size() == usize) {
                            data = std::move(inf.out);
                            ok = true;
                        }
                    }
                    break;
                }
            }
        }
    }
    CloseHandle(f);
    return ok;
}

// --- GIF textures (GDI+) ------------------------------------------------------------------
bool DecodeImage(const std::vector<unsigned char>& file, Texture& out)
{
    static ULONG_PTR token;
    if (!token) {
        Gdiplus::GdiplusStartupInput input;
        if (Gdiplus::GdiplusStartup(&token, &input, nullptr) != Gdiplus::Ok)
            return false;
    }
    IStream* stream = SHCreateMemStream(file.data(), (UINT)file.size());
    if (!stream)
        return false;
    bool ok = false;
    {
        Gdiplus::Bitmap bitmap(stream);
        if (bitmap.GetLastStatus() == Gdiplus::Ok) {
            Gdiplus::Rect rect(0, 0, bitmap.GetWidth(), bitmap.GetHeight());
            Gdiplus::BitmapData data;
            if (bitmap.LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &data) == Gdiplus::Ok) {
                out.w = data.Width;
                out.h = data.Height;
                out.pixels.resize(out.w * out.h);
                for (unsigned y = 0; y < out.h; ++y)
                    memcpy(&out.pixels[y * out.w], static_cast<unsigned char*>(data.Scan0) + y * data.Stride, out.w * 4);
                bitmap.UnlockBits(&data);
                ok = out.w && out.h;
            }
        }
    }
    stream->Release();
    return ok;
}

bool LoadTexture(const std::string& crf, const std::string& name, Texture& out)
{
    std::vector<unsigned char> file;
    for (const char* folder : {"txt16/", "txt/"})
        if (ReadZipEntry(crf, folder + name, file) && DecodeImage(file, out))
            return true;
    return false;
}

}  // namespace

bool DecodeImage(const void* data, size_t size, Texture& out)
{
    std::vector<unsigned char> file(static_cast<const unsigned char*>(data), static_cast<const unsigned char*>(data) + size);
    return DecodeImage(file, out);
}

bool Load(const char* name, Model& out)
{
    std::string crf = GameDir() + "RES\\obj.crf";
    std::vector<unsigned char> d;
    if (!ReadZipEntry(crf, name, d) || d.size() < 132 || memcmp(d.data(), "LGMD", 4) != 0) {
        Log("Models: couldn't read %s from RES\\obj.crf", name);
        return false;
    }
    auto u16 = [&](size_t o) { return (unsigned)(d[o] | d[o + 1] << 8); };
    auto u32 = [&](size_t o) { return (unsigned)(d[o] | d[o + 1] << 8 | d[o + 2] << 16 | (unsigned)d[o + 3] << 24); };
    auto f32 = [&](size_t o) {
        float v;
        memcpy(&v, &d[o], 4);
        return v;
    };
    unsigned version = u32(4);
    // Header (after magic + version): name[8], radius, max poly radius,
    // bbox max, bbox min, parent centre, then counts and offsets.
    out.name = name;
    out.bbox_max = {f32(24), f32(28), f32(32)};
    out.bbox_min = {f32(36), f32(40), f32(44)};
    unsigned num_pgons = u16(60), num_verts = u16(62), num_mats = d[66];
    // (u8 counts at 66..69, then the offsets from byte 70: sub-objects, materials,
    // UVs, vhots, vertices, lights, normals, polygons, nodes, size)
    unsigned mat_off = u32(74), uv_off = u32(78), vhot_off = u32(82), vert_off = u32(86), pgon_off = u32(98),
             node_off = u32(102);
    if (version != 3 && version != 4) {
        Log("Models: %s is LGMD version %u (not supported)", name, version);
        return false;
    }
    if (vert_off + num_verts * 12 > d.size() || node_off > d.size() || mat_off + num_mats * 26 > d.size()) {
        Log("Models: %s is malformed", name);
        return false;
    }
    // Materials: name[16], type, slot, ...; polygons refer to the slot.
    std::vector<int> slot_to_material(256, -1);
    out.textures.resize(num_mats);
    for (unsigned m = 0; m < num_mats; ++m) {
        size_t o = mat_off + m * 26;
        std::string mname(reinterpret_cast<const char*>(&d[o]), strnlen(reinterpret_cast<const char*>(&d[o]), 16));
        slot_to_material[d[o + 17]] = (int)m;
        if (!LoadTexture(crf, mname, out.textures[m])) {
            Log("Models: texture %s for %s not found", mname.c_str(), name);
            out.textures[m].w = out.textures[m].h = 1;
            out.textures[m].pixels.assign(1, 0xff808080);
        }
    }
    unsigned num_uvs = (vhot_off - uv_off) / 8;
    auto vert = [&](unsigned i) { return Vec3{f32(vert_off + i * 12), f32(vert_off + i * 12 + 4), f32(vert_off + i * 12 + 8)}; };
    size_t p = pgon_off;
    for (unsigned i = 0; i < num_pgons; ++i) {
        if (p + 12 > node_off)
            break;
        unsigned data = u16(p + 2), type = d[p + 4], n = d[p + 5];
        p += 12;
        size_t verts = p, uvs = 0;
        p += 4 * n;  // vertex and light indices
        if ((type & 3) == 3) {
            uvs = p;
            p += 2 * n;
        }
        if (version == 4)
            p += 1;
        if (p > node_off || n < 3)
            break;
        int material = data < 256 ? slot_to_material[data] : -1;
        for (unsigned k = 1; k + 1 < n; ++k) {  // fan
            Triangle t;
            t.material = material < 0 ? 0 : material;
            unsigned corner[3] = {0, k, k + 1};
            bool valid = true;
            for (int c = 0; c < 3; ++c) {
                unsigned vi = u16(verts + corner[c] * 2);
                valid &= vi < num_verts;
                t.p[c] = vi < num_verts ? vert(vi) : Vec3{};
                t.uv[c][0] = t.uv[c][1] = 0;
                if (uvs) {
                    unsigned ui = u16(uvs + corner[c] * 2);
                    if (ui < num_uvs) {
                        t.uv[c][0] = f32(uv_off + ui * 8);
                        t.uv[c][1] = f32(uv_off + ui * 8 + 4);
                    }
                }
            }
            if (valid)
                out.triangles.push_back(t);
        }
    }
    Log("Models: %s loaded (%u polygons -> %d triangles, %u textures)", name, num_pgons, (int)out.triangles.size(),
        num_mats);
    return !out.triangles.empty();
}

bool LoadMeshUvs(const char* name, std::vector<Uv>& out)
{
    std::string crf = GameDir() + "RES\\mesh.crf";
    std::vector<unsigned char> d;
    if (!ReadZipEntry(crf, name, d) || d.size() < 68 || memcmp(d.data(), "LGMM", 4) != 0) {
        Log("Models: couldn't read %s from RES\\mesh.crf", name);
        return false;
    }
    // Header: vertex count at 26, the vertices' {u, v, packed normal} at the offset at 60.
    unsigned verts = d[26] | d[27] << 8;
    unsigned uvn = d[60] | d[61] << 8 | d[62] << 16 | (unsigned)d[63] << 24;
    if (uvn + (size_t)verts * 12 > d.size())
        return false;
    out.resize(verts);
    for (unsigned i = 0; i < verts; ++i) {
        memcpy(&out[i].u, &d[uvn + i * 12], 4);
        memcpy(&out[i].v, &d[uvn + i * 12 + 4], 4);
    }
    return verts > 0;
}

}  // namespace game_models

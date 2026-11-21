#include <windows.h>
#include <windowsx.h>
#include <gdiplus.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <mmsystem.h>
#include <intrin.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")

template <typename C, size_t N>
struct Hidden {
    C v[N];
    constexpr Hidden(const C (&s)[N]) : v{} {
        for (size_t i = 0; i < N; i++) v[i] = static_cast<C>(s[i] ^ static_cast<C>(0x3B + i * 17));
    }
    std::basic_string<C> get() const {
        std::basic_string<C> r;
        for (size_t i = 0; i + 1 < N; i++) r.push_back(static_cast<C>(v[i] ^ static_cast<C>(0x3B + i * 17)));
        return r;
    }
};
#define HID(s) ([]() -> std::string { constexpr Hidden<char, sizeof(s)> h(s); return h.get(); }())
#define HIDW(s) ([]() -> std::wstring { constexpr Hidden<wchar_t, sizeof(s) / sizeof(wchar_t)> h(s); return h.get(); }())

static const wchar_t* AppName() { static std::wstring a = HIDW(L"DeltaHUD"); return a.c_str(); }
static const wchar_t* APP = AppName();
static std::wstring g_dir;

static void Fail(const std::wstring& why) {
    MessageBoxW(NULL, why.c_str(), APP, MB_ICONERROR | MB_TOPMOST);
    ExitProcess(1);
}
#define WIDE2(x) L##x
#define WIDE(x) WIDE2(x)
#define CHECK(c) if (!(c)) Fail(L"Self-check failed: " WIDE(#c))

static std::string ReadText(const std::wstring& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

static void WriteText(const std::wstring& path, const std::string& text) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f << text;
}

static std::vector<long long> Numbers(const std::string& text) {
    std::vector<long long> out;
    size_t i = 0;
    while (i < text.size()) {
        bool digit = isdigit((unsigned char)text[i]) != 0;
        bool minus = text[i] == '-' && i + 1 < text.size() && isdigit((unsigned char)text[i + 1]);
        if (!digit && !minus) { i++; continue; }
        size_t end = i + 1;
        while (end < text.size() && isdigit((unsigned char)text[end])) end++;
        if (end - i <= 18) out.push_back(std::stoll(text.substr(i, end - i)));
        i = end;
    }
    return out;
}

static bool g_log;

namespace store {
typedef std::vector<std::pair<std::string, std::string>> Lines;
static Lines lines;
static std::wstring key = HIDW(L"Software\\DeltaHUD");

static void Parse(const std::string& text) {
    lines.clear();
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        size_t eq = line.find('=');
        if (eq != std::string::npos && eq > 0) lines.push_back({line.substr(0, eq), line.substr(eq + 1)});
    }
}

static std::string Get(const std::string& name) {
    for (const auto& l : lines)
        if (l.first == name) return l.second;
    return "";
}

static void Put(const std::string& name, const std::string& value) {
    for (size_t i = 0; i < lines.size(); i++)
        if (lines[i].first == name) {
            if (value.empty()) lines.erase(lines.begin() + i);
            else lines[i].second = value;
            return;
        }
    if (!value.empty()) lines.push_back({name, value});
}

static std::wstring Wide(const std::string& text) {
    std::wstring wide;
    for (unsigned char c : text) wide += wchar_t(c);
    return wide;
}
static std::string Narrow(const std::wstring& wide) {
    std::string text;
    for (wchar_t c : wide) text += char(c);
    return text;
}

static Lines Stored() {
    Lines out;
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, KEY_READ, &k) != ERROR_SUCCESS) return out;
    DWORD count = 0, longestName = 0, longestValue = 0;
    RegQueryInfoKeyW(k, NULL, NULL, NULL, NULL, NULL, NULL, &count, &longestName, &longestValue, NULL, NULL);
    std::vector<wchar_t> name(longestName + 2);
    std::vector<wchar_t> value(longestValue / sizeof(wchar_t) + 2);
    for (DWORD i = 0; i < count; i++) {
        DWORD nameLength = DWORD(name.size()), bytes = DWORD(value.size() * sizeof(wchar_t)), type = 0;
        if (RegEnumValueW(k, i, name.data(), &nameLength, NULL, &type, (BYTE*)value.data(), &bytes) != ERROR_SUCCESS || type != REG_SZ) continue;
        std::wstring text(value.data(), bytes / sizeof(wchar_t));
        while (!text.empty() && text.back() == 0) text.pop_back();
        if (nameLength && !text.empty()) out.push_back({Narrow(std::wstring(name.data(), nameLength)), Narrow(text)});
    }
    RegCloseKey(k);
    return out;
}

static bool Save() {
    Lines stored = Stored();
    auto was = [&](const std::string& name) {
        for (const auto& s : stored)
            if (s.first == name) return s.second;
        return std::string();
    };
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, NULL, 0, KEY_READ | KEY_WRITE, NULL, &k, NULL) != ERROR_SUCCESS) return false;
    bool ok = true;
    for (const auto& l : lines) {
        if (was(l.first) == l.second) continue;
        std::wstring value = Wide(l.second);
        ok = RegSetValueExW(k, Wide(l.first).c_str(), 0, REG_SZ, (const BYTE*)value.c_str(), DWORD((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS && ok;
    }
    for (const auto& s : stored)
        if (Get(s.first).empty()) ok = RegDeleteValueW(k, Wide(s.first).c_str()) == ERROR_SUCCESS && ok;
    RegCloseKey(k);
    return ok;
}

static void Load() {
    lines = Stored();
    if (!lines.empty()) return;
    std::vector<std::wstring> old;
    auto take = [&](const std::wstring& file, const std::string& name) {
        std::string text = ReadText(g_dir + file);
        for (char& c : text)
            if (c == '\n' || c == '\r') c = ' ';
        while (!text.empty() && text.back() == ' ') text.pop_back();
        if (text.empty()) return;
        Put(name, text);
        old.push_back(g_dir + file);
    };
    Parse(ReadText(g_dir + L"DeltaHUD.txt"));
    if (!lines.empty()) old.push_back(g_dir + L"DeltaHUD.txt");
    else {
        Parse(ReadText(g_dir + L"settings.txt"));
        Lines settings;
        lines.swap(settings);
        for (const auto& s : settings)
            if (s.first == "size" || s.first == "opacity" || s.first == "summary" || s.first == "always") Put(s.first, s.second);
        if (!settings.empty()) old.push_back(g_dir + L"settings.txt");
        take(L"scale.txt", "scale");
        take(L"region.txt", "region");
        take(L"overlay.txt", "overlay");
        WIN32_FIND_DATAW found;
        HANDLE search = FindFirstFileW((g_dir + L"best_*.txt").c_str(), &found);
        if (search != INVALID_HANDLE_VALUE) {
            do {
                std::wstring file = found.cFileName;
                take(file, HID("best ") + Narrow(file.substr(5, file.size() - 9)));
            } while (FindNextFileW(search, &found));
            FindClose(search);
        }
    }
    if (lines.empty() || !Save()) return;
    Lines kept = Stored(), want = lines;
    std::sort(kept.begin(), kept.end());
    std::sort(want.begin(), want.end());
    if (kept == want)
        for (const std::wstring& file : old) DeleteFileW(file.c_str());
}
}

static double Now() {
    static LARGE_INTEGER freq, start;
    LARGE_INTEGER t;
    if (!freq.QuadPart) { QueryPerformanceFrequency(&freq); QueryPerformanceCounter(&start); }
    QueryPerformanceCounter(&t);
    return double(t.QuadPart - start.QuadPart) / double(freq.QuadPart);
}

namespace hud {
const int W = 24, H = 20;
std::string names;
std::vector<std::string> shapes;

typedef std::array<uint64_t, 8> Bits;
std::vector<Bits> packed;

static Bits Pack(const std::string& grid) {
    Bits b = {};
    for (int i = 0; i < W * H; i++)
        if (grid[i] == '1') b[i >> 6] |= 1ull << (i & 63);
    return b;
}

static inline int Differ(const Bits& a, const Bits& b) {
    int n = 0;
    for (int i = 0; i < 8; i++) n += int(__popcnt64(a[i] ^ b[i]));
    return n;
}

static bool Load() {
    HRSRC found = FindResourceW(NULL, MAKEINTRESOURCEW(2), RT_RCDATA);
    HGLOBAL data = found ? LoadResource(NULL, found) : NULL;
    const char* bytes = data ? (const char*)LockResource(data) : NULL;
    if (!bytes) return false;
    DWORD count = SizeofResource(NULL, found);
    std::string raw(bytes, count);
    for (DWORD i = 0; i < count; i++) raw[i] = char((unsigned char)raw[i] ^ (unsigned char)(0x7C + i * 31));
    std::istringstream in(raw);
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.size() != size_t(2 + W * H)) continue;
        names += line[0];
        shapes.push_back(line.substr(2));
        packed.push_back(Pack(shapes.back()));
    }
    return shapes.size() >= 10;
}

static inline int Brightest(uint32_t p) {
    int r = (p >> 16) & 255, g = (p >> 8) & 255, b = p & 255;
    return r > g ? (r > b ? r : b) : (g > b ? g : b);
}

static inline bool BarBright(uint32_t p) { return ((p >> 16) & 255) >= 200 && ((p >> 8) & 255) >= 170; }

static bool FindBar(const uint32_t* px, int w, int h, RECT& bar, int* width, std::vector<int>& tried) {
    struct Run { int x, top, bottom; };
    std::vector<Run> runs;
    std::vector<int> votes(h + 5, 0);
    for (int x = 0; x < w; x++) {
        int top = 0, len = 0;
        for (int y = 0; y <= h; y++) {
            if (y < h && BarBright(px[y * w + x])) { if (!len) top = y; len++; continue; }
            bool boxed = top > 0 && y < h;
            bool whole = len == h && h <= 120;
            if (len >= 24 && len <= 160 && (boxed || whole)) runs.push_back({x, top, y - 1});
            len = 0;
        }
    }
    auto busiest = [&](bool bottoms, int topRow) {
        std::fill(votes.begin(), votes.end(), 0);
        for (const Run& r : runs)
            if (topRow < 0 || abs(r.top - topRow) <= 2) votes[bottoms ? r.bottom : r.top]++;
        auto around = [&](int y) { int sum = 0; for (int d = -2; d <= 2; d++) if (y + d >= 0 && y + d < h) sum += votes[y + d]; return sum; };
        int best = -1, bestSum = 0;
        for (int y = 0; y < h; y++) {
            bool blocked = false;
            if (topRow < 0) for (int t : tried) blocked = blocked || abs(y - t) <= 6;
            if (blocked) continue;
            int sum = around(y);
            if (sum > bestSum) { bestSum = sum; best = y; }
        }
        if (bottoms && best >= 0)
            for (int y = h - 1; y > best; y--)
                if (around(y) >= (std::max)(24, bestSum / 4)) {
                    best = y;
                    for (int d = 1; d <= 4 && y - d >= 0; d++) if (votes[y - d] > votes[best]) best = y - d;
                    break;
                }
        return best;
    };
    int top = busiest(false, -1);
    if (top < 0) return false;
    tried.push_back(top);
    int bottom = busiest(true, top), tall = bottom - top + 1;
    if (tall < 24) return false;
    int first = -1, last = -1, count = 0, from = -1, prev = -2;
    for (const Run& r : runs) {
        if (abs(r.top - top) > 2 || abs(r.bottom - bottom) > 2) continue;
        if (r.x != prev + 1) from = r.x;
        prev = r.x;
        count++;
        if (r.x - from + 1 >= 8) { if (first < 0) first = from; last = r.x; }
    }
    if (count < 24 || first < 0) return false;
    if (width) *width = last - first + 1;
    int inset = (std::max)(3, tall / 14);
    bar.left = first + inset; bar.top = top + inset; bar.right = last + 1 - inset; bar.bottom = bottom + 1 - inset;
    return bar.right - bar.left >= 20;
}

struct Glyph {
    int x, right, height;
    std::string grid;
};

static Glyph Cut(const std::vector<char>& mask, int gw, int gh, int x0, int c0, int c1) {
    int left = gw, right = -1, top = gh, bottom = -1;
    for (int y = 0; y < gh; y++)
        for (int x = c0; x < c1; x++)
            if (mask[y * gw + x]) {
                left = (std::min)(left, x); right = (std::max)(right, x);
                top = (std::min)(top, y); bottom = (std::max)(bottom, y);
            }
    if (right < 0) return {x0 + c0, x0 + c0, 0, ""};
    int tw = right - left + 1, th = bottom - top + 1;
    double scale = H / double(th);
    Glyph g = {x0 + left, x0 + right, th, ""};
    if (tw * scale > W) return g;
    g.grid.assign(W * H, '0');
    for (int cy = 0; cy < H; cy++)
        for (int cx = 0; cx < W; cx++) {
            int sx = int((cx + 0.5) / scale), sy = int((cy + 0.5) / scale);
            if (sx < tw && mask[(top + sy) * gw + left + sx]) g.grid[cy * W + cx] = '1';
        }
    return g;
}

static int Misfit(const std::string& grid) {
    Bits bits = Pack(grid);
    int best = INT_MAX;
    for (const Bits& shape : packed) best = (std::min)(best, Differ(bits, shape));
    return best;
}

static int Split(const std::vector<char>& mask, int gw, int gh, int x0, int c0, int parts, std::vector<Glyph>& out) {
    Glyph rest = Cut(mask, gw, gh, x0, c0, gw);
    if (rest.height == 0) return INT_MAX;
    if (!rest.grid.empty()) { out.assign(1, rest); return Misfit(rest.grid); }
    if (parts < 2) return INT_MAX;
    int left = rest.x - x0, best = INT_MAX;
    std::vector<Glyph> tail;
    for (int cut = left + (std::max)(3, rest.height * 35 / 100); cut <= left + rest.height * 125 / 100 && cut < gw; cut++) {
        Glyph piece = Cut(mask, gw, gh, x0, c0, cut);
        if (piece.grid.empty()) continue;
        int fit = Misfit(piece.grid);
        if (fit > 120 || fit >= best) continue;
        int more = Split(mask, gw, gh, x0, cut, parts - 1, tail);
        if (more == INT_MAX || fit + more >= best) continue;
        best = fit + more;
        out.assign(1, piece);
        out.insert(out.end(), tail.begin(), tail.end());
    }
    return best;
}

static void Emit(const std::vector<char>& mask, int gw, int gh, int x0, std::vector<Glyph>& out) {
    std::vector<Glyph> parts;
    if (Split(mask, gw, gh, x0, 0, 4, parts) == INT_MAX) parts.assign(1, Cut(mask, gw, gh, x0, 0, gw));
    out.insert(out.end(), parts.begin(), parts.end());
}

static std::vector<Glyph> Segment(const uint32_t* px, int w, const RECT& bar, bool light) {
    int bw = bar.right - bar.left, bh = bar.bottom - bar.top;
    std::vector<char> ink(size_t(bw) * bh);
    for (int y = 0; y < bh; y++)
        for (int x = 0; x < bw; x++) {
            uint32_t p = px[(bar.top + y) * w + bar.left + x];
            ink[y * bw + x] = light ? (p & 255) >= 190 && ((p >> 8) & 255) >= 190 && ((p >> 16) & 255) >= 190 : Brightest(p) < 60;
        }
    std::vector<Glyph> glyphs;
    std::vector<int> stack, cells;
    for (int start = 0; start < bw * bh; start++) {
        if (!ink[start]) continue;
        cells.clear();
        int minX = bw, maxX = 0, minY = bh, maxY = 0;
        stack.push_back(start);
        ink[start] = 0;
        while (!stack.empty()) {
            int p = stack.back();
            stack.pop_back();
            cells.push_back(p);
            int x = p % bw, y = p / bw;
            minX = (std::min)(minX, x); maxX = (std::max)(maxX, x);
            minY = (std::min)(minY, y); maxY = (std::max)(maxY, y);
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    int nx = x + dx, ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= bw || ny >= bh) continue;
                    int q = ny * bw + nx;
                    if (ink[q]) { ink[q] = 0; stack.push_back(q); }
                }
        }
        if (cells.size() < 8) continue;
        if (minX == 0 || minY == 0 || maxX == bw - 1 || maxY == bh - 1) continue;
        int gw = maxX - minX + 1, gh = maxY - minY + 1;
        std::vector<char> mask(size_t(gw) * gh);
        for (int p : cells) mask[(p / bw - minY) * gw + (p % bw - minX)] = 1;
        Emit(mask, gw, gh, minX, glyphs);
    }
    std::sort(glyphs.begin(), glyphs.end(), [](const Glyph& a, const Glyph& b) { return a.x < b.x; });
    return glyphs;
}

static bool ValidScore(std::string text, std::string& digits) {
    digits.clear();
    if (text.size() < 4 || text.compare(text.size() - 3, 3, HID("PTS")) != 0) return false;
    text.erase(text.size() - 3);
    size_t group = 0, groups = 0;
    for (size_t i = 0; i <= text.size(); i++) {
        if (i == text.size() || text[i] == ',') {
            if (group == 0 || group > 3 || (groups > 0 && group != 3)) return false;
            groups++;
            group = 0;
        } else if (isdigit((unsigned char)text[i])) {
            digits += text[i];
            group++;
        } else return false;
    }
    return digits.size() <= 15;
}

static std::string ReadBar(const uint32_t* px, int w, const RECT& bar, bool light, bool* sawInk, bool* sure, std::string* trace,
                           std::vector<std::pair<char, std::string>>* learned = nullptr) {
    std::vector<Glyph> glyphs = Segment(px, w, bar, light);
    *sawInk = !glyphs.empty();
    *sure = true;
    int tall = 0;
    for (const Glyph& g : glyphs) tall = (std::max)(tall, g.height);
    std::string text;
    std::vector<int> misses(shapes.size());
    for (const Glyph& g : glyphs) {
        if (g.grid.empty()) { if (trace) *trace += "wide "; *sure = false; continue; }
        if (g.height < tall * 0.6) { text += ','; if (trace) *trace += ", "; continue; }
        size_t best = 0;
        Bits bits = Pack(g.grid);
        for (size_t t = 0; t < shapes.size(); t++) {
            misses[t] = Differ(bits, packed[t]);
            if (misses[t] < misses[best]) best = t;
        }
        int rival = INT_MAX;
        for (size_t t = 0; t < shapes.size(); t++)
            if (names[t] != names[best]) rival = (std::min)(rival, misses[t]);
        if (trace) *trace += std::string(1, names[best]) + ":" + std::to_string(misses[best]) + "/" + std::to_string(rival - misses[best]) + " ";
        if (learned) learned->push_back({names[best], g.grid});
        int miss = misses[best], lead = rival - miss;
        if (!((miss <= 72 && lead >= 12) || (miss <= 100 && lead >= 40))) *sure = false;
        text += names[best];
    }
    std::string digits;
    if (!ValidScore(text, digits)) return "";
    int barWide = bar.right - bar.left;
    if (abs(glyphs.front().x + glyphs.back().right - barWide) > barWide / 10 + 8) return "";
    return digits;
}

static std::string Read(const uint32_t* px, int w, int h, bool* unreadable = nullptr, int* barWidth = nullptr, std::string* trace = nullptr,
                        RECT* where = nullptr) {
    if (unreadable) *unreadable = false;
    std::vector<int> tried;
    for (int attempt = 0; attempt < 3; attempt++) {
        RECT bar;
        int width = 0;
        size_t before = tried.size();
        if (!FindBar(px, w, h, bar, &width, tried)) {
            if (tried.size() == before) break;
            continue;
        }
        bool sawInk = false, sure = false;
        std::string digits = ReadBar(px, w, bar, false, &sawInk, &sure, attempt == 0 ? trace : nullptr);
        if (digits.empty() || !sure) {
            bool sawLight = false, sureLight = false;
            if (attempt == 0 && trace) *trace += "/ ";
            std::string white = ReadBar(px, w, bar, true, &sawLight, &sureLight, attempt == 0 ? trace : nullptr);
            if (!white.empty() && sureLight) { digits = white; sure = true; }
            sawInk = sawInk || sawLight;
        }
        if (attempt == 0 && barWidth) *barWidth = width;
        if (!digits.empty() && sure) {
            if (barWidth) *barWidth = width;
            if (unreadable) *unreadable = false;
            if (where) *where = bar;
            return digits;
        }
        if (attempt == 0 && unreadable) *unreadable = sawInk;
    }
    return "";
}

static bool Locate(const uint32_t* px, int w, int h, RECT& bar, std::string* note = nullptr) {
    struct Run { int x, top, len; };
    std::vector<Run> runs;
    std::vector<int> len(w, 0);
    std::vector<unsigned char> start(size_t(w) * h, 0);
    for (int y = 0; y <= h; y++)
        for (int x = 0; x < w; x++) {
            if (y < h && BarBright(px[size_t(y) * w + x])) { len[x]++; continue; }
            int n = len[x];
            len[x] = 0;
            if (n < 24 || n > 160 || n == y || y == h) continue;
            runs.push_back({x, y - n, n});
            start[size_t(y - n) * w + x] = (unsigned char)n;
        }
    auto beside = [&](int x, int top, int n) {
        if (x < 0 || x >= w) return -1;
        for (int t = (std::max)(0, top - 1); t <= top + 1 && t < h; t++) {
            int m = start[size_t(t) * w + x];
            if (m && abs(t + m - top - n) <= 1) return t;
        }
        return -1;
    };
    int stretches = 0, reads = 0;
    std::vector<uint32_t> piece;
    for (const Run& r : runs) {
        if (beside(r.x - 1, r.top, r.len) >= 0) continue;
        int x = r.x, top = r.top, tall = r.len, high = r.len, highTop = r.top, t;
        while ((t = beside(x + 1, top, tall)) >= 0) {
            x++;
            top = t;
            tall = start[size_t(t) * w + x];
            if (tall > high) { high = tall; highTop = top; }
        }
        if (x - r.x + 1 < 8) continue;
        stretches++;
        auto agrees = [&](int column) {
            for (int row = (std::max)(0, highTop - 2); row <= highTop + 2 && row < h; row++) {
                int m = start[size_t(row) * w + column];
                if (m && abs(row + m - highTop - high) <= 2) return true;
            }
            return false;
        };
        int first = r.x, last = x;
        for (int column = x + 1; column < w && column <= r.x + 10 * high; column++) if (agrees(column)) last = column;
        for (int column = r.x - 1; column >= 0 && column >= x - 10 * high; column--) if (agrees(column)) first = column;
        for (int side = 0; side < 2 && reads < 400; side++) {
            int from = side ? first : r.x, to = side ? x : last;
            if (side && first == r.x && last == x) continue;
            if ((to - from + 1) * 2 < high * 11 || (to - from + 1) * 2 > high * 19) continue;
            int left = (std::max)(0, from - 2 * high), right = (std::min)(w, to + 1 + 2 * high);
            int up = (std::max)(0, highTop - high), down = (std::min)(h, highTop + 2 * high);
            int pw = right - left, ph = down - up;
            piece.resize(size_t(pw) * ph);
            for (int y = 0; y < ph; y++) memcpy(&piece[size_t(y) * pw], px + size_t(up + y) * w + left, size_t(pw) * 4);
            int width = 0;
            RECT at;
            reads++;
            std::string digits = Read(piece.data(), pw, ph, nullptr, &width, nullptr, &at);
            if (digits.empty() || width * 2 < high * 11 || width * 2 > high * 19) continue;
            bar.left = at.left + left; bar.top = at.top + up; bar.right = at.right + left; bar.bottom = at.bottom + up;
            if (note) *note = digits + " after " + std::to_string(stretches) + " stretches, " + std::to_string(reads) + " pieces read";
            return true;
        }
    }
    if (note) *note = "nothing in " + std::to_string(stretches) + " stretches, " + std::to_string(reads) + " pieces read";
    return false;
}
}

struct Tracker {
    static constexpr long long FreshMax = 1000000;
    long long score = 0;
    int rate = 10;
    int bestRate = 1;
    std::vector<long long> best;
    std::string bestKey;
    std::vector<long long> run;
    bool running = false;
    bool ready = true;
    double newBestAt = -1e9;
    double endedAt = -1e9;
    bool paceKnown = false;
    double pace = 0;
    double runStart = 0, lastRead = 0;
    double freshSince = -1, freshLast = -1e9;
    long long freshLow = 0, freshPrev = 0;
    double freshLowAt = -1, freshRise = -1;
    bool freshFalling = false;
    double belowSince = -1, belowLast = -1e9;
    long long belowPrev = 0;
    int belowFalls = 0;
    bool belowFell = false;
    std::vector<std::pair<double, long long>> recent;
    long long base = 0;
    bool rising = false;
    double startHint = -1;
    static constexpr long long SureAt = 1000;
    static constexpr double KeepUp = 0.75, GiveUp = 3;
    double trialAt = -1;
    double trialOpen = 0;
    double trialRise = 0;
    long long trialScore = 0;

    bool Idle() const { return !running && ready && (score < FreshMax || trialAt >= 0); }

    long long Add(long long v, double now) {
        while (!recent.empty() && now - recent.front().first > 0.15) recent.erase(recent.begin());
        recent.push_back({now, v});
        if (v < score && v < FreshMax) {
            if (freshSince < 0 || now - freshLast > 0.5) {
                freshSince = freshLowAt = now;
                freshLow = freshPrev = v;
                freshRise = -1;
                freshFalling = false;
            }
            freshLast = now;
            if (v != freshPrev) freshFalling = v < freshPrev;
            freshPrev = v;
            if (v < freshLow) { freshLow = v; freshLowAt = now; freshRise = -1; }
            else if (v > freshLow && freshRise < 0) freshRise = now;
            if (now - freshSince < 0.3) return -1;
            long long last = score;
            score = 0;
            base = freshFalling ? v : 0;
            startHint = freshFalling ? -1 : freshLow > 0 ? freshLowAt : freshRise;
            freshSince = belowSince = -1;
            recent.assign(1, {now, v});
            return last;
        }
        freshSince = -1;
        long long low = v;
        for (const auto& r : recent) low = (std::min)(low, r.second);
        bool settled = now - recent.front().first >= 0.1;
        if (v < score) {
            if (belowSince < 0 || now - belowLast > 0.5) { belowSince = now; belowPrev = v; belowFalls = 0; belowFell = false; }
            belowLast = now;
            if (v < belowPrev) belowFalls++;
            else if (v > belowPrev) belowFalls = 0;
            belowPrev = v;
            if (belowFalls >= 2) belowFell = true;
            if (belowFell && v < score / 10) {
                long long last = score;
                score = 0;
                base = v;
                startHint = -1;
                freshSince = belowSince = -1;
                recent.assign(1, {now, v});
                return last;
            }
            if (settled && now - belowSince >= (belowFell ? 0.5 : 3)) {
                score = low;
                if (!belowFell) for (long long& s : run) s = (std::min)(s, low);
                belowSince = -1;
            }
            return -1;
        }
        belowSince = -1;
        if (settled && low > score) score = low;
        return -1;
    }

    void Step(const std::string& digits, double now) {
        rising = false;
        if (!digits.empty()) {
            long long v = std::stoll(digits);
            if (!running && now - lastRead > 3 && v < FreshMax) { score = 0; recent.clear(); ready = true; }
            if (!running && now - lastRead > 1) { base = 0; startHint = -1; }
            lastRead = now;
            long long last = Add(v, now);
            if (last >= 0) {
                if (running) Stop(last, now);
                ready = true;
            }
            if (!running && v < base) base = v;
            rising = v >= score;
        }
        if (running && now - lastRead > 5) {
            run.resize((std::min)(run.size(), size_t((lastRead - runStart) * rate) + 1));
            Stop(score, now);
        }
        if (!running && ready && trialAt < 0 && rising && score > base && score < FreshMax) {
            trialAt = startHint >= 0 && now - startHint < 4 ? startHint : recent.empty() ? now : recent.front().first;
            trialOpen = trialRise = now;
            trialScore = score;
            startHint = -1;
            run.clear();
        }
        if (trialAt >= 0) {
            bool rose = score > trialScore;
            if (score < trialScore) trialAt = -1;
            else if (score >= SureAt || (rose && now - trialOpen >= KeepUp)) {
                running = true;
                runStart = trialAt;
                trialAt = -1;
                ready = false;
            } else if (rose) {
                trialScore = score;
                trialRise = now;
            } else if (now - trialRise > GiveUp) {
                trialAt = -1;
                base = score;
            }
        }
        if (running || trialAt >= 0)
            while (double(run.size()) <= (now - (running ? runStart : trialAt)) * rate) run.push_back(score);
    }

    static bool Pace(const std::vector<long long>& run, int rate, const std::vector<long long>& best, int bestRate, double& pace) {
        double sum = 0;
        int records = 0;
        for (size_t i = rate; i < run.size() && !best.empty(); i++) {
            double b = At(best, double(i) * bestRate / rate);
            if (run[i] <= 0 || b <= 0) continue;
            sum += log(double(run[i]) / b);
            records++;
        }
        if (!records) return false;
        pace = exp(sum / records) - 1;
        return true;
    }

    static double At(const std::vector<long long>& record, double position) {
        size_t i = size_t(position);
        if (i + 1 >= record.size()) return double(record.back());
        return record[i] + (record[i + 1] - record[i]) * (position - i);
    }

    static std::string BestText(const std::vector<long long>& best, int rate) {
        std::string text = HID("rate=") + std::to_string(rate) + " ";
        for (size_t i = 0; i < best.size(); i++) text += (i ? "," : "") + std::to_string(best[i]);
        return text;
    }
    static void ParseBest(const std::string& text, std::vector<long long>& best, int& rate) {
        rate = 1;
        size_t from = 0;
        if (text.compare(0, 5, HID("rate=")) == 0) {
            rate = (std::max)(1, (std::min)(100, atoi(text.c_str() + 5)));
            from = text.find_first_of(" \n");
            if (from == std::string::npos) from = text.size();
        }
        best = Numbers(text.substr(from));
    }

    void Stop(long long last, double now) {
        paceKnown = Pace(run, rate, best, bestRate, pace);
        endedAt = now;
        run.push_back(last);
        if (last > (best.empty() ? 0 : best.back())) {
            best = run;
            bestRate = rate;
            newBestAt = now;
            if (!bestKey.empty()) {
                store::Put(bestKey, BestText(best, bestRate));
                store::Save();
            }
        }
        running = false;
    }

    double Target(double now) const {
        if (!running || best.empty()) return 0;
        return At(best, (std::max)(0.0, now - runStart) * bestRate);
    }
};

static std::wstring Grouped(double n) {
    std::wstring s = std::to_wstring(llround(fabs(n)));
    for (int i = int(s.size()) - 3; i > 0; i -= 3) s.insert(i, L",");
    return s;
}

static std::wstring PaceWords(double pace) {
    long long percent = llround(pace * 100);
    if (!percent) return HIDW(L"AVG LEVEL WITH BEST PACE");
    return HIDW(L"AVG ") + Grouped(double(percent)) + (percent > 0 ? HIDW(L"% ABOVE BEST PACE") : HIDW(L"% BELOW BEST PACE"));
}

struct Settings {
    int size = 100;
    int opacity = 100;
    int summary = 10;
    bool always = false;
};
static Settings g_set;

static Settings ParseSettings(const std::string& text) {
    Settings s;
    auto number = [&](const char* name, int low, int high, int fallback) {
        size_t at = text.find(std::string(name) + "=");
        if (at == std::string::npos || (at > 0 && text[at - 1] != '\n')) return fallback;
        const char* p = text.c_str() + at + strlen(name) + 1;
        if (*p < '0' || *p > '9') return fallback;
        return (std::max)(low, (std::min)(high, atoi(p)));
    };
    s.size = number("size", 50, 200, s.size);
    s.opacity = number("opacity", 30, 100, s.opacity);
    s.summary = number("summary", 0, 30, s.summary);
    s.always = number("always", 0, 1, s.always) != 0;
    return s;
}

static std::string SettingsText(const Settings& s) {
    return "size=" + std::to_string(s.size) + "\nopacity=" + std::to_string(s.opacity) + "\nsummary=" + std::to_string(s.summary) +
           "\nalways=" + std::to_string(int(s.always)) + "\n";
}

static Settings LoadSettings() {
    std::string text;
    for (const char* name : {"size", "opacity", "summary", "always"}) text += std::string(name) + "=" + store::Get(name) + "\n";
    return ParseSettings(text);
}

static void SaveSettingsLines(const Settings& s) {
    std::istringstream in(SettingsText(s));
    std::string line;
    while (std::getline(in, line)) store::Put(line.substr(0, line.find('=')), line.substr(line.find('=') + 1));
}

static void SaveSettings(const Settings& s) {
    SaveSettingsLines(s);
    store::Save();
}

static double Bezier(double x1, double y1, double x2, double y2, double p) {
    if (p <= 0) return 0;
    if (p >= 1) return 1;
    double t = p;
    for (int i = 0; i < 8; i++) {
        double x = 3 * (1 - t) * (1 - t) * t * x1 + 3 * (1 - t) * t * t * x2 + t * t * t - p;
        double dx = 3 * (1 - t) * (1 - t) * x1 + 6 * (1 - t) * t * (x2 - x1) + 3 * t * t * (1 - x2);
        if (fabs(dx) < 1e-6) break;
        t = (std::min)(1.0, (std::max)(0.0, t - x / dx));
    }
    return 3 * (1 - t) * (1 - t) * t * y1 + 3 * (1 - t) * t * t * y2 + t * t * t;
}
static double EaseOut(double p) { return Bezier(0.23, 1, 0.32, 1, p); }
static double EaseInOut(double p) { return Bezier(0.77, 0, 0.175, 1, p); }

static void SelfCheck() {
    auto feed = [](Tracker& t, long long score, double from, double to) {
        for (double at = from; at < to - 1e-9; at += 0.01) t.Step(std::to_string(score), at);
    };

    Tracker a;
    feed(a, 10, 0, 0.3);
    a.Step("500", 0.3);
    feed(a, 20, 0.31, 0.6);
    a.Step("3", 0.6);
    feed(a, 30, 0.61, 0.9);
    CHECK(!a.running && a.Idle() && a.score == 30);
    feed(a, 40, 0.9, 1.7);
    feed(a, 50, 1.7, 2.0);
    CHECK(a.running && a.score == 50 && a.best.empty() && fabs(a.runStart) < 0.02);

    Tracker g;
    g.best = {100, 200};
    feed(g, 18, 0, 5.0);
    CHECK(!g.running && g.Idle());
    feed(g, 36, 5.0, 9.0);
    CHECK(!g.running && g.Idle());
    CHECK(g.newBestAt < 0 && g.best.size() == 2);
    feed(g, 500, 9.0, 9.5);
    CHECK(!g.running);
    feed(g, 3000, 9.5, 9.8);
    CHECK(g.running);
    feed(g, 3000, 9.8, 10.0);
    feed(g, 30000, 10.0, 10.5);
    feed(g, 400000, 10.5, 11.0);
    feed(g, 900000, 11.0, 11.5);
    CHECK(g.running && g.score == 900000 && fabs(g.runStart - 9.0) < 0.05);

    Tracker b;
    feed(b, 100, 0, 2.0);
    feed(b, 500, 2.0, 2.3);
    feed(b, 50000000, 2.3, 3.0);
    feed(b, 2000000, 3.0, 3.4);
    CHECK(b.running && b.score == 50000000);
    feed(b, 6, 3.4, 3.6);
    feed(b, 40, 3.6, 5.4);
    feed(b, 90, 5.4, 5.7);
    CHECK(b.best.back() == 50000000 && b.best.front() == 100 && b.score == 90 && b.running);
    CHECK(fabs(b.runStart - 3.4) < 0.02);

    Tracker f;
    f.rate = 1;
    feed(f, 100, 0, 2.0);
    feed(f, 500, 2.0, 2.3);
    feed(f, 30000000, 2.3, 3.0);
    f.Step("29000000", 3.00);
    f.Step("27000000", 3.01);
    feed(f, 25800000, 3.02, 3.6);
    CHECK(f.running && f.score == 25800000);
    feed(f, 26000000, 3.6, 3.8);
    f.Step("20000000", 3.80);
    f.Step("9000000", 3.81);
    CHECK(f.running);
    f.Step("2500000", 3.82);
    CHECK(!f.running && f.best.back() == 26000000);
    CHECK(f.best[3] == 30000000);
    feed(f, 1200000, 3.83, 3.9);
    feed(f, 900000, 3.9, 4.0);
    feed(f, 300000, 4.0, 4.1);
    feed(f, 97, 4.1, 5.1);
    CHECK(!f.running && f.best.back() == 26000000);
    feed(f, 18, 6.0, 8.0);
    CHECK(!f.running);
    feed(f, 60, 8.0, 8.3);
    CHECK(f.running && f.score == 60 && fabs(f.runStart - 6.0) < 0.02);

    Tracker c;
    c.rate = 1;
    feed(c, 100, 0, 2.0);
    feed(c, 500, 2.0, 2.3);
    feed(c, 80000000, 2.3, 3.2);
    feed(c, 5000000, 3.2, 6.5);
    CHECK(c.running && c.score == 5000000 && c.best.empty() && c.run[3] == 5000000);

    Tracker late;
    feed(late, 460000000, 0, 1.0);
    CHECK(!late.running && !late.Idle() && late.score == 460000000);
    feed(late, 716000000, 1.0, 2.0);
    late.Step("", 8);
    CHECK(!late.running && late.best.empty());
    feed(late, 100, 9.0, 11.0);
    feed(late, 300, 11.0, 11.3);
    CHECK(late.running && late.score == 300);

    Tracker u;
    u.rate = 1;
    u.best = {100, 200, 300, 350};
    feed(u, 20, 10.0, 12.0);
    feed(u, 50, 12.0, 12.3);
    CHECK(u.running && fabs(u.Target(u.runStart + 0.5) - 150) < 0.001 && u.Target(u.runStart + 99) == 350);
    u.Step("", 18);
    CHECK(!u.running && u.best.back() == 350 && u.newBestAt < 0);
    CHECK(u.paceKnown && u.pace > -0.93 && u.pace < -0.9);
    feed(u, 8953, 19.0, 21.0);
    feed(u, 9500, 21.0, 21.3);
    CHECK(u.running && u.score == 9500);

    Tracker v;
    v.rate = 1;
    v.best = {10, 20};
    feed(v, 500, 0, 0.3);
    feed(v, 900, 2.5, 2.8);
    v.Step("", 9);
    CHECK(!v.running && v.newBestAt == 9 && v.best.size() == 4 && v.best.front() == 500 && v.best.back() == 900);

    CHECK(v.endedAt == 9 && u.endedAt == 18);
    CHECK(v.paceKnown && fabs(v.pace - 24) < 1e-6 && !b.paceKnown);

    double p = 0;
    CHECK(Tracker::Pace({10, 200, 400}, 1, {10, 100, 200, 300}, 1, p) && fabs(p - 1) < 1e-9);
    CHECK(Tracker::Pace({10, 50, 400}, 1, {10, 100, 200, 300}, 1, p) && fabs(p) < 1e-9);
    CHECK(Tracker::Pace({10, 75, 150, 225, 225}, 1, {10, 100, 200, 300}, 1, p) && fabs(p + 0.25) < 1e-9);
    CHECK(!Tracker::Pace({10}, 1, {10, 100}, 1, p) && !Tracker::Pace({10, 50}, 1, {}, 1, p));
    CHECK(Tracker::Pace({1, 1, 100, 150}, 2, {10, 100, 200}, 1, p) && fabs(p) < 1e-9);
    CHECK(Tracker::At({0, 100, 200}, 0.5) == 50 && Tracker::At({0, 100, 200}, 1.25) == 125 && Tracker::At({0, 100, 200}, 7) == 200);

    Tracker w;
    feed(w, 100, 0, 2.0);
    feed(w, 500, 2.0, 2.3);
    w.Step("", 9);
    CHECK(!w.running && w.bestRate == 10 && w.best.size() == 24 && w.best.front() == 100 && w.best[21] == 100 && w.best.back() == 500);
    std::vector<long long> kept;
    int keptRate = 0;
    Tracker::ParseBest(Tracker::BestText({5, 60, 700}, 10), kept, keptRate);
    CHECK(keptRate == 10 && kept == std::vector<long long>({5, 60, 700}));
    Tracker::ParseBest("5,60,700", kept, keptRate);
    CHECK(keptRate == 1 && kept == std::vector<long long>({5, 60, 700}));
    CHECK(PaceWords(-0.25) == L"AVG 25% BELOW BEST PACE" && PaceWords(0.124) == L"AVG 12% ABOVE BEST PACE");
    CHECK(PaceWords(0.001) == L"AVG LEVEL WITH BEST PACE" && PaceWords(11.5) == L"AVG 1,150% ABOVE BEST PACE");

    Settings s = ParseSettings("size=150\nopacity=5\nsummary=999\nhide=1\nalways=1\n");
    CHECK(s.size == 150 && s.opacity == 30 && s.summary == 30 && s.always);
    Settings back = ParseSettings(SettingsText(s));
    CHECK(back.size == 150 && back.opacity == 30 && back.summary == 30 && back.always);
    Settings plain = ParseSettings("resize=70\nsize=abc\n");
    CHECK(plain.size == 100 && plain.opacity == 100 && plain.summary == 10 && !plain.always);

    CHECK(Grouped(1246227) == L"1,246,227" && Grouped(-76845930) == L"76,845,930" && Grouped(950) == L"950");

    store::Parse("size=150\r\nbest a_b=rate=10 5,60\n\nnot a line\n=nameless\n");
    CHECK(store::Get("size") == "150" && store::Get("best a_b") == "rate=10 5,60" && store::Get("not a line").empty());
    store::Put("size", "90");
    store::Put("overlay", "1,-2");
    store::Put("best a_b", "");
    CHECK(store::lines.size() == 2 && store::Get("size") == "90" && store::Get("overlay") == "1,-2" && store::Get("best a_b").empty());
    SaveSettingsLines(s);
    Settings kept2 = LoadSettings();
    CHECK(kept2.size == 150 && kept2.opacity == 30 && kept2.summary == 30 && kept2.always && store::Get("overlay") == "1,-2");
    store::lines.clear();

    CHECK(EaseOut(0) == 0 && EaseOut(1) == 1 && EaseOut(0.5) > 0.8);
    CHECK(EaseInOut(0.1) < 0.05 && EaseInOut(0.5) > 0.4 && EaseInOut(0.5) < 0.75);

    std::string d;
    CHECK(hud::ValidScore("1,578,942PTS", d) && d == "1578942" && hud::ValidScore("18PTS", d) && d == "18");
    CHECK(!hud::ValidScore("18", d) && !hud::ValidScore("1234PTS", d) && !hud::ValidScore("12,34PTS", d) && !hud::ValidScore("PTS", d));

    int cells = 0;
    for (int i = 0; i < hud::W * hud::H; i++) cells += hud::shapes[0][i] != hud::shapes[1][i];
    CHECK(cells > 0 && hud::Differ(hud::packed[0], hud::packed[1]) == cells && hud::Misfit(hud::shapes[1]) == 0);

    const int w2 = 220, h2 = 60;
    std::vector<uint32_t> px(w2 * h2, 0x00FFFFFF);
    int left = 23;
    for (char ch : std::string("907PTS")) {
        const std::string& shape = hud::shapes[hud::names.find(ch)];
        for (int i = 0; i < hud::W * hud::H; i++)
            if (shape[i] == '1') px[(20 + i / hud::W) * w2 + left + i % hud::W] = 0;
        left += hud::W + 6;
    }
    CHECK(hud::Read(px.data(), w2, h2) == "907");

    const int sw = 900, sh = 500, bx = 310, by = 330, bw = 456, bh = 60;
    std::vector<uint32_t> screen(sw * sh, 0x00202020);
    auto fill = [&](int x0, int y0, int fw, int fh, uint32_t colour) {
        for (int y = y0; y < y0 + fh; y++)
            for (int x = x0; x < x0 + fw; x++) screen[y * sw + x] = colour;
    };
    fill(0, 0, sw, 200, 0x00F0F0F0);
    fill(40, 240, 300, 60, 0x00FFFFFF);
    fill(bx, by, bw, bh, 0x00FFFFFF);
    left = bx + (bw - 174) / 2;
    for (char ch : std::string("907PTS")) {
        const std::string& shape = hud::shapes[hud::names.find(ch)];
        for (int i = 0; i < hud::W * hud::H; i++)
            if (shape[i] == '1') screen[(by + 20 + i / hud::W) * sw + left + i % hud::W] = 0;
        left += hud::W + 6;
    }
    RECT at;
    CHECK(hud::Locate(screen.data(), sw, sh, at));
    CHECK(abs(at.left - bx) <= 6 && abs(at.top - by) <= 6 && abs(at.right - bx - bw) <= 6 && abs(at.bottom - by - bh) <= 6);
    fill(bx, by, bw, bh, 0x00FFFFFF);
    CHECK(!hud::Locate(screen.data(), sw, sh, at));
}

static RECT g_box;
static POINT g_corner;
static Tracker g_tracker;

static bool LoadPlaces() {
    std::vector<long long> r = Numbers(store::Get("region"));
    bool have = (r.size() == 4 || r.size() == 8) && r[2] >= 8 && r[3] >= 8;
    if (have) {
        g_box.left = LONG(r[0]); g_box.top = LONG(r[1]); g_box.right = LONG(r[0] + r[2]); g_box.bottom = LONG(r[1] + r[3]);
        g_corner.x = g_box.left - 10; g_corner.y = g_box.bottom + 25;
        if (r.size() == 8) {
            g_corner.x = LONG(r[4] - r[7] / 4);
            g_corner.y = LONG(r[5] + r[7] + r[7] * 5 / 4);
        }
    } else {
        g_corner.x = (GetSystemMetrics(SM_CXSCREEN) - 413) / 2;
        g_corner.y = GetSystemMetrics(SM_CYSCREEN) / 5;
    }
    std::vector<long long> o = Numbers(store::Get("overlay"));
    if (o.size() == 2) { g_corner.x = LONG(o[0]); g_corner.y = LONG(o[1]); }
    return have;
}

static std::string IniValue(const std::string& ini, const std::string& key) {
    size_t pos = 0;
    while (pos < ini.size()) {
        size_t end = ini.find('\n', pos);
        if (end == std::string::npos) end = ini.size();
        if (ini.compare(pos, key.size() + 1, key + "=") == 0) {
            std::string v = ini.substr(pos + key.size() + 1, end - pos - key.size() - 1);
            while (!v.empty() && (v.back() == '\r' || v.back() == ' ')) v.pop_back();
            return v;
        }
        pos = end + 1;
    }
    return "";
}

static void LoadBest() {
    std::string key = "default";
    PWSTR docs = NULL;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, NULL, &docs))) {
        std::string ini = ReadText(std::wstring(docs) + HIDW(L"\\Assetto Corsa\\cfg\\race.ini"));
        if (!ini.empty()) key = IniValue(ini, HID("TRACK")) + "_" + IniValue(ini, HID("MODEL"));
        CoTaskMemFree(docs);
    }
    for (char& c : key)
        if (c == '=' || c == '\r' || c == '\n') c = '_';
    g_tracker.bestKey = HID("best ") + key;
    Tracker::ParseBest(store::Get(g_tracker.bestKey), g_tracker.best, g_tracker.bestRate);
    g_tracker.paceKnown = false;
    if (!g_tracker.best.empty() && g_tracker.best.front() >= Tracker::FreshMax) g_tracker.best.clear();
}

static const int PW = 413, PH = 70;
static HWND g_main, g_panel;
static HWND g_settings;
static bool g_moving;
static double g_shownAt;
static HDC g_grabDC, g_panelDC;
static HBITMAP g_grabBmp, g_panelBmp;
static uint32_t* g_grabPx;
static void* g_panelBits;
static int g_grabW, g_grabH;
static std::mutex g_grabLock;
static std::mutex g_readLock;
static std::string g_readDigits;
static int g_readCount;
static std::atomic<bool> g_runLive;
static std::atomic<bool> g_watching;
static std::atomic<bool> g_viaGpu;
static std::atomic<int> g_barWidth;
static int g_placesVersion;
static std::atomic<bool> g_gameFront;
static RECT g_foundBox, g_foundBar;

static void* MakeDib(HDC& dc, HBITMAP& bmp, int w, int h) {
    if (bmp) { DeleteDC(dc); DeleteObject(bmp); }
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = NULL;
    dc = CreateCompatibleDC(NULL);
    bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    SelectObject(dc, bmp);
    return bits;
}

static void ApplyPlaces() {
    std::lock_guard<std::mutex> hold(g_grabLock);
    if (!LoadPlaces()) return;
    g_grabW = g_box.right - g_box.left;
    g_grabH = g_box.bottom - g_box.top;
    g_grabPx = (uint32_t*)MakeDib(g_grabDC, g_grabBmp, g_grabW, g_grabH);
    g_placesVersion++;
}

namespace ui {
using namespace Gdiplus;

const float STRETCH = 1.15f;
FontFamily* family;
StringFormat* typographic;
Graphics* measure;
float digitH, digitC;
bool motion = true;
float scale = 1;
int cw = PW, ch = PH;
std::vector<uint32_t> backdrop;

const Color White(255, 255, 255, 255), Black(255, 0, 0, 0), Green(255, 74, 214, 109), Red(255, 255, 92, 92);

static Color Fade(const Color& c, double a) {
    a = (std::max)(0.0, (std::min)(1.0, a));
    return Color(BYTE(c.GetA() * a + 0.5), c.GetR(), c.GetG(), c.GetB());
}

static void RoundPath(GraphicsPath& p, float x, float y, float w, float h, float r) {
    float d = 2 * r;
    p.AddArc(x, y, d, d, 180, 90);
    p.AddArc(x + w - d, y, d, d, 270, 90);
    p.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    p.AddArc(x, y + h - d, d, d, 90, 90);
    p.CloseFigure();
}

static void Block(Graphics& g, const Color& c, float x, float y, float w, float h, float r) {
    GraphicsPath p;
    RoundPath(p, x, y, w, h, r);
    SolidBrush brush(c);
    g.FillPath(&brush, &p);
}

struct Label {
    float capH, cx, cy, maxW;
    std::wstring text;
    GraphicsPath* path = nullptr;

    void Set(const std::wstring& s) {
        if (path && s == text) return;
        text = s;
        delete path;
        path = new GraphicsPath();
        float em = capH / digitH;
        path->AddString(s.c_str(), -1, family, FontStyleRegular, em, PointF(0, 0), typographic);
        Font font(family, em, FontStyleRegular, UnitPixel);
        RectF box;
        measure->MeasureString(s.c_str(), -1, &font, PointF(0, 0), typographic, &box);
        float k = (std::min)(1.0f, maxW / (box.Width * STRETCH));
        Matrix m;
        m.Translate(cx - box.Width * STRETCH * k / 2, cy - digitC * em * k);
        m.Scale(STRETCH * k, k);
        path->Transform(&m);
    }

    void Draw(Graphics& g, const Color& c, float dy = 0) const {
        if (!path) return;
        SolidBrush brush(c);
        g.TranslateTransform(0, dy);
        g.FillPath(&brush, path);
        g.TranslateTransform(0, -dy);
    }
};

static void Setup(Graphics& g) {
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);
    g.SetTextRenderingHint(TextRenderingHintAntiAlias);
}

static void Init() {
    family = new FontFamily(L"Segoe UI Black");
    if (family->GetLastStatus() != Ok) family = new FontFamily(L"Arial Black");
    if (family->GetLastStatus() != Ok) family = FontFamily::GenericSansSerif()->Clone();
    typographic = StringFormat::GenericTypographic()->Clone();
    typographic->SetFormatFlags(typographic->GetFormatFlags() | StringFormatFlagsMeasureTrailingSpaces);
    measure = new Graphics(new Bitmap(1, 1, PixelFormat32bppPARGB));
    measure->SetTextRenderingHint(TextRenderingHintAntiAlias);
    GraphicsPath zero;
    zero.AddString(L"0", -1, family, FontStyleRegular, 100, PointF(0, 0), typographic);
    RectF b;
    zero.GetBounds(&b);
    digitH = b.Height / 100;
    digitC = (b.Y + b.Height / 2) / 100;
    BOOL animations = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0);
    motion = animations != 0;
}

static void Resize(float s) {
    scale = s;
    cw = int(ceil(PW * s));
    ch = int(ceil(PH * s));
    g_panelBits = MakeDib(g_panelDC, g_panelBmp, cw, ch);
    backdrop.assign(size_t(cw) * ch, 0);
    Bitmap bmp(cw, ch, cw * 4, PixelFormat32bppPARGB, (BYTE*)backdrop.data());
    Graphics g(&bmp);
    Setup(g);
    g.ScaleTransform(s, s);
    GraphicsPath body;
    RoundPath(body, 0.5f, 0.5f, PW - 1.0f, PH - 1.0f, 13);
    SolidBrush glass(Color(172, 16, 16, 18));
    g.FillPath(&glass, &body);
    g.SetClip(&body);
    SolidBrush dot(Color(13, 255, 255, 255));
    for (int y = 4; y < PH; y += 4)
        for (int x = 4; x < PW; x += 4) g.FillRectangle(&dot, x, y, 1, 1);
    g.ResetClip();
    Pen edge(Color(30, 255, 255, 255), 1);
    g.DrawPath(&edge, &body);
    Block(g, White, 9, 9, 395, 52, 6.5f);
}

static void Frame(double now) {
    static Label number = {19.5f, 206.5f, 35, 366}, note = {12.5f, 206.5f, 35, 366};
    static int mode = -1;
    static double modeAt = 0, textAt = 0;
    static double shown = 0, shownAt = 0;

    const Tracker& t = g_tracker;
    double bestFinal = t.best.empty() ? 0 : double(t.best.back());
    double target = t.Target(now), gap = t.score - target;
    bool celebrate = !t.running && now - t.newBestAt < 4;
    const double SummaryFor = g_set.summary, SummaryFade = 0.5;
    double summaryEnd = (std::max)(t.endedAt, t.newBestAt + 4) + SummaryFor;
    enum { Moving, Looking, NewBest, Summary, Waiting, Over, Live, First };
    int newMode = g_moving ? Moving : !g_grabPx ? Looking : celebrate ? NewBest : !t.running ? (t.paceKnown && now < summaryEnd ? Summary : t.Idle() ? Waiting : Over) : target > 0 ? Live : First;
    bool refresh = now >= textAt || newMode != mode;
    static double held = 0;
    if (newMode == Live && mode == Live && now - t.lastRead > 0.25) gap = held;
    held = gap;
    if (newMode != mode) {
        mode = newMode;
        modeAt = now;
        if (mode == Live) shown = gap;
    }

    shown += (gap - shown) * (1 - exp(-(now - shownAt) / 0.08));
    shownAt = now;
    if (mode == Live) number.Set((shown >= 0 ? HIDW(L"AHEAD ") : HIDW(L"BEHIND ")) + Grouped(shown));
    else if (refresh) {
        textAt = now + 0.1;
        if (mode == NewBest) number.Set(HIDW(L"NEW BEST ") + Grouped(bestFinal));
        else if (mode == Summary) number.Set(PaceWords(t.pace));
        else note.Set(mode == Moving ? HIDW(L"DRAG ME, THEN LET GO") : mode == Looking ? HIDW(L"LOOKING FOR YOUR SCORE") : mode == Waiting ? HIDW(L"WAITING FOR POINTS") : mode == Over ? HIDW(L"WAITING FOR NEXT RUN") : HIDW(L"FIRST RUN SETS YOUR BEST"));
    }

    memcpy(g_panelBits, backdrop.data(), backdrop.size() * 4);
    Bitmap canvas(cw, ch, cw * 4, PixelFormat32bppPARGB, (BYTE*)g_panelBits);
    Graphics g(&canvas);
    Setup(g);
    g.ScaleTransform(scale, scale);

    bool wide = mode == NewBest || mode == Live || mode == Summary;
    bool good = mode == NewBest || (mode == Summary ? llround(t.pace * 100) >= 0 : shown >= 0);
    double leave = mode == Summary ? (summaryEnd - now) / SummaryFade : 1;
    if (wide) Block(g, Fade(good ? Green : Red, leave), 9, 9, 395, 52, 6.5f);

    static Font* markFont = new Font(L"Segoe UI", 11, FontStyleBold, UnitPixel);
    static StringFormat* markAt = [] { auto f = new StringFormat(); f->SetAlignment(StringAlignmentFar); f->SetLineAlignment(StringAlignmentFar); return f; }();
    SolidBrush mark(Color(235, 88, 101, 242));
    g.DrawString(HIDW(L"deltahud.com").c_str(), -1, markFont, RectF(200, 40, 199, 20), markAt, &mark);

    double arrive = EaseOut((now - modeAt) / 0.2);
    (wide ? number : note).Draw(g, Fade(Black, arrive * (std::min)(1.0, (std::max)(0.0, leave))), motion ? float((1 - arrive) * 6) : 0);

    if (mode == NewBest && motion && now - t.newBestAt < 0.7) {
        float x = float(9 - 90 + (395 + 90) * EaseInOut((now - t.newBestAt) / 0.7));
        GraphicsPath bar;
        RoundPath(bar, 9, 9, 395, 52, 6.5f);
        g.SetClip(&bar);
        Color clear(0, 255, 255, 255), bright(190, 255, 255, 255);
        Color colours[3] = {clear, bright, clear};
        REAL stops[3] = {0, 0.5f, 1};
        LinearGradientBrush shine(PointF(x - 0.5f, 0), PointF(x + 90.5f, 0), clear, clear);
        shine.SetInterpolationColors(colours, stops, 3);
        g.FillRectangle(&shine, x, 9.0f, 90.0f, 52.0f);
        g.ResetClip();
    }

    POINT pos = g_corner, zero = {0, 0};
    SIZE size = {cw, ch};
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, BYTE(255 * EaseOut((now - g_shownAt) / 0.25) * g_set.opacity / 100), AC_SRC_ALPHA};
    UpdateLayeredWindow(g_panel, NULL, g_moving ? NULL : &pos, &size, g_panelDC, &zero, 0, &blend, ULW_ALPHA);
}
}

static std::wstring ExeName(DWORD pid) {
    std::wstring name;
    HANDLE process = pid ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid) : NULL;
    if (!process) return name;
    wchar_t path[1024];
    DWORD size = 1024, code = 0;
    if (GetExitCodeProcess(process, &code) && code == STILL_ACTIVE && QueryFullProcessImageNameW(process, 0, path, &size)) {
        name = path;
        name.erase(0, name.find_last_of(L'\\') + 1);
    }
    CloseHandle(process);
    return name;
}

static DWORD g_gamePid;

static bool GameRunning() {
    if (g_gamePid && _wcsicmp(ExeName(g_gamePid).c_str(), HIDW(L"acs.exe").c_str())) g_gamePid = 0;
    return g_gamePid != 0;
}

static bool g_always;

static float g_baseScale = 1;
static void ApplyScale() { ui::Resize((std::max)(0.3f, (std::min)(6.0f, g_baseScale * g_set.size / 100.0f))); }

static bool GameInFront() {
    static DWORD lastPid = 0;
    static bool last = false, game = false;
    static double leftAt = 0;
    DWORD pid = 0;
    HWND front = GetForegroundWindow();
    if (front) GetWindowThreadProcessId(front, &pid);
    if (pid == lastPid) return last;
    lastPid = pid;
    std::wstring name = pid ? ExeName(pid) : L"";
    bool wasGame = game;
    game = !_wcsicmp(name.c_str(), HIDW(L"acs.exe").c_str());
    if (game) g_gamePid = pid;
    g_gameFront = game;
    last = game || pid == GetCurrentProcessId();
    if (wasGame != game) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        char line[400];
        if (game) sprintf_s(line, "%02d:%02d:%02d.%03d  the game is in front again after %.1f s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, Now() - leftAt);
        else sprintf_s(line, "%02d:%02d:%02d.%03d  the game lost the front to %ls\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, name.empty() ? L"(nothing)" : name.c_str());
        leftAt = Now();
        if (g_log) std::ofstream(g_dir + L"focus.txt", std::ios::app) << line;
    }
    return last;
}

static void SaveMiss(double now) {
    static int count = 0;
    static double lastAt = -1;
    if (!g_log || count >= 60 || now - lastAt < 0.5) return;
    lastAt = now;
    CreateDirectoryW((g_dir + L"misses").c_str(), NULL);
    wchar_t name[32];
    swprintf(name, 32, L"misses\\%02d.bmp", count++);
    DWORD bytes = DWORD(g_grabW) * g_grabH * 4;
    BITMAPFILEHEADER fh = {0x4D42, DWORD(sizeof fh + sizeof(BITMAPINFOHEADER)) + bytes, 0, 0, DWORD(sizeof fh + sizeof(BITMAPINFOHEADER))};
    BITMAPINFOHEADER ih = {sizeof ih, g_grabW, -g_grabH, 1, 32, BI_RGB};
    std::ofstream f(g_dir + name, std::ios::binary | std::ios::trunc);
    f.write((const char*)&fh, sizeof fh);
    f.write((const char*)&ih, sizeof ih);
    f.write((const char*)g_grabPx, bytes);
}

struct GpuGrab {
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> dup;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
    RECT monitor = {};
    int w = 0, h = 0;
    double retryAt = 0;

    bool On(const RECT& area) const {
        return dup && area.left >= monitor.left && area.top >= monitor.top && area.right <= monitor.right && area.bottom <= monitor.bottom;
    }

    bool Reach(const RECT& area, double now, bool isNew = false) {
        if (On(area)) return true;
        if (now < retryAt && !isNew) return false;
        retryAt = now + 5;
        return Open(area);
    }

    bool Open(const RECT& box) {
        using Microsoft::WRL::ComPtr;
        dup.Reset(); staging.Reset(); context.Reset(); device.Reset();
        w = h = 0;
        ComPtr<IDXGIFactory1> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;
        POINT mid = {(box.left + box.right) / 2, (box.top + box.bottom) / 2};
        ComPtr<IDXGIAdapter1> adapter;
        for (UINT a = 0; factory->EnumAdapters1(a, adapter.ReleaseAndGetAddressOf()) == S_OK; a++) {
            ComPtr<IDXGIOutput> output;
            for (UINT o = 0; adapter->EnumOutputs(o, output.ReleaseAndGetAddressOf()) == S_OK; o++) {
                DXGI_OUTPUT_DESC desc;
                output->GetDesc(&desc);
                monitor = desc.DesktopCoordinates;
                if (!PtInRect(&monitor, mid)) continue;
                if (box.left < monitor.left || box.top < monitor.top || box.right > monitor.right || box.bottom > monitor.bottom) return false;
                if (FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &device, NULL, &context))) return false;
                ComPtr<IDXGIOutput1> output1;
                if (FAILED(output.As(&output1)) || FAILED(output1->DuplicateOutput(device.Get(), &dup))) return false;
                DXGI_OUTDUPL_DESC dd;
                dup->GetDesc(&dd);
                if (dd.ModeDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM || dd.Rotation > DXGI_MODE_ROTATION_IDENTITY) { dup.Reset(); return false; }
                return true;
            }
        }
        return false;
    }

    int Grab(const RECT& box, uint32_t* px, UINT waitMs) {
        using Microsoft::WRL::ComPtr;
        if (!staging || box.right - box.left != w || box.bottom - box.top != h) {
            w = box.right - box.left;
            h = box.bottom - box.top;
            D3D11_TEXTURE2D_DESC td = {};
            td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
            td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_STAGING;
            td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (FAILED(device->CreateTexture2D(&td, NULL, staging.ReleaseAndGetAddressOf()))) { dup.Reset(); return -1; }
        }
        DXGI_OUTDUPL_FRAME_INFO info;
        ComPtr<IDXGIResource> resource;
        HRESULT hr = dup->AcquireNextFrame(waitMs, &info, &resource);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) return 0;
        if (FAILED(hr)) { dup.Reset(); return -1; }
        int result = 0;
        ComPtr<ID3D11Texture2D> frame;
        if (info.LastPresentTime.QuadPart != 0 && SUCCEEDED(resource.As(&frame))) {
            D3D11_BOX src = {UINT(box.left - monitor.left), UINT(box.top - monitor.top), 0, UINT(box.right - monitor.left), UINT(box.bottom - monitor.top), 1};
            context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, frame.Get(), 0, &src);
            D3D11_MAPPED_SUBRESOURCE map;
            if (SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map))) {
                for (int y = 0; y < h; y++) memcpy(px + size_t(y) * w, (const BYTE*)map.pData + size_t(y) * map.RowPitch, size_t(w) * 4);
                context->Unmap(staging.Get(), 0);
                result = 1;
            }
        }
        frame.Reset();
        resource.Reset();
        dup->ReleaseFrame();
        return result;
    }
};

static bool LookForScore(GpuGrab& gpu, HDC screen, double now, RECT& box, RECT& bar) {
    MONITORINFO mi = {sizeof mi};
    if (!GetMonitorInfoW(MonitorFromWindow(GetForegroundWindow(), MONITOR_DEFAULTTOPRIMARY), &mi)) return false;
    const RECT all = mi.rcMonitor;
    int w = all.right - all.left, h = all.bottom - all.top;
    HDC dc = NULL;
    HBITMAP bmp = NULL;
    uint32_t* px = (uint32_t*)MakeDib(dc, bmp, w, h);
    bool found = false;
    if (px) {
        bool got = gpu.Reach(all, now) ? gpu.Grab(all, px, 100) > 0 : BitBlt(dc, 0, 0, w, h, screen, all.left, all.top, SRCCOPY) != 0;
        if (got && hud::Locate(px, w, h, bar)) {
            OffsetRect(&bar, all.left, all.top);
            int sideways = (bar.right - bar.left) / 4, updown = (bar.bottom - bar.top) * 3 / 2;
            RECT roomy = {bar.left - sideways, bar.top - updown, bar.right + sideways, bar.bottom + updown};
            found = IntersectRect(&box, &roomy, &all) != 0;
        }
    }
    DeleteDC(dc);
    DeleteObject(bmp);
    return found;
}

static void ReaderThread() {
    HDC screen = GetDC(NULL);
    GpuGrab gpu;
    bool havePicture = false, unreadable = false;
    int version = -1;
    double scoreAt = 0;
    double lookAt = 0;
    std::string digits;
    for (;;) {
        double started = Now();
        bool haveBox = false;
        {
            std::lock_guard<std::mutex> hold(g_grabLock);
            if (g_grabPx) {
                haveBox = true;
                bool isNew = version != g_placesVersion;
                if (isNew) {
                    version = g_placesVersion;
                    scoreAt = started;
                }
                if (isNew || !gpu.On(g_box)) havePicture = false;
                bool viaGpu = gpu.Reach(g_box, started, isNew), fresh = false;
                if (viaGpu) {
                    int got = gpu.Grab(g_box, g_grabPx, havePicture ? 0 : 100);
                    if (got < 0) viaGpu = false;
                    fresh = got > 0;
                }
                if (!viaGpu) fresh = BitBlt(g_grabDC, 0, 0, g_grabW, g_grabH, screen, g_box.left, g_box.top, SRCCOPY) != 0;
                if (fresh) {
                    int barWidth = 0;
                    digits = hud::Read(g_grabPx, g_grabW, g_grabH, &unreadable, &barWidth);
                    if (!digits.empty()) g_barWidth = barWidth;
                    havePicture = true;
                    if (digits.empty() && unreadable && g_runLive) SaveMiss(started);
                }
                if (havePicture) {
                    if (!digits.empty()) scoreAt = started;
                    std::lock_guard<std::mutex> hand(g_readLock);
                    g_readDigits = digits;
                    g_readCount++;
                }
                g_viaGpu = viaGpu;
            }
        }
        if (g_gameFront && started >= lookAt && (!haveBox || started - scoreAt > 3)) {
            lookAt = started + (haveBox ? 2 : 0.5);
            RECT box, bar;
            if (LookForScore(gpu, screen, started, box, bar)) {
                {
                    std::lock_guard<std::mutex> hand(g_readLock);
                    g_foundBox = box;
                    g_foundBar = bar;
                }
                PostMessageW(g_main, WM_COMMAND, 6, 0);
                scoreAt = started;
                lookAt = started + 2;
            }
        }
        double rest = 0.008 - (Now() - started);
        if (!g_watching && !g_runLive) rest = 0.2;
        Sleep(rest > 0.001 ? DWORD(rest * 1000) : 1);
    }
}

static void Tick() {
    static std::string lastDigits = "none yet";
    static int frames = 0, seen = 0, seenAtReport = 0;
    static double reportAt = 0, drawCost = 0;
    static bool sawGame = false;

    double now = Now();
    std::string digits;
    {
        std::lock_guard<std::mutex> hold(g_readLock);
        if (g_readCount != seen) digits = g_readDigits;
        seen = g_readCount;
    }
    if (!digits.empty()) lastDigits = digits;
    g_tracker.Step(digits, now);
    g_runLive = g_tracker.running;
    static float pending = 0;
    static double pendingSince = 0;
    int barWidth = g_barWidth;
    if (barWidth > 0) {
        float want = (std::max)(0.6f, (std::min)(3.0f, barWidth / 394.0f));
        if (fabs(want - g_baseScale) <= 0.08f * g_baseScale) pending = 0;
        else if (pending == 0 || fabs(want - pending) > 0.03f * pending) { pending = want; pendingSince = now; }
        else if (now - pendingSince >= 1) {
            g_baseScale = pending;
            ApplyScale();
            store::Put("scale", std::to_string(int(pending * 1000)));
            store::Save();
            pending = 0;
        }
    }
    bool front = GameInFront();
    bool show = g_always || g_set.always || g_moving || front;
    if (show != (IsWindowVisible(g_panel) != 0)) {
        if (show) g_shownAt = now;
        ShowWindow(g_panel, show ? SW_SHOWNOACTIVATE : SW_HIDE);
    }
    g_watching = show;
    if (show) ui::Frame(now);
    drawCost += (Now() - now - drawCost) * 0.05;
    frames++;
    if (now - reportAt < 5) return;

    double fps = frames / (now - reportAt), rps = (seen - seenAtReport) / (now - reportAt);
    char status[240];
    sprintf_s(status, "%.0f frames a second (%.1f ms to draw one), %.0f score reads a second %s, last score read: %s", fps, drawCost * 1000, rps,
              g_viaGpu ? "from the graphics card" : "by screen copy (the slower fallback)", lastDigits.c_str());
    if (g_log) WriteText(g_dir + L"status.txt", status);
    reportAt = now;
    frames = 0;
    seenAtReport = seen;

    bool game = GameRunning();
    if (game && !sawGame && !g_tracker.running) LoadBest();
    if (sawGame && !game) DestroyWindow(g_main);
    sawGame = sawGame || game;
}

static LRESULT CALLBACK PanelProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_NCHITTEST && g_moving) return HTCAPTION;
    if (m == WM_EXITSIZEMOVE && g_moving) {
        RECT r;
        GetWindowRect(h, &r);
        g_corner.x = r.left;
        g_corner.y = r.top;
        char text[64];
        sprintf_s(text, "%ld,%ld", r.left, r.top);
        store::Put("overlay", text);
        store::Save();
        g_moving = false;
        SetWindowLongPtrW(h, GWL_EXSTYLE, GetWindowLongPtrW(h, GWL_EXSTYLE) | WS_EX_TRANSPARENT);
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

namespace settings {
using namespace Gdiplus;

const float W = 420, H = 466, PAD = 28;
enum { None, Size, Opacity, Summary, Always, Move, Reset, Exit, Close };
struct SliderInfo { const wchar_t* label; int low, high, step; int* value; float y; };
static SliderInfo sliders[3] = {
    {L"Panel size", 50, 200, 5, &g_set.size, 104},
    {L"Panel opacity", 30, 100, 5, &g_set.opacity, 166},
    {L"Summary stays for", 0, 30, 1, &g_set.summary, 228},
};
static const wchar_t* buttonLabels[2] = {L"MOVE PANEL", L"RESET BEST"};
static float px = 1;
static int hot = None;
static int held = None;

static RectF Area(int id) {
    if (id >= Size && id <= Summary) return RectF(PAD, sliders[id - Size].y + 22, W - 2 * PAD, 26);
    if (id == Always) return RectF(PAD, 292, W - 2 * PAD, 32);
    if (id >= Move && id <= Reset) { float w = (W - 2 * PAD - 10) / 2; return RectF(PAD + (id - Move) * (w + 10), 344, w, 40); }
    if (id == Exit) return RectF(PAD, 396, W - 2 * PAD, 36);
    if (id == Close) return RectF(W - 20 - 52, 20, 52, 52);
    return RectF();
}

static int At(float x, float y) {
    for (int id = Size; id <= Close; id++)
        if (Area(id).Contains(x, y)) return id;
    return None;
}

static void Save() { SaveSettings(g_set); }

static void Drag(int id, float x) {
    SliderInfo& s = sliders[id - Size];
    RectF a = Area(id);
    float part = (std::max)(0.0f, (std::min)(1.0f, (x - a.X - 8) / (a.Width - 16)));
    int value = s.low + int((part * (s.high - s.low)) / s.step + 0.5f) * s.step;
    if (value == *s.value) return;
    *s.value = value;
    if (id == Size) ApplyScale();
}

static void Words(Graphics& g, const wchar_t* text, const Font& font, const Color& colour, const RectF& box, StringAlignment across) {
    StringFormat format;
    format.SetAlignment(across);
    format.SetLineAlignment(StringAlignmentCenter);
    format.SetFormatFlags(StringFormatFlagsNoWrap);
    SolidBrush brush(colour);
    g.DrawString(text, -1, &font, box, &format, &brush);
}

static void Paint(HWND h) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    RECT client;
    GetClientRect(h, &client);
    Bitmap buffer(client.right, client.bottom, PixelFormat32bppPARGB);
    {
        Graphics g(&buffer);
        ui::Setup(g);
        g.ScaleTransform(px, px);
        g.Clear(Color(255, 16, 16, 18));
        SolidBrush dot(Color(13, 255, 255, 255));
        for (int y = 4; y < H; y += 4)
            for (int x = 4; x < W; x += 4) g.FillRectangle(&dot, x, y, 1, 1);
        Pen edge(Color(40, 255, 255, 255), 1);
        g.DrawRectangle(&edge, 0.5f, 0.5f, W - 1, H - 1);

        static Font heavy(ui::family, 19, FontStyleRegular, UnitPixel), heavySmall(ui::family, 11.5f, FontStyleRegular, UnitPixel);
        static Font value(ui::family, 14, FontStyleRegular, UnitPixel);
        static Font label(L"Segoe UI", 14.5f, FontStyleRegular, UnitPixel), mark(L"Segoe UI", 11, FontStyleBold, UnitPixel);
        const Color dim(255, 160, 166, 180), track(46, 255, 255, 255);

        ui::Block(g, ui::Green, 20, 20, W - 40, 52, 6.5f);
        Words(g, HIDW(L"DELTAHUD  SETTINGS").c_str(), heavy, ui::Black, RectF(34, 20, W - 120, 54), StringAlignmentNear);
        RectF x = Area(Close);
        if (hot == Close) ui::Block(g, Color(40, 0, 0, 0), x.X + 8, x.Y + 8, 36, 36, 6.5f);
        Pen cross(ui::Black, 2.6f);
        cross.SetStartCap(LineCapRound);
        cross.SetEndCap(LineCapRound);
        g.DrawLine(&cross, x.X + 18, x.Y + 26, x.X + 34, x.Y + 26);

        for (int id = Size; id <= Summary; id++) {
            const SliderInfo& s = sliders[id - Size];
            RectF a = Area(id);
            wchar_t text[32];
            if (id == Summary && !*s.value) wcscpy_s(text, L"OFF");
            else swprintf_s(text, id == Summary ? L"%d SEC" : L"%d%%", *s.value);
            Words(g, s.label, label, ui::White, RectF(PAD, s.y, 240, 22), StringAlignmentNear);
            Words(g, text, value, ui::Green, RectF(W - PAD - 120, s.y, 120, 22), StringAlignmentFar);
            float part = float(*s.value - s.low) / (s.high - s.low), knob = a.X + 8 + part * (a.Width - 16), mid = a.Y + a.Height / 2;
            ui::Block(g, track, a.X, mid - 3, a.Width, 6, 3);
            if (knob - a.X > 6) ui::Block(g, ui::Green, a.X, mid - 3, knob - a.X, 6, 3);
            float r = hot == id || held == id ? 10.0f : 8.0f;
            SolidBrush white(ui::White);
            g.FillEllipse(&white, knob - r, mid - r, 2 * r, 2 * r);
        }

        {
            RectF a = Area(Always);
            bool on = g_set.always;
            Words(g, HIDW(L"Show over every app").c_str(), label, hot == Always ? ui::White : Color(255, 226, 229, 236), RectF(a.X, a.Y, a.Width - 60, a.Height), StringAlignmentNear);
            float sx = a.X + a.Width - 46, sy = a.Y + 4;
            ui::Block(g, on ? ui::Green : track, sx, sy, 46, 24, 12);
            SolidBrush knob(on ? ui::Black : ui::White);
            g.FillEllipse(&knob, on ? sx + 25 : sx + 3, sy + 3, 18.0f, 18.0f);
        }

        for (int id = Move; id <= Reset; id++) {
            RectF a = Area(id);
            bool down = held == id && hot == id;
            Color fill = id == Reset ? ui::Red : ui::White;
            ui::Block(g, hot == id && !down ? fill : ui::Fade(fill, down ? 0.7 : 0.88), a.X, a.Y + (down ? 1 : 0), a.Width, a.Height, 6.5f);
            Words(g, buttonLabels[id - Move], heavySmall, ui::Black, RectF(a.X, a.Y + (down ? 2 : 1), a.Width, a.Height), StringAlignmentCenter);
        }

        {
            RectF a = Area(Exit);
            GraphicsPath outline;
            ui::RoundPath(outline, a.X + 0.5f, a.Y + 0.5f, a.Width - 1, a.Height - 1, 6.5f);
            if (hot == Exit) ui::Block(g, Color(held == Exit ? 60 : 34, 255, 255, 255), a.X, a.Y, a.Width, a.Height, 6.5f);
            Pen line(Color(90, 255, 255, 255), 1);
            g.DrawPath(&line, &outline);
            Words(g, HIDW(L"EXIT DELTAHUD").c_str(), heavySmall, ui::White, RectF(a.X, a.Y + 1, a.Width, a.Height), StringAlignmentCenter);
        }

        Words(g, HIDW(L"Changes apply straight away").c_str(), mark, dim, RectF(PAD, H - 26, 220, 16), StringAlignmentNear);
        Words(g, HIDW(L"deltahud.com").c_str(), mark, Color(235, 88, 101, 242), RectF(W - PAD - 120, H - 26, 120, 16), StringAlignmentFar);
    }
    Graphics screen(dc);
    screen.DrawImage(&buffer, 0, 0);
    EndPaint(h, &ps);
}

static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    float x = GET_X_LPARAM(lp) / px, y = GET_Y_LPARAM(lp) / px;
    switch (m) {
    case WM_PAINT:
        Paint(h);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_NCHITTEST: {
        POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(h, &p);
        if (p.y / px < 78 && At(p.x / px, p.y / px) != Close) return HTCAPTION;
        return HTCLIENT;
    }
    case WM_MOUSEMOVE: {
        if (held >= Size && held <= Summary) Drag(held, x);
        int now = At(x, y);
        if (now != hot || held) InvalidateRect(h, NULL, FALSE);
        hot = now;
        TRACKMOUSEEVENT leave = {sizeof leave, TME_LEAVE, h, 0};
        TrackMouseEvent(&leave);
        return 0;
    }
    case WM_MOUSELEAVE:
        hot = None;
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_LBUTTONDOWN:
        held = At(x, y);
        if (held) SetCapture(h);
        if (held >= Size && held <= Summary) Drag(held, x);
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_LBUTTONUP: {
        int was = held;
        held = None;
        ReleaseCapture();
        InvalidateRect(h, NULL, FALSE);
        if (was >= Size && was <= Summary) Save();
        if (!was || At(x, y) != was) return 0;
        if (was == Always) { g_set.always = !g_set.always; Save(); }
        if (was == Move) SendMessageW(g_main, WM_COMMAND, 4, 0);
        if (was == Reset) SendMessageW(g_main, WM_COMMAND, 2, 0);
        if (was == Exit) DestroyWindow(g_main);
        if (was == Close) ShowWindow(h, SW_MINIMIZE);
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) ShowWindow(h, SW_MINIMIZE);
        return 0;
    case WM_CLOSE:
        DestroyWindow(g_main);
        return 0;
    case WM_DESTROY:
        g_settings = NULL;
        hot = held = None;
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

static void Create(HINSTANCE inst) {
    if (!g_settings) {
        std::wstring clsSet = HIDW(L"PaceVsBestSettings");
        static bool registered = false;
        if (!registered) {
            registered = true;
            WNDCLASSW wc = {};
            wc.style = CS_DROPSHADOW;
            wc.hInstance = inst;
            wc.lpfnWndProc = Proc;
            wc.lpszClassName = clsSet.c_str();
            wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
            wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
            RegisterClassW(&wc);
        }
        HDC screen = GetDC(NULL);
        px = GetDeviceCaps(screen, LOGPIXELSX) / 96.0f;
        ReleaseDC(NULL, screen);
        POINT mouse;
        GetCursorPos(&mouse);
        MONITORINFO mi = {sizeof mi};
        GetMonitorInfoW(MonitorFromPoint(mouse, MONITOR_DEFAULTTONEAREST), &mi);
        int w = int(W * px + 0.5f), hgt = int(H * px + 0.5f);
        g_settings = CreateWindowExW(WS_EX_APPWINDOW, clsSet.c_str(), APP, WS_POPUP | WS_SYSMENU | WS_MINIMIZEBOX,
                                     (mi.rcWork.left + mi.rcWork.right - w) / 2, (mi.rcWork.top + mi.rcWork.bottom - hgt) / 2, w, hgt,
                                     NULL, NULL, inst, NULL);
        int round = 2;
        DwmSetWindowAttribute(g_settings, 33, &round, sizeof round);
    }
}
}

static LRESULT CALLBACK MainProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case 2:
            if (MessageBoxW(h, HIDW(L"Delete the saved best run for this track and car?").c_str(), APP, MB_YESNO | MB_ICONQUESTION | MB_TOPMOST) == IDYES) {
                g_tracker.best.clear();
                g_tracker.paceKnown = false;
                store::Put(g_tracker.bestKey, "");
                store::Save();
            }
            break;
        case 3:
            DestroyWindow(h);
            break;
        case 5:
            ShowWindow(g_settings, SW_RESTORE);
            SetForegroundWindow(g_settings);
            break;
        case 4:
            g_moving = true;
            SetWindowLongPtrW(g_panel, GWL_EXSTYLE, GetWindowLongPtrW(g_panel, GWL_EXSTYLE) & ~WS_EX_TRANSPARENT);
            break;
        case 6: {
            RECT box, bar;
            {
                std::lock_guard<std::mutex> hold(g_readLock);
                box = g_foundBox;
                bar = g_foundBar;
            }
            if (g_grabPx && EqualRect(&box, &g_box)) break;
            char text[200];
            sprintf_s(text, "%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld", box.left, box.top, box.right - box.left, box.bottom - box.top,
                      bar.left, bar.top, bar.right - bar.left, bar.bottom - bar.top);
            store::Put("region", text);
            store::Save();
            ApplyPlaces();
            break;
        }
        }
        return 0;
    case WM_DESTROY:
        if (g_tracker.running) g_tracker.Stop(g_tracker.score, Now());
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

static int ReadImage(int argc, LPWSTR* a) {
    Gdiplus::Bitmap img(a[2]);
    bool whole = argc == 4;
    int x = whole ? 0 : _wtoi(a[3]), y = whole ? 0 : _wtoi(a[4]), w = whole ? int(img.GetWidth()) : _wtoi(a[5]), h = whole ? int(img.GetHeight()) : _wtoi(a[6]);
    Gdiplus::Rect r(x, y, w, h);
    Gdiplus::BitmapData d;
    if (img.LockBits(&r, Gdiplus::ImageLockModeRead, PixelFormat32bppRGB, &d) != Gdiplus::Ok) return 2;
    std::vector<uint32_t> px(size_t(w) * h);
    for (int row = 0; row < h; row++) memcpy(&px[size_t(row) * w], (BYTE*)d.Scan0 + row * d.Stride, size_t(w) * 4);
    img.UnlockBits(&d);
    std::ofstream f(a[whole ? 3 : 7], std::ios::app);
    if (whole) {
        RECT bar = {};
        std::string note;
        double from = Now();
        bool found = hud::Locate(px.data(), w, h, bar, &note);
        char place[120] = "-";
        if (found) sprintf_s(place, "bar=%ld,%ld,%ld,%ld", bar.left, bar.top, bar.right - bar.left, bar.bottom - bar.top);
        f << place << " | " << note << " | " << int((Now() - from) * 1000) << " ms\n";
        return 0;
    }
    if (!wcsncmp(a[1], L"--learn", 7)) {
        std::vector<int> tried;
        RECT bar;
        std::vector<std::pair<char, std::string>> learned;
        bool sawInk, sure;
        bool light = a[1][7] == L'w';
        if (hud::FindBar(px.data(), w, h, bar, nullptr, tried) && !hud::ReadBar(px.data(), w, bar, light, &sawInk, &sure, nullptr, &learned).empty())
            for (const auto& glyph : learned) f << glyph.first << " " << glyph.second << "\n";
        return 0;
    }
    int barWidth = 0;
    std::string trace;
    std::string digits = hud::Read(px.data(), w, h, nullptr, &barWidth, &trace);
    f << (digits.empty() ? "-" : digits) << " bar=" << barWidth << " | " << trace << "\n";
    return 0;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    SetProcessDPIAware();
    Gdiplus::GdiplusStartupInput gdiplusInput;
    ULONG_PTR gdiplusToken;
    Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusInput, NULL);

    wchar_t path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    g_dir = path;
    g_dir.erase(g_dir.find_last_of(L'\\') + 1);
    if (!hud::Load()) Fail(HIDW(L"The digit shapes built into DeltaHUD.exe are missing or damaged. Download it again."));
    SelfCheck();

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc == 8 && (!wcscmp(argv[1], L"--read") || !wcsncmp(argv[1], L"--learn", 7))) return ReadImage(argc, argv);
    if (argc == 4 && !wcscmp(argv[1], L"--find")) return ReadImage(argc, argv);
    for (int i = 1; i < argc; i++) {
        if (!wcscmp(argv[i], L"--always")) g_always = true;
        if (!wcscmp(argv[i], L"--log")) g_log = true;
    }

    CreateMutexW(NULL, TRUE, HIDW(L"PaceVsBestSingleInstance").c_str());
    if (GetLastError() == ERROR_ALREADY_EXISTS) Fail(HIDW(L"DeltaHUD is already running. It is in the taskbar: click it there for its settings and its Exit button."));

    std::wstring clsMain = HIDW(L"PaceVsBest"), clsPanel = HIDW(L"PaceVsBestPanel");
    WNDCLASSW wc = {};
    wc.hInstance = inst;
    wc.lpfnWndProc = MainProc;
    wc.lpszClassName = clsMain.c_str();
    RegisterClassW(&wc);
    wc.lpfnWndProc = PanelProc;
    wc.lpszClassName = clsPanel.c_str();
    RegisterClassW(&wc);

    store::Load();
    LoadPlaces();
    LoadBest();
    ui::Init();
    std::vector<long long> savedScale = Numbers(store::Get("scale"));
    g_baseScale = savedScale.size() == 1 && savedScale[0] >= 600 && savedScale[0] <= 3000 ? savedScale[0] / 1000.0f : 1.0f;
    g_set = LoadSettings();
    ApplyScale();

    g_main = CreateWindowExW(WS_EX_TOOLWINDOW, clsMain.c_str(), APP, WS_POPUP, 0, 0, 0, 0, NULL, NULL, inst, NULL);
    g_panel = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
                              clsPanel.c_str(), L"", WS_POPUP, g_corner.x, g_corner.y, PW, PH, NULL, NULL, inst, NULL);
    ApplyPlaces();
    Now();
    timeBeginPeriod(1);
    std::thread(ReaderThread).detach();

    settings::Create(inst);
    ShowWindow(g_settings, SW_SHOWMINNOACTIVE);
    ShowWindow(g_settings, SW_SHOWMINNOACTIVE);

    DWM_TIMING_INFO timing = {sizeof timing};
    double refresh = 60;
    if (SUCCEEDED(DwmGetCompositionTimingInfo(NULL, &timing)) && timing.rateRefresh.uiDenominator)
        refresh = (std::max)(30.0, (std::min)(500.0, double(timing.rateRefresh.uiNumerator) / timing.rateRefresh.uiDenominator));
    for (;;) {
        double started = Now();
        MSG msg;
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) return 0;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Tick();
        if (FAILED(DwmFlush())) Sleep(5);
        while (Now() - started < 0.75 / refresh) Sleep(1);
    }
}

#include "StringUtil.h"
#include <algorithm>
#include <cctype>

std::string UrlEncode(const std::string& s)
{
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if ((c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~')
        {
            out.push_back(c);
        }
        else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 15]);
        }
    }
    return out;
}

std::string Trim(const std::string& s)
{
    size_t a = 0;
    while (a < s.size() && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) a++;

    size_t b = s.size();
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) b--;

    return s.substr(a, b - a);
}

void ReplaceAll(std::string& s, const std::string& from, const std::string& to)
{
    if (from.empty()) return;

    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
}

std::string SanitizeTikTok(const std::string& input)
{
    std::string s = Trim(input);
    s.erase(std::remove(s.begin(), s.end(), '@'), s.end());
    return Trim(s);
}

std::string SanitizeYouTubeHandle(const std::string& input)
{
    std::string s = Trim(input);

    s.erase(std::remove(s.begin(), s.end(), '@'), s.end());
    s.erase(std::remove_if(s.begin(), s.end(),
        [](unsigned char c) { return std::isspace(c) != 0; }), s.end());

    return Trim(s);
}

std::string SanitizeTwitchLogin(std::string s)
{
    s = Trim(s);

    if (!s.empty() && s[0] == '#')
        s.erase(s.begin());

    std::transform(s.begin(), s.end(), s.begin(),
        [](unsigned char c) { return (char)std::tolower(c); });

    return s;
}

static std::string ReadFileUtf8(const std::wstring& path) {
    FILE* f = nullptr;
    _wfopen_s(&f, path.c_str(), L"rb");
    if (!f) return "";
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::string data;
    data.resize(sz > 0 ? (size_t)sz : 0);
    if (sz > 0) fread(data.data(), 1, (size_t)sz, f);
    fclose(f);
    return data;
}

static std::string UrlEncodeGoogleFontFamily(std::string family)
{
    for (char& c : family) {
        if (c == ' ') c = '+';
    }
    return family;
}

static int ClampInt(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static int ParseIntOrDefault(const std::wstring& w, int def)
{
    try {
        if (w.empty()) return def;
        return std::stoi(w);
    }
    catch (...) {
        return def;
    }

}
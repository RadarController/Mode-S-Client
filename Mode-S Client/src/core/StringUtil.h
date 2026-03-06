#pragma once

#include <string>

std::string Trim(const std::string& s);
std::string UrlEncode(const std::string& s);
std::string SanitizeTikTok(const std::string& input);
std::string SanitizeYouTubeHandle(const std::string& input);
std::string SanitizeTwitchLogin(std::string s);
void ReplaceAll(std::string& s, const std::string& from, const std::string& to);

std::string ReadFileUtf8(const std::wstring& path);
std::string UrlEncodeGoogleFontFamily(std::string family);
int ClampInt(int v, int lo, int hi);
int ParseIntOrDefault(const std::wstring& w, int def);

std::string ToUtf8(const std::wstring& w);
std::wstring ToW(const std::string& s);
#pragma once

#include <string>

// Basic string helpers
std::string Trim(const std::string& s);
void ReplaceAll(std::string& s, const std::string& from, const std::string& to);
std::string UrlEncode(const std::string& s);

// Platform sanitizers
std::string SanitizeTikTok(const std::string& input);
std::string SanitizeYouTubeHandle(const std::string& input);
std::string SanitizeTwitchLogin(std::string s);
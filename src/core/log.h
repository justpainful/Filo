// Minimal thread-safe file logger. Never logs file *contents*; file names only at Debug level.
#pragma once
#include <string>
#include <string_view>
#include <cstdarg>

namespace filo::log {
enum class Level { Debug, Info, Warn, Error };
void Init(const wchar_t* fileName = L"filo.log");
void SetMinLevel(Level l);
void Write(Level l, std::string_view component, std::string_view msg);
void Writef(Level l, std::string_view component, const char* fmt, ...);
std::wstring CurrentLogPath();
}
#define FILO_LOG(level, comp, ...) ::filo::log::Writef(::filo::log::Level::level, comp, __VA_ARGS__)

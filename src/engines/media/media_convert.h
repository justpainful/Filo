// Conversion entry points used by the capability registrations (register.cpp). Public so tests and the CLI can call them.
#pragma once
#include "registry/capability.h"

namespace filo::media {
// Converts task.input to task.outExt (video container, gif, or audio format) writing task.output. Real ffmpeg work.
EngineResult ConvertMedia(const ConvertTask& task, JobContext& ctx);
// Opens the produced file with ffprobe and checks it has the right kind of stream. `ext` is the produced format.
bool VerifyMediaOutput(const fs::path& produced, const std::wstring& ext, std::wstring* why);
}

// Internal helpers of the metadata engine.
#pragma once
#include "engines/meta/metadata.h"
#include "core/json.h"
#include "core/process.h"
#include <atomic>
#include <functional>

namespace filo::meta::detail {

struct EtRun {
  bool started = false, timedOut = false, cancelled = false;
  int exit = -1;
  std::string out, err;
};

// Runs ExifTool with an argument file (UTF-8, one argument per line) so that Unicode names / values and quotes survive.
// `args` are the options and file names; the common prefix (-charset utf8 -charset filename=utf8 ...) is added here.
EtRun RunExifTool(const std::vector<std::wstring>& args, JobContext* ctx, DWORD timeoutMs = 180000);

// Escapes a value for `-E` (ExifTool then unescapes &amp; &lt; &gt; &quot; &#NN;) so that newlines and & survive an args file.
std::string EscapeValue(const std::wstring& v);

struct Classified { Group group; std::wstring label; FieldType type; };

// Format helpers
std::wstring FormatOf(const fs::path& f);                 // DetectFile().format
enum class Kind { Image, Audio, Video, Pdf, Office, Other };
Kind KindOf(const std::wstring& format);
bool ExifToolWritable(const std::wstring& format);        // ExifTool can write this container
bool FfmpegTaggable(const std::wstring& format);          // audio + mkv / webm tags via ffmpeg
bool OfficeFormat(const std::wstring& format);            // docx / xlsx / pptx

std::wstring Humanize(const std::string& tag);
Classified Classify(const std::string& group1, const std::string& tag, const std::wstring& format, const Json& rawValue);
bool KeyEditable(const std::string& group1, const std::string& tag, const std::wstring& format);

// Content fingerprints (see ContentFingerprint)
std::optional<std::string> PixelFingerprint(const fs::path& file, std::wstring* how);
std::optional<std::string> PacketFingerprint(const fs::path& file, std::wstring* how, JobContext* ctx);
std::optional<std::string> PdfFingerprint(const fs::path& file, std::wstring* how);
std::optional<std::string> OfficeFingerprint(const fs::path& file, std::wstring* how);

// Office documents: edit docProps (core.xml, app.xml) by rewriting the zip with miniz.
WriteResult WriteOffice(const fs::path& input, const Edits& edits, const fs::path& tempOut, bool stripAll, bool personalOnly);

// ffmpeg tag path (audio containers and MKV / WebM)
WriteResult WriteViaFfmpeg(const fs::path& input, const std::wstring& format, const Edits& edits, const fs::path& tempOut, JobContext* ctx);

std::string Sha256Hex(const void* data, size_t n);
class Sha256 {
 public:
  Sha256();
  ~Sha256();
  void Update(const void* d, size_t n);
  std::string Finish();      // hex
 private:
  void* alg_ = nullptr; void* h_ = nullptr; std::vector<uint8_t> obj_;
};

} // namespace filo::meta::detail

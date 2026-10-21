// Filo archive engine: safe listing / extraction and writers for ZIP, TAR, TAR.GZ and GZ, plus RAR / 7z (read only,
// through the Windows bsdtar). Headless: no UI, no shell. Everything long-running takes an optional JobContext for
// progress + cancellation, and never writes outside the directory / temp file it is given.
#pragma once
#include "core/job.h"
#include "core/util.h"
#include <string>
#include <string_view>
#include <vector>

namespace filo::archive {

// ---------------------------------------------------------------------------------------------------------------------
// Entry path validation (pure function, no file system access)
// ---------------------------------------------------------------------------------------------------------------------

enum class PathVerdict {
  Ok,       // usable exactly as written (after separator normalisation)
  Fixed,    // usable after a safe rename; `path` holds the new name and `notes` say what changed
  Reject,   // dangerous (traversal, absolute, drive, UNC/device, alternate data stream...) - never extract
};

struct PathCheck {
  PathVerdict verdict = PathVerdict::Reject;
  std::wstring path;                  // safe relative path, '\' separators, no leading/trailing separator ("" = the root itself)
  std::wstring reason;                // Reject: why (user-facing). Fixed: first note.
  std::vector<std::wstring> notes;    // every adjustment made ("reserved name 'CON' renamed", "name shortened"...)
  bool Usable() const { return verdict != PathVerdict::Reject; }
};

struct PathPolicy {
  size_t maxComponent = 255;          // UTF-16 units per name component (NTFS limit)
  size_t maxRelative = 30000;         // total length budget for the relative path (the caller subtracts the destination length)
  bool renameReserved = true;         // CON, NUL, COM1... -> "_CON"; false = Reject
  bool replaceInvalidChars = true;    // < > " | ? * and control characters -> '_'; false = Reject
};

// Classifies one archive entry name. Both '/' and '\' are separators for every format (a "..\x" inside a tar is still
// a traversal on Windows). Rejects: empty after normalisation is allowed only for "." / "./" (root); NUL bytes, absolute
// paths (leading separator), drive letters, UNC and \\?\ \\.\ device prefixes, any '..' component, ':' anywhere
// (alternate data streams / drive tricks), components made only of dots and spaces. Fixes (with notes): reserved device
// names, trailing dots/spaces, invalid characters, bidi override characters, over-long components / paths.
PathCheck ValidateEntryPath(std::wstring_view name, const PathPolicy& policy = {});

// Validates the target of a symbolic / hard link stored at `entryName`. Reject when the target is absolute, has a
// drive / UNC / device prefix, or resolves (relative to the entry's folder; hard links relative to the archive root)
// outside the archive root. `hardLink` targets are archive-root relative.
PathCheck ValidateLinkTarget(std::wstring_view entryName, std::wstring_view target, bool hardLink);

// Decodes a name stored in a legacy 8-bit / DBCS code page (ZIP without the UTF-8 flag). forcedCodepage 0 = heuristic:
// valid UTF-8 -> UTF-8; else GBK / Shift-JIS / Korean / Big5 when the bytes are a clean double-byte text; else CP437
// (the ZIP default). `chosenCodepage` receives 65001 / 437 / 936 ...; `legacy` is true when it was not plain ASCII/UTF-8.
std::wstring DecodeLegacyName(const std::string& bytes, int forcedCodepage, int* chosenCodepage = nullptr, bool* legacy = nullptr);

// ---------------------------------------------------------------------------------------------------------------------
// Listing
// ---------------------------------------------------------------------------------------------------------------------

enum class Format { Unknown, Zip, Tar, TarGz, Gz, SevenZ, Rar, OtherLibarchive /* tar.bz2 / tar.xz / tar.zst... via bsdtar */ };
const wchar_t* FormatName(Format f);

enum class EntryType { File, Dir, Symlink, Hardlink, Other };

struct Entry {
  std::wstring name;                  // as stored (decoded to UTF-16), original separators
  EntryType type = EntryType::File;
  uint64_t size = 0;                  // uncompressed bytes
  uint64_t packedSize = 0;            // 0 = unknown
  uint64_t mtime = 0;                 // FILETIME ticks (UTC), 0 = unknown
  uint32_t crc32 = 0;
  bool hasCrc = false;
  uint32_t mode = 0;                  // unix mode bits when stored
  bool encrypted = false;
  bool legacyName = false;            // decoded with a legacy code page heuristic
  std::wstring linkTarget;            // symlink / hardlink target as stored
  // Filled by ListArchive:
  std::wstring safeName;              // where it would be extracted (relative); empty when refused
  std::wstring problem;               // non-empty => extraction would refuse (or rename) this entry; user-facing
  bool unsafe = false;                // true when problem means "refused"
};

struct ListOptions {
  int nameEncoding = 0;               // ZIP legacy code page override (0 = auto), like `unzip -O`
  uint64_t maxEntries = 1000000;
};

struct ListResult {
  bool ok = false;
  std::wstring error;
  Format format = Format::Unknown;
  std::vector<Entry> entries;
  uint64_t totalSize = 0;             // sum of file sizes
  uint64_t archiveSize = 0;
  bool encrypted = false;
  bool hasUnsafe = false;
  std::vector<std::wstring> warnings;
};

// Detects the container from its content (never from the extension).
Format DetectFormat(const fs::path& file);

// Lists without extracting. ZIP uses the central directory. TAR(.GZ) must be streamed (no index): the data is
// decompressed but not stored. RAR / 7z / other libarchive formats are first converted to a temporary tar by bsdtar.
ListResult ListArchive(const fs::path& file, const ListOptions& opt = {}, JobContext* ctx = nullptr);

// ---------------------------------------------------------------------------------------------------------------------
// Extraction
// ---------------------------------------------------------------------------------------------------------------------

struct ExtractOptions {
  uint64_t maxTotalBytes = 8ull << 30;     // 8 GiB of uncompressed data
  uint64_t maxEntries = 1000000;
  double maxRatio = 1000.0;                // uncompressed / compressed; only enforced once > ratioFloorBytes were produced
  uint64_t ratioFloorBytes = 1ull << 30;
  uint64_t reserveFreeBytes = 64ull << 20; // free space that must remain on the destination volume
  int nameEncoding = 0;                    // ZIP legacy code page override
  bool skipUnsafe = false;                 // false: any Reject entry aborts the whole extraction; true: skip them + warn
  bool extractLinks = false;               // reserved: symbolic links are never created; safe hard links are copied
  PathPolicy path;
};

struct ExtractResult {
  bool ok = false;
  std::wstring error;                      // user-facing
  std::vector<std::wstring> warnings;
  Format format = Format::Unknown;
  uint64_t files = 0, dirs = 0, bytes = 0, skipped = 0;
  std::vector<std::wstring> written;       // relative paths created (files only), in order
};

// Extracts into `destDir` (must exist; normally the temp directory of an OutputDirTransaction). Every entry is
// validated before any byte is written; nothing is created outside destDir (canonical prefix check). On failure the
// partial content is left for the caller's transaction to delete.
ExtractResult ExtractArchive(const fs::path& file, const fs::path& destDir, const ExtractOptions& opt = {}, JobContext* ctx = nullptr);

// Convenience for direct callers: extracts into `desiredDir` (made unique) through an OutputDirTransaction.
ExtractResult ExtractArchiveToFolder(const fs::path& file, const fs::path& desiredDir, fs::path* finalDir, const ExtractOptions& opt = {}, JobContext* ctx = nullptr);

// ---------------------------------------------------------------------------------------------------------------------
// Writers
// ---------------------------------------------------------------------------------------------------------------------

enum class ZipMethod { Deflate, Store, Auto /* store already-compressed extensions (jpg, mp4, zip...) */ };

struct CreateOptions {
  ZipMethod method = ZipMethod::Auto;
  int level = 6;                           // 1..9 (deflate)
  bool includeRoot = true;                 // folders: keep the folder name as the top-level entry
  bool verify = true;                      // re-list + fully read the result with bsdtar and compare names / sizes
};

struct CreateResult {
  bool ok = false;
  std::wstring error;
  std::vector<std::wstring> warnings;
  fs::path path;                           // committed final path (CreateZip... only)
  uint64_t entries = 0, bytes = 0;
};

// Public helpers for tool dialogs: archive files and / or folders. Written through an OutputTransaction at `desiredOut`
// (never overwrites silently; the committed path is in CreateResult::path).
CreateResult CreateZip(const std::vector<fs::path>& inputs, const fs::path& desiredOut, const CreateOptions& opt = {}, JobContext* ctx = nullptr);
CreateResult CreateTar(const std::vector<fs::path>& inputs, const fs::path& desiredOut, const CreateOptions& opt = {}, JobContext* ctx = nullptr);
CreateResult CreateTarGz(const std::vector<fs::path>& inputs, const fs::path& desiredOut, const CreateOptions& opt = {}, JobContext* ctx = nullptr);

// Lower level (used by the registry capabilities): write to exactly `outFile` (a temp path owned by the caller).
enum class OutKind { Zip, Tar, TarGz };
CreateResult WriteFromPaths(const std::vector<fs::path>& inputs, OutKind kind, const fs::path& outFile, const CreateOptions& opt, JobContext* ctx);
// Re-packs another archive (any readable format) entry by entry. Unsafe entries abort unless ExtractOptions::skipUnsafe.
CreateResult Repack(const fs::path& inputArchive, OutKind kind, const fs::path& outFile, const CreateOptions& opt, const ExtractOptions& eopt, JobContext* ctx);
// Decompresses a .gz holding a tar into a plain tar (gz>tar). Wraps a non-tar payload into a one-entry tar (warning).
CreateResult GunzipToTar(const fs::path& gzFile, const fs::path& outFile, const CreateOptions& opt, JobContext* ctx);

// Compresses one file with gzip (tar>gz): byte-exact, verified by decompressing and comparing with the source.
CreateResult GzipFile(const fs::path& in, const fs::path& outFile, const CreateOptions& opt, JobContext* ctx);

// Independent verification of a produced zip / tar / tar.gz: lists it with bsdtar (names + sizes), reads every byte
// (bsdtar checks the CRCs) and compares against the entries the writer reported.
struct WrittenEntry { std::wstring name; bool isDir = false; uint64_t size = 0; uint32_t crc32 = 0; };
bool VerifyWithBsdtar(const fs::path& produced, const std::vector<WrittenEntry>& expected, std::wstring* why, JobContext* ctx = nullptr);

// Lighter check for a produced archive when no entry list is known: bsdtar must list and fully read it without complaint.
bool VerifyReadable(const fs::path& produced, std::wstring* why, JobContext* ctx = nullptr);

// bsdtar (Windows tar.exe) availability: present, and able to read a built-in 7z and RAR5 sample (cached).
fs::path BsdtarPath();
bool BsdtarCanRead7z();
bool BsdtarCanReadRar();

} // namespace filo::archive

namespace filo {
void RegisterArchiveEngine();
}

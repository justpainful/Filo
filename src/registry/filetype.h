// File type registry: identifies the *real* format of a file from its content (magic bytes), never from the extension alone.
#pragma once
#include "core/util.h"
#include <string>
#include <vector>

namespace filo {

enum class Group { Unknown, Image, Video, Audio, Pdf, Document, Presentation, Spreadsheet, Subtitle, Archive, Folder, Text };
const wchar_t* GroupName(Group g);

struct FileKind {
  fs::path path;
  bool exists = false;
  bool isDir = false;
  bool readable = false;
  bool cloudPlaceholder = false;     // OneDrive "files on demand" not downloaded yet (we never trigger a download implicitly)
  uint64_t size = 0;
  std::wstring format;               // canonical lowercase format id of the detected content: "png","jpg","mp4","docx",...
  Group group = Group::Unknown;
  std::wstring declaredExt;          // lowercase extension as named on disk ("" if none)
  bool extensionMismatch = false;    // detected format differs from the declared extension (e.g. a PNG named .jpg)
  bool detectedByContent = false;    // false => decided from the extension only (e.g. legacy OLE, text formats)
  bool Supported() const { return group != Group::Unknown; }
};

// Reads at most 64 KiB. Directories are reported as Group::Folder (format "folder"; ".rtfd" bundles as "rtfd").
FileKind DetectFile(const fs::path& p);

// Pure helper for tests: classify from a header buffer + declared extension.
FileKind ClassifyBytes(const uint8_t* data, size_t n, const std::wstring& declaredExt);

// Canonical extension for a format id when writing files ("jpg" for jpeg, "tiff" for tif...).
std::wstring CanonicalExt(const std::wstring& ext);
Group GroupForExt(const std::wstring& ext);   // extension-only classification (used for outputs and fallbacks)

} // namespace filo

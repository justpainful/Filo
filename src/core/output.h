// Atomic output safety. Engines never write to the final path: they write to OutputTransaction::TempPath(), the
// transaction verifies the file exists and is non-empty, then moves it into place (never replacing silently).
#pragma once
#include "core/util.h"
#include "core/settings.h"
#include <functional>

namespace filo {

// Where output for `input` should go, honouring settings (next to the original, or a fixed folder) and falling back to
// "Documents\Filo Output" when the original's folder is read-only / unavailable. `fellBack` reports the fallback.
fs::path ResolveOutputDir(const fs::path& input, const Settings& s, bool* fellBack = nullptr);

// "<stem><suffix>.<ext>" inside ResolveOutputDir. Does not resolve name conflicts (the transaction does that at commit).
fs::path DesiredOutputPath(const fs::path& input, const std::wstring& outExt, const Settings& s, const std::wstring& stemSuffix = L"");

bool IsDirectoryWritable(const fs::path& dir);

class OutputTransaction {
 public:
  // `desiredFinal`: where the user expects the result. The temp file is created beside it (same volume => atomic rename).
  explicit OutputTransaction(const fs::path& desiredFinal, OverwritePolicy policy = OverwritePolicy::UniqueName);
  ~OutputTransaction();
  OutputTransaction(const OutputTransaction&) = delete;
  OutputTransaction& operator=(const OutputTransaction&) = delete;

  const fs::path& TempPath() const { return temp_; }       // keeps the real extension, e.g. ".filo-1a2b3c.tmp.webp"
  // Verifies (non-empty + optional verifier) and moves into place. Returns false and removes the temp on failure.
  bool Commit(std::wstring* error = nullptr, const std::function<bool(const fs::path&, std::wstring*)>& verify = {});
  void Rollback();                                          // deletes the temp file (also done by the destructor)
  const fs::path& FinalPath() const { return final_; }      // valid after Commit()
  bool Committed() const { return committed_; }

 private:
  fs::path desired_, temp_, final_;
  OverwritePolicy policy_;
  bool committed_ = false;
};

// Directory variant for operations that produce many files (PDF->images, split video, extract archive).
class OutputDirTransaction {
 public:
  explicit OutputDirTransaction(const fs::path& desiredDir);   // e.g. "photos (frames)"
  ~OutputDirTransaction();
  const fs::path& TempDir() const { return temp_; }
  bool Commit(std::wstring* error = nullptr);                 // renames temp dir to a unique final name
  const fs::path& FinalDir() const { return final_; }
 private:
  fs::path desired_, temp_, final_;
  bool committed_ = false;
};

} // namespace filo

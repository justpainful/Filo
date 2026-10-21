#include "core/output.h"
#include "core/log.h"
#include <shlobj.h>
#include <rpc.h>
#include <fstream>

namespace filo {

static std::wstring ShortId() {
  UUID u; UuidCreate(&u);
  wchar_t b[16]; swprintf_s(b, L"%08x", u.Data1);
  return b;
}

bool IsDirectoryWritable(const fs::path& dir) {
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) return false;
  fs::path probe = dir / (L".filo-probe-" + ShortId());
  HANDLE h = CreateFileW(LongPath(probe).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  CloseHandle(h);
  return true;
}

fs::path ResolveOutputDir(const fs::path& input, const Settings& s, bool* fellBack) {
  if (fellBack) *fellBack = false;
  fs::path dir;
  if (s.outputLocation == OutputLocation::FixedFolder && !s.fixedOutputFolder.empty()) dir = s.fixedOutputFolder;
  else dir = input.parent_path();
  if (!dir.empty() && IsDirectoryWritable(dir)) return dir;
  PWSTR docs = nullptr; fs::path fb;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_CREATE, nullptr, &docs))) fb = fs::path(docs) / L"Filo Output";
  CoTaskMemFree(docs);
  std::error_code ec; fs::create_directories(fb, ec);
  if (fellBack) *fellBack = true;
  FILO_LOG(Info, "output", "output folder not writable; using the Documents fallback");
  return fb;
}

fs::path DesiredOutputPath(const fs::path& input, const std::wstring& outExt, const Settings& s, const std::wstring& stemSuffix) {
  fs::path dir = ResolveOutputDir(input, s);
  std::wstring name = input.stem().wstring() + s.filenameSuffix + stemSuffix + L"." + outExt;
  return dir / name;
}

OutputTransaction::OutputTransaction(const fs::path& desiredFinal, OverwritePolicy policy) : desired_(desiredFinal), policy_(policy) {
  std::wstring ext = desired_.extension().wstring();
  temp_ = desired_.parent_path() / (L".filo-" + ShortId() + L".tmp" + ext);
}
OutputTransaction::~OutputTransaction() { if (!committed_) Rollback(); }

void OutputTransaction::Rollback() {
  DeleteFileW(LongPath(temp_).c_str());
}

bool OutputTransaction::Commit(std::wstring* error, const std::function<bool(const fs::path&, std::wstring*)>& verify) {
  auto fail = [&](const std::wstring& m) { if (error) *error = m; Rollback(); return false; };
  WIN32_FILE_ATTRIBUTE_DATA fad{};
  if (!GetFileAttributesExW(LongPath(temp_).c_str(), GetFileExInfoStandard, &fad)) return fail(L"The output file was not created.");
  if (fad.nFileSizeLow == 0 && fad.nFileSizeHigh == 0) return fail(L"The output file is empty.");
  if (verify) { std::wstring why; if (!verify(temp_, &why)) return fail(why.empty() ? L"The output could not be verified." : why); }
  fs::path target = policy_ == OverwritePolicy::UniqueName ? UniquePath(desired_) : desired_;
  // Retry briefly: indexers / antivirus sometimes hold a just-written file.
  for (int attempt = 0; attempt < 20; ++attempt) {
    DWORD flags = policy_ == OverwritePolicy::Overwrite ? MOVEFILE_REPLACE_EXISTING : 0;
    if (MoveFileExW(LongPath(temp_).c_str(), LongPath(target).c_str(), flags | MOVEFILE_COPY_ALLOWED)) { committed_ = true; final_ = target; return true; }
    DWORD err = GetLastError();
    if (err == ERROR_ALREADY_EXISTS || err == ERROR_FILE_EXISTS) { target = UniquePath(desired_); continue; }   // lost a race
    if (err == ERROR_SHARING_VIOLATION || err == ERROR_ACCESS_DENIED) { Sleep(100); continue; }
    return fail(L"Could not save the result: " + LastErrorMessage(err));
  }
  return fail(L"Could not save the result (the file is locked).");
}

OutputDirTransaction::OutputDirTransaction(const fs::path& desiredDir) : desired_(desiredDir) {
  temp_ = desired_.parent_path() / (L".filo-" + ShortId() + L".tmpdir");
  std::error_code ec; fs::create_directories(temp_, ec);
}
OutputDirTransaction::~OutputDirTransaction() { if (!committed_) { std::error_code ec; fs::remove_all(temp_, ec); } }
bool OutputDirTransaction::Commit(std::wstring* error) {
  std::error_code ec;
  bool any = false;
  for (auto& e : fs::directory_iterator(temp_, ec)) { (void)e; any = true; break; }
  if (!any) { if (error) *error = L"No files were produced."; return false; }
  fs::path target = UniquePath(desired_);
  for (int i = 0; i < 20; ++i) {
    if (MoveFileExW(LongPath(temp_).c_str(), LongPath(target).c_str(), 0)) { committed_ = true; final_ = target; return true; }
    DWORD err = GetLastError();
    if (err == ERROR_ALREADY_EXISTS) { target = UniquePath(desired_); continue; }
    Sleep(100);
  }
  if (error) *error = L"Could not save the output folder.";
  return false;
}

} // namespace filo

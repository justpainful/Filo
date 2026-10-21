#include "shell/actions.h"
#include "core/com.h"
#include "core/log.h"
#include "core/settings.h"
#include "registry/selection.h"
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <algorithm>

// Share UI via the WinRT interop (documented: IDataTransferManagerInterop) using C++/WinRT headers from the SDK.
#include <unknwn.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <shobjidl_core.h>
#include <windows.applicationmodel.datatransfer.h>
#include <ShlObj_core.h>

#pragma comment(lib, "windowsapp.lib")

namespace filo::shell {

namespace {
HWND g_owner = nullptr;

std::vector<fs::path> ExistingOnly(const std::vector<fs::path>& v) {
  std::vector<fs::path> o;
  std::error_code ec;
  for (auto& p : v) if (fs::exists(p, ec)) o.push_back(p);
  return o;
}

bool MakeDataObject(const std::vector<fs::path>& files, ComPtr<IDataObject>& out) {
  std::vector<PIDLIST_ABSOLUTE> pidls;
  for (auto& f : files) {
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (SUCCEEDED(SHParseDisplayName(f.c_str(), nullptr, &pidl, 0, nullptr)) && pidl) pidls.push_back(pidl);
  }
  if (pidls.empty()) return false;
  ComPtr<IShellItemArray> arr;
  HRESULT hr = SHCreateShellItemArrayFromIDLists((UINT)pidls.size(), (PCIDLIST_ABSOLUTE_ARRAY)pidls.data(), &arr);
  for (auto p : pidls) ILFree(p);
  if (FAILED(hr)) return false;
  return SUCCEEDED(arr->BindToHandler(nullptr, BHID_DataObject, __uuidof(IDataObject), out.AsVoid()));
}
} // namespace

bool CopyFilesToClipboard(const std::vector<fs::path>& files0, std::wstring* err) {
  auto files = ExistingOnly(files0);
  if (files.empty()) { if (err) *err = L"The files are no longer available."; return false; }
  ComPtr<IDataObject> d;
  if (!MakeDataObject(files, d)) { if (err) *err = L"Could not prepare the files for the clipboard."; return false; }
  // "Preferred DropEffect" = copy, so Explorer's Paste copies instead of moving.
  UINT fmt = RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
  HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
  if (h) {
    *static_cast<DWORD*>(GlobalLock(h)) = DROPEFFECT_COPY; GlobalUnlock(h);
    FORMATETC f{(CLIPFORMAT)fmt, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM m{}; m.tymed = TYMED_HGLOBAL; m.hGlobal = h;
    if (FAILED(d->SetData(&f, &m, TRUE))) GlobalFree(h);
  }
  HRESULT hr = OleSetClipboard(d.Get());
  if (FAILED(hr)) { if (err) *err = L"The clipboard is busy. Try again."; return false; }
  OleFlushClipboard();      // keeps the data available after we exit
  return true;
}

bool CopyPathsToClipboard(const std::vector<fs::path>& files, bool quoted) {
  std::wstring text;
  for (size_t i = 0; i < files.size(); ++i) {
    if (i) text += L"\r\n";
    if (quoted) text += L"\"" + files[i].wstring() + L"\""; else text += files[i].wstring();
  }
  for (int attempt = 0; attempt < 10; ++attempt) {
    if (OpenClipboard(g_owner)) {
      EmptyClipboard();
      HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t));
      if (h) { memcpy(GlobalLock(h), text.c_str(), (text.size() + 1) * sizeof(wchar_t)); GlobalUnlock(h); SetClipboardData(CF_UNICODETEXT, h); }
      CloseClipboard();
      return h != nullptr;
    }
    Sleep(30);
  }
  return false;
}

bool OpenTerminalAt(const fs::path& target, const std::wstring& whichIn, std::wstring* err) {
  std::wstring which = whichIn;
  std::error_code ec;
  fs::path dir = fs::is_directory(target, ec) ? target : target.parent_path();
  if (dir.empty() || !fs::is_directory(dir, ec)) { if (err) *err = L"The folder is not available."; return false; }
  auto launch = [&](const fs::path& exe, const std::wstring& args) {
    std::wstring cmd = QuoteArg(exe.wstring()) + (args.empty() ? L"" : L" " + args);
    STARTUPINFOW si{sizeof si}; PROCESS_INFORMATION pi{};
    std::wstring cwd = dir.wstring();
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE, nullptr, cwd.c_str(), &si, &pi)) return false;
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return true;
  };
  auto find = [](const wchar_t* name) { wchar_t buf[MAX_PATH * 2]; return SearchPathW(nullptr, name, nullptr, (DWORD)std::size(buf), buf, nullptr) ? fs::path(buf) : fs::path(); };
  if (which == L"wt" || which.empty()) {
    fs::path wt = find(L"wt.exe");
    if (!wt.empty()) {
      // `-d` takes the start directory as a single argument; QuoteArg handles spaces/quotes safely.
      std::wstring cmd = QuoteArg(wt.wstring()) + L" -d " + QuoteArg(dir.wstring());
      STARTUPINFOW si{sizeof si}; PROCESS_INFORMATION pi{};
      if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) { CloseHandle(pi.hProcess); CloseHandle(pi.hThread); return true; }
    }
    which = L"powershell";
  }
  if (which == L"powershell") {
    fs::path ps = find(L"pwsh.exe"); if (ps.empty()) ps = find(L"powershell.exe");
    if (!ps.empty() && launch(ps, L"-NoLogo -NoExit")) return true;
    which = L"cmd";
  }
  fs::path cmd = find(L"cmd.exe");
  if (!cmd.empty() && launch(cmd, L"")) return true;
  if (err) *err = L"No terminal could be started.";
  return false;
}

bool MoveFilesInteractive(const std::vector<fs::path>& files0, HWND owner, std::wstring* err) {
  auto files = ExistingOnly(files0);
  if (files.empty()) { if (err) *err = L"The files are no longer available."; return false; }
  ComPtr<IFileOpenDialog> dlg;
  if (FAILED(CoCreate(CLSID_FileOpenDialog, dlg))) return false;
  DWORD opts = 0; dlg->GetOptions(&opts);
  dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
  dlg->SetTitle(L"Move to...");
  if (FAILED(dlg->Show(owner))) return false;           // cancelled: nothing happens
  ComPtr<IShellItem> dest;
  if (FAILED(dlg->GetResult(&dest))) return false;
  ComPtr<IFileOperation> op;
  if (FAILED(CoCreate(CLSID_FileOperation, op))) { if (err) *err = L"The Windows file service is unavailable."; return false; }
  op->SetOperationFlags(FOF_ALLOWUNDO | FOFX_ADDUNDORECORD | FOFX_SHOWELEVATIONPROMPT | FOFX_NOMINIMIZEBOX);
  if (owner) op->SetOwnerWindow(owner);
  int queued = 0;
  for (auto& f : files) {
    ComPtr<IShellItem> src;
    if (SUCCEEDED(SHCreateItemFromParsingName(f.c_str(), nullptr, __uuidof(IShellItem), src.AsVoid()))) {
      if (SUCCEEDED(op->MoveItem(src.Get(), dest.Get(), nullptr, nullptr))) ++queued;
    }
  }
  if (!queued) { if (err) *err = L"Nothing could be moved."; return false; }
  HRESULT hr = op->PerformOperations();     // Windows shows progress, resolves name conflicts, handles cross-volume moves
  BOOL aborted = FALSE; op->GetAnyOperationsAborted(&aborted);
  if (FAILED(hr) && !aborted) { if (err) *err = L"The move failed: " + HResultMessage(hr); return false; }
  return !aborted;
}

bool ShareFiles(const std::vector<fs::path>& files0, HWND owner, std::wstring* err) {
  auto files = ExistingOnly(files0);
  if (files.empty()) { if (err) *err = L"The files are no longer available."; return false; }
  try {
    using namespace winrt::Windows::ApplicationModel::DataTransfer;
    auto interop = winrt::get_activation_factory<DataTransferManager, IDataTransferManagerInterop>();
    winrt::com_ptr<ABI::Windows::ApplicationModel::DataTransfer::IDataTransferManager> mgr;
    winrt::check_hresult(interop->GetForWindow(owner, winrt::guid_of<ABI::Windows::ApplicationModel::DataTransfer::IDataTransferManager>(), mgr.put_void()));
    DataTransferManager dtm{nullptr};
    winrt::copy_from_abi(dtm, mgr.get());
    std::vector<std::wstring> paths; for (auto& f : files) paths.push_back(f.wstring());
    auto token = std::make_shared<winrt::event_token>();
    *token = dtm.DataRequested([paths, dtm, token](DataTransferManager const&, DataRequestedEventArgs const& args) {
      auto deferral = args.Request().GetDeferral();
      try {
        std::vector<winrt::Windows::Foundation::IInspectable> items;
        auto list = winrt::single_threaded_vector<winrt::Windows::Storage::IStorageItem>();
        for (auto& p : paths) {
          auto f = winrt::Windows::Storage::StorageFile::GetFileFromPathAsync(p).get();
          list.Append(f);
        }
        args.Request().Data().Properties().Title(L"Share from Filo");
        args.Request().Data().SetStorageItems(list);
      } catch (...) { args.Request().FailWithDisplayText(L"These files cannot be shared."); }
      deferral.Complete();
      dtm.DataRequested(*token);
    });
    winrt::check_hresult(interop->ShowShareUIForWindow(owner));
    return true;
  } catch (winrt::hresult_error const& e) {
    if (err) *err = L"The Windows Share panel could not be opened: " + std::wstring(e.message());
    return false;
  }
}

void RevealInExplorer(const fs::path& file) {
  PIDLIST_ABSOLUTE pidl = nullptr;
  if (SUCCEEDED(SHParseDisplayName(file.c_str(), nullptr, &pidl, 0, nullptr)) && pidl) {
    SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
    ILFree(pidl);
  }
}

void RegisterShellActions(HWND owner) {
  g_owner = owner;
  auto& reg = ToolRegistry::Get();
  auto anyFiles = [](const Selection& s, std::wstring* why) { if (s.empty()) { if (why) *why = L"Nothing selected."; return false; } return true; };

  reg.AddAction({"share", L"Share", "share", anyFiles, [](const Selection& s) {
    std::vector<fs::path> p; for (auto& i : s.items) p.push_back(i.path);
    std::wstring err; if (!ShareFiles(p, g_owner, &err)) MessageBoxW(g_owner, err.c_str(), L"Filo", MB_OK | MB_ICONINFORMATION);
  }});
  reg.AddAction({"copy", L"Copy", "copy", anyFiles, [](const Selection& s) {
    std::vector<fs::path> p; for (auto& i : s.items) p.push_back(i.path);
    std::wstring err; if (!CopyFilesToClipboard(p, &err)) MessageBoxW(g_owner, err.c_str(), L"Filo", MB_OK | MB_ICONWARNING);
  }});
  reg.AddAction({"move", L"Move", "move", anyFiles, [](const Selection& s) {
    std::vector<fs::path> p; for (auto& i : s.items) p.push_back(i.path);
    std::wstring err; if (!MoveFilesInteractive(p, g_owner, &err) && !err.empty()) MessageBoxW(g_owner, err.c_str(), L"Filo", MB_OK | MB_ICONWARNING);
  }});
  reg.AddAction({"copypath", L"Copy Path", "copypath", anyFiles, [](const Selection& s) {
    std::vector<fs::path> p; for (auto& i : s.items) p.push_back(i.path);
    bool quoted = (GetKeyState(VK_SHIFT) & 0x8000) != 0;      // hold Shift while choosing to get quoted paths
    CopyPathsToClipboard(p, quoted);
  }});
  reg.AddAction({"terminal", L"Terminal", "terminal", anyFiles, [](const Selection& s) {
    if (s.empty()) return;
    std::wstring err; Settings st = SettingsStore::Get().Snapshot();
    if (!OpenTerminalAt(s.items[0].path, st.terminal, &err)) MessageBoxW(g_owner, err.c_str(), L"Filo", MB_OK | MB_ICONWARNING);
  }});
}

} // namespace filo::shell

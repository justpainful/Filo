// filo-cli: headless harness for the conversion engines (integration tests and diagnostics). Never shows a window.
//   filo-cli list [--all]                              capabilities registered on this PC (--all includes non-runnable ones)
//   filo-cli convert <input> <outFormat> [k=v ...]     runs one conversion through the registry (temp -> verify -> atomic commit)
// Options are passed to the engine as JSON (numbers and true/false are recognised): e.g.  filo-cli convert a.pdf docx ocr=false
#include "core/util.h"
#include "core/job.h"
#include "core/json.h"
#include "core/settings.h"
#include "registry/capability.h"
#include "registry/filetype.h"
#include "engines/engines.h"
#include "engines/web/web.h"
#include <objbase.h>
#include <cstdio>
#include <string>

using namespace filo;

static int Usage() {
  std::puts("usage: filo-cli list [--all]\n       filo-cli convert <input> <outFormat> [key=value ...]\n"
            "       filo-cli probe <url>\n       filo-cli download <url> [--out dir] [--items 0,1|all] [--audio|--audio-only] [--compat] [--folder N] [--text]\n"
            "       filo-cli account <url> [--list] [--limit N] [--out dir] [--no-videos] [--no-pictures] [--no-text] [--redo] [--parallel N] [--reposts]\n"
            "                              [--cookies firefox|chrome|edge|<cookies.txt>]");
  return 2;
}

int wmain(int argc, wchar_t** argv) {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  SettingsStore::Get().Load();
  RegisterAllEngines();
  if (argc < 2) return Usage();
  const std::wstring cmd = argv[1];
  if (cmd == L"list") {
    const bool all = argc > 2 && std::wstring(argv[2]) == L"--all";
    int n = 0;
    for (const Capability& c : CapabilityRegistry::Get().All()) {
      if (!all && !c.Runnable()) continue;
      std::printf("%s\t%s\t%s\n", Narrow(c.in).c_str(), Narrow(c.out).c_str(), c.Runnable() ? "runnable" : "not runnable here");
      ++n;
    }
    std::printf("%d capabilities\n", n);
    return 0;
  }
  if (cmd == L"convert" && argc >= 4) {
    fs::path input = fs::absolute(argv[2]);
    const std::wstring out = argv[3];
    FileKind kind = DetectFile(input);
    const Capability* cap = CapabilityRegistry::Get().Find(kind.format, out);
    if (!cap) { std::printf("{\"ok\":false,\"error\":\"no conversion from %s to %s\"}\n", Narrow(kind.format).c_str(), Narrow(out).c_str()); return 1; }
    if (!cap->Runnable()) { std::printf("{\"ok\":false,\"error\":\"%s to %s is not available on this PC\"}\n", Narrow(kind.format).c_str(), Narrow(out).c_str()); return 1; }
    Json opts = Json::MakeObject();
    for (int i = 4; i < argc; ++i) {
      std::string kv = Narrow(argv[i]); const size_t eq = kv.find('=');
      if (eq == std::string::npos) continue;
      const std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
      char* end = nullptr; const double d = strtod(v.c_str(), &end);
      if (v == "true" || v == "false") opts[k] = (v == "true");
      else if (end && *end == 0 && !v.empty()) opts[k] = d;
      else opts[k] = v;
    }
    Capability capCopy = *cap;
    JobSpec spec; spec.operation = L"convert"; spec.title = input.filename().wstring(); spec.inputs = {input};
    spec.work = [capCopy, input, opts](JobContext& ctx) { return RunConversion(capCopy, input, opts, ctx); };
    const uint64_t id = JobManager::Get().Submit(std::move(spec));
    JobManager::Get().WaitFor(id, 30 * 60 * 1000);
    auto info = JobManager::Get().Info(id);
    Json j = Json::MakeObject();
    const bool ok = info && info->state == JobState::Completed;
    j["ok"] = ok;
    j["error"] = Narrow(info ? info->result.error : std::wstring(L"timed out"));
    Json outs = Json::MakeArray(); if (info) for (auto& o : info->result.outputs) outs.Push(Narrow(o.wstring())); j["outputs"] = outs;
    Json ws = Json::MakeArray(); if (info) for (auto& w : info->result.warnings) ws.Push(Narrow(w)); j["warnings"] = ws;
    std::printf("%s\n", j.Dump().c_str());
    return ok ? 0 : 1;
  }
  if ((cmd == L"probe" || cmd == L"download") && argc >= 3) {
    // filo-cli probe <url>
    // filo-cli download <url> [--out dir] [--items 1,2,3|all] [--audio] [--audio-only] [--compat] [--folder N]
    const std::wstring url = web::ExtractUrl(argv[2]);
    web::LinkInfo li = web::Probe(url.empty() ? std::wstring(argv[2]) : url);
    Json j = Json::MakeObject();
    j["ok"] = li.ok; j["error"] = Narrow(li.error); j["platform"] = Narrow(li.platform.name); j["title"] = Narrow(li.title);
    j["uploader"] = Narrow(li.uploader); j["baseName"] = Narrow(web::BaseName(li));
    Json items = Json::MakeArray();
    for (size_t i = 0; i < li.items.size(); ++i) {
      const web::Item& it = li.items[i];
      Json o = Json::MakeObject();
      o["i"] = (int)i; o["kind"] = it.kind == web::ItemKind::Video ? "video" : (it.kind == web::ItemKind::Image ? "image" : "audio");
      o["ext"] = it.ext; o["w"] = it.width; o["h"] = it.height; o["duration"] = it.duration; o["size"] = it.size; o["detail"] = Narrow(it.detail);
      items.Push(o);
    }
    j["items"] = items;
    if (cmd == L"probe" || !li.ok) { std::printf("%s\n", j.Dump(true).c_str()); return li.ok ? 0 : 1; }

    web::DownloadRequest req; req.info = li; req.destRoot = web::DefaultDownloadsDir(); req.folderThreshold = 4;
    bool all = true, audioOnly = false;
    for (int i = 3; i < argc; ++i) {
      const std::wstring a = argv[i];
      if (a == L"--out" && i + 1 < argc) req.destRoot = fs::absolute(argv[++i]);
      else if (a == L"--items" && i + 1 < argc) {
        const std::wstring v = argv[++i];
        if (v != L"all") { all = false; for (auto& p : Split(v, L',')) if (!p.empty()) req.items.push_back(std::stoi(p)); }
      }
      else if (a == L"--audio") req.wantAudio = true;
      else if (a == L"--audio-only") { req.wantAudio = true; audioOnly = true; }
      else if (a == L"--audio-format" && i + 1 < argc) {
        const std::wstring format = ToLower(argv[++i]);
        if (format == L"mp3") req.audioFormat = web::AudioFormat::Mp3;
        else if (format == L"m4a") req.audioFormat = web::AudioFormat::M4a;
        else if (format == L"wav") req.audioFormat = web::AudioFormat::Wav;
        else if (format != L"original") { std::fprintf(stderr, "Unsupported audio format.\n"); return 2; }
      }
      else if (a == L"--compat") req.quality = web::Quality::Compatible;
      else if (a == L"--video-format" && i + 1 < argc) {
        const std::wstring format = ToLower(argv[++i]);
        if (format == L"mp4") req.videoContainer = web::VideoContainer::Mp4;
        else if (format == L"mkv") req.videoContainer = web::VideoContainer::Mkv;
        else if (format == L"webm") req.videoContainer = web::VideoContainer::Webm;
        else if (format != L"auto") { std::fprintf(stderr, "Unsupported video format.\n"); return 2; }
      }
      else if (a == L"--folder" && i + 1 < argc) req.folderThreshold = std::stoi(argv[++i]);
      else if (a == L"--text") req.saveText = true;
    }
    if (all && !audioOnly) for (size_t i = 0; i < li.items.size(); ++i) req.items.push_back((int)i);
    if (audioOnly) { req.items.clear(); for (size_t i = 0; i < li.items.size(); ++i) if (li.items[i].kind == web::ItemKind::Audio) req.items.push_back((int)i); }
    JobSpec spec; spec.operation = L"download"; spec.title = url;
    spec.work = [req](JobContext& ctx) { return web::Download(req, ctx); };
    const uint64_t id = JobManager::Get().Submit(std::move(spec));
    JobManager::Get().WaitFor(id, 60 * 60 * 1000);
    auto info = JobManager::Get().Info(id);
    const bool ok = info && info->state == JobState::Completed;
    j["downloaded"] = ok;
    j["downloadError"] = Narrow(info ? info->result.error : std::wstring(L"timed out"));
    Json outs = Json::MakeArray(); if (info) for (auto& o : info->result.outputs) outs.Push(Narrow(o.wstring())); j["outputs"] = outs;
    std::printf("%s\n", j.Dump(true).c_str());
    return ok ? 0 : 1;
  }
  if (cmd == L"account" && argc >= 3) {
    // filo-cli account <url> [--list] [--limit N] [--out dir] [--no-videos] [--no-pictures] [--no-text] [--redo] [--parallel N] [--reposts] [--cookies ...]
    const std::wstring url = web::ExtractUrl(argv[2]).empty() ? std::wstring(argv[2]) : web::ExtractUrl(argv[2]);
    const Settings st = SettingsStore::Get().Snapshot();
    web::AccountDownloadRequest req; req.destRoot = web::DefaultDownloadsDir(); req.folderThreshold = st.downloadFolderThreshold; req.parallel = st.downloadParallel;
    web::ListOptions lo; bool listOnly = false;
    web::SetCookies(web::CookieSource(st.downloadCookies), st.downloadCookiesFile);
    for (int i = 3; i < argc; ++i) {
      const std::wstring a = argv[i];
      if (a == L"--list") listOnly = true;
      else if (a == L"--limit" && i + 1 < argc) lo.limit = std::stoi(argv[++i]);
      else if (a == L"--out" && i + 1 < argc) req.destRoot = fs::absolute(argv[++i]);
      else if (a == L"--no-videos") req.videos = false;
      else if (a == L"--no-pictures") req.pictures = false;
      else if (a == L"--no-text") req.text = false;
      else if (a == L"--redo") req.skipDone = false;
      else if (a == L"--parallel" && i + 1 < argc) req.parallel = std::stoi(argv[++i]);
      else if (a == L"--reposts") lo.reposts = true;
      else if (a == L"--folder" && i + 1 < argc) req.folderThreshold = std::stoi(argv[++i]);
      else if (a == L"--cookies" && i + 1 < argc) {
        const std::wstring c = ToLower(argv[++i]);
        if (c == L"firefox") web::SetCookies(web::CookieSource::Firefox);
        else if (c == L"chrome") web::SetCookies(web::CookieSource::Chrome);
        else if (c == L"edge") web::SetCookies(web::CookieSource::Edge);
        else web::SetCookies(web::CookieSource::File, fs::absolute(argv[i]));
      }
    }
    web::AccountInfo ai = web::ListAccount(url, lo, nullptr, nullptr);
    Json j = Json::MakeObject();
    j["ok"] = ai.ok; j["error"] = Narrow(ai.error); j["platform"] = Narrow(ai.platform.name); j["handle"] = Narrow(ai.handle); j["name"] = Narrow(ai.name);
    j["posts"] = (int)ai.entries.size(); j["needsLogin"] = ai.needsLogin; j["capped"] = ai.capped;
    Json first = Json::MakeArray();
    for (size_t i = 0; i < ai.entries.size() && i < 8; ++i) {
      const web::AccountEntry& e = ai.entries[i];
      Json o = Json::MakeObject();
      o["id"] = e.id; o["url"] = e.url; o["title"] = Narrow(e.title); o["when"] = Narrow(web::FormatUtcTime(e.timestamp)); o["media"] = (int)e.media.size(); o["hasMedia"] = e.hasMedia;
      first.Push(o);
    }
    j["first"] = first;
    if (listOnly || !ai.ok) { std::printf("%s\n", j.Dump(true).c_str()); return ai.ok ? 0 : 1; }
    for (size_t i = 0; i < ai.entries.size(); ++i) req.entries.push_back((int)i);
    req.account = ai;
    JobSpec spec; spec.operation = L"account"; spec.title = url;
    spec.work = [req](JobContext& ctx) { return web::DownloadAccount(req, ctx); };
    const uint64_t id = JobManager::Get().Submit(std::move(spec));
    JobManager::Get().WaitFor(id, 6 * 60 * 60 * 1000);
    auto info = JobManager::Get().Info(id);
    const bool ok = info && info->state == JobState::Completed;
    j["downloaded"] = ok;
    j["downloadError"] = Narrow(info ? info->result.error : std::wstring(L"timed out"));
    Json outs = Json::MakeArray(); if (info) for (auto& o : info->result.outputs) outs.Push(Narrow(o.wstring())); j["outputs"] = outs;
    Json ws = Json::MakeArray(); if (info) for (auto& w : info->result.warnings) ws.Push(Narrow(w)); j["warnings"] = ws;
    std::printf("%s\n", j.Dump(true).c_str());
    return ok ? 0 : 1;
  }
  return Usage();
}

#include "engines/pdf/pdf_ocr.h"
#include "engines/pdf/pdf_wic.h"
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Storage.Streams.h>
#include <algorithm>
#include <exception>
#include <cwctype>
#include <thread>

namespace wo = winrt::Windows::Media::Ocr;
namespace wg = winrt::Windows::Graphics::Imaging;
namespace ws = winrt::Windows::Storage::Streams;

namespace filo::pdf {
namespace {

// All WinRT work happens on a dedicated MTA thread (blocking .get() is not allowed on STA / UI threads).
template <class F> bool OnMtaThread(F&& fn, std::wstring* err) {
  bool ok = false;
  std::wstring e;
  std::thread t([&] {
    try {
      winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (...) {}
    try { ok = fn(e); }
    catch (const winrt::hresult_error& ex) { e = L"Text recognition failed: " + std::wstring(ex.message().c_str()); ok = false; }
    catch (const std::exception&) { e = L"Text recognition failed."; ok = false; }
  });
  t.join();
  if (!ok && err) *err = e.empty() ? L"Text recognition failed." : e;
  return ok;
}

int CountChars(const OcrResult& r) {
  int n = 0;
  for (auto& l : r.lines) for (wchar_t c : l.text) if (!iswspace(c)) ++n;
  return n;
}

bool RunEngine(wo::OcrEngine const& eng, const wg::SoftwareBitmap& bmp, double scaleBack, OcrResult& out) {
  auto res = eng.RecognizeAsync(bmp).get();
  out.lines.clear();
  out.language = eng.RecognizerLanguage().LanguageTag().c_str();
  for (auto const& line : res.Lines()) {
    OcrLineOut l;
    l.text = line.Text().c_str();
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    struct W { std::wstring t; float cx; };
    std::vector<W> words;
    int rtlLetters = 0, otherLetters = 0;
    for (auto const& w : line.Words()) {
      auto r = w.BoundingRect();
      x0 = std::min(x0, r.X); y0 = std::min(y0, r.Y); x1 = std::max(x1, r.X + r.Width); y1 = std::max(y1, r.Y + r.Height);
      std::wstring wt = w.Text().c_str();
      for (wchar_t c : wt) {
        if ((c >= 0x0590 && c <= 0x08FF) || (c >= 0xFB1D && c <= 0xFDFF) || (c >= 0xFE70 && c <= 0xFEFF)) ++rtlLetters;
        else if (iswalpha(c)) ++otherLetters;
      }
      words.push_back({std::move(wt), r.X + r.Width * 0.5f});
    }
    // Windows OCR lists the words of a right-to-left line in visual (left-to-right) order; rebuild the logical order
    if (rtlLetters > otherLetters && words.size() > 1) {
      std::stable_sort(words.begin(), words.end(), [](const W& a, const W& b) { return a.cx > b.cx; });
      std::wstring t;
      for (size_t i = 0; i < words.size(); ++i) { if (i) t += L' '; t += words[i].t; }
      l.text = std::move(t);
    }
    if (x1 > x0) { l.x = (float)(x0 * scaleBack); l.y = (float)(y0 * scaleBack); l.w = (float)((x1 - x0) * scaleBack); l.h = (float)((y1 - y0) * scaleBack); }
    if (!l.text.empty()) out.lines.push_back(std::move(l));
  }
  out.chars = CountChars(out);
  return true;
}

} // namespace

OcrInfo QueryOcr() {
  OcrInfo info;
  OnMtaThread([&](std::wstring&) {
    for (auto const& l : wo::OcrEngine::AvailableRecognizerLanguages()) info.languages.push_back(l.LanguageTag().c_str());
    info.maxImageDimension = (int)wo::OcrEngine::MaxImageDimension();
    if (auto e = wo::OcrEngine::TryCreateFromUserProfileLanguages()) info.profileLanguage = e.RecognizerLanguage().LanguageTag().c_str();
    return true;
  }, nullptr);
  info.available = !info.languages.empty();
  if (!info.available)
    info.message = L"No text recognition (OCR) language is installed. Add a language with OCR support in Windows Settings > Time & language > Language & region.";
  return info;
}

bool OcrRecognize(const uint8_t* bgra, int w, int h, const std::wstring& lang, OcrResult& out, std::wstring* err) {
  return OnMtaThread([&](std::wstring& e) {
    int maxDim = (int)wo::OcrEngine::MaxImageDimension();
    double scale = 1.0;
    std::vector<uint8_t> scaled;
    const uint8_t* px = bgra; int pw = w, ph = h;
    if (w > maxDim || h > maxDim) {
      scale = (double)maxDim / std::max(w, h);
      pw = std::max(1, (int)(w * scale)); ph = std::max(1, (int)(h * scale));
      scaled = wic::Downscale(bgra, w, h, pw, ph);
      px = scaled.data();
    }
    ws::DataWriter dw;
    dw.WriteBytes(winrt::array_view<const uint8_t>(px, px + (size_t)pw * ph * 4));
    auto buf = dw.DetachBuffer();
    auto bmp = wg::SoftwareBitmap::CreateCopyFromBuffer(buf, wg::BitmapPixelFormat::Bgra8, pw, ph, wg::BitmapAlphaMode::Ignore);

    std::vector<wo::OcrEngine> engines;
    if (lang.empty() || lang == L"auto") {
      std::vector<std::wstring> seen;
      auto add = [&](wo::OcrEngine const& en) {
        if (!en) return;
        std::wstring tag = en.RecognizerLanguage().LanguageTag().c_str();
        if (std::find(seen.begin(), seen.end(), tag) != seen.end()) return;
        seen.push_back(tag); engines.push_back(en);
      };
      add(wo::OcrEngine::TryCreateFromUserProfileLanguages());
      for (auto const& l : wo::OcrEngine::AvailableRecognizerLanguages()) add(wo::OcrEngine::TryCreateFromLanguage(l));
    } else {
      auto en = wo::OcrEngine::TryCreateFromLanguage(winrt::Windows::Globalization::Language(lang));
      if (!en) { e = L"The OCR language \"" + lang + L"\" is not installed."; return false; }
      engines.push_back(en);
    }
    if (engines.empty()) {
      e = L"No text recognition (OCR) language is installed. Add a language with OCR support in Windows Settings > Time & language > Language & region.";
      return false;
    }
    bool have = false;
    OcrResult best;
    for (auto& en : engines) {
      OcrResult r;
      RunEngine(en, bmp, 1.0 / scale, r);
      if (!have || r.chars > best.chars) { best = std::move(r); have = true; }
      if (engines.size() > 1 && best.chars > 0 && &en == &engines.front() && lang.size() && lang != L"auto") break;
    }
    out = std::move(best);
    return true;
  }, err);
}

} // namespace filo::pdf

#include "tools/vision.h"
#include <unknwn.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.FaceAnalysis.h>
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Storage.Streams.h>
#include <algorithm>
#include <cstring>

namespace filo::tools {

using namespace winrt::Windows::Graphics::Imaging;
using namespace winrt::Windows::Storage::Streams;

namespace {
SoftwareBitmap ToSoftwareBitmap(const img::Bitmap& bm) {
  const uint32_t size = (uint32_t)((size_t)bm.width * bm.height * 4);
  Buffer buf(size);
  buf.Length(size);
  memcpy(buf.data(), bm.px.data(), size);
  return SoftwareBitmap::CreateCopyFromBuffer(buf, BitmapPixelFormat::Bgra8, bm.width, bm.height, BitmapAlphaMode::Ignore);
}
// Apartment guard: the callers run on plain worker threads.
struct Apartment {
  Apartment() { try { winrt::init_apartment(winrt::apartment_type::multi_threaded); } catch (...) {} }
};
} // namespace

bool DetectFaces(const img::Bitmap& bm, std::vector<VisionBox>& out, std::wstring* err) {
  Apartment apt;
  out.clear();
  try {
    if (bm.Empty()) { if (err) *err = L"No image."; return false; }
    auto detector = winrt::Windows::Media::FaceAnalysis::FaceDetector::CreateAsync().get();
    SoftwareBitmap gray = SoftwareBitmap::Convert(ToSoftwareBitmap(bm), BitmapPixelFormat::Gray8);
    auto faces = detector.DetectFacesAsync(gray).get();
    for (auto const& f : faces) {
      auto b = f.FaceBox();
      const float padX = b.Width * 0.18f, padY = b.Height * 0.22f;     // cover hair line and chin, not just the detector box
      VisionBox v{(float)b.X - padX, (float)b.Y - padY * 1.3f, (float)b.Width + 2 * padX, (float)b.Height + padY * 2.1f};
      v.x = std::max(0.f, v.x); v.y = std::max(0.f, v.y);
      v.w = std::min(v.w, bm.width - v.x); v.h = std::min(v.h, bm.height - v.y);
      out.push_back(v);
    }
    return true;
  } catch (winrt::hresult_error const& e) {
    if (err) *err = L"Face detection is not available on this PC: " + std::wstring(e.message());
    return false;
  }
}

bool DetectTextLines(const img::Bitmap& bm, std::vector<VisionBox>& out, std::wstring* err) {
  Apartment apt;
  out.clear();
  try {
    if (bm.Empty()) { if (err) *err = L"No image."; return false; }
    auto engine = winrt::Windows::Media::Ocr::OcrEngine::TryCreateFromUserProfileLanguages();
    if (!engine) { if (err) *err = L"No text-recognition language is installed (Settings > Time & language > Language & region)."; return false; }
    const uint32_t maxDim = winrt::Windows::Media::Ocr::OcrEngine::MaxImageDimension();
    if ((uint32_t)bm.width > maxDim || (uint32_t)bm.height > maxDim) { if (err) *err = L"The image is too large for text detection."; return false; }
    auto result = engine.RecognizeAsync(ToSoftwareBitmap(bm)).get();
    for (auto const& line : result.Lines()) {
      float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
      for (auto const& w : line.Words()) {
        auto r = w.BoundingRect();
        x0 = std::min(x0, (float)r.X); y0 = std::min(y0, (float)r.Y); x1 = std::max(x1, (float)(r.X + r.Width)); y1 = std::max(y1, (float)(r.Y + r.Height));
      }
      if (x1 > x0 && y1 > y0) out.push_back({x0 - 2, y0 - 2, x1 - x0 + 4, y1 - y0 + 4});
    }
    return true;
  } catch (winrt::hresult_error const& e) {
    if (err) *err = L"Text detection is not available on this PC: " + std::wstring(e.message());
    return false;
  }
}

} // namespace filo::tools

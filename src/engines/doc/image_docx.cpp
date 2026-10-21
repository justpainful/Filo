// Image -> DOCX: one page holding the picture, scaled to fit the page's text area. The picture is decoded with the image engine
// (EXIF orientation applied, colour managed to sRGB) and stored as PNG, or as the original bytes when they are an upright JPEG.
#include "engines/doc/convert_common.h"
#include "engines/doc/docx.h"
#include "engines/image/bitmap.h"
#include <algorithm>

namespace filo::doc {

EngineResult RunImageToDocx(const ConvertTask& t, JobContext& ctx) {
  img::Bitmap bm; std::wstring err;
  img::LoadOptions lo; lo.ctx = &ctx;
  if (!img::LoadBitmap(t.input, bm, lo, &err)) return Fail(err.empty() ? L"The image could not be opened." : err);
  if (ctx.Cancelled()) return Fail(L"Cancelled");
  auto im = std::make_shared<ImageData>();
  Bytes raw;
  const bool isJpeg = bm.info.format == L"jpg" || bm.info.format == L"jpeg";
  if (isJpeg && bm.info.orientation <= 1 && img::ReadFileBytes(t.input, raw, nullptr)) { im->bytes = std::move(raw); im->mime = "image/jpeg"; }
  else {
    img::Scratch tmp;
    const fs::path png = tmp.dir / L"page.png";
    img::SaveOptions so; so.preserveMetadata = false; so.ctx = &ctx;
    img::Bitmap flat = bm;
    if (!bm.info.hasAlpha) img::FlattenOnWhite(flat);
    img::SaveResult sr = img::SaveBitmap(png, flat, L"png", so);
    if (!sr.ok || !img::ReadFileBytes(png, im->bytes, nullptr)) return Fail(sr.ok ? L"The picture could not be prepared." : sr.error);
    im->mime = "image/png";
  }
  im->w = bm.width; im->h = bm.height; im->name = Narrow(t.input.filename().wstring());
  im->alt = Narrow(t.input.stem().wstring());
  // A4 with the writer's default margins: about 451 x 698 pt of text area
  const double maxW = 451.0, maxH = 690.0;
  const double s = std::min(maxW / bm.width, maxH / bm.height);
  im->widthPt = bm.width * s; im->heightPt = bm.height * s;
  Document d;
  d.meta.title = Narrow(t.input.stem().wstring());
  Block b; b.kind = Block::Kind::Image; b.image = im; b.align = 'c';
  d.blocks.push_back(std::move(b));
  DocxWriteOptions wo;
  std::vector<std::wstring> warnings;
  if (!WriteDocx(d, t.output, wo, &err, &warnings)) return Fail(err.empty() ? L"The DOCX file could not be written." : err);
  EngineResult r = EngineResult::Ok();
  AddWarnings(r, warnings);
  if (bm.info.isAnimated) r.warnings.push_back(L"Only the first frame of the animation was used.");
  return r;
}

}  // namespace filo::doc

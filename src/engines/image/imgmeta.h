// Image container parsing and metadata surgery (EXIF / XMP / ICC) for JPEG, PNG, WebP and HEIF/AVIF (ISO-BMFF).
// Everything here works on raw bytes - no codec involved - so metadata can be carried across formats that the
// encoders (WIC, libwebp, libaom) do not carry themselves.
#pragma once
#include "core/util.h"
#include <cstdint>
#include <string>
#include <vector>

namespace filo::img {

using Bytes = std::vector<uint8_t>;

struct Metadata {
  Bytes exif;   // TIFF-structured EXIF payload ("II*\0" / "MM\0*..."), WITHOUT the "Exif\0\0" prefix
  Bytes xmp;    // XMP packet (UTF-8 XML)
  Bytes icc;    // ICC profile
  bool Empty() const { return exif.empty() && xmp.empty() && icc.empty(); }
  bool HasExifOrXmp() const { return !exif.empty() || !xmp.empty(); }
};

// ---- EXIF helpers (TIFF structure) ---------------------------------------------------------------------------------
int  ExifOrientation(const Bytes& exif);                     // 1..8, 0 when absent/invalid
// Rewrites the orientation tag (if present and newOrientation>0), the PixelX/YDimension tags (if present and w,h>0) and
// drops the IFD1 thumbnail link (the thumbnail would keep the old orientation).
void ExifPatch(Bytes& exif, int newOrientation, int pixelW, int pixelH);

// ---- ICC helpers ---------------------------------------------------------------------------------------------------
Bytes SrgbIccProfile();                                      // compact sRGB (v2) profile generated in code (no third-party data)
bool  IccIsSrgb(const Bytes& icc);                           // profile description mentions sRGB
std::string IccColorSpace(const Bytes& icc);                 // "RGB ", "GRAY", "CMYK", "" when invalid
std::wstring IccDescription(const Bytes& icc);               // best-effort profile description

// ---- JPEG ----------------------------------------------------------------------------------------------------------
struct JpegInfo {
  int width = 0, height = 0, components = 0, precision = 8;
  bool progressive = false, arithmetic = false, lossless = false;
  bool adobe = false; int adobeTransform = -1;
  bool jfif = false;
  bool complete = true;                    // an EOI marker follows the scan data (false: truncated file)
};
bool ParseJpeg(const Bytes& file, JpegInfo& info, Metadata& meta);       // false when `file` is not a JPEG stream
// Inserts EXIF/XMP/ICC APPn segments right after SOI/JFIF, removing any that exist. Returns false when nothing fits.
bool InjectJpegMeta(Bytes& file, const Metadata& meta, std::vector<std::wstring>* dropped = nullptr);

// ---- PNG -----------------------------------------------------------------------------------------------------------
struct PngInfo { int width = 0, height = 0, bitDepth = 0, colorType = 0; bool hasSrgbChunk = false, hasTrns = false, animated = false, complete = false; };
bool ParsePng(const Bytes& file, PngInfo& info, Metadata& meta);
uint32_t Crc32(const uint8_t* d, size_t n, uint32_t seed = 0);

// ---- WebP ----------------------------------------------------------------------------------------------------------
struct WebpInfo { int width = 0, height = 0; bool animated = false, hasAlpha = false, lossless = false, complete = true; int frames = 1; };
bool ParseWebp(const Bytes& file, WebpInfo& info, Metadata& meta);
// Rebuilds the RIFF as VP8X with ICCP/EXIF/XMP chunks (replacing existing ones).
bool InjectWebpMeta(Bytes& file, const Metadata& meta);

// ---- HEIF / AVIF (ISO-BMFF) ----------------------------------------------------------------------------------------
struct HeifInfo {
  int width = 0, height = 0;          // primary item size (ispe) BEFORE crop / irot
  int cropWidth = 0, cropHeight = 0;  // clean aperture ('clap') size when the encoder padded the picture (e.g. odd sizes), else 0
  int rotationQuarterTurns = 0;       // irot: anticlockwise quarter turns (0..3)
  int mirror = -1;                    // imir axis (0 = vertical axis / left-right flip, 1 = horizontal axis), -1 none
  bool hasAlphaAux = false;           // an auxiliary alpha plane is present
  bool isSequence = false;            // 'avis'/'hevc' image sequence brands
  int BaseWidth() const { return cropWidth > 0 ? cropWidth : width; }
  int BaseHeight() const { return cropHeight > 0 ? cropHeight : height; }
  int DisplayWidth() const { return (rotationQuarterTurns & 1) ? BaseHeight() : BaseWidth(); }
  int DisplayHeight() const { return (rotationQuarterTurns & 1) ? BaseWidth() : BaseHeight(); }
};
bool ParseHeif(const Bytes& file, HeifInfo& info, Metadata& meta);

} // namespace filo::img

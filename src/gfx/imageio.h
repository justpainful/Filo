// Small WIC helpers for the rendering layer (visual tests, backdrop override, thumbnails). Full image conversion lives
// in engines/image.
#pragma once
#include "gfx/backdrop.h"
#include <filesystem>
#include <vector>

namespace filo::gfx {

// Saves BGRA pixels as PNG. `premultiplied` selects the WIC pixel format (the swap chain is premultiplied).
bool SavePng(const std::filesystem::path& path, const uint8_t* bgra, int w, int h, bool premultiplied);
// Decodes any WIC-supported image to straight BGRA.
bool LoadBgra(const std::filesystem::path& path, std::vector<uint8_t>& bgra, int& w, int& h);
// Builds a BackdropImage (sharp + blurred) from an image file - lets us render the wheel over the reference wallpaper.
bool BackdropFromImage(const std::filesystem::path& path, const RECT& screenRect, float blurSigmaPx, BackdropImage& out);

// Screen capture of a rectangle (physical px, virtual-screen coordinates) to PNG: used by visual tests of real windows.
bool CaptureScreenToPng(const RECT& rect, const std::filesystem::path& path);

} // namespace filo::gfx

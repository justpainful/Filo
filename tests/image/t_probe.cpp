#include "t_util.h"
using namespace tu;
TEST(probe_codecs_and_ffmpeg_capabilities) {
  std::printf("    ffmpeg: %ls | %s\n", ff::FfmpegPath().c_str(), ff::Version().c_str());
  std::printf("    libwebp=%d libwebp_anim=%d libaom=%d avifmux=%d libdav1d=%d hevc=%d\n", ff::HasEncoder("libwebp"), ff::HasEncoder("libwebp_anim"), ff::HasEncoder("libaom-av1"), ff::HasMuxer("avif"), ff::HasDecoder("libdav1d"), ff::HasDecoder("hevc"));
  for (const wchar_t* f : {L"jpg", L"png", L"webp", L"avif", L"heic", L"tiff", L"bmp", L"gif"})
    std::printf("    %ls: encode=%d decode=%d\n", f, (int)img::CanEncode(f), (int)img::CanDecode(f));
}

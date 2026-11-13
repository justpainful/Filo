#include "test.h"
#include "shell/browser_url.h"

using namespace filo::shell;

TEST(address_bar_text_becomes_an_absolute_address) {
  CHECK(NormalizeAddressBarText(L"https://x.com/a/status/1?s=20") == L"https://x.com/a/status/1?s=20");
  CHECK(NormalizeAddressBarText(L"  http://example.com/p  ") == L"http://example.com/p");
  CHECK(NormalizeAddressBarText(L"www.youtube.com/watch?v=6Eu2wkEc5xM") == L"https://www.youtube.com/watch?v=6Eu2wkEc5xM");   // Chrome hides the scheme
  CHECK(NormalizeAddressBarText(L"tiktok.com/@dram_i/video/1") == L"https://tiktok.com/@dram_i/video/1");
}

TEST(address_bar_text_that_is_not_an_address_is_ignored) {
  CHECK(NormalizeAddressBarText(L"").empty());
  CHECK(NormalizeAddressBarText(L"how to bake bread").empty());          // a search phrase
  CHECK(NormalizeAddressBarText(L"bread.recipes for dinner").empty());   // spaces: a search
  CHECK(NormalizeAddressBarText(L"localhost").empty());
  CHECK(NormalizeAddressBarText(L"about:blank").empty());
  CHECK(NormalizeAddressBarText(L"a/b.c").empty());
  CHECK(NormalizeAddressBarText(L"..").empty());
}

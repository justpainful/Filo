#include "design/theme.h"
#include <windows.h>

namespace filo::ds {

static DWORD ReadDword(const wchar_t* key, const wchar_t* name, DWORD def) {
  DWORD v = def, sz = sizeof v;
  if (RegGetValueW(HKEY_CURRENT_USER, key, name, RRF_RT_REG_DWORD, nullptr, &v, &sz) != ERROR_SUCCESS) return def;
  return v;
}

Theme& Theme::Get() { static Theme t; return t; }
Theme::Theme() { RefreshFromSystem(); }

void Theme::RefreshFromSystem() {
  sysDark_ = ReadDword(L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme", 1) == 0;
  sysTransparency_ = ReadDword(L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"EnableTransparency", 1) != 0;
  BOOL anim = TRUE; SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &anim, 0);
  sysAnimations_ = anim != FALSE;
  HIGHCONTRASTW hc{sizeof hc}; SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof hc, &hc, 0);
  st_.highContrast = (hc.dwFlags & HCF_HIGHCONTRASTON) != 0;
  Recompute();
}

void Theme::SetUserState(const ThemeState& s) {
  bool hc = st_.highContrast;
  st_ = s; st_.highContrast = hc;
  Recompute();
}

void Theme::Recompute() {
  switch (st_.mode) {
    case ThemeMode::Light: dark_ = false; break;
    case ThemeMode::Dark: dark_ = true; break;
    default: dark_ = sysDark_; break;
  }
}

} // namespace filo::ds

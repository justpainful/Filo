// Resolves the active palette + accessibility switches (system theme, transparency, reduced motion, high contrast).
#pragma once
#include "design/tokens.h"
#include <string>

namespace filo::ds {

enum class ThemeMode { System, Light, Dark };

struct ThemeState {
  ThemeMode mode = ThemeMode::System;
  std::string iconPack = "classic"; // Stable plugin ID; existing users retain the original icons.
  int glassStyle = 0;             // 0 Liquid Glass, 1 Soft Glass, 2 Windows Acrylic
  float glassReflections = .65f;
  float glassRefraction = 1.f;
  float acrylicTransparency = .85f; // 0 opaque, 1 maximum colour transfer
  float acrylicBlur = 30.f;          // DIP, independently adjustable
  float glassIntensity = 1.0f;      // 0..1, user setting
  bool reduceTransparency = false;  // user setting OR system "Transparency effects" off
  bool reduceMotion = false;        // user setting OR system "Animation effects" off
  bool highContrast = false;
  float wheelScale = 1.0f;          // user setting
};

class Theme {
 public:
  static Theme& Get();
  const Palette& Colors() const { return dark_ ? kDark : kLight; }
  bool IsDark() const { return dark_; }
  const ThemeState& State() const { return st_; }
  void SetUserState(const ThemeState& s);   // call after settings change
  void RefreshFromSystem();                 // call on WM_SETTINGCHANGE
  float MotionScale() const { return (st_.reduceMotion || !sysAnimations_) ? 0.f : 1.f; }
  // Effective glass intensity after accessibility overrides (0 = opaque fallback).
  float EffectiveGlass() const { return (st_.reduceTransparency || !sysTransparency_ || st_.highContrast) ? 0.f : (st_.glassStyle==2?1.f:st_.glassIntensity); }

 private:
  Theme();
  bool dark_ = false;
  ThemeState st_;
  bool sysTransparency_ = true, sysAnimations_ = true;
  bool sysDark_ = false;
  void Recompute();
};

} // namespace filo::ds

// Data-only icon plugins. Paths are D2D geometry, never executable code or SVG documents.
#pragma once
#include "core/json.h"
#include "core/util.h"
#include <unordered_map>

namespace filo::gfx {
struct IconLayer {
  std::string path;
  bool filled = false;
  float opacity = 1.f, stroke = 1.7f;
};
struct IconPack {
  std::string id;
  std::wstring name, description;
  bool round = true;
  std::unordered_map<std::string, std::vector<IconLayer>> icons;
};
// Strict manifest validation; partial packs deliberately inherit Classic icons.
bool ParseIconPack(const Json& manifest, IconPack& out);
class IconPacks {
 public:
  static IconPacks& Get();
  const std::vector<IconPack>& All() const { return packs_; }
  const IconPack& Resolve(std::string_view id) const;
  // Re-read installed manifests only on explicit reload, never during a frame.
  void Reload();
 private:
  IconPacks();
  std::vector<IconPack> packs_;
};
} // namespace filo::gfx

#include "gfx/icon_packs.h"
#include "gfx/icons.h"
#include "core/log.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>

namespace filo::gfx {
bool ParseIconPack(const Json& j, IconPack& out) {
  IconPack p;
  if (!j.IsObject() || !j.Get("schema").IsNumber() || j.Get("schema").AsNumber() != 1) return false;
  p.id=j.Get("id").AsString();
  if (p.id.empty() || p.id.size()>48 || p.id=="classic") return false;
  for (char c:p.id) if (!((c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-')) return false;
  auto name=j.Get("name").AsString(), desc=j.Get("description").AsString();
  if (name.empty()||name.size()>80||desc.size()>240) return false;
  p.name=Widen(name);p.description=Widen(desc);
  p.round=j.Get("round").AsBool(true);
  const auto& icons=j.Get("icons");
  if (!icons.IsObject() || icons.Size()==0 || icons.Size()>128) return false;
  for (const auto& [key, value]:icons.Items()) {
    if (!IconCache::Get().Has(key) || !value.IsArray() || value.Size()==0 || value.Size()>6) return false;
    auto& layers=p.icons[key];
    for (const auto& layer:value.Elements()) {
      IconLayer l;
      if (!layer.IsObject() || !layer.Get("fill").IsBool()) return false;
      l.path=layer.Get("path").AsString();l.filled=layer.Get("fill").AsBool();
      if (l.path.empty()||l.path.size()>8192) return false;
      if (layer.Has("opacity")&&!layer.Get("opacity").IsNumber()) return false;
      if (layer.Has("stroke")&&!layer.Get("stroke").IsNumber()) return false;
      l.opacity=(float)layer.Get("opacity").AsNumber(1);l.stroke=(float)layer.Get("stroke").AsNumber(1.7);
      if (!std::isfinite(l.opacity)||!std::isfinite(l.stroke)||l.opacity<.12f||l.opacity>1||l.stroke<.5f||l.stroke>3.5f) return false;
      // Validate the complete path before publishing a pack, so rendering cannot hang on malformed data.
      auto geo=ParseSvgPath(l.path,l.filled);
      D2D1_RECT_F bounds{};
      if (!geo || FAILED(l.filled ? geo->GetBounds(nullptr,&bounds) : geo->GetWidenedBounds(.01f,nullptr,nullptr,&bounds)) || !std::isfinite(bounds.left)||!std::isfinite(bounds.right)||
          !std::isfinite(bounds.top)||!std::isfinite(bounds.bottom)||bounds.left<-.02f||bounds.top<-.02f||bounds.right>24.02f||bounds.bottom>24.02f||bounds.left>bounds.right||bounds.top>bounds.bottom) return false;
      layers.push_back(std::move(l));
    }
    // Keep a fully opaque structural layer in every glyph for legibility on glass.
    if (std::none_of(layers.begin(),layers.end(),[](const auto& l){return l.opacity==1.f;})) return false;
  }
  out=std::move(p);return true;
}
IconPacks& IconPacks::Get() { static IconPacks p;return p; }
IconPacks::IconPacks() { Reload(); }
const IconPack& IconPacks::Resolve(std::string_view id) const {
  for (const auto& p:packs_) if(p.id==id)return p;
  return packs_.front();
}
void IconPacks::Reload() {
  packs_.clear();packs_.push_back({"classic",L"Classic",L"Original Filo outlines. Light, familiar and unchanged."});
  // Installed packs win duplicate IDs. User packs can extend, never replace Classic or installed IDs.
  for (const auto& dir:{ExeDir()/L"icon-packs",AppDataDir()/L"icon-packs"}) {
    std::error_code ec;std::vector<fs::path> files;
    for(fs::directory_iterator it(dir,ec),end;!ec&&it!=end;it.increment(ec)) {
      if(it->is_regular_file(ec)&&it->path().extension()==L".json")files.push_back(it->path());
    }
    std::sort(files.begin(),files.end());
    for(const auto& file:files) {
      if(packs_.size()>=33)break;
      auto bytes=fs::file_size(file,ec);if(ec||bytes>512*1024)continue;
      std::ifstream f(file,std::ios::binary);std::string data((std::istreambuf_iterator<char>(f)),{});
      auto j=Json::Parse(data);IconPack p;
      if(!j||!ParseIconPack(*j,p))continue;
      if(std::any_of(packs_.begin(),packs_.end(),[&](const auto& other){return other.id==p.id;}))continue;
      packs_.push_back(std::move(p));
    }
  }
  IconCache::Get().Clear();
}
} // namespace filo::gfx

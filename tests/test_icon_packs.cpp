#include "test.h"
#include "gfx/icon_packs.h"
#include "gfx/icons.h"
#include "core/settings.h"
#include <fstream>
#include <iterator>
using namespace filo;

static Json SamplePack() {
  auto j=Json::MakeObject();j["schema"]=1;j["id"]="test-pack";j["name"]="Test";
  j["icons"]=Json::MakeObject();auto layers=Json::MakeArray();auto l=Json::MakeObject();
  l["path"]="M4 4h16v16H4z";l["fill"]=true;layers.Push(l);j["icons"]["image"]=layers;return j;
}
TEST(icon_plugins_ship_three_complete_packs_and_preserve_classic) {
  const auto names=gfx::IconCache::Names();CHECK_EQ(names.size(),68u);
  for(const auto& file:fs::directory_iterator(ExeDir()/L"icon-packs")) {
    if(file.path().extension()!=L".json")continue;
    std::ifstream stream(file.path());std::string data((std::istreambuf_iterator<char>(stream)),{});
    auto manifest=Json::Parse(data);CHECK(manifest.has_value());if(!manifest)continue;
    for(const auto& [name,layers]:manifest->Get("icons").Items()) {
      auto one=*manifest;one["icons"]=Json::MakeObject();one["icons"][name]=layers;gfx::IconPack parsed;
      if(!gfx::ParseIconPack(one,parsed)) {std::printf("Invalid glyph %s / %s\n",manifest->Get("id").AsString().c_str(),name.c_str());
        for(const auto& l:layers.Elements()) {auto g=gfx::ParseSvgPath(l.Get("path").AsString(),l.Get("fill").AsBool());D2D1_RECT_F b{};if(g)g->GetBounds(nullptr,&b);std::printf("  geo %d bounds %.2f %.2f %.2f %.2f\n",(bool)g,b.left,b.top,b.right,b.bottom);}
        CHECK(false);}
    }
  }
  for(const char* id:{"pebble","circuit","facet"}) {
    const auto& pack=gfx::IconPacks::Get().Resolve(id);
    CHECK_EQ(pack.id,id);CHECK_EQ(pack.icons.size(),names.size());
    if(pack.id==id)for(const auto& n:names)CHECK(pack.icons.contains(n));
  }
  CHECK_EQ(gfx::IconPacks::Get().Resolve("classic").icons.size(),0u);
  CHECK_EQ(gfx::IconPacks::Get().Resolve("removed-pack").id,"classic");
}
TEST(icon_plugin_manifest_rejects_bad_paths_and_accepts_partial_inheritance) {
  gfx::IconPack p;auto j=SamplePack();CHECK(gfx::ParseIconPack(j,p));CHECK_EQ(p.icons.size(),1u);
  for(const char* bad:{"M0 0z 1 1","Z","M1 1X2 2","M1e999 2L3 4","M-1 4h20v10z","M2 2L26 2"}) {
    auto corrupt=SamplePack();auto l=Json::MakeObject();l["path"]=bad;l["fill"]=true;
    auto a=Json::MakeArray();a.Push(l);corrupt["icons"]["image"]=a;CHECK(!gfx::ParseIconPack(corrupt,p));
  }
  for(const char* id:{"classic","../escape","UPPER",""}) {auto corrupt=SamplePack();corrupt["id"]=id;CHECK(!gfx::ParseIconPack(corrupt,p));}
  auto bad=SamplePack();bad["schema"]=2;CHECK(!gfx::ParseIconPack(bad,p));
  bad=SamplePack();bad["icons"]["unknown"]=bad["icons"].Get("image");CHECK(!gfx::ParseIconPack(bad,p));
  auto l=Json::MakeObject();l["path"]="M4 4h16v16H4z";l["fill"]=true;l["opacity"]=.25;
  auto a=Json::MakeArray();a.Push(l);bad=SamplePack();bad["icons"]["image"]=a;CHECK(!gfx::ParseIconPack(bad,p));
  l["opacity"]=1;l["stroke"]=-1;a=Json::MakeArray();a.Push(l);bad["icons"]["image"]=a;CHECK(!gfx::ParseIconPack(bad,p));
}
TEST(icon_plugin_selection_roundtrips_without_changing_glass_or_processing) {
  CHECK_EQ(Settings::FromJson(Json::MakeObject()).iconPack,"classic");
  Settings s;s.glassStyle=1;s.reduceMotion=true;s.imageQuality=73;
  for(const char* id:{"classic","pebble","circuit","facet","custom-pack"}) {
    s.iconPack=id;const auto copy=Settings::FromJson(s.ToJson());CHECK_EQ(copy.iconPack,id);
    CHECK_EQ(copy.glassStyle,1);CHECK(copy.reduceMotion);CHECK_EQ(copy.imageQuality,73);
  }
}

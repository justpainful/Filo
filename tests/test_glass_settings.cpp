#include "test.h"
#include "core/settings.h"
using namespace filo;
TEST(glass_settings_keep_existing_preferences_and_enable_both_materials) {
  Json old=Json::MakeObject();old["theme"]=2;old["glassIntensity"]=.55;old["reduceMotion"]=true;
  Settings migrated=Settings::FromJson(old);
  CHECK_EQ(migrated.glassStyle,4);CHECK_EQ(migrated.theme,2);CHECK(migrated.reduceMotion);      // a file without a version predates 0.1.8: Symbolic Glass becomes the default
  CHECK(migrated.iconPack=="symbolic");
  for(int style:{0,1,2,3,4}) {
    migrated.glassStyle=style;migrated.glassReflections=.35f;migrated.glassRefraction=.8f;
    Settings restored=Settings::FromJson(migrated.ToJson());
    CHECK_EQ(restored.glassStyle,style);CHECK_EQ(restored.glassReflections,.35f);CHECK_EQ(restored.glassRefraction,.8f);
    CHECK_EQ(restored.glassIntensity,migrated.glassIntensity);
  }
  old["glassStyle"]=17;old["glassReflections"]=-3;old["glassRefraction"]=5;
  auto bounded=Settings::FromJson(old);
  CHECK_EQ(bounded.glassStyle,4);CHECK_EQ(bounded.glassReflections,0.f);CHECK_EQ(bounded.glassRefraction,1.f);
}

TEST(acrylic_transparency_and_blur_are_independent_and_persistent) {
  auto old=Settings::FromJson(Json::MakeObject());
  CHECK_EQ(old.acrylicTransparency,.85f);CHECK_EQ(old.acrylicBlur,30.f);
  Settings s;s.acrylicTransparency=.37f;s.acrylicBlur=8;s.glassIntensity=.6f;s.glassStyle=2;
  auto restored=Settings::FromJson(s.ToJson());
  CHECK_EQ(restored.acrylicTransparency,.37f);CHECK_EQ(restored.acrylicBlur,8.f);CHECK_EQ(restored.glassIntensity,.6f);
  auto j=s.ToJson();j["acrylicTransparency"]=7;j["acrylicBlur"]=-12;
  auto bounded=Settings::FromJson(j);CHECK_EQ(bounded.acrylicTransparency,1.f);CHECK_EQ(bounded.acrylicBlur,0.f);
  j["acrylicTransparency"]=-1;j["acrylicBlur"]=80;bounded=Settings::FromJson(j);
  CHECK_EQ(bounded.acrylicTransparency,0.f);CHECK_EQ(bounded.acrylicBlur,48.f);
}

TEST(symbolic_defaults_are_migrated_once_and_later_choices_are_kept) {
  Settings fresh;
  CHECK_EQ(fresh.glassStyle,4);CHECK(fresh.iconPack=="symbolic");
  auto j=fresh.ToJson();CHECK_EQ(j.Get("version").AsInt(),2);
  // a version 2 file that picked another material and pack keeps them
  j["glassStyle"]=0;j["iconPack"]="pebble";
  auto kept=Settings::FromJson(j);CHECK_EQ(kept.glassStyle,0);CHECK(kept.iconPack=="pebble");
  // a version 1 file (before 0.1.8) is moved to the new defaults
  j["version"]=1;
  auto moved=Settings::FromJson(j);CHECK_EQ(moved.glassStyle,4);CHECK(moved.iconPack=="symbolic");
}

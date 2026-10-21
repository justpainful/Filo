#include "test.h"
#include "wheel/geometry.h"
#include <random>

using namespace filo::wheel;

TEST(geometry_sector_zero_is_at_top_and_clockwise) {
  Layout L; L.n = 8; L.R = 100;
  CHECK_EQ(NearestSector(L, {0, -70}), 0);          // 12 o'clock
  CHECK_EQ(NearestSector(L, {70, 0}), 2);           // 3 o'clock
  CHECK_EQ(NearestSector(L, {0, 70}), 4);           // 6 o'clock
  CHECK_EQ(NearestSector(L, {-70, 0}), 6);          // 9 o'clock
  Hit h = HitTest(L, {0, -67});
  CHECK(h.zone == Zone::Sector && h.sector == 0);
}

TEST(geometry_gap_between_sectors_is_not_a_hit) {
  Layout L; L.n = 8; L.R = 100;
  // boundary between sector 0 and 1 is at 22.5 degrees from 12 o'clock
  const float a = 22.5f * kPi / 180.f, r = 67.f;
  Hit h = HitTest(L, {std::sin(a) * r, -std::cos(a) * r});
  CHECK(h.zone == Zone::Rim);                       // inside the gap => rim, never a sector
}

TEST(geometry_hole_and_outside) {
  Layout L; L.n = 6; L.R = 100;
  CHECK(HitTest(L, {0, 0}).zone == Zone::Hole);
  CHECK(HitTest(L, {200, 0}).zone == Zone::Outside);
  CHECK(HitTest(L, {0, -99}).zone == Zone::Rim);    // between sector outer (0.955R) and rim (1.0R)
}

TEST(geometry_sdf_negative_inside_positive_outside) {
  Layout L; L.n = 8; L.R = 100;
  CHECK(SectorSdf(L, 0, {0, -67}) < 0);
  CHECK(SectorSdf(L, 0, {0, -10}) > 0);
  CHECK(SectorSdf(L, 0, {0, -120}) > 0);
  CHECK(SectorSdf(L, 0, {67, 0}) > 0);              // sector 2's territory
}

TEST(geometry_every_sector_has_one_owner_for_random_points) {
  std::mt19937 rng(7);
  std::uniform_real_distribution<float> u(-110.f, 110.f);
  for (int n : {1, 2, 3, 5, 7, 8, 12, 16}) {
    Layout L; L.n = n; L.R = 100;
    for (int i = 0; i < 4000; ++i) {
      Vec2 p{u(rng), u(rng)};
      Hit h = HitTest(L, p);
      if (h.zone == Zone::Sector) {
        CHECK(h.sector >= 0 && h.sector < n);
        // no other sector may also claim the point
        for (int k = 0; k < n; ++k) if (k != h.sector) CHECK(SectorSdf(L, k, p) > -0.01f);
      }
    }
  }
}

TEST(geometry_hover_scale_grows_hit_area) {
  Layout L; L.n = 8; L.R = 100;
  float scales[16]; for (auto& s : scales) s = 1.f;
  // a point just outside the sector's outer arc
  Vec2 p{0, -97.f * 0.99f};
  Hit h0 = HitTest(L, p, scales);
  scales[0] = 1.05f;
  Hit h1 = HitTest(L, p, scales);
  CHECK(h0.zone != Zone::Sector);
  CHECK(h1.zone == Zone::Sector && h1.sector == 0);
}

TEST(geometry_text_box_positive) {
  for (int n : {1, 2, 4, 8, 12, 16}) {
    Layout L; L.n = n; L.R = 150;
    Vec2 b = SectorTextBox(L);
    CHECK(b.x >= 10 && b.y >= 10);
  }
}

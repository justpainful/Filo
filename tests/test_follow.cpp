#include "test.h"
#include "ui/ui.h"
#include <cmath>

using filo::ui::FollowStep;

// The lens of a segmented control follows its target with FollowStep. It must come to rest exactly on ANY target (1/3 and 2/3 are
// the lens positions of tabs 2 and 3 of a four-tab bar), never overshoot, and move monotonically.
TEST(follow_comes_to_rest_exactly_on_the_target_for_any_position) {
  for (float target : {0.f, 1.f / 3.f, 0.5f, 2.f / 3.f, 0.25f, 0.75f, 1.f}) {
    float v = 0.5f;
    float last = std::fabs(target - v);
    for (int i = 0; i < 200 && v != target; ++i) {
      v = FollowStep(v, target, 1.f / 60.f, 0.22f);
      const float d = std::fabs(target - v);
      CHECK(d <= last + 1e-6f);                 // never moves away, never overshoots past the target
      last = d;
    }
    CHECK(v == target);
  }
}

TEST(follow_reaches_ninety_five_percent_in_about_the_requested_time) {
  float v = 0.f;
  for (int i = 0; i < 13; ++i) v = FollowStep(v, 1.f, 1.f / 60.f, 0.22f);     // 13 frames = 217 ms
  CHECK(v > 0.90f && v < 0.99f);
}

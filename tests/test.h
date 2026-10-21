// Minimal unit-test runner: TEST(name) { CHECK(cond); CHECK_EQ(a,b); CHECK_NEAR(a,b,eps); }
#pragma once
#include <cstdio>
#include <cmath>
#include <functional>
#include <string>
#include <vector>

namespace ftest {
struct Case { const char* name; std::function<void()> fn; };
inline std::vector<Case>& Cases() { static std::vector<Case> c; return c; }
inline int& Failures() { static int f = 0; return f; }
inline const char*& CurrentName() { static const char* n = ""; return n; }
struct Reg { Reg(const char* n, std::function<void()> f) { Cases().push_back({n, std::move(f)}); } };
inline int RunAll(int argc, char** argv) {
  const char* filter = argc > 1 ? argv[1] : nullptr;
  int ran = 0;
  for (auto& c : Cases()) {
    if (filter && std::string(c.name).find(filter) == std::string::npos) continue;
    CurrentName() = c.name;
    int before = Failures();
    try { c.fn(); } catch (const std::exception& e) { std::printf("  EXCEPTION in %s: %s\n", c.name, e.what()); ++Failures(); }
    std::printf("[%s] %s\n", Failures() == before ? " ok " : "FAIL", c.name);
    ++ran;
  }
  std::printf("\n%d test(s), %d failure(s)\n", ran, Failures());
  return Failures() ? 1 : 0;
}
}

#define FT_CAT2(a, b) a##b
#define FT_CAT(a, b) FT_CAT2(a, b)
#define TEST(name) static void FT_CAT(test_, name)(); static ftest::Reg FT_CAT(reg_, name)(#name, FT_CAT(test_, name)); static void FT_CAT(test_, name)()
#define CHECK(c) do { if (!(c)) { std::printf("  %s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); ++ftest::Failures(); } } while (0)
#define CHECK_EQ(a, b) do { auto _a = (a); auto _b = (b); if (!(_a == _b)) { std::printf("  %s:%d: CHECK_EQ failed: %s != %s\n", __FILE__, __LINE__, #a, #b); ++ftest::Failures(); } } while (0)
#define CHECK_NEAR(a, b, eps) do { double _a = (a), _b = (b); if (std::fabs(_a - _b) > (eps)) { std::printf("  %s:%d: CHECK_NEAR failed: %s=%g vs %s=%g\n", __FILE__, __LINE__, #a, _a, #b, _b); ++ftest::Failures(); } } while (0)

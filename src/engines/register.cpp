#include "engines/engines.h"
#include "registry/capability.h"
#include <mutex>

// Weak fallbacks keep the build green until each engine library provides its real Register function.
#pragma comment(linker, "/alternatename:?RegisterImageEngine@filo@@YAXXZ=?RegisterImageEngineStub@filo@@YAXXZ")
#pragma comment(linker, "/alternatename:?RegisterMediaEngine@filo@@YAXXZ=?RegisterMediaEngineStub@filo@@YAXXZ")
#pragma comment(linker, "/alternatename:?RegisterPdfEngine@filo@@YAXXZ=?RegisterPdfEngineStub@filo@@YAXXZ")
#pragma comment(linker, "/alternatename:?RegisterDocEngine@filo@@YAXXZ=?RegisterDocEngineStub@filo@@YAXXZ")
#pragma comment(linker, "/alternatename:?RegisterArchiveEngine@filo@@YAXXZ=?RegisterArchiveEngineStub@filo@@YAXXZ")

namespace filo {
void RegisterImageEngineStub() {}
void RegisterMediaEngineStub() {}
void RegisterPdfEngineStub() {}
void RegisterDocEngineStub() {}
void RegisterArchiveEngineStub() {}

void RegisterAllEngines() {
  static std::once_flag once;
  std::call_once(once, [] {
    RegisterImageEngine();
    RegisterMediaEngine();
    RegisterPdfEngine();
    RegisterDocEngine();
    RegisterArchiveEngine();
  });
}
}

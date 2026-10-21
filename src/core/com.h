// Tiny COM smart pointer (the machine has no ATL; we avoid the dependency anyway).
#pragma once
#include <windows.h>
#include <unknwn.h>
#include <utility>
#include <memory>

namespace filo {

template <class T> class ComPtr {
 public:
  ComPtr() = default;
  ComPtr(std::nullptr_t) {}
  explicit ComPtr(T* p, bool addRef = true) : p_(p) { if (p_ && addRef) p_->AddRef(); }
  ComPtr(const ComPtr& o) : p_(o.p_) { if (p_) p_->AddRef(); }
  ComPtr(ComPtr&& o) noexcept : p_(o.p_) { o.p_ = nullptr; }
  ~ComPtr() { Reset(); }
  ComPtr& operator=(const ComPtr& o) { if (this != std::addressof(o)) { ComPtr t(o); Swap(t); } return *this; }
  ComPtr& operator=(ComPtr&& o) noexcept { if (this != std::addressof(o)) { Reset(); p_ = o.p_; o.p_ = nullptr; } return *this; }
  T* operator->() const { return p_; }
  T& operator*() const { return *p_; }
  explicit operator bool() const { return p_ != nullptr; }
  bool operator!() const { return p_ == nullptr; }
  T* Get() const { return p_; }
  T** operator&() { Reset(); return &p_; }          // for out-params (resets first)
  T** GetAddressOf() { return &p_; }
  T* Detach() { T* t = p_; p_ = nullptr; return t; }
  void Attach(T* p) { Reset(); p_ = p; }
  void Reset() { if (p_) { p_->Release(); p_ = nullptr; } }
  void Swap(ComPtr& o) { std::swap(p_, o.p_); }
  template <class U> HRESULT As(ComPtr<U>* out) const { return p_ ? p_->QueryInterface(__uuidof(U), (void**)out->GetAddressOfReset()) : E_POINTER; }
  template <class U> ComPtr<U> As() const { ComPtr<U> r; if (p_) p_->QueryInterface(__uuidof(U), (void**)r.GetAddressOfReset()); return r; }
  T** GetAddressOfReset() { Reset(); return &p_; }
  void** AsVoid() { Reset(); return reinterpret_cast<void**>(&p_); }
 private:
  T* p_ = nullptr;
};

// Mirrors IID_PPV_ARGS for ComPtr: ComPtr<IFoo> p; hr = obj->QueryInterface(PPV(p));
#define FILO_PPV(cp) __uuidof(**(cp).GetAddressOf()), (cp).AsVoid()

template <class T> HRESULT CoCreate(REFCLSID clsid, ComPtr<T>& out, DWORD ctx = CLSCTX_ALL) {
  return CoCreateInstance(clsid, nullptr, ctx, __uuidof(T), out.AsVoid());
}

struct ComInit {   // RAII CoInitializeEx
  HRESULT hr;
  explicit ComInit(DWORD model = COINIT_APARTMENTTHREADED) : hr(CoInitializeEx(nullptr, model)) {}
  ~ComInit() { if (SUCCEEDED(hr)) CoUninitialize(); }
};

} // namespace filo

// Small self-contained JSON value / parser / writer (UTF-8). Used for settings, tool presets, ffprobe output, test reports.
#pragma once
#include <string>
#include <string_view>
#include <vector>
#include <map>
#include <memory>
#include <cstdint>
#include <optional>

namespace filo {

class Json {
 public:
  enum class Type { Null, Bool, Number, String, Array, Object };
  using Array = std::vector<Json>;
  using Object = std::map<std::string, Json>;

  Json() = default;
  Json(std::nullptr_t) {}
  Json(bool b) : type_(Type::Bool), b_(b) {}
  Json(int v) : type_(Type::Number), n_(v) {}
  Json(int64_t v) : type_(Type::Number), n_((double)v) {}
  Json(uint64_t v) : type_(Type::Number), n_((double)v) {}
  Json(double v) : type_(Type::Number), n_(v) {}
  Json(const char* s) : type_(Type::String), s_(s) {}
  Json(std::string s) : type_(Type::String), s_(std::move(s)) {}
  Json(std::string_view s) : type_(Type::String), s_(s) {}
  static Json MakeArray() { Json j; j.type_ = Type::Array; return j; }
  static Json MakeObject() { Json j; j.type_ = Type::Object; return j; }

  Type type() const { return type_; }
  bool IsNull() const { return type_ == Type::Null; }
  bool IsBool() const { return type_ == Type::Bool; }
  bool IsNumber() const { return type_ == Type::Number; }
  bool IsString() const { return type_ == Type::String; }
  bool IsArray() const { return type_ == Type::Array; }
  bool IsObject() const { return type_ == Type::Object; }

  bool AsBool(bool def = false) const { return type_ == Type::Bool ? b_ : def; }
  double AsNumber(double def = 0) const;                 // also parses numeric strings (ffprobe prints numbers as strings)
  int64_t AsInt(int64_t def = 0) const { return (int64_t)AsNumber((double)def); }
  const std::string& AsString() const { static const std::string e; return type_ == Type::String ? s_ : e; }
  std::string StringOr(std::string def) const { return type_ == Type::String ? s_ : def; }

  // Object access. operator[] on a non-object converts it to an object (like most JSON libs).
  Json& operator[](const std::string& key);
  const Json& Get(const std::string& key) const;         // returns a shared null when missing
  bool Has(const std::string& key) const { return type_ == Type::Object && o_.count(key) > 0; }
  const Object& Items() const { static const Object e; return type_ == Type::Object ? o_ : e; }

  // Array access
  void Push(Json v);
  size_t Size() const { return type_ == Type::Array ? a_.size() : (type_ == Type::Object ? o_.size() : 0); }
  const Json& At(size_t i) const;
  const Array& Elements() const { static const Array e; return type_ == Type::Array ? a_ : e; }

  std::string Dump(bool pretty = false) const;
  static std::optional<Json> Parse(std::string_view text, std::string* error = nullptr);

 private:
  void DumpTo(std::string& out, bool pretty, int indent) const;
  Type type_ = Type::Null;
  bool b_ = false;
  double n_ = 0;
  std::string s_;
  Array a_;
  Object o_;
};

} // namespace filo

#pragma once

#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace corevideo::rpc {

class Json {
 public:
  using Array = std::vector<Json>;
  using Object = std::map<std::string, Json>;
  using Value = std::variant<std::nullptr_t, bool, double, std::string, Array, Object>;

  Json();
  Json(std::nullptr_t);
  Json(bool value);
  Json(int value);
  Json(double value);
  Json(const char* value);
  Json(std::string value);
  Json(Array value);
  Json(Object value);

  [[nodiscard]] bool isNull() const;
  [[nodiscard]] bool isBool() const;
  [[nodiscard]] bool isNumber() const;
  [[nodiscard]] bool isString() const;
  [[nodiscard]] bool isArray() const;
  [[nodiscard]] bool isObject() const;

  [[nodiscard]] bool asBool(bool fallback = false) const;
  [[nodiscard]] double asNumber(double fallback = 0) const;
  [[nodiscard]] const std::string& asString() const;
  [[nodiscard]] const Array& asArray() const;
  [[nodiscard]] const Object& asObject() const;

  [[nodiscard]] const Json* get(const std::string& key) const;
  [[nodiscard]] std::string getString(const std::string& key, std::string fallback = "") const;
  [[nodiscard]] std::vector<std::string> getStringArray(const std::string& key) const;

  [[nodiscard]] std::string stringify() const;
  [[nodiscard]] const Value& value() const { return value_; }

  static std::optional<Json> parse(const std::string& input, std::string* error = nullptr);

 private:
  Value value_;
};

}  // namespace corevideo::rpc

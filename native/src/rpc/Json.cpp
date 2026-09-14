#include "rpc/Json.h"

#include <cctype>
#include <sstream>
#include <stdexcept>

namespace corevideo::rpc {
namespace {

const Json::Array kEmptyArray;
const Json::Object kEmptyObject;
const std::string kEmptyString;

std::string escapeString(const std::string& value) {
  std::ostringstream out;
  out << '"';
  for (char ch : value) {
    switch (ch) {
      case '"': out << "\\\""; break;
      case '\\': out << "\\\\"; break;
      case '\n': out << "\\n"; break;
      case '\r': out << "\\r"; break;
      case '\t': out << "\\t"; break;
      default:
        if (static_cast<unsigned char>(ch) < 0x20) {
          out << "\\u00";
          constexpr char hex[] = "0123456789abcdef";
          out << hex[(ch >> 4) & 0xf] << hex[ch & 0xf];
        } else {
          out << ch;
        }
    }
  }
  out << '"';
  return out.str();
}

class Parser {
 public:
  explicit Parser(const std::string& input) : input_(input) {}

  Json parse() {
    skipSpace();
    Json value = parseValue();
    skipSpace();
    if (pos_ != input_.size()) {
      throw std::runtime_error("Unexpected trailing JSON data.");
    }
    return value;
  }

 private:
  Json parseValue() {
    skipSpace();
    if (pos_ >= input_.size()) {
      throw std::runtime_error("Unexpected end of JSON.");
    }
    char ch = input_[pos_];
    if (ch == '"') return Json(parseString());
    if (ch == '{') return Json(parseObject());
    if (ch == '[') return Json(parseArray());
    if (ch == 't') return parseLiteral("true", Json(true));
    if (ch == 'f') return parseLiteral("false", Json(false));
    if (ch == 'n') return parseLiteral("null", Json(nullptr));
    if (ch == '-' || std::isdigit(static_cast<unsigned char>(ch))) return Json(parseNumber());
    throw std::runtime_error("Invalid JSON value.");
  }

  Json parseLiteral(const std::string& literal, Json value) {
    if (input_.substr(pos_, literal.size()) != literal) {
      throw std::runtime_error("Invalid JSON literal.");
    }
    pos_ += literal.size();
    return value;
  }

  std::string parseString() {
    expect('"');
    std::string result;
    while (pos_ < input_.size()) {
      char ch = input_[pos_++];
      if (ch == '"') return result;
      if (ch == '\\') {
        if (pos_ >= input_.size()) throw std::runtime_error("Invalid escape sequence.");
        char escaped = input_[pos_++];
        switch (escaped) {
          case '"': result.push_back('"'); break;
          case '\\': result.push_back('\\'); break;
          case '/': result.push_back('/'); break;
          case 'b': result.push_back('\b'); break;
          case 'f': result.push_back('\f'); break;
          case 'n': result.push_back('\n'); break;
          case 'r': result.push_back('\r'); break;
          case 't': result.push_back('\t'); break;
          case 'u': {
            // \uXXXX unicode escape. The host (System.Text.Json default encoder)
            // emits these for non-ASCII and HTML-sensitive characters (e.g. the
            // "·" in participant labels, "+", "&"), which appear in the larger
            // media-core-sync / spine payloads. Without this the parser threw and
            // every such request was answered with id="unknown" -> the bridge never
            // matched the real id and timed out at 4s. Decode to a code point
            // (combining surrogate pairs) and append as UTF-8.
            unsigned int cp = parseHex4();
            if (cp >= 0xD800 && cp <= 0xDBFF) {  // high surrogate -> expect low surrogate
              if (pos_ + 1 < input_.size() && input_[pos_] == '\\' && input_[pos_ + 1] == 'u') {
                pos_ += 2;
                const unsigned int low = parseHex4();
                if (low >= 0xDC00 && low <= 0xDFFF) {
                  cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                } else {
                  cp = 0xFFFD;  // unpaired low -> replacement char
                }
              } else {
                cp = 0xFFFD;  // lone high surrogate -> replacement char
              }
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
              cp = 0xFFFD;  // lone low surrogate -> replacement char
            }
            appendUtf8(result, cp);
            break;
          }
          default: throw std::runtime_error("Unsupported JSON escape sequence.");
        }
      } else {
        result.push_back(ch);
      }
    }
    throw std::runtime_error("Unterminated JSON string.");
  }

  // Read exactly 4 hex digits (consumed) and return their value.
  unsigned int parseHex4() {
    if (pos_ + 4 > input_.size()) throw std::runtime_error("Invalid \\u escape: truncated.");
    unsigned int value = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = input_[pos_++];
      value <<= 4;
      if (c >= '0' && c <= '9') value |= static_cast<unsigned int>(c - '0');
      else if (c >= 'a' && c <= 'f') value |= static_cast<unsigned int>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') value |= static_cast<unsigned int>(c - 'A' + 10);
      else throw std::runtime_error("Invalid \\u escape: non-hex digit.");
    }
    return value;
  }

  static void appendUtf8(std::string& out, unsigned int cp) {
    if (cp <= 0x7F) {
      out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
      out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
      out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }

  double parseNumber() {
    size_t start = pos_;
    if (input_[pos_] == '-') ++pos_;
    while (pos_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[pos_]))) ++pos_;
    if (pos_ < input_.size() && input_[pos_] == '.') {
      ++pos_;
      while (pos_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[pos_]))) ++pos_;
    }
    return std::stod(input_.substr(start, pos_ - start));
  }

  Json::Array parseArray() {
    expect('[');
    Json::Array result;
    skipSpace();
    if (consume(']')) return result;
    while (true) {
      result.push_back(parseValue());
      skipSpace();
      if (consume(']')) return result;
      expect(',');
    }
  }

  Json::Object parseObject() {
    expect('{');
    Json::Object result;
    skipSpace();
    if (consume('}')) return result;
    while (true) {
      skipSpace();
      std::string key = parseString();
      skipSpace();
      expect(':');
      result.emplace(std::move(key), parseValue());
      skipSpace();
      if (consume('}')) return result;
      expect(',');
    }
  }

  void skipSpace() {
    while (pos_ < input_.size() && std::isspace(static_cast<unsigned char>(input_[pos_]))) ++pos_;
  }

  bool consume(char expected) {
    if (pos_ < input_.size() && input_[pos_] == expected) {
      ++pos_;
      return true;
    }
    return false;
  }

  void expect(char expected) {
    if (!consume(expected)) {
      throw std::runtime_error("Unexpected JSON character.");
    }
  }

  const std::string& input_;
  size_t pos_ = 0;
};

}  // namespace

Json::Json() : value_(nullptr) {}
Json::Json(std::nullptr_t) : value_(nullptr) {}
Json::Json(bool value) : value_(value) {}
Json::Json(int value) : value_(static_cast<double>(value)) {}
Json::Json(double value) : value_(value) {}
Json::Json(const char* value) : value_(std::string(value)) {}
Json::Json(std::string value) : value_(std::move(value)) {}
Json::Json(Array value) : value_(std::move(value)) {}
Json::Json(Object value) : value_(std::move(value)) {}

bool Json::isNull() const { return std::holds_alternative<std::nullptr_t>(value_); }
bool Json::isBool() const { return std::holds_alternative<bool>(value_); }
bool Json::isNumber() const { return std::holds_alternative<double>(value_); }
bool Json::isString() const { return std::holds_alternative<std::string>(value_); }
bool Json::isArray() const { return std::holds_alternative<Array>(value_); }
bool Json::isObject() const { return std::holds_alternative<Object>(value_); }

bool Json::asBool(bool fallback) const { return isBool() ? std::get<bool>(value_) : fallback; }
double Json::asNumber(double fallback) const { return isNumber() ? std::get<double>(value_) : fallback; }
const std::string& Json::asString() const { return isString() ? std::get<std::string>(value_) : kEmptyString; }
const Json::Array& Json::asArray() const { return isArray() ? std::get<Array>(value_) : kEmptyArray; }
const Json::Object& Json::asObject() const { return isObject() ? std::get<Object>(value_) : kEmptyObject; }

const Json* Json::get(const std::string& key) const {
  if (!isObject()) return nullptr;
  auto it = std::get<Object>(value_).find(key);
  return it == std::get<Object>(value_).end() ? nullptr : &it->second;
}

std::string Json::getString(const std::string& key, std::string fallback) const {
  const Json* child = get(key);
  return child && child->isString() ? child->asString() : std::move(fallback);
}

double Json::getNumber(const std::string& key, double fallback) const {
  const Json* child = get(key);
  return child && child->isNumber() ? child->asNumber() : fallback;
}

std::vector<std::string> Json::getStringArray(const std::string& key) const {
  std::vector<std::string> result;
  const Json* child = get(key);
  if (!child || !child->isArray()) return result;
  for (const Json& item : child->asArray()) {
    if (item.isString()) result.push_back(item.asString());
  }
  return result;
}

std::string Json::stringify() const {
  if (isNull()) return "null";
  if (isBool()) return std::get<bool>(value_) ? "true" : "false";
  if (isNumber()) {
    std::ostringstream out;
    double value = std::get<double>(value_);
    if (value == static_cast<long long>(value)) out << static_cast<long long>(value);
    else out << value;
    return out.str();
  }
  if (isString()) return escapeString(std::get<std::string>(value_));
  if (isArray()) {
    std::string out = "[";
    bool first = true;
    for (const Json& item : std::get<Array>(value_)) {
      if (!first) out += ",";
      first = false;
      out += item.stringify();
    }
    out += "]";
    return out;
  }
  std::string out = "{";
  bool first = true;
  for (const auto& [key, value] : std::get<Object>(value_)) {
    if (!first) out += ",";
    first = false;
    out += escapeString(key) + ":" + value.stringify();
  }
  out += "}";
  return out;
}

std::optional<Json> Json::parse(const std::string& input, std::string* error) {
  try {
    return Parser(input).parse();
  } catch (const std::exception& ex) {
    if (error) *error = ex.what();
    return std::nullopt;
  }
}

}  // namespace corevideo::rpc

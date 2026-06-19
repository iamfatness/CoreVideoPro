#include "ProgramPreview.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <limits>
#include <sstream>

namespace {

constexpr int kMaxDimension = 8192;
constexpr std::uint64_t kMaxPixelBytes = 512ull * 1024ull * 1024ull;

bool isWhitespace(char value) {
  return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

void skipWhitespace(std::string_view text, size_t& offset) {
  while (offset < text.size() && isWhitespace(text[offset])) {
    ++offset;
  }
}

bool appendJsonEscape(char value, std::string& output) {
  switch (value) {
    case '"':
    case '\\':
    case '/':
      output.push_back(value);
      return true;
    case 'b':
      output.push_back('\b');
      return true;
    case 'f':
      output.push_back('\f');
      return true;
    case 'n':
      output.push_back('\n');
      return true;
    case 'r':
      output.push_back('\r');
      return true;
    case 't':
      output.push_back('\t');
      return true;
    default:
      return false;
  }
}

bool parseJsonStringAt(std::string_view text, size_t& offset, std::string& output) {
  if (offset >= text.size() || text[offset] != '"') {
    return false;
  }

  output.clear();
  ++offset;
  while (offset < text.size()) {
    const char value = text[offset++];
    if (value == '"') {
      return true;
    }
    if (static_cast<unsigned char>(value) < 0x20) {
      return false;
    }
    if (value != '\\') {
      output.push_back(value);
      continue;
    }
    if (offset >= text.size()) {
      return false;
    }
    const char escaped = text[offset++];
    if (escaped == 'u') {
      return false;
    }
    if (!appendJsonEscape(escaped, output)) {
      return false;
    }
  }

  return false;
}

bool findJsonField(std::string_view text, std::string_view name, size_t& valueOffset) {
  size_t offset = 0;
  while (offset < text.size()) {
    if (text[offset] != '"') {
      ++offset;
      continue;
    }

    const size_t stringStart = offset;
    std::string key;
    if (!parseJsonStringAt(text, offset, key)) {
      offset = stringStart + 1;
      continue;
    }

    size_t colon = offset;
    skipWhitespace(text, colon);
    if (colon >= text.size() || text[colon] != ':') {
      continue;
    }
    ++colon;
    skipWhitespace(text, colon);

    if (key == name) {
      valueOffset = colon;
      return true;
    }
  }

  return false;
}

bool getJsonString(std::string_view text, std::string_view name, std::string& value) {
  size_t offset = 0;
  if (!findJsonField(text, name, offset)) {
    return false;
  }
  return parseJsonStringAt(text, offset, value);
}

bool parseUnsignedAt(std::string_view text, size_t offset, std::uint64_t& value) {
  if (offset >= text.size() || text[offset] < '0' || text[offset] > '9') {
    return false;
  }

  const char* first = text.data() + offset;
  const char* last = text.data() + text.size();
  const auto result = std::from_chars(first, last, value);
  if (result.ec != std::errc()) {
    return false;
  }
  if (result.ptr < last) {
    const char next = *result.ptr;
    if (!isWhitespace(next) && next != ',' && next != '}' && next != ']') {
      return false;
    }
  }
  return true;
}

bool getJsonUnsigned(std::string_view text, std::string_view name, std::uint64_t& value) {
  size_t offset = 0;
  if (!findJsonField(text, name, offset)) {
    return false;
  }
  return parseUnsignedAt(text, offset, value);
}

bool isPreviewEvent(std::string_view line) {
  std::string value;
  if (getJsonString(line, "type", value) && value == "program-frame-preview") {
    return true;
  }
  if (getJsonString(line, "event", value) && value == "program-frame-preview") {
    return true;
  }
  return line.find("\"program-frame-preview\"") != std::string_view::npos;
}

int base64Value(char value) {
  if (value >= 'A' && value <= 'Z') {
    return value - 'A';
  }
  if (value >= 'a' && value <= 'z') {
    return value - 'a' + 26;
  }
  if (value >= '0' && value <= '9') {
    return value - '0' + 52;
  }
  if (value == '+') {
    return 62;
  }
  if (value == '/') {
    return 63;
  }
  return -1;
}

bool decodeBase64(std::string_view input, std::vector<std::uint8_t>& output) {
  output.clear();
  std::array<int, 4> block{};
  int blockSize = 0;
  bool seenPadding = false;

  for (char value : input) {
    if (isWhitespace(value)) {
      continue;
    }

    if (value == '=') {
      seenPadding = true;
      block[blockSize++] = -2;
    } else {
      const int decoded = base64Value(value);
      if (decoded < 0 || seenPadding) {
        return false;
      }
      block[blockSize++] = decoded;
    }

    if (blockSize != 4) {
      continue;
    }

    if (block[0] < 0 || block[1] < 0) {
      return false;
    }

    output.push_back(static_cast<std::uint8_t>((block[0] << 2) | (block[1] >> 4)));
    if (block[2] == -2) {
      if (block[3] != -2 || (block[1] & 0x0f) != 0) {
        return false;
      }
    } else {
      if (block[2] < 0) {
        return false;
      }
      output.push_back(static_cast<std::uint8_t>(((block[1] & 0x0f) << 4) | (block[2] >> 2)));
      if (block[3] != -2) {
        if (block[3] < 0) {
          return false;
        }
        output.push_back(static_cast<std::uint8_t>(((block[2] & 0x03) << 6) | block[3]));
      } else if ((block[2] & 0x03) != 0) {
        return false;
      }
    }

    blockSize = 0;
  }

  return blockSize == 0;
}

bool validDimensions(std::uint64_t width, std::uint64_t height, std::uint64_t& byteCount) {
  if (width == 0 || height == 0 || width > kMaxDimension || height > kMaxDimension) {
    return false;
  }
  if (width > std::numeric_limits<std::uint64_t>::max() / height) {
    return false;
  }
  const std::uint64_t pixels = width * height;
  if (pixels > std::numeric_limits<std::uint64_t>::max() / 4ull) {
    return false;
  }
  byteCount = pixels * 4ull;
  return byteCount <= kMaxPixelBytes;
}

RECT fitRect(const RECT& bounds, int width, int height) {
  const int boundsWidth = static_cast<int>(std::max<LONG>(0, bounds.right - bounds.left));
  const int boundsHeight = static_cast<int>(std::max<LONG>(0, bounds.bottom - bounds.top));
  RECT result = bounds;
  if (boundsWidth == 0 || boundsHeight == 0 || width <= 0 || height <= 0) {
    return result;
  }

  const std::uint64_t fitByWidthHeight = static_cast<std::uint64_t>(boundsWidth) * static_cast<std::uint64_t>(height);
  const std::uint64_t fitByHeightWidth = static_cast<std::uint64_t>(boundsHeight) * static_cast<std::uint64_t>(width);

  int drawWidth = boundsWidth;
  int drawHeight = boundsHeight;
  if (fitByWidthHeight <= fitByHeightWidth) {
    drawHeight = static_cast<int>(fitByWidthHeight / static_cast<std::uint64_t>(width));
  } else {
    drawWidth = static_cast<int>(fitByHeightWidth / static_cast<std::uint64_t>(height));
  }

  result.left = bounds.left + (boundsWidth - drawWidth) / 2;
  result.top = bounds.top + (boundsHeight - drawHeight) / 2;
  result.right = result.left + drawWidth;
  result.bottom = result.top + drawHeight;
  return result;
}

std::wstring widenUtf8(std::string_view value) {
  if (value.empty()) {
    return {};
  }
  const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
  if (size <= 0) {
    return {};
  }
  std::wstring result(static_cast<size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size);
  return result;
}

void drawCenteredMultiline(HDC dc, const RECT& bounds, const std::wstring& text, COLORREF color) {
  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, color);
  HFONT font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
  HGDIOBJ previousFont = SelectObject(dc, font);
  RECT textBounds = bounds;
  DrawTextW(dc, text.c_str(), -1, &textBounds, DT_CENTER | DT_VCENTER | DT_WORDBREAK);
  SelectObject(dc, previousFont);
}

void drawPlaceholder(HDC dc, const RECT& bounds, std::string_view waitingHint) {
  FillRect(dc, &bounds, reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));

  RECT header = bounds;
  header.bottom = header.top + 24;
  FillRect(dc, &header, reinterpret_cast<HBRUSH>(GetStockObject(GRAY_BRUSH)));
  drawCenteredMultiline(dc, header, L"PROGRAM PREVIEW", RGB(245, 245, 245));

  RECT body = bounds;
  body.top = header.bottom + 8;
  body.bottom -= 8;
  std::wstring message = L"No program frames yet.\r\n\r\n";
  message += widenUtf8(waitingHint);
  drawCenteredMultiline(dc, body, message, RGB(190, 190, 190));
}

void drawMetadataOverlay(HDC dc, const RECT& bounds, const ProgramPreview::Metadata& metadata) {
  RECT header = bounds;
  header.bottom = header.top + 22;
  FillRect(dc, &header, reinterpret_cast<HBRUSH>(GetStockObject(GRAY_BRUSH)));
  drawCenteredMultiline(dc, header, L"PROGRAM PREVIEW", RGB(245, 245, 245));

  RECT footer = bounds;
  footer.top = std::max(footer.top, footer.bottom - 28);
  HBRUSH overlayBrush = CreateSolidBrush(RGB(18, 18, 18));
  FillRect(dc, &footer, overlayBrush);
  DeleteObject(overlayBrush);

  std::ostringstream caption;
  caption << "Frame " << metadata.frameNumber;
  if (metadata.width > 0 && metadata.height > 0) {
    caption << "  " << metadata.width << "x" << metadata.height;
  }
  if (!metadata.health.empty()) {
    caption << "  health " << metadata.health;
  }
  if (!metadata.renderer.empty()) {
    caption << "  " << metadata.renderer;
  }
  if (!metadata.renderPlanId.empty()) {
    caption << "  plan " << metadata.renderPlanId;
  }

  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, RGB(220, 220, 220));
  HFONT font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
  HGDIOBJ previousFont = SelectObject(dc, font);
  RECT textBounds = footer;
  textBounds.left += 8;
  textBounds.right -= 8;
  DrawTextW(dc, widenUtf8(caption.str()).c_str(), -1, &textBounds, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
  SelectObject(dc, previousFont);
}

}  // namespace

bool ProgramPreview::updateFromEventLine(std::string_view line) {
  if (!isPreviewEvent(line)) {
    return false;
  }

  std::uint64_t width = 0;
  std::uint64_t height = 0;
  std::uint64_t frameNumber = 0;
  std::uint64_t byteCount = 0;
  std::string bgraBase64;
  if (!getJsonUnsigned(line, "width", width) ||
      !getJsonUnsigned(line, "height", height) ||
      !getJsonUnsigned(line, "frameNumber", frameNumber) ||
      !getJsonString(line, "bgraBase64", bgraBase64) ||
      !validDimensions(width, height, byteCount)) {
    return false;
  }

  std::vector<std::uint8_t> pixels;
  if (!decodeBase64(bgraBase64, pixels) || pixels.size() != byteCount) {
    return false;
  }

  Frame frame;
  frame.metadata.width = static_cast<int>(width);
  frame.metadata.height = static_cast<int>(height);
  frame.metadata.frameNumber = frameNumber;
  getJsonString(line, "health", frame.metadata.health);
  getJsonString(line, "renderer", frame.metadata.renderer);
  getJsonString(line, "renderPlanId", frame.metadata.renderPlanId);
  frame.bgra = std::move(pixels);

  std::lock_guard lock(mutex_);
  latest_ = std::move(frame);
  return true;
}

void ProgramPreview::setWaitingHint(std::string hint) {
  std::lock_guard lock(mutex_);
  waitingHint_ = std::move(hint);
}

void ProgramPreview::clear() {
  std::lock_guard lock(mutex_);
  latest_.reset();
}

bool ProgramPreview::hasFrame() const {
  std::lock_guard lock(mutex_);
  return latest_.has_value();
}

std::optional<ProgramPreview::Metadata> ProgramPreview::latestMetadata() const {
  std::lock_guard lock(mutex_);
  if (!latest_) {
    return std::nullopt;
  }
  return latest_->metadata;
}

void ProgramPreview::render(HDC dc, const RECT& bounds) const {
  if (!dc || bounds.right <= bounds.left || bounds.bottom <= bounds.top) {
    return;
  }

  Frame frame;
  std::string waitingHint;
  {
    std::lock_guard lock(mutex_);
    waitingHint = waitingHint_;
    if (!latest_) {
      drawPlaceholder(dc, bounds, waitingHint);
      return;
    }
    frame = *latest_;
  }

  FillRect(dc, &bounds, reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
  const RECT target = fitRect(bounds, frame.metadata.width, frame.metadata.height);
  if (target.right <= target.left || target.bottom <= target.top) {
    return;
  }

  BITMAPINFO info{};
  info.bmiHeader.biSize = sizeof(info.bmiHeader);
  info.bmiHeader.biWidth = frame.metadata.width;
  info.bmiHeader.biHeight = -frame.metadata.height;
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;

  SetStretchBltMode(dc, HALFTONE);
  StretchDIBits(
      dc,
      target.left,
      target.top,
      target.right - target.left,
      target.bottom - target.top,
      0,
      0,
      frame.metadata.width,
      frame.metadata.height,
      frame.bgra.data(),
      &info,
      DIB_RGB_COLORS,
      SRCCOPY);

  drawMetadataOverlay(dc, bounds, frame.metadata);
}

#include "core/Protocol.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

std::string readRepoFile(const std::string& relativePath) {
  const std::filesystem::path root = COREVIDEO_REPO_ROOT;
  std::ifstream input(root / relativePath);
  if (!input) {
    return {};
  }
  std::ostringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
}

template <typename Strings>
void expectAllStringsPresent(const std::string& source, const Strings& strings) {
  for (auto value : strings) {
    EXPECT_NE(source.find(std::string(value)), std::string::npos) << "Missing protocol string: " << value;
  }
}

}  // namespace

TEST(ContractParity, MediaCoreCommandTypesMatchTypeScriptProtocol) {
  const std::string source = readRepoFile("src/engine/nativeMediaCoreProtocol.ts");
  ASSERT_FALSE(source.empty());
  expectAllStringsPresent(source, corevideo::core::kNativeMediaCoreCommandTypes);
}

TEST(ContractParity, CapabilityStringsMatchTypeScriptProtocol) {
  const std::string source = readRepoFile("src/engine/nativeMediaCoreProtocol.ts");
  ASSERT_FALSE(source.empty());
  expectAllStringsPresent(source, corevideo::core::kNativeMediaCoreCapabilities);
  expectAllStringsPresent(source, corevideo::core::kRequiredMvpCapabilities);
}

TEST(ContractParity, BridgeEnvelopeTypesMatchTypeScriptProtocol) {
  const std::string source = readRepoFile("src/engine/nativeBridgeProtocol.ts");
  ASSERT_FALSE(source.empty());
  expectAllStringsPresent(source, corevideo::core::kNativeBridgeCommandTypes);
  EXPECT_NE(source.find("id: string"), std::string::npos);
  EXPECT_NE(source.find("ok: true"), std::string::npos);
  EXPECT_NE(source.find("ok: false"), std::string::npos);
}

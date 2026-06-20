#include "modules/Interfaces.h"
#include "modules/NdiSourceName.h"

#include <gtest/gtest.h>

namespace {
using namespace corevideo::modules;
}  // namespace

TEST(NdiSourceName, CanonicalFormIsParsedAndMachineUpperCased) {
  const auto parsed = parseNdiSourceName("studio-mac (Program)");
  EXPECT_TRUE(parsed.valid);
  EXPECT_EQ(parsed.machineName, "STUDIO-MAC");
  EXPECT_EQ(parsed.programName, "Program");
  EXPECT_EQ(parsed.canonical, "STUDIO-MAC (Program)");
  EXPECT_TRUE(parsed.errors.empty());
}

TEST(NdiSourceName, BareProgramNameGetsCorevideoPlaceholder) {
  const auto parsed = parseNdiSourceName("Keynote Stage");
  EXPECT_TRUE(parsed.valid);
  EXPECT_EQ(parsed.machineName, "COREVIDEO");
  EXPECT_EQ(parsed.programName, "Keynote Stage");
  EXPECT_EQ(parsed.canonical, "COREVIDEO (Keynote Stage)");
  ASSERT_FALSE(parsed.warnings.empty());
}

TEST(NdiSourceName, EmptyInputIsRejected) {
  const auto parsed = parseNdiSourceName("   ");
  EXPECT_FALSE(parsed.valid);
  ASSERT_FALSE(parsed.errors.empty());
  EXPECT_EQ(parsed.errors.front(), "NDI source name is required.");
}

TEST(NdiSourceName, ReservedCharactersAreRejected) {
  const auto parsed = parseNdiSourceName("Bad/Name");
  EXPECT_FALSE(parsed.valid);
  bool sawInvalidChars = false;
  for (const auto& error : parsed.errors) {
    if (error == "NDI source name contains invalid characters.") {
      sawInvalidChars = true;
    }
  }
  EXPECT_TRUE(sawInvalidChars);
}

TEST(NdiSourceName, OverlongNameIsRejected) {
  const std::string longName(kNdiMaxSourceNameLength + 1, 'A');
  const auto parsed = parseNdiSourceName(longName);
  EXPECT_FALSE(parsed.valid);
  ASSERT_FALSE(parsed.errors.empty());
}

TEST(NdiSourceName, EmptyParenthesizedProgramIsRejected) {
  const auto parsed = parseNdiSourceName("STUDIO ()");
  EXPECT_FALSE(parsed.valid);
}

TEST(NdiSourceName, ConventionalPreviewNameHasNoShortNameWarning) {
  const auto parsed = parseNdiSourceName("STUDIO (Preview)");
  EXPECT_TRUE(parsed.valid);
  for (const auto& warning : parsed.warnings) {
    EXPECT_EQ(warning.find("Short NDI program names"), std::string::npos);
  }
}

// Stub build: the runtime probe is compiled out and the factory must return
// nullptr so the synthetic sender (and module composition) stays unchanged.
TEST(NdiOutputSenderFactory, ReturnsNullptrWhenRuntimeAbsentInStubBuild) {
  auto sender = createNdiOutputSender();
#if !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_NDI_OUTPUT
  // In a dev build the factory still returns nullptr when no libNDI runtime is
  // installed on the machine running the tests.
  if (sender) {
    SUCCEED() << "libNDI runtime detected on this dev machine.";
  } else {
    SUCCEED() << "libNDI runtime absent; factory returned nullptr as expected.";
  }
#else
  EXPECT_EQ(sender, nullptr);
#endif
}

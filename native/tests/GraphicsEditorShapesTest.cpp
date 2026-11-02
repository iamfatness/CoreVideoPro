#include "graphics/GraphicsTemplate.h"
#include "gtest/gtest.h"
#include <filesystem>
#include <fstream>
#include <sstream>

// Shapes the shell's GraphicsTemplateEditor emits (gradient-stop bindings, renamed fields, an extra bound text line
// with a number-bound fontSize and collapseWhenEmpty) must be accepted by the native validator.
TEST(GraphicsEditorShapes, EditorOutputShapesLoadNatively) {
  for (const char* name : {"gradient-stop-binding", "renamed-field", "location-third-line"}) {
    std::ifstream in(std::filesystem::path(COREVIDEO_REPO_ROOT) / "native" / "tests" / "fixtures" / "graphics-editor" / (std::string(name) + ".json"), std::ios::binary);
    ASSERT_TRUE(in.good()) << name;
    std::ostringstream text; text << in.rdbuf();
    std::string error;
    ASSERT_TRUE(corevideo::graphics::GraphicsTemplate::load(text.str(), error)) << name << ": " << error;
  }
}

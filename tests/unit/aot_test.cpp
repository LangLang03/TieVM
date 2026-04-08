#include <gtest/gtest.h>

#include "test_helpers.hpp"

namespace tie::vm {

TEST(AotTest, EmitMetadataFiles) {
    AotPipeline pipeline;
    AotUnit unit;
    unit.module_name = "test.aot";
    unit.ir_payload = {1, 2, 3, 4};
    unit.metadata = {{"arch", "x64"}, {"opt", "O2"}};
    ASSERT_TRUE(pipeline.AddUnit(std::move(unit)).ok());

    const auto out_dir = test::TempPath("aotmeta");
    ASSERT_TRUE(pipeline.EmitMetadataDirectory(out_dir).ok());
    EXPECT_TRUE(std::filesystem::exists(out_dir / "test.aot.aotmeta"));
}

}  // namespace tie::vm

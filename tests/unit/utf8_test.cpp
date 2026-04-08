#include <gtest/gtest.h>

#include "tie/vm/utf8/utf8.hpp"

namespace tie::vm {

TEST(Utf8Test, ValidateStrictUtf8) {
    EXPECT_TRUE(utf8::Validate("hello世界").ok());
    const std::string invalid = std::string("a") + static_cast<char>(0xFF);
    EXPECT_FALSE(utf8::Validate(invalid).ok());
}

TEST(Utf8Test, DecodeAndCount) {
    const std::string text = "\xE4\xBD\xA0"
                             "a"
                             "\xE5\xA5\xBD";
    auto count_or = utf8::CountCodePoints(text);
    ASSERT_TRUE(count_or.ok()) << count_or.status().message();
    EXPECT_EQ(count_or.value(), 3u);

    auto cps_or = utf8::DecodeCodePoints(text);
    ASSERT_TRUE(cps_or.ok()) << cps_or.status().message();
    EXPECT_EQ(cps_or.value().size(), 3u);
}

}  // namespace tie::vm

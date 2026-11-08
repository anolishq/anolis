#include "automation/parameter_types.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

#include "automation/parameter_value_bridge.hpp"

namespace anolis {
namespace automation {
namespace {

TEST(ParameterTypesTest, TypeStringRoundTrip) {
    const auto double_type = parameter_type_from_string("double");
    ASSERT_TRUE(double_type.has_value());
    EXPECT_EQ(ParameterType::DOUBLE, *double_type);
    EXPECT_STREQ("double", parameter_type_to_string(*double_type));

    const auto int64_type = parameter_type_from_string("int64");
    ASSERT_TRUE(int64_type.has_value());
    EXPECT_EQ(ParameterType::INT64, *int64_type);
    EXPECT_STREQ("int64", parameter_type_to_string(*int64_type));

    const auto bool_type = parameter_type_from_string("bool");
    ASSERT_TRUE(bool_type.has_value());
    EXPECT_EQ(ParameterType::BOOL, *bool_type);
    EXPECT_STREQ("bool", parameter_type_to_string(*bool_type));

    const auto string_type = parameter_type_from_string("string");
    ASSERT_TRUE(string_type.has_value());
    EXPECT_EQ(ParameterType::STRING, *string_type);
    EXPECT_STREQ("string", parameter_type_to_string(*string_type));

    EXPECT_FALSE(parameter_type_from_string("bytes").has_value());
}

TEST(ParameterTypesTest, TypeMatchValidation) {
    EXPECT_TRUE(parameter_value_matches_type(ParameterType::DOUBLE, ParameterValue{10.5}));
    EXPECT_TRUE(parameter_value_matches_type(ParameterType::INT64, ParameterValue{int64_t{42}}));
    EXPECT_TRUE(parameter_value_matches_type(ParameterType::BOOL, ParameterValue{true}));
    EXPECT_TRUE(parameter_value_matches_type(ParameterType::STRING, ParameterValue{std::string{"AUTO"}}));

    EXPECT_FALSE(parameter_value_matches_type(ParameterType::DOUBLE, ParameterValue{int64_t{42}}));
    EXPECT_FALSE(parameter_value_matches_type(ParameterType::INT64, ParameterValue{true}));
    EXPECT_FALSE(parameter_value_matches_type(ParameterType::BOOL, ParameterValue{std::string{"false"}}));
}

TEST(ParameterTypesTest, ValueToStringUsesCanonicalEncoding) {
    EXPECT_EQ("3.140000", parameter_value_to_string(ParameterValue{3.14}));
    EXPECT_EQ("17", parameter_value_to_string(ParameterValue{int64_t{17}}));
    EXPECT_EQ("true", parameter_value_to_string(ParameterValue{true}));
    EXPECT_EQ("manual", parameter_value_to_string(ParameterValue{std::string{"manual"}}));
}

TEST(ParameterTypesTest, BridgeConvertsParameterValueToTypedValue) {
    events::TypedValue typed = parameter_value_to_typed_value(ParameterValue{int64_t{7}});
    EXPECT_TRUE(std::holds_alternative<int64_t>(typed));
    EXPECT_EQ(int64_t{7}, std::get<int64_t>(typed));
}

TEST(ParameterTypesTest, BridgeConvertsTypedValueToParameterValue) {
    std::string error;
    auto converted = parameter_value_from_typed_value(events::TypedValue{uint64_t{123}}, &error);
    ASSERT_TRUE(converted.has_value()) << error;
    EXPECT_TRUE(std::holds_alternative<int64_t>(*converted));
    EXPECT_EQ(int64_t{123}, std::get<int64_t>(*converted));
}

TEST(ParameterTypesTest, BridgeRejectsUint64Overflow) {
    std::string error;
    auto converted = parameter_value_from_typed_value(events::TypedValue{std::numeric_limits<uint64_t>::max()}, &error);
    EXPECT_FALSE(converted.has_value());
    EXPECT_NE(std::string::npos, error.find("exceeds int64 range"));
}

// A bytes value is its own TypedValue alternative (#594), so it names itself
// "bytes" and does not pass for a string parameter.
TEST(ParameterTypesTest, BytesTypedValueIsNotAStringParameter) {
    const events::TypedValue bytes = events::BytesValue{std::string("\xFF\x00", 2)};
    EXPECT_STREQ("bytes", events::value_type_name(bytes));
    EXPECT_FALSE(events::values_equal(bytes, events::TypedValue{std::string("\xFF\x00", 2)}));

    std::string error;
    EXPECT_FALSE(parameter_value_from_typed_value(bytes, &error).has_value());
    EXPECT_NE(std::string::npos, error.find("Unsupported"));
}

}  // namespace
}  // namespace automation
}  // namespace anolis

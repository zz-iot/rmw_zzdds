// A participant has one Topic per name, so a context's publishers and
// subscriptions of a topic share one, which the last of them deletes.

#include <gtest/gtest.h>

#include "rcutils/allocator.h"
#include "rmw/enclave.h"
#include "rmw/error_handling.h"
#include "rmw/rmw.h"
#include "rmw_zzdds_test/msg/nested_record.hpp"
#include "rmw_zzdds_test/msg/primitive_record.hpp"
#include "rosidl_typesupport_zzdds_cpp/message_type_support.hpp"

namespace
{
class SharedTopic : public ::testing::Test
{
protected:
  void SetUp() override
  {
    allocator_ = rcutils_get_default_allocator();
    options_ = rmw_get_zero_initialized_init_options();
    ASSERT_EQ(RMW_RET_OK, rmw_init_options_init(&options_, allocator_));
    ASSERT_EQ(RMW_RET_OK, rmw_enclave_options_copy("/test", &allocator_, &options_.enclave));
    context_ = rmw_get_zero_initialized_context();
    ASSERT_EQ(RMW_RET_OK, rmw_init(&options_, &context_));
    node_ = rmw_create_node(&context_, "shared_topic", "/rmw_zzdds");
    ASSERT_NE(nullptr, node_) << rmw_get_error_string().str;
  }

  void TearDown() override
  {
    if (node_ != nullptr) {EXPECT_EQ(RMW_RET_OK, rmw_destroy_node(node_));}
    EXPECT_EQ(RMW_RET_OK, rmw_shutdown(&context_));
    EXPECT_EQ(RMW_RET_OK, rmw_context_fini(&context_));
    EXPECT_EQ(RMW_RET_OK, rmw_init_options_fini(&options_));
  }

  template<typename Message>
  rmw_publisher_t * publisher(const char * topic)
  {
    const auto options = rmw_get_default_publisher_options();
    return rmw_create_publisher(
      node_, rosidl_typesupport_zzdds_cpp::get_message_type_support_handle<Message>(), topic,
      &rmw_qos_profile_default, &options);
  }

  template<typename Message>
  rmw_subscription_t * subscription(const char * topic)
  {
    const auto options = rmw_get_default_subscription_options();
    return rmw_create_subscription(
      node_, rosidl_typesupport_zzdds_cpp::get_message_type_support_handle<Message>(), topic,
      &rmw_qos_profile_default, &options);
  }

  rcutils_allocator_t allocator_{};
  rmw_init_options_t options_{};
  rmw_context_t context_{};
  rmw_node_t * node_{nullptr};
};

using Primitive = rmw_zzdds_test::msg::PrimitiveRecord;
using Nested = rmw_zzdds_test::msg::NestedRecord;

TEST_F(SharedTopic, PublishersAndSubscriptionsOfATopicShareIt)
{
  rmw_publisher_t * first = publisher<Primitive>("/shared");
  ASSERT_NE(nullptr, first) << rmw_get_error_string().str;
  rmw_publisher_t * second = publisher<Primitive>("/shared");
  ASSERT_NE(nullptr, second) << rmw_get_error_string().str;
  rmw_subscription_t * reader = subscription<Primitive>("/shared");
  ASSERT_NE(nullptr, reader) << rmw_get_error_string().str;

  // Destroying some leaves the Topic to the others.
  EXPECT_EQ(RMW_RET_OK, rmw_destroy_publisher(node_, first));
  rmw_subscription_t * another_reader = subscription<Primitive>("/shared");
  ASSERT_NE(nullptr, another_reader) << rmw_get_error_string().str;
  EXPECT_EQ(RMW_RET_OK, rmw_destroy_subscription(node_, reader));
  EXPECT_EQ(RMW_RET_OK, rmw_destroy_publisher(node_, second));
  EXPECT_EQ(RMW_RET_OK, rmw_destroy_subscription(node_, another_reader));

  // The last one deleted it, and the topic can be used again.
  rmw_publisher_t * again = publisher<Primitive>("/shared");
  ASSERT_NE(nullptr, again) << rmw_get_error_string().str;
  EXPECT_EQ(RMW_RET_OK, rmw_destroy_publisher(node_, again));
}

TEST_F(SharedTopic, AnotherTypeOnATopicInUseIsRefused)
{
  rmw_publisher_t * typed = publisher<Primitive>("/typed");
  ASSERT_NE(nullptr, typed) << rmw_get_error_string().str;
  EXPECT_EQ(nullptr, publisher<Nested>("/typed"));
  rmw_reset_error();
  EXPECT_EQ(nullptr, subscription<Nested>("/typed"));
  rmw_reset_error();
  EXPECT_EQ(RMW_RET_OK, rmw_destroy_publisher(node_, typed));

  // Once its last user is gone, the topic takes another type.
  rmw_subscription_t * retyped = subscription<Nested>("/typed");
  ASSERT_NE(nullptr, retyped) << rmw_get_error_string().str;
  EXPECT_EQ(RMW_RET_OK, rmw_destroy_subscription(node_, retyped));
}
}  // namespace

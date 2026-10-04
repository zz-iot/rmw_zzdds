#include <atomic>
#include <chrono>
#include <functional>
#include <thread>

#include <gtest/gtest.h>

#include "rcutils/allocator.h"
#include "rmw/enclave.h"
#include "rmw/events_statuses/matched.h"
#include "rmw/rmw.h"
#include "rmw/subscription_content_filter_options.h"
#include "rmw_zzdds_test/msg/primitive_record.hpp"
#include "rosidl_typesupport_zzdds_cpp/message_type_support.hpp"

namespace
{
void count_events(const void * data, size_t count)
{
  static_cast<std::atomic_size_t *>(const_cast<void *>(data))->fetch_add(count);
}

bool wait_until(const std::function<bool()> & done)
{
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < deadline) {
    if (done()) {return true;}
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return done();
}

size_t matched_publishers(const rmw_subscription_t * subscription)
{
  size_t count = 0U;
  EXPECT_EQ(RMW_RET_OK, rmw_subscription_count_matched_publishers(subscription, &count));
  return count;
}

// A content-filter expression change replaces the subscription's DDS reader.
// The subscription's matched event must continue across the replacement: the
// already-matched publisher is not reported as a new match, and a publisher
// that appears afterwards still reaches the registered callback.
TEST(EventTransport, subscription_matched_continues_across_content_filter_change)
{
  auto allocator = rcutils_get_default_allocator();
  auto options = rmw_get_zero_initialized_init_options();
  ASSERT_EQ(RMW_RET_OK, rmw_init_options_init(&options, allocator));
  ASSERT_EQ(RMW_RET_OK, rmw_enclave_options_copy("/test", &allocator, &options.enclave));
  auto context = rmw_get_zero_initialized_context();
  ASSERT_EQ(RMW_RET_OK, rmw_init(&options, &context));
  rmw_node_t * node = rmw_create_node(&context, "content_filter_events", "/rmw_zzdds");
  ASSERT_NE(nullptr, node);
  const auto * type_support = rosidl_typesupport_zzdds_cpp::get_message_type_support_handle<
    rmw_zzdds_test::msg::PrimitiveRecord>();
  const char * topic = "/content_filter_events";

  auto qos = rmw_qos_profile_default;
  qos.reliability = RMW_QOS_POLICY_RELIABILITY_RELIABLE;
  const auto subscription_options = rmw_get_default_subscription_options();
  rmw_subscription_t * subscription = rmw_create_subscription(
    node, type_support, topic, &qos, &subscription_options);
  ASSERT_NE(nullptr, subscription);
  rmw_event_t event = rmw_get_zero_initialized_event();
  ASSERT_EQ(
    RMW_RET_OK, rmw_subscription_event_init(&event, subscription, RMW_EVENT_SUBSCRIPTION_MATCHED));
  std::atomic_size_t callback_count{0U};
  ASSERT_EQ(RMW_RET_OK, rmw_event_set_callback(&event, count_events, &callback_count));

  const auto publisher_options = rmw_get_default_publisher_options();
  rmw_publisher_t * first = rmw_create_publisher(
    node, type_support, topic, &qos, &publisher_options);
  ASSERT_NE(nullptr, first);
  ASSERT_TRUE(wait_until([&] {return matched_publishers(subscription) == 1U;}));
  ASSERT_TRUE(wait_until([&] {return callback_count.load() >= 1U;}));
  rmw_matched_status_t status{};
  bool taken = false;
  ASSERT_EQ(RMW_RET_OK, rmw_take_event(&event, &status, &taken));
  EXPECT_TRUE(taken);
  EXPECT_EQ(1U, status.total_count);
  EXPECT_EQ(1U, status.total_count_change);
  EXPECT_EQ(1U, status.current_count);
  EXPECT_EQ(1, status.current_count_change);

  // An expression (not just parameter) change replaces the reader.
  auto filter = rmw_get_zero_initialized_content_filter_options();
  ASSERT_EQ(
    RMW_RET_OK,
    rmw_subscription_content_filter_options_init("id > 10", 0U, nullptr, &allocator, &filter));
  ASSERT_EQ(RMW_RET_OK, rmw_subscription_set_content_filter(subscription, &filter));
  EXPECT_EQ(RMW_RET_OK, rmw_subscription_content_filter_options_fini(&filter, &allocator));
  EXPECT_TRUE(subscription->is_cft_enabled);
  ASSERT_TRUE(wait_until([&] {return matched_publishers(subscription) == 1U;}));

  // The replacement reader re-matched the same publisher: no new match.
  ASSERT_EQ(RMW_RET_OK, rmw_take_event(&event, &status, &taken));
  EXPECT_TRUE(taken);
  EXPECT_EQ(1U, status.total_count);
  EXPECT_EQ(0U, status.total_count_change);
  EXPECT_EQ(1U, status.current_count);
  EXPECT_EQ(0, status.current_count_change);

  // A new publisher is still reported through the callback.
  const size_t callbacks_before = callback_count.load();
  rmw_publisher_t * second = rmw_create_publisher(
    node, type_support, topic, &qos, &publisher_options);
  ASSERT_NE(nullptr, second);
  ASSERT_TRUE(wait_until([&] {return matched_publishers(subscription) == 2U;}));
  ASSERT_TRUE(wait_until([&] {return callback_count.load() > callbacks_before;}));
  ASSERT_EQ(RMW_RET_OK, rmw_take_event(&event, &status, &taken));
  EXPECT_TRUE(taken);
  EXPECT_EQ(2U, status.total_count);
  EXPECT_EQ(1U, status.total_count_change);
  EXPECT_EQ(2U, status.current_count);
  EXPECT_EQ(1, status.current_count_change);

  EXPECT_EQ(RMW_RET_OK, rmw_event_fini(&event));
  EXPECT_EQ(RMW_RET_OK, rmw_destroy_publisher(node, second));
  EXPECT_EQ(RMW_RET_OK, rmw_destroy_publisher(node, first));
  EXPECT_EQ(RMW_RET_OK, rmw_destroy_subscription(node, subscription));
  EXPECT_EQ(RMW_RET_OK, rmw_destroy_node(node));
  EXPECT_EQ(RMW_RET_OK, rmw_shutdown(&context));
  EXPECT_EQ(RMW_RET_OK, rmw_context_fini(&context));
  EXPECT_EQ(RMW_RET_OK, rmw_init_options_fini(&options));
}
}  // namespace

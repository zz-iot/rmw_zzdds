#include <atomic>
#include <chrono>
#include <functional>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "rcutils/allocator.h"
#include "rmw/enclave.h"
#include "rmw/error_handling.h"
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

void publish_id(const rmw_publisher_t * publisher, uint32_t id)
{
  rmw_zzdds_test::msg::PrimitiveRecord message;
  message.id = id;
  ASSERT_EQ(RMW_RET_OK, rmw_publish(publisher, &message, nullptr)) << rmw_get_error_string().str;
}

// Takes samples until `count` have arrived or 10 s pass; returns their ids.
std::vector<uint32_t> take_ids(const rmw_subscription_t * subscription, size_t count)
{
  std::vector<uint32_t> ids;
  (void)wait_until([&] {
      rmw_zzdds_test::msg::PrimitiveRecord message;
      bool taken = false;
      while (rmw_take(subscription, &message, &taken, nullptr) == RMW_RET_OK && taken) {
        ids.push_back(message.id);
      }
      return ids.size() >= count;
    });
  return ids;
}

void set_filter(
  rmw_subscription_t * subscription, const char * expression, const char * parameter)
{
  auto allocator = rcutils_get_default_allocator();
  auto filter = rmw_get_zero_initialized_content_filter_options();
  const char * parameters[] = {parameter};
  ASSERT_EQ(
    RMW_RET_OK,
    rmw_subscription_content_filter_options_init(
      expression, parameter == nullptr ? 0U : 1U, parameter == nullptr ? nullptr : parameters,
      &allocator, &filter));
  EXPECT_EQ(RMW_RET_OK, rmw_subscription_set_content_filter(subscription, &filter))
    << rmw_get_error_string().str;
  EXPECT_EQ(RMW_RET_OK, rmw_subscription_content_filter_options_fini(&filter, &allocator));
}

// rmw_subscription_set_content_filter changes the filter in place, on the same
// DDS reader: the subscription stays matched (no new match is reported), a
// sample received before the change is still there to take, the new filter
// applies to samples received afterwards, an empty expression clears it, and
// matched events keep reaching the registered callback.
TEST(EventTransport, content_filter_changes_in_place)
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
  EXPECT_FALSE(subscription->is_cft_enabled);
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

  // A sample that has arrived but not been taken yet.
  publish_id(first, 5U);
  rmw_wait_set_t * wait_set = rmw_create_wait_set(&context, 1U);
  ASSERT_NE(nullptr, wait_set);
  void * subscription_entries[] = {subscription->data};
  rmw_subscriptions_t subscriptions{1U, subscription_entries};
  const rmw_time_t wait_timeout{10U, 0U};
  ASSERT_EQ(
    RMW_RET_OK,
    rmw_wait(&subscriptions, nullptr, nullptr, nullptr, nullptr, wait_set, &wait_timeout));
  ASSERT_NE(nullptr, subscription_entries[0]);
  EXPECT_EQ(RMW_RET_OK, rmw_destroy_wait_set(wait_set));

  // Add a filter (an expression change, not just parameters).
  set_filter(subscription, "id > %0", "10");
  EXPECT_TRUE(subscription->is_cft_enabled);
  auto current = rmw_get_zero_initialized_content_filter_options();
  ASSERT_EQ(RMW_RET_OK, rmw_subscription_get_content_filter(subscription, &allocator, &current));
  EXPECT_STREQ("id > %0", current.filter_expression);
  ASSERT_EQ(1U, current.expression_parameters.size);
  EXPECT_STREQ("10", current.expression_parameters.data[0]);
  EXPECT_EQ(RMW_RET_OK, rmw_subscription_content_filter_options_fini(&current, &allocator));

  // Still the same match: nothing new to report.
  EXPECT_EQ(1U, matched_publishers(subscription));
  ASSERT_EQ(RMW_RET_OK, rmw_take_event(&event, &status, &taken));
  EXPECT_TRUE(taken);
  EXPECT_EQ(1U, status.total_count);
  EXPECT_EQ(0U, status.total_count_change);
  EXPECT_EQ(1U, status.current_count);
  EXPECT_EQ(0, status.current_count_change);

  // The sample from before the change survives; 7 is filtered out.
  publish_id(first, 7U);
  publish_id(first, 20U);
  EXPECT_EQ((std::vector<uint32_t>{5U, 20U}), take_ids(subscription, 2U));

  // An empty expression clears the filter.
  set_filter(subscription, "", nullptr);
  EXPECT_FALSE(subscription->is_cft_enabled);
  current = rmw_get_zero_initialized_content_filter_options();
  EXPECT_EQ(
    RMW_RET_UNSUPPORTED, rmw_subscription_get_content_filter(subscription, &allocator, &current));
  rmw_reset_error();
  publish_id(first, 3U);
  EXPECT_EQ((std::vector<uint32_t>{3U}), take_ids(subscription, 1U));

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

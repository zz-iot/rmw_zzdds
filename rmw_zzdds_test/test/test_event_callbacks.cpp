#include <atomic>
#include <chrono>
#include <functional>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "rcutils/allocator.h"
#include "rmw/enclave.h"
#include "rmw/error_handling.h"
#include "rmw/events_statuses/liveliness_changed.h"
#include "rmw/events_statuses/matched.h"
#include "rmw/rmw.h"
#include "rmw_zzdds_cpp/endpoint_impl.hpp"
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

// One node with a subscription and a subscription-matched event, no callback
// registered yet.
class EventCallbacks : public ::testing::Test
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
    node_ = rmw_create_node(&context_, "event_callbacks", "/rmw_zzdds");
    ASSERT_NE(nullptr, node_);
    type_support_ = rosidl_typesupport_zzdds_cpp::get_message_type_support_handle<
      rmw_zzdds_test::msg::PrimitiveRecord>();
    qos_ = rmw_qos_profile_default;
    qos_.reliability = RMW_QOS_POLICY_RELIABILITY_RELIABLE;
    const auto subscription_options = rmw_get_default_subscription_options();
    subscription_ = rmw_create_subscription(
      node_, type_support_, topic_, &qos_, &subscription_options);
    ASSERT_NE(nullptr, subscription_);
    event_ = rmw_get_zero_initialized_event();
    ASSERT_EQ(
      RMW_RET_OK,
      rmw_subscription_event_init(&event_, subscription_, RMW_EVENT_SUBSCRIPTION_MATCHED));
  }

  void TearDown() override
  {
    // Before destroying the publisher, whose unmatch would otherwise reach it.
    EXPECT_EQ(RMW_RET_OK, rmw_event_set_callback(&event_, nullptr, nullptr));
    EXPECT_EQ(RMW_RET_OK, rmw_event_fini(&event_));
    for (rmw_publisher_t * publisher : publishers_) {
      EXPECT_EQ(RMW_RET_OK, rmw_destroy_publisher(node_, publisher));
    }
    EXPECT_EQ(RMW_RET_OK, rmw_destroy_subscription(node_, subscription_));
    EXPECT_EQ(RMW_RET_OK, rmw_destroy_node(node_));
    EXPECT_EQ(RMW_RET_OK, rmw_shutdown(&context_));
    EXPECT_EQ(RMW_RET_OK, rmw_context_fini(&context_));
    EXPECT_EQ(RMW_RET_OK, rmw_init_options_fini(&options_));
  }

  // Creates `count` more publishers, each on its own thread, and waits until
  // the subscription matches all of them.
  void match_publishers(size_t count)
  {
    const size_t first = publishers_.size();
    publishers_.resize(first + count, nullptr);
    std::vector<std::thread> threads;
    for (size_t i = first; i < publishers_.size(); ++i) {
      threads.emplace_back(
        [this, i] {
          const auto publisher_options = rmw_get_default_publisher_options();
          publishers_[i] = rmw_create_publisher(
            node_, type_support_, topic_, &qos_, &publisher_options);
        });
    }
    for (std::thread & thread : threads) {thread.join();}
    for (rmw_publisher_t * publisher : publishers_) {ASSERT_NE(nullptr, publisher);}
    ASSERT_TRUE(
      wait_until(
        [&] {
          size_t matched = 0U;
          return rmw_subscription_count_matched_publishers(subscription_, &matched) ==
          RMW_RET_OK && matched == publishers_.size();
        }));
  }

  void expect_taken(size_t total, size_t total_change, size_t current, int32_t current_change)
  {
    rmw_matched_status_t status{};
    bool taken = false;
    ASSERT_EQ(RMW_RET_OK, rmw_take_event(&event_, &status, &taken));
    EXPECT_TRUE(taken);
    EXPECT_EQ(total, status.total_count);
    EXPECT_EQ(total_change, status.total_count_change);
    EXPECT_EQ(current, status.current_count);
    EXPECT_EQ(current_change, status.current_count_change);
  }

  // Whether a wait on `event` alone returns at once.
  bool ready(rmw_event_t & event)
  {
    rmw_wait_set_t * wait_set = rmw_create_wait_set(&context_, 1U);
    EXPECT_NE(nullptr, wait_set);
    void * entries[] = {&event};
    rmw_events_t events{1U, entries};
    const rmw_time_t timeout{0U, 0U};
    const rmw_ret_t ret = rmw_wait(nullptr, nullptr, nullptr, nullptr, &events, wait_set, &timeout);
    EXPECT_EQ(RMW_RET_OK, rmw_destroy_wait_set(wait_set));
    EXPECT_TRUE(ret == RMW_RET_OK || ret == RMW_RET_TIMEOUT);
    return ret == RMW_RET_OK && entries[0] != nullptr;
  }

  // The zzdds listener rmw_zzdds installed on the subscription's reader.
  DDS_DataReaderListener reader_listener()
  {
    auto * impl = static_cast<rmw_zzdds_cpp::SubscriptionImpl *>(subscription_->data);
    return DDS_DataReader_get_listener(impl->reader);
  }

  const char * topic_ = "/event_callbacks";
  rcutils_allocator_t allocator_{};
  rmw_init_options_t options_{};
  rmw_context_t context_{};
  rmw_node_t * node_ = nullptr;
  const rosidl_message_type_support_t * type_support_ = nullptr;
  rmw_qos_profile_t qos_{};
  rmw_subscription_t * subscription_ = nullptr;
  std::vector<rmw_publisher_t *> publishers_;
  rmw_event_t event_{};
  std::atomic_size_t callback_count_{0U};
};

// A status taken with no callback registered is still the subscription's
// status once a callback is registered: the counts carry over and the taken
// change is not reported again.
TEST_F(EventCallbacks, status_taken_before_registering_a_callback_carries_over)
{
  match_publishers(1U);
  expect_taken(1U, 1U, 1U, 1);
  ASSERT_EQ(RMW_RET_OK, rmw_event_set_callback(&event_, count_events, &callback_count_));
  EXPECT_EQ(0U, callback_count_.load());
  expect_taken(1U, 0U, 1U, 0);
}

// Changes from before a callback is registered are reported to it when it is
// registered (rmw_event_set_callback: number_of_events may then be > 1), and
// then taken.
TEST_F(EventCallbacks, changes_before_registration_reach_the_new_callback)
{
  match_publishers(2U);
  ASSERT_EQ(RMW_RET_OK, rmw_event_set_callback(&event_, count_events, &callback_count_));
  EXPECT_EQ(2U, callback_count_.load());
  expect_taken(2U, 2U, 2U, 2);
}

// The counts reported to the callback add up to the number of events when
// publishers match concurrently. A read that finds several changes reports
// them all; this test cannot force that interleaving (each local match's
// listener usually reads before the next match), so it checks the total
// rather than reproducing the coalescing.
TEST_F(EventCallbacks, concurrent_matches_are_all_reported)
{
  ASSERT_EQ(RMW_RET_OK, rmw_event_set_callback(&event_, count_events, &callback_count_));
  match_publishers(4U);
  ASSERT_TRUE(wait_until([&] {return callback_count_.load() >= 4U;}));
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  EXPECT_EQ(4U, callback_count_.load());
  expect_taken(4U, 4U, 4U, 4);
}

// Registering a callback again while reported events are still untaken does
// not report them again.
TEST_F(EventCallbacks, re_registering_does_not_repeat_reported_events)
{
  ASSERT_EQ(RMW_RET_OK, rmw_event_set_callback(&event_, count_events, &callback_count_));
  match_publishers(1U);
  ASSERT_TRUE(wait_until([&] {return callback_count_.load() >= 1U;}));
  ASSERT_EQ(RMW_RET_OK, rmw_event_set_callback(&event_, count_events, &callback_count_));
  EXPECT_EQ(1U, callback_count_.load());
  expect_taken(1U, 1U, 1U, 1);
}

// Clearing the callback does not lose changes the callback path already
// collected.
TEST_F(EventCallbacks, changes_survive_clearing_the_callback)
{
  ASSERT_EQ(RMW_RET_OK, rmw_event_set_callback(&event_, count_events, &callback_count_));
  match_publishers(1U);
  ASSERT_TRUE(wait_until([&] {return callback_count_.load() >= 1U;}));
  ASSERT_EQ(RMW_RET_OK, rmw_event_set_callback(&event_, nullptr, nullptr));
  expect_taken(1U, 1U, 1U, 1);
  expect_taken(1U, 0U, 1U, 0);
}
// Events while no callback is registered (after one was cleared) are reported
// to the next callback; the ones already reported are not.
TEST_F(EventCallbacks, events_after_clearing_reach_the_next_callback)
{
  ASSERT_EQ(RMW_RET_OK, rmw_event_set_callback(&event_, count_events, &callback_count_));
  match_publishers(1U);
  ASSERT_TRUE(wait_until([&] {return callback_count_.load() >= 1U;}));
  ASSERT_EQ(RMW_RET_OK, rmw_event_set_callback(&event_, nullptr, nullptr));
  // An unmatch and a match while no callback is registered.
  ASSERT_EQ(RMW_RET_OK, rmw_destroy_publisher(node_, publishers_.back()));
  publishers_.pop_back();
  ASSERT_TRUE(
    wait_until(
      [&] {
        size_t matched = 1U;
        return rmw_subscription_count_matched_publishers(subscription_, &matched) ==
        RMW_RET_OK && matched == 0U;
      }));
  match_publishers(1U);
  ASSERT_EQ(RMW_RET_OK, rmw_event_set_callback(&event_, count_events, &callback_count_));
  EXPECT_EQ(3U, callback_count_.load());
  expect_taken(2U, 2U, 1U, 1);
}

// zzdds calls a listener after releasing its lock, so a take can read a change
// before the change's listener runs. That listener then finds nothing new: it
// neither triggers the event nor calls the callback. The test replays the late
// listener call by calling the installed listener itself.
TEST_F(EventCallbacks, a_listener_running_after_its_change_was_taken_reports_nothing)
{
  ASSERT_EQ(RMW_RET_OK, rmw_event_set_callback(&event_, count_events, &callback_count_));
  match_publishers(1U);
  ASSERT_TRUE(wait_until([&] {return callback_count_.load() >= 1U;}));
  expect_taken(1U, 1U, 1U, 1);
  ASSERT_FALSE(ready(event_));

  const DDS_DataReaderListener listener = reader_listener();
  ASSERT_NE(nullptr, listener.on_subscription_matched);
  DDS_SubscriptionMatchedStatus late{};
  late.total_count = 1;
  late.total_count_change = 1;
  late.current_count = 1;
  late.current_count_change = 1;
  listener.on_subscription_matched(nullptr, &late, listener.listener_data);

  EXPECT_EQ(1U, callback_count_.load());
  EXPECT_FALSE(ready(event_));
  expect_taken(1U, 0U, 1U, 0);
}

// The same for liveliness changed, whose listener compares the status zzdds
// passed with the one at the last take (or, as here, before any change): a
// listener passing that status counts nothing.
TEST_F(EventCallbacks, a_liveliness_listener_passing_the_taken_status_reports_nothing)
{
  rmw_event_t liveliness = rmw_get_zero_initialized_event();
  ASSERT_EQ(
    RMW_RET_OK,
    rmw_subscription_event_init(&liveliness, subscription_, RMW_EVENT_LIVELINESS_CHANGED));
  std::atomic_size_t liveliness_count{0U};
  ASSERT_EQ(RMW_RET_OK, rmw_event_set_callback(&liveliness, count_events, &liveliness_count));
  ASSERT_FALSE(ready(liveliness));

  const DDS_DataReaderListener listener = reader_listener();
  ASSERT_NE(nullptr, listener.on_liveliness_changed);
  DDS_LivelinessChangedStatus late{};
  listener.on_liveliness_changed(nullptr, &late, listener.listener_data);

  EXPECT_EQ(0U, liveliness_count.load());
  EXPECT_FALSE(ready(liveliness));
  EXPECT_EQ(RMW_RET_OK, rmw_event_set_callback(&liveliness, nullptr, nullptr));
  EXPECT_EQ(RMW_RET_OK, rmw_event_fini(&liveliness));
}
}  // namespace

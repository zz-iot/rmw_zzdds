#include <gtest/gtest.h>

#include <cstring>
#include <cstdlib>
#include <limits>

#include "rcutils/allocator.h"
#include "rcutils/types/string_array.h"
#include "rmw/enclave.h"
#include "rmw/error_handling.h"
#include "rmw/rmw.h"

namespace
{

class RmwLifecycle : public ::testing::Test
{
protected:
  void SetUp() override
  {
    allocator_ = rcutils_get_default_allocator();
    options_ = rmw_get_zero_initialized_init_options();
    ASSERT_EQ(RMW_RET_OK, rmw_init_options_init(&options_, allocator_));
    ASSERT_EQ(RMW_RET_OK, rmw_enclave_options_copy("/test", &allocator_, &options_.enclave));
    context_ = rmw_get_zero_initialized_context();
  }

  void TearDown() override
  {
    if (context_.impl != nullptr) {
      const rmw_ret_t shutdown_ret = rmw_shutdown(&context_);
      const rmw_ret_t context_ret = rmw_context_fini(&context_);
      (void)shutdown_ret;
      (void)context_ret;
    }
    if (options_.implementation_identifier != nullptr) {
      const rmw_ret_t options_ret = rmw_init_options_fini(&options_);
      (void)options_ret;
    }
    rmw_reset_error();
  }

  rcutils_allocator_t allocator_{};
  rmw_init_options_t options_{};
  rmw_context_t context_{};
};

struct FailingAllocatorState
{
  size_t calls{0U};
  size_t live_allocations{0U};
  size_t fail_after{std::numeric_limits<size_t>::max()};
};

void * failing_allocate(size_t size, void * state_pointer)
{
  auto & state = *static_cast<FailingAllocatorState *>(state_pointer);
  if (state.calls++ >= state.fail_after) {
    return nullptr;
  }
  void * result = std::malloc(size);
  if (result != nullptr) {
    ++state.live_allocations;
  }
  return result;
}

void failing_deallocate(void * pointer, void * state_pointer)
{
  if (pointer != nullptr) {
    auto & state = *static_cast<FailingAllocatorState *>(state_pointer);
    --state.live_allocations;
    std::free(pointer);
  }
}

void * failing_reallocate(void * pointer, size_t size, void * state_pointer)
{
  auto & state = *static_cast<FailingAllocatorState *>(state_pointer);
  if (state.calls++ >= state.fail_after) {
    return nullptr;
  }
  void * result = std::realloc(pointer, size);
  if (result != nullptr && pointer == nullptr) {
    ++state.live_allocations;
  }
  return result;
}

void * failing_zero_allocate(size_t count, size_t size, void * state_pointer)
{
  auto & state = *static_cast<FailingAllocatorState *>(state_pointer);
  if (state.calls++ >= state.fail_after) {
    return nullptr;
  }
  void * result = std::calloc(count, size);
  if (result != nullptr) {
    ++state.live_allocations;
  }
  return result;
}

rcutils_allocator_t make_failing_allocator(FailingAllocatorState * state)
{
  return rcutils_allocator_t{
    failing_allocate,
    failing_deallocate,
    failing_reallocate,
    failing_zero_allocate,
    state};
}

TEST_F(RmwLifecycle, InitOptionsCopyOwnsNestedData)
{
  rmw_init_options_t copy = rmw_get_zero_initialized_init_options();
  ASSERT_EQ(RMW_RET_OK, rmw_init_options_copy(&options_, &copy));
  ASSERT_NE(options_.enclave, copy.enclave);
  EXPECT_STREQ(options_.enclave, copy.enclave);

  options_.enclave[1] = 'X';
  EXPECT_STREQ("/test", copy.enclave);
  EXPECT_EQ(RMW_RET_OK, rmw_init_options_fini(&copy));
}

TEST_F(RmwLifecycle, ContextOwnsAnIndependentOptionsCopy)
{
  ASSERT_EQ(RMW_RET_OK, rmw_init(&options_, &context_));
  ASSERT_NE(options_.enclave, context_.options.enclave);
  EXPECT_STREQ(options_.enclave, context_.options.enclave);
  EXPECT_EQ(RMW_RET_OK, rmw_shutdown(&context_));
  EXPECT_EQ(RMW_RET_OK, rmw_context_fini(&context_));
}

TEST_F(RmwLifecycle, ActiveEntitiesPreventContextFinalization)
{
  ASSERT_EQ(RMW_RET_OK, rmw_init(&options_, &context_));
  rmw_node_t * node = rmw_create_node(&context_, "lifecycle_node", "/test");
  ASSERT_NE(nullptr, node) << rmw_get_error_string().str;
  rmw_guard_condition_t * guard = rmw_create_guard_condition(&context_);
  ASSERT_NE(nullptr, guard) << rmw_get_error_string().str;

  EXPECT_EQ(RMW_RET_OK, rmw_shutdown(&context_));
  EXPECT_EQ(RMW_RET_INVALID_ARGUMENT, rmw_context_fini(&context_));
  rmw_reset_error();

  EXPECT_EQ(RMW_RET_OK, rmw_trigger_guard_condition(guard));
  EXPECT_EQ(RMW_RET_OK, rmw_destroy_guard_condition(guard));
  EXPECT_EQ(RMW_RET_OK, rmw_destroy_node(node));
  EXPECT_EQ(RMW_RET_OK, rmw_context_fini(&context_));
}

TEST_F(RmwLifecycle, WaitSetTimesOutAndConsumesTriggeredGuardCondition)
{
  ASSERT_EQ(RMW_RET_OK, rmw_init(&options_, &context_));
  rmw_guard_condition_t * guard = rmw_create_guard_condition(&context_);
  ASSERT_NE(nullptr, guard) << rmw_get_error_string().str;
  rmw_wait_set_t * wait_set = rmw_create_wait_set(&context_, 1U);
  ASSERT_NE(nullptr, wait_set) << rmw_get_error_string().str;

  void * entries[] = {guard->data};
  rmw_guard_conditions_t guards{1U, entries};
  const rmw_time_t zero_timeout{0U, 0U};
  EXPECT_EQ(
    RMW_RET_TIMEOUT,
    rmw_wait(nullptr, &guards, nullptr, nullptr, nullptr, wait_set, &zero_timeout));
  EXPECT_EQ(nullptr, entries[0]);

  ASSERT_EQ(RMW_RET_OK, rmw_trigger_guard_condition(guard));
  entries[0] = guard->data;
  EXPECT_EQ(
    RMW_RET_OK,
    rmw_wait(nullptr, &guards, nullptr, nullptr, nullptr, wait_set, &zero_timeout));
  EXPECT_EQ(guard->data, entries[0]);

  entries[0] = guard->data;
  EXPECT_EQ(
    RMW_RET_TIMEOUT,
    rmw_wait(nullptr, &guards, nullptr, nullptr, nullptr, wait_set, &zero_timeout));
  EXPECT_EQ(nullptr, entries[0]);

  EXPECT_EQ(RMW_RET_OK, rmw_destroy_wait_set(wait_set));
  EXPECT_EQ(RMW_RET_OK, rmw_destroy_guard_condition(guard));
  EXPECT_EQ(RMW_RET_OK, rmw_shutdown(&context_));
  EXPECT_EQ(RMW_RET_OK, rmw_context_fini(&context_));
}

TEST_F(RmwLifecycle, GraphGuardConditionTracksGraphChanges)
{
  ASSERT_EQ(RMW_RET_OK, rmw_init(&options_, &context_));
  rmw_node_t * node = rmw_create_node(&context_, "graph_node", "/test");
  ASSERT_NE(nullptr, node) << rmw_get_error_string().str;
  const rmw_guard_condition_t * graph_guard = rmw_node_get_graph_guard_condition(node);
  ASSERT_NE(nullptr, graph_guard) << rmw_get_error_string().str;
  rmw_wait_set_t * wait_set = rmw_create_wait_set(&context_, 1U);
  ASSERT_NE(nullptr, wait_set) << rmw_get_error_string().str;

  void * entries[] = {graph_guard->data};
  rmw_guard_conditions_t guards{1U, entries};
  const rmw_time_t zero_timeout{0U, 0U};

  // Creating the first node changed the graph before its guard was queried.
  EXPECT_EQ(
    RMW_RET_OK,
    rmw_wait(nullptr, &guards, nullptr, nullptr, nullptr, wait_set, &zero_timeout));
  EXPECT_EQ(graph_guard->data, entries[0]);

  entries[0] = graph_guard->data;
  EXPECT_EQ(
    RMW_RET_TIMEOUT,
    rmw_wait(nullptr, &guards, nullptr, nullptr, nullptr, wait_set, &zero_timeout));
  EXPECT_EQ(nullptr, entries[0]);

  rmw_node_t * second_node = rmw_create_node(&context_, "second_graph_node", "/test");
  ASSERT_NE(nullptr, second_node) << rmw_get_error_string().str;
  entries[0] = graph_guard->data;
  EXPECT_EQ(
    RMW_RET_OK,
    rmw_wait(nullptr, &guards, nullptr, nullptr, nullptr, wait_set, &zero_timeout));
  EXPECT_EQ(graph_guard->data, entries[0]);

  ASSERT_EQ(RMW_RET_OK, rmw_destroy_node(second_node));
  entries[0] = graph_guard->data;
  EXPECT_EQ(
    RMW_RET_OK,
    rmw_wait(nullptr, &guards, nullptr, nullptr, nullptr, wait_set, &zero_timeout));
  EXPECT_EQ(graph_guard->data, entries[0]);

  EXPECT_EQ(RMW_RET_OK, rmw_destroy_wait_set(wait_set));
  EXPECT_EQ(RMW_RET_OK, rmw_destroy_node(node));
  EXPECT_EQ(RMW_RET_OK, rmw_shutdown(&context_));
  EXPECT_EQ(RMW_RET_OK, rmw_context_fini(&context_));
}

TEST_F(RmwLifecycle, NodeNamesReflectTheLocalGraph)
{
  ASSERT_EQ(RMW_RET_OK, rmw_init(&options_, &context_));
  rmw_node_t * first = rmw_create_node(&context_, "first_node", "/one");
  rmw_node_t * second = rmw_create_node(&context_, "second_node", "/two");
  ASSERT_NE(nullptr, first) << rmw_get_error_string().str;
  ASSERT_NE(nullptr, second) << rmw_get_error_string().str;

  rcutils_string_array_t names = rcutils_get_zero_initialized_string_array();
  rcutils_string_array_t namespaces = rcutils_get_zero_initialized_string_array();
  rcutils_string_array_t enclaves = rcutils_get_zero_initialized_string_array();
  ASSERT_EQ(
    RMW_RET_OK,
    rmw_get_node_names_with_enclaves(first, &names, &namespaces, &enclaves));
  ASSERT_EQ(2U, names.size);
  ASSERT_EQ(names.size, namespaces.size);
  ASSERT_EQ(names.size, enclaves.size);

  bool found_first = false;
  bool found_second = false;
  for (size_t i = 0U; i < names.size; ++i) {
    EXPECT_STREQ("/test", enclaves.data[i]);
    found_first |= std::strcmp(names.data[i], "first_node") == 0 &&
      std::strcmp(namespaces.data[i], "/one") == 0;
    found_second |= std::strcmp(names.data[i], "second_node") == 0 &&
      std::strcmp(namespaces.data[i], "/two") == 0;
  }
  EXPECT_TRUE(found_first);
  EXPECT_TRUE(found_second);

  EXPECT_EQ(RCUTILS_RET_OK, rcutils_string_array_fini(&enclaves));
  EXPECT_EQ(RCUTILS_RET_OK, rcutils_string_array_fini(&namespaces));
  EXPECT_EQ(RCUTILS_RET_OK, rcutils_string_array_fini(&names));
  EXPECT_EQ(RMW_RET_OK, rmw_destroy_node(second));
  EXPECT_EQ(RMW_RET_OK, rmw_destroy_node(first));
  EXPECT_EQ(RMW_RET_OK, rmw_shutdown(&context_));
  EXPECT_EQ(RMW_RET_OK, rmw_context_fini(&context_));
}

TEST_F(RmwLifecycle, RejectsCreationAfterShutdown)
{
  ASSERT_EQ(RMW_RET_OK, rmw_init(&options_, &context_));
  ASSERT_EQ(RMW_RET_OK, rmw_shutdown(&context_));
  EXPECT_EQ(nullptr, rmw_create_node(&context_, "late_node", "/test"));
  rmw_reset_error();
  EXPECT_EQ(nullptr, rmw_create_guard_condition(&context_));
  rmw_reset_error();
  EXPECT_EQ(RMW_RET_OK, rmw_context_fini(&context_));
}

TEST_F(RmwLifecycle, RejectsSecurityEnforcement)
{
  options_.security_options.enforce_security = RMW_SECURITY_ENFORCEMENT_ENFORCE;
  EXPECT_EQ(RMW_RET_UNSUPPORTED, rmw_init(&options_, &context_));
  EXPECT_EQ(nullptr, context_.impl);
}

TEST_F(RmwLifecycle, RepeatedLifecycle)
{
  for (size_t i = 0; i < 25U; ++i) {
    SCOPED_TRACE(i);
    ASSERT_EQ(RMW_RET_OK, rmw_init(&options_, &context_));
    rmw_node_t * node = rmw_create_node(&context_, "repeat_node", "/test");
    ASSERT_NE(nullptr, node) << rmw_get_error_string().str;
    ASSERT_EQ(RMW_RET_OK, rmw_destroy_node(node));
    ASSERT_EQ(RMW_RET_OK, rmw_shutdown(&context_));
    ASSERT_EQ(RMW_RET_OK, rmw_context_fini(&context_));
    context_ = rmw_get_zero_initialized_context();
  }
}

TEST(RmwAllocatorFailure, InitRollsBackEveryRmwAllocation)
{
  FailingAllocatorState state;
  rcutils_allocator_t allocator = make_failing_allocator(&state);
  rmw_init_options_t options = rmw_get_zero_initialized_init_options();
  ASSERT_EQ(RMW_RET_OK, rmw_init_options_init(&options, allocator));
  ASSERT_EQ(RMW_RET_OK, rmw_enclave_options_copy("/failure", &allocator, &options.enclave));
  const size_t options_allocations = state.live_allocations;

  // Let the context's enclave copy succeed and fail its implementation object.
  state.fail_after = state.calls + 1U;
  rmw_context_t context = rmw_get_zero_initialized_context();
  EXPECT_EQ(RMW_RET_BAD_ALLOC, rmw_init(&options, &context));
  EXPECT_EQ(nullptr, context.impl);
  EXPECT_EQ(options_allocations, state.live_allocations);
  rmw_reset_error();

  state.fail_after = std::numeric_limits<size_t>::max();
  EXPECT_EQ(RMW_RET_OK, rmw_init_options_fini(&options));
  EXPECT_EQ(0U, state.live_allocations);
}

TEST(RmwAllocatorFailure, NodeRollsBackEveryPartialConstruction)
{
  FailingAllocatorState state;
  rcutils_allocator_t allocator = make_failing_allocator(&state);
  rmw_init_options_t options = rmw_get_zero_initialized_init_options();
  ASSERT_EQ(RMW_RET_OK, rmw_init_options_init(&options, allocator));
  ASSERT_EQ(RMW_RET_OK, rmw_enclave_options_copy("/failure", &allocator, &options.enclave));
  rmw_context_t context = rmw_get_zero_initialized_context();
  ASSERT_EQ(RMW_RET_OK, rmw_init(&options, &context));

  for (size_t successful_allocations = 0U; successful_allocations < 4U;
    ++successful_allocations)
  {
    SCOPED_TRACE(successful_allocations);
    const size_t before = state.live_allocations;
    state.fail_after = state.calls + successful_allocations;
    EXPECT_EQ(nullptr, rmw_create_node(&context, "failure_node", "/failure"));
    EXPECT_EQ(before, state.live_allocations);
    rmw_reset_error();
  }

  state.fail_after = std::numeric_limits<size_t>::max();
  rmw_node_t * node = rmw_create_node(&context, "recovery_node", "/failure");
  ASSERT_NE(nullptr, node);
  EXPECT_EQ(RMW_RET_OK, rmw_destroy_node(node));
  EXPECT_EQ(RMW_RET_OK, rmw_shutdown(&context));
  EXPECT_EQ(RMW_RET_OK, rmw_context_fini(&context));
  EXPECT_EQ(RMW_RET_OK, rmw_init_options_fini(&options));
  EXPECT_EQ(0U, state.live_allocations);
}

}  // namespace

// Copyright 2026 Zenzen IoT
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <zzdds_c.h>

#include <cstdint>
#include <limits>
#include <new>
#include <vector>

#include "rmw/error_handling.h"
#include "rmw/rmw.h"

#include "rmw_zzdds_cpp/allocator.hpp"
#include "rmw_zzdds_cpp/endpoint_impl.hpp"
#include "rmw_zzdds_cpp/event_impl.hpp"
#include "rmw_zzdds_cpp/guard_condition_impl.hpp"
#include "rmw_zzdds_cpp/identifier.hpp"
#include "rmw_zzdds_cpp/service_impl.hpp"
#include "rmw_zzdds_cpp/wait_set_impl.hpp"

namespace
{

using rmw_zzdds_cpp::ContextImpl;
using rmw_zzdds_cpp::ClientImpl;
using rmw_zzdds_cpp::GuardConditionImpl;
using rmw_zzdds_cpp::ServiceImpl;
using rmw_zzdds_cpp::SubscriptionImpl;
using rmw_zzdds_cpp::WaitSetImpl;

bool validate_array(size_t count, void ** entries, const char * name)
{
  if (count == 0U) {return true;}
  if (entries == nullptr) {
    RMW_SET_ERROR_MSG_WITH_FORMAT_STRING("%s array is null", name);
    return false;
  }
  for (size_t i = 0U; i < count; ++i) {
    if (entries[i] == nullptr) {
      RMW_SET_ERROR_MSG_WITH_FORMAT_STRING("%s array contains a null entry", name);
      return false;
    }
  }
  return true;
}

bool contains(const DDS_ConditionSeq & active, DDS_Condition condition)
{
  for (uint32_t i = 0U; i < active._length; ++i) {
    if (active._buffer[i] == condition) {return true;}
  }
  return false;
}

bool to_dds_duration(const rmw_time_t * timeout, DDS_Duration_t & result)
{
  if (timeout == nullptr) {
    result.sec = DDS_DURATION_INFINITE_SEC;
    result.nanosec = DDS_DURATION_INFINITE_NSEC;
    return true;
  }
  constexpr uint64_t billion = 1000000000ULL;
  const uint64_t extra_seconds = timeout->nsec / billion;
  if (timeout->sec > std::numeric_limits<uint64_t>::max() - extra_seconds) {
    RMW_SET_ERROR_MSG("wait timeout overflows after nanosecond normalization");
    return false;
  }
  const uint64_t seconds = timeout->sec + extra_seconds;
  if (seconds > static_cast<uint64_t>(std::numeric_limits<int32_t>::max())) {
    RMW_SET_ERROR_MSG("wait timeout exceeds the DDS duration range");
    return false;
  }
  result.sec = static_cast<int32_t>(seconds);
  result.nanosec = static_cast<uint32_t>(timeout->nsec % billion);
  return true;
}

}  // namespace

extern "C"
{
rmw_wait_set_t * rmw_create_wait_set(rmw_context_t * context, size_t max_conditions)
{
  if (context == nullptr || context->impl == nullptr) {
    RMW_SET_ERROR_MSG("an initialized context is required");
    return nullptr;
  }
  if (context->implementation_identifier != rmw_zzdds_cpp::identifier) {
    RMW_SET_ERROR_MSG("context belongs to a different RMW implementation");
    return nullptr;
  }
  auto * context_impl = reinterpret_cast<ContextImpl *>(context->impl);
  const std::lock_guard<std::mutex> lock(context_impl->mutex);
  if (context_impl->shutdown) {
    RMW_SET_ERROR_MSG("cannot create a wait set after context shutdown");
    return nullptr;
  }
  DDS_WaitSet native = zzdds_create_waitset();
  if (zzdds_waitset_is_nil(native)) {
    RMW_SET_ERROR_MSG("failed to create the zzdds wait set");
    return nullptr;
  }
  const auto allocator = context_impl->allocator;
  auto * impl = rmw_zzdds_cpp::allocate_object<WaitSetImpl>(
    allocator, WaitSetImpl{context_impl, native, max_conditions});
  auto * handle = rmw_zzdds_cpp::allocate_object<rmw_wait_set_t>(allocator);
  if (impl == nullptr || handle == nullptr) {
    rmw_zzdds_cpp::deallocate_object(allocator, handle);
    rmw_zzdds_cpp::deallocate_object(allocator, impl);
    zzdds_destroy_waitset(native);
    RMW_SET_ERROR_MSG("failed to allocate wait set resources");
    return nullptr;
  }
  handle->implementation_identifier = rmw_zzdds_cpp::identifier;
  handle->guard_conditions = nullptr;
  handle->data = impl;
  ++context_impl->active_entities;
  return handle;
}

rmw_ret_t rmw_destroy_wait_set(rmw_wait_set_t * wait_set)
{
  if (wait_set == nullptr || wait_set->data == nullptr) {
    RMW_SET_ERROR_MSG("a valid wait set is required");
    return RMW_RET_INVALID_ARGUMENT;
  }
  if (wait_set->implementation_identifier != rmw_zzdds_cpp::identifier) {
    RMW_SET_ERROR_MSG("wait set belongs to a different RMW implementation");
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
  }
  auto * impl = static_cast<WaitSetImpl *>(wait_set->data);
  auto * context = impl->context;
  const auto allocator = context->allocator;
  zzdds_destroy_waitset(impl->native);
  rmw_zzdds_cpp::deallocate_object(allocator, impl);
  rmw_zzdds_cpp::deallocate_object(allocator, wait_set);
  --context->active_entities;
  return RMW_RET_OK;
}

rmw_ret_t rmw_wait(
  rmw_subscriptions_t * subscriptions, rmw_guard_conditions_t * guard_conditions,
  rmw_services_t * services, rmw_clients_t * clients, rmw_events_t * events,
  rmw_wait_set_t * wait_set, const rmw_time_t * wait_timeout)
{
  if (wait_set == nullptr || wait_set->data == nullptr) {
    RMW_SET_ERROR_MSG("a valid wait set is required");
    return RMW_RET_INVALID_ARGUMENT;
  }
  if (wait_set->implementation_identifier != rmw_zzdds_cpp::identifier) {
    RMW_SET_ERROR_MSG("wait set belongs to a different RMW implementation");
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
  }
  const size_t subscription_count = subscriptions == nullptr ? 0U : subscriptions->subscriber_count;
  const size_t guard_count = guard_conditions == nullptr ? 0U :
    guard_conditions->guard_condition_count;
  const size_t service_count = services == nullptr ? 0U : services->service_count;
  const size_t client_count = clients == nullptr ? 0U : clients->client_count;
  const size_t event_count = events == nullptr ? 0U : events->event_count;
  if ((subscriptions != nullptr && !validate_array(
      subscription_count, subscriptions->subscribers, "subscription")) ||
    (guard_conditions != nullptr && !validate_array(
      guard_count, guard_conditions->guard_conditions, "guard condition")) ||
    (services != nullptr && !validate_array(service_count, services->services, "service")) ||
    (clients != nullptr && !validate_array(client_count, clients->clients, "client")) ||
    (events != nullptr && !validate_array(event_count, events->events, "event")))
  {
    return RMW_RET_INVALID_ARGUMENT;
  }
  auto * impl = static_cast<WaitSetImpl *>(wait_set->data);
  const size_t condition_count =
    subscription_count + guard_count + service_count + client_count + event_count;
  if (condition_count < subscription_count || condition_count < guard_count ||
    condition_count < service_count || condition_count < client_count || condition_count < event_count ||
    (impl->max_conditions != 0U && condition_count > impl->max_conditions))
  {
    RMW_SET_ERROR_MSG("condition count exceeds the wait set capacity");
    return RMW_RET_INVALID_ARGUMENT;
  }
  DDS_Duration_t timeout{};
  if (!to_dds_duration(wait_timeout, timeout)) {return RMW_RET_INVALID_ARGUMENT;}

  std::vector<DDS_Condition> attached;
  try {
    attached.reserve(condition_count);
  } catch (const std::bad_alloc &) {
    RMW_SET_ERROR_MSG("failed to allocate wait condition bookkeeping");
    return RMW_RET_BAD_ALLOC;
  }
  auto attach = [&](DDS_Condition condition) {
      if (DDS_WaitSet_attach_condition(impl->native, condition) != DDS_RETCODE_OK) {
        RMW_SET_ERROR_MSG("zzdds failed to attach a wait condition");
        return false;
      }
      attached.push_back(condition);
      return true;
    };
  for (size_t i = 0U; i < subscription_count; ++i) {
    auto * subscription = static_cast<SubscriptionImpl *>(subscriptions->subscribers[i]);
    if (subscription->context != impl->context ||
      !attach(DDS_ReadCondition_as_DDS_Condition(subscription->read_condition)))
    {
      for (DDS_Condition condition : attached) {
        (void)DDS_WaitSet_detach_condition(impl->native, condition);
      }
      return subscription->context == impl->context ? RMW_RET_ERROR : RMW_RET_INVALID_ARGUMENT;
    }
  }
  for (size_t i = 0U; i < guard_count; ++i) {
    auto * guard = static_cast<GuardConditionImpl *>(guard_conditions->guard_conditions[i]);
    ContextImpl * guard_context = guard->context == nullptr ? nullptr :
      reinterpret_cast<ContextImpl *>(guard->context->impl);
    if (guard_context != impl->context ||
      !attach(DDS_GuardCondition_as_DDS_Condition(guard->native)))
    {
      for (DDS_Condition condition : attached) {
        (void)DDS_WaitSet_detach_condition(impl->native, condition);
      }
      return guard_context == impl->context ? RMW_RET_ERROR : RMW_RET_INVALID_ARGUMENT;
    }
  }
  for (size_t i = 0U; i < service_count; ++i) {
    auto * service = static_cast<ServiceImpl *>(services->services[i]);
    auto * subscription = static_cast<SubscriptionImpl *>(service->request_subscription->data);
    if (service->context != impl->context ||
      !attach(DDS_ReadCondition_as_DDS_Condition(subscription->read_condition)))
    {
      for (DDS_Condition condition : attached) {
        (void)DDS_WaitSet_detach_condition(impl->native, condition);
      }
      return service->context == impl->context ? RMW_RET_ERROR : RMW_RET_INVALID_ARGUMENT;
    }
  }
  for (size_t i = 0U; i < client_count; ++i) {
    auto * client = static_cast<ClientImpl *>(clients->clients[i]);
    auto * subscription = static_cast<SubscriptionImpl *>(client->response_subscription->data);
    if (client->context != impl->context ||
      !attach(DDS_ReadCondition_as_DDS_Condition(subscription->read_condition)))
    {
      for (DDS_Condition condition : attached) {
        (void)DDS_WaitSet_detach_condition(impl->native, condition);
      }
      return client->context == impl->context ? RMW_RET_ERROR : RMW_RET_INVALID_ARGUMENT;
    }
  }
  for (size_t i = 0U; i < event_count; ++i) {
    auto * event = static_cast<rmw_event_t *>(events->events[i]);
    if (event->implementation_identifier != rmw_zzdds_cpp::identifier ||
      event->data == nullptr || rmw_zzdds_cpp::event_context(event) != impl->context ||
      !attach(rmw_zzdds_cpp::event_condition(event)))
    {
      for (DDS_Condition condition : attached) {
        (void)DDS_WaitSet_detach_condition(impl->native, condition);
      }
      return RMW_RET_INVALID_ARGUMENT;
    }
  }

  DDS_ConditionSeq active{};
  const DDS_ReturnCode_t wait_result = DDS_WaitSet_wait(impl->native, &active, &timeout);
  for (DDS_Condition condition : attached) {
    (void)DDS_WaitSet_detach_condition(impl->native, condition);
  }
  if (wait_result != DDS_RETCODE_OK && wait_result != DDS_RETCODE_TIMEOUT) {
    DDS_ConditionSeq_free(&active);
    RMW_SET_ERROR_MSG("zzdds wait failed");
    return RMW_RET_ERROR;
  }
  for (size_t i = 0U; i < subscription_count; ++i) {
    auto * subscription = static_cast<SubscriptionImpl *>(subscriptions->subscribers[i]);
    if (!contains(active, DDS_ReadCondition_as_DDS_Condition(subscription->read_condition))) {
      subscriptions->subscribers[i] = nullptr;
    }
  }
  for (size_t i = 0U; i < guard_count; ++i) {
    auto * guard = static_cast<GuardConditionImpl *>(guard_conditions->guard_conditions[i]);
    if (contains(active, DDS_GuardCondition_as_DDS_Condition(guard->native))) {
      (void)DDS_GuardCondition_set_trigger_value(guard->native, false);
    } else {
      guard_conditions->guard_conditions[i] = nullptr;
    }
  }
  for (size_t i = 0U; i < service_count; ++i) {
    auto * service = static_cast<ServiceImpl *>(services->services[i]);
    auto * subscription = static_cast<SubscriptionImpl *>(service->request_subscription->data);
    if (!contains(active, DDS_ReadCondition_as_DDS_Condition(subscription->read_condition))) {
      services->services[i] = nullptr;
    }
  }
  for (size_t i = 0U; i < client_count; ++i) {
    auto * client = static_cast<ClientImpl *>(clients->clients[i]);
    auto * subscription = static_cast<SubscriptionImpl *>(client->response_subscription->data);
    if (!contains(active, DDS_ReadCondition_as_DDS_Condition(subscription->read_condition))) {
      clients->clients[i] = nullptr;
    }
  }
  for (size_t i = 0U; i < event_count; ++i) {
    auto * event = static_cast<rmw_event_t *>(events->events[i]);
    const DDS_Condition condition = rmw_zzdds_cpp::event_condition(event);
    if (!contains(active, condition)) {events->events[i] = nullptr;}
  }
  DDS_ConditionSeq_free(&active);
  return wait_result == DDS_RETCODE_TIMEOUT ? RMW_RET_TIMEOUT : RMW_RET_OK;
}
}  // extern "C"
// Copyright 2026 Zenzen IoT
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

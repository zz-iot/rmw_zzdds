#include "rmw/error_handling.h"
#include "rmw/rmw.h"

#include "rmw_zzdds_cpp/allocator.hpp"
#include "rmw_zzdds_cpp/context_impl.hpp"
#include "rmw_zzdds_cpp/guard_condition_impl.hpp"
#include "rmw_zzdds_cpp/identifier.hpp"

extern "C"
{

rmw_guard_condition_t * rmw_create_guard_condition(rmw_context_t * context)
{
  if (context == nullptr || context->impl == nullptr) {
    RMW_SET_ERROR_MSG("an initialized context is required");
    return nullptr;
  }
  if (context->implementation_identifier != rmw_zzdds_cpp::identifier) {
    RMW_SET_ERROR_MSG("context belongs to a different RMW implementation");
    return nullptr;
  }

  auto * context_impl = reinterpret_cast<rmw_zzdds_cpp::ContextImpl *>(context->impl);
  const std::lock_guard<std::mutex> lock(context_impl->mutex);
  if (context_impl->shutdown) {
    RMW_SET_ERROR_MSG("cannot create a guard condition after context shutdown");
    return nullptr;
  }

  DDS_GuardCondition native = zzdds_create_guardcondition();
  if (zzdds_guardcondition_is_nil(native)) {
    RMW_SET_ERROR_MSG("failed to create the zzdds guard condition");
    return nullptr;
  }

  const rcutils_allocator_t allocator = context_impl->allocator;
  auto * impl = rmw_zzdds_cpp::allocate_object<rmw_zzdds_cpp::GuardConditionImpl>(
    allocator, context, native);
  auto * handle = rmw_zzdds_cpp::allocate_object<rmw_guard_condition_t>(allocator);
  if (impl == nullptr || handle == nullptr) {
    rmw_zzdds_cpp::deallocate_object(allocator, handle);
    rmw_zzdds_cpp::deallocate_object(allocator, impl);
    zzdds_destroy_guardcondition(native);
    RMW_SET_ERROR_MSG("failed to allocate guard condition resources");
    return nullptr;
  }

  handle->implementation_identifier = rmw_zzdds_cpp::identifier;
  handle->data = impl;
  handle->context = context;
  ++context_impl->active_entities;
  return handle;
}

rmw_ret_t rmw_destroy_guard_condition(rmw_guard_condition_t * guard_condition)
{
  if (guard_condition == nullptr || guard_condition->data == nullptr ||
    guard_condition->context == nullptr)
  {
    RMW_SET_ERROR_MSG("a valid guard condition is required");
    return RMW_RET_INVALID_ARGUMENT;
  }
  if (guard_condition->implementation_identifier != rmw_zzdds_cpp::identifier) {
    RMW_SET_ERROR_MSG("guard condition belongs to a different RMW implementation");
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
  }

  auto * context_impl = reinterpret_cast<rmw_zzdds_cpp::ContextImpl *>(
    guard_condition->context->impl);
  if (context_impl == nullptr) {
    RMW_SET_ERROR_MSG("guard condition context is invalid");
    return RMW_RET_INVALID_ARGUMENT;
  }
  const rcutils_allocator_t allocator = context_impl->allocator;
  auto * impl = static_cast<rmw_zzdds_cpp::GuardConditionImpl *>(guard_condition->data);
  zzdds_destroy_guardcondition(impl->native);
  rmw_zzdds_cpp::deallocate_object(allocator, impl);
  rmw_zzdds_cpp::deallocate_object(allocator, guard_condition);
  --context_impl->active_entities;
  return RMW_RET_OK;
}

rmw_ret_t rmw_trigger_guard_condition(const rmw_guard_condition_t * guard_condition)
{
  if (guard_condition == nullptr || guard_condition->data == nullptr) {
    RMW_SET_ERROR_MSG("a valid guard condition is required");
    return RMW_RET_INVALID_ARGUMENT;
  }
  if (guard_condition->implementation_identifier != rmw_zzdds_cpp::identifier) {
    RMW_SET_ERROR_MSG("guard condition belongs to a different RMW implementation");
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
  }

  const auto * impl = static_cast<const rmw_zzdds_cpp::GuardConditionImpl *>(
    guard_condition->data);
  if (DDS_GuardCondition_set_trigger_value(impl->native, true) != DDS_RETCODE_OK) {
    RMW_SET_ERROR_MSG("zzdds failed to trigger the guard condition");
    return RMW_RET_ERROR;
  }
  return RMW_RET_OK;
}

}  // extern "C"


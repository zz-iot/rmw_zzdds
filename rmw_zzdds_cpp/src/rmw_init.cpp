#include <cstdint>
#include <cstring>
#include <limits>

#include "rmw/discovery_options.h"
#include "rmw/enclave.h"
#include "rmw/error_handling.h"
#include "rmw/init.h"
#include "rmw/init_options.h"
#include "rmw/security_options.h"

#include "rmw_zzdds_cpp/allocator.hpp"
#include "rmw_zzdds_cpp/context_impl.hpp"
#include "rmw_zzdds_cpp/graph_discovery.hpp"
#include "rmw_zzdds_cpp/identifier.hpp"

namespace rmw_zzdds_cpp
{

DdsContext::~DdsContext()
{
  (void)reset();
}

bool DdsContext::initialize(uint32_t domain_id) noexcept
{
  (void)reset();

  factory_ = zzdds_create_factory();
  if (zzdds_factory_is_nil(factory_)) {
    factory_ = nullptr;
    return false;
  }

  dds_factory_ =
    zzdds_DomainParticipantFactory_as_DDS_DomainParticipantFactory(factory_);
  participant_ = DDS_DomainParticipantFactory_create_participant(
    dds_factory_, domain_id, nullptr, nullptr, 0U);
  if (participant_ == nullptr) {
    (void)reset();
    return false;
  }

  publisher_ = DDS_DomainParticipant_create_publisher(
    participant_, nullptr, nullptr, 0U);
  if (publisher_ == nullptr) {
    (void)reset();
    return false;
  }

  subscriber_ = DDS_DomainParticipant_create_subscriber(
    participant_, nullptr, nullptr, 0U);
  if (subscriber_ == nullptr) {
    (void)reset();
    return false;
  }

  return true;
}

bool DdsContext::finalize() noexcept
{
  return reset();
}

bool DdsContext::reset() noexcept
{
  bool success = true;
  if (participant_ != nullptr && subscriber_ != nullptr) {
    success = DDS_Subscriber_delete_contained_entities(subscriber_) == DDS_RETCODE_OK && success;
    success = DDS_DomainParticipant_delete_subscriber(participant_, subscriber_) ==
      DDS_RETCODE_OK && success;
    subscriber_ = nullptr;
  }
  if (participant_ != nullptr && publisher_ != nullptr) {
    success = DDS_Publisher_delete_contained_entities(publisher_) == DDS_RETCODE_OK && success;
    success = DDS_DomainParticipant_delete_publisher(participant_, publisher_) ==
      DDS_RETCODE_OK && success;
    publisher_ = nullptr;
  }
  if (participant_ != nullptr) {
    success = DDS_DomainParticipant_delete_contained_entities(participant_) ==
      DDS_RETCODE_OK && success;
    if (dds_factory_ != nullptr) {
      success = DDS_DomainParticipantFactory_delete_participant(dds_factory_, participant_) ==
        DDS_RETCODE_OK && success;
    }
    participant_ = nullptr;
  }
  if (factory_ != nullptr) {
    zzdds_destroy_factory(factory_);
    factory_ = nullptr;
  }
  dds_factory_ = nullptr;
  return success;
}

namespace
{

void cleanup_options(rmw_init_options_t * options) noexcept
{
  if (options->enclave != nullptr) {
    (void)rmw_enclave_options_fini(options->enclave, &options->allocator);
    options->enclave = nullptr;
  }
  const rmw_ret_t discovery_ret = rmw_discovery_options_fini(&options->discovery_options);
  const rmw_ret_t security_ret =
    rmw_security_options_fini(&options->security_options, &options->allocator);
  (void)discovery_ret;
  (void)security_ret;
  *options = rmw_get_zero_initialized_init_options();
}

rmw_ret_t copy_options(const rmw_init_options_t * src, rmw_init_options_t * dst)
{
  rmw_init_options_t result = rmw_get_zero_initialized_init_options();
  result.instance_id = src->instance_id;
  result.implementation_identifier = identifier;
  result.domain_id = src->domain_id;
  result.allocator = src->allocator;
  result.security_options = rmw_get_zero_initialized_security_options();
  result.discovery_options = rmw_get_zero_initialized_discovery_options();

  rmw_ret_t ret = rmw_security_options_copy(
    &src->security_options, &result.allocator, &result.security_options);
  if (ret != RMW_RET_OK) {
    cleanup_options(&result);
    return ret;
  }

  ret = rmw_discovery_options_copy(
    &src->discovery_options, &result.allocator, &result.discovery_options);
  if (ret != RMW_RET_OK) {
    cleanup_options(&result);
    return ret;
  }

  if (src->enclave != nullptr) {
    ret = rmw_enclave_options_copy(src->enclave, &result.allocator, &result.enclave);
    if (ret != RMW_RET_OK) {
      cleanup_options(&result);
      return ret;
    }
  }

  *dst = result;
  return RMW_RET_OK;
}

}  // namespace
}  // namespace rmw_zzdds_cpp

extern "C"
{

rmw_ret_t rmw_init_options_init(
  rmw_init_options_t * init_options, rcutils_allocator_t allocator)
{
  if (init_options == nullptr || !rcutils_allocator_is_valid(&allocator)) {
    RMW_SET_ERROR_MSG("init options and a valid allocator are required");
    return RMW_RET_INVALID_ARGUMENT;
  }
  if (init_options->implementation_identifier != nullptr) {
    RMW_SET_ERROR_MSG("init options must be zero initialized");
    return RMW_RET_INVALID_ARGUMENT;
  }

  rmw_init_options_t result = rmw_get_zero_initialized_init_options();
  result.implementation_identifier = rmw_zzdds_cpp::identifier;
  result.domain_id = RMW_DEFAULT_DOMAIN_ID;
  result.allocator = allocator;
  result.security_options = rmw_get_default_security_options();
  result.discovery_options = rmw_get_zero_initialized_discovery_options();

  const rmw_ret_t ret = rmw_discovery_options_init(
    &result.discovery_options, 0U, &result.allocator);
  if (ret != RMW_RET_OK) {
    return ret;
  }

  *init_options = result;
  return RMW_RET_OK;
}

rmw_ret_t rmw_init_options_copy(
  const rmw_init_options_t * src, rmw_init_options_t * dst)
{
  if (src == nullptr || dst == nullptr || src == dst) {
    RMW_SET_ERROR_MSG("distinct source and destination init options are required");
    return RMW_RET_INVALID_ARGUMENT;
  }
  if (src->implementation_identifier == nullptr) {
    RMW_SET_ERROR_MSG("source init options are not initialized");
    return RMW_RET_INVALID_ARGUMENT;
  }
  if (src->implementation_identifier != rmw_zzdds_cpp::identifier) {
    RMW_SET_ERROR_MSG("source init options belong to a different RMW implementation");
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
  }
  if (dst->implementation_identifier != nullptr) {
    RMW_SET_ERROR_MSG("destination init options must be zero initialized");
    return RMW_RET_INVALID_ARGUMENT;
  }
  if (!rcutils_allocator_is_valid(&src->allocator)) {
    RMW_SET_ERROR_MSG("source init options contain an invalid allocator");
    return RMW_RET_INVALID_ARGUMENT;
  }
  return rmw_zzdds_cpp::copy_options(src, dst);
}

rmw_ret_t rmw_init_options_fini(rmw_init_options_t * init_options)
{
  if (init_options == nullptr || init_options->implementation_identifier == nullptr) {
    RMW_SET_ERROR_MSG("initialized init options are required");
    return RMW_RET_INVALID_ARGUMENT;
  }
  if (init_options->implementation_identifier != rmw_zzdds_cpp::identifier) {
    RMW_SET_ERROR_MSG("init options belong to a different RMW implementation");
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
  }
  rmw_zzdds_cpp::cleanup_options(init_options);
  return RMW_RET_OK;
}

rmw_ret_t rmw_init(const rmw_init_options_t * options, rmw_context_t * context)
{
  if (options == nullptr || context == nullptr) {
    RMW_SET_ERROR_MSG("init options and context are required");
    return RMW_RET_INVALID_ARGUMENT;
  }
  if (options->implementation_identifier == nullptr || options->enclave == nullptr) {
    RMW_SET_ERROR_MSG("initialized options with an enclave are required");
    return RMW_RET_INVALID_ARGUMENT;
  }
  if (options->implementation_identifier != rmw_zzdds_cpp::identifier) {
    RMW_SET_ERROR_MSG("init options belong to a different RMW implementation");
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
  }
  if (context->implementation_identifier != nullptr || context->impl != nullptr) {
    RMW_SET_ERROR_MSG("context must be zero initialized");
    return RMW_RET_INVALID_ARGUMENT;
  }
  if (options->security_options.enforce_security == RMW_SECURITY_ENFORCEMENT_ENFORCE) {
    RMW_SET_ERROR_MSG("DDS Security is not supported by rmw_zzdds_cpp");
    return RMW_RET_UNSUPPORTED;
  }

  const size_t domain = options->domain_id == RMW_DEFAULT_DOMAIN_ID ? 0U : options->domain_id;
  if (domain > std::numeric_limits<uint32_t>::max()) {
    RMW_SET_ERROR_MSG("domain id does not fit the zzdds domain id type");
    return RMW_RET_INVALID_ARGUMENT;
  }

  rmw_init_options_t copied_options = rmw_get_zero_initialized_init_options();
  rmw_ret_t ret = rmw_zzdds_cpp::copy_options(options, &copied_options);
  if (ret != RMW_RET_OK) {
    return ret;
  }

  auto * impl = rmw_zzdds_cpp::allocate_object<rmw_zzdds_cpp::ContextImpl>(
    options->allocator, options->allocator, static_cast<uint32_t>(domain));
  if (impl == nullptr) {
    rmw_zzdds_cpp::cleanup_options(&copied_options);
    RMW_SET_ERROR_MSG("failed to allocate the RMW context implementation");
    return RMW_RET_BAD_ALLOC;
  }

  if (!impl->dds.initialize(static_cast<uint32_t>(domain))) {
    rmw_zzdds_cpp::deallocate_object(options->allocator, impl);
    rmw_zzdds_cpp::cleanup_options(&copied_options);
    RMW_SET_ERROR_MSG("failed to initialize the zzdds participant context");
    return RMW_RET_ERROR;
  }
  zzdds_RtpsGuid participant_guid{};
  const zzdds_DomainParticipant participant_extension =
    DDS_DomainParticipant_as_zzdds_DomainParticipant(impl->dds.participant());
  if (participant_extension == nullptr ||
    zzdds_DomainParticipant_get_rtps_guid(participant_extension, &participant_guid) !=
    DDS_RETCODE_OK)
  {
    (void)impl->dds.finalize();
    rmw_zzdds_cpp::deallocate_object(options->allocator, impl);
    rmw_zzdds_cpp::cleanup_options(&copied_options);
    RMW_SET_ERROR_MSG("failed to obtain the zzdds participant RTPS GUID");
    return RMW_RET_ERROR;
  }
  impl->participant_gid.implementation_identifier = rmw_zzdds_cpp::identifier;
  static_assert(sizeof(participant_guid.value) <= RMW_GID_STORAGE_SIZE);
  std::memcpy(
    impl->participant_gid.data, participant_guid.value, sizeof(participant_guid.value));
  impl->common.gid = impl->participant_gid;
  impl->common.graph_cache.add_participant(
    impl->participant_gid, options->enclave == nullptr ? "" : options->enclave);

  context->instance_id = options->instance_id;
  context->implementation_identifier = rmw_zzdds_cpp::identifier;
  context->options = copied_options;
  context->actual_domain_id = domain;
  context->impl = reinterpret_cast<rmw_context_impl_t *>(impl);
  impl->graph_guard_native = zzdds_create_guardcondition();
  impl->graph_guard_impl = rmw_zzdds_cpp::allocate_object<rmw_zzdds_cpp::GuardConditionImpl>(
    options->allocator, context, impl->graph_guard_native);
  impl->graph_guard_handle = rmw_zzdds_cpp::allocate_object<rmw_guard_condition_t>(
    options->allocator);
  if (impl->graph_guard_native == nullptr || impl->graph_guard_impl == nullptr ||
    impl->graph_guard_handle == nullptr)
  {
    rmw_zzdds_cpp::deallocate_object(options->allocator, impl->graph_guard_handle);
    rmw_zzdds_cpp::deallocate_object(options->allocator, impl->graph_guard_impl);
    if (impl->graph_guard_native != nullptr) {
      zzdds_destroy_guardcondition(impl->graph_guard_native);
    }
    (void)impl->dds.finalize();
    rmw_zzdds_cpp::deallocate_object(options->allocator, impl);
    rmw_zzdds_cpp::cleanup_options(&context->options);
    *context = rmw_get_zero_initialized_context();
    RMW_SET_ERROR_MSG("failed to allocate the graph guard condition");
    return RMW_RET_BAD_ALLOC;
  }
  impl->graph_guard_handle->implementation_identifier = rmw_zzdds_cpp::identifier;
  impl->graph_guard_handle->data = impl->graph_guard_impl;
  impl->graph_guard_handle->context = context;
  ret = rmw_zzdds_cpp::initialize_graph_channel(context);
  if (ret != RMW_RET_OK) {
    rmw_zzdds_cpp::deallocate_object(options->allocator, impl->graph_guard_handle);
    rmw_zzdds_cpp::deallocate_object(options->allocator, impl->graph_guard_impl);
    zzdds_destroy_guardcondition(impl->graph_guard_native);
    (void)impl->dds.finalize();
    rmw_zzdds_cpp::deallocate_object(options->allocator, impl);
    rmw_zzdds_cpp::cleanup_options(&context->options);
    *context = rmw_get_zero_initialized_context();
    RMW_SET_ERROR_MSG("failed to initialize the ROS graph discovery channel");
    return ret;
  }
  return RMW_RET_OK;
}

rmw_ret_t rmw_shutdown(rmw_context_t * context)
{
  if (context == nullptr || context->implementation_identifier == nullptr || context->impl == nullptr) {
    RMW_SET_ERROR_MSG("an initialized context is required");
    return RMW_RET_INVALID_ARGUMENT;
  }
  if (context->implementation_identifier != rmw_zzdds_cpp::identifier) {
    RMW_SET_ERROR_MSG("context belongs to a different RMW implementation");
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
  }

  auto * impl = reinterpret_cast<rmw_zzdds_cpp::ContextImpl *>(context->impl);
  {
    const std::lock_guard<std::mutex> lock(impl->mutex);
    impl->shutdown = true;
  }
  rmw_zzdds_cpp::stop_graph_listener(impl);
  return RMW_RET_OK;
}

rmw_ret_t rmw_context_fini(rmw_context_t * context)
{
  if (context == nullptr || context->implementation_identifier == nullptr || context->impl == nullptr) {
    RMW_SET_ERROR_MSG("an initialized context is required");
    return RMW_RET_INVALID_ARGUMENT;
  }
  if (context->implementation_identifier != rmw_zzdds_cpp::identifier) {
    RMW_SET_ERROR_MSG("context belongs to a different RMW implementation");
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
  }

  auto * impl = reinterpret_cast<rmw_zzdds_cpp::ContextImpl *>(context->impl);
  const rmw_ret_t graph_ret = rmw_zzdds_cpp::finalize_graph_channel(context);
  if (graph_ret != RMW_RET_OK) {
    RMW_SET_ERROR_MSG("failed to finalize the ROS graph discovery channel");
    return graph_ret;
  }
  {
    const std::lock_guard<std::mutex> lock(impl->mutex);
    if (!impl->shutdown) {
      RMW_SET_ERROR_MSG("context must be shut down before finalization");
      return RMW_RET_INVALID_ARGUMENT;
    }
    if (impl->active_entities.load() != 0U) {
      RMW_SET_ERROR_MSG("context still owns active RMW entities");
      return RMW_RET_INVALID_ARGUMENT;
    }
  }

  const rcutils_allocator_t allocator = impl->allocator;
  rmw_zzdds_cpp::deallocate_object(allocator, impl->graph_guard_handle);
  rmw_zzdds_cpp::deallocate_object(allocator, impl->graph_guard_impl);
  zzdds_destroy_guardcondition(impl->graph_guard_native);
  const bool dds_cleanup_succeeded = impl->dds.finalize();
  rmw_zzdds_cpp::deallocate_object(allocator, impl);
  const rmw_ret_t options_ret = rmw_init_options_fini(&context->options);
  *context = rmw_get_zero_initialized_context();
  if (!dds_cleanup_succeeded) {
    RMW_SET_ERROR_MSG("one or more zzdds entities failed to finalize cleanly");
    return RMW_RET_ERROR;
  }
  return options_ret;
}

}  // extern "C"

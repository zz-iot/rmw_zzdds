#include "rmw/error_handling.h"
#include "rmw/get_network_flow_endpoints.h"
#include "rmw/get_node_info_and_types.h"
#include "rmw/get_service_endpoint_info.h"
#include "rmw/get_service_names_and_types.h"
#include "rmw/get_topic_endpoint_info.h"
#include "rmw/get_topic_names_and_types.h"
#include "rmw/names_and_types.h"
#include "rmw/rmw.h"

namespace
{

rmw_ret_t unsupported(const char * operation)
{
  RMW_SET_ERROR_MSG_WITH_FORMAT_STRING("%s is not implemented by rmw_zzdds_cpp", operation);
  return RMW_RET_UNSUPPORTED;
}

}  // namespace

#define RMW_ZZDDS_UNSUPPORTED(name, signature) \
  rmw_ret_t name signature {return unsupported(#name);}
#define RMW_ZZDDS_UNSUPPORTED_POINTER(type, name, signature) \
  type name signature {RMW_SET_ERROR_MSG(#name " is not implemented by rmw_zzdds_cpp"); return nullptr;}

extern "C"
{

RMW_ZZDDS_UNSUPPORTED(
  rmw_init_publisher_allocation,
  (const rosidl_message_type_support_t *, const rosidl_runtime_c__Sequence__bound *,
  rmw_publisher_allocation_t *))
RMW_ZZDDS_UNSUPPORTED(rmw_fini_publisher_allocation, (rmw_publisher_allocation_t *))
RMW_ZZDDS_UNSUPPORTED(rmw_publisher_assert_liveliness, (const rmw_publisher_t *))
RMW_ZZDDS_UNSUPPORTED(
  rmw_get_serialized_message_size,
  (const rosidl_message_type_support_t *, const rosidl_runtime_c__Sequence__bound *, size_t *))
RMW_ZZDDS_UNSUPPORTED(
  rmw_init_subscription_allocation,
  (const rosidl_message_type_support_t *, const rosidl_runtime_c__Sequence__bound *,
  rmw_subscription_allocation_t *))
RMW_ZZDDS_UNSUPPORTED(rmw_fini_subscription_allocation, (rmw_subscription_allocation_t *))

RMW_ZZDDS_UNSUPPORTED(
  rmw_publisher_get_network_flow_endpoints,
  (const rmw_publisher_t *, rcutils_allocator_t *, rmw_network_flow_endpoint_array_t *))
RMW_ZZDDS_UNSUPPORTED(
  rmw_subscription_get_network_flow_endpoints,
  (const rmw_subscription_t *, rcutils_allocator_t *, rmw_network_flow_endpoint_array_t *))

RMW_ZZDDS_UNSUPPORTED(
  rmw_subscription_set_on_new_message_callback,
  (rmw_subscription_t *, rmw_event_callback_t, const void *))
RMW_ZZDDS_UNSUPPORTED(
  rmw_service_set_on_new_request_callback,
  (rmw_service_t *, rmw_event_callback_t, const void *))
RMW_ZZDDS_UNSUPPORTED(
  rmw_client_set_on_new_response_callback,
  (rmw_client_t *, rmw_event_callback_t, const void *))
bool rmw_feature_supported(rmw_feature_t) {return false;}
RMW_ZZDDS_UNSUPPORTED(
  rmw_take_dynamic_message,
  (const rmw_subscription_t *, rosidl_dynamic_typesupport_dynamic_data_t *, bool *,
  rmw_subscription_allocation_t *))
RMW_ZZDDS_UNSUPPORTED(
  rmw_take_dynamic_message_with_info,
  (const rmw_subscription_t *, rosidl_dynamic_typesupport_dynamic_data_t *, bool *,
  rmw_message_info_t *, rmw_subscription_allocation_t *))
RMW_ZZDDS_UNSUPPORTED(
  rmw_serialization_support_init,
  (const char *, rcutils_allocator_t *, rosidl_dynamic_typesupport_serialization_support_t *))

rmw_ret_t rmw_set_log_severity(rmw_log_severity_t severity)
{
  return rcutils_logging_set_logger_level("rmw_zzdds_cpp", static_cast<int>(severity)) ==
         RCUTILS_RET_OK ? RMW_RET_OK : RMW_RET_ERROR;
}

}  // extern "C"

#undef RMW_ZZDDS_UNSUPPORTED_POINTER
#undef RMW_ZZDDS_UNSUPPORTED

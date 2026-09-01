#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "rcutils/strdup.h"
#include "rcutils/types/string_array.h"
#include "rmw/error_handling.h"
#include "rmw/names_and_types.h"
#include "rmw/rmw.h"
#include "rmw/service_endpoint_info_array.h"
#include "rmw/sanity_checks.h"
#include "rmw/topic_endpoint_info_array.h"
#include "rmw/validate_namespace.h"
#include "rmw/validate_node_name.h"
#include "rmw/validate_full_topic_name.h"

#include "rmw_zzdds_cpp/context_impl.hpp"
#include "rmw_zzdds_cpp/graph_discovery.hpp"
#include "rmw_zzdds_cpp/identifier.hpp"

namespace
{
rmw_ret_t validate_count_arguments(
  const rmw_node_t * node, const char * name, size_t * count)
{
  if (node == nullptr || node->data == nullptr || node->context == nullptr ||
    node->context->impl == nullptr || name == nullptr || count == nullptr)
  {
    RMW_SET_ERROR_MSG("a valid node, name, and count output are required");
    return RMW_RET_INVALID_ARGUMENT;
  }
  if (node->implementation_identifier != rmw_zzdds_cpp::identifier) {
    RMW_SET_ERROR_MSG("node belongs to another RMW implementation");
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
  }
  int validation = RMW_TOPIC_VALID;
  if (rmw_validate_full_topic_name(name, &validation, nullptr) != RMW_RET_OK ||
    validation != RMW_TOPIC_VALID)
  {
    RMW_SET_ERROR_MSG("name is not a valid fully-qualified ROS name");
    return RMW_RET_INVALID_ARGUMENT;
  }
  return RMW_RET_OK;
}

rmw_zzdds_cpp::ContextImpl * context_of(const rmw_node_t * node)
{
  return reinterpret_cast<rmw_zzdds_cpp::ContextImpl *>(node->context->impl);
}

rmw_ret_t get_node_names_impl(
  const rmw_node_t * node, rcutils_string_array_t * names,
  rcutils_string_array_t * namespaces, rcutils_string_array_t * enclaves)
{
  if (node == nullptr || node->data == nullptr || node->context == nullptr ||
    node->context->impl == nullptr || names == nullptr || namespaces == nullptr)
  {
    RMW_SET_ERROR_MSG("a valid node and output arrays are required");
    return RMW_RET_INVALID_ARGUMENT;
  }
  if (node->implementation_identifier != rmw_zzdds_cpp::identifier) {
    RMW_SET_ERROR_MSG("node belongs to another RMW implementation");
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
  }
  if (rmw_check_zero_rmw_string_array(names) != RMW_RET_OK) {return RMW_RET_INVALID_ARGUMENT;}
  if (rmw_check_zero_rmw_string_array(namespaces) != RMW_RET_OK) {
    return RMW_RET_INVALID_ARGUMENT;
  }
  if (enclaves != nullptr && rmw_check_zero_rmw_string_array(enclaves) != RMW_RET_OK) {
    return RMW_RET_INVALID_ARGUMENT;
  }

  auto * context = context_of(node);
  const rmw_ret_t refresh = rmw_zzdds_cpp::refresh_discovered_endpoints(context);
  if (refresh != RMW_RET_OK) {return refresh;}
  auto allocator = context->allocator;
  return context->common.graph_cache.get_node_names(
    names, namespaces, enclaves, &allocator);
}

using NamesAndTypes = std::map<std::string, std::set<std::string>>;

std::string demangle_dds_type(const std::string & dds_type)
{
  std::string result = dds_type;
  const std::string marker = "::dds_::";
  const size_t marker_position = result.find(marker);
  if (marker_position != std::string::npos) {
    result.erase(marker_position, marker.size() - 2U);
  }
  if (!result.empty() && result.back() == '_') {result.pop_back();}
  for (size_t position = result.find("::"); position != std::string::npos;
    position = result.find("::", position + 1U))
  {
    result.replace(position, 2U, "/");
  }
  return result;
}

std::string demangle_dds_service_type(const std::string & dds_type)
{
  std::string result = demangle_dds_type(dds_type);
  constexpr const char * suffixes[] = {"_Request", "_Response"};
  for (const char * suffix : suffixes) {
    const size_t suffix_size = std::strlen(suffix);
    if (result.size() >= suffix_size &&
      result.compare(result.size() - suffix_size, suffix_size, suffix) == 0)
    {
      result.resize(result.size() - suffix_size);
      break;
    }
  }
  return result;
}

NamesAndTypes extract_service_topics(
  const rmw_names_and_types_t & endpoints, const char * prefix, const char * suffix)
{
  NamesAndTypes result;
  const size_t prefix_size = std::strlen(prefix);
  const size_t suffix_size = std::strlen(suffix);
  for (size_t index = 0U; index < endpoints.names.size; ++index) {
    const std::string topic(endpoints.names.data[index]);
    if (topic.size() <= prefix_size + suffix_size || topic.rfind(prefix, 0U) != 0U ||
      topic.compare(topic.size() - suffix_size, suffix_size, suffix) != 0)
    {
      continue;
    }
    const std::string service = topic.substr(
      prefix_size, topic.size() - prefix_size - suffix_size);
    for (size_t type_index = 0U; type_index < endpoints.types[index].size; ++type_index) {
      result[service].insert(demangle_dds_service_type(endpoints.types[index].data[type_index]));
    }
  }
  return result;
}

NamesAndTypes intersect_services(const NamesAndTypes & first, const NamesAndTypes & second)
{
  NamesAndTypes result;
  for (const auto & [name, first_types] : first) {
    const auto second_entry = second.find(name);
    if (second_entry == second.end()) {continue;}
    for (const std::string & type : first_types) {
      if (second_entry->second.count(type) != 0U) {result[name].insert(type);}
    }
  }
  return result;
}

std::string identity_name(const std::string & name)
{
  return name;
}

std::string demangle_dds_topic(const std::string & dds_topic)
{
  if (dds_topic.rfind("rt/", 0U) != 0U) {return "";}
  const std::string ros_topic = dds_topic.substr(2U);
  if (ros_topic.rfind("/rq", 0U) == 0U || ros_topic.rfind("/rr", 0U) == 0U) {return "";}
  return ros_topic;
}

rmw_ret_t fill_names_and_types(
  const NamesAndTypes & values, rcutils_allocator_t * allocator, rmw_names_and_types_t * output)
{
  if (rmw_names_and_types_init(output, values.size(), allocator) != RMW_RET_OK) {
    return RMW_RET_BAD_ALLOC;
  }
  size_t index = 0U;
  for (const auto & [name, types] : values) {
    output->names.data[index] = rcutils_strdup(name.c_str(), *allocator);
    if (output->names.data[index] == nullptr ||
      rcutils_string_array_init(&output->types[index], types.size(), allocator) != RCUTILS_RET_OK)
    {
      const rmw_ret_t fini_result = rmw_names_and_types_fini(output);
      (void)fini_result;
      return RMW_RET_BAD_ALLOC;
    }
    size_t type_index = 0U;
    for (const std::string & type : types) {
      output->types[index].data[type_index] = rcutils_strdup(type.c_str(), *allocator);
      if (output->types[index].data[type_index] == nullptr) {
        const rmw_ret_t fini_result = rmw_names_and_types_fini(output);
        (void)fini_result;
        return RMW_RET_BAD_ALLOC;
      }
      ++type_index;
    }
    ++index;
  }
  return RMW_RET_OK;
}

rmw_ret_t get_service_names_and_types_from_cache(
  rmw_zzdds_cpp::ContextImpl * context, rcutils_allocator_t * allocator,
  const char * node_name, const char * node_namespace, bool clients,
  rmw_names_and_types_t * output)
{
  rmw_names_and_types_t request_endpoints = rmw_get_zero_initialized_names_and_types();
  rmw_names_and_types_t reply_endpoints = rmw_get_zero_initialized_names_and_types();
  rmw_ret_t result;
  if (clients) {
    result = context->common.graph_cache.get_writer_names_and_types_by_node(
      node_name, node_namespace, identity_name, identity_name, allocator, &request_endpoints);
    if (result == RMW_RET_OK) {
      result = context->common.graph_cache.get_reader_names_and_types_by_node(
        node_name, node_namespace, identity_name, identity_name, allocator, &reply_endpoints);
    }
  } else {
    result = context->common.graph_cache.get_reader_names_and_types_by_node(
      node_name, node_namespace, identity_name, identity_name, allocator, &request_endpoints);
    if (result == RMW_RET_OK) {
      result = context->common.graph_cache.get_writer_names_and_types_by_node(
        node_name, node_namespace, identity_name, identity_name, allocator, &reply_endpoints);
    }
  }
  if (result == RMW_RET_OK) {
    try {
      const NamesAndTypes requests = extract_service_topics(
        request_endpoints, "rt/rq", "Request");
      const NamesAndTypes replies = extract_service_topics(reply_endpoints, "rt/rr", "Reply");
      result = fill_names_and_types(intersect_services(requests, replies), allocator, output);
    } catch (const std::bad_alloc &) {
      RMW_SET_ERROR_MSG("failed to allocate service graph bookkeeping");
      result = RMW_RET_BAD_ALLOC;
    }
  }
  const rmw_ret_t reply_fini = rmw_names_and_types_fini(&reply_endpoints);
  const rmw_ret_t request_fini = rmw_names_and_types_fini(&request_endpoints);
  (void)reply_fini;
  (void)request_fini;
  return result;
}

rmw_ret_t validate_names_and_types_arguments(
  const rmw_node_t * node, rcutils_allocator_t * allocator, rmw_names_and_types_t * output)
{
  if (node == nullptr || node->data == nullptr || node->context == nullptr ||
    node->context->impl == nullptr || allocator == nullptr || output == nullptr)
  {
    RMW_SET_ERROR_MSG("a valid node, allocator, and names-and-types output are required");
    return RMW_RET_INVALID_ARGUMENT;
  }
  if (node->implementation_identifier != rmw_zzdds_cpp::identifier) {
    RMW_SET_ERROR_MSG("node belongs to another RMW implementation");
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
  }
  if (!rcutils_allocator_is_valid(allocator)) {
    RMW_SET_ERROR_MSG("allocator is invalid");
    return RMW_RET_INVALID_ARGUMENT;
  }
  return rmw_names_and_types_check_zero(output);
}

rmw_ret_t validate_queried_node(const char * name, const char * namespace_)
{
  if (name == nullptr || namespace_ == nullptr) {
    RMW_SET_ERROR_MSG("node name and namespace are required");
    return RMW_RET_INVALID_ARGUMENT;
  }
  int validation = RMW_NODE_NAME_VALID;
  if (rmw_validate_node_name(name, &validation, nullptr) != RMW_RET_OK ||
    validation != RMW_NODE_NAME_VALID)
  {
    RMW_SET_ERROR_MSG("invalid node name");
    return RMW_RET_INVALID_ARGUMENT;
  }
  validation = RMW_NAMESPACE_VALID;
  if (rmw_validate_namespace(namespace_, &validation, nullptr) != RMW_RET_OK ||
    validation != RMW_NAMESPACE_VALID)
  {
    RMW_SET_ERROR_MSG("invalid node namespace");
    return RMW_RET_INVALID_ARGUMENT;
  }
  return RMW_RET_OK;
}

rmw_ret_t validate_endpoint_arguments(
  const rmw_node_t * node, rcutils_allocator_t * allocator, const char * name,
  const void * output)
{
  if (node == nullptr || node->data == nullptr || node->context == nullptr ||
    node->context->impl == nullptr || allocator == nullptr || name == nullptr || output == nullptr)
  {
    RMW_SET_ERROR_MSG("a valid node, allocator, name, and endpoint output are required");
    return RMW_RET_INVALID_ARGUMENT;
  }
  if (node->implementation_identifier != rmw_zzdds_cpp::identifier) {
    RMW_SET_ERROR_MSG("node belongs to another RMW implementation");
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
  }
  if (!rcutils_allocator_is_valid(allocator)) {
    RMW_SET_ERROR_MSG("allocator is invalid");
    return RMW_RET_INVALID_ARGUMENT;
  }
  return RMW_RET_OK;
}

}  // namespace

extern "C"
{
const rmw_guard_condition_t * rmw_node_get_graph_guard_condition(const rmw_node_t * node)
{
  if (node == nullptr || node->data == nullptr || node->context == nullptr ||
    node->context->impl == nullptr)
  {
    RMW_SET_ERROR_MSG("a valid node is required");
    return nullptr;
  }
  if (node->implementation_identifier != rmw_zzdds_cpp::identifier) {
    RMW_SET_ERROR_MSG("node belongs to another RMW implementation");
    return nullptr;
  }
  return context_of(node)->graph_guard_handle;
}

rmw_ret_t rmw_get_node_names(
  const rmw_node_t * node, rcutils_string_array_t * node_names,
  rcutils_string_array_t * node_namespaces)
{
  return get_node_names_impl(node, node_names, node_namespaces, nullptr);
}

rmw_ret_t rmw_get_node_names_with_enclaves(
  const rmw_node_t * node, rcutils_string_array_t * node_names,
  rcutils_string_array_t * node_namespaces, rcutils_string_array_t * enclaves)
{
  if (enclaves == nullptr) {
    RMW_SET_ERROR_MSG("an enclave output array is required");
    return RMW_RET_INVALID_ARGUMENT;
  }
  return get_node_names_impl(node, node_names, node_namespaces, enclaves);
}

rmw_ret_t rmw_get_topic_names_and_types(
  const rmw_node_t * node, rcutils_allocator_t * allocator, bool no_demangle,
  rmw_names_and_types_t * output)
{
  const rmw_ret_t validation = validate_names_and_types_arguments(node, allocator, output);
  if (validation != RMW_RET_OK) {return validation;}
  auto * context = context_of(node);
  const rmw_ret_t refresh = rmw_zzdds_cpp::refresh_discovered_endpoints(context);
  if (refresh != RMW_RET_OK) {return refresh;}
  return context->common.graph_cache.get_names_and_types(
    no_demangle ? identity_name : demangle_dds_topic,
    no_demangle ? identity_name : demangle_dds_type, allocator, output);
}

rmw_ret_t rmw_get_publisher_names_and_types_by_node(
  const rmw_node_t * node, rcutils_allocator_t * allocator, const char * node_name,
  const char * node_namespace, bool no_demangle, rmw_names_and_types_t * output)
{
  const rmw_ret_t validation = validate_names_and_types_arguments(node, allocator, output);
  if (validation != RMW_RET_OK) {return validation;}
  const rmw_ret_t node_validation = validate_queried_node(node_name, node_namespace);
  if (node_validation != RMW_RET_OK) {return node_validation;}
  auto * context = context_of(node);
  const rmw_ret_t refresh = rmw_zzdds_cpp::refresh_discovered_endpoints(context);
  if (refresh != RMW_RET_OK) {return refresh;}
  return context->common.graph_cache.get_writer_names_and_types_by_node(
    node_name, node_namespace, no_demangle ? identity_name : demangle_dds_topic,
    no_demangle ? identity_name : demangle_dds_type, allocator, output);
}

rmw_ret_t rmw_get_subscriber_names_and_types_by_node(
  const rmw_node_t * node, rcutils_allocator_t * allocator, const char * node_name,
  const char * node_namespace, bool no_demangle, rmw_names_and_types_t * output)
{
  const rmw_ret_t validation = validate_names_and_types_arguments(node, allocator, output);
  if (validation != RMW_RET_OK) {return validation;}
  const rmw_ret_t node_validation = validate_queried_node(node_name, node_namespace);
  if (node_validation != RMW_RET_OK) {return node_validation;}
  auto * context = context_of(node);
  const rmw_ret_t refresh = rmw_zzdds_cpp::refresh_discovered_endpoints(context);
  if (refresh != RMW_RET_OK) {return refresh;}
  return context->common.graph_cache.get_reader_names_and_types_by_node(
    node_name, node_namespace, no_demangle ? identity_name : demangle_dds_topic,
    no_demangle ? identity_name : demangle_dds_type, allocator, output);
}

rmw_ret_t rmw_get_service_names_and_types(
  const rmw_node_t * node, rcutils_allocator_t * allocator, rmw_names_and_types_t * output)
{
  const rmw_ret_t validation = validate_names_and_types_arguments(node, allocator, output);
  if (validation != RMW_RET_OK) {return validation;}
  auto * context = context_of(node);
  const rmw_ret_t refresh = rmw_zzdds_cpp::refresh_discovered_endpoints(context);
  if (refresh != RMW_RET_OK) {return refresh;}
  rcutils_string_array_t names = rcutils_get_zero_initialized_string_array();
  rcutils_string_array_t namespaces = rcutils_get_zero_initialized_string_array();
  rmw_ret_t result = context->common.graph_cache.get_node_names(
    &names, &namespaces, nullptr, allocator);
  NamesAndTypes services;
  for (size_t index = 0U; result == RMW_RET_OK && index < names.size; ++index) {
    rmw_names_and_types_t node_services = rmw_get_zero_initialized_names_and_types();
    result = get_service_names_and_types_from_cache(
      context, allocator, names.data[index], namespaces.data[index], false, &node_services);
    if (result == RMW_RET_OK) {
      try {
        for (size_t service_index = 0U; service_index < node_services.names.size;
          ++service_index)
        {
          for (size_t type_index = 0U; type_index < node_services.types[service_index].size;
            ++type_index)
          {
            services[node_services.names.data[service_index]].insert(
              node_services.types[service_index].data[type_index]);
          }
        }
      } catch (const std::bad_alloc &) {
        RMW_SET_ERROR_MSG("failed to allocate service graph bookkeeping");
        result = RMW_RET_BAD_ALLOC;
      }
    }
    const rmw_ret_t node_services_fini = rmw_names_and_types_fini(&node_services);
    (void)node_services_fini;
  }
  if (result == RMW_RET_OK) {result = fill_names_and_types(services, allocator, output);}
  const rcutils_ret_t namespaces_fini = rcutils_string_array_fini(&namespaces);
  const rcutils_ret_t names_fini = rcutils_string_array_fini(&names);
  (void)namespaces_fini;
  (void)names_fini;
  return result;
}

rmw_ret_t rmw_get_service_names_and_types_by_node(
  const rmw_node_t * node, rcutils_allocator_t * allocator, const char * node_name,
  const char * node_namespace, rmw_names_and_types_t * output)
{
  const rmw_ret_t validation = validate_names_and_types_arguments(node, allocator, output);
  if (validation != RMW_RET_OK) {return validation;}
  const rmw_ret_t node_validation = validate_queried_node(node_name, node_namespace);
  if (node_validation != RMW_RET_OK) {return node_validation;}
  auto * context = context_of(node);
  const rmw_ret_t refresh = rmw_zzdds_cpp::refresh_discovered_endpoints(context);
  if (refresh != RMW_RET_OK) {return refresh;}
  return get_service_names_and_types_from_cache(
    context, allocator, node_name, node_namespace, false, output);
}

rmw_ret_t rmw_get_client_names_and_types_by_node(
  const rmw_node_t * node, rcutils_allocator_t * allocator, const char * node_name,
  const char * node_namespace, rmw_names_and_types_t * output)
{
  const rmw_ret_t validation = validate_names_and_types_arguments(node, allocator, output);
  if (validation != RMW_RET_OK) {return validation;}
  const rmw_ret_t node_validation = validate_queried_node(node_name, node_namespace);
  if (node_validation != RMW_RET_OK) {return node_validation;}
  auto * context = context_of(node);
  const rmw_ret_t refresh = rmw_zzdds_cpp::refresh_discovered_endpoints(context);
  if (refresh != RMW_RET_OK) {return refresh;}
  return get_service_names_and_types_from_cache(
    context, allocator, node_name, node_namespace, true, output);
}

rmw_ret_t rmw_get_publishers_info_by_topic(
  const rmw_node_t * node, rcutils_allocator_t * allocator, const char * requested_name,
  bool no_mangle, rmw_topic_endpoint_info_array_t * output)
{
  const rmw_ret_t validation = validate_endpoint_arguments(node, allocator, requested_name, output);
  if (validation != RMW_RET_OK) {return validation;}
  const rmw_ret_t zero = rmw_topic_endpoint_info_array_check_zero(output);
  if (zero != RMW_RET_OK) {return RMW_RET_INVALID_ARGUMENT;}
  auto * context = context_of(node);
  const rmw_ret_t refresh = rmw_zzdds_cpp::refresh_discovered_endpoints(context);
  if (refresh != RMW_RET_OK) {return refresh;}
  const std::string dds_topic = no_mangle ? requested_name : std::string("rt") + requested_name;
  return context->common.graph_cache.get_writers_info_by_topic(
    dds_topic, no_mangle ? identity_name : demangle_dds_type, allocator, output);
}

rmw_ret_t rmw_get_subscriptions_info_by_topic(
  const rmw_node_t * node, rcutils_allocator_t * allocator, const char * requested_name,
  bool no_mangle, rmw_topic_endpoint_info_array_t * output)
{
  const rmw_ret_t validation = validate_endpoint_arguments(node, allocator, requested_name, output);
  if (validation != RMW_RET_OK) {return validation;}
  const rmw_ret_t zero = rmw_topic_endpoint_info_array_check_zero(output);
  if (zero != RMW_RET_OK) {return RMW_RET_INVALID_ARGUMENT;}
  auto * context = context_of(node);
  const rmw_ret_t refresh = rmw_zzdds_cpp::refresh_discovered_endpoints(context);
  if (refresh != RMW_RET_OK) {return refresh;}
  const std::string dds_topic = no_mangle ? requested_name : std::string("rt") + requested_name;
  return context->common.graph_cache.get_readers_info_by_topic(
    dds_topic, no_mangle ? identity_name : demangle_dds_type, allocator, output);
}

rmw_ret_t rmw_get_clients_info_by_service(
  const rmw_node_t * node, rcutils_allocator_t * allocator, const char * requested_name,
  bool no_mangle, rmw_service_endpoint_info_array_t * output)
{
  const rmw_ret_t validation = validate_endpoint_arguments(node, allocator, requested_name, output);
  if (validation != RMW_RET_OK) {return validation;}
  const rmw_ret_t zero = rmw_service_endpoint_info_array_check_zero(output);
  if (zero != RMW_RET_OK) {return RMW_RET_INVALID_ARGUMENT;}
  if (no_mangle) {
    RMW_SET_ERROR_MSG("unmangled service endpoint queries are unsupported for DDS services");
    return RMW_RET_UNSUPPORTED;
  }
  auto * context = context_of(node);
  const rmw_ret_t refresh = rmw_zzdds_cpp::refresh_discovered_endpoints(context);
  if (refresh != RMW_RET_OK) {return refresh;}
  rmw_topic_endpoint_info_array_t readers = rmw_get_zero_initialized_topic_endpoint_info_array();
  rmw_topic_endpoint_info_array_t writers = rmw_get_zero_initialized_topic_endpoint_info_array();
  const std::string service(requested_name);
  rmw_ret_t result = context->common.graph_cache.get_readers_info_by_topic(
    "rt/rr" + service + "Reply", demangle_dds_service_type, allocator, &readers);
  if (result == RMW_RET_OK) {
    result = context->common.graph_cache.get_writers_info_by_topic(
      "rt/rq" + service + "Request", demangle_dds_service_type, allocator, &writers);
  }
  if (result == RMW_RET_OK) {
    result = context->common.graph_cache.get_clients_info_by_service(
      &readers, &writers, allocator, output);
  }
  const rmw_ret_t writers_fini = rmw_topic_endpoint_info_array_fini(&writers, allocator);
  const rmw_ret_t readers_fini = rmw_topic_endpoint_info_array_fini(&readers, allocator);
  (void)writers_fini;
  (void)readers_fini;
  return result;
}

rmw_ret_t rmw_get_servers_info_by_service(
  const rmw_node_t * node, rcutils_allocator_t * allocator, const char * requested_name,
  bool no_mangle, rmw_service_endpoint_info_array_t * output)
{
  const rmw_ret_t validation = validate_endpoint_arguments(node, allocator, requested_name, output);
  if (validation != RMW_RET_OK) {return validation;}
  const rmw_ret_t zero = rmw_service_endpoint_info_array_check_zero(output);
  if (zero != RMW_RET_OK) {return RMW_RET_INVALID_ARGUMENT;}
  if (no_mangle) {
    RMW_SET_ERROR_MSG("unmangled service endpoint queries are unsupported for DDS services");
    return RMW_RET_UNSUPPORTED;
  }
  auto * context = context_of(node);
  const rmw_ret_t refresh = rmw_zzdds_cpp::refresh_discovered_endpoints(context);
  if (refresh != RMW_RET_OK) {return refresh;}
  rmw_topic_endpoint_info_array_t readers = rmw_get_zero_initialized_topic_endpoint_info_array();
  rmw_topic_endpoint_info_array_t writers = rmw_get_zero_initialized_topic_endpoint_info_array();
  const std::string service(requested_name);
  rmw_ret_t result = context->common.graph_cache.get_readers_info_by_topic(
    "rt/rq" + service + "Request", demangle_dds_service_type, allocator, &readers);
  if (result == RMW_RET_OK) {
    result = context->common.graph_cache.get_writers_info_by_topic(
      "rt/rr" + service + "Reply", demangle_dds_service_type, allocator, &writers);
  }
  if (result == RMW_RET_OK) {
    result = context->common.graph_cache.get_servers_info_by_service(
      &readers, &writers, allocator, output);
  }
  const rmw_ret_t writers_fini = rmw_topic_endpoint_info_array_fini(&writers, allocator);
  const rmw_ret_t readers_fini = rmw_topic_endpoint_info_array_fini(&readers, allocator);
  (void)writers_fini;
  (void)readers_fini;
  return result;
}

rmw_ret_t rmw_count_publishers(const rmw_node_t * node, const char * topic_name, size_t * count)
{
  const rmw_ret_t result = validate_count_arguments(node, topic_name, count);
  if (result != RMW_RET_OK) {return result;}
  auto * context = context_of(node);
  const rmw_ret_t refresh = rmw_zzdds_cpp::refresh_discovered_endpoints(context);
  if (refresh != RMW_RET_OK) {return refresh;}
  const std::string mangled = std::string("rt") + topic_name;
  size_t native_count = 0U;
  rmw_ret_t cache_result = context->common.graph_cache.get_writer_count(mangled, count);
  if (cache_result == RMW_RET_OK) {
    cache_result = context->common.graph_cache.get_writer_count(topic_name, &native_count);
    *count += native_count;
  }
  return cache_result;
}

rmw_ret_t rmw_count_subscribers(const rmw_node_t * node, const char * topic_name, size_t * count)
{
  const rmw_ret_t result = validate_count_arguments(node, topic_name, count);
  if (result != RMW_RET_OK) {return result;}
  auto * context = context_of(node);
  const rmw_ret_t refresh = rmw_zzdds_cpp::refresh_discovered_endpoints(context);
  if (refresh != RMW_RET_OK) {return refresh;}
  const std::string mangled = std::string("rt") + topic_name;
  size_t native_count = 0U;
  rmw_ret_t cache_result = context->common.graph_cache.get_reader_count(mangled, count);
  if (cache_result == RMW_RET_OK) {
    cache_result = context->common.graph_cache.get_reader_count(topic_name, &native_count);
    *count += native_count;
  }
  return cache_result;
}

rmw_ret_t rmw_count_clients(const rmw_node_t * node, const char * service_name, size_t * count)
{
  const rmw_ret_t result = validate_count_arguments(node, service_name, count);
  if (result != RMW_RET_OK) {return result;}
  auto * context = context_of(node);
  auto allocator = context->allocator;
  rmw_service_endpoint_info_array_t info =
    rmw_get_zero_initialized_service_endpoint_info_array();
  const rmw_ret_t query = rmw_get_clients_info_by_service(
    node, &allocator, service_name, false, &info);
  if (query == RMW_RET_OK) {*count = info.size;}
  const rmw_ret_t fini = rmw_service_endpoint_info_array_fini(&info, &allocator);
  (void)fini;
  return query;
}

rmw_ret_t rmw_count_services(const rmw_node_t * node, const char * service_name, size_t * count)
{
  const rmw_ret_t result = validate_count_arguments(node, service_name, count);
  if (result != RMW_RET_OK) {return result;}
  auto * context = context_of(node);
  auto allocator = context->allocator;
  rmw_service_endpoint_info_array_t info =
    rmw_get_zero_initialized_service_endpoint_info_array();
  const rmw_ret_t query = rmw_get_servers_info_by_service(
    node, &allocator, service_name, false, &info);
  if (query == RMW_RET_OK) {*count = info.size;}
  const rmw_ret_t fini = rmw_service_endpoint_info_array_fini(&info, &allocator);
  (void)fini;
  return query;
}
}

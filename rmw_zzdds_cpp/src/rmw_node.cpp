#include <new>

#include "rmw/error_handling.h"
#include "rmw/rmw.h"
#include "rmw/validate_namespace.h"
#include "rmw/validate_node_name.h"

#include "rmw_zzdds_cpp/allocator.hpp"
#include "rmw_zzdds_cpp/context_impl.hpp"
#include "rmw_zzdds_cpp/endpoint_impl.hpp"
#include "rmw_zzdds_cpp/identifier.hpp"
#include "rmw_zzdds_cpp/node_impl.hpp"
#include "rmw_zzdds_cpp/service_impl.hpp"

namespace
{

bool valid_name(const char * name)
{
  int result = RMW_NODE_NAME_VALID;
  if (rmw_validate_node_name(name, &result, nullptr) != RMW_RET_OK) {
    return false;
  }
  if (result != RMW_NODE_NAME_VALID) {
    RMW_SET_ERROR_MSG(rmw_node_name_validation_result_string(result));
    return false;
  }
  return true;
}

bool valid_namespace(const char * namespace_)
{
  int result = RMW_NAMESPACE_VALID;
  if (rmw_validate_namespace(namespace_, &result, nullptr) != RMW_RET_OK) {
    return false;
  }
  if (result != RMW_NAMESPACE_VALID) {
    RMW_SET_ERROR_MSG(rmw_namespace_validation_result_string(result));
    return false;
  }
  return true;
}

bool node_owns_entities(
  const rmw_zzdds_cpp::ContextImpl * context, const rmw_node_t * node)
{
  for (const rmw_publisher_t * publisher : context->publishers) {
    const auto * impl = static_cast<const rmw_zzdds_cpp::PublisherImpl *>(publisher->data);
    if (impl->node == node) {return true;}
  }
  for (const rmw_subscription_t * subscription : context->subscriptions) {
    const auto * impl = static_cast<const rmw_zzdds_cpp::SubscriptionImpl *>(subscription->data);
    if (impl->node == node) {return true;}
  }
  for (const rmw_client_t * client : context->clients) {
    const auto * impl = static_cast<const rmw_zzdds_cpp::ClientImpl *>(client->data);
    if (impl->node == node) {return true;}
  }
  for (const rmw_service_t * service : context->services) {
    const auto * impl = static_cast<const rmw_zzdds_cpp::ServiceImpl *>(service->data);
    if (impl->node == node) {return true;}
  }
  return false;
}

}  // namespace

extern "C"
{

rmw_node_t * rmw_create_node(
  rmw_context_t * context, const char * name, const char * namespace_)
{
  if (context == nullptr || name == nullptr || namespace_ == nullptr) {
    RMW_SET_ERROR_MSG("context, node name, and namespace are required");
    return nullptr;
  }
  if (context->implementation_identifier != rmw_zzdds_cpp::identifier || context->impl == nullptr) {
    RMW_SET_ERROR_MSG("context is invalid or belongs to another RMW implementation");
    return nullptr;
  }
  if (!valid_name(name) || !valid_namespace(namespace_)) {
    return nullptr;
  }

  auto * context_impl = reinterpret_cast<rmw_zzdds_cpp::ContextImpl *>(context->impl);
  const std::lock_guard<std::mutex> lock(context_impl->mutex);
  if (context_impl->shutdown) {
    RMW_SET_ERROR_MSG("cannot create a node after context shutdown");
    return nullptr;
  }

  const rcutils_allocator_t allocator = context_impl->allocator;
  auto * node_impl = rmw_zzdds_cpp::allocate_object<rmw_zzdds_cpp::NodeImpl>(allocator, context);
  auto * node = rmw_zzdds_cpp::allocate_object<rmw_node_t>(allocator);
  char * name_copy = rmw_zzdds_cpp::duplicate_string(allocator, name);
  char * namespace_copy = rmw_zzdds_cpp::duplicate_string(allocator, namespace_);
  if (node_impl == nullptr || node == nullptr || name_copy == nullptr || namespace_copy == nullptr) {
    if (namespace_copy != nullptr) {
      allocator.deallocate(namespace_copy, allocator.state);
    }
    if (name_copy != nullptr) {
      allocator.deallocate(name_copy, allocator.state);
    }
    rmw_zzdds_cpp::deallocate_object(allocator, node);
    rmw_zzdds_cpp::deallocate_object(allocator, node_impl);
    RMW_SET_ERROR_MSG("failed to allocate node resources");
    return nullptr;
  }

  node->implementation_identifier = rmw_zzdds_cpp::identifier;
  node->data = node_impl;
  node->name = name_copy;
  node->namespace_ = namespace_copy;
  node->context = context;
  try {
    context_impl->nodes.insert(node);
    if (!context_impl->suppress_graph_updates) {
      if (context_impl->common.add_node_graph(name, namespace_) != RMW_RET_OK) {
        context_impl->nodes.erase(node);
        allocator.deallocate(namespace_copy, allocator.state);
        allocator.deallocate(name_copy, allocator.state);
        rmw_zzdds_cpp::deallocate_object(allocator, node);
        rmw_zzdds_cpp::deallocate_object(allocator, node_impl);
        RMW_SET_ERROR_MSG("failed to publish the node graph update");
        return nullptr;
      }
      context_impl->notify_graph_change();
    }
  } catch (const std::bad_alloc &) {
    allocator.deallocate(namespace_copy, allocator.state);
    allocator.deallocate(name_copy, allocator.state);
    rmw_zzdds_cpp::deallocate_object(allocator, node);
    rmw_zzdds_cpp::deallocate_object(allocator, node_impl);
    RMW_SET_ERROR_MSG("failed to register the node in the local graph");
    return nullptr;
  }
  ++context_impl->active_entities;
  return node;
}

rmw_ret_t rmw_destroy_node(rmw_node_t * node)
{
  if (node == nullptr || node->data == nullptr || node->context == nullptr) {
    RMW_SET_ERROR_MSG("a valid node is required");
    return RMW_RET_INVALID_ARGUMENT;
  }
  if (node->implementation_identifier != rmw_zzdds_cpp::identifier) {
    RMW_SET_ERROR_MSG("node belongs to a different RMW implementation");
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION;
  }

  auto * context_impl = reinterpret_cast<rmw_zzdds_cpp::ContextImpl *>(node->context->impl);
  if (context_impl == nullptr) {
    RMW_SET_ERROR_MSG("node context is invalid");
    return RMW_RET_INVALID_ARGUMENT;
  }
  const rcutils_allocator_t allocator = context_impl->allocator;
  auto * node_impl = static_cast<rmw_zzdds_cpp::NodeImpl *>(node->data);
  {
    const std::lock_guard<std::mutex> lock(context_impl->mutex);
    if (node_owns_entities(context_impl, node)) {
      RMW_SET_ERROR_MSG("node still owns publishers, subscriptions, clients, or services");
      return RMW_RET_ERROR;
    }
    context_impl->nodes.erase(node);
    if (!context_impl->suppress_graph_updates) {
      (void)context_impl->common.remove_node_graph(node->name, node->namespace_);
      context_impl->notify_graph_change();
    }
  }
  allocator.deallocate(const_cast<char *>(node->namespace_), allocator.state);
  allocator.deallocate(const_cast<char *>(node->name), allocator.state);
  rmw_zzdds_cpp::deallocate_object(allocator, node_impl);
  rmw_zzdds_cpp::deallocate_object(allocator, node);
  --context_impl->active_entities;
  return RMW_RET_OK;
}

}  // extern "C"

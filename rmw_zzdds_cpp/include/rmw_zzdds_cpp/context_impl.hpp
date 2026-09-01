#ifndef RMW_ZZDDS_CPP__CONTEXT_IMPL_HPP_
#define RMW_ZZDDS_CPP__CONTEXT_IMPL_HPP_

#include <atomic>
#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <set>
#include <unordered_set>
#include <unordered_map>

#include "rcutils/allocator.h"
#include "rmw/types.h"
#include "rmw_dds_common/context.hpp"

#include "dcps.h"
#include "zzdds_c.h"
#include "rmw_zzdds_cpp/guard_condition_impl.hpp"

namespace rmw_zzdds_cpp
{

class DdsContext final
{
public:
  DdsContext() = default;
  DdsContext(const DdsContext &) = delete;
  DdsContext & operator=(const DdsContext &) = delete;
  ~DdsContext();

  bool initialize(uint32_t domain_id) noexcept;
  bool finalize() noexcept;

  DDS_DomainParticipant participant() const noexcept {return participant_;}
  DDS_Publisher publisher() const noexcept {return publisher_;}
  DDS_Subscriber subscriber() const noexcept {return subscriber_;}

private:
  bool reset() noexcept;

  zzdds_DomainParticipantFactory factory_{nullptr};
  DDS_DomainParticipantFactory dds_factory_{nullptr};
  DDS_DomainParticipant participant_{nullptr};
  DDS_Publisher publisher_{nullptr};
  DDS_Subscriber subscriber_{nullptr};
};

struct ContextImpl final
{
  ContextImpl(rcutils_allocator_t allocator_in, uint32_t domain_id_in)
  : allocator(allocator_in), domain_id(domain_id_in) {}

  rcutils_allocator_t allocator;
  uint32_t domain_id;
  DdsContext dds;
  rmw_gid_t participant_gid{};
  std::mutex mutex;
  std::set<std::array<uint8_t, 16U>> local_publication_guids;
  rmw_dds_common::Context common{};
  std::set<std::array<uint8_t, 16U>> discovered_writer_guids;
  std::set<std::array<uint8_t, 16U>> discovered_reader_guids;
  std::unordered_map<DDS_InstanceHandle_t, rmw_gid_t> discovered_writer_instances;
  std::unordered_map<DDS_InstanceHandle_t, rmw_gid_t> discovered_reader_instances;
  std::unordered_set<rmw_node_t *> nodes;
  std::unordered_set<rmw_publisher_t *> publishers;
  std::unordered_set<rmw_subscription_t *> subscriptions;
  std::unordered_set<rmw_client_t *> clients;
  std::unordered_set<rmw_service_t *> services;
  DDS_GuardCondition graph_guard_native{nullptr};
  GuardConditionImpl * graph_guard_impl{nullptr};
  rmw_guard_condition_t * graph_guard_handle{nullptr};
  size_t next_content_filter_id{0U};
  std::atomic_size_t active_entities{0U};
  rmw_node_t * graph_node{nullptr};
  bool suppress_graph_updates{false};
  bool shutdown{false};

  void notify_graph_change() const noexcept
  {
    if (graph_guard_native != nullptr) {
      (void)DDS_GuardCondition_set_trigger_value(graph_guard_native, true);
    }
  }
};

}  // namespace rmw_zzdds_cpp

#endif  // RMW_ZZDDS_CPP__CONTEXT_IMPL_HPP_

#ifndef RMW_ZZDDS_CPP__MATCHED_STATUS_HPP_
#define RMW_ZZDDS_CPP__MATCHED_STATUS_HPP_

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <unordered_set>
#include <utility>

#include "zzdds_c.h"

namespace rmw_zzdds_cpp
{

// A subscription's matched status, kept continuous when a content-filter
// change replaces its DDS reader. The replacement reader starts from zero and
// matches again every publication the old one had matched; those re-matches
// are not new matches for the subscription. Not thread-safe: the owner
// (SubscriptionImpl) guards it with its event_mutex.
struct SubscriptionMatchedContinuity
{
  // The subscription's status as reported to rmw: absolute counts, plus the
  // changes accumulated since the matched event was last taken.
  DDS_SubscriptionMatchedStatus status{};
  // Added to the current reader's own total_count to give status.total_count.
  int32_t total_offset{0};
  // Publications the replaced reader had matched that the current reader has
  // not matched yet.
  std::unordered_set<DDS_InstanceHandle_t> inherited;

  // The bookkeeping that counts the current reader's matches, saved by
  // begin_replacement so a failed replacement can return to it.
  struct Checkpoint
  {
    int32_t total_offset{0};
    std::unordered_set<DDS_InstanceHandle_t> inherited;
  };

  // Starts counting for a replacement reader. `publications` are the
  // publications the reader being replaced currently matches; call after
  // folding in that reader's final status. Returns what cancel_replacement
  // needs to go back to counting for the reader being replaced.
  Checkpoint begin_replacement(const DDS_InstanceHandle_t * publications, size_t count)
  {
    Checkpoint previous{total_offset, std::move(inherited)};
    inherited.clear();
    inherited.insert(publications, publications + count);
    total_offset = status.total_count;
    return previous;
  }

  // Goes back to counting for the reader begin_replacement replaced. Only the
  // bookkeeping is restored, not `status`: changes taken meanwhile stay taken.
  // Then fold in that reader's current status as usual.
  void cancel_replacement(Checkpoint && previous)
  {
    total_offset = previous.total_offset;
    inherited = std::move(previous.inherited);
  }

  // Folds in the current reader's status. `publications` are the publications
  // that reader currently matches; they are only consulted while some
  // inherited publication has not been matched again.
  void fold(
    const DDS_SubscriptionMatchedStatus & reader_status,
    const DDS_InstanceHandle_t * publications, size_t count)
  {
    for (size_t i = 0; i < count && !inherited.empty(); ++i) {
      if (inherited.erase(publications[i]) > 0U) {--total_offset;}
    }
    // Changes come from the absolute counts rather than the reader's own
    // *_change fields, so a re-match adds nothing. The total never
    // decreases: a publication can show up in the matched list just before
    // the reader's total includes it. current_count follows the reader as
    // is, so it can dip while a replacement reader re-matches.
    const int32_t total = std::max(status.total_count, total_offset + reader_status.total_count);
    status.total_count_change += total - status.total_count;
    status.current_count_change += reader_status.current_count - status.current_count;
    status.total_count = total;
    status.current_count = reader_status.current_count;
    status.last_publication_handle = reader_status.last_publication_handle;
  }

  // Returns the status and clears its accumulated changes.
  DDS_SubscriptionMatchedStatus take()
  {
    const DDS_SubscriptionMatchedStatus taken = status;
    status.total_count_change = 0;
    status.current_count_change = 0;
    return taken;
  }
};

}  // namespace rmw_zzdds_cpp

#endif  // RMW_ZZDDS_CPP__MATCHED_STATUS_HPP_

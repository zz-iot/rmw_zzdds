#include <gtest/gtest.h>

#include <vector>

#include "rmw_zzdds_cpp/matched_status.hpp"

namespace
{
using rmw_zzdds_cpp::SubscriptionMatchedContinuity;

constexpr DDS_InstanceHandle_t kA = 11;
constexpr DDS_InstanceHandle_t kB = 12;
constexpr DDS_InstanceHandle_t kC = 13;
constexpr DDS_InstanceHandle_t kD = 14;

// One reader's own view: its lifetime total and its current matches.
struct Reader
{
  int32_t total = 0;
  std::vector<DDS_InstanceHandle_t> matched;

  void match(DDS_InstanceHandle_t h) {++total; matched.push_back(h);}
  DDS_SubscriptionMatchedStatus status() const
  {
    DDS_SubscriptionMatchedStatus s{};
    s.total_count = total;
    s.current_count = static_cast<int32_t>(matched.size());
    return s;
  }
};

void fold(SubscriptionMatchedContinuity & c, const Reader & r)
{
  c.fold(r.status(), r.matched.data(), r.matched.size());
}

void begin_replacement(SubscriptionMatchedContinuity & c, const Reader & old_reader)
{
  c.begin_replacement(old_reader.matched.data(), old_reader.matched.size());
}

TEST(SubscriptionMatchedContinuity, counts_each_match_of_a_single_reader)
{
  SubscriptionMatchedContinuity c;
  Reader r;
  r.match(kA);
  fold(c, r);
  r.match(kB);
  fold(c, r);
  const auto s = c.take();
  EXPECT_EQ(2, s.total_count);
  EXPECT_EQ(2, s.total_count_change);
  EXPECT_EQ(2, s.current_count);
  EXPECT_EQ(2, s.current_count_change);
  const auto again = c.take();
  EXPECT_EQ(0, again.total_count_change);
  EXPECT_EQ(0, again.current_count_change);
}

TEST(SubscriptionMatchedContinuity, replacement_rematching_the_same_publications_adds_nothing)
{
  SubscriptionMatchedContinuity c;
  Reader old_reader;
  old_reader.match(kA);
  old_reader.match(kB);
  fold(c, old_reader);
  (void)c.take();

  begin_replacement(c, old_reader);
  Reader replacement;
  replacement.match(kA);
  fold(c, replacement);
  replacement.match(kB);
  fold(c, replacement);
  const auto s = c.take();
  EXPECT_EQ(2, s.total_count);
  EXPECT_EQ(0, s.total_count_change);
  EXPECT_EQ(2, s.current_count);
  EXPECT_EQ(0, s.current_count_change);
}

// A publication matched by the old reader disappears before the replacement
// matches it; a later, genuinely new publication must still count.
TEST(SubscriptionMatchedContinuity, new_publication_counts_when_an_inherited_one_never_rematches)
{
  SubscriptionMatchedContinuity c;
  Reader old_reader;
  old_reader.match(kA);
  old_reader.match(kB);
  old_reader.match(kC);
  fold(c, old_reader);
  (void)c.take();

  begin_replacement(c, old_reader);
  Reader replacement;
  replacement.match(kA);
  fold(c, replacement);
  replacement.match(kB);
  fold(c, replacement);
  replacement.match(kD);
  fold(c, replacement);
  const auto s = c.take();
  EXPECT_EQ(4, s.total_count);
  EXPECT_EQ(1, s.total_count_change);
  EXPECT_EQ(3, s.current_count);
  EXPECT_EQ(0, s.current_count_change);
}

// The matched list can include a publication before the reader's total does;
// the total must not go down meanwhile.
TEST(SubscriptionMatchedContinuity, total_never_decreases_when_the_list_runs_ahead)
{
  SubscriptionMatchedContinuity c;
  Reader old_reader;
  old_reader.match(kA);
  fold(c, old_reader);
  (void)c.take();

  begin_replacement(c, old_reader);
  Reader replacement;
  const std::vector<DDS_InstanceHandle_t> listed{kA};
  c.fold(replacement.status(), listed.data(), listed.size());  // total not yet updated
  EXPECT_EQ(1, c.status.total_count);
  EXPECT_EQ(0, c.status.total_count_change);
  replacement.match(kA);
  fold(c, replacement);
  const auto s = c.take();
  EXPECT_EQ(1, s.total_count);
  EXPECT_EQ(0, s.total_count_change);
}
}  // namespace

#ifndef PINTOOL_CACHE_CACHE_H
#define PINTOOL_CACHE_CACHE_H

#include <algorithm>
#include <limits>
#include <list>
#include <map>
#include <set>
#include <vector>

#include "pin_profile.H"

#include "misc.h"

#define KILO 1024
#define MEGA (KILO * KILO)
#define GIGA (MEGA * KILO)

static inline UINT32 FirstReadSize(INS ins)
{
    for (UINT32 i = 0; i < INS_MemoryOperandCount(ins); i++)
        if (INS_MemoryOperandIsRead(ins, i))
            return INS_MemoryOperandSize(ins, i);
    return 0;
}

static inline UINT32 FirstWriteSize(INS ins)
{
    for (UINT32 i = 0; i < INS_MemoryOperandCount(ins); i++)
        if (INS_MemoryOperandIsWritten(ins, i))
            return INS_MemoryOperandSize(ins, i);
    return 0;
}

class CacheTag // {{{
{
 public:
  CacheTag(ADDRINT tag = 0) : tag_(tag) { }
  bool operator==(const CacheTag& right) const { return tag_ == right.tag_; }
  operator ADDRINT() const { return tag_; }

 private:
  ADDRINT tag_;
}; // CacheTag }}}

// Base cache class {{{
class BaseCache
{
 public:
  typedef enum {
    kWriteAllocate,
    kNoWriteAllocate,
    kNumAllocationTypes,
  } AllocationType;

  typedef enum {
    kICache,
    kDCache,
    kUCache,
    kNumCacheTypes,
  } CacheType;

  typedef enum {
    kLoad,
    kStore,
    kNumAccessTypes,
  } AccessType;

  static const UINT32 kMissHit   = 2;
  static const UINT32 kMruOthers = 2;
  typedef UINT64 CacheStats;
  typedef CounterArray<CacheStats,
          (kNumAccessTypes * kMissHit) + (kNumAccessTypes * kMruOthers)>
    HitMissByAccessType;
  typedef CompressorCounter<ADDRINT, UINT32, HitMissByAccessType> Profile;

  typedef struct {
    UINT64 replace_count;
    UINT64 live_time;
    UINT64 dead_time;
    UINT64 access_count;
    UINT64 access_interval;
  } SetLife;

  typedef std::vector<SetLife> VSetLife;

  BaseCache(std::string name, UINT32 size, UINT32 line_size, UINT32 num_ways);

  UINT32 size()      const { return size_; }
  UINT32 line_size() const { return line_size_; }
  UINT32 num_ways()  const { return num_ways_; }
  UINT32 num_sets()  const { return num_sets_; }
  UINT64 miss()            { return miss_hit_[0]; }
  UINT64 hit()             { return miss_hit_[1]; }
  UINT64 access()          { return miss_hit_[0] + miss_hit_[1]; }

  void SplitAddr(const ADDRINT addr, CacheTag& tag, UINT32& set_index) const;
  void SplitAddr(const ADDRINT addr, CacheTag& tag, UINT32& set_index,
      UINT32& line_index) const;

  // For optimal caches
  void IncInst(UINT32 inst_id, AccessType access_type, bool is_hit) {
    of_inst_[inst_id][access_type * kMissHit + is_hit]++;
  }
  void IncAddr(UINT32 addr_id, AccessType access_type,
      bool is_hit) {
    of_addr_[addr_id][access_type * kMissHit + is_hit]++;
  }
  void IncTime(UINT32 epoch, AccessType access_type,
      bool is_hit) {
    of_time_[epoch][access_type * kMissHit + is_hit]++;
  }
  // For a realistic cache
  void IncInst(UINT32 inst_id, AccessType access_type, bool is_hit, bool is_mru) {
    of_inst_[inst_id][(access_type * kMissHit) + is_hit]++;
    if (is_hit) {
      UINT32 index = (kNumAccessTypes * kMissHit) + (access_type * kMruOthers) + is_mru;
      of_inst_[inst_id][index]++;
    }
  }
  void IncAddr(UINT32 addr_id, AccessType access_type, bool is_hit, bool is_mru) {
    of_addr_[addr_id][(access_type * kMissHit) + is_hit]++;
    if (is_hit) {
      UINT32 index = (kNumAccessTypes * kMissHit) + (access_type * kMruOthers) + is_mru;
      of_addr_[addr_id][index]++;
    }
  }
  void IncTime(UINT32 epoch, AccessType access_type, bool is_hit, bool is_mru) {
    of_time_[epoch][(access_type * kMissHit) + is_hit]++;
    if (is_hit) {
      UINT32 index = (kNumAccessTypes * kMissHit) + (access_type * kMruOthers) + is_mru;
      of_time_[epoch][index]++;
    }
  }
  void Inc(bool is_hit) { miss_hit_[is_hit]++; }

  Profile  *of_inst() { return &of_inst_; }
  Profile  *of_addr() { return &of_addr_; }
  Profile  *of_time() { return &of_time_; }
  VSetLife *of_life() { return &v_set_life_; }

 protected:
  // Too lazy to write a 2D array class
  Profile of_inst_;
  Profile of_addr_;
  Profile of_time_;
  VSetLife v_set_life_;

 private:
  const std::string name_;
  const UINT32 size_;
  const UINT32 line_size_;
  const UINT32 num_ways_;

  const UINT32 num_sets_;
  const UINT32 addr_shift_;

        UINT32 miss_hit_[kMissHit];
};

BaseCache::BaseCache(std::string name, UINT32 size, UINT32 line_size, UINT32 num_ways)
    : name_(name),
      size_(size),
      line_size_(line_size),
      num_ways_(num_ways),
      num_sets_(size / (num_ways * line_size)),
      addr_shift_(FloorLog2(line_size))
{
  ASSERTX(num_sets_ >= 1);
  ASSERTX(IsPower2(line_size_));
  ASSERTX(IsPower2(num_sets_));

  v_set_life_.resize(num_sets_);
  miss_hit_[0] = 0;
  miss_hit_[1] = 0;
}

void BaseCache::SplitAddr(const ADDRINT addr, CacheTag& tag, UINT32& set_index)
  const
{
  tag = addr >> addr_shift_;
  set_index = tag & (num_sets_ - 1);
}

void BaseCache::SplitAddr(const ADDRINT addr, CacheTag& tag, UINT32& set_index,
    UINT32& line_index) const
{
  line_index = addr & (line_size_ - 1);
  SplitAddr(addr, tag, set_index);
}
// BaseCache class }}}

// Global typedefs {{{
typedef struct {
  CacheTag tag;
  UINT32 inst_id;
  UINT32 addr_id;
  BaseCache::AccessType access_type;
  INT64 next_ref;
  INT64 priority;
  UINT32 epoch;
} AccessTrace;

typedef INT64 Priority;
typedef std::map<CacheTag, Priority> MStack;
typedef std::vector<std::pair<CacheTag, Priority> > VStack;
typedef std::vector<AccessTrace> VTrace;
// }}}

namespace cache_set // {{{
{
// Cache set with Round Robin replacement policy {{{
template <UINT32 kMaxNumWays>
class RoundRobin
{
 public:
  RoundRobin(UINT32 num_ways = kMaxNumWays)
      : num_ways_(num_ways) { }

  void   SetNumWays(UINT32 num_ways) { num_ways_ = num_ways; }
  UINT32 GetNumWays()                { return num_ways_; }

  // Needs to update
  UINT32 Find(UINT32 set_index, CacheTag tag) {
    ASSERTX(tags_.size() <= num_ways_);
    bool is_hit = true;

    for (std::list<CacheTag>::iterator it = tags_.begin(); it != tags_.end(); it++) {
      if (*it == tag) {
        return is_hit;
      }
    }
    is_hit = false;

    return is_hit;
  }
  // Needs to update
  void Replace(CacheTag tag) {
    ASSERTX(tags_.size() <= num_ways_);
    if (tags_.size() == num_ways_) {
      tags_.pop_back();
    }
    tags_.push_front(tag);
  }

 private:
  UINT32 num_ways_;
  std::list<CacheTag> tags_;
}; // }}}

typedef struct {
  bool is_hit;
  bool is_mru;
} HitMru;

typedef struct {
  INT64 first;
  INT64 last;
} LineLife;

typedef std::map<CacheTag, LineLife> MLineLife;

// Cache set with Least Recently Used replacement policy {{{
template <UINT32 kMaxNumWays>
class LeastRecentlyUsed
{
 public:
  LeastRecentlyUsed(UINT32 num_ways = kMaxNumWays)
      : num_ways_(num_ways) { }

  void   SetNumWays(UINT32 num_ways) { num_ways_ = num_ways; }
  UINT32 GetNumWays()                { return num_ways_; }
  INT64  mru_id()                    { return mru_id_; }

  HitMru Find(UINT32 set_index, CacheTag tag, INT64 id) { // set_index is for debugging purpose
    ASSERTX(tags_.size() <= num_ways_);
    HitMru result;
    result.is_hit = false;
    result.is_mru = false;

    for (std::list<CacheTag>::iterator it = tags_.begin(); it != tags_.end(); it++) {
      if (*it == tag) {
        tags_.erase(it);
        tags_.push_front(tag);
        m_line_life_[tag].last = id;
        result.is_hit = true;
        break;
      }
    }

    if (mru_tag_ == tag) {
      result.is_mru = true;
    }
    mru_tag_ = tag;
    mru_id_ = id;

    return result;
  }

  LineLife Replace(CacheTag tag, INT64 id) {
    ASSERTX(tags_.size() <= num_ways_);
    LineLife result;
    result.first = 0;
    result.last  = 0;

    if (tags_.size() == num_ways_) {
      result = m_line_life_[tags_.back()];
      m_line_life_.erase(tags_.back());
      tags_.pop_back();
    }
    m_line_life_[tag].first = id;
    m_line_life_[tag].last = id;
    tags_.push_front(tag);

    return result;
  }

 private:
  UINT32 num_ways_;
  std::list<CacheTag> tags_;
  CacheTag mru_tag_;
  MLineLife m_line_life_;
  INT64 mru_id_;
}; // }}}

// Cache set with Optimal replacement policy based on [Sugumar et al., 1993] {{{
template <UINT32 kMaxNumWays>
class Optimal
{
 public:
  Optimal(UINT32 num_ways = kMaxNumWays)
      : num_ways_(num_ways),
      kMax_(std::numeric_limits<int64_t>::max()),
      ref_id_(0),
      lookahead_ref_count_(0) { }

  void   SetNumWays(UINT32 num_ways)         { num_ways_ = num_ways; }
  UINT32 GetNumWays()                        { return num_ways_; }

  INT64  ref_id()                            { return ref_id_; }
  MStack known_tags()                        { return known_tags_; }
  MStack unknown_tags()                      { return unknown_tags_; }
  MStack unknown_tags_pool()                 { return unknown_tags_pool_; }
  VTrace lookahead_trace()                   { return lookahead_trace_; }
  UINT32 lookahead_ref_count()               { return lookahead_ref_count_; }
  UINT64 num_repairs()                       { return num_repairs_; }

  bool is_known(VTrace::iterator it)         { return it->next_ref != kMax_; }

  void ref()                                 { ref_id_++; }
  void update_known_tags(MStack tags)        { known_tags_ = tags; }
  void update_unknown_tags(MStack tags)      { unknown_tags_ = tags; }
  void update_unknown_tags_pool(MStack tags) { unknown_tags_pool_ = tags; }
  void update_lookahead_trace(VTrace trace)  { lookahead_trace_ = trace; }
  void update_lookahead_ref_count(UINT32 distance) {
    lookahead_ref_count_ = distance;
  }

  void Lookahead(CacheTag tag, BaseCache::AccessType access_type,
      UINT32 inst_id, UINT32 addr_id, UINT32 epoch) {
    bool is_processed = false;
    // Search if previously referenced and update when found
    //
    // TODO(sasaki): this process IS WRONG! Sometimes the index
    // "unknown_in_lookahead_trace_[tag]" doesn't match the correct one (has a
    // value "process_window_" larger) -- need to look into this later
    //
    // if (unknown_in_lookahead_trace_.find(tag) != unknown_in_lookahead_trace_.end()
    //     && lookahead_trace_[unknown_in_lookahead_trace_[tag]].next_ref == kMax_) {
    //   lookahead_trace_[unknown_in_lookahead_trace_[tag]].next_ref = ref_id_;
    //   lookahead_trace_[unknown_in_lookahead_trace_[tag]].priority = kMax_ - ref_id_;
    //   is_processed = true;
    // }
    //
    // I thought the above process using unknown_in_lookahead_trace_ would
    // give a speedup but since it contains a bug I'll keep this for now
    VTrace::iterator it;
    for (it = lookahead_trace_.begin(); it != lookahead_trace_.end(); it++) {
      if (it->tag == tag && !is_known(it)) {
        it->next_ref = ref_id_;
        it->priority = kMax_ - ref_id_;
        is_processed = true;
        break;
      }
    }
    if (!is_processed) {
      // Unknown becomes known: called "stack repair" in the original paper
      // Swap the tag in unknown_tags_pool_ which became known and an unknown
      // in the tags_ with the highest dummy priority with greater priority
      // than the one which becamse known (if any)
      // Not sure how important this is (depends heavily on lookahead_distance)
      if (unknown_tags_.find(tag) != unknown_tags_.end()) {
        unknown_tags_.erase(unknown_tags_.find(tag));
        known_tags_[tag] = kMax_ - ref_id_;
      }
      else if (unknown_tags_pool_.find(tag) != unknown_tags_pool_.end()) {
        ASSERTX(unknown_tags_.size() >= 0);
        // Repair by replacing with the unknown with the highest dummy
        // priority
        if (unknown_tags_.size() > 0) {
          // The vector is just for fast sort
          VStack v_tags(unknown_tags_.begin(), unknown_tags_.end());
          // Sort by priority in ascending order
          // TODO(sasaki): ascending seems correct but I thought it should be
          // descending
          std::sort(v_tags.begin(), v_tags.end(),
              misc_functions::sort_second_ascending<CacheTag, Priority>());
          bool should_repair = false;
          VStack::iterator v;
          for (v = v_tags.begin(); v != v_tags.end(); v++) {
            if (v->second > unknown_tags_pool_[tag]) {
              should_repair = true;
              break;
            }
          }
          if (should_repair) {
            unknown_tags_pool_.erase(tag);
            known_tags_[tag] = kMax_ - ref_id_;
            unknown_tags_.erase(v->first);
            unknown_tags_pool_[v->first] = v->second;
            num_repairs_++;
          }
        }
      }
    }
    // Store the information of the access
    AccessTrace trace;
    trace.tag = tag;
    trace.inst_id = inst_id;
    trace.addr_id = addr_id;
    trace.access_type = access_type;
    trace.next_ref = kMax_;
    trace.priority = -(ref_id_);
    trace.epoch = epoch;
    lookahead_trace_.push_back(trace);
    // unknown_in_lookahead_trace_[tag] = lookahead_ref_count_;
    lookahead_ref_count_++;
  }

 private:
  UINT32 num_ways_;
  const INT64 kMax_;
  INT64 ref_id_;
  MStack known_tags_;
  MStack unknown_tags_;
  MStack unknown_tags_pool_;
  VTrace lookahead_trace_;
  UINT32 lookahead_ref_count_;
  // MStack unknown_in_lookahead_trace_;
  UINT64 num_repairs_;
}; // }}}

// Cache set with Infinite capacity {{{
class Infinite
{
 public:
  Infinite() { }

  void   SetNumWays(UINT32 num_ways_) { }           // dummy
  UINT32 GetNumWays()                 { return 0; } // dummy
  INT64  mru_id()                     { return 0; } // dummy

  HitMru Find(UINT32 set_index, CacheTag tag, INT64 id) {
    HitMru result;
    result.is_hit = false;
    result.is_mru = false;
    result.is_hit = tags_.find(tag) != tags_.end();

    return result;
  }

  LineLife Replace(CacheTag tag, INT64 id) {
    LineLife result;
    result.first = 0;
    result.last  = 0;

    tags_.insert(tag);

    return result;
  }

 private:
  std::set<CacheTag> tags_;
}; // }}}
} // namespace cache_set }}}

// Cache implementation {{{
// Realistic replacement cache class (used for infinite size as well) {{{
template <class CacheSet, const UINT32 kMaxSets, const UINT32 Allocation>
class Cache : public BaseCache
{
 public:
  Cache(std::string name, UINT32 size, UINT32 line_size, UINT32 num_ways)
      : BaseCache(name, size, line_size, num_ways)
  {
    ASSERTX(num_sets() <= kMaxSets);
    for (UINT32 i = 0; i < num_sets(); i++) {
      sets_[i].SetNumWays(num_ways);
    }
    ref_id_ = 0; // TODO(sasaki): fix
  }

  bool AccessMultipleLines(ADDRINT inst_addr, ADDRINT addr,
      UINT32 size, AccessType access_type);
  bool AccessSingleLine(ADDRINT inst_addr, ADDRINT addr,
      AccessType access_type);
  bool FetchMultipleLines(ADDRINT inst_addr, UINT32 size);
  bool FetchSingleLine(ADDRINT inst_addr);

 private:
  CacheSet sets_[kMaxSets];
  INT64 ref_id_;
};

template <class CacheSet, const UINT32 kMaxSets, const UINT32 Allocation>
bool Cache<CacheSet, kMaxSets, Allocation>
    ::AccessMultipleLines(ADDRINT inst_addr, ADDRINT addr,
        UINT32 size, AccessType access_type)
{
  // TODO(sasaki): To make it consistent with optimal replacement I'm
  // commenting this out; as can be seen in the comment in
  // OptimalCache::AccessMultipleLines() it is wrong (original code is correct)
  // so I need to come back and fix it at some point
  //
  // bool is_all_hit = true;
  // const UINT32 inst_id = of_inst_.Map(inst_addr);
  // const UINT32 addr_id = of_addr_.Map(addr);
  // const ADDRINT high_addr = addr + size;
  // const ADDRINT not_line_mask = ~(static_cast<ADDRINT>(line_size()) - 1);
  // do
  // {
  //   CacheTag tag;
  //   UINT32 set_index;
  //
  //   SplitAddr(addr, tag, set_index);
  //   CacheSet& set = sets_[set_index];
  //
  //   bool is_local_hit = set.Find(set_index, tag);
  //   bool should_allocate = (access_type == kLoad ||
  //       Allocation == kWriteAllocate);
  //
  //   if ((!is_local_hit) && should_allocate) {
  //     set.Replace(tag);
  //   }
  //
  //   is_all_hit &= is_local_hit;
  //   addr = (addr & not_line_mask) + line_size();
  // }
  // while (addr < high_addr);
  //
  // IncInst(inst_id, access_type, is_hit);
  // IncAddr(addr_id, access_type, is_hit);
  //
  // CacheTag tag;
  // UINT32 set_index;
  //
  // SplitAddr(addr, tag, set_index);

  CacheTag tag;
  UINT32 set_index;

  SplitAddr(addr, tag, set_index);

  return AccessSingleLine(inst_addr, addr, access_type);
}

template <class CacheSet, const UINT32 kMaxSets, const UINT32 Allocation>
bool Cache<CacheSet, kMaxSets, Allocation>
    ::AccessSingleLine(ADDRINT inst_addr, ADDRINT addr, AccessType access_type)
{
  CacheTag tag;
  UINT32 set_index;

  SplitAddr(addr, tag, set_index);
  CacheSet& set = sets_[set_index];
  const UINT32 inst_id = of_inst_.Map(inst_addr);
  const UINT32 addr_id = of_addr_.Map(addr);
  const UINT32 epoch = of_time_.Map(ref_id_ / KILO);
  ref_id_++;

  v_set_life_[set_index].access_count++;
  v_set_life_[set_index].access_interval += ref_id_ - set.mru_id();
  cache_set::HitMru found = set.Find(set_index, tag, ref_id_);
  bool is_hit = found.is_hit;
  bool is_mru = found.is_mru;

  bool should_allocate = (access_type == kLoad || Allocation == kWriteAllocate);

  if ((!is_hit) && should_allocate) {
    cache_set::LineLife replaced = set.Replace(tag, ref_id_);
    INT64 first = replaced.first;
    INT64 last = replaced.last;

    v_set_life_[set_index].replace_count++;
    v_set_life_[set_index].live_time += last - first;
    v_set_life_[set_index].dead_time += ref_id_ - last;
  }

  IncInst(inst_id, access_type, is_hit, is_mru);
  IncAddr(addr_id, access_type, is_hit, is_mru);
  IncTime(epoch,   access_type, is_hit, is_mru);
  Inc(is_hit);

  return is_hit;
}

template <class CacheSet, const UINT32 kMaxSets, const UINT32 Allocation>
bool Cache<CacheSet, kMaxSets, Allocation>
    ::FetchMultipleLines(ADDRINT inst_addr, UINT32 size)
{
  CacheTag tag;
  UINT32 set_index;

  SplitAddr(inst_addr, tag, set_index);

  return FetchSingleLine(inst_addr);
}

template <class CacheSet, const UINT32 kMaxSets, const UINT32 Allocation>
bool Cache<CacheSet, kMaxSets, Allocation>
    ::FetchSingleLine(ADDRINT inst_addr)
{
  CacheTag tag;
  UINT32 set_index;

  SplitAddr(inst_addr, tag, set_index);
  CacheSet& set = sets_[set_index];
  const UINT32 inst_id = of_inst_.Map(inst_addr);
  const UINT32 epoch = of_time_.Map(ref_id_ / KILO);
  ref_id_++;

  v_set_life_[set_index].access_count++;
  v_set_life_[set_index].access_interval += ref_id_ - set.mru_id();
  cache_set::HitMru found = set.Find(set_index, tag, ref_id_);
  bool is_hit = found.is_hit;
  bool is_mru = found.is_mru;
  bool should_allocate = (Allocation == kWriteAllocate);

  if ((!is_hit) && should_allocate) {
    cache_set::LineLife replaced = set.Replace(tag, ref_id_);
    INT64 first = replaced.first;
    INT64 last = replaced.last;

    v_set_life_[set_index].replace_count++;
    v_set_life_[set_index].live_time += last - first;
    v_set_life_[set_index].dead_time += ref_id_ - last;
  }

  IncInst(inst_id, kLoad, is_hit, is_mru);
  IncTime(epoch,   kLoad, is_hit, is_mru);
  Inc(is_hit);

  return is_hit;
}
// Realistic replacement cache class (including infinite size; no replacement) }}}

// Optimal replacement cache class based on [Sugumar et al. 1993] {{{
// Needs to be initiated with Optimal replacement cache set
template <class CacheSet, const UINT32 kMaxSets, const UINT32 Allocation>
class OptimalCache : public BaseCache
{
 public:
  OptimalCache(std::string name, UINT32 size, UINT32 line_size, UINT32 num_ways,
      UINT32 lookahead_distance, UINT32 process_window)
      : BaseCache(name, size, line_size, num_ways),
        lookahead_distance_(lookahead_distance), process_window_(process_window)
  {
    // TODO(sasaki): how to assert whether the cache set is Optimal or not?
    ASSERTX(num_sets() <= kMaxSets);
    for (UINT32 i = 0; i < num_sets(); i++) {
      sets_[i].SetNumWays(num_ways);
    }
    ref_id_ = 0; // TODO(sasaki): fix
  }

  void AccessMultipleLines(ADDRINT inst_addr, ADDRINT addr,
      UINT32 size, AccessType access_type);
  void AccessSingleLine(ADDRINT inst_addr, ADDRINT addr,
      AccessType access_type);
  void Access(ADDRINT inst_addr, ADDRINT addr,
      AccessType access_type, CacheTag tag, UINT32 set_index);
  void FetchMultipleLines(ADDRINT inst_addr, UINT32 size);
  void FetchSingleLine(ADDRINT inst_addr);
  void Fetch(ADDRINT inst_addr, AccessType access_type, CacheTag tag, UINT32 set_index);
  void Process(UINT32 set_index, UINT32 iterations);
  void ProcessAllSets();

 private:
  CacheSet sets_[kMaxSets];
  const UINT32 lookahead_distance_;
  const UINT32 process_window_;
  INT64 ref_id_;
};

template <class CacheSet, const UINT32 kMaxSets, const UINT32 Allocation>
void OptimalCache<CacheSet, kMaxSets, Allocation>
    ::AccessMultipleLines(ADDRINT inst_addr, ADDRINT addr,
        UINT32 size, AccessType access_type)
{
  CacheTag tag;
  UINT32 set_index;

  SplitAddr(addr, tag, set_index);

  // Here I am making AccessMultipleLines() and AccessSingleLine() identical
  // because I believe it will be difficult to manage optimal replacement when
  // there's an access going to multiple sets and (because of the "lookahead -
  // stack process" nature of the algorithm)
  // So the access to the second line is getting ignored; it is introducing an
  // error (a miss might be counted as a hit; status update of the second line
  // is not being performed)
  //
  // const ADDRINT not_line_mask = ~(static_cast<ADDRINT>(line_size()) - 1);
  // const ADDRINT high_addr = addr + size;
  // ASSERTX(((addr & not_line_mask) + line_size()) >= high_addr);

  Access(inst_addr, addr, access_type, tag, set_index);
}

template <class CacheSet, const UINT32 kMaxSets, const UINT32 Allocation>
void OptimalCache<CacheSet, kMaxSets, Allocation>
    ::AccessSingleLine(ADDRINT inst_addr, ADDRINT addr,
        AccessType access_type)
{
  CacheTag tag;
  UINT32 set_index;

  SplitAddr(addr, tag, set_index);
  Access(inst_addr, addr, access_type, tag, set_index);
}

template <class CacheSet, const UINT32 kMaxSets, const UINT32 Allocation>
void OptimalCache<CacheSet, kMaxSets, Allocation>
    ::Access(ADDRINT inst_addr, ADDRINT addr,
        AccessType access_type, CacheTag tag, UINT32 set_index)
{
  CacheSet& set = sets_[set_index];
  const UINT32 inst_id = of_inst_.Map(inst_addr);
  const UINT32 addr_id = of_addr_.Map(addr);
  const UINT32 epoch   = of_time_.Map(ref_id_ / KILO);
  ref_id_++;

  set.ref(); // Increment reference id (sort of timestamp)

  set.Lookahead(tag, access_type, inst_id, addr_id, epoch);
  if (set.lookahead_ref_count() >= lookahead_distance_ + process_window_) {
    Process(set_index, process_window_);
  }
}

template <class CacheSet, const UINT32 kMaxSets, const UINT32 Allocation>
void OptimalCache<CacheSet, kMaxSets, Allocation>
    ::FetchMultipleLines(ADDRINT inst_addr, UINT32 size)
{
  CacheTag tag;
  UINT32 set_index;

  SplitAddr(inst_addr, tag, set_index);

  Fetch(inst_addr, kLoad, tag, set_index);
}

template <class CacheSet, const UINT32 kMaxSets, const UINT32 Allocation>
void OptimalCache<CacheSet, kMaxSets, Allocation>
    ::FetchSingleLine(ADDRINT inst_addr)
{
  CacheTag tag;
  UINT32 set_index;

  SplitAddr(inst_addr, tag, set_index);
  Fetch(inst_addr, kLoad, tag, set_index);
}

template <class CacheSet, const UINT32 kMaxSets, const UINT32 Allocation>
void OptimalCache<CacheSet, kMaxSets, Allocation>
    ::Fetch(ADDRINT inst_addr, AccessType access_type, CacheTag tag, UINT32 set_index)
{
  CacheSet& set = sets_[set_index];
  const UINT32 inst_id = of_inst_.Map(inst_addr);
  const UINT32 epoch = of_time_.Map(ref_id_ / KILO);
  ref_id_++;

  set.ref(); // Increment reference id (sort of timestamp)

  set.Lookahead(tag, kLoad, inst_id, 0, epoch);
  if (set.lookahead_ref_count() >= lookahead_distance_ + process_window_) {
    Process(set_index, process_window_);
  }
}

template <class CacheSet, const UINT32 kMaxSets, const UINT32 Allocation>
void OptimalCache<CacheSet, kMaxSets, Allocation>
    ::ProcessAllSets()
{
  for (UINT32 set_index = 0; set_index < num_sets(); set_index++) {
    Process(set_index, lookahead_distance_ + process_window_);
    // CacheSet& set = sets_[set_index];
    // cerr << set_index << " " << set.num_repairs() << endl;
  }
}

// Called "stack process" in the original paper
template <class CacheSet, const UINT32 kMaxSets, const UINT32 Allocation>
void OptimalCache<CacheSet, kMaxSets, Allocation>
    ::Process(UINT32 set_index, UINT32 iterations)
{
  CacheSet& set = sets_[set_index];
  VTrace lookahead_trace = set.lookahead_trace();
  MStack known_tags = set.known_tags();
  MStack unknown_tags = set.unknown_tags();
  MStack unknown_tags_pool = set.unknown_tags_pool();
  // Iterate the lookahead_trace to decide hit/miss
  VTrace::iterator it = lookahead_trace.begin();
  for (UINT32 i = 0; i < iterations; i++) {
    // Check first because lookahead_trace might be empty from the beginning
    if (lookahead_trace.empty()) {
      break;
    }
    ASSERTX(known_tags.size() + unknown_tags.size() <= set.GetNumWays());
    bool is_hit;
    if (known_tags.find(it->tag) != known_tags.end()) { // Hit
      is_hit = true;
      // Update the next reference
      if (set.is_known(it)) {
        known_tags[it->tag] = it->priority;
      }
      else {
        known_tags.erase(known_tags.find(it->tag));
        unknown_tags[it->tag] = it->priority;
      }
    }
    else { // Miss
      is_hit = false;
      // Set has room so just insert
      if (known_tags.size() + unknown_tags.size() < set.GetNumWays()) {
        if (set.is_known(it)) {
          known_tags[it->tag] = it->priority;
        }
        else {
          unknown_tags[it->tag] = it->priority;
        }
      }
      // Set has no room (full) so replace/bypass required
      else {
        // We need to select the tag with the lowest prioirty
        // Vector is used for fast sort (although not sure whether it's
        // faster than iterating the map or not)
        if (set.is_known(it)) {
          if (unknown_tags.size() == 0) {
            // This routine does not contain any errors (known-known)
            VStack v_tags(known_tags.begin(), known_tags.end());
            std::sort(v_tags.begin(), v_tags.end(),
                misc_functions::sort_second_ascending<CacheTag, Priority>());
            VStack::iterator v = v_tags.begin();
            // Compare with the current reference and replace or bypass
            if (v->second < it->priority) {
              known_tags.erase(v->first);
              known_tags[it->tag] = it->priority;
            }
            // Bypass: no need to change state
            else {
            }
          }
          else {
            // This routine does not include any errors (known-unknown)
            VStack v_tags(unknown_tags.begin(), unknown_tags.end());
            std::sort(v_tags.begin(), v_tags.end(),
                misc_functions::sort_second_ascending<CacheTag, Priority>());
            VStack::iterator v = v_tags.begin();
            // Replace
            unknown_tags.erase(v->first);
            unknown_tags_pool[v->first] = v->second;
            known_tags[it->tag] = it->priority;
          }
        }
        else {
          // Bypass: no need to change state
          // This routine potentially contain errors (unknown-unknown)
          unknown_tags_pool[it->tag] = it->priority;
        }
      }
    }
    // When next_reference of all the lines in a set are known all the lines
    // in unknown_tags can be clared since none of them won't reside in the
    // set (should be evicted) -- TODO(sasaki): needs to make sure
    if (unknown_tags.size() == 0) {
      unknown_tags_pool.clear();
    }

    IncInst(it->inst_id, it->access_type, is_hit);
    IncAddr(it->addr_id, it->access_type, is_hit);
    IncTime(it->epoch,   it->access_type, is_hit);
    Inc(is_hit);

    it = lookahead_trace.erase(it); // or lookahead_trace.erase(it++);
    if (lookahead_trace.empty()) {
      break;
    }
  }
  set.update_known_tags(known_tags);
  set.update_unknown_tags(unknown_tags);
  set.update_unknown_tags_pool(unknown_tags_pool);
  set.update_lookahead_trace(lookahead_trace);
  set.update_lookahead_ref_count(lookahead_distance_);
}
// Optimal replacement cache class based on [Sugumar et al. 1993] }}}
// Cache implementation }}}

#endif

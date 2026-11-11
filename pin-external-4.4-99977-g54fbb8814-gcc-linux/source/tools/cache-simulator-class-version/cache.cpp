#include <fstream>
#include <iomanip>  // std::setw, std::setprecesion
#include <iostream> // std::fixed
#include <vector>

#include <string.h>

#include "pin.H"

#include "cache.h"

#if defined(TARGET_MAC)
#define MALLOC "_malloc"
#define FREE "_free"
#else
#define MALLOC "malloc"
#define FREE "free"
#endif

// Use the same cache line size for all caches
KNOB<UINT32> KnobLineSize(KNOB_MODE_WRITEONCE, "pintool",
    "b",  "64", "Cache line size in bytes");
// L1 data cache {{{
KNOB<bool>   KnobDL1(KNOB_MODE_WRITEONCE, "pintool",
    "dl1",    "true", "Simulate DL1?");
KNOB<UINT32> KnobDL1NumWays(KNOB_MODE_WRITEONCE, "pintool",
    "dl1_a",  "8", "Cache associativity (1 for direct map) of DL1");
KNOB<UINT32> KnobDL1CacheSize(KNOB_MODE_WRITEONCE, "pintool",
    "dl1_c",  "32", "Cache size of DL1 in kilobytes");
KNOB<string> KnobDL1OutputInst(KNOB_MODE_WRITEONCE, "pintool",
    "dl1_oi", "cache.dl1.inst.out", "Output file name for DL1 (instruction)");
KNOB<string> KnobDL1OutputAddr(KNOB_MODE_WRITEONCE, "pintool",
    "dl1_oa", "cache.dl1.addr.out", "Output file name for DL1 (address)");
KNOB<string> KnobDL1OutputTime(KNOB_MODE_WRITEONCE, "pintool",
    "dl1_ot", "cache.dl1.time.out", "Output file name for DL1 (time)");
KNOB<string> KnobDL1OutputLife(KNOB_MODE_WRITEONCE, "pintool",
    "dl1_ol", "cache.dl1.life.out", "Output file name for DL1 (lifetime)");
// }}}
// L1 instruction cache {{{
KNOB<bool>   KnobIL1(KNOB_MODE_WRITEONCE, "pintool",
    "il1",    "false", "Simulate IL1?");
KNOB<UINT32> KnobIL1NumWays(KNOB_MODE_WRITEONCE, "pintool",
    "il1_a",  "8", "Cache associativity (1 for direct map) of IL1");
KNOB<UINT32> KnobIL1CacheSize(KNOB_MODE_WRITEONCE, "pintool",
    "il1_c",  "32", "Cache size of IL1 in kilobytes");
KNOB<string> KnobIL1OutputInst(KNOB_MODE_WRITEONCE, "pintool",
    "il1_oi", "cache.il1.inst.out", "Output file name for IL1 (instruction)");
KNOB<string> KnobIL1OutputTime(KNOB_MODE_WRITEONCE, "pintool",
    "il1_ot", "cache.il1.time.out", "Output file name for IL1 (time)");
KNOB<string> KnobIL1OutputLife(KNOB_MODE_WRITEONCE, "pintool",
    "il1_ol", "cache.il1.life.out", "Output file name for IL1 (lifetime)");
// }}}
// L2 cache {{{
KNOB<bool>   KnobUL2(KNOB_MODE_WRITEONCE, "pintool",
    "ul2",    "false", "Simulate L2?");
KNOB<UINT32> KnobUL2NumWays(KNOB_MODE_WRITEONCE, "pintool",
    "ul2_a",  "8", "Cache associativity (1 for direct map) of L2");
KNOB<UINT32> KnobUL2CacheSize(KNOB_MODE_WRITEONCE, "pintool",
    "ul2_c",  "256", "Cache size of UL2 in kilobytes");
KNOB<string> KnobUL2OutputInst(KNOB_MODE_WRITEONCE, "pintool",
    "ul2_oi", "cache.ul2.inst.out", "Output file name for L2 (instruction)");
KNOB<string> KnobUL2OutputAddr(KNOB_MODE_WRITEONCE, "pintool",
    "ul2_oa", "cache.ul2.addr.out", "Output file name for L2 (address)");
KNOB<string> KnobUL2OutputTime(KNOB_MODE_WRITEONCE, "pintool",
    "ul2_ot", "cache.ul2.time.out", "Output file name for L2 (time)");
KNOB<string> KnobUL2OutputLife(KNOB_MODE_WRITEONCE, "pintool",
    "ul2_ol", "cache.ul2.life.out", "Output file name for L2 (lifetime)");
// }}}

// Parameters for OPT replacement (just affect the execution speed)
KNOB<UINT32> KnobLookaheadDistance(KNOB_MODE_WRITEONCE, "pintool",
    "ld", "100", "Lookahead distance for the optimal cache");
KNOB<UINT32> KnobProcessWindow(KNOB_MODE_WRITEONCE, "pintool",
    "pw", "400", "Process window for the optimal cache");

// At the moment thresholds are not used (we might want to introduce some kind
// of thresholds to restrict the output in the future)
KNOB<UINT32> KnobThresholdHit(KNOB_MODE_WRITEONCE, "pintool",
    "rh", "100", "Only report memops with hit count above threshold");
KNOB<UINT32> KnobThresholdMiss(KNOB_MODE_WRITEONCE, "pintool",
    "rm", "100", "Only report memops with miss count above threshold");

std::ofstream dl1_output_inst;
std::ofstream dl1_output_addr;
std::ofstream dl1_output_time;
std::ofstream dl1_output_life;

std::ofstream il1_output_inst;
std::ofstream il1_output_time;
std::ofstream il1_output_life;

std::ofstream ul2_output_inst;
std::ofstream ul2_output_addr;
std::ofstream ul2_output_time;
std::ofstream ul2_output_life;

typedef struct
{
  std::string name;
  std::string image;
  ADDRINT size;
  ADDRINT range;
} RoutineTag;

typedef struct
{
  UINT32 id;
  ADDRINT addr;
  ADDRINT size;
} ObjectTag;

// routine_tag is indexed by the instruction pointer
std::map<ADDRINT, RoutineTag> routine_tag;
std::vector<ObjectTag> object_tag;

typedef enum {
  kInstruction,
  kAddress,
  kTime,
  kNumAnalyses,
} Analyses;

typedef enum {
  kInf,
  kFaOpt,
  kSaOpt,
  kSaReal,
  kNumCacheInstances,
} CacheInstances;

namespace l1_cache // {{{
{
// const UINT32 kMaxSets = KILO;
// mierlak: change kMaxSets for CSEE 4824 assignment
const UINT32 kMaxSets = KILO * 4;
const UINT32 kMaxNumWays = 64;
const BaseCache::AllocationType  wa = BaseCache::kWriteAllocate;
const BaseCache::AllocationType nwa = BaseCache::kNoWriteAllocate;

typedef        Cache<cache_set::Infinite,                       kMaxSets, wa> InfCache;
typedef OptimalCache<cache_set::Optimal<kMaxNumWays>,           kMaxSets, wa> OptCache;
typedef        Cache<cache_set::LeastRecentlyUsed<kMaxNumWays>, kMaxSets, wa> LruCache;
typedef        Cache<cache_set::RoundRobin<kMaxNumWays>,        kMaxSets, wa> RrCache;
} // namespace l1_cache }}}

l1_cache::InfCache    *inf_dl1 = 0;
l1_cache::OptCache *fa_opt_dl1 = 0;
l1_cache::OptCache *sa_opt_dl1 = 0;
l1_cache::LruCache *sa_lru_dl1 = 0;

l1_cache::InfCache    *inf_il1 = 0;
l1_cache::OptCache *fa_opt_il1 = 0;
l1_cache::OptCache *sa_opt_il1 = 0;
l1_cache::LruCache *sa_lru_il1 = 0;

namespace l2_cache // {{{
{
const UINT32 kMaxSets = MEGA;
const UINT32 kMaxNumWays = KILO;
const BaseCache::AllocationType  wa = BaseCache::kWriteAllocate;
const BaseCache::AllocationType nwa = BaseCache::kNoWriteAllocate;

typedef        Cache<cache_set::Infinite,                       kMaxSets, wa> InfCache;
typedef OptimalCache<cache_set::Optimal<kMaxNumWays>,           kMaxSets, wa> OptCache;
typedef        Cache<cache_set::LeastRecentlyUsed<kMaxNumWays>, kMaxSets, wa> LruCache;
typedef        Cache<cache_set::RoundRobin<kMaxNumWays>,        kMaxSets, wa> RrCache;
} // namespace l2_cache }}}

l2_cache::InfCache    *inf_ul2 = 0;
l2_cache::OptCache *fa_opt_ul2 = 0;
l2_cache::OptCache *sa_opt_ul2 = 0;
l2_cache::LruCache *sa_lru_ul2 = 0;

// Data cache {{{
// Possibly span multiple cache lines
void LoadMultipleLines(ADDRINT inst_addr, ADDRINT addr, UINT32 size)
{
  if (KnobDL1) {
       inf_dl1->AccessMultipleLines(inst_addr, addr, size, BaseCache::kLoad);
    fa_opt_dl1->AccessMultipleLines(inst_addr, addr, size, BaseCache::kLoad);
    sa_opt_dl1->AccessMultipleLines(inst_addr, addr, size, BaseCache::kLoad);
    const bool is_dl1_hit =
      sa_lru_dl1->AccessMultipleLines(inst_addr, addr, size, BaseCache::kLoad);

    if (KnobUL2) {
      if (!is_dl1_hit) {
           inf_ul2->AccessMultipleLines(inst_addr, addr, size, BaseCache::kLoad);
        fa_opt_ul2->AccessMultipleLines(inst_addr, addr, size, BaseCache::kLoad);
        sa_opt_ul2->AccessMultipleLines(inst_addr, addr, size, BaseCache::kLoad);
        sa_lru_ul2->AccessMultipleLines(inst_addr, addr, size, BaseCache::kLoad);
      }
    }
  }
}

// Possibly span multiple cache lines
void StoreMultipleLines(ADDRINT inst_addr, ADDRINT addr, UINT32 size)
{
  if (KnobDL1) {
       inf_dl1->AccessMultipleLines(inst_addr, addr, size, BaseCache::kStore);
    fa_opt_dl1->AccessMultipleLines(inst_addr, addr, size, BaseCache::kStore);
    sa_opt_dl1->AccessMultipleLines(inst_addr, addr, size, BaseCache::kStore);
    const bool is_dl1_hit =
      sa_lru_dl1->AccessMultipleLines(inst_addr, addr, size, BaseCache::kStore);

    if (KnobUL2) {
      if (!is_dl1_hit) {
           inf_ul2->AccessMultipleLines(inst_addr, addr, size, BaseCache::kStore);
        fa_opt_ul2->AccessMultipleLines(inst_addr, addr, size, BaseCache::kStore);
        sa_opt_ul2->AccessMultipleLines(inst_addr, addr, size, BaseCache::kStore);
        sa_lru_ul2->AccessMultipleLines(inst_addr, addr, size, BaseCache::kStore);
      }
    }
  }
}

void LoadSingleLine(ADDRINT inst_addr, ADDRINT addr)
{
  if (KnobDL1) {
       inf_dl1->AccessSingleLine(inst_addr, addr, BaseCache::kLoad);
    fa_opt_dl1->AccessSingleLine(inst_addr, addr, BaseCache::kLoad);
    sa_opt_dl1->AccessSingleLine(inst_addr, addr, BaseCache::kLoad);
    const bool is_dl1_hit =
      sa_lru_dl1->AccessSingleLine(inst_addr, addr, BaseCache::kLoad);

    if (KnobUL2) {
      if (!is_dl1_hit) {
           inf_ul2->AccessSingleLine(inst_addr, addr, BaseCache::kLoad);
        fa_opt_ul2->AccessSingleLine(inst_addr, addr, BaseCache::kLoad);
        sa_opt_ul2->AccessSingleLine(inst_addr, addr, BaseCache::kLoad);
        sa_lru_ul2->AccessSingleLine(inst_addr, addr, BaseCache::kLoad);
      }
    }
  }
}

void StoreSingleLine(ADDRINT inst_addr, ADDRINT addr)
{
  if (KnobDL1) {
       inf_dl1->AccessSingleLine(inst_addr, addr, BaseCache::kStore);
    fa_opt_dl1->AccessSingleLine(inst_addr, addr, BaseCache::kStore);
    sa_opt_dl1->AccessSingleLine(inst_addr, addr, BaseCache::kStore);
    const bool is_dl1_hit =
      sa_lru_dl1->AccessSingleLine(inst_addr, addr, BaseCache::kStore);

    if (KnobUL2) {
      if (!is_dl1_hit) {
           inf_ul2->AccessSingleLine(inst_addr, addr, BaseCache::kStore);
        fa_opt_ul2->AccessSingleLine(inst_addr, addr, BaseCache::kStore);
        sa_opt_ul2->AccessSingleLine(inst_addr, addr, BaseCache::kStore);
        sa_lru_ul2->AccessSingleLine(inst_addr, addr, BaseCache::kStore);
      }
    }
  }
}
// }}}

// Instruction cache {{{
// Possibly span multiple cache lines
void FetchMultipleLines(ADDRINT inst_addr, UINT32 size)
{
  if (KnobIL1) {
       inf_il1->FetchMultipleLines(inst_addr, size);
    fa_opt_il1->FetchMultipleLines(inst_addr, size);
    sa_opt_il1->FetchMultipleLines(inst_addr, size);
    const bool is_il1_hit = sa_lru_il1->FetchMultipleLines(inst_addr, size);

    if (KnobUL2) {
      if (!is_il1_hit) {
           inf_ul2->FetchMultipleLines(inst_addr, size);
        fa_opt_ul2->FetchMultipleLines(inst_addr, size);
        sa_opt_ul2->FetchMultipleLines(inst_addr, size);
        sa_lru_ul2->FetchMultipleLines(inst_addr, size);
      }
    }
  }
}

void FetchSingleLine(ADDRINT inst_addr)
{
  if (KnobIL1) {
       inf_il1->FetchSingleLine(inst_addr);
    fa_opt_il1->FetchSingleLine(inst_addr);
    sa_opt_il1->FetchSingleLine(inst_addr);
    const bool is_il1_hit = sa_lru_il1->FetchSingleLine(inst_addr);

    if (KnobUL2) {
      if (!is_il1_hit) {
           inf_ul2->FetchSingleLine(inst_addr);
        fa_opt_ul2->FetchSingleLine(inst_addr);
        sa_opt_ul2->FetchSingleLine(inst_addr);
        sa_lru_ul2->FetchSingleLine(inst_addr);
      }
    }
  }
}
// }}}

const UINT32 S = 11;
const UINT32 L = 20; // PC

void PrintLifetime(BaseCache::VSetLife *lifetime,
    std::ofstream &output)
{
  // Header
  output
    << std::setw(L) << "Set"           << " "
    << std::setw(L) << "Replace"       << " "
    << std::setw(L) << "Avg-Live-time" << " "
    << std::setw(L) << "Avg-Dead-time" << " "
    << std::setw(L) << "Access"        << " "
    << std::setw(L) << "Avg-access-interval"
    << std::endl;

  BaseCache::VSetLife::iterator vit;
  UINT32 i;
  for (vit = lifetime->begin(), i = 0; vit != lifetime->end(); vit++, i++) {
    output
      << std::fixed   << std::setprecision(1)
      << std::setw(L) << i                                           << " "
      << std::setw(L) <<         vit->replace_count                  << " "
      << std::setw(L) << (double)vit->live_time / vit->replace_count << " "
      << std::setw(L) << (double)vit->dead_time / vit->replace_count << " "
      << std::setw(L) <<         vit->access_count                   << " "
      << std::setw(L) << (double)vit->access_interval / vit->access_count
      << std::endl;
  }
}

void PrintOutput(BaseCache::Profile *profile[kNumCacheInstances],
    std::ofstream &output, UINT32 analysis)
{
  typedef enum {
    kLoadMiss,
    kLoadHit,
    kStoreMiss,
    kStoreHit,
    kNumRefTypes,
  } RefTypes;

  typedef enum {
    kLoadHitIsNotMru,
    kLoadHitIsMru,
    kStoreHitIsNotMru,
    kStoreHitIsMru,
    kNumMruTypes,
  } MruTypes;

  typedef std::vector<BaseCache::CacheStats> VCacheStats;
  typedef std::vector<std::pair<ADDRINT, VCacheStats > > GlobalProfile;
  GlobalProfile global_profile;
  VCacheStats vcounters;
  std::pair<ADDRINT, VCacheStats > p;

  std::map<ADDRINT, UINT32> *addr_map[kNumCacheInstances];
  std::vector<BaseCache::HitMissByAccessType> *counters[kNumCacheInstances];
  BaseCache::HitMissByAccessType *counter;

  for (UINT32 i = 0; i < kNumCacheInstances; i++) {
    addr_map[i] = profile[i]->map();
    counters[i] = profile[i]->counters();
  }

  const UINT64 num_identical_instructions = addr_map[kInf]->size();
  ASSERTX(num_identical_instructions == addr_map[kFaOpt]->size());
  ASSERTX(num_identical_instructions == addr_map[kSaOpt]->size());
  ASSERTX(num_identical_instructions == addr_map[kSaReal]->size());

  std::map<ADDRINT, UINT32>::iterator mit;
  for (mit = addr_map[kInf]->begin(); mit != addr_map[kInf]->end(); mit++) {
    for (UINT32 i = 0; i < kNumCacheInstances; i++) {
      counter = &((*counters[i])[mit->second]);
      for (UINT32 j = 0; j < kNumRefTypes; j++) {
        vcounters.push_back((*counter)[j]);
      }
    }

    // For is_mru
    counter = &((*counters[kSaReal])[mit->second]);
    for (UINT32 k = 0; k < kNumMruTypes; k++) {
      vcounters.push_back((*counter)[kNumRefTypes + k]);
    }
    // vcounters.push_back((*counter)[kNumRefTypes+kLoadHitIsNotMru]);
    // vcounters.push_back((*counter)[kNumRefTypes+kLoadHitIsMru]);
    // vcounters.push_back((*counter)[kNumRefTypes+kStoreHitIsNotMru]);
    // vcounters.push_back((*counter)[kNumRefTypes+kStoreHitIsMru]);

    p = std::make_pair(mit->first, vcounters);
    vcounters.clear();
    global_profile.push_back(p);
  }

  // Header
  output << std::setw(S) << "Ld(0)/St(1)" << " ";
  switch(analysis) {
    case kInstruction:
    case kAddress:
      output << std::setw(L) << "PC" << " ";
      break;
    case kTime:
      output << std::setw(L) << "Epoch" << " ";
      break;
  }
  output
    << std::setw(S) << "Reference"   << " "
    << std::setw(S) << "Inf-miss"    << " "
    << std::setw(S) << "FA-OPT-miss" << " "
    << std::setw(S) << "SA-OPT-miss" << " "
    << std::setw(S) << "SA-LRU-miss" << " "
    << std::setw(S) << "Hit"         << " "
    << std::setw(S) << "Cap-miss"    << " "
    << std::setw(S) << "Map-miss"    << " "
    << std::setw(S) << "Repl-miss"   << " "
    << std::setw(S) << "Cold-miss"   << " ";
  switch(analysis) {
    case kAddress:
      output << std::setw(S) << "Nth-malloc" << " ";
      break;
    case kInstruction:
      output
        << std::setw(S) << "Function" << " "
        << std::setw(S) << "Image"    << " ";
      break;
  }
  output
    << std::setw(S) << "Hit-not-MRU" << " "
    << std::setw(S) << "Hit-MRU";
  output << std::endl;

  GlobalProfile::iterator git;
  for (git = global_profile.begin(); git != global_profile.end(); git++) {
    INT64     ld_references = 0;
    INT64     ld_inf_misses = 0;
    INT64  ld_fa_opt_misses = 0;
    INT64  ld_sa_opt_misses = 0;
    INT64  ld_sa_lru_misses = 0;
    INT64     st_references = 0;
    INT64     st_inf_misses = 0;
    INT64  st_fa_opt_misses = 0;
    INT64  st_sa_opt_misses = 0;
    INT64  st_sa_lru_misses = 0;
    INT64 ld_hit_is_not_mru = 0;
    INT64     ld_hit_is_mru = 0;
    INT64 st_hit_is_not_mru = 0;
    INT64     st_hit_is_mru = 0;
    UINT32 i = 0;
    VCacheStats::iterator vit;
    for (vit = git->second.begin(); vit != git->second.end(); vit++) {
      switch(i) {
        case    (kInf * kNumRefTypes) + kLoadMiss:
          ld_references += *vit;
          ld_inf_misses  = *vit;
          break;
        case    (kInf * kNumRefTypes) + kLoadHit:
          ld_references += *vit;
          break;
        case    (kInf * kNumRefTypes) + kStoreMiss:
          st_references += *vit;
          st_inf_misses  = *vit;
          break;
        case    (kInf * kNumRefTypes) + kStoreHit:
          st_references += *vit;
          break;
        case  (kFaOpt * kNumRefTypes) + kLoadMiss:
          ld_fa_opt_misses = *vit;
          break;
        case  (kFaOpt * kNumRefTypes) + kStoreMiss:
          st_fa_opt_misses = *vit;
          break;
        case  (kSaOpt * kNumRefTypes) + kLoadMiss:
          ld_sa_opt_misses = *vit;
          break;
        case  (kSaOpt * kNumRefTypes) + kStoreMiss:
          st_sa_opt_misses = *vit;
          break;
        case (kSaReal * kNumRefTypes) + kLoadMiss:
          ld_sa_lru_misses = *vit;
          break;
        case (kSaReal * kNumRefTypes) + kStoreMiss:
          st_sa_lru_misses = *vit;
          break;
        case (kNumCacheInstances * kNumRefTypes) + kLoadHitIsNotMru:
          ld_hit_is_not_mru = *vit;
          break;
        case (kNumCacheInstances * kNumRefTypes) + kLoadHitIsMru:
          ld_hit_is_mru = *vit;
          break;
        case (kNumCacheInstances * kNumRefTypes) + kStoreHitIsNotMru:
          st_hit_is_not_mru = *vit;
          break;
        case (kNumCacheInstances * kNumRefTypes) + kStoreHitIsMru:
          st_hit_is_mru = *vit;
          break;
        default:
          break;
      }
      i++;
    }

    // Load/store stats are separated (written in different rows)
    for (UINT32 j = 0; j < BaseCache::kNumAccessTypes; j++) {
      // Hit/miss stats {{{
      output
        << std::setw(S)  << j          << " "
        << std::setw(L) << git->first << " "
        << std::setw(S);
      switch(j) {
        case BaseCache::kLoad:
          output << ld_references                       << " " << std::setw(S);
          output << ld_inf_misses                       << " " << std::setw(S);
          output << ld_fa_opt_misses                    << " " << std::setw(S);
          output << ld_sa_opt_misses                    << " " << std::setw(S);
          output << ld_sa_lru_misses                    << " " << std::setw(S);
          output << ld_references    - ld_sa_lru_misses << " " << std::setw(S); // Hits
          output << ld_inf_misses                       << " " << std::setw(S); // Cold misses
          output << ld_fa_opt_misses - ld_inf_misses    << " " << std::setw(S); // Capacity misses
          output << ld_sa_opt_misses - ld_fa_opt_misses << " " << std::setw(S); // Mapping misses
          output << ld_sa_lru_misses - ld_sa_opt_misses << " " << std::setw(S); // Replacement misses
          break;
        case BaseCache::kStore:
          output << st_references                       << " " << std::setw(S);
          output << st_inf_misses                       << " " << std::setw(S);
          output << st_fa_opt_misses                    << " " << std::setw(S);
          output << st_sa_opt_misses                    << " " << std::setw(S);
          output << st_sa_lru_misses                    << " " << std::setw(S);
          output << st_references    - st_sa_lru_misses << " " << std::setw(S); // Hits
          output << st_inf_misses                       << " " << std::setw(S); // Cold misses
          output << st_fa_opt_misses - st_inf_misses    << " " << std::setw(S); // Capacity misses
          output << st_sa_opt_misses - st_fa_opt_misses << " " << std::setw(S); // Mapping misses
          output << st_sa_lru_misses - st_sa_opt_misses << " " << std::setw(S); // Replacement misses
          break;
        default:
          break;
      }
      // }}}

      // Additional information {{{
      bool no_information = true;
      switch(analysis) {
        case kInstruction: {
          std::map<ADDRINT, RoutineTag>::iterator p;
          for (p = routine_tag.begin(); p != routine_tag.end(); p++) {
            if ((p->first <= git->first) && (git->first <= p->first + p->second.size)) {
              output
                << p->second.name  << " " << std::setw(S)
                << p->second.image << " " << std::setw(S);
              no_information = false;
              break;
            }
          }
          if (no_information == true) {
            output
              << "Unknown" << " " << std::setw(S)
              << "Unknown" << " " << std::setw(S);
          }
          break;
        }
        case kAddress: {
          std::vector<ObjectTag>::iterator v;
          for (v = object_tag.begin(); v != object_tag.end(); v++) {
            if ((v->addr <= git->first) && (git->first <= v->addr + v->size)) {
              output << v->id << " " << std::setw(S);
              no_information = false;
              break;
            }
          }
          if (no_information == true) {
            output << -1 << " " << std::setw(S);
          }
          break;
        }
        default:
          break;
      }
      // }}}

      // Further additional information... {{{
      switch(j) {
        case BaseCache::kLoad:
          output << ld_hit_is_not_mru << " " << std::setw(S) << ld_hit_is_mru;
          break;
        case BaseCache::kStore:
          output << st_hit_is_not_mru << " " << std::setw(S) << st_hit_is_mru;
          break;
        default:
          break;
      }
      // }}}
      output << std::endl;
    }
  }
}

static UINT64 inst_count = 0; // Total number of instructions
const UINT32 W = 12;
#define PrintMissStats(cache)\
  std::cerr\
    << std::fixed        << std::setprecision(4)\
    << " Instructions: " << inst_count << "\t"\
    << " References: "   << sa_lru_##cache->access() << "\t"\
    << " Miss rate: "    << (double)sa_lru_##cache->miss() / sa_lru_##cache->access() << "\t"\
    << " MPKI: "         << (double)sa_lru_##cache->miss() / inst_count * 1000\
    << std::endl;\
  std::cerr\
    << std::setw(W)   << "Total"       << " "\
    << std::setw(W)   << "Cold"        << " "\
    << std::setw(W)   << "Capacity"    << " "\
    << std::setw(W)   << "Mapping"     << " "\
    << std::setw(W)   << "Replacement" << " "\
    << std::endl;\
  std::cerr\
    << std::setw(W) << sa_lru_##cache->miss()                      << " "\
    << std::setw(W) <<    inf_##cache->miss()                      << " "\
    << std::setw(W) << fa_opt_##cache->miss() -    inf_##cache->miss() << " "\
    << std::setw(W) << sa_opt_##cache->miss() - fa_opt_##cache->miss() << " "\
    << std::setw(W) << sa_lru_##cache->miss() - sa_opt_##cache->miss() << " "\
    << std::endl;

// http://www.parashift.com/c++-faq-lite/macros-with-token-pasting.html
#define NAME2(a,b)          NAME2_HIDDEN(a,b)
#define NAME2_HIDDEN(a,b)   a ## b
#define NAME3(a,b,c)        NAME3_HIDDEN(a,b,c)
#define NAME3_HIDDEN(a,b,c) a ## b ## c

#define FiniAddr(cache)\
  profile[kInf]    =    inf_##cache->of_addr();\
  profile[kFaOpt]  = fa_opt_##cache->of_addr();\
  profile[kSaOpt]  = sa_opt_##cache->of_addr();\
  profile[kSaReal] = sa_lru_##cache->of_addr();\
  PrintOutput(profile, NAME2(cache,_output_addr), kAddress);\
\
  NAME2(cache,_output_addr).close();

// Optimal replacement needs to finish the stack process by ProcessAllSets()
#define FiniElse(cache)\
  fa_opt_##cache->ProcessAllSets();\
  sa_opt_##cache->ProcessAllSets();\
\
  profile[kInf]    =    inf_##cache->of_inst();\
  profile[kFaOpt]  = fa_opt_##cache->of_inst();\
  profile[kSaOpt]  = sa_opt_##cache->of_inst();\
  profile[kSaReal] = sa_lru_##cache->of_inst();\
  PrintOutput(profile, NAME2(cache,_output_inst), kInstruction);\
\
  profile[kInf]    =    inf_##cache->of_time();\
  profile[kFaOpt]  = fa_opt_##cache->of_time();\
  profile[kSaOpt]  = sa_opt_##cache->of_time();\
  profile[kSaReal] = sa_lru_##cache->of_time();\
  PrintOutput(profile, NAME2(cache,_output_time), kTime);\
\
  lifetime         = sa_lru_##cache->of_life();\
  PrintLifetime(lifetime, NAME2(cache,_output_life));\
\
  NAME2(cache,_output_inst).close();\
  NAME2(cache,_output_time).close();\
  NAME2(cache,_output_life).close();

void PrintStats()
{
  std::cerr << std::endl;
  if (KnobDL1) {
    std::cerr << "L1 Data (hit/miss)\t";
    PrintMissStats(dl1);
  }
  if (KnobIL1) {
    std::cerr << "L1 Inst (hit/miss)\t";
    PrintMissStats(il1);
  }
  if (KnobUL2) {
    std::cerr << "L2 (hit/miss)\t\t";
    PrintMissStats(ul2);
  }
}

void Fini(int code, void *v)
{
  BaseCache::Profile  *profile[kNumCacheInstances];
  BaseCache::VSetLife *lifetime;

  if (KnobDL1) {
    FiniAddr(dl1);
    FiniElse(dl1);
  }
  if (KnobIL1) {
    FiniElse(il1);
  }
  if (KnobUL2) {
    FiniAddr(ul2);
    FiniElse(ul2);
  }

  PrintStats();
}

void Instruction(INS ins, void *v)
{
  UINT32 size = INS_Size(ins);
  bool is_single = (size <= 4);

  if (is_single) {
    INS_InsertPredicatedCall(ins, IPOINT_BEFORE,
        (AFUNPTR)FetchSingleLine,
        IARG_INST_PTR,
        IARG_END);
  }
  else {
    INS_InsertPredicatedCall(ins, IPOINT_BEFORE,
        (AFUNPTR)FetchMultipleLines,
        IARG_INST_PTR,
        IARG_UINT32, size,
        IARG_END);
  }

  if (INS_IsMemoryRead(ins) && INS_IsStandardMemop(ins)) {
    size = FirstReadSize(ins); 
    is_single = (size <= 4);

    if (is_single) {
      INS_InsertPredicatedCall(ins, IPOINT_BEFORE,
          (AFUNPTR)LoadSingleLine,
          IARG_INST_PTR,
          IARG_MEMORYREAD_EA,
          IARG_END);
    }
    else {
      INS_InsertPredicatedCall(ins, IPOINT_BEFORE,
          (AFUNPTR)LoadMultipleLines,
          IARG_INST_PTR,
          IARG_MEMORYREAD_EA,
          IARG_MEMORYREAD_SIZE,
          IARG_END);
    }
  }
  if (INS_IsMemoryWrite(ins) && INS_IsStandardMemop(ins)) {
    size = FirstWriteSize(ins);
    is_single = (size <= 4);

    if (is_single) {
      INS_InsertPredicatedCall(ins, IPOINT_BEFORE,
          (AFUNPTR)StoreSingleLine,
          IARG_INST_PTR,
          IARG_MEMORYWRITE_EA,
          IARG_END);
    }
    else {
      INS_InsertPredicatedCall(ins, IPOINT_BEFORE,
          (AFUNPTR)StoreMultipleLines,
          IARG_INST_PTR,
          IARG_MEMORYWRITE_EA,
          IARG_MEMORYWRITE_SIZE,
          IARG_END);
    }
  }
}

// For reporting MPKI
void PIN_FAST_ANALYSIS_CALL IncInst(UINT32 c) { inst_count += c; }

// Pin calls this function every time a new basic block is encountered
// It inserts a call to docount
void Trace(TRACE trace, void *v)
{
  // Visit every basic block in the trace
  for (BBL bbl = TRACE_BblHead(trace); BBL_Valid(bbl); bbl = BBL_Next(bbl)) {
    // Insert a call to IncInst() for every bbl, passing the number of instructions.
    // IPOINT_ANYWHERE allows Pin to schedule the call anywhere in the bbl to obtain best performance.
    // Use a fast linkage for the call.
    BBL_InsertCall(bbl, IPOINT_ANYWHERE, AFUNPTR(IncInst), IARG_FAST_ANALYSIS_CALL, IARG_UINT32, BBL_NumIns(bbl), IARG_END);
  }
}

void Routine(RTN routine, void *v)
{
  routine_tag[RTN_Address(routine)].name = RTN_Name(routine);
  routine_tag[RTN_Address(routine)].image = StripPath(IMG_Name(SEC_Img(RTN_Sec(routine))).c_str());
  routine_tag[RTN_Address(routine)].size = RTN_Size(routine);
  // At the moment range is not used (and perhaps we won't in the future)
  routine_tag[RTN_Address(routine)].range = RTN_Range(routine);
}

void MallocBefore(ADDRINT size)
{
  ObjectTag tag;
  // Caller specifies the size
  tag.size = size;
  object_tag.push_back(tag);
}

void MallocAfter(ADDRINT addr)
{
  static UINT32 id = 1;
  ObjectTag tag;

  // Preserve the size
  tag.size = object_tag.back().size;
  // Pop, fill the information, and push back
  object_tag.pop_back();
  // Address is returned
  tag.addr = addr;
  // Save and increment id
  tag.id = (id++);
  // Debug
  // std::cerr << tag.addr << " " << tag.size << " " << tag.id << endl;
  object_tag.push_back(tag);
}

void Image(IMG img, void *v)
{
  //  Find malloc()
  RTN malloc_routine = RTN_FindByName(img, MALLOC);
  if (RTN_Valid(malloc_routine)) {
    RTN_Open(malloc_routine);

    RTN_InsertCall(malloc_routine, IPOINT_BEFORE,
        (AFUNPTR)MallocBefore,
        IARG_FUNCARG_ENTRYPOINT_VALUE, 0,
        IARG_END);
    RTN_InsertCall(malloc_routine, IPOINT_AFTER,
        (AFUNPTR)MallocAfter,
        IARG_FUNCRET_EXITPOINT_VALUE,
        IARG_END);

    RTN_Close(malloc_routine);
  }
}

INT32 PrintUsage()
{
  std::cerr <<
"This pintool is a cache simulator for our causality work.\n\n"
"02/13/2015 output for Simha's CSEE 4824 homework added.\n"
"09/24/2014 additional information is collected (number of MRU hits;\n"
"           average lifetime, both live- and dead-time).\n"
"08/07/2014 L2 cache is implemented.\n"
"08/05/2014 L1 instruction cache is implemented.\n"
"08/04/2014 additional information (function name/binary image for\n"
"           instruction; nth malloc() for address) along with the\n"
"           miss statistics are reported.\n"
"07/28/2014 misses are reported in different forms:\n"
"           {address/instruction/time}-based.\n"
"07/25/2014 the number of misses of each cache is reported.\n"
"07/23/2014 optimal replacement policy is implemented.\n"
"07/18/2014 L1 data cache is implemented.\n"
"\n";
  std::cerr << KNOB_BASE::StringKnobSummary() << std::endl;

  return -1;
}

#define PrintCacheConfigMain(cache)\
  std::cerr\
    << " Size: "          << NAME3(Knob,cache,CacheSize).Value() << "(KB)\t"\
    << " Line size: "     << KnobLineSize.Value()     << "(B)\t"\
    << " Associativity: " << NAME3(Knob,cache,NumWays).Value()   << "\t"\
    << " Num sets: "\
    << (NAME3(Knob,cache,CacheSize).Value() * KILO)\
        / (KnobLineSize.Value() * NAME3(Knob,cache,NumWays).Value())\
    << std::endl << std::endl;\


void PrintCacheConfig()
{
  if (KnobDL1) {
    std::cerr << "L1 Data (config)\t";
    PrintCacheConfigMain(DL1);
  }
  if (KnobIL1) {
    std::cerr << "L1 Inst (config)\t";
    PrintCacheConfigMain(IL1);
  }
  if (KnobUL2) {
    std::cerr << "L2 (config)\t\t";
    PrintCacheConfigMain(UL2);
  }
}

int main(int argc, char *argv[])
{
  // Initialize symbol table code, needed for RTN instrumentation
  PIN_InitSymbols();

  if (PIN_Init(argc, argv)) {
    return PrintUsage();
  }

  // L1 data cache {{{
  if (KnobDL1) {
    dl1_output_inst.open(KnobDL1OutputInst.Value().c_str());
    dl1_output_addr.open(KnobDL1OutputAddr.Value().c_str());
    dl1_output_time.open(KnobDL1OutputTime.Value().c_str());
    dl1_output_life.open(KnobDL1OutputLife.Value().c_str());

    inf_dl1    = new l1_cache::InfCache(
        // 2nd and 4th doesn't really matter but make it same as real
        "Infinite L1 data cache",
        KnobDL1CacheSize.Value() * KILO,
        KnobLineSize.Value(),
        KnobDL1NumWays.Value());

    fa_opt_dl1 = new l1_cache::OptCache(
        "Fully associative L1 data cache with Optimal replacement",
        KnobDL1CacheSize.Value() * KILO,
        KnobLineSize.Value(),
        KnobDL1CacheSize.Value() * KILO / KnobLineSize.Value(),
        KnobLookaheadDistance.Value(),
        KnobProcessWindow.Value());

    sa_opt_dl1 = new l1_cache::OptCache(
        "Set associative L1 data cache with Optimal replacement",
        KnobDL1CacheSize.Value() * KILO,
        KnobLineSize.Value(),
        KnobDL1NumWays.Value(),
        KnobLookaheadDistance.Value(),
        KnobProcessWindow.Value());

    sa_lru_dl1 = new l1_cache::LruCache(
        "Set associative L1 data cache with LRU replacement",
        KnobDL1CacheSize.Value() * KILO,
        KnobLineSize.Value(),
        KnobDL1NumWays.Value());
  }
  // }}}
  // L1 instruction cache {{{
  if (KnobIL1) {
    il1_output_inst.open(KnobIL1OutputInst.Value().c_str());
    il1_output_time.open(KnobIL1OutputTime.Value().c_str());
    il1_output_life.open(KnobIL1OutputLife.Value().c_str());

    inf_il1    = new l1_cache::InfCache(
        // 2nd and 4th doesn't really matter but make it same as real
        "Infinite L1 instruction cache",
        KnobIL1CacheSize.Value() * KILO,
        KnobLineSize.Value(),
        KnobIL1NumWays.Value());

    fa_opt_il1 = new l1_cache::OptCache(
        "Fully associative L1 instruction cache with Optimal replacement",
        KnobIL1CacheSize.Value() * KILO,
        KnobLineSize.Value(),
        KnobIL1CacheSize.Value() * KILO / KnobLineSize.Value(),
        KnobLookaheadDistance.Value(),
        KnobProcessWindow.Value());

    sa_opt_il1 = new l1_cache::OptCache(
        "Set associative L1 instruction cache with Optimal replacement",
        KnobIL1CacheSize.Value() * KILO,
        KnobLineSize.Value(),
        KnobIL1NumWays.Value(),
        KnobLookaheadDistance.Value(),
        KnobProcessWindow.Value());

    sa_lru_il1 = new l1_cache::LruCache(
        "Set associative L1 instruction cache with LRU replacement",
        KnobIL1CacheSize.Value() * KILO,
        KnobLineSize.Value(),
        KnobIL1NumWays.Value());
  }
  // }}}
  // L2 cache {{{
  if (KnobUL2) {
    ul2_output_inst.open(KnobUL2OutputInst.Value().c_str());
    ul2_output_addr.open(KnobUL2OutputAddr.Value().c_str());
    ul2_output_time.open(KnobUL2OutputTime.Value().c_str());
    ul2_output_life.open(KnobUL2OutputLife.Value().c_str());

    inf_ul2    = new l2_cache::InfCache(
        // 2nd and 4th doesn't really matter but make it same as real
        "Infinite L2 cache",
        KnobUL2CacheSize.Value() * KILO,
        KnobLineSize.Value(),
        KnobUL2NumWays.Value());

    fa_opt_ul2 = new l2_cache::OptCache(
        "Fully associative L2 cache with Optimal replacement",
        KnobUL2CacheSize.Value() * KILO,
        KnobLineSize.Value(),
        KnobUL2CacheSize.Value() * KILO / KnobLineSize.Value(),
        KnobLookaheadDistance.Value(),
        KnobProcessWindow.Value());

    sa_opt_ul2 = new l2_cache::OptCache(
        "Set associative L2 cache with Optimal replacement",
        KnobUL2CacheSize.Value() * KILO,
        KnobLineSize.Value(),
        KnobUL2NumWays.Value(),
        KnobLookaheadDistance.Value(),
        KnobProcessWindow.Value());

    sa_lru_ul2 = new l2_cache::LruCache(
        "Set associative L2 cache with LRU replacement",
        KnobUL2CacheSize.Value() * KILO,
        KnobLineSize.Value(),
        KnobUL2NumWays.Value());
  }
  // }}}

  PrintCacheConfig();

  IMG_AddInstrumentFunction(Image, 0);
  TRACE_AddInstrumentFunction(Trace, 0);
  RTN_AddInstrumentFunction(Routine, 0);
  INS_AddInstrumentFunction(Instruction, 0);

  PIN_AddFiniFunction(Fini, 0);

  // Never returns
  PIN_StartProgram();

  return 0;
}

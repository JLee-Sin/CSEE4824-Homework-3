#ifndef PINTOOL_CACHE_MISC_H
#define PINTOOL_CACHE_MISC_H

#include <sstream>
// Added by TA Adam Hastings on Tuesday, October 5, 2019 in order
// to get the cache simulator to compile on the simacc server...
using namespace std;

namespace misc_functions // {{{
{
template <class First, class Second>
class sort_second_ascending {
 public:
  bool operator() (const std::pair<First, Second> &left,
      const std::pair<First, Second> &right) {
    return left.second < right.second;
  }
};
} // namespace misc_functions }}}

const char *StripPath(const char *path)
{
  const char *file = strrchr(path, '/');
  if (file) {
    return file + 1;
  }
  else {
    return path;
  }
}

/*! RMR (rodric@gmail.com)
 *   - temporary work around because decstr()
 *     casts 64 bit ints to 32 bit ones
 */
static string MyDecstr(UINT64 v, UINT32 w)
{
  ostringstream o;
  o.width(w);
  o << v;
  string str(o.str());
  return str;
}

/*!
 *  @brief Checks if n is a power of 2.
 *  @returns true if n is power of 2
 */
static inline bool IsPower2(UINT32 n)
{
  return ((n & (n - 1)) == 0);
}

/*!
 *  @brief Computes floor(log2(n))
 *  Works by finding position of MSB set.
 *  @returns -1 if n == 0.
 */
static inline INT32 FloorLog2(UINT32 n)
{
  INT32 p = 0;

  if (n == 0) return -1;

  if (n & 0xffff0000) { p += 16; n >>= 16; }
  if (n & 0x0000ff00) { p +=  8; n >>=  8; }
  if (n & 0x000000f0) { p +=  4; n >>=  4; }
  if (n & 0x0000000c) { p +=  2; n >>=  2; }
  if (n & 0x00000002) { p +=  1; }

  return p;
}

/*!
 *  @brief Computes ceil(log2(n))
 *  Works by finding position of MSB set.
 *  @returns -1 if n == 0.
 */
static inline INT32 CeilLog2(UINT32 n)
{
  return FloorLog2(n - 1) + 1;
}

// Code below is taken from "pin_profile.H" and is modified for my purpose
/*!
 *  Class to map arbitrary sequences of sparse input values to
 *  a range of compact indices [0..N],
 *  such that the same input value always produces the same index.
 */
template <class Key, class Index>
class Compressor
{
 public:
  Compressor() { next_index_ = 0; }

  void SetKeyName(const std::string &key_name) { key_name_ = key_name; }
  Index Map(Key key) {
    typename std::map<Key, Index>::const_iterator it = map_.find(key);
    // Key found: return index
    if (it != map_.end()) {
      return it->second;
    }
    // Key not yet present: insert and return new index
    else {
      const std::pair<const Key, Index> p(key, next_index_);
      map_.insert(p);
      return next_index_++;
    }
  }

 protected:
  std::map<Key, Index> map_;
  Index next_index_;
  std::string key_name_;
};

/*!
 *  Class to provide a counter for each compresses index. Counters are
 *  accessed similar to standard library classes with array syntax [] for
 *  unchecked accesses, and with at() for range-checked accesses. The
 *  array of counters is auto-extending as necessary and is guaranteed to
 *  contain as many entries as have been mapped.
 */
template <class Key, class Index, class Counter>
class CompressorCounter : public Compressor<Key, Index>
{
 public:
  CompressorCounter(UINT32 initial_counter_size = default_initial_counter_size_)
    : Compressor<Key, Index>(), counters_(initial_counter_size) { }

  void SetCounterName(const std::string &counter_name) { counter_name_ = counter_name; }
  void SetThreshold(const Counter &threshold) { threshold_ = threshold; }
  Index Map(Key key) {
    // Use compressor to map
    const Index index = Compressor<Key, Index>::Map(key);

    // ... and check if need to add more counters
    if (index + 1 >= counters_.size()) {
      counters_.resize(2 * counters_.size());
    }
    return index;
  }
  const Counter &operator[] (Index index) const { return counters_[index]; }
        Counter &operator[] (Index index)       { return counters_[index]; }
  const Counter &at(Index index)          const { return counters_.at(index); }
        Counter &at(Index index)                { return counters_.at(index); }

  std::vector<Counter> *counters()              { return &counters_; }
  std::map<Key, Index> *map()                   { return &(this->map_); }

 private:
  static const UINT32 default_initial_counter_size_ = 8 * 1024;
  std::vector<Counter> counters_;
  std::string counter_name_;
  Counter threshold_;
};

/*!
 *  Class to provide an array of counters for use with CompressorCounter
 *  if more than a single counter is required.
 *  Counters are accessed similar to standard library classes with array
 *  syntax [] for unchecked accesses, and with at() for range-checked
 *  accesses. The array of counters is auto-extending as necessary and is
 *  guaranteed to contain as many entries as have been mapped.
 */
template <class CounterType, UINT32 NumCounters>
class CounterArray
{
 public:
  std::string str() const {
    std::string s;
    for (UINT32 i = 0; i < NumCounters; i++) {
      if (i != 0) {
        s += " ";
      }
      s += MyDecstr(counters_[i], 12);
    }
    return s;
  }
  // Allow compare to 0
  bool operator==(const CounterArray &x) const {
    for (UINT32 i = 0; i < NumCounters; i++) {
      if (counters_[i] != x.counters_[i]) {
        return false;
      }
    }
    return true;
  }
  bool operator!=(const CounterArray &x) const { return ! operator==(x); }
  bool operator<=(const CounterArray &x) const {
    for (UINT32 i = 0; i < NumCounters; i++) {
      if (counters_[i] > x.counters_[i]) {
        return false;
      }
    }
    return true;
  }

  // Modifiers
  const CounterType &operator[] (UINT32 index) const { return counters_[index]; }
        CounterType &operator[] (UINT32 index)       { return counters_[index]; }
  const CounterType &at(UINT32 index) const {
    assert(index < NumCounters);
    return counters_[index];
  }
        CounterType &at(UINT32 index) {
    assert(index < NumCounters);
    return counters_[index];
  }

 private:
  CounterType counters_[NumCounters];
};

#endif

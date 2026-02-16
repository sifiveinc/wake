/*
 * Copyright 2022 SiFive, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You should have received a copy of LICENSE.Apache2 along with
 * this software. If not, you may obtain a copy at
 *
 *    https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace wcl {

// wcl::trie maps sequences of equality comparable keys to values of any type.
// It does not efficently handle high fan out of the trie but it handles
// very small fan out extremely well.
// Assumes both Key and Value are movable.
template <class Key, class Value>
class trie {
 private:
  struct trie_node {
    // TODO: This would be an awesome use case for a small vector but its
    //       extra work to get that working and right now I just need a trie.
    //       by using a uint32_t and a properly implemented small_vector, you
    //       can get a 4 element small vector for *free* here which would make
    //       this trie entirely in place for the vast vast majority of nodes.
    std::vector<std::size_t> child_indexes;
    Key key;
    std::optional<Value> value;

    trie_node(trie_node&&) = default;
    trie_node(const trie_node&) = delete;
    explicit trie_node(Key&& k) : child_indexes(), key(std::move(k)), value() {}
  };
  std::vector<trie_node> nodes;
  // Because we chose to stores keys in nodes we have some funky handling
  // of the "first" node which is essentially the node coresponding to the empty
  // sequence. The benifit is that with a small_vector our trie will be very compact
  // and memory efficent.
  std::vector<size_t> starts;
  std::optional<Value> empty_seq;

  std::optional<size_t> matching_start(const Key& key) const {
    for (size_t index : starts) {
      if (key == nodes[index].key) return index;
    }
    return {};
  }

  std::optional<size_t> matching_child(size_t pindex, const Key& key) const {
    const trie_node* node = &nodes[pindex];
    for (size_t child_index : node->child_indexes) {
      if (key == nodes[child_index].key) return child_index;
    }
    return {};
  }

  template <class F>
  void for_each(F f, std::vector<Key>& prefix, const trie_node& node) const {
    prefix.push_back(node.key);
    if (node.value) {
      f(const_cast<const std::vector<Key>&>(prefix), *node.value);
    }

    for (size_t index : node.child_indexes) {
      for_each(f, prefix, nodes[index]);
    }
    prefix.pop_back();
  }

 public:
  template <class F>
  void for_each(F f) const {
    std::vector<Key> prefix = {};
    if (empty_seq) {
      f(const_cast<const std::vector<Key>&>(prefix), *empty_seq);
    }

    for (size_t start : starts) {
      for_each(f, prefix, nodes[start]);
    }
  }

  // NOTE: This insert moves the keys and constructs the Value
  //       emplace.

  template <class KeyIter, class... Args>
  void move_emplace(KeyIter begin, KeyIter end, Args&&... args) {
    // First handle the empty sequence
    if (begin == end) {
      empty_seq = std::make_optional<Value>(std::forward<Args>(args)...);
      return;
    }

    // Next we have to handle the first key specially because we chose to put keys
    // on trie nodes instead of on edges pointing to try nodes.
    std::optional<size_t> mroot = matching_start(*begin);
    size_t root;
    if (!mroot) {
      root = nodes.size();
      starts.push_back(root);
      nodes.emplace_back(std::move(*begin));
    } else {
      root = *mroot;
    }
    ++begin;

    // Now handle the common sub cases
    while (begin != end) {
      // Check if there's a matching child already
      std::optional<size_t> child = matching_child(root, *begin);
      if (child) {
        root = *child;
        ++begin;
        continue;
      }

      // If not that just means we have to allocate a node
      size_t new_node = nodes.size();
      nodes.emplace_back(std::move(*begin++));
      if (new_node != root) nodes[root].child_indexes.push_back(new_node);
      root = new_node;
    }

    // TODO: We're actually adding a move here and really there's no requirement that
    //       Value be movable. std::optional has an "emplace" method that could be used.
    nodes[root].value = std::make_optional<Value>(std::forward<Args>(args)...);
  }

  // Find the maximum prefix of a given sequence in the trie.
  template <class KeyIter>
  std::pair<const Value*, KeyIter> find_max(KeyIter begin, KeyIter end) const {
    // First we handle the empty sequence
    if (begin == end) {
      if (empty_seq) return std::make_pair(&*empty_seq, begin);
      return std::make_pair(nullptr, begin);
    }

    // Now handle the first node differently.
    KeyIter out_iter = begin;
    std::optional<size_t> mroot = matching_start(*begin++);
    size_t root;
    const Value* out = nullptr;

    if (!mroot) {
      return std::make_pair(nullptr, begin);
    } else {
      root = *mroot;
      if (nodes[root].value) {
        out = &*nodes[root].value;
        out_iter = begin;
      }
    }

    while (begin != end) {
      // Check if there's a matching child already
      std::optional<size_t> child = matching_child(root, *begin);
      if (child) {
        root = *child;
        ++begin;
        if (nodes[root].value) {
          out = &*nodes[root].value;
          out_iter = begin;
        }
        continue;
      }

      return std::make_pair(out, out_iter);
    }

    return std::make_pair(out, out_iter);
  }

  // Find the maximum prefix of a given sequence in the trie.
  template <class KeyIter>
  std::pair<Value*, KeyIter> find_max(KeyIter begin, KeyIter end) {
    // First we handle the empty sequence
    if (begin == end) {
      if (empty_seq) return std::make_pair(&*empty_seq, begin);
      return std::make_pair(nullptr, begin);
    }

    // Now handle the first node differently.
    KeyIter out_iter = begin;
    std::optional<size_t> mroot = matching_start(*begin++);
    size_t root;
    Value* out = nullptr;

    if (!mroot) {
      return std::make_pair(nullptr, begin);
    } else {
      root = *mroot;
      if (nodes[root].value) {
        out = &*nodes[root].value;
        out_iter = begin;
      }
    }

    while (begin != end) {
      // Check if there's a matching child already
      std::optional<size_t> child = matching_child(root, *begin);
      if (child) {
        root = *child;
        ++begin;
        if (nodes[root].value) {
          out = &*nodes[root].value;
          out_iter = begin;
        }
        continue;
      }

      return std::make_pair(out, out_iter);
    }

    return std::make_pair(out, out_iter);
  }

  template <class KeyIter>
  Value* find(KeyIter begin, KeyIter end) {
    auto pair = find_max(begin, end);
    if (pair.second == end) return pair.first;
    return nullptr;
  }

  template <class KeyIter>
  const Value* find(KeyIter begin, KeyIter end) const {
    auto pair = find_max(begin, end);
    if (pair.second == end) return pair.first;
    return nullptr;
  }
};

}  // namespace wcl

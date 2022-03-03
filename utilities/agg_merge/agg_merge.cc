//  Copyright (c) 2017-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#include "agg_merge.h"

#include <assert.h>

#include <deque>
#include <memory>
#include <utility>
#include <vector>

#include "rocksdb/merge_operator.h"
#include "rocksdb/slice.h"
#include "rocksdb/utilities/options_type.h"
#include "util/coding.h"
#include "utilities/merge_operators.h"

namespace ROCKSDB_NAMESPACE {
static std::unordered_map<std::string, std::unique_ptr<Aggregator>> func_map;

void AddAggregator(const std::string& function_name,
                   std::unique_ptr<Aggregator>&& agg) {
  func_map.emplace(function_name, std::move(agg));
}

AggMergeOperator::AggMergeOperator() {
}

std::string EncodeHelper::EncodeFuncAndInt(const Slice& function_name,
                                           int64_t value) {
  std::string encoded_value;
  PutVarsignedint64(&encoded_value, value);
  return EncodeFuncAndValue(function_name, encoded_value);
}

std::string EncodeHelper::EncodeInt(int64_t value) {
  std::string encoded_value;
  PutVarsignedint64(&encoded_value, value);
  return encoded_value;
}

std::string EncodeHelper::EncodeFuncAndValue(const Slice& function_name,
                                             const Slice& value) {
  std::string result;
  PutLengthPrefixedSlice(&result, function_name);
  result += value.ToString();
  return result;
}

std::string EncodeHelper::EncodeFuncAndList(const Slice& function_name,
                                            const std::vector<Slice>& list) {
  return EncodeFuncAndValue(function_name, EncodeList(list));
}

std::string EncodeHelper::EncodeList(const std::vector<Slice>& list) {
  std::string result;
  for (const Slice& entity : list) {
    PutLengthPrefixedSlice(&result, entity);
  }
  return result;
}

bool EncodeHelper::ExtractFuncAndValue(const Slice& op, Slice* func,
                                       Slice* value) {
  *value = op;
  return GetLengthPrefixedSlice(value, func);
}

std::string SumAggregator::Aggregate(const std::deque<Slice>& item_list) const {
  int64_t sum = 0;
  for (const Slice& item : item_list) {
    int64_t ivalue;
    Slice v = item;
    bool ret = GetVarsignedint64(&v, &ivalue);
    assert(ret);
    sum += ivalue;
  }
  std::string result;
  return EncodeHelper::EncodeInt(sum);
}

std::string Last3Aggregator::Aggregate(
    const std::deque<Slice>& item_list) const {
  std::vector<Slice> last3;
  last3.reserve(3);
  for (auto it = item_list.rbegin(); it != item_list.rend(); it++) {
    Slice item = *it;
    Slice entity;
    bool ret;
    while ((ret = GetLengthPrefixedSlice(&item, &entity))) {
      last3.push_back(entity);
      if (last3.size() >= 3) {
        break;
      }
    }
    if (last3.size() >= 3) {
      break;
    }
    if (!ret) {
      continue;
    }
  }
  return EncodeHelper::EncodeList(last3);
}

class Accumulator {
 public:
  void Add(const Slice& op) {
    Slice my_func;
    Slice my_value;
    bool ret = EncodeHelper::ExtractFuncAndValue(op, &my_func, &my_value);
    assert(ret);
    assert(func_.empty() || func_ == my_func);
    func_ = my_func;
    values_.push_back(my_value);
  }
  std::string GetResult() {
    return EncodeHelper::EncodeFuncAndValue(
        func_, func_map.at(func_.ToString())->Aggregate(values_));
  }
 private:
  Slice func_;
  std::deque<Slice> values_;
};

bool AggMergeOperator::FullMergeV2(
    const MergeOperationInput& merge_in,
    MergeOperationOutput* merge_out) const {
  Slice func;
  Accumulator agg;
  if (merge_in.existing_value != nullptr) {
    agg.Add(*merge_in.existing_value);
  }
  for (const Slice& op : merge_in.operand_list) {
    agg.Add(op);
  }
  std::string result = agg.GetResult();
  merge_out->new_value = result;
  return true;
}

bool AggMergeOperator::PartialMergeMulti(
    const Slice& /*key*/, const std::deque<Slice>& /*operand_list*/,
    std::string* /*new_value*/, Logger* /*logger*/) const {
  return false;
}
}  // namespace ROCKSDB_NAMESPACE

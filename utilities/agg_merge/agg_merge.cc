//  Copyright (c) 2017-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#include "agg_merge.h"

#include <assert.h>

#include <deque>
#include <memory>

#include "rocksdb/merge_operator.h"
#include "rocksdb/slice.h"
#include "rocksdb/utilities/options_type.h"
#include "util/coding.h"
#include "utilities/merge_operators.h"

namespace ROCKSDB_NAMESPACE {
AggMergeOperator::AggMergeOperator() {
}

std::string AggMergeOperator::EncodeIntValue(const Slice& function_name, int64_t value) {
    std::string result = function_name.ToString() + ";";
    PutVarsignedint64(&result, value);
    return result;
}

namespace {
bool ExtractFuncAndValue(const Slice& op,
Slice* func, Slice* value) {
  size_t fun_len = 0;
  for (fun_len = 0; fun_len < op.size(); fun_len++) {
    if (op.data()[fun_len] == ';') {
      break;
    }
  }
  if (fun_len == op.size()) {
    // Should not happen.
    return false;
  }
  *func = Slice(op.data(), fun_len);
  *value = Slice(op.data() + fun_len + 1, op.size() - fun_len - 1);
  return true;
}
}

class SumAccumulator {
 public:
  void Add(const Slice& element) {
    int64_t ivalue;
    Slice v = element;
    bool ret = GetVarsignedint64(&v, &ivalue);
    assert(ret);
    sum_ += ivalue;
  }

  std::string GetResult() {
    return AggMergeOperator::EncodeIntValue("sum", sum_);
  }
 private:
  int64_t sum_ = 0;
};

class Accumulator {
 public:
  void Add(const Slice& op) {
    Slice my_func;
    Slice my_value;
    bool ret = ExtractFuncAndValue(op, &my_func, &my_value);
    assert(ret);
    assert(func_.empty() || func_ == my_func);
    func_ = my_func;
    values_.push_back(my_value);
  }
  std::string GetResult() {
    SumAccumulator sum_agg;
    for (Slice& v : values_) {
      sum_agg.Add(v);
    }
    return sum_agg.GetResult();
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

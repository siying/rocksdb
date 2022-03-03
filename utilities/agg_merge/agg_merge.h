//  Copyright (c) 2017-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#pragma once
#include <algorithm>
#include <cstddef>
#include <memory>
#include <unordered_map>

#include "rocksdb/merge_operator.h"
#include "rocksdb/slice.h"
#include "utilities/cassandra/cassandra_options.h"

namespace ROCKSDB_NAMESPACE {

class Aggregator {
 public:
  virtual ~Aggregator() {}
  virtual std::string Aggregate(const std::deque<Slice>&) const = 0;
};

extern void AddAggregator(const std::string& function_name,
                          std::unique_ptr<Aggregator>&& agg);

class AggMergeOperator : public MergeOperator {
public:
 explicit AggMergeOperator();

 virtual bool FullMergeV2(const MergeOperationInput& merge_in,
                          MergeOperationOutput* merge_out) const override;

 virtual bool PartialMergeMulti(const Slice& key,
                                const std::deque<Slice>& operand_list,
                                std::string* new_value,
                                Logger* logger) const override;

 const char* Name() const override { return kClassName(); }
 static const char* kClassName() { return "AggMergeOperator"; }

 virtual bool AllowSingleOperand() const override { return true; }

 virtual bool ShouldMerge(const std::vector<Slice>&) const override {
   return false;
 }
};

class EncodeHelper {
 public:
  static std::string EncodeFuncAndValue(const Slice& function_name,
                                        const Slice& value);
  static std::string EncodeFuncAndInt(const Slice& function_name,
                                      int64_t value);
  static std::string EncodeInt(int64_t value);
  static std::string EncodeList(const std::vector<Slice>& list);
  static std::string EncodeFuncAndList(const Slice& function_name,
                                       const std::vector<Slice>& list);
  static bool ExtractFuncAndValue(const Slice& op, Slice* func, Slice* value);
};

class SumAggregator : public Aggregator {
 public:
  ~SumAggregator() override {}
  std::string Aggregate(const std::deque<Slice>& item_list) const override;
};

class Last3Aggregator : public Aggregator {
 public:
  ~Last3Aggregator() override {}
  std::string Aggregate(const std::deque<Slice>& item_list) const override;
};

}  // namespace ROCKSDB_NAMESPACE

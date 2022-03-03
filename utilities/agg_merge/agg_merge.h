//  Copyright (c) 2017-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#pragma once
#include "rocksdb/merge_operator.h"
#include "rocksdb/slice.h"
#include "utilities/cassandra/cassandra_options.h"

namespace ROCKSDB_NAMESPACE {

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

  static std::string EncodeIntValue(const Slice& function_name, int64_t value);

private:
};
}  // namespace ROCKSDB_NAMESPACE

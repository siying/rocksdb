// Copyright (c) 2017-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#include "agg_merge.h"
#include <memory>
#include "db/db_test_util.h"
#include "test_util/testharness.h"


namespace ROCKSDB_NAMESPACE {

class AggMergeTest : public DBTestBase {
 public:
  AggMergeTest() : DBTestBase("agg_merge_db_test", /*env_do_fsync=*/true) {}
};

TEST_F(AggMergeTest, TestMerge) {
  Options options = CurrentOptions();
  options.merge_operator = std::make_shared<AggMergeOperator>();
  Reopen(options);
  std::string v = AggMergeOperator::EncodeIntValue("sum", 10);
  ASSERT_OK(Merge("foo", v));
  v = AggMergeOperator::EncodeIntValue("sum", 20);
  ASSERT_OK(Merge("foo", v));
  v = AggMergeOperator::EncodeIntValue("sum", 15);
  ASSERT_OK(Merge("foo", v));
  Flush();
  EXPECT_EQ(AggMergeOperator::EncodeIntValue("sum", 45), Get("foo"));
}

}  // namespace ROCKSDB_NAMESPACE

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

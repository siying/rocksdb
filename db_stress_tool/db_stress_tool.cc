//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
// The test uses an array to compare against values written to the database.
// Keys written to the array are in 1:1 correspondence to the actual values in
// the database according to the formula in the function GenerateValue.

// Space is reserved in the array from 0 to FLAGS_max_key and values are
// randomly written/deleted/read from those positions. During verification we
// compare all the positions in the array. To shorten/elongate the running
// time, you could change the settings: FLAGS_max_key, FLAGS_ops_per_thread,
// (sometimes also FLAGS_threads).
//
// NOTE that if FLAGS_test_batches_snapshots is set, the test will have
// different behavior. See comment of the flag for details.

#ifdef GFLAGS
#include "db_stress_common.h"

namespace rocksdb {
int db_stress_tool(int argc, char** argv) {
  SetUsageMessage(std::string("\nUSAGE:\n") + std::string(argv[0]) +
                  " [OPTIONS]...");
  ParseCommandLineFlags(&argc, &argv, true);

  if (FLAGS_statistics) {
    dbstats = rocksdb::CreateDBStatistics();
    if (FLAGS_enable_secondary) {
      dbstats_secondaries = rocksdb::CreateDBStatistics();
    }
  }
  FLAGS_compression_type_e =
      StringToCompressionType(FLAGS_compression_type.c_str());
  FLAGS_checksum_type_e = StringToChecksumType(FLAGS_checksum_type.c_str());
  if (!FLAGS_hdfs.empty()) {
    if (!FLAGS_env_uri.empty()) {
      fprintf(stderr, "Cannot specify both --hdfs and --env_uri.\n");
      exit(1);
    }
    FLAGS_env = new rocksdb::HdfsEnv(FLAGS_hdfs);
  } else if (!FLAGS_env_uri.empty()) {
    Status s = Env::LoadEnv(FLAGS_env_uri, &FLAGS_env, &env_guard);
    if (FLAGS_env == nullptr) {
      fprintf(stderr, "No Env registered for URI: %s\n", FLAGS_env_uri.c_str());
      exit(1);
    }
  }
  FLAGS_rep_factory = StringToRepFactory(FLAGS_memtablerep.c_str());

  // The number of background threads should be at least as much the
  // max number of concurrent compactions.
  FLAGS_env->SetBackgroundThreads(FLAGS_max_background_compactions);
  FLAGS_env->SetBackgroundThreads(FLAGS_num_bottom_pri_threads,
                                  rocksdb::Env::Priority::BOTTOM);
  if (FLAGS_prefixpercent > 0 && FLAGS_prefix_size < 0) {
    fprintf(stderr,
            "Error: prefixpercent is non-zero while prefix_size is "
            "not positive!\n");
    exit(1);
  }
  if (FLAGS_test_batches_snapshots && FLAGS_prefix_size <= 0) {
    fprintf(stderr,
            "Error: please specify prefix_size for "
            "test_batches_snapshots test!\n");
    exit(1);
  }
  if (FLAGS_memtable_prefix_bloom_size_ratio > 0.0 && FLAGS_prefix_size < 0) {
    fprintf(stderr,
            "Error: please specify positive prefix_size in order to use "
            "memtable_prefix_bloom_size_ratio\n");
    exit(1);
  }
  if ((FLAGS_readpercent + FLAGS_prefixpercent + FLAGS_writepercent +
       FLAGS_delpercent + FLAGS_delrangepercent + FLAGS_iterpercent) != 100) {
    fprintf(stderr,
            "Error: Read+Prefix+Write+Delete+DeleteRange+Iterate percents != "
            "100!\n");
    exit(1);
  }
  if (FLAGS_disable_wal == 1 && FLAGS_reopen > 0) {
    fprintf(stderr, "Error: Db cannot reopen safely with disable_wal set!\n");
    exit(1);
  }
  if ((unsigned)FLAGS_reopen >= FLAGS_ops_per_thread) {
    fprintf(stderr,
            "Error: #DB-reopens should be < ops_per_thread\n"
            "Provided reopens = %d and ops_per_thread = %lu\n",
            FLAGS_reopen, (unsigned long)FLAGS_ops_per_thread);
    exit(1);
  }
  if (FLAGS_test_batches_snapshots && FLAGS_delrangepercent > 0) {
    fprintf(stderr,
            "Error: nonzero delrangepercent unsupported in "
            "test_batches_snapshots mode\n");
    exit(1);
  }
  if (FLAGS_active_width > FLAGS_max_key) {
    fprintf(stderr, "Error: active_width can be at most max_key\n");
    exit(1);
  } else if (FLAGS_active_width == 0) {
    FLAGS_active_width = FLAGS_max_key;
  }
  if (FLAGS_value_size_mult * kRandomValueMaxFactor > kValueMaxLen) {
    fprintf(stderr, "Error: value_size_mult can be at most %d\n",
            kValueMaxLen / kRandomValueMaxFactor);
    exit(1);
  }
  if (FLAGS_use_merge && FLAGS_nooverwritepercent == 100) {
    fprintf(
        stderr,
        "Error: nooverwritepercent must not be 100 when using merge operands");
    exit(1);
  }
  if (FLAGS_ingest_external_file_one_in > 0 && FLAGS_nooverwritepercent > 0) {
    fprintf(stderr,
            "Error: nooverwritepercent must be 0 when using file ingestion\n");
    exit(1);
  }
  if (FLAGS_clear_column_family_one_in > 0 && FLAGS_backup_one_in > 0) {
    fprintf(stderr,
            "Error: clear_column_family_one_in must be 0 when using backup\n");
    exit(1);
  }
  if (FLAGS_test_cf_consistency && FLAGS_disable_wal) {
    FLAGS_atomic_flush = true;
  }

  if (FLAGS_read_only) {
    if (FLAGS_writepercent != 0 || FLAGS_delpercent != 0 ||
        FLAGS_delrangepercent != 0) {
      fprintf(stderr, "Error: updates are not supported in read only mode\n");
      exit(1);
    } else if (FLAGS_checkpoint_one_in > 0 &&
               FLAGS_clear_column_family_one_in > 0) {
      fprintf(stdout,
              "Warn: checkpoint won't be validated since column families may "
              "be dropped.\n");
    }
  }

  // Choose a location for the test database if none given with --db=<path>
  if (FLAGS_db.empty()) {
    std::string default_db_path;
    FLAGS_env->GetTestDirectory(&default_db_path);
    default_db_path += "/dbstress";
    FLAGS_db = default_db_path;
  }

  if (FLAGS_enable_secondary && FLAGS_secondaries_base.empty()) {
    std::string default_secondaries_path;
    FLAGS_env->GetTestDirectory(&default_secondaries_path);
    default_secondaries_path += "/dbstress_secondaries";
    rocksdb::Status s = FLAGS_env->CreateDirIfMissing(default_secondaries_path);
    if (!s.ok()) {
      fprintf(stderr, "Failed to create directory %s: %s\n",
              default_secondaries_path.c_str(), s.ToString().c_str());
      exit(1);
    }
    FLAGS_secondaries_base = default_secondaries_path;
  }

  if (!FLAGS_enable_secondary && FLAGS_secondary_catch_up_one_in > 0) {
    fprintf(stderr, "Secondary instance is disabled.\n");
    exit(1);
  }

  rocksdb_kill_odds = FLAGS_kill_random_test;
  rocksdb_kill_prefix_blacklist = SplitString(FLAGS_kill_prefix_blacklist);

  std::unique_ptr<rocksdb::StressTest> stress;
  if (FLAGS_test_cf_consistency) {
    stress.reset(CreateCfConsistencyStressTest());
  } else if (FLAGS_test_batches_snapshots) {
    stress.reset(CreateBatchedOpsStressTest());
  } else {
    stress.reset(CreateNonBatchedOpsStressTest());
  }
  if (stress->Run()) {
    return 0;
  } else {
    return 1;
  }
}

}  // namespace rocksdb
#endif  // GFLAGS

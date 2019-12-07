//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//

#ifdef GFLAGS
#include "db_stress_common.h"

namespace rocksdb {
StressTest::StressTest()
    : cache_(NewCache(FLAGS_cache_size)),
      compressed_cache_(NewLRUCache(FLAGS_compressed_cache_size)),
      filter_policy_(FLAGS_bloom_bits >= 0
                         ? FLAGS_use_block_based_filter
                               ? NewBloomFilterPolicy(FLAGS_bloom_bits, true)
                               : NewBloomFilterPolicy(FLAGS_bloom_bits, false)
                         : nullptr),
      db_(nullptr),
#ifndef ROCKSDB_LITE
      txn_db_(nullptr),
#endif
      new_column_family_name_(1),
      num_times_reopened_(0),
      db_preload_finished_(false) {
  if (FLAGS_destroy_db_initially) {
    std::vector<std::string> files;
    FLAGS_env->GetChildren(FLAGS_db, &files);
    for (unsigned int i = 0; i < files.size(); i++) {
      if (Slice(files[i]).starts_with("heap-")) {
        FLAGS_env->DeleteFile(FLAGS_db + "/" + files[i]);
      }
    }
    Options options;
    options.env = FLAGS_env;
    Status s = DestroyDB(FLAGS_db, options);
    if (!s.ok()) {
      fprintf(stderr, "Cannot destroy original db: %s\n", s.ToString().c_str());
      exit(1);
    }
  }
}

StressTest::~StressTest() {
  for (auto cf : column_families_) {
    delete cf;
  }
  column_families_.clear();
  delete db_;

  assert(secondaries_.size() == secondary_cfh_lists_.size());
  size_t n = secondaries_.size();
  for (size_t i = 0; i != n; ++i) {
    for (auto* cf : secondary_cfh_lists_[i]) {
      delete cf;
    }
    secondary_cfh_lists_[i].clear();
    delete secondaries_[i];
  }
  secondaries_.clear();
}

std::shared_ptr<Cache> StressTest::NewCache(size_t capacity) {
  if (capacity <= 0) {
    return nullptr;
  }
  if (FLAGS_use_clock_cache) {
    auto cache = NewClockCache((size_t)capacity);
    if (!cache) {
      fprintf(stderr, "Clock cache not supported.");
      exit(1);
    }
    return cache;
  } else {
    return NewLRUCache((size_t)capacity);
  }
}

bool StressTest::BuildOptionsTable() {
  if (FLAGS_set_options_one_in <= 0) {
    return true;
  }

  std::unordered_map<std::string, std::vector<std::string>> options_tbl = {
      {"write_buffer_size",
       {ToString(options_.write_buffer_size),
        ToString(options_.write_buffer_size * 2),
        ToString(options_.write_buffer_size * 4)}},
      {"max_write_buffer_number",
       {ToString(options_.max_write_buffer_number),
        ToString(options_.max_write_buffer_number * 2),
        ToString(options_.max_write_buffer_number * 4)}},
      {"arena_block_size",
       {
           ToString(options_.arena_block_size),
           ToString(options_.write_buffer_size / 4),
           ToString(options_.write_buffer_size / 8),
       }},
      {"memtable_huge_page_size", {"0", ToString(2 * 1024 * 1024)}},
      {"max_successive_merges", {"0", "2", "4"}},
      {"inplace_update_num_locks", {"100", "200", "300"}},
      // TODO(ljin): enable test for this option
      // {"disable_auto_compactions", {"100", "200", "300"}},
      {"soft_rate_limit", {"0", "0.5", "0.9"}},
      {"hard_rate_limit", {"0", "1.1", "2.0"}},
      {"level0_file_num_compaction_trigger",
       {
           ToString(options_.level0_file_num_compaction_trigger),
           ToString(options_.level0_file_num_compaction_trigger + 2),
           ToString(options_.level0_file_num_compaction_trigger + 4),
       }},
      {"level0_slowdown_writes_trigger",
       {
           ToString(options_.level0_slowdown_writes_trigger),
           ToString(options_.level0_slowdown_writes_trigger + 2),
           ToString(options_.level0_slowdown_writes_trigger + 4),
       }},
      {"level0_stop_writes_trigger",
       {
           ToString(options_.level0_stop_writes_trigger),
           ToString(options_.level0_stop_writes_trigger + 2),
           ToString(options_.level0_stop_writes_trigger + 4),
       }},
      {"max_compaction_bytes",
       {
           ToString(options_.target_file_size_base * 5),
           ToString(options_.target_file_size_base * 15),
           ToString(options_.target_file_size_base * 100),
       }},
      {"target_file_size_base",
       {
           ToString(options_.target_file_size_base),
           ToString(options_.target_file_size_base * 2),
           ToString(options_.target_file_size_base * 4),
       }},
      {"target_file_size_multiplier",
       {
           ToString(options_.target_file_size_multiplier),
           "1",
           "2",
       }},
      {"max_bytes_for_level_base",
       {
           ToString(options_.max_bytes_for_level_base / 2),
           ToString(options_.max_bytes_for_level_base),
           ToString(options_.max_bytes_for_level_base * 2),
       }},
      {"max_bytes_for_level_multiplier",
       {
           ToString(options_.max_bytes_for_level_multiplier),
           "1",
           "2",
       }},
      {"max_sequential_skip_in_iterations", {"4", "8", "12"}},
  };

  options_table_ = std::move(options_tbl);

  for (const auto& iter : options_table_) {
    options_index_.push_back(iter.first);
  }
  return true;
}

bool StressTest::Run() {
  uint64_t now = FLAGS_env->NowMicros();
  fprintf(stdout, "%s Initializing db_stress\n",
          FLAGS_env->TimeToString(now / 1000000).c_str());
  PrintEnv();
  Open();
  BuildOptionsTable();
  SharedState shared(FLAGS_env, this);

  if (FLAGS_read_only) {
    now = FLAGS_env->NowMicros();
    fprintf(stdout, "%s Preloading db with %" PRIu64 " KVs\n",
            FLAGS_env->TimeToString(now / 1000000).c_str(), FLAGS_max_key);
    PreloadDbAndReopenAsReadOnly(FLAGS_max_key, &shared);
  }
  uint32_t n = shared.GetNumThreads();

  now = FLAGS_env->NowMicros();
  fprintf(stdout, "%s Initializing worker threads\n",
          FLAGS_env->TimeToString(now / 1000000).c_str());
  std::vector<ThreadState*> threads(n);
  for (uint32_t i = 0; i < n; i++) {
    threads[i] = new ThreadState(i, &shared);
    FLAGS_env->StartThread(ThreadBody, threads[i]);
  }
  ThreadState bg_thread(0, &shared);
  if (FLAGS_compaction_thread_pool_adjust_interval > 0) {
    FLAGS_env->StartThread(PoolSizeChangeThread, &bg_thread);
  }

  // Each thread goes through the following states:
  // initializing -> wait for others to init -> read/populate/depopulate
  // wait for others to operate -> verify -> done

  {
    MutexLock l(shared.GetMutex());
    while (!shared.AllInitialized()) {
      shared.GetCondVar()->Wait();
    }
    if (shared.ShouldVerifyAtBeginning()) {
      if (shared.HasVerificationFailedYet()) {
        printf("Crash-recovery verification failed :(\n");
      } else {
        printf("Crash-recovery verification passed :)\n");
      }
    }

    now = FLAGS_env->NowMicros();
    fprintf(stdout, "%s Starting database operations\n",
            FLAGS_env->TimeToString(now / 1000000).c_str());

    shared.SetStart();
    shared.GetCondVar()->SignalAll();
    while (!shared.AllOperated()) {
      shared.GetCondVar()->Wait();
    }

    now = FLAGS_env->NowMicros();
    if (FLAGS_test_batches_snapshots) {
      fprintf(stdout, "%s Limited verification already done during gets\n",
              FLAGS_env->TimeToString((uint64_t)now / 1000000).c_str());
    } else {
      fprintf(stdout, "%s Starting verification\n",
              FLAGS_env->TimeToString((uint64_t)now / 1000000).c_str());
    }

    shared.SetStartVerify();
    shared.GetCondVar()->SignalAll();
    while (!shared.AllDone()) {
      shared.GetCondVar()->Wait();
    }
  }

  for (unsigned int i = 1; i < n; i++) {
    threads[0]->stats.Merge(threads[i]->stats);
  }
  threads[0]->stats.Report("Stress Test");

  for (unsigned int i = 0; i < n; i++) {
    delete threads[i];
    threads[i] = nullptr;
  }
  now = FLAGS_env->NowMicros();
  if (!FLAGS_test_batches_snapshots && !shared.HasVerificationFailedYet()) {
    fprintf(stdout, "%s Verification successful\n",
            FLAGS_env->TimeToString(now / 1000000).c_str());
  }
  PrintStatistics();

  if (FLAGS_compaction_thread_pool_adjust_interval > 0) {
    MutexLock l(shared.GetMutex());
    shared.SetShouldStopBgThread();
    while (!shared.BgThreadFinished()) {
      shared.GetCondVar()->Wait();
    }
  }

#ifndef ROCKSDB_LITE
  if (FLAGS_enable_secondary) {
    now = FLAGS_env->NowMicros();
    fprintf(
        stdout, "%s Start to verify secondaries against primary\n",
        FLAGS_env->TimeToString(static_cast<uint64_t>(now) / 1000000).c_str());
  }
  for (size_t k = 0; k != secondaries_.size(); ++k) {
    Status s = secondaries_[k]->TryCatchUpWithPrimary();
    if (!s.ok()) {
      fprintf(stderr, "Secondary failed to catch up with primary\n");
      return false;
    }
    ReadOptions ropts;
    ropts.total_order_seek = true;
    // Verify only the default column family since the primary may have
    // dropped other column families after most recent reopen.
    std::unique_ptr<Iterator> iter1(db_->NewIterator(ropts));
    std::unique_ptr<Iterator> iter2(secondaries_[k]->NewIterator(ropts));
    for (iter1->SeekToFirst(), iter2->SeekToFirst();
         iter1->Valid() && iter2->Valid(); iter1->Next(), iter2->Next()) {
      if (iter1->key().compare(iter2->key()) != 0 ||
          iter1->value().compare(iter2->value())) {
        fprintf(stderr,
                "Secondary %d contains different data from "
                "primary.\nPrimary: %s : %s\nSecondary: %s : %s\n",
                static_cast<int>(k),
                iter1->key().ToString(/*hex=*/true).c_str(),
                iter1->value().ToString(/*hex=*/true).c_str(),
                iter2->key().ToString(/*hex=*/true).c_str(),
                iter2->value().ToString(/*hex=*/true).c_str());
        return false;
      }
    }
    if (iter1->Valid() && !iter2->Valid()) {
      fprintf(stderr,
              "Secondary %d record count is smaller than that of primary\n",
              static_cast<int>(k));
      return false;
    } else if (!iter1->Valid() && iter2->Valid()) {
      fprintf(stderr,
              "Secondary %d record count is larger than that of primary\n",
              static_cast<int>(k));
      return false;
    }
  }
  if (FLAGS_enable_secondary) {
    now = FLAGS_env->NowMicros();
    fprintf(
        stdout, "%s Verification of secondaries succeeded\n",
        FLAGS_env->TimeToString(static_cast<uint64_t>(now) / 1000000).c_str());
  }
#endif  // ROCKSDB_LITE

  if (shared.HasVerificationFailedYet()) {
    printf("Verification failed :(\n");
    return false;
  }
  return true;
}

void StressTest::ThreadBody(void* v) {
  ThreadState* thread = reinterpret_cast<ThreadState*>(v);
  SharedState* shared = thread->shared;

  if (shared->ShouldVerifyAtBeginning()) {
    thread->shared->GetStressTest()->VerifyDb(thread);
  }
  {
    MutexLock l(shared->GetMutex());
    shared->IncInitialized();
    if (shared->AllInitialized()) {
      shared->GetCondVar()->SignalAll();
    }
    while (!shared->Started()) {
      shared->GetCondVar()->Wait();
    }
  }
  thread->shared->GetStressTest()->OperateDb(thread);

  {
    MutexLock l(shared->GetMutex());
    shared->IncOperated();
    if (shared->AllOperated()) {
      shared->GetCondVar()->SignalAll();
    }
    while (!shared->VerifyStarted()) {
      shared->GetCondVar()->Wait();
    }
  }

  thread->shared->GetStressTest()->VerifyDb(thread);

  {
    MutexLock l(shared->GetMutex());
    shared->IncDone();
    if (shared->AllDone()) {
      shared->GetCondVar()->SignalAll();
    }
  }
}

Status StressTest::AssertSame(DB* db, ColumnFamilyHandle* cf,
                              ThreadState::SnapshotState& snap_state) {
  Status s;
  if (cf->GetName() != snap_state.cf_at_name) {
    return s;
  }
  ReadOptions ropt;
  ropt.snapshot = snap_state.snapshot;
  PinnableSlice exp_v(&snap_state.value);
  exp_v.PinSelf();
  PinnableSlice v;
  s = db->Get(ropt, cf, snap_state.key, &v);
  if (!s.ok() && !s.IsNotFound()) {
    return s;
  }
  if (snap_state.status != s) {
    return Status::Corruption(
        "The snapshot gave inconsistent results for key " +
        ToString(Hash(snap_state.key.c_str(), snap_state.key.size(), 0)) +
        " in cf " + cf->GetName() + ": (" + snap_state.status.ToString() +
        ") vs. (" + s.ToString() + ")");
  }
  if (s.ok()) {
    if (exp_v != v) {
      return Status::Corruption("The snapshot gave inconsistent values: (" +
                                exp_v.ToString() + ") vs. (" + v.ToString() +
                                ")");
    }
  }
  if (snap_state.key_vec != nullptr) {
    // When `prefix_extractor` is set, seeking to beginning and scanning
    // across prefixes are only supported with `total_order_seek` set.
    ropt.total_order_seek = true;
    std::unique_ptr<Iterator> iterator(db->NewIterator(ropt));
    std::unique_ptr<std::vector<bool>> tmp_bitvec(
        new std::vector<bool>(FLAGS_max_key));
    for (iterator->SeekToFirst(); iterator->Valid(); iterator->Next()) {
      uint64_t key_val;
      if (GetIntVal(iterator->key().ToString(), &key_val)) {
        (*tmp_bitvec.get())[key_val] = true;
      }
    }
    if (!std::equal(snap_state.key_vec->begin(), snap_state.key_vec->end(),
                    tmp_bitvec.get()->begin())) {
      return Status::Corruption("Found inconsistent keys at this snapshot");
    }
  }
  return Status::OK();
}

// Currently PreloadDb has to be single-threaded.
void StressTest::PreloadDbAndReopenAsReadOnly(int64_t number_of_keys,
                                              SharedState* shared) {
  WriteOptions write_opts;
  write_opts.disableWAL = FLAGS_disable_wal;
  if (FLAGS_sync) {
    write_opts.sync = true;
  }
  char value[100];
  int cf_idx = 0;
  Status s;
  for (auto cfh : column_families_) {
    for (int64_t k = 0; k != number_of_keys; ++k) {
      std::string key_str = Key(k);
      Slice key = key_str;
      size_t sz = GenerateValue(0 /*value_base*/, value, sizeof(value));
      Slice v(value, sz);
      shared->Put(cf_idx, k, 0, true /* pending */);

      if (FLAGS_use_merge) {
        if (!FLAGS_use_txn) {
          s = db_->Merge(write_opts, cfh, key, v);
        } else {
#ifndef ROCKSDB_LITE
          Transaction* txn;
          s = NewTxn(write_opts, &txn);
          if (s.ok()) {
            s = txn->Merge(cfh, key, v);
            if (s.ok()) {
              s = CommitTxn(txn);
            }
          }
#endif
        }
      } else {
        if (!FLAGS_use_txn) {
          s = db_->Put(write_opts, cfh, key, v);
        } else {
#ifndef ROCKSDB_LITE
          Transaction* txn;
          s = NewTxn(write_opts, &txn);
          if (s.ok()) {
            s = txn->Put(cfh, key, v);
            if (s.ok()) {
              s = CommitTxn(txn);
            }
          }
#endif
        }
      }

      shared->Put(cf_idx, k, 0, false /* pending */);
      if (!s.ok()) {
        break;
      }
    }
    if (!s.ok()) {
      break;
    }
    ++cf_idx;
  }
  if (s.ok()) {
    s = db_->Flush(FlushOptions(), column_families_);
  }
  if (s.ok()) {
    for (auto cf : column_families_) {
      delete cf;
    }
    column_families_.clear();
    delete db_;
    db_ = nullptr;
#ifndef ROCKSDB_LITE
    txn_db_ = nullptr;
#endif

    db_preload_finished_.store(true);
    auto now = FLAGS_env->NowMicros();
    fprintf(stdout, "%s Reopening database in read-only\n",
            FLAGS_env->TimeToString(now / 1000000).c_str());
    // Reopen as read-only, can ignore all options related to updates
    Open();
  } else {
    fprintf(stderr, "Failed to preload db");
    exit(1);
  }
}

Status StressTest::SetOptions(ThreadState* thread) {
  assert(FLAGS_set_options_one_in > 0);
  std::unordered_map<std::string, std::string> opts;
  std::string name =
      options_index_[thread->rand.Next() % options_index_.size()];
  int value_idx = thread->rand.Next() % options_table_[name].size();
  if (name == "soft_rate_limit" || name == "hard_rate_limit") {
    opts["soft_rate_limit"] = options_table_["soft_rate_limit"][value_idx];
    opts["hard_rate_limit"] = options_table_["hard_rate_limit"][value_idx];
  } else if (name == "level0_file_num_compaction_trigger" ||
             name == "level0_slowdown_writes_trigger" ||
             name == "level0_stop_writes_trigger") {
    opts["level0_file_num_compaction_trigger"] =
        options_table_["level0_file_num_compaction_trigger"][value_idx];
    opts["level0_slowdown_writes_trigger"] =
        options_table_["level0_slowdown_writes_trigger"][value_idx];
    opts["level0_stop_writes_trigger"] =
        options_table_["level0_stop_writes_trigger"][value_idx];
  } else {
    opts[name] = options_table_[name][value_idx];
  }

  int rand_cf_idx = thread->rand.Next() % FLAGS_column_families;
  auto cfh = column_families_[rand_cf_idx];
  return db_->SetOptions(cfh, opts);
}

#ifndef ROCKSDB_LITE
Status StressTest::NewTxn(WriteOptions& write_opts, Transaction** txn) {
  if (!FLAGS_use_txn) {
    return Status::InvalidArgument("NewTxn when FLAGS_use_txn is not set");
  }
  static std::atomic<uint64_t> txn_id = {0};
  TransactionOptions txn_options;
  *txn = txn_db_->BeginTransaction(write_opts, txn_options);
  auto istr = std::to_string(txn_id.fetch_add(1));
  Status s = (*txn)->SetName("xid" + istr);
  return s;
}

Status StressTest::CommitTxn(Transaction* txn) {
  if (!FLAGS_use_txn) {
    return Status::InvalidArgument("CommitTxn when FLAGS_use_txn is not set");
  }
  Status s = txn->Prepare();
  if (s.ok()) {
    s = txn->Commit();
  }
  delete txn;
  return s;
}
#endif

void StressTest::OperateDb(ThreadState* thread) {
  ReadOptions read_opts(FLAGS_verify_checksum, true);
  WriteOptions write_opts;
  auto shared = thread->shared;
  char value[100];
  std::string from_db;
  if (FLAGS_sync) {
    write_opts.sync = true;
  }
  write_opts.disableWAL = FLAGS_disable_wal;
  const int prefixBound = (int)FLAGS_readpercent + (int)FLAGS_prefixpercent;
  const int writeBound = prefixBound + (int)FLAGS_writepercent;
  const int delBound = writeBound + (int)FLAGS_delpercent;
  const int delRangeBound = delBound + (int)FLAGS_delrangepercent;
  const uint64_t ops_per_open = FLAGS_ops_per_thread / (FLAGS_reopen + 1);

  thread->stats.Start();
  for (int open_cnt = 0; open_cnt <= FLAGS_reopen; ++open_cnt) {
    if (thread->shared->HasVerificationFailedYet()) {
      break;
    }
    if (open_cnt != 0) {
      thread->stats.FinishedSingleOp();
      MutexLock l(thread->shared->GetMutex());
      while (!thread->snapshot_queue.empty()) {
        db_->ReleaseSnapshot(thread->snapshot_queue.front().second.snapshot);
        delete thread->snapshot_queue.front().second.key_vec;
        thread->snapshot_queue.pop();
      }
      thread->shared->IncVotedReopen();
      if (thread->shared->AllVotedReopen()) {
        thread->shared->GetStressTest()->Reopen();
        thread->shared->GetCondVar()->SignalAll();
      } else {
        thread->shared->GetCondVar()->Wait();
      }
      // Commenting this out as we don't want to reset stats on each open.
      // thread->stats.Start();
    }

    for (uint64_t i = 0; i < ops_per_open; i++) {
      if (thread->shared->HasVerificationFailedYet()) {
        break;
      }

      // Change Options
      if (FLAGS_set_options_one_in > 0 &&
          thread->rand.OneIn(FLAGS_set_options_one_in)) {
        SetOptions(thread);
      }

      if (FLAGS_set_in_place_one_in > 0 &&
          thread->rand.OneIn(FLAGS_set_in_place_one_in)) {
        options_.inplace_update_support ^= options_.inplace_update_support;
      }

      MaybeClearOneColumnFamily(thread);

#ifndef ROCKSDB_LITE
      if (FLAGS_compact_files_one_in > 0 &&
          thread->rand.Uniform(FLAGS_compact_files_one_in) == 0) {
        auto* random_cf =
            column_families_[thread->rand.Next() % FLAGS_column_families];
        rocksdb::ColumnFamilyMetaData cf_meta_data;
        db_->GetColumnFamilyMetaData(random_cf, &cf_meta_data);

        // Randomly compact up to three consecutive files from a level
        const int kMaxRetry = 3;
        for (int attempt = 0; attempt < kMaxRetry; ++attempt) {
          size_t random_level = thread->rand.Uniform(
              static_cast<int>(cf_meta_data.levels.size()));

          const auto& files = cf_meta_data.levels[random_level].files;
          if (files.size() > 0) {
            size_t random_file_index =
                thread->rand.Uniform(static_cast<int>(files.size()));
            if (files[random_file_index].being_compacted) {
              // Retry as the selected file is currently being compacted
              continue;
            }

            std::vector<std::string> input_files;
            input_files.push_back(files[random_file_index].name);
            if (random_file_index > 0 &&
                !files[random_file_index - 1].being_compacted) {
              input_files.push_back(files[random_file_index - 1].name);
            }
            if (random_file_index + 1 < files.size() &&
                !files[random_file_index + 1].being_compacted) {
              input_files.push_back(files[random_file_index + 1].name);
            }

            size_t output_level =
                std::min(random_level + 1, cf_meta_data.levels.size() - 1);
            auto s =
                db_->CompactFiles(CompactionOptions(), random_cf, input_files,
                                  static_cast<int>(output_level));
            if (!s.ok()) {
              fprintf(stdout, "Unable to perform CompactFiles(): %s\n",
                      s.ToString().c_str());
              thread->stats.AddNumCompactFilesFailed(1);
            } else {
              thread->stats.AddNumCompactFilesSucceed(1);
            }
            break;
          }
        }
      }
#endif  // !ROCKSDB_LITE
      int64_t rand_key = GenerateOneKey(thread, i);
      int rand_column_family = thread->rand.Next() % FLAGS_column_families;
      std::string keystr = Key(rand_key);
      Slice key = keystr;
      std::unique_ptr<MutexLock> lock;
      if (ShouldAcquireMutexOnKey()) {
        lock.reset(new MutexLock(
            shared->GetMutexForKey(rand_column_family, rand_key)));
      }

      auto column_family = column_families_[rand_column_family];

      if (FLAGS_compact_range_one_in > 0 &&
          thread->rand.Uniform(FLAGS_compact_range_one_in) == 0) {
        int64_t end_key_num;
        if (port::kMaxInt64 - rand_key < FLAGS_compact_range_width) {
          end_key_num = port::kMaxInt64;
        } else {
          end_key_num = FLAGS_compact_range_width + rand_key;
        }
        std::string end_key_buf = Key(end_key_num);
        Slice end_key(end_key_buf);

        CompactRangeOptions cro;
        cro.exclusive_manual_compaction =
            static_cast<bool>(thread->rand.Next() % 2);
        Status status = db_->CompactRange(cro, column_family, &key, &end_key);
        if (!status.ok()) {
          printf("Unable to perform CompactRange(): %s\n",
                 status.ToString().c_str());
        }
      }

      std::vector<int> rand_column_families =
          GenerateColumnFamilies(FLAGS_column_families, rand_column_family);

      if (FLAGS_flush_one_in > 0 &&
          thread->rand.Uniform(FLAGS_flush_one_in) == 0) {
        FlushOptions flush_opts;
        std::vector<ColumnFamilyHandle*> cfhs;
        std::for_each(
            rand_column_families.begin(), rand_column_families.end(),
            [this, &cfhs](int k) { cfhs.push_back(column_families_[k]); });
        Status status = db_->Flush(flush_opts, cfhs);
        if (!status.ok()) {
          fprintf(stdout, "Unable to perform Flush(): %s\n",
                  status.ToString().c_str());
        }
      }

      std::vector<int64_t> rand_keys = GenerateKeys(rand_key);

      if (FLAGS_ingest_external_file_one_in > 0 &&
          thread->rand.Uniform(FLAGS_ingest_external_file_one_in) == 0) {
        TestIngestExternalFile(thread, rand_column_families, rand_keys, lock);
      }

      if (FLAGS_backup_one_in > 0 &&
          thread->rand.Uniform(FLAGS_backup_one_in) == 0) {
        Status s = TestBackupRestore(thread, rand_column_families, rand_keys);
        if (!s.ok()) {
          VerificationAbort(shared, "Backup/restore gave inconsistent state",
                            s);
        }
      }

      if (FLAGS_checkpoint_one_in > 0 &&
          thread->rand.Uniform(FLAGS_checkpoint_one_in) == 0) {
        Status s = TestCheckpoint(thread, rand_column_families, rand_keys);
        if (!s.ok()) {
          VerificationAbort(shared, "Checkpoint gave inconsistent state", s);
        }
      }

      if (FLAGS_acquire_snapshot_one_in > 0 &&
          thread->rand.Uniform(FLAGS_acquire_snapshot_one_in) == 0) {
#ifndef ROCKSDB_LITE
        auto db_impl = reinterpret_cast<DBImpl*>(db_->GetRootDB());
        const bool ww_snapshot = thread->rand.OneIn(10);
        const Snapshot* snapshot =
            ww_snapshot ? db_impl->GetSnapshotForWriteConflictBoundary()
                        : db_->GetSnapshot();
#else
        const Snapshot* snapshot = db_->GetSnapshot();
#endif  // !ROCKSDB_LITE
        ReadOptions ropt;
        ropt.snapshot = snapshot;
        std::string value_at;
        // When taking a snapshot, we also read a key from that snapshot. We
        // will later read the same key before releasing the snapshot and
        // verify that the results are the same.
        auto status_at = db_->Get(ropt, column_family, key, &value_at);
        std::vector<bool>* key_vec = nullptr;

        if (FLAGS_compare_full_db_state_snapshot && (thread->tid == 0)) {
          key_vec = new std::vector<bool>(FLAGS_max_key);
          // When `prefix_extractor` is set, seeking to beginning and scanning
          // across prefixes are only supported with `total_order_seek` set.
          ropt.total_order_seek = true;
          std::unique_ptr<Iterator> iterator(db_->NewIterator(ropt));
          for (iterator->SeekToFirst(); iterator->Valid(); iterator->Next()) {
            uint64_t key_val;
            if (GetIntVal(iterator->key().ToString(), &key_val)) {
              (*key_vec)[key_val] = true;
            }
          }
        }

        ThreadState::SnapshotState snap_state = {
            snapshot, rand_column_family, column_family->GetName(),
            keystr,   status_at,          value_at,
            key_vec};
        thread->snapshot_queue.emplace(
            std::min(FLAGS_ops_per_thread - 1, i + FLAGS_snapshot_hold_ops),
            snap_state);
      }
      while (!thread->snapshot_queue.empty() &&
             i >= thread->snapshot_queue.front().first) {
        auto snap_state = thread->snapshot_queue.front().second;
        assert(snap_state.snapshot);
        // Note: this is unsafe as the cf might be dropped concurrently. But
        // it is ok since unclean cf drop is cunnrently not supported by write
        // prepared transactions.
        Status s =
            AssertSame(db_, column_families_[snap_state.cf_at], snap_state);
        if (!s.ok()) {
          VerificationAbort(shared, "Snapshot gave inconsistent state", s);
        }
        db_->ReleaseSnapshot(snap_state.snapshot);
        delete snap_state.key_vec;
        thread->snapshot_queue.pop();
      }

      int prob_op = thread->rand.Uniform(100);
      // Reset this in case we pick something other than a read op. We don't
      // want to use a stale value when deciding at the beginning of the loop
      // whether to vote to reopen
      if (prob_op >= 0 && prob_op < (int)FLAGS_readpercent) {
        // OPERATION read
        if (FLAGS_use_multiget) {
          // Leave room for one more iteration of the loop with a single key
          // batch. This is to ensure that each thread does exactly the same
          // number of ops
          int multiget_batch_size = static_cast<int>(
              std::min(static_cast<uint64_t>(thread->rand.Uniform(64)),
                       FLAGS_ops_per_thread - i - 1));
          // If its the last iteration, ensure that multiget_batch_size is 1
          multiget_batch_size = std::max(multiget_batch_size, 1);
          rand_keys = GenerateNKeys(thread, multiget_batch_size, i);
          TestMultiGet(thread, read_opts, rand_column_families, rand_keys);
          i += multiget_batch_size - 1;
        } else {
          TestGet(thread, read_opts, rand_column_families, rand_keys);
        }
      } else if ((int)FLAGS_readpercent <= prob_op && prob_op < prefixBound) {
        // OPERATION prefix scan
        // keys are 8 bytes long, prefix size is FLAGS_prefix_size. There are
        // (8 - FLAGS_prefix_size) bytes besides the prefix. So there will
        // be 2 ^ ((8 - FLAGS_prefix_size) * 8) possible keys with the same
        // prefix
        TestPrefixScan(thread, read_opts, rand_column_families, rand_keys);
      } else if (prefixBound <= prob_op && prob_op < writeBound) {
        // OPERATION write
        TestPut(thread, write_opts, read_opts, rand_column_families, rand_keys,
                value, lock);
      } else if (writeBound <= prob_op && prob_op < delBound) {
        // OPERATION delete
        TestDelete(thread, write_opts, rand_column_families, rand_keys, lock);
      } else if (delBound <= prob_op && prob_op < delRangeBound) {
        // OPERATION delete range
        TestDeleteRange(thread, write_opts, rand_column_families, rand_keys,
                        lock);
      } else {
        // OPERATION iterate
        int num_seeks = static_cast<int>(
            std::min(static_cast<uint64_t>(thread->rand.Uniform(4)),
                     FLAGS_ops_per_thread - i - 1));
        rand_keys = GenerateNKeys(thread, num_seeks, i);
        i += num_seeks - 1;
        TestIterate(thread, read_opts, rand_column_families, rand_keys);
      }
      thread->stats.FinishedSingleOp();
#ifndef ROCKSDB_LITE
      uint32_t tid = thread->tid;
      assert(secondaries_.empty() ||
             static_cast<size_t>(tid) < secondaries_.size());
      if (FLAGS_secondary_catch_up_one_in > 0 &&
          thread->rand.Uniform(FLAGS_secondary_catch_up_one_in) == 0) {
        Status s = secondaries_[tid]->TryCatchUpWithPrimary();
        if (!s.ok()) {
          VerificationAbort(shared, "Secondary instance failed to catch up", s);
          break;
        }
      }
#endif
    }
  }
  while (!thread->snapshot_queue.empty()) {
    db_->ReleaseSnapshot(thread->snapshot_queue.front().second.snapshot);
    delete thread->snapshot_queue.front().second.key_vec;
    thread->snapshot_queue.pop();
  }

  thread->stats.Stop();
}

#ifndef ROCKSDB_LITE
// Generated a list of keys that close to boundaries of SST keys.
// If there isn't any SST file in the DB, return empty list.
std::vector<std::string> StressTest::GetWhiteBoxKeys(ThreadState* thread,
                                                     DB* db,
                                                     ColumnFamilyHandle* cfh,
                                                     size_t num_keys) {
  ColumnFamilyMetaData cfmd;
  db->GetColumnFamilyMetaData(cfh, &cfmd);
  std::vector<std::string> boundaries;
  for (const LevelMetaData& lmd : cfmd.levels) {
    for (const SstFileMetaData& sfmd : lmd.files) {
      boundaries.push_back(sfmd.smallestkey);
      boundaries.push_back(sfmd.largestkey);
    }
  }
  if (boundaries.empty()) {
    return {};
  }

  std::vector<std::string> ret;
  for (size_t j = 0; j < num_keys; j++) {
    std::string k =
        boundaries[thread->rand.Uniform(static_cast<int>(boundaries.size()))];
    if (thread->rand.OneIn(3)) {
      // Reduce one byte from the string
      for (int i = static_cast<int>(k.length()) - 1; i >= 0; i--) {
        uint8_t cur = k[i];
        if (cur > 0) {
          k[i] = static_cast<char>(cur - 1);
          break;
        } else if (i > 0) {
          k[i] = 0xFF;
        }
      }
    } else if (thread->rand.OneIn(2)) {
      // Add one byte to the string
      for (int i = static_cast<int>(k.length()) - 1; i >= 0; i--) {
        uint8_t cur = k[i];
        if (cur < 255) {
          k[i] = static_cast<char>(cur + 1);
          break;
        } else if (i > 0) {
          k[i] = 0x00;
        }
      }
    }
    ret.push_back(k);
  }
  return ret;
}
#endif  // !ROCKSDB_LITE

// Given a key K, this creates an iterator which scans to K and then
// does a random sequence of Next/Prev operations.
Status StressTest::TestIterate(ThreadState* thread,
                               const ReadOptions& read_opts,
                               const std::vector<int>& rand_column_families,
                               const std::vector<int64_t>& rand_keys) {
  Status s;
  const Snapshot* snapshot = db_->GetSnapshot();
  ReadOptions readoptionscopy = read_opts;
  readoptionscopy.snapshot = snapshot;

  if (thread->rand.OneIn(16)) {
    // When prefix extractor is used, it's useful to cover total order seek.
    readoptionscopy.total_order_seek = true;
  }

  std::string upper_bound_str;
  Slice upper_bound;
  if (thread->rand.OneIn(16)) {
    // in 1/16 chance, set a iterator upper bound
    int64_t rand_upper_key = GenerateOneKey(thread, FLAGS_ops_per_thread);
    upper_bound_str = Key(rand_upper_key);
    upper_bound = Slice(upper_bound_str);
    // uppder_bound can be smaller than seek key, but the query itself
    // should not crash either.
    readoptionscopy.iterate_upper_bound = &upper_bound;
  }
  std::string lower_bound_str;
  Slice lower_bound;
  if (thread->rand.OneIn(16)) {
    // in 1/16 chance, enable iterator lower bound
    int64_t rand_lower_key = GenerateOneKey(thread, FLAGS_ops_per_thread);
    lower_bound_str = Key(rand_lower_key);
    lower_bound = Slice(lower_bound_str);
    // uppder_bound can be smaller than seek key, but the query itself
    // should not crash either.
    readoptionscopy.iterate_lower_bound = &lower_bound;
  }

  auto cfh = column_families_[rand_column_families[0]];
  std::unique_ptr<Iterator> iter(db_->NewIterator(readoptionscopy, cfh));

  std::vector<std::string> key_str;
  if (thread->rand.OneIn(16)) {
    // Generate keys close to lower or upper bound of SST files.
    key_str = GetWhiteBoxKeys(thread, db_, cfh, rand_keys.size());
  }
  if (key_str.empty()) {
    // If key string is not geneerated using white block keys,
    // Use randomized key passe in.
    for (int64_t rkey : rand_keys) {
      key_str.push_back(Key(rkey));
    }
  }

  for (const std::string& skey : key_str) {
    Slice key = skey;

    if (readoptionscopy.iterate_upper_bound != nullptr &&
        thread->rand.OneIn(2)) {
      // 1/2 chance, change the upper bound.
      // It is possible that it is changed without first use, but there is no
      // problem with that.
      int64_t rand_upper_key = GenerateOneKey(thread, FLAGS_ops_per_thread);
      upper_bound_str = Key(rand_upper_key);
      upper_bound = Slice(upper_bound_str);
    }

    // Set up an iterator and does the same without bounds and with total
    // order seek and compare the results. This is to identify bugs related
    // to bounds, prefix extractor or reseeking. Sometimes we are comparing
    // iterators with the same set-up, and it doesn't hurt to check them
    // to be equal.
    ReadOptions cmp_ro;
    cmp_ro.snapshot = snapshot;
    cmp_ro.total_order_seek = true;
    ColumnFamilyHandle* cmp_cfh =
        GetControlCfh(thread, rand_column_families[0]);
    std::unique_ptr<Iterator> cmp_iter(db_->NewIterator(cmp_ro, cmp_cfh));
    bool diverged = false;

    LastIterateOp last_op;
    if (thread->rand.OneIn(8)) {
      iter->SeekForPrev(key);
      cmp_iter->SeekForPrev(key);
      last_op = kLastOpSeekForPrev;
    } else {
      iter->Seek(key);
      cmp_iter->Seek(key);
      last_op = kLastOpSeek;
    }
    VerifyIterator(thread, cmp_cfh, readoptionscopy, iter.get(), cmp_iter.get(),
                   last_op, key, &diverged);

    bool no_reverse =
        (FLAGS_memtablerep == "prefix_hash" && !read_opts.total_order_seek &&
         options_.prefix_extractor.get() != nullptr);
    for (uint64_t i = 0; i < FLAGS_num_iterations && iter->Valid(); i++) {
      if (no_reverse || thread->rand.OneIn(2)) {
        iter->Next();
        if (!diverged) {
          assert(cmp_iter->Valid());
          cmp_iter->Next();
        }
      } else {
        iter->Prev();
        if (!diverged) {
          assert(cmp_iter->Valid());
          cmp_iter->Prev();
        }
      }
      last_op = kLastOpNextOrPrev;
      VerifyIterator(thread, cmp_cfh, readoptionscopy, iter.get(),
                     cmp_iter.get(), last_op, key, &diverged);
    }

    if (s.ok()) {
      thread->stats.AddIterations(1);
    } else {
      thread->stats.AddErrors(1);
      break;
    }
  }

  db_->ReleaseSnapshot(snapshot);

  return s;
}

// Compare the two iterator, iter and cmp_iter are in the same position,
// unless iter might be made invalidate or undefined because of
// upper or lower bounds, or prefix extractor.
// Will flag failure if the verification fails.
// diverged = true if the two iterator is already diverged.
// True if verification passed, false if not.
void StressTest::VerifyIterator(ThreadState* thread,
                                ColumnFamilyHandle* cmp_cfh,
                                const ReadOptions& ro, Iterator* iter,
                                Iterator* cmp_iter, LastIterateOp op,
                                const Slice& seek_key, bool* diverged) {
  if (*diverged) {
    return;
  }

  if (op == kLastOpSeek && ro.iterate_lower_bound != nullptr &&
      (options_.comparator->Compare(*ro.iterate_lower_bound, seek_key) >= 0 ||
       (ro.iterate_upper_bound != nullptr &&
        options_.comparator->Compare(*ro.iterate_lower_bound,
                                     *ro.iterate_upper_bound) >= 0))) {
    // Lower bound behavior is not well defined if it is larger than
    // seek key or upper bound. Disable the check for now.
    *diverged = true;
    return;
  }

  if (op == kLastOpSeekForPrev && ro.iterate_upper_bound != nullptr &&
      (options_.comparator->Compare(*ro.iterate_upper_bound, seek_key) <= 0 ||
       (ro.iterate_lower_bound != nullptr &&
        options_.comparator->Compare(*ro.iterate_lower_bound,
                                     *ro.iterate_upper_bound) >= 0))) {
    // Uppder bound behavior is not well defined if it is smaller than
    // seek key or lower bound. Disable the check for now.
    *diverged = true;
    return;
  }

  if (iter->Valid() && !cmp_iter->Valid()) {
    fprintf(stderr,
            "Control interator is invalid but iterator has key %s seek key "
            "%s\n",
            iter->key().ToString(true).c_str(),
            seek_key.ToString(true).c_str());
    if (ro.iterate_upper_bound != nullptr) {
      fprintf(stderr, "upper bound %s\n",
              ro.iterate_upper_bound->ToString(true).c_str());
    }
    if (ro.iterate_lower_bound != nullptr) {
      fprintf(stderr, "lower bound %s\n",
              ro.iterate_lower_bound->ToString(true).c_str());
    }

    *diverged = true;
  } else if (cmp_iter->Valid()) {
    // Iterator is not valid. It can be legimate if it has already been
    // out of upper or lower bound, or filtered out by prefix iterator.
    const Slice& total_order_key = cmp_iter->key();
    const SliceTransform* pe =
        ro.total_order_seek ? nullptr : options_.prefix_extractor.get();
    const Comparator* cmp = options_.comparator;

    if (pe != nullptr) {
      if (!pe->InDomain(seek_key)) {
        // Prefix seek a non-in-domain key is undefined. Skip checking for
        // this scenario.
        *diverged = true;
        return;
      }

      if (!pe->InDomain(total_order_key) ||
          pe->Transform(total_order_key) != pe->Transform(seek_key)) {
        // If the prefix is exhausted, the only thing needs to check
        // is the iterator isn't return a position in prefix.
        // Either way, checking can stop from here.
        *diverged = true;
        if (!iter->Valid() || !pe->InDomain(iter->key()) ||
            pe->Transform(iter->key()) != pe->Transform(seek_key)) {
          return;
        }
        fprintf(stderr,
                "Iterator stays in prefix bug contol doesn't"
                " seek key %s iterator key %s control iterator key %s\n",
                seek_key.ToString(true).c_str(),
                iter->key().ToString(true).c_str(),
                cmp_iter->key().ToString(true).c_str());
      }
    }
    // Check upper or lower bounds.
    if (!*diverged) {
      if ((iter->Valid() && iter->key() != cmp_iter->key()) ||
          (!iter->Valid() &&
           (ro.iterate_upper_bound == nullptr ||
            cmp->Compare(total_order_key, *ro.iterate_upper_bound) < 0) &&
           (ro.iterate_lower_bound == nullptr ||
            cmp->Compare(total_order_key, *ro.iterate_lower_bound) > 0))) {
        fprintf(stderr,
                "Iterator diverged from control iterator which"
                " has value %s seek key %s\n",
                total_order_key.ToString(true).c_str(),
                seek_key.ToString(true).c_str());
        if (iter->Valid()) {
          fprintf(stderr, "iterator has value %s\n",
                  iter->key().ToString(true).c_str());
        } else {
          fprintf(stderr, "iterator is not valid\n");
        }
        if (ro.iterate_upper_bound != nullptr) {
          fprintf(stderr, "upper bound %s\n",
                  ro.iterate_upper_bound->ToString(true).c_str());
        }
        if (ro.iterate_lower_bound != nullptr) {
          fprintf(stderr, "lower bound %s\n",
                  ro.iterate_lower_bound->ToString(true).c_str());
        }
        *diverged = true;
      }
    }
  }
  if (*diverged) {
    fprintf(stderr, "Control CF %s\n", cmp_cfh->GetName().c_str());
    thread->stats.AddErrors(1);
    // Fail fast to preserve the DB state.
    thread->shared->SetVerificationFailure();
  }
}

#ifdef ROCKSDB_LITE
Status StressTest::TestBackupRestore(
    ThreadState* /* thread */,
    const std::vector<int>& /* rand_column_families */,
    const std::vector<int64_t>& /* rand_keys */) {
  assert(false);
  fprintf(stderr,
          "RocksDB lite does not support "
          "TestBackupRestore\n");
  std::terminate();
}

Status StressTest::TestCheckpoint(
    ThreadState* /* thread */,
    const std::vector<int>& /* rand_column_families */,
    const std::vector<int64_t>& /* rand_keys */) {
  assert(false);
  fprintf(stderr,
          "RocksDB lite does not support "
          "TestCheckpoint\n");
  std::terminate();
}
#else   // ROCKSDB_LITE
Status StressTest::TestBackupRestore(
    ThreadState* thread, const std::vector<int>& rand_column_families,
    const std::vector<int64_t>& rand_keys) {
  // Note the column families chosen by `rand_column_families` cannot be
  // dropped while the locks for `rand_keys` are held. So we should not have
  // to worry about accessing those column families throughout this function.
  assert(rand_column_families.size() == rand_keys.size());
  std::string backup_dir = FLAGS_db + "/.backup" + ToString(thread->tid);
  std::string restore_dir = FLAGS_db + "/.restore" + ToString(thread->tid);
  BackupableDBOptions backup_opts(backup_dir);
  BackupEngine* backup_engine = nullptr;
  Status s = BackupEngine::Open(FLAGS_env, backup_opts, &backup_engine);
  if (s.ok()) {
    s = backup_engine->CreateNewBackup(db_);
  }
  if (s.ok()) {
    delete backup_engine;
    backup_engine = nullptr;
    s = BackupEngine::Open(FLAGS_env, backup_opts, &backup_engine);
  }
  if (s.ok()) {
    s = backup_engine->RestoreDBFromLatestBackup(restore_dir /* db_dir */,
                                                 restore_dir /* wal_dir */);
  }
  if (s.ok()) {
    s = backup_engine->PurgeOldBackups(0 /* num_backups_to_keep */);
  }
  DB* restored_db = nullptr;
  std::vector<ColumnFamilyHandle*> restored_cf_handles;
  if (s.ok()) {
    Options restore_options(options_);
    restore_options.listeners.clear();
    std::vector<ColumnFamilyDescriptor> cf_descriptors;
    // TODO(ajkr): `column_family_names_` is not safe to access here when
    // `clear_column_family_one_in != 0`. But we can't easily switch to
    // `ListColumnFamilies` to get names because it won't necessarily give
    // the same order as `column_family_names_`.
    assert(FLAGS_clear_column_family_one_in == 0);
    for (auto name : column_family_names_) {
      cf_descriptors.emplace_back(name, ColumnFamilyOptions(restore_options));
    }
    s = DB::Open(DBOptions(restore_options), restore_dir, cf_descriptors,
                 &restored_cf_handles, &restored_db);
  }
  // for simplicity, currently only verifies existence/non-existence of a few
  // keys
  for (size_t i = 0; s.ok() && i < rand_column_families.size(); ++i) {
    std::string key_str = Key(rand_keys[i]);
    Slice key = key_str;
    std::string restored_value;
    Status get_status = restored_db->Get(
        ReadOptions(), restored_cf_handles[rand_column_families[i]], key,
        &restored_value);
    bool exists = thread->shared->Exists(rand_column_families[i], rand_keys[i]);
    if (get_status.ok()) {
      if (!exists) {
        s = Status::Corruption("key exists in restore but not in original db");
      }
    } else if (get_status.IsNotFound()) {
      if (exists) {
        s = Status::Corruption("key exists in original db but not in restore");
      }
    } else {
      s = get_status;
    }
  }
  if (backup_engine != nullptr) {
    delete backup_engine;
    backup_engine = nullptr;
  }
  if (restored_db != nullptr) {
    for (auto* cf_handle : restored_cf_handles) {
      restored_db->DestroyColumnFamilyHandle(cf_handle);
    }
    delete restored_db;
    restored_db = nullptr;
  }
  if (!s.ok()) {
    printf("A backup/restore operation failed with: %s\n",
           s.ToString().c_str());
  }
  return s;
}

Status StressTest::TestCheckpoint(ThreadState* thread,
                                  const std::vector<int>& rand_column_families,
                                  const std::vector<int64_t>& rand_keys) {
  // Note the column families chosen by `rand_column_families` cannot be
  // dropped while the locks for `rand_keys` are held. So we should not have
  // to worry about accessing those column families throughout this function.
  assert(rand_column_families.size() == rand_keys.size());
  std::string checkpoint_dir =
      FLAGS_db + "/.checkpoint" + ToString(thread->tid);
  Options tmp_opts(options_);
  tmp_opts.listeners.clear();
  DestroyDB(checkpoint_dir, tmp_opts);
  Checkpoint* checkpoint = nullptr;
  Status s = Checkpoint::Create(db_, &checkpoint);
  if (s.ok()) {
    s = checkpoint->CreateCheckpoint(checkpoint_dir);
  }
  std::vector<ColumnFamilyHandle*> cf_handles;
  DB* checkpoint_db = nullptr;
  if (s.ok()) {
    delete checkpoint;
    checkpoint = nullptr;
    Options options(options_);
    options.listeners.clear();
    std::vector<ColumnFamilyDescriptor> cf_descs;
    // TODO(ajkr): `column_family_names_` is not safe to access here when
    // `clear_column_family_one_in != 0`. But we can't easily switch to
    // `ListColumnFamilies` to get names because it won't necessarily give
    // the same order as `column_family_names_`.
    if (FLAGS_clear_column_family_one_in == 0) {
      for (const auto& name : column_family_names_) {
        cf_descs.emplace_back(name, ColumnFamilyOptions(options));
      }
      s = DB::OpenForReadOnly(DBOptions(options), checkpoint_dir, cf_descs,
                              &cf_handles, &checkpoint_db);
    }
  }
  if (checkpoint_db != nullptr) {
    for (size_t i = 0; s.ok() && i < rand_column_families.size(); ++i) {
      std::string key_str = Key(rand_keys[i]);
      Slice key = key_str;
      std::string value;
      Status get_status = checkpoint_db->Get(
          ReadOptions(), cf_handles[rand_column_families[i]], key, &value);
      bool exists =
          thread->shared->Exists(rand_column_families[i], rand_keys[i]);
      if (get_status.ok()) {
        if (!exists) {
          s = Status::Corruption(
              "key exists in checkpoint but not in original db");
        }
      } else if (get_status.IsNotFound()) {
        if (exists) {
          s = Status::Corruption(
              "key exists in original db but not in checkpoint");
        }
      } else {
        s = get_status;
      }
    }
    for (auto cfh : cf_handles) {
      delete cfh;
    }
    cf_handles.clear();
    delete checkpoint_db;
    checkpoint_db = nullptr;
  }
  DestroyDB(checkpoint_dir, tmp_opts);
  if (!s.ok()) {
    fprintf(stderr, "A checkpoint operation failed with: %s\n",
            s.ToString().c_str());
  }
  return s;
}
#endif  // ROCKSDB_LITE

void StressTest::PrintEnv() const {
  fprintf(stdout, "RocksDB version           : %d.%d\n", kMajorVersion,
          kMinorVersion);
  fprintf(stdout, "Format version            : %d\n", FLAGS_format_version);
  fprintf(stdout, "TransactionDB             : %s\n",
          FLAGS_use_txn ? "true" : "false");
  fprintf(stdout, "Read only mode            : %s\n",
          FLAGS_read_only ? "true" : "false");
  fprintf(stdout, "Atomic flush              : %s\n",
          FLAGS_atomic_flush ? "true" : "false");
  fprintf(stdout, "Column families           : %d\n", FLAGS_column_families);
  if (!FLAGS_test_batches_snapshots) {
    fprintf(stdout, "Clear CFs one in          : %d\n",
            FLAGS_clear_column_family_one_in);
  }
  fprintf(stdout, "Number of threads         : %d\n", FLAGS_threads);
  fprintf(stdout, "Ops per thread            : %lu\n",
          (unsigned long)FLAGS_ops_per_thread);
  std::string ttl_state("unused");
  if (FLAGS_ttl > 0) {
    ttl_state = NumberToString(FLAGS_ttl);
  }
  fprintf(stdout, "Time to live(sec)         : %s\n", ttl_state.c_str());
  fprintf(stdout, "Read percentage           : %d%%\n", FLAGS_readpercent);
  fprintf(stdout, "Prefix percentage         : %d%%\n", FLAGS_prefixpercent);
  fprintf(stdout, "Write percentage          : %d%%\n", FLAGS_writepercent);
  fprintf(stdout, "Delete percentage         : %d%%\n", FLAGS_delpercent);
  fprintf(stdout, "Delete range percentage   : %d%%\n", FLAGS_delrangepercent);
  fprintf(stdout, "No overwrite percentage   : %d%%\n",
          FLAGS_nooverwritepercent);
  fprintf(stdout, "Iterate percentage        : %d%%\n", FLAGS_iterpercent);
  fprintf(stdout, "DB-write-buffer-size      : %" PRIu64 "\n",
          FLAGS_db_write_buffer_size);
  fprintf(stdout, "Write-buffer-size         : %d\n", FLAGS_write_buffer_size);
  fprintf(stdout, "Iterations                : %lu\n",
          (unsigned long)FLAGS_num_iterations);
  fprintf(stdout, "Max key                   : %lu\n",
          (unsigned long)FLAGS_max_key);
  fprintf(stdout, "Ratio #ops/#keys          : %f\n",
          (1.0 * FLAGS_ops_per_thread * FLAGS_threads) / FLAGS_max_key);
  fprintf(stdout, "Num times DB reopens      : %d\n", FLAGS_reopen);
  fprintf(stdout, "Batches/snapshots         : %d\n",
          FLAGS_test_batches_snapshots);
  fprintf(stdout, "Do update in place        : %d\n", FLAGS_in_place_update);
  fprintf(stdout, "Num keys per lock         : %d\n",
          1 << FLAGS_log2_keys_per_lock);
  std::string compression = CompressionTypeToString(FLAGS_compression_type_e);
  fprintf(stdout, "Compression               : %s\n", compression.c_str());
  std::string checksum = ChecksumTypeToString(FLAGS_checksum_type_e);
  fprintf(stdout, "Checksum type             : %s\n", checksum.c_str());
  fprintf(stdout, "Max subcompactions        : %" PRIu64 "\n",
          FLAGS_subcompactions);
  fprintf(stdout, "Use MultiGet              : %s\n",
          FLAGS_use_multiget ? "true" : "false");

  const char* memtablerep = "";
  switch (FLAGS_rep_factory) {
    case kSkipList:
      memtablerep = "skip_list";
      break;
    case kHashSkipList:
      memtablerep = "prefix_hash";
      break;
    case kVectorRep:
      memtablerep = "vector";
      break;
  }

  fprintf(stdout, "Memtablerep               : %s\n", memtablerep);

  fprintf(stdout, "Test kill odd             : %d\n", rocksdb_kill_odds);
  if (!rocksdb_kill_prefix_blacklist.empty()) {
    fprintf(stdout, "Skipping kill points prefixes:\n");
    for (auto& p : rocksdb_kill_prefix_blacklist) {
      fprintf(stdout, "  %s\n", p.c_str());
    }
  }
  fprintf(stdout, "Periodic Compaction Secs  : %" PRIu64 "\n",
          FLAGS_periodic_compaction_seconds);
  fprintf(stdout, "Compaction TTL            : %" PRIu64 "\n",
          FLAGS_compaction_ttl);

  fprintf(stdout, "------------------------------------------------\n");
}

void StressTest::Open() {
  assert(db_ == nullptr);
#ifndef ROCKSDB_LITE
  assert(txn_db_ == nullptr);
#endif
  if (FLAGS_options_file.empty()) {
    BlockBasedTableOptions block_based_options;
    block_based_options.block_cache = cache_;
    block_based_options.cache_index_and_filter_blocks =
        FLAGS_cache_index_and_filter_blocks;
    block_based_options.block_cache_compressed = compressed_cache_;
    block_based_options.checksum = FLAGS_checksum_type_e;
    block_based_options.block_size = FLAGS_block_size;
    block_based_options.format_version =
        static_cast<uint32_t>(FLAGS_format_version);
    block_based_options.index_block_restart_interval =
        static_cast<int32_t>(FLAGS_index_block_restart_interval);
    block_based_options.filter_policy = filter_policy_;
    block_based_options.partition_filters = FLAGS_partition_filters;
    block_based_options.index_type =
        static_cast<BlockBasedTableOptions::IndexType>(FLAGS_index_type);
    options_.table_factory.reset(
        NewBlockBasedTableFactory(block_based_options));
    options_.db_write_buffer_size = FLAGS_db_write_buffer_size;
    options_.write_buffer_size = FLAGS_write_buffer_size;
    options_.max_write_buffer_number = FLAGS_max_write_buffer_number;
    options_.min_write_buffer_number_to_merge =
        FLAGS_min_write_buffer_number_to_merge;
    options_.max_write_buffer_number_to_maintain =
        FLAGS_max_write_buffer_number_to_maintain;
    options_.max_write_buffer_size_to_maintain =
        FLAGS_max_write_buffer_size_to_maintain;
    options_.memtable_prefix_bloom_size_ratio =
        FLAGS_memtable_prefix_bloom_size_ratio;
    options_.memtable_whole_key_filtering = FLAGS_memtable_whole_key_filtering;
    options_.max_background_compactions = FLAGS_max_background_compactions;
    options_.max_background_flushes = FLAGS_max_background_flushes;
    options_.compaction_style =
        static_cast<rocksdb::CompactionStyle>(FLAGS_compaction_style);
    if (FLAGS_prefix_size >= 0) {
      options_.prefix_extractor.reset(
          NewFixedPrefixTransform(FLAGS_prefix_size));
    }
    options_.max_open_files = FLAGS_open_files;
    options_.statistics = dbstats;
    options_.env = FLAGS_env;
    options_.use_fsync = FLAGS_use_fsync;
    options_.compaction_readahead_size = FLAGS_compaction_readahead_size;
    options_.allow_mmap_reads = FLAGS_mmap_read;
    options_.allow_mmap_writes = FLAGS_mmap_write;
    options_.use_direct_reads = FLAGS_use_direct_reads;
    options_.use_direct_io_for_flush_and_compaction =
        FLAGS_use_direct_io_for_flush_and_compaction;
    options_.recycle_log_file_num =
        static_cast<size_t>(FLAGS_recycle_log_file_num);
    options_.target_file_size_base = FLAGS_target_file_size_base;
    options_.target_file_size_multiplier = FLAGS_target_file_size_multiplier;
    options_.max_bytes_for_level_base = FLAGS_max_bytes_for_level_base;
    options_.max_bytes_for_level_multiplier =
        FLAGS_max_bytes_for_level_multiplier;
    options_.level0_stop_writes_trigger = FLAGS_level0_stop_writes_trigger;
    options_.level0_slowdown_writes_trigger =
        FLAGS_level0_slowdown_writes_trigger;
    options_.level0_file_num_compaction_trigger =
        FLAGS_level0_file_num_compaction_trigger;
    options_.compression = FLAGS_compression_type_e;
    options_.compression_opts.max_dict_bytes = FLAGS_compression_max_dict_bytes;
    options_.compression_opts.zstd_max_train_bytes =
        FLAGS_compression_zstd_max_train_bytes;
    options_.create_if_missing = true;
    options_.max_manifest_file_size = FLAGS_max_manifest_file_size;
    options_.inplace_update_support = FLAGS_in_place_update;
    options_.max_subcompactions = static_cast<uint32_t>(FLAGS_subcompactions);
    options_.allow_concurrent_memtable_write =
        FLAGS_allow_concurrent_memtable_write;
    options_.periodic_compaction_seconds = FLAGS_periodic_compaction_seconds;
    options_.ttl = FLAGS_compaction_ttl;
    options_.enable_pipelined_write = FLAGS_enable_pipelined_write;
    options_.enable_write_thread_adaptive_yield =
        FLAGS_enable_write_thread_adaptive_yield;
    options_.compaction_options_universal.size_ratio =
        FLAGS_universal_size_ratio;
    options_.compaction_options_universal.min_merge_width =
        FLAGS_universal_min_merge_width;
    options_.compaction_options_universal.max_merge_width =
        FLAGS_universal_max_merge_width;
    options_.compaction_options_universal.max_size_amplification_percent =
        FLAGS_universal_max_size_amplification_percent;
    options_.atomic_flush = FLAGS_atomic_flush;
  } else {
#ifdef ROCKSDB_LITE
    fprintf(stderr, "--options_file not supported in lite mode\n");
    exit(1);
#else
    DBOptions db_options;
    std::vector<ColumnFamilyDescriptor> cf_descriptors;
    Status s = LoadOptionsFromFile(FLAGS_options_file, FLAGS_env, &db_options,
                                   &cf_descriptors);
    db_options.env = FLAGS_env;
    if (!s.ok()) {
      fprintf(stderr, "Unable to load options file %s --- %s\n",
              FLAGS_options_file.c_str(), s.ToString().c_str());
      exit(1);
    }
    options_ = Options(db_options, cf_descriptors[0].options);
#endif  // ROCKSDB_LITE
  }

  if (FLAGS_rate_limiter_bytes_per_sec > 0) {
    options_.rate_limiter.reset(NewGenericRateLimiter(
        FLAGS_rate_limiter_bytes_per_sec, 1000 /* refill_period_us */,
        10 /* fairness */,
        FLAGS_rate_limit_bg_reads ? RateLimiter::Mode::kReadsOnly
                                  : RateLimiter::Mode::kWritesOnly));
    if (FLAGS_rate_limit_bg_reads) {
      options_.new_table_reader_for_compaction_inputs = true;
    }
  }

  if (FLAGS_prefix_size == 0 && FLAGS_rep_factory == kHashSkipList) {
    fprintf(stderr,
            "prefeix_size cannot be zero if memtablerep == prefix_hash\n");
    exit(1);
  }
  if (FLAGS_prefix_size != 0 && FLAGS_rep_factory != kHashSkipList) {
    fprintf(stderr,
            "WARNING: prefix_size is non-zero but "
            "memtablerep != prefix_hash\n");
  }
  switch (FLAGS_rep_factory) {
    case kSkipList:
      // no need to do anything
      break;
#ifndef ROCKSDB_LITE
    case kHashSkipList:
      options_.memtable_factory.reset(NewHashSkipListRepFactory(10000));
      break;
    case kVectorRep:
      options_.memtable_factory.reset(new VectorRepFactory());
      break;
#else
    default:
      fprintf(stderr,
              "RocksdbLite only supports skip list mem table. Skip "
              "--rep_factory\n");
#endif  // ROCKSDB_LITE
  }

  if (FLAGS_use_full_merge_v1) {
    options_.merge_operator = MergeOperators::CreateDeprecatedPutOperator();
  } else {
    options_.merge_operator = MergeOperators::CreatePutOperator();
  }

  fprintf(stdout, "DB path: [%s]\n", FLAGS_db.c_str());

  Status s;
  if (FLAGS_ttl == -1) {
    std::vector<std::string> existing_column_families;
    s = DB::ListColumnFamilies(DBOptions(options_), FLAGS_db,
                               &existing_column_families);  // ignore errors
    if (!s.ok()) {
      // DB doesn't exist
      assert(existing_column_families.empty());
      assert(column_family_names_.empty());
      column_family_names_.push_back(kDefaultColumnFamilyName);
    } else if (column_family_names_.empty()) {
      // this is the first call to the function Open()
      column_family_names_ = existing_column_families;
    } else {
      // this is a reopen. just assert that existing column_family_names are
      // equivalent to what we remember
      auto sorted_cfn = column_family_names_;
      std::sort(sorted_cfn.begin(), sorted_cfn.end());
      std::sort(existing_column_families.begin(),
                existing_column_families.end());
      if (sorted_cfn != existing_column_families) {
        fprintf(stderr, "Expected column families differ from the existing:\n");
        printf("Expected: {");
        for (auto cf : sorted_cfn) {
          printf("%s ", cf.c_str());
        }
        printf("}\n");
        printf("Existing: {");
        for (auto cf : existing_column_families) {
          printf("%s ", cf.c_str());
        }
        printf("}\n");
      }
      assert(sorted_cfn == existing_column_families);
    }
    std::vector<ColumnFamilyDescriptor> cf_descriptors;
    for (auto name : column_family_names_) {
      if (name != kDefaultColumnFamilyName) {
        new_column_family_name_ =
            std::max(new_column_family_name_.load(), std::stoi(name) + 1);
      }
      cf_descriptors.emplace_back(name, ColumnFamilyOptions(options_));
    }
    while (cf_descriptors.size() < (size_t)FLAGS_column_families) {
      std::string name = ToString(new_column_family_name_.load());
      new_column_family_name_++;
      cf_descriptors.emplace_back(name, ColumnFamilyOptions(options_));
      column_family_names_.push_back(name);
    }
    options_.listeners.clear();
    options_.listeners.emplace_back(
        new DbStressListener(FLAGS_db, options_.db_paths, cf_descriptors));
    options_.create_missing_column_families = true;
    if (!FLAGS_use_txn) {
      if (db_preload_finished_.load() && FLAGS_read_only) {
        s = DB::OpenForReadOnly(DBOptions(options_), FLAGS_db, cf_descriptors,
                                &column_families_, &db_);
      } else {
        s = DB::Open(DBOptions(options_), FLAGS_db, cf_descriptors,
                     &column_families_, &db_);
      }
    } else {
#ifndef ROCKSDB_LITE
      TransactionDBOptions txn_db_options;
      // For the moment it is sufficient to test WRITE_PREPARED policy
      txn_db_options.write_policy = TxnDBWritePolicy::WRITE_PREPARED;
      s = TransactionDB::Open(options_, txn_db_options, FLAGS_db,
                              cf_descriptors, &column_families_, &txn_db_);
      db_ = txn_db_;
      // after a crash, rollback to commit recovered transactions
      std::vector<Transaction*> trans;
      txn_db_->GetAllPreparedTransactions(&trans);
      Random rand(static_cast<uint32_t>(FLAGS_seed));
      for (auto txn : trans) {
        if (rand.OneIn(2)) {
          s = txn->Commit();
          assert(s.ok());
        } else {
          s = txn->Rollback();
          assert(s.ok());
        }
        delete txn;
      }
      trans.clear();
      txn_db_->GetAllPreparedTransactions(&trans);
      assert(trans.size() == 0);
#endif
    }
    assert(!s.ok() || column_families_.size() ==
                          static_cast<size_t>(FLAGS_column_families));

    if (FLAGS_enable_secondary) {
#ifndef ROCKSDB_LITE
      secondaries_.resize(FLAGS_threads);
      std::fill(secondaries_.begin(), secondaries_.end(), nullptr);
      secondary_cfh_lists_.clear();
      secondary_cfh_lists_.resize(FLAGS_threads);
      Options tmp_opts;
      tmp_opts.max_open_files = FLAGS_open_files;
      tmp_opts.statistics = dbstats_secondaries;
      tmp_opts.env = FLAGS_env;
      for (size_t i = 0; i != static_cast<size_t>(FLAGS_threads); ++i) {
        const std::string secondary_path =
            FLAGS_secondaries_base + "/" + std::to_string(i);
        s = DB::OpenAsSecondary(tmp_opts, FLAGS_db, secondary_path,
                                cf_descriptors, &secondary_cfh_lists_[i],
                                &secondaries_[i]);
        if (!s.ok()) {
          break;
        }
      }
#else
      fprintf(stderr, "Secondary is not supported in RocksDBLite\n");
      exit(1);
#endif
    }
  } else {
#ifndef ROCKSDB_LITE
    DBWithTTL* db_with_ttl;
    s = DBWithTTL::Open(options_, FLAGS_db, &db_with_ttl, FLAGS_ttl);
    db_ = db_with_ttl;
    if (FLAGS_enable_secondary) {
      secondaries_.resize(FLAGS_threads);
      std::fill(secondaries_.begin(), secondaries_.end(), nullptr);
      Options tmp_opts;
      tmp_opts.env = options_.env;
      tmp_opts.max_open_files = FLAGS_open_files;
      for (size_t i = 0; i != static_cast<size_t>(FLAGS_threads); ++i) {
        const std::string secondary_path =
            FLAGS_secondaries_base + "/" + std::to_string(i);
        s = DB::OpenAsSecondary(tmp_opts, FLAGS_db, secondary_path,
                                &secondaries_[i]);
        if (!s.ok()) {
          break;
        }
      }
    }
#else
    fprintf(stderr, "TTL is not supported in RocksDBLite\n");
    exit(1);
#endif
  }
  if (!s.ok()) {
    fprintf(stderr, "open error: %s\n", s.ToString().c_str());
    exit(1);
  }
}

void StressTest::Reopen() {
  for (auto cf : column_families_) {
    delete cf;
  }
  column_families_.clear();
  delete db_;
  db_ = nullptr;
#ifndef ROCKSDB_LITE
  txn_db_ = nullptr;
#endif

  assert(secondaries_.size() == secondary_cfh_lists_.size());
  size_t n = secondaries_.size();
  for (size_t i = 0; i != n; ++i) {
    for (auto* cf : secondary_cfh_lists_[i]) {
      delete cf;
    }
    secondary_cfh_lists_[i].clear();
    delete secondaries_[i];
  }
  secondaries_.clear();

  num_times_reopened_++;
  auto now = FLAGS_env->NowMicros();
  fprintf(stdout, "%s Reopening database for the %dth time\n",
          FLAGS_env->TimeToString(now / 1000000).c_str(), num_times_reopened_);
  Open();
}
}  // namespace rocksdb
#endif  // GFLAGS

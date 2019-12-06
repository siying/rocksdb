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
void ThreadBody(void* v) {
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

bool RunStressTest(StressTest* stress) {
  stress->InitDb();

  SharedState shared(FLAGS_env, stress);
  if (FLAGS_read_only) {
    stress->InitReadonlyDb(&shared);
  }

  uint32_t n = shared.GetNumThreads();

  uint64_t now = FLAGS_env->NowMicros();
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
  stress->PrintStatistics();

  if (FLAGS_compaction_thread_pool_adjust_interval > 0) {
    MutexLock l(shared.GetMutex());
    shared.SetShouldStopBgThread();
    while (!shared.BgThreadFinished()) {
      shared.GetCondVar()->Wait();
    }
  }

  if (!stress->VerifySecondaries()) {
    return false;
  }

  if (shared.HasVerificationFailedYet()) {
    printf("Verification failed :(\n");
    return false;
  }
  return true;
}
}  // namespace rocksdb
#endif  // GFLAGS

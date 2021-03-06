// Copyright (c) 2016 Nuxi, https://nuxi.nl/
//
// This file is distributed under a 2-clause BSD license.
// See the LICENSE file for details.

#ifndef LOCKING_H
#define LOCKING_H

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <time.h>

#ifndef __has_extension
#define __has_extension(x) 0
#endif

#if __has_extension(c_thread_safety_attributes)
#define LOCK_ANNOTATE(x) __attribute__((x))
#else
#define LOCK_ANNOTATE(x)
#endif

// Lock annotation macros.

#define LOCKABLE LOCK_ANNOTATE(lockable)

#define LOCKS_EXCLUSIVE(...) LOCK_ANNOTATE(exclusive_lock_function(__VA_ARGS__))
#define LOCKS_SHARED(...) LOCK_ANNOTATE(shared_lock_function(__VA_ARGS__))

#define TRYLOCKS_EXCLUSIVE(...) \
  LOCK_ANNOTATE(exclusive_trylock_function(__VA_ARGS__))
#define TRYLOCKS_SHARED(...) LOCK_ANNOTATE(shared_trylock_function(__VA_ARGS__))

#define UNLOCKS(...) LOCK_ANNOTATE(unlock_function(__VA_ARGS__))

#define REQUIRES_EXCLUSIVE(...) \
  LOCK_ANNOTATE(exclusive_locks_required(__VA_ARGS__))
#define REQUIRES_SHARED(...) LOCK_ANNOTATE(shared_locks_required(__VA_ARGS__))
#define REQUIRES_UNLOCKED(...) LOCK_ANNOTATE(locks_excluded(__VA_ARGS__))

#define NO_LOCK_ANALYSIS LOCK_ANNOTATE(no_thread_safety_analysis)

// Mutex that uses the lock annotations.

struct LOCKABLE mutex {
  pthread_mutex_t object;
};

#define MUTEX_INITIALIZER \
  { PTHREAD_MUTEX_INITIALIZER }

static inline void mutex_init(struct mutex *lock) REQUIRES_UNLOCKED(*lock) {
  pthread_mutex_init(&lock->object, NULL);
}

static inline void mutex_destroy(struct mutex *lock) REQUIRES_UNLOCKED(*lock) {
  pthread_mutex_destroy(&lock->object);
}

static inline void mutex_lock(struct mutex *lock)
    LOCKS_EXCLUSIVE(*lock) NO_LOCK_ANALYSIS {
  pthread_mutex_lock(&lock->object);
}

static inline void mutex_unlock(struct mutex *lock)
    UNLOCKS(*lock) NO_LOCK_ANALYSIS {
  pthread_mutex_unlock(&lock->object);
}

// Read-write lock that uses the lock annotations.

struct LOCKABLE rwlock {
  pthread_rwlock_t object;
};

static inline void rwlock_init(struct rwlock *lock) REQUIRES_UNLOCKED(*lock) {
  pthread_rwlock_init(&lock->object, NULL);
}

static inline void rwlock_rdlock(struct rwlock *lock)
    LOCKS_SHARED(*lock) NO_LOCK_ANALYSIS {
  pthread_rwlock_rdlock(&lock->object);
}

static inline void rwlock_wrlock(struct rwlock *lock)
    LOCKS_EXCLUSIVE(*lock) NO_LOCK_ANALYSIS {
  pthread_rwlock_wrlock(&lock->object);
}

static inline void rwlock_unlock(struct rwlock *lock)
    UNLOCKS(*lock) NO_LOCK_ANALYSIS {
  pthread_rwlock_unlock(&lock->object);
}

// Condition variable that uses the lock annotations.

struct LOCKABLE cond {
  pthread_cond_t object;
};

#ifdef CLOCK_MONOTONIC
#define HAS_COND_INIT_MONOTONIC 1
#else
#define HAS_COND_INIT_MONOTONIC 0
#endif

#if HAS_COND_INIT_MONOTONIC
static inline void cond_init_monotonic(struct cond *cond) {
  pthread_condattr_t attr;
  pthread_condattr_init(&attr);
  pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
  pthread_cond_init(&cond->object, &attr);
  pthread_condattr_destroy(&attr);
}
#endif

static inline void cond_init_realtime(struct cond *cond) {
  pthread_cond_init(&cond->object, NULL);
}

static inline void cond_destroy(struct cond *cond) {
  pthread_cond_destroy(&cond->object);
}

static inline void cond_signal(struct cond *cond) {
  pthread_cond_signal(&cond->object);
}

static inline bool cond_timedwait(struct cond *cond, struct mutex *lock,
                                  uint64_t timeout)
    REQUIRES_EXCLUSIVE(*lock) NO_LOCK_ANALYSIS {
  struct timespec ts = {
      .tv_sec = timeout / 1000000000, .tv_nsec = timeout % 1000000000,
  };
  int ret = pthread_cond_timedwait(&cond->object, &lock->object, &ts);
  assert((ret == 0 || ret == ETIMEDOUT) && "pthread_cond_timedwait() failed");
  return ret == ETIMEDOUT;
}

static inline void cond_wait(struct cond *cond, struct mutex *lock)
    REQUIRES_EXCLUSIVE(*lock) NO_LOCK_ANALYSIS {
  pthread_cond_wait(&cond->object, &lock->object);
}

#endif

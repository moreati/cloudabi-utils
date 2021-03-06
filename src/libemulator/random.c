// Copyright (c) 2016 Nuxi, https://nuxi.nl/
//
// This file is distributed under a 2-clause BSD license.
// See the LICENSE file for details.

#include "config.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "random.h"

#if CONFIG_HAS_ARC4RANDOM_BUF

void random_buf(void *buf, size_t len) {
  arc4random_buf(buf, len);
}

#else

static int urandom;

static void open_urandom(void) {
  urandom = open("/dev/urandom", O_RDONLY);
  if (urandom < 0) {
    fputs("Failed to open /dev/urandom\n", stderr);
    abort();
  }
}

void random_buf(void *buf, size_t len) {
  static pthread_once_t open_once = PTHREAD_ONCE_INIT;
  pthread_once(&open_once, open_urandom);

  if (read(urandom, buf, len) != len) {
    fputs("Short read on /dev/urandom\n", stderr);
    abort();
  }
}

#endif

// Calculates a random number within the range [0, upper - 1] without
// any modulo bias.
//
// The function below repeatedly obtains a random number from
// arc4random() until it lies within the range [2^k % upper, 2^k). As
// this range has length k * upper, we can safely obtain a number
// without any modulo bias.
uintmax_t random_uniform(uintmax_t upper) {
  // Compute 2^k % upper
  //      == (2^k - upper) % upper
  //      == -upper % upper.
  uintmax_t lower = -upper % upper;
  for (;;) {
    uintmax_t value;
    random_buf(&value, sizeof(value));
    if (value >= lower)
      return value % upper;
  }
}

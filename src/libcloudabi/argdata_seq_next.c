// Copyright (c) 2016 Nuxi (https://nuxi.nl/) and contributors.
//
// This file is distributed under a 2-clause BSD license.
// See the LICENSE file for details.

#include <argdata.h>
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

#include "argdata_impl.h"

bool argdata_seq_next(argdata_seq_iterator_t *it_, const argdata_t **value) {
  struct cloudabi_argdata_seq_iterator *it =
      (struct cloudabi_argdata_seq_iterator *)it_;
  const argdata_t *ad = it->container;
  if (ad->type == AD_BUFFER) {
    const uint8_t *buf = ad->buffer + it->offset;
    size_t len = ad->length - it->offset;
    if (len == 0)
      return false;
    int error = parse_subfield(&it->value, &buf, &len);
    if (error != 0) {
      it->error = error;
      return false;
    }
    it->offset = buf - ad->buffer;
    *value = &it->value;
    return true;
  } else {
    assert(ad->type == AD_SEQ && "Iterator points to value of the wrong type");
    if (it->offset >= ad->seq.count)
      return false;
    *value = ad->seq.entries[it->offset++];
    return true;
  }
}

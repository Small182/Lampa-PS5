// SPDX-License-Identifier: GPL-3.0-or-later
//
// Pure, platform-independent helpers for the PS5 process spawner. No ptrace,
// no PS5 kernel access: compiled on both host and target and unit-tested on
// the host (tests/ps5).

#include "prosperoweb/spawn/spawn.h"

#include <string.h>

// Fixed install location of our own ELF, next to its data directory.
static const char kDefaultSelfPath[] = "/data/lampa-ps5/prosperoweb.elf";

int ps5_spawn_resolve_self_path(const char* argv0,
                                const char* (*getenv_fn)(const char*),
                                char* out_path, size_t out_len) {
  if (!out_path || out_len == 0) {
    return -1;
  }

  const char* chosen = NULL;

  if (getenv_fn) {
    const char* env = getenv_fn("PROSPEROWEB_SELF_PATH");
    if (env && env[0] != '\0') {
      chosen = env;
    }
  }

  if (!chosen) {
    chosen = kDefaultSelfPath;
    // The default path is always valid; fall through to copy it. argv0 is a
    // last-resort override only when it is absolute, so prefer the default
    // unless it somehow does not fit (handled below).
  }

  // If the default does not fit but an absolute argv0 does, we still want a
  // usable path; however the common case is the default fitting, so try the
  // chosen path first and only consider argv0 when chosen is unusable.
  size_t len = strlen(chosen);
  if (len + 1 > out_len) {
    // chosen does not fit; try an absolute argv0 as a fallback.
    if (argv0 && argv0[0] == '/') {
      size_t alen = strlen(argv0);
      if (alen + 1 <= out_len) {
        memcpy(out_path, argv0, alen + 1);
        return 0;
      }
    }
    return -1;
  }

  memcpy(out_path, chosen, len + 1);
  return 0;
}

int ps5_spawn_build_fd_table(const int* fds, int nfds, int src_pid,
                             struct ps5_spawn_fd_entry* out, int out_max) {
  if (nfds < 0 || nfds > PS5_SPAWN_MAX_FDS || src_pid < 0) {
    return -1;
  }
  if (nfds > 0 && (!fds || !out)) {
    return -1;
  }
  if (nfds > out_max) {
    return -1;
  }

  for (int i = 0; i < nfds; i++) {
    if (fds[i] < 0) {
      return -1;
    }
    out[i].parent_fd = fds[i];
    out[i].child_fd = 3 + i;
    out[i].src_pid = src_pid;
  }

  return nfds;
}

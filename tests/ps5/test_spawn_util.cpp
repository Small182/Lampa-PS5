// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstring>
#include <string>

#include <doctest/doctest.h>

#include "prosperoweb/spawn/spawn.h"

namespace {

// Injectable getenv stubs for ps5_spawn_resolve_self_path.
const char* env_unset(const char*) { return nullptr; }
const char* env_empty(const char*) { return ""; }
const char* env_custom(const char* name) {
  if (std::strcmp(name, "PROSPEROWEB_SELF_PATH") == 0) {
    return "/mnt/usb/my.elf";
  }
  return nullptr;
}

}  // namespace

TEST_SUITE("spawn_self_path") {
  TEST_CASE("env override wins") {
    char buf[256] = {};
    CHECK(ps5_spawn_resolve_self_path("/data/prosperoweb/prosperoweb.elf",
                                      env_custom, buf, sizeof(buf)) == 0);
    CHECK(std::string(buf) == "/mnt/usb/my.elf");
  }

  TEST_CASE("falls back to the fixed install path") {
    char buf[256] = {};
    CHECK(ps5_spawn_resolve_self_path("WebProcess", env_unset, buf,
                                      sizeof(buf)) == 0);
    CHECK(std::string(buf) == "/data/lampa-ps5/prosperoweb.elf");
  }

  TEST_CASE("empty env is ignored") {
    char buf[256] = {};
    CHECK(ps5_spawn_resolve_self_path("WebProcess", env_empty, buf,
                                      sizeof(buf)) == 0);
    CHECK(std::string(buf) == "/data/lampa-ps5/prosperoweb.elf");
  }

  TEST_CASE("absolute argv0 rescues a too-small buffer") {
    // Buffer too small for the default path but big enough for argv0.
    char buf[12] = {};
    CHECK(ps5_spawn_resolve_self_path("/a/b.elf", env_unset, buf,
                                      sizeof(buf)) == 0);
    CHECK(std::string(buf) == "/a/b.elf");
  }

  TEST_CASE("relative argv0 cannot rescue a too-small buffer") {
    char buf[12] = {};
    CHECK(ps5_spawn_resolve_self_path("WebProcess", env_unset, buf,
                                      sizeof(buf)) == -1);
  }

  TEST_CASE("null output is rejected") {
    CHECK(ps5_spawn_resolve_self_path("x", env_unset, nullptr, 10) == -1);
    char buf[4];
    CHECK(ps5_spawn_resolve_self_path("x", env_unset, buf, 0) == -1);
  }
}

TEST_SUITE("spawn_fd_table") {
  TEST_CASE("maps each fd to child fd 3+i") {
    const int fds[] = {7, 9, 4};
    ps5_spawn_fd_entry out[PS5_SPAWN_MAX_FDS];
    int n = ps5_spawn_build_fd_table(fds, 3, 0, out, PS5_SPAWN_MAX_FDS);
    REQUIRE(n == 3);
    CHECK(out[0].parent_fd == 7);
    CHECK(out[0].child_fd == 3);
    CHECK(out[1].parent_fd == 9);
    CHECK(out[1].child_fd == 4);
    CHECK(out[2].parent_fd == 4);
    CHECK(out[2].child_fd == 5);
    CHECK(out[0].src_pid == 0);  // the spawning process itself
  }

  TEST_CASE("carries the source process of the descriptors") {
    const int fds[] = {13, 14};
    ps5_spawn_fd_entry out[PS5_SPAWN_MAX_FDS];
    REQUIRE(ps5_spawn_build_fd_table(fds, 2, 93, out, PS5_SPAWN_MAX_FDS) == 2);
    CHECK(out[0].src_pid == 93);
    CHECK(out[1].src_pid == 93);
    CHECK(out[1].parent_fd == 14);
    CHECK(out[1].child_fd == 4);
    CHECK(ps5_spawn_build_fd_table(fds, 2, -1, out, PS5_SPAWN_MAX_FDS) == -1);
  }

  TEST_CASE("the common single-fd case lands at fd 3") {
    const int fds[] = {11};
    ps5_spawn_fd_entry out[PS5_SPAWN_MAX_FDS];
    int n = ps5_spawn_build_fd_table(fds, 1, 0, out, PS5_SPAWN_MAX_FDS);
    REQUIRE(n == 1);
    CHECK(out[0].parent_fd == 11);
    CHECK(out[0].child_fd == 3);
  }

  TEST_CASE("zero fds is valid") {
    ps5_spawn_fd_entry out[PS5_SPAWN_MAX_FDS];
    CHECK(ps5_spawn_build_fd_table(nullptr, 0, 0, out, PS5_SPAWN_MAX_FDS) == 0);
  }

  TEST_CASE("rejects negative descriptors") {
    const int fds[] = {3, -1};
    ps5_spawn_fd_entry out[PS5_SPAWN_MAX_FDS];
    CHECK(ps5_spawn_build_fd_table(fds, 2, 0, out, PS5_SPAWN_MAX_FDS) == -1);
  }

  TEST_CASE("rejects more than the maximum") {
    const int fds[PS5_SPAWN_MAX_FDS + 1] = {};
    ps5_spawn_fd_entry out[PS5_SPAWN_MAX_FDS];
    CHECK(ps5_spawn_build_fd_table(fds, PS5_SPAWN_MAX_FDS + 1, 0, out,
                                   PS5_SPAWN_MAX_FDS) == -1);
  }

  TEST_CASE("rejects a table that does not fit") {
    const int fds[] = {1, 2, 3};
    ps5_spawn_fd_entry out[2];
    CHECK(ps5_spawn_build_fd_table(fds, 3, 0, out, 2) == -1);
  }
}

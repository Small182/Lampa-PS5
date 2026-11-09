// SPDX-License-Identifier: GPL-3.0-or-later
//
// Process environment the WebKit runtime reads (fontconfig, TLS roots,
// language, temp/home directories). The defaults are set in main() before
// the role dispatch, so the UI, WebProcess and NetworkProcess all get them
// even if the spawner's environment copy does not reach a child; the UI
// then applies the config.json language, which the spawner forwards.
#pragma once

#include <string>

namespace prosperoweb::frontend {

// Install and persistence root on the console.
inline constexpr const char* kPs5DataDir = "/data/lampa-ps5";

// Install roots of the releases under earlier names, newest first
// (ProsperoBrowser, ps5browser). The newest one that has a profile is
// copied into kPs5DataDir once (prosperoweb/core/legacy_profile.h).
inline constexpr const char* kLegacyDataDirs[] = {"/data/lampa-ps5"};

// Maps a config.json language ("en", "ru", or already a tag like "de-DE")
// to the BCP 47 tag WebKit expects in SYSTEM_LANGUAGE.
std::string system_language_tag(const std::string& language);

// Sets FONTCONFIG_FILE, FONTCONFIG_PATH and PROSPEROWEB_CA_BUNDLE for the
// read-only files under assets_dir, and SYSTEM_LANGUAGE (en-US), TMPDIR and
// HOME under the writable profile base_dir, unless already set; then
// exports base_dir/config.json "env" (overriding), then defaults JSC_useJIT
// and JSC_useWasm to true, and creates base_dir and its temp directory. Any
// process role. The payload keeps both in kPs5DataDir; the native title
// reads its assets from the title folder (<title>/data, docs/native-title.md).
void setup_runtime_env(const std::string& base_dir, const std::string& assets_dir);

// The assets_dir of the last setup_runtime_env() (kPs5DataDir before it):
// the UI's own read-only files (the splash font).
const std::string& runtime_assets_dir();

// The value of one config.json "env" entry under base_dir (strings,
// numbers and booleans as setup_runtime_env exports them), or "" when it is
// absent. For settings needed before setup_runtime_env runs.
std::string config_env_value(const std::string& base_dir, const std::string& name);

// A top-level config.json switch under base_dir as a level: true or 1 -> 1,
// a number or numeric string -> that number, absent/false/invalid -> 0
// ("probe_fork": the native title's start-up fork probe).
int config_probe_level(const std::string& base_dir, const std::string& key);

// UI role: SYSTEM_LANGUAGE from config.json (overrides the default above).
void apply_language_env(const std::string& language);

}  // namespace prosperoweb::frontend

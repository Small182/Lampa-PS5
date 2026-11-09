// SPDX-License-Identifier: GPL-3.0-or-later
//
// Runtime configuration read from /data/prosperoweb/config.json.
#pragma once

#include <string>

#include "prosperoweb/core/addons_config.h"

namespace prosperoweb::core {
class Storage;
}

namespace prosperoweb::frontend {

// The default user agent: desktop Safari. Sites serve a "PlayStation"
// user agent TV or legacy variants, or nothing at all.
inline constexpr const char* kDefaultUserAgent =
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 "
    "(KHTML, like Gecko) Version/17.5 Safari/605.1.15";

struct Ps5Config {
  std::string home_url = "http://lampa.mx";
  std::string user_agent = kDefaultUserAgent;
  std::string language = "en";  // "en" or "ru"
  // Directory where chrome.html and its assets are installed, as the
  // WebProcess sees it. PROSPEROWEB_CHROME_DIR (the native title's
  // <title>/data/chrome) replaces the default.
  std::string chrome_dir = "/data/lampa-ps5/chrome";
  // Text entry: "system" (the PS5 keyboard dialog, built-in OSK as the
  // fallback) or "builtin" (the HTML OSK only). PROSPEROWEB_OSK overrides it.
  std::string osk = "system";
  int width = 1920;
  int height = 1080;
  // Natural (content-follows-finger) touchpad scrolling; default false
  // (mouse-wheel direction, the previous behaviour).
  bool natural_scroll = false;
  // Bundled add-ons (ad blocker, SponsorBlock); defaults when absent.
  core::AddonsConfig addons;

  // Reads "config" from storage (JSON). Missing or invalid keys keep the
  // defaults. Never throws.
  static Ps5Config load(core::Storage& storage);

  // Writes the "addons" object into config.json, preserving every other key
  // already in the file. Returns false on write failure. Never throws.
  static bool save_addons(core::Storage& storage, const core::AddonsConfig& addons);

  // Writes the "natural_scroll" boolean into config.json, preserving every
  // other key. Returns false on write failure. Never throws.
  static bool save_natural_scroll(core::Storage& storage, bool natural_scroll);
};

}  // namespace prosperoweb::frontend

// SPDX-License-Identifier: GPL-3.0-or-later
#include "ps5_config.h"

#include <cstdlib>
#include <functional>

#include <nlohmann/json.hpp>

#include "prosperoweb/core/storage.h"

namespace prosperoweb::frontend {

Ps5Config Ps5Config::load(core::Storage& storage) {
  Ps5Config cfg;
  // The native title's own chrome (main() sets it); config.json still wins.
  if (const char* dir = std::getenv("PROSPEROWEB_CHROME_DIR"); dir && dir[0]) {
    cfg.chrome_dir = dir;
  }
  auto raw = storage.read("config");
  if (!raw) {
    return cfg;
  }

  nlohmann::json doc = nlohmann::json::parse(*raw, nullptr, /*allow_exceptions=*/false);
  if (!doc.is_object()) {
    return cfg;
  }

  if (auto it = doc.find("home_url"); it != doc.end() && it->is_string()) {
    cfg.home_url = it->get<std::string>();
  }
  if (auto it = doc.find("user_agent"); it != doc.end() && it->is_string()) {
    cfg.user_agent = it->get<std::string>();
  }
  if (auto it = doc.find("language"); it != doc.end() && it->is_string()) {
    cfg.language = it->get<std::string>();
  }
  if (auto it = doc.find("chrome_dir"); it != doc.end() && it->is_string()) {
    cfg.chrome_dir = it->get<std::string>();
  }
  if (auto it = doc.find("osk"); it != doc.end() && it->is_string()) {
    cfg.osk = it->get<std::string>();
  }
  if (auto it = doc.find("width"); it != doc.end() && it->is_number_integer()) {
    cfg.width = it->get<int>();
  }
  if (auto it = doc.find("height"); it != doc.end() && it->is_number_integer()) {
    cfg.height = it->get<int>();
  }
  if (auto it = doc.find("natural_scroll"); it != doc.end() && it->is_boolean()) {
    cfg.natural_scroll = it->get<bool>();
  }
  cfg.addons = core::AddonsConfig::from_config_root(doc);
  cfg.home_url = "http://lampa.mx";

  return cfg;
}

namespace {
// Read config.json, apply `mutate` to its root object (preserving every other
// key), and write it back. Returns false on write failure.
bool update_config(core::Storage& storage, const std::function<void(nlohmann::json&)>& mutate) {
  nlohmann::json doc = nlohmann::json::object();
  if (auto raw = storage.read("config")) {
    nlohmann::json parsed = nlohmann::json::parse(*raw, nullptr, /*allow_exceptions=*/false);
    if (parsed.is_object()) {
      doc = std::move(parsed);
    }
  }
  mutate(doc);
  return storage.write("config", doc.dump(2));
}
}  // namespace

bool Ps5Config::save_addons(core::Storage& storage, const core::AddonsConfig& addons) {
  return update_config(storage, [&](nlohmann::json& doc) { doc["addons"] = addons.to_json(); });
}

bool Ps5Config::save_natural_scroll(core::Storage& storage, bool natural_scroll) {
  return update_config(storage, [&](nlohmann::json& doc) { doc["natural_scroll"] = natural_scroll; });
}

}  // namespace prosperoweb::frontend

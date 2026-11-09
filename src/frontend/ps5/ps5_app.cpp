// SPDX-License-Identifier: GPL-3.0-or-later
#include "ps5_app.h"

#include <algorithm>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <thread>

#include <sys/resource.h>
#include <unistd.h>

#include <WebKit/WKRunLoop.h>

#include "prosperoweb/spawn/spawn.h"

#include "compose.h"
#include "prosperoweb/core/gamepad.h"
#include "prosperoweb/core/page_scripts.h"
#include "ps5_diagnostics.h"
#include "ps5_hwdec.h"
#include "ps5_log.h"
#include "ps5_runtime_env.h"
#include "ps5_splash_text.h"
#include "title_paths.h"

#if PROSPEROWEB_NATIVE_TITLE
#include "native/native_title.h"  // native_boot_say: durable boot.txt breadcrumbs
#define PRWB_BOOT_CRUMB(s) native_boot_say("[prosperoweb] " s "\n")
#else
#define PRWB_BOOT_CRUMB(s) ((void)0)
#endif

namespace prosperoweb::frontend {

namespace {

// Console diagnostics: reports the first DOM pointer events of each document
// (type, target, detail, buttons, isTrusted, defaultPrevented) through the
// "prosperoweb" handler as {type: 'input_trace'}; Ps5EngineView logs them.
// Shows whether a press/release pair became a click and what got focus.
constexpr const char* kInputTraceScript = R"JS((function () {
  'use strict';
  if (window !== window.top) return;
  var left = 24;
  function desc(e) {
    if (!e || !e.tagName) return String(e && e.nodeName);
    var d = e.tagName.toLowerCase();
    if (e.id) d += '#' + e.id;
    return d;
  }
  ['pointerdown', 'mousedown', 'mouseup', 'click', 'dblclick', 'focusin'].forEach(function (t) {
    document.addEventListener(t, function (ev) {
      if (left <= 0) return;
      left--;
      var target = ev.target;
      setTimeout(function () {
        try {
          window.webkit.messageHandlers.prosperoweb.postMessage({
            type: 'input_trace', ev: t, target: desc(target),
            x: ev.clientX === undefined ? -1 : ev.clientX, y: ev.clientY === undefined ? -1 : ev.clientY,
            detail: ev.detail === undefined ? -1 : ev.detail, buttons: ev.buttons === undefined ? -1 : ev.buttons,
            trusted: ev.isTrusted, prevented: ev.defaultPrevented,
            active: desc(document.activeElement)
          });
        } catch (e) {}
      }, 0);
    }, true);
  });
})();
)JS";

// Lampa occasionally passes a failed poster/backdrop image to Canvas while
// preparing its blurred background. WebKit correctly rejects that drawImage,
// but Lampa's global error overlay then obscures the movie details. Ignore
// only unusable HTMLImageElements; preserve every other Canvas exception.
constexpr const char* kLampaCanvasImageGuard = R"JS((function () {
  'use strict';
  if (location.hostname !== 'lampa.mx' && !location.hostname.endsWith('.lampa.mx')) return;
  if (!window.CanvasRenderingContext2D) return;
  var proto = CanvasRenderingContext2D.prototype;
  var original = proto.drawImage;
  if (typeof original !== 'function') return;
  proto.drawImage = function (source) {
    if (source instanceof HTMLImageElement &&
        (!source.complete || source.naturalWidth === 0 || source.naturalHeight === 0)) return;
    return original.apply(this, arguments);
  };
})();
)JS";

constexpr const char* kBaseDir = kPs5DataDir;

std::string tmp_path(const std::string& name) { return std::string(kBaseDir) + "/tmp/" + name; }

// A path the WebProcess is given, as this process sees it: the native title
// reads its folder as /app0, the children by its real path (title_paths.h;
// main() exports both roots). Unchanged for the payload.
std::string ui_view(const std::string& path) {
  const char* ui = std::getenv("PROSPEROWEB_TITLE_UI_ROOT");
  const char* child = std::getenv("PROSPEROWEB_TITLE_CHILD_ROOT");
  return ui && child ? spawn::rebase_path(path, child, ui) : path;
}
// Milliseconds since `start` (an SDL_GetPerformanceCounter() value).
double ms_since(std::uint64_t start) {
  static const double ms_per_tick = 1000.0 / static_cast<double>(SDL_GetPerformanceFrequency());
  return static_cast<double>(SDL_GetPerformanceCounter() - start) * ms_per_tick;
}

std::optional<core::Button> sdl_button_to_core(SDL_GameControllerButton b) {
  switch (b) {
    case SDL_CONTROLLER_BUTTON_A: return core::Button::Cross;
    case SDL_CONTROLLER_BUTTON_B: return core::Button::Circle;
    case SDL_CONTROLLER_BUTTON_X: return core::Button::Square;
    case SDL_CONTROLLER_BUTTON_Y: return core::Button::Triangle;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return core::Button::L1;
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return core::Button::R1;
    case SDL_CONTROLLER_BUTTON_LEFTSTICK: return core::Button::L3;
    case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return core::Button::R3;
    case SDL_CONTROLLER_BUTTON_DPAD_UP: return core::Button::Up;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return core::Button::Down;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return core::Button::Left;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return core::Button::Right;
    case SDL_CONTROLLER_BUTTON_START: return core::Button::Options;
    case SDL_CONTROLLER_BUTTON_BACK: return core::Button::Share;
    case SDL_CONTROLLER_BUTTON_GUIDE: return core::Button::PS;
    case SDL_CONTROLLER_BUTTON_TOUCHPAD: return core::Button::Touchpad;
    default: return std::nullopt;
  }
}

std::uint32_t dom_key_code(SDL_Keycode k) {
  if (k >= SDLK_a && k <= SDLK_z) {
    return static_cast<std::uint32_t>('A' + (k - SDLK_a));
  }
  if (k >= SDLK_0 && k <= SDLK_9) {
    return static_cast<std::uint32_t>('0' + (k - SDLK_0));
  }
  switch (k) {
    case SDLK_RETURN: case SDLK_KP_ENTER: return 13;
    case SDLK_BACKSPACE: return 8;
    case SDLK_TAB: return 9;
    case SDLK_ESCAPE: return 27;
    case SDLK_SPACE: return 32;
    case SDLK_LEFT: return 37;
    case SDLK_UP: return 38;
    case SDLK_RIGHT: return 39;
    case SDLK_DOWN: return 40;
    case SDLK_PAGEUP: return 33;
    case SDLK_PAGEDOWN: return 34;
    case SDLK_END: return 35;
    case SDLK_HOME: return 36;
    case SDLK_INSERT: return 45;
    case SDLK_DELETE: return 46;
    default:
      if (k >= SDLK_F1 && k <= SDLK_F12) {
        return static_cast<std::uint32_t>(112 + (k - SDLK_F1));
      }
      return 0;
  }
}

std::uint32_t dom_modifiers(SDL_Keymod m) {
  std::uint32_t out = 0;
  if (m & KMOD_SHIFT) out |= shell::kModShift;
  if (m & KMOD_CTRL) out |= shell::kModControl;
  if (m & KMOD_ALT) out |= shell::kModAlt;
  if (m & KMOD_GUI) out |= shell::kModMeta;
  return out;
}

}  // namespace

Ps5App::Ps5App() = default;

Ps5App::~Ps5App() {
  loader_retry_stop_.store(true, std::memory_order_release);
  if (loader_retry_thread_.joinable()) {
    loader_retry_thread_.join();
  }
  ps5_spawn_set_progress(nullptr, nullptr);
  ps5_spawn_set_media_control(nullptr, nullptr, nullptr);
  WkWebView::set_damage_hook(nullptr, nullptr);
  video_overlay_.reset();  // its thread calls request_frame()
  presenter_.reset();     // before the window goes
  audio_server_.reset();  // before SDL_Quit
  if (pad_) {
    SDL_GameControllerClose(pad_);
  }
  if (window_) {
    SDL_DestroyWindow(window_);
  }
  SDL_Quit();
}

int Ps5App::run() {
  // Durable breadcrumbs to boot.txt (independent of log.txt): if log.txt stays
  // empty, these localize whether run() was entered and the logger opened.
  PRWB_BOOT_CRUMB("run: entered");
  log_open(std::string(kBaseDir) + "/log.txt");  // no-op: main() opened it
  PRWB_BOOT_CRUMB("run: log opened");
  log_line("ProsperoWeb UI starting");
  main_thread_ = std::this_thread::get_id();

  log_line("SDL_Init: calling");
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
    log_printf("SDL_Init failed: %s", SDL_GetError());
    return 1;
  }
  log_printf("SDL_Init: ok, video driver %s",
             SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "(none)");

  // Before the WebProcess is spawned, so its media can connect at once.
  audio_server_ = Ps5AudioServer::start();
  video_overlay_ = Ps5VideoOverlay::start([this] { request_frame(); });
  if (const char* test = std::getenv("PROSPEROWEB_AUDIO_TEST");
      audio_server_ && test && !std::strcmp(test, "1")) {
    audio_server_->play_test_tone();
  }
  // Media control sockets: when a server could not listen() (the media
  // application-category sandbox refuses it), WebProcess media must reach it
  // over an inherited socketpair instead (spawn_glue.cpp passes one end to
  // each child). PROSPEROWEB_MEDIA_CHANNEL forces it: socketpair (always),
  // tcp (never, keeps the listen path even if it fails). Default: auto.
  const char* channel = std::getenv("PROSPEROWEB_MEDIA_CHANNEL");
  const std::string channel_mode = channel ? channel : "auto";
  const bool listen_failed = (audio_server_ && !audio_server_->listening()) ||
                             (video_overlay_ && !video_overlay_->listening());
  const bool use_control = channel_mode == "socketpair" || (channel_mode != "tcp" && listen_failed);
  if (use_control && (audio_server_ || video_overlay_)) {
    ps5_spawn_set_media_control(&Ps5App::audio_control_fd, &Ps5App::video_control_fd, this);
    log_printf("media: WebProcess audio/video arrive over control sockets (%s; audio listen %s, "
               "video listen %s)",
               channel_mode.c_str(),
               audio_server_ ? (audio_server_->listening() ? "ok" : "refused") : "off",
               video_overlay_ ? (video_overlay_->listening() ? "ok" : "refused") : "off");
  }

  // Widen the file-descriptor budget: each attached ad-block content rule list
  // hands a SharedMemory descriptor to the WebProcess (the sandbox cannot dup
  // handles), so more fds let more/bigger lists fit. Best-effort.
  {
    struct rlimit rl {};
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
      const rlim_t before = rl.rlim_cur;
      rl.rlim_cur = rl.rlim_max;  // raise soft limit to the hard ceiling
      const bool raised = setrlimit(RLIMIT_NOFILE, &rl) == 0;
      struct rlimit now {};
      getrlimit(RLIMIT_NOFILE, &now);
      log_printf("fd budget: RLIMIT_NOFILE %ld -> %ld (max %ld)%s", static_cast<long>(before),
                 static_cast<long>(now.rlim_cur), static_cast<long>(now.rlim_max),
                 raised ? "" : " (raise failed)");
    }
  }

  storage_ = std::make_unique<core::FileStorage>(kBaseDir);
  config_ = Ps5Config::load(*storage_);
  width_ = config_.width;
  height_ = config_.height;

  log_printf("SDL_CreateWindow: calling %dx%d", width_, height_);
  window_ = SDL_CreateWindow("ProsperoWeb", SDL_WINDOWPOS_UNDEFINED,
                             SDL_WINDOWPOS_UNDEFINED, width_, height_, 0);
  if (!window_) {
    log_printf("SDL_CreateWindow failed: %s", SDL_GetError());
    return 1;
  }
  log_line("SDL_CreateWindow: ok");
  // Before any WebKit work: proves the display path independently of
  // WebKit painting (a WKPagePaint without a WebProcess leaves zeros).
  draw_splash();
  PRWB_BOOT_CRUMB("run: splash drawn");
  mark_splash_stage(kSplashWindow);
  // The broker and the WebKit processes report from WebKit's launcher.
  ps5_spawn_set_progress(&Ps5App::splash_spawn_progress, this);
  // Printable keyboard input arrives as SDL_TEXTINPUT (see pump_sdl_events).
  // Without the hint SDL_StartTextInput() would also open the system
  // keyboard dialog (SDL's PS5 driver implements the screen keyboard with
  // sceImeDialog); Ps5ImeTextInput opens it on demand instead.
  SDL_SetHint(SDL_HINT_ENABLE_SCREEN_KEYBOARD, "0");
  SDL_StartTextInput();

  page_buffer_.assign(static_cast<size_t>(width_) * height_, 0xFF000000u);
  chrome_buffer_.assign(static_cast<size_t>(width_) * height_, 0u);

  // main() set the runtime defaults for every role; the language comes from
  // config.json and must precede WebKit initialisation and the first
  // auxiliary process spawn (which copies the environment).
  apply_language_env(config_.language);

  // WebKit main run loop (also initializes WebKit2).
  log_line("WKRunLoopInitializeMain: calling");
  WKRunLoopInitializeMain();
  log_line("WKRunLoopInitializeMain: ok");

  std::vector<std::string> user_scripts = {core::kEmeDetectorScript,
                                           core::kFocusReporterScript,
                                           core::kGamepadKeyBridgeScript,
                                           kLampaCanvasImageGuard,
                                           kInputTraceScript};
  factory_ = std::make_unique<Ps5EngineFactory>(kBaseDir, user_scripts,
                                                config_.user_agent);
  log_line("WebKit: context and data store created");
  mark_splash_stage(kSplashWebKit);

  // config.json may still name the websrv install's chrome
  // (/data/prosperoweb/chrome, config.json.sample); the native title falls
  // back to its own copy (PROSPEROWEB_CHROME_DIR, set by main()).
  std::string chrome_dir = config_.chrome_dir;
  const char* title_chrome = std::getenv("PROSPEROWEB_CHROME_DIR");
  const bool native_title = std::getenv("PROSPEROWEB_TITLE_UI_ROOT") != nullptr;
  if (native_title && title_chrome && title_chrome[0] &&
      access(ui_view(std::string(title_chrome) + "/chrome.html").c_str(), R_OK) == 0) {
    // The native title always uses the chrome it ships: a /data/prosperoweb/
    // chrome left by the websrv install may be older than this shell and not
    // understand its updates (console run 33: tab titles and the address bar
    // stayed empty).
    if (chrome_dir != title_chrome) {
      log_printf("chrome: using the title's %s, not %s", title_chrome, chrome_dir.c_str());
    }
    chrome_dir = title_chrome;
  } else if (access(ui_view(chrome_dir + "/chrome.html").c_str(), R_OK) != 0 && title_chrome &&
             title_chrome[0] && chrome_dir != title_chrome) {
    log_printf("chrome: %s/chrome.html not found, using the title's %s", chrome_dir.c_str(),
               title_chrome);
    chrome_dir = title_chrome;
  }
  const std::string chrome_path = chrome_dir + "/chrome.html";
  std::string chrome_url = "file://" + chrome_path;
  if (access(ui_view(chrome_path).c_str(), R_OK) != 0) {
    log_printf("chrome: %s is missing (%s); reinstall the chrome/ directory",
               ui_view(chrome_path).c_str(), std::strerror(errno));
    chrome_url.clear();  // Ps5ChromeUi shows its built-in fallback
  } else {
    log_printf("chrome: %s", chrome_url.c_str());
  }
  chrome_ = std::make_unique<Ps5ChromeUi>(
      factory_->context(), factory_->data_store(), chrome_url, width_, height_,
      [this](const nlohmann::json& e) {
        if (controller_) {
          controller_->on_chrome_event(e);
        }
      });

  // Add-ons: the bundled baseline lives next to the chrome (title dir or
  // /data/prosperoweb/addons); the writable cache and recompiled rulesets go
  // in the profile. Apply to every content view as it is created.
  {
    std::string addons_parent = chrome_dir;
    if (auto slash = addons_parent.find_last_of('/'); slash != std::string::npos) {
      addons_parent = addons_parent.substr(0, slash);
    }
    std::string addons_dir = addons_parent + "/addons";
    if (access(ui_view(addons_dir + "/adblock-ads.json").c_str(), R_OK) != 0) {
      addons_dir = std::string(kBaseDir) + "/addons";
    }
    const std::string addons_cache = std::string(kBaseDir) + "/addons/cache";
    log_printf("addons: baseline %s, cache %s", addons_dir.c_str(), addons_cache.c_str());
    addons_ = std::make_unique<Ps5Addons>(addons_dir, addons_cache, config_.addons);
    // All content views share one user content controller; the add-ons attach
    // to it once (a content rule list's SharedMemory handle can be handed over
    // only once, so per-view controllers cannot share it).
    addons_->attach_controller(factory_->shared_content_controller(), factory_->base_scripts());
  }

  shell::ShellController::Config cfg;
  cfg.width = width_;
  cfg.height = height_;
  cfg.home_url = config_.home_url;
  cfg.kiosk_mode = true;
  // The compositor draws the pointer (display/pointer.h): moving it never
  // repaints the chrome view.
  cfg.native_pointer = true;
  cfg.user_agent = config_.user_agent;
  cfg.language = config_.language;
  cfg.diagnostics_url = "file://" + chrome_dir + "/diag.html";
  cfg.addons = config_.addons;
  cfg.addons_backend = addons_.get();
  cfg.on_addons_changed = [this](const core::AddonsConfig& a) {
    if (!Ps5Config::save_addons(*storage_, a)) {
      log_line("addons: failed to persist config.json");
    }
  };
  cfg.natural_scroll = config_.natural_scroll;
  cfg.on_natural_scroll_changed = [this](bool v) {
    log_printf("input: natural scrolling %s", v ? "on" : "off");
    if (!Ps5Config::save_natural_scroll(*storage_, v)) {
      log_line("input: failed to persist natural_scroll");
    }
  };
  if (use_system_ime(config_.osk)) {
    ime_input_ = std::make_unique<Ps5ImeTextInput>();
    cfg.text_input = ime_input_.get();
    log_line("text input: system keyboard dialog (built-in OSK as fallback)");
  } else {
    log_line("text input: built-in OSK (PROSPEROWEB_OSK / config osk = builtin)");
  }
  controller_ = std::make_unique<shell::ShellController>(*factory_, *chrome_, *storage_,
                                                         cfg);
  log_line("shell: starting");
  controller_->start();
  log_line("shell: started");
  // Now that the first content view exists, compile/apply the add-ons and
  // wire the refresh-done callback back into the shell menu.
  addons_->set_on_filters_updated(
      [this](std::int64_t when) { controller_->on_filters_updated(when); });
  addons_->start();
  // The hardware decoder only now, and off this thread (ps5_hwdec.h).
  hwdec_startup_done();

  running_ = true;
  last_frame_ticks_ = SDL_GetTicks64();
  {
    const char* env = std::getenv("PROSPEROWEB_PRESENT_THREAD");
    const bool threaded = !(env && !std::strcmp(env, "0"));
    presenter_ = std::make_unique<Ps5Presenter>(window_, threaded, [this] { request_frame(); });
    log_printf("present: %s, frames paced by flips and a %u ms tick",
               threaded ? "presenter thread" : "main thread (PROSPEROWEB_PRESENT_THREAD=0)",
               Ps5Presenter::kTickMs);
  }
  WkWebView::set_damage_hook(
      [](void* self) { static_cast<Ps5App*>(self)->request_frame(); }, this);
  frame_loop_ready_.store(true, std::memory_order_release);
  request_frame();
  log_line("WKRunLoopRunMain: entering");
  WKRunLoopRunMain();

  log_line("ProsperoWeb UI stopped");
  log_close();
  return 0;
}

void Ps5App::draw_splash() {
  const std::uint64_t t0 = SDL_GetTicks64();
  SDL_Surface* surface = SDL_GetWindowSurface(window_);
  if (!surface) {
    log_printf("splash: SDL_GetWindowSurface failed: %s", SDL_GetError());
    return;
  }
  // Quiet launch: keep a black frame until the first real WebKit paint.
  // Progress remains in the log; loader errors still have their own screen.
  SDL_FillRect(surface, nullptr, SDL_MapRGB(surface->format, 0, 0, 0));
  if (SDL_UpdateWindowSurface(window_) != 0) {
    log_printf("splash: SDL_UpdateWindowSurface failed: %s", SDL_GetError());
    return;
  }
  splash_drawn_ = true;
  log_printf("splash: quiet launch frame shown (%llu ms)",
             static_cast<unsigned long long>(SDL_GetTicks64() - t0));

  const char* hold = std::getenv("PROSPEROWEB_SPLASH_HOLD_MS");
  const long hold_ms = hold ? std::strtol(hold, nullptr, 10) : 0;
  if (hold_ms > 0) {
    log_printf("splash: holding %ld ms (PROSPEROWEB_SPLASH_HOLD_MS)", hold_ms);
    SDL_Delay(static_cast<std::uint32_t>(hold_ms));
    log_line("splash: hold done");
  }
}

void Ps5App::splash_spawn_progress(const char* event, void* data) {
  auto* self = static_cast<Ps5App*>(data);
  const std::string name = event ? event : "";
  static constexpr char kNoLoader[] = "no-loader:";
  static constexpr char kUnreachable[] = "broker-unreachable:";
  if (name.rfind(kNoLoader, 0) == 0) {
    self->note_loader_missing(name.substr(sizeof(kNoLoader) - 1));
    return;
  }
  if (name.rfind(kUnreachable, 0) == 0) {
    self->note_broker_unreachable(name.substr(sizeof(kUnreachable) - 1));
    return;
  }
  unsigned stage = 0;
  if (name == "broker") {
    stage = kSplashBroker;
  } else if (name.find("NetworkProcess") != std::string::npos) {
    stage = kSplashBroker | kSplashNetworkProcess;  // direct spawns have no broker
  } else if (name.find("WebProcess") != std::string::npos) {
    stage = kSplashBroker | kSplashWebProcess;
  }
  if (stage) {
    self->mark_splash_stage(stage);
  }
}

void Ps5App::note_loader_missing(const std::string& detail) {
  note_startup_problem(1, detail);
}

void Ps5App::note_broker_unreachable(const std::string& detail) {
  note_startup_problem(2, detail);
}

void Ps5App::note_startup_problem(int state, const std::string& detail) {
  {
    std::lock_guard<std::mutex> lock(loader_detail_mu_);
    loader_detail_ = detail;
    loader_state_ = state;
  }
  start_loader_retry();  // keep trying in the background whatever the cause
  if (loader_missing_.exchange(true)) {
    // Already flagged. The frame loop will repaint if the state changed
    // (render_loader_missing checks loader_drawn_state_).
    if (frame_loop_ready_.load(std::memory_order_acquire)) {
      request_frame();
    }
    return;
  }
  ps5_log_printf("startup: %s: %s",
                 state == 2 ? "loader up but broker unreachable" : "no ELF loader reachable",
                 detail.c_str());
  if (frame_loop_ready_.load(std::memory_order_acquire)) {
    request_frame();
  } else if (std::this_thread::get_id() == main_thread_) {
    if (SDL_Surface* surface = SDL_GetWindowSurface(window_)) {
      render_loader_missing(surface);
    }
  }
}

// Replaces the splash with a clear message about why the web engine has not
// started. Repainted only when the state changes (the content is otherwise
// static) and left in place until WebKit paints, if a loader/broker comes up.
void Ps5App::render_loader_missing(SDL_Surface* surface) {
  int state;
  std::string detail;
  {
    std::lock_guard<std::mutex> lock(loader_detail_mu_);
    state = loader_state_;
    detail = loader_detail_;
  }
  if (!surface || (loader_message_drawn_ && loader_drawn_state_ == state)) {
    return;
  }
  loader_message_drawn_ = true;
  loader_drawn_state_ = state;
  if (detail.size() > 110) {
    detail = detail.substr(0, 107) + "...";
  }
  const std::string font = runtime_assets_dir() + "/fonts/NotoSans-Regular.ttf";
  const int head_px = std::max(20, surface->h * 48 / 1080);
  const int body_px = std::max(14, surface->h * 30 / 1080);
  const int small_px = std::max(11, surface->h * 22 / 1080);
  std::string error;
  display::GrayImage l1, l2, l3, l4;
  if (state == 2) {
    render_splash_text(font, "ProsperoWeb started its loader but could not connect.", head_px, l1,
                       error);
    render_splash_text(font, "Waiting for the spawn broker on 127.0.0.1:17771 to answer.",
                       body_px, l2, error);
    render_splash_text(font, "Still trying — it should start on its own in a moment.", body_px, l3,
                       error);
  } else {
    render_splash_text(font, "ProsperoWeb could not reach an ELF loader.", head_px, l1, error);
    render_splash_text(font, "It needs a loader on 127.0.0.1:9021 to start its web engine.",
                       body_px, l2, error);
    render_splash_text(font, "Start etaHEN or elfldr — it will continue on its own, no relaunch.",
                       body_px, l3, error);
  }
  if (!detail.empty()) {
    render_splash_text(font, detail, small_px, l4, error);
  }
  if (SDL_MUSTLOCK(surface)) {
    SDL_LockSurface(surface);
  }
  const display::SplashTarget target{static_cast<std::uint32_t*>(surface->pixels), surface->w,
                                     surface->h, surface->pitch / 4,
                                     surface->format->format == SDL_PIXELFORMAT_ABGR8888};
  display::paint_message(target, {&l1, &l2, &l3, &l4});
  if (SDL_MUSTLOCK(surface)) {
    SDL_UnlockSurface(surface);
  }
  const SDL_Rect rect{0, 0, surface->w, surface->h};
  if (presenter_) {
    presenter_->submit({rect});
  } else {
    SDL_UpdateWindowSurfaceRects(window_, &rect, 1);
  }
  log_printf("startup: showing the startup-error message (state %d: %s)", loader_drawn_state_,
             detail.c_str());
}

void Ps5App::start_loader_retry() {
  // Exactly once for the life of the app: std::call_once guarantees the
  // std::thread is assigned a single time and never move-assigned while
  // joinable (which would std::terminate). Re-reporting a startup problem just
  // no-ops here.
  std::call_once(loader_retry_once_,
                 [this] { loader_retry_thread_ = std::thread(&Ps5App::loader_retry_main, this); });
}

// Polls the broker periodically off the UI thread so a loader or broker that
// the user brings up after the error screen is picked up without relaunching
// the app. Bounded, non-blocking for the UI loop; it just holds a broker up so
// WebKit's next process launch connects. Stops on success or at shutdown.
void Ps5App::loader_retry_main() {
  constexpr int kRetryMs = 3000;
  log_line("startup: background broker-retry started");
  while (!loader_retry_stop_.load(std::memory_order_acquire)) {
    for (int slept = 0; slept < kRetryMs && !loader_retry_stop_.load(std::memory_order_acquire);
         slept += 100) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (loader_retry_stop_.load(std::memory_order_acquire)) {
      break;
    }
    if (ps5_spawn_try_bring_up_broker()) {
      log_line("startup: broker is up now; the web engine will start");
      if (frame_loop_ready_.load(std::memory_order_acquire)) {
        request_frame();
      }
      break;  // the connection is held; WebKit's next launch spawns
    }
  }
}

void Ps5App::audio_control_fd(int fd, void* data) {
  auto* self = static_cast<Ps5App*>(data);
  if (self && self->audio_server_) {
    self->audio_server_->add_control_fd(fd);
  } else {
    ::close(fd);
  }
}

void Ps5App::video_control_fd(int fd, void* data) {
  auto* self = static_cast<Ps5App*>(data);
  if (self && self->video_overlay_) {
    self->video_overlay_->add_control_fd(fd);
  } else {
    ::close(fd);
  }
}

// Any thread. Once the frame loop runs, the next frame draws the bar; before
// that the main thread draws it at once (nothing else touches the surface
// yet) and other threads leave it to the first frame.
void Ps5App::mark_splash_stage(unsigned stage) {
  const unsigned before = splash_stages_.fetch_or(stage, std::memory_order_acq_rel);
  if ((before | stage) == before) {
    return;
  }
  if (frame_loop_ready_.load(std::memory_order_acquire)) {
    request_frame();
  } else if (std::this_thread::get_id() == main_thread_) {
    if (SDL_Surface* surface = SDL_GetWindowSurface(window_)) {
      update_splash_progress(surface);
    }
  }
}

// Main thread, only while the splash is up and the surface is not being
// presented.
void Ps5App::update_splash_progress(SDL_Surface* surface) {
  (void)surface;
  const unsigned stages = splash_stages_.load(std::memory_order_acquire);
  if (!splash_drawn_ || stages == splash_shown_stages_) {
    return;
  }
  splash_shown_stages_ = stages;
  const int done = std::popcount(stages);
  log_printf("splash: stage %d of %d (0x%x)", done, kSplashStages, stages);
}

void Ps5App::frame_trampoline(void* self) {
  static_cast<Ps5App*>(self)->frame();
}

void Ps5App::request_frame() {
  // At most one frame queued on the run loop: a slow frame does not pile up
  // ticks behind it.
  if (!frame_queued_.exchange(true, std::memory_order_acq_rel)) {
    WKRunLoopCallOnMainThread(&Ps5App::frame_trampoline, this);
  }
}

void Ps5App::frame() {
  frame_queued_.store(false, std::memory_order_release);
  if (!running_) {
    return;
  }
  std::uint64_t now = SDL_GetTicks64();
  if (frames_ == 0) {
    log_line("frame: first frame");
  }
  frames_++;
  stats_.frames++;
  float dt = static_cast<float>(now - last_frame_ticks_) / 1000.0f;
  last_frame_ticks_ = now;

  // Before SDL_PollEvent(): SDL's PS5 driver watches the same dialog there
  // and would otherwise consume its result first (see Ps5SystemIme).
  if (ime_input_) {
    ime_input_->poll();
  }
  pump_sdl_events();
  handle_axis_motion();
  handle_touchpad(now);
  if (share_down_ && !share_long_fired_ && now - share_down_ticks_ >= kShareLongPressMs) {
    share_long_fired_ = true;
    screenshot_pending_ = true;
    log_line("screenshot: requested by a Share long press");
  }
  poll_diagnostic_requests(now);
  // Page and chrome script messages (see WkWebView::poll_all()).
  WkWebView::poll_all();
  if (addons_) {
    addons_->pump();  // finish a background filter-list refresh, daily cadence
  }
  controller_->tick(dt);
  composite_and_present();
  collect_and_log_stats(now);

  if (controller_->quit_requested()) {
    controller_->shutdown();
    running_ = false;
    presenter_->stop();
    WKRunLoopStopMain();
  }
  // The next frame comes from the presenter (after a flip, or its tick):
  // the run loop stays free for WebKit's IPC in between.
}

void Ps5App::pump_sdl_events() {
  SDL_Event e;
  while (SDL_PollEvent(&e)) {
    switch (e.type) {
      case SDL_QUIT:
        if (controller_) {
          controller_->on_button(core::Button::PS, true);
        }
        break;
      case SDL_CONTROLLERDEVICEADDED:
        if (!pad_) {
          pad_ = SDL_GameControllerOpen(e.cdevice.which);
          touchpad_.reset();
        }
        break;
      case SDL_CONTROLLERDEVICEREMOVED:
        if (pad_) {
          SDL_GameControllerClose(pad_);
          pad_ = nullptr;
          touchpad_.reset();
        }
        break;
      case SDL_CONTROLLERBUTTONDOWN:
        handle_controller_button(e.cbutton, true);
        break;
      case SDL_CONTROLLERBUTTONUP:
        handle_controller_button(e.cbutton, false);
        break;
      case SDL_KEYDOWN:
      case SDL_KEYUP: {
        const bool is_return = e.key.keysym.sym == SDLK_RETURN;
        if (ime_input_ && ime_input_->ime().is_open()) {
          if (is_return && e.type == SDL_KEYDOWN) {
            ime_input_->ime().note_sdl_return();
            swallow_return_up_ = true;
          }
          break;  // the dialog owns text entry
        }
        if (is_return && e.type == SDL_KEYUP && swallow_return_up_) {
          swallow_return_up_ = false;
          break;
        }
        // Key events carry no text: printable characters arrive separately
        // as SDL_TEXTINPUT and are inserted below, so they are not typed twice.
        std::uint32_t code = dom_key_code(e.key.keysym.sym);
        auto* view = controller_->active_view();
        if (view && code != 0) {
          view->key(code, dom_modifiers(SDL_GetModState()), e.type == SDL_KEYDOWN,
                    std::string());
        }
        break;
      }
      case SDL_TEXTINPUT: {
        if (ime_input_ && ime_input_->ime().is_open()) {
          break;
        }
        if (auto* view = controller_->active_view()) {
          view->insert_text(e.text.text);
        }
        break;
      }
      default:
        break;
    }
  }
}

void Ps5App::handle_controller_button(const SDL_ControllerButtonEvent& e, bool pressed) {
  auto b = sdl_button_to_core(static_cast<SDL_GameControllerButton>(e.button));
  if (!b) {
    return;
  }
  if (*b == core::Button::Share) {
    // Decided on release: a long press (see frame()) is a screen dump and
    // never reaches the shell; a short one is delivered as a whole.
    if (pressed) {
      share_down_ = true;
      share_long_fired_ = false;
      share_down_ticks_ = SDL_GetTicks64();
    } else if (share_down_) {
      share_down_ = false;
      if (!share_long_fired_) {
        controller_->on_button(*b, true);
        controller_->on_button(*b, false);
      }
    }
    return;
  }
  controller_->on_button(*b, pressed);
}

void Ps5App::poll_diagnostic_requests(std::uint64_t now) {
  if (now - last_request_poll_ticks_ < kRequestPollMs) {
    return;
  }
  last_request_poll_ticks_ = now;
  if (take_request_file(tmp_path(kScreenshotRequestFile))) {
    log_printf("screenshot: requested by %s", kScreenshotRequestFile);
    screenshot_pending_ = true;
  }
  if (take_request_file(tmp_path(kProbeRequestFile))) {
    log_printf("probe: requested by %s", kProbeRequestFile);
    run_form_probe();
  }
}

void Ps5App::dump_screen(SDL_Surface* surface, const std::vector<display::Rect>* chrome_rects,
                         int top, const video::PageKey& video_page) {
  screenshot_pending_ = false;
  const std::uint64_t t0 = SDL_GetTicks64();
  if (chrome_rects && width_ > 0 && height_ > 0 && !page_buffer_.empty() &&
      !chrome_buffer_.empty()) {
    // The composition of frame(), over the whole screen, into ABGR8888.
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(width_) *
                                      static_cast<std::size_t>(height_));
    const display::Layers layers{page_buffer_.data(), chrome_buffer_.data(), width_, height_};
    std::size_t video_layers = 0;
    {
      std::optional<video::VideoServer::Frames> frames;
      std::vector<display::VideoLayer> videos;
      if (video_overlay_ && video_page.pid) {
        frames.emplace(video_overlay_->acquire(video_page));
        videos = Ps5VideoOverlay::layers(*frames, top, width_, height_);
      }
      video_layers = videos.size();
      display::compose_rect(layers, *chrome_rects, display::Rect{0, 0, width_, height_},
                            pixels.data(), width_, videos);
    }
    const std::string png = encode_png_rgbx(reinterpret_cast<const std::uint8_t*>(pixels.data()),
                                            width_, height_, width_ * 4);
    if (png.empty()) {
      log_line("screenshot: PNG encoding failed");
      return;
    }
    const std::string path = tmp_path("screen-" + std::to_string(t0) + ".png");
    const bool ok = write_file_atomically(path, png);
    const bool latest_ok = write_file_atomically(tmp_path(kScreenLatestFile), png);
    if (ok) {
      log_printf("screenshot: written %s (%llu ms, %dx%d composed, %zu video layer%s, %zu bytes%s)",
                 path.c_str(), static_cast<unsigned long long>(SDL_GetTicks64() - t0), width_,
                 height_, video_layers, video_layers == 1 ? "" : "s", png.size(),
                 latest_ok ? ", also screen-latest.png" : "");
    }
    return;
  }
  // The window surface is ABGR8888 (R, G, B, A bytes); convert anything else.
  SDL_Surface* converted = nullptr;
  SDL_Surface* src = surface;
  if (surface->format->format != SDL_PIXELFORMAT_ABGR8888) {
    converted = SDL_ConvertSurfaceFormat(surface, SDL_PIXELFORMAT_ABGR8888, 0);
    if (!converted) {
      log_printf("screenshot: converting the surface failed: %s", SDL_GetError());
      return;
    }
    src = converted;
  }
  const std::string png = encode_png_rgbx(static_cast<const std::uint8_t*>(src->pixels),
                                          src->w, src->h, src->pitch);
  const int w = src->w;
  const int h = src->h;
  if (converted) {
    SDL_FreeSurface(converted);
  }
  if (png.empty()) {
    log_line("screenshot: PNG encoding failed");
    return;
  }
  const std::string path = tmp_path("screen-" + std::to_string(t0) + ".png");
  const bool ok = write_file_atomically(path, png);
  const bool latest_ok = write_file_atomically(tmp_path(kScreenLatestFile), png);
  if (ok) {
    log_printf("screenshot: written %s (%llu ms, %dx%d, %zu bytes%s)", path.c_str(),
               static_cast<unsigned long long>(SDL_GetTicks64() - t0), w, h, png.size(),
               latest_ok ? ", also screen-latest.png" : "");
  }
}

void Ps5App::run_form_probe() {
  auto* view = static_cast<Ps5EngineView*>(controller_->active_view());
  if (!view) {
    log_line("probe: no active page");
    return;
  }
  const std::uint64_t t0 = SDL_GetTicks64();
  view->web().evaluate(form_probe_script(), [t0](std::optional<std::string> result) {
    if (!result) {
      log_line("probe: the page script returned no result (failed, or no page process)");
      return;
    }
    const auto doc = nlohmann::json::parse(*result, nullptr, /*allow_exceptions=*/false);
    std::string text = *result;
    if (!doc.is_discarded()) {
      text = doc.dump(2, ' ', false, nlohmann::json::error_handler_t::replace);
    }
    text += '\n';
    const std::string path = tmp_path("probe-" + std::to_string(t0) + ".json");
    const bool ok = write_file_atomically(path, text);
    write_file_atomically(tmp_path(kProbeLatestFile), text);
    if (ok) {
      const auto controls = doc.is_object() ? doc.find("controlCount") : doc.end();
      log_printf("probe: written %s (%llu ms, %s controls, %s)", path.c_str(),
                 static_cast<unsigned long long>(SDL_GetTicks64() - t0),
                 controls != doc.end() ? controls->dump().c_str() : "?",
                 doc.is_object() ? doc.value("url", std::string("?")).c_str() : "?");
    }
  });
}

void Ps5App::handle_axis_motion() {
  if (!pad_) {
    controller_->on_sticks(0, 0, 0, 0);
    trigger_edge(core::Button::L2, false, l2_down_);
    trigger_edge(core::Button::R2, false, r2_down_);
    return;
  }
  auto norm = [this](SDL_GameControllerAxis axis) {
    float v = SDL_GameControllerGetAxis(pad_, axis) / 32767.0f;
    if (v > 1.0f) v = 1.0f;
    if (v < -1.0f) v = -1.0f;
    if (v > -0.15f && v < 0.15f) v = 0.0f;  // deadzone
    return v;
  };
  controller_->on_sticks(norm(SDL_CONTROLLER_AXIS_LEFTX), norm(SDL_CONTROLLER_AXIS_LEFTY),
                         norm(SDL_CONTROLLER_AXIS_RIGHTX),
                         norm(SDL_CONTROLLER_AXIS_RIGHTY));

  // L2/R2 analog triggers act as buttons past half-press. Report only the
  // press/release edges: the shell runs an action on every press.
  trigger_edge(core::Button::L2,
               SDL_GameControllerGetAxis(pad_, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16000,
               l2_down_);
  trigger_edge(core::Button::R2,
               SDL_GameControllerGetAxis(pad_, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000,
               r2_down_);
}

void Ps5App::handle_touchpad(std::uint64_t now) {
  touchpad_.read(pad_, now, touch_contacts_);
  if (touchpad_.aspect() != touchpad_aspect_) {
    touchpad_aspect_ = touchpad_.aspect();
    controller_->set_touchpad_aspect(touchpad_aspect_);
  }
  controller_->on_touchpad(touch_contacts_, now);
}

void Ps5App::trigger_edge(core::Button button, bool down, bool& was_down) {
  if (down != was_down) {
    was_down = down;
    controller_->on_button(button, down);
  }
}

std::vector<display::Rect> Ps5App::derive_chrome_rects() {
  // The chrome renders asynchronously (WebProcess round trip), so its pixels
  // lag the shell state by a frame or two: keep composing the areas of the
  // last few frames, or a moving pointer or a closing panel would be cut off.
  std::vector<display::Rect> current;
  for (const auto& r : controller_->chrome_rects()) {
    current.push_back(display::Rect{r.x, r.y, r.width, r.height});
  }
  std::vector<display::Rect> rects = current;
  for (const auto& old : recent_chrome_rects_) {
    rects.insert(rects.end(), old.begin(), old.end());
  }
  recent_chrome_rects_.push_front(std::move(current));
  if (recent_chrome_rects_.size() > kChromeRectHistory) {
    recent_chrome_rects_.pop_back();
  }
  return rects;
}

void Ps5App::paint_chrome() {
  display::Region dirty = chrome_->web().take_dirty();
  webkit_painted_ = webkit_painted_ || !dirty.empty();
  if (!chrome_painted_) {
    dirty.clear();
    dirty.add(display::Rect{0, 0, width_, height_});
    chrome_painted_ = true;
  }
  dirty.clip(display::Rect{0, 0, width_, height_});
  for (const display::Rect& r : dirty.rects()) {
    // WebKit blends its backing store over the target (source-over), so the
    // area must start out transparent or old chrome pixels would show through.
    for (int y = r.y; y < r.bottom(); y++) {
      std::uint32_t* row = &chrome_buffer_[static_cast<size_t>(y) * width_];
      std::fill(row + r.x, row + r.right(), 0u);
    }
    const std::uint64_t t0 = SDL_GetPerformanceCounter();
    chrome_->web().paint(reinterpret_cast<unsigned char*>(chrome_buffer_.data()), width_,
                         height_, r);
    stats_.chrome_paint.add(ms_since(t0), r.area());
  }
  // Visible only inside the chrome rects; compose_damage() clips it.
  pending_chrome_damage_.add(dirty);
}

void Ps5App::paint_content() {
  auto* view = static_cast<Ps5EngineView*>(controller_->active_view());
  const std::uint64_t tab = controller_->tabs().active_id().value_or(0);
  const int top = std::clamp(controller_->content_top(), 0, height_);
  const int content_height = height_ - top;
  display::Region dirty = view ? view->web().take_dirty() : display::Region{};
  webkit_painted_ = webkit_painted_ || !dirty.empty();

  // A different view or geometry invalidates the whole cache (and screen);
  // otherwise only what WebKit reported changed is repainted (nothing on an
  // idle page).
  const bool full = tab != painted_tab_ || top != painted_top_;
  if (full) {
    std::fill(page_buffer_.begin(), page_buffer_.end(), 0xFF000000u);
    painted_tab_ = tab;
    painted_top_ = top;
    dirty.clear();
    dirty.add(display::Rect{0, 0, width_, content_height});
    pending_damage_.add(display::Rect{0, 0, width_, height_});
  }
  if (!view || dirty.empty() || content_height <= 0) {
    return;
  }
  dirty.clip(display::Rect{0, 0, width_, content_height});
  // The view is width_ x content_height and sits at window y = top, the same
  // offset ShellController::to_view() removes from pointer coordinates.
  auto* origin = reinterpret_cast<unsigned char*>(page_buffer_.data() +
                                                  static_cast<size_t>(top) * width_);
  for (const display::Rect& r : dirty.rects()) {
    if (video_overlay_) {
      // WebKit blends its backing store over the target (source-over): a
      // video's transparent hole must replace what was there, not show it.
      for (int y = r.y; y < r.bottom(); y++) {
        std::uint32_t* row = &page_buffer_[static_cast<size_t>(top + y) * width_];
        std::fill(row + r.x, row + r.right(), 0u);
      }
    }
    const std::uint64_t t0 = SDL_GetPerformanceCounter();
    view->web().paint(origin, width_, content_height, r);
    stats_.page_paint.add(ms_since(t0), r.area());
  }
  dirty.translate(0, top);
  pending_damage_.add(dirty);
}

void Ps5App::collect_and_log_stats(std::uint64_t now) {
  if (auto* view = static_cast<Ps5EngineView*>(controller_->active_view())) {
    stats_.page_needs_display.merge(view->web().take_needs_display_stats());
  }
  stats_.chrome_needs_display.merge(chrome_->web().take_needs_display_stats());
  if (stats_start_ticks_ == 0) {
    stats_start_ticks_ = now;
    return;
  }
  const std::uint64_t elapsed = now - stats_start_ticks_;
  if (elapsed < kStatsWindowMs) {
    return;
  }
  if (presenter_) {
    stats_.present.merge(presenter_->take_stats());
  }
  if (stats_.active()) {
    log_line(stats_.format(static_cast<double>(elapsed) / 1000.0));
  }
  stats_ = display::FrameStats{};
  stats_start_ticks_ = now;
}

display::Region Ps5App::take_frame_damage(const std::vector<display::Rect>& chrome_rects) {
  display::Region damage = pending_damage_;
  // A chrome rect that appeared, moved or went away changes what is blended
  // where: recompose the old and the new areas.
  if (chrome_rects != composed_chrome_rects_) {
    for (const display::Rect& r : composed_chrome_rects_) {
      damage.add(r);
    }
    for (const display::Rect& r : chrome_rects) {
      damage.add(r);
    }
  }
  // Chrome repaints show only inside the chrome rects.
  for (const display::Rect& d : pending_chrome_damage_.rects()) {
    for (const display::Rect& c : chrome_rects) {
      damage.add(display::intersect(d, c));
    }
  }
  damage.clip(display::Rect{0, 0, width_, height_});
  pending_damage_.clear();
  pending_chrome_damage_.clear();
  composed_chrome_rects_ = chrome_rects;
  return damage;
}

void Ps5App::composite_and_present() {
  SDL_Surface* surface = SDL_GetWindowSurface(window_);
  if (!surface_logged_) {
    surface_logged_ = true;
    if (surface) {
      log_printf("SDL_GetWindowSurface: %dx%d %s pitch %d", surface->w, surface->h,
                 SDL_GetPixelFormatName(surface->format->format), surface->pitch);
    } else {
      log_printf("SDL_GetWindowSurface failed: %s", SDL_GetError());
    }
  }
  if (!surface) {
    return;
  }

  // 1. Bring the content and chrome caches up to date (dirty areas only);
  //    their damage accumulates until it is composed.
  paint_content();
  paint_chrome();
  const std::vector<display::Rect> chrome_rects = derive_chrome_rects();
  // Video overlay frames of the active page: their rects need recomposing
  // whenever a new frame arrived or a player moved (no WebKit paint).
  const int top = std::clamp(controller_->content_top(), 0, height_);
  video::PageKey video_page;
  if (video_overlay_) {
    if (auto* view = static_cast<Ps5EngineView*>(controller_->active_view())) {
      video_page = Ps5VideoOverlay::page_key(view->web().page());
    }
    pending_damage_.add(video_overlay_->take_damage(video_page, top, width_, height_));
  }

  // Keep the splash until WebKit has painted something real; composing the
  // still empty caches would replace it with black.
  if (!splash_replaced_) {
    if (!webkit_painted_) {
      if (!presenter_->busy()) {
        // No ELF loader: a clear message instead of a progress bar that would
        // otherwise sit half-full forever (the web engine never starts).
        if (loader_missing_.load(std::memory_order_acquire)) {
          render_loader_missing(surface);
        } else {
          update_splash_progress(surface);
        }
      }
      return;
    }
    splash_replaced_ = true;
    pending_damage_.add(display::Rect{0, 0, width_, height_});
    log_printf("splash: replaced by the first WebKit frame (frame %llu)",
               static_cast<unsigned long long>(frames_));
  }

  // 2. The surface belongs to the presenter while a flip is in flight; the
  //    damage waits for the next frame (which the flip completion requests).
  if (presenter_->busy()) {
    if (!pending_damage_.empty() || !pending_chrome_damage_.empty()) {
      stats_.present_waits++;
    }
    return;
  }
  // The pointer goes over everything: recompose where it was and where it
  // is now when it moved, appeared or went away.
  std::optional<display::Rect> pointer;
  const core::Vec2 pointer_pos = controller_->pointer();
  const int pointer_x = static_cast<int>(pointer_pos.x);
  const int pointer_y = static_cast<int>(pointer_pos.y);
  if (controller_->pointer_visible()) {
    pointer = display::pointer_rect(pointer_x, pointer_y);
  }
  if (pointer != composed_pointer_) {
    if (composed_pointer_) {
      pending_damage_.add(*composed_pointer_);
    }
    if (pointer) {
      pending_damage_.add(*pointer);
    }
    composed_pointer_ = pointer;
  }
  const display::Region damage = take_frame_damage(chrome_rects);
  if (damage.empty()) {
    if (screenshot_pending_) {
      dump_screen(surface, &chrome_rects, top, video_page);
    }
    return;
  }

  // 3. Compose the damaged rects into the ABGR8888 window surface: the page,
  //    with the premultiplied chrome blended over it inside the chrome rects.
  //    Neither cache is written here, so a closed overlay leaves no trace.
  const std::uint64_t compose_t0 = SDL_GetPerformanceCounter();
  if (SDL_MUSTLOCK(surface)) {
    SDL_LockSurface(surface);
  }
  const display::Layers layers{page_buffer_.data(), chrome_buffer_.data(), width_, height_};
  const int out_w = std::min(width_, surface->w);
  const int out_h = std::min(height_, surface->h);
  // The overlay's frames stay locked (not swapped by its thread) while used.
  std::optional<video::VideoServer::Frames> video_frames;
  std::vector<display::VideoLayer> videos;
  if (video_overlay_ && video_page.pid) {
    video_frames.emplace(video_overlay_->acquire(video_page));
    videos = Ps5VideoOverlay::layers(*video_frames, top, width_, height_);
    if (video_frames->new_frames()) {
      std::uint64_t area = 0;
      for (const auto& v : videos) {
        area += display::intersect(v.rect, v.clip).area();
      }
      stats_.video_frames.add(area);
    }
  }
  std::vector<SDL_Rect> present_rects;
  for (const display::Rect& r : damage.rects()) {
    const display::Rect clipped = display::intersect(r, display::Rect{0, 0, out_w, out_h});
    if (clipped.empty()) {
      continue;
    }
    display::compose_rect(layers, chrome_rects, clipped, static_cast<std::uint32_t*>(surface->pixels),
                          surface->pitch / 4, videos);
    if (pointer) {
      display::draw_pointer(static_cast<std::uint32_t*>(surface->pixels), surface->pitch / 4,
                            clipped, pointer_x, pointer_y);
    }
    present_rects.push_back(SDL_Rect{clipped.x, clipped.y, clipped.w, clipped.h});
  }
  video_frames.reset();
  stats_.compose.add(ms_since(compose_t0), damage.area());
  // The composed frame, before the flip hands the buffer to the display.
  if (screenshot_pending_) {
    dump_screen(surface, &chrome_rects, top, video_page);
  }
  if (SDL_MUSTLOCK(surface)) {
    SDL_UnlockSurface(surface);
  }
  if (present_rects.empty()) {
    return;
  }
  // Under the SDL PS5 driver this de-tiles the rects' bounding box and
  // submits the flip (sceVideoOutSubmitFlip), see ps5_presenter.h.
  stats_.damage_rects += static_cast<std::uint32_t>(present_rects.size());
  presenter_->submit(std::move(present_rects));
}

}  // namespace prosperoweb::frontend

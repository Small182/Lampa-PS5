// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "prosperoweb/core/addons_config.h"
#include "prosperoweb/core/bookmarks.h"
#include "prosperoweb/core/drm_policy.h"
#include "prosperoweb/core/fullscreen_controller.h"
#include "prosperoweb/core/gamepad.h"
#include "prosperoweb/core/gamepad_mapper.h"
#include "prosperoweb/core/history.h"
#include "prosperoweb/core/on_screen_keyboard.h"
#include "prosperoweb/core/pointer_model.h"
#include "prosperoweb/core/repeat_timer.h"
#include "prosperoweb/core/session.h"
#include "prosperoweb/core/storage.h"
#include "prosperoweb/core/tab_manager.h"
#include "prosperoweb/core/touchpad_model.h"
#include "prosperoweb/shell/addons.h"
#include "prosperoweb/shell/chrome_ui.h"
#include "prosperoweb/shell/engine_view.h"
#include "prosperoweb/shell/text_input.h"

namespace prosperoweb::shell {

// Engine-independent browser logic: tabs, navigation, gamepad handling,
// on-screen keyboard, fullscreen, menu, DRM notices and session persistence.
// The frontend feeds it input (on_button, on_sticks, on_mouse_*, tick) and
// chrome events, and implements EngineFactory/EngineView and ChromeUi.
class ShellController {
 public:
  // Height of the top band (tab strip + address bar) in window pixels.
  static constexpr int kChromeBandHeight = 96;
  // D-pad scroll step in px.
  static constexpr int kScrollStep = 120;
  // Fraction of the viewport scrolled by L2/R2.
  static constexpr float kPageScrollFraction = 0.85f;
  // How long after a Cross click a text_input_focused(true) opens the OSK.
  static constexpr float kClickFocusWindowSeconds = 1.0f;
  static constexpr std::size_t kMenuHistoryItems = 30;
  // Tabs and history are saved at most this long after they change, so a
  // console app kill (PS button, crash) loses little; shutdown() saves too.
  static constexpr float kSessionSaveDelaySeconds = 5.0f;
  // Auto-repeat of held OSK buttons (D-pad key moves, Square, L1/R1).
  static constexpr std::uint32_t kOskRepeatDelayMs = 350;
  static constexpr std::uint32_t kOskRepeatIntervalMs = 50;
  // In element fullscreen the pointer hides after this long without
  // pointer input (stick, touchpad, buttons) and shows on the next one.
  static constexpr float kFullscreenPointerHideSeconds = 3.0f;

  struct Config {
    int width = 1920;
    int height = 1080;
    std::string home_url = "about:blank";
    // PS5 Lampa launcher mode; disabled by default to preserve shell tests.
    bool kiosk_mode = false;
    std::string user_agent;
    std::string language = "en";  // "en" or "ru"
    // Input/feature probe page, offered as "Diagnostics" in the main menu
    // (opens in a new tab); no menu item when empty.
    std::string diagnostics_url;
    // Unix time source for history/bookmarks; defaults to time(nullptr).
    std::function<std::int64_t()> now;
    // Text entry front end (e.g. the PS5 system keyboard dialog); must
    // outlive the controller. nullptr: the built-in HTML OSK. The built-in
    // OSK is also the fallback when the provider reports an error.
    TextInputProvider* text_input = nullptr;
    // The frontend draws the gamepad pointer itself (the PS5 compositor):
    // chrome_rects() leaves it out and the state marks it "native", so the
    // HTML chrome never repaints for pointer motion.
    bool native_pointer = false;
    // Initial add-ons state (from config.json). Applied through addons_backend
    // as content views are created by the frontend.
    core::AddonsConfig addons;
    // Engine-side application of the add-on toggles; nullptr disables the
    // Add-ons menu. Must outlive the controller.
    AddonsBackend* addons_backend = nullptr;
    // Persists the add-ons state after a toggle (the frontend writes
    // config.json). Called with the new state.
    std::function<void(const core::AddonsConfig&)> on_addons_changed;
    // Natural (content-follows-finger) touchpad scrolling. Default false
    // (mouse-wheel direction). Applied to the touchpad model at startup.
    bool natural_scroll = false;
    // Persists the natural-scroll setting on toggle; nullptr hides the menu
    // entry. Must outlive the controller.
    std::function<void(bool)> on_natural_scroll_changed;
  };

  enum class Focus : std::uint8_t { Page, Chrome, Osk, Menu, Notice };
  enum class OskTarget : std::uint8_t { Address, Page };
  enum class MenuSection : std::uint8_t { Main, Bookmarks, History, Adblock, SponsorBlock };

  ShellController(EngineFactory& factory, ChromeUi& chrome, core::Storage& storage,
                  Config config);
  ~ShellController();

  ShellController(const ShellController&) = delete;
  ShellController& operator=(const ShellController&) = delete;

  // Restores the saved session (only the active tab is loaded right away,
  // the others load when first activated) or opens the home page.
  void start();
  // Saves tabs, history and bookmarks (tabs and history are also saved
  // from tick(), kSessionSaveDelaySeconds after a change).
  void shutdown();

  // Gamepad input.
  void on_button(core::Button button, bool pressed);
  // Left stick moves the pointer, right stick scrolls; values in [-1, 1].
  void on_sticks(float lx, float ly, float rx, float ry);
  // Touchpad as a trackpad (core::TouchpadModel): every finger now on the
  // pad, once per frame before tick(); now_ms is a monotonic clock. One
  // finger moves the pointer, a tap clicks, two fingers scroll the page.
  // Only while the page has focus. The pad's button arrives through
  // on_button(Button::Touchpad): on the page it is a mouse press held
  // until the button is released (press and slide = drag).
  void on_touchpad(std::span<const core::TouchContact> contacts, std::uint64_t now_ms);
  // Pad height / width (its resolution), see TouchpadModel::set_aspect().
  void set_touchpad_aspect(float aspect) { touchpad_.set_aspect(aspect); }
  // Advances pointer motion, scrolling and key repeat; renders the chrome
  // if anything changed.
  void tick(float dt_seconds);

  // Host mouse passthrough (window coordinates). The real mouse goes to the
  // engine directly on the host; these keep the pointer model in sync and
  // hide the gamepad pointer while the mouse is in use.
  void on_mouse_move(float x, float y);

  // Events from the HTML chrome:
  //   {type:'ready'}                       chrome loaded, re-render
  //   {type:'navigate', text}              address bar submit
  //   {type:'nav', action:'back'|'forward'|'reload'|'stop'}
  //   {type:'tab_select', id} {type:'tab_close', id} {type:'tab_new'}
  //   {type:'menu_open'} {type:'menu_select', index} {type:'menu_close'}
  //   {type:'notice_dismiss', proceed}
  //   {type:'osk_key', row, col} {type:'osk_commit'} {type:'osk_cancel'}
  //   {type:'address_focus'} {type:'address_blur'}
  // event is a JSON object; a JSON string holding such an object is decoded
  // first. Anything else is ignored.
  void on_chrome_event(const nlohmann::json& event);

  // Called by the frontend (on the shell thread) when a background filter-list
  // update finishes successfully: records the timestamp, persists it and
  // refreshes the Add-ons menu if it is open.
  void on_filters_updated(std::int64_t when);

  // Opens url in a new tab (shows the tab-limit notice when full).
  bool open_url_in_new_tab(const std::string& url);
  // Navigates the active tab to address-bar input (normalized).
  void navigate(const std::string& input);

  void set_viewport(int width, int height);

  // A window-space rectangle.
  struct Rect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
  };
  // Window areas where the chrome may draw right now: the top band, the OSK
  // panel, the whole window under a menu or notice, and the gamepad pointer.
  // Compositing frontends blend the chrome over the page only inside these
  // (published as "chrome_rects" in chrome_state()).
  std::vector<Rect> chrome_rects() const;

  // Builds the chrome state document (see ChromeUi).
  nlohmann::json chrome_state() const;
  // Renders the chrome now, regardless of the dirty flag.
  void render_now();

  // Introspection (frontends and tests).
  Focus focus() const { return focus_; }
  core::Mode mode() const;
  const core::TabManager& tabs() const { return tabs_; }
  const core::History& history() const { return history_; }
  const core::Bookmarks& bookmarks() const { return bookmarks_; }
  const core::FullscreenController& fullscreen() const { return fullscreen_; }
  const core::OnScreenKeyboard& osk() const { return osk_; }
  core::Vec2 pointer() const { return pointer_.position(); }
  // True while the gamepad pointer is shown: page focus, not hidden by mouse
  // use, the active page's cursor is not `none`, and not idle for
  // kFullscreenPointerHideSeconds in element fullscreen. Frontends with
  // Config::native_pointer draw it at pointer().
  bool pointer_visible() const;
  // The cursor the active page last asked for.
  PageCursor page_cursor() const;
  // Window y of the content view's top edge: the chrome band height while
  // the chrome is visible, else 0. The content view is width x (height -
  // content_top()) and page coordinates are window coordinates minus this.
  int content_top() const;
  EngineView* active_view();
  EngineView* view_for(std::uint64_t tab_id);
  bool quit_requested() const { return quit_requested_; }
  // Whether natural touchpad scrolling is active (reflects the touchpad model).
  bool natural_scroll() const { return touchpad_.natural_scroll(); }
  // True while the active page reports a focused editable element; hosts
  // with a physical keyboard use it to stop treating letters as buttons.
  bool text_input_focused() const { return text_input_focused_; }
  // True while a TextInputProvider request is running (the shell ignores
  // gamepad input and keeps its focus meanwhile).
  bool text_input_active() const { return text_request_.has_value(); }

 private:
  class TabClient;
  friend class TabClient;

  struct TabEntry {
    std::unique_ptr<TabClient> client;
    std::unique_ptr<EngineView> view;
    bool loaded = false;  // a URL was sent to the view
    PageCursor cursor = PageCursor::Pointer;
  };

  struct Notice {
    std::string kind;  // "drm", "error", "info"
    std::string title;
    std::string text;
  };

  struct PendingNavigation {
    std::uint64_t tab_id = 0;
    std::string url;
  };

  // The focused page field as reported by core::kFocusReporterScript
  // ({type:'focus_info'}); value is unset when the script did not send it
  // (passwords, contenteditable, very long values).
  struct FocusedField {
    std::string type;
    std::optional<std::string> value;
  };

  // A running TextInputProvider request.
  struct TextRequest {
    OskTarget target = OskTarget::Address;
    std::uint64_t tab_id = 0;
    std::string initial;
    bool replace = false;  // the result replaces the field's whole value
  };

  struct MenuItem {
    std::string label;
    std::string url;
    std::function<void()> run;
  };

  // Engine callbacks (per tab).
  void on_url_changed(std::uint64_t id, const std::string& url);
  void on_title_changed(std::uint64_t id, const std::string& title);
  void on_load_started(std::uint64_t id);
  void on_load_finished(std::uint64_t id);
  void on_load_failed(std::uint64_t id, const std::string& url, const std::string& error);
  void on_nav_state(std::uint64_t id, bool back, bool fwd);
  void on_fullscreen_entered(std::uint64_t id);
  void on_fullscreen_exited(std::uint64_t id);
  void on_script_message(std::uint64_t id, const std::string& json);
  void on_new_window(std::uint64_t id, const std::string& url);
  void on_text_input_focused(std::uint64_t id, bool focused);
  void on_cursor_changed(std::uint64_t id, PageCursor cursor);

  // Actions.
  void run_action(core::Action action);
  void run_page_action(core::Action action);
  void run_chrome_action(core::Action action);
  void run_menu_action(core::Action action);
  void run_osk_action(core::Action action);
  void handle_notice_button(core::Button button);

  // Tabs and engines.
  std::optional<std::uint64_t> open_tab(const std::string& url, bool load_now);
  void close_tab(std::uint64_t id);
  void activate_tab(std::uint64_t id);
  void after_tab_switch();
  TabEntry& ensure_entry(std::uint64_t id);
  void load_in_tab(std::uint64_t id, const std::string& url);
  void load_in_tab_now(std::uint64_t id, const std::string& url);
  void apply_geometry();
  void sync_visibility();

  // Input helpers.
  void click_at_pointer();
  // The pad's button on the page: mouse down now, up on release.
  void press_at_pointer();
  void release_pointer_press();
  // Sends the pointer position to the page if its integer position changed
  // since the last move sent to the active tab (see tick()).
  void send_page_pointer_move();
  // Moves the pointer ring through ChromeUi::pointer() when it changed.
  void sync_chrome_pointer();
  void note_page_pointer(std::uint64_t tab_id, int x, int y);
  void scroll_active(int dx, int dy);
  core::Vec2 to_view(core::Vec2 window_pos) const;

  // Overlays.
  // Text entry for the address bar or the focused page field: the provider
  // when configured, else (or when it cannot start) the built-in OSK.
  void request_text_entry(OskTarget target);
  void on_text_input_result(const TextInputResult& result);
  // Navigates (address) or puts text into the focused field (page); with
  // replace the field's current value is selected first so it is replaced.
  void commit_text(OskTarget target, std::uint64_t tab_id, const std::string& text,
                   bool replace);
  void open_osk(OskTarget target, const std::string& text, bool replace = false);
  void close_osk();
  void commit_osk();
  void open_menu();
  void show_menu_section(MenuSection section);
  void close_menu();
  void activate_menu_item(std::size_t index);
  // Add-ons menu helpers.
  void build_adblock_menu();
  void build_sponsorblock_menu();
  void apply_addons();
  void persist_addons();
  // Rebuilds the current menu section but keeps the selection on `keep`.
  void rebuild_menu_keeping(std::size_t keep);
  void show_notice(Notice notice);
  void dismiss_notice(bool proceed);
  void toggle_bookmark();
  void toggle_fullscreen();
  void exit_fullscreen();
  void set_focus(Focus focus);
  void stop_repeat();
  core::RepeatTimer& held_timer() { return held_in_osk_ ? osk_repeat_ : repeat_; }

  std::int64_t now() const;
  core::Language language() const;
  void mark_dirty() { dirty_ = true; }
  void mark_session_dirty() { session_dirty_ = true; }
  void save_session_if_due(float dt_seconds);

  EngineFactory& factory_;
  ChromeUi& chrome_;
  core::Session session_;
  Config config_;

  core::TabManager tabs_;
  core::History history_;
  core::Bookmarks bookmarks_;
  core::GamepadMapper mapper_;
  core::PointerModel pointer_;
  core::TouchpadModel touchpad_;
  // The tab holding a mouse press from the pad's button (see on_button()).
  std::optional<std::uint64_t> pressed_tab_;
  core::RepeatTimer repeat_;
  core::RepeatTimer osk_repeat_{kOskRepeatDelayMs, kOskRepeatIntervalMs};
  core::OnScreenKeyboard osk_;
  core::FullscreenController fullscreen_;
  core::DrmPolicy drm_;
  core::AddonsConfig addons_;

  std::map<std::uint64_t, TabEntry> entries_;
  std::optional<std::uint64_t> visible_tab_;

  Focus focus_ = Focus::Page;
  Focus focus_before_notice_ = Focus::Page;
  OskTarget osk_target_ = OskTarget::Address;
  bool osk_open_ = false;
  bool osk_replace_ = false;  // commit replaces the page field's value
  std::optional<FocusedField> focused_field_;
  std::optional<TextRequest> text_request_;
  bool menu_open_ = false;
  MenuSection menu_section_ = MenuSection::Main;
  std::vector<MenuItem> menu_items_;
  std::size_t menu_selected_ = 0;
  std::optional<Notice> notice_;
  std::optional<PendingNavigation> pending_nav_;

  // Held button for auto-repeat; held_in_osk_ selects osk_repeat_.
  std::optional<core::Button> held_button_;
  bool held_in_osk_ = false;
  float repeat_carry_ms_ = 0.0f;

  float sticks_[4] = {0, 0, 0, 0};
  core::Vec2 scroll_carry_;
  bool pointer_hidden_by_mouse_ = false;
  bool pointer_moved_ = false;  // the pointer moved during this tick
  // Seconds since the last pointer input (fullscreen auto-hide).
  float pointer_idle_seconds_ = 0.0f;
  // The last pointer_move() sent to a page (coalescing, see tick()).
  struct PagePointer {
    std::uint64_t tab_id = 0;
    int x = 0;
    int y = 0;
  };
  std::optional<PagePointer> page_pointer_;
  // The last ChromeUi::pointer() call.
  struct ChromePointer {
    int x = 0;
    int y = 0;
    bool visible = false;
    bool operator==(const ChromePointer&) const = default;
  };
  std::optional<ChromePointer> chrome_pointer_;
  float since_click_ = -1.0f;  // seconds since the last Cross click, <0 = none
  bool text_input_focused_ = false;
  bool quit_requested_ = false;
  bool dirty_ = true;
  bool session_dirty_ = false;
  float since_session_change_ = 0.0f;
};

}  // namespace prosperoweb::shell

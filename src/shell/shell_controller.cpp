// SPDX-License-Identifier: GPL-3.0-or-later
#include "prosperoweb/shell/shell_controller.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <utility>

#include "prosperoweb/core/url_normalizer.h"

namespace prosperoweb::shell {

using core::Action;
using core::Button;
using core::Mode;
using core::OnScreenKeyboard;
using FsState = core::FullscreenController::State;
using FsEffect = core::FullscreenController::Effect;
using json = nlohmann::json;

namespace {

// "scheme://host" for http(s) URLs, the URL itself otherwise.
std::string origin_of(const std::string& url) {
  const auto sep = url.find("://");
  if (sep == std::string::npos) {
    return url;
  }
  return url.substr(0, sep) + "://" + core::UrlNormalizer::display_host(url);
}

// Selects the focused field's whole value, so the following insertText
// replaces it (the text input was pre-filled with that value).
constexpr const char* kSelectFieldScript =
    "(function(){var e=document.activeElement;if(!e)return;"
    "try{if(typeof e.select==='function'){e.select();return;}}catch(x){}"
    "document.execCommand('selectAll',false);})()";

bool is_internal_url(const std::string& url) {
  return url.empty() || url.starts_with("about:") || url.starts_with("file:");
}

const char* focus_name(ShellController::Focus focus) {
  switch (focus) {
    case ShellController::Focus::Page: return "page";
    case ShellController::Focus::Chrome: return "chrome";
    case ShellController::Focus::Osk: return "osk";
    case ShellController::Focus::Menu: return "menu";
    case ShellController::Focus::Notice: return "notice";
  }
  return "page";
}

const char* layout_name(OnScreenKeyboard::Layout layout) {
  switch (layout) {
    case OnScreenKeyboard::Layout::Latin: return "latin";
    case OnScreenKeyboard::Layout::Symbols: return "symbols";
    case OnScreenKeyboard::Layout::Cyrillic: return "cyrillic";
  }
  return "latin";
}

std::pair<const char*, double> key_code_and_width(const OnScreenKeyboard::Key& key) {
  using Kind = OnScreenKeyboard::KeyKind;
  switch (key.kind) {
    case Kind::Char: return {nullptr, 1.0};
    case Kind::Shift: return {"shift", 1.5};
    case Kind::LayoutSwitch: return {"layout", 1.5};
    case Kind::Space: return {"space", 3.0};
    case Kind::Backspace: return {"backspace", 1.5};
    case Kind::Enter: return {"enter", 1.5};
    case Kind::Clear: return {"clear", 1.5};
    case Kind::CaretLeft: return {"left", 1.0};
    case Kind::CaretRight: return {"right", 1.0};
  }
  return {nullptr, 1.0};
}

const char* fullscreen_name(FsState state) {
  switch (state) {
    case FsState::Normal: return "normal";
    case FsState::ChromeHidden: return "chrome_hidden";
    case FsState::ElementFullscreen: return "element";
  }
  return "normal";
}

bool is_direction(Button b) {
  return b == Button::Up || b == Button::Down || b == Button::Left || b == Button::Right;
}

// OSK buttons that repeat while held: key-grid moves, Backspace and the
// caret. Holding Square only deletes faster; clearing stays an explicit key.
bool is_osk_repeatable(Button b) {
  return is_direction(b) || b == Button::Square || b == Button::L1 || b == Button::R1;
}

}  // namespace

// Forwards engine callbacks of one tab to the controller, tagged with the
// tab id.
class ShellController::TabClient final : public EngineViewClient {
 public:
  TabClient(ShellController& owner, std::uint64_t id) : owner_(owner), id_(id) {}

  void url_changed(const std::string& url) override { owner_.on_url_changed(id_, url); }
  void title_changed(const std::string& title) override { owner_.on_title_changed(id_, title); }
  void load_started() override { owner_.on_load_started(id_); }
  void load_finished() override { owner_.on_load_finished(id_); }
  void load_failed(const std::string& url, const std::string& error_text) override {
    owner_.on_load_failed(id_, url, error_text);
  }
  void nav_state_changed(bool can_go_back, bool can_go_forward) override {
    owner_.on_nav_state(id_, can_go_back, can_go_forward);
  }
  void fullscreen_entered() override { owner_.on_fullscreen_entered(id_); }
  void fullscreen_exited() override { owner_.on_fullscreen_exited(id_); }
  void script_message(const std::string& json_text) override {
    owner_.on_script_message(id_, json_text);
  }
  void new_window_requested(const std::string& url) override { owner_.on_new_window(id_, url); }
  void text_input_focused(bool focused) override { owner_.on_text_input_focused(id_, focused); }
  void cursor_changed(PageCursor cursor) override { owner_.on_cursor_changed(id_, cursor); }

 private:
  ShellController& owner_;
  std::uint64_t id_;
};

ShellController::ShellController(EngineFactory& factory, ChromeUi& chrome, core::Storage& storage,
                                 Config config)
    : factory_(factory),
      chrome_(chrome),
      session_(storage),
      config_(std::move(config)),
      pointer_(static_cast<float>(config_.width), static_cast<float>(config_.height)) {
  pointer_.set_position({config_.width / 2.0f, config_.height / 2.0f});
  addons_ = config_.addons;
  touchpad_.set_natural_scroll(config_.natural_scroll);
}

ShellController::~ShellController() {
  // Views first: they may reference their clients until destroyed.
  for (auto& [id, entry] : entries_) {
    entry.view.reset();
  }
}

// ---------------------------------------------------------------------------
// Lifecycle

void ShellController::start() {
  session_.load_history(history_);
  session_.load_bookmarks(bookmarks_);
  // Lampa kiosk: open the fixed HTTP homepage on every launch.
  open_tab(config_.home_url, true);
  fullscreen_.toggle_chrome();
  apply_geometry();
  render_now();
}

void ShellController::shutdown() {
  session_.save_all(tabs_, history_, bookmarks_);
  session_dirty_ = false;
}

void ShellController::save_session_if_due(float dt_seconds) {
  if (!session_dirty_) {
    return;
  }
  since_session_change_ += dt_seconds;
  if (since_session_change_ < kSessionSaveDelaySeconds) {
    return;
  }
  session_.save_tabs(tabs_);
  session_.save_history(history_);
  session_dirty_ = false;
  since_session_change_ = 0.0f;
}

std::int64_t ShellController::now() const {
  return config_.now ? config_.now() : static_cast<std::int64_t>(std::time(nullptr));
}

core::Language ShellController::language() const {
  return config_.language == "ru" ? core::Language::Ru : core::Language::En;
}

void ShellController::set_viewport(int width, int height) {
  config_.width = width;
  config_.height = height;
  pointer_.set_viewport(static_cast<float>(width), static_cast<float>(height));
  apply_geometry();
  mark_dirty();
}

// ---------------------------------------------------------------------------
// Tabs and engines

ShellController::TabEntry& ShellController::ensure_entry(std::uint64_t id) {
  auto it = entries_.find(id);
  if (it != entries_.end()) {
    return it->second;
  }
  TabEntry entry;
  entry.client = std::make_unique<TabClient>(*this, id);
  entry.view = factory_.create(*entry.client);
  if (!config_.user_agent.empty()) {
    entry.view->set_user_agent(config_.user_agent);
  }
  entry.view->set_visible(false);
  auto& stored = entries_.emplace(id, std::move(entry)).first->second;
  apply_geometry();
  return stored;
}

EngineView* ShellController::view_for(std::uint64_t tab_id) {
  auto it = entries_.find(tab_id);
  return it == entries_.end() ? nullptr : it->second.view.get();
}

EngineView* ShellController::active_view() {
  const auto id = tabs_.active_id();
  return id ? view_for(*id) : nullptr;
}

std::optional<std::uint64_t> ShellController::open_tab(const std::string& url, bool load_now) {
  if (tabs_.full()) {
    show_notice({"error", "Tab limit reached",
                 "Close a tab before opening a new one (at most " +
                     std::to_string(core::TabManager::kMaxTabs) + " tabs)."});
    return std::nullopt;
  }
  const auto id = tabs_.open(url);
  if (!id) {
    return std::nullopt;
  }
  ensure_entry(*id);
  after_tab_switch();
  if (load_now && !entries_[*id].loaded) {
    load_in_tab(*id, url);
  }
  mark_dirty();
  mark_session_dirty();
  return id;
}

bool ShellController::open_url_in_new_tab(const std::string& url) {
  return open_tab(core::UrlNormalizer::normalize(url), true).has_value();
}

void ShellController::close_tab(std::uint64_t id) {
  if (!tabs_.find(id)) {
    return;
  }
  if (pending_nav_ && pending_nav_->tab_id == id) {
    pending_nav_.reset();
  }
  if (tabs_.active_id() == id && fullscreen_.state() == FsState::ElementFullscreen) {
    fullscreen_.user_exit();
  }
  auto it = entries_.find(id);
  if (it != entries_.end()) {
    it->second.view.reset();
    entries_.erase(it);
  }
  if (visible_tab_ == id) {
    visible_tab_.reset();
  }
  tabs_.close(id);
  mark_session_dirty();
  if (tabs_.empty()) {
    open_tab(config_.home_url, true);
  } else {
    after_tab_switch();
  }
  mark_dirty();
}

void ShellController::activate_tab(std::uint64_t id) {
  if (tabs_.active_id() == id || !tabs_.activate(id)) {
    return;
  }
  after_tab_switch();
}

void ShellController::after_tab_switch() {
  if (fullscreen_.state() == FsState::ElementFullscreen && visible_tab_) {
    if (auto* old = view_for(*visible_tab_)) {
      old->request_exit_fullscreen();
    }
    fullscreen_.user_exit();
  }
  if (osk_open_ && osk_target_ == OskTarget::Page) {
    close_osk();
  }
  text_input_focused_ = false;
  focused_field_.reset();
  if (const auto* tab = tabs_.active()) {
    auto& entry = ensure_entry(tab->id);
    if (!entry.loaded && !tab->url.empty()) {
      load_in_tab(tab->id, tab->url);
    }
  }
  sync_visibility();
  apply_geometry();
  mark_dirty();
  mark_session_dirty();
}

void ShellController::sync_visibility() {
  const auto active = tabs_.active_id();
  if (visible_tab_ == active) {
    return;
  }
  if (visible_tab_) {
    if (auto* old = view_for(*visible_tab_)) {
      old->set_visible(false);
    }
  }
  visible_tab_ = active;
  if (active) {
    if (auto* view = view_for(*active)) {
      view->set_visible(true);
    }
  }
}

void ShellController::apply_geometry() {
  const int height = std::max(1, config_.height - content_top());
  for (auto& [id, entry] : entries_) {
    entry.view->set_size(config_.width, height);
  }
}

void ShellController::navigate(const std::string& input) {
  const std::string url = core::UrlNormalizer::normalize(input);
  const auto id = tabs_.active_id();
  if (!id) {
    open_tab(url, true);
    return;
  }
  load_in_tab(*id, url);
}

void ShellController::load_in_tab(std::uint64_t id, const std::string& url) {
  const std::string host = core::UrlNormalizer::display_host(url);
  if (!is_internal_url(url) && core::DrmPolicy::is_known_widevine_only(host) &&
      drm_.should_notify(origin_of(url))) {
    pending_nav_ = PendingNavigation{id, url};
    tabs_.update_url(id, url);
    show_notice({"drm",
                 language() == core::Language::Ru ? "Защищённое видео не поддерживается"
                                                  : "Protected video is not supported",
                 std::string(core::DrmPolicy::notice_text(language()))});
    return;
  }
  load_in_tab_now(id, url);
}

void ShellController::load_in_tab_now(std::uint64_t id, const std::string& url) {
  auto& entry = ensure_entry(id);
  entry.loaded = true;
  tabs_.update_url(id, url);
  entry.view->load(url);
  mark_dirty();
  mark_session_dirty();
}

// ---------------------------------------------------------------------------
// Engine callbacks

void ShellController::on_url_changed(std::uint64_t id, const std::string& url) {
  if (tabs_.update_url(id, url)) {
    mark_dirty();
    mark_session_dirty();
  }
}

void ShellController::on_title_changed(std::uint64_t id, const std::string& title) {
  if (tabs_.update_title(id, title)) {
    mark_dirty();
    mark_session_dirty();
  }
}

void ShellController::on_load_started(std::uint64_t id) {
  if (tabs_.set_loading(id, true)) {
    mark_dirty();
  }
}

void ShellController::on_load_finished(std::uint64_t id) {
  tabs_.set_loading(id, false);
  if (const auto* tab = tabs_.find(id); tab && !is_internal_url(tab->url)) {
    history_.add(tab->url, tab->title, now());
  }
  mark_dirty();
  mark_session_dirty();
}

void ShellController::on_load_failed(std::uint64_t id, const std::string& url,
                                     const std::string& error) {
  tabs_.set_loading(id, false);
  if (tabs_.active_id() == id) {
    show_notice({"error", "Could not load the page", url + "\n" + error});
  }
  mark_dirty();
}

void ShellController::on_nav_state(std::uint64_t id, bool back, bool fwd) {
  if (tabs_.set_nav_state(id, back, fwd)) {
    mark_dirty();
  }
}

void ShellController::on_fullscreen_entered(std::uint64_t id) {
  if (tabs_.active_id() != id) {
    if (auto* view = view_for(id)) {
      view->request_exit_fullscreen();
    }
    return;
  }
  fullscreen_.page_entered_fullscreen();
  apply_geometry();
  mark_dirty();
}

void ShellController::on_fullscreen_exited(std::uint64_t id) {
  if (tabs_.active_id() != id) {
    return;
  }
  fullscreen_.page_exited_fullscreen();
  apply_geometry();
  mark_dirty();
}

void ShellController::on_cursor_changed(std::uint64_t id, PageCursor cursor) {
  auto it = entries_.find(id);
  if (it == entries_.end() || it->second.cursor == cursor) {
    return;
  }
  // The page decides (like a desktop browser hiding the mouse cursor over
  // `cursor: none`); pages that hide it usually show it again on the next
  // mouse move, which the stick and touchpad send.
  it->second.cursor = cursor;
}

void ShellController::on_script_message(std::uint64_t id, const std::string& text) {
  const json msg = json::parse(text, nullptr, /*allow_exceptions=*/false);
  if (!msg.is_object()) {
    return;
  }
  const std::string type = msg.value("type", std::string());
  if (type == "focus_info") {
    // Sent right before the {type:'focus', editable:true} report that
    // on_text_input_focused() receives.
    if (tabs_.active_id() == id) {
      FocusedField field;
      field.type = msg.value("inputType", std::string());
      if (const auto it = msg.find("value"); it != msg.end() && it->is_string()) {
        field.value = it->get<std::string>();
      }
      focused_field_ = std::move(field);
    }
    return;
  }
  if (type != "eme") {
    return;
  }
  const std::string origin = msg.value("origin", std::string());
  const std::string key_system = msg.value("keySystem", std::string());
  if (drm_.record_eme_request(origin, key_system)) {
    show_notice({"drm",
                 language() == core::Language::Ru ? "Защищённое видео не поддерживается"
                                                  : "Protected video is not supported",
                 std::string(core::DrmPolicy::notice_text(language()))});
  }
}

void ShellController::on_new_window(std::uint64_t /*id*/, const std::string& url) {
  open_tab(url.empty() ? std::string("about:blank") : url, true);
}

void ShellController::on_text_input_focused(std::uint64_t id, bool focused) {
  if (tabs_.active_id() != id) {
    return;
  }
  text_input_focused_ = focused;
  if (!focused) {
    focused_field_.reset();
    return;
  }
  if (focus_ == Focus::Page && !text_request_ && since_click_ >= 0.0f &&
      since_click_ <= kClickFocusWindowSeconds) {
    since_click_ = -1.0f;
    request_text_entry(OskTarget::Page);
  }
}

// ---------------------------------------------------------------------------
// Input

core::Mode ShellController::mode() const {
  switch (focus_) {
    case Focus::Page: return Mode::Page;
    case Focus::Osk: return Mode::Osk;
    case Focus::Chrome:
    case Focus::Menu:
    case Focus::Notice: return Mode::Chrome;
  }
  return Mode::Page;
}

void ShellController::on_button(Button button, bool pressed) {
  if (text_request_) {
    return;  // the text input provider owns the gamepad meanwhile
  }
  if (pressed) {
    pointer_idle_seconds_ = 0.0f;
  }
  if (button == Button::Touchpad) {
    touchpad_.set_pressed(pressed);
    if (!pressed && pressed_tab_) {
      release_pointer_press();
      return;
    }
    // On the page the pad's button is the mouse button: held, it drags.
    if (pressed && focus_ == Focus::Page && mapper_.map(button, true, mode()) == Action::Click) {
      press_at_pointer();
      return;
    }
  }
  if (!pressed) {
    if (held_button_ == button) {
      stop_repeat();
    }
    return;
  }
  if (focus_ == Focus::Notice) {
    handle_notice_button(button);
    return;
  }
  const bool in_osk = mode() == Mode::Osk;
  if (in_osk ? is_osk_repeatable(button) : is_direction(button)) {
    stop_repeat();
    held_button_ = button;
    held_in_osk_ = in_osk;
    repeat_carry_ms_ = 0.0f;
    held_timer().press();
  }
  run_action(mapper_.map(button, true, mode()));
}

void ShellController::on_sticks(float lx, float ly, float rx, float ry) {
  if (text_request_) {
    lx = ly = rx = ry = 0.0f;
  }
  sticks_[0] = lx;
  sticks_[1] = ly;
  sticks_[2] = rx;
  sticks_[3] = ry;
}

void ShellController::on_mouse_move(float x, float y) {
  pointer_.set_position({x, y});
  if (!pointer_hidden_by_mouse_) {
    pointer_hidden_by_mouse_ = true;
    mark_dirty();
  }
}

void ShellController::on_touchpad(std::span<const core::TouchContact> contacts,
                                  std::uint64_t now_ms) {
  if (text_request_ || focus_ != Focus::Page) {
    touchpad_.reset();
    return;
  }
  touchpad_.update(contacts, now_ms);
  const core::TouchpadModel::Output out = touchpad_.take();
  if (out.pointer.x != 0.0f || out.pointer.y != 0.0f) {
    const core::Vec2 p = pointer_.position();
    pointer_.set_position({p.x + out.pointer.x, p.y + out.pointer.y});
    pointer_moved_ = true;
    if (pointer_hidden_by_mouse_) {
      pointer_hidden_by_mouse_ = false;
      mark_dirty();
    }
  }
  // Scrolled from tick(), with the right stick's carry.
  scroll_carry_.x += out.scroll.x;
  scroll_carry_.y += out.scroll.y;
  for (int i = 0; i < out.clicks; ++i) {
    click_at_pointer();
  }
}

void ShellController::tick(float dt_seconds) {
  if (config_.text_input && text_request_) {
    config_.text_input->poll();
  }
  if (since_click_ >= 0.0f) {
    since_click_ += dt_seconds;
    if (since_click_ > kClickFocusWindowSeconds) {
      since_click_ = -1.0f;
    }
  }

  if (held_button_) {
    repeat_carry_ms_ += dt_seconds * 1000.0f;
    const auto whole = static_cast<std::uint32_t>(repeat_carry_ms_);
    repeat_carry_ms_ -= static_cast<float>(whole);
    const std::uint32_t fires = held_timer().update(whole);
    for (std::uint32_t i = 0; i < fires && held_button_ && focus_ != Focus::Notice; ++i) {
      run_action(mapper_.map(*held_button_, true, mode()));
    }
  }

  if (focus_ == Focus::Page) {
    const core::Vec2 delta = pointer_.update(sticks_[0], sticks_[1], dt_seconds);
    if (delta.x != 0.0f || delta.y != 0.0f) {
      pointer_moved_ = true;
      if (pointer_hidden_by_mouse_) {
        pointer_hidden_by_mouse_ = false;
        mark_dirty();
      }
    }
    // Pointer motion reaches the page at most once per tick, and only when
    // its integer position changed: every move costs the WebProcess a hit
    // test, which starves page animations on the console. The chrome is not
    // re-rendered for it either (sync_chrome_pointer() below).
    if (pointer_moved_) {
      send_page_pointer_move();
    }
    const core::Vec2 s = core::PointerModel::scroll(sticks_[2], sticks_[3], dt_seconds);
    scroll_carry_.x += s.x;
    scroll_carry_.y += s.y;
    const int dx = static_cast<int>(scroll_carry_.x);
    const int dy = static_cast<int>(scroll_carry_.y);
    if (dx != 0 || dy != 0) {
      scroll_carry_.x -= static_cast<float>(dx);
      scroll_carry_.y -= static_cast<float>(dy);
      scroll_active(dx, dy);
    }
  }

  if (pointer_moved_) {
    pointer_idle_seconds_ = 0.0f;
  } else if (pointer_idle_seconds_ < kFullscreenPointerHideSeconds) {
    pointer_idle_seconds_ += dt_seconds;
  }
  pointer_moved_ = false;

  save_session_if_due(dt_seconds);

  if (dirty_) {
    render_now();
  }
  sync_chrome_pointer();
}

bool ShellController::pointer_visible() const {
  if (focus_ != Focus::Page || pointer_hidden_by_mouse_ || page_cursor() == PageCursor::None) {
    return false;
  }
  return fullscreen_.state() != core::FullscreenController::State::ElementFullscreen ||
         pointer_idle_seconds_ < kFullscreenPointerHideSeconds;
}

PageCursor ShellController::page_cursor() const {
  const auto id = tabs_.active_id();
  if (!id) {
    return PageCursor::Pointer;
  }
  auto it = entries_.find(*id);
  return it == entries_.end() ? PageCursor::Pointer : it->second.cursor;
}

void ShellController::note_page_pointer(std::uint64_t tab_id, int x, int y) {
  page_pointer_ = PagePointer{tab_id, x, y};
}

void ShellController::send_page_pointer_move() {
  const core::Vec2 p = to_view(pointer_.position());
  const auto tab_id = tabs_.active_id();
  auto* view = active_view();
  if (p.y < 0.0f || !view || !tab_id) {
    return;
  }
  const int x = static_cast<int>(p.x);
  const int y = static_cast<int>(p.y);
  if (page_pointer_ && page_pointer_->tab_id == *tab_id && page_pointer_->x == x &&
      page_pointer_->y == y) {
    return;
  }
  view->pointer_move(x, y);
  note_page_pointer(*tab_id, x, y);
}

void ShellController::sync_chrome_pointer() {
  const core::Vec2 p = pointer_.position();
  const ChromePointer now{static_cast<int>(p.x), static_cast<int>(p.y), pointer_visible()};
  if (chrome_pointer_ == now) {
    return;
  }
  chrome_pointer_ = now;
  if (!config_.native_pointer) {
    chrome_.pointer(now.x, now.y, now.visible);
  }
}

int ShellController::content_top() const {
  return fullscreen_.chrome_visible() ? kChromeBandHeight : 0;
}

core::Vec2 ShellController::to_view(core::Vec2 window_pos) const {
  return {window_pos.x, window_pos.y - static_cast<float>(content_top())};
}

void ShellController::click_at_pointer() {
  const core::Vec2 w = pointer_.position();
  const core::Vec2 p = to_view(w);
  if (p.y < 0.0f) {
    chrome_.click(static_cast<int>(w.x), static_cast<int>(w.y));
    return;
  }
  auto* view = active_view();
  if (!view) {
    return;
  }
  const int x = static_cast<int>(p.x);
  const int y = static_cast<int>(p.y);
  view->pointer_move(x, y);
  if (const auto tab_id = tabs_.active_id()) {
    note_page_pointer(*tab_id, x, y);
  }
  view->pointer_button(x, y, MouseButton::Left, true);
  view->pointer_button(x, y, MouseButton::Left, false);
  since_click_ = 0.0f;
}

void ShellController::press_at_pointer() {
  const core::Vec2 w = pointer_.position();
  const core::Vec2 p = to_view(w);
  if (p.y < 0.0f) {
    chrome_.click(static_cast<int>(w.x), static_cast<int>(w.y));
    return;
  }
  auto* view = active_view();
  const auto tab_id = tabs_.active_id();
  if (!view || !tab_id) {
    return;
  }
  const int x = static_cast<int>(p.x);
  const int y = static_cast<int>(p.y);
  view->pointer_move(x, y);
  note_page_pointer(*tab_id, x, y);
  view->pointer_button(x, y, MouseButton::Left, true);
  pressed_tab_ = *tab_id;
  since_click_ = 0.0f;
}

void ShellController::release_pointer_press() {
  const std::uint64_t tab_id = *pressed_tab_;
  pressed_tab_.reset();
  if (tabs_.active_id() != tab_id) {
    return;  // the tab changed meanwhile: its view saw no more moves
  }
  auto* view = active_view();
  if (!view) {
    return;
  }
  core::Vec2 p = to_view(pointer_.position());
  p.y = std::max(0.0f, p.y);
  view->pointer_button(static_cast<int>(p.x), static_cast<int>(p.y), MouseButton::Left, false);
}

void ShellController::scroll_active(int dx, int dy) {
  auto* view = active_view();
  if (!view) {
    return;
  }
  core::Vec2 p = to_view(pointer_.position());
  p.y = std::max(0.0f, p.y);
  view->scroll(static_cast<int>(p.x), static_cast<int>(p.y), dx, dy);
}

void ShellController::run_action(Action action) {
  if (action == Action::None) {
    return;
  }
  switch (focus_) {
    case Focus::Page: run_page_action(action); break;
    case Focus::Chrome: run_chrome_action(action); break;
    case Focus::Menu: run_menu_action(action); break;
    case Focus::Osk: run_osk_action(action); break;
    case Focus::Notice: break;
  }
}

void ShellController::run_page_action(Action action) {
  auto* view = active_view();
  const int page = static_cast<int>(
      static_cast<float>(config_.height - content_top()) *
      kPageScrollFraction);
  switch (action) {
    case Action::Click: click_at_pointer(); break;
    case Action::Back:
      if (fullscreen_.state() == FsState::ElementFullscreen) {
        exit_fullscreen();
      } else if (const auto* tab = tabs_.active(); tab && tab->can_go_back && view) {
        view->go_back();
      }
      break;
    case Action::Forward:
      if (view) view->go_forward();
      break;
    case Action::Reload:
      if (view) view->reload();
      break;
    case Action::Stop:
      if (view) view->stop();
      break;
    case Action::ScrollUp: scroll_active(0, -kScrollStep); break;
    case Action::ScrollDown: scroll_active(0, kScrollStep); break;
    case Action::ScrollLeft: scroll_active(-kScrollStep, 0); break;
    case Action::ScrollRight: scroll_active(kScrollStep, 0); break;
    case Action::PageUp: scroll_active(0, -page); break;
    case Action::PageDown: scroll_active(0, page); break;
    case Action::FocusAddressBar: request_text_entry(OskTarget::Address); break;
    case Action::OpenOsk: request_text_entry(OskTarget::Page); break;
    case Action::PrevTab:
      tabs_.prev();
      after_tab_switch();
      break;
    case Action::NextTab:
      tabs_.next();
      after_tab_switch();
      break;
    case Action::NewTab: open_tab(config_.home_url, true); break;
    case Action::CloseTab:
      if (const auto id = tabs_.active_id()) close_tab(*id);
      break;
    case Action::ToggleChrome:
    case Action::ToggleFullscreen: toggle_fullscreen(); break;
    case Action::ExitFullscreen: exit_fullscreen(); break;
    case Action::OpenMenu: open_menu(); break;
    case Action::ToggleBookmark: toggle_bookmark(); break;
    case Action::ShowHistory:
      open_menu();
      show_menu_section(MenuSection::History);
      break;
    case Action::Quit: quit_requested_ = true; break;
    default: break;
  }
}

void ShellController::run_chrome_action(Action action) {
  switch (action) {
    case Action::Activate:
    case Action::NavigateUp:
    case Action::FocusAddressBar: run_page_action(Action::FocusAddressBar); break;
    case Action::NavigateLeft:
    case Action::PrevTab: run_page_action(Action::PrevTab); break;
    case Action::NavigateRight:
    case Action::NextTab: run_page_action(Action::NextTab); break;
    case Action::NavigateDown:
    case Action::ToggleChrome: set_focus(Focus::Page); break;
    case Action::CloseTab:
    case Action::NewTab:
    case Action::Reload:
    case Action::OpenMenu:
    case Action::ToggleFullscreen: run_page_action(action); break;
    default: break;
  }
}

void ShellController::run_menu_action(Action action) {
  switch (action) {
    case Action::NavigateUp:
      if (!menu_items_.empty()) {
        menu_selected_ = menu_selected_ == 0 ? menu_items_.size() - 1 : menu_selected_ - 1;
        mark_dirty();
      }
      break;
    case Action::NavigateDown:
      if (!menu_items_.empty()) {
        menu_selected_ = (menu_selected_ + 1) % menu_items_.size();
        mark_dirty();
      }
      break;
    case Action::Activate: activate_menu_item(menu_selected_); break;
    case Action::NavigateLeft:
    case Action::ToggleChrome:
      if (menu_section_ != MenuSection::Main) {
        show_menu_section(MenuSection::Main);
      } else if (action == Action::ToggleChrome) {
        close_menu();
      }
      break;
    case Action::OpenMenu: close_menu(); break;
    default: break;
  }
}

void ShellController::run_osk_action(Action action) {
  using Dir = OnScreenKeyboard::Direction;
  switch (action) {
    case Action::OskMoveUp: osk_.move(Dir::Up); break;
    case Action::OskMoveDown: osk_.move(Dir::Down); break;
    case Action::OskMoveLeft: osk_.move(Dir::Left); break;
    case Action::OskMoveRight: osk_.move(Dir::Right); break;
    case Action::OskSelect:
      if (osk_.press() == OnScreenKeyboard::PressResult::Committed) {
        commit_osk();
        return;
      }
      break;
    case Action::OskConfirm: commit_osk(); return;
    case Action::OskCancel: close_osk(); return;
    case Action::OskBackspace: osk_.backspace(); break;
    case Action::OskSpace: osk_.space(); break;
    case Action::OskShift: osk_.toggle_shift(); break;
    case Action::OskNextLayout: osk_.next_layout(); break;
    case Action::OskCaretLeft: osk_.caret_left(); break;
    case Action::OskCaretRight: osk_.caret_right(); break;
    default: break;
  }
  mark_dirty();
}

void ShellController::handle_notice_button(Button button) {
  if (button == Button::Cross) {
    dismiss_notice(true);
  } else if (button == Button::Circle) {
    dismiss_notice(false);
  }
}

// ---------------------------------------------------------------------------
// Overlays

void ShellController::set_focus(Focus focus) {
  if (focus_ != focus) {
    focus_ = focus;
    stop_repeat();
    mark_dirty();
  }
}

void ShellController::stop_repeat() {
  held_button_.reset();
  repeat_.release();
  osk_repeat_.release();
}

void ShellController::request_text_entry(OskTarget target) {
  if (text_request_) {
    return;
  }
  const bool ru = language() == core::Language::Ru;
  TextInputRequest request;
  bool replace = false;
  if (target == OskTarget::Address) {
    const auto* tab = tabs_.active();
    request.kind = TextInputKind::Url;
    request.title = ru ? "Адрес или поиск" : "Address or search";
    request.initial = tab ? tab->url : std::string();
  } else {
    // A known field value is pre-filled and replaced on commit; without one
    // (no focus report, contenteditable, password) the text is inserted at
    // the caret, as typed.
    request.kind = focused_field_ ? text_input_kind_from_type(focused_field_->type)
                                  : TextInputKind::Text;
    request.title = ru ? "Ввод текста" : "Enter text";
    if (focused_field_ && focused_field_->value) {
      request.initial = *focused_field_->value;
      replace = true;
    }
  }

  if (config_.text_input && !config_.text_input->active()) {
    if (menu_open_) {
      close_menu();
    }
    stop_repeat();
    text_request_ = TextRequest{target, tabs_.active_id().value_or(0), request.initial, replace};
    mark_dirty();
    if (config_.text_input->request_text(
            request, [this](const TextInputResult& result) { on_text_input_result(result); })) {
      return;
    }
    text_request_.reset();  // could not start: fall back to the built-in OSK
  }
  open_osk(target, request.initial, replace);
}

void ShellController::on_text_input_result(const TextInputResult& result) {
  if (!text_request_) {
    return;
  }
  const TextRequest request = *text_request_;
  text_request_.reset();
  mark_dirty();
  switch (result.outcome) {
    case TextInputResult::Outcome::Done:
      commit_text(request.target, request.tab_id, result.text, request.replace);
      break;
    case TextInputResult::Outcome::Cancelled: break;
    case TextInputResult::Outcome::Error:
      if (tabs_.active_id().value_or(0) == request.tab_id) {
        open_osk(request.target, request.initial, request.replace);
      }
      break;
  }
}

void ShellController::commit_text(OskTarget target, std::uint64_t tab_id,
                                  const std::string& text, bool replace) {
  if (target == OskTarget::Address) {
    navigate(text);
    return;
  }
  if (tabs_.active_id().value_or(0) != tab_id) {
    return;  // the field's tab is gone or in the background
  }
  auto* view = active_view();
  if (!view) {
    return;
  }
  if (replace) {
    view->run_javascript(kSelectFieldScript);
    if (text.empty()) {
      view->run_javascript("document.execCommand('delete', false)");
      return;
    }
  }
  if (!text.empty()) {
    view->insert_text(text);
  }
}

void ShellController::open_osk(OskTarget target, const std::string& text, bool replace) {
  if (menu_open_) {
    close_menu();
  }
  osk_target_ = target;
  osk_replace_ = target == OskTarget::Page && replace;
  osk_open_ = true;
  osk_.set_text(text);
  set_focus(Focus::Osk);
  mark_dirty();
}

void ShellController::close_osk() {
  if (!osk_open_) {
    return;
  }
  osk_open_ = false;
  if (focus_ == Focus::Osk) {
    set_focus(Focus::Page);
  }
  mark_dirty();
}

void ShellController::commit_osk() {
  const std::string text = osk_.text();
  const OskTarget target = osk_target_;
  const bool replace = osk_replace_;
  close_osk();
  commit_text(target, tabs_.active_id().value_or(0), text, replace);
}

void ShellController::open_menu() {
  menu_open_ = true;
  set_focus(Focus::Menu);
  show_menu_section(MenuSection::Main);
}

void ShellController::close_menu() {
  if (!menu_open_) {
    return;
  }
  menu_open_ = false;
  menu_items_.clear();
  if (focus_ == Focus::Menu) {
    set_focus(Focus::Page);
  }
  mark_dirty();
}

void ShellController::show_menu_section(MenuSection section) {
  menu_section_ = section;
  menu_items_.clear();
  menu_selected_ = 0;
  auto load_url = [this](std::string url) {
    return [this, url = std::move(url)] {
      close_menu();
      navigate(url);
    };
  };
  switch (section) {
    case MenuSection::Main: {
      const auto* tab = tabs_.active();
      const bool bookmarked = tab && bookmarks_.contains(tab->url);
      menu_items_.push_back({"Bookmarks", "", [this] { show_menu_section(MenuSection::Bookmarks); }});
      menu_items_.push_back({"History", "", [this] { show_menu_section(MenuSection::History); }});
      menu_items_.push_back({bookmarked ? "Remove bookmark" : "Add bookmark", "", [this] {
                               toggle_bookmark();
                               close_menu();
                             }});
      menu_items_.push_back({"New tab", "", [this] {
                               close_menu();
                               open_tab(config_.home_url, true);
                             }});
      menu_items_.push_back({"Close tab", "", [this] {
                               close_menu();
                               if (const auto id = tabs_.active_id()) close_tab(*id);
                             }});
      menu_items_.push_back({"Forward", "", [this] {
                               close_menu();
                               if (auto* view = active_view()) view->go_forward();
                             }});
      menu_items_.push_back({"Reload", "", [this] {
                               close_menu();
                               if (auto* view = active_view()) view->reload();
                             }});
      if (config_.addons_backend) {
        // One coherent entry per add-on; each opens its own section.
        menu_items_.push_back(
            {"Ad blocker", "", [this] { show_menu_section(MenuSection::Adblock); }});
        menu_items_.push_back(
            {"SponsorBlock", "", [this] { show_menu_section(MenuSection::SponsorBlock); }});
      }
      if (config_.on_natural_scroll_changed) {
        menu_items_.push_back(
            {std::string("Natural scrolling: ") + (touchpad_.natural_scroll() ? "On" : "Off"), "",
             [this] {
               const bool v = !touchpad_.natural_scroll();
               touchpad_.set_natural_scroll(v);
               config_.on_natural_scroll_changed(v);
               rebuild_menu_keeping(menu_selected_);
             }});
      }
      if (!config_.diagnostics_url.empty()) {
        menu_items_.push_back({"Diagnostics", "", [this] {
                                 close_menu();
                                 open_tab(config_.diagnostics_url, true);
                               }});
      }
      // No "Quit" item: the native title's programmatic exit does not return
      // to the home screen (black screen), so the user closes the app via the
      // PS button / system "Close". The session is persisted continuously
      // (save_session_if_due on tick, bookmarks on change), so a system close
      // restores the open tabs, history and bookmarks next launch.
      break;
    }
    case MenuSection::Bookmarks:
      for (const auto& b : bookmarks_.list()) {
        menu_items_.push_back({b.title.empty() ? b.url : b.title, b.url, load_url(b.url)});
      }
      if (menu_items_.empty()) {
        menu_items_.push_back({"No bookmarks yet", "", [] {}});
      }
      break;
    case MenuSection::History:
      for (const auto& h : history_.recent(kMenuHistoryItems)) {
        menu_items_.push_back({h.title.empty() ? h.url : h.title, h.url, load_url(h.url)});
      }
      if (menu_items_.empty()) {
        menu_items_.push_back({"History is empty", "", [] {}});
      }
      break;
    case MenuSection::Adblock:
      build_adblock_menu();
      break;
    case MenuSection::SponsorBlock:
      build_sponsorblock_menu();
      break;
  }
  mark_dirty();
}

namespace {

std::string format_last_updated(std::int64_t last, std::int64_t now) {
  if (last <= 0) {
    return "never";
  }
  std::int64_t age = now - last;
  if (age < 0) {
    age = 0;
  }
  if (age < 120) {
    return "just now";
  }
  if (age < 3600) {
    return std::to_string(age / 60) + " min ago";
  }
  if (age < 86400) {
    return std::to_string(age / 3600) + " h ago";
  }
  return std::to_string(age / 86400) + " d ago";
}

}  // namespace

void ShellController::build_adblock_menu() {
  menu_items_.clear();
  auto onoff = [](bool b) { return b ? "On" : "Off"; };
  auto& ab = addons_.adblock;

  menu_items_.push_back(
      {std::string("Ad blocker (EasyList/EasyPrivacy): ") + onoff(ab.enabled), "", [this] {
         addons_.adblock.enabled = !addons_.adblock.enabled;
         apply_addons();
         persist_addons();
         rebuild_menu_keeping(menu_selected_);
       }});
  menu_items_.push_back(
      {std::string("Block cookie banners (EasyList-Cookie): ") + onoff(ab.cookie_banners), "",
       [this] {
         addons_.adblock.cookie_banners = !addons_.adblock.cookie_banners;
         apply_addons();
         persist_addons();
         rebuild_menu_keeping(menu_selected_);
       }});
  menu_items_.push_back(
      {std::string("Auto-update filter lists: ") + onoff(ab.auto_update), "", [this] {
         addons_.adblock.auto_update = !addons_.adblock.auto_update;
         apply_addons();
         persist_addons();
         rebuild_menu_keeping(menu_selected_);
       }});
  menu_items_.push_back({"Update filter lists now", "", [this] {
                           if (config_.addons_backend) {
                             config_.addons_backend->update_filters_now();
                           }
                           rebuild_menu_keeping(menu_selected_);
                         }});
  {
    std::int64_t last = ab.last_updated;
    if (config_.addons_backend && config_.addons_backend->filters_last_updated() > last) {
      last = config_.addons_backend->filters_last_updated();
    }
    const bool updating = config_.addons_backend && config_.addons_backend->filters_updating();
    const std::string when = updating ? std::string("updating...") : format_last_updated(last, now());
    menu_items_.push_back({std::string("Filter lists updated: ") + when, "", [] {}});
  }
  menu_items_.push_back({"Back", "", [this] { show_menu_section(MenuSection::Main); }});
}

void ShellController::build_sponsorblock_menu() {
  menu_items_.clear();
  auto onoff = [](bool b) { return b ? "On" : "Off"; };
  auto& sb = addons_.sponsorblock;

  menu_items_.push_back({std::string("SponsorBlock: ") + onoff(sb.enabled), "", [this] {
                           addons_.sponsorblock.enabled = !addons_.sponsorblock.enabled;
                           apply_addons();
                           persist_addons();
                           rebuild_menu_keeping(menu_selected_);
                         }});
  for (const auto& cat : core::AddonsConfig::all_sponsorblock_categories()) {
    const bool on = std::find(sb.categories.begin(), sb.categories.end(), cat) != sb.categories.end();
    menu_items_.push_back(
        {std::string("  ") + cat + ": " + onoff(on), "", [this, cat] {
           auto& cats = addons_.sponsorblock.categories;
           auto it = std::find(cats.begin(), cats.end(), cat);
           if (it != cats.end()) {
             cats.erase(it);
           } else {
             cats.push_back(cat);
           }
           apply_addons();
           persist_addons();
           rebuild_menu_keeping(menu_selected_);
         }});
  }
  menu_items_.push_back({"Back", "", [this] { show_menu_section(MenuSection::Main); }});
}

void ShellController::apply_addons() {
  if (!config_.addons_backend) {
    return;
  }
  config_.addons_backend->set_adblock(addons_.adblock.enabled, addons_.adblock.cookie_banners);
  config_.addons_backend->set_sponsorblock(addons_.sponsorblock.enabled,
                                           addons_.sponsorblock.categories);
}

void ShellController::persist_addons() {
  if (config_.on_addons_changed) {
    config_.on_addons_changed(addons_);
  }
}

void ShellController::rebuild_menu_keeping(std::size_t keep) {
  if (!menu_open_) {
    return;
  }
  show_menu_section(menu_section_);
  if (keep < menu_items_.size()) {
    menu_selected_ = keep;
  }
  mark_dirty();
}

void ShellController::on_filters_updated(std::int64_t when) {
  addons_.adblock.last_updated = when;
  persist_addons();
  if (menu_open_ &&
      (menu_section_ == MenuSection::Adblock || menu_section_ == MenuSection::SponsorBlock)) {
    rebuild_menu_keeping(menu_selected_);
  } else {
    mark_dirty();
  }
}

void ShellController::activate_menu_item(std::size_t index) {
  if (!menu_open_ || index >= menu_items_.size()) {
    return;
  }
  menu_selected_ = index;
  // Copy: the callback may rebuild menu_items_.
  const auto run = menu_items_[index].run;
  if (run) {
    run();
  }
  mark_dirty();
}

void ShellController::show_notice(Notice notice) {
  if (focus_ != Focus::Notice) {
    focus_before_notice_ = focus_;
  }
  notice_ = std::move(notice);
  focus_ = Focus::Notice;
  stop_repeat();
  mark_dirty();
}

void ShellController::dismiss_notice(bool proceed) {
  if (!notice_) {
    return;
  }
  notice_.reset();
  focus_ = focus_before_notice_;
  if (focus_ == Focus::Osk && !osk_open_) focus_ = Focus::Page;
  if (focus_ == Focus::Menu && !menu_open_) focus_ = Focus::Page;
  auto pending = std::move(pending_nav_);
  pending_nav_.reset();
  if (pending && proceed && tabs_.find(pending->tab_id)) {
    load_in_tab_now(pending->tab_id, pending->url);
  }
  mark_dirty();
}

void ShellController::toggle_bookmark() {
  const auto* tab = tabs_.active();
  if (!tab || is_internal_url(tab->url)) {
    return;
  }
  if (!bookmarks_.remove(tab->url)) {
    bookmarks_.add(tab->url, tab->title, now());
  }
  session_.save_bookmarks(bookmarks_);
  mark_dirty();
}

void ShellController::toggle_fullscreen() {
  if (fullscreen_.state() == FsState::ElementFullscreen) {
    exit_fullscreen();
    return;
  }
  fullscreen_.toggle_chrome();
  apply_geometry();
  mark_dirty();
}

void ShellController::exit_fullscreen() {
  if (fullscreen_.user_exit() == FsEffect::RequestExitElementFullscreen) {
    if (auto* view = active_view()) {
      view->request_exit_fullscreen();
    }
  }
  apply_geometry();
  mark_dirty();
}

// ---------------------------------------------------------------------------
// Chrome events and state

void ShellController::on_chrome_event(const json& event) {
  if (event.is_string()) {
    // A frontend may deliver the posted value still encoded (the chrome used
    // to post JSON.stringify(event)); decode it once.
    const json decoded =
        json::parse(event.get<std::string>(), nullptr, /*allow_exceptions=*/false);
    if (decoded.is_object()) {
      on_chrome_event(decoded);
    }
    return;
  }
  if (!event.is_object()) {
    return;
  }
  const std::string type = event.value("type", std::string());
  auto id_of = [&event]() -> std::uint64_t {
    const auto it = event.find("id");
    if (it == event.end() || !it->is_number()) return 0;
    return it->is_number_float() ? static_cast<std::uint64_t>(it->get<double>())
                                 : it->get<std::uint64_t>();
  };
  auto int_of = [&event](const char* key) -> long {
    const auto it = event.find(key);
    if (it == event.end() || !it->is_number()) return -1;
    return it->is_number_float() ? static_cast<long>(it->get<double>()) : it->get<long>();
  };

  if (type == "ready") {
    mark_dirty();
  } else if (type == "navigate") {
    close_osk();
    close_menu();
    if (focus_ == Focus::Chrome) set_focus(Focus::Page);
    navigate(event.value("text", std::string()));
  } else if (type == "nav") {
    const std::string action = event.value("action", std::string());
    if (action == "back") run_page_action(Action::Back);
    else if (action == "forward") run_page_action(Action::Forward);
    else if (action == "reload") run_page_action(Action::Reload);
    else if (action == "stop") run_page_action(Action::Stop);
  } else if (type == "tab_select") {
    activate_tab(id_of());
  } else if (type == "tab_close") {
    close_tab(id_of());
  } else if (type == "tab_new") {
    open_tab(config_.home_url, true);
  } else if (type == "menu_open") {
    open_menu();
  } else if (type == "menu_select") {
    const long index = int_of("index");
    if (index >= 0) activate_menu_item(static_cast<std::size_t>(index));
  } else if (type == "menu_close") {
    close_menu();
  } else if (type == "notice_dismiss") {
    dismiss_notice(event.value("proceed", true));
  } else if (type == "osk_key") {
    const long row = int_of("row");
    const long col = int_of("col");
    if (osk_open_ && row >= 0 && col >= 0 &&
        osk_.set_cursor({static_cast<std::size_t>(row), static_cast<std::size_t>(col)})) {
      run_osk_action(Action::OskSelect);
    }
  } else if (type == "osk_commit") {
    if (osk_open_) commit_osk();
  } else if (type == "osk_cancel") {
    close_osk();
  } else if (type == "address_focus") {
    if (focus_ == Focus::Page) set_focus(Focus::Chrome);
  } else if (type == "address_blur") {
    if (focus_ == Focus::Chrome) set_focus(Focus::Page);
  }
}

void ShellController::render_now() {
  dirty_ = false;
  chrome_.render(chrome_state());
  // The state carries the pointer: no separate ChromeUi::pointer() for it.
  const core::Vec2 p = pointer_.position();
  chrome_pointer_ = ChromePointer{static_cast<int>(p.x), static_cast<int>(p.y), pointer_visible()};
}

std::vector<ShellController::Rect> ShellController::chrome_rects() const {
  // chrome.css is laid out in rem, 16 px at 1920 wide.
  const float rem = static_cast<float>(config_.width) / 120.0f;
  std::vector<Rect> rects;
  if (menu_open_ || notice_) {
    // The menu panel and the notice backdrop: compose the whole window.
    rects.push_back({0, 0, config_.width, config_.height});
    return rects;
  }
  if (fullscreen_.chrome_visible()) {
    rects.push_back({0, 0, config_.width, kChromeBandHeight});
  }
  if (osk_open_) {
    // .osk: at most 45vh tall plus a 3rem shadow above it.
    const int top = config_.height - config_.height * 45 / 100 - static_cast<int>(3.0f * rem);
    rects.push_back({0, std::max(0, top), config_.width, config_.height - std::max(0, top)});
  }
  if (pointer_visible() && !config_.native_pointer) {
    // .pointer: a 2.25rem ring centred on the position, plus its shadow.
    const int r = static_cast<int>(2.25f * rem);
    const core::Vec2 p = pointer_.position();
    rects.push_back({static_cast<int>(p.x) - r, static_cast<int>(p.y) - r, 2 * r, 2 * r});
  }
  return rects;
}

json ShellController::chrome_state() const {
  json state;
  json tabs = json::array();
  const auto active = tabs_.active_id();
  for (const auto& tab : tabs_.tabs()) {
    tabs.push_back({{"id", tab.id},
                    {"title", tab.title},
                    {"url", tab.url},
                    {"active", active == tab.id},
                    {"loading", tab.loading}});
  }
  state["tabs"] = std::move(tabs);

  const auto* tab = tabs_.active();
  const bool editing_address = osk_open_ && osk_target_ == OskTarget::Address;
  state["address"] = {{"text", editing_address ? osk_.text() : (tab ? tab->url : "")},
                      {"editing", editing_address || focus_ == Focus::Chrome}};
  state["nav"] = {{"back", tab && tab->can_go_back},
                  {"fwd", tab && tab->can_go_forward},
                  {"loading", tab && tab->loading}};
  state["chrome_visible"] = fullscreen_.chrome_visible();
  state["fullscreen_state"] = fullscreen_name(fullscreen_.state());
  const int top = content_top();
  state["content"] = {{"x", 0}, {"y", top}, {"width", config_.width},
                      {"height", std::max(1, config_.height - top)}};

  const core::Vec2 p = pointer_.position();
  state["pointer"] = {{"x", p.x},
                      {"y", p.y},
                      {"visible", pointer_visible()},
                      {"native", config_.native_pointer}};
  json rects = json::array();
  for (const Rect& r : chrome_rects()) {
    rects.push_back({{"x", r.x}, {"y", r.y}, {"width", r.width}, {"height", r.height}});
  }
  state["chrome_rects"] = std::move(rects);

  json osk = {{"open", osk_open_},
              {"layout", layout_name(osk_.layout())},
              {"shift", osk_.shift()},
              {"text", osk_.text()},
              {"caret_index", osk_.caret()},
              {"target", osk_target_ == OskTarget::Address ? "address" : "page"},
              {"cursor", {{"row", osk_.cursor().row}, {"col", osk_.cursor().col}}}};
  json rows = json::array();
  if (osk_open_) {
    for (const auto& row : osk_.keys()) {
      json r = json::array();
      for (const auto& key : row) {
        const auto [code, width] = key_code_and_width(key);
        r.push_back({{"label", osk_.label(key)},
                     {"code", code ? std::string(code) : key.value},
                     {"width", width}});
      }
      rows.push_back(std::move(r));
    }
  }
  osk["rows"] = std::move(rows);
  state["osk"] = std::move(osk);

  if (notice_) {
    state["notice"] = {{"kind", notice_->kind}, {"title", notice_->title}, {"text", notice_->text}};
  } else {
    state["notice"] = {{"kind", nullptr}, {"title", ""}, {"text", ""}};
  }

  json items = json::array();
  for (std::size_t i = 0; i < menu_items_.size(); ++i) {
    items.push_back({{"label", menu_items_[i].label},
                     {"url", menu_items_[i].url},
                     {"selected", i == menu_selected_}});
  }
  const char* section = menu_section_ == MenuSection::Main        ? "main"
                        : menu_section_ == MenuSection::Bookmarks ? "bookmarks"
                        : menu_section_ == MenuSection::Adblock   ? "adblock"
                        : menu_section_ == MenuSection::SponsorBlock ? "sponsorblock"
                                                                  : "history";
  state["menu"] = {{"open", menu_open_},
                   {"section", section},
                   {"items", std::move(items)},
                   {"selected", menu_selected_}};
  state["text_input"] = {
      {"active", text_request_.has_value()},
      {"target", text_request_ && text_request_->target == OskTarget::Page ? "page" : "address"}};
  state["focus"] = focus_name(focus_);
  return state;
}

}  // namespace prosperoweb::shell

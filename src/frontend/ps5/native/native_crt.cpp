// SPDX-License-Identifier: GPL-3.0-or-later
//
// Native title startup (PRWB00001/eboot.bin).
//
// Derived from ps5-native-app-boilerplate tooling/native/app_crt.cpp
// (Copyright (C) 2026 BlackBearReloaded, GPL-3.0-or-later; vendored under
// third_party/ps5-native-tool): _start(process_parameters, teardown)
// initialises the platform runtime, runs the static constructors and main.
//
// ProsperoWeb additions, after the first console runs died before main()
// (klog: "page fault ... fault address: 0 rip: 0" with return addresses in
// _init, i.e. a constructor reached an import the system left at 0):
// - breadcrumbs at every stage, on the system debug output (klog) and in
//   /data/prosperoweb/boot.txt (libkernel's own file calls);
// - before anything else, the image's import slots are checked: every slot
//   at 0 is reported with its symbol (from /app0/imports.txt, written by
//   scripts/ps5/native/check-eboot.py), looked up by name in the system
//   modules (sceKernelDlsym) and, if found there, filled in; otherwise it
//   points at a stand-in that logs the call and its caller instead of
//   jumping to address 0;
// - every init_array entry is checked before it is called.
// Nothing here may itself depend on an import that could be missing: each
// libkernel function is checked before use.

#include <cstddef>
#include <cstdint>

using Destructor = void (*)();
using Initializer = void (*)();

extern "C" {
void _init_env(void* process_parameters);
int atexit(Destructor callback);
[[noreturn]] void exit(int status);
int application_main(int argc, char** argv, char** envp) __asm__("main");

int sceKernelDebugOutText(int channel, const char* text);
int sceKernelMprotect(const void* address, std::size_t length, int protection);
int sceKernelOpen(const char* path, int flags, int mode);
long sceKernelRead(int fd, void* buffer, std::size_t length);
long sceKernelWrite(int fd, const void* buffer, std::size_t length);
int sceKernelClose(int fd);
int sceKernelMkdir(const char* path, int mode);
int sceKernelLoadStartModule(const char* path, std::size_t args, const void* argp,
                             std::uint32_t flags, void* option, int* result);
int sceKernelDlsym(int handle, const char* symbol, void** address);
void native_libc_init(void);  // src/frontend/ps5/native/native_libc.c

extern Initializer __preinit_array_start[] __attribute__((weak));
extern Initializer __preinit_array_end[] __attribute__((weak));
extern Initializer __init_array_start[] __attribute__((weak));
extern Initializer __init_array_end[] __attribute__((weak));
extern Initializer __fini_array_start[] __attribute__((weak));
extern Initializer __fini_array_end[] __attribute__((weak));

// src/frontend/ps5/native/ps5-native-app.ld.
extern char __prwb_image_base[];
extern void* __prwb_got_start[];
extern void* __prwb_got_end[];
}

namespace {

// The address of an imported function as the GOT holds it, read so that
// the compiler cannot assume it is non-null.
template <typename F>
bool present(F* function) {
  void* volatile address = reinterpret_cast<void*>(function);
  return address != nullptr;
}

constexpr int kOpenRead = 0x0;
constexpr int kOpenWriteCreateTruncate = 0x1 | 0x200 | 0x400;  // O_WRONLY|O_CREAT|O_TRUNC
constexpr int kOpenWriteCreateAppend = 0x1 | 0x8 | 0x200;      // O_WRONLY|O_APPEND|O_CREAT
constexpr const char* kBootFile = "/data/lampa-ps5/boot.txt";
bool g_boot_file = false;

std::size_t length_of(const char* text) {
  std::size_t n = 0;
  while (text[n]) {
    n++;
  }
  return n;
}

// Each line opens boot.txt for appending, writes and closes it again: no
// descriptor is kept that later code could close, reuse or redirect.
void say(const char* text) {
  if (present(&sceKernelDebugOutText)) {
    sceKernelDebugOutText(0, text);
  }
  if (!g_boot_file || !present(&sceKernelOpen) || !present(&sceKernelWrite) ||
      !present(&sceKernelClose)) {
    return;
  }
  const int fd = sceKernelOpen(kBootFile, kOpenWriteCreateAppend, 0644);
  if (fd >= 0) {
    sceKernelWrite(fd, text, length_of(text));
    sceKernelClose(fd);
  }
}

// /data/prosperoweb/boot.txt, emptied at every start: the breadcrumbs reach
// it even when klog does not show them.
void open_boot_file() {
  if (!present(&sceKernelOpen) || !present(&sceKernelClose)) {
    return;
  }
  if (present(&sceKernelMkdir)) {
    sceKernelMkdir("/data/lampa-ps5", 0755);
  }
  const int fd = sceKernelOpen(kBootFile, kOpenWriteCreateTruncate, 0644);
  if (fd >= 0) {
    sceKernelClose(fd);
    g_boot_file = true;
  }
}

// Minimal formatting without libc (snprintf may be one of the missing ones).
struct Line {
  char text[256];
  std::size_t n = 0;
  Line& add(const char* s) {
    while (s && *s && n + 1 < sizeof(text)) {
      text[n++] = *s++;
    }
    text[n] = 0;
    return *this;
  }
  Line& hex(std::uintptr_t value) {
    char digits[19] = "0x";
    int length = 2;
    bool started = false;
    for (int shift = 60; shift >= 0; shift -= 4) {
      const unsigned d = (value >> shift) & 15;
      if (d || started || shift == 0) {
        digits[length++] = "0123456789abcdef"[d];
        started = true;
      }
    }
    digits[length] = 0;
    return add(digits);
  }
  Line& dec(std::uintptr_t value) {
    char digits[21];
    int length = 0;
    do {
      digits[length++] = static_cast<char>('0' + value % 10);
      value /= 10;
    } while (value);
    char out[21];
    for (int i = 0; i < length; i++) {
      out[i] = digits[length - 1 - i];
    }
    out[length] = 0;
    return add(out);
  }
  void out() {
    add("\n");
    say(text);
  }
};

// /app0/imports.txt: "<slot offset hex> <symbol>" per line.
constexpr std::size_t kImportsMax = 64 * 1024;
char g_imports[kImportsMax];
std::size_t g_imports_len = 0;

void load_imports() {
  if (!present(&sceKernelOpen) || !present(&sceKernelRead) || !present(&sceKernelClose)) {
    return;
  }
  const int fd = sceKernelOpen("/app0/imports.txt", kOpenRead, 0);
  if (fd < 0) {
    return;
  }
  long n;
  while (g_imports_len + 1 < kImportsMax &&
         (n = sceKernelRead(fd, g_imports + g_imports_len, kImportsMax - 1 - g_imports_len)) >
             0) {
    g_imports_len += static_cast<std::size_t>(n);
  }
  g_imports[g_imports_len] = 0;
  sceKernelClose(fd);
}

// Looks a symbol up by name in the system modules: the ones whose names
// the title imports from, then every module handle up to 64.
void* lookup_by_name(const char* symbol) {
  static int handles[8];
  static int handle_count = -1;
  if (!present(&sceKernelDlsym)) {
    return nullptr;
  }
  if (handle_count < 0) {
    handle_count = 0;
    static const char* const kModules[] = {"libSceLibcInternal.sprx", "libkernel.sprx",
                                           "libc.prx", "libSceNet.sprx"};
    for (const char* module : kModules) {
      int result = 0;
      const int handle = present(&sceKernelLoadStartModule)
                             ? sceKernelLoadStartModule(module, 0, nullptr, 0, nullptr, &result)
                             : -1;
      Line().add("[prosperoweb] crt: module ").add(module).add(" handle ").hex(
          static_cast<std::uint32_t>(handle)).out();
      if (handle >= 0) {
        handles[handle_count++] = handle;
      }
    }
  }
  void* address = nullptr;
  for (int i = 0; i < handle_count; i++) {
    if (sceKernelDlsym(handles[i], symbol, &address) == 0 && address) {
      return address;
    }
  }
  for (int handle = 0; handle < 64; handle++) {
    if (sceKernelDlsym(handle, symbol, &address) == 0 && address) {
      return address;
    }
  }
  return nullptr;
}

// The symbol of the import slot at offset, or nullptr if none is listed
// (writes it NUL-terminated into out).
const char* import_name(std::uintptr_t offset, char* out, std::size_t out_len) {
  std::size_t i = 0;
  while (i < g_imports_len) {
    std::uintptr_t value = 0;
    while (i < g_imports_len && g_imports[i] != ' ' && g_imports[i] != '\n') {
      const char c = g_imports[i++];
      value = value * 16 + static_cast<std::uintptr_t>(c >= 'a' ? c - 'a' + 10 : c - '0');
    }
    if (i < g_imports_len && g_imports[i] == ' ') {
      i++;
    }
    std::size_t k = 0;
    while (i < g_imports_len && g_imports[i] != '\n') {
      if (value == offset && k + 1 < out_len) {
        out[k++] = g_imports[i];
      }
      i++;
    }
    i++;
    if (value == offset) {
      out[k] = 0;
      return out;
    }
  }
  return nullptr;
}

// Where a call through an unresolved import lands instead of address 0.
extern "C" __attribute__((noinline)) long prwb_unresolved_import() {
  Line()
      .add("[prosperoweb] crt: CALL TO AN UNRESOLVED IMPORT from image+")
      .hex(reinterpret_cast<std::uintptr_t>(__builtin_return_address(0)) -
           reinterpret_cast<std::uintptr_t>(__prwb_image_base))
      .add(" (returning 0)")
      .out();
  return 0;
}

// Imports of the modules the UI loads with sceSysmoduleLoadModule.
bool loaded_later(const char* symbol) {
  static const char* const kPrefixes[] = {"sceVideodec2", "sceImeDialog"};
  for (const char* prefix : kPrefixes) {
    std::size_t i = 0;
    while (prefix[i] && symbol[i] == prefix[i]) {
      i++;
    }
    if (!prefix[i]) {
      return true;
    }
  }
  return false;
}

// Reports the import slots the system left at 0 and points the listed ones
// at prwb_unresolved_import. Slots not in imports.txt that are 0 are weak
// references resolved to 0 at link time (check-eboot.py lists them) and are
// left alone.
void check_imports() {
  load_imports();
  const auto base = reinterpret_cast<std::uintptr_t>(__prwb_image_base);
  const std::size_t slots = static_cast<std::size_t>(__prwb_got_end - __prwb_got_start);
  Line()
      .add("[prosperoweb] crt: image at ")
      .hex(base)
      .add(", ")
      .dec(slots)
      .add(" GOT slots, imports.txt ")
      .add(g_imports_len ? "read" : "MISSING")
      .out();
  std::size_t zero = 0;
  std::size_t patched = 0;
  bool writable = false;
  for (std::size_t i = 0; i < slots; i++) {
    if (__prwb_got_start[i] != nullptr) {
      continue;
    }
    zero++;
    const auto offset = reinterpret_cast<std::uintptr_t>(&__prwb_got_start[i]) - base;
    char name[128];
    const char* symbol = import_name(offset, name, sizeof(name));
    Line line;
    line.add("[prosperoweb] crt: import slot image+").hex(offset).add(" is 0: ");
    line.add(symbol ? symbol : "(not an import: a weak reference)");
    if (symbol && loaded_later(symbol)) {
      // The UI loads that module itself; the system binds the slot then
      // (run 16: the decoder answered after sceSysmoduleLoadModule).
      line.add(" (its module is loaded later by the UI: left alone)");
    } else if (symbol) {
      if (!writable && present(&sceKernelMprotect)) {
        // The GOT is in the RELRO range, read-only after relocation.
        const std::uintptr_t first =
            reinterpret_cast<std::uintptr_t>(__prwb_got_start) & ~std::uintptr_t{0x3fff};
        const std::uintptr_t last = reinterpret_cast<std::uintptr_t>(__prwb_got_end);
        writable = sceKernelMprotect(reinterpret_cast<void*>(first), last - first, 0x3) == 0;
      }
      if (writable) {
        void* found = lookup_by_name(symbol);
        __prwb_got_start[i] = found ? found : reinterpret_cast<void*>(&prwb_unresolved_import);
        patched++;
        if (found) {
          line.add(" (found by name at ").hex(reinterpret_cast<std::uintptr_t>(found)).add(")");
        } else {
          line.add(" (not found by name: pointed at the stand-in)");
        }
      } else {
        line.add(" (GOT not writable: left at 0)");
      }
    }
    line.out();
  }
  Line()
      .add("[prosperoweb] crt: ")
      .dec(zero)
      .add(" zero slots, ")
      .dec(patched)
      .add(" unresolved imports patched")
      .out();
}

void run_forward(Initializer* first, Initializer* last, const char* what) {
  if (first == nullptr || last == nullptr) {
    return;
  }
  Line().add("[prosperoweb] crt: ").add(what).add(": ").dec(
      static_cast<std::uintptr_t>(last - first)).add(" entries").out();
  for (std::size_t i = 0; first != last; ++first, ++i) {
    if (*first == nullptr) {
      Line().add("[prosperoweb] crt: ").add(what).add("[").dec(i).add("] is 0, skipped").out();
      continue;
    }
    (*first)();
  }
}

void run_reverse(Initializer* first, Initializer* last) {
  if (first == nullptr || last == nullptr) {
    return;
  }
  while (last != first) {
    if (*--last != nullptr) {
      (*last)();
    }
  }
}

}  // namespace

// A breadcrumb in boot.txt and klog, for code outside the CRT
// (native_title.h).
extern "C" void native_boot_say(const char* text) {
  say(text);
}

extern "C" __attribute__((weak)) void catchReturnFromMain(int status) { (void)status; }

extern "C" void _init() {
  run_forward(__preinit_array_start, __preinit_array_end, "preinit_array");
  run_forward(__init_array_start, __init_array_end, "init_array");
}

extern "C" void _fini() { run_reverse(__fini_array_start, __fini_array_end); }

extern "C" [[noreturn]] __attribute__((visibility("default"))) void _start(
    void* process_parameters, Destructor loader_teardown) {
  open_boot_file();
  say("[prosperoweb] crt: _start\n");
  check_imports();
  const int argc = *static_cast<const int*>(process_parameters);
  auto* parameters = static_cast<std::uint8_t*>(process_parameters);
  auto** argv = reinterpret_cast<char**>(parameters + sizeof(std::uint64_t));

  _init_env(process_parameters);
  say("[prosperoweb] crt: _init_env done\n");
  native_libc_init();
  say("[prosperoweb] crt: native libc stdio bound\n");
  if (loader_teardown != nullptr) {
    (void)atexit(loader_teardown);
  }
  (void)atexit(_fini);
  _init();
  say("[prosperoweb] crt: constructors done, calling main\n");
  const int status = application_main(argc, argv, nullptr);
  catchReturnFromMain(status);
  exit(status);
}

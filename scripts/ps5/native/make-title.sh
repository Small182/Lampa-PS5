#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Package ProsperoWeb as a native PS5 title PRWB00001: a home-screen tile
# with icon and backgrounds, a signed development FSELF eboot.bin, the
# clean-room libc.prx runtime companion and sce_sys/param.json, plus the
# ProsperoStore catalog artifact (build-dist/PRWB00001.zip, PRWB00001/ at the
# top). See docs/native-title.md for the design and the launch chain.
#
#   scripts/ps5/native/make-title.sh [native.elf [payload.elf]]
#
# Inputs (defaults in brackets):
#   native.elf   the UI linked as a native app, target prosperoweb_native
#                [build-ps5/src/frontend/ps5/prosperoweb-native.elf]; checked
#                (check-eboot.py), converted and signed into eboot.bin.
#   payload.elf  the payload ELF the spawn broker and the children run,
#                shipped as PRWB00001/prosperoweb.elf
#                [dist/prosperoweb/prosperoweb.elf, from scripts/ps5/package.sh].
#   BROKER_ELF   the broker-only payload the UI asks elfldr/websrv to start,
#                shipped as PRWB00001/prosperoweb-broker.elf (debug info
#                stripped) [build-ps5/src/frontend/ps5/prosperoweb-broker.elf,
#                target prosperoweb_broker]; optional.
#
# Without a native ELF (no WebKit build) the script builds the small native
# loader stub from scripts/ps5/native/stub/ instead, so the tile format can
# still be checked, and says so loudly. NATIVE_ALLOW_STUB=0 makes that an
# error (CI uses it once the native target is built).
#
# Host tools are built from vendored source (third_party/ps5-native-tool,
# third_party/bc7enc_rdo); no Sony SDK or ISPC toolchain is used. Light builds
# land in build-native/.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
SDK="${PS5_PAYLOAD_SDK:-/opt/ps5-payload-sdk}"
TITLE_ID="LAMP00001"
NATIVE_ELF="${1:-${ROOT}/build-ps5/src/frontend/ps5/prosperoweb-native.elf}"
PAYLOAD_ELF="${2:-${ROOT}/dist/prosperoweb/prosperoweb.elf}"
BROKER_ELF="${BROKER_ELF:-$(dirname "${NATIVE_ELF}")/prosperoweb-broker.elf}"
VIDEODEC2_STUB="${VIDEODEC2_STUB:-$(dirname "${NATIVE_ELF}")/libSceVideodec2.so}"
LLVM_BIN="${LLVM_BIN:-$("${PS5_PAYLOAD_SDK:-/opt/ps5-payload-sdk}/bin/prospero-llvm-config" --bindir)}"
# ProsperoStore's own asset validator, when a clone is at hand.
PROSPEROSTORE_DIR="${PROSPEROSTORE_DIR:-}"

NT="${ROOT}/third_party/ps5-native-tool"
BC="${ROOT}/third_party/bc7enc_rdo"
WORK="${ROOT}/build-native"
HOST="${WORK}/host"
OUTDIR="${ROOT}/build-dist/native"
APP="${OUTDIR}/${TITLE_ID}"
BG="${ROOT}/build-dist/assets/background-3840x2160.png"
DIST="${ROOT}/dist/prosperoweb"

# Loader/container constants, verified on firmware 6.02/12.70 by the vendored
# boilerplate; kept separate from the public app version in param.json.
MODULE_SDK=0x02000009
COMPANION_SDK=0x08050001
FSELF_MAGIC=0x1D3D154F

log() { printf '[native] %s\n' "$*"; }
die() { printf '[native] error: %s\n' "$*" >&2; exit 1; }

CXX="${CXX:-$(command -v clang++-18 || command -v clang++)}"
GXX="${GXX:-$(command -v g++)}"
CLANG18="${PS5_CLANG:-$(command -v clang-18 || command -v clang)}"
[[ -n "${CXX}" ]] || die "clang++ is required for the host tools"
[[ -n "${GXX}" ]] || die "g++ (with OpenMP) is required to build bc7enc"
[[ -d "${SDK}" ]] || die "PS5_PAYLOAD_SDK not found at ${SDK}"

mkdir -p "${HOST}"

# --- 1. Build the host tools from vendored source. --------------------------
build_native_tool() {
    [[ -x "${HOST}/ps5-native-tool" && "${HOST}/ps5-native-tool" -nt "${NT}/tooling/native/native_app_builder.cpp" ]] && return
    log "building ps5-native-tool"
    local n="${NT}/tooling/native"
    "${CXX}" -std=c++20 -O2 -w -c "${n}/native_app_builder.cpp" -o "${HOST}/native_app_builder.o"
    "${CXX}" -std=c++20 -O2 -w -c "${n}/self_container.cpp"     -o "${HOST}/self_container.o"
    "${CXX}" -std=c++20 -O2 -w -c "${n}/elf_object.cpp"         -o "${HOST}/elf_object.o"
    "${CXX}" -std=c++20 -O2 -w -c "${n}/sce_module_writer.cpp"  -o "${HOST}/sce_module_writer.o"
    "${CXX}" -std=c++20 -O2 -w "${HOST}"/{native_app_builder,self_container,elf_object,sce_module_writer}.o \
        -lz -o "${HOST}/ps5-native-tool"
    "${CXX}" -std=c++20 -O2 -w -c "${n}/libc_builder.cpp" -o "${HOST}/libc_builder.o"
    "${CXX}" "${HOST}/libc_builder.o" -lz -o "${HOST}/libc-builder"
}

build_bc7enc() {
    [[ -x "${HOST}/bc7enc" ]] && return
    log "building bc7enc (CPU BC7, no ISPC)"
    ( cd "${BC}" && "${GXX}" -O2 -w -DSUPPORT_BC7E=0 -fopenmp -std=c++14 -include cstdint \
        test.cpp rdo_bc_encoder.cpp bc7enc.cpp bc7decomp.cpp bc7decomp_ref.cpp \
        rgbcx.cpp ert.cpp lodepng.cpp utils.cpp -pthread -o "${HOST}/bc7enc" )
}

build_native_tool
build_bc7enc

# --- 2. Clean-room libc.prx, checksum-gated against the vendored digest. -----
make_libc() {
    local manifest="${NT}/tooling/native/runtime/libc.prx.sha256"
    "${HOST}/libc-builder" "${NT}/tooling/native/runtime/api-surface.txt" \
        "${NT}/tooling/native/runtime/imports.txt" "${WORK}/libc.raw.elf" >/dev/null
    "${HOST}/ps5-native-tool" self --sign --in "${WORK}/libc.raw.elf" \
        --out "${WORK}/libc.prx" >/dev/null
    local want got
    want="$(awk '{print $1}' "${manifest}")"
    got="$(sha256sum "${WORK}/libc.prx" | cut -d' ' -f1)"
    [[ "${got}" == "${want}" ]] || die "libc.prx digest mismatch: ${got} != ${want}"
    log "libc.prx reproduced (${got})"
}
make_libc

# --- 3. Home-screen backgrounds: 3840x2160 BC7 DDS from the 4K PNG. ----------
make_dds() {
    [[ -f "${BG}" ]] || die "${BG} not found; run scripts/assets/render.sh first"
    "${HOST}/bc7enc" -C -u4 -g -f -o "${BG}" "${WORK}/pic0.dds" >/dev/null 2>&1 \
        || die "bc7enc failed"
    # bc7enc writes dwMipMapCount=0; the shell wants a single surface (=1).
    python3 -I - "${WORK}/pic0.dds" <<'PY'
import struct, sys
d = bytearray(open(sys.argv[1], "rb").read())
struct.pack_into("<I", d, 28, 1)
open(sys.argv[1], "wb").write(d)
PY
    cp -- "${WORK}/pic0.dds" "${WORK}/pic1.dds"
    log "pic0.dds / pic1.dds: 3840x2160 BC7_UNORM DX10 ($(stat -c %s "${WORK}/pic0.dds") bytes)"
}
make_dds

# --- 4. param.json from assets/app.json (single source of truth). -----------
make_param() {
    python3 -I - "${ROOT}/assets/app.json" "${WORK}/param.json" "${NATIVE_FLEXIBLE_MB:-512}" <<'PY'
import json, re, sys
app = json.load(open(sys.argv[1], encoding="utf-8"))
flexible_mb = int(sys.argv[3])
tid = app["titleId"]
assert re.fullmatch(r"[A-Z]{4}\d{5}", tid), f"bad titleId {tid!r}"
assert re.fullmatch(r"[A-Z]{2}\d{4}-" + tid + r"_00-[A-Z0-9]{16}", app["contentId"])
assert re.fullmatch(r"\d{2}\.\d{3}\.\d{3}", app["contentVersion"])
# Category 65536 (Prospero Native Media App) + badge 2 puts the title on the
# home-screen Media row, like SmartTube and ytps5-sponsorblock. The media
# sandbox REFUSES listen() on loopback TCP, so the audio (127.0.0.1:17772) and
# video-overlay (17773) servers no longer depend on it: each WebProcess hands
# its stream sockets to the UI over an inherited control socket (see
# docs/native-title.md), verified on console with working audio + hw video and
# no perf regression. Category 0 + badge 1 is the Games shelf (unchanged TCP
# path); switch via assets/app.json. Defaulted to 65536/2 (Media).
category = int(app.get("applicationCategoryType", 0))
badge = int(app.get("contentBadgeType", 1))
param = {
    "applicationCategoryType": category,
    "applicationDrmType": "free",
    "attribute": 0,
    "conceptId": tid[4:],
    "contentBadgeType": badge,
    "contentId": app["contentId"],
    "contentVersion": app["contentVersion"],
    # Positive so the sandbox /download0 scratch mount exists; the writable
    # browser profile itself lives at /data/prosperoweb (ShadowMountPlus
    # mounts /data writable via nullfs for a homebrew title).
    "downloadDataSize": 256,
    # Flexible memory: the image, thread stacks, the system libc heap and
    # anonymous mmap come from it (the UI's own heap is direct memory,
    # src/frontend/ps5/native/native_memory.c). syobon-action-ps5 runs with
    # 256 MiB (sce_sys/param.json, on fw 13.42); this image alone is about
    # 130 MiB, hence 512 MiB by default (NATIVE_FLEXIBLE_MB). The largest
    # value the system accepts is not known; docs/native-title.md says how
    # to lower it on the console if the title does not start.
    "kernel": {"flexibleMemorySize": flexible_mb << 20},
    "localizedParameters": {
        "defaultLanguage": app["language"],
        app["language"]: {"titleName": app["name"]},
    },
    "masterVersion": app["masterVersion"],
    "requiredSystemSoftwareVersion": "0x0000000000000000",
    "sdkVersion": "0x0000000000000000",
    "titleId": tid,
}
with open(sys.argv[2], "w", encoding="utf-8", newline="\n") as f:
    json.dump(param, f, indent=2, ensure_ascii=False)
    f.write("\n")
PY
    log "param.json generated from assets/app.json"
}
make_param

# --- 5. eboot.bin: convert the native-app ELF, else build the loader stub. --
STUB_USED=0
# Converts an LLVM PIE linked with the native layout and signs it; prints
# what the converter and the signer say, and the converted ELF's header and
# dynamic section (before signing).
make_eboot() {
    local pie="$1" out="$2"
    shift 2
    local stubs=(--stub-dir "${SDK}/target/lib")
    local extra
    for extra in "$@"; do stubs+=(--stub "${extra}"); done
    "${HOST}/ps5-native-tool" link --in "${pie}" --out "${WORK}/eboot.elf" \
        "${stubs[@]}" --module-sdk "${MODULE_SDK}" \
        --companion-sdk "${COMPANION_SDK}" --heap-size 0xffffffffffffffff \
        --file-name eboot.elf 2>&1 | sed 's/^/[native]   link: /'
    [[ ${PIPESTATUS[0]} -eq 0 ]] || return 1
    log "converted ELF (before signing):"
    file "${WORK}/eboot.elf" | sed 's/^/[native]   /'
    "${LLVM_BIN}/llvm-readelf" -h -d "${WORK}/eboot.elf" 2>&1 \
        | grep -E 'Type:|Entry point|NEEDED|SONAME|INIT|FINI' | sed 's/^/[native]   /'
    "${HOST}/ps5-native-tool" self --sign --in "${WORK}/eboot.elf" \
        --out "${out}" --magic "${FSELF_MAGIC}" 2>&1 | sed 's/^/[native]   sign: /'
    [[ ${PIPESTATUS[0]} -eq 0 ]]
}

build_stub() {
    local stub="${ROOT}/scripts/ps5/native/stub/stub_main.c"
    local n="${NT}/tooling/native"
    log "building native loader stub from scripts/ps5/native/stub/"
    sh "${NT}/tooling/prospero-clang18" -O2 -c "${n}/app_crt.cpp"         -o "${WORK}/app_crt.o"
    sh "${NT}/tooling/prospero-clang18" -O2 -c "${n}/app_cpp_runtime.cpp" -o "${WORK}/app_cpp_runtime.o"
    sh "${NT}/tooling/prospero-clang18" -O2 -c "${stub}"                  -o "${WORK}/stub_main.o"
    "${SDK}/bin/prospero-lld" -T "${n}/ps5-pie.ld" --eh-frame-hdr \
        --version-script "${n}/app-symbols.map" -e _start -o "${WORK}/stub-pie.elf" \
        "${WORK}"/{app_crt,app_cpp_runtime,stub_main}.o \
        --as-needed "${SDK}"/target/lib/*.so
    make_eboot "${WORK}/stub-pie.elf" "${APP}/eboot.bin" \
        || die "could not convert the loader stub"
}

# The browser UI as a native app: checked first (no syscall instruction,
# every import from a module the title gets or loads), then converted.
build_native_eboot() {
    [[ -f "${VIDEODEC2_STUB}" ]] || die "${VIDEODEC2_STUB} not found (built with prosperoweb_native)"
    log "checking ${NATIVE_ELF}"
    python3 -I "${ROOT}/scripts/ps5/native/check-eboot.py" --imports-out "${WORK}/imports.txt" \
        "${LLVM_BIN}" "${NATIVE_ELF}" \
        "${SDK}/target/lib" "${VIDEODEC2_STUB}" | sed 's/^/[native]   /'
    [[ ${PIPESTATUS[0]} -eq 0 ]] || die "native ELF check failed"
    make_eboot "${NATIVE_ELF}" "${APP}/eboot.bin" "${VIDEODEC2_STUB}" \
        || die "could not convert ${NATIVE_ELF}"
}

PS5_PAYLOAD_SDK="${SDK}"; export PS5_PAYLOAD_SDK
rm -rf -- "${APP}"
mkdir -p "${APP}/sce_sys" "${APP}/sce_module"

if [[ -f "${NATIVE_ELF}" ]]; then
    build_native_eboot
    log "eboot.bin: the browser UI, converted and signed from ${NATIVE_ELF}"
else
    [[ "${NATIVE_ALLOW_STUB:-1}" == 1 ]] \
        || die "${NATIVE_ELF} not found (cmake --build build-ps5 --target prosperoweb_native)"
    log "WARNING: ${NATIVE_ELF} not found; eboot.bin is the LOADER STUB, not the browser."
    log "WARNING: build the prosperoweb_native target for the real eboot (docs/native-title.md)."
    STUB_USED=1
    build_stub
fi

# The payload ELF the broker and the children run (docs/native-title.md,
# "Launch chain"). Without it the UI starts but cannot open pages.
if [[ "${STUB_USED}" == 0 ]]; then
    [[ -f "${PAYLOAD_ELF}" ]] || die "${PAYLOAD_ELF} not found (run scripts/ps5/package.sh)"
    cp -- "${PAYLOAD_ELF}" "${APP}/prosperoweb.elf"
    if [[ -f "${BROKER_ELF}" ]]; then
        "${SDK}/bin/llvm-strip" --strip-debug -o "${APP}/prosperoweb-broker.elf" "${BROKER_ELF}"
        log "prosperoweb-broker.elf: the broker elfldr/websrv start ($(stat -c %s "${APP}/prosperoweb-broker.elf") bytes)"
    else
        log "WARNING: ${BROKER_ELF} not found; the UI will start the 176 MB prosperoweb.elf as the broker"
    fi
    # The import slots by offset: the start-up code names unresolved ones.
    cp -- "${WORK}/imports.txt" "${APP}/imports.txt"
    log "prosperoweb.elf: the payload for the broker and children ($(stat -c %s "${APP}/prosperoweb.elf") bytes)"
fi

# --- 6. Assemble the title folder. ------------------------------------------
cp -- "${WORK}/libc.prx"  "${APP}/sce_module/libc.prx"
cp -- "${WORK}/param.json" "${APP}/sce_sys/param.json"
cp -- "${ROOT}/assets/sce_sys/icon0.png" "${APP}/sce_sys/icon0.png"
cp -- "${WORK}/pic0.dds" "${APP}/sce_sys/pic0.dds"
cp -- "${WORK}/pic1.dds" "${APP}/sce_sys/pic1.dds"

# Read-only runtime data under the title (the UI reads it as /app0/data or
# <install folder>/data, the children by the install folder). The writable
# profile stays at /data/prosperoweb (ShadowMountPlus nullfs).
# NATIVE_SKIP_DATA=1 leaves it out (smaller zip for a format-only check).
if [[ "${NATIVE_SKIP_DATA:-0}" != 1 && -d "${DIST}" ]]; then
    mkdir -p "${APP}/data"
    cp -a "${DIST}/chrome" "${APP}/data/chrome"
    cp -a "${DIST}/fonts"  "${APP}/data/fonts"
    cp -a "${DIST}/LICENSES" "${APP}/data/LICENSES"
    # Add-ons baseline (ad-block rulesets + sponsorblock.js); the browser
    # recompiles/refreshes into the writable profile at runtime.
    [[ -d "${DIST}/addons" ]] && cp -a "${DIST}/addons" "${APP}/data/addons"
    [[ -f "${DIST}/cacert.pem" ]] && cp -- "${DIST}/cacert.pem" "${APP}/data/cacert.pem"
    # The install folder is not known here: fontconfig finds the fonts next
    # to fonts.conf (prefix="relative"), its cache stays in the profile.
    python3 -I - "${APP}/data/fonts/fonts.conf" <<'PY'
import re, sys
path = sys.argv[1]
text = open(path, encoding="utf-8").read()
text, n = re.subn(r"<dir>[^<]*</dir>", '<dir prefix="relative">.</dir>', text, count=1)
assert n == 1, "fonts.conf has no <dir>"
open(path, "w", encoding="utf-8").write(text)
PY
    # The sample without the websrv install's chrome_dir: the title uses its own.
    [[ -f "${DIST}/config.json.sample" ]] && python3 -I - "${DIST}/config.json.sample" \
        "${APP}/data/config.json.sample" <<'PY'
import json, sys
config = json.load(open(sys.argv[1], encoding="utf-8"))
config.pop("chrome_dir", None)
with open(sys.argv[2], "w", encoding="utf-8") as f:
    json.dump(config, f, indent=2)
    f.write("\n")
PY
    log "staged read-only runtime data under ${TITLE_ID}/data/"
else
    log "runtime data not staged (NATIVE_SKIP_DATA=1 or dist/ absent)"
fi

# --- 7. Validate the presentation assets (self-contained check). ------------
python3 -I - "${APP}/sce_sys" <<'PY'
from pathlib import Path
import struct, sys
root = Path(sys.argv[1])
icon = (root / "icon0.png").read_bytes()
assert icon[:8] == b"\x89PNG\r\n\x1a\n" and struct.unpack(">II", icon[16:24]) == (512, 512), \
    "icon0.png must be a 512x512 PNG"
for name in ("pic0.dds", "pic1.dds"):
    d = (root / name).read_bytes()
    hsz = struct.unpack_from("<I", d, 4)[0]
    h, w = struct.unpack_from("<II", d, 12)
    mips = struct.unpack_from("<I", d, 28)[0]
    fmt, dim = struct.unpack_from("<II", d, 128)
    arr = struct.unpack_from("<I", d, 140)[0]
    exp = 148 + w // 4 * (h // 4) * 16
    got = (hsz, w, h, mips, d[84:88], fmt, dim, arr, len(d))
    assert got == (124, 3840, 2160, 1, b"DX10", 98, 3, 1, exp), f"{name} bad DDS profile: {got}"
print("presentation assets validated:", root)
PY
if [[ -n "${PROSPEROSTORE_DIR}" && -f "${PROSPEROSTORE_DIR}/tools/validate-assets.sh" ]]; then
    bash "${PROSPEROSTORE_DIR}/tools/validate-assets.sh" "${APP}/sce_sys" | sed 's/^/[native]   ProsperoStore: /'
    [[ ${PIPESTATUS[0]} -eq 0 ]] || die "ProsperoStore validate-assets.sh failed"
else
    log "ProsperoStore validate-assets.sh not run (set PROSPEROSTORE_DIR to a clone)"
fi

# --- 8. ProsperoStore catalog artifact: PRWB00001.zip, PRWB00001/ at top. ---
ZIP="${ROOT}/build-dist/${TITLE_ID}.zip"
rm -f -- "${ZIP}"
# Built with python's zipfile (no external zip dependency): PRWB00001/ at the
# top of the archive, with eboot.bin kept executable (0755) so a console-side
# unzip preserves the bit the loader needs.
python3 -I - "${OUTDIR}" "${TITLE_ID}" "${ZIP}" <<'PY'
import os, stat, sys, zipfile
base, tid, out = sys.argv[1], sys.argv[2], sys.argv[3]
root = os.path.join(base, tid)
paths = []
for dirpath, _dirs, files in os.walk(root):
    for f in sorted(files):
        paths.append(os.path.join(dirpath, f))
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    for p in sorted(paths):
        arc = os.path.relpath(p, base)
        zi = zipfile.ZipInfo(arc)
        zi.compress_type = zipfile.ZIP_DEFLATED
        mode = 0o755 if os.path.basename(p) == "eboot.bin" else 0o644
        zi.external_attr = (stat.S_IFREG | mode) << 16
        with open(p, "rb") as fh:
            z.writestr(zi, fh.read())
PY

# --- Report. ----------------------------------------------------------------
log "title tree:"
( cd "${OUTDIR}" && find "${TITLE_ID}" -maxdepth 2 -type f -printf '%12s  %p\n' | sort -k2 )
log "eboot.bin inspect:"
"${HOST}/ps5-native-tool" self --inspect --file "${APP}/eboot.bin" | sed 's/^/[native]   /'
log "artifact: ${ZIP} ($(stat -c %s "${ZIP}") bytes, sha256 $(sha256sum "${ZIP}" | cut -c1-16)...)"
[[ "${STUB_USED}" == 1 ]] && log "NOTE: eboot.bin is the LOADER STUB, not the browser (see docs/native-title.md)."
log "done"

#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Assemble the PS5 distribution from a linked prosperoweb.elf:
#
#   dist/prosperoweb/                     -> copy to /data/prosperoweb/ on the console
#     prosperoweb.elf                     debug info stripped (symbols kept)
#     chrome/                             HTML chrome (src/chrome-ui)
#     cacert.pem                          curl's CA bundle (MPL-2.0)
#     fonts/                              Noto, Liberation, DejaVu + fonts.conf (FONTCONFIG_FILE)
#     addons/                             ad-block rulesets + sponsorblock.js (first-run baseline)
#     config.json.sample                  copy to config.json to override defaults
#     LICENSES/                           GPL-3.0 (ProsperoWeb), MPL-2.0 notice (cacert),
#                                         font licenses
#     README-install.txt
#   dist/homebrew/prosperoweb/            -> copy to /data/homebrew/prosperoweb/ (websrv
#                                         tile, the only supported way to launch the browser)
#     homebrew.js                         launches /data/prosperoweb/prosperoweb.elf
#     sce_sys/icon0.png                   512x512 tile icon      (assets/sce_sys/)
#     sce_sys/pic0.png, pic1.png          1920x1080 backgrounds  (assets/sce_sys/)
#     sce_sys/param.json                  title identity from assets/app.json
#
#   scripts/ps5/package.sh [path/to/prosperoweb.elf]
#
# cacert.pem is cached in build-dist/dl/ and verified against curl.se's
# published .sha256. Fonts are pinned in scripts/ps5/fonts/fonts.lock and
# staged (with fonts.conf) by scripts/ps5/fonts/stage-fonts.sh.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ELF="${1:-${ROOT}/build-ps5/src/frontend/ps5/prosperoweb.elf}"
SDK="${PS5_PAYLOAD_SDK:-/opt/ps5-payload-sdk}"
WORK="${ROOT}/build-dist"
DL="${WORK}/dl"
DIST="${ROOT}/dist"
OUT="${DIST}/prosperoweb"
TILE="${DIST}/homebrew/prosperoweb"
INSTALL_DIR="/data/lampa-ps5"

CACERT_URL="https://curl.se/ca/cacert.pem"

log() { printf '[package] %s\n' "$*"; }
die() { printf '[package] error: %s\n' "$*" >&2; exit 1; }

[[ -f "${ELF}" ]] || die "${ELF} not found; build the prosperoweb target first"
for t in curl sha256sum tar python3; do
    command -v "${t}" >/dev/null || die "${t} is required"
done

# download <url> <file>: fetch into the cache unless already there.
download() {
    local url="$1" out="${DL}/$2"
    if [[ ! -f "${out}" ]]; then
        log "downloading ${url}"
        curl -fsSL --retry 3 -o "${out}.part" "${url}"
        mv -f -- "${out}.part" "${out}"
    fi
}

mkdir -p "${DL}"

# --- CA bundle: verified against the checksum curl.se publishes. -----------
# Always refetched: the bundle changes, and a stale cached pair still verifies.
rm -f -- "${DL}/cacert.pem" "${DL}/cacert.pem.sha256"
download "${CACERT_URL}" cacert.pem
download "${CACERT_URL}.sha256" cacert.pem.sha256
(cd "${DL}" && sha256sum --check --quiet cacert.pem.sha256) || die "cacert.pem checksum mismatch"

# --- Assemble. dist/ is generated output, rebuilt from scratch. -------------
rm -rf -- "${OUT}" "${TILE}"
mkdir -p "${OUT}/chrome" "${OUT}/fonts" "${OUT}/addons" "${OUT}/LICENSES" "${TILE}/sce_sys"

# elfldr and the spawner only need the program headers, the section headers
# and the SHT_RELA sections, so debug info can go. The symbol table stays
# for crash backtraces.
"${SDK}/bin/llvm-strip" --strip-debug -o "${OUT}/prosperoweb.elf" "${ELF}"

cp -- "${ROOT}"/src/chrome-ui/*.html "${ROOT}"/src/chrome-ui/*.css "${ROOT}"/src/chrome-ui/*.js "${OUT}/chrome/"
cp -- "${DL}/cacert.pem" "${OUT}/cacert.pem"
# Fonts, their licenses and fonts.conf (pointing at ${INSTALL_DIR}/fonts).
"${ROOT}/scripts/ps5/fonts/stage-fonts.sh" "${OUT}/fonts" "${OUT}/LICENSES" "${INSTALL_DIR}"
# Lampa PS5 intentionally ships without optional ad-block filter snapshots.
# Those live lists change between CI runs and are not needed to run Lampa.
# Keep a valid empty manifest for the shared ProsperoWeb add-ons loader.
mkdir -p "${OUT}/addons"
cp -- "${ROOT}/scripts/ps5/addons/lampa-sources.json" "${OUT}/addons/sources.json"
cp -- "${ROOT}/LICENSE" "${OUT}/LICENSES/GPL-3.0-or-later.txt"

cat > "${OUT}/LICENSES/cacert-MPL-2.0.txt" <<'EOF'
cacert.pem is the CA certificate bundle extracted from Mozilla's NSS by the
curl project (https://curl.se/docs/caextract.html). It is distributed under
the Mozilla Public License, version 2.0, unmodified: cacert.pem is itself the
Source Code Form. A copy of the license is available at
https://mozilla.org/MPL/2.0/.
EOF

cat > "${OUT}/LICENSES/README.txt" <<'EOF'
prosperoweb.elf is free software under the GNU General Public License,
version 3 or later (GPL-3.0-or-later.txt). It statically links WebKit
(LGPL-2.1 / BSD), Skia (BSD-3-Clause), ICU (Unicode-3.0), FreeType (FTL),
HarfBuzz (MIT), libwpe (BSD-2-Clause), curl (curl), OpenSSL (Apache-2.0),
SQLite (public domain), libxml2 (MIT), fontconfig (MIT), SDL2 (Zlib) and
FFmpeg with x264, which makes the combined binary GPL. The corresponding
source is the ProsperoWeb repository at the commit this package was built
from, plus the upstream sources listed in its scripts/ps5/build-deps.sh and
scripts/ps5/fetch-webkit.sh.

Fonts: Noto Sans, Noto Serif, Noto Sans Mono, Noto Color Emoji and
Noto Sans CJK (SIL OFL 1.1: Noto-fonts-OFL-1.1.txt,
Noto-Color-Emoji-OFL-1.1.txt, Noto-Sans-CJK-OFL-1.1.txt), Liberation
Sans, Serif and Mono (SIL OFL 1.1: Liberation-fonts-OFL-1.1.txt), DejaVu
(DejaVu-fonts.txt). The fonts are unmodified.
CA bundle: MPL-2.0 (cacert-MPL-2.0.txt).
This Lampa build does not bundle optional ad-block filter lists or
SponsorBlock scripts. The shared browser engine retains the add-on APIs,
but both optional features are disabled in this package.
EOF

# Defaults from src/frontend/ps5/ps5_config.h; config.json overrides them.
cat > "${OUT}/config.json.sample" <<EOF
{
  "home_url": "http://lampa.mx",
  "language": "en",
  "chrome_dir": "${INSTALL_DIR}/chrome",
  "width": 1920,
  "height": 1080,
  "natural_scroll": false,
  "addons": {
    "adblock": { "enabled": false, "cookie_banners": false, "auto_update": false, "last_updated": 0 },
    "sponsorblock": { "enabled": false, "categories": ["sponsor"] }
  }
}
EOF

# websrv (ps5-payload-dev/websrv) lists /data/homebrew/<name>/ entries that
# have a homebrew.js or an eboot.elf, with sce_sys/icon0.png as the tile.
# homebrew.js wins over eboot.elf, so the tile launches the installed ELF
# in place instead of carrying a second 100+ MB copy.
cat > "${TILE}/homebrew.js" <<EOF
// SPDX-License-Identifier: GPL-3.0-or-later
// websrv launcher entry for ProsperoWeb (installed in ${INSTALL_DIR}).
async function main() {
    return {
        mainText: "ProsperoWeb",
        secondaryText: "WebKit web browser",
        onclick: async () => {
            return {
                path: "${INSTALL_DIR}/prosperoweb.elf",
                cwd: "${INSTALL_DIR}",
                args: []
            };
        }
    };
}
EOF

# websrv tile: only icon0.png. Do NOT put param.json or pic*.png here:
# ShadowMountPlus scans /data/homebrew/*/sce_sys/param.json and tries to
# install the folder as a native title, which fails without an eboot.bin and
# leaves a "installing" title with no home-screen tile. The native-title
# identity (param.json, backgrounds) is staged separately for a future
# eboot.bin build under build-dist/native/PRWB00001/ (docs/store-submission.md).
cp -- "${ROOT}"/assets/sce_sys/icon0.png "${TILE}/sce_sys/"
NATIVE="${ROOT}/build-dist/native/sce_sys"
mkdir -p "${NATIVE}"
cp -- "${ROOT}"/assets/sce_sys/icon0.png "${ROOT}"/assets/sce_sys/pic0.png \
    "${ROOT}"/assets/sce_sys/pic1.png "${NATIVE}/"
python3 -I - "${ROOT}/assets/app.json" "${NATIVE}/param.json" <<'PY'
import json, re, sys
app = json.load(open(sys.argv[1], encoding="utf-8"))
title_id = app["titleId"]
if not re.fullmatch(r"[A-Z]{4}\d{5}", title_id):
    sys.exit(f"bad titleId {title_id!r}")
if not re.fullmatch(r"[A-Z]{2}\d{4}-" + title_id + r"_00-[A-Z0-9]{16}", app["contentId"]):
    sys.exit(f"bad contentId {app['contentId']!r}")
if not re.fullmatch(r"\d{2}\.\d{3}\.\d{3}", app["contentVersion"]):
    sys.exit(f"bad contentVersion {app['contentVersion']!r}")
param = {
    "applicationCategoryType": 0,
    "applicationDrmType": "free",
    "conceptId": title_id[4:],
    "contentId": app["contentId"],
    "contentVersion": app["contentVersion"],
    "localizedParameters": {
        "defaultLanguage": app["language"],
        app["language"]: {"titleName": app["name"]},
    },
    "masterVersion": app["masterVersion"],
    "titleId": title_id,
}
with open(sys.argv[2], "w", encoding="utf-8", newline="\n") as out:
    json.dump(param, out, indent=2, ensure_ascii=False)
    out.write("\n")
PY

cat > "${OUT}/README-install.txt" <<EOF
ProsperoWeb - installation
==========================

Requirements: a jailbroken PS5 running ps5-payload-dev/websrv (port 8080),
which launches the browser; ftpsrv (port 2121) to copy the files;
optionally klogsrv (port 3232) to read the debug output.

1. Copy the files (FTP, ftpsrv on port 2121):

     prosperoweb/          -> ${INSTALL_DIR}/
     homebrew/prosperoweb/ -> /data/homebrew/prosperoweb/   (the websrv tile)

   For example with lftp:

     lftp -p 2121 PS5_IP -e "mirror -R prosperoweb ${INSTALL_DIR}; mirror -R homebrew/prosperoweb /data/homebrew/prosperoweb; quit"

   The browser must live in ${INSTALL_DIR}: it re-launches
   ${INSTALL_DIR}/prosperoweb.elf for its WebProcess and NetworkProcess
   (override with PROSPEROWEB_SELF_PATH), and reads its fonts, CA bundle
   and chrome from there.

   Upgrading from ProsperoBrowser (/data/prosperobrowser) or ps5browser
   (/data/ps5browser): remove the old tile (/data/homebrew/prosperobrowser/
   or /data/homebrew/ps5browser/) so websrv lists only the new one, and
   leave the old directory in place for the first start. The browser then
   copies (does not move) tabs.json, history.json, bookmarks.json,
   config.json, storage/, localstorage/, indexeddb/ and cache/ once into
   ${INSTALL_DIR}/ from the newest old directory that has a profile
   (/data/prosperobrowser first) and logs "migrate: copied <n> items from
   <dir>"; old install paths in the copied config.json are pointed at
   ${INSTALL_DIR}. PROSPEROBROWSER_* and PS5BROWSER_* variables (also in
   config.json "env") still work for this release; rename them to
   PROSPEROWEB_*.

2. Optional: copy config.json.sample to config.json and edit it.

3. Launch ONLY through the websrv tile: open http://PS5_IP:8080/ (or the
   Homebrew Launcher app) and select the "ProsperoWeb" tile. websrv starts
   the browser as the foreground app (hbldr, title FAKE00000), which is the
   only way it gets the screen and the controller.

   Do NOT launch prosperoweb.elf through elfldr (port 9021) or the Payload
   Manager. That starts it as a background payload: there is no picture,
   and the UI plus its WebProcess and NetworkProcess keep running in the
   background (about 300 MB each) until the console runs out of memory and
   swaps. There is no reliable way to stop them other than a reboot.

   Reboot the console between attempts (and after any launch that showed
   no picture), so no copy of the browser is left running.

   websrv must keep running while the browser runs (it does, since its
   tile launched the browser): on the first page load the browser asks
   websrv (http://127.0.0.1:8080) to start its spawn broker, a background
   helper ("spawn-broker") that starts the WebProcess and NetworkProcess.
   Starting them from the browser itself panics the console. The broker
   listens on 127.0.0.1:17771 (loopback only; PROSPEROWEB_SPAWN_PORT
   changes it) and exits on its own after the browser quits.

   PROSPEROWEB_SPAWN_MODE=direct (spawn without the broker) is only for a
   plain-payload launch through elfldr, used for debugging; never set it
   for the tile launch.

   To quit, use Options -> Quit; this also saves the session.

Logs:
  - ${INSTALL_DIR}/log.txt: one line per startup stage, from every role
    (UI, WebProcess, NetworkProcess), each prefixed with the milliseconds
    since that process started and the role, for example
      [    812 UI] spawn: role=NetworkProcess pid=1120 fds=[25->3] ok in 41ms, budget_set(0) ok
    The file is appended to on every launch and written through to disk
    line by line, so it survives a crash or a power-off; fetch it over FTP.
    The last line shows where startup stopped.
  - klog: the same lines (prefixed "[prosperoweb]") plus the kernel's and
    the loader's messages. Start klogsrv and connect BEFORE launching:
      nc PS5_IP 3232 > klog.txt
    Look for "[prosperoweb]", "spawn:", "broker", "websrv:" and
    "exit_value" lines.

Environment the browser sets for itself (unless already set):
  FONTCONFIG_FILE=${INSTALL_DIR}/fonts/fonts.conf
  FONTCONFIG_PATH=${INSTALL_DIR}/fonts
  PROSPEROWEB_CA_BUNDLE=${INSTALL_DIR}/cacert.pem
  SYSTEM_LANGUAGE=en-US (from config.json "language")
Media: PROSPEROWEB_MEDIA_AUDIO=null disables audio output.
EOF

# --- Report. ---------------------------------------------------------------
size_of() { stat -c %s "$1"; }
log "prosperoweb.elf: $(size_of "${ELF}") bytes as linked, $(size_of "${OUT}/prosperoweb.elf") bytes with --strip-debug"
"${SDK}/bin/llvm-strip" --strip-all -o "${WORK}/prosperoweb.strip-all.elf" "${ELF}"
log "              ($(size_of "${WORK}/prosperoweb.strip-all.elf") bytes with --strip-all, not shipped)"
log "dist tree:"
(cd "${DIST}" && find prosperoweb homebrew -type f -printf '%10s  %p\n' | sort -k2)
du -sh "${OUT}" "${TILE}"

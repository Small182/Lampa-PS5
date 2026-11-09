#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Cross-compile the libraries the WebKit PlayStation port needs and the
# PS5 payload SDK does not ship (or ships without the required features):
#
#   ICU (filtered data), brotli, freetype (with brotli -> WOFF2),
#   harfbuzz (with harfbuzz-icu), libxkbcommon, libwpe
#
# Everything is static, release, installed into $PS5_DEPS_PREFIX
# (default: third_party/ps5-prefix). Tarballs are cached in
# third_party/dl/, sources and build trees live in build-deps/<lib>/.
#
# Idempotent: a library is skipped when $PREFIX/.stamp-<lib> exists.
# Remove the stamp (and build-deps/<lib>) to rebuild a single library.
#
# Environment:
#   PS5_PAYLOAD_SDK   SDK root                 (/opt/ps5-payload-sdk)
#   PS5_DEPS_PREFIX   install prefix           (third_party/ps5-prefix)
#   PS5_DEPS_WORK     source/build trees       (build-deps)
#   PS5_DEPS_DL       tarball cache            (third_party/dl)
#   JOBS              parallel jobs            (4)
#   ICU_JOBS          parallel jobs for ICU    (3)
#   ICU_FULL_DATA=1   skip the ICU data filter, ship the full data

set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SCRIPTS="${REPO}/scripts/ps5"

PS5_PAYLOAD_SDK="${PS5_PAYLOAD_SDK:-/opt/ps5-payload-sdk}"
SDK="$(realpath "${PS5_PAYLOAD_SDK}")"
SYSROOT="$(realpath "${SDK}/target")"
PREFIX="$(realpath -m "${PS5_DEPS_PREFIX:-${REPO}/third_party/ps5-prefix}")"
WORK="$(realpath -m "${PS5_DEPS_WORK:-${REPO}/build-deps}")"
DL="$(realpath -m "${PS5_DEPS_DL:-${REPO}/third_party/dl}")"
JOBS="${JOBS:-4}"
ICU_JOBS="${ICU_JOBS:-3}"
ICU_FULL_DATA="${ICU_FULL_DATA:-0}"

export PS5_PAYLOAD_SDK="${SDK}" PS5_DEPS_PREFIX="${PREFIX}"
# prospero.sh (if sourced by the caller) exports these for its own
# DESTDIR-based packaging; they must not leak into our installs.
unset DESTDIR PKG_CONFIG_PATH PKG_CONFIG_LIBDIR PKG_CONFIG_SYSROOT_DIR

# --- pinned sources ---------------------------------------------------------

ICU_VER=77.1
ICU_URL=https://github.com/unicode-org/icu/releases/download/release-77-1/icu4c-77_1-src.tgz
ICU_SHA=588e431f77327c39031ffbb8843c0e3bc122c211374485fa87dc5f3faff24061
# The -src tarball carries only the prebuilt .dat; the data filter needs
# the data sources.
ICU_DATA_URL=https://github.com/unicode-org/icu/releases/download/release-77-1/icu4c-77_1-data.zip
ICU_DATA_SHA=1e08bfafa442260ccabf9a872d4eab12de813d42b90769df056bab032b37e1d3

BROTLI_VER=1.1.0
BROTLI_URL=https://github.com/google/brotli/archive/refs/tags/v1.1.0.tar.gz
BROTLI_SHA=e720a6ca29428b803f4ad165371771f5398faba397edf6778837a18599ea13ff

FREETYPE_VER=2.13.3
FREETYPE_URL=https://download.savannah.gnu.org/releases/freetype/freetype-2.13.3.tar.xz
FREETYPE_SHA=0550350666d427c74daeb85d5ac7bb353acba5f76956395995311a9c6f063289

HARFBUZZ_VER=10.4.0
HARFBUZZ_URL=https://github.com/harfbuzz/harfbuzz/releases/download/10.4.0/harfbuzz-10.4.0.tar.xz
HARFBUZZ_SHA=480b6d25014169300669aa1fc39fb356c142d5028324ea52b3a27648b9beaad8

XKBCOMMON_VER=1.7.0
XKBCOMMON_URL=https://xkbcommon.org/download/libxkbcommon-1.7.0.tar.xz
XKBCOMMON_SHA=65782f0a10a4b455af9c6baab7040e2f537520caa2ec2092805cdfd36863b247

LIBWPE_VER=1.16.3
LIBWPE_URL=https://github.com/WebPlatformForEmbedded/libwpe/releases/download/1.16.3/libwpe-1.16.3.tar.xz
LIBWPE_SHA=c880fa8d607b2aa6eadde7d6d6302b1396ebc38368fe2332fa20e193c7ee1420

# Host meson (used only if no new enough meson is on PATH).
MESON_PIP_VER=1.6.1

# --- helpers ----------------------------------------------------------------

run() {
    printf '+ %s\n' "$*" >&2
    "$@"
}

die() {
    echo "error: $*" >&2
    exit 1
}

# fetch <url> <file> <sha256>
fetch() {
    local url="$1" out="${DL}/$2" sha="$3"
    mkdir -p "${DL}"
    if [[ ! -f "${out}" ]]; then
        if ! run curl -fL --retry 3 -o "${out}.part" "${url}"; then
            if [[ "${url}" == "${ICU_DATA_URL}" ]]; then
                # GitHub's release-assets CDN sometimes returns persistent 504s.
                # SourceForge mirrors the same upstream ICU release artifact;
                # the pinned SHA256 below still verifies its contents.
                run curl -fL --retry 5 --retry-all-errors \
                    -o "${out}.part" \
                    "https://sourceforge.net/projects/international-component.mirror/files/release-77-1/icu4c-77_1-data.zip/download"
            else
                die "download failed: ${url}"
            fi
        fi
        run mv "${out}.part" "${out}"
    fi
    echo "${sha}  ${out}" | sha256sum --check --quiet - \
        || die "checksum mismatch for ${out}"
}

# unpack <name> <file>: extract into $WORK/<name>/src (strip top dir)
unpack() {
    local name="$1" file="${DL}/$2" src="${WORK}/$1/src"
    if [[ ! -d "${src}" ]]; then
        run mkdir -p "${src}.tmp"
        run tar -C "${src}.tmp" --strip-components=1 -xf "${file}"
        apply_patches "${name}" "${src}.tmp"
        run mv "${src}.tmp" "${src}"
    fi
}

apply_patches() {
    local name="$1" dir="$2" p
    [[ -d "${SCRIPTS}/patches/${name}" ]] || return 0
    for p in "${SCRIPTS}/patches/${name}"/*.patch; do
        run patch -d "${dir}" -p1 --forward -i "${p}"
    done
}

stamp() { echo "${PREFIX}/.stamp-$1"; }

declare -A BUILD_TIME=()

# build <name> <function>
build() {
    local name="$1" fn="$2" t0
    if [[ -e "$(stamp "${name}")" ]]; then
        echo "=== ${name}: already built ($(stamp "${name}")), skipping"
        BUILD_TIME[${name}]="$(cat "$(stamp "${name}")")"
        return 0
    fi
    echo "=== ${name}: building"
    t0=${SECONDS}
    "${fn}"
    fix_pkgconfig
    BUILD_TIME[${name}]="$((SECONDS - t0))s"
    echo "${BUILD_TIME[${name}]}" > "$(stamp "${name}")"
    echo "=== ${name}: done in ${BUILD_TIME[${name}]}"
}

# Number of path components of the sysroot, as "/.." repeated.
REL_UP="$(printf '%s' "${SYSROOT}" | tr -cd '/' | sed 's#/#/..#g')"
REL_PREFIX="${REL_UP}${PREFIX}"

# Make our .pc files usable through the sysroot-aware pkg-config wrapper:
#  * prefix paths become "/../../..$PREFIX", which resolves to $PREFIX
#    with or without PKG_CONFIG_SYSROOT_DIR prepended;
#  * "-lfoo" for archives we own becomes an absolute archive path. The SDK
#    compiler wrapper puts "-L <sdk>/user/homebrew/lib" in front of the
#    user's flags, so a plain "-lfreetype"/"-lharfbuzz" would silently pick
#    the SDK copies (no brotli, no ICU) instead of ours.
fix_pkgconfig() {
    local pc
    for pc in "${PREFIX}"/lib/pkgconfig/*.pc "${PREFIX}"/share/pkgconfig/*.pc; do
        [[ -f "${pc}" ]] || continue
        PREFIX="${PREFIX}" REL_PREFIX="${REL_PREFIX}" run perl -i -pe '
            BEGIN { $p = $ENV{PREFIX}; $r = $ENV{REL_PREFIX}; }
            s/(?<!\.\.)\Q$p\E/$r/g;
            if (/^Libs(\.private)?:/) {
                s{(^|\s)-l([\w+.-]+)}{
                    -f "$p/lib/lib$2.a" ? "$1$r/lib/lib$2.a" : "$1-l$2"
                }ge;
            }
        ' "${pc}"
    done
}

ensure_meson() {
    if command -v meson >/dev/null 2>&1 \
        && [[ "$(printf '%s\n' 1.1.0 "$(meson --version)" | sort -V | head -1)" == 1.1.0 ]]; then
        MESON="$(command -v meson)"
        return 0
    fi
    local venv="${WORK}/.venv"
    if [[ ! -x "${venv}/bin/meson" ]]; then
        run python3 -m venv "${venv}"
        run "${venv}/bin/pip" install --quiet "meson==${MESON_PIP_VER}"
    fi
    MESON="${venv}/bin/meson"
}

# Machine file with the SDK path for prospero-cross.ini.
write_meson_constants() {
    MESON_CONSTANTS="${WORK}/meson-sdk.ini"
    mkdir -p "${WORK}"
    printf "[constants]\nsdk = '%s'\n" "${SDK}" > "${MESON_CONSTANTS}"
}

# meson_build <name> [meson options...]
# MESON_TARGETS (optional): build only these targets instead of "all".
meson_build() {
    local name="$1"; shift
    local src="${WORK}/${name}/src" bld="${WORK}/${name}/build"
    if [[ ! -f "${bld}/build.ninja" ]]; then
        run "${MESON}" setup "${bld}" "${src}" \
            --cross-file "${MESON_CONSTANTS}" \
            --cross-file "${SCRIPTS}/prospero-cross.ini" \
            --prefix "${PREFIX}" --libdir lib \
            --buildtype release \
            --default-library static \
            --wrap-mode nodownload \
            "$@"
    fi
    # shellcheck disable=SC2086
    run "${MESON}" compile -C "${bld}" -j "${JOBS}" ${MESON_TARGETS:-}
    run "${MESON}" install -C "${bld}" --no-rebuild
}

# cmake_build <name> [cmake options...]
cmake_build() {
    local name="$1"; shift
    local src="${WORK}/${name}/src" bld="${WORK}/${name}/build"
    run cmake -S "${src}" -B "${bld}" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="${SDK}/toolchain/prospero.cmake" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="${PREFIX}" \
        -DCMAKE_INSTALL_LIBDIR=lib \
        -DCMAKE_FIND_ROOT_PATH="${PREFIX};${SYSROOT}" \
        -DCMAKE_VERBOSE_MAKEFILE=OFF \
        -DPKG_CONFIG_EXECUTABLE="${SCRIPTS}/pkg-config" \
        -DBUILD_SHARED_LIBS=OFF \
        "$@"
    run cmake --build "${bld}" -j "${JOBS}"
    run cmake --install "${bld}"
}

# --- libraries --------------------------------------------------------------

build_icu() {
    fetch "${ICU_URL}" "icu4c-${ICU_VER}-src.tgz" "${ICU_SHA}"
    fetch "${ICU_DATA_URL}" "icu4c-${ICU_VER}-data.zip" "${ICU_DATA_SHA}"
    unpack icu "icu4c-${ICU_VER}-src.tgz"

    local src="${WORK}/icu/src/source"
    local host="${WORK}/icu/build-host" bld="${WORK}/icu/build"
    local -a filter_env=()

    if [[ "${ICU_FULL_DATA}" != 1 ]]; then
        # Replace the prebuilt .dat with the data sources so the filter
        # can be applied (rm is confined to our own extracted tree).
        if [[ ! -d "${src}/data/locales" ]]; then
            run rm -rf "${src}/data"
            run unzip -q "${DL}/icu4c-${ICU_VER}-data.zip" -d "${src}"
        fi
        filter_env=(ICU_DATA_FILTER_FILE="${SCRIPTS}/icu-data-filter.json")
    fi

    # 1. Host build: provides the data tools (genrb, icupkg, ...).
    if [[ ! -f "${host}/.built" ]]; then
        run mkdir -p "${host}"
        (cd "${host}" && run env "${filter_env[@]}" \
            CC=gcc CXX=g++ CFLAGS=-O2 CXXFLAGS=-O2 \
            "${src}/runConfigureICU" Linux \
            --disable-samples --disable-tests --disable-extras \
            --disable-icuio --disable-layoutex)
        run make -C "${host}" -j "${ICU_JOBS}"
        touch "${host}/.built"
    fi

    # 2. Cross build. config.sub does not know "sie-ps5"; the SDK clang
    #    wrapper sets the real target, configure only needs a FreeBSD triple.
    run mkdir -p "${bld}"
    if [[ ! -f "${bld}/Makefile" ]]; then
        (cd "${bld}" && run env "${filter_env[@]}" \
            CC="${SDK}/bin/prospero-clang" \
            CXX="${SDK}/bin/prospero-clang++" \
            AR="${SDK}/bin/prospero-ar" \
            RANLIB="${SDK}/bin/prospero-ranlib" \
            CFLAGS="-O2 -fPIC" CXXFLAGS="-O2 -fPIC" \
            "${src}/configure" \
            --host=x86_64-unknown-freebsd \
            --build="$(sh "${src}/config.guess")" \
            --with-cross-build="${host}" \
            --prefix="${PREFIX}" \
            --enable-static --disable-shared \
            --disable-samples --disable-tests --disable-tools \
            --disable-extras --disable-icuio --disable-layoutex \
            --with-data-packaging=static)
    fi
    run make -C "${bld}" -j "${ICU_JOBS}"
    run make -C "${bld}" install

    # Static ICU needs the C++ runtime; say so in icu-uc.pc so that C
    # consumers linking through pkg-config --static get it too.
    run sed -i 's/^baselibs = .*/baselibs = -lc++ -lc++abi -lunwind -lpthread -lm/' \
        "${PREFIX}/lib/pkgconfig/icu-uc.pc"
}

build_brotli() {
    fetch "${BROTLI_URL}" "brotli-${BROTLI_VER}.tar.gz" "${BROTLI_SHA}"
    unpack brotli "brotli-${BROTLI_VER}.tar.gz"
    cmake_build brotli -DBROTLI_DISABLE_TESTS=ON
}

build_freetype() {
    fetch "${FREETYPE_URL}" "freetype-${FREETYPE_VER}.tar.xz" "${FREETYPE_SHA}"
    unpack freetype "freetype-${FREETYPE_VER}.tar.xz"
    meson_build freetype \
        -Dbrotli=enabled -Dpng=enabled -Dzlib=system \
        -Dharfbuzz=disabled -Dbzip2=disabled -Dtests=disabled
}

build_harfbuzz() {
    fetch "${HARFBUZZ_URL}" "harfbuzz-${HARFBUZZ_VER}.tar.xz" "${HARFBUZZ_SHA}"
    unpack harfbuzz "harfbuzz-${HARFBUZZ_VER}.tar.xz"
    # ICU >= 75 headers need C++17.
    meson_build harfbuzz \
        -Dcpp_std=c++17 \
        -Dicu=enabled -Dicu_builtin=false -Dfreetype=enabled \
        -Dglib=disabled -Dgobject=disabled -Dcairo=disabled \
        -Dchafa=disabled -Dgraphite2=disabled \
        -Dtests=disabled -Ddocs=disabled -Dintrospection=disabled \
        -Dutilities=disabled -Dbenchmark=disabled
}

build_xkbcommon() {
    fetch "${XKBCOMMON_URL}" "libxkbcommon-${XKBCOMMON_VER}.tar.xz" "${XKBCOMMON_SHA}"
    unpack xkbcommon "libxkbcommon-${XKBCOMMON_VER}.tar.xz"
    # Its tests are always configured and are not cross-linkable as-is;
    # build just the library (install only copies, --no-rebuild).
    MESON_TARGETS=xkbcommon meson_build xkbcommon \
        -Denable-x11=false -Denable-wayland=false \
        -Denable-xkbregistry=false -Denable-tools=false \
        -Denable-docs=false -Denable-bash-completion=false
}

build_libwpe() {
    fetch "${LIBWPE_URL}" "libwpe-${LIBWPE_VER}.tar.xz" "${LIBWPE_SHA}"
    unpack libwpe "libwpe-${LIBWPE_VER}.tar.xz"
    meson_build libwpe -Denable-xkb=true -Dbuild-docs=false
}

write_manifest() {
    local m="${PREFIX}/MANIFEST.txt"
    {
        echo "# PS5 WebKit dependency prefix, generated by scripts/ps5/build-deps.sh"
        echo "# sdk: ${SDK} ($("${SDK}/bin/clang" --version | head -1))"
        echo "# icu data: $([[ "${ICU_FULL_DATA}" == 1 ]] && echo full || echo filtered by scripts/ps5/icu-data-filter.json)"
        echo
        echo "## versions (build time)"
        printf '%-10s %-8s %s\n' \
            icu "${ICU_VER}" "${BUILD_TIME[icu]}" \
            brotli "${BROTLI_VER}" "${BUILD_TIME[brotli]}" \
            freetype "${FREETYPE_VER}" "${BUILD_TIME[freetype]}" \
            harfbuzz "${HARFBUZZ_VER}" "${BUILD_TIME[harfbuzz]}" \
            xkbcommon "${XKBCOMMON_VER}" "${BUILD_TIME[xkbcommon]}" \
            libwpe "${LIBWPE_VER}" "${BUILD_TIME[libwpe]}"
        echo
        echo "## sha256 of sources"
        (cd "${DL}" && sha256sum \
            "icu4c-${ICU_VER}-src.tgz" "icu4c-${ICU_VER}-data.zip" \
            "brotli-${BROTLI_VER}.tar.gz" "freetype-${FREETYPE_VER}.tar.xz" \
            "harfbuzz-${HARFBUZZ_VER}.tar.xz" \
            "libxkbcommon-${XKBCOMMON_VER}.tar.xz" \
            "libwpe-${LIBWPE_VER}.tar.xz")
    } > "${m}"
    cat "${m}"
}

# --- main -------------------------------------------------------------------

main() {
    [[ "${PREFIX}" != /user/homebrew* ]] \
        || die "PS5_DEPS_PREFIX points into the target namespace (${PREFIX})"
    [[ -x "${SDK}/bin/prospero-clang" ]] || die "no PS5 payload SDK at ${SDK}"
    local t
    for t in curl tar unzip perl make ninja cmake python3 gcc g++ bison; do
        command -v "${t}" >/dev/null || die "missing host tool: ${t}"
    done

    mkdir -p "${PREFIX}" "${WORK}" "${DL}"
    ensure_meson
    write_meson_constants

    build icu build_icu
    build brotli build_brotli
    build freetype build_freetype
    build harfbuzz build_harfbuzz
    build xkbcommon build_xkbcommon
    build libwpe build_libwpe

    write_manifest
    touch "${PREFIX}/.done"
    echo "=== all dependencies installed into ${PREFIX}"
}

main "$@"

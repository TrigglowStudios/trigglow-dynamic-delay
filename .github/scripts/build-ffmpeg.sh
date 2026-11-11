#!/usr/bin/env bash
# The plugin's own FFmpeg on Linux and macOS: static libavformat, libavcodec
# and libavutil with only what the plugin uses, installed into .deps for
# CMakeLists.txt's OS_LINUX and OS_MACOS blocks:
#
#   Linux  .deps/ffmpeg-linux-<arch>      the MJPEG encoder and decoder (scene
#                                         mode, src/mjpeg-codec.cpp) and the
#                                         stream mode relay; TLS: the system's
#                                         OpenSSL 3 (libssl.so.3), linked
#                                         dynamically
#   macOS  .deps/ffmpeg-macos-universal   the stream mode relay only; one
#                                         build per architecture (arm64,
#                                         x86_64) joined with lipo; TLS:
#                                         Secure Transport (Security.framework)
#
# The stream mode relay (src/stream-relay.cpp) needs the FLV demuxer and
# muxer, the RTMP and RTMPS protocols over TCP and TLS, and the H.264, HEVC,
# AV1 and AAC parsers (avformat_find_stream_info). It decodes nothing.
#
# Why our own and static: the distro's FFmpeg has a different soname on each
# release (libavcodec60 on Ubuntu 24.04, 61 on Debian 13, 62 on Ubuntu 26.04),
# so a package linked to one installs nowhere else, and macOS has no system
# FFmpeg at all. Compiled with -fvisibility=hidden (and on Linux linked with
# --exclude-libs and a version script), none of its symbols is exported, so
# it never clashes with the avcodec and avformat OBS itself loads into the
# same process.
#
# Source: FFmpeg's own git repository (its GitHub mirror) at the release tag
# pinned by buildspec.json's "ffmpeg-source" ("tag"), built only if that tag
# is the pinned commit ("commit") and git verify-tag finds a valid signature
# of the key in ffmpeg-tag-signing-key.asc, pinned below by fingerprint. The
# source of that commit is kept as a tarball for the release (LGPL). Licence:
# LGPL 2.1 or later. The build stops if FFmpeg's configure reports anything
# else (a GPL, version 3 or nonfree build), the same guard the Windows job
# applies to its FFmpeg DLLs. OpenSSL 3 (Apache 2.0) and Secure Transport are
# system libraries, not part of this build.
#
# Usage: build-ffmpeg.sh   (from anywhere; rebuilds only when the pin or
# this script change). Bash 3.2 compatible (macOS's /bin/bash).

set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
spec="${root}/buildspec.json"
os=$(uname -s)

sha256_of() {
  if command -v sha256sum > /dev/null; then
    sha256sum "$1" | cut -d ' ' -f 1
  else
    shasum -a 256 "$1" | cut -d ' ' -f 1
  fi
}

case ${os} in
  Linux)
    target="linux-$(uname -m)"
    archs=("$(uname -m)")
    jobs=$(nproc)
    ;;
  Darwin)
    target=macos-universal
    archs=(arm64 x86_64)
    jobs=$(sysctl -n hw.ncpu)
    # The plugin's own minimum (CMakePresets.json), so the linker sees no
    # object built for a newer macOS than the plugin.
    macos_min=$(jq -r '.configurePresets[] | select(.name == "macos") | .cacheVariables.CMAKE_OSX_DEPLOYMENT_TARGET' \
      "${root}/CMakePresets.json")
    [[ ${macos_min} =~ ^[0-9]+\.[0-9]+$ ]] || {
      echo "${0##*/}: no CMAKE_OSX_DEPLOYMENT_TARGET in CMakePresets.json's macos preset" >&2
      exit 1
    }
    ;;
  *)
    echo "${0##*/}: ${os} is not supported (Windows uses BtbN's build, buildspec.json \"ffmpeg\")" >&2
    exit 1
    ;;
esac

version=$(jq -r '.dependencies["ffmpeg-source"].version' "${spec}")
repo_url=$(jq -r '.dependencies["ffmpeg-source"].baseUrl' "${spec}")
tag=$(jq -r '.dependencies["ffmpeg-source"].tag' "${spec}")
commit=$(jq -r '.dependencies["ffmpeg-source"].commit // empty' "${spec}")

if [[ ! ${commit} =~ ^[0-9a-f]{40}$ ]]; then
  echo "${0##*/}: buildspec.json has no ffmpeg-source commit" >&2
  exit 1
fi

prefix="${root}/.deps/ffmpeg-${target}"
work="${root}/.deps/ffmpeg-${version}-build"
stamp="${prefix}/.trigglow-build"
want="${version} ${tag} ${commit} $(sha256_of "${BASH_SOURCE[0]}")"

if [[ -f ${stamp} && $(< "${stamp}") == "${want}" && -f ${prefix}/lib/libavformat.a
  && -f ${prefix}/share/source/ffmpeg-${tag}.tar.xz ]]; then
  echo "FFmpeg ${version} (static, ${target}) is up to date in ${prefix}"
  exit 0
fi

rm -rf "${work}" "${prefix}"
src="${work}/src"
mkdir -p "${src}"
git -C "${src}" init --quiet
git -C "${src}" fetch --quiet --depth 1 "${repo_url}.git" "refs/tags/${tag}:refs/tags/${tag}"
# Never build anything else than the pinned commit.
fetched=$(git -C "${src}" rev-parse "refs/tags/${tag}^{commit}")
if [[ ${fetched} != "${commit}" ]]; then
  echo "::error title=FFmpeg source::${tag} is ${fetched}, not the pinned ${commit}" >&2
  exit 1
fi
# The tag must carry a valid signature of the key kept in this repository
# (ffmpeg-tag-signing-key.asc: Michael Niedermayer, FFmpeg's release
# maintainer, who signs the release tags), pinned here by fingerprint, in a
# keyring of its own so that no other key counts.
tag_key="${root}/.github/scripts/ffmpeg-tag-signing-key.asc"
tag_key_fpr=DD1EC9E8DE085C629B3E1846B18E8928B3948D64
gnupg_home=$(mktemp -d)
trap 'rm -rf "${gnupg_home}"' EXIT
chmod 700 "${gnupg_home}"
GNUPGHOME="${gnupg_home}" gpg --batch --quiet --import "${tag_key}"
if ! status=$(GNUPGHOME="${gnupg_home}" git -C "${src}" verify-tag --raw "${tag}" 2>&1); then
  echo "${status}" >&2
  echo "::error title=FFmpeg source::${tag} has no valid signature of ${tag_key_fpr}" >&2
  exit 1
fi
if ! grep -Eq "^\[GNUPG:\] VALIDSIG [0-9A-F]+ .* ${tag_key_fpr}\$" <<< "${status}"; then
  echo "${status}" >&2
  echo "::error title=FFmpeg source::${tag} is not signed by ${tag_key_fpr}" >&2
  exit 1
fi
echo "${tag}: good signature of ${tag_key_fpr}"
git -C "${src}" -c advice.detachedHead=false checkout --quiet "${commit}"

common_args=(
  --enable-static
  --disable-shared
  --enable-pic
  --disable-debug
  --disable-doc
  --disable-programs
  --disable-autodetect
  --disable-everything
  --disable-avdevice
  --disable-avfilter
  --disable-swscale
  --disable-swresample
  --enable-avcodec
  --enable-avutil
  --enable-avformat
  --enable-network
  --enable-demuxer=flv
  --enable-muxer=flv
  --enable-protocol=tcp,tls,rtmp,rtmps
  --enable-parser=h264,hevc,av1,aac
)
required=(
  CONFIG_FLV_DEMUXER
  CONFIG_FLV_MUXER
  CONFIG_TCP_PROTOCOL
  CONFIG_TLS_PROTOCOL
  CONFIG_RTMP_PROTOCOL
  CONFIG_RTMPS_PROTOCOL
)
if [[ ${os} == Linux ]]; then
  # Scene mode's MJPEG storage (src/mjpeg-codec.cpp), and TLS for RTMPS
  # through the system's OpenSSL 3, linked dynamically.
  common_args+=(--enable-encoder=mjpeg --enable-decoder=mjpeg --enable-openssl --extra-cflags=-fvisibility=hidden)
  required+=(CONFIG_MJPEG_ENCODER CONFIG_MJPEG_DECODER CONFIG_OPENSSL)
else
  # TLS for RTMPS through Secure Transport, part of macOS (Security.framework).
  common_args+=(--enable-securetransport)
  required+=(CONFIG_SECURETRANSPORT)
fi

# Licence guard: only an LGPL 2.1-or-later build may be linked into the
# plugin (data/licenses/LICENSE-ffmpeg.txt says so).
fail_license() {
  echo "::error title=FFmpeg licence::${1}" >&2
  exit 1
}

for arch in "${archs[@]}"; do
  build="${work}/build-${arch}"
  if [[ ${os} == Darwin ]]; then
    arch_prefix="${work}/install-${arch}"
  else
    arch_prefix="${prefix}"
  fi
  args=(--prefix="${arch_prefix}" "${common_args[@]}")
  if [[ ${os} == Darwin ]]; then
    darwin_flags="-arch ${arch} -mmacosx-version-min=${macos_min}"
    args+=(
      --enable-cross-compile
      --target-os=darwin
      --arch="${arch}"
      --cc=clang
      "--extra-cflags=${darwin_flags} -fvisibility=hidden"
      "--extra-ldflags=${darwin_flags}"
    )
    # No nasm needed, and the relay decodes nothing: plain C on x86_64.
    if [[ ${arch} == x86_64 ]]; then
      args+=(--disable-x86asm)
    fi
  fi

  mkdir -p "${build}"
  pushd "${build}" > /dev/null
  "${src}/configure" "${args[@]}"

  grep -qx '#define CONFIG_GPL 0' config.h || fail_license 'configured with --enable-gpl'
  grep -qx '#define CONFIG_NONFREE 0' config.h || fail_license 'configured with --enable-nonfree'
  grep -qx '#define CONFIG_VERSION3 0' config.h || fail_license 'configured with --enable-version3'
  grep -qx '#define FFMPEG_LICENSE "LGPL version 2.1 or later"' config.h \
    || fail_license "licence is $(grep FFMPEG_LICENSE config.h || echo unknown), not LGPL version 2.1 or later"
  for component in "${required[@]}"; do
    if ! cat config.h config_components.h 2> /dev/null | grep -qx "#define ${component} 1"; then
      echo "${0##*/}: ${component} is not enabled (${arch})" >&2
      exit 1
    fi
  done

  make -j"${jobs}"
  make install
  popd > /dev/null
done

if [[ ${os} == Darwin ]]; then
  # One universal archive per library, like the plugin itself (arm64;x86_64).
  mkdir -p "${prefix}/lib"
  cp -R "${work}/install-${archs[0]}/include" "${prefix}/include"
  for lib in libavformat.a libavcodec.a libavutil.a; do
    lipo -create "${work}/install-arm64/lib/${lib}" "${work}/install-x86_64/lib/${lib}" -output "${prefix}/lib/${lib}"
    lipo -info "${prefix}/lib/${lib}"
  done
fi

# LGPL: the exact source of this build, which the Linux packaging puts in
# release/ for the GitHub release next to the packages (one tag for Linux and
# macOS). git archive of the pinned commit (FFmpeg's own tarballs also add a
# VERSION file; this one is made the same way here) and single-threaded xz,
# so it is the same file on every build.
mkdir -p "${prefix}/share/source"
git -C "${src}" archive --format=tar --prefix="ffmpeg-${tag}/" \
  --add-virtual-file="ffmpeg-${tag}/VERSION:${version}" "${commit}" \
  | xz -T1 -9 > "${prefix}/share/source/ffmpeg-${tag}.tar.xz"

# The licence travels with the packages (.github/scripts/linux/package.sh;
# on macOS CMakeLists.txt installs it into the bundle's data/licenses).
mkdir -p "${prefix}/share/licenses"
cp "${src}/COPYING.LGPLv2.1" "${prefix}/share/licenses/COPYING.LGPLv2.1"
{
  echo "FFmpeg ${version} (${target})"
  echo "Source: ${repo_url}, tag ${tag}"
  echo "Commit: ${commit}"
  echo "Configure: ${common_args[*]}"
} > "${prefix}/share/licenses/ffmpeg-build.txt"

rm -rf "${work}"
echo "${want}" > "${stamp}"
echo "FFmpeg ${version} (static, ${target}, LGPL 2.1 or later) installed in ${prefix}"

#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
build_dir="${BUILD_DIR:-$repo_root/build}"
build_type="${BUILD_TYPE:-RelWithDebInfo}"
generator="${CMAKE_GENERATOR:-}"

setup_candidates=(
  "$repo_root/../nebula/install/setup.bash"
  "$repo_root/../install/setup.bash"
)

have_nebula_pkg=0
if [[ "${CMAKE_PREFIX_PATH:-}" == *"nebula_core_common"* ]]; then
  have_nebula_pkg=1
fi

if [[ "${CMAKE_PREFIX_PATH:-}" == *"$repo_root/../install"* ]] && [[ "${CMAKE_PREFIX_PATH:-}" != *"$repo_root/../nebula/install"* ]]; then
  cat >&2 <<EOF
Detected the older workspace overlay in CMAKE_PREFIX_PATH:
  $repo_root/../install

This repository expects the newer Nebula workspace at:
  $repo_root/../nebula/install

Use a fresh shell, or source only:
  source $repo_root/../nebula/install/setup.bash

Then re-run:
  ./build.sh
EOF
  exit 1
fi

if [[ $have_nebula_pkg -eq 0 ]]; then
  for setup_script in "${setup_candidates[@]}"; do
    if [[ -f "$setup_script" ]]; then
      export COLCON_TRACE="${COLCON_TRACE:-}"
      set +u
      # shellcheck disable=SC1090
      source "$setup_script"
      set -u
      have_nebula_pkg=1
      echo "Sourced workspace: $setup_script"
      break
    fi
  done
fi

if [[ $have_nebula_pkg -eq 0 ]]; then
  cat >&2 <<EOF
Could not find a Nebula workspace setup script.

Expected one of:
  ${setup_candidates[0]}
  ${setup_candidates[1]}

Build the parent workspace first or source its setup script manually, then re-run:
  source /path/to/install/setup.bash
  ./build.sh
EOF
  exit 1
fi

mkdir -p "$build_dir"
cd "$build_dir"

cmake_args=(
  -DCMAKE_BUILD_TYPE="$build_type"
  "$repo_root"
)

if [[ -n "$generator" ]]; then
  cmake_args=(-G "$generator" "${cmake_args[@]}")
fi

cmake "${cmake_args[@]}"
cmake --build . --parallel "${BUILD_JOBS:-$(nproc)}"

echo "Built: $build_dir/pcap_visualizer"

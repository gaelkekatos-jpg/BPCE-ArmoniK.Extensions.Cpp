#!/usr/bin/env bash
# Runs the unit tests of a coverage-instrumented build and produces an lcov/HTML report.
#
# Usage: tools/coverage.sh [build-dir] [extra ctest args...]
#   build-dir  CMake build directory configured with -DARMONIK_COVERAGE=ON (default: build)
#
# Environment:
#   COVERAGE_LABEL   ctest label to run (default: unit; empty runs every test)
#   COVERAGE_OUTPUT  output directory (default: <repo>/coverage-report)
#   GCOV_TOOL        gcov executable matching the compiler (default: gcov)
#
# Outputs <output>/lcov.info (filtered, e.g. for Codecov) and <output>/index.html.

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/.." && pwd)"

build_dir="${1:-build}"
[ $# -gt 0 ] && shift
label="${COVERAGE_LABEL-unit}"
output_dir="${COVERAGE_OUTPUT:-${repo_root}/coverage-report}"
gcov_tool="${GCOV_TOOL:-gcov}"

die() {
  echo "coverage.sh: error: $*" >&2
  exit 1
}

for tool in ctest lcov genhtml "${gcov_tool}"; do
  command -v "${tool}" >/dev/null 2>&1 || die "'${tool}' not found in PATH (install lcov and a gcov matching your compiler)."
done

[ -d "${build_dir}" ] || die "build directory '${build_dir}' does not exist."
build_dir="$(cd "${build_dir}" && pwd)"
cache="${build_dir}/CMakeCache.txt"
[ -f "${cache}" ] || die "'${build_dir}' is not a CMake build directory (no CMakeCache.txt)."
grep -Eq '^ARMONIK_COVERAGE:BOOL=(ON|TRUE|1|YES)$' "${cache}" ||
  die "'${build_dir}' was not configured with -DARMONIK_COVERAGE=ON; reconfigure and rebuild first."

ctest_filter=()
[ -n "${label}" ] && ctest_filter=(-L "${label}")

test_count="$(cd "${build_dir}" && ctest -N ${ctest_filter[@]+"${ctest_filter[@]}"} "$@" | sed -n 's/^Total Tests: \([0-9]*\)$/\1/p')"
if [ -z "${test_count}" ] || [ "${test_count}" -eq 0 ]; then
  die "no tests match 'ctest -N ${ctest_filter[*]} $*' in '${build_dir}'. Make sure the test targets are built" \
    "(-DBUILD_TESTS=ON) and that tests carry the '${label}' ctest label."
fi

# lcov >= 2.0 turns several benign gcov inconsistencies into hard errors; listing a category twice
# also silences its warning.
lcov_major="$(lcov --version | sed -n 's/.*LCOV version \([0-9]*\).*/\1/p')"
lcov_opts=(--gcov-tool "${gcov_tool}")
genhtml_opts=()
if [ "${lcov_major:-1}" -ge 2 ]; then
  ignored="mismatch,mismatch,negative,negative,unused,unused,inconsistent,inconsistent,empty,empty"
  lcov_opts+=(--ignore-errors "${ignored}" --rc geninfo_unexecuted_blocks=1)
  genhtml_opts+=(--ignore-errors "${ignored}")
fi

mkdir -p "${output_dir}"
tmp_dir="$(mktemp -d)"
trap 'rm -rf "${tmp_dir}"' EXIT

echo "==> Resetting coverage counters in ${build_dir}"
lcov "${lcov_opts[@]}" --quiet --zerocounters --directory "${build_dir}"

echo "==> Capturing baseline (so unexecuted SDK files report 0%)"
lcov "${lcov_opts[@]}" --quiet --capture --initial --directory "${build_dir}" --base-directory "${repo_root}" \
  --output-file "${tmp_dir}/baseline.info"

echo "==> Running ctest --output-on-failure ${ctest_filter[*]+${ctest_filter[*]} }$* (${test_count} tests)"
test_status=0
(cd "${build_dir}" && ctest --output-on-failure ${ctest_filter[@]+"${ctest_filter[@]}"} "$@") || test_status=$?

echo "==> Capturing coverage"
lcov "${lcov_opts[@]}" --quiet --capture --directory "${build_dir}" --base-directory "${repo_root}" \
  --output-file "${tmp_dir}/tests.info"
lcov "${lcov_opts[@]}" --quiet --add-tracefile "${tmp_dir}/baseline.info" --add-tracefile "${tmp_dir}/tests.info" \
  --output-file "${tmp_dir}/total.info"

echo "==> Filtering system headers, dependencies, generated and test sources"
lcov "${lcov_opts[@]}" --quiet --extract "${tmp_dir}/total.info" "${repo_root}/*" --output-file "${tmp_dir}/sdk.info"
lcov "${lcov_opts[@]}" --quiet --remove "${tmp_dir}/sdk.info" \
  '/usr/*' \
  '*/_deps/*' \
  '*.pb.h' '*.pb.cc' \
  '*/ArmoniK.SDK.Client.Test/*' \
  '*/ArmoniK.SDK.Worker.Test/*' \
  '*/ArmoniK.SDK.Worker.Test.Unit/*' \
  --output-file "${output_dir}/lcov.info"

echo "==> Generating HTML report"
genhtml ${genhtml_opts[@]+"${genhtml_opts[@]}"} --quiet --prefix "${repo_root}" --title "ArmoniK C++ SDK" \
  --output-directory "${output_dir}" "${output_dir}/lcov.info"

lcov "${lcov_opts[@]}" --summary "${output_dir}/lcov.info"
echo "==> Report: ${output_dir}/index.html"
echo "==> Tracefile: ${output_dir}/lcov.info"

if [ "${test_status}" -ne 0 ]; then
  die "ctest failed with exit code ${test_status} (coverage report was still generated)."
fi

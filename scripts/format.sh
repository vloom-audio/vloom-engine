#!/usr/bin/env bash
# Formats all C/C++ sources with clang-format.
#   scripts/format.sh          rewrite files in place
#   scripts/format.sh --check  only report files that need formatting (used by CI)
#
# clang-format output changes between releases, so every machine and CI must use
# the exact version below. Install it with:
#   pipx install clang-format==<version>     (or: pip install clang-format==<version>)
# Set CLANG_FORMAT=/path/to/clang-format to use a binary that is not first on PATH.
set -euo pipefail

readonly required_version="23.1.3"

cd "$(dirname "$0")/.."

clang_format="${CLANG_FORMAT:-clang-format}"
if ! command -v "$clang_format" >/dev/null 2>&1; then
    echo "error: $clang_format not found; install it with: pipx install clang-format==$required_version" >&2
    exit 1
fi

actual_version="$("$clang_format" --version | sed -nE 's/.*clang-format version ([0-9]+\.[0-9]+\.[0-9]+).*/\1/p')"
if [[ "$actual_version" != "$required_version" ]]; then
    echo "error: clang-format $required_version required, found ${actual_version:-unknown} ($clang_format)" >&2
    echo "       install it with: pipx install clang-format==$required_version" >&2
    echo "       or point CLANG_FORMAT at the right binary" >&2
    exit 1
fi

mode=(-i)
if [[ "${1:-}" == "--check" ]]; then
    mode=(--dry-run --Werror)
fi

# --others includes files not yet added to git; --exclude-standard honours .gitignore.
git ls-files -z --cached --others --exclude-standard '*.cpp' '*.hpp' '*.c' '*.h' \
    | xargs -0 "$clang_format" "${mode[@]}"
echo "clang-format $required_version: ok"

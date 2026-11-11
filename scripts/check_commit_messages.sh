#!/usr/bin/env bash
# Checks that every commit subject in a range follows the project format:
#   <gitmoji> <type>(<scope>)?: <subject>      e.g. "✨ feat(engine): add swing"
# Types: feat fix sec ops re refactor test docs chore. Merge, revert, fixup! and
# squash! commits are skipped. Needs git and node (preinstalled on CI runners).
#
#   scripts/check_commit_messages.sh <git-range>     e.g. origin/main..HEAD
set -euo pipefail

range="${1:?usage: $0 <git-range>, e.g. origin/main..HEAD}"

git log --format=%s "$range" | node -e '
const subjects = require("fs").readFileSync(0, "utf8").split("\n").filter(Boolean);
const valid = /^\p{Extended_Pictographic}️?\s*(feat|fix|sec|ops|re|refactor|test|docs|chore)(\([a-zA-Z0-9_\-.\/]+\))?!?: .+/u;
const skipped = /^(Merge |Revert |revert|fixup! |squash! )/;
const bad = subjects.filter((s) => !skipped.test(s) && !valid.test(s));
for (const s of bad) console.log(`bad commit message: ${s}`);
if (bad.length > 0) {
    console.log("\nExpected \"<gitmoji> <type>(<scope>): <subject>\", e.g. \"✨ feat(engine): add swing\".");
    console.log("Types: feat fix sec ops re refactor test docs chore. See CONTRIBUTING.md.");
    process.exit(1);
}
console.log(`commit messages: ${subjects.length} ok`);
'

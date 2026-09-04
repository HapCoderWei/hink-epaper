#!/usr/bin/env bash
set -eu
root="$(cd "$(dirname "$0")/.." && pwd)"
revision=3778d296f418c30b05310d86dafa4e3404071cb4
target="$root/build/firmware"
if [ -e "$target" ]; then
  echo "Refusing to overwrite $target; preserve/rename it before preparing again." >&2
  exit 1
fi
cache="$(mktemp -d "${TMPDIR:-/tmp}/hink-upstream.XXXXXX")"
git clone --no-checkout https://github.com/atc1441/ATC_TLSR_Paper.git "$cache/upstream"
git -C "$cache/upstream" checkout --detach "$revision"
test "$(git -C "$cache/upstream" rev-parse HEAD)" = "$revision"
mkdir -p "$target"
for part in components make static_src tc32_linux; do
  cp -R "$cache/upstream/Firmware/$part" "$target/$part"
done
# Upstream executables may lack executable bits. Do this before parallel make;
# its independent chmod_all target can otherwise race the first compile.
chmod -R u+rx "$target/tc32_linux"
cp -R "$root/firmware/src" "$target/src"
cp "$root/firmware/makefile" "$target/makefile"
printf 'Prepared %s\nUpstream cache retained: %s\n' "$target" "$cache"

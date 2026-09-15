#!/usr/bin/env bash
set -eu
root="$(cd "$(dirname "$0")/.." && pwd)"
source_dir="$root/firmware"
test_dir="$(mktemp -d "${TMPDIR:-/tmp}/hink-b1-test.XXXXXX")"
awk '/^_attribute_ram_code_ void EPD_SPI_Write/{exit} !/^#include/' "$source_dir/src/epd_spi.c" > "$test_dir/production.inc"
sed '/^#include/d' "$source_dir/src/epd.c" >> "$test_dir/production.inc"
sed '/^#include/d' "$source_dir/src/led.c" >> "$test_dir/production.inc"
cc -std=c99 -Wall -Wextra -Werror -I "$test_dir" "$root/tests/epd_power_test.c" -o "$test_dir/epd_power_test"
"$test_dir/epd_power_test"
cc -std=c99 -Wall -Wextra -Werror -I "$test_dir" "$root/tests/epd_ssd1680_test.c" -o "$test_dir/epd_ssd1680_test"
"$test_dir/epd_ssd1680_test"
sed '/^#include/d' "$source_dir/src/ota.c" > "$test_dir/production_ota.inc"
cc -std=c99 -Wall -Wextra -Werror -I "$test_dir" -I "$source_dir/src" \
  "$root/tests/ota_v2_test.c" "$source_dir/src/ota_recovery.c" \
  -o "$test_dir/ota_v2_test"
"$test_dir/ota_v2_test"
cc -std=c99 -Wall -Wextra -Werror -I "$source_dir/src" \
  "$root/tests/ota_recovery_test.c" "$source_dir/src/ota_recovery.c" \
  -o "$test_dir/ota_recovery_test"
"$test_dir/ota_recovery_test"
python3 "$root/tests/ota_manifest_test.py"
node "$root/tests/image_processing_test.js"
node "$root/tests/web_ui_test.js"
printf 'Generated test files retained at %s\n' "$test_dir"

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
printf 'Generated test files retained at %s\n' "$test_dir"


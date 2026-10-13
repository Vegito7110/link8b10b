#!/usr/bin/env bash
# Mutation testing: plant one small bug at a time in a copy of the RTL and
# check that the testbench notices. A testbench that passes buggy RTL is useless.
# Mutants are built with -Wno-fatal: some bugs (e.g. an unused signal) are
# also lint warnings, but the point here is whether the *simulation* catches them.
# Usage: scripts/mutation_test.sh
set -uo pipefail
cd "$(dirname "$0")/.."

# file # sed expression (basic regex, '@' delimiter) # description
MUTANTS=(
  "rtl/link_pkg.sv#s@(!rd1 && (x == 5'd17@(!rd1 \&\& (x == 5'd16@#alternate-7 rule uses x=16 instead of x=17"
  "rtl/link_pkg.sv#s@5'd13: return 6'b101100;@5'd13: return 6'b101010;@#one 5b/6b table entry wrong (D13)"
  "rtl/link_pkg.sv#s@if (c == 4'b0011) return 1'b1;@if (c == 4'b0011) return rd;@#RD rule for the 0011 sub-block"
  "rtl/link_pkg.sv#s@32'hEDB8_8320@32'hEDB8_8321@#CRC polynomial off by one bit"
  "rtl/dec8b10b.sv#s@out_disp_err <= !ok_same && ok_other;@out_disp_err <= 1'b0;@#decoder never reports disparity errors"
  "rtl/dec8b10b.sv#s@rd           <= rd_next;@rd           <= rd;@#decoder never updates its RD"
  "rtl/scrambler.sv#s@16'h0039@16'h0029@#scrambler LFSR tap missing"
  "rtl/scrambler.sv#s@lfsr_next = 16'hFFFF;@lfsr_next = 16'hFFFE;@#COM re-seeds the LFSR with the wrong value"
  "rtl/link_top.sv#s@crc       <= CRC_INIT;@crc       <= crc;@#CRC not reset at STP"
)

caught=0; total=0
for m in "${MUTANTS[@]}"; do
  IFS='#' read -r file expr desc <<< "$m"
  total=$((total + 1))
  work=$(mktemp -d)
  cp -r rtl tb model tests Makefile "$work"/
  mkdir -p "$work/build"
  sed -i "$expr" "$work/$file"
  if cmp -s "$file" "$work/$file"; then
    echo "ERROR  mutation did not apply: $desc"; rm -rf "$work"; continue
  fi
  if ! (cd "$work" && make -s build/obj/Vlink_top VEXTRA=-Wno-fatal > build.log 2>&1); then
    echo "ERROR  mutant does not build: $desc"; rm -rf "$work"; continue
  fi
  if (cd "$work" && ./build/obj/Vlink_top --packets 500 > sim.log 2>&1); then
    echo "MISSED $desc"
  else
    first=$(grep -m1 -E "ERROR|FAIL|INCOMPLETE" "$work/sim.log" | sed 's/^ *//' | cut -c1-120)
    echo "caught $desc"
    echo "         -> $first"
    caught=$((caught + 1))
  fi
  rm -rf "$work"
done
echo "$caught / $total mutants caught"
[[ $caught -eq $total ]]

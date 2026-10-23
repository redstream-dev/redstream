#!/bin/sh
# Usage: rds_test3_dry_run.sh <path-to-rds-test3>
set -eu
out=$("$1" --dry-run file:///dev/null file:///dev/null)
echo "$out"
echo "$out" | grep -qx 'sources: 2'
echo "$out" | grep -qx 'nvstreammux -> rdsstreammux'
echo "$out" | grep -qx 'nvinfer -> rdsinfer'
echo "$out" | grep -qx 'nvmultistreamtiler -> rdsmultistreamtiler'
echo "$out" | grep -qx 'nveglglessink -> rdsvideosink'

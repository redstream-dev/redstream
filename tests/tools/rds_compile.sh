#!/bin/sh
# Usage: rds_compile.sh <rds-compile> <gst-launch-1.0> <scale2.onnx> <work dir>
# rds-compile on the scale2 test model in <work dir>/rds-compile: build, up to date, --force, overrides, -o, option
# and config errors, then rdsinfer in gst-launch-1.0 loading the program instead of building it. An empty
# <rds-compile> (built without rdsinfer) or no ROCm device skips; an empty <gst-launch-1.0> (not found, or a sanitized
# build, whose plugins it can't load) skips the element checks.
set -eu
compile=$1 launch=$2 model=$3 work=$4

if [ -z "$compile" ]; then
  echo 'built without rdsinfer: skipped'
  exit 77
fi
if [ ! -e /dev/kfd ]; then
  echo 'no ROCm device (/dev/kfd): skipped'
  exit 77
fi

dir=$work/rds-compile
rm -rf "$dir"
mkdir -p "$dir"
cp "$model" "$dir/scale2.onnx"
cfg=$dir/cfg.txt
printf '[property]\nonnx-file=scale2.onnx\ninfer-dims=3;48;64\nnetwork-type=100\n' >"$cfg"
# a valid nvinfer key file that rdsinfer refuses: no model
bad=$dir/bad.txt
printf '[property]\nnetwork-type=100\n' >"$bad"

fail() {
  echo "FAIL: $*"
  exit 1
}

# run WANT ARGS...: runs rds-compile ARGS, checks its exit status is WANT; stdout in $out, stderr in $err
run() {
  want=$1
  shift
  status=0
  "$compile" "$@" >"$dir/stdout" 2>"$dir/stderr" || status=$?
  out=$(cat "$dir/stdout")
  err=$(cat "$dir/stderr")
  echo "rds-compile $*: exit $status"
  [ -z "$out" ] || echo "  stdout: $out"
  [ -z "$err" ] || echo "$err" | sed 's/^/  stderr: /'
  [ "$status" -eq "$want" ] || fail "exit $status, expected $want"
}

# built: one line on stdout, the program and its manifest exist
run 0 -c "$cfg"
prog=$out
case $prog in
  "$dir"/scale2_b1_3x48x64_gfx*_fp32.mxr) ;;
  *) fail "unexpected program path $prog" ;;
esac
[ -f "$prog" ] && [ -f "$prog.json" ] || fail "$prog or its manifest is missing"
echo "$err" | grep -q '^rds-compile: built ' || fail 'no build reason on stderr'

# up to date, then rebuilt with --force
run 2 -c "$cfg"
[ "$out" = "$prog" ] || fail "up to date: printed $out"
run 0 --force -c "$cfg"
[ "$out" = "$prog" ] || fail "--force: printed $out"
echo "$err" | grep -q 'forced' || fail '--force: the reason does not say forced'

# overrides: another batch size and precision is another program
run 0 -c "$cfg" -b 2 --fp16
prog2=$out
case $prog2 in
  "$dir"/scale2_b2_*fp16.mxr) ;;
  *) fail "unexpected program path $prog2" ;;
esac
[ -f "$prog2" ] && [ -f "$prog2.json" ] || fail "$prog2 or its manifest is missing"
run 2 -c "$cfg" --batch-size=2 --fp16
[ "$out" = "$prog2" ] || fail "up to date: printed $out"
run 2 -c "$cfg" --fp32
[ "$out" = "$prog" ] || fail "--fp32: printed $out"

# -o: written there, with its manifest next to it, and checked there
run 0 -c "$cfg" -o "$dir/custom.mxr"
[ "$out" = "$dir/custom.mxr" ] && [ -f "$dir/custom.mxr.json" ] || fail '-o: program or manifest missing'
run 2 -c "$cfg" -o "$dir/custom.mxr"
run 0 -c "$cfg" -o "$dir/sub/x.mxr"
[ -f "$dir/sub/x.mxr.json" ] || fail '-o: the directory was not made'
# a directory that can't be written: built but not saved, an error
mkdir -m 0555 "$dir/ro"
if [ -w "$dir/ro" ]; then
  echo 'running as root: a 0555 directory is still writable, unwritable case skipped'
else
  run 1 -c "$cfg" -o "$dir/ro/x.mxr"
  [ -z "$out" ] || fail "unwritable -o: printed $out"
fi
chmod 0755 "$dir/ro"

# bad options: exit 1, an error prefixed with the program name, nothing on stdout
for args in "-c $cfg --fp16 --fp32" "-c $cfg -b 0" "-c $cfg -b 1025" "-c $cfg -g -1" "-c $cfg extra" \
    "-c $cfg --no-such-option" "-b 2" "-c $cfg -g 99"; do
  # shellcheck disable=SC2086
  run 1 $args
  [ -z "$out" ] || fail "$args: printed $out"
  echo "$err" | grep -q '^rds-compile: ' || fail "$args: no prefixed error"
done
run 1 -c "$dir/missing.txt"

# a bad config: the element's own message
run 1 -c "$bad"
echo "$err" | grep -q "^rds-compile: $bad:onnx-file: " || fail 'bad config: unexpected message'
compile_msg=$(echo "$err" | sed -n 's/^rds-compile: //p' | head -n 1)

if [ -z "$launch" ]; then
  echo 'no gst-launch-1.0 given (not found, or a sanitized build): element checks skipped'
  exit 0
fi
pipeline() {
  # shellcheck disable=SC2086
  GST_DEBUG=rdsinfer:4 GST_DEBUG_NO_COLOR=1 "$launch" -q videotestsrc num-buffers=2 \
    ! video/x-raw,format=NV12,width=64,height=48 ! m.sink_0 \
    rdsstreammux name=m batch-size=1 width=64 height=48 ! rdsinfer config-file-path="$1" ! fakesink \
    >"$dir/launch.log" 2>&1
}
status=0
pipeline "$bad" || status=$?
[ "$status" -ne 0 ] || fail 'the element accepted the bad config'
element_msg=$(sed -n 's/^ERROR: from element [^ ]*: //p' "$dir/launch.log" | head -n 1)
echo "element: $element_msg"
[ "$element_msg" = "$compile_msg" ] || fail "rds-compile said \"$compile_msg\", the element \"$element_msg\""

pipeline "$cfg" || { cat "$dir/launch.log"; fail 'the pipeline failed'; }
grep "rdsinfer" "$dir/launch.log" | grep -E 'loaded|built' || true
grep -q "loaded $prog\$" "$dir/launch.log" || fail "the element did not load $prog"
if grep -q "built .* because" "$dir/launch.log"; then
  fail 'the element built a program'
fi
echo 'ok'

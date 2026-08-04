#!/usr/bin/env bash
#
# End-to-end verification against the real bus.
#
#   ./scripts/verify.sh              # everything
#   ./scripts/verify.sh --phase 2    # one phase
#   PI=pi@10.0.0.5 ./scripts/verify.sh
#
# Runs from the Mac and drives the Pi over SSH. Exits non-zero if any check
# fails, so it can gate a commit.
#
# THE RULE THIS SCRIPT LIVES BY: it must not contain the numbers it is
# checking. Steps per revolution, the angle limit and the step interval are all
# read from the running slave over CoE, and everything else is derived from
# those. A literal 200 or 180 anywhere below is a bug, because it would let the
# script agree with firmware that had drifted away from its own devicetree —
# which is precisely the failure it exists to catch.
#
# The ESP32 must be flashed with a SOES + nema17 build, the 12 V supply on, and
# the Pi reachable.
set -uo pipefail

PI="${PI:-pi@192.168.0.133}"
IFACE="${IFACE:-eth0}"
MASTER="sudo ~/ethercat/master/build/servo_master ${IFACE}"

PHASE_FILTER="${1:-}"
if [ "${PHASE_FILTER}" = "--phase" ]; then
	PHASE_FILTER="${2:-}"
else
	PHASE_FILTER=""
fi

PASS=0
FAIL=0

# --- reporting -------------------------------------------------------------

ok()   { printf '  \033[32mPASS\033[0m  %-46s %s\n' "$1" "${2:-}"; PASS=$((PASS + 1)); }
bad()  { printf '  \033[31mFAIL\033[0m  %-46s %s\n' "$1" "${2:-}"; FAIL=$((FAIL + 1)); }
note() { printf '        %-46s %s\n' "$1" "${2:-}"; }
head_() { printf '\n\033[1m%s\033[0m\n' "$1"; }

# check <label> <condition-exit-status> <detail>
check() { if [ "$2" -eq 0 ]; then ok "$1" "$3"; else bad "$1" "$3"; fi; }

want_phase() { [ -z "${PHASE_FILTER}" ] || [ "${PHASE_FILTER}" = "$1" ]; }

m() { ssh -o ConnectTimeout=10 "${PI}" "${MASTER} $*" 2>&1; }

# Grep the output of a master command, ignoring its exit status.
#
# Needed because `set -o pipefail` is on and several of these commands exit
# non-zero BY DESIGN — a refused SDO write is a correct result, and the master
# reports it as EXIT_FAILURE. Piping straight into grep made the pipeline fail
# whenever the thing being tested for actually happened, so the two refusal
# checks failed while the behaviour they check was working perfectly.
mgrep() {
	local pattern="$1"; shift
	local out
	out="$(m "$@")"
	printf '%s\n' "${out}" | grep -q "${pattern}"
}

# Compare with awk so this works without bc.
fcmp() { awk -v a="$1" -v b="$2" 'BEGIN{exit !(a<b)}'; }

# --- preflight -------------------------------------------------------------

head_ "Preflight"

if ! ssh -o ConnectTimeout=10 "${PI}" true 2>/dev/null; then
	bad "Pi reachable at ${PI}" "ssh failed"
	echo; echo "Cannot continue without the Pi."; exit 1
fi
ok "Pi reachable" "${PI}"

SCAN="$(m scan)"
VENDOR="$(printf '%s\n' "${SCAN}" | sed -n 's/.*vendor=0x\([0-9a-f]*\).*/\1/p' | head -1)"
if [ -z "${VENDOR}" ]; then
	bad "slave enumerates" "no slave on ${IFACE}"
	echo; echo "Cannot continue without a slave."; exit 1
fi
ok "slave enumerates" "vendor 0x${VENDOR}"

# --- parameters, read from the slave rather than assumed --------------------

head_ "Axis parameters (read over CoE, used to derive every expectation)"

# One retry, because a mailbox read issued immediately after a state
# transition genuinely fails sometimes on this hardware — observed as a lone
# wkc 0 on an object that reads fine a second later. Retrying a known-transient
# condition is not the same as retrying until it passes: a single retry that is
# reported when it happens keeps a real intermittent fault visible instead of
# hiding it behind a loop.
sdo_read() {
	local out val
	out="$(m "sdo read $1 $2")"
	val="$(printf '%s\n' "${out}" | sed -n 's/^0x[0-9a-fA-F]*:[0-9a-fA-F]* = \([0-9-]*\) .*/\1/p' | head -1)"
	if [ -z "${val}" ]; then
		sleep 1
		out="$(m "sdo read $1 $2")"
		val="$(printf '%s\n' "${out}" | sed -n 's/^0x[0-9a-fA-F]*:[0-9a-fA-F]* = \([0-9-]*\) .*/\1/p' | head -1)"
		[ -n "${val}" ] && note "retried SDO read $1:$2" "first attempt failed"
	fi
	printf '%s' "${val}"
}

STEP_NS="$(sdo_read 0x8000 1)"
MAX_ANGLE="$(sdo_read 0x8000 2)"
FULL_STEPS="$(sdo_read 0x8000 3)"
MICROSTEP="$(sdo_read 0x8000 4)"

if [ -z "${STEP_NS}" ] || [ -z "${MAX_ANGLE}" ] || [ -z "${FULL_STEPS}" ] || [ -z "${MICROSTEP}" ]; then
	bad "read 0x8000 axis parameters" "one or more empty"
	echo; echo "Cannot derive expectations without these."; exit 1
fi

STEPS_PER_REV=$((FULL_STEPS * MICROSTEP))
# Steps in a full-range move, same rounding as step_math.h.
RANGE_STEPS=$(( (MAX_ANGLE * STEPS_PER_REV + 180) / 360 ))

ok "0x8000 readable" "interval ${STEP_NS} ns, max ${MAX_ANGLE} deg"
note "steps/rev" "${FULL_STEPS} x${MICROSTEP} = ${STEPS_PER_REV}"
note "full-range move" "${RANGE_STEPS} steps"

# ===========================================================================
if want_phase 1; then
head_ "Phase 1 — position feedback, not an echo"

MOVE="$(m "move 0 ${MAX_ANGLE}")"
INTER="$(printf '%s\n' "${MOVE}" | sed -n 's/.*intermediates=\([0-9]*\).*/\1/p')"
FIN_A="$(printf '%s\n' "${MOVE}" | sed -n 's/.*final_actual=\([0-9]*\).*/\1/p')"
FIN_E="$(printf '%s\n' "${MOVE}" | sed -n 's/.*final_echo=\([0-9]*\).*/\1/p')"
T1="$(printf '%s\n' "${MOVE}" | sed -n 's/.*elapsed_ms=\([0-9.]*\).*/\1/p')"

# An echo of the command jumps straight to the target: zero intermediates.
# Anything that passes through a meaningful fraction of the range is reading a
# real position counter.
MIN_INTER=$((RANGE_STEPS / 10))
[ -n "${INTER}" ] && [ "${INTER}" -ge "${MIN_INTER}" ]
check "move passes through intermediate positions" $? "${INTER:-none} seen, need >= ${MIN_INTER}"

[ -n "${FIN_A}" ] && [ "${FIN_A}" = "${FIN_E}" ] && [ "${FIN_A}" = "${MAX_ANGLE}" ]
check "settled: actual == echo == target" $? "actual=${FIN_A:-?} echo=${FIN_E:-?}"

SWEEP="$(ssh -o ConnectTimeout=10 "${PI}" "sudo timeout 25 ${MASTER#sudo } sweep" 2>&1)"
printf '%s\n' "${SWEEP}" | python3 -c '
import sys, re
spr = int(sys.argv[1]); maxang = int(sys.argv[2])
txt = sys.stdin.read()
vals = [int(v) for v in re.findall(r"actual=\s*(\d+)", txt)]
if not vals:
    print("NOVALS"); sys.exit(2)
valid = {(s * 360 + spr // 2) // spr for s in range(spr + 1)}
offenders = sorted({v for v in vals if v not in valid})
over = sorted({v for v in vals if v > maxang})
print(f"{len(vals)} {len(offenders)} {len(over)} {max(vals)}")
sys.exit(0 if not offenders and not over else 1)
' "${STEPS_PER_REV}" "${MAX_ANGLE}" > /tmp/_q.$$ 2>&1
QRC=$?
QOUT="$(cat /tmp/_q.$$)"; rm -f /tmp/_q.$$
set -- ${QOUT}
check "every actual lands on a step boundary" ${QRC} "${1:-0} samples, ${2:-?} off-grid, ${3:-?} over limit"
note "derived from" "steps/rev ${STEPS_PER_REV}, max ${MAX_ANGLE} deg"
fi

# ===========================================================================
if want_phase 2; then
head_ "Phase 2 — clean shutdown and the SyncManager watchdog"

for SIG in TERM INT; do
	ssh "${PI}" "sudo timeout -s ${SIG} 6 ${MASTER#sudo } set 90 >/dev/null 2>&1; sleep 1" >/dev/null 2>&1
	AL="$(m alstate | sed -n 's/.*AL Status 0x\([0-9a-f]*\).*/\1/p' | head -1)"
	[ "${AL}" = "0001" ]
	check "SIG${SIG} exit leaves the slave in INIT" $? "AL Status 0x${AL:-????}"
done

WD="$(ssh "${PI}" "sudo timeout 45 ${MASTER#sudo } wdtest" 2>&1)"
TRIP="$(printf '%s\n' "${WD}" | sed -n 's/.*tripped after \([0-9.]*\) ms.*/\1/p')"
PROG="$(printf '%s\n' "${WD}" | sed -n 's/.*programmed \([0-9]*\) ms.*/\1/p')"
CODE="$(printf '%s\n' "${WD}" | sed -n 's/.*AL Status Code 0x\([0-9a-f]*\).*/\1/p')"
ALW="$(printf '%s\n' "${WD}" | sed -n 's/.*AL Status 0x\([0-9a-f]*\),.*/\1/p')"

if [ -n "${TRIP}" ] && [ -n "${PROG}" ]; then
	LO=$(awk -v p="${PROG}" 'BEGIN{print p*0.5}')
	HI=$(awk -v p="${PROG}" 'BEGIN{print p*1.5}')
	fcmp "${LO}" "${TRIP}" && fcmp "${TRIP}" "${HI}"
	check "watchdog trips near its programmed time" $? "${TRIP} ms vs ${PROG} ms programmed"
else
	bad "watchdog trips near its programmed time" "could not parse wdtest output"
fi

[ "${CODE}" = "001b" ]
check "AL Status Code is 0x001b (SM watchdog)" $? "0x${CODE:-????}, AL 0x${ALW:-????}"

REC="$(ssh "${PI}" "sudo timeout 12 ${MASTER#sudo } set 45" 2>&1)"
printf '%s\n' "${REC}" | grep -q 'wkc=3/3'
check "recovers to OP without a power cycle" $? "$(printf '%s\n' "${REC}" | grep -c 'wkc=3/3') healthy samples"
fi

# ===========================================================================
if want_phase 3; then
head_ "Phase 3 — CoE object dictionary"

SDO_VENDOR="$(sdo_read 0x1018 1)"
DEC_VENDOR=$((16#${VENDOR}))
[ "${SDO_VENDOR}" = "${DEC_VENDOR}" ]
check "0x1018:01 over mailbox matches the scan" $? "SDO ${SDO_VENDOR:-?} vs scan ${DEC_VENDOR}"

# Phase 3 must stand alone under --phase 3, where phase 1 never ran and so
# never timed a baseline move.
if [ -z "${T1:-}" ]; then
	T1="$(m "move 0 ${MAX_ANGLE}" | sed -n 's/.*elapsed_ms=\([0-9.]*\).*/\1/p')"
	note "baseline move timed here" "${T1:-?} ms at ${STEP_NS} ns"
fi

DOUBLE=$((STEP_NS * 2))
mgrep OK "sdo write 0x8000 1 ${DOUBLE} 4"
check "RW write accepted" $? "0x8000:01 <- ${DOUBLE}"

RB="$(sdo_read 0x8000 1)"
[ "${RB}" = "${DOUBLE}" ]
check "RW write reads back" $? "${RB:-?}"

MOVE2="$(m "move 0 ${MAX_ANGLE}")"
T2="$(printf '%s\n' "${MOVE2}" | sed -n 's/.*elapsed_ms=\([0-9.]*\).*/\1/p')"

# Doubling the interval adds exactly one interval per step. Comparing the
# INCREASE rather than the ratio cancels the fixed per-move overhead (cycle
# granularity, OP entry), so this is a much tighter claim than "about twice".
if [ -n "${T1:-}" ] && [ -n "${T2}" ]; then
	EXPECT=$(awk -v s="${RANGE_STEPS}" -v ns="${STEP_NS}" 'BEGIN{print s*ns/1000000}')
	DELTA=$(awk -v a="${T2}" -v b="${T1}" 'BEGIN{print a-b}')
	ERR=$(awk -v d="${DELTA}" -v e="${EXPECT}" 'BEGIN{x=(d-e)/e; print (x<0?-x:x)*100}')
	fcmp "${ERR}" 25
	check "slower interval costs the predicted time" $? \
		"+${DELTA} ms vs ${EXPECT} ms predicted (${ERR}% off)"
else
	bad "slower interval costs the predicted time" "missing a move timing"
fi

mgrep REFUSED "sdo write 0x8000 1 0 4"
check "out-of-range write refused" $? "abort expected"

RB2="$(sdo_read 0x8000 1)"
[ "${RB2}" = "${DOUBLE}" ]
check "refused write left the value untouched" $? "${RB2:-?}"

mgrep REFUSED "sdo write 0x8000 3 999 2"
check "read-only entry refused" $? "0x8000:03"

mgrep OK "sdo write 0x8000 1 ${STEP_NS} 4"
check "original interval restored" $? "${STEP_NS} ns"

# Mailbox traffic while the bus is in OP. SDOs are blocking transactions and
# the loop has a 10 ms budget behind a 100 ms watchdog, so this is a real
# question. Sparse traffic is the realistic case: configuration is normally
# done in PRE-OP, and an occasional read during operation is the exception.
SDOOP="$(ssh "${PI}" "sudo timeout 40 ${MASTER#sudo } sdotest 10 20" 2>&1)"
SD_LOW="$(printf '%s\n' "${SDOOP}" | sed -n 's/.*low_wkc=\([0-9]*\).*/\1/p')"
SD_FAIL="$(printf '%s\n' "${SDOOP}" | sed -n 's/.*sdo_failed=\([0-9]*\).*/\1/p')"
SD_N="$(printf '%s\n' "${SDOOP}" | sed -n 's/.*sdos=\([0-9]*\).*/\1/p')"
SD_OP="$(printf '%s\n' "${SDOOP}" | grep -c 'still OP')"
SD_DC="$(printf '%s\n' "${SDOOP}" | sed -n 's/.*worst DC phase error after settling: \([0-9]*\) ns.*/\1/p')"
SD_CYC="$(printf '%s\n' "${SDOOP}" | sed -n 's/.*worst cycle \([0-9.]*\) ms.*/\1/p')"

[ "${SD_LOW:-1}" = "0" ] && [ "${SD_FAIL:-1}" = "0" ] && [ "${SD_OP}" = "1" ]
check "SDOs during OP keep the bus healthy" $? "${SD_N:-?} SDOs, low_wkc ${SD_LOW:-?}, failed ${SD_FAIL:-?}, still OP"

# The honest part: mailbox traffic does NOT leave timing alone. An SDO blocks
# the cyclic loop long enough to miss its deadline, and that shifts when the
# frame lands relative to SYNC0. Measured on this bench: ~182 us worst phase
# error with no SDOs, ~643 us with one every 20 cycles, and ~3.4 ms with one
# every cycle. Sparse traffic is asserted to stay inside a tenth of the cycle;
# doing it every cycle would fail this, correctly.
[ -n "${SD_DC}" ] && [ "${SD_DC}" -lt 1000000 ]
check "DC phase error stays under 1 ms with sparse SDOs" $? "${SD_DC:-?} ns, worst cycle ${SD_CYC:-?} ms vs 10 ms budget"
fi

# ===========================================================================
head_ "Regression — the bus stays healthy"

GATE="$(ssh "${PI}" "sudo timeout 35 ${MASTER#sudo } sweep" 2>&1)"
printf '%s\n' "${GATE}" | python3 -c '
import sys, re, statistics
txt = sys.stdin.read()
lines = [l for l in txt.splitlines() if re.search(r"\bt=\s*[\d.]+s", l)]
bad_wkc = [l for l in lines if not re.search(r"(?<!low_)wkc=3/3", l)]
bad_low = [l for l in lines if not re.search(r"low_wkc=0\b", l)]
rows = [(float(t), int(d)) for t, d in re.findall(r"t=\s*([\d.]+)s.*?dc=\s*([+-]\d+)ns", txt)]
settled = [abs(d) for t, d in rows if t > 2.0]
med = int(statistics.median(settled)) if settled else -1
print(f"{len(lines)} {len(bad_wkc)} {len(bad_low)} {med}")
sys.exit(0 if lines and not bad_wkc and not bad_low else 1)
' > /tmp/_g.$$ 2>&1
GRC=$?
GOUT="$(cat /tmp/_g.$$)"; rm -f /tmp/_g.$$
set -- ${GOUT}
check "wkc 3/3 and low_wkc 0 for the whole run" ${GRC} "${1:-0} samples, ${2:-?} bad wkc, ${3:-?} bad low_wkc"

MED="${4:-999999}"
[ "${MED}" -ge 0 ] && [ "${MED}" -lt 100000 ]
check "settled DC phase error under 100 us" $? "median ${MED} ns"

# ---------------------------------------------------------------------------

printf '\n\033[1m%d passed, %d failed\033[0m\n' "${PASS}" "${FAIL}"
[ "${FAIL}" -eq 0 ] || exit 1

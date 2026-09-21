#!/usr/bin/env bash
# Run with Bash 3.2+; kubectl, tar, uv, GNU timeout, and the selected Python are required.
set -euo pipefail
if [[ $# != 7 ]]; then
    printf 'Usage: bash %s WHEEL CONTEXT NAMESPACE PREFILL_POD DECODE_POD CONTAINER PYTHON\n' "$0" >&2
    exit 2
fi
wheel=$1; context=$2; namespace=$3; prefill=$4; decode=$5; container=$6; python=$7
probe_dir=$(cd -- "$(dirname -- "$0")" && pwd)
[[ -f "$wheel" && "$wheel" == *.whl && -f "$probe_dir/probe.py" ]] || {
    printf 'Need an existing .whl and a sibling probe.py.\n' >&2; exit 2;
}
[[ "$prefill" != "$decode" ]] || { printf 'Choose two different pods.\n' >&2; exit 2; }
wheel=$(cd -- "$(dirname -- "$wheel")" && printf '%s/%s' "$PWD" "$(basename -- "$wheel")")
wheel_name=$(basename -- "$wheel")
gpu=${GPU:-0}; nic=${NIC:-mlx5_2}; gid=${GID_INDEX:-5}
k=(kubectl --context "$context" --namespace "$namespace" --request-timeout=105s)
logs=$(mktemp -d "${TMPDIR:-/tmp}/mooncake-wheel-probe.XXXXXX")
target_pid=; source_pid=
cleanup() {
    local pid
    for pid in "$target_pid" "$source_pid"; do
        if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
            kill "$pid" 2>/dev/null || printf 'Local kubectl process %s already exited.\n' "$pid" >&2
        fi
    done
    printf 'Local logs retained: %s\n' "$logs"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
pod_ip() {
    local pod=$1 state phase ip ready container_ready
    state=$("${k[@]}" get pod "$pod" -o "jsonpath={.status.phase}|{.status.podIP}|{.status.conditions[?(@.type==\"Ready\")].status}|{.status.containerStatuses[?(@.name==\"$container\")].ready}") || return
    IFS='|' read -r phase ip ready container_ready <<< "$state"
    [[ "$phase" == Running && "$ready" == True && "$container_ready" == true && "$ip" =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]] || {
        printf 'Pod/container must be running and ready with an IPv4 address: %s (%s)\n' "$pod" "$state" >&2
        return 1
    }
    printf '%s' "$ip"
}
prefill_ip=$(pod_ip "$prefill")
decode_ip=$(pod_ip "$decode")
[[ "$prefill_ip" != "$decode_ip" ]] || { printf 'Pods must have different IPs.\n' >&2; exit 2; }
prepare() {
    local pod=$1 role=$2 remote
    remote=$("${k[@]}" exec "$pod" -c "$container" -- mktemp -d /tmp/mooncake-dd.XXXXXX) || return
    [[ "$remote" == /tmp/mooncake-dd.* && "$remote" != *$'\n'* ]] || {
        printf 'Unexpected temporary directory: %s\n' "$remote" >&2; return 1;
    }
    printf '%s temporary files retained in %s: %s\n' "$role" "$pod" "$remote" >&2
    printf '%s\n' "$remote" > "$logs/$role-directory.txt" || return
    "${k[@]}" cp "$wheel" "$pod:$remote/$wheel_name" -c "$container" >&2 || return
    "${k[@]}" cp "$probe_dir/probe.py" "$pod:$remote/probe.py" -c "$container" >&2 || return
    if ! "${k[@]}" exec "$pod" -c "$container" -- timeout -k 5s 90s uv pip install \
        --no-deps --no-index --python "$python" --target "$remote/wheel" "$remote/$wheel_name" \
        > "$logs/$role-install.log" 2>&1; then
        cat "$logs/$role-install.log" >&2
        return 1
    fi
    printf '%s' "$remote"
}
prefill_dir=$(prepare "$prefill" prefill)
decode_dir=$(prepare "$decode" decode)
printf 'Prefill %s (%s), decode %s (%s); GPU=%s NIC=%s GID_INDEX=%s\n' \
    "$prefill" "$prefill_ip" "$decode" "$decode_ip" "$gpu" "$nic" "$gid" | tee "$logs/config.txt"
launch_probe() {
    local pod=$1 remote=$2 role=$3 local_ip=$4 peer_ip=$5 dd=$6 port=$7
    # This function runs only in a background subshell, so its PID becomes kubectl's PID.
    exec "${k[@]}" exec "$pod" -c "$container" -- timeout -k 5s 90s \
        env -u LD_PRELOAD -u PYTHONOPTIMIZE PYTHONPATH="$remote/wheel" "$python" -u "$remote/probe.py" "$role" \
        --local-ip "$local_ip" --peer-ip "$peer_ip" --control-port "$port" \
        --gpu "$gpu" --nic "$nic" --gid-index "$gid" --wheel-root "$remote/wheel" --data-direct "$dd"
}
overall=0
for dd in 1 0; do
    target_log="$logs/dd$dd-target.log"; source_log="$logs/dd$dd-initiator.log"
    printf 'Starting DD%s; each remote process has a 90-second limit.\n' "$dd"
    (launch_probe "$decode" "$decode_dir" target "$decode_ip" "$prefill_ip" "$dd" 0) > "$target_log" 2>&1 &
    target_pid=$!
    port=
    for ((attempt = 0; attempt < 45; attempt++)); do
        port=$(sed -n 's/.*"event": *"ready".*"control_port": *\([0-9][0-9]*\).*/\1/p' "$target_log")
        [[ -z "$port" ]] || break
        kill -0 "$target_pid" 2>/dev/null || break
        sleep 1
    done
    source_rc=not_started
    if [[ "$port" =~ ^[0-9]+$ ]] && ((port > 0 && port <= 65535)); then
        (launch_probe "$prefill" "$prefill_dir" initiator "$prefill_ip" "$decode_ip" "$dd" "$port") > "$source_log" 2>&1 &
        source_pid=$!
        if wait "$source_pid"; then source_rc=0; else source_rc=$?; fi
        source_pid=
    else
        printf 'DD%s target did not report readiness within 45 seconds; initiator not started.\n' "$dd" | tee "$source_log"
    fi
    if wait "$target_pid"; then target_rc=0; else target_rc=$?; fi
    target_pid=
    result=FAIL
    if [[ "$source_rc" == 0 && "$target_rc" == 0 ]] && \
        grep -Eq '"event": *"result".*"role": *"target".*"write": *"PASS".*"read": *"PASS"' "$target_log" && \
        grep -Eq '"event": *"result".*"role": *"initiator".*"write": *"PASS".*"read": *"PASS"' "$source_log"; then
        result=PASS
    else
        overall=1
    fi
    printf 'DD%s %s: initiator_exit=%s target_exit=%s (PASS requires both payload checks)\n' "$dd" "$result" "$source_rc" "$target_rc" | tee -a "$logs/results.txt"
done
printf 'DD0 is the control; its failure remains a FAIL even when expected. Inspect both conditions in %s/results.txt.\n' "$logs"
exit "$overall"

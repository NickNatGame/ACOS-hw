#!/usr/bin/env bash
# Координатор и философы запускаются оболочкой как независимые процессы.
kind=${1:-session}
binary_prefix=philosophers_
log_prefix=philosophers-
if [ "$kind" = "mq" ]; then
  binary_prefix=philosophers_mq_
  log_prefix=philosophers-mq-
fi
if [ "$#" -gt 1 ] || { [ "$kind" != "session" ] && [ "$kind" != "mq" ]; } ||
   [ ! -x "./bin/${binary_prefix}coordinator" ] ||
   [ ! -x "./bin/${binary_prefix}participant" ]; then
  echo "Соберите проект и запустите bash tests/run_session_examples.sh [session|mq] из его корня." >&2
  exit 1
fi
root=$(pwd)
work_dir=$(mktemp -d) || exit 1
coordinator_pid=
participants=()
cleanup() {
  # Сначала даём координатору остановить сессию и удалить её имена.
  if [ -n "$coordinator_pid" ]; then
    kill -TERM "$coordinator_pid" 2>/dev/null
    wait "$coordinator_pid" 2>/dev/null
  fi
  for pid in "${participants[@]}"; do
    kill -TERM "$pid" 2>/dev/null
    wait "$pid" 2>/dev/null
  done
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

while read -r name expected parameters; do
  if [ -z "$name" ] || [ "$name" = "#" ]; then
    continue
  fi
  read -r -a arguments <<< "$parameters"
  session="example_$$_$name"
  (
    cd "$work_dir" || exit 1
    exec "$root/bin/${binary_prefix}coordinator" "$session" "${arguments[@]}"
  ) > "$work_dir/$name.out" 2> "$work_dir/$name.err" &
  coordinator_pid=$!
  ready=0
  for ((attempt=0; attempt<200; attempt++)); do
    if grep -q '^ГОТОВО:' "$work_dir/$name.out"; then
      ready=1
      break
    fi
    sleep 0.01
  done
  if [ "$ready" -ne 1 ]; then
    echo "FAIL: координатор не запустился, диагностика в $work_dir" >&2
    exit 1
  fi
  participants=()
  for ((id=1; id<=arguments[0]; id++)); do
    "$root/bin/${binary_prefix}participant" "$session" "$id" \
      > "$work_dir/$name-$id.out" 2> "$work_dir/$name-$id.err" &
    participants+=("$!")
  done
  wait "$coordinator_pid"
  result=$?
  coordinator_pid=
  failed=0
  for pid in "${participants[@]}"; do
    wait "$pid"
    worker_result=$?
    if [ "$worker_result" -ne 0 ] && [ "$worker_result" -ne "$expected" ]; then
      failed=1
    fi
  done
  participants=()
  if [ "$result" -ne "$expected" ] || [ "$failed" -ne 0 ]; then
    echo "FAIL: $name, код $result вместо $expected; диагностика в $work_dir" >&2
    exit 1
  fi
  cp "$work_dir/$log_prefix$session.log" "$root/tests/${kind}_$name.log" || exit 1
  echo "OK: $kind/$name, код $result, журнал tests/${kind}_$name.log"
done < tests/scenarios.txt

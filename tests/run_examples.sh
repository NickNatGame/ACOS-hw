#!/usr/bin/env bash
# Запуск наборов данных и обновление журналов для отчёта
kind=${1:-unnamed}
if [ "$#" -gt 1 ] || { [ "$kind" != "unnamed" ] && [ "$kind" != "named" ]; }; then
  echo "Использование: $0 [unnamed|named]" >&2
  exit 1
fi
if [ ! -x "./bin/philosophers_$kind" ]; then
  echo "Сначала соберите программу и запустите скрипт из корня проекта." >&2
  exit 1
fi

root=$(pwd)
work_dir=$(mktemp -d) || exit 1
prefix=
if [ "$kind" = "named" ]; then
  prefix=named_
fi
# При выходе удаляем временный каталог с рабочим логом программы
trap 'rm -rf "$work_dir"' EXIT

while read -r name expected parameters; do
  if [ -z "$name" ] || [ "$name" = "#" ]; then
    continue
  fi

  (
    cd "$work_dir" || exit 1
    # Параметры из набора разделяем на аргументы по пробелам
    "$root/bin/philosophers_$kind" $parameters
  ) > "$root/tests/$prefix$name.log"
  result=$?

  if [ "$result" -ne "$expected" ]; then
    echo "FAIL: $name, код $result вместо $expected" >&2
    exit 1
  fi

  echo "OK: $kind/$name, код $result, журнал tests/$prefix$name.log"
done < tests/scenarios.txt

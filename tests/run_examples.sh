#!/usr/bin/env bash
# Запуск наборов данных и обновление журналов для отчёта
if [ ! -x ./bin/philosophers ]; then
  echo "Сначала соберите программу и запустите скрипт из корня проекта." >&2
  exit 1
fi

root=$(pwd)
work_dir=$(mktemp -d) || exit 1
# При выходе удаляем временный каталог с рабочим логом программы
trap 'rm -rf "$work_dir"' EXIT

while read -r name expected parameters; do
  if [ -z "$name" ] || [ "$name" = "#" ]; then
    continue
  fi

  (
    cd "$work_dir" || exit 1
    # Параметры из набора разделяем на аргументы по пробелам
    "$root/bin/philosophers" $parameters
  ) > "$root/tests/$name.log"
  result=$?

  if [ "$result" -ne "$expected" ]; then
    echo "FAIL: $name, код $result вместо $expected" >&2
    exit 1
  fi

  echo "OK: $name, код $result, журнал tests/$name.log"
done < tests/scenarios.txt

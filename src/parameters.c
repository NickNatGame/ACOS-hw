#include "philosophers.h"
#include "soft_assert.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void print_help(const char *program) {
  dprintf(STDOUT_FILENO,
          "Использование: %s [N] [cycles] [think_min] [think_max] "
          "[eat_min] [eat_max] [max_wait] [display_delay] [seed] "
          "[strategy] [time_limit]\n"
          "N — одновременно количество философов и вилок (от 2).\n"
          "cycles: от 1. Времена: от 0 ms; "
          "eat_min и max_wait должны быть положительными.\n"
          "think_max >= think_min, eat_max >= eat_min.\n"
          "seed: 0..%d. Единственная стратегия: fifo.\n"
          "Все числа должны помещаться в int (не больше %d).\n"
          "Размер модели ограничен доступной памятью; "
          "сочетание параметров не должно переполнять время и статистику.\n"
          "time_limit - общий лимит времени, 0 отключает его.\n"
          "display_delay - реальная пауза между событиями, 0 - без пауз.\n"
          "По умолчанию: 5 3 500 1500 400 1000 5000 120 42 fifo 60000.\n"
          "Пример: %s 5 3 500 1500 400 1000 5000 0 42 fifo 60000\n",
          program, INT_MAX, INT_MAX, program);
}

static int read_number(const char *text, int *number) {
  int fl_err = 0;
  char *end;
  errno = 0; /* Сбрасываем код ошибки перед вызовом strtol */
  long value = strtol(text, &end, 10);

  /* Проверяем диапазон до приведения long к int */
  SOFT_ASSERT_ERR(errno == 0);
  SOFT_ASSERT_ERR(end != text);
  SOFT_ASSERT_ERR(*end == '\0');
  SOFT_ASSERT_ERR(value >= 0);
  SOFT_ASSERT_ERR(value <= INT_MAX);
  if (fl_err != 0) {
    dprintf(STDERR_FILENO, "Ошибка: '%s' — недопустимое число.\n", text);
    return -1;
  }
  *number = (int)value;
  return 0;
}

int read_parameters(int argc, char **argv, Parameters *parameters) {
  int fl_err = 0;
  int values[9] = {5, 3, 500, 1500, 400, 1000, 5000, 120, 42};
  int time_limit = 60000;

  if (argc == 2 &&
      (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
    print_help(argv[0]);
    return 0;
  }
  SOFT_ASSERT_ERR(argc <= 12);
  if (fl_err != 0) {
    dprintf(STDERR_FILENO, "Ошибка: слишком много аргументов.\n");
    return -1;
  }

  for (int i = 1; i < argc && i <= 9; i++) {
    if (read_number(argv[i], &values[i - 1]) == -1) {
      return -1;
    }
  }
  if (argc >= 11) {
    SOFT_ASSERT_ERR(strcmp(argv[10], "fifo") == 0);
    if (fl_err != 0) {
      dprintf(STDERR_FILENO, "Ошибка: поддерживается только стратегия fifo.\n");
      return -1;
    }
  }
  if (argc == 12 && read_number(argv[11], &time_limit) == -1) {
    return -1;
  }

  parameters->count = values[0];
  parameters->cycles = values[1];
  parameters->think_min = values[2];
  parameters->think_max = values[3];
  parameters->eat_min = values[4];
  parameters->eat_max = values[5];
  parameters->max_wait = values[6];
  parameters->display_delay = values[7];
  parameters->seed = values[8];
  parameters->time_limit = time_limit;

  SOFT_ASSERT_ERR(parameters->count >= 2);
  SOFT_ASSERT_ERR(parameters->cycles >= 1);
  SOFT_ASSERT_ERR(parameters->think_max >= parameters->think_min);
  SOFT_ASSERT_ERR(parameters->eat_min >= 1);
  SOFT_ASSERT_ERR(parameters->eat_max >= parameters->eat_min);
  SOFT_ASSERT_ERR(parameters->max_wait >= 1);
  if (fl_err != 0) {
    dprintf(STDERR_FILENO, "Ошибка: некорректные параметры. См. --help.\n");
    return -1;
  }

  /* Верхние оценки считаем через деление, чтобы сама проверка не переполнилась
   */
  long long meals = (long long)parameters->count * parameters->cycles;
  long long cycle_time = (long long)parameters->think_max + parameters->eat_max;
  long long time_budget = LLONG_MAX - parameters->max_wait - 1;
  SOFT_ASSERT_ERR(meals <= time_budget / cycle_time);
  SOFT_ASSERT_ERR(meals <= LLONG_MAX / parameters->max_wait);
  if (fl_err != 0) {
    dprintf(STDERR_FILENO, "Ошибка: сочетание параметров может переполнить "
                           "модельное время или статистику.\n");
    return -1;
  }
  return 1;
}

#include "console.h"
#include "soft_assert.h"

#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int write_error = 0;

/* Флаг безопасно менять из обработчика сигнала */
_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "signal flag must be lock-free");
static atomic_int stop_signal = 0;

/* Обработчик только запоминает сигнал, итоги печатает основная программа */
static void handle_signal(int number)
{
  stop_signal = number;
}

int open_console(void)
{
  int fl_err = 0;
  struct sigaction action = {0};
  action.sa_handler = handle_signal;

  SOFT_ASSERT_ERR(sigemptyset(&action.sa_mask) == 0);
  SOFT_ASSERT_ERR(sigaction(SIGINT, &action, NULL) == 0);
  SOFT_ASSERT_ERR(sigaction(SIGTERM, &action, NULL) == 0);
  if (fl_err != 0)
  {
    dprintf(STDERR_FILENO, "Ошибка sigaction: %s\n", strerror(errno));
    return -1;
  }

  /* Закрытый канал вывода должен дать ошибку записи, а не оборвать программу */
  action.sa_handler = SIG_IGN;
  SOFT_ASSERT_ERR(sigaction(SIGPIPE, &action, NULL) == 0);
  if (fl_err != 0)
  {
    dprintf(STDERR_FILENO, "Ошибка sigaction: %s\n", strerror(errno));
    return -1;
  }
  return 0;
}

void print_log(const char *format, ...)
{
  int fl_err = 0;
  va_list arguments;
  va_start(arguments, format);
  int screen_result = vdprintf(STDOUT_FILENO, format, arguments);
  va_end(arguments);

  if (!write_error)
  {
    SOFT_ASSERT_ERR(screen_result >= 0);
    if (fl_err != 0)
    {
      dprintf(STDERR_FILENO, "Ошибка записи в терминал.\n");
      write_error = 1;
    }
  }
}

int received_signal(void)
{
  return stop_signal;
}
int output_failed(void)
{
  return write_error;
}

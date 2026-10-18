#include "console.h"
#include "soft_assert.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int log_fd = -1;       /* тут будет файл для логирования */
static int write_error = 0;
static volatile sig_atomic_t stop_signal = 0;     /* тут будет указано, если программа завершилась неожиданно,  */ 
                                                  /* к примеру SIGINT или SIGTERM                               */         

/* Обработчик только запоминает сигнал. Итоги печатает основная программа */
static void handle_signal(int number) { stop_signal = number; }

int open_console(void) {
  int fl_err = 0;
  struct sigaction action = {0};
  action.sa_handler = handle_signal;

  /* настраиваем наш хендлер на выполнение программы, про прерывании человеком */
  SOFT_ASSERT_ERR(sigemptyset(&action.sa_mask) == 0);
  SOFT_ASSERT_ERR(sigaction(SIGINT, &action, NULL) == 0);
  SOFT_ASSERT_ERR(sigaction(SIGTERM, &action, NULL) == 0);
  if (fl_err != 0) {
    dprintf(STDERR_FILENO, "Ошибка sigaction: %s\n", strerror(errno));
    return -1;
  }

  /* Закрытый канал вывода должен дать ошибку записи, а не оборвать, те просто настраиваем на игнор */
  action.sa_handler = SIG_IGN;
  SOFT_ASSERT_ERR(sigaction(SIGPIPE, &action, NULL) == 0);
  if (fl_err != 0) {
    dprintf(STDERR_FILENO, "Ошибка sigaction: %s\n", strerror(errno));
    return -1;
  }

  log_fd = open("philosophers.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
  SOFT_ASSERT_ERR(log_fd >= 0);
  if (fl_err != 0) {
    dprintf(STDERR_FILENO, "Ошибка открытия лога: %s\n", strerror(errno));
    return -1;
  }
  return 0;
}

void print_log(const char *format, ...) {
  int fl_err = 0;
  va_list arguments;  /* <- для доступа к остальным параметрам кроме format */

  va_start(arguments, format);
  int screen_result = vdprintf(STDOUT_FILENO, format, arguments);
  va_end(arguments);

  /* Для второго вывода начинаем обход аргументов заново */
  va_start(arguments, format);
  int log_result = vdprintf(log_fd, format, arguments);
  va_end(arguments);

  if (!write_error) {
    SOFT_ASSERT_ERR(screen_result >= 0);
    SOFT_ASSERT_ERR(log_result >= 0);
    if (fl_err != 0) {
      dprintf(STDERR_FILENO, "Ошибка записи в терминал или лог.\n");
      write_error = 1;
    }
  }
}

void pause_display(int milliseconds) {
  int fl_err = 0;
  struct timespec delay;
  delay.tv_sec = milliseconds / 1000;
  delay.tv_nsec = (milliseconds % 1000) * 1000000L;

  while (!stop_signal && nanosleep(&delay, &delay) == -1) {
    SOFT_ASSERT_ERR(errno == EINTR);
    if (fl_err != 0) {
      break;
    }
  }
}

int received_signal(void) { return stop_signal; }

int output_failed(void) { return write_error; }

int close_console(void) {
  int fl_err = 0;
  SOFT_ASSERT_ERR(close(log_fd) == 0);
  if (fl_err != 0) {
    dprintf(STDERR_FILENO, "Ошибка закрытия лога: %s\n", strerror(errno));
    return -1;
  }
  return 0;
}

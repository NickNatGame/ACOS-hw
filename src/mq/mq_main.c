#include "console.h"
#include "mq_philosophers.h"
#include "session.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
  if (argc == 2 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h")))
  {
#ifdef MQ_COORDINATOR_MAIN
    dprintf(STDOUT_FILENO, "Использование: %s SESSION [N cycles think_min think_max eat_min eat_max "
            "max_wait display_delay seed fifo time_limit]\n"
            "После ГОТОВО запустите philosophers_mq_participant SESSION ID для каждого ID=1..N.\n"
            "На подключение всех философов даётся 10 секунд.\n", argv[0]);
#else
    dprintf(STDOUT_FILENO, "Использование: %s SESSION ID\n"
            "Сначала запустите MQ-координатора; ID от 1 до N.\n", argv[0]);
#endif
    return 0;
  }

  if (argc < 2 || !valid_session_name(argv[1]))
  {
    dprintf(STDERR_FILENO, "Ошибка: SESSION — от 1 до 32 символов A-Z, a-z, 0-9, _ или -.\n");
    return 1;
  }

#ifdef MQ_COORDINATOR_MAIN
  if (argc > 13)
  {
    dprintf(STDERR_FILENO, "Ошибка: слишком много аргументов.\n");
    return 1;
  }
  Parameters parameters;
  char *arguments[13] = {argv[0]};
  for (int i = 2; i < argc; i++)
  {
    arguments[i - 1] = argv[i];
  }
  int parsed = read_parameters(argc - 1, arguments, &parameters);
  if (parsed != 1)
  {
    return parsed == 0 ? 0 : 1;
  }
#else
  if (argc != 3)
  {
    dprintf(STDERR_FILENO, "Ошибка: нужны SESSION и ID философа.\n");
    return 1;
  }
  char *end;
  errno = 0;
  long id = strtol(argv[2], &end, 10);
  if (argv[2][0] < '0' || argv[2][0] > '9' || *end || errno ||
      id < 1 || id > MAX_PHILOSOPHERS)
  {
    dprintf(STDERR_FILENO, "Ошибка: ID — целое число от 1 до %d.\n", MAX_PHILOSOPHERS);
    return 1;
  }
#endif
  if (open_console_mode(1) == -1)
  {
    return 1;
  }
#ifdef MQ_COORDINATOR_MAIN
  int result = run_mq_coordinator(argv[1], &parameters);
#else
  int result = run_mq_participant(argv[1], (int)id - 1);
#endif
  return close_console() == -1 ? 1 : result;
}

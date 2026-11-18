#include "tcp_philosophers.h"
#include "console.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
  if (argc == 2 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h")))
  {
#ifdef TCP_SERVER_MAIN
    dprintf(1, "Использование: %s IPv4 PORT [N cycles think_min think_max eat_min eat_max "
               "max_wait display_delay seed fifo time_limit]\n"
               "На подключение всех N философов даётся 10 секунд.\n"
               "display_delay сохранён для совместимости; общих снимков в TCP-версии нет.\n",
            argv[0]);
#else
    dprintf(1, "Использование: %s SERVER_IPv4 PORT ID\nID: 1..N.\n", argv[0]);
#endif
    return 0;
  }

  struct sockaddr_in address;
  if (argc < 3 || tcp_address(argv[1], argv[2], &address) < 0)
  {
    dprintf(2, "Ошибка: нужны IPv4 и порт 1..65535. См. --help.\n");
    return 1;
  }

#ifdef TCP_SERVER_MAIN
  if (argc > 14)
  {
    dprintf(2, "Ошибка: слишком много аргументов.\n");
    return 1;
  }

  char *arguments[13] = {argv[0]};
  for (int i = 3; i < argc; i++)
    arguments[i - 2] = argv[i];

  Parameters parameters;
  if (read_parameters(argc - 2, arguments, &parameters) != 1)
    return 1;

#else
  if (argc != 4)
  {
    dprintf(2, "Ошибка: нужны SERVER_IPv4 PORT ID.\n");
    return 1;
  }

  errno = 0;
  char *end;
  long id = strtol(argv[3], &end, 10);
  if (*argv[3] < '0' || *argv[3] > '9' || *end || errno || id < 1 || id > MAX_PHILOSOPHERS)
  {
    dprintf(2, "Ошибка: ID должен быть от 1 до %d.\n", MAX_PHILOSOPHERS);
    return 1;
  }
#endif

  if (open_console() < 0)
    return 1;
#ifdef TCP_SERVER_MAIN
  int result = run_tcp_server(&address, &parameters);
#else
  int result = run_tcp_client(&address, (int)id);
#endif

  if (output_failed())
  {
    return 1;
  }

  return result;
}

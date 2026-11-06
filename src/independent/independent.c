#include "console.h"
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
#ifdef COORDINATOR_MAIN
    dprintf(STDOUT_FILENO, "Использование: %s SESSION [параметры симуляции]\n"
            "После сообщения ГОТОВО запустите философов с номерами 1..N.\n"
            "На подключение всех участников даётся 10 секунд.\n", argv[0]);
#else
    dprintf(STDOUT_FILENO, "Использование: %s SESSION ID\n"
            "ID это номер философа от 1 до N. Сначала запустите координатора.\n", argv[0]);
#endif
    return 0;
  }
  if (argc < 2 || !valid_session_name(argv[1]))
  {
    dprintf(STDERR_FILENO, "Ошибка: SESSION - от 1 до 32 символов A-Z, a-z, 0-9, _ или -.\n");
    return 1;
  }
#ifdef COORDINATOR_MAIN
  Parameters parameters;
  
  /* Парсер получает обычные аргументы, без имени сессии */
  char *arguments[13] = {argv[0]};
  if (argc > 13)
  {
    dprintf(STDERR_FILENO, "Ошибка: слишком много аргументов.\n");
    return 1;
  }
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
#ifdef COORDINATOR_MAIN
  int result = run_coordinator(argv[1], &parameters);
#else
  int result = run_participant(argv[1], (int)id - 1);
#endif
  return close_console() == -1 ? 1 : result;
}

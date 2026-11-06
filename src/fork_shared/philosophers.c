#include "philosophers.h"
#include "console.h"

int main(int argc, char **argv) {
  Parameters parameters;
  int result = read_parameters(argc, argv, &parameters);

  if (result == 0) {
    return 0; /* Пользователь ввел help */
  }
  if (result == -1) {
    return 1; /* Ошибка входных данных */
  }
  if (open_console() == -1) {
    return 1;
  }

  result = run_simulation(&parameters);
  if (close_console() == -1) {
    return 1;
  }
  return result;
}

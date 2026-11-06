#include "session.h"

#include <string.h>

int valid_session_name(const char *name)
{
  size_t length = strlen(name);
  if (length == 0 || length > 32)
  {
    return 0;
  }
  for (size_t i = 0; i < length; i++)
  {
    char ch = name[i];
    if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
          (ch >= '0' && ch <= '9') || ch == '_' || ch == '-'))
    {
      return 0;
    }
  }
  return 1;
}

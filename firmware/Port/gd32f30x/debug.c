#include "debug.h"

#include <stdarg.h>
#include "SEGGER_RTT.h"

void Dbg_Init(void)
{
  SEGGER_RTT_Init();
}

void Dbg_Printf(const char *fmt, ...)
{
  va_list args;
  va_start(args, fmt);
  (void)SEGGER_RTT_vprintf(0, fmt, &args);
  va_end(args);
}

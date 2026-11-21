#ifndef DEBUG_H
#define DEBUG_H

#ifdef __cplusplus
extern "C" {
#endif

void Dbg_Init(void);
void Dbg_Printf(const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif /* DEBUG_H */

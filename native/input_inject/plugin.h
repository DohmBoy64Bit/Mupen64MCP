#ifndef INPUT_INJECT_PLUGIN_H
#define INPUT_INJECT_PLUGIN_H

#ifdef __cplusplus
extern "C" {
#endif

#define M64P_PLUGIN_PROTOTYPES 1
#include "m64p_plugin.h"
#include "m64p_config.h"

#define PLUGIN_NAME "Mupen64Plus Input Inject"
#define PLUGIN_VERSION 0x010000

extern void DebugMessage(int level, const char *msg, ...);

/* Called by daemon to inject controller state */
EXPORT void CALL SetControllerState(int channel, unsigned int buttons, signed char x, signed char y, int sticky);

#ifdef __cplusplus
}
#endif

#endif /* INPUT_INJECT_PLUGIN_H */

#include "plugin.h"
#include <string.h>

static int l_PluginInit = 0;
static void (*l_DebugCallback)(void *, int, const char *) = NULL;
static void *l_DebugCallContext = NULL;

/* 4 channels of injected state */
static BUTTONS sInjectedState[4];
static int sSticky[4];       /* 1 = persistent across GetKeys calls */
static int sDirty[4];        /* 1 = injected data not yet consumed */

void DebugMessage(int level, const char *msg, ...)
{
    if (l_DebugCallback)
        (*l_DebugCallback)(l_DebugCallContext, level, msg);
}

EXPORT void CALL SetControllerState(int channel, unsigned int buttons, signed char x, signed char y, int sticky)
{
    if (channel < 0 || channel > 3)
        return;
    sInjectedState[channel].Value = buttons;
    sInjectedState[channel].X_AXIS = x;
    sInjectedState[channel].Y_AXIS = y;
    sSticky[channel] = sticky ? 1 : 0;
    sDirty[channel] = 1;
}

EXPORT m64p_error CALL PluginStartup(m64p_dynlib_handle CoreLibHandle, void *Context,
                                     void (*DebugCallback)(void *, int, const char *))
{
    (void)CoreLibHandle;
    if (l_PluginInit)
        return M64ERR_ALREADY_INIT;

    l_DebugCallback = DebugCallback;
    l_DebugCallContext = Context;

    memset(sInjectedState, 0, sizeof(sInjectedState));
    memset(sSticky, 0, sizeof(sSticky));
    memset(sDirty, 0, sizeof(sDirty));

    DebugMessage(M64MSG_INFO, "%s v%i.%i.%i started.", PLUGIN_NAME,
                 (PLUGIN_VERSION >> 16) & 0xFF, (PLUGIN_VERSION >> 8) & 0xFF, PLUGIN_VERSION & 0xFF);

    l_PluginInit = 1;
    return M64ERR_SUCCESS;
}

EXPORT m64p_error CALL PluginShutdown(void)
{
    l_PluginInit = 0;
    memset(sInjectedState, 0, sizeof(sInjectedState));
    memset(sSticky, 0, sizeof(sSticky));
    memset(sDirty, 0, sizeof(sDirty));
    DebugMessage(M64MSG_INFO, "%s shutdown.", PLUGIN_NAME);
    return M64ERR_SUCCESS;
}

EXPORT m64p_error CALL PluginGetVersion(m64p_plugin_type *PluginType, int *PluginVersion,
                                        int *APIVersion, const char **PluginNamePtr, int *Capabilities)
{
    if (PluginType)
        *PluginType = M64PLUGIN_INPUT;
    if (PluginVersion)
        *PluginVersion = PLUGIN_VERSION;
    if (APIVersion)
        *APIVersion = 0x020101;
    if (PluginNamePtr)
        *PluginNamePtr = PLUGIN_NAME;
    if (Capabilities)
        *Capabilities = 0;
    return M64ERR_SUCCESS;
}

EXPORT void CALL GetKeys(int Control, BUTTONS *Keys)
{
    if (Control < 0 || Control > 3)
    {
        memset(Keys, 0, sizeof(BUTTONS));
        return;
    }

    if (sDirty[Control])
    {
        *Keys = sInjectedState[Control];
        if (!sSticky[Control])
        {
            sInjectedState[Control].Value = 0;
            sDirty[Control] = 0;
        }
    }
    else
    {
        memset(Keys, 0, sizeof(BUTTONS));
    }
}

EXPORT void CALL InitiateControllers(CONTROL_INFO ControlInfo)
{
    int i;
    for (i = 0; i < 4; i++)
    {
        ControlInfo.Controls[i].Present = 1;
        ControlInfo.Controls[i].RawData = 0;
        ControlInfo.Controls[i].Plugin = PLUGIN_MEMPAK;
        ControlInfo.Controls[i].Type = CONT_TYPE_STANDARD;
    }
    DebugMessage(M64MSG_INFO, "%s: 4 controllers initialized.", PLUGIN_NAME);
}

EXPORT void CALL ControllerCommand(int Control, unsigned char *Command)
{
    (void)Control;
    (void)Command;
}

EXPORT void CALL ReadController(int Control, unsigned char *Command)
{
    (void)Control;
    (void)Command;
}

EXPORT int CALL RomOpen(void)
{
    memset(sInjectedState, 0, sizeof(sInjectedState));
    memset(sDirty, 0, sizeof(sDirty));
    return 1;
}

EXPORT void CALL RomClosed(void)
{
    memset(sInjectedState, 0, sizeof(sInjectedState));
    memset(sDirty, 0, sizeof(sDirty));
}

EXPORT void CALL SDL_KeyDown(int keymod, int keysym)
{
    (void)keymod;
    (void)keysym;
}

EXPORT void CALL SDL_KeyUp(int keymod, int keysym)
{
    (void)keymod;
    (void)keysym;
}

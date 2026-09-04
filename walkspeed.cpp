// walkspeed - walk speed modification for TesmioLoader.
//
// 1. Speed scaling:
//    - With multiplier == 1.0f, citizens' speed is not changed and remains 100% vanilla (0.9 - 1.1 random at birth).
//    - When the multiplier changes, the ratio from the old value to the new one is correctly recalculated without breaking vanilla limits.
// 2. Travel timer (travel timeout):
//    - Default 1.0x (disabled), so it does not affect the internal state-timer of citizens.

#include "../../src/tesmio_plugin.h"

// ---------------------------------------------------------------- constants and addresses

// std::vector<Person*> in SOVIET64.exe memory (v1.1.1.9)
#define RVA_PERSON_VECTOR 0x9E75B8

// Pointer to world object (to detect new game loads)
#define RVA_WORLD_PTR     0x9941F0

// Speed multiplier offset inside Person (float, vanilla range 0.9 - 1.1)
#define OFF_SPEED         0xA4

// State timer for travel/waiting offset inside Person (float)
#define OFF_STATE_TIMER   0x70

// Terrain render export for per-frame tick
#define SYM_TERRAIN_RENDER "?Render@C3D_TERRAIN@@QEAAX_NPEAVC3D_CAMERA@@0HH@Z"

// ---------------------------------------------------------------- settings

static int   g_enabled            = 1;
static float g_mult               = 1.0f;
static float g_travelTimeoutMult  = 1.0f;

// ---------------------------------------------------------------- state

typedef void (*t_TerrainRender)(void*, bool, void*, void*, int, int);
static t_TerrainRender o_TerrainRender = NULL;

static void*  g_lastWorld = NULL;
static size_t g_processedCount = 0;
static float  g_appliedMult = 1.0f;
static float  g_appliedTimeoutMult = 1.0f;
static int    g_timerTickCounter = 0;

// ---------------------------------------------------------------- fast logic

static void* GetWorldPtr(void)
{
    void** slot = (void**)(g_exeBase + RVA_WORLD_PTR);
    if (!ReadablePtr(slot, sizeof(void*))) return NULL;
    return *slot;
}

// Highly optimized citizen processing
static void FastProcessCitizens(void)
{
    void* currentWorld = GetWorldPtr();
    if (!currentWorld)
    {
        g_lastWorld = NULL;
        g_processedCount = 0;
        return;
    }

    bool settingsChanged = (g_appliedMult != g_mult) || (g_appliedTimeoutMult != g_travelTimeoutMult);
    bool isNewWorld = (currentWorld != g_lastWorld) || settingsChanged;
    float oldMult = g_appliedMult;

    if (isNewWorld)
    {
        g_lastWorld = currentWorld;
        g_processedCount = 0;
        g_appliedMult = g_mult;
        g_appliedTimeoutMult = g_travelTimeoutMult;
        g_timerTickCounter = 0;
    }

    BYTE*** v = (BYTE***)(g_exeBase + RVA_PERSON_VECTOR);
    if (!ReadablePtr(v, 16)) return;

    BYTE** begin = (BYTE**)v[0];
    BYTE** end   = (BYTE**)v[1];
    if (!begin || !end || end < begin) return;

    size_t count = (size_t)(end - begin);
    if (count == 0 || count > 1000000) return;

    // If it is a new world or settings changed, process from 0, otherwise only newly spawned from g_processedCount
    size_t startIndex = isNewWorld ? 0 : g_processedCount;
    if (startIndex > count) startIndex = 0;

    // Timer dampening only if explicitly enabled (> 1.01f)
    bool shouldDampTimers = false;
    if (g_travelTimeoutMult > 1.01f)
    {
        g_timerTickCounter++;
        if (g_timerTickCounter >= 60)
        {
            g_timerTickCounter = 0;
            shouldDampTimers = true;
        }
    }

    // If there is nothing to do and speed is vanilla (1.0f), return
    bool needSpeedUpdate = (isNewWorld && (oldMult != g_mult || g_mult != 1.0f)) ||
                           (!isNewWorld && g_mult != 1.0f && startIndex < count);

    if (!needSpeedUpdate && !shouldDampTimers)
    {
        g_processedCount = count;
        return;
    }

    __try
    {
        // 1. Citizen speed processing
        if (needSpeedUpdate && startIndex < count)
        {
            for (size_t i = startIndex; i < count; i++)
            {
                BYTE* person = begin[i];
                if (!person) continue;

                float* pSpeed = (float*)(person + OFF_SPEED);
                float cur = *pSpeed;

                if (cur >= 0.1f && cur <= 50.0f)
                {
                    float base = cur;
                    if (isNewWorld && oldMult > 0.01f && oldMult != 1.0f)
                    {
                        base = cur / oldMult;
                    }
                    else if (isNewWorld && oldMult == 1.0f && g_mult != 1.0f)
                    {
                        // If loading a save and the value is already significantly higher than vanilla (0.9-1.1), it might have been modified earlier
                        if (cur > 1.3f || cur < 0.7f)
                        {
                            base = cur / g_mult;
                        }
                    }

                    // Safety bounds for vanilla base (vanilla is ~0.9 to 1.1)
                    if (base < 0.5f || base > 1.5f) base = 1.0f;

                    if (g_mult == 1.0f)
                    {
                        *pSpeed = base;
                    }
                    else
                    {
                        *pSpeed = base * g_mult;
                    }
                }
            }
            g_processedCount = count;
        }
        else if (startIndex < count)
        {
            g_processedCount = count;
        }

        // 2. Periodic travel timer dampening (only 1x per 60 frames, if enabled)
        if (shouldDampTimers)
        {
            float damp = (g_travelTimeoutMult - 1.0f) / g_travelTimeoutMult;
            float scale = 1.0f - (damp * 0.5f);
            if (scale < 0.3f) scale = 0.3f;

            for (size_t i = 0; i < count; i++)
            {
                BYTE* person = begin[i];
                if (!person) continue;

                float* pTimer = (float*)(person + OFF_STATE_TIMER);
                float t = *pTimer;
                if (t > 2.0f && t < 10000.0f)
                {
                    *pTimer = t * scale;
                }
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        // Safely catch memory errors
    }
}

static void HookTerrainRender(void* self, bool a, void* b, void* c, int d, int e)
{
    if (o_TerrainRender)
    {
        o_TerrainRender(self, a, b, c, d, e);
    }

    if (g_enabled)
    {
        FastProcessCitizens();
    }
}

// ---------------------------------------------------------------- configuration and exports

static void ReadSettings(void)
{
    const char* ini = "plugins\\walkspeed.ini";
    char buf[64];

    g_enabled = H->configInt(ini, "walkspeed", "enabled", g_enabled);

    if (H->configString(ini, "walkspeed", "multiplier", buf, sizeof(buf), "") && buf[0])
    {
        float val = (float)atof(buf);
        if (val >= 0.1f && val <= 20.0f)
        {
            g_mult = val;
        }
        else
        {
            Logf("walkspeed  multiplier %.2f out of bounds, clamped", val);
            if (val < 0.1f) g_mult = 0.1f;
            if (val > 20.0f) g_mult = 20.0f;
        }
    }

    if (H->configString(ini, "walkspeed", "travel_timeout_multiplier", buf, sizeof(buf), "") && buf[0])
    {
        float val = (float)atof(buf);
        if (val >= 0.5f && val <= 50.0f)
        {
            g_travelTimeoutMult = val;
        }
        else
        {
            Logf("walkspeed  travel_timeout_multiplier %.2f out of bounds, clamped", val);
            if (val < 0.5f) g_travelTimeoutMult = 0.5f;
            if (val > 50.0f) g_travelTimeoutMult = 50.0f;
        }
    }
}

extern "C" __declspec(dllexport) unsigned TsmPluginApiVersion(void)
{
    return TSM_API_VERSION;
}

extern "C" __declspec(dllexport) int TsmPluginInit(const TsmHost* host, TsmPluginInfo* info)
{
    TsmBind(host);
    info->name    = "walkspeed";
    info->version = "2.3";

    ReadSettings();

    if (!g_enabled)
    {
        Logf("walkspeed  disabled");
        return 1;
    }

    Logf("walkspeed  init (speed mult: %.2f, travel timeout mult: %.2f)",
         g_mult, g_travelTimeoutMult);
    return 0;
}

extern "C" __declspec(dllexport) int TsmPluginStart(void)
{
    if (!g_enabled) return 1;

    if (!PatchIat(g_exe, DLL_ENGINE, SYM_TERRAIN_RENDER,
                  (void*)HookTerrainRender, (void**)&o_TerrainRender, "terrain render"))
    {
        Logf("walkspeed  failed to hook terrain render");
        return 1;
    }

    Logf("walkspeed  active (speed: %.2fx, travel timeout: %.2fx)",
         g_mult, g_travelTimeoutMult);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }

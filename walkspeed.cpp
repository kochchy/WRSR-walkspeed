// walkspeed - walk speed and travel timeout modification for TesmioLoader.
//
// 1. Speed scaling:
//    - With multiplier == 1.0f, citizens' speed is not changed and remains 100% vanilla (0.9 - 1.1 random at birth).
//    - When the multiplier changes, the ratio from the old value to the new one is correctly recalculated without breaking vanilla limits.
// 2. Travel & Waiting timeout:
//    - In vanilla WRSR (SOVIET64.exe v1.1.1.9):
//        * Station/bus stop waiting timeout is 600.0s (~1 hour), loaded into XMM7 at RVA 0x832F1B.
//        * Vehicle (bus/train/car) travel timeout is 380.0s (~4-5 hours), checked at RVA 0x8341A1.
//        * Accumulated waiting/travel time is kept in Person+0x6C.
//        * (Person+0x70 is the work/shift duration timer - touching it cuts shifts short!)
//    - We patch the RIP-relative float loads in FUN_140832e90 to point to dynamically scaled limits,
//      cleanly extending max waiting and travel times without affecting citizen work shifts or CPU performance.

#include "../../src/tesmio_plugin.h"
#include <stdint.h>

// ---------------------------------------------------------------- constants and addresses

// std::vector<Person*> in SOVIET64.exe memory (v1.1.1.9)
#define RVA_PERSON_VECTOR 0x9E75B8

// Pointer to world object (to detect new game loads)
#define RVA_WORLD_PTR     0x9941F0

// Speed multiplier offset inside Person (float, vanilla range 0.9 - 1.1)
#define OFF_SPEED         0xA4

// In citizen tick FUN_140832e90 (SOVIET64.exe v1.1.1.9):
// 1. Station wait timeout limit load: movss xmm7, dword ptr [0x14090af9c] (600.0f)
#define RVA_STATION_TIMEOUT_LOAD 0x832F1B
// 2. Vehicle travel timeout compare: comiss xmm0, dword ptr [0x14090aea8] (380.0f)
#define RVA_VEHICLE_TIMEOUT_CMP  0x8341A1

// Terrain render export for per-frame tick
#define SYM_TERRAIN_RENDER "?Render@C3D_TERRAIN@@QEAAX_NPEAVC3D_CAMERA@@0HH@Z"

// ---------------------------------------------------------------- settings

static int   g_enabled              = 1;
static float g_mult                 = 1.0f;
static float g_stationTimeoutMult   = 1.0f;
static float g_vehicleTimeoutMult   = 1.0f;

// ---------------------------------------------------------------- state

typedef void (*t_TerrainRender)(void*, bool, void*, void*, int, int);
static t_TerrainRender o_TerrainRender = NULL;

static void*  g_lastWorld = NULL;
static size_t g_processedCount = 0;
static float  g_appliedMult = 1.0f;
static float  g_appliedStationMult = 1.0f;
static float  g_appliedVehicleMult = 1.0f;

// Pointer to our allocated float constants within 2GB of g_exeBase:
// [0] = station timeout (vanilla 600.0f)
// [1] = vehicle travel timeout (vanilla 380.0f)
static float* g_customTimeouts = NULL;

// ---------------------------------------------------------------- fast logic

static void* GetWorldPtr(void)
{
    void** slot = (void**)(g_exeBase + RVA_WORLD_PTR);
    if (!ReadablePtr(slot, sizeof(void*))) return NULL;
    return *slot;
}

static void UpdateTimeouts(void)
{
    if (!g_customTimeouts) return;
    g_customTimeouts[0] = 600.0f * g_stationTimeoutMult;
    g_customTimeouts[1] = 380.0f * g_vehicleTimeoutMult;
    Logf("walkspeed  timeouts updated: station=%.1fs (%.2fx), vehicle=%.1fs (%.2fx)",
         g_customTimeouts[0], g_stationTimeoutMult,
         g_customTimeouts[1], g_vehicleTimeoutMult);
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

    bool settingsChanged = (g_appliedMult != g_mult) ||
                           (g_appliedStationMult != g_stationTimeoutMult) ||
                           (g_appliedVehicleMult != g_vehicleTimeoutMult);
    bool isNewWorld = (currentWorld != g_lastWorld) || settingsChanged;
    float oldMult = g_appliedMult;

    if (isNewWorld)
    {
        g_lastWorld = currentWorld;
        g_processedCount = 0;
        g_appliedMult = g_mult;
    }

    if (g_appliedStationMult != g_stationTimeoutMult || g_appliedVehicleMult != g_vehicleTimeoutMult)
    {
        g_appliedStationMult = g_stationTimeoutMult;
        g_appliedVehicleMult = g_vehicleTimeoutMult;
        UpdateTimeouts();
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

    // If speed is vanilla (1.0f) and not resetting, nothing to do
    bool needSpeedUpdate = (isNewWorld && (oldMult != g_mult || g_mult != 1.0f)) ||
                           (!isNewWorld && g_mult != 1.0f && startIndex < count);

    if (!needSpeedUpdate)
    {
        g_processedCount = count;
        return;
    }

    __try
    {
        // Citizen speed processing (OFF_SPEED = 0xA4)
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
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        // Safely catch memory errors
    }
}

// ---------------------------------------------------------------- timeout patches

static bool ApplyTimeoutPatches(void)
{
    BYTE* base = (BYTE*)g_exeBase;

    // Allocate 64 bytes near FUN_140832e90 for our custom float constants
    BYTE* mem = AllocNear(base + RVA_STATION_TIMEOUT_LOAD, 64);
    if (!mem)
    {
        Logf("walkspeed  failed to allocate memory near exe for timeout constants");
        return false;
    }

    g_customTimeouts = (float*)mem;
    g_customTimeouts[0] = 600.0f * g_stationTimeoutMult;
    g_customTimeouts[1] = 380.0f * g_vehicleTimeoutMult;

    // 1. Station wait timeout patch at RVA 0x832F1B
    // Expected: F3 0F 10 3D 79 80 0D 00 (movss xmm7, [rip + 0xd8079])
    BYTE* siteStation = base + RVA_STATION_TIMEOUT_LOAD;
    static const BYTE kExpectStation[4] = { 0xF3, 0x0F, 0x10, 0x3D };
    if (memcmp(siteStation, kExpectStation, sizeof(kExpectStation)) != 0)
    {
        Logf("walkspeed  station timeout instruction mismatch, skipping patch");
        return false;
    }

    int32_t dispStation = (int32_t)((BYTE*)&g_customTimeouts[0] - (siteStation + 8));
    DWORD oldProt;
    if (VirtualProtect(siteStation, 8, PAGE_EXECUTE_READWRITE, &oldProt))
    {
        *(int32_t*)(siteStation + 4) = dispStation;
        VirtualProtect(siteStation, 8, oldProt, &oldProt);
    }
    else
    {
        Logf("walkspeed  VirtualProtect failed on station timeout site");
        return false;
    }

    // 2. Vehicle travel timeout patch at RVA 0x8341A1
    // Expected: 0F 2F 05 00 6D 0D 00 (comiss xmm0, [rip + 0xd6d00])
    BYTE* siteVehicle = base + RVA_VEHICLE_TIMEOUT_CMP;
    static const BYTE kExpectVehicle[3] = { 0x0F, 0x2F, 0x05 };
    if (memcmp(siteVehicle, kExpectVehicle, sizeof(kExpectVehicle)) != 0)
    {
        Logf("walkspeed  vehicle timeout instruction mismatch, skipping patch");
        return false;
    }

    int32_t dispVehicle = (int32_t)((BYTE*)&g_customTimeouts[1] - (siteVehicle + 7));
    if (VirtualProtect(siteVehicle, 7, PAGE_EXECUTE_READWRITE, &oldProt))
    {
        *(int32_t*)(siteVehicle + 3) = dispVehicle;
        VirtualProtect(siteVehicle, 7, oldProt, &oldProt);
    }
    else
    {
        Logf("walkspeed  VirtualProtect failed on vehicle timeout site");
        return false;
    }

    FlushInstructionCache(GetCurrentProcess(), siteStation, 0x1500);
    Logf("walkspeed  timeout limits successfully hooked (station: %.1fs [%.2fx], vehicle: %.1fs [%.2fx])",
         g_customTimeouts[0], g_stationTimeoutMult, g_customTimeouts[1], g_vehicleTimeoutMult);
    return true;
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

    // Specific station waiting timeout multiplier
    if (H->configString(ini, "walkspeed", "station_timeout_multiplier", buf, sizeof(buf), "") && buf[0])
    {
        float val = (float)atof(buf);
        if (val >= 0.5f && val <= 50.0f)
        {
            g_stationTimeoutMult = val;
        }
        else
        {
            Logf("walkspeed  station_timeout_multiplier %.2f out of bounds, clamped", val);
            if (val < 0.5f) g_stationTimeoutMult = 0.5f;
            if (val > 50.0f) g_stationTimeoutMult = 50.0f;
        }
    }
    // Legacy fallback
    else if (H->configString(ini, "walkspeed", "travel_timeout_multiplier", buf, sizeof(buf), "") && buf[0])
    {
        float val = (float)atof(buf);
        if (val >= 0.5f && val <= 50.0f) g_stationTimeoutMult = val;
    }

    // Specific vehicle travel timeout multiplier
    if (H->configString(ini, "walkspeed", "vehicle_timeout_multiplier", buf, sizeof(buf), "") && buf[0])
    {
        float val = (float)atof(buf);
        if (val >= 0.5f && val <= 50.0f)
        {
            g_vehicleTimeoutMult = val;
        }
        else
        {
            Logf("walkspeed  vehicle_timeout_multiplier %.2f out of bounds, clamped", val);
            if (val < 0.5f) g_vehicleTimeoutMult = 0.5f;
            if (val > 50.0f) g_vehicleTimeoutMult = 50.0f;
        }
    }
    // Legacy fallback
    else if (H->configString(ini, "walkspeed", "travel_timeout_multiplier", buf, sizeof(buf), "") && buf[0])
    {
        float val = (float)atof(buf);
        if (val >= 0.5f && val <= 50.0f) g_vehicleTimeoutMult = val;
    }

    UpdateTimeouts();
}

extern "C" __declspec(dllexport) unsigned TsmPluginApiVersion(void)
{
    return TSM_API_VERSION;
}

extern "C" __declspec(dllexport) int TsmPluginInit(const TsmHost* host, TsmPluginInfo* info)
{
    TsmBind(host);
    info->name    = "walkspeed";
    info->version = "3.0";

    ReadSettings();

    if (!g_enabled)
    {
        Logf("walkspeed  disabled");
        return 1;
    }

    Logf("walkspeed  init (speed: %.2fx, station timeout: %.2fx, vehicle timeout: %.2fx)",
         g_mult, g_stationTimeoutMult, g_vehicleTimeoutMult);
    return 0;
}

extern "C" __declspec(dllexport) int TsmPluginStart(void)
{
    if (!g_enabled) return 1;

    // 1. Hook terrain render to maintain citizen walking speeds
    if (!PatchIat(g_exe, DLL_ENGINE, SYM_TERRAIN_RENDER,
                  (void*)HookTerrainRender, (void**)&o_TerrainRender, "terrain render"))
    {
        Logf("walkspeed  failed to hook terrain render");
        return 1;
    }

    // 2. Patch station and vehicle timeouts directly in simulation logic
    ApplyTimeoutPatches();

    Logf("walkspeed  active (speed: %.2fx, station timeout: %.2fx, vehicle timeout: %.2fx)",
         g_mult, g_stationTimeoutMult, g_vehicleTimeoutMult);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }

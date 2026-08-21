
#include "../Include/KittyMemory/MemoryPatch.h"
#include "../Include/ImGui.h"
#include "../Include/RemapTools.h"
#include "../Include/Drawing.h"
#include "../Include/Unity.h"
#include <unistd.h>
#include <dobby.h>
#include <dlfcn.h>
#include <vector>
#include <mutex>
#include <sys/time.h>
#include <cstring>
#include <cstdio>
#include <cmath>

// ─────────────────────────────────────────────────────────────────────────────
//  Helpers
// ─────────────────────────────────────────────────────────────────────────────
static inline void* rva(const char* lib, uintptr_t off) {
    return (void*)getAbsoluteAddress(lib, off);
}

static inline uint8_t* field(void* obj, uintptr_t off) {
    return (uint8_t*)obj + off;
}

template<typename T>
static inline T& fld(void* obj, uintptr_t off) {
    return *reinterpret_cast<T*>((uint8_t*)obj + off);
}

// IL2CPP string layout: [header 16 bytes] [int len @ 0x10] [char16_t chars @ 0x14]
static inline void il2cpp_str_to_char(void* strPtr, char* buf, int bufLen) {
    if (!strPtr) { buf[0] = '\0'; return; }
    int len = fld<int>(strPtr, 0x10);
    if (len <= 0 || len >= bufLen) { buf[0] = '\0'; return; }
    uint16_t* wch = (uint16_t*)((uint8_t*)strPtr + 0x14);
    for (int i = 0; i < len && i < bufLen - 1; i++)
        buf[i] = (char)(wch[i] & 0x7F);
    buf[len < bufLen ? len : bufLen - 1] = '\0';
}

// ObscuredInt read/write (AntiCheats Toolkit v2.x struct layout)
// struct ObscuredInt { int hiddenValue; int currentCryptoKey; ... }
static inline int obscured_read(void* obj, uintptr_t off) {
    int hidden = fld<int>(obj, off + 0x00);
    int key    = fld<int>(obj, off + 0x04);
    return hidden ^ key;
}
static inline void obscured_write(void* obj, uintptr_t off, int val) {
    int key = fld<int>(obj, off + 0x04);
    if (key == 0) key = 0x4B5AC1;            // use a stable key if uninitialized
    fld<int>(obj, off + 0x00) = val ^ key;   // hiddenValue = realValue XOR key
    fld<int>(obj, off + 0x04) = key;
}

static long now_ms() {
    struct timeval tv; gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000L + tv.tv_usec / 1000;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Config / feature toggles
// ─────────────────────────────────────────────────────────────────────────────
static int   lang_idx      = 0;   // 0=Vietnamese 1=English
static bool  bGodMode      = false;
static bool  bInfAmmo      = false;
static bool  bNoRecoil     = false;
static bool  bNoSpread     = false;
static bool  bSpeedHack    = false;
static float speedMult     = 2.0f;
static bool  bEspBox       = false;
static bool  bEspHealth    = false;
static bool  bEspLine      = false;
static bool  bEspDist      = false;
static bool  bEspTeamColor = true;
static bool  bAimbotCircle = false;
static float AimFov        = 150.0f;
static bool  bMenuOpen     = true;

static const char* T(const char* vn, const char* en) {
    return lang_idx == 0 ? vn : en;
}

// ─────────────────────────────────────────────────────────────────────────────
//  ESP entity list
// ─────────────────────────────────────────────────────────────────────────────
struct EspEntry {
    void*  instance;
    long   timestamp;
    int    myTeam;
    bool   isBot;
    float  hp;           // currentHpForShow
    char   name[32];     // Photon nickname (best-effort)
};

static EspEntry g_esp[100];
static std::mutex g_espMtx;

// ─────────────────────────────────────────────────────────────────────────────
//  IL2CPP thread attach
// ─────────────────────────────────────────────────────────────────────────────
static bool g_attached = false;
static void EnsureAttached() {
    if (g_attached) return;
    void* lib = dlopen("libil2cpp.so", RTLD_LAZY | RTLD_NOLOAD);
    if (!lib) return;
    auto domain_get    = (void*(*)())      dlsym(lib, "il2cpp_domain_get");
    auto thread_attach = (void*(*)(void*)) dlsym(lib, "il2cpp_thread_attach");
    if (domain_get && thread_attach) {
        thread_attach(domain_get());
        g_attached = true;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Unity API function pointers  (offsets inherited from build — adjust if needed)
// ─────────────────────────────────────────────────────────────────────────────
static void*    (*Camera_get_main)();
static Vector3  (*Camera_WorldToScreenPoint)(void* cam, Vector3 world);
static void*    (*Component_get_transform)(void* comp);
static Vector3  (*Transform_get_position)(void* tr);

// ─────────────────────────────────────────────────────────────────────────────
//  Try to read player nickname via Kit_PlayerBehaviour → PhotonView → Owner
//  photonView on MonoBehaviourPunCallbacks is accessed via Component system;
//  PUN2 stores it in a lazy-init backing field, typically near offset 0xE8–0x108
//  on the MonoBehaviour subclass depending on Unity / PUN version.
//  We probe a safe window and validate the result.
// ─────────────────────────────────────────────────────────────────────────────
static void try_read_nickname(void* pb, char* out, int outLen) {
    // Probe common photonView cache offsets in MonoBehaviourPunCallbacks
    static const uintptr_t PV_OFFSETS[] = { 0xE0, 0xE8, 0xF0, 0xF8, 0x100 };
    for (auto pvOff : PV_OFFSETS) {
        void* pv = fld<void*>(pb, pvOff);
        if (!pv) continue;
        // Validate: PhotonView.viewIdField @ 0x94 should be > 0 and < 50000
        int vid = fld<int>(pv, 0x94);
        if (vid <= 0 || vid > 50000) continue;
        // PhotonView.Owner @ 0x80
        void* owner = fld<void*>(pv, 0x80);
        if (!owner) continue;
        // Player.nickName @ 0x20 (IL2CppString*)
        void* strPtr = fld<void*>(owner, 0x20);
        if (!strPtr) continue;
        il2cpp_str_to_char(strPtr, out, outLen);
        if (out[0] != '\0') return;
    }
    strncpy(out, "Player", outLen);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Register / update enemy in ESP list  (called from hooked Update)
// ─────────────────────────────────────────────────────────────────────────────
static void RegisterEnemy(void* pb) {
    if (!pb) return;
    if (!bEspBox && !bEspLine && !bEspDist && !bEspHealth) return;

    long  now     = now_ms();
    int   myTeam  = fld<int>(pb,   0x1B8);
    bool  isBot   = fld<bool>(pb,  0x1D0);
    float hp      = fld<float>(pb, 0x148);

    g_espMtx.lock();
    bool found = false;
    for (int i = 0; i < 100; i++) {
        if (g_esp[i].instance == pb) {
            g_esp[i].timestamp = now;
            g_esp[i].myTeam   = myTeam;
            g_esp[i].hp       = hp;
            found = true;
            break;
        }
    }
    if (!found) {
        for (int i = 0; i < 100; i++) {
            if (!g_esp[i].instance || now - g_esp[i].timestamp > 4000) {
                g_esp[i].instance  = pb;
                g_esp[i].timestamp = now;
                g_esp[i].myTeam    = myTeam;
                g_esp[i].isBot     = isBot;
                g_esp[i].hp        = hp;
                try_read_nickname(pb, g_esp[i].name, sizeof(g_esp[i].name));
                break;
            }
        }
    }
    g_espMtx.unlock();
}

// ─────────────────────────────────────────────────────────────────────────────
//  HOOK: Kit_PlayerBehaviour.Update  — Offset: 0x1BB7658
//   • Registers every player instance for ESP
//   • Detects local player (isController @ 0x1BC) for per-frame patches
// ─────────────────────────────────────────────────────────────────────────────
static void (*old_PBUpdate)(void* pb);
static void PBUpdate(void* pb) {
    old_PBUpdate(pb);
    if (!pb) return;

    bool isLocal = fld<bool>(pb, 0x1BC);   // isController

    if (!isLocal) {
        RegisterEnemy(pb);
    } else {
        // ── Speed hack: patch Kit_DefaultMovement ScriptableObject ────────
        if (bSpeedHack) {
            void* mov = fld<void*>(pb, 0xC8);  // movement : Kit_MovementBase
            if (mov) {
                // Kit_DefaultMovement: sprintSpeed@0x18, walkSpeed@0x20, crouchSpeed@0x28
                static float baseSprintSpeed = 0.0f;
                static float baseWalkSpeed   = 0.0f;
                if (baseSprintSpeed <= 0.1f)
                    baseSprintSpeed = fld<float>(mov, 0x18);
                if (baseWalkSpeed <= 0.1f)
                    baseWalkSpeed   = fld<float>(mov, 0x20);
                fld<float>(mov, 0x18) = baseSprintSpeed * speedMult;
                fld<float>(mov, 0x20) = baseWalkSpeed   * speedMult;
                fld<float>(mov, 0x28) = baseWalkSpeed   * speedMult * 0.65f;
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  HOOK: Kit_PlayerBehaviour.LocalDamage  — Offset: 0x1BB8BE4
//   void LocalDamage(float dmg, int gunID, Vec3 shotPos, Vec3 forward,
//                    float force, Vec3 hitPos, int id, bool botShot, int idWhoShot)
//  Returns early (skipping all damage logic) when GodMode is on.
// ─────────────────────────────────────────────────────────────────────────────
typedef void (*LocalDmg_t)(void*, float, int,
                            float, float, float,   // shotPos
                            float, float, float,   // forward
                            float,
                            float, float, float,   // hitPos
                            int, bool, int);
static LocalDmg_t old_LocalDamage;
static void LocalDamage(void* pb, float dmg, int gunID,
                         float spX, float spY, float spZ,
                         float fwX, float fwY, float fwZ,
                         float force,
                         float hpX, float hpY, float hpZ,
                         int id, bool botShot, int idWhoShot) {
    if (bGodMode) return;
    old_LocalDamage(pb, dmg, gunID,
                    spX, spY, spZ,
                    fwX, fwY, fwZ,
                    force,
                    hpX, hpY, hpZ,
                    id, botShot, idWhoShot);
}

// ─────────────────────────────────────────────────────────────────────────────
//  HOOK: Kit_PlayerBehaviour.ApplyDamageNetwork  — Offset: 0x1BBAD30  [PunRPC]
//   void ApplyDamageNetwork(float dmg, bool botShot, int idWhoShot,
//                           int gunID, Vec3 shotPos, Vec3 forward,
//                           float force, Vec3 hitPos, int id)
//  Intercepts networked damage RPCs — god mode backup guard.
// ─────────────────────────────────────────────────────────────────────────────
typedef void (*ApplyDmgNet_t)(void*, float, bool, int, int,
                               float, float, float,
                               float, float, float,
                               float,
                               float, float, float,
                               int);
static ApplyDmgNet_t old_ApplyDmgNet;
static void ApplyDamageNetwork(void* pb, float dmg, bool botShot, int idWhoShot, int gunID,
                                float spX, float spY, float spZ,
                                float fwX, float fwY, float fwZ,
                                float force,
                                float hpX, float hpY, float hpZ,
                                int id) {
    if (bGodMode) return;
    old_ApplyDmgNet(pb, dmg, botShot, idWhoShot, gunID,
                    spX, spY, spZ,
                    fwX, fwY, fwZ,
                    force,
                    hpX, hpY, hpZ,
                    id);
}

// ─────────────────────────────────────────────────────────────────────────────
//  HOOK: Kit_PlayerBehaviour.Die(bool,int,int)  — Offset: 0x1BBA640
//  Final godmode wall — blocks death event entirely.
// ─────────────────────────────────────────────────────────────────────────────
static void (*old_Die)(void*, bool, int, int);
static void Die(void* pb, bool botShot, int killer, int gunID) {
    if (bGodMode) return;
    old_Die(pb, botShot, killer, gunID);
}

// ─────────────────────────────────────────────────────────────────────────────
//  HOOK: Kit_ModernWeaponScript.CalculateWeaponUpdate — Offset: 0x1C29BC8
//   void CalculateWeaponUpdate(Kit_PlayerBehaviour pb, object runtimeData)
//
//  self       = Kit_ModernWeaponScript* (ScriptableObject)
//  pb         = Kit_PlayerBehaviour* (player using this weapon)
//  rtData     = WeaponControllerRuntimeData*
//
//  Handles:
//    • Infinite ammo — writes to WeaponControllerRuntimeData.bulletsLeft (ObscuredInt)
//                      and bulletsLeftToReload
//    • No recoil     — zeros recoilPerShotMin/Max in the ScriptableObject
//    • No spread     — zeros bulletSpreadHipBase/AimBase in the ScriptableObject
// ─────────────────────────────────────────────────────────────────────────────
static void (*old_CalcWpnUpdate)(void* self, void* pb, void* rtData);
static void CalcWpnUpdate(void* self, void* pb, void* rtData) {
    // No recoil + no spread: patch ScriptableObject fields BEFORE update
    // so fire logic reads zero spread/recoil this frame
    if (bNoRecoil && self) {
        fld<float>(self, 0x18C) = 0.0f;   // recoilPerShotMin.x
        fld<float>(self, 0x190) = 0.0f;   // recoilPerShotMin.y
        fld<float>(self, 0x194) = 0.0f;   // recoilPerShotMax.x
        fld<float>(self, 0x198) = 0.0f;   // recoilPerShotMax.y
        fld<float>(self, 0x19C) = 0.0f;   // recoilApplyTime
    }
    if (bNoSpread && self) {
        fld<float>(self, 0x164) = 0.0f;   // bulletSpreadHipBase
        fld<float>(self, 0x168) = 0.0f;   // bulletSpreadHipVelocityAdd
        fld<float>(self, 0x170) = 0.0f;   // bulletSpreadAimBase
        fld<float>(self, 0x174) = 0.0f;   // bulletSpreadAimVelocityAdd
        // reset spray pattern state in runtimeData
        if (rtData) fld<float>(rtData, 0xCC) = 0.0f; // sprayPatternState
    }

    old_CalcWpnUpdate(self, pb, rtData);

    // Infinite ammo: re-fill AFTER the original (which may have decremented)
    if (bInfAmmo && rtData) {
        int maxBullets = self ? fld<int>(self, 0xF4) : 30; // bulletsPerMag
        if (maxBullets <= 0) maxBullets = 30;

        // ObscuredInt bulletsLeft @ 0x50 — write encrypted value
        obscured_write(rtData, 0x50, maxBullets);

        // bulletsLeftToReload @ 0x64 — plain int
        fld<int>(rtData, 0x64) = maxBullets;

        // clear reload flag @ 0x6C
        fld<bool>(rtData, 0x6C) = false;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  ImGui draw utilities
// ─────────────────────────────────────────────────────────────────────────────
static void DrawHealthBar(ImDrawList* dl, float x, float y, float w, float h, float pct) {
    // Background
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h),
                      IM_COL32(0, 0, 0, 160));
    // Foreground
    ImU32 col;
    if      (pct > 0.6f) col = IM_COL32(60,  220, 60,  255);
    else if (pct > 0.3f) col = IM_COL32(230, 180, 20,  255);
    else                 col = IM_COL32(220, 40,  40,  255);

    float filled = w * pct;
    if (filled > 0.5f)
        dl->AddRectFilled(ImVec2(x, y), ImVec2(x + filled, y + h), col);

    // Border
    dl->AddRect(ImVec2(x, y), ImVec2(x + w, y + h),
                IM_COL32(0, 0, 0, 200), 0.0f, 0, 0.8f);
}

static void DrawCornerBox(ImDrawList* dl, float x, float y, float w, float h,
                          ImU32 col, float thick = 1.5f) {
    float lx = w * 0.25f;
    float ly = h * 0.25f;
    // Top-left
    dl->AddLine({x,y},     {x+lx,y},   col, thick);
    dl->AddLine({x,y},     {x,y+ly},   col, thick);
    // Top-right
    dl->AddLine({x+w,y},   {x+w-lx,y}, col, thick);
    dl->AddLine({x+w,y},   {x+w,y+ly}, col, thick);
    // Bottom-left
    dl->AddLine({x,y+h},   {x+lx,y+h}, col, thick);
    dl->AddLine({x,y+h},   {x,y+h-ly}, col, thick);
    // Bottom-right
    dl->AddLine({x+w,y+h}, {x+w-lx,y+h}, col, thick);
    dl->AddLine({x+w,y+h}, {x+w,y+h-ly}, col, thick);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Main draw callback — called every frame by the mod menu renderer
// ─────────────────────────────────────────────────────────────────────────────
static void DrawMenu() {
    EnsureAttached();

    // ── Style ────────────────────────────────────────────────────────────────
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding  = 8.0f;
    s.FrameRounding   = 5.0f;
    s.ItemSpacing     = {8, 7};
    s.WindowPadding   = {12, 10};
    s.GrabRounding    = 4.0f;
    s.Colors[ImGuiCol_WindowBg]      = {0.04f, 0.04f, 0.08f, 0.97f};
    s.Colors[ImGuiCol_TitleBgActive] = {0.00f, 0.18f, 0.45f, 1.00f};
    s.Colors[ImGuiCol_FrameBg]       = {0.08f, 0.08f, 0.16f, 1.00f};
    s.Colors[ImGuiCol_CheckMark]     = {0.00f, 0.90f, 1.00f, 1.00f};
    s.Colors[ImGuiCol_Tab]           = {0.05f, 0.10f, 0.20f, 1.00f};
    s.Colors[ImGuiCol_TabActive]     = {0.00f, 0.20f, 0.50f, 1.00f};
    s.Colors[ImGuiCol_TabHovered]    = {0.00f, 0.30f, 0.70f, 1.00f};
    s.Colors[ImGuiCol_SliderGrab]    = {0.00f, 0.75f, 1.00f, 1.00f};
    s.Colors[ImGuiCol_Button]        = {0.00f, 0.18f, 0.40f, 1.00f};
    s.Colors[ImGuiCol_ButtonHovered] = {0.00f, 0.28f, 0.60f, 1.00f};
    s.Colors[ImGuiCol_Header]        = {0.00f, 0.18f, 0.45f, 0.80f};
    s.Colors[ImGuiCol_HeaderHovered] = {0.00f, 0.28f, 0.60f, 0.90f};
    s.Colors[ImGuiCol_Separator]     = {0.10f, 0.30f, 0.60f, 0.60f};

    ImDrawList* dl   = ImGui::GetBackgroundDrawList();
    ImVec2      sc   = ImGui::GetIO().DisplaySize;

    // ── Aimbot FOV circle ─────────────────────────────────────────────────────
    if (bAimbotCircle)
        dl->AddCircle({sc.x * 0.5f, sc.y * 0.5f},
                      AimFov, IM_COL32(255, 255, 255, 130), 120, 1.2f);

    // ── ESP render ────────────────────────────────────────────────────────────
    if ((bEspBox || bEspLine || bEspDist || bEspHealth) &&
        Camera_get_main && Camera_WorldToScreenPoint &&
        Component_get_transform && Transform_get_position)
    {
        void* cam = Camera_get_main();
        if (cam) {
            long now = now_ms();
            g_espMtx.lock();

            for (int i = 0; i < 100; i++) {
                void* pb = g_esp[i].instance;
                if (!pb) continue;

                // Expire stale entries (enemy dead or despawned)
                if (now - g_esp[i].timestamp > 4000) {
                    g_esp[i].instance = nullptr;
                    continue;
                }

                // Read realtime position via syncPos field @ 0x22C
                float wx = fld<float>(pb, 0x22C);
                float wy = fld<float>(pb, 0x230);
                float wz = fld<float>(pb, 0x234);

                if (wx == 0.0f && wy == 0.0f && wz == 0.0f) continue;
                if (wy > 300.0f || wy < -100.0f) continue;

                Vector3 footW = {wx, wy,          wz};
                Vector3 headW = {wx, wy + 1.80f,  wz};

                Vector3 footS = Camera_WorldToScreenPoint(cam, footW);
                Vector3 headS = Camera_WorldToScreenPoint(cam, headW);

                if (footS.Z < 0.5f || footS.Z > 800.0f) continue;

                // Flip Y (Unity → screen)
                float fx = footS.X;
                float fy = sc.y - footS.Y;
                float hy = sc.y - headS.Y;
                float h  = fy - hy;

                if (h < 6.0f) continue;

                float w  = h * 0.42f;
                float cx = fx;
                float dist = footS.Z;

                // Team color
                ImU32 boxCol;
                if (bEspTeamColor) {
                    int tm = g_esp[i].myTeam;
                    if      (tm == 0) boxCol = IM_COL32(255, 70,  70,  230);  // Team 0 = red
                    else if (tm == 1) boxCol = IM_COL32(70,  170, 255, 230);  // Team 1 = blue
                    else              boxCol = IM_COL32(255, 220, 0,   230);  // other
                } else {
                    boxCol = g_esp[i].isBot
                        ? IM_COL32(255, 200, 0, 230)
                        : IM_COL32(0,   255, 80, 230);
                }

                // Snap line
                if (bEspLine)
                    dl->AddLine({sc.x * 0.5f, sc.y},
                                {cx, fy},
                                IM_COL32(255, 80, 80, 180), 1.1f);

                // Corner box
                if (bEspBox)
                    DrawCornerBox(dl, cx - w * 0.5f, hy, w, h, boxCol, 1.5f);

                // Health bar (left side of box)
                if (bEspHealth) {
                    float maxHp = 100.0f;
                    float pct   = g_esp[i].hp / maxHp;
                    if (pct < 0.0f) pct = 0.0f;
                    if (pct > 1.0f) pct = 1.0f;
                    DrawHealthBar(dl, cx - w * 0.5f - 6.0f, hy, 4.0f, h, pct);
                }

                // Distance label
                if (bEspDist) {
                    char buf[20];
                    snprintf(buf, sizeof(buf), "%.0fm", dist);
                    dl->AddText({cx - 12.0f, hy - 14.0f},
                                IM_COL32(255, 230, 0, 255), buf);
                }

                // Bot tag
                if (g_esp[i].isBot) {
                    dl->AddText({cx - w * 0.5f, hy - 26.0f},
                                IM_COL32(255, 200, 0, 200), "[BOT]");
                }
            }

            g_espMtx.unlock();
        }
    }

    // ── Menu window ───────────────────────────────────────────────────────────
    if (!bMenuOpen) return;

    ImGui::SetNextWindowSize({340, 370}, ImGuiCond_FirstUseEver);
    ImGui::Begin("  MWMod  |  DUONG DEV  ", &bMenuOpen,
                 ImGuiWindowFlags_NoCollapse);

    ImGui::TextColored({0.0f, 0.85f, 1.0f, 1.0f}, "  DUONG DEV TEAM");
    ImGui::TextColored({0.4f, 0.4f,  0.4f, 1.0f}, "  t.me/chanelmodteam");
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::BeginTabBar("##tabs")) {

        // ── Tab: Player ───────────────────────────────────────────────────
        if (ImGui::BeginTabItem(T("NGUOI CHOI", "PLAYER"))) {
            ImGui::Spacing();

            ImGui::Checkbox(T("Bat Tu (God Mode)", "God Mode"), &bGodMode);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(T("Chan ca LocalDamage + ApplyDamageNetwork + Die",
                                    "Blocks LocalDamage, ApplyDamageNetwork, Die"));
            ImGui::Spacing();

            ImGui::Checkbox(T("Tang Toc", "Speed Hack"), &bSpeedHack);
            if (bSpeedHack) {
                ImGui::SameLine();
                ImGui::SetNextItemWidth(120.0f);
                ImGui::SliderFloat("##spd", &speedMult, 1.0f, 5.0f, "x%.1f");
            }
            ImGui::Spacing();

            ImGui::Separator();
            ImGui::Spacing();
            ImGui::TextColored({0.5f, 0.9f, 0.5f, 1.0f},
                               T("Trang Thai:", "Status:"));
            ImGui::TextColored(bGodMode ? ImVec4{0,1,0,1} : ImVec4{1,0.3f,0.3f,1},
                               T(bGodMode ? " Bat Tu: BAT" : " Bat Tu: TAT",
                                 bGodMode ? " GodMode: ON" : " GodMode: OFF"));
            ImGui::EndTabItem();
        }

        // ── Tab: Weapon ───────────────────────────────────────────────────
        if (ImGui::BeginTabItem(T("VU KHI", "WEAPON"))) {
            ImGui::Spacing();

            ImGui::Checkbox(T("Vo Han Dan", "Infinite Ammo"), &bInfAmmo);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(T("Ghi vao WeaponControllerRuntimeData.bulletsLeft (ObscuredInt)",
                                    "Writes to WeaponControllerRuntimeData.bulletsLeft (ObscuredInt)"));
            ImGui::Spacing();

            ImGui::Checkbox(T("Khong Giat Sung", "No Recoil"), &bNoRecoil);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(T("Zero recoilPerShotMin/Max trong ScriptableObject",
                                    "Zeros recoilPerShotMin/Max in ScriptableObject"));
            ImGui::Spacing();

            ImGui::Checkbox(T("Khong Tan Dan", "No Spread"), &bNoSpread);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(T("Zero bulletSpreadHip/AimBase + reset spray pattern",
                                    "Zeros bulletSpreadHip/AimBase + resets spray pattern"));
            ImGui::EndTabItem();
        }

        // ── Tab: ESP ──────────────────────────────────────────────────────
        if (ImGui::BeginTabItem("ESP")) {
            ImGui::Spacing();

            ImGui::Checkbox(T("Ve Hop (Corner)", "Draw Corner Box"), &bEspBox);
            ImGui::Spacing();
            ImGui::Checkbox(T("Thanh Mau", "Health Bar"), &bEspHealth);
            ImGui::Spacing();
            ImGui::Checkbox(T("Ve Duong", "Snap Line"), &bEspLine);
            ImGui::Spacing();
            ImGui::Checkbox(T("Khoang Cach", "Distance"), &bEspDist);
            ImGui::Spacing();
            ImGui::Checkbox(T("Mau Theo Team", "Team Color"), &bEspTeamColor);
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            // Live entity count
            int count = 0;
            g_espMtx.lock();
            long now = now_ms();
            for (int i = 0; i < 100; i++)
                if (g_esp[i].instance && now - g_esp[i].timestamp < 4000) count++;
            g_espMtx.unlock();

            ImGui::TextColored({0.6f, 0.8f, 1.0f, 1.0f},
                               T("Ke Thu Hien Tai: %d", "Enemies Tracked: %d"), count);
            ImGui::EndTabItem();
        }

        // ── Tab: Aim ──────────────────────────────────────────────────────
        if (ImGui::BeginTabItem("AIM")) {
            ImGui::Spacing();
            ImGui::Checkbox(T("Hien Vong Tron FOV", "Show FOV Circle"), &bAimbotCircle);
            if (bAimbotCircle) {
                ImGui::Spacing();
                ImGui::SetNextItemWidth(180.0f);
                ImGui::SliderFloat(T("Pham Vi (px)", "FOV (px)"),
                                   &AimFov, 40.0f, 600.0f, "%.0f px");
            }
            ImGui::Spacing();
            ImGui::TextColored({0.6f, 0.6f, 0.6f, 1.0f},
                               T("Aimbot auto: dung syncPos @ 0x22C",
                                 "Auto-aim target: syncPos @ 0x22C"));
            ImGui::EndTabItem();
        }

        // ── Tab: Settings ─────────────────────────────────────────────────
        if (ImGui::BeginTabItem(T("CAI DAT", "SETTINGS"))) {
            ImGui::Spacing();
            const char* langs[] = {" Tieng Viet", " English"};
            ImGui::SetNextItemWidth(160.0f);
            ImGui::Combo(T("Ngon Ngu", "Language"), &lang_idx, langs, 2);
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::TextColored({1.0f, 0.8f, 0.0f, 1.0f}, "ADMIN: @DUONG DEV");
            ImGui::TextColored({0.0f, 0.8f, 1.0f, 1.0f}, "TG: t.me/chanelmodteam");
            ImGui::TextColored({0.3f, 0.3f, 0.3f, 1.0f}, "MWMod v2.0 | MarsFPSKit Build");
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    ImGui::End();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Mod init thread — waits for libil2cpp, resolves APIs, installs hooks
// ─────────────────────────────────────────────────────────────────────────────
static void* ModThread(void*) {
    initModMenu((void*)DrawMenu);

    // Wait for libil2cpp.so to be loaded
    while (getAbsoluteAddress("libil2cpp.so", 0) == 0)
        sleep(1);
    sleep(5);   // additional settle time for Photon init

    // ── Unity internal API addresses ─────────────────────────────────────────
    // Offsets below are for the Unity version bundled with this APK.
    // If ESP position looks wrong, re-scan for Camera::WorldToScreenPoint.
    Camera_get_main           = (void*(*)())
        rva("libil2cpp.so", 0x2b6dc50);
    Camera_WorldToScreenPoint = (Vector3(*)(void*, Vector3))
        rva("libil2cpp.so", 0x2b6da24);
    Component_get_transform   = (void*(*)(void*))
        rva("libil2cpp.so", 0x2b95be4);
    Transform_get_position    = (Vector3(*)(void*))
        rva("libil2cpp.so", 0x2ba1fdc);

    // ── Hooks ─────────────────────────────────────────────────────────────────

    // Kit_PlayerBehaviour.Update — entity registration + local player patches
    DobbyHook(rva("libil2cpp.so", 0x1BB7658),
              (void*)PBUpdate, (void**)&old_PBUpdate);

    // Kit_PlayerBehaviour.LocalDamage (float overload)
    DobbyHook(rva("libil2cpp.so", 0x1BB8BE4),
              (void*)LocalDamage, (void**)&old_LocalDamage);

    // Kit_PlayerBehaviour.ApplyDamageNetwork [PunRPC]
    DobbyHook(rva("libil2cpp.so", 0x1BBAD30),
              (void*)ApplyDamageNetwork, (void**)&old_ApplyDmgNet);

    // Kit_PlayerBehaviour.Die(bool, int, int)
    DobbyHook(rva("libil2cpp.so", 0x1BBA640),
              (void*)Die, (void**)&old_Die);

    // Kit_ModernWeaponScript.CalculateWeaponUpdate — ammo + recoil + spread
    DobbyHook(rva("libil2cpp.so", 0x1C29BC8),
              (void*)CalcWpnUpdate, (void**)&old_CalcWpnUpdate);

    pthread_exit(nullptr);
}

// ─────────────────────────────────────────────────────────────────────────────
//  JNI + constructor entry
// ─────────────────────────────────────────────────────────────────────────────
extern "C" {
    static JavaVM* g_jvm  = nullptr;
    static JNIEnv* g_jenv = nullptr;

    __attribute__((visibility("default")))
    jint loadJNI(JavaVM* vm) {
        g_jvm = vm;
        vm->AttachCurrentThread(&g_jenv, nullptr);
        return JNI_VERSION_1_6;
    }
}

__attribute__((constructor))
void init() {
    pthread_t t;
    pthread_create(&t, nullptr, ModThread, nullptr);
    RemapTools::RemapLibrary("libLoader.so");
}

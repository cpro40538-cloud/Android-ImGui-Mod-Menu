#include "../Include/KittyMemory/MemoryPatch.h"
#include "../Include/ImGui.h"
#include "../Include/RemapTools.h"
#include "../Include/Drawing.h"
#include "../Include/Unity.h"
#include <unistd.h>
#include <dobby.h>
#include <dlfcn.h>
#include <mutex>
#include <atomic>
#include <cstdarg>
#include <sys/time.h>
#include <cstdint>
#include <android/log.h>

#define LOG_TAG "AXIOM"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ================================================================
//  LANGUAGE
// ================================================================
static int lang_idx = 0;
static const char* T(const char* vn, const char* en) {
    return lang_idx == 0 ? vn : en;
}

// ================================================================
//  IN-APP DEBUG LOG — KHONG CAN PC/adb/root
//  Xem truc tiep trong tab DEBUG cua menu ImGui.
// ================================================================
#define DBG_LOG_LINES    80
#define DBG_LOG_LINE_LEN 168
struct DbgLogBuffer {
    char lines[DBG_LOG_LINES][DBG_LOG_LINE_LEN];
    int  head  = 0;
    int  count = 0;
    std::mutex mtx;
};
static DbgLogBuffer g_dbgLog;

static void DbgLog(const char* fmt, ...) {
    char buf[DBG_LOG_LINE_LEN];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "%s", buf);

    g_dbgLog.mtx.lock();
    strncpy(g_dbgLog.lines[g_dbgLog.head], buf, DBG_LOG_LINE_LEN - 1);
    g_dbgLog.lines[g_dbgLog.head][DBG_LOG_LINE_LEN - 1] = '\0';
    g_dbgLog.head = (g_dbgLog.head + 1) % DBG_LOG_LINES;
    if (g_dbgLog.count < DBG_LOG_LINES) g_dbgLog.count++;
    g_dbgLog.mtx.unlock();
}

// ================================================================
//  TOGGLES
// ================================================================
static bool bGodMode      = false;
static bool bOneShotKill  = false;
static bool bInfAmmo      = false;
static bool bNoRecoil     = false;
static bool bSpeedHack    = false;
static bool bFullMoney    = false;
static bool bFullBox      = false;
static bool bNoAds        = false;
static bool bUnlockWeapon = false;
static bool bEspBox       = false;
static bool bEspLine      = false;
static bool bEspDist      = false;
static bool bEspHp        = false;
static bool bAimCircle    = false;

static float speedMult  = 2.0f;
static float AimFov     = 150.0f;
static float damageMult = 999.0f;

#define CT_GOLD       1
#define CT_GRENADE    2
#define CT_MEDICAL    3
#define CT_TICKET     4
#define CT_BOX_COPPER 103
#define CT_BOX_SILVER 104
#define CT_BOX_GOLDEN 105

// ================================================================
//  ESP
// ================================================================
struct EspEntry { void* inst; long ts; float hp; float maxHp; };
static EspEntry   espList[128] = {};
static std::mutex espMtx;
static long now_ms() {
    struct timeval tv; gettimeofday(&tv, NULL);
    return (long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

// ================================================================
//  DEBUG COUNTERS/FLAGS — hien trong tab DEBUG
// ================================================================
static std::atomic<bool> dbgFirstApplyDamage{false};
static std::atomic<bool> dbgFirstCharDmgUpdate{false};
static std::atomic<bool> dbgFirstArcGetMoney{false};
static std::atomic<bool> dbgFirstArcUseMoney{false};
static std::atomic<bool> dbgFirstArcGetKeys{false};
static std::atomic<bool> dbgFirstArcUseKeys{false};
static std::atomic<bool> dbgFirstArcUnlockWeapons{false};
static std::atomic<bool> dbgFirstLoadingAwake{false};
static std::atomic<bool> dbgLoadingInstFoundViaStatic{false};
static std::atomic<int>  dbgApplyDamageCount{0};
static std::atomic<int>  dbgCharDmgUpdateCount{0};
static std::atomic<int>  dbgArcUseMoneyCount{0};
static std::atomic<int>  dbgArcUseKeysCount{0};

// ================================================================
//  UNITY CAMERA API
// ================================================================
static void*   (*Camera_get_main)()                                   = nullptr;
static Vector3 (*Camera_WorldToScreenPoint)(void* cam, Vector3 world) = nullptr;
static void*   (*Component_get_transform)(void* comp)                 = nullptr;
static Vector3 (*Transform_get_position)(void* trans)                 = nullptr;

// ================================================================
//  IL2CPP THREAD ATTACH + STATIC FIELD ACCESS
// ================================================================
static std::atomic<bool> g_attached{false};
static void EnsureAttached() {
    if (g_attached.load(std::memory_order_relaxed)) return;
    void* lib = dlopen("libil2cpp.so", RTLD_LAZY | RTLD_NOLOAD);
    if (!lib) return;
    auto domain_get    = (void*(*)())      dlsym(lib, "il2cpp_domain_get");
    auto thread_attach = (void*(*)(void*)) dlsym(lib, "il2cpp_thread_attach");
    if (domain_get && thread_attach) {
        thread_attach(domain_get());
        g_attached.store(true, std::memory_order_release);
    }
}

static void* (*il2cpp_domain_get_p)()                                        = nullptr;
static void* (*il2cpp_domain_assembly_open_p)(void*, const char*)            = nullptr;
static void* (*il2cpp_assembly_get_image_p)(void*)                           = nullptr;
static void* (*il2cpp_class_from_name_p)(void*, const char*, const char*)    = nullptr;
static void* (*il2cpp_class_get_static_field_data_p)(void*)                  = nullptr;

static void InitIl2CppStaticAPI() {
    if (il2cpp_domain_get_p) return;
    void* lib = dlopen("libil2cpp.so", RTLD_LAZY | RTLD_NOLOAD);
    if (!lib) return;
    il2cpp_domain_get_p                 = (void*(*)())dlsym(lib, "il2cpp_domain_get");
    il2cpp_domain_assembly_open_p       = (void*(*)(void*,const char*))dlsym(lib, "il2cpp_domain_assembly_open");
    il2cpp_assembly_get_image_p         = (void*(*)(void*))dlsym(lib, "il2cpp_assembly_get_image");
    il2cpp_class_from_name_p            = (void*(*)(void*,const char*,const char*))dlsym(lib, "il2cpp_class_from_name");
    il2cpp_class_get_static_field_data_p= (void*(*)(void*))dlsym(lib, "il2cpp_class_get_static_field_data");
}

static void* GetLoadingOncesInstance() {
    InitIl2CppStaticAPI();
    if (!il2cpp_domain_get_p || !il2cpp_class_from_name_p || !il2cpp_class_get_static_field_data_p)
        return nullptr;
    void* domain = il2cpp_domain_get_p();
    if (!domain) return nullptr;
    void* assembly = il2cpp_domain_assembly_open_p(domain, "Assembly-CSharp");
    if (!assembly) return nullptr;
    void* image = il2cpp_assembly_get_image_p(assembly);
    if (!image) return nullptr;
    void* klass = il2cpp_class_from_name_p(image, "", "LoadingOnces");
    if (!klass) return nullptr;
    void* staticData = il2cpp_class_get_static_field_data_p(klass);
    if (!staticData) return nullptr;
    void* inst = *(void**)((uint8_t*)staticData + 0x0);
    if (inst && !dbgLoadingInstFoundViaStatic.exchange(true))
        DbgLog("[OK] LoadingOnces.Instance doc duoc qua static field, inst=%p", inst);
    return inst;
}

// ================================================================
//  HOOK: God Mode + One Shot Kill — CharacterDamage.ApplyDamage
//  RVA: 0x119235C — LUU Y: hook nay CHI fire trong che do
//  OFFLINE/PvE (zombie mode). Trong PVP, damage tinh o SERVER,
//  ham nay khong duoc goi cuc bo -> khong the GodMode/OneShot
//  PVP bang cach patch memory client duoc (day la thiet ke
//  anti-cheat chuan cua game multiplayer, khong phai loi code).
// ================================================================
static void (*old_ApplyDamage)(void* _this,
    float dmg, float dx, float dy, float dz,
    float ax, float ay, float az,
    void* attacker, bool isPlayer, bool isExplosion,
    void* hitBody, float force, bool isHead);

static void hook_ApplyDamage(void* _this,
    float dmg, float dx, float dy, float dz,
    float ax, float ay, float az,
    void* attacker, bool isPlayer, bool isExplosion,
    void* hitBody, float force, bool isHead)
{
    if (!dbgFirstApplyDamage.exchange(true))
        DbgLog("[FIRE] ApplyDamage lan dau! dmg=%.1f isPlayer=%d", dmg, isPlayer);
    dbgApplyDamageCount++;

    if (_this) {
        void* aiComp = *(void**)((uint8_t*)_this + 0x30);
        bool targetIsPlayer = (aiComp == nullptr);
        if (bGodMode && targetIsPlayer) return;
        if (bOneShotKill && !targetIsPlayer) dmg *= damageMult;
    }

    if (old_ApplyDamage)
        old_ApplyDamage(_this, dmg, dx, dy, dz, ax, ay, az,
                        attacker, isPlayer, isExplosion, hitBody, force, isHead);
}

// ================================================================
//  HOOK: Infinite Ammo + No Recoil spread — WeaponBehavior.Update
//  RVA: 0x1309740
// ================================================================
static void (*old_WeapUpdate)(void* inst);
static void hook_WeapUpdate(void* inst) {
    if (old_WeapUpdate) old_WeapUpdate(inst);
    if (!inst) return;

    if (bInfAmmo) {
        int perClip = *(int*)((uint8_t*)inst + 0x118);
        int maxAmmo = *(int*)((uint8_t*)inst + 0x120);
        *(int*)((uint8_t*)inst + 0x114) = perClip;
        *(int*)((uint8_t*)inst + 0x110) = maxAmmo;
    }
    if (bNoRecoil) {
        *(float*)((uint8_t*)inst + 0x5E0) = 0.0f;
        *(float*)((uint8_t*)inst + 0x5E4) = 0.0f;
        *(float*)((uint8_t*)inst + 0x560) = 0.0f;
        *(float*)((uint8_t*)inst + 0x608) = 0.0f;
        *(float*)((uint8_t*)inst + 0x60C) = 0.0f;
        *(float*)((uint8_t*)inst + 0x610) = 0.0f;
    }
}

static void (*old_WeaponKick)(void* inst);
static void hook_WeaponKick(void* inst) {
    if (bNoRecoil) return;
    if (old_WeaponKick) old_WeaponKick(inst);
}

// ================================================================
//  HOOK: Speed Hack — FPSRigidBodyWalker.FixedUpdate RVA: 0x12EC900
// ================================================================
static void (*old_FPSFixedUpdate)(void* inst);
static void hook_FPSFixedUpdate(void* inst) {
    if (!inst) { if (old_FPSFixedUpdate) old_FPSFixedUpdate(inst); return; }
    if (bSpeedHack) {
        float origWalk   = *(float*)((uint8_t*)inst + 0xD4);
        float origSprint = *(float*)((uint8_t*)inst + 0xD8);
        *(float*)((uint8_t*)inst + 0xD4) = origWalk   * speedMult;
        *(float*)((uint8_t*)inst + 0xD8) = origSprint * speedMult;
        if (old_FPSFixedUpdate) old_FPSFixedUpdate(inst);
        *(float*)((uint8_t*)inst + 0xD4) = origWalk;
        *(float*)((uint8_t*)inst + 0xD8) = origSprint;
    } else {
        if (old_FPSFixedUpdate) old_FPSFixedUpdate(inst);
    }
}

// ================================================================
//  HOOK: ArchiveData — he thong tien/key that su
// ================================================================
static int  (*old_ArcGetMoney)(void* self);
static void (*old_ArcUseMoney)(void* self, int moneyUse);
static int  (*old_ArcGetTickets)(void* self);
static void (*old_ArcUseTickets)(void* self, int ticketUse);
static int  (*old_ArcGetKeys)(void* self);
static void (*old_ArcUseKeys)(void* self, int keyUse);
static int  (*old_ArcGetSpecialKeys)(void* self);
static void (*old_ArcUseSpecialKeys)(void* self, int keyUse);
static int  (*old_ArcGetUnlockAllWeapons)(void* self);

static int hook_ArcGetMoney(void* self) {
    if (!dbgFirstArcGetMoney.exchange(true)) DbgLog("[FIRE] ArcGetMoney lan dau!");
    if (bFullMoney) return 999999999;
    return old_ArcGetMoney ? old_ArcGetMoney(self) : 0;
}
static void hook_ArcUseMoney(void* self, int moneyUse) {
    if (!dbgFirstArcUseMoney.exchange(true)) DbgLog("[FIRE] ArcUseMoney lan dau! use=%d", moneyUse);
    dbgArcUseMoneyCount++;
    if (bFullMoney) return;
    if (old_ArcUseMoney) old_ArcUseMoney(self, moneyUse);
}
static int hook_ArcGetTickets(void* self) {
    if (bFullMoney) return 999999999;
    return old_ArcGetTickets ? old_ArcGetTickets(self) : 0;
}
static void hook_ArcUseTickets(void* self, int ticketUse) {
    if (bFullMoney) return;
    if (old_ArcUseTickets) old_ArcUseTickets(self, ticketUse);
}
static int hook_ArcGetKeys(void* self) {
    if (!dbgFirstArcGetKeys.exchange(true)) DbgLog("[FIRE] ArcGetKeys lan dau!");
    if (bFullBox) return 999999999;
    return old_ArcGetKeys ? old_ArcGetKeys(self) : 0;
}
static void hook_ArcUseKeys(void* self, int keyUse) {
    if (!dbgFirstArcUseKeys.exchange(true)) DbgLog("[FIRE] ArcUseKeys lan dau! use=%d", keyUse);
    dbgArcUseKeysCount++;
    if (bFullBox) return;
    if (old_ArcUseKeys) old_ArcUseKeys(self, keyUse);
}
static int hook_ArcGetSpecialKeys(void* self) {
    if (bFullBox) return 999999999;
    return old_ArcGetSpecialKeys ? old_ArcGetSpecialKeys(self) : 0;
}
static void hook_ArcUseSpecialKeys(void* self, int keyUse) {
    if (bFullBox) return;
    if (old_ArcUseSpecialKeys) old_ArcUseSpecialKeys(self, keyUse);
}
static int hook_ArcGetUnlockAllWeapons(void* self) {
    if (!dbgFirstArcUnlockWeapons.exchange(true)) DbgLog("[FIRE] ArcGetUnlockAllWeapons lan dau!");
    if (bUnlockWeapon) return 1;
    return old_ArcGetUnlockAllWeapons ? old_ArcGetUnlockAllWeapons(self) : 0;
}

// ================================================================
//  HOOK: ItemDataManager — backup display, an toan (khong force-write)
// ================================================================
static int  (*old_GetCurrency)(int type);
static void (*old_SetCurrency)(int type, int num);

static int hook_GetCurrency(int type) {
    int real = old_GetCurrency ? old_GetCurrency(type) : 0;
    if (bFullMoney && (type==CT_GOLD||type==CT_GRENADE||type==CT_MEDICAL||type==CT_TICKET))
        return 999999999;
    if (bFullBox && (type==CT_BOX_COPPER||type==CT_BOX_SILVER||type==CT_BOX_GOLDEN))
        return 999999999;
    return real;
}
static void hook_SetCurrency(int type, int num) {
    bool isMoneyType = (type==CT_GOLD||type==CT_GRENADE||type==CT_MEDICAL||type==CT_TICKET);
    bool isBoxType   = (type==CT_BOX_COPPER||type==CT_BOX_SILVER||type==CT_BOX_GOLDEN);
    if ((bFullMoney && isMoneyType) || (bFullBox && isBoxType)) {
        int real = old_GetCurrency ? old_GetCurrency(type) : 0;
        if (num < real) return;
    }
    if (old_SetCurrency) old_SetCurrency(type, num);
}

// ================================================================
//  HOOK: LoadingOnces
// ================================================================
static void* g_loadingInst = nullptr;
static void (*old_LoadingAwake)(void* inst);
static void hook_LoadingAwake(void* inst) {
    if (!dbgFirstLoadingAwake.exchange(true)) DbgLog("[FIRE] LoadingOnces.Awake lan dau!");
    g_loadingInst = inst;
    if (old_LoadingAwake) old_LoadingAwake(inst);
    if (bNoAds)        *(bool*)((uint8_t*)inst + 0x48) = true;
    if (bUnlockWeapon) *(bool*)((uint8_t*)inst + 0x45) = true;
}

// ================================================================
//  HOOK: ESP — CharacterDamage.Update RVA: 0x1191E10
// ================================================================
static void (*old_CharDmgUpdate)(void* inst);
static void hook_CharDmgUpdate(void* inst) {
    if (old_CharDmgUpdate) old_CharDmgUpdate(inst);
    if (!inst) return;

    if (!dbgFirstCharDmgUpdate.exchange(true)) DbgLog("[FIRE] CharDmgUpdate lan dau!");
    dbgCharDmgUpdateCount++;

    if (!bEspBox && !bEspLine && !bEspDist && !bEspHp) return;

    void* aiComp = *(void**)((uint8_t*)inst + 0x30);
    if (!aiComp) return;

    int factionNum = *(int*)((uint8_t*)aiComp + 0xEC);
    if (factionNum == 1) return;

    float hp    = *(float*)((uint8_t*)inst + 0x40);
    float maxHp = *(float*)((uint8_t*)inst + 0x44);
    if (hp <= 0.0f || maxHp <= 0.0f) return;

    long now = now_ms();
    espMtx.lock();
    bool found = false;
    for (int i = 0; i < 128; i++) {
        if (espList[i].inst == inst) { espList[i] = {inst, now, hp, maxHp}; found = true; break; }
    }
    if (!found) {
        for (int i = 0; i < 128; i++) {
            if (!espList[i].inst || now - espList[i].ts > 5000) {
                espList[i] = {inst, now, hp, maxHp}; break;
            }
        }
    }
    espMtx.unlock();
}

// ================================================================
//  DRAW
// ================================================================
static void DrawMenu() {
    EnsureAttached();

    if (!g_loadingInst) g_loadingInst = GetLoadingOncesInstance();
    if (g_loadingInst) {
        if (bNoAds)        *(bool*)((uint8_t*)g_loadingInst + 0x48) = true;
        if (bUnlockWeapon) *(bool*)((uint8_t*)g_loadingInst + 0x45) = true;
    }

    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 10.0f; s.FrameRounding = 5.0f;
    s.GrabRounding   =  4.0f; s.ItemSpacing   = ImVec2(8, 7);
    s.WindowPadding  = ImVec2(12, 12);
    s.Colors[ImGuiCol_WindowBg]      = ImVec4(0.04f,0.04f,0.08f,0.97f);
    s.Colors[ImGuiCol_TitleBg]       = ImVec4(0.00f,0.12f,0.30f,1.00f);
    s.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.00f,0.20f,0.50f,1.00f);
    s.Colors[ImGuiCol_FrameBg]       = ImVec4(0.08f,0.08f,0.15f,1.00f);
    s.Colors[ImGuiCol_CheckMark]     = ImVec4(0.00f,0.90f,1.00f,1.00f);
    s.Colors[ImGuiCol_Tab]           = ImVec4(0.05f,0.10f,0.20f,1.00f);
    s.Colors[ImGuiCol_TabActive]     = ImVec4(0.00f,0.20f,0.50f,1.00f);
    s.Colors[ImGuiCol_TabHovered]    = ImVec4(0.00f,0.30f,0.70f,1.00f);
    s.Colors[ImGuiCol_SliderGrab]    = ImVec4(0.00f,0.70f,1.00f,1.00f);
    s.Colors[ImGuiCol_Button]        = ImVec4(0.00f,0.25f,0.55f,1.00f);
    s.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.00f,0.40f,0.80f,1.00f);
    s.Colors[ImGuiCol_Separator]     = ImVec4(0.20f,0.20f,0.30f,1.00f);

    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    ImVec2 scr       = ImGui::GetIO().DisplaySize;

    if (bAimCircle)
        draw->AddCircle(ImVec2(scr.x*0.5f, scr.y*0.5f), AimFov, IM_COL32(255,255,255,170), 128, 1.6f);

    if ((bEspBox||bEspLine||bEspDist||bEspHp) &&
        Camera_get_main && Component_get_transform &&
        Camera_WorldToScreenPoint && Transform_get_position)
    {
        void* cam = Camera_get_main();
        if (cam) {
            long now = now_ms();
            espMtx.lock();
            for (int i = 0; i < 128; i++) {
                void* entry = espList[i].inst;
                if (!entry) continue;
                if (now - espList[i].ts > 5000) { espList[i].inst = nullptr; continue; }
                void* myTrans = *(void**)((uint8_t*)entry + 0xF0);
                if (!myTrans) continue;
                Vector3 foot = Transform_get_position(myTrans);
                if (foot.X==0.f && foot.Y==0.f && foot.Z==0.f) continue;
                if (foot.Y>400.f || foot.Y<-200.f) continue;
                Vector3 head = foot; head.Y += 1.85f;
                Vector3 sf = Camera_WorldToScreenPoint(cam, foot);
                Vector3 sh = Camera_WorldToScreenPoint(cam, head);
                if (sf.Z<0.2f || sf.Z>600.f) continue;
                float fx=sf.X, fy=scr.y-sf.Y, hy=scr.y-sh.Y;
                float h=fy-hy; if (h<4.f) continue;
                float w=h*0.45f, d=sf.Z;
                ImU32 boxCol = d<20.f?IM_COL32(255,60,60,245):d<50.f?IM_COL32(255,200,0,245):IM_COL32(60,255,120,245);
                if (bEspLine) draw->AddLine(ImVec2(scr.x*0.5f,scr.y),ImVec2(fx,fy),IM_COL32(255,70,70,200),1.3f);
                if (bEspBox) {
                    float lx=fx-w*0.5f, rx=fx+w*0.5f, cw=w*0.22f, ch=h*0.18f;
                    draw->AddRect(ImVec2(lx-1,hy-1),ImVec2(rx+1,fy+1),IM_COL32(0,0,0,160),0,0,2.5f);
                    draw->AddRect(ImVec2(lx,hy),ImVec2(rx,fy),boxCol,0,0,1.4f);
                    draw->AddLine(ImVec2(lx,hy),ImVec2(lx+cw,hy),IM_COL32(255,255,255,220),2.f);
                    draw->AddLine(ImVec2(lx,hy),ImVec2(lx,hy+ch),IM_COL32(255,255,255,220),2.f);
                    draw->AddLine(ImVec2(rx,hy),ImVec2(rx-cw,hy),IM_COL32(255,255,255,220),2.f);
                    draw->AddLine(ImVec2(rx,hy),ImVec2(rx,hy+ch),IM_COL32(255,255,255,220),2.f);
                    draw->AddLine(ImVec2(lx,fy),ImVec2(lx+cw,fy),IM_COL32(255,255,255,220),2.f);
                    draw->AddLine(ImVec2(lx,fy),ImVec2(lx,fy-ch),IM_COL32(255,255,255,220),2.f);
                    draw->AddLine(ImVec2(rx,fy),ImVec2(rx-cw,fy),IM_COL32(255,255,255,220),2.f);
                    draw->AddLine(ImVec2(rx,fy),ImVec2(rx,fy-ch),IM_COL32(255,255,255,220),2.f);
                }
                if (bEspDist) { char buf[24]; snprintf(buf,sizeof(buf),"%.0fm",d); draw->AddText(ImVec2(fx-10.f,hy-16.f),IM_COL32(255,220,0,255),buf); }
                if (bEspHp && espList[i].maxHp>0.f) {
                    float ratio = espList[i].hp/espList[i].maxHp;
                    if (ratio<0.f) ratio=0.f; if (ratio>1.f) ratio=1.f;
                    float bx = fx+w*0.5f+4.f;
                    draw->AddRectFilled(ImVec2(bx-1,hy-1),ImVec2(bx+5,fy+1),IM_COL32(0,0,0,180));
                    ImU32 hpCol = ratio>0.6f?IM_COL32(0,255,80,255):ratio>0.3f?IM_COL32(255,200,0,255):IM_COL32(255,0,0,255);
                    float top = hy+h*(1.f-ratio);
                    draw->AddRectFilled(ImVec2(bx,top),ImVec2(bx+4,fy),hpCol);
                    char hpBuf[16]; snprintf(hpBuf,sizeof(hpBuf),"%.0f",espList[i].hp);
                    draw->AddText(ImVec2(bx+6.f,top-2.f),IM_COL32(255,255,255,220),hpBuf);
                }
            }
            espMtx.unlock();
        }
    }

    ImGui::SetNextWindowSize(ImVec2(380, 540), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(10, 10),    ImGuiCond_FirstUseEver);
    ImGui::Begin("  ZOMBIE3D MOD  |  AXIOM DEV  ", nullptr, 0);
    ImGui::TextColored(ImVec4(0.f,0.8f,1.f,1.f), "  AXIOM DEVELOPMENT");
    ImGui::Separator(); ImGui::Spacing();

    if (ImGui::BeginTabBar("tabs")) {

        if (ImGui::BeginTabItem(T("NGUOI CHOI","PLAYER"))) {
            ImGui::Spacing();
            ImGui::Checkbox(T("Bat Tu (God Mode)","God Mode"), &bGodMode); ImGui::Spacing();
            ImGui::Checkbox(T("Mot Phat Diet Tat","One Shot Kill"), &bOneShotKill);
            if (bOneShotKill) { ImGui::SetNextItemWidth(-1); ImGui::SliderFloat(T("Nhan Sat Thuong","Dmg Multi"),&damageMult,10.f,9999.f,"x%.0f"); }
            ImGui::Spacing();
            ImGui::Checkbox(T("Dan Vo Han","Inf Ammo"), &bInfAmmo); ImGui::Spacing();
            ImGui::Checkbox(T("Khong Giat Sung","No Recoil"), &bNoRecoil); ImGui::Spacing();
            ImGui::Checkbox(T("Tang Toc Do Di Chuyen","Speed Hack"), &bSpeedHack);
            if (bSpeedHack) { ImGui::SetNextItemWidth(-1); ImGui::SliderFloat(T("Toc Do","Speed"),&speedMult,1.f,5.f,"x%.1f"); }
            ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
            ImGui::TextColored(ImVec4(1.f,0.6f,0.2f,1.f),
                T("  Luu y: GodMode/OneShot CHI hoat dong o che\n  do Zombie (offline). PVP tinh damage tren\n  server, khong the patch cuc bo duoc.",
                  "  Note: GodMode/OneShot ONLY work in Zombie\n  (offline) mode. PVP calculates damage on\n  server, can't be patched locally."));
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem(T("TAI NGUYEN","RESOURCES"))) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(1.f,0.85f,0.f,1.f), T("  TIEN (Money+Tickets)","  MONEY+TICKETS"));
            ImGui::Checkbox(T("Full Tien [999,999,999]","Full Money"), &bFullMoney);
            ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
            ImGui::TextColored(ImVec4(1.f,0.6f,0.f,1.f), T("  RUONG (Key thuong+vang)","  BOXES (Keys)"));
            ImGui::Checkbox(T("Full Ruong [999,999,999 Key]","Full Boxes"), &bFullBox);
            ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.7f,0.7f,1.f,1.f), T("  VAT PHAM","  ITEMS"));
            ImGui::Checkbox(T("Mo Khoa Tat Ca Sung","Unlock All Weapons"), &bUnlockWeapon); ImGui::Spacing();
            ImGui::Checkbox(T("Bo Quang Cao","No Ads"), &bNoAds);
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem(T("DINH VI","ESP"))) {
            ImGui::Spacing();
            ImGui::Checkbox(T("Hop Zombie/Dich","Enemy Box"),  &bEspBox);  ImGui::Spacing();
            ImGui::Checkbox(T("Duong Den Dich","Enemy Line"),  &bEspLine); ImGui::Spacing();
            ImGui::Checkbox(T("Khoang Cach","Distance"),       &bEspDist); ImGui::Spacing();
            ImGui::Checkbox(T("Thanh Mau HP","HP Bar"),        &bEspHp);   ImGui::Spacing();
            ImGui::Separator(); ImGui::Spacing();
            ImGui::Checkbox(T("Vong Tron Aim","Aim Circle"),   &bAimCircle);
            if (bAimCircle) { ImGui::SetNextItemWidth(-1); ImGui::SliderFloat("FOV",&AimFov,50.f,400.f,"%.0f px"); }
            ImGui::EndTabItem();
        }

        // ── TAB DEBUG — xem log KHONG CAN PC/adb ────────────────
        if (ImGui::BeginTabItem("DEBUG")) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.5f,1.f,1.f,1.f),
                T("Trang thai hook (xanh=OK, do=chua fire):","Hook status (green=OK, red=not fired):"));
            ImGui::Spacing();

            auto StatusLine = [](const char* name, bool fired, int count=-1) {
                ImGui::TextColored(fired?ImVec4(0.3f,1,0.3f,1):ImVec4(1,0.35f,0.35f,1), "%s", fired?"[OK]":"[--]");
                ImGui::SameLine();
                if (count>=0) ImGui::Text("%s (%d)", name, count);
                else ImGui::Text("%s", name);
            };
            StatusLine("ApplyDamage (GodMode/OneShot)", dbgFirstApplyDamage.load(), dbgApplyDamageCount.load());
            StatusLine("CharDmgUpdate (ESP)",           dbgFirstCharDmgUpdate.load(), dbgCharDmgUpdateCount.load());
            StatusLine("ArcGetMoney",                    dbgFirstArcGetMoney.load());
            StatusLine("ArcUseMoney (tru tien)",         dbgFirstArcUseMoney.load(), dbgArcUseMoneyCount.load());
            StatusLine("ArcGetKeys",                     dbgFirstArcGetKeys.load());
            StatusLine("ArcUseKeys (tru key ruong)",     dbgFirstArcUseKeys.load(), dbgArcUseKeysCount.load());
            StatusLine("ArcGetUnlockAllWeapons",         dbgFirstArcUnlockWeapons.load());
            StatusLine("LoadingOnces.Awake",             dbgFirstLoadingAwake.load());
            StatusLine("LoadingOnces.Instance (static)", dbgLoadingInstFoundViaStatic.load());

            ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
            ImGui::Text(T("Log gan day:","Recent log:"));
            ImGui::BeginChild("logscroll", ImVec2(0, 220), true);
            g_dbgLog.mtx.lock();
            int n = g_dbgLog.count;
            int start = (g_dbgLog.head - n + DBG_LOG_LINES) % DBG_LOG_LINES;
            for (int i = 0; i < n; i++) {
                int idx = (start + i) % DBG_LOG_LINES;
                ImGui::TextWrapped("%s", g_dbgLog.lines[idx]);
            }
            g_dbgLog.mtx.unlock();
            if (n > 0) ImGui::SetScrollHereY(1.0f);
            ImGui::EndChild();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem(T("NGON NGU","LANG"))) {
            ImGui::Spacing();
            if (ImGui::RadioButton("Tieng Viet", lang_idx==0)) lang_idx=0;
            if (ImGui::RadioButton("English",    lang_idx==1)) lang_idx=1;
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }
    ImGui::End();
}

// ================================================================
//  MAIN THREAD
// ================================================================
void* thread(void*) {
    initModMenu((void*)DrawMenu);

    do { sleep(1); } while (getAbsoluteAddress("libil2cpp.so", 0) == 0);
    DbgLog("[+] libil2cpp detected, waiting 3s...");
    sleep(3);

    Camera_get_main = (void*(*)())(void*)getAbsoluteAddress("libil2cpp.so", 0x251B400);
    Camera_WorldToScreenPoint = (Vector3(*)(void*,Vector3))(void*)getAbsoluteAddress("libil2cpp.so", 0x251B1DC);
    Component_get_transform = (void*(*)(void*))(void*)getAbsoluteAddress("libil2cpp.so", 0x253EF08);
    Transform_get_position = (Vector3(*)(void*))(void*)getAbsoluteAddress("libil2cpp.so", 0x254B0DC);

    #define HOOK(rva, hk, orig) do { \
        void* _addr = (void*)getAbsoluteAddress("libil2cpp.so", rva); \
        int _r = DobbyHook(_addr, (void*)hk, (void**)&orig); \
        DbgLog("[HOOK] %s RVA=0x%lx ret=%d", #hk, (uintptr_t)rva, _r); \
    } while(0)

    HOOK(0x119235C, hook_ApplyDamage,   old_ApplyDamage);
    HOOK(0x1191E10, hook_CharDmgUpdate, old_CharDmgUpdate);
    HOOK(0x1309740, hook_WeapUpdate,    old_WeapUpdate);
    HOOK(0x130E3BC, hook_WeaponKick,    old_WeaponKick);
    HOOK(0x12EC900, hook_FPSFixedUpdate, old_FPSFixedUpdate);

    HOOK(0x1323878, hook_ArcGetMoney,          old_ArcGetMoney);
    HOOK(0x13238C0, hook_ArcUseMoney,          old_ArcUseMoney);
    HOOK(0x13239D0, hook_ArcGetTickets,        old_ArcGetTickets);
    HOOK(0x1323A18, hook_ArcUseTickets,        old_ArcUseTickets);
    HOOK(0x1323B08, hook_ArcGetKeys,           old_ArcGetKeys);
    HOOK(0x1323B50, hook_ArcUseKeys,           old_ArcUseKeys);
    HOOK(0x1323D74, hook_ArcGetSpecialKeys,    old_ArcGetSpecialKeys);
    HOOK(0x1323DBC, hook_ArcUseSpecialKeys,    old_ArcUseSpecialKeys);
    HOOK(0x1324784, hook_ArcGetUnlockAllWeapons, old_ArcGetUnlockAllWeapons);

    HOOK(0x12BF2FC, hook_GetCurrency, old_GetCurrency);
    HOOK(0x12BF350, hook_SetCurrency, old_SetCurrency);

    HOOK(0x12D711C, hook_LoadingAwake, old_LoadingAwake);

    #undef HOOK

    DbgLog("[+] ALL HOOKS DONE — AXIOM DEV");
    pthread_exit(0);
}

// ================================================================
//  JNI INIT
// ================================================================
extern "C" {
    JavaVM* jvm = nullptr;
    JNIEnv* env = nullptr;
    __attribute__((visibility("default")))
    jint loadJNI(JavaVM* vm) {
        jvm = vm;
        vm->AttachCurrentThread(&env, nullptr);
        return JNI_VERSION_1_6;
    }
}

__attribute__((constructor))
void init() {
    pthread_t t;
    pthread_create(&t, nullptr, thread, nullptr);
    RemapTools::RemapLibrary("libLoader.so");
}

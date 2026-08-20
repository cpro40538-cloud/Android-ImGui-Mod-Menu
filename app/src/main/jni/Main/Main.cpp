#include "../Include/KittyMemory/MemoryPatch.h"
#include "../Include/ImGui.h"
#include "../Include/RemapTools.h"
#include "../Include/Drawing.h"
#include "../Include/Unity.h"
#include <unistd.h>
#include <dobby.h>
#include <dlfcn.h>
#include <pthread.h>
#include <cstdio>
#include <cstring>
#include <android/log.h>

#define LOG_TAG "THROWIO_AXIOM"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ================================================================
//  TOGGLES & BIEN TOAN CUC
// ================================================================
bool bInfiniteSoft    = false;
bool bInfiniteHard    = false;
bool bInfiniteEnergy  = false;
bool bAutoMaxLevel    = false;
bool bGodMode         = false;
bool bSpeedHack       = false;
bool bNoDamage        = false;
bool bNoAds           = false;
bool bVipActive       = false;
bool bBypassAntiCheat = true;
float speedFactor     = 3.0f;
int targetLevel       = 99;

void* g_BalanceInstance_New = nullptr;
void* g_BalanceInstance_Old = nullptr;
void* g_PlayerDataInstance  = nullptr;

// ================================================================
//  DEBUG STATE
// ================================================================
static int   g_hookOK   = 0;
static int   g_hookFAIL = 0;
static char  g_lastFail[128] = "chua co";
static void* g_il2cppBase = nullptr;
static bool  g_threadStarted = false;
static bool  g_hooksInstalled = false;

static int g_callCount_SoftMoney_New = 0;
static int g_callCount_SoftMoney_Old = 0;
static int g_callCount_AddMoney      = 0;
static int g_callCount_SpeedFactor   = 0;   // [NEW] hàm chạy liên tục mỗi frame

static char g_bytesDump[8][96];
static int  g_bytesDumpCount = 0;

// ================================================================
//  BYTE SANITY CHECK
// ================================================================
static void DumpBytesAtAddr(const char* label, void* addr) {
    if (g_bytesDumpCount >= 8) return;

    if (!addr) {
        snprintf(g_bytesDump[g_bytesDumpCount++], 96, "%s: addr=NULL", label);
        return;
    }
    unsigned char* p = (unsigned char*)addr;
    snprintf(g_bytesDump[g_bytesDumpCount++], 96,
             "%s: %02X %02X %02X %02X %02X %02X %02X %02X",
             label, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
    LOGI("[BYTES] %s @ %p: %02X %02X %02X %02X %02X %02X %02X %02X",
         label, addr, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
}

// ================================================================
//  OFFSETS
// ================================================================
namespace Offsets {
    constexpr uintptr_t set_SoftMoney_New  = 0x1315B48;
    constexpr uintptr_t set_HardMoney_New  = 0x1315B58;
    constexpr uintptr_t set_Level_New      = 0x1315B68;
    constexpr uintptr_t set_Exp_New        = 0x1315B78;
    constexpr uintptr_t set_Energy_New     = 0x1313408;
    constexpr uintptr_t set_NoAds_New      = 0x130B728;
    constexpr uintptr_t set_VipActive_New  = 0x130B874;
    constexpr uintptr_t set_SoftMoney_Old  = 0x1315CFC;
    constexpr uintptr_t set_HardMoney_Old  = 0x1315D7C;
    constexpr uintptr_t set_Level_Old      = 0x1315DFC;
    constexpr uintptr_t set_Energy_Old     = 0x1315EFC;
    constexpr uintptr_t set_NoAds_Old      = 0x1315C0C;
    constexpr uintptr_t set_VipActive_Old  = 0x1315F5C;
    constexpr uintptr_t set_undead         = 0x12FA72C;
    constexpr uintptr_t SetMoveSpeedFactor = 0x12FB514;
    constexpr uintptr_t ApplyDamage        = 0x12FB55C;
    constexpr uintptr_t AddMoney           = 0x130B2C0;
}

// ================================================================
//  POINTERS
// ================================================================
using fn_void_int64  = void (*)(void*, int64_t);
using fn_void_int    = void (*)(void*, int);
using fn_void_bool   = void (*)(void*, bool);
using fn_void_float  = void (*)(void*, float);
using fn_void_damage = void (*)(void*, int64_t, void*, bool, bool, bool, int);
using fn_void_addmoney = void (*)(void*, int, int64_t, void*, void*);

fn_void_int64  old_set_SoftMoney_New  = nullptr;
fn_void_int64  old_set_HardMoney_New  = nullptr;
fn_void_int    old_set_Level_New      = nullptr;
fn_void_int    old_set_Exp_New        = nullptr;
fn_void_int64  old_set_Energy_New     = nullptr;
fn_void_bool   old_set_NoAds_New      = nullptr;
fn_void_bool   old_set_VipActive_New  = nullptr;
fn_void_int64  old_set_SoftMoney_Old  = nullptr;
fn_void_int64  old_set_HardMoney_Old  = nullptr;
fn_void_int    old_set_Level_Old      = nullptr;
fn_void_int64  old_set_Energy_Old     = nullptr;
fn_void_bool   old_set_NoAds_Old      = nullptr;
fn_void_bool   old_set_VipActive_Old  = nullptr;
fn_void_bool   old_set_undead         = nullptr;
fn_void_float  old_SetMoveSpeedFactor = nullptr;
fn_void_damage old_ApplyDamage        = nullptr;
fn_void_addmoney old_AddMoney         = nullptr;

// ================================================================
//  HOOK IMPLEMENTATIONS
// ================================================================
void hk_set_SoftMoney_New(void* self, int64_t v) {
    if (self) g_BalanceInstance_New = self;
    g_callCount_SoftMoney_New++;
    if (bInfiniteSoft) v = 999999999LL;
    if (old_set_SoftMoney_New) old_set_SoftMoney_New(self, v);
}
void hk_set_SoftMoney_Old(void* self, int64_t v) {
    if (self) g_BalanceInstance_Old = self;
    g_callCount_SoftMoney_Old++;
    if (bInfiniteSoft) v = 999999999LL;
    if (old_set_SoftMoney_Old) old_set_SoftMoney_Old(self, v);
}
void hk_set_HardMoney_New(void* self, int64_t v) { if (self) g_BalanceInstance_New = self; if (bInfiniteHard) v = 999999999LL; if (old_set_HardMoney_New) old_set_HardMoney_New(self, v); }
void hk_set_HardMoney_Old(void* self, int64_t v) { if (self) g_BalanceInstance_Old = self; if (bInfiniteHard) v = 999999999LL; if (old_set_HardMoney_Old) old_set_HardMoney_Old(self, v); }
void hk_set_Level_New(void* self, int v) { if (self) g_BalanceInstance_New = self; if (bAutoMaxLevel) v = targetLevel; if (old_set_Level_New) old_set_Level_New(self, v); }
void hk_set_Level_Old(void* self, int v) { if (self) g_BalanceInstance_Old = self; if (bAutoMaxLevel) v = targetLevel; if (old_set_Level_Old) old_set_Level_Old(self, v); }
void hk_set_Exp_New(void* self, int v) { if (self) g_BalanceInstance_New = self; if (bAutoMaxLevel) v = 0x7FFFFFFF; if (old_set_Exp_New) old_set_Exp_New(self, v); }
void hk_set_Energy_New(void* self, int64_t v) { if (self) g_BalanceInstance_New = self; if (bInfiniteEnergy) v = 9999LL; if (old_set_Energy_New) old_set_Energy_New(self, v); }
void hk_set_Energy_Old(void* self, int64_t v) { if (self) g_BalanceInstance_Old = self; if (bInfiniteEnergy) v = 9999LL; if (old_set_Energy_Old) old_set_Energy_Old(self, v); }
void hk_set_NoAds_New(void* self, bool v) { if (self) g_BalanceInstance_New = self; if (bNoAds) v = true; if (old_set_NoAds_New) old_set_NoAds_New(self, v); }
void hk_set_NoAds_Old(void* self, bool v) { if (self) g_BalanceInstance_Old = self; if (bNoAds) v = true; if (old_set_NoAds_Old) old_set_NoAds_Old(self, v); }
void hk_set_VipActive_New(void* self, bool v) { if (self) g_BalanceInstance_New = self; if (bVipActive) v = true; if (old_set_VipActive_New) old_set_VipActive_New(self, v); }
void hk_set_VipActive_Old(void* self, bool v) { if (self) g_BalanceInstance_Old = self; if (bVipActive) v = true; if (old_set_VipActive_Old) old_set_VipActive_Old(self, v); }
void hk_set_undead(void* self, bool v) { if (bGodMode) v = true; if (old_set_undead) old_set_undead(self, v); }

// [FIX] Thêm counter — hàm này PHẢI chạy liên tục khi di chuyển
void hk_SetMoveSpeedFactor(void* self, float factor) {
    g_callCount_SpeedFactor++;
    if (bSpeedHack) factor *= speedFactor;
    if (old_SetMoveSpeedFactor) old_SetMoveSpeedFactor(self, factor);
}

void hk_ApplyDamage(void* self, int64_t dmg, void* from, bool isCrit,
                     bool isPoison, bool isCandyPoison, int src) {
    if (bNoDamage) return;
    if (old_ApplyDamage)
        old_ApplyDamage(self, dmg, from, isCrit, isPoison, isCandyPoison, src);
}

void hk_AddMoney(void* self, int type, int64_t number, void* source, void* item) {
    if (self) g_PlayerDataInstance = self;
    g_callCount_AddMoney++;
    if (old_AddMoney) old_AddMoney(self, type, number, source, item);
}

// ================================================================
//  FORCE APPLY
// ================================================================
void ForceApplyToggles() {
    void* inst = g_BalanceInstance_New ? g_BalanceInstance_New : g_BalanceInstance_Old;
    if (!inst) return;

    bool useNew = (g_BalanceInstance_New != nullptr);

    if (bInfiniteSoft) {
        if (useNew && old_set_SoftMoney_New) old_set_SoftMoney_New(inst, 999999999LL);
        else if (old_set_SoftMoney_Old) old_set_SoftMoney_Old(inst, 999999999LL);
    }
    if (bInfiniteHard) {
        if (useNew && old_set_HardMoney_New) old_set_HardMoney_New(inst, 999999999LL);
        else if (old_set_HardMoney_Old) old_set_HardMoney_Old(inst, 999999999LL);
    }
    if (bInfiniteEnergy) {
        if (useNew && old_set_Energy_New) old_set_Energy_New(inst, 9999LL);
        else if (old_set_Energy_Old) old_set_Energy_Old(inst, 9999LL);
    }
    if (bAutoMaxLevel) {
        if (useNew && old_set_Level_New) old_set_Level_New(inst, targetLevel);
        else if (old_set_Level_Old) old_set_Level_Old(inst, targetLevel);
        if (useNew && old_set_Exp_New) old_set_Exp_New(inst, 0x7FFFFFFF);
    }
    if (bNoAds) {
        if (useNew && old_set_NoAds_New) old_set_NoAds_New(inst, true);
        else if (old_set_NoAds_Old) old_set_NoAds_Old(inst, true);
    }
    if (bVipActive) {
        if (useNew && old_set_VipActive_New) old_set_VipActive_New(inst, true);
        else if (old_set_VipActive_Old) old_set_VipActive_Old(inst, true);
    }
}

// ================================================================
//  VE GIAO DIEN
// ================================================================
void DrawMenu() {
    ForceApplyToggles();

    static bool bStyleInit = false;
    if (!bStyleInit) {
        ImGuiStyle& st = ImGui::GetStyle();
        st.WindowRounding = 10.0f;
        st.FrameRounding = 5.0f;
        st.Colors[ImGuiCol_WindowBg] = ImVec4(0.04f, 0.04f, 0.08f, 0.97f);
        st.Colors[ImGuiCol_TitleBg] = ImVec4(0.00f, 0.12f, 0.28f, 1.00f);
        st.Colors[ImGuiCol_CheckMark] = ImVec4(0.00f, 0.90f, 1.00f, 1.00f);
        bStyleInit = true;
    }

    ImGui::SetNextWindowSize(ImVec2(460, 650), ImGuiCond_FirstUseEver);
    ImGui::Begin("  THROW.IO  |  AXIOM MOD  ");

    void* inst = g_BalanceInstance_New ? g_BalanceInstance_New : g_BalanceInstance_Old;

    // ── DEBUG PANEL ─────────────────────────────────────────────
    ImGui::TextColored(ImVec4(1,1,0,1), "-- DEBUG --");
    ImGui::Text("il2cpp base: %p", g_il2cppBase);
    ImGui::TextColored(g_hooksInstalled ? ImVec4(0,1,0,1) : ImVec4(1,0.5f,0,1),
                       "Hooks installed: %s", g_hooksInstalled ? "YES" : "WAITING");
    ImGui::Text("Hook OK=%d FAIL=%d", g_hookOK, g_hookFAIL);

    // [NEW] Highlight riêng SpeedFactor — đây là phép test quyết định
    ImGui::Separator();
    ImGui::TextColored(ImVec4(1,0.8f,0,1), ">> TEST QUYET DINH <<");
    ImGui::TextColored(
        g_callCount_SpeedFactor > 0 ? ImVec4(0,1,0,1) : ImVec4(1,0.3f,0.3f,1),
        "SpeedFactor calls: %d  (di chuyen nhan vat de test)",
        g_callCount_SpeedFactor
    );
    ImGui::Separator();

    ImGui::Text("CallCount: SoftNew=%d SoftOld=%d AddMoney=%d",
                g_callCount_SoftMoney_New, g_callCount_SoftMoney_Old, g_callCount_AddMoney);
    ImGui::TextColored(
        inst ? ImVec4(0,1,0,1) : ImVec4(1,0.3f,0.3f,1),
        inst ? "Instance: OK" : "Instance: CHUA CO"
    );
    ImGui::Separator();

    ImGui::TextColored(ImVec4(0,0.9f,1,1), "-- RAW BYTES @ PATCH ADDR --");
    for (int i = 0; i < g_bytesDumpCount; i++) {
        ImGui::TextWrapped("%s", g_bytesDump[i]);
    }
    ImGui::Separator();
    // ─────────────────────────────────────────────────────────────

    if (ImGui::BeginTabBar("MainTabs")) {
        if (ImGui::BeginTabItem(" TIEN TE ")) {
            ImGui::Checkbox("Vo Han Tien Mem", &bInfiniteSoft);
            ImGui::Checkbox("Vo Han Tien Cung", &bInfiniteHard);
            ImGui::Checkbox("Vo Han Nang Luong", &bInfiniteEnergy);
            ImGui::Checkbox("Bo Quang Cao", &bNoAds);
            ImGui::Checkbox("Kich Hoat VIP", &bVipActive);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(" CHIEN DAU ")) {
            ImGui::Checkbox("God Mode - Bat Tu", &bGodMode);
            ImGui::Checkbox("Khong Nhan Sat Thuong", &bNoDamage);
            ImGui::Checkbox("Tang Toc Do Di Chuyen", &bSpeedHack);
            if (bSpeedHack) ImGui::SliderFloat("He So", &speedFactor, 1.5f, 10.0f);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

// ================================================================
//  LUONG CHINH
// ================================================================
void* thread(void*) {
    initModMenu((void*)DrawMenu);
    g_threadStarted = true;

    do { sleep(1); } while (getAbsoluteAddress("libil2cpp.so", 0) == 0);

    g_il2cppBase = (void*)getAbsoluteAddress("libil2cpp.so", 0);
    LOGI("[+] libil2cpp base=%p", g_il2cppBase);
    sleep(3);

    #define HOOK(off, hk, orig) \
        do { \
            void* addr = (void*)getAbsoluteAddress("libil2cpp.so", off); \
            if (!addr) { \
                g_hookFAIL++; \
                snprintf(g_lastFail, sizeof(g_lastFail), "%s addrNULL@0x%lX", #hk, (unsigned long)off); \
                break; \
            } \
            auto ret = DobbyHook(addr, (void*)hk, (void**)&orig); \
            if (ret == 0) g_hookOK++; \
            else { \
                g_hookFAIL++; \
                snprintf(g_lastFail, sizeof(g_lastFail), "%s DobbyFail ret=%d", #hk, (int)ret); \
            } \
        } while (0)

    DumpBytesAtAddr("AddMoney", (void*)getAbsoluteAddress("libil2cpp.so", Offsets::AddMoney));
    DumpBytesAtAddr("SoftMoney_New", (void*)getAbsoluteAddress("libil2cpp.so", Offsets::set_SoftMoney_New));
    DumpBytesAtAddr("SoftMoney_Old", (void*)getAbsoluteAddress("libil2cpp.so", Offsets::set_SoftMoney_Old));
    DumpBytesAtAddr("SetUndead", (void*)getAbsoluteAddress("libil2cpp.so", Offsets::set_undead));

    HOOK(Offsets::set_SoftMoney_New,  hk_set_SoftMoney_New,  old_set_SoftMoney_New);
    HOOK(Offsets::set_HardMoney_New,  hk_set_HardMoney_New,  old_set_HardMoney_New);
    HOOK(Offsets::set_Level_New,      hk_set_Level_New,      old_set_Level_New);
    HOOK(Offsets::set_Exp_New,        hk_set_Exp_New,        old_set_Exp_New);
    HOOK(Offsets::set_Energy_New,     hk_set_Energy_New,     old_set_Energy_New);
    HOOK(Offsets::set_NoAds_New,      hk_set_NoAds_New,      old_set_NoAds_New);
    HOOK(Offsets::set_VipActive_New,  hk_set_VipActive_New,  old_set_VipActive_New);
    HOOK(Offsets::set_SoftMoney_Old,  hk_set_SoftMoney_Old,  old_set_SoftMoney_Old);
    HOOK(Offsets::set_HardMoney_Old,  hk_set_HardMoney_Old,  old_set_HardMoney_Old);
    HOOK(Offsets::set_Level_Old,      hk_set_Level_Old,      old_set_Level_Old);
    HOOK(Offsets::set_Energy_Old,     hk_set_Energy_Old,     old_set_Energy_Old);
    HOOK(Offsets::set_NoAds_Old,      hk_set_NoAds_Old,      old_set_NoAds_Old);
    HOOK(Offsets::set_VipActive_Old,  hk_set_VipActive_Old,  old_set_VipActive_Old);
    HOOK(Offsets::set_undead,         hk_set_undead,         old_set_undead);
    HOOK(Offsets::SetMoveSpeedFactor, hk_SetMoveSpeedFactor, old_SetMoveSpeedFactor);
    HOOK(Offsets::ApplyDamage,        hk_ApplyDamage,        old_ApplyDamage);
    HOOK(Offsets::AddMoney,           hk_AddMoney,           old_AddMoney);

    #undef HOOK

    g_hooksInstalled = true;
    LOGI("[+] ALL HOOKS DONE! OK=%d FAIL=%d", g_hookOK, g_hookFAIL);
    pthread_exit(0);
}

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

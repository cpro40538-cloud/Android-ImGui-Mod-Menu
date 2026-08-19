#include "../Include/KittyMemory/MemoryPatch.h"
#include "../Include/ImGui.h"
#include "../Include/RemapTools.h"
#include "../Include/Drawing.h"
#include "../Include/Unity.h"
#include <unistd.h>
#include <dobby.h>
#include <dlfcn.h>
#include <pthread.h>
#include <android/log.h>
#include <cstdio>
#include <cstring>

#define LOG_TAG "THROWIO_AXIOM"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ================================================================
//  TOGGLES & BIẾN TOÀN CỤC
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

uintptr_t g_libBase = 0;

// ================================================================
//  UTIL: Lấy base address libil2cpp.so
// ================================================================
uintptr_t getLibBase(const char* name) {
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return 0;
    char line[512];
    uintptr_t base = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, name) && strstr(line, "r-xp")) {
            base = (uintptr_t)strtoul(line, nullptr, 16);
            break;
        }
    }
    fclose(f);
    return base;
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
}

// ================================================================
//  POINTERS
// ================================================================
using fn_void_int64       = void (*)(void*, int64_t);
using fn_void_int         = void (*)(void*, int);
using fn_void_bool        = void (*)(void*, bool);
using fn_void_float       = void (*)(void*, float);
using fn_bool_damage      = bool (*)(void*, int64_t, void*, bool, bool, int);

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
fn_bool_damage old_ApplyDamage        = nullptr;

// ================================================================
//  HOOK IMPLEMENTATIONS
// ================================================================
void hk_set_SoftMoney_New(void* self, int64_t v) { if (bInfiniteSoft) v = 999999999LL; if (old_set_SoftMoney_New) old_set_SoftMoney_New(self, v); }
void hk_set_SoftMoney_Old(void* self, int64_t v) { if (bInfiniteSoft) v = 999999999LL; if (old_set_SoftMoney_Old) old_set_SoftMoney_Old(self, v); }
void hk_set_HardMoney_New(void* self, int64_t v) { if (bInfiniteHard) v = 999999999LL; if (old_set_HardMoney_New) old_set_HardMoney_New(self, v); }
void hk_set_HardMoney_Old(void* self, int64_t v) { if (bInfiniteHard) v = 999999999LL; if (old_set_HardMoney_Old) old_set_HardMoney_Old(self, v); }
void hk_set_Level_New(void* self, int v) { if (bAutoMaxLevel) v = targetLevel; if (old_set_Level_New) old_set_Level_New(self, v); }
void hk_set_Level_Old(void* self, int v) { if (bAutoMaxLevel) v = targetLevel; if (old_set_Level_Old) old_set_Level_Old(self, v); }
void hk_set_Exp_New(void* self, int v) { if (bAutoMaxLevel) v = 0x7FFFFFFF; if (old_set_Exp_New) old_set_Exp_New(self, v); }
void hk_set_Energy_New(void* self, int64_t v) { if (bInfiniteEnergy) v = 9999LL; if (old_set_Energy_New) old_set_Energy_New(self, v); }
void hk_set_Energy_Old(void* self, int64_t v) { if (bInfiniteEnergy) v = 9999LL; if (old_set_Energy_Old) old_set_Energy_Old(self, v); }
void hk_set_NoAds_New(void* self, bool v) { if (bNoAds) v = true; if (old_set_NoAds_New) old_set_NoAds_New(self, v); }
void hk_set_NoAds_Old(void* self, bool v) { if (bNoAds) v = true; if (old_set_NoAds_Old) old_set_NoAds_Old(self, v); }
void hk_set_VipActive_New(void* self, bool v) { if (bVipActive) v = true; if (old_set_VipActive_New) old_set_VipActive_New(self, v); }
void hk_set_VipActive_Old(void* self, bool v) { if (bVipActive) v = true; if (old_set_VipActive_Old) old_set_VipActive_Old(self, v); }
void hk_set_undead(void* self, bool v) { if (bGodMode) v = true; if (old_set_undead) old_set_undead(self, v); }
void hk_SetMoveSpeedFactor(void* self, float factor) { if (bSpeedHack) factor *= speedFactor; if (old_SetMoveSpeedFactor) old_SetMoveSpeedFactor(self, factor); }
bool hk_ApplyDamage(void* self, int64_t dmg, void* from, bool isCrit, bool isPoison, int src) {
    if (bNoDamage) return false;
    return old_ApplyDamage ? old_ApplyDamage(self, dmg, from, isCrit, isPoison, src) : false;
}

// ================================================================
//  INSTALL HOOKS
// ================================================================
void installHooks() {
    g_libBase = getLibBase("libil2cpp.so");
    if (!g_libBase) {
        LOGE("[-] libil2cpp.so NOT FOUND!");
        return;
    }
    LOGI("[+] libil2cpp base: 0x%lx", g_libBase);

    #define HOOK(off, hk, orig) \
        DobbyHook(reinterpret_cast<void*>(g_libBase + off), \
                  reinterpret_cast<void*>(hk), \
                  reinterpret_cast<void**>(&orig));

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

    #undef HOOK
    LOGI("[+] ALL HOOKS DONE!");
}

// ================================================================
//  VẼ GIAO DIỆN (IMGUI MENU)
// ================================================================
void DrawMenu() {
    // Đã XÓA dòng ImGui::ShowDemoWindow() và chèn UI của bạn vào
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

    ImGui::SetNextWindowSize(ImVec2(390, 450), ImGuiCond_FirstUseEver);
    ImGui::Begin("  THROW.IO  |  AXIOM MOD  ");

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
//  LUỒNG CHÍNH (MAIN THREAD)
// ================================================================
void *thread(void *) {
    LOGI(OBFUSCATE("Main Thread Loaded: %d"), gettid());
    initModMenu((void *)DrawMenu);

    // Vòng lặp chờ game load libil2cpp.so vào bộ nhớ
    while (!getLibBase("libil2cpp.so")) {
        sleep(1);
    }
    LOGI("[+] libil2cpp detected, waiting for unpack...");
    
    sleep(3); // Đợi 3 giây cho game khởi tạo xong hoàn toàn
    installHooks();

    LOGI("Main thread done");
    pthread_exit(0);
}

// ================================================================
//  KHỞI TẠO JNI & THƯ VIỆN
// ================================================================
extern "C" {
    JavaVM *jvm = nullptr;
    JNIEnv *env = nullptr;

    // ĐÃ FIX: Sửa thành JNI_OnLoad chuẩn của Android thay vì loadJNI
    __attribute__((visibility ("default")))
    jint JNI_OnLoad(JavaVM *vm, void* reserved) {
        jvm = vm;
        vm->AttachCurrentThread(&env, nullptr);
        LOGI("JNI_OnLoad(): Initialized");
        return JNI_VERSION_1_6;
    }
}

__attribute__((constructor))
void init() {
    LOGI("Loaded Mod Menu");

    pthread_t t;
    pthread_create(&t, nullptr, thread, nullptr);

    // Ẩn module khỏi bộ nhớ
    RemapTools::RemapLibrary("libLoader.so");
}

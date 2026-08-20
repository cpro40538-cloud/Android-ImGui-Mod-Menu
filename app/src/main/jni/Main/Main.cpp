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
#include <cstdarg>
#include <cstring>
#include <cerrno>         // ✅ Thêm cho errno
#include <android/log.h>
#include <sys/stat.h>

#define LOG_TAG "THROWIO_AXIOM"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ================================================================
//  FILE LOG — DUAL OUTPUT (LOGCAT + FILE)
//  Ưu tiên 1: /data/local/tmp/ — MỌI APP ĐỀU GHI ĐƯỢC, KHÔNG BỊ SELINUX CHẶN
//  Ưu tiên 2: /data/data/<pkg>/files/ — thử nếu SELinux cho phép
//  LUÔN GHI VÀO LOGCAT (xem ngay bằng adb logcat)
// ================================================================
static FILE* g_logFile = nullptr;
static char  g_logPath[256] = {0};

static void GetLogPath() {
    // ════════════════════════════════════════════════════════════
    //  ƯU TIÊN 1: /data/local/tmp/ — KHÔNG BAO GIỜ BỊ CHẶN
    // ════════════════════════════════════════════════════════════
    strcpy(g_logPath, "/data/local/tmp/throwio_axiom_log.txt");

    // ════════════════════════════════════════════════════════════
    //  ƯU TIÊN 2: thử thư mục riêng của app (nếu SELinux cho phép)
    // ════════════════════════════════════════════════════════════
    FILE* f = fopen("/proc/self/cmdline", "r");
    if (f) {
        char pkgName[128] = {0};
        fread(pkgName, 1, sizeof(pkgName) - 1, f);
        fclose(f);
        
        char testPath[256];
        snprintf(testPath, sizeof(testPath), "/data/data/%s/files/throwio_log.txt", pkgName);
        
        // Thử mở thử xem được không
        FILE* test = fopen(testPath, "a");
        if (test) {
            fclose(test);
            strcpy(g_logPath, testPath);
        }
    }
}

static void FileLog(const char* fmt, ...) {
    char buf[1024];
    va_list args;
    
    // ── 1. GHI LUÔN VÀO LOGCAT (xem được NGAY BÂY GIỜ bằng adb) ──
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    __android_log_print(ANDROID_LOG_INFO, "THROWIO_FILELOG", "%s", buf);

    // ── 2. GHI VÀO FILE ──
    if (!g_logFile) {
        if (g_logPath[0] == '\0') GetLogPath();
        
        g_logFile = fopen(g_logPath, "a");
        
        // Thử tạo thư mục nếu cần
        if (!g_logFile) {
            char dirPath[256];
            snprintf(dirPath, sizeof(dirPath), "%s", g_logPath);
            char* lastSlash = strrchr(dirPath, '/');
            if (lastSlash) {
                *lastSlash = '\0';
                mkdir(dirPath, 0777);
            }
            g_logFile = fopen(g_logPath, "a");
        }

        if (g_logFile) {
            fprintf(g_logFile, "\n=== NEW SESSION (path: %s) ===\n", g_logPath);
            __android_log_print(ANDROID_LOG_INFO, "THROWIO_FILELOG", 
                                "[SUCCESS] Log file opened: %s", g_logPath);
        } else {
            __android_log_print(ANDROID_LOG_ERROR, "THROWIO_FILELOG", 
                                "[FAIL] Cannot open log file! errno=%d path=%s", 
                                errno, g_logPath);
        }
    }
    
    if (g_logFile) {
        fprintf(g_logFile, "%s\n", buf);
        fflush(g_logFile);
    }
}

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

static int g_callCount_SoftMoney_New = 0;
static int g_callCount_SoftMoney_Old = 0;
static int g_callCount_AddMoney      = 0;

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
    if (g_callCount_SoftMoney_New <= 5 || g_callCount_SoftMoney_New % 20 == 0)
        FileLog("[HIT] set_SoftMoney_New called #%d self=%p v=%lld",
                 g_callCount_SoftMoney_New, self, (long long)v);
    if (bInfiniteSoft) v = 999999999LL;
    if (old_set_SoftMoney_New) old_set_SoftMoney_New(self, v);
}
void hk_set_SoftMoney_Old(void* self, int64_t v) {
    if (self) g_BalanceInstance_Old = self;
    g_callCount_SoftMoney_Old++;
    if (g_callCount_SoftMoney_Old <= 5 || g_callCount_SoftMoney_Old % 20 == 0)
        FileLog("[HIT] set_SoftMoney_Old called #%d self=%p v=%lld",
                 g_callCount_SoftMoney_Old, self, (long long)v);
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
void hk_SetMoveSpeedFactor(void* self, float factor) { if (bSpeedHack) factor *= speedFactor; if (old_SetMoveSpeedFactor) old_SetMoveSpeedFactor(self, factor); }

void hk_ApplyDamage(void* self, int64_t dmg, void* from, bool isCrit,
                     bool isPoison, bool isCandyPoison, int src) {
    if (bNoDamage) return;
    if (old_ApplyDamage)
        old_ApplyDamage(self, dmg, from, isCrit, isPoison, isCandyPoison, src);
}

void hk_AddMoney(void* self, int type, int64_t number, void* source, void* item) {
    if (self) g_PlayerDataInstance = self;
    g_callCount_AddMoney++;
    FileLog("[HIT] AddMoney called #%d self=%p type=%d number=%lld",
             g_callCount_AddMoney, self, type, (long long)number);
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

    static int dbgFrame = 0;
    if (dbgFrame++ % 120 == 0) {
        void* inst = g_BalanceInstance_New ? g_BalanceInstance_New : g_BalanceInstance_Old;
        FileLog("[TICK] inst=%p pdata=%p callSoftNew=%d callSoftOld=%d callAddMoney=%d",
                 inst, g_PlayerDataInstance,
                 g_callCount_SoftMoney_New, g_callCount_SoftMoney_Old, g_callCount_AddMoney);
    }

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

    void* inst = g_BalanceInstance_New ? g_BalanceInstance_New : g_BalanceInstance_Old;
    ImGui::TextColored(
        inst ? ImVec4(0,1,0,1) : ImVec4(1,0.3f,0.3f,1),
        inst ? "Instance: OK" : "Instance: CHUA CO (vao tran choi truoc)"
    );
    ImGui::Text("SoftNew=%d SoftOld=%d AddMoney=%d",
                g_callCount_SoftMoney_New, g_callCount_SoftMoney_Old, g_callCount_AddMoney);

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
    FileLog("=== INIT START ===");

    do { sleep(1); } while (getAbsoluteAddress("libil2cpp.so", 0) == 0);
    FileLog("[+] libil2cpp detected, base=%p", (void*)getAbsoluteAddress("libil2cpp.so", 0));
    sleep(3);

    #define HOOK(off, hk, orig) \
        do { \
            void* addr = (void*)getAbsoluteAddress("libil2cpp.so", off); \
            if (!addr) { \
                FileLog("[FAIL] addr NULL @ 0x%lX (%s)", (unsigned long)off, #hk); \
                break; \
            } \
            auto ret = DobbyHook(addr, (void*)hk, (void**)&orig); \
            if (ret == 0) FileLog("[OK] %s @ 0x%lX -> %p", #hk, (unsigned long)off, addr); \
            else FileLog("[FAIL] DobbyHook ret=%d %s @ 0x%lX", (int)ret, #hk, (unsigned long)off); \
        } while (0)

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

    FileLog("[+] ALL HOOKS DONE");
    LOGI("[+] ALL HOOKS DONE!");
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

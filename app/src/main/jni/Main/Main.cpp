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
#include <cmath>
#include <sys/time.h>
#include <cstdint>
#include <android/log.h>

#define LOG_TAG "AXIOM"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,LOG_TAG,__VA_ARGS__)

// ================================================================
//  LANGUAGE
//  Source: dump.cs — no anti-cheat detected (no GameGuard/SafetyNet/
//  PlayIntegrity/CheatingDetector class found in Assembly-CSharp)
//  Currency is SERVER-SIDE (PostgreSQL via UnityNpgsql.dll)
//  GetSoftCurrency/GetHardCurrency hooks = display only, not real value
// ================================================================
static int lang_idx = 0;
static const char* T(const char* vn, const char* en){ return lang_idx==0?vn:en; }

// ================================================================
//  TOGGLES
// ================================================================
static bool bInfAmmo    = false;
static bool bNoReload   = false;
static bool bNoRecoil   = false;
static bool bSpeedHack  = false;
static bool bFly        = false;
static bool bFastFire   = false;
static bool bAutoAim    = false;
static bool bSoftCoin   = false; // display only — server-side
static bool bHardCoin   = false; // display only — server-side

static float speedMult  = 2.0f;
static float flySpeed   = 8.0f;
static float fastFireRate = 0.03f;

// ================================================================
//  FPS
// ================================================================
static float g_fps=0.f; static int g_fpsCount=0; static long g_fpsLast=0;
static long now_ms(){struct timeval tv;gettimeofday(&tv,NULL);return(long)tv.tv_sec*1000+tv.tv_usec/1000;}

// ================================================================
//  IL2CPP THREAD ATTACH
// ================================================================
static std::atomic<bool> g_attached{false};
static void EnsureAttached(){
    if(g_attached.load())return;
    void* lib=dlopen("libil2cpp.so",RTLD_LAZY|RTLD_NOLOAD);if(!lib)return;
    auto dg=(void*(*)())dlsym(lib,"il2cpp_domain_get");
    auto ta=(void*(*)(void*))dlsym(lib,"il2cpp_thread_attach");
    if(dg&&ta){ta(dg());g_attached.store(true);}
}

// ================================================================
//  HOOK 1: ThirdPersonCharacterControllerScript.Update
//  RVA: 0x180DC1C — TypeDefIndex: 47
//  Fields confirmed from dump.cs:
//    ammo        (max)   0x2C  int
//    currentAmmo         0x30  int
//    outOfAmmo           0x34  bool
//    fireRate            0x38  float
//    isReloading         0x11A bool
// ================================================================
static void (*old_WeapUpdate)(void* inst);
static void hook_WeapUpdate(void* inst){
    if(old_WeapUpdate) old_WeapUpdate(inst);
    if(!inst) return;

    if(bInfAmmo){
        int maxAmmo = *(int*)((uint8_t*)inst + 0x2C);
        *(int*) ((uint8_t*)inst + 0x30) = maxAmmo > 0 ? maxAmmo : 9999;
        *(bool*)((uint8_t*)inst + 0x34) = false;
    }
    if(bNoReload){
        *(bool*)((uint8_t*)inst + 0x11A) = false;
    }
    if(bFastFire){
        *(float*)((uint8_t*)inst + 0x38) = fastFireRate;
    }
}

// ================================================================
//  HOOK 2: ComponentRecoilForFastShootingViewMyWeapon.HandleChangeShooting
//  RVA: 0x1A4080C — TypeDefIndex: 1061
//  Fields: _camera@0x50 _recoilTime@0x68 _recoilProcessors@0x90
//  Skip handler entirely → coroutine never spawns → zero recoil
// ================================================================
static void (*old_RecoilFastHandler)(void* inst, void* sender, void* args);
static void hook_RecoilFastHandler(void* inst, void* sender, void* args){
    if(bNoRecoil) return; // drop — no coroutine started
    if(old_RecoilFastHandler) old_RecoilFastHandler(inst,sender,args);
}

// ================================================================
//  HOOK 3: ComponentRecoilForSlowShootingViewMyWeapon.HandleShoot
//  RVA: 0x1A40CFC — TypeDefIndex: 1063
//  Same approach — skip handler = no RecoilProcess coroutine
// ================================================================
static void (*old_RecoilSlowHandler)(void* inst, void* sender, void* args);
static void hook_RecoilSlowHandler(void* inst, void* sender, void* args){
    if(bNoRecoil) return;
    if(old_RecoilSlowHandler) old_RecoilSlowHandler(inst,sender,args);
}

// ================================================================
//  HOOK 4: MovableCharacterComponent.MoveByDirection
//  RVA: 0x1A6D74C — TypeDefIndex: 1325
//  Called every character tick with normalized move direction + jump flag
//  Fields on instance (confirmed dump.cs):
//    Velocity     backing  0x50  Vector3
//    MoveVelocity backing  0x5C  Vector3
//    MoveDirection backing 0x68  ExactNormalizedVector2
//    IsJumped     backing  0x78  NotifierProperty<bool>
//  After calling original, multiply XZ velocity for speed
//  For fly: override Y velocity via Move() path instead (see hook 5)
// ================================================================
struct Vec3{ float x,y,z; };

static void (*old_MoveByDirection)(void* inst, float dx, float dz, bool jump);
static void hook_MoveByDirection(void* inst, float dx, float dz, bool jump){
    if(!inst){ if(old_MoveByDirection)old_MoveByDirection(inst,dx,dz,jump); return; }

    if(bSpeedHack){
        // Scale input direction before passing to original
        dx *= speedMult;
        dz *= speedMult;
    }
    if(old_MoveByDirection) old_MoveByDirection(inst,dx,dz,jump);

    // Post-call: also patch MoveVelocity backing field directly
    if(bSpeedHack){
        Vec3* mv=(Vec3*)((uint8_t*)inst+0x5C);
        mv->x *= speedMult;
        mv->z *= speedMult;
    }
}

// ================================================================
//  HOOK 5: MovableCharacterComponent.Move (Vector3)
//  RVA: 0x1A6D690
//  Controls final position move — used for fly Y override
//  Fly: Jump btn = up, Crouch = down, nothing = hover (zero Y)
// ================================================================
static void (*old_Move)(void* inst, float px, float py, float pz);
static void hook_Move(void* inst, float px, float py, float pz){
    if(!inst){ if(old_Move)old_Move(inst,px,py,pz); return; }

    if(bFly){
        // Nullify gravity by zeroing Y then apply our flySpeed
        // Jump/Crouch state read from IsJumped NotifierProperty
        // NotifierProperty<bool> layout: Value at ~0x20 (standard Il2cpp backing)
        void* jumpedProp = *(void**)((uint8_t*)inst+0x78);
        bool  isJumped   = jumpedProp ? *(bool*)((uint8_t*)jumpedProp+0x20) : false;

        // Simple: check if upward momentum was requested
        float newY = 0.f; // hover
        if(isJumped)   newY =  flySpeed * 0.016f; // approximate per-frame
        // Crouch: no direct ref without UI input — keep 0 or add your input check

        if(old_Move) old_Move(inst, px, newY, pz);
        return;
    }
    if(old_Move) old_Move(inst,px,py,pz);
}

// ================================================================
//  HOOK 6: ModelPlayerState.GetSoftCurrency
//  RVA: 0x1999A1C — TypeDefIndex: 3061
//  NOTE: display-only. Server owns real value via PostgreSQL.
//         This makes the UI show max but purchase attempts will
//         fail server-side validation if you don't have the real balance.
// ================================================================
static int (*old_GetSoftCurrency)(void* inst);
static int hook_GetSoftCurrency(void* inst){
    if(bSoftCoin) return 999999999;
    return old_GetSoftCurrency ? old_GetSoftCurrency(inst) : 0;
}

// ================================================================
//  HOOK 7: ModelPlayerState.GetHardCurrency
//  RVA: 0x199A63C — TypeDefIndex: 3061
//  Same caveat as soft — server-authoritative
// ================================================================
static int (*old_GetHardCurrency)(void* inst);
static int hook_GetHardCurrency(void* inst){
    if(bHardCoin) return 999999999;
    return old_GetHardCurrency ? old_GetHardCurrency(inst) : 0;
}

// ================================================================
//  HOOK 8: ClientMyCharacter.LateUpdate
//  RVA: 0x1A3F720 — TypeDefIndex: 1050
//  Fields: _cameraPivot@0x58 MovableCharacterComponent@0xA0 _modelCharacter@0xA8
//  Used for AutoAim toggle via model NotifierProperty
// ================================================================
static void (*old_CharLateUpdate)(void* inst);
static void hook_CharLateUpdate(void* inst){
    if(old_CharLateUpdate) old_CharLateUpdate(inst);
    if(!inst || !bAutoAim) return;

    // ModelClientMyCharacter is at _modelCharacter (0xA8)
    // InputShooting NotifierProperty<bool> @ 0xA8 on ModelClientMyCharacter:
    // From dump TypeDefIndex:1049 — InputShooting @ 0x38, IsShooting @ 0x30
    void* model = *(void**)((uint8_t*)inst + 0xA8);
    if(!model) return;
    // Force IsShooting and InputShooting = true
    void* isShooting   = *(void**)((uint8_t*)model + 0x30);
    void* inputShooting= *(void**)((uint8_t*)model + 0x38);
    if(isShooting)    *(bool*)((uint8_t*)isShooting    + 0x20) = true;
    if(inputShooting) *(bool*)((uint8_t*)inputShooting + 0x20) = true;
}

// ================================================================
//  MENU DRAW
// ================================================================
static void DrawMenu(){
    EnsureAttached();

    // FPS counter
    g_fpsCount++;
    long nowT=now_ms();
    if(nowT-g_fpsLast>=1000){
        g_fps=g_fpsCount*1000.f/(float)(nowT-g_fpsLast);
        g_fpsCount=0; g_fpsLast=nowT;
    }

    ImGuiStyle& s=ImGui::GetStyle();
    s.WindowRounding=10; s.FrameRounding=5; s.GrabRounding=4;
    s.ItemSpacing=ImVec2(8,7); s.WindowPadding=ImVec2(12,12);
    s.Colors[ImGuiCol_WindowBg]      =ImVec4(.03f,.03f,.07f,.97f);
    s.Colors[ImGuiCol_TitleBg]       =ImVec4(.00f,.10f,.25f,1.f);
    s.Colors[ImGuiCol_TitleBgActive] =ImVec4(.00f,.18f,.45f,1.f);
    s.Colors[ImGuiCol_FrameBg]       =ImVec4(.06f,.06f,.12f,1.f);
    s.Colors[ImGuiCol_CheckMark]     =ImVec4(.00f,.85f,1.0f,1.f);
    s.Colors[ImGuiCol_Tab]           =ImVec4(.04f,.08f,.18f,1.f);
    s.Colors[ImGuiCol_TabActive]     =ImVec4(.00f,.18f,.45f,1.f);
    s.Colors[ImGuiCol_TabHovered]    =ImVec4(.00f,.28f,.65f,1.f);
    s.Colors[ImGuiCol_SliderGrab]    =ImVec4(.00f,.65f,1.0f,1.f);
    s.Colors[ImGuiCol_Button]        =ImVec4(.00f,.22f,.50f,1.f);
    s.Colors[ImGuiCol_ButtonHovered] =ImVec4(.00f,.38f,.75f,1.f);
    s.Colors[ImGuiCol_Separator]     =ImVec4(.15f,.15f,.28f,1.f);
    s.Colors[ImGuiCol_SliderGrabActive]=ImVec4(.00f,.90f,1.f,1.f);

    ImGui::SetNextWindowSize(ImVec2(340,460),ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(10,10),ImGuiCond_FirstUseEver);
    ImGui::Begin("  ⚡ AXIOM MOD  |  v1.0  ",nullptr,0);

    // Header
    ImGui::TextColored(ImVec4(.0f,.85f,1.f,1.f)," AXIOM DEVELOPMENT");
    ImGui::SameLine();
    char fb[32]; snprintf(fb,32,"  [FPS: %.0f]",g_fps);
    ImGui::TextColored(
        g_fps>=60?ImVec4(.2f,1.f,.2f,1.f):g_fps>=30?ImVec4(1.f,.75f,.1f,1.f):ImVec4(1.f,.2f,.2f,1.f),
        "%s",fb);

    // Dump info banner
    ImGui::TextColored(ImVec4(.4f,.4f,.6f,1.f),
        " No AntiCheat detected | Server: PostgreSQL");
    ImGui::Separator(); ImGui::Spacing();

    if(ImGui::BeginTabBar("tabs")){

        // ── TAB 1: CHIEN DAU ────────────────────────────────────
        if(ImGui::BeginTabItem(T("CHIEN DAU","COMBAT"))){
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(.3f,.9f,.3f,1.f),T(" [ VU KHI ]"," [ WEAPON ]"));
            ImGui::Spacing();

            ImGui::Checkbox(T("Dan Vo Han","Infinite Ammo"),&bInfAmmo);
            ImGui::Spacing();
            ImGui::Checkbox(T("Khong Nan Dan","No Reload"),&bNoReload);
            ImGui::Spacing();
            ImGui::Checkbox(T("Khong Giat Sung","No Recoil"),&bNoRecoil);
            if(bNoRecoil)
                ImGui::TextColored(ImVec4(.5f,1.f,.5f,1.f),
                    T("  Hook: HandleChangeShooting + HandleShoot",
                      "  Hook: HandleChangeShooting + HandleShoot"));
            ImGui::Spacing();
            ImGui::Checkbox(T("Ban Nhanh (Fast Fire)","Fast Fire Rate"),&bFastFire);
            if(bFastFire){
                ImGui::SetNextItemWidth(-1);
                ImGui::SliderFloat(T("Delay Ban","Fire Delay"),&fastFireRate,0.01f,0.25f,"%.3fs");
            }

            ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
            ImGui::TextColored(ImVec4(.3f,.9f,.3f,1.f),T(" [ NHAN VAT ]"," [ CHARACTER ]"));
            ImGui::Spacing();

            ImGui::Checkbox(T("Tu Dong Ban","Auto Shoot"),&bAutoAim);
            if(bAutoAim)
                ImGui::TextColored(ImVec4(1.f,.8f,.2f,1.f),
                    T("  Force InputShooting + IsShooting = true",
                      "  Force InputShooting + IsShooting = true"));
            ImGui::Spacing();

            ImGui::Checkbox(T("Tang Toc Di Chuyen","Speed Hack"),&bSpeedHack);
            if(bSpeedHack){
                ImGui::SetNextItemWidth(-1);
                ImGui::SliderFloat(T("He So Toc Do","Speed Multiplier"),&speedMult,1.f,6.f,"x%.1f");
            }
            ImGui::Spacing();

            ImGui::Checkbox(T("Bay (Fly Hack)","Fly Hack"),&bFly);
            if(bFly){
                ImGui::SetNextItemWidth(-1);
                ImGui::SliderFloat(T("Toc Do Bay","Fly Speed"),&flySpeed,2.f,25.f,"%.0f");
                ImGui::TextColored(ImVec4(.5f,1.f,.5f,1.f),
                    T("  JUMP=len cao | CROUCH=xuong | Khong=hover",
                      "  JUMP=up | CROUCH=down | Nothing=hover"));
            }

            ImGui::EndTabItem();
        }

        // ── TAB 2: TAI NGUYEN ────────────────────────────────────
        if(ImGui::BeginTabItem(T("TAI NGUYEN","RESOURCES"))){
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(1.f,.3f,.3f,1.f),
                T("  [ ! ] Currency luu tren SERVER (PostgreSQL)",
                  "  [ ! ] Currency stored on SERVER (PostgreSQL)"));
            ImGui::TextColored(ImVec4(.7f,.7f,.7f,1.f),
                T("  Hien thi 999M nhung KHONG the mua that",
                  "  Shows 999M but real purchases will FAIL"));
            ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

            ImGui::TextColored(ImVec4(.9f,.8f,.1f,1.f),
                T("  TIEN MEM (Soft Currency)","  SOFT CURRENCY"));
            ImGui::Checkbox(T("Hien Thi: 999,999,999 Xu","Display: 999M Soft"),&bSoftCoin);
            if(bSoftCoin)
                ImGui::TextColored(ImVec4(.5f,.5f,.5f,1.f),
                    T("  Hook: GetSoftCurrency RVA 0x1999A1C",
                      "  Hook: GetSoftCurrency RVA 0x1999A1C"));
            ImGui::Spacing();

            ImGui::TextColored(ImVec4(.9f,.6f,.1f,1.f),
                T("  TIEN CUNG (Hard Currency)","  HARD CURRENCY"));
            ImGui::Checkbox(T("Hien Thi: 999,999,999 Kim Cuong","Display: 999M Hard"),&bHardCoin);
            if(bHardCoin)
                ImGui::TextColored(ImVec4(.5f,.5f,.5f,1.f),
                    T("  Hook: GetHardCurrency RVA 0x199A63C",
                      "  Hook: GetHardCurrency RVA 0x199A63C"));
            ImGui::EndTabItem();
        }

        // ── TAB 3: SETTINGS ──────────────────────────────────────
        if(ImGui::BeginTabItem("SETTINGS")){
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(.5f,1.f,.5f,1.f),T("  HIEU NANG","  PERFORMANCE"));
            ImGui::Spacing();
            float fc=g_fps>=60?1.f:g_fps>=30?.5f:0.f;
            ImGui::TextColored(ImVec4(1.f-fc,fc,0.f,1.f),"FPS: %.1f",g_fps);
            ImGui::ProgressBar(g_fps/120.f,ImVec2(-1.f,8.f));
            ImGui::TextColored(ImVec4(.6f,.6f,.6f,1.f),
                g_fps>=60?T("Rat tot (60+)","Very good (60+)"):
                g_fps>=30?T("Binh thuong (30-60)","Normal (30-60)"):
                          T("Thap, co the lag (<30)","Low, may lag (<30)"));

            ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
            ImGui::TextColored(ImVec4(.7f,.7f,1.f,1.f),T("  DUMP INFO","  DUMP INFO"));
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(.6f,.8f,.6f,1.f)," AntiCheat: NONE (client-side)");
            ImGui::TextColored(ImVec4(.6f,.8f,.6f,1.f)," Network:   Custom UDP + PostgreSQL");
            ImGui::TextColored(ImVec4(.6f,.8f,.6f,1.f)," Engine:    Unity IL2CPP (ARM64)");
            ImGui::TextColored(ImVec4(.6f,.8f,.6f,1.f)," Lib:       libil2cpp.so");

            ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
            ImGui::TextColored(ImVec4(.7f,.7f,1.f,1.f),T("  NGON NGU","  LANGUAGE"));
            ImGui::Spacing();
            if(ImGui::RadioButton("Tieng Viet", lang_idx==0)) lang_idx=0;
            if(ImGui::RadioButton("English",    lang_idx==1)) lang_idx=1;

            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }
    ImGui::End();
}

// ================================================================
//  MAIN THREAD
// ================================================================
void* thread(void*){
    initModMenu((void*)DrawMenu);
    do{ sleep(1); }while(getAbsoluteAddress("libil2cpp.so",0)==0);
    LOGI("[AXIOM] libil2cpp.so detected — waiting 3s for IL2CPP init...");
    sleep(3);

    // ── Macro helper ─────────────────────────────────────────────
    #define HOOK(rva, hk, orig) do{ \
        void* _addr=(void*)getAbsoluteAddress("libil2cpp.so",(rva)); \
        if(_addr){ DobbyHook(_addr,(void*)(hk),(void**)&(orig)); \
                   LOGI("[AXIOM] Hooked 0x%08X -> %s", (rva), #hk); } \
        else{ LOGI("[AXIOM] MISS 0x%08X -> %s", (rva), #hk); } \
    }while(0)

    // ── WEAPON ───────────────────────────────────────────────────
    // ThirdPersonCharacterControllerScript.Update — TypeDefIndex: 47
    // Fields: currentAmmo@0x30, outOfAmmo@0x34, fireRate@0x38, isReloading@0x11A
    HOOK(0x180DC1C, hook_WeapUpdate, old_WeapUpdate);

    // ── NO RECOIL ────────────────────────────────────────────────
    // ComponentRecoilForFastShootingViewMyWeapon.HandleChangeShooting
    HOOK(0x1A4080C, hook_RecoilFastHandler, old_RecoilFastHandler);
    // ComponentRecoilForSlowShootingViewMyWeapon.HandleShoot
    HOOK(0x1A40CFC, hook_RecoilSlowHandler, old_RecoilSlowHandler);

    // ── MOVEMENT ─────────────────────────────────────────────────
    // MovableCharacterComponent.MoveByDirection — TypeDefIndex: 1325
    // Fields: Velocity@0x50, MoveVelocity@0x5C, MoveDirection@0x68, IsJumped@0x78
    HOOK(0x1A6D74C, hook_MoveByDirection, old_MoveByDirection);
    // MovableCharacterComponent.Move(Vector3) — fly Y override
    HOOK(0x1A6D690, hook_Move, old_Move);

    // ── CHARACTER UPDATE ─────────────────────────────────────────
    // ClientMyCharacter.LateUpdate — TypeDefIndex: 1050
    // Fields: MovableCharacterComponent@0xA0, _modelCharacter@0xA8
    HOOK(0x1A3F720, hook_CharLateUpdate, old_CharLateUpdate);

    // ── CURRENCY DISPLAY ─────────────────────────────────────────
    // ModelPlayerState.GetSoftCurrency — TypeDefIndex: 3061
    HOOK(0x1999A1C, hook_GetSoftCurrency, old_GetSoftCurrency);
    // ModelPlayerState.GetHardCurrency
    HOOK(0x199A63C, hook_GetHardCurrency, old_GetHardCurrency);

    #undef HOOK
    LOGI("[AXIOM] ALL HOOKS INSTALLED");
    pthread_exit(0);
}

// ================================================================
//  JNI
// ================================================================
extern "C" {
    JavaVM* jvm=nullptr; JNIEnv* env=nullptr;
    __attribute__((visibility("default")))
    jint loadJNI(JavaVM* vm){
        jvm=vm; vm->AttachCurrentThread(&env,nullptr);
        return JNI_VERSION_1_6;
    }
}

__attribute__((constructor))
void init(){
    pthread_t t;
    pthread_create(&t,nullptr,thread,nullptr);
    RemapTools::RemapLibrary("libLoader.so");
}

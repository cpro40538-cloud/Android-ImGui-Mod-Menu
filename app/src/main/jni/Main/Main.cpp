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
//  TOGGLES
// ================================================================
static bool bGodMode      = false;
static bool bOneShotKill  = false;
static bool bInfAmmo      = false;
static bool bNoRecoil     = false;
static bool bSpeedHack    = false;
static bool bFreezeMoney  = false;
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
//  UNITY API — RVA từ dump.cs (UnityEngine module)
//  Camera.get_main          @ 0x251B400
//  Camera.WorldToScreenPoint@ 0x251B1DC  (no eye param)
//  Component.get_transform  @ 0x253EF08
//  Transform.get_position   @ 0x254B0DC
// ================================================================
static void*   (*Camera_get_main)()                                   = nullptr;
static Vector3 (*Camera_WorldToScreenPoint)(void* cam, Vector3 world) = nullptr;
static void*   (*Component_get_transform)(void* comp)                 = nullptr;
static Vector3 (*Transform_get_position)(void* trans)                 = nullptr;

// ================================================================
//  IL2CPP THREAD ATTACH
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

// ================================================================
//  HOOK: God Mode + One Shot Kill
//  CharacterDamage.ApplyDamage — RVA: 0x119235C
//  Fields: AIComponent@0x30, hitPoints@0x40, initialHitPoints@0x44
// ================================================================
static void (*old_ApplyDamage)(void* _this,
    float dmg,
    float dx, float dy, float dz,
    float ax, float ay, float az,
    void* attacker, bool isPlayer, bool isExplosion,
    void* hitBody, float force, bool isHead);

static void hook_ApplyDamage(void* _this,
    float dmg,
    float dx, float dy, float dz,
    float ax, float ay, float az,
    void* attacker, bool isPlayer, bool isExplosion,
    void* hitBody, float force, bool isHead)
{
    if (bGodMode && isPlayer) return;
    if (bOneShotKill && !isPlayer) dmg *= damageMult;
    if (old_ApplyDamage)
        old_ApplyDamage(_this, dmg, dx, dy, dz, ax, ay, az,
                        attacker, isPlayer, isExplosion, hitBody, force, isHead);
}

// ================================================================
//  HOOK: Infinite Ammo
//  WeaponBehavior.Update() — RVA: 0x1309740
//  Fields: bulletsLeft@0x114, bulletsPerClip@0x118
//          ammo@0x110, maxAmmo@0x120
//
//  KHÔNG có HaveAmmo()/DecreaseBullets() trong dump!
//  Hook Update() và ghi đè trực tiếp vào field mỗi frame.
// ================================================================
static void (*old_WeapUpdate)(void* inst);
static void hook_WeapUpdate(void* inst) {
    if (old_WeapUpdate) old_WeapUpdate(inst);
    if (!bInfAmmo || !inst) return;
    int perClip = *(int*)((uint8_t*)inst + 0x118);
    int maxAmmo = *(int*)((uint8_t*)inst + 0x120);
    *(int*)((uint8_t*)inst + 0x114) = perClip; // bulletsLeft = bulletsPerClip
    *(int*)((uint8_t*)inst + 0x110) = maxAmmo; // ammo = maxAmmo
}

// ================================================================
//  HOOK: No Recoil / No Spread
//  WeaponBehavior.SprayDirection() — RVA: 0x130E23C
//  Trả về Vector3.zero khi bNoRecoil bật.
//  KHÔNG có CalcSpread() trong dump — SprayDirection là hàm đúng.
// ================================================================
static Vector3 (*old_SprayDirection)(void* inst);
static Vector3 hook_SprayDirection(void* inst) {
    if (bNoRecoil) return {0.0f, 0.0f, 0.0f};
    return old_SprayDirection ? old_SprayDirection(inst) : Vector3{0,0,0};
}

// ================================================================
//  HOOK: Speed Hack
//  FPSRigidBodyWalker.FixedUpdate() — RVA: 0x12EC900
//  Fields: walkSpeed@0xD4, sprintSpeed@0xD8, moveSpeedMult@0xE8
//
//  KHÔNG có get_speed() method — speed là plain fields.
//  Nhân tạm trước khi gọi FixedUpdate, khôi phục sau để
//  tránh lưu giá trị nhân vào file save.
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
//  HOOK: Freeze Money
//  ItemDataManager.GetCurrency(CommonDataType type) — RVA: 0x12BF2FC
//  STATIC method, không có this ptr.
// ================================================================
static int (*old_GetCurrency)(int type);
static int hook_GetCurrency(int type) {
    int real = old_GetCurrency ? old_GetCurrency(type) : 0;
    LOGI("[GetCurrency] type=%d real=%d bFreezeMoney=%d", type, real, bFreezeMoney);
    return bFreezeMoney ? 999999999 : real;
}

// ================================================================
//  HOOK: No Ads + Unlock All Weapons
//  LoadingOnces — KHÔNG có getter/setter properties trong dump!
//  isNoAds là public bool @ 0x48, unLockAllWeapon @ 0x45.
//  Cách đúng: hook Awake() để lấy instance pointer, sau đó
//  ghi trực tiếp vào field mỗi giây trong polling thread.
//
//  LoadingOnces.Awake() — RVA: 0x12D711C
// ================================================================
static void*  g_loadingInst = nullptr;

static void (*old_LoadingAwake)(void* inst);
static void hook_LoadingAwake(void* inst) {
    g_loadingInst = inst;
    if (old_LoadingAwake) old_LoadingAwake(inst);
    // Áp dụng ngay khi Awake chạy
    if (bNoAds)        *(bool*)((uint8_t*)inst + 0x48) = true;
    if (bUnlockWeapon) *(bool*)((uint8_t*)inst + 0x45) = true;
}

// ================================================================
//  HOOK: ESP — CharacterDamage.Update()
//  RVA: 0x1191E10
//  Fields: AIComponent@0x30 (null=player), hitPoints@0x40,
//          initialHitPoints@0x44, myTransform@0xF0
// ================================================================
static void (*old_CharDmgUpdate)(void* inst);
static void hook_CharDmgUpdate(void* inst) {
    if (old_CharDmgUpdate) old_CharDmgUpdate(inst);
    if (!inst) return;
    if (!bEspBox && !bEspLine && !bEspDist && !bEspHp) return;

    void* aiComp = *(void**)((uint8_t*)inst + 0x30);
    if (!aiComp) return; // player, bo qua

    float hp    = *(float*)((uint8_t*)inst + 0x40);
    float maxHp = *(float*)((uint8_t*)inst + 0x44);
    if (hp <= 0.0f || maxHp <= 0.0f) return;

    long now = now_ms();
    espMtx.lock();
    bool found = false;
    for (int i = 0; i < 128; i++) {
        if (espList[i].inst == inst) {
            espList[i] = {inst, now, hp, maxHp};
            found = true; break;
        }
    }
    if (!found) {
        for (int i = 0; i < 128; i++) {
            if (!espList[i].inst || now - espList[i].ts > 5000) {
                espList[i] = {inst, now, hp, maxHp};
                break;
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

    // Polling: cap nhat NoAds/Unlock moi frame neu co instance
    if (g_loadingInst) {
        if (bNoAds)        *(bool*)((uint8_t*)g_loadingInst + 0x48) = true;
        if (bUnlockWeapon) *(bool*)((uint8_t*)g_loadingInst + 0x45) = true;
    }

    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding    = 10.0f; s.FrameRounding  =  5.0f;
    s.GrabRounding      =  4.0f; s.ItemSpacing    = ImVec2(8, 7);
    s.WindowPadding     = ImVec2(12, 12);
    s.Colors[ImGuiCol_WindowBg]      = ImVec4(0.04f, 0.04f, 0.08f, 0.97f);
    s.Colors[ImGuiCol_TitleBg]       = ImVec4(0.00f, 0.12f, 0.30f, 1.00f);
    s.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.00f, 0.20f, 0.50f, 1.00f);
    s.Colors[ImGuiCol_FrameBg]       = ImVec4(0.08f, 0.08f, 0.15f, 1.00f);
    s.Colors[ImGuiCol_CheckMark]     = ImVec4(0.00f, 0.90f, 1.00f, 1.00f);
    s.Colors[ImGuiCol_Tab]           = ImVec4(0.05f, 0.10f, 0.20f, 1.00f);
    s.Colors[ImGuiCol_TabActive]     = ImVec4(0.00f, 0.20f, 0.50f, 1.00f);
    s.Colors[ImGuiCol_TabHovered]    = ImVec4(0.00f, 0.30f, 0.70f, 1.00f);
    s.Colors[ImGuiCol_SliderGrab]    = ImVec4(0.00f, 0.70f, 1.00f, 1.00f);
    s.Colors[ImGuiCol_Button]        = ImVec4(0.00f, 0.25f, 0.55f, 1.00f);
    s.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.00f, 0.40f, 0.80f, 1.00f);
    s.Colors[ImGuiCol_Separator]     = ImVec4(0.20f, 0.20f, 0.30f, 1.00f);

    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    ImVec2 scr       = ImGui::GetIO().DisplaySize;

    if (bAimCircle)
        draw->AddCircle(ImVec2(scr.x * 0.5f, scr.y * 0.5f),
                        AimFov, IM_COL32(255,255,255,170), 128, 1.6f);

    // ESP
    if ((bEspBox || bEspLine || bEspDist || bEspHp) &&
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
                if (foot.X == 0.f && foot.Y == 0.f && foot.Z == 0.f) continue;
                if (foot.Y > 400.f || foot.Y < -200.f) continue;

                Vector3 head = foot; head.Y += 1.85f;
                Vector3 sf = Camera_WorldToScreenPoint(cam, foot);
                Vector3 sh = Camera_WorldToScreenPoint(cam, head);
                if (sf.Z < 0.2f || sf.Z > 600.f) continue;

                float fx = sf.X, fy = scr.y - sf.Y, hy = scr.y - sh.Y;
                float h = fy - hy;
                if (h < 4.f) continue;
                float w = h * 0.45f, d = sf.Z;

                ImU32 boxCol = d < 20.f ? IM_COL32(255,60,60,245)
                             : d < 50.f ? IM_COL32(255,200,0,245)
                                        : IM_COL32(60,255,120,245);

                if (bEspLine)
                    draw->AddLine(ImVec2(scr.x*0.5f, scr.y), ImVec2(fx,fy),
                                  IM_COL32(255,70,70,200), 1.3f);

                if (bEspBox) {
                    float lx=fx-w*0.5f, rx=fx+w*0.5f;
                    float cw=w*0.22f, ch=h*0.18f;
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
                if (bEspDist) {
                    char buf[24]; snprintf(buf,sizeof(buf),"%.0fm",d);
                    draw->AddText(ImVec2(fx-10.f,hy-16.f),IM_COL32(255,220,0,255),buf);
                }
                if (bEspHp && espList[i].maxHp > 0.f) {
                    float ratio = espList[i].hp / espList[i].maxHp;
                    if (ratio<0.f) ratio=0.f; if (ratio>1.f) ratio=1.f;
                    float bx = fx+w*0.5f+4.f;
                    draw->AddRectFilled(ImVec2(bx-1,hy-1),ImVec2(bx+5,fy+1),IM_COL32(0,0,0,180));
                    ImU32 hpCol = ratio>0.6f ? IM_COL32(0,255,80,255)
                                : ratio>0.3f ? IM_COL32(255,200,0,255)
                                             : IM_COL32(255,0,0,255);
                    float top = hy+h*(1.f-ratio);
                    draw->AddRectFilled(ImVec2(bx,top),ImVec2(bx+4,fy),hpCol);
                    char hpBuf[16]; snprintf(hpBuf,sizeof(hpBuf),"%.0f",espList[i].hp);
                    draw->AddText(ImVec2(bx+6.f,top-2.f),IM_COL32(255,255,255,220),hpBuf);
                }
            }
            espMtx.unlock();
        }
    }

    // ── MENU WINDOW ────────────────────────────────────────────
    // FIX: Bỏ NoCollapse|NoResize để menu kéo to/đóng nhỏ được
    ImGui::SetNextWindowSize(ImVec2(360, 460), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(10, 10),    ImGuiCond_FirstUseEver);
    ImGui::Begin("  ZOMBIE3D MOD  |  AXIOM DEV  ", nullptr, 0);

    ImGui::TextColored(ImVec4(0.f,0.8f,1.f,1.f), "  AXIOM DEVELOPMENT");
    ImGui::Separator(); ImGui::Spacing();

    if (ImGui::BeginTabBar("tabs")) {

        if (ImGui::BeginTabItem(T("NGUOI CHOI","PLAYER"))) {
            ImGui::Spacing();
            ImGui::Checkbox(T("Bat Tu (God Mode)",      "God Mode"),      &bGodMode);    ImGui::Spacing();
            ImGui::Checkbox(T("Mot Phat Diet Tat",      "One Shot Kill"), &bOneShotKill);
            if (bOneShotKill) { ImGui::SetNextItemWidth(-1); ImGui::SliderFloat(T("Nhan Sat Thuong","Dmg Multi"),&damageMult,10.f,9999.f,"x%.0f"); }
            ImGui::Spacing();
            ImGui::Checkbox(T("Dan Vo Han",             "Inf Ammo"),      &bInfAmmo);    ImGui::Spacing();
            ImGui::Checkbox(T("Khong Giat Sung",        "No Recoil"),     &bNoRecoil);   ImGui::Spacing();
            ImGui::Checkbox(T("Tang Toc Do Di Chuyen",  "Speed Hack"),    &bSpeedHack);
            if (bSpeedHack) { ImGui::SetNextItemWidth(-1); ImGui::SliderFloat(T("Toc Do x","Speed x"),&speedMult,1.f,5.f,"x%.1f"); }
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem(T("TIEN TE","ECONOMY"))) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.6f,0.6f,0.6f,1.f),"  Gold/Grenade/Medical -> 999,999,999");
            ImGui::Spacing();
            ImGui::Checkbox(T("Dong Bang Tien",         "Freeze Money"),  &bFreezeMoney); ImGui::Spacing();
            ImGui::Separator(); ImGui::Spacing();
            ImGui::TextColored(ImVec4(1.f,0.75f,0.f,1.f), T("  Vat Pham","  Items")); ImGui::Spacing();
            ImGui::Checkbox(T("Mo Khoa Tat Ca Sung",    "Unlock All Weapons"),&bUnlockWeapon); ImGui::Spacing();
            ImGui::Checkbox(T("Bo Quang Cao",           "No Ads"),        &bNoAds);
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem(T("DINH VI","ESP"))) {
            ImGui::Spacing();
            ImGui::Checkbox(T("Hop Zombie",     "Enemy Box"),  &bEspBox);  ImGui::Spacing();
            ImGui::Checkbox(T("Duong Den Dich", "Enemy Line"), &bEspLine); ImGui::Spacing();
            ImGui::Checkbox(T("Khoang Cach",    "Distance"),   &bEspDist); ImGui::Spacing();
            ImGui::Checkbox(T("Thanh Mau HP",   "HP Bar"),     &bEspHp);   ImGui::Spacing();
            ImGui::Separator(); ImGui::Spacing();
            ImGui::Checkbox(T("Vong Tron Aim",  "Aim Circle"), &bAimCircle);
            if (bAimCircle) { ImGui::SetNextItemWidth(-1); ImGui::SliderFloat("FOV",&AimFov,50.f,400.f,"%.0f px"); }
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
//  MAIN THREAD — hook toàn bộ sau khi libil2cpp load xong
// ================================================================
void* thread(void*) {
    initModMenu((void*)DrawMenu);

    // Chờ libil2cpp nạp xong
    do { sleep(1); } while (getAbsoluteAddress("libil2cpp.so", 0) == 0);
    LOGI("[+] libil2cpp detected, waiting 3s for unpack...");
    sleep(3);

    // Gán Unity API pointers (không dùng DobbyHook, chỉ cast)
    uintptr_t base = getAbsoluteAddress("libil2cpp.so", 0);

    Camera_get_main          = (void*(*)())
        (void*)getAbsoluteAddress("libil2cpp.so", 0x251B400);
    Camera_WorldToScreenPoint= (Vector3(*)(void*,Vector3))
        (void*)getAbsoluteAddress("libil2cpp.so", 0x251B1DC);
    Component_get_transform  = (void*(*)(void*))
        (void*)getAbsoluteAddress("libil2cpp.so", 0x253EF08);
    Transform_get_position   = (Vector3(*)(void*))
        (void*)getAbsoluteAddress("libil2cpp.so", 0x254B0DC);

    // Macro helper với log
    #define HOOK(rva, hk, orig) do { \
        void* _addr = (void*)getAbsoluteAddress("libil2cpp.so", rva); \
        int _r = DobbyHook(_addr, (void*)hk, (void**)&orig); \
        LOGI("[HOOK] %s RVA=0x%lx addr=%p ret=%d", #hk, (uintptr_t)rva, _addr, _r); \
    } while(0)

    // CharacterDamage
    HOOK(0x119235C, hook_ApplyDamage,   old_ApplyDamage);
    HOOK(0x1191E10, hook_CharDmgUpdate, old_CharDmgUpdate);

    // WeaponBehavior: Inf Ammo + No Recoil
    HOOK(0x1309740, hook_WeapUpdate,    old_WeapUpdate);
    HOOK(0x130E23C, hook_SprayDirection,old_SprayDirection);

    // FPSRigidBodyWalker: Speed Hack
    HOOK(0x12EC900, hook_FPSFixedUpdate,old_FPSFixedUpdate);

    // ItemDataManager: Freeze Money
    HOOK(0x12BF2FC, hook_GetCurrency,   old_GetCurrency);

    // LoadingOnces: No Ads + Unlock Weapons (qua Awake)
    HOOK(0x12D711C, hook_LoadingAwake,  old_LoadingAwake);

    #undef HOOK

    LOGI("[+] ALL HOOKS DONE!");
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

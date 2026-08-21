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
static bool bGodMode      = false;  // block damage khi isPlayer=true
static bool bOneShotKill  = false;  // nhan damage khung khi !isPlayer
static bool bInfAmmo      = false;  // khong giam dan
static bool bNoRecoil     = false;  // spread = 0
static bool bSpeedHack    = false;  // nhan toc do
static bool bFreezeMoney  = false;  // GetCurrency luon tra 999M
static bool bNoAds        = false;  // isNoAds getter = true
static bool bUnlockWeapon = false;  // unLockAllWeapon getter = true
static bool bEspBox       = false;  // ve hop vien bi
static bool bEspLine      = false;  // ve duong den dich
static bool bEspDist      = false;  // hien khoang cach
static bool bEspHp        = false;  // hien thanh mau
static bool bAimCircle    = false;  // vong tron aim

static float speedMult  = 2.0f;
static float AimFov     = 150.0f;
static float damageMult = 999.0f;

// ================================================================
//  ESP DATA STRUCTS
//  Dung CharacterDamage lam don vi tracking
//  Field offset lay thang tu dump.cs:
//    AIComponent   @ 0x30 — null neu la player, non-null neu la NPC
//    hitPoints     @ 0x40
//    initialHits   @ 0x44
//    myTransform   @ 0xF0
// ================================================================
struct EspEntry {
    void*  inst;       // CharacterDamage*
    long   ts;         // timestamp ms
    float  hp;
    float  maxHp;
};
static EspEntry   espList[128] = {};
static std::mutex espMtx;

static long now_ms() {
    struct timeval tv; gettimeofday(&tv, NULL);
    return (long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

// ================================================================
//  UNITY API POINTERS
//  Offset lay tu dump UnityEngine — can xac nhan lai voi build
// ================================================================
static void*   (*Camera_get_main)()                                    = nullptr;
static Vector3 (*Camera_WorldToScreenPoint)(void* cam, Vector3 world)  = nullptr;
static void*   (*Component_get_transform)(void* comp)                  = nullptr;
static Vector3 (*Transform_get_position)(void* trans)                  = nullptr;

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
//  HOOK: Anti-Cheat bypass
//  TODO: doi sang RVA anti-cheat dung cua game nay
// ================================================================
static void (*old_StartDetection)();
static void hook_StartDetection() { /* silently drop */ }

// ================================================================
//  HOOK: God Mode + One Shot Kill
//  CharacterDamage.ApplyDamage
//  RVA tu dump.cs: 0x119235C (version day du 9 param)
//
//  Signature goc: void ApplyDamage(float damage, Vector3 attackDir,
//                 Vector3 attackerPos, Transform attacker,
//                 bool isPlayer, bool isExplosion,
//                 Rigidbody hitBody, float bodyForce, bool isHeadShot)
//
//  ARM64 calling convention: Vector3 (3 float = 12 byte) duoc
//  pass inline trong register file — tach thanh 3 float rieng
//  cho an toan voi Dobby
// ================================================================
static void (*old_ApplyDamage)(void* _this,
    float dmg,
    float dx, float dy, float dz,   // attackDir Vector3
    float ax, float ay, float az,   // attackerPos Vector3
    void* attacker, bool isPlayer, bool isExplosion,
    void* hitBody, float force, bool isHead);

static void hook_ApplyDamage(void* _this,
    float dmg,
    float dx, float dy, float dz,
    float ax, float ay, float az,
    void* attacker, bool isPlayer, bool isExplosion,
    void* hitBody, float force, bool isHead)
{
    // God Mode: block moi damage vao player
    if (bGodMode && isPlayer) return;

    // One Shot Kill: nhan damage khung len enemy
    if (bOneShotKill && !isPlayer)
        dmg *= damageMult;

    old_ApplyDamage(_this, dmg, dx, dy, dz, ax, ay, az,
                    attacker, isPlayer, isExplosion,
                    hitBody, force, isHead);
}

// ================================================================
//  HOOK: Infinite Ammo
//  WeaponBehavior/GunScript — TODO: tim RVA chinh xac tu dump
//  Tam giu offset tu template, can kiem tra lai
// ================================================================
static bool (*old_HaveAmmo)(void* inst);
static bool hook_HaveAmmo(void* inst) {
    return bInfAmmo ? true : old_HaveAmmo(inst);
}

static void (*old_DecreaseBullets)(void* inst);
static void hook_DecreaseBullets(void* inst) {
    if (!bInfAmmo) old_DecreaseBullets(inst);
}

// ================================================================
//  HOOK: No Recoil / No Spread
//  TODO: tim RVA CalcSpread hoac SetRecoil tu GunScript/WeaponBehavior
// ================================================================
static float (*old_CalcSpread)(void* inst);
static float hook_CalcSpread(void* inst) {
    return bNoRecoil ? 0.0f : old_CalcSpread(inst);
}

// ================================================================
//  HOOK: Speed Hack
//  FPSRigidBodyWalker — TODO: tim RVA get_speed hoac FixedUpdate
// ================================================================
static float (*old_GetMoveSpeed)(void* inst);
static float hook_GetMoveSpeed(void* inst) {
    float spd = old_GetMoveSpeed(inst);
    return bSpeedHack ? spd * speedMult : spd;
}

// ================================================================
//  HOOK: Freeze Money (Gold, Grenade, Medical, Ticket...)
//  ItemDataManager.GetCurrency — RVA tu dump: 0x12BF2FC
//  Signature: int GetCurrency(int type) — STATIC, khong co this ptr
// ================================================================
static int (*old_GetCurrency)(int type);
static int hook_GetCurrency(int type) {
    return bFreezeMoney ? 999999999 : old_GetCurrency(type);
}

// ================================================================
//  HOOK: No Ads
//  LoadingOnces.isNoAds getter
//  Field isNoAds @ 0x48, RVA getter — TODO: tim chinh xac
// ================================================================
static bool (*old_GetIsNoAds)(void* inst);
static bool hook_GetIsNoAds(void* inst) {
    return bNoAds ? true : old_GetIsNoAds(inst);
}

// ================================================================
//  HOOK: Unlock All Weapons
//  LoadingOnces.unLockAllWeapon getter
//  Field @ 0x45 — TODO: tim RVA getter
// ================================================================
static bool (*old_GetUnlockWeapon)(void* inst);
static bool hook_GetUnlockWeapon(void* inst) {
    return bUnlockWeapon ? true : old_GetUnlockWeapon(inst);
}

// ================================================================
//  HOOK: ESP — CharacterDamage.Update
//  RVA tu dump: 0x1191E10
//  Chay moi frame tren moi NPC — perfect cho viec capture instance
//  Doc field truc tiep tu offset dump.cs:
//    AIComponent   @ 0x30 — null = player, non-null = NPC
//    hitPoints     @ 0x40 — HP hien tai
//    initialHits   @ 0x44 — HP toi da
//    myTransform   @ 0xF0 — Transform de lay world position
// ================================================================
static void (*old_CharDmgUpdate)(void* inst);
static void hook_CharDmgUpdate(void* inst) {
    old_CharDmgUpdate(inst);
    if (!inst) return;
    if (!bEspBox && !bEspLine && !bEspDist && !bEspHp) return;

    // Chi lay NPC: AIComponent phai non-null
    void* aiComp = *(void**)((uint8_t*)inst + 0x30);
    if (!aiComp) return;

    float hp    = *(float*)((uint8_t*)inst + 0x40);
    float maxHp = *(float*)((uint8_t*)inst + 0x44);
    if (hp <= 0.0f || maxHp <= 0.0f) return; // zombie da chet

    long now = now_ms();
    espMtx.lock();
    bool found = false;
    for (int i = 0; i < 128; i++) {
        if (espList[i].inst == inst) {
            espList[i].ts    = now;
            espList[i].hp    = hp;
            espList[i].maxHp = maxHp;
            found = true; break;
        }
    }
    if (!found) {
        for (int i = 0; i < 128; i++) {
            if (!espList[i].inst || now - espList[i].ts > 5000) {
                espList[i] = { inst, now, hp, maxHp };
                break;
            }
        }
    }
    espMtx.unlock();
}

// ================================================================
//  DRAW — goi moi frame tu ImGui loop
// ================================================================
static void DrawMenu() {
    EnsureAttached();

    // Dark blue theme
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding    = 10.0f;
    s.FrameRounding     =  5.0f;
    s.GrabRounding      =  4.0f;
    s.ItemSpacing       = ImVec2(8, 7);
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

    // Vong tron FOV aim
    if (bAimCircle)
        draw->AddCircle(
            ImVec2(scr.x * 0.5f, scr.y * 0.5f),
            AimFov, IM_COL32(255, 255, 255, 170), 128, 1.6f
        );

    // ── ESP REALTIME ───────────────────────────────────────────
    if ((bEspBox || bEspLine || bEspDist || bEspHp) &&
        Camera_get_main && Component_get_transform &&
        Camera_WorldToScreenPoint && Transform_get_position) {

        void* cam = Camera_get_main();
        if (cam) {
            long now = now_ms();
            espMtx.lock();

            for (int i = 0; i < 128; i++) {
                void* entry = espList[i].inst;
                if (!entry) continue;

                // Het thoi gian timeout — xoa
                if (now - espList[i].ts > 5000) {
                    espList[i].inst = nullptr; continue;
                }

                // Doc myTransform @ 0xF0 tu CharacterDamage
                void* myTrans = *(void**)((uint8_t*)entry + 0xF0);
                if (!myTrans) continue;

                // Lay world position realtime
                Vector3 foot = Transform_get_position(myTrans);
                if (foot.X == 0.0f && foot.Y == 0.0f && foot.Z == 0.0f) continue;
                if (foot.Y > 400.0f || foot.Y < -200.0f) continue;

                Vector3 head = foot;
                head.Y += 1.85f;

                Vector3 sf = Camera_WorldToScreenPoint(cam, foot);
                Vector3 sh = Camera_WorldToScreenPoint(cam, head);
                if (sf.Z < 0.2f || sf.Z > 600.0f) continue;

                float fx = sf.X;
                float fy = scr.y - sf.Y;
                float hy = scr.y - sh.Y;
                float h  = fy - hy;
                if (h < 4.0f) continue;

                float w = h * 0.45f;
                float d = sf.Z;

                // Mau box theo khoang cach
                ImU32 boxCol = d < 20.0f ? IM_COL32(255, 60,  60,  245)
                             : d < 50.0f ? IM_COL32(255, 200,  0,  245)
                                         : IM_COL32( 60, 255, 120, 245);

                // Duong den zombie
                if (bEspLine)
                    draw->AddLine(
                        ImVec2(scr.x * 0.5f, scr.y),
                        ImVec2(fx, fy),
                        IM_COL32(255, 70, 70, 200), 1.3f
                    );

                // Hop + corner accents
                if (bEspBox) {
                    float lx = fx - w * 0.5f, rx = fx + w * 0.5f;
                    float cw = w * 0.22f, ch = h * 0.18f;

                    // Shadow
                    draw->AddRect(ImVec2(lx-1, hy-1), ImVec2(rx+1, fy+1),
                                  IM_COL32(0,0,0,160), 0, 0, 2.5f);
                    // Main box
                    draw->AddRect(ImVec2(lx, hy), ImVec2(rx, fy),
                                  boxCol, 0, 0, 1.4f);
                    // TL
                    draw->AddLine(ImVec2(lx, hy), ImVec2(lx+cw, hy), IM_COL32(255,255,255,220), 2.0f);
                    draw->AddLine(ImVec2(lx, hy), ImVec2(lx, hy+ch), IM_COL32(255,255,255,220), 2.0f);
                    // TR
                    draw->AddLine(ImVec2(rx, hy), ImVec2(rx-cw, hy), IM_COL32(255,255,255,220), 2.0f);
                    draw->AddLine(ImVec2(rx, hy), ImVec2(rx, hy+ch), IM_COL32(255,255,255,220), 2.0f);
                    // BL
                    draw->AddLine(ImVec2(lx, fy), ImVec2(lx+cw, fy), IM_COL32(255,255,255,220), 2.0f);
                    draw->AddLine(ImVec2(lx, fy), ImVec2(lx, fy-ch), IM_COL32(255,255,255,220), 2.0f);
                    // BR
                    draw->AddLine(ImVec2(rx, fy), ImVec2(rx-cw, fy), IM_COL32(255,255,255,220), 2.0f);
                    draw->AddLine(ImVec2(rx, fy), ImVec2(rx, fy-ch), IM_COL32(255,255,255,220), 2.0f);
                }

                // Khoang cach
                if (bEspDist) {
                    char buf[24]; snprintf(buf, sizeof(buf), "%.0fm", d);
                    draw->AddText(ImVec2(fx - 10.0f, hy - 16.0f),
                                  IM_COL32(255, 220, 0, 255), buf);
                }

                // Thanh mau HP
                if (bEspHp && espList[i].maxHp > 0.0f) {
                    float ratio = espList[i].hp / espList[i].maxHp;
                    if (ratio < 0.0f) ratio = 0.0f;
                    if (ratio > 1.0f) ratio = 1.0f;

                    float bx = fx + w * 0.5f + 4.0f;

                    // Nen toi
                    draw->AddRectFilled(ImVec2(bx-1, hy-1), ImVec2(bx+5, fy+1),
                                        IM_COL32(0, 0, 0, 180));

                    // Mau HP: xanh la -> vang -> do
                    ImU32 hpCol = ratio > 0.60f ? IM_COL32(  0, 255,  80, 255)
                                : ratio > 0.30f ? IM_COL32(255, 200,   0, 255)
                                                : IM_COL32(255,   0,   0, 255);

                    float top = hy + h * (1.0f - ratio);
                    draw->AddRectFilled(ImVec2(bx, top), ImVec2(bx+4, fy), hpCol);

                    // So HP hien thi
                    char hpBuf[16];
                    snprintf(hpBuf, sizeof(hpBuf), "%.0f", espList[i].hp);
                    draw->AddText(ImVec2(bx + 6.0f, top - 2.0f),
                                  IM_COL32(255, 255, 255, 220), hpBuf);
                }
            }
            espMtx.unlock();
        }
    }

    // ── MENU WINDOW ────────────────────────────────────────────
    ImGui::SetNextWindowSize(ImVec2(360, 460), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(10, 10),    ImGuiCond_FirstUseEver);
    ImGui::Begin("  ZOMBIE3D MOD  |  AXIOM DEV  ",
                 nullptr,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize);

    ImGui::TextColored(ImVec4(0.0f, 0.8f, 1.0f, 1.0f),
                       "  AXIOM DEVELOPMENT");
    ImGui::Separator(); ImGui::Spacing();

    if (ImGui::BeginTabBar("tabs")) {

        // ──────────────────────────────────────────────────────
        //  TAB: NGUOI CHOI / PLAYER
        // ──────────────────────────────────────────────────────
        if (ImGui::BeginTabItem(T("NGUOI CHOI", "PLAYER"))) {
            ImGui::Spacing();

            ImGui::Checkbox(T("Bat Tu (God Mode)",     "God Mode"),     &bGodMode);
            ImGui::Spacing();

            ImGui::Checkbox(T("Mot Phat Diet Tat",     "One Shot Kill"),&bOneShotKill);
            if (bOneShotKill) {
                ImGui::SetNextItemWidth(-1);
                ImGui::SliderFloat(T("Nhan Sat Thuong","Dmg Multi"),
                                   &damageMult, 10.0f, 9999.0f, "x%.0f");
            }
            ImGui::Spacing();

            ImGui::Checkbox(T("Dan Vo Han",            "Inf Ammo"),     &bInfAmmo);
            ImGui::Spacing();

            ImGui::Checkbox(T("Khong Giat Sung",       "No Recoil"),    &bNoRecoil);
            ImGui::Spacing();

            ImGui::Checkbox(T("Tang Toc Do Di Chuyen", "Speed Hack"),   &bSpeedHack);
            if (bSpeedHack) {
                ImGui::SetNextItemWidth(-1);
                ImGui::SliderFloat(T("Toc Do x","Speed x"),
                                   &speedMult, 1.0f, 5.0f, "x%.1f");
            }

            ImGui::EndTabItem();
        }

        // ──────────────────────────────────────────────────────
        //  TAB: TIEN TE / ECONOMY
        // ──────────────────────────────────────────────────────
        if (ImGui::BeginTabItem(T("TIEN TE", "ECONOMY"))) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.6f,0.6f,0.6f,1.0f),
                T("  Gold/Grenade/Medical -> 999,999,999",
                  "  Gold/Grenade/Medical -> 999,999,999"));
            ImGui::Spacing();

            ImGui::Checkbox(T("Dong Bang Tien",     "Freeze Money"),    &bFreezeMoney);
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::TextColored(ImVec4(1.0f,0.75f,0.0f,1.0f),
                               T("  Vat Pham","  Items"));
            ImGui::Spacing();

            ImGui::Checkbox(T("Mo Khoa Tat Ca Sung","Unlock All Weapons"),&bUnlockWeapon);
            ImGui::Spacing();

            ImGui::Checkbox(T("Bo Quang Cao",        "No Ads"),          &bNoAds);

            ImGui::EndTabItem();
        }

        // ──────────────────────────────────────────────────────
        //  TAB: DINH VI / ESP
        // ──────────────────────────────────────────────────────
        if (ImGui::BeginTabItem(T("DINH VI", "ESP"))) {
            ImGui::Spacing();

            ImGui::Checkbox(T("Hop Zombie",     "Enemy Box"),    &bEspBox);
            ImGui::Spacing();

            ImGui::Checkbox(T("Duong Den Dich", "Enemy Line"),   &bEspLine);
            ImGui::Spacing();

            ImGui::Checkbox(T("Khoang Cach",    "Distance"),     &bEspDist);
            ImGui::Spacing();

            ImGui::Checkbox(T("Thanh Mau HP",   "HP Bar"),       &bEspHp);
            ImGui::Spacing();

            ImGui::TextColored(ImVec4(0.6f,0.6f,0.6f,1.0f),
                T("  Xanh=xa  Vang=gan  Do=sat",
                  "  Green=far  Yellow=near  Red=close"));

            ImGui::EndTabItem();
        }

        // ──────────────────────────────────────────────────────
        //  TAB: AIM
        // ──────────────────────────────────────────────────────
        if (ImGui::BeginTabItem("AIM")) {
            ImGui::Spacing();

            ImGui::Checkbox(T("Vong Tron Aim","Aim Circle"), &bAimCircle);
            if (bAimCircle) {
                ImGui::SetNextItemWidth(-1);
                ImGui::SliderFloat(T("Pham Vi px","FOV px"),
                                   &AimFov, 30.0f, 600.0f, "%.0fpx");
            }

            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.6f,0.6f,0.6f,1.0f),
                T("  Tip: Aim thu cong vao trong vong tron",
                  "  Tip: Manually aim inside the circle"));

            ImGui::EndTabItem();
        }

        // ──────────────────────────────────────────────────────
        //  TAB: CAI DAT / SETTING
        // ──────────────────────────────────────────────────────
        if (ImGui::BeginTabItem(T("CAI DAT","SETTING"))) {
            ImGui::Spacing();

            const char* langs[] = { "Tieng Viet", "English" };
            ImGui::SetNextItemWidth(-1);
            ImGui::Combo(T("Ngon ngu","Language"), &lang_idx, langs, 2);

            ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "  AXIOM DEV");
            ImGui::TextColored(ImVec4(0.4f, 0.4f, 0.4f, 1.0f),
                               "  Zombie3D All-In-One v1.0");
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.3f,0.3f,0.3f,1.0f),
                "  Confirmed offsets from dump.cs:");
            ImGui::TextColored(ImVec4(0.3f,0.3f,0.3f,1.0f),
                "  ApplyDamage 0x119235C | ESP 0x1191E10");
            ImGui::TextColored(ImVec4(0.3f,0.3f,0.3f,1.0f),
                "  GetCurrency 0x12BF2FC");

            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }
    ImGui::End();
}

// ================================================================
//  HACK THREAD
// ================================================================
static void* hack_thread(void*) {
    initModMenu((void*)DrawMenu);

    do { sleep(1); }
    while (getAbsoluteAddress("libil2cpp.so", 0) == 0);
    sleep(5);

    // ── Unity API ──────────────────────────────────────────────
    // NOTE: offset nay tu template goc — can xac nhan voi build
    Camera_get_main = (void*(*)())
        getAbsoluteAddress("libil2cpp.so", 0x2b6dc50);
    Camera_WorldToScreenPoint = (Vector3(*)(void*, Vector3))
        getAbsoluteAddress("libil2cpp.so", 0x2b6da24);
    Component_get_transform = (void*(*)(void*))
        getAbsoluteAddress("libil2cpp.so", 0x2b95be4);
    Transform_get_position = (Vector3(*)(void*))
        getAbsoluteAddress("libil2cpp.so", 0x2ba1fdc);

    // ── Hook: Anti-Cheat (placeholder) ─────────────────────────
    DobbyHook((void*)getAbsoluteAddress("libil2cpp.so", 0x2a93d74),
              (void*)hook_StartDetection, (void**)&old_StartDetection);

    // ── Hook: God Mode + One Shot Kill ─────────────────────────
    // CharacterDamage.ApplyDamage — RVA CONFIRMED tu dump: 0x119235C
    DobbyHook((void*)getAbsoluteAddress("libil2cpp.so", 0x119235C),
              (void*)hook_ApplyDamage, (void**)&old_ApplyDamage);

    // ── Hook: Infinite Ammo ────────────────────────────────────
    // TODO: tim RVA HaveAmmo/DecreaseBullets trong WeaponBehavior
    //       Offset ben duoi tu template — can kiem tra lai
    DobbyHook((void*)getAbsoluteAddress("libil2cpp.so", 0x23ff1e8),
              (void*)hook_HaveAmmo, (void**)&old_HaveAmmo);
    DobbyHook((void*)getAbsoluteAddress("libil2cpp.so", 0x23ff128),
              (void*)hook_DecreaseBullets, (void**)&old_DecreaseBullets);

    // ── Hook: No Recoil ────────────────────────────────────────
    // TODO: tim RVA CalcSpread hoac GetRecoil tu GunScript
    DobbyHook((void*)getAbsoluteAddress("libil2cpp.so", 0x22f9678),
              (void*)hook_CalcSpread, (void**)&old_CalcSpread);

    // ── Hook: Speed Hack ───────────────────────────────────────
    // TODO: tim RVA FPSRigidBodyWalker speed getter
    DobbyHook((void*)getAbsoluteAddress("libil2cpp.so", 0x22ebe84),
              (void*)hook_GetMoveSpeed, (void**)&old_GetMoveSpeed);

    // ── Hook: Freeze Money ─────────────────────────────────────
    // ItemDataManager.GetCurrency — RVA CONFIRMED tu dump: 0x12BF2FC
    DobbyHook((void*)getAbsoluteAddress("libil2cpp.so", 0x12BF2FC),
              (void*)hook_GetCurrency, (void**)&old_GetCurrency);

    // ── Hook: No Ads ───────────────────────────────────────────
    // LoadingOnces isNoAds getter — TODO: tim RVA chinh xac
    DobbyHook((void*)getAbsoluteAddress("libil2cpp.so", 0x230ddb0),
              (void*)hook_GetIsNoAds, (void**)&old_GetIsNoAds);

    // ── Hook: Unlock All Weapons ───────────────────────────────
    // LoadingOnces unLockAllWeapon getter — TODO: tim RVA chinh xac
    DobbyHook((void*)getAbsoluteAddress("libil2cpp.so", 0x230dc00),
              (void*)hook_GetUnlockWeapon, (void**)&old_GetUnlockWeapon);

    // ── Hook: ESP (CharacterDamage.Update) ─────────────────────
    // RVA CONFIRMED tu dump: 0x1191E10
    // Doc AIComponent@0x30, hitPoints@0x40, initialHits@0x44, myTransform@0xF0
    DobbyHook((void*)getAbsoluteAddress("libil2cpp.so", 0x1191E10),
              (void*)hook_CharDmgUpdate, (void**)&old_CharDmgUpdate);

    pthread_exit(nullptr);
}

// ================================================================
//  ENTRY POINT
// ================================================================
extern "C" {
    JavaVM* jvm = nullptr;
    JNIEnv* env = nullptr;
    __attribute__((visibility("default")))
    jint loadJNI(JavaVM* vm) {
        jvm = vm; vm->AttachCurrentThread(&env, nullptr);
        return JNI_VERSION_1_6;
    }
}

__attribute__((constructor))
void init() {
    pthread_t t;
    pthread_create(&t, nullptr, hack_thread, nullptr);
    RemapTools::RemapLibrary("libLoader.so");
}


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
#include <vector>
#include <cmath>
#include <cstdarg>
#include <sys/time.h>
#include <cstdint>
#include <android/log.h>

#define LOG_TAG "AXIOM"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,LOG_TAG,__VA_ARGS__)

// ================================================================
//  UTILS
// ================================================================
static int lang_idx = 0;
static const char* T(const char* vn, const char* en){ return lang_idx==0?vn:en; }

static long now_ms(){
    struct timeval tv; gettimeofday(&tv,NULL);
    return (long)tv.tv_sec*1000 + tv.tv_usec/1000;
}

static float g_fps=0.f; static int g_fpsFrames=0; static long g_fpsLast=0;

// ================================================================
//  STRUCTS
// ================================================================
struct Vec2{ float x,y; };
struct Vec3{ float x,y,z; };

// NotifierProperty<T>._value is at offset 0x30
// Layout (ARM64 IL2CPP, field order from dump):
//  0x10 OnValueChanged (ref, 8B)
//  0x18 _args          (ref, 8B)
//  0x20 _dispatchEventMode (ref or enum, 8B padded)
//  0x28 _onChangeAction (ref, 8B)
//  0x30 _value         (T — bool/ushort/enum/Vec3)
#define NOTIFIER_VALUE_OFFSET 0x30

// ================================================================
//  TOGGLES
// ================================================================
static bool bInfAmmo   = false;
static bool bNoReload  = false;
static bool bNoRecoil  = false;
static bool bSpeedHack = false;
static bool bFly       = false;
static bool bFastFire  = false;
static bool bESP       = false;
static bool bESP_Box   = true;
static bool bESP_Snap  = true;
static bool bESP_Dist  = true;
static bool bESP_Head  = true;
static bool bESP_Plr   = true;
static bool bESP_Zom   = true;
static bool bSoftCoin  = false;  // display only
static bool bHardCoin  = false;  // display only

static float speedMult   = 2.0f;
static float flySpeed    = 8.0f;
static float fastFireMs  = 0.05f; // seconds between shots
static float espMaxDist  = 200.f;

// ================================================================
//  CACHED POINTERS (written by hooks, read by draw)
// ================================================================
static void* g_weaponModel    = nullptr; // ModelWeapon* from ViewMyWeapon.Init
static void* g_charSettings   = nullptr; // CharacterSettings* from ClientMyCharacter

// ── Camera data (written per-frame in LateUpdate hook) ────────────
static Vec3  g_camPos  = {0,0,0};
static Vec3  g_camFwd  = {0,0,1};
static Vec3  g_camRgt  = {1,0,0};
static Vec3  g_camUp   = {0,1,0};
static float g_camFov  = 60.f;
static float g_scrW    = 1080.f;
static float g_scrH    = 2400.f;

// ── ESP entity cache ──────────────────────────────────────────────
struct ESPEntry {
    void* inst;
    bool  isZombie;
    bool  isDead;
    Vec3  foot;
    Vec3  head;
    float dist;
};
static std::mutex            g_espMtx;
static std::vector<ESPEntry> g_espList;
static std::vector<ESPEntry> g_espDraw;

// ── IL2CPP resolved function pointers ────────────────────────────
typedef void  (*FnVec3Out) (void* self, Vec3* ret);
typedef void* (*FnObjOut)  (void* self);
typedef void* (*FnGetClass)(void* obj);

static FnVec3Out  fn_tr_pos  = nullptr; // Transform.get_position
static FnVec3Out  fn_tr_fwd  = nullptr; // Transform.get_forward
static FnVec3Out  fn_tr_rgt  = nullptr; // Transform.get_right
static FnVec3Out  fn_tr_up   = nullptr; // Transform.get_up
static FnObjOut   fn_comp_tr = nullptr; // Component.get_transform
static FnGetClass fn_obj_cls = nullptr; // il2cpp_object_get_class

static void* g_cls_ViewCharacter = nullptr;
static void* g_cls_ViewZombie    = nullptr;
static bool  g_esp_ready         = false;

// ================================================================
//  IL2CPP THREAD ATTACH
// ================================================================
static std::atomic<bool> g_attached{false};
static void EnsureAttached(){
    if(g_attached.load()) return;
    void* lib = dlopen("libil2cpp.so", RTLD_LAZY|RTLD_NOLOAD);
    if(!lib) return;
    auto dg = (void*(*)())      dlsym(lib,"il2cpp_domain_get");
    auto ta = (void*(*)(void*)) dlsym(lib,"il2cpp_thread_attach");
    if(dg && ta){ ta(dg()); g_attached.store(true); }
}

// ================================================================
//  IL2CPP RESOLVE (called in thread after sleep)
// ================================================================
static void InitIL2CPP(){
    void* lib = dlopen("libil2cpp.so", RTLD_LAZY|RTLD_NOLOAD);
    if(!lib){ LOGI("[AXIOM] libil2cpp not found"); return; }

    typedef void* (*DG)();
    typedef void* (*AO)(void*,const char*);
    typedef void* (*AG)(void*);
    typedef void* (*CF)(void*,const char*,const char*);
    typedef void* (*CM)(void*,const char*,int);
    typedef void* (*MF)(const void*);

    auto dget  = (DG) dlsym(lib,"il2cpp_domain_get");
    auto aopen = (AO) dlsym(lib,"il2cpp_domain_assembly_open");
    auto aimgt = (AG) dlsym(lib,"il2cpp_assembly_get_image");
    auto cfn   = (CF) dlsym(lib,"il2cpp_class_from_name");
    auto cmeth = (CM) dlsym(lib,"il2cpp_class_get_method_from_name");
    auto mfptr = (MF) dlsym(lib,"il2cpp_method_get_function_pointer");
    fn_obj_cls = (FnGetClass) dlsym(lib,"il2cpp_object_get_class");

    if(!dget||!aopen||!aimgt||!cfn||!cmeth||!mfptr){
        LOGI("[AXIOM] il2cpp sym fail"); return;
    }

    void* dom = dget();

    // ── UnityEngine.CoreModule ────────────────────────────────────
    void* coreAsm = aopen(dom,"UnityEngine.CoreModule");
    void* coreImg = coreAsm ? aimgt(coreAsm) : nullptr;
    if(coreImg){
        void* tCls = cfn(coreImg,"UnityEngine","Transform");
        if(tCls){
            auto R=[&](const char* n)->FnVec3Out{
                void* m=cmeth(tCls,n,0);
                return m?(FnVec3Out)mfptr(m):nullptr;
            };
            fn_tr_pos = R("get_position");
            fn_tr_fwd = R("get_forward");
            fn_tr_rgt = R("get_right");
            fn_tr_up  = R("get_up");
        }
        void* cCls = cfn(coreImg,"UnityEngine","Component");
        if(cCls){
            void* m=cmeth(cCls,"get_transform",0);
            fn_comp_tr = m?(FnObjOut)mfptr(m):nullptr;
        }
    }
    LOGI("[AXIOM] Transform: pos=%p fwd=%p rgt=%p up=%p comp_tr=%p",
         fn_tr_pos,fn_tr_fwd,fn_tr_rgt,fn_tr_up,fn_comp_tr);

    // ── Assembly-CSharp (ViewCharacter + ViewZombie) ──────────────
    void* gameAsm = aopen(dom,"Assembly-CSharp");
    void* gameImg = gameAsm ? aimgt(gameAsm) : nullptr;
    if(gameImg){
        // Namespace "Client.Views" from dump TypeDefIndex 1027, 1033
        g_cls_ViewCharacter = cfn(gameImg,"Client.Views","ViewCharacter");
        g_cls_ViewZombie    = cfn(gameImg,"Client.Views","ViewZombie");
    }
    LOGI("[AXIOM] ViewCharacter=%p ViewZombie=%p",
         g_cls_ViewCharacter, g_cls_ViewZombie);

    g_esp_ready = (fn_tr_pos != nullptr);
    LOGI("[AXIOM] IL2CPP init done. ESP ready=%d", g_esp_ready);
}

// ================================================================
//  HELPERS
// ================================================================
static inline Vec3 GetTransformPos(void* transform){
    Vec3 v{0,0,0};
    if(fn_tr_pos && transform) fn_tr_pos(transform,&v);
    return v;
}
static inline Vec3 GetTransformFwd(void* transform){
    Vec3 v{0,0,1};
    if(fn_tr_fwd && transform) fn_tr_fwd(transform,&v);
    return v;
}
static inline Vec3 GetTransformRgt(void* transform){
    Vec3 v{1,0,0};
    if(fn_tr_rgt && transform) fn_tr_rgt(transform,&v);
    return v;
}
static inline Vec3 GetTransformUp(void* transform){
    Vec3 v{0,1,0};
    if(fn_tr_up && transform) fn_tr_up(transform,&v);
    return v;
}

// ── NotifierProperty read/write helpers ───────────────────────────
static inline uint16_t NP_GetUShort(void* np){
    if(!np) return 0;
    return *(uint16_t*)((uint8_t*)np + NOTIFIER_VALUE_OFFSET);
}
static inline void NP_SetUShort(void* np, uint16_t val){
    if(!np) return;
    *(uint16_t*)((uint8_t*)np + NOTIFIER_VALUE_OFFSET) = val;
}
static inline bool NP_GetBool(void* np){
    if(!np) return false;
    return *(bool*)((uint8_t*)np + NOTIFIER_VALUE_OFFSET);
}
static inline void NP_SetBool(void* np, bool val){
    if(!np) return;
    *(bool*)((uint8_t*)np + NOTIFIER_VALUE_OFFSET) = val;
}
static inline uint32_t NP_GetUInt(void* np){
    if(!np) return 0;
    return *(uint32_t*)((uint8_t*)np + NOTIFIER_VALUE_OFFSET);
}
static inline void NP_SetUInt(void* np, uint32_t val){
    if(!np) return;
    *(uint32_t*)((uint8_t*)np + NOTIFIER_VALUE_OFFSET) = val;
}

// ================================================================
//  WORLD TO SCREEN
//  Manual MVP — uses camera data filled every frame in LateUpdate
// ================================================================
static bool W2S(Vec3 world, Vec2& out){
    float dx = world.x - g_camPos.x;
    float dy = world.y - g_camPos.y;
    float dz = world.z - g_camPos.z;

    float fwd_d = dx*g_camFwd.x + dy*g_camFwd.y + dz*g_camFwd.z;
    if(fwd_d < 0.01f) return false;

    float rgt_d = dx*g_camRgt.x + dy*g_camRgt.y + dz*g_camRgt.z;
    float up_d  = dx*g_camUp.x  + dy*g_camUp.y  + dz*g_camUp.z;

    float tanFov = tanf(g_camFov * 0.5f * 3.14159265f / 180.f);
    float aspect = (g_scrH > 0.f) ? g_scrW/g_scrH : 0.45f;

    out.x = g_scrW*0.5f * (1.f + rgt_d / (fwd_d * tanFov * aspect));
    out.y = g_scrH*0.5f * (1.f - up_d  / (fwd_d * tanFov));
    return true;
}

// ================================================================
//  HOOK 1: ClientMyCharacter.LateUpdate — RVA: 0x1A3F720
//  ── Camera data capture
//  ── Speed / Fly via MoveVelocity + Gravity patch
//  ── Ammo via ModelWeapon NotifierProperty
//  ── ESP draw buffer swap
//  Fields (confirmed dump.cs TypeDefIndex 1050):
//    _cameraPivot   Transform  0x58
//    _charSettings  CharSet    0x88
//    MovableComp             0xA0
//    _modelCharacter         0xA8
// ================================================================
static void (*old_LateUpdate)(void* inst);
static void hook_LateUpdate(void* inst){
    if(old_LateUpdate) old_LateUpdate(inst);
    if(!inst) return;

    // ── Cache CharacterSettings once ──────────────────────────────
    if(!g_charSettings)
        g_charSettings = *(void**)((uint8_t*)inst + 0x88);

    // ── Camera data ────────────────────────────────────────────────
    void* camPivot = *(void**)((uint8_t*)inst + 0x58);
    if(camPivot && g_esp_ready){
        // _cameraPivot IS a Transform directly
        g_camPos = GetTransformPos(camPivot);
        g_camFwd = GetTransformFwd(camPivot);
        g_camRgt = GetTransformRgt(camPivot);
        g_camUp  = GetTransformUp(camPivot);
    }

    // ── MovableCharacterComponent ──────────────────────────────────
    void* mover = *(void**)((uint8_t*)inst + 0xA0);
    if(mover){
        Vec3* mv = (Vec3*)((uint8_t*)mover + 0x5C); // MoveVelocity backing

        if(bSpeedHack){
            mv->x *= speedMult;
            mv->z *= speedMult;
        }
        if(bFly){
            // Zero gravity via CharacterSettings.Gravity (+0x50)
            if(g_charSettings)
                *(float*)((uint8_t*)g_charSettings + 0x50) = 0.f;
            // Drive upward Y — read jump input from IsJumped NotifierProperty
            void* jumpedNP = *(void**)((uint8_t*)mover + 0x78);
            bool  isJumped = NP_GetBool(jumpedNP);
            mv->y = isJumped ? flySpeed : 0.f;
        }
        // Restore gravity when fly disabled
        if(!bFly && g_charSettings){
            float* grav = (float*)((uint8_t*)g_charSettings + 0x50);
            if(*grav == 0.f) *grav = -20.f; // game default gravity
        }
    }

    // ── Ammo / Reload / FireRate via ModelWeapon ───────────────────
    if(g_weaponModel){
        // ModelWeapon fields (TypeDefIndex 1703):
        //  State        NotifierProperty<WeaponState>  0x38
        //  Bullets      NotifierProperty<ushort>        0x40
        //  BulletsInHolder NotifierProperty<ushort>     0x48
        void* stateNP   = *(void**)((uint8_t*)g_weaponModel + 0x38);
        void* bulletsNP = *(void**)((uint8_t*)g_weaponModel + 0x40);
        void* holderNP  = *(void**)((uint8_t*)g_weaponModel + 0x48);

        if(bInfAmmo){
            NP_SetUShort(bulletsNP, 9999);
            NP_SetUShort(holderNP,  9999);
        }
        if(bNoReload && stateNP){
            // WeaponState enum — Reloading is typically 2
            // If state == 2 (Reloading), force to 0 (Ready)
            uint32_t st = NP_GetUInt(stateNP);
            if(st == 2) NP_SetUInt(stateNP, 0);
        }
        if(bFastFire){
            // Fast fire: keep IsShooting=true so full-auto logic never pauses
            void* isShootingNP = *(void**)((uint8_t*)g_weaponModel + 0x50);
            NP_SetBool(isShootingNP, true);
        }
    }

    // ── ESP draw buffer swap ───────────────────────────────────────
    if(bESP){
        std::lock_guard<std::mutex> lk(g_espMtx);
        g_espDraw = g_espList;
        if(g_espList.size() > 128) g_espList.clear();
    }
}

// ================================================================
//  HOOK 2: ViewMyWeapon.Init — RVA: 0x1A45A0C
//  void Init(ViewMyHand hand, ModelWeapon model, DefinitionWeapon def)
//  ARM64: x0=inst, x1=hand, x2=model, x3=def
//  WeaponModel backing at inst+0x90
// ================================================================
typedef void (*WeaponInitFn)(void*,void*,void*,void*);
static WeaponInitFn old_WeaponInit = nullptr;
static void hook_WeaponInit(void* inst, void* hand, void* model, void* def){
    if(old_WeaponInit) old_WeaponInit(inst,hand,model,def);
    g_weaponModel = model;
    LOGI("[AXIOM] WeaponModel cached: %p", model);
}

// ================================================================
//  HOOK 3 & 4: No Recoil — skip recoil coroutine start
//  3a. ComponentRecoilForFastShootingViewMyWeapon.HandleChangeShooting
//      RVA: 0x1A4080C — event handler, skipping prevents RecoilProcess launch
//  3b. ComponentRecoilForSlowShootingViewMyWeapon.HandleShoot
//      RVA: 0x1A40CFC
// ================================================================
typedef void (*RecoilHandlerFn)(void*, void*, void*);
static RecoilHandlerFn old_RecoilFastHandler = nullptr;
static void hook_RecoilFastHandler(void* inst, void* sender, void* args){
    if(bNoRecoil) return; // drop — coroutine never starts
    if(old_RecoilFastHandler) old_RecoilFastHandler(inst,sender,args);
}
static RecoilHandlerFn old_RecoilSlowHandler = nullptr;
static void hook_RecoilSlowHandler(void* inst, void* sender, void* args){
    if(bNoRecoil) return;
    if(old_RecoilSlowHandler) old_RecoilSlowHandler(inst,sender,args);
}

// ================================================================
//  HOOK 5 & 6: No Recoil — zero out yRecoilMovement inside running coroutines
//  Even if a coroutine is already mid-way, we zero every MoveNext tick
//  5. <RecoilProcess>d__13.MoveNext (fast) RVA: 0x1A40928
//     yRecoilMovement at coroutine state +0x2C (TypeDefIndex 1059)
//  6. <RecoilProcess>d__11.MoveNext (slow) RVA: 0x1A40F64
//     yRecoilMovement at coroutine state +0x2C (TypeDefIndex 1062)
// ================================================================
typedef bool (*MoveNextFn)(void*);
static MoveNextFn old_RecoilFastMN = nullptr;
static bool hook_RecoilFastMN(void* inst){
    bool r = old_RecoilFastMN ? old_RecoilFastMN(inst) : false;
    if(bNoRecoil && inst){
        *(float*)((uint8_t*)inst + 0x2C) = 0.f; // yRecoilMovement
        *(float*)((uint8_t*)inst + 0x34) = 0.f; // currentMoveValue
    }
    return r;
}
static MoveNextFn old_RecoilSlowMN = nullptr;
static bool hook_RecoilSlowMN(void* inst){
    bool r = old_RecoilSlowMN ? old_RecoilSlowMN(inst) : false;
    if(bNoRecoil && inst){
        *(float*)((uint8_t*)inst + 0x2C) = 0.f;
        *(float*)((uint8_t*)inst + 0x34) = 0.f;
    }
    return r;
}

// ================================================================
//  HOOK 7: ViewCharacterBase.Update — entity collection for ESP
//  RVA: 0x1A3B170 — TypeDefIndex: 1032
//  Fields: ViewModel@0x50 (ViewCharacterModel*), _pivotHead@0xA8 (Transform)
//  ViewCharacterModel.IsDead @ 0x29 (bool)
// ================================================================
static void (*old_VCBUpdate)(void* inst);
static void hook_VCBUpdate(void* inst){
    if(old_VCBUpdate) old_VCBUpdate(inst);
    if(!inst || !bESP || !g_esp_ready) return;

    // Type check
    bool isZombie = false;
    if(fn_obj_cls){
        void* cls = fn_obj_cls(inst);
        if(cls == g_cls_ViewZombie)    isZombie = true;
        else if(cls != g_cls_ViewCharacter) return; // skip unknown
    }
    if(isZombie && !bESP_Zom) return;
    if(!isZombie && !bESP_Plr) return;

    // Read IsDead from ViewModel (ptr at +0x50) -> offset 0x29
    void* viewModel = *(void**)((uint8_t*)inst + 0x50);
    bool  isDead    = viewModel ? *(bool*)((uint8_t*)viewModel + 0x29) : false;
    if(isDead) return;

    // Foot position via root transform (MonoBehaviour.gameObject.transform)
    void* rootTr = fn_comp_tr ? fn_comp_tr(inst) : nullptr;
    Vec3  foot   = GetTransformPos(rootTr);

    // Head via _pivotHead transform at +0xA8
    void* headTr = *(void**)((uint8_t*)inst + 0xA8);
    Vec3  head   = headTr ? GetTransformPos(headTr)
                          : Vec3{foot.x, foot.y + 1.75f, foot.z};

    // Distance cull
    float dx=foot.x-g_camPos.x, dy=foot.y-g_camPos.y, dz=foot.z-g_camPos.z;
    float dist = sqrtf(dx*dx + dy*dy + dz*dz);
    if(dist > espMaxDist) return;

    // Upsert
    std::lock_guard<std::mutex> lk(g_espMtx);
    for(auto& e : g_espList){
        if(e.inst == inst){
            e.foot=foot; e.head=head; e.dist=dist; e.isDead=isDead;
            return;
        }
    }
    g_espList.push_back({inst,isZombie,isDead,foot,head,dist});
}

// ================================================================
//  HOOK 8: ViewCharacterBase.Kill — remove dead entities
//  RVA: 0x1A3AE34
// ================================================================
static void (*old_VCBKill)(void* inst);
static void hook_VCBKill(void* inst){
    if(old_VCBKill) old_VCBKill(inst);
    if(!inst) return;
    std::lock_guard<std::mutex> lk(g_espMtx);
    g_espList.erase(
        std::remove_if(g_espList.begin(), g_espList.end(),
                       [inst](const ESPEntry& e){ return e.inst==inst; }),
        g_espList.end());
}

// ================================================================
//  HOOK 9 & 10: Currency display
//  GetSoftCurrency RVA: 0x1999A1C | GetHardCurrency RVA: 0x199A63C
//  NOTE: display only — server (PostgreSQL) holds real value
// ================================================================
static int (*old_GetSoftCoin)(void* inst);
static int hook_GetSoftCoin(void* inst){
    if(bSoftCoin) return 999999999;
    return old_GetSoftCoin ? old_GetSoftCoin(inst) : 0;
}
static int (*old_GetHardCoin)(void* inst);
static int hook_GetHardCoin(void* inst){
    if(bHardCoin) return 999999999;
    return old_GetHardCoin ? old_GetHardCoin(inst) : 0;
}

// ================================================================
//  ESP DRAW
// ================================================================
static void DrawESP(){
    if(!bESP || !g_esp_ready || g_espDraw.empty()) return;

    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    if(!dl) return;

    const float SW = g_scrW, SH = g_scrH;
    const ImU32 COL_PLR    = IM_COL32( 30,220, 60,230); // green
    const ImU32 COL_ZOM    = IM_COL32(220, 40, 40,230); // red
    const ImU32 COL_CORNER = IM_COL32(255,255,255,240);
    const ImU32 COL_SNAP   = IM_COL32(  0,200,255,140);
    const ImU32 COL_DIST   = IM_COL32(255,255,180,210);
    const ImU32 COL_SHADOW = IM_COL32(  0,  0,  0,180);

    for(const auto& e : g_espDraw){
        if(e.isDead) continue;

        Vec2 sFoot, sHead;
        if(!W2S(e.foot, sFoot)) continue;
        if(!W2S(e.head, sHead)) continue;

        float boxH = fabsf(sFoot.y - sHead.y);
        if(boxH < 5.f || boxH > SH) continue;

        float boxW = boxH * 0.45f;
        float x1   = sFoot.x - boxW * 0.5f;
        float y1   = sHead.y;
        float x2   = x1 + boxW;
        float y2   = sFoot.y;

        ImU32 col = e.isZombie ? COL_ZOM : COL_PLR;

        // ── Box ─────────────────────────────────────────────────
        if(bESP_Box){
            float th = 1.5f;
            // Full outline
            dl->AddRect(ImVec2(x1,y1), ImVec2(x2,y2), col, 0.f, 0, th);
            // Corner accents
            float ca = boxW * 0.25f;
            float ct = th + 1.f;
            // TL
            dl->AddLine(ImVec2(x1,y1),     ImVec2(x1+ca,y1),    COL_CORNER,ct);
            dl->AddLine(ImVec2(x1,y1),     ImVec2(x1,y1+ca),    COL_CORNER,ct);
            // TR
            dl->AddLine(ImVec2(x2,y1),     ImVec2(x2-ca,y1),    COL_CORNER,ct);
            dl->AddLine(ImVec2(x2,y1),     ImVec2(x2,y1+ca),    COL_CORNER,ct);
            // BL
            dl->AddLine(ImVec2(x1,y2),     ImVec2(x1+ca,y2),    COL_CORNER,ct);
            dl->AddLine(ImVec2(x1,y2),     ImVec2(x1,y2-ca),    COL_CORNER,ct);
            // BR
            dl->AddLine(ImVec2(x2,y2),     ImVec2(x2-ca,y2),    COL_CORNER,ct);
            dl->AddLine(ImVec2(x2,y2),     ImVec2(x2,y2-ca),    COL_CORNER,ct);
        }

        // ── Head dot ────────────────────────────────────────────
        if(bESP_Head){
            float hy = sHead.y - 4.f;
            dl->AddCircleFilled(ImVec2(sHead.x,hy), 4.5f, col);
            dl->AddCircle      (ImVec2(sHead.x,hy), 4.5f, COL_CORNER,16,1.5f);
        }

        // ── Snapline ────────────────────────────────────────────
        if(bESP_Snap)
            dl->AddLine(ImVec2(SW*0.5f,SH), ImVec2(sFoot.x,sFoot.y),
                        COL_SNAP, 1.f);

        // ── Distance ────────────────────────────────────────────
        if(bESP_Dist){
            char buf[32];
            snprintf(buf,sizeof(buf),"%s  %.0fm",
                     e.isZombie?"[Z]":"[P]", e.dist);
            float tx = x1 + boxW*0.5f - 20.f;
            float ty = y2 + 3.f;
            dl->AddText(ImVec2(tx+1,ty+1), COL_SHADOW, buf);
            dl->AddText(ImVec2(tx,  ty  ), COL_DIST,   buf);
        }
    }
}

// ================================================================
//  MENU DRAW
// ================================================================
static void DrawMenu(){
    EnsureAttached();

    // FPS
    g_fpsFrames++;
    long nowT = now_ms();
    if(nowT - g_fpsLast >= 1000){
        g_fps       = g_fpsFrames * 1000.f / (float)(nowT - g_fpsLast);
        g_fpsFrames = 0;
        g_fpsLast   = nowT;
    }

    // Draw ESP behind the UI
    DrawESP();

    // Style
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding  = 10; s.FrameRounding  = 5; s.GrabRounding  = 4;
    s.ItemSpacing     = ImVec2(8,7);
    s.WindowPadding   = ImVec2(12,12);
    s.Colors[ImGuiCol_WindowBg]       = ImVec4(.03f,.03f,.07f,.97f);
    s.Colors[ImGuiCol_TitleBg]        = ImVec4(.00f,.10f,.25f,1.f);
    s.Colors[ImGuiCol_TitleBgActive]  = ImVec4(.00f,.18f,.48f,1.f);
    s.Colors[ImGuiCol_FrameBg]        = ImVec4(.06f,.06f,.13f,1.f);
    s.Colors[ImGuiCol_CheckMark]      = ImVec4(.00f,.85f,1.0f,1.f);
    s.Colors[ImGuiCol_Tab]            = ImVec4(.04f,.08f,.18f,1.f);
    s.Colors[ImGuiCol_TabActive]      = ImVec4(.00f,.18f,.48f,1.f);
    s.Colors[ImGuiCol_TabHovered]     = ImVec4(.00f,.28f,.68f,1.f);
    s.Colors[ImGuiCol_SliderGrab]     = ImVec4(.00f,.65f,1.0f,1.f);
    s.Colors[ImGuiCol_SliderGrabActive]=ImVec4(.00f,.90f,1.0f,1.f);
    s.Colors[ImGuiCol_Button]         = ImVec4(.00f,.22f,.52f,1.f);
    s.Colors[ImGuiCol_ButtonHovered]  = ImVec4(.00f,.38f,.78f,1.f);
    s.Colors[ImGuiCol_Separator]      = ImVec4(.12f,.12f,.22f,1.f);
    s.Colors[ImGuiCol_Header]         = ImVec4(.00f,.18f,.40f,.9f);
    s.Colors[ImGuiCol_HeaderHovered]  = ImVec4(.00f,.28f,.60f,.9f);

    ImGui::SetNextWindowSize(ImVec2(340,520), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos (ImVec2(10,10),   ImGuiCond_FirstUseEver);

    ImGui::Begin("  AXIOM MOD  |  v1.0  ", nullptr, 0);

    // Header
    ImGui::TextColored(ImVec4(.0f,.85f,1.f,1.f)," AXIOM DEVELOPMENT");
    ImGui::SameLine();
    char fb[32]; snprintf(fb,32,"  [%.0f FPS]", g_fps);
    ImGui::TextColored(
        g_fps>=60 ? ImVec4(.2f,1.f,.2f,1.f) :
        g_fps>=30 ? ImVec4(1.f,.75f,.1f,1.f) :
                    ImVec4(1.f,.2f,.2f,1.f), "%s", fb);

    ImGui::TextColored(ImVec4(.35f,.35f,.55f,1.f),
        " NoAntiCheat | Server: PostgreSQL | All offsets verified");
    ImGui::Separator(); ImGui::Spacing();

    if(ImGui::BeginTabBar("##tabs")){

        // ────────────────────────────────────────────────────────
        // TAB: CHIEN DAU / COMBAT
        // ────────────────────────────────────────────────────────
        if(ImGui::BeginTabItem(T("CHIEN DAU","COMBAT"))){
            ImGui::Spacing();

            // ── Weapon ──────────────────────────────────────────
            ImGui::TextColored(ImVec4(.3f,.9f,.3f,1.f),
                T("  [ VU KHI ]","  [ WEAPON ]"));
            ImGui::Spacing();

            ImGui::Checkbox(T("Dan Vo Han","Infinite Ammo"), &bInfAmmo);
            if(bInfAmmo) ImGui::TextColored(ImVec4(.5f,.5f,.5f,1.f),
                T("   ModelWeapon.Bullets + BulletsInHolder = 9999",
                  "   ModelWeapon.Bullets + BulletsInHolder = 9999"));
            ImGui::Spacing();

            ImGui::Checkbox(T("Khong Nan Dan","No Reload"), &bNoReload);
            if(bNoReload) ImGui::TextColored(ImVec4(.5f,.5f,.5f,1.f),
                T("   WeaponState.Reloading(2) -> Ready(0)",
                  "   WeaponState.Reloading(2) -> Ready(0)"));
            ImGui::Spacing();

            ImGui::Checkbox(T("Khong Giat Sung","No Recoil"), &bNoRecoil);
            if(bNoRecoil) ImGui::TextColored(ImVec4(.5f,.5f,.5f,1.f),
                T("   Hook HandleChangeShooting + MoveNext x2",
                  "   Hook HandleChangeShooting + MoveNext x2"));
            ImGui::Spacing();

            ImGui::Checkbox(T("Ban Nhanh","Fast Fire"), &bFastFire);
            if(bFastFire){
                ImGui::TextColored(ImVec4(.5f,.5f,.5f,1.f),
                    T("   Force IsShooting = true",
                      "   Force IsShooting = true"));
            }
            ImGui::Spacing();

            // ── Character ───────────────────────────────────────
            ImGui::Separator(); ImGui::Spacing();
            ImGui::TextColored(ImVec4(.3f,.9f,.3f,1.f),
                T("  [ NHAN VAT ]","  [ CHARACTER ]"));
            ImGui::Spacing();

            ImGui::Checkbox(T("Tang Toc Di Chuyen","Speed Hack"), &bSpeedHack);
            if(bSpeedHack){
                ImGui::SetNextItemWidth(-1);
                ImGui::SliderFloat(T("He So","Multiplier"),
                    &speedMult, 1.f, 6.f, "x%.1f");
                ImGui::TextColored(ImVec4(.5f,.5f,.5f,1.f),
                    T("   Patch MoveVelocity.xz * mult",
                      "   Patch MoveVelocity.xz * mult"));
            }
            ImGui::Spacing();

            ImGui::Checkbox(T("Bay","Fly Hack"), &bFly);
            if(bFly){
                ImGui::SetNextItemWidth(-1);
                ImGui::SliderFloat(T("Toc Do Bay","Fly Speed"),
                    &flySpeed, 1.f, 30.f, "%.0f");
                ImGui::TextColored(ImVec4(.5f,1.f,.5f,1.f),
                    T("  JUMP = len  |  Khong nhan = hover",
                      "  JUMP = up   |  No input = hover"));
                ImGui::TextColored(ImVec4(.5f,.5f,.5f,1.f),
                    T("   Gravity -> 0, Velocity.y = flySpeed",
                      "   Gravity -> 0, Velocity.y = flySpeed"));
            }

            ImGui::EndTabItem();
        }

        // ────────────────────────────────────────────────────────
        // TAB: ESP
        // ────────────────────────────────────────────────────────
        if(ImGui::BeginTabItem("DINH VI")){
            ImGui::Spacing();

            ImGui::Checkbox(T("Bat ESP","Enable ESP"), &bESP);
            if(!g_esp_ready)
                ImGui::TextColored(ImVec4(1.f,.3f,.3f,1.f),
                    T("  [!] IL2CPP chua resolve — ESP chua san sang",
                      "  [!] IL2CPP not resolved yet — ESP not ready"));

            if(bESP){
                ImGui::Separator(); ImGui::Spacing();

                ImGui::TextColored(ImVec4(.3f,.9f,.3f,1.f),
                    T("  [ HIEN THI ]","  [ DISPLAY ]"));
                ImGui::Spacing();
                ImGui::Checkbox(T("Khung Vien ","Box Outline"),     &bESP_Box);
                ImGui::Checkbox(T("Duong Goc (Snapline)","Snapline"),    &bESP_Snap);
                ImGui::Checkbox(T("Khoang Cach","Distance Label"),       &bESP_Dist);
                ImGui::Checkbox(T("Diem Dau","Head Dot"),                &bESP_Head);

                ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
                ImGui::TextColored(ImVec4(.3f,.9f,.3f,1.f),
                    T("  [ LOC DOI TUONG ]","  [ FILTER ]"));
                ImGui::Spacing();
                ImGui::Checkbox(T("Nguoi Choi","Players"), &bESP_Plr);
                ImGui::Checkbox(T("Zombie/Bot","Zombie/Bot"),&bESP_Zom);

                ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
                ImGui::Text(T("Khoang Cach Max (m)","Max Distance (m)"));
                ImGui::SetNextItemWidth(-1);
                ImGui::SliderFloat("##edist", &espMaxDist, 30.f, 500.f, "%.0f m");

                ImGui::Spacing();
                ImGui::TextColored(ImVec4(.4f,.8f,.4f,1.f),
                    " Entities tracked: %d", (int)g_espDraw.size());
            }
            ImGui::EndTabItem();
        }

        // ────────────────────────────────────────────────────────
        // TAB: TAI NGUYEN / RESOURCES
        // ────────────────────────────────────────────────────────
        if(ImGui::BeginTabItem(T("TAI NGUYEN","RESOURCES"))){
            ImGui::Spacing();

            ImGui::TextColored(ImVec4(1.f,.3f,.3f,1.f),
                T("  [!] Server-side (PostgreSQL) — chi hien thi",
                  "  [!] Server-side (PostgreSQL) — display only"));
            ImGui::TextColored(ImVec4(.6f,.6f,.6f,1.f),
                T("  Mua that se that bai do server validate",
                  "  Real purchases fail due to server validation"));
            ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

            ImGui::TextColored(ImVec4(.9f,.8f,.1f,1.f),
                T("  TIEN MEM (Soft)","  SOFT CURRENCY"));
            ImGui::Checkbox(T("Hien Thi 999,999,999 Xu","Display 999M Soft"),
                &bSoftCoin);
            ImGui::Spacing();

            ImGui::TextColored(ImVec4(.9f,.55f,.1f,1.f),
                T("  TIEN CUNG (Hard)","  HARD CURRENCY"));
            ImGui::Checkbox(T("Hien Thi 999,999,999 Kim Cuong","Display 999M Hard"),
                &bHardCoin);
            ImGui::EndTabItem();
        }

        // ────────────────────────────────────────────────────────
        // TAB: SETTINGS
        // ────────────────────────────────────────────────────────
        if(ImGui::BeginTabItem("SETTINGS")){
            ImGui::Spacing();

            // FPS display
            ImGui::TextColored(ImVec4(.5f,1.f,.5f,1.f),
                T("  HIEU NANG","  PERFORMANCE"));
            ImGui::Spacing();
            float fc = g_fps>=60 ? 1.f : g_fps>=30 ? 0.5f : 0.f;
            ImGui::TextColored(ImVec4(1.f-fc,fc,0.f,1.f),"FPS: %.1f", g_fps);
            ImGui::ProgressBar(g_fps/120.f, ImVec2(-1.f,8.f));
            ImGui::TextColored(ImVec4(.5f,.5f,.5f,1.f),
                g_fps>=60 ? T("Rat tot (60+)","Very good (60+)") :
                g_fps>=30 ? T("Binh thuong","Normal (30-60)") :
                            T("Thap, lag (<30)","Low, may lag (<30)"));

            ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

            // Dump / game info
            ImGui::TextColored(ImVec4(.7f,.7f,1.f,1.f),
                T("  THONG TIN GAME","  GAME INFO"));
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(.5f,.8f,.5f,1.f)," AntiCheat  : NONE (client)");
            ImGui::TextColored(ImVec4(.5f,.8f,.5f,1.f)," Network    : Custom UDP");
            ImGui::TextColored(ImVec4(.5f,.8f,.5f,1.f)," Database   : PostgreSQL (server)");
            ImGui::TextColored(ImVec4(.5f,.8f,.5f,1.f)," Engine     : Unity IL2CPP ARM64");
            ImGui::TextColored(ImVec4(.5f,.8f,.5f,1.f),
                " WeaponModel: %s", g_weaponModel?"CACHED":"waiting...");
            ImGui::TextColored(ImVec4(.5f,.8f,.5f,1.f),
                " IL2CPP ESP : %s", g_esp_ready?"READY":"waiting...");

            ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

            // Language
            ImGui::TextColored(ImVec4(.7f,.7f,1.f,1.f),
                T("  NGON NGU","  LANGUAGE"));
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

    // Wait for libil2cpp.so to load
    do{ sleep(1); }while(getAbsoluteAddress("libil2cpp.so",0)==0);
    LOGI("[AXIOM] libil2cpp.so detected — waiting 3s...");
    sleep(3);

    // Resolve IL2CPP APIs for ESP + Transform reads
    InitIL2CPP();

    // ── Dobby hook helper macro ───────────────────────────────────
    #define HOOK(rva, hookFn, origFn) do{ \
        void* _a = (void*)getAbsoluteAddress("libil2cpp.so",(rva)); \
        if(_a){ \
            DobbyHook(_a, (void*)(hookFn), (void**)&(origFn)); \
            LOGI("[AXIOM] HOOK OK  0x%08X  %s", (unsigned)(rva), #hookFn); \
        } else { \
            LOGI("[AXIOM] HOOK MISS 0x%08X  %s", (unsigned)(rva), #hookFn); \
        } \
    }while(0)

    // ── CORE: LateUpdate — handles cam/speed/fly/ammo/fire/swap ──
    // ClientMyCharacter.LateUpdate (TypeDefIndex: 1050)
    // Fields: _cameraPivot@0x58 _charSettings@0x88 MovableComp@0xA0 _modelChar@0xA8
    HOOK(0x1A3F720, hook_LateUpdate,         old_LateUpdate);

    // ── AMMO: cache ModelWeapon via ViewMyWeapon.Init ─────────────
    // ViewMyWeapon.Init(hand,model,def) — TypeDefIndex: 1097
    // WeaponModel backing at inst+0x90
    HOOK(0x1A45A0C, hook_WeaponInit,         old_WeaponInit);

    // ── NO RECOIL: skip handler (fast + slow) ────────────────────
    // ComponentRecoilForFastShootingViewMyWeapon.HandleChangeShooting
    HOOK(0x1A4080C, hook_RecoilFastHandler,  old_RecoilFastHandler);
    // ComponentRecoilForSlowShootingViewMyWeapon.HandleShoot
    HOOK(0x1A40CFC, hook_RecoilSlowHandler,  old_RecoilSlowHandler);

    // ── NO RECOIL: zero MoveNext (catches already-running coroutines)
    // <RecoilProcess>d__13.MoveNext (fast) — yRecoilMovement@0x2C
    HOOK(0x1A40928, hook_RecoilFastMN,       old_RecoilFastMN);
    // <RecoilProcess>d__11.MoveNext (slow) — yRecoilMovement@0x2C
    HOOK(0x1A40F64, hook_RecoilSlowMN,       old_RecoilSlowMN);

    // ── ESP: entity collection ────────────────────────────────────
    // ViewCharacterBase.Update (TypeDefIndex: 1032)
    // ViewModel@0x50, _pivotHead@0xA8
    HOOK(0x1A3B170, hook_VCBUpdate,          old_VCBUpdate);
    // ViewCharacterBase.Kill — remove dead entities
    HOOK(0x1A3AE34, hook_VCBKill,            old_VCBKill);

    // ── CURRENCY: display hooks ───────────────────────────────────
    // ModelPlayerState.GetSoftCurrency (TypeDefIndex: 3061)
    HOOK(0x1999A1C, hook_GetSoftCoin,        old_GetSoftCoin);
    // ModelPlayerState.GetHardCurrency
    HOOK(0x199A63C, hook_GetHardCoin,        old_GetHardCoin);

    #undef HOOK
    LOGI("[AXIOM] ALL HOOKS INSTALLED");
    pthread_exit(0);
}

// ================================================================
//  JNI ENTRY
// ================================================================
extern "C" {
    JavaVM* jvm = nullptr;
    JNIEnv* env = nullptr;

    __attribute__((visibility("default")))
    jint loadJNI(JavaVM* vm){
        jvm = vm;
        vm->AttachCurrentThread(&env, nullptr);
        return JNI_VERSION_1_6;
    }
}

__attribute__((constructor))
void init(){
    pthread_t t;
    pthread_create(&t, nullptr, thread, nullptr);
    RemapTools::RemapLibrary("libLoader.so");
}

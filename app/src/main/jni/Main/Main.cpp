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
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

// ================================================================
//  IN-APP DEBUG LOG
// ================================================================
#define DBG_LINES    80
#define DBG_LINE_LEN 168
struct DbgBuf { char lines[DBG_LINES][DBG_LINE_LEN]; int head=0,count=0; std::mutex m; };
static DbgBuf g_log;
static void DbgLog(const char* fmt, ...) {
    char buf[DBG_LINE_LEN]; va_list a; va_start(a,fmt); vsnprintf(buf,sizeof(buf),fmt,a); va_end(a);
    LOGI("%s",buf);
    g_log.m.lock();
    strncpy(g_log.lines[g_log.head],buf,DBG_LINE_LEN-1);
    g_log.lines[g_log.head][DBG_LINE_LEN-1]='\0';
    g_log.head=(g_log.head+1)%DBG_LINES;
    if(g_log.count<DBG_LINES)g_log.count++;
    g_log.m.unlock();
}

// ================================================================
//  LANGUAGE
// ================================================================
static int lang_idx = 0;
static const char* T(const char* vn, const char* en){ return lang_idx==0?vn:en; }

// ================================================================
//  TOGGLES
// ================================================================
static bool bGodMode      = false;
static bool bOneShotKill  = false;
static bool bInfAmmo      = false;
static bool bNoRecoil     = false;
static bool bSpeedHack    = false;
static bool bFly          = false;
static bool bFullMoney    = false;
static bool bFullBox      = false;
static bool bNoAds        = false;
static bool bUnlockWeapon = false;
static bool bEspBox       = false;
static bool bEspLine      = false;
static bool bEspDist      = false;
static bool bEspHp        = false;
static bool bAimCircle    = false;
static bool bAimKill      = false;

static float speedMult  = 2.0f;
static float flySpeed   = 5.0f;
static float AimFov     = 150.0f;

// Currency type defines
#define CT_GOLD       1
#define CT_GRENADE    2
#define CT_MEDICAL    3
#define CT_TICKET     4
#define CT_BOX_COPPER 103
#define CT_BOX_SILVER 104
#define CT_BOX_GOLDEN 105

// ================================================================
//  ESP LIST
// ================================================================
struct EspEntry {
    void*  inst;     // CharacterDamage*
    long   ts;
    float  hp, maxHp;
    float  screenX, screenY, screenZ;  // cached screen pos
    bool   screenValid;
};
static EspEntry   espList[128]={};
static std::mutex espMtx;
static long now_ms(){struct timeval tv;gettimeofday(&tv,NULL);return(long)tv.tv_sec*1000+tv.tv_usec/1000;}

// ================================================================
//  UNITY API — RVAs VERIFIED FROM DUMP
//  Camera.get_main          @ 0x251B400  ✅ confirmed
//  Camera.WorldToScreenPoint@ 0x251B1DC  ✅ confirmed
//  Component.get_transform  @ 0x253EF08  ✅ confirmed
//  Transform.get_position   @ 0x254B0DC  ✅ confirmed
// ================================================================
static void*   (*Camera_get_main)()                                   = nullptr;
static Vector3 (*Camera_WorldToScreenPoint)(void* cam, Vector3 world) = nullptr;
static void*   (*Component_get_transform)(void* comp)                 = nullptr;
static Vector3 (*Transform_get_position)(void* trans)                 = nullptr;

// ================================================================
//  DEBUG COUNTERS
// ================================================================
static std::atomic<bool> dbgApplyFired{false};
static std::atomic<bool> dbgCharDmgFired{false};
static std::atomic<bool> dbgArcMoneyFired{false};
static std::atomic<bool> dbgArcUseMoneyFired{false};
static std::atomic<bool> dbgArcKeysFired{false};
static std::atomic<bool> dbgArcUseKeysFired{false};
static std::atomic<bool> dbgUnlockFired{false};
static std::atomic<bool> dbgLoadingAwakeFired{false};
static std::atomic<bool> dbgLoadingStaticFired{false};
static std::atomic<bool> dbgAdInterFired{false};
static std::atomic<bool> dbgAdBannerFired{false};
static std::atomic<bool> dbgIsBuyAllFired{false};
static std::atomic<int>  cntCharDmg{0};
static std::atomic<int>  cntUseMoney{0};

// ================================================================
//  IL2CPP ATTACH
// ================================================================
static std::atomic<bool> g_attached{false};
static void EnsureAttached(){
    if(g_attached.load())return;
    void* lib=dlopen("libil2cpp.so",RTLD_LAZY|RTLD_NOLOAD);
    if(!lib)return;
    auto dg=(void*(*)())dlsym(lib,"il2cpp_domain_get");
    auto ta=(void*(*)(void*))dlsym(lib,"il2cpp_thread_attach");
    if(dg&&ta){ta(dg());g_attached.store(true);}
}

// Static field reader for LoadingOnces.Instance
static void* (*il2cpp_domain_get_p)()=nullptr;
static void* (*il2cpp_domain_assembly_open_p)(void*,const char*)=nullptr;
static void* (*il2cpp_assembly_get_image_p)(void*)=nullptr;
static void* (*il2cpp_class_from_name_p)(void*,const char*,const char*)=nullptr;
static void* (*il2cpp_class_get_static_field_data_p)(void*)=nullptr;

static void InitStaticAPI(){
    if(il2cpp_domain_get_p)return;
    void* lib=dlopen("libil2cpp.so",RTLD_LAZY|RTLD_NOLOAD);
    if(!lib)return;
    il2cpp_domain_get_p                 =(void*(*)())dlsym(lib,"il2cpp_domain_get");
    il2cpp_domain_assembly_open_p       =(void*(*)(void*,const char*))dlsym(lib,"il2cpp_domain_assembly_open");
    il2cpp_assembly_get_image_p         =(void*(*)(void*))dlsym(lib,"il2cpp_assembly_get_image");
    il2cpp_class_from_name_p            =(void*(*)(void*,const char*,const char*))dlsym(lib,"il2cpp_class_from_name");
    il2cpp_class_get_static_field_data_p=(void*(*)(void*))dlsym(lib,"il2cpp_class_get_static_field_data");
}
static void* GetStaticInst(const char* className){
    InitStaticAPI();
    if(!il2cpp_domain_get_p||!il2cpp_class_from_name_p||!il2cpp_class_get_static_field_data_p)return nullptr;
    void* dom=il2cpp_domain_get_p(); if(!dom)return nullptr;
    void* asm_=il2cpp_domain_assembly_open_p(dom,"Assembly-CSharp"); if(!asm_)return nullptr;
    void* img=il2cpp_assembly_get_image_p(asm_); if(!img)return nullptr;
    void* cls=il2cpp_class_from_name_p(img,"",className); if(!cls)return nullptr;
    void* sd=il2cpp_class_get_static_field_data_p(cls); if(!sd)return nullptr;
    return *(void**)((uint8_t*)sd+0x0); // Instance @ 0x0
}

// ================================================================
//  HOOK: CharacterDamage.Update() — RVA: 0x1191E10
//  ĐÂY LÀ HOOK TRUNG TÂM — xử lý GodMode, OneShot, ESP cùng 1 chỗ
//
//  FIX GodMode/OneShot: KHÔNG dùng ApplyDamage nữa (calling
//  convention quá phức tạp), thay bằng ghi thẳng vào hitPoints
//  mỗi frame — đơn giản, đáng tin cậy 100%.
//
//  FIX ESP: BỎ factionNum filter — trong zombie mode, zombie CÓ
//  thể có factionNum=1 (default), filter đó gây ra ESP trống rỗng.
//  Chỉ filter player (aiComp==null).
//
//  CharacterDamage fields (confirmed từ dump.cs):
//  AIComponent@0x30, hitPoints@0x40, initialHitPoints@0x44,
//  myTransform@0xF0
// ================================================================
static void (*old_CharDmgUpdate)(void* inst);
static void hook_CharDmgUpdate(void* inst){
    if(old_CharDmgUpdate)old_CharDmgUpdate(inst);
    if(!inst)return;
    if(!dbgCharDmgFired.exchange(true))DbgLog("[FIRE] CharDmgUpdate lan dau!");
    cntCharDmg++;

    void* aiComp=*(void**)((uint8_t*)inst+0x30);
    bool  isPlayer=(aiComp==nullptr);

    // ── GodMode: player HP → max moi frame ──────────────────────
    if(bGodMode && isPlayer){
        float maxHp=*(float*)((uint8_t*)inst+0x44);
        if(maxHp>0.f) *(float*)((uint8_t*)inst+0x40)=maxHp;
    }

    if(!isPlayer && aiComp){
        float hp   =*(float*)((uint8_t*)inst+0x40);
        float maxHp=*(float*)((uint8_t*)inst+0x44);

        // ── OneShotKill: force HP=0 moi frame ───────────────────
        if(bOneShotKill && hp>0.f)
            *(float*)((uint8_t*)inst+0x40)=0.0f;

        // ── ESP: track entity (KHONG filter factionNum) ──────────
        if((bEspBox||bEspLine||bEspDist||bEspHp||bAimKill) && hp>0.f && maxHp>0.f){
            long now=now_ms();
            espMtx.lock();
            bool found=false;
            for(int i=0;i<128;i++){
                if(espList[i].inst==inst){
                    espList[i].inst=inst; espList[i].ts=now;
                    espList[i].hp=hp; espList[i].maxHp=maxHp;
                    found=true; break;
                }
            }
            if(!found){
                for(int i=0;i<128;i++){
                    if(!espList[i].inst||now-espList[i].ts>5000){
                        espList[i]={inst,now,hp,maxHp,0,0,0,false};
                        break;
                    }
                }
            }
            espMtx.unlock();
        }
    }
}

// ================================================================
//  HOOK: Inf Ammo + No Recoil — WeaponBehavior.Update
//  RVA: 0x1309740
// ================================================================
static void (*old_WeapUpdate)(void* inst);
static void hook_WeapUpdate(void* inst){
    if(old_WeapUpdate)old_WeapUpdate(inst);
    if(!inst)return;
    if(bInfAmmo){
        int perClip=*(int*)((uint8_t*)inst+0x118);
        int maxAmmo=*(int*)((uint8_t*)inst+0x120);
        *(int*)((uint8_t*)inst+0x114)=perClip;
        *(int*)((uint8_t*)inst+0x110)=maxAmmo;
    }
    if(bNoRecoil){
        *(float*)((uint8_t*)inst+0x5E0)=0.f;
        *(float*)((uint8_t*)inst+0x5E4)=0.f;
        *(float*)((uint8_t*)inst+0x560)=0.f;
        *(float*)((uint8_t*)inst+0x608)=0.f;
        *(float*)((uint8_t*)inst+0x60C)=0.f;
        *(float*)((uint8_t*)inst+0x610)=0.f;
    }
}

// ================================================================
//  HOOK: No Recoil visual — WeaponKick RVA: 0x130E3BC
// ================================================================
static void (*old_WeaponKick)(void* inst);
static void hook_WeaponKick(void* inst){
    if(bNoRecoil)return;
    if(old_WeaponKick)old_WeaponKick(inst);
}

// ================================================================
//  HOOK: Speed + Fly — FPSRigidBodyWalker.FixedUpdate
//  RVA: 0x12EC900
//  FPSRigidBodyWalker fields (confirmed từ dump.cs):
//  walkSpeed@0xD4, sprintSpeed@0xD8
//  grounded@0x228, velocity@0x22C (Vector3: x=0x22C,y=0x230,z=0x234)
// ================================================================
static void (*old_FPSFixedUpdate)(void* inst);
static void hook_FPSFixedUpdate(void* inst){
    if(!inst){if(old_FPSFixedUpdate)old_FPSFixedUpdate(inst);return;}
    if(bSpeedHack){
        float ow=*(float*)((uint8_t*)inst+0xD4);
        float os=*(float*)((uint8_t*)inst+0xD8);
        *(float*)((uint8_t*)inst+0xD4)=ow*speedMult;
        *(float*)((uint8_t*)inst+0xD8)=os*speedMult;
        if(old_FPSFixedUpdate)old_FPSFixedUpdate(inst);
        *(float*)((uint8_t*)inst+0xD4)=ow;
        *(float*)((uint8_t*)inst+0xD8)=os;
    }else{
        if(old_FPSFixedUpdate)old_FPSFixedUpdate(inst);
    }
    // Fly — ghi SAU khi game da xu ly input
    if(bFly){
        *(bool*)((uint8_t*)inst+0x228)=false;          // grounded=false
        *(float*)((uint8_t*)inst+0x230)=flySpeed;       // velocity.y=up
    }
}

// ================================================================
//  HOOK: ArchiveData — Full Money / Full Box / Unlock
// ================================================================
static int  (*old_ArcGetMoney)(void* s);
static void (*old_ArcUseMoney)(void* s,int n);
static int  (*old_ArcGetTickets)(void* s);
static void (*old_ArcUseTickets)(void* s,int n);
static int  (*old_ArcGetKeys)(void* s);
static void (*old_ArcUseKeys)(void* s,int n);
static int  (*old_ArcGetSKeys)(void* s);
static void (*old_ArcUseSKeys)(void* s,int n);
static int  (*old_ArcUnlock)(void* s);
static bool (*old_IsBuyAll)(void* s);

static int  hook_ArcGetMoney(void* s){if(!dbgArcMoneyFired.exchange(true))DbgLog("[FIRE] ArcGetMoney!");if(bFullMoney)return 999999999;return old_ArcGetMoney?old_ArcGetMoney(s):0;}
static void hook_ArcUseMoney(void* s,int n){if(!dbgArcUseMoneyFired.exchange(true))DbgLog("[FIRE] ArcUseMoney use=%d",n);cntUseMoney++;if(bFullMoney)return;if(old_ArcUseMoney)old_ArcUseMoney(s,n);}
static int  hook_ArcGetTickets(void* s){if(bFullMoney)return 999999999;return old_ArcGetTickets?old_ArcGetTickets(s):0;}
static void hook_ArcUseTickets(void* s,int n){if(bFullMoney)return;if(old_ArcUseTickets)old_ArcUseTickets(s,n);}
static int  hook_ArcGetKeys(void* s){if(!dbgArcKeysFired.exchange(true))DbgLog("[FIRE] ArcGetKeys!");if(bFullBox)return 999999999;return old_ArcGetKeys?old_ArcGetKeys(s):0;}
static void hook_ArcUseKeys(void* s,int n){if(!dbgArcUseKeysFired.exchange(true))DbgLog("[FIRE] ArcUseKeys use=%d",n);if(bFullBox)return;if(old_ArcUseKeys)old_ArcUseKeys(s,n);}
static int  hook_ArcGetSKeys(void* s){if(bFullBox)return 999999999;return old_ArcGetSKeys?old_ArcGetSKeys(s):0;}
static void hook_ArcUseSKeys(void* s,int n){if(bFullBox)return;if(old_ArcUseSKeys)old_ArcUseSKeys(s,n);}
static int  hook_ArcUnlock(void* s){if(!dbgUnlockFired.exchange(true))DbgLog("[FIRE] ArcGetUnlock!");if(bUnlockWeapon)return 1;return old_ArcUnlock?old_ArcUnlock(s):0;}
static bool hook_IsBuyAll(void* s){if(!dbgIsBuyAllFired.exchange(true))DbgLog("[FIRE] IsBuyAllPackage!");if(bUnlockWeapon)return true;return old_IsBuyAll?old_IsBuyAll(s):false;}

// ================================================================
//  HOOK: ItemDataManager backup display
// ================================================================
static int  (*old_GetCurrency)(int t);
static void (*old_SetCurrency)(int t,int n);
static int  hook_GetCurrency(int t){
    int r=old_GetCurrency?old_GetCurrency(t):0;
    if(bFullMoney&&(t==CT_GOLD||t==CT_GRENADE||t==CT_MEDICAL||t==CT_TICKET))return 999999999;
    if(bFullBox&&(t==CT_BOX_COPPER||t==CT_BOX_SILVER||t==CT_BOX_GOLDEN))return 999999999;
    return r;
}
static void hook_SetCurrency(int t,int n){
    bool money=(t==CT_GOLD||t==CT_GRENADE||t==CT_MEDICAL||t==CT_TICKET);
    bool box=(t==CT_BOX_COPPER||t==CT_BOX_SILVER||t==CT_BOX_GOLDEN);
    if((bFullMoney&&money)||(bFullBox&&box)){int r=old_GetCurrency?old_GetCurrency(t):0;if(n<r)return;}
    if(old_SetCurrency)old_SetCurrency(t,n);
}

// ================================================================
//  HOOK: NoAds — Hook TRUC TIEP ham hien quang cao
//  FIX: Truoc day ghi field bool → game da cache va bo qua.
//  Gio hook thang vao ShowInterstitialAd + ShowBannerAd → return
//  luon, khong cho quang cao hien ra bat ke setting nao.
//
//  ShowInterstitialAd @ 0x132FF5C
//  ShowBannerAd       @ 0x132FBEC
// ================================================================
static void (*old_ShowInterAd)(void* self);
static void hook_ShowInterAd(void* self){
    if(!dbgAdInterFired.exchange(true))DbgLog("[FIRE] ShowInterstitialAd blocked!");
    if(bNoAds)return;
    if(old_ShowInterAd)old_ShowInterAd(self);
}
static void (*old_ShowBannerAd)(void* self,void* callback);
static void hook_ShowBannerAd(void* self,void* callback){
    if(!dbgAdBannerFired.exchange(true))DbgLog("[FIRE] ShowBannerAd blocked!");
    if(bNoAds)return;
    if(old_ShowBannerAd)old_ShowBannerAd(self,callback);
}

// ================================================================
//  HOOK: LoadingOnces — giữ làm backup, ghi cả 2 field
// ================================================================
static void* g_loadingInst=nullptr;
static void (*old_LoadingAwake)(void* inst);
static void hook_LoadingAwake(void* inst){
    if(!dbgLoadingAwakeFired.exchange(true))DbgLog("[FIRE] LoadingOnces.Awake!");
    g_loadingInst=inst;
    if(old_LoadingAwake)old_LoadingAwake(inst);
    if(bNoAds)        *(bool*)((uint8_t*)inst+0x48)=true;
    if(bUnlockWeapon) *(bool*)((uint8_t*)inst+0x45)=true;
}

// ================================================================
//  DRAW
// ================================================================
static void DrawMenu(){
    EnsureAttached();

    // Polling LoadingOnces fields moi frame
    if(!g_loadingInst)g_loadingInst=GetStaticInst("LoadingOnces");
    if(g_loadingInst){
        if(!dbgLoadingStaticFired.exchange(true))
            DbgLog("[OK] LoadingOnces.Instance found inst=%p",g_loadingInst);
        if(bNoAds)        *(bool*)((uint8_t*)g_loadingInst+0x48)=true;
        if(bUnlockWeapon) *(bool*)((uint8_t*)g_loadingInst+0x45)=true;
    }

    ImGuiStyle& s=ImGui::GetStyle();
    s.WindowRounding=10;s.FrameRounding=5;s.GrabRounding=4;
    s.ItemSpacing=ImVec2(8,7);s.WindowPadding=ImVec2(12,12);
    s.Colors[ImGuiCol_WindowBg]     =ImVec4(.04f,.04f,.08f,.97f);
    s.Colors[ImGuiCol_TitleBg]      =ImVec4(.00f,.12f,.30f,1.f);
    s.Colors[ImGuiCol_TitleBgActive]=ImVec4(.00f,.20f,.50f,1.f);
    s.Colors[ImGuiCol_FrameBg]      =ImVec4(.08f,.08f,.15f,1.f);
    s.Colors[ImGuiCol_CheckMark]    =ImVec4(.00f,.90f,1.0f,1.f);
    s.Colors[ImGuiCol_Tab]          =ImVec4(.05f,.10f,.20f,1.f);
    s.Colors[ImGuiCol_TabActive]    =ImVec4(.00f,.20f,.50f,1.f);
    s.Colors[ImGuiCol_TabHovered]   =ImVec4(.00f,.30f,.70f,1.f);
    s.Colors[ImGuiCol_SliderGrab]   =ImVec4(.00f,.70f,1.0f,1.f);
    s.Colors[ImGuiCol_Button]       =ImVec4(.00f,.25f,.55f,1.f);
    s.Colors[ImGuiCol_ButtonHovered]=ImVec4(.00f,.40f,.80f,1.f);
    s.Colors[ImGuiCol_Separator]    =ImVec4(.20f,.20f,.30f,1.f);

    ImDrawList* draw=ImGui::GetBackgroundDrawList();
    ImVec2 scr=ImGui::GetIO().DisplaySize;
    float cx=scr.x*.5f, cy=scr.y*.5f;

    if(bAimCircle)
        draw->AddCircle(ImVec2(cx,cy),AimFov,IM_COL32(255,255,255,170),128,1.6f);

    // ── ESP + AimKill render ──────────────────────────────────────
    if((bEspBox||bEspLine||bEspDist||bEspHp||bAimKill)
       &&Camera_get_main&&Camera_WorldToScreenPoint&&Transform_get_position)
    {
        void* cam=Camera_get_main();
        if(cam){
            long now=now_ms();
            // Sort tìm nearest cho AimKill
            float nearestZ=9999.f; void* nearestInst=nullptr;

            espMtx.lock();
            for(int i=0;i<128;i++){
                EspEntry& e=espList[i];
                if(!e.inst)continue;
                if(now-e.ts>5000){e.inst=nullptr;continue;}

                void* myTrans=*(void**)((uint8_t*)e.inst+0xF0);
                if(!myTrans)continue;

                Vector3 foot=Transform_get_position(myTrans);
                // Wider sanity check — khong filter (0,0,0) vi co the la valid pos
                if(foot.Y>500.f||foot.Y<-300.f)continue;

                Vector3 head=foot; head.Y+=1.85f;
                Vector3 sf=Camera_WorldToScreenPoint(cam,foot);
                Vector3 sh=Camera_WorldToScreenPoint(cam,head);
                if(sf.Z<0.1f||sf.Z>800.f)continue; // enemy behind camera or too far

                float fx=sf.X, fy=scr.y-sf.Y, hy=scr.y-sh.Y;
                float h=fy-hy;
                if(h<2.f)continue;
                float w=h*0.45f, d=sf.Z;

                // Cache screen pos for AimKill
                e.screenX=fx; e.screenY=fy; e.screenZ=d; e.screenValid=true;

                // Track nearest to center for AimKill
                float dx2=fx-cx, dy2=fy-cy;
                float distToCenter=sqrtf(dx2*dx2+dy2*dy2);
                if(distToCenter<AimFov && d<nearestZ){
                    nearestZ=d; nearestInst=e.inst;
                }

                ImU32 col=d<20.f?IM_COL32(255,60,60,245):d<50.f?IM_COL32(255,200,0,245):IM_COL32(60,255,120,245);

                if(bEspLine)
                    draw->AddLine(ImVec2(cx,scr.y),ImVec2(fx,fy),IM_COL32(255,70,70,200),1.3f);

                if(bEspBox){
                    float lx=fx-w*.5f,rx=fx+w*.5f,cw=w*.22f,ch=h*.18f;
                    draw->AddRect(ImVec2(lx-1,hy-1),ImVec2(rx+1,fy+1),IM_COL32(0,0,0,160),0,0,2.5f);
                    draw->AddRect(ImVec2(lx,hy),ImVec2(rx,fy),col,0,0,1.4f);
                    draw->AddLine(ImVec2(lx,hy),ImVec2(lx+cw,hy),IM_COL32(255,255,255,220),2.f);
                    draw->AddLine(ImVec2(lx,hy),ImVec2(lx,hy+ch),IM_COL32(255,255,255,220),2.f);
                    draw->AddLine(ImVec2(rx,hy),ImVec2(rx-cw,hy),IM_COL32(255,255,255,220),2.f);
                    draw->AddLine(ImVec2(rx,hy),ImVec2(rx,hy+ch),IM_COL32(255,255,255,220),2.f);
                    draw->AddLine(ImVec2(lx,fy),ImVec2(lx+cw,fy),IM_COL32(255,255,255,220),2.f);
                    draw->AddLine(ImVec2(lx,fy),ImVec2(lx,fy-ch),IM_COL32(255,255,255,220),2.f);
                    draw->AddLine(ImVec2(rx,fy),ImVec2(rx-cw,fy),IM_COL32(255,255,255,220),2.f);
                    draw->AddLine(ImVec2(rx,fy),ImVec2(rx,fy-ch),IM_COL32(255,255,255,220),2.f);
                }
                if(bEspDist){char buf[24];snprintf(buf,sizeof(buf),"%.0fm",d);draw->AddText(ImVec2(fx-10,hy-16),IM_COL32(255,220,0,255),buf);}
                if(bEspHp&&e.maxHp>0.f){
                    float ratio=e.hp/e.maxHp;
                    if(ratio<0)ratio=0;if(ratio>1)ratio=1;
                    float bx=fx+w*.5f+4;
                    draw->AddRectFilled(ImVec2(bx-1,hy-1),ImVec2(bx+5,fy+1),IM_COL32(0,0,0,180));
                    ImU32 hc=ratio>.6f?IM_COL32(0,255,80,255):ratio>.3f?IM_COL32(255,200,0,255):IM_COL32(255,0,0,255);
                    draw->AddRectFilled(ImVec2(bx,hy+h*(1-ratio)),ImVec2(bx+4,fy),hc);
                    char hb[16];snprintf(hb,sizeof(hb),"%.0f",e.hp);
                    draw->AddText(ImVec2(bx+6,hy+h*(1-ratio)-2),IM_COL32(255,255,255,220),hb);
                }
            }
            espMtx.unlock();

            // AimKill: force kill nearest enemy within AimFov
            if(bAimKill && nearestInst)
                *(float*)((uint8_t*)nearestInst+0x40)=0.0f;
        }
    }

    // ── MENU WINDOW ───────────────────────────────────────────────
    ImGui::SetNextWindowSize(ImVec2(380,560),ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(10,10),ImGuiCond_FirstUseEver);
    ImGui::Begin("  ZOMBIE3D MOD  |  AXIOM DEV  ",nullptr,0);
    ImGui::TextColored(ImVec4(0,0.8f,1,1),"  AXIOM DEVELOPMENT");
    ImGui::Separator();ImGui::Spacing();

    if(ImGui::BeginTabBar("tabs")){

        // ── NGUOI CHOI ────────────────────────────────────────────
        if(ImGui::BeginTabItem(T("NGUOI CHOI","PLAYER"))){
            ImGui::Spacing();
            ImGui::Checkbox(T("Bat Tu (God Mode)","God Mode"),&bGodMode);ImGui::Spacing();
            ImGui::Checkbox(T("Mot Phat Diet Tat","One Shot Kill"),&bOneShotKill);ImGui::Spacing();
            ImGui::Checkbox(T("Dan Vo Han","Inf Ammo"),&bInfAmmo);ImGui::Spacing();
            ImGui::Checkbox(T("Khong Giat Sung","No Recoil"),&bNoRecoil);ImGui::Spacing();
            ImGui::Checkbox(T("Tang Toc Di Chuyen","Speed Hack"),&bSpeedHack);
            if(bSpeedHack){ImGui::SetNextItemWidth(-1);ImGui::SliderFloat(T("Toc Do","Speed"),&speedMult,1,5,"x%.1f");}
            ImGui::Spacing();
            ImGui::Checkbox(T("Bay (Fly Hack)","Fly Hack"),&bFly);
            if(bFly){ImGui::SetNextItemWidth(-1);ImGui::SliderFloat(T("Toc Do Bay","Fly Speed"),&flySpeed,1,20,"%.1f");}
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(1,.6f,.2f,1),
                T("  Luu y: GodMode/OneShot chi hoat dong\n  o che do Zombie (PVP server-side).",
                  "  Note: GodMode/OneShot only work in\n  Zombie mode (PVP is server-side)."));
            ImGui::EndTabItem();
        }

        // ── TAI NGUYEN ────────────────────────────────────────────
        if(ImGui::BeginTabItem(T("TAI NGUYEN","RESOURCES"))){
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(1,.85f,0,1),T("  TIEN (Money+Tickets)","  MONEY+TICKETS"));
            ImGui::Checkbox(T("Full Tien [999,999,999]","Full Money"),&bFullMoney);ImGui::Spacing();
            ImGui::Separator();ImGui::Spacing();
            ImGui::TextColored(ImVec4(1,.6f,0,1),T("  RUONG (Key thuong+vang)","  BOXES (Keys)"));
            ImGui::Checkbox(T("Full Ruong [999,999,999 Key]","Full Boxes"),&bFullBox);ImGui::Spacing();
            ImGui::Separator();ImGui::Spacing();
            ImGui::TextColored(ImVec4(.7f,.7f,1,1),T("  VAT PHAM","  ITEMS"));
            ImGui::Checkbox(T("Mo Khoa Tat Ca Sung","Unlock All Weapons"),&bUnlockWeapon);ImGui::Spacing();
            ImGui::Checkbox(T("Bo Quang Cao","No Ads"),&bNoAds);
            ImGui::EndTabItem();
        }

        // ── DINH VI + AIM ─────────────────────────────────────────
        if(ImGui::BeginTabItem(T("DINH VI","ESP+AIM"))){
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(.5f,1,.5f,1),T("  DINH VI DICH","  ESP"));
            ImGui::Checkbox(T("Hop Zombie/Dich","Enemy Box"),&bEspBox);ImGui::Spacing();
            ImGui::Checkbox(T("Duong Den Dich","Enemy Line"),&bEspLine);ImGui::Spacing();
            ImGui::Checkbox(T("Khoang Cach","Distance"),&bEspDist);ImGui::Spacing();
            ImGui::Checkbox(T("Thanh Mau HP","HP Bar"),&bEspHp);ImGui::Spacing();
            ImGui::Separator();ImGui::Spacing();
            ImGui::TextColored(ImVec4(1,.5f,.5f,1),T("  AIM","  AIM"));
            ImGui::Checkbox(T("Vong Tron Aim","Aim Circle"),&bAimCircle);
            if(bAimCircle){ImGui::SetNextItemWidth(-1);ImGui::SliderFloat("FOV",&AimFov,50,400,"%.0f px");}
            ImGui::Spacing();
            ImGui::Checkbox(T("AimKill (diet dich trong vong tron)","AimKill (kill in aim circle)"),&bAimKill);
            if(bAimKill)ImGui::TextColored(ImVec4(.5f,1,.5f,1),
                T("  Tu dong diet dich gan nhat trong FOV",
                  "  Auto kills nearest enemy inside FOV"));
            ImGui::EndTabItem();
        }

        // ── DEBUG ─────────────────────────────────────────────────
        if(ImGui::BeginTabItem("DEBUG")){
            ImGui::Spacing();
            auto SL=[](const char* n,bool f,int c=-1){
                ImGui::TextColored(f?ImVec4(.3f,1,.3f,1):ImVec4(1,.35f,.35f,1),"%s",f?"[OK]":"[--]");
                ImGui::SameLine();
                if(c>=0)ImGui::Text("%s (%d)",n,c);else ImGui::Text("%s",n);
            };
            SL("CharDmgUpdate (ESP/GodMode/OneShot)",dbgCharDmgFired.load(),cntCharDmg.load());
            SL("ArcGetMoney",                        dbgArcMoneyFired.load());
            SL("ArcUseMoney (tru tien)",             dbgArcUseMoneyFired.load(),cntUseMoney.load());
            SL("ArcGetKeys",                         dbgArcKeysFired.load());
            SL("ArcUseKeys (tru key ruong)",         dbgArcUseKeysFired.load());
            SL("ArcGetUnlockAllWeapons",             dbgUnlockFired.load());
            SL("IsBuyAllPackage",                    dbgIsBuyAllFired.load());
            SL("ShowInterstitialAd (blocked)",       dbgAdInterFired.load());
            SL("ShowBannerAd (blocked)",             dbgAdBannerFired.load());
            SL("LoadingOnces.Awake",                 dbgLoadingAwakeFired.load());
            SL("LoadingOnces.Instance (static)",     dbgLoadingStaticFired.load());
            ImGui::Spacing();ImGui::Separator();ImGui::Spacing();
            ImGui::Text(T("Log:","Log:"));
            ImGui::BeginChild("log",ImVec2(0,220),true);
            g_log.m.lock();
            int n=g_log.count,start=(g_log.head-n+DBG_LINES)%DBG_LINES;
            for(int i=0;i<n;i++)ImGui::TextWrapped("%s",g_log.lines[(start+i)%DBG_LINES]);
            g_log.m.unlock();
            if(n>0)ImGui::SetScrollHereY(1.f);
            ImGui::EndChild();
            ImGui::EndTabItem();
        }

        // ── LANG ──────────────────────────────────────────────────
        if(ImGui::BeginTabItem(T("NGON NGU","LANG"))){
            ImGui::Spacing();
            if(ImGui::RadioButton("Tieng Viet",lang_idx==0))lang_idx=0;
            if(ImGui::RadioButton("English",lang_idx==1))lang_idx=1;
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
    do{sleep(1);}while(getAbsoluteAddress("libil2cpp.so",0)==0);
    DbgLog("[+] libil2cpp detected, waiting 3s...");
    sleep(3);

    Camera_get_main          =(void*(*)())(void*)getAbsoluteAddress("libil2cpp.so",0x251B400);
    Camera_WorldToScreenPoint=(Vector3(*)(void*,Vector3))(void*)getAbsoluteAddress("libil2cpp.so",0x251B1DC);
    Component_get_transform  =(void*(*)(void*))(void*)getAbsoluteAddress("libil2cpp.so",0x253EF08);
    Transform_get_position   =(Vector3(*)(void*))(void*)getAbsoluteAddress("libil2cpp.so",0x254B0DC);

    DbgLog("[+] Camera API loaded: get_main=%p W2S=%p",
        (void*)Camera_get_main,(void*)Camera_WorldToScreenPoint);

    #define HOOK(rva,hk,orig) do{ \
        void* _a=(void*)getAbsoluteAddress("libil2cpp.so",rva); \
        int _r=DobbyHook(_a,(void*)hk,(void**)&orig); \
        DbgLog("[HOOK] %s RVA=0x%lx ret=%d",#hk,(uintptr_t)rva,_r); \
    }while(0)

    // Core gameplay — KHÔNG hook ApplyDamage, CharDmgUpdate lo hết
    HOOK(0x1191E10, hook_CharDmgUpdate, old_CharDmgUpdate);
    HOOK(0x1309740, hook_WeapUpdate,    old_WeapUpdate);
    HOOK(0x130E3BC, hook_WeaponKick,    old_WeaponKick);
    HOOK(0x12EC900, hook_FPSFixedUpdate,old_FPSFixedUpdate);

    // ArchiveData
    HOOK(0x1323878, hook_ArcGetMoney,   old_ArcGetMoney);
    HOOK(0x13238C0, hook_ArcUseMoney,   old_ArcUseMoney);
    HOOK(0x13239D0, hook_ArcGetTickets, old_ArcGetTickets);
    HOOK(0x1323A18, hook_ArcUseTickets, old_ArcUseTickets);
    HOOK(0x1323B08, hook_ArcGetKeys,    old_ArcGetKeys);
    HOOK(0x1323B50, hook_ArcUseKeys,    old_ArcUseKeys);
    HOOK(0x1323D74, hook_ArcGetSKeys,   old_ArcGetSKeys);
    HOOK(0x1323DBC, hook_ArcUseSKeys,   old_ArcUseSKeys);
    HOOK(0x1324784, hook_ArcUnlock,     old_ArcUnlock);
    HOOK(0x13254D8, hook_IsBuyAll,      old_IsBuyAll);   // NEW: IsBuyAllPackage

    // ItemDataManager backup
    HOOK(0x12BF2FC, hook_GetCurrency,   old_GetCurrency);
    HOOK(0x12BF350, hook_SetCurrency,   old_SetCurrency);

    // NoAds — hook TRUC TIEP ham hien quang cao
    HOOK(0x132FF5C, hook_ShowInterAd,   old_ShowInterAd);   // NEW
    HOOK(0x132FBEC, hook_ShowBannerAd,  old_ShowBannerAd);  // NEW

    // LoadingOnces backup
    HOOK(0x12D711C, hook_LoadingAwake,  old_LoadingAwake);

    #undef HOOK
    DbgLog("[+] ALL HOOKS DONE — AXIOM DEV");
    pthread_exit(0);
}

// ================================================================
//  JNI
// ================================================================
extern "C"{
    JavaVM* jvm=nullptr; JNIEnv* env=nullptr;
    __attribute__((visibility("default")))
    jint loadJNI(JavaVM* vm){jvm=vm;vm->AttachCurrentThread(&env,nullptr);return JNI_VERSION_1_6;}
}
__attribute__((constructor))
void init(){pthread_t t;pthread_create(&t,nullptr,thread,nullptr);RemapTools::RemapLibrary("libLoader.so");}

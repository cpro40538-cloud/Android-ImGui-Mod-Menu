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
//  IN-APP DEBUG LOG
// ================================================================
#define DBG_LINES    80
#define DBG_LINE_LEN 168
struct DbgBuf{char lines[DBG_LINES][DBG_LINE_LEN];int head=0,count=0;std::mutex m;};
static DbgBuf g_log;
static void DbgLog(const char* fmt,...){
    char buf[DBG_LINE_LEN];va_list a;va_start(a,fmt);vsnprintf(buf,sizeof(buf),fmt,a);va_end(a);
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
static int lang_idx=0;
static const char* T(const char* vn,const char* en){return lang_idx==0?vn:en;}

// ================================================================
//  TOGGLES
// ================================================================
static bool bGodMode      = false;
static bool bOneShotKill  = false;
static bool bInfAmmo      = false;
static bool bNoRecoil     = false;
static bool bSpeedHack    = false;
static bool bFly          = false;
static bool bAimbot       = false;
static bool bFullMoney    = false;
static bool bFullBox      = false;
static bool bNoAds        = false;
static bool bUnlockWeapon = false;
static bool bFullSkin     = false;
static bool bEspBox       = false;
static bool bEspLine      = false;
static bool bEspDist      = false;
static bool bEspHp        = false;
static bool bAimCircle    = false;
static bool bAimKill      = false;

static float speedMult    = 2.0f;
static float flySpeed     = 8.0f;
static float AimFov       = 150.0f;
static float aimbotSpeed  = 0.15f;  // 0.0→1.0, toc do lock

#define CT_GOLD       1
#define CT_GRENADE    2
#define CT_MEDICAL    3
#define CT_TICKET     4
#define CT_BOX_COPPER 103
#define CT_BOX_SILVER 104
#define CT_BOX_GOLDEN 105

// ================================================================
//  FPS COUNTER
// ================================================================
static float  g_fps       = 0.f;
static int    g_fpsCount  = 0;
static long   g_fpsLast   = 0;

// ================================================================
//  ESP LIST — lưu cả world position cho aimbot
// ================================================================
struct EspEntry{
    void*  inst;
    long   ts;
    float  hp, maxHp;
    float  wx, wy, wz;       // world position
    float  sx, sy, sz;       // screen position
    bool   screenValid;
};
static EspEntry   espList[128]={};
static std::mutex espMtx;
static long now_ms(){struct timeval tv;gettimeofday(&tv,NULL);return(long)tv.tv_sec*1000+tv.tv_usec/1000;}

// ================================================================
//  UNITY CAMERA API — RVAs VERIFIED từ dump mới
// ================================================================
static void*   (*Camera_get_main)()                                   = nullptr;
static Vector3 (*Camera_WorldToScreenPoint)(void* cam,Vector3 world)  = nullptr;
static void*   (*Component_get_transform)(void* comp)                 = nullptr;
static Vector3 (*Transform_get_position)(void* trans)                 = nullptr;

// ================================================================
//  DEBUG FLAGS
// ================================================================
static std::atomic<bool> dbgCharDmg{false},dbgArcMoney{false},dbgArcUseMoney{false};
static std::atomic<bool> dbgArcKeys{false},dbgArcUseKeys{false},dbgUnlock{false};
static std::atomic<bool> dbgIsBuyAll{false},dbgInterAd{false},dbgBannerAd{false};
static std::atomic<bool> dbgLoading{false},dbgLoadingStatic{false};
static std::atomic<bool> dbgLoadSettings{false},dbgSkinCount{false};
static std::atomic<int>  cntCharDmg{0},cntUseMoney{0};

// ================================================================
//  IL2CPP THREAD ATTACH + STATIC FIELD READER
// ================================================================
static std::atomic<bool> g_attached{false};
static void EnsureAttached(){
    if(g_attached.load())return;
    void* lib=dlopen("libil2cpp.so",RTLD_LAZY|RTLD_NOLOAD);if(!lib)return;
    auto dg=(void*(*)())dlsym(lib,"il2cpp_domain_get");
    auto ta=(void*(*)(void*))dlsym(lib,"il2cpp_thread_attach");
    if(dg&&ta){ta(dg());g_attached.store(true);}
}
static void* (*_domain_get)()=nullptr;
static void* (*_asm_open)(void*,const char*)=nullptr;
static void* (*_asm_img)(void*)=nullptr;
static void* (*_cls_from_name)(void*,const char*,const char*)=nullptr;
static void* (*_cls_static)(void*)=nullptr;
static void InitAPI(){
    if(_domain_get)return;
    void* lib=dlopen("libil2cpp.so",RTLD_LAZY|RTLD_NOLOAD);if(!lib)return;
    _domain_get    =(void*(*)())dlsym(lib,"il2cpp_domain_get");
    _asm_open      =(void*(*)(void*,const char*))dlsym(lib,"il2cpp_domain_assembly_open");
    _asm_img       =(void*(*)(void*))dlsym(lib,"il2cpp_assembly_get_image");
    _cls_from_name =(void*(*)(void*,const char*,const char*))dlsym(lib,"il2cpp_class_from_name");
    _cls_static    =(void*(*)(void*))dlsym(lib,"il2cpp_class_get_static_field_data");
}
static void* GetStaticInst(const char* cls){
    InitAPI();
    if(!_domain_get||!_cls_from_name||!_cls_static)return nullptr;
    void* d=_domain_get();if(!d)return nullptr;
    void* a=_asm_open(d,"Assembly-CSharp");if(!a)return nullptr;
    void* img=_asm_img(a);if(!img)return nullptr;
    void* c=_cls_from_name(img,"",cls);if(!c)return nullptr;
    void* sd=_cls_static(c);if(!sd)return nullptr;
    return *(void**)((uint8_t*)sd+0x0);
}

// ================================================================
//  HOOK: CharacterDamage.Update() — RVA: 0x1191e10
//  Hub trung tâm: GodMode, OneShot, ESP tracking
//
//  Confirmed offsets từ dump mới:
//  AIComponent@0x30, hitPoints@0x40, initialHitPoints@0x44, myTransform@0xF0
//
//  FIX ESP: Lưu world position (wx,wy,wz) cho aimbot
//  FIX disappearing NPC: Bỏ check hp>0 ở bước tracking,
//  chỉ check maxHp>0 để tránh entities chưa init
// ================================================================
static void (*old_CharDmgUpdate)(void* inst);
static void hook_CharDmgUpdate(void* inst){
    if(old_CharDmgUpdate)old_CharDmgUpdate(inst);
    if(!inst)return;
    if(!dbgCharDmg.exchange(true))DbgLog("[FIRE] CharDmgUpdate!");
    cntCharDmg++;

    void* aiComp=*(void**)((uint8_t*)inst+0x30);
    bool  isPlayer=(aiComp==nullptr);

    // GodMode: Player HP → max mỗi frame
    if(bGodMode&&isPlayer){
        float maxHp=*(float*)((uint8_t*)inst+0x44);
        if(maxHp>0.f)*(float*)((uint8_t*)inst+0x40)=maxHp;
    }

    if(!isPlayer&&aiComp){
        // FIX ESP COMPANION: Dung followPlayer@0x125 + playerFollow@0x129
        // huntPlayer@0x10C SAI vi = false khi zombie chua spot player
        // followPlayer/playerFollow chi true voi NPC companion theo player
        // → Zombie (du chua hunt) se KHONG co followPlayer=true → HIEN dung
        bool isCompanion = *(bool*)((uint8_t*)aiComp+0x125)  // followPlayer
                        || *(bool*)((uint8_t*)aiComp+0x129);  // playerFollow
        if(isCompanion)return; // companion, bo qua

        float hp   =*(float*)((uint8_t*)inst+0x40);
        float maxHp=*(float*)((uint8_t*)inst+0x44);

        // OneShotKill — chi giet zombie/enemy that su (da filter companion phia tren)
        if(bOneShotKill&&hp>0.f)
            *(float*)((uint8_t*)inst+0x40)=0.0f;

        // ESP + AimKill tracking — luu world pos
        bool needTrack=(bEspBox||bEspLine||bEspDist||bEspHp||bAimKill||bAimbot);
        if(needTrack&&maxHp>0.f){
            // Lấy world position từ myTransform
            float wx=0,wy=0,wz=0;
            void* myTrans=*(void**)((uint8_t*)inst+0xF0);
            if(myTrans&&Transform_get_position){
                Vector3 pos=Transform_get_position(myTrans);
                wx=pos.X;wy=pos.Y;wz=pos.Z;
            }

            long now=now_ms();
            espMtx.lock();
            bool found=false;
            for(int i=0;i<128;i++){
                if(espList[i].inst==inst){
                    espList[i].ts=now;
                    espList[i].hp=hp;espList[i].maxHp=maxHp;
                    if(wx!=0||wy!=0||wz!=0){
                        espList[i].wx=wx;espList[i].wy=wy;espList[i].wz=wz;
                    }
                    found=true;break;
                }
            }
            if(!found){
                for(int i=0;i<128;i++){
                    if(!espList[i].inst||now-espList[i].ts>5000){
                        espList[i]={inst,now,hp,maxHp,wx,wy,wz,0,0,0,false};break;
                    }
                }
            }
            espMtx.unlock();
        }
    }
}

// ================================================================
//  HOOK: Inf Ammo + No Recoil — WeaponBehavior.Update @ 0x1309740
// ================================================================
static void (*old_WeapUpdate)(void* inst);
static void hook_WeapUpdate(void* inst){
    if(old_WeapUpdate)old_WeapUpdate(inst);
    if(!inst)return;
    if(bInfAmmo){
        *(int*)((uint8_t*)inst+0x114)=*(int*)((uint8_t*)inst+0x118);
        *(int*)((uint8_t*)inst+0x110)=*(int*)((uint8_t*)inst+0x120);
    }
    // FIX can't shoot after fly: Force canShoot=true, cantFireState=false
    // canShoot@0x39C bi set false khi flying=true (game tu lock weapon)
    // cantFireState@0x3E5 cung can reset
    if(bFly){
        *(bool*)((uint8_t*)inst+0x39C)=true;  // canShoot = true
        *(bool*)((uint8_t*)inst+0x3E5)=false; // cantFireState = false
    }
    if(bNoRecoil){
        *(float*)((uint8_t*)inst+0x5E0)=0.f;*(float*)((uint8_t*)inst+0x5E4)=0.f;
        *(float*)((uint8_t*)inst+0x560)=0.f;*(float*)((uint8_t*)inst+0x608)=0.f;
        *(float*)((uint8_t*)inst+0x60C)=0.f;*(float*)((uint8_t*)inst+0x610)=0.f;
    }
}
static void (*old_WeaponKick)(void* inst);
static void hook_WeaponKick(void* inst){if(bNoRecoil)return;if(old_WeaponKick)old_WeaponKick(inst);}

// ================================================================
//  HOOK: Speed + FLY + AIMBOT — FPSRigidBodyWalker.FixedUpdate
//  RVA: 0x12ec900
//
//  FIX Fly (bay sát đất): Trước đây set velocity.y — SAI.
//  Dump mới xác nhận: `public bool flying; // 0x295`
//  Game có SẴN fly mode! Chỉ cần set flying=true + flyDownSpeed=0
//  Đây là lý do bay sát đất — velocity.y bị game override bởi
//  gravity, nhưng flying=true thì game TỰ xử lý bay không rớt.
//
//  FPSRigidBodyWalker fields (từ dump mới):
//  flying@0x295, flyDownSpeed@0x298, verticalSpeedAmt@0x2A0
//  SmoothMouseLookComponent@0x20, myTransform@0x60
//
//  SmoothMouseLook fields:
//  rotationX@0x58 (yaw), rotationY@0x5c (pitch)
// ================================================================
static void (*old_FPSFixedUpdate)(void* inst);
// Lưu FPSRigidBodyWalker instance cho aimbot
static void* g_walkerInst=nullptr;

static void hook_FPSFixedUpdate(void* inst){
    if(!inst){if(old_FPSFixedUpdate)old_FPSFixedUpdate(inst);return;}
    g_walkerInst=inst;

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

    // FLY — flying@0x295 + jumpBtn@0x2B1 (xac nhan tu dump)
    // FIX BAY NHU BONG BAY: Khong set verticalSpeedAmt lien tuc
    // Chi len khi nhan jump, xuong khi nhan crouch, hover khi khong nhan gi
    if(bFly){
        *(bool*) ((uint8_t*)inst+0x295)=true;     // flying = true
        *(float*)((uint8_t*)inst+0x298)=0.0f;     // flyDownSpeed = 0 (khong rot tu do)
        *(bool*) ((uint8_t*)inst+0x228)=false;    // grounded = false
        *(bool*) ((uint8_t*)inst+0x1A0)=false;    // hideWeapon = false (fix can't shoot)
        *(bool*) ((uint8_t*)inst+0x195)=false;    // lowerGunForClimb = false

        // Lay InputControl tu FPSRigidBodyWalker@0x30
        void* inputCtrl=*(void**)((uint8_t*)inst+0x30);
        bool jumpHeld  = *(bool*)((uint8_t*)inst+0x2B1); // jumpBtn@0x2B1
        bool crouchHeld= inputCtrl ? *(bool*)((uint8_t*)inputCtrl+0x2F) : false; // crouchHold@0x2F

        // Dieu khien chieu doc:
        // Jump  → len cao
        // Crouch → xuong thap
        // Khong nhan gi → hover tai cho (verticalSpeedAmt=0)
        float vSpeed = 0.0f;
        if(jumpHeld)   vSpeed =  flySpeed;  // len
        if(crouchHeld) vSpeed = -flySpeed;  // xuong
        *(float*)((uint8_t*)inst+0x2A0)=vSpeed; // verticalSpeedAmt
    }else{
        *(bool*) ((uint8_t*)inst+0x295)=false;    // flying = false, physics lai
        *(float*)((uint8_t*)inst+0x2A0)=0.0f;    // reset vertical speed
        *(bool*) ((uint8_t*)inst+0x1A0)=false;    // hideWeapon = false
    }
}

// ================================================================
//  AIMBOT — chạy trong DrawMenu (main thread, sau khi có screen pos)
//  Screen-space aimbot: tính delta từ screen center → target screen pos
//  → Thêm delta vào rotationX/Y của SmoothMouseLook
//  Không cần world-to-angle conversion phức tạp
// ================================================================
static void ApplyAimbot(ImVec2 scr){
    if(!g_walkerInst)return;
    void* sml=*(void**)((uint8_t*)g_walkerInst+0x20); // SmoothMouseLookComponent
    if(!sml)return;

    float cx=scr.x*.5f, cy=scr.y*.5f;
    float nearestDist=9999.f;
    float bestSX=0,bestSY=0;

    espMtx.lock();
    long now=now_ms();
    for(int i=0;i<128;i++){
        EspEntry& e=espList[i];
        if(!e.inst||!e.screenValid)continue;
        if(now-e.ts>3000)continue;
        if(e.hp<=0)continue;
        float dx=e.sx-cx, dy=e.sy-cy;
        float d=sqrtf(dx*dx+dy*dy);
        if(d<AimFov&&d<nearestDist){
            nearestDist=d;bestSX=e.sx;bestSY=e.sy;
        }
    }
    espMtx.unlock();

    if(nearestDist>=9999.f)return; // không có target

    // Delta từ screen center đến target head (aim lên 1/4 chiều cao)
    float deltaX = (bestSX-cx) * aimbotSpeed * 0.05f;  // horizontal
    float deltaY = (bestSY-cy) * aimbotSpeed * 0.05f;  // vertical (screen Y → rotation)

    // Cộng delta vào rotationX (yaw) và rotationY (pitch)
    *(float*)((uint8_t*)sml+0x58)+=deltaX;
    *(float*)((uint8_t*)sml+0x5c)-=deltaY; // âm vì screen Y tăng xuống dưới
}

// ================================================================
//  HOOK: ArchiveData — Full Money / Full Box / Unlock Weapon
//  RVAs confirmed từ dump mới (không đổi)
// ================================================================
static int  (*old_ArcGetMoney)(void* s);     static void (*old_ArcUseMoney)(void* s,int n);
static int  (*old_ArcGetTickets)(void* s);   static void (*old_ArcUseTickets)(void* s,int n);
static int  (*old_ArcGetKeys)(void* s);      static void (*old_ArcUseKeys)(void* s,int n);
static int  (*old_ArcGetSKeys)(void* s);     static void (*old_ArcUseSKeys)(void* s,int n);
static int  (*old_ArcUnlock)(void* s);       static bool (*old_IsBuyAll)(void* s);

static int  hk_GetMoney(void* s){if(!dbgArcMoney.exchange(true))DbgLog("[FIRE] ArcGetMoney!");if(bFullMoney)return 999999999;return old_ArcGetMoney?old_ArcGetMoney(s):0;}
static void hk_UseMoney(void* s,int n){if(!dbgArcUseMoney.exchange(true))DbgLog("[FIRE] ArcUseMoney use=%d",n);cntUseMoney++;if(bFullMoney)return;if(old_ArcUseMoney)old_ArcUseMoney(s,n);}
static int  hk_GetTix(void* s){if(bFullMoney)return 999999999;return old_ArcGetTickets?old_ArcGetTickets(s):0;}
static void hk_UseTix(void* s,int n){if(bFullMoney)return;if(old_ArcUseTickets)old_ArcUseTickets(s,n);}
static int  hk_GetKeys(void* s){if(!dbgArcKeys.exchange(true))DbgLog("[FIRE] ArcGetKeys!");if(bFullBox)return 999999999;return old_ArcGetKeys?old_ArcGetKeys(s):0;}
static void hk_UseKeys(void* s,int n){if(!dbgArcUseKeys.exchange(true))DbgLog("[FIRE] ArcUseKeys use=%d",n);if(bFullBox)return;if(old_ArcUseKeys)old_ArcUseKeys(s,n);}
static int  hk_GetSKeys(void* s){if(bFullBox)return 999999999;return old_ArcGetSKeys?old_ArcGetSKeys(s):0;}
static void hk_UseSKeys(void* s,int n){if(bFullBox)return;if(old_ArcUseSKeys)old_ArcUseSKeys(s,n);}
static int  hk_Unlock(void* s){if(!dbgUnlock.exchange(true))DbgLog("[FIRE] ArcGetUnlock!");if(bUnlockWeapon)return 1;return old_ArcUnlock?old_ArcUnlock(s):0;}
static bool hk_IsBuyAll(void* s){if(!dbgIsBuyAll.exchange(true))DbgLog("[FIRE] IsBuyAllPackage!");if(bUnlockWeapon)return true;return old_IsBuyAll?old_IsBuyAll(s):false;}

// ================================================================
//  HOOK: WeaponSkinManager.GetWeaponOwnSkinCount — RVA: 0x11e0ddc
//  FIX Full Skin: Hook hàm này return 999 → game hiện TẤT CẢ
//  skin như đã mua. Xác nhận từ dump: PurchaseItem.owned@0x18
// ================================================================
static int (*old_GetSkinCount)(void* s,int weaponID);
static int hk_GetSkinCount(void* s,int weaponID){
    if(!dbgSkinCount.exchange(true))DbgLog("[FIRE] GetWeaponOwnSkinCount weapon=%d",weaponID);
    if(bFullSkin)return 999;
    return old_GetSkinCount?old_GetSkinCount(s,weaponID):0;
}

// ================================================================
//  HOOK: ItemDataManager backup display
// ================================================================
static int  (*old_GetCurrency)(int t);  static void (*old_SetCurrency)(int t,int n);
static int  hk_GetCurrency(int t){
    int r=old_GetCurrency?old_GetCurrency(t):0;
    if(bFullMoney&&(t==CT_GOLD||t==CT_GRENADE||t==CT_MEDICAL||t==CT_TICKET))return 999999999;
    if(bFullBox&&(t==CT_BOX_COPPER||t==CT_BOX_SILVER||t==CT_BOX_GOLDEN))return 999999999;
    return r;
}
static void hk_SetCurrency(int t,int n){
    bool money=(t==CT_GOLD||t==CT_GRENADE||t==CT_MEDICAL||t==CT_TICKET);
    bool box=(t==CT_BOX_COPPER||t==CT_BOX_SILVER||t==CT_BOX_GOLDEN);
    if((bFullMoney&&money)||(bFullBox&&box)){int r=old_GetCurrency?old_GetCurrency(t):0;if(n<r)return;}
    if(old_SetCurrency)old_SetCurrency(t,n);
}

// ================================================================
//  HOOK: LoadingOnces.LoadingSettings — RVA: 0x12d7344
//  FIX NoAds + UnlockWeapon: Đây là hàm THẬT SỰ đọc PlayerPrefs
//  và ghi đè fields của LoadingOnces. Trước đây ta hook Awake() nhưng
//  LoadingSettings() chạy SAU, overwrite lại isNoAds/unLockAllWeapon.
//  Giờ hook thẳng LoadingSettings, ghi value CỦA MÌNH sau khi
//  original chạy xong → không bao giờ bị overwrite nữa.
// ================================================================
static void* g_loadingInst=nullptr;
static void (*old_LoadingSettings)(void* inst);
static void hk_LoadingSettings(void* inst){
    if(!dbgLoadSettings.exchange(true))DbgLog("[FIRE] LoadingSettings!");
    g_loadingInst=inst;
    if(old_LoadingSettings)old_LoadingSettings(inst);
    // Ghi SAU khi game đọc PlayerPrefs xong → không bị overwrite
    if(bNoAds)        *(bool*)((uint8_t*)inst+0x48)=true;
    if(bUnlockWeapon) *(bool*)((uint8_t*)inst+0x45)=true;
}

// ================================================================
//  HOOK: NoAds — block TRỰC TIẾP hàm hiện quảng cáo
// ================================================================
static void (*old_ShowInterAd)(void* self);
static void hk_ShowInterAd(void* self){
    if(!dbgInterAd.exchange(true))DbgLog("[FIRE] ShowInterstitialAd blocked!");
    if(bNoAds)return;
    if(old_ShowInterAd)old_ShowInterAd(self);
}
static void (*old_ShowBannerAd)(void* self,void* cb);
static void hk_ShowBannerAd(void* self,void* cb){
    if(!dbgBannerAd.exchange(true))DbgLog("[FIRE] ShowBannerAd blocked!");
    if(bNoAds)return;
    if(old_ShowBannerAd)old_ShowBannerAd(self,cb);
}

// ================================================================
//  DRAW
// ================================================================
static void DrawMenu(){
    EnsureAttached();

    // FPS counter
    g_fpsCount++;
    long nowT=now_ms();
    if(nowT-g_fpsLast>=1000){
        g_fps=g_fpsCount*1000.f/(float)(nowT-g_fpsLast);
        g_fpsCount=0;g_fpsLast=nowT;
    }

    // Polling LoadingOnces mỗi frame (backup đảm bảo)
    if(!g_loadingInst)g_loadingInst=GetStaticInst("LoadingOnces");
    if(g_loadingInst){
        if(!dbgLoadingStatic.exchange(true))
            DbgLog("[OK] LoadingOnces.Instance=%p",g_loadingInst);
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
    float cx=scr.x*.5f,cy=scr.y*.5f;

    if(bAimCircle)
        draw->AddCircle(ImVec2(cx,cy),AimFov,IM_COL32(255,255,255,170),128,1.6f);

    // ── ESP + AimKill + Aimbot render ────────────────────────────
    bool needESP=(bEspBox||bEspLine||bEspDist||bEspHp||bAimKill||bAimbot);
    if(needESP&&Camera_get_main&&Camera_WorldToScreenPoint&&Transform_get_position){
        void* cam=Camera_get_main();
        if(cam){
            long now=now_ms();
            float nearestZ=9999.f;void* nearestInst=nullptr;

            espMtx.lock();
            for(int i=0;i<128;i++){
                EspEntry& e=espList[i];
                if(!e.inst)continue;
                if(now-e.ts>5000){e.inst=nullptr;e.screenValid=false;continue;}

                // Doc world pos TRUC TIEP tu myTransform — luon fresh, khong stale
                void* myTrans=*(void**)((uint8_t*)e.inst+0xF0); // myTransform@0xF0
                if(!myTrans){e.screenValid=false;continue;}
                Vector3 foot=Transform_get_position(myTrans);
                // Luu lai cho aimbot
                e.wx=foot.X;e.wy=foot.Y;e.wz=foot.Z;
                if(foot.Y>500.f||foot.Y<-300.f){e.screenValid=false;continue;}

                Vector3 head=foot;head.Y+=1.85f;
                Vector3 sf=Camera_WorldToScreenPoint(cam,foot);
                Vector3 sh=Camera_WorldToScreenPoint(cam,head);

                if(sf.Z<0.05f||sf.Z>1000.f){e.screenValid=false;continue;}

                float fx=sf.X,fy=scr.y-sf.Y,hy=scr.y-sh.Y;
                float h=fy-hy;if(h<1.f){e.screenValid=false;continue;}
                float w=h*.45f,d=sf.Z;

                // Lưu screen pos cho aimbot
                e.sx=fx;e.sy=(hy+fy)*.5f; // aim giữa body
                e.sz=d;e.screenValid=true;

                // AimKill nearest in circle
                float ddx=fx-cx,ddy=e.sy-cy;
                float distC=sqrtf(ddx*ddx+ddy*ddy);
                if(distC<AimFov&&d<nearestZ){nearestZ=d;nearestInst=e.inst;}

                ImU32 col=d<20.f?IM_COL32(255,60,60,245):d<50.f?IM_COL32(255,200,0,245):IM_COL32(60,255,120,245);

                if(bEspLine)draw->AddLine(ImVec2(cx,scr.y),ImVec2(fx,fy),IM_COL32(255,70,70,200),1.3f);
                if(bEspBox){
                    float lx=fx-w*.5f,rx=fx+w*.5f,cw2=w*.22f,ch2=h*.18f;
                    draw->AddRect(ImVec2(lx-1,hy-1),ImVec2(rx+1,fy+1),IM_COL32(0,0,0,160),0,0,2.5f);
                    draw->AddRect(ImVec2(lx,hy),ImVec2(rx,fy),col,0,0,1.4f);
                    draw->AddLine(ImVec2(lx,hy),ImVec2(lx+cw2,hy),IM_COL32(255,255,255,220),2.f);
                    draw->AddLine(ImVec2(lx,hy),ImVec2(lx,hy+ch2),IM_COL32(255,255,255,220),2.f);
                    draw->AddLine(ImVec2(rx,hy),ImVec2(rx-cw2,hy),IM_COL32(255,255,255,220),2.f);
                    draw->AddLine(ImVec2(rx,hy),ImVec2(rx,hy+ch2),IM_COL32(255,255,255,220),2.f);
                    draw->AddLine(ImVec2(lx,fy),ImVec2(lx+cw2,fy),IM_COL32(255,255,255,220),2.f);
                    draw->AddLine(ImVec2(lx,fy),ImVec2(lx,fy-ch2),IM_COL32(255,255,255,220),2.f);
                    draw->AddLine(ImVec2(rx,fy),ImVec2(rx-cw2,fy),IM_COL32(255,255,255,220),2.f);
                    draw->AddLine(ImVec2(rx,fy),ImVec2(rx,fy-ch2),IM_COL32(255,255,255,220),2.f);
                }
                if(bEspDist){char buf[24];snprintf(buf,24,"%.0fm",d);draw->AddText(ImVec2(fx-10,hy-16),IM_COL32(255,220,0,255),buf);}
                if(bEspHp&&e.maxHp>0.f){
                    float r=e.hp/e.maxHp;if(r<0)r=0;if(r>1)r=1;
                    float bx=fx+w*.5f+4;
                    draw->AddRectFilled(ImVec2(bx-1,hy-1),ImVec2(bx+5,fy+1),IM_COL32(0,0,0,180));
                    ImU32 hc=r>.6f?IM_COL32(0,255,80,255):r>.3f?IM_COL32(255,200,0,255):IM_COL32(255,0,0,255);
                    draw->AddRectFilled(ImVec2(bx,hy+h*(1-r)),ImVec2(bx+4,fy),hc);
                    char hb[16];snprintf(hb,16,"%.0f",e.hp);
                    draw->AddText(ImVec2(bx+6,hy+h*(1-r)-2),IM_COL32(255,255,255,220),hb);
                }
            }
            espMtx.unlock();

            // AimKill
            if(bAimKill&&nearestInst)
                *(float*)((uint8_t*)nearestInst+0x40)=0.0f;
        }
    }

    // Aimbot (sau khi có screen pos cập nhật)
    if(bAimbot)ApplyAimbot(scr);

    // ── MENU ─────────────────────────────────────────────────────
    ImGui::SetNextWindowSize(ImVec2(390,580),ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(10,10),ImGuiCond_FirstUseEver);
    ImGui::Begin("  ZOMBIE3D MOD  |  AXIOM DEV  ",nullptr,0);

    // FPS hiện ở header
    ImGui::TextColored(ImVec4(0,.8f,1,1),"  AXIOM DEVELOPMENT");
    ImGui::SameLine();
    char fpsBuf[32];snprintf(fpsBuf,32,"  [FPS: %.0f]",g_fps);
    ImGui::TextColored(g_fps>=60?ImVec4(.3f,1,.3f,1):g_fps>=30?ImVec4(1,.8f,.2f,1):ImVec4(1,.3f,.3f,1),"%s",fpsBuf);
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
            if(bFly){ImGui::SetNextItemWidth(-1);ImGui::SliderFloat(T("Toc Do Bay","Fly Speed"),&flySpeed,1,30,"%.0f");}
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(1,.6f,.2f,1),
                T("  GodMode/OneShot chi hoat dong\n  o Zombie (PVP = server-side).",
                  "  GodMode/OneShot only work in\n  Zombie mode (PVP is server-side)."));
            ImGui::EndTabItem();
        }

        // ── TAI NGUYEN ────────────────────────────────────────────
        if(ImGui::BeginTabItem(T("TAI NGUYEN","RESOURCES"))){
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(1,.85f,0,1),T("  TIEN (Money+Tickets)","  MONEY"));
            ImGui::Checkbox(T("Full Tien [999,999,999]","Full Money"),&bFullMoney);ImGui::Spacing();
            ImGui::Separator();ImGui::Spacing();
            ImGui::TextColored(ImVec4(1,.6f,0,1),T("  RUONG (Key thuong+vang)","  BOXES"));
            ImGui::Checkbox(T("Full Ruong [999,999,999 Key]","Full Boxes"),&bFullBox);ImGui::Spacing();
            ImGui::Separator();ImGui::Spacing();
            ImGui::TextColored(ImVec4(.7f,.7f,1,1),T("  VAT PHAM","  ITEMS"));
            ImGui::Checkbox(T("Mo Khoa Tat Ca Sung","Unlock All Weapons"),&bUnlockWeapon);ImGui::Spacing();
            ImGui::Checkbox(T("Full Tat Ca Skin","Full All Skins"),&bFullSkin);ImGui::Spacing();
            ImGui::Checkbox(T("Bo Quang Cao","No Ads"),&bNoAds);
            ImGui::EndTabItem();
        }

        // ── DINH VI + AIM ─────────────────────────────────────────
        if(ImGui::BeginTabItem(T("DINH VI","ESP+AIM"))){
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(.5f,1,.5f,1),T("  DINH VI ZOMBIE/DICH","  ESP"));
            ImGui::Checkbox(T("Hop (Box)","Enemy Box"),&bEspBox);ImGui::Spacing();
            ImGui::Checkbox(T("Duong Den Dich (Line)","Enemy Line"),&bEspLine);ImGui::Spacing();
            ImGui::Checkbox(T("Khoang Cach (Distance)","Distance"),&bEspDist);ImGui::Spacing();
            ImGui::Checkbox(T("Thanh Mau HP (HP Bar)","HP Bar"),&bEspHp);ImGui::Spacing();
            ImGui::Separator();ImGui::Spacing();
            ImGui::TextColored(ImVec4(1,.5f,.5f,1),T("  AIM","  AIM"));
            ImGui::Checkbox(T("Vong Tron Aim","Aim Circle"),&bAimCircle);
            if(bAimCircle){ImGui::SetNextItemWidth(-1);ImGui::SliderFloat("FOV",&AimFov,30,500,"%.0f px");}
            ImGui::Spacing();
            ImGui::Checkbox(T("AimKill (diet dich trong vong)","AimKill (kill in circle)"),&bAimKill);ImGui::Spacing();
            ImGui::Checkbox(T("Aimbot (tu dong ngam)","Aimbot (auto aim)"),&bAimbot);
            if(bAimbot){
                ImGui::SetNextItemWidth(-1);
                ImGui::SliderFloat(T("Toc Do Ngam","Aim Speed"),&aimbotSpeed,0.01f,1.f,"%.2f");
                ImGui::TextColored(ImVec4(.5f,1,.5f,1),
                    T("  Aimbot hoat dong khi co dich trong\n  vong tron FOV.",
                      "  Aimbot works when enemy is within\n  FOV circle."));
            }
            ImGui::EndTabItem();
        }

        // ── SETTING (FPS) ─────────────────────────────────────────
        if(ImGui::BeginTabItem(T("SETTING","SETTING"))){
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(.5f,1,.5f,1),T("  HIEU NANG","  PERFORMANCE"));
            ImGui::Spacing();
            // FPS counter lớn hơn trong tab
            float fpsColor=g_fps>=60?1.f:g_fps>=30?.5f:0.f;
            ImGui::TextColored(ImVec4(1.f-fpsColor,fpsColor,0,1),"FPS THUC: %.1f",g_fps);
            ImGui::ProgressBar(g_fps/120.f,ImVec2(-1,8));
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(.6f,.6f,.6f,1),
                g_fps>=60?"Rat tot (60+)":g_fps>=30?"Binh thuong (30+)":"Thap, co the lag (<30)");
            ImGui::Spacing();ImGui::Separator();ImGui::Spacing();
            ImGui::TextColored(ImVec4(.7f,.7f,1,1),T("  NGON NGU","  LANGUAGE"));
            ImGui::Spacing();
            if(ImGui::RadioButton("Tieng Viet",lang_idx==0))lang_idx=0;
            if(ImGui::RadioButton("English",lang_idx==1))lang_idx=1;
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
            SL("CharDmgUpdate (ESP/God/1Shot)",dbgCharDmg.load(),cntCharDmg.load());
            SL("ArcGetMoney",                  dbgArcMoney.load());
            SL("ArcUseMoney (tru tien)",        dbgArcUseMoney.load(),cntUseMoney.load());
            SL("ArcGetKeys",                   dbgArcKeys.load());
            SL("ArcUseKeys (tru key ruong)",   dbgArcUseKeys.load());
            SL("ArcGetUnlockAllWeapons",       dbgUnlock.load());
            SL("IsBuyAllPackage",              dbgIsBuyAll.load());
            SL("GetWeaponOwnSkinCount (skin)", dbgSkinCount.load());
            SL("ShowInterstitialAd (blocked)", dbgInterAd.load());
            SL("ShowBannerAd (blocked)",       dbgBannerAd.load());
            SL("LoadingSettings (NoAds/Unlock)",dbgLoadSettings.load());
            SL("LoadingOnces.Instance (static)",dbgLoadingStatic.load());
            ImGui::Spacing();ImGui::Separator();ImGui::Spacing();
            ImGui::Text("Log:");
            ImGui::BeginChild("log",ImVec2(0,200),true);
            g_log.m.lock();
            int n=g_log.count,start=(g_log.head-n+DBG_LINES)%DBG_LINES;
            for(int i=0;i<n;i++)ImGui::TextWrapped("%s",g_log.lines[(start+i)%DBG_LINES]);
            g_log.m.unlock();
            if(n>0)ImGui::SetScrollHereY(1.f);
            ImGui::EndChild();
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
    DbgLog("[+] Camera API: main=%p W2S=%p pos=%p",(void*)Camera_get_main,(void*)Camera_WorldToScreenPoint,(void*)Transform_get_position);

    #define HOOK(rva,hk,orig) do{ \
        void* _a=(void*)getAbsoluteAddress("libil2cpp.so",rva); \
        int _r=DobbyHook(_a,(void*)hk,(void**)&orig); \
        DbgLog("[HOOK] %s @0x%lx ret=%d",#hk,(uintptr_t)rva,_r); \
    }while(0)

    HOOK(0x1191E10, hook_CharDmgUpdate, old_CharDmgUpdate); // GodMode/OneShot/ESP
    HOOK(0x1309740, hook_WeapUpdate,    old_WeapUpdate);    // InfAmmo/NoRecoil
    HOOK(0x130E3BC, hook_WeaponKick,    old_WeaponKick);    // NoRecoil visual
    HOOK(0x12EC900, hook_FPSFixedUpdate,old_FPSFixedUpdate);// Speed/Fly/Aimbot

    HOOK(0x1323878, hk_GetMoney,  old_ArcGetMoney);
    HOOK(0x13238C0, hk_UseMoney,  old_ArcUseMoney);
    HOOK(0x13239D0, hk_GetTix,    old_ArcGetTickets);
    HOOK(0x1323A18, hk_UseTix,    old_ArcUseTickets);
    HOOK(0x1323B08, hk_GetKeys,   old_ArcGetKeys);
    HOOK(0x1323B50, hk_UseKeys,   old_ArcUseKeys);
    HOOK(0x1323D74, hk_GetSKeys,  old_ArcGetSKeys);
    HOOK(0x1323DBC, hk_UseSKeys,  old_ArcUseSKeys);
    HOOK(0x1324784, hk_Unlock,    old_ArcUnlock);
    HOOK(0x13254D8, hk_IsBuyAll,  old_IsBuyAll);

    HOOK(0x11E0DDC, hk_GetSkinCount,   old_GetSkinCount); // FULL SKIN

    HOOK(0x12BF2FC, hk_GetCurrency,   old_GetCurrency);
    HOOK(0x12BF350, hk_SetCurrency,   old_SetCurrency);

    HOOK(0x12D7344, hk_LoadingSettings, old_LoadingSettings); // NoAds/Unlock FIX
    HOOK(0x132FF5C, hk_ShowInterAd,     old_ShowInterAd);     // Block ads
    HOOK(0x132FBEC, hk_ShowBannerAd,    old_ShowBannerAd);    // Block ads

    #undef HOOK
    DbgLog("[+] ALL HOOKS DONE — AXIOM DEV");
    pthread_exit(0);
}

// ================================================================
//  JNI
// ================================================================
extern "C"{
    JavaVM* jvm=nullptr;JNIEnv* env=nullptr;
    __attribute__((visibility("default")))
    jint loadJNI(JavaVM* vm){jvm=vm;vm->AttachCurrentThread(&env,nullptr);return JNI_VERSION_1_6;}
}
__attribute__((constructor))
void init(){pthread_t t;pthread_create(&t,nullptr,thread,nullptr);RemapTools::RemapLibrary("libLoader.so");}

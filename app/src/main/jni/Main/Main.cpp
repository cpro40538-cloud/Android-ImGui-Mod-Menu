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
// ================================================================
static int lang_idx=0;
static const char* T(const char* vn,const char* en){return lang_idx==0?vn:en;}

// ================================================================
//  TOGGLES
// ================================================================
static bool bInfAmmo   = false;
static bool bNoRecoil  = false;
static bool bSpeedHack = false;
static bool bFly       = false;
static bool bFullMoney = false;
static bool bFullBox   = false;
static bool bNoAds     = false;

static float speedMult = 2.0f;
static float flySpeed  = 8.0f;

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
static float g_fps=0.f;
static int   g_fpsCount=0;
static long  g_fpsLast=0;
static long  now_ms(){struct timeval tv;gettimeofday(&tv,NULL);return(long)tv.tv_sec*1000+tv.tv_usec/1000;}

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
static void* (*_dget)()=nullptr;
static void* (*_aopen)(void*,const char*)=nullptr;
static void* (*_aimg)(void*)=nullptr;
static void* (*_cname)(void*,const char*,const char*)=nullptr;
static void* (*_cstatic)(void*)=nullptr;
static void InitAPI(){
    if(_dget)return;
    void* lib=dlopen("libil2cpp.so",RTLD_LAZY|RTLD_NOLOAD);if(!lib)return;
    _dget   =(void*(*)())dlsym(lib,"il2cpp_domain_get");
    _aopen  =(void*(*)(void*,const char*))dlsym(lib,"il2cpp_domain_assembly_open");
    _aimg   =(void*(*)(void*))dlsym(lib,"il2cpp_assembly_get_image");
    _cname  =(void*(*)(void*,const char*,const char*))dlsym(lib,"il2cpp_class_from_name");
    _cstatic=(void*(*)(void*))dlsym(lib,"il2cpp_class_get_static_field_data");
}
static void* GetStaticInst(const char* cls){
    InitAPI();
    if(!_dget||!_cname||!_cstatic)return nullptr;
    void* d=_dget();if(!d)return nullptr;
    void* a=_aopen(d,"Assembly-CSharp");if(!a)return nullptr;
    void* img=_aimg(a);if(!img)return nullptr;
    void* c=_cname(img,"",cls);if(!c)return nullptr;
    void* sd=_cstatic(c);if(!sd)return nullptr;
    return *(void**)((uint8_t*)sd+0x0);
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
    if(bNoRecoil){
        *(float*)((uint8_t*)inst+0x5E0)=0.f;
        *(float*)((uint8_t*)inst+0x5E4)=0.f;
        *(float*)((uint8_t*)inst+0x560)=0.f;
        *(float*)((uint8_t*)inst+0x608)=0.f;
        *(float*)((uint8_t*)inst+0x60C)=0.f;
        *(float*)((uint8_t*)inst+0x610)=0.f;
    }
    // Fix can't shoot khi fly
    if(bFly){
        *(bool*)((uint8_t*)inst+0x39C)=true;
        *(bool*)((uint8_t*)inst+0x3E5)=false;
    }
}

static void (*old_WeaponKick)(void* inst);
static void hook_WeaponKick(void* inst){
    if(bNoRecoil)return;
    if(old_WeaponKick)old_WeaponKick(inst);
}

// ================================================================
//  HOOK: Speed + Fly — FPSRigidBodyWalker.FixedUpdate @ 0x12ec900
//  jumpBtn@0x2B1, InputControl@0x30, crouchHold@0x2F
//  flying@0x295, flyDownSpeed@0x298, verticalSpeedAmt@0x2A0
//  Fly control: Jump=len, Crouch=xuong, Khong nhan=hover
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

    if(bFly){
        *(bool*) ((uint8_t*)inst+0x295)=true;
        *(float*)((uint8_t*)inst+0x298)=0.0f;
        *(bool*) ((uint8_t*)inst+0x228)=false;
        *(bool*) ((uint8_t*)inst+0x1A0)=false;
        *(bool*) ((uint8_t*)inst+0x195)=false;

        void* ic=*(void**)((uint8_t*)inst+0x30);
        bool  jmp=*(bool*)((uint8_t*)inst+0x2B1);
        bool  cro=ic?*(bool*)((uint8_t*)ic+0x2F):false;

        float v=0.f;
        if(jmp) v= flySpeed;
        if(cro) v=-flySpeed;
        *(float*)((uint8_t*)inst+0x2A0)=v;
    }else{
        *(bool*) ((uint8_t*)inst+0x295)=false;
        *(float*)((uint8_t*)inst+0x2A0)=0.f;
        *(bool*) ((uint8_t*)inst+0x1A0)=false;
    }
}

// ================================================================
//  HOOK: ArchiveData — Full Money / Full Box
// ================================================================
static int  (*old_ArcGetMoney)(void* s);     static void (*old_ArcUseMoney)(void* s,int n);
static int  (*old_ArcGetTickets)(void* s);   static void (*old_ArcUseTickets)(void* s,int n);
static int  (*old_ArcGetKeys)(void* s);      static void (*old_ArcUseKeys)(void* s,int n);
static int  (*old_ArcGetSKeys)(void* s);     static void (*old_ArcUseSKeys)(void* s,int n);

static int  hk_GetMoney(void* s){if(bFullMoney)return 999999999;return old_ArcGetMoney?old_ArcGetMoney(s):0;}
static void hk_UseMoney(void* s,int n){if(bFullMoney)return;if(old_ArcUseMoney)old_ArcUseMoney(s,n);}
static int  hk_GetTix(void* s){if(bFullMoney)return 999999999;return old_ArcGetTickets?old_ArcGetTickets(s):0;}
static void hk_UseTix(void* s,int n){if(bFullMoney)return;if(old_ArcUseTickets)old_ArcUseTickets(s,n);}
static int  hk_GetKeys(void* s){if(bFullBox)return 999999999;return old_ArcGetKeys?old_ArcGetKeys(s):0;}
static void hk_UseKeys(void* s,int n){if(bFullBox)return;if(old_ArcUseKeys)old_ArcUseKeys(s,n);}
static int  hk_GetSKeys(void* s){if(bFullBox)return 999999999;return old_ArcGetSKeys?old_ArcGetSKeys(s):0;}
static void hk_UseSKeys(void* s,int n){if(bFullBox)return;if(old_ArcUseSKeys)old_ArcUseSKeys(s,n);}

// ================================================================
//  HOOK: ItemDataManager backup display
// ================================================================
static int  (*old_GetCurrency)(int t); static void (*old_SetCurrency)(int t,int n);
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
//  HOOK: NoAds
// ================================================================
static void* g_loadingInst=nullptr;
static void (*old_LoadingSettings)(void* inst);
static void hk_LoadingSettings(void* inst){
    g_loadingInst=inst;
    if(old_LoadingSettings)old_LoadingSettings(inst);
    if(bNoAds)*(bool*)((uint8_t*)inst+0x48)=true;
}
static void (*old_ShowInterAd)(void* self);
static void hk_ShowInterAd(void* self){if(bNoAds)return;if(old_ShowInterAd)old_ShowInterAd(self);}
static void (*old_ShowBannerAd)(void* self,void* cb);
static void hk_ShowBannerAd(void* self,void* cb){if(bNoAds)return;if(old_ShowBannerAd)old_ShowBannerAd(self,cb);}

// ================================================================
//  DRAW
// ================================================================
static void DrawMenu(){
    EnsureAttached();

    // FPS
    g_fpsCount++;
    long nowT=now_ms();
    if(nowT-g_fpsLast>=1000){
        g_fps=g_fpsCount*1000.f/(float)(nowT-g_fpsLast);
        g_fpsCount=0;g_fpsLast=nowT;
    }

    // Polling LoadingOnces
    if(!g_loadingInst)g_loadingInst=GetStaticInst("LoadingOnces");
    if(g_loadingInst&&bNoAds)*(bool*)((uint8_t*)g_loadingInst+0x48)=true;

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

    ImGui::SetNextWindowSize(ImVec2(340,440),ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(10,10),ImGuiCond_FirstUseEver);
    ImGui::Begin("  ZOMBIE3D MOD  |  DUONG DEV  ",nullptr,0);

    // Header + FPS
    ImGui::TextColored(ImVec4(0,.8f,1,1),"  DUONG DEVELOPMENT");
    ImGui::SameLine();
    char fb[24];snprintf(fb,24,"  [FPS: %.0f]",g_fps);
    ImGui::TextColored(g_fps>=60?ImVec4(.3f,1,.3f,1):g_fps>=30?ImVec4(1,.8f,.2f,1):ImVec4(1,.3f,.3f,1),"%s",fb);
    ImGui::Separator();ImGui::Spacing();

    if(ImGui::BeginTabBar("tabs")){

        // ── NGUOI CHOI ────────────────────────────────────────────
        if(ImGui::BeginTabItem(T("NGUOI CHOI","PLAYER"))){
            ImGui::Spacing();
            ImGui::Checkbox(T("Dan Vo Han","Inf Ammo"),&bInfAmmo);ImGui::Spacing();
            ImGui::Checkbox(T("Khong Giat Sung","No Recoil"),&bNoRecoil);ImGui::Spacing();
            ImGui::Checkbox(T("Tang Toc Di Chuyen","Speed Hack"),&bSpeedHack);
            if(bSpeedHack){
                ImGui::SetNextItemWidth(-1);
                ImGui::SliderFloat(T("Toc Do","Speed"),&speedMult,1,5,"x%.1f");
            }
            ImGui::Spacing();
            ImGui::Checkbox(T("Bay","Fly Hack"),&bFly);
            if(bFly){
                ImGui::SetNextItemWidth(-1);
                ImGui::SliderFloat(T("Toc Do Bay","Fly Speed"),&flySpeed,1,30,"%.0f");
                ImGui::TextColored(ImVec4(.5f,1,.5f,1),
                    T("  Nhan JUMP de len cao\n  Nhan CROUCH de xuong thap\n  Khong nhan = hover tai cho",
                      "  Hold JUMP to go up\n  Hold CROUCH to go down\n  No button = hover"));
            }
            ImGui::EndTabItem();
        }

        // ── TAI NGUYEN ────────────────────────────────────────────
        if(ImGui::BeginTabItem(T("TAI NGUYEN","RESOURCES"))){
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(1,.85f,0,1),T("  TIEN (Money+Tickets)","  MONEY+TICKETS"));
            ImGui::Checkbox(T("Full Tien [999,999,999]","Full Money"),&bFullMoney);
            ImGui::Spacing();ImGui::Separator();ImGui::Spacing();
            ImGui::TextColored(ImVec4(1,.6f,0,1),T("  RUONG (Key thuong+vang)","  BOXES (Keys)"));
            ImGui::Checkbox(T("Full Ruong [999,999,999 Key]","Full Boxes"),&bFullBox);
            ImGui::Spacing();ImGui::Separator();ImGui::Spacing();
            ImGui::TextColored(ImVec4(.7f,.7f,1,1),T("  CAI DAT KHAC","  OTHER"));
            ImGui::Checkbox(T("Bo Quang Cao","No Ads"),&bNoAds);
            ImGui::EndTabItem();
        }

        // ── SETTING ───────────────────────────────────────────────
        if(ImGui::BeginTabItem("SETTING")){
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(.5f,1,.5f,1),T("  HIEU NANG","  PERFORMANCE"));
            ImGui::Spacing();
            float fc=g_fps>=60?1.f:g_fps>=30?.5f:0.f;
            ImGui::TextColored(ImVec4(1.f-fc,fc,0,1),"FPS: %.1f",g_fps);
            ImGui::ProgressBar(g_fps/120.f,ImVec2(-1,8));
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(.6f,.6f,.6f,1),
                g_fps>=60?T("Rat tot (60+)","Very good (60+)"):
                g_fps>=30?T("Binh thuong (30+)","Normal (30+)"):
                          T("Thap, co the lag (<30)","Low, may lag (<30)"));
            ImGui::Spacing();ImGui::Separator();ImGui::Spacing();
            ImGui::TextColored(ImVec4(.7f,.7f,1,1),T("  NGON NGU","  LANGUAGE"));
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
    LOGI("[AXIOM] libil2cpp detected, waiting 3s...");
    sleep(3);

    #define HOOK(rva,hk,orig) do{ \
        void* _a=(void*)getAbsoluteAddress("libil2cpp.so",rva); \
        DobbyHook(_a,(void*)hk,(void**)&orig); \
    }while(0)

    HOOK(0x1309740, hook_WeapUpdate,    old_WeapUpdate);
    HOOK(0x130E3BC, hook_WeaponKick,    old_WeaponKick);
    HOOK(0x12EC900, hook_FPSFixedUpdate,old_FPSFixedUpdate);

    HOOK(0x1323878, hk_GetMoney,  old_ArcGetMoney);
    HOOK(0x13238C0, hk_UseMoney,  old_ArcUseMoney);
    HOOK(0x13239D0, hk_GetTix,    old_ArcGetTickets);
    HOOK(0x1323A18, hk_UseTix,    old_ArcUseTickets);
    HOOK(0x1323B08, hk_GetKeys,   old_ArcGetKeys);
    HOOK(0x1323B50, hk_UseKeys,   old_ArcUseKeys);
    HOOK(0x1323D74, hk_GetSKeys,  old_ArcGetSKeys);
    HOOK(0x1323DBC, hk_UseSKeys,  old_ArcUseSKeys);

    HOOK(0x12BF2FC, hk_GetCurrency, old_GetCurrency);
    HOOK(0x12BF350, hk_SetCurrency, old_SetCurrency);

    HOOK(0x12D7344, hk_LoadingSettings, old_LoadingSettings);
    HOOK(0x132FF5C, hk_ShowInterAd,     old_ShowInterAd);
    HOOK(0x132FBEC, hk_ShowBannerAd,    old_ShowBannerAd);

    #undef HOOK
    LOGI("[AXIOM] ALL HOOKS DONE");
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

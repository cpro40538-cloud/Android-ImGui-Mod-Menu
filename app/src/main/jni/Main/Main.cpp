#include "../Include/KittyMemory/MemoryPatch.h"
#include "../Include/ImGui.h"
#include "../Include/RemapTools.h"
#include "../Include/Drawing.h"
#include "../Include/Unity.h"
#include <unistd.h>
#include <dobby.h>
#include <dlfcn.h>
#include <vector>
#include <mutex>
#include <sys/time.h>

static int lang_idx = 0;
bool bGodMode = false, bInfAmmo = false, bNoRecoil = false;
bool bAimbot = false, bEspBox = false, bEspLine = false, bEspDist = false;
float AimFov = 150.0f;

const char* T(const char* vn, const char* en) {
    return (lang_idx == 0) ? vn : en;
}

long now_ms() {
    struct timeval tv; gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

// Lưu instance enemy + timestamp (không lưu vị trí cũ)
struct EspInfo { void* instance; long timestamp; };
EspInfo espList[100];
std::mutex espMtx;

// UNITY API (lấy từ dump)
void* (*Camera_get_main)();
Vector3 (*Camera_WorldToScreenPoint)(void* cam, Vector3 worldPos);
void* (*Component_get_transform)(void* component);
Vector3 (*Transform_get_position)(void* transform); // UnityEngine.Transform.get_position

// IL2CPP thread attach
bool g_attached = false;
void EnsureAttached() {
    if (g_attached) return;
    void* lib = dlopen("libil2cpp.so", RTLD_LAZY);
    if (!lib) return;
    auto domain_get    = (void*(*)())dlsym(lib, "il2cpp_domain_get");
    auto thread_attach = (void*(*)(void*))dlsym(lib, "il2cpp_thread_attach");
    if (domain_get && thread_attach) {
        thread_attach(domain_get());
        g_attached = true;
    }
}

// Anticheat
void (*old_StartDetection)();
void StartDetection() { return; }

// GodMode
float (*old_TakeDamage)(void* instance, void* hitInfo, void* weapon);
float TakeDamage(void* instance, void* hitInfo, void* weapon) {
    if (bGodMode) return 0.0f;
    return old_TakeDamage(instance, hitInfo, weapon);
}

// Infinite Ammo
bool (*old_HaveAmmo)(void* instance);
bool HaveAmmo(void* instance) {
    return bInfAmmo ? true : old_HaveAmmo(instance);
}
void (*old_DecreaseBullets)(void* instance);
void DecreaseBullets(void* instance) {
    if (!bInfAmmo) old_DecreaseBullets(instance);
}

// No Recoil
float (*old_CalcSpread)(void* instance);
float CalcSpread(void* instance) {
    return bNoRecoil ? 0.0f : old_CalcSpread(instance);
}

// Đăng ký enemy (chỉ lưu instance, timestamp)
void RegisterEnemy(void* instance) {
    if (!instance) return;
    if (!bEspBox && !bEspLine && !bEspDist && !bAimbot) return;

    long now = now_ms();
    espMtx.lock();
    bool found = false;
    for (int i = 0; i < 100; i++) {
        if (espList[i].instance == instance) {
            espList[i].timestamp = now;
            found = true; break;
        }
    }
    if (!found) {
        for (int i = 0; i < 100; i++) {
            if (!espList[i].instance || now - espList[i].timestamp > 3000) {
                espList[i].instance  = instance;
                espList[i].timestamp = now;
                break;
            }
        }
    }
    espMtx.unlock();
}

void (*old_NetworkUpdate)(void* instance);
void NetworkUpdate(void* instance) {
    old_NetworkUpdate(instance);
    RegisterEnemy(instance);
}

void (*old_BotUpdate)(void* instance);
void BotUpdate(void* instance) {
    old_BotUpdate(instance);
    RegisterEnemy(instance);
}

// MENU + VẼ ESP REALTIME (mỗi frame)
void DrawMenu() {
    EnsureAttached();

    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding    = 8.0f;
    style.FrameRounding     = 4.0f;
    style.ItemSpacing       = ImVec2(8, 7);
    style.WindowPadding     = ImVec2(10, 10);
    style.Colors[ImGuiCol_WindowBg]      = ImVec4(0.05f, 0.05f, 0.08f, 0.97f);
    style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.0f,  0.2f,  0.5f,  1.0f);
    style.Colors[ImGuiCol_FrameBg]       = ImVec4(0.08f, 0.08f, 0.15f, 1.0f);
    style.Colors[ImGuiCol_CheckMark]     = ImVec4(0.0f,  0.9f,  1.0f,  1.0f);
    style.Colors[ImGuiCol_Tab]           = ImVec4(0.05f, 0.1f,  0.2f,  1.0f);
    style.Colors[ImGuiCol_TabActive]     = ImVec4(0.0f,  0.2f,  0.5f,  1.0f);
    style.Colors[ImGuiCol_TabHovered]    = ImVec4(0.0f,  0.3f,  0.7f,  1.0f);
    style.Colors[ImGuiCol_SliderGrab]    = ImVec4(0.0f,  0.7f,  1.0f,  1.0f);

    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    ImVec2 screen    = ImGui::GetIO().DisplaySize;

    // Vòng tròn Aim
    if (bAimbot)
        draw->AddCircle(
            ImVec2(screen.x * 0.5f, screen.y * 0.5f),
            AimFov, IM_COL32(255, 255, 255, 150), 100, 1.2f
        );

    // VẼ ESP – tính toán position mới mỗi frame
    if ((bEspBox || bEspLine || bEspDist) &&
        Camera_get_main && Component_get_transform &&
        Camera_WorldToScreenPoint && Transform_get_position) {

        void* cam = Camera_get_main();
        if (cam) {
            long now = now_ms();
            espMtx.lock();
            for (int i = 0; i < 100; i++) {
                void* enemy = espList[i].instance;
                if (!enemy) continue;

                if (now - espList[i].timestamp > 3000) {
                    espList[i].instance = nullptr; continue;
                }

                void* trans = Component_get_transform(enemy);
                if (!trans) continue;

                Vector3 footPos = Transform_get_position(trans); // realtime

                if (footPos.X == 0.0f && footPos.Y == 0.0f && footPos.Z == 0.0f) continue;
                if (footPos.Y > 200.0f || footPos.Y < -100.0f) continue;

                Vector3 headPos = footPos;
                headPos.Y += 1.8f;

                Vector3 sFoot = Camera_WorldToScreenPoint(cam, footPos);
                Vector3 sHead = Camera_WorldToScreenPoint(cam, headPos);

                if (sFoot.Z < 0.5f || sFoot.Z > 1000.0f) continue;

                float fx = sFoot.X;
                float fy = screen.y - sFoot.Y;
                float hy = screen.y - sHead.Y;

                float h = fy - hy;
                if (h < 5.0f) continue;

                float w = h * 0.45f;
                float d = sFoot.Z;

                if (bEspLine)
                    draw->AddLine(
                        ImVec2(screen.x * 0.5f, screen.y),
                        ImVec2(fx, fy),
                        IM_COL32(255, 50, 50, 220), 1.2f
                    );

                if (bEspBox)
                    draw->AddRect(
                        ImVec2(fx - w * 0.5f, hy),
                        ImVec2(fx + w * 0.5f, fy),
                        IM_COL32(0, 255, 0, 230),
                        0.0f, 0, 1.5f
                    );

                if (bEspDist) {
                    char buf[16];
                    snprintf(buf, sizeof(buf), "[%.0fm]", d);
                    draw->AddText(
                        ImVec2(fx - 12.0f, hy - 14.0f),
                        IM_COL32(255, 220, 0, 255), buf
                    );
                }
            }
            espMtx.unlock();
        }
    }

    // UI Menu
    ImGui::SetNextWindowSize(ImVec2(320, 320), ImGuiCond_FirstUseEver);
    ImGui::Begin("  AWP ULTIMATE  |  DUONG DEV  ");

    ImGui::TextColored(ImVec4(0.0f, 0.8f, 1.0f, 1.0f), "  DUONG DEV TEAM");
    ImGui::TextColored(ImVec4(0.4f, 0.4f, 0.4f, 1.0f), "  t.me/chanelmodteam");
    ImGui::Separator(); ImGui::Spacing();

    if (ImGui::BeginTabBar("tabs")) {

        if (ImGui::BeginTabItem(T("NGUOI CHOI", "PLAYER"))) {
            ImGui::Spacing();
            ImGui::Checkbox(T("Bat Tu", "God Mode"), &bGodMode);
            ImGui::Spacing();
            ImGui::Checkbox(T("Vo Han Dan", "Infinite Ammo"), &bInfAmmo);
            ImGui::Spacing();
            ImGui::Checkbox(T("Ban Khong Giat", "No Recoil"), &bNoRecoil);
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem(T("DINH VI", "ESP"))) {
            ImGui::Spacing();
            ImGui::Checkbox(T("Ve Hop", "Draw Box"), &bEspBox);
            ImGui::Spacing();
            ImGui::Checkbox(T("Ve Duong", "Draw Line"), &bEspLine);
            ImGui::Spacing();
            ImGui::Checkbox(T("Khoang Cach", "Distance"), &bEspDist);
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem(T("AIM", "AIM"))) {
            ImGui::Spacing();
            ImGui::Checkbox(T("Vong Tron Aim", "Aim Circle"), &bAimbot);
            if (bAimbot) {
                ImGui::Spacing();
                ImGui::SliderFloat(
                    T("Pham Vi", "FOV"),
                    &AimFov, 50.0f, 500.0f, "%.0fpx");
            }
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem(T("CAI DAT", "SETTING"))) {
            ImGui::Spacing();
            const char* langs[] = { "Tieng Viet", "English" };
            ImGui::Combo(T("Ngon ngu", "Language"), &lang_idx, langs, 2);
            ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "ADMIN: @DUONG DEV");
            ImGui::TextColored(ImVec4(0.0f, 0.8f, 1.0f, 1.0f), "TG: t.me/chanelmodteam");
            ImGui::TextColored(ImVec4(0.3f, 0.3f, 0.3f, 1.0f), "AWP v1.8.0 | Duong Dev");
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }
    ImGui::End();
}

void* thread(void*) {
    initModMenu((void*)DrawMenu);
    do { sleep(1); } while (getAbsoluteAddress("libil2cpp.so", 0) == 0);
    sleep(5);

    // Lấy địa chỉ API
    Camera_get_main           = (void*(*)())
        getAbsoluteAddress("libil2cpp.so", 0x2b6dc50);
    Camera_WorldToScreenPoint = (Vector3(*)(void*, Vector3))
        getAbsoluteAddress("libil2cpp.so", 0x2b6da24);
    Component_get_transform   = (void*(*)(void*))
        getAbsoluteAddress("libil2cpp.so", 0x2b95be4);
    Transform_get_position    = (Vector3(*)(void*))
        getAbsoluteAddress("libil2cpp.so", 0x2ba1fdc); // UnityEngine.Transform.get_position

    // Hooks
    DobbyHook((void*)getAbsoluteAddress("libil2cpp.so", 0x2a93d74),
              (void*)StartDetection, (void**)&old_StartDetection);
    DobbyHook((void*)getAbsoluteAddress("libil2cpp.so", 0x24107bc),
              (void*)TakeDamage, (void**)&old_TakeDamage);
    DobbyHook((void*)getAbsoluteAddress("libil2cpp.so", 0x23ff1e8),
              (void*)HaveAmmo, (void**)&old_HaveAmmo);
    DobbyHook((void*)getAbsoluteAddress("libil2cpp.so", 0x23ff128),
              (void*)DecreaseBullets, (void**)&old_DecreaseBullets);
    DobbyHook((void*)getAbsoluteAddress("libil2cpp.so", 0x22f9678),
              (void*)CalcSpread, (void**)&old_CalcSpread);
    DobbyHook((void*)getAbsoluteAddress("libil2cpp.so", 0x230ddb0),
              (void*)NetworkUpdate, (void**)&old_NetworkUpdate);
    DobbyHook((void*)getAbsoluteAddress("libil2cpp.so", 0x22ebe84),
              (void*)BotUpdate, (void**)&old_BotUpdate);

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

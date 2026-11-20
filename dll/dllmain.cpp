#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <GL/gl.h>
#include <vector>
#include <cmath>
#include <cstdio>
#include <cstdarg>
#include <string>
#include <map>
#include <set>
#include <algorithm>
#include "MinHook.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_win32.h"

// ── Выбор рендер-бэкенда ImGui: 1 = OpenGL3 (тест, VBO+шейдер), 0 = OpenGL2 (legacy, vertex arrays) ──
// Управляется из сборки: /p:AdditionalOptions="/DUSE_GL3_BACKEND=0" вернёт GL2. По умолчанию тестовый GL3.
#ifndef USE_GL3_BACKEND
#define USE_GL3_BACKEND 1
#endif
#if USE_GL3_BACKEND
#include "imgui_impl_opengl3.h"
#define IMGL_Init()            ImGui_ImplOpenGL3_Init(nullptr)   // nullptr = автодетект GLSL из контекста (не знаем версию инжектнутого)
#define IMGL_NewFrame()        ImGui_ImplOpenGL3_NewFrame()
#define IMGL_RenderDrawData(d) ImGui_ImplOpenGL3_RenderDrawData(d)
#define IMGL_Shutdown()        ImGui_ImplOpenGL3_Shutdown()
#define IMGL_BACKEND_NAME      "OpenGL3"
#else
#include "imgui_impl_opengl2.h"
#define IMGL_Init()            ImGui_ImplOpenGL2_Init()
#define IMGL_NewFrame()        ImGui_ImplOpenGL2_NewFrame()
#define IMGL_RenderDrawData(d) ImGui_ImplOpenGL2_RenderDrawData(d)
#define IMGL_Shutdown()        ImGui_ImplOpenGL2_Shutdown()
#define IMGL_BACKEND_NAME      "OpenGL2"
#endif

// ── Способ перехвата GL-функций: 1 = ПРЯМОЙ MinHook по экспортам opengl32 (инжект в любой момент, даже когда игра
//    уже запущена), 0 = перехват резолвинга через GetProcAddressForCaller (ловит только момент старта -> нужен пре-лонч).
//    Прямой режим ловит вызовы игры независимо от времени инжекта (кэш-указатели LWJGL для GL 1.1 = адреса экспортов
//    opengl32, а мы патчим код по этим адресам — как уже сделано для wglSwapBuffers/glFinish).
#ifndef GL_HOOK_MODE_DIRECT
#define GL_HOOK_MODE_DIRECT 1
#endif
// РАНТАЙМ-выбор механизма перехвата GL (не компайл-флаг), чтобы одна DLL работала на обоих клиентах:
//   g_directHooks = true  -> ПРЯМОЙ MinHook экспортов opengl32 (Lunar; инжект в рантайм ок).
//   g_directHooks = false -> РЕЗОЛВ-перехват через GetProcAddressForCaller, БЕЗ патча байтов opengl32
//                            (VimeWorld: прямой MinHook GL-функций там не работает; это «старый» механизм).
// Value: marker %TEMP%\mle_hook_mode.txt (written by injector: "resolve"/"direct"); no marker -> auto
// загрузки opengl32 на момент инжекта (уже загружен = игра запущена = прямой; ещё нет = пре-лонч = резолв).
static bool g_directHooks = (GL_HOOK_MODE_DIRECT != 0);

#include "radar/mc_players.h"   // Radar module: players + real nicknames (for nametags)
#include <unordered_set>

// Auth/anti-debug guards removed in this fork — no-op expansions so call-sites compile unchanged.
#define AUTH_GUARD_A()           ((void)0)
#define AUTH_GUARD_A_RET(v)      ((void)0)
#define AUTH_GUARD_B()           ((void)0)
#define AUTH_GUARD_B_RET(v)      ((void)0)
#define AUTH_GUARD_C()           ((void)0)
#define AUTH_GUARD_C_RET(v)      ((void)0)
#define AUTH_GUARD_D()           ((void)0)
#define AUTH_GUARD_D_RET(v)      ((void)0)
#define AUTH_GUARD_E()           ((void)0)
#define AUTH_GUARD_E_RET(v)      ((void)0)

#pragma comment(lib, "opengl32.lib")
#include "outline_fx.h"   // screen-space обводка силуэта (Glow) через FBO-маску + шейдер
#include "overlay2d.h"    // нативный 2D-рендер оверлея (неймтеги без ImGui-кадра)
#include "monocraft_ttf.h" // встроенный MC-стиль шрифт для неймтегов


// ══════════════════════════════════════════════════════════════════════════════
//  OPENGL RENDER OVERLAY & DIAGNOSTICS MODULE (IMGUI INTEGRATION)
// ══════════════════════════════════════════════════════════════════════════════

struct KeybindConfig {
    int  key;           // 0 = нет горячей клавиши ([None])
    bool toggleMode;    // true = триггерный режим (переключение по нажатию), false = режим удержания
};

struct RenderConfig {
    // ── Геометрический оверлей и эффекты глубины ──────────────────────────────
    bool wallhack          = false;  // Смещение глубины полигонов (glPolygonOffset)
    KeybindConfig kWallhack = { 0, true };

    // Свечение по силуэту модели (Glow): перерисовка всей модели в FBO-маску + мягкое свечение шейдером (outline_fx.h).
    // Всегда видимое сквозь стены — FBO-based, игнорирует depth.
    bool  outline          = false;
    float outlineColor[4]  = { 1.0f, 1.0f, 1.0f, 1.0f };     // цвет свечения (белый по умолчанию)
    float outlineSpread    = 1.5f;                           // ширина/радиус свечения

    bool esp2d             = false;  // Отрисовка 2D экранной проекции (Screen Box)
    KeybindConfig kEsp2d   = { 0, true };
    float esp2dColor[4]    = { 1.0f, 1.0f, 1.0f, 1.0f };     // цвет 2D рамки (default: белый)

    bool tracers           = false;  // Векторные направляющие линии к объектам сцены
    KeybindConfig kTracers = { 0, true };
    int  tracersOrigin     = 1;      // Точка начала линий: 0 = Верх экрана, 1 = Центр (прицел), 2 = Низ экрана

    bool showDistance      = false;  // Расчет и вывод метрической дистанции до объекта
    float distanceOffsetY  = 0.0f;   // доп смещение бейджа дистанции по Y (пиксели)
    float distanceScale    = 1.0f;   // масштаб текста дистанции (1.0 = нативный размер MC-шрифта)

    bool nametags          = false;  // Nametags: real player nicknames (from radar) above boxes
    float nametagOffsetY   = 0.0f;   // Extra Y offset for nametag (px)
    float nametagScale     = 1.0f;   // Nametag text scale

    // ── Input automation ──────────────────────────────────────────────────────
    bool aimbot            = false;
    KeybindConfig kAimbot  = { 0, false };
    float aimSmooth        = 0.35f;
    float aimFov           = 300.0f;

    bool autoclicker       = false;  // Automatic mouse clicks on a timer
    KeybindConfig kAutoclicker = { 0, false };
    int  cps               = 12;

    // ── Menu control ──────────────────────────────────────────────────────────
    int  menuKey           = VK_INSERT;
};

static RenderConfig g_config;
static bool g_menuOpen = false;

// ── Radar module (dynamic discovery + PlayerRadar) for nametags ────────────────
static mc::Layout      g_radarLayout;
static mc::PlayerRadar g_playerRadar;
static volatile bool   g_radarReady = false;
static volatile bool   g_radarStop  = false;   // signal to stop discovery thread on DLL unload

// UTF-16 (ник) -> UTF-8 для ImGui
static std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

// ── Captured scene object structs ──────────────────────────────────────────────
struct CapturedEntity {
    float eyeX, eyeY, eyeZ;     // 3D координаты в системе координат камеры (Eye Space)
    float screenX, screenY;     // 2D экранная позиция верхней точки объекта (Screen Space)
    float screenBottomY;        // 2D экранная позиция основания объекта
    float distance;             // Евклидово расстояние от камеры до объекта
    bool  isChest;              // Флаг контейнера / блока хранения
    bool  isMurderer;           // Флаг приоритетного объекта сцены
    float relX, relY, relZ;     // Позиция сущности в МИРОВЫХ осях относительно камеры (аргумент glTranslatef перед scale 0.9375)
};

static std::vector<CapturedEntity> g_entities;
static int g_itemMvReads = 0;                   // кап glGetFloatv(MODELVIEW) на предметы за кадр (перф)
static CRITICAL_SECTION g_entityLock;
static float g_lastProj[16]      = {};
static float g_lastTx = 0, g_lastTy = 0, g_lastTz = 0;   // последний аргумент glTranslatef (позиция сущности отн. камеры, мировые оси)
static int g_screenWidth        = 1920;
static int g_screenHeight       = 1080;
static bool g_inEntityRender     = false;
static wchar_t g_logFilePath[MAX_PATH] = {};

// ── ImGui & Window State ──────────────────────────────────────────────────────
static bool g_imguiInitialized = false;
static HWND g_hwnd             = nullptr;
static WNDPROC oWndProc        = nullptr;
static int g_waitingBindId     = -1;
static ImFont* g_fontBody      = nullptr;
static ImFont* g_fontBold      = nullptr;
static ImFont* g_fontTitle     = nullptr;
static ImFont* g_fontSmall     = nullptr;
static ImFont* g_fontMC        = nullptr;   // Monocraft (MC-стиль) — для нативных неймтегов

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// ── Логирование ───────────────────────────────────────────────────────────────
// В Release-билде Log — пустой макрос. Компилятор вырежет все литералы (dead code elimination).
#ifdef NDEBUG
    #define Log(...) ((void)0)
#else
    static void Log(const char* fmt, ...) {
        char buf[2048];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);

        OutputDebugStringA(buf);
        if (g_logFilePath[0]) {
            FILE* f = nullptr;
            if (_wfopen_s(&f, g_logFilePath, L"a+") == 0 && f) {
                fprintf(f, "[HW-HOOK] %s\n", buf);
                fclose(f);
            }
        }
    }
#endif

// LogCrit — noop (строки + вызовы вырезаются компилятором).
#define LogCrit(...) ((void)0)


// ── Имена клавиш ──────────────────────────────────────────────────────────────
static const char* GetKeyName(int vk) {
    if (vk == 0) return "None";
    if (vk == VK_LBUTTON) return "LMB";
    if (vk == VK_RBUTTON) return "RMB";
    if (vk == VK_MBUTTON) return "MMB";
    if (vk == VK_XBUTTON1) return "X1";
    if (vk == VK_XBUTTON2) return "X2";
    if (vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT) return "SHIFT";
    if (vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL) return "CTRL";
    if (vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU) return "ALT";
    if (vk == VK_SPACE) return "SPACE";
    if (vk == VK_INSERT) return "INSERT";
    if (vk == VK_DELETE) return "DELETE";
    if (vk == VK_TAB) return "TAB";
    if (vk == VK_CAPITAL) return "CAPS";
    if (vk >= 'A' && vk <= 'Z') {
        static char ch[2] = {0, 0};
        ch[0] = (char)vk;
        return ch;
    }
    if (vk >= '0' && vk <= '9') {
        static char num[2] = {0, 0};
        num[0] = (char)vk;
        return num;
    }
    if (vk >= VK_F1 && vk <= VK_F12) {
        static char f[8];
        snprintf(f, sizeof(f), "F%d", vk - VK_F1 + 1);
        return f;
    }
    static char hex[16];
    snprintf(hex, sizeof(hex), "0x%X", vk);
    return hex;
}

// ── Проекция 3D -> 2D ─────────────────────────────────────────────────────────
static bool ProjectEyeToScreen(float eyeX, float eyeY, float eyeZ,
                               const float* proj, int screenW, int screenH,
                               float& outX, float& outY) {
    if (eyeZ >= -0.1f) return false;

    float clipX = eyeX * proj[0] + eyeY * proj[4] + eyeZ * proj[8]  + proj[12];
    float clipY = eyeX * proj[1] + eyeY * proj[5] + eyeZ * proj[9]  + proj[13];
    float clipW = eyeX * proj[3] + eyeY * proj[7] + eyeZ * proj[11] + proj[15];

    if (clipW <= 0.0001f) return false;

    float ndcX = clipX / clipW;
    float ndcY = clipY / clipW;

    outX = (ndcX + 1.0f) * 0.5f * (float)screenW;
    outY = (1.0f - ndcY) * 0.5f * (float)screenH;
    return true;
}

// ── Оригинальные указатели на функции ──────────────────────────────────────────
typedef FARPROC (WINAPI* pfn_GetProcAddressForCaller)(HMODULE hModule, LPCSTR lpProcName, LPVOID lpCaller);
typedef BOOL    (WINAPI* pfn_wglSwapBuffers)(HDC hdc);

typedef void (APIENTRY* pfn_glOrtho)(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top, GLdouble zNear, GLdouble zFar);
typedef void (APIENTRY* pfn_glScalef)(GLfloat x, GLfloat y, GLfloat z);
typedef void (APIENTRY* pfn_glTranslatef)(GLfloat x, GLfloat y, GLfloat z);
typedef void (APIENTRY* pfn_glEnable)(GLenum cap);
typedef void (APIENTRY* pfn_glDisable)(GLenum cap);
typedef void (APIENTRY* pfn_glRotatef)(GLfloat angle, GLfloat x, GLfloat y, GLfloat z);

static pfn_GetProcAddressForCaller oGetProcAddressForCaller = nullptr;
static pfn_wglSwapBuffers          oWglSwapBuffers          = nullptr;
static pfn_glOrtho                 oGlOrtho                 = nullptr;
static pfn_glScalef                oGlScalef                = nullptr;
static pfn_glTranslatef            oGlTranslatef            = nullptr;
static pfn_glEnable                oGlEnable                = nullptr;
static pfn_glDisable               oGlDisable               = nullptr;
static pfn_glRotatef               oGlRotatef               = nullptr;

// ── Зонд отрисовки моделей сущностей: определить, ЧЕМ рисуются модели (display list / VBO / immediate) для outline ──
typedef void (APIENTRY* pfn_glCallList)(GLuint list);
typedef void (APIENTRY* pfn_glDrawArrays)(GLenum mode, GLint first, GLsizei count);
typedef void (APIENTRY* pfn_glDrawElements)(GLenum mode, GLsizei count, GLenum type, const GLvoid* indices);
typedef void (APIENTRY* pfn_glBegin)(GLenum mode);
static pfn_glCallList     oGlCallList     = nullptr;
static pfn_glDrawArrays   oGlDrawArrays   = nullptr;
static pfn_glDrawElements oGlDrawElements = nullptr;
static pfn_glBegin        oGlBegin        = nullptr;
static bool g_glProbe = false;                                           // тумблер диагностики отрисовки (пишет в лог раз в 2с)
static unsigned g_cCallList = 0, g_cDrawArr = 0, g_cDrawElem = 0, g_cBegin = 0;   // вызовы внутри окна entity-render за интервал

// OUTLINE: копим части модели (display list + его modelview). Hull-стиль рисует оболочки в конце сущности;
// Edge/Glow-стили (outline_fx.h) перерисовывают всю модель в FBO-маску и обводят силуэт шейдером в swapbuffers.
static std::vector<OutlinePart> g_outlineParts;   // OutlinePart определён в outline_fx.h; заполняется в hkGlCallList, чистится в swapbuffers

// Ориентация камеры, пойманная из glRotatef в начале кадра (для world->screen проекции неймтегов).
static float g_camPitch = 0.0f, g_camYaw = 0.0f;
static bool  g_camValid = false, g_camCaptured = false, g_camGotPitch = false;
// Реконструкция eye-space БЕЗ glGetFloatv (0=валидируем, 1=реконструкция ок, 2=откат на glGetFloatv).
static int   g_reconMode = 0, g_reconOk = 0, g_reconBad = 0;

// ══════════════════════════════════════════════════════════════════════════════
//  ПЕРЕХВАТ ФУНКЦИЙ OPENGL (PIPELINE HOOKS)
// ══════════════════════════════════════════════════════════════════════════════

void APIENTRY hkGlScalef(GLfloat x, GLfloat y, GLfloat z) {
    // Entity model geometry is drawn with glScalef(0.9375) (RenderLivingEntity::preRenderCallback)
    if (fabs(x - 0.9375f) < 0.002f && fabs(y - 0.9375f) < 0.002f && fabs(z - 0.9375f) < 0.002f) {
        g_inEntityRender = true;

        if (g_config.wallhack) {
            glEnable(GL_POLYGON_OFFSET_FILL);
            glPolygonOffset(1.0f, -1100000.0f);
        }

        bool needEntities = g_config.esp2d || g_config.tracers || g_config.showDistance || g_config.nametags || g_config.outline;
        if (needEntities) {
            // Eye-space БЕЗ glGetFloatv: MODELVIEW[12..14] = R(pitch)·R(yaw)·(последний translate сущности).
            // Реальные углы камеры (hkGlRotatef) + последний translate. Первые ~40 захватов сверяем с glGetFloatv:
            // совпало -> дальше без него (нет столлов конвейера); не совпало -> откат на glGetFloatv (без регресса).
            const float yr = (g_camYaw + 180.0f) * 0.01745329f, pr = g_camPitch * 0.01745329f;
            const float cyw = cosf(yr), syw = sinf(yr), cpi = cosf(pr), spi = sinf(pr);
            const float ry_x =  g_lastTx * cyw + g_lastTz * syw;   // Ry(yaw)
            const float ry_y =  g_lastTy;
            const float ry_z = -g_lastTx * syw + g_lastTz * cyw;
            const float recX = ry_x, recY = ry_y * cpi - ry_z * spi, recZ = ry_y * spi + ry_z * cpi;   // Rx(pitch)
            float eyeX, eyeY, eyeZ;
            if (g_reconMode == 1 && g_camValid) {                  // реконструкция подтверждена -> без glGetFloatv
                eyeX = recX; eyeY = recY; eyeZ = recZ;
            } else {
                float mv[16]; glGetFloatv(GL_MODELVIEW_MATRIX, mv);
                eyeX = mv[12]; eyeY = mv[13]; eyeZ = mv[14];
                if (g_reconMode == 0 && g_camValid) {              // самопроверка математики реконструкции
                    const float d = fabsf(recX - eyeX) + fabsf(recY - eyeY) + fabsf(recZ - eyeZ);
                    if (d < 0.15f) ++g_reconOk; else ++g_reconBad;
                    if (g_reconOk + g_reconBad >= 40) g_reconMode = (g_reconBad == 0) ? 1 : 2;
                }
            }
            float dist = sqrtf(eyeX * eyeX + eyeY * eyeY + eyeZ * eyeZ);

            if (dist > 0.8f && dist < 120.0f) {
                float sx = 0.0f, syHead = 0.0f, syFeet = 0.0f;
                ProjectEyeToScreen(eyeX, eyeY + 1.85f, eyeZ, g_lastProj, g_screenWidth, g_screenHeight, sx, syHead);
                ProjectEyeToScreen(eyeX, eyeY,         eyeZ, g_lastProj, g_screenWidth, g_screenHeight, sx, syFeet);

                CapturedEntity ent;
                ent.eyeX          = eyeX;
                ent.eyeY          = eyeY;
                ent.eyeZ          = eyeZ;
                ent.screenX       = sx;
                ent.screenY       = syHead;
                ent.screenBottomY = syFeet;
                ent.distance      = dist;
                ent.isChest       = false;
                ent.isMurderer    = false;
                ent.relX          = g_lastTx;   // позиция этой сущности отн. камеры в МИРОВЫХ осях (последний translate перед scale)
                ent.relY          = g_lastTy;
                ent.relZ          = g_lastTz;

                EnterCriticalSection(&g_entityLock);
                g_entities.push_back(ent);              // g_lastProj уже актуален (снят в hkGlRotatef) — memcpy не нужен
                LeaveCriticalSection(&g_entityLock);
            }
        }
    }
    if (oGlScalef) oGlScalef(x, y, z);
}

// Ловим ориентацию камеры: MC в начале кадра делает glRotatef(pitch,1,0,0) затем glRotatef(yaw+180,0,1,0).
// Берём ПЕРВУЮ такую пару за кадр (флаг сбрасывается в swapBuffers). Это точные углы игры, без чтения памяти.
void APIENTRY hkGlRotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z) {
    if (oGlRotatef) oGlRotatef(angle, x, y, z);
    if (g_camCaptured) return;
    if (!g_camGotPitch) {
        if (fabs(x - 1.0f) < 0.01f && fabs(y) < 0.01f && fabs(z) < 0.01f && fabs(angle) <= 90.5f) {
            g_camPitch = angle; g_camGotPitch = true;
        }
    } else {
        if (fabs(y - 1.0f) < 0.01f && fabs(x) < 0.01f && fabs(z) < 0.01f) {
            g_camYaw = angle - 180.0f; g_camCaptured = true; g_camValid = true;
            glGetFloatv(GL_PROJECTION_MATRIX, g_lastProj);   // проекция уже выставлена (setupCameraTransform) — берём её тут, чтобы неймтеги не зависели от рендера сущностей
        } else {
            g_camGotPitch = false;   // между pitch и yaw вклинился другой поворот — это не камера, ждём настоящую пару
        }
    }
}

void APIENTRY hkGlTranslatef(GLfloat x, GLfloat y, GLfloat z) {
    if (oGlTranslatef) oGlTranslatef(x, y, z);
    g_lastTx = x; g_lastTy = y; g_lastTz = z;   // remember last translate (entity position relative to camera, before scale 0.9375)
}

void APIENTRY hkGlEnable(GLenum cap) {
    if (oGlEnable) oGlEnable(cap);

    // Восстановление смещения полигонов при включении освещения (завершение прохода рендера сущностей)
    if (cap == GL_LIGHTING) {
        if (g_inEntityRender) {
            if (g_config.wallhack) {
                glDisable(GL_POLYGON_OFFSET_FILL);
                glPolygonOffset(1.0f, 0.0f);
            }
            g_inEntityRender = false;
        }
    }
}

void APIENTRY hkGlDisable(GLenum cap) {
    if (oGlDisable) oGlDisable(cap);

    // Restore polygon offset when texturing / lighting state changes.
    if (cap == 0x803A && g_inEntityRender) {
        if (g_config.wallhack) {
            glDisable(GL_POLYGON_OFFSET_FILL);
            glPolygonOffset(1.0f, 0.0f);
        }
        g_inEntityRender = false;
    }
}

// ── Зонд + GLOW: модели рисуются через display lists. Рисуем модель как обычно, а part+матрицу КОПИМ —
// в swapbuffers по ним перерисовываем всю модель в FBO-маску и рисуем свечение по силуэту (outline_fx.h).
void APIENTRY hkGlCallList(GLuint list) {
    if (g_glProbe && g_inEntityRender) ++g_cCallList;
    if (!oGlCallList) return;
    oGlCallList(list);                                         // normal model (color + depth)
    if (g_config.outline && g_inEntityRender && g_outlineParts.size() < 8192) {
        OutlinePart p; p.list = list;
        glGetFloatv(GL_MODELVIEW_MATRIX, p.mv);               // FF-матрица CPU-side -> дёшево, без GPU-столла
        g_outlineParts.push_back(p);
    }
}
void APIENTRY hkGlDrawArrays(GLenum m, GLint f, GLsizei c) { if (g_glProbe && g_inEntityRender) ++g_cDrawArr; if (oGlDrawArrays) oGlDrawArrays(m, f, c); }
void APIENTRY hkGlDrawElements(GLenum m, GLsizei c, GLenum t, const GLvoid* i) { if (g_glProbe && g_inEntityRender) ++g_cDrawElem; if (oGlDrawElements) oGlDrawElements(m, c, t, i); }
void APIENTRY hkGlBegin(GLenum m) { if (g_glProbe && g_inEntityRender) ++g_cBegin; if (oGlBegin) oGlBegin(m); }

void APIENTRY hkGlOrtho(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top,
                        GLdouble zNear, GLdouble zFar) {
    // Intercept orthographic projection before HUD — draw tracers here (the only 3D-ish overlay).
    if (fabs(zNear - 1000.0) < 1.0 && fabs(zFar - 3000.0) < 1.0) {
        if (g_config.tracers) {
            glPushAttrib(GL_ALL_ATTRIB_BITS);

            glDisable(GL_TEXTURE_2D);
            glDisable(GL_LIGHTING);
            glDisable(GL_DEPTH_TEST);
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glLineWidth(1.8f);

            glMatrixMode(GL_PROJECTION);
            glPushMatrix();
            glLoadMatrixf(g_lastProj);

            glMatrixMode(GL_MODELVIEW);
            glPushMatrix();
            glLoadIdentity();

            EnterCriticalSection(&g_entityLock);
            for (const auto& ent : g_entities) {
                if (ent.isChest) continue;
                glColor4f(1.0f, 0.3f, 0.3f, 0.65f);
                glBegin(GL_LINES);
                float startY = 0.0f;
                if (g_config.tracersOrigin == 0) startY = 0.8f;
                else if (g_config.tracersOrigin == 1) startY = 0.0f;
                else if (g_config.tracersOrigin == 2) startY = -0.6f;
                glVertex3f(0.0f, startY, -1.0f);
                glVertex3f(ent.eyeX, ent.eyeY + 0.9f, ent.eyeZ);
                glEnd();
            }
            LeaveCriticalSection(&g_entityLock);

            glMatrixMode(GL_MODELVIEW);
            glPopMatrix();

            glMatrixMode(GL_PROJECTION);
            glPopMatrix();

            glPopAttrib();
        }
    }

    if (oGlOrtho) oGlOrtho(left, right, bottom, top, zNear, zFar);
}

// ══════════════════════════════════════════════════════════════════════════════
//  2D РЕНДЕР ОВЕРЛЕЯ IMGUI: РАМКИ ПРОЕКЦИИ И ДИАГНОСТИЧЕСКИЕ МЕТКИ
// ══════════════════════════════════════════════════════════════════════════════

// ── Отрисовка 2D проекционных рамок и дистанции через ImGui DrawList ──────────
// НАТИВНО: 2D-боксы (esp2d) + бейдж дистанции. Вызывается внутри nov::Begin/End.
static void RenderOverlayNative() {
    if (!g_config.esp2d && !g_config.showDistance) return;
    EnterCriticalSection(&g_entityLock);
    for (const auto& ent : g_entities) {
        if (ent.isChest) continue;
        if (ent.screenX <= 0.0f || ent.screenY <= 0.0f || ent.screenBottomY <= 0.0f) continue;
        float boxHeight = ent.screenBottomY - ent.screenY;
        if (boxHeight < 5.0f || boxHeight > 1000.0f) continue;
        const float boxWidth = boxHeight * 0.55f;
        const float left = ent.screenX - boxWidth * 0.5f, right = ent.screenX + boxWidth * 0.5f;
        const float top = ent.screenY, bottom = ent.screenBottomY;

        if (g_config.esp2d) {
            const ImU32 userColor = IM_COL32(
                (int)(g_config.esp2dColor[0] * 255),
                (int)(g_config.esp2dColor[1] * 255),
                (int)(g_config.esp2dColor[2] * 255),
                (int)(g_config.esp2dColor[3] * 255));
            const ImU32 boxColor = ent.isMurderer ? IM_COL32(239, 68, 68, 255) : userColor;
            nov::RectOutline(left - 1, top - 1, right + 1, bottom + 1, IM_COL32(0, 0, 0, 220), ent.isMurderer ? 2.5f : 2.0f);
            nov::RectOutline(left, top, right, bottom, boxColor, ent.isMurderer ? 2.0f : 1.5f);
        }
        if (g_config.showDistance) {
            char distStr[48];
            if (ent.isMurderer) snprintf(distStr, sizeof(distStr), "TARGET %.0fm", ent.distance);
            else                snprintf(distStr, sizeof(distStr), "%.0fm", ent.distance);
            // Round scale to integer size (7..24 px) чтобы попасть в предпечённый атлас.
            float fpxReqD = nov::PX_TAG * g_config.distanceScale;
            if (fpxReqD < 7.0f) fpxReqD = 7.0f; else if (fpxReqD > 24.0f) fpxReqD = 24.0f;
            const float fpx = (float)(int)(fpxReqD + 0.5f);
            const float tw  = nov::TextW(distStr, fpx);
            const float txtX = ent.screenX - tw * 0.5f;
            const float txtY = top - fpx - 4.0f + g_config.distanceOffsetY;
            const ImU32 bgCol   = ent.isMurderer ? IM_COL32(40, 8, 12, 170) : IM_COL32(0, 0, 0, 140);
            const ImU32 textCol = ent.isMurderer ? IM_COL32(255, 120, 120, 255) : IM_COL32(255, 255, 255, 255);
            nov::FillRect(txtX - 3, txtY - 1, txtX + tw + 3, txtY + fpx + 2, bgCol);
            nov::Text(txtX, txtY, distStr, textCol, fpx, true);
        }
    }
    LeaveCriticalSection(&g_entityLock);
}

// ── world->screen по пойманной ориентации камеры (glRotatef) + позиции глаз локального игрока (radar) ──
static bool WorldToScreen(double wx, double wy, double wz, float& sx, float& sy) {
    if (!g_camValid) return false;
    double cx, cy, cz; if (!g_playerRadar.LocalEye(cx, cy, cz)) return false;
    const double rx = wx - cx, ry = wy - cy, rz = wz - cz;
    const double d2r = 3.14159265358979323846 / 180.0;
    const double ya = ((double)g_camYaw + 180.0) * d2r, pa = (double)g_camPitch * d2r;
    const double cyaw = cos(ya), syaw = sin(ya);
    const double v1x =  cyaw * rx + syaw * rz;                 // Ry(yaw+180)
    const double v1y =  ry;
    const double v1z = -syaw * rx + cyaw * rz;
    const double cp = cos(pa), sp = sin(pa);                   // Rx(pitch)
    const double ex = v1x, ey = cp * v1y - sp * v1z, ez = sp * v1y + cp * v1z;
    return ProjectEyeToScreen((float)ex, (float)ey, (float)ez, g_lastProj, g_screenWidth, g_screenHeight, sx, sy);
}

// ── НЕЙМТЕГИ ПО МИРУ: проецируем КАЖДОГО нашего игрока (кроме локального) в экран и рисуем ник. Никакого
//    сопоставления с боксами (нет путаницы). Позиции перечитываем каждый кадр по адресам сущностей (real-time). ──
// «Развернуть» eye-space (пойманный ESP) обратно в мировые координаты: world = cam + Ry(-a)·Rx(-p)·eye.
static void EyeToWorld(double ex, double ey, double ez, double cx, double cy, double cz,
                       double& wx, double& wy, double& wz) {
    const double d2r = 3.14159265358979323846 / 180.0;
    const double a = ((double)g_camYaw + 180.0) * d2r, p = (double)g_camPitch * d2r;
    const double ca = cos(a), sa = sin(a), cp = cos(p), sp = sin(p);
    const double v1x = ex;                       // Rx(-pitch)
    const double v1y = cp * ey + sp * ez;
    const double v1z = -sp * ey + cp * ez;
    const double rx = ca * v1x - sa * v1z;        // Ry(-(yaw+180))
    const double ry = v1y;
    const double rz = sa * v1x + ca * v1z;
    wx = cx + rx; wy = cy + ry; wz = cz + rz;
}

// НЕЙМТЕГИ: вешаем ник на уже пойманный (видимый) бокс сущности. Имя присваиваем ТОЧНО — разворачиваем eye-space
// сущности в мир (по пойманной камере) и матчим с ближайшим нашим игроком по МИРУ. Ничего не летает (позиция —
// из матрицы игры), тегируем только видимых, мобы/армор-стенды не совпадут ни с кем -> без тега.
// Last match stats (written by RenderPlayerOverlaysNative, read by UpdateGameState for coord self-heal).
static volatile int g_lastOnscr = 0, g_lastMatched = 0;
static ULONGLONG     g_coordHealAt = 0;
static volatile float g_scoreNormal = 0.0f, g_scoreSwap = 0.0f;
static volatile int   g_scoreN = 0;
static volatile bool      g_everMatched = false;
static volatile ULONGLONG g_lastOnscrAt = 0;
static volatile ULONGLONG g_lastMatchAt = 0;

#if 0  // Legacy ImGui nametags — disabled, replaced by native RenderPlayerOverlaysNative (below).
static void RenderPlayerOverlays(ImDrawList* dl) {
    const bool anyTag = g_config.nametags || g_config.maniacDetector || g_config.bowDetector;
    if (!dl || !anyTag || !g_radarReady) return;
    double lx = 0, ly = 0, lz = 0;
    const bool haveL = g_playerRadar.LocalEye(lx, ly, lz);
    static std::vector<mc::PlayerInfo> s_players;   // снапшот радара кэшируем ~33 мс (радар обновляется ~10 Гц) — не копируем каждый кадр
    static ULONGLONG s_snapAt = 0;
    const ULONGLONG snapNow = GetTickCount64();
    static std::vector<std::string> s_names;   // UTF-8 ники — кэш параллельно снапшоту (не конвертируем каждый кадр)
    if (snapNow - s_snapAt > 33) {
        s_players = g_playerRadar.Get(); s_snapAt = snapNow;
        s_names.resize(s_players.size());
        for (size_t i = 0; i < s_players.size(); ++i) s_names[i] = WideToUtf8(s_players[i].name);
    }
    const std::vector<mc::PlayerInfo>& players = s_players;
    int onscr = 0, matched = 0;
    std::vector<char> used(players.size(), 0);
    double sumN = 0, sumS = 0; int scoreN = 0;               // агрегаты для детекта свапа X/Z (для UpdateGameState)
    EnterCriticalSection(&g_entityLock);
    for (const CapturedEntity& e : g_entities) {
        if (!haveL) break;
        if (e.isChest) continue;
        if (e.screenX <= 0.0f || e.screenY <= 0.0f || e.screenBottomY <= 0.0f) continue;
        const double rx = e.relX, rz = e.relZ;
        if (rx * rx + rz * rz < 0.36) continue;              // rel≈0 -> это ТЫ (3-е лицо)
        ++onscr;
        int best = -1; double bestErr = 1.2, nearD = 1e9, swD = 1e9;   // ближайший (порог/без порога/со свапом X<->Z)
        for (size_t i = 0; i < players.size(); ++i) {
            const mc::PlayerInfo& p = players[i];
            if (p.isLocal || used[i] || p.name.empty() || p.name[0] == L'-') continue;
            const double dx = (p.x - lx) - rx, dz = (p.z - lz) - rz;
            const double d = sqrt(dx * dx + dz * dz);
            if (d < bestErr) { bestErr = d; best = (int)i; }
            if (d < nearD) nearD = d;
            const double sx = (p.z - lz) - rx, sz = (p.x - lx) - rz;   // со свапом X<->Z
            const double ds = sqrt(sx * sx + sz * sz);
            if (ds < swD) swD = ds;
        }
        if (nearD < 1e8) { sumN += nearD; sumS += swD; ++scoreN; }
        if (best < 0) continue;
        ++matched;
        used[best] = 1;
        const mc::PlayerInfo& p = players[best];

        const bool maniac   = g_config.maniacDetector && g_gameWatch.IsManiac(p.name);
        const bool bow      = g_config.bowDetector    && g_gameWatch.HasBow(p.name);
        const bool showNick = g_config.nametags || maniac;   // маньяк форсит красный ник
        if (!showNick && !bow) continue;                     // рисовать нечего

        const std::string& nm = s_names[best];               // из кэша (без per-frame WideToUtf8)
        const float       fh  = ImGui::GetFontSize();
        const ImVec2      nsz = showNick ? ImGui::CalcTextSize(nm.c_str()) : ImVec2(0.0f, fh);
        const float       ih  = fh;                          // сторона иконки = высота шрифта
        const float       gap = 5.0f;

        float totalW = nsz.x; bool prev = showNick;          // ширина строки метки
        if (maniac) { if (prev) totalW += gap; totalW += ih; prev = true; }
        if (bow)    { if (prev) totalW += gap; totalW += ih; prev = true; }
        const float x = e.screenX - totalW * 0.5f;
        const float y = e.screenY - nsz.y - 6.0f;            // над боксом

        const float padX = 5.0f, padY = 2.0f;
        const ImU32 bg = IM_COL32(18, 14, 24, 225);
        const ImU32 bd = maniac ? IM_COL32(239, 68, 68, 235)
                       : bow    ? IM_COL32(80, 140, 240, 215)
                                : IM_COL32(145, 95, 215, 170);
        dl->AddRectFilled(ImVec2(x - padX, y - padY), ImVec2(x + totalW + padX, y + nsz.y + padY), bg, 4.0f);
        dl->AddRect      (ImVec2(x - padX, y - padY), ImVec2(x + totalW + padX, y + nsz.y + padY), bd, 4.0f, 0, 1.0f);

        float cx = x; bool prevP = false;
        if (showNick) {
            const ImU32 col = maniac ? IM_COL32(255, 82, 82, 255) : IM_COL32(236, 224, 255, 255);
            dl->AddText(ImVec2(cx, y), col, nm.c_str());
            cx += nsz.x; prevP = true;
        }
        if (maniac) { if (prevP) cx += gap; DrawSwordIcon(dl, ImVec2(cx + ih * 0.5f, y + nsz.y * 0.5f), ih * 0.86f, IM_COL32(255, 72, 72, 255)); cx += ih; prevP = true; }
        if (bow)    { if (prevP) cx += gap; DrawBowIcon  (dl, ImVec2(cx + ih * 0.5f, y + nsz.y * 0.5f), ih * 0.86f, IM_COL32(96, 165, 250, 255)); cx += ih; prevP = true; }
    }
    LeaveCriticalSection(&g_entityLock);
    g_lastOnscr = onscr; g_lastMatched = matched;
    g_scoreNormal = (float)sumN; g_scoreSwap = (float)sumS; g_scoreN = scoreN;   // для детекта свапа X/Z (UpdateGameState)
}

#endif // ЛЕГАСИ ImGui RenderPlayerOverlays

// ── Native nametags (no ImGui frame): MC font + light dark backdrop in vanilla MC style. ──
// Entity<->player matching uses horizontal distance (relX/relZ ~ p.x-lx / p.z-lz).
static void RenderPlayerOverlaysNative() {
    if (!g_config.nametags || !g_radarReady || !nov::Ready()) return;
    double lx = 0, ly = 0, lz = 0;
    const bool haveL = g_playerRadar.LocalEye(lx, ly, lz);
    static std::vector<mc::PlayerInfo> s_players; static ULONGLONG s_snapAt = 0;
    static std::vector<std::string> s_names;
    const ULONGLONG snapNow = GetTickCount64();
    if (snapNow - s_snapAt > 33) {
        s_players = g_playerRadar.Get(); s_snapAt = snapNow;
        s_names.resize(s_players.size());
        for (size_t i = 0; i < s_players.size(); ++i) s_names[i] = WideToUtf8(s_players[i].name);
    }
    const std::vector<mc::PlayerInfo>& players = s_players;
    int onscr = 0, matched = 0;
    std::vector<char> used(players.size(), 0);
    double sumN = 0, sumS = 0; int scoreN = 0;
    // Размер неймтега: round to integer (7..24 px) чтобы попасть в предпечённый размер атласа.
    float fpxReq = nov::PX_TAG * g_config.nametagScale;
    if (fpxReq < 7.0f) fpxReq = 7.0f; else if (fpxReq > 24.0f) fpxReq = 24.0f;
    const float fpx = (float)(int)(fpxReq + 0.5f);
    EnterCriticalSection(&g_entityLock);
    for (const CapturedEntity& e : g_entities) {
        if (!haveL) break;
        if (e.isChest) continue;
        if (e.screenX <= 0.0f || e.screenY <= 0.0f || e.screenBottomY <= 0.0f) continue;
        const double rx = e.relX, rz = e.relZ;
        if (rx * rx + rz * rz < 0.36) continue;
        ++onscr;
        int best = -1; double bestErr = 1.2, nearD = 1e9, swD = 1e9;
        for (size_t i = 0; i < players.size(); ++i) {
            const mc::PlayerInfo& p = players[i];
            if (p.isLocal || used[i] || p.name.empty() || p.name[0] == L'-') continue;
            const double dx = (p.x - lx) - rx, dz = (p.z - lz) - rz; const double d = sqrt(dx * dx + dz * dz);
            if (d < bestErr) { bestErr = d; best = (int)i; }
            if (d < nearD) nearD = d;
            const double sx = (p.z - lz) - rx, sz = (p.x - lx) - rz; const double ds = sqrt(sx * sx + sz * sz);
            if (ds < swD) swD = ds;
        }
        if (nearD < 1e8) { sumN += nearD; sumS += swD; ++scoreN; }
        if (best < 0) continue;
        ++matched; used[best] = 1;

        const std::string& nm = s_names[best];
        const float nsw = nov::TextW(nm.c_str(), fpx);
        const float x = e.screenX - nsw * 0.5f;
        const float y = e.screenY - fpx - 6.0f + g_config.nametagOffsetY;
        const float padX = 3.0f, padY = 1.0f;
        nov::FillRect(x - padX, y - padY, x + nsw + padX, y + fpx + padY, IM_COL32(0, 0, 0, 130));
        nov::Text(x, y, nm.c_str(), IM_COL32(255, 255, 255, 255), fpx, true);
    }
    LeaveCriticalSection(&g_entityLock);
    g_lastOnscr = onscr; g_lastMatched = matched;
    g_scoreNormal = (float)sumN; g_scoreSwap = (float)sumS; g_scoreN = scoreN;
    const ULONGLONG tnow = GetTickCount64();                       // сигналы для монитора здоровья
    if (onscr > 0)   g_lastOnscrAt = tnow;
    if (matched > 0) { g_everMatched = true; g_lastMatchAt = tnow; }
}

// ── Coordinate self-heal: detects swapped X/Z from render↔radar disagreement, triggered only before
//    first successful match. Runs in render thread at ~8 Hz. ──
static ULONGLONG g_lastWatch    = 0;

static void UpdateGameState() {
    if (!g_radarReady) return;
    const ULONGLONG now = GetTickCount64();
    if (now - g_lastWatch < 120) return;
    g_lastWatch = now;

    if (g_scoreN >= 3 && now - g_coordHealAt > 1000 && !g_everMatched) {
        const float avgN = g_scoreNormal / g_scoreN, avgS = g_scoreSwap / g_scoreN;
        if (avgN > 1.5f && avgS < 1.0f && avgS < avgN * 0.4f) {
            g_coordHealAt = now;
            int px = 0, py = 0, pz = 0; g_playerRadar.Offsets(px, py, pz);
            g_playerRadar.SetCoords(pz, py, px);             // swap X<->Z
            g_radarLayout.entPosX = pz; g_radarLayout.entPosZ = px;
            Log("RADAR: X/Z offsets swap-corrected (avgN=%.2f avgS=%.2f)", avgN, avgS);
        }
    }
}

// ══════════════════════════════════════════════════════════════════════════════
//  MENU UI
// ══════════════════════════════════════════════════════════════════════════════

static int g_selectedTab = 0; // 0 = Visuals, 1 = Combat, 2 = System

// ── Vector Icons (Anti-Aliased & Crisp) ───────────────────────────────────────
static void DrawIconEye(ImDrawList* dl, ImVec2 center, float size, ImU32 color) {
    float rx = size * 0.46f;
    float ry = size * 0.25f;
    dl->AddEllipse(center, ImVec2(rx, ry), color, 0.0f, 24, 1.7f);
    dl->AddCircleFilled(center, size * 0.16f, color);
    dl->AddCircleFilled(ImVec2(center.x + size * 0.05f, center.y - size * 0.05f), size * 0.05f, IM_COL32(255, 255, 255, 230));
}

static void DrawIconCrosshair(ImDrawList* dl, ImVec2 center, float size, ImU32 color) {
    float r = size * 0.32f;
    dl->AddCircle(center, r, color, 20, 1.7f);
    dl->AddCircleFilled(center, 2.0f, color);
    dl->AddLine(ImVec2(center.x, center.y - r - 3.5f), ImVec2(center.x, center.y - r + 2.0f), color, 1.7f);
    dl->AddLine(ImVec2(center.x, center.y + r - 2.0f), ImVec2(center.x, center.y + r + 3.5f), color, 1.7f);
    dl->AddLine(ImVec2(center.x - r - 3.5f, center.y), ImVec2(center.x - r + 2.0f, center.y), color, 1.7f);
    dl->AddLine(ImVec2(center.x + r - 2.0f, center.y), ImVec2(center.x + r + 3.5f, center.y), color, 1.7f);
}

static void DrawIconWorld(ImDrawList* dl, ImVec2 center, float size, ImU32 color) {
    float r = size * 0.38f;
    dl->AddCircle(center, r, color, 20, 1.7f);
    dl->AddLine(ImVec2(center.x - r, center.y), ImVec2(center.x + r, center.y), color, 1.3f);
    dl->AddLine(ImVec2(center.x, center.y - r), ImVec2(center.x, center.y + r), color, 1.3f);
    dl->AddEllipse(center, ImVec2(r * 0.45f, r), color, 0.0f, 16, 1.3f);
}

static void DrawIconSliders(ImDrawList* dl, ImVec2 center, float size, ImU32 color) {
    float hw = size * 0.40f;
    float y1 = center.y - size * 0.26f;
    float y2 = center.y;
    float y3 = center.y + size * 0.26f;
    dl->AddLine(ImVec2(center.x - hw, y1), ImVec2(center.x + hw, y1), color, 1.6f);
    dl->AddLine(ImVec2(center.x - hw, y2), ImVec2(center.x + hw, y2), color, 1.6f);
    dl->AddLine(ImVec2(center.x - hw, y3), ImVec2(center.x + hw, y3), color, 1.6f);
    dl->AddCircleFilled(ImVec2(center.x - hw * 0.35f, y1), 2.8f, color);
    dl->AddCircleFilled(ImVec2(center.x + hw * 0.35f, y2), 2.8f, color);
    dl->AddCircleFilled(ImVec2(center.x - hw * 0.10f, y3), 2.8f, color);
}

// ── Neverlose Style Keybind Pill ──────────────────────────────────────────────
static bool KeybindPill(const char* id, int& key, int bindId, const ImVec2& size = ImVec2(52, 19)) {
    char btnText[32];
    bool isWaiting = (g_waitingBindId == bindId);
    if (isWaiting) {
        snprintf(btnText, sizeof(btnText), "...");
    } else {
        const char* kn = GetKeyName(key);
        snprintf(btnText, sizeof(btnText), "%s", kn);
    }

    ImGui::PushID(id);
    if (isWaiting) {
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.55f, 0.25f, 0.90f, 0.95f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.65f, 0.35f, 1.00f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.75f, 0.45f, 1.00f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_Border,        ImVec4(0.85f, 0.55f, 1.00f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(1.00f, 1.00f, 1.00f, 1.00f));
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.10f, 0.11f, 0.17f, 0.90f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.24f, 0.18f, 0.36f, 0.90f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.40f, 0.22f, 0.60f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_Border,        ImVec4(0.22f, 0.20f, 0.34f, 0.80f));
        ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(0.75f, 0.72f, 0.85f, 0.90f));
    }
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.0f, 0.0f));

    if (g_fontSmall) ImGui::PushFont(g_fontSmall);
    bool clicked = ImGui::Button(btnText, size);
    if (g_fontSmall) ImGui::PopFont();

    if (clicked) {
        if (g_waitingBindId == bindId) g_waitingBindId = -1;
        else g_waitingBindId = bindId;
    }

    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(5);
    ImGui::PopID();

    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Click to set hotkey.\nESC, DELETE, or BACKSPACE to clear.");
    }
    return clicked;
}

// ── Neverlose Style Feature Row (Label + Keybind + Capsule Toggle Switch) ────
static bool FeatureRow(const char* label, bool* v, int* key = nullptr, int bindId = -1, const char* tooltip = nullptr) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems) return false;

    ImGuiContext& g = *GImGui;
    const ImGuiID id = window->GetID(label);

    float rowH = 28.0f;
    float availW = ImGui::GetContentRegionAvail().x;
    ImVec2 pos = window->DC.CursorPos;

    float switchW = 34.0f, switchH = 18.0f, radius = switchH * 0.5f;
    float keybindW = (key != nullptr && bindId != -1) ? 54.0f : 0.0f;
    float switchX = pos.x + availW - switchW - 2.0f;
    float keybindX = switchX - keybindW - 6.0f;

    ImRect row_bb(pos, ImVec2(pos.x + availW, pos.y + rowH));
    ImGui::ItemSize(row_bb, 0.0f);
    if (!ImGui::ItemAdd(row_bb, id)) return false;

    ImRect key_bb(ImVec2(keybindX, pos.y + (rowH - 19.0f) * 0.5f), ImVec2(keybindX + keybindW, pos.y + (rowH + 19.0f) * 0.5f));

    bool keyHovered = false;
    if (key != nullptr && bindId != -1) {
        keyHovered = ImGui::IsMouseHoveringRect(key_bb.Min, key_bb.Max);
        if (keyHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            if (g_waitingBindId == bindId) g_waitingBindId = -1;
            else g_waitingBindId = bindId;
        }
    }

    bool hovered, held;
    bool pressed = ImGui::ButtonBehavior(row_bb, id, &hovered, &held);

    if (pressed && !keyHovered) {
        *v = !(*v);
        ImGui::MarkItemEdited(id);
    }

    ImDrawList* dl = window->DrawList;

    if (hovered && !keyHovered) {
        dl->AddRectFilled(row_bb.Min, row_bb.Max, IM_COL32(255, 255, 255, 8), 4.0f);
    }

    ImU32 textColor = *v ? IM_COL32(245, 245, 250, 255) : (hovered ? IM_COL32(195, 200, 215, 255) : IM_COL32(145, 150, 165, 255));
    dl->AddText(ImVec2(pos.x + 4.0f, pos.y + (rowH - ImGui::GetFontSize()) * 0.5f), textColor, label);

    if (key != nullptr && bindId != -1) {
        bool isWaiting = (g_waitingBindId == bindId);
        char btnText[32];
        if (isWaiting) snprintf(btnText, sizeof(btnText), "...");
        else snprintf(btnText, sizeof(btnText), "%s", GetKeyName(*key));

        ImU32 bgCol = isWaiting ? IM_COL32(147, 51, 234, 240) : (keyHovered ? IM_COL32(50, 40, 75, 230) : IM_COL32(25, 22, 38, 230));
        ImU32 borderCol = isWaiting ? IM_COL32(192, 132, 252, 255) : (keyHovered ? IM_COL32(100, 80, 140, 200) : IM_COL32(55, 48, 80, 180));
        ImU32 textCol = isWaiting ? IM_COL32(255, 255, 255, 255) : (keyHovered ? IM_COL32(230, 220, 250, 255) : IM_COL32(170, 160, 190, 255));

        dl->AddRectFilled(key_bb.Min, key_bb.Max, bgCol, 4.0f);
        dl->AddRect(key_bb.Min, key_bb.Max, borderCol, 4.0f, 0, 1.0f);

        ImVec2 txtSz = ImGui::CalcTextSize(btnText);
        dl->AddText(ImVec2(key_bb.Min.x + (keybindW - txtSz.x) * 0.5f, key_bb.Min.y + (19.0f - txtSz.y) * 0.5f), textCol, btnText);
    }

    float swY = pos.y + (rowH - switchH) * 0.5f;
    ImVec2 swMin(switchX, swY);
    ImVec2 swMax(switchX + switchW, swY + switchH);

    if (*v) {
        dl->AddRectFilled(swMin, swMax, IM_COL32(147, 51, 234, 255), radius);
        dl->AddRect(swMin, swMax, IM_COL32(192, 132, 252, 220), radius, 0, 1.0f);

        float knobX = swMax.x - radius;
        float knobY = swY + radius;
        dl->AddCircleFilled(ImVec2(knobX, knobY), radius - 2.5f, IM_COL32(255, 255, 255, 255));
    } else {
        dl->AddRectFilled(swMin, swMax, (hovered && !keyHovered) ? IM_COL32(35, 30, 52, 255) : IM_COL32(22, 20, 34, 255), radius);
        dl->AddRect(swMin, swMax, IM_COL32(48, 42, 70, 200), radius, 0, 1.0f);

        float knobX = swMin.x + radius;
        float knobY = swY + radius;
        dl->AddCircleFilled(ImVec2(knobX, knobY), radius - 2.5f, IM_COL32(110, 105, 135, 255));
    }

    if (tooltip && hovered && !keyHovered) {
        ImGui::SetTooltip("%s", tooltip);
    }

    return pressed;
}

// ── Neverlose Style Card Container ────────────────────────────────────────────
static void BeginCard(const char* id, const char* title, const ImVec2& size = ImVec2(0, 0)) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.06f, 0.07f, 0.11f, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_Border,  ImVec4(0.18f, 0.16f, 0.28f, 0.70f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 9.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 12.0f));

    ImGui::BeginChild(id, size, true, ImGuiWindowFlags_None);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 cp = ImGui::GetCursorScreenPos();
    float aw = ImGui::GetContentRegionAvail().x;

    // Glowing neon violet pill indicator next to title
    dl->AddRectFilled(ImVec2(cp.x, cp.y + 3.0f), ImVec2(cp.x + 3.0f, cp.y + 14.0f), IM_COL32(168, 85, 247, 255), 1.5f);

    // Card Title
    if (g_fontBold) ImGui::PushFont(g_fontBold);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 8.0f);
    ImGui::TextColored(ImVec4(0.92f, 0.85f, 1.00f, 1.0f), "%s", title);
    if (g_fontBold) ImGui::PopFont();

    ImGui::Spacing();
    cp = ImGui::GetCursorScreenPos();
    dl->AddLine(ImVec2(cp.x, cp.y), ImVec2(cp.x + aw, cp.y), IM_COL32(35, 30, 52, 180), 1.0f);

    ImGui::Spacing();
    ImGui::Spacing();
}

static void EndCard() {
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}

// ── Modern Minimalist Custom Sliders ──────────────────────────────────────────
static bool ModernSliderFloat(const char* label, float* v, float v_min, float v_max, const char* format) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems) return false;

    ImGuiContext& g = *GImGui;
    const ImGuiID id = window->GetID(label);

    float availW = ImGui::GetContentRegionAvail().x;
    float rowH = 38.0f;
    ImVec2 pos = window->DC.CursorPos;

    ImRect total_bb(pos, ImVec2(pos.x + availW, pos.y + rowH));
    ImGui::ItemSize(total_bb, 0.0f);
    if (!ImGui::ItemAdd(total_bb, id)) return false;

    char valBuf[32];
    snprintf(valBuf, sizeof(valBuf), format, *v);
    ImVec2 valSz = ImGui::CalcTextSize(valBuf);

    ImDrawList* dl = window->DrawList;
    dl->AddText(ImVec2(pos.x + 2.0f, pos.y + 1.0f), IM_COL32(205, 210, 225, 255), label);
    dl->AddText(ImVec2(pos.x + availW - valSz.x - 2.0f, pos.y + 1.0f), IM_COL32(192, 132, 252, 255), valBuf);

    float trackY = pos.y + 24.0f;
    float trackH = 4.0f;
    float trackPad = 2.0f;
    ImRect track_bb(ImVec2(pos.x + trackPad, trackY), ImVec2(pos.x + availW - trackPad, trackY + trackH));

    ImRect grab_bb(ImVec2(track_bb.Min.x - 6.0f, track_bb.Min.y - 12.0f), ImVec2(track_bb.Max.x + 6.0f, track_bb.Max.y + 12.0f));
    bool hovered, held;
    ImGui::ButtonBehavior(grab_bb, id, &hovered, &held);

    bool changed = false;
    if (held) {
        float mouseX = g.IO.MousePos.x;
        float ratio = ImClamp((mouseX - track_bb.Min.x) / (track_bb.Max.x - track_bb.Min.x), 0.0f, 1.0f);
        float newVal = v_min + ratio * (v_max - v_min);
        if (*v != newVal) {
            *v = newVal;
            changed = true;
            ImGui::MarkItemEdited(id);
        }
    }

    dl->AddRectFilled(track_bb.Min, track_bb.Max, IM_COL32(24, 28, 42, 255), 2.0f);

    float currentRatio = ImClamp((*v - v_min) / (v_max - v_min), 0.0f, 1.0f);
    float fillX = track_bb.Min.x + currentRatio * (track_bb.Max.x - track_bb.Min.x);
    if (fillX > track_bb.Min.x) {
        dl->AddRectFilled(track_bb.Min, ImVec2(fillX, track_bb.Max.y), IM_COL32(168, 85, 247, 255), 2.0f);
    }

    float knobRadius = (hovered || held) ? 6.5f : 5.0f;
    ImVec2 knobCenter(fillX, trackY + trackH * 0.5f);
    dl->AddCircleFilled(knobCenter, knobRadius + 1.5f, IM_COL32(168, 85, 247, (hovered || held) ? 140 : 50));
    dl->AddCircleFilled(knobCenter, knobRadius, IM_COL32(255, 255, 255, 255));
    dl->AddCircle(knobCenter, knobRadius, IM_COL32(168, 85, 247, 255), 16, 1.2f);

    return changed;
}

static bool ModernSliderInt(const char* label, int* v, int v_min, int v_max, const char* format) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems) return false;

    ImGuiContext& g = *GImGui;
    const ImGuiID id = window->GetID(label);

    float availW = ImGui::GetContentRegionAvail().x;
    float rowH = 38.0f;
    ImVec2 pos = window->DC.CursorPos;

    ImRect total_bb(pos, ImVec2(pos.x + availW, pos.y + rowH));
    ImGui::ItemSize(total_bb, 0.0f);
    if (!ImGui::ItemAdd(total_bb, id)) return false;

    char valBuf[32];
    snprintf(valBuf, sizeof(valBuf), format, *v);
    ImVec2 valSz = ImGui::CalcTextSize(valBuf);

    ImDrawList* dl = window->DrawList;
    dl->AddText(ImVec2(pos.x + 2.0f, pos.y + 1.0f), IM_COL32(205, 210, 225, 255), label);
    dl->AddText(ImVec2(pos.x + availW - valSz.x - 2.0f, pos.y + 1.0f), IM_COL32(192, 132, 252, 255), valBuf);

    float trackY = pos.y + 24.0f;
    float trackH = 4.0f;
    float trackPad = 2.0f;
    ImRect track_bb(ImVec2(pos.x + trackPad, trackY), ImVec2(pos.x + availW - trackPad, trackY + trackH));

    ImRect grab_bb(ImVec2(track_bb.Min.x - 6.0f, track_bb.Min.y - 12.0f), ImVec2(track_bb.Max.x + 6.0f, track_bb.Max.y + 12.0f));
    bool hovered, held;
    ImGui::ButtonBehavior(grab_bb, id, &hovered, &held);

    bool changed = false;
    if (held) {
        float mouseX = g.IO.MousePos.x;
        float ratio = ImClamp((mouseX - track_bb.Min.x) / (track_bb.Max.x - track_bb.Min.x), 0.0f, 1.0f);
        int newVal = v_min + (int)roundf(ratio * (float)(v_max - v_min));
        if (newVal < v_min) newVal = v_min;
        if (newVal > v_max) newVal = v_max;
        if (*v != newVal) {
            *v = newVal;
            changed = true;
            ImGui::MarkItemEdited(id);
        }
    }

    dl->AddRectFilled(track_bb.Min, track_bb.Max, IM_COL32(24, 28, 42, 255), 2.0f);

    float currentRatio = ImClamp((float)(*v - v_min) / (float)(v_max - v_min), 0.0f, 1.0f);
    float fillX = track_bb.Min.x + currentRatio * (track_bb.Max.x - track_bb.Min.x);
    if (fillX > track_bb.Min.x) {
        dl->AddRectFilled(track_bb.Min, ImVec2(fillX, track_bb.Max.y), IM_COL32(168, 85, 247, 255), 2.0f);
    }

    float knobRadius = (hovered || held) ? 6.5f : 5.0f;
    ImVec2 knobCenter(fillX, trackY + trackH * 0.5f);
    dl->AddCircleFilled(knobCenter, knobRadius + 1.5f, IM_COL32(168, 85, 247, (hovered || held) ? 140 : 50));
    dl->AddCircleFilled(knobCenter, knobRadius, IM_COL32(255, 255, 255, 255));
    dl->AddCircle(knobCenter, knobRadius, IM_COL32(168, 85, 247, 255), 16, 1.2f);

    return changed;
}

// ── RenderMenu Main ───────────────────────────────────────────────────────────
static void RenderMenu() {
    ImGui::SetNextWindowSize(ImVec2(760, 510), ImGuiCond_Always);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

    if (ImGui::Begin("MLE_ESP_Root", &g_menuOpen, flags)) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 p = ImGui::GetWindowPos();
        ImVec2 sz = ImGui::GetWindowSize();

        // Outer window background & subtle violet border
        dl->AddRectFilled(p, ImVec2(p.x + sz.x, p.y + sz.y), IM_COL32(11, 13, 20, 252), 10.0f);
        dl->AddRect(p, ImVec2(p.x + sz.x, p.y + sz.y), IM_COL32(65, 45, 95, 140), 10.0f, 0, 1.2f);

        // ── TOP HEADER BAR (Height: 48px) ────────────────────────────────────
        dl->AddRectFilled(p, ImVec2(p.x + sz.x, p.y + 48.0f), IM_COL32(15, 12, 23, 255), 10.0f, ImDrawFlags_RoundCornersTop);
        dl->AddLine(ImVec2(p.x, p.y + 48.0f), ImVec2(p.x + sz.x, p.y + 48.0f), IM_COL32(42, 32, 62, 180), 1.0f);

        // Header Brand Diamond Logo
        ImVec2 logoCenter(p.x + 24.0f, p.y + 24.0f);
        float diamondR = 7.0f;
        ImVec2 diamondPts[4] = {
            ImVec2(logoCenter.x, logoCenter.y - diamondR),
            ImVec2(logoCenter.x + diamondR, logoCenter.y),
            ImVec2(logoCenter.x, logoCenter.y + diamondR),
            ImVec2(logoCenter.x - diamondR, logoCenter.y)
        };
        dl->AddConvexPolyFilled(diamondPts, 4, IM_COL32(168, 85, 247, 255));
        dl->AddPolyline(diamondPts, 4, IM_COL32(230, 195, 255, 255), ImDrawFlags_Closed, 1.2f);

        // Brand Title
        if (g_fontTitle) ImGui::PushFont(g_fontTitle);
        dl->AddText(ImVec2(p.x + 40.0f, p.y + 13.0f), IM_COL32(255, 255, 255, 255), "Legacy ESP");
        if (g_fontTitle) ImGui::PopFont();

        // (branding trimmed: only the title lockup — no version badge, no breadcrumb)

        // Close button (X)
        ImGui::SetCursorPos(ImVec2(sz.x - 38.0f, 11.0f));
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.45f, 0.16f, 0.30f, 0.80f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.60f, 0.20f, 0.36f, 0.95f));
        ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(0.85f, 0.80f, 0.95f, 1.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.0f);
        if (ImGui::Button("X", ImVec2(26, 26))) {
            g_menuOpen = false;
        }
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(4);

        // ── BODY: SIDEBAR + CONTENT ──────────────────────────────────────────
        ImGui::SetCursorPos(ImVec2(0.0f, 48.0f));

        // 1. Sidebar (Width: 175px)
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.08f, 0.06f, 0.12f, 0.98f));
        ImGui::BeginChild("Sidebar", ImVec2(175.0f, sz.y - 48.0f), false, ImGuiWindowFlags_NoScrollbar);

        ImGui::Spacing();
        ImGui::Spacing();

        auto NavTab = [](const char* label, int tabIdx, int iconType) {
            bool selected = (g_selectedTab == tabIdx);
            ImVec2 cp = ImGui::GetCursorScreenPos();
            float w = 155.0f, h = 42.0f;
            ImDrawList* cdl = ImGui::GetWindowDrawList();

            ImGui::SetCursorPosX(10.0f);
            ImGui::PushID(tabIdx);

            ImRect bb(cp, ImVec2(cp.x + w, cp.y + h));
            ImGuiID id = ImGui::GetID(label);
            ImGui::ItemSize(bb, 0.0f);
            if (!ImGui::ItemAdd(bb, id)) {
                ImGui::PopID();
                return;
            }

            bool hovered, held;
            bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);
            if (pressed) {
                g_selectedTab = tabIdx;
            }

            if (selected) {
                cdl->AddRectFilled(bb.Min, bb.Max, IM_COL32(32, 23, 49, 230), 7.0f);
                cdl->AddRect(bb.Min, bb.Max, IM_COL32(130, 75, 200, 160), 7.0f, 0, 1.0f);
                cdl->AddRectFilled(bb.Min, ImVec2(bb.Min.x + 3.5f, bb.Max.y), IM_COL32(192, 132, 252, 255), 7.0f, ImDrawFlags_RoundCornersLeft);
            } else if (hovered) {
                cdl->AddRectFilled(bb.Min, bb.Max, IM_COL32(22, 17, 32, 180), 7.0f);
            }

            ImU32 iconColor = selected ? IM_COL32(225, 180, 255, 255) : (hovered ? IM_COL32(190, 180, 215, 255) : IM_COL32(125, 115, 145, 255));
            ImVec2 iconCenter(bb.Min.x + 22.0f, bb.Min.y + h * 0.5f);
            if (iconType == 0) DrawIconEye(cdl, iconCenter, 18.0f, iconColor);
            else if (iconType == 1) DrawIconCrosshair(cdl, iconCenter, 18.0f, iconColor);
            else if (iconType == 2) DrawIconWorld(cdl, iconCenter, 18.0f, iconColor);
            else if (iconType == 3) DrawIconSliders(cdl, iconCenter, 18.0f, iconColor);

            ImU32 textColor = selected ? IM_COL32(255, 255, 255, 255) : (hovered ? IM_COL32(220, 215, 235, 255) : IM_COL32(150, 140, 170, 255));
            if (selected && g_fontBold) ImGui::PushFont(g_fontBold);
            ImVec2 txtSz = ImGui::CalcTextSize(label);
            cdl->AddText(ImVec2(bb.Min.x + 44.0f, bb.Min.y + (h - txtSz.y) * 0.5f), textColor, label);
            if (selected && g_fontBold) ImGui::PopFont();

            ImGui::PopID();
            ImGui::Spacing();
        };

        NavTab("Visuals", 0, 0);   // eye
        NavTab("Combat",  1, 1);   // crosshair
        NavTab("System",  2, 3);   // sliders

        ImGui::EndChild();
        ImGui::PopStyleColor();

        // Divider between Sidebar and Content Area
        dl->AddLine(ImVec2(p.x + 175.0f, p.y + 48.0f), ImVec2(p.x + 175.0f, p.y + sz.y), IM_COL32(38, 28, 56, 180), 1.0f);

        // 2. Content Area (Width: sz.x - 175.0f = 585px)
        ImGui::SetCursorPos(ImVec2(187.0f, 58.0f));
        ImGui::BeginChild("ContentArea", ImVec2(sz.x - 199.0f, sz.y - 70.0f), false, ImGuiWindowFlags_None);

        float colW = 270.0f;

        if (g_selectedTab == 0) {
            // ── TAB: VISUALS — ESP (left) + PLAYERS/WORLD (right) ──────────
            BeginCard("card_esp", "ESP", ImVec2(colW, 0));
            FeatureRow("Wallhack", &g_config.wallhack, &g_config.kWallhack.key, 1, "See player models through terrain (depth bias)");
            FeatureRow("2D Box",   &g_config.esp2d,    &g_config.kEsp2d.key,    9, "Screen-space rectangle around each player");
            if (g_config.esp2d) {
                ImGui::PushItemWidth(-1.0f);
                ImGui::ColorEdit3("##esp2dCol", g_config.esp2dColor, ImGuiColorEditFlags_NoInputs);
                ImGui::PopItemWidth();
            }
            FeatureRow("Tracers",  &g_config.tracers,  &g_config.kTracers.key,  3, "Line from a screen anchor to each player");
            if (g_config.tracers) {
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.70f, 0.65f, 0.78f, 1.0f), "Tracer origin");
                const char* tracerOrigins[] = { "Screen Top", "Crosshair", "Screen Bottom" };
                ImGui::SetNextItemWidth(-1);
                ImGui::Combo("##trOrigin", &g_config.tracersOrigin, tracerOrigins, 3);
                ImGui::Spacing();
            }
            FeatureRow("Nametags", &g_config.nametags, nullptr, -1, "Real player nicknames above boxes (native render)");
            if (g_config.nametags) {
                ModernSliderFloat("Y offset",  &g_config.nametagOffsetY, -40.0f, 40.0f, "%.0f px");
                ModernSliderFloat("Scale",     &g_config.nametagScale,    0.6f,  2.0f, "%.2fx");
                // Ручной re-discovery радара (на случай если авто-переоткрытие не сработало — напр. 0/N матч).
                // Таймаут 10с: если discovery завис в Reloading, можно повторно нажать (авто-retry).
                static ULONGLONG s_lastReloadAt = 0;
                const ULONGLONG rNow = GetTickCount64();
                const bool stuck = !g_radarReady && s_lastReloadAt && (rNow - s_lastReloadAt > 10000);
                const bool clickable = g_radarReady || stuck;
                const char* lbl = g_radarReady ? "Reload radar" : stuck ? "Retry (stuck)" : "Reloading...";
                if (ImGui::Button(lbl, ImVec2(-1, 0)) && clickable) {
                    Log("RADAR: manual reload requested from UI%s", stuck ? " (previous stuck)" : "");
                    g_radarReady = false;
                    s_lastReloadAt = rNow;
                }
            }
            FeatureRow("Distance", &g_config.showDistance, nullptr, -1, "Distance in blocks under each nametag");
            if (g_config.showDistance) {
                ModernSliderFloat("Y offset##d", &g_config.distanceOffsetY, -40.0f, 40.0f, "%.0f px");
                ModernSliderFloat("Scale##d",    &g_config.distanceScale,    0.6f,  2.0f, "%.2fx");
            }
            FeatureRow("Glow",     &g_config.outline, nullptr, -1, "Soft glowing outline around the player model (always through walls)");
            if (g_config.outline) {
                ImGui::PushItemWidth(-1.0f);
                ImGui::ColorEdit3("##outlineCol", g_config.outlineColor, ImGuiColorEditFlags_NoInputs);
                ImGui::SliderFloat("##outlineSpread", &g_config.outlineSpread, 0.5f, 5.0f, "width %.2f");
                ImGui::PopItemWidth();
            }
            EndCard();

        }
        else if (g_selectedTab == 1) {
            // ── TAB: COMBAT — Auto Clicker + Aimbot ───────────────────────────
            BeginCard("card_clicker", "AUTO CLICKER", ImVec2(colW, 0));
            FeatureRow("Auto Clicker", &g_config.autoclicker, &g_config.kAutoclicker.key, 6, "Auto left-click while the bind (or LMB) is held");
            ImGui::Spacing();
            ModernSliderInt("Clicks / sec", &g_config.cps, 6, 22, "%d CPS");
            ImGui::Spacing();
            ImGui::Spacing();
            ImGui::TextDisabled("Hold the bind key (or LMB with [None])");
            ImGui::TextDisabled("in the game window to click.");
            EndCard();

            ImGui::SameLine(0, 14.0f);

            BeginCard("card_aimbot", "AIMBOT", ImVec2(colW, 0));
            FeatureRow("Aimbot", &g_config.aimbot, &g_config.kAimbot.key, 5, "Aim at the nearest player's chest while the bind (default RMB) is held");
            ImGui::Spacing();
            ModernSliderFloat("Strength", &g_config.aimSmooth, 0.05f, 1.0f, "%.2f");
            ImGui::Spacing();
            ModernSliderFloat("FOV", &g_config.aimFov, 30.0f, 500.0f, "%.0f px");
            EndCard();
        }
        else if (g_selectedTab == 2) {
            // ── TAB: SYSTEM — Status · Controls ────────────────────────────────
            // Left column: live status
            ImGui::BeginGroup();
            BeginCard("card_status", "STATUS", ImVec2(colW, 250.0f));
            ImGui::TextColored(ImVec4(0.70f, 0.65f, 0.78f, 1.0f), "Resolution:");
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - 70.0f);
            ImGui::TextColored(ImVec4(0.85f, 0.65f, 1.0f, 1.0f), "%dx%d", g_screenWidth, g_screenHeight);

            ImGui::TextColored(ImVec4(0.70f, 0.65f, 0.78f, 1.0f), "Framerate:");
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - 60.0f);
            ImGui::TextColored(ImVec4(0.85f, 0.65f, 1.0f, 1.0f), "%.0f FPS", ImGui::GetIO().Framerate);

            EnterCriticalSection(&g_entityLock);
            size_t entCount = g_entities.size();
            LeaveCriticalSection(&g_entityLock);
            ImGui::TextColored(ImVec4(0.70f, 0.65f, 0.78f, 1.0f), "Entities:");
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - 40.0f);
            ImGui::TextColored(ImVec4(0.85f, 0.65f, 1.0f, 1.0f), "%zu", entCount);

            ImGui::Spacing();
            const mc::KlassCodec& cc = mc::Codec();
            const char* cli = !cc.detected ? "detecting..." : (cc.base == 0 ? "VimeWorld" : "Lunar / Java 9+");
            ImGui::TextColored(ImVec4(0.70f, 0.65f, 0.78f, 1.0f), "Client:");
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(cli).x);
            ImGui::TextColored(g_radarReady ? ImVec4(0.55f, 0.9f, 0.55f, 1.0f) : ImVec4(0.9f, 0.8f, 0.4f, 1.0f), "%s", cli);

            const char* rst = g_radarReady ? "ready" : "scanning...";
            ImGui::TextColored(ImVec4(0.70f, 0.65f, 0.78f, 1.0f), "Radar:");
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(rst).x);
            ImGui::TextColored(g_radarReady ? ImVec4(0.55f, 0.9f, 0.55f, 1.0f) : ImVec4(0.9f, 0.8f, 0.4f, 1.0f), "%s", rst);

            char tg[32]; snprintf(tg, sizeof(tg), "%d / %d", (int)g_lastMatched, (int)g_lastOnscr);   // реально ли матчатся игроки
            ImGui::TextColored(ImVec4(0.70f, 0.65f, 0.78f, 1.0f), "Tags match/seen:");
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(tg).x);
            ImGui::TextColored(g_lastMatched > 0 ? ImVec4(0.55f, 0.9f, 0.55f, 1.0f) : ImVec4(0.9f, 0.8f, 0.4f, 1.0f), "%s", tg);

            ImGui::Spacing();
            ImGui::TextDisabled("Auto-detects the client and self-heals.");
            EndCard();
            ImGui::EndGroup();

            ImGui::SameLine(0, 14.0f);

            // Right column: Controls (menu key, binds, reset)
            BeginCard("card_controls", "CONTROLS", ImVec2(colW, 0));
            ImGui::TextColored(ImVec4(0.70f, 0.65f, 0.78f, 1.0f), "Menu key");
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - 58.0f);
            KeybindPill("menu_bind", g_config.menuKey, 7, ImVec2(54, 19));

            ImGui::Spacing();
            ImGui::Spacing();
            if (ImGui::Button("Unbind all keys", ImVec2(-1, 28))) {
                g_config.kWallhack.key    = 0;
                g_config.kEsp2d.key       = 0;
                g_config.kTracers.key     = 0;
                g_config.kAimbot.key      = 0;
                g_config.kAutoclicker.key = 0;
            }
            ImGui::Spacing();
            if (ImGui::Button("Reset to defaults", ImVec2(-1, 28))) {
                g_config = RenderConfig();
            }

            ImGui::Spacing();
            ImGui::Spacing();
            ImGui::TextDisabled("Click a key box, then press a key.");
            ImGui::TextDisabled("ESC / DEL / BACKSPACE clears a bind.");
            EndCard();
        }

        ImGui::EndChild();
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
}

// ── Обработка оконных сообщений и горячих клавиш в WndProc ───────────────────
LRESULT CALLBACK hkWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (g_waitingBindId != -1) {
        int newKey = 0;
        if (msg == WM_KEYDOWN) {
            newKey = (int)wParam;
            if (newKey == VK_ESCAPE || newKey == VK_DELETE || newKey == VK_BACK) {
                newKey = 0;
            }
        } else if (msg == WM_XBUTTONDOWN) {
            WORD btn = GET_XBUTTON_WPARAM(wParam);
            newKey = (btn == XBUTTON1) ? VK_XBUTTON1 : VK_XBUTTON2;
        } else if (msg == WM_MBUTTONDOWN) {
            newKey = VK_MBUTTON;
        }

        if (newKey != 0 || msg == WM_KEYDOWN) {
            switch (g_waitingBindId) {
                case 1: g_config.kWallhack.key    = newKey; break;
                case 3: g_config.kTracers.key     = newKey; break;
                case 5: g_config.kAimbot.key      = newKey; break;
                case 6: g_config.kAutoclicker.key = newKey; break;
                case 7: if (newKey != 0) g_config.menuKey = newKey; break;
                case 9: g_config.kEsp2d.key       = newKey; break;
            }
            g_waitingBindId = -1;
            return TRUE;
        }
    }

    if (msg == WM_KEYDOWN) {
        if (wParam == (WPARAM)g_config.menuKey) {
            g_menuOpen = !g_menuOpen;
            if (g_menuOpen) {
                ClipCursor(NULL);
                ShowCursor(TRUE);
            } else {
                ShowCursor(FALSE);
            }
            return TRUE;
        }

        if (!g_menuOpen) {
            if (g_config.kWallhack.key && wParam == (WPARAM)g_config.kWallhack.key)
                g_config.wallhack = !g_config.wallhack;
            if (g_config.kEsp2d.key && wParam == (WPARAM)g_config.kEsp2d.key)
                g_config.esp2d = !g_config.esp2d;
            if (g_config.kTracers.key && wParam == (WPARAM)g_config.kTracers.key)
                g_config.tracers = !g_config.tracers;
            // AutoClicker and AimBot work on hold, not toggle
        }
    }

    if (g_menuOpen) {
        ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam);

        // Intercept mouse input so game cursor doesn't turn camera
        switch (msg) {
            case WM_MOUSEMOVE:
            case WM_LBUTTONDOWN:
            case WM_LBUTTONUP:
            case WM_RBUTTONDOWN:
            case WM_RBUTTONUP:
            case WM_MBUTTONDOWN:
            case WM_MBUTTONUP:
            case WM_MOUSEWHEEL:
            case WM_INPUT:
            case WM_SETCURSOR:
                return TRUE;
        }
    }

    return CallWindowProcW(oWndProc, hwnd, msg, wParam, lParam);
}

// ══════════════════════════════════════════════════════════════════════════════
//  SWAPBUFFERS
// ══════════════════════════════════════════════════════════════════════════════

static ULONGLONG g_lastClickTime = 0;

// ── Physical ЛКМ tracking через WH_MOUSE_LL (отсекаем synthetic из mouse_event через LLMHF_INJECTED). ──
// Нужно потому что auto-clicker без бинда триггерится по LBUTTON hold, но его собственный
// mouse_event(LEFTUP) сбрасывает GetAsyncKeyState(VK_LBUTTON) и trigger обрывается.
// Низкоуровневый hook требует message pump в thread'е установки — выделяем свой с GetMessage-циклом.
static volatile bool g_physLButtonDown = false;
static HHOOK g_mouseHook = nullptr;
static LRESULT CALLBACK PhysLButtonHook(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        const MSLLHOOKSTRUCT* p = (const MSLLHOOKSTRUCT*)lParam;
        if (p && !(p->flags & LLMHF_INJECTED)) {
            if (wParam == WM_LBUTTONDOWN) g_physLButtonDown = true;
            else if (wParam == WM_LBUTTONUP) g_physLButtonDown = false;
        }
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

BOOL WINAPI hkWglSwapBuffers(HDC hdc) {
    // Паттерн A: XOR check. После SelfDisable() g_authValid=0 → return сразу.
    AUTH_GUARD_A_RET(oWglSwapBuffers(hdc));

    // Паттерн D: hard session timeout — защита от debugger freeze (пауза, патч, resume).
    AUTH_GUARD_D_RET(oWglSwapBuffers(hdc));

    // Паттерн E: VEH breakpoint detection — если кто-то поставил int3 внутри DLL, g_bpDetected=true.
    AUTH_GUARD_E_RET(oWglSwapBuffers(hdc));

    // ── Lwjgl fullscreen-toggle recovery: HWND может пересоздаться / Lwjgl восстановить свой WndProc.
    //    ESP продолжает работать (GL context жив), но меню теряет input. Переустанавливаем hook. ──
    if (g_imguiInitialized) {
        HWND curH = WindowFromDC(hdc);
        if (curH) {
            if (curH != g_hwnd) {
                // HWND recreated (toggle fullscreen в Lwjgl 2.x destroy+create). Перенос hook'а.
                g_hwnd = curH;
                oWndProc = (WNDPROC)SetWindowLongPtrW(curH, GWLP_WNDPROC, (LONG_PTR)hkWndProc);
                ImGui_ImplWin32_Shutdown();
                ImGui_ImplWin32_Init(curH);
                Log("WndProc: HWND recreated, re-attached to %p", curH);
            } else if ((WNDPROC)GetWindowLongPtrW(curH, GWLP_WNDPROC) != hkWndProc) {
                // Lwjgl восстановил свой WndProc поверх нашего. Пере-ставим hook.
                oWndProc = (WNDPROC)SetWindowLongPtrW(curH, GWLP_WNDPROC, (LONG_PTR)hkWndProc);
                Log("WndProc: hook overwritten by host, re-installed on %p", curH);
            }
        }
    }

    if (!g_imguiInitialized) {
        g_hwnd = WindowFromDC(hdc);
        if (g_hwnd) {
            oWndProc = (WNDPROC)SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, (LONG_PTR)hkWndProc);

            IMGUI_CHECKVERSION();
            ImGui::CreateContext();
            ImGuiIO& io = ImGui::GetIO();
            io.IniFilename = NULL;
            io.ConfigErrorRecovery = false;
            io.ConfigErrorRecoveryEnableTooltip = false;
            io.ConfigErrorRecoveryEnableAssert = false;

            // Load crisp anti-aliased Segoe UI fonts
            ImFontConfig fontCfg;
            fontCfg.OversampleH = 3;
            fontCfg.OversampleV = 2;
            fontCfg.PixelSnapH = false;

            const ImWchar* glyphRanges = io.Fonts->GetGlyphRangesCyrillic();

            char winDir[MAX_PATH] = {}; GetWindowsDirectoryA(winDir, MAX_PATH);   // без хардкода C:\Windows
            char fReg[MAX_PATH], fBold[MAX_PATH];
            snprintf(fReg,  sizeof(fReg),  "%s\\Fonts\\segoeui.ttf",  winDir);
            snprintf(fBold, sizeof(fBold), "%s\\Fonts\\segoeuib.ttf", winDir);
            if (GetFileAttributesA(fReg) != INVALID_FILE_ATTRIBUTES) {
                g_fontBody  = io.Fonts->AddFontFromFileTTF(fReg, 15.0f, &fontCfg, glyphRanges);
                g_fontSmall = io.Fonts->AddFontFromFileTTF(fReg, 12.0f, &fontCfg, glyphRanges);
            }
            if (GetFileAttributesA(fBold) != INVALID_FILE_ATTRIBUTES) {
                g_fontBold  = io.Fonts->AddFontFromFileTTF(fBold, 15.0f, &fontCfg, glyphRanges);
                g_fontTitle = io.Fonts->AddFontFromFileTTF(fBold, 19.0f, &fontCfg, glyphRanges);
            }

            if (!g_fontBody) {
                g_fontBody = io.Fonts->AddFontDefault();
            }

            // MC-стиль шрифт (Monocraft) для неймтегов — печём в тот же атлас, рисуем нативно (без ImGui-кадра).
            ImFontConfig mcCfg;
            mcCfg.FontDataOwnedByAtlas = false;   // данные статические (встроены) — ImGui не должен их освобождать
            mcCfg.OversampleH = 1; mcCfg.OversampleV = 1; mcCfg.PixelSnapH = true;   // чёткие пиксели MC
            g_fontMC = io.Fonts->AddFontFromMemoryTTF((void*)g_monocraft_ttf, (int)g_monocraft_ttf_len, 18.0f, &mcCfg);

            ImGui_ImplWin32_Init(g_hwnd);
            IMGL_Init();

            // Пред-выпекаем глифы ВСЕХ шрифтов ДО снимка атласа (nov::Init) — иначе ленивая выпечка меню-шрифтов
            // в рантайме репакнет атлас (динамические шрифты 1.93) и наш снимок-текстура станет невалидным.
            auto prebake = [](ImFont* f, float sz) { if (!f) return; ImFontBaked* b = f->GetFontBaked(sz); if (!b) return;
                for (ImWchar c = 32; c < 256; ++c) b->FindGlyph(c); for (ImWchar c = 0x410; c <= 0x44F; ++c) b->FindGlyph(c); };
            prebake(g_fontBody, 15.0f); prebake(g_fontSmall, 12.0f); prebake(g_fontBold, 15.0f); prebake(g_fontTitle, 19.0f);

            // Нативный рендер оверлея: своя GL-текстура из (уже полного) атласа ImGui + глифы MC-шрифта. Один раз.
            if (nov::Init(g_fontMC)) Log("NATIVE-OVERLAY ready (MC font atlas uploaded)");
            else                     Log("NATIVE-OVERLAY init failed — overlays disabled (menu still works)");

            ImGui::StyleColorsDark();
            ImGuiStyle& style = ImGui::GetStyle();
            style.WindowRounding    = 10.0f;
            style.ChildRounding     = 8.0f;
            style.FrameRounding     = 5.0f;
            style.PopupRounding     = 6.0f;
            style.ScrollbarRounding = 6.0f;
            style.GrabRounding      = 5.0f;
            style.TabRounding       = 6.0f;
            style.WindowBorderSize  = 1.0f;
            style.FrameBorderSize   = 1.0f;
            style.WindowPadding     = ImVec2(14, 14);
            style.ItemSpacing       = ImVec2(10, 8);
            style.ItemInnerSpacing  = ImVec2(8, 6);

            // Neverlose CS2 Obsidian & Deep Violet Palette
            ImVec4* colors = style.Colors;
            colors[ImGuiCol_WindowBg]             = ImVec4(0.04f, 0.05f, 0.08f, 0.98f);
            colors[ImGuiCol_ChildBg]              = ImVec4(0.06f, 0.07f, 0.11f, 0.75f);
            colors[ImGuiCol_PopupBg]              = ImVec4(0.07f, 0.08f, 0.13f, 0.98f);
            colors[ImGuiCol_Border]               = ImVec4(0.18f, 0.16f, 0.28f, 0.70f);
            colors[ImGuiCol_BorderShadow]         = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
            colors[ImGuiCol_FrameBg]              = ImVec4(0.10f, 0.11f, 0.17f, 0.85f);
            colors[ImGuiCol_FrameBgHovered]       = ImVec4(0.18f, 0.16f, 0.28f, 0.90f);
            colors[ImGuiCol_FrameBgActive]        = ImVec4(0.26f, 0.20f, 0.40f, 0.95f);
            colors[ImGuiCol_TitleBg]              = ImVec4(0.07f, 0.06f, 0.11f, 1.00f);
            colors[ImGuiCol_TitleBgActive]        = ImVec4(0.13f, 0.10f, 0.20f, 1.00f);
            colors[ImGuiCol_TitleBgCollapsed]     = ImVec4(0.05f, 0.05f, 0.08f, 0.75f);
            colors[ImGuiCol_MenuBarBg]            = ImVec4(0.07f, 0.06f, 0.11f, 1.00f);
            colors[ImGuiCol_ScrollbarBg]          = ImVec4(0.05f, 0.05f, 0.08f, 0.60f);
            colors[ImGuiCol_ScrollbarGrab]        = ImVec4(0.24f, 0.16f, 0.36f, 0.80f);
            colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.36f, 0.22f, 0.52f, 0.90f);
            colors[ImGuiCol_ScrollbarGrabActive]  = ImVec4(0.48f, 0.28f, 0.68f, 1.00f);
            colors[ImGuiCol_CheckMark]            = ImVec4(0.85f, 0.55f, 1.00f, 1.00f);
            colors[ImGuiCol_SliderGrab]           = ImVec4(0.68f, 0.38f, 0.95f, 1.00f);
            colors[ImGuiCol_SliderGrabActive]     = ImVec4(0.85f, 0.55f, 1.00f, 1.00f);
            colors[ImGuiCol_Button]               = ImVec4(0.12f, 0.11f, 0.18f, 0.90f);
            colors[ImGuiCol_ButtonHovered]        = ImVec4(0.28f, 0.18f, 0.44f, 1.00f);
            colors[ImGuiCol_ButtonActive]         = ImVec4(0.42f, 0.24f, 0.65f, 1.00f);
            colors[ImGuiCol_Header]               = ImVec4(0.20f, 0.15f, 0.32f, 0.75f);
            colors[ImGuiCol_HeaderHovered]        = ImVec4(0.32f, 0.20f, 0.50f, 0.85f);
            colors[ImGuiCol_HeaderActive]         = ImVec4(0.45f, 0.25f, 0.70f, 1.00f);
            colors[ImGuiCol_Separator]            = ImVec4(0.20f, 0.17f, 0.30f, 0.60f);
            colors[ImGuiCol_SeparatorHovered]     = ImVec4(0.38f, 0.25f, 0.58f, 0.80f);
            colors[ImGuiCol_SeparatorActive]      = ImVec4(0.55f, 0.32f, 0.82f, 1.00f);
            colors[ImGuiCol_ResizeGrip]           = ImVec4(0.25f, 0.18f, 0.40f, 0.40f);
            colors[ImGuiCol_ResizeGripHovered]    = ImVec4(0.45f, 0.25f, 0.70f, 0.70f);
            colors[ImGuiCol_ResizeGripActive]     = ImVec4(0.65f, 0.38f, 0.95f, 0.95f);
            colors[ImGuiCol_Tab]                  = ImVec4(0.10f, 0.09f, 0.15f, 0.85f);
            colors[ImGuiCol_TabHovered]           = ImVec4(0.28f, 0.18f, 0.44f, 0.90f);
            colors[ImGuiCol_TabActive]            = ImVec4(0.22f, 0.14f, 0.35f, 1.00f);
            colors[ImGuiCol_TabUnfocused]         = ImVec4(0.08f, 0.07f, 0.12f, 0.90f);
            colors[ImGuiCol_TabUnfocusedActive]   = ImVec4(0.15f, 0.11f, 0.24f, 1.00f);
            colors[ImGuiCol_Text]                 = ImVec4(0.95f, 0.95f, 0.98f, 1.00f);
            colors[ImGuiCol_TextDisabled]         = ImVec4(0.50f, 0.52f, 0.62f, 1.00f);

            g_imguiInitialized = true;
            Log("SUCCESS: ImGui initialized on hwnd %p", g_hwnd);
        }
    }

    if (g_hwnd) {
        RECT rc;
        if (GetClientRect(g_hwnd, &rc)) {
            g_screenWidth  = rc.right - rc.left;
            g_screenHeight = rc.bottom - rc.top;
        }
    }

    // ── Модуль автоматической генерации кликов мыши по таймеру ───────────────
    static ULONGLONG s_lastClickCycleTime = 0;
    static bool s_clickDown = false;

    bool clickerHoldActive = false;
    if (g_config.autoclicker && !g_menuOpen) {
        if (g_config.kAutoclicker.key != 0) {
            clickerHoldActive = (GetAsyncKeyState(g_config.kAutoclicker.key) & 0x8000) != 0;
        } else {
            // ВАЖНО: нельзя GetAsyncKeyState(VK_LBUTTON) — mouse_event(LEFTUP) ниже
            // синтетически сбрасывает глобальный state ЛКМ → trigger обрывается на 1-м клике.
            // g_physLButtonDown трекается через WH_MOUSE_LL с фильтром LLMHF_INJECTED (physical only).
            clickerHoldActive = g_physLButtonDown;
        }
    }

    if (clickerHoldActive && g_hwnd) {
        ULONGLONG now = GetTickCount64();
        int targetCps = (g_config.cps >= 1 && g_config.cps <= 30) ? g_config.cps : 12;
        ULONGLONG fullPeriod = 1000 / targetCps;
        ULONGLONG downDuration = fullPeriod / 2;
        if (downDuration < 20) downDuration = 20;

        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(g_hwnd, &pt);
        LPARAM lp = MAKELPARAM(pt.x, pt.y);

        if (!s_clickDown) {
            if (now - s_lastClickCycleTime >= fullPeriod) {
                s_lastClickCycleTime = now;
                s_clickDown = true;

                PostMessageW(g_hwnd, WM_LBUTTONDOWN, MK_LBUTTON, lp);
                mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
            }
        } else {
            if (now - s_lastClickCycleTime >= downDuration) {
                s_clickDown = false;

                PostMessageW(g_hwnd, WM_LBUTTONUP, 0, lp);
                mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
            }
        }
    } else {
        if (s_clickDown && g_hwnd) {
            s_clickDown = false;
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(g_hwnd, &pt);
            LPARAM lp = MAKELPARAM(pt.x, pt.y);

            PostMessageW(g_hwnd, WM_LBUTTONUP, 0, lp);
            mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
        }
    }

    // ── Аимбот: пока зажат бинд (или ПКМ по умолчанию) тянем прицел к ближайшему к центру игроку — в грудь ──
    // Цель — из УЖЕ ОТРИСОВАННЫХ моделей кадра (g_entities: screenX/screenY посчитаны в GL-хуке, как у ESP-боксов).
    if (g_config.aimbot && !g_menuOpen && g_screenWidth > 0 && g_screenHeight > 0 && g_hwnd) {
        bool aimHold;
        if (g_config.kAimbot.key != 0) aimHold = (GetAsyncKeyState(g_config.kAimbot.key) & 0x8000) != 0;
        else                           aimHold = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;   // по умолчанию — ПКМ
        if (aimHold) {
            const float ccx = g_screenWidth * 0.5f, ccy = g_screenHeight * 0.5f;
            const float fov = (g_config.aimFov >= 5.0f) ? g_config.aimFov : 300.0f;
            float best = fov, tx = 0.0f, ty = 0.0f; bool found = false;
            EnterCriticalSection(&g_entityLock);
            for (const auto& e : g_entities) {
                if (e.isChest) continue;                                            // только игроки/модели, не сундуки
                if (e.screenX <= 0.0f || e.screenY <= 0.0f || e.screenBottomY <= 0.0f) continue;
                const float chestY = e.screenY + 0.30f * (e.screenBottomY - e.screenY);   // грудь: 30% вниз от головы к ступням
                const float ex = e.screenX - ccx, ey = chestY - ccy;
                const float d = sqrtf(ex * ex + ey * ey);
                if (d < best) { best = d; tx = e.screenX; ty = chestY; found = true; }
            }
            LeaveCriticalSection(&g_entityLock);
            if (found) {
                const float g = (g_config.aimSmooth > 0.01f && g_config.aimSmooth <= 1.0f) ? g_config.aimSmooth : 0.35f;
                float fx = (tx - ccx) * g, fy = (ty - ccy) * g;
                const float cap = 100.0f;                                            // предохранитель от резкого рывка/овершута
                if (fx >  cap) fx =  cap; else if (fx < -cap) fx = -cap;
                if (fy >  cap) fy =  cap; else if (fy < -cap) fy = -cap;
                int mdx = (int)(fx + (fx >= 0 ? 0.5f : -0.5f));
                int mdy = (int)(fy + (fy >= 0 ? 0.5f : -0.5f));
                if (mdx != 0 || mdy != 0) mouse_event(MOUSEEVENTF_MOVE, (DWORD)mdx, (DWORD)mdy, 0, 0);
            }
        }
    }

    // ── Glow по силуэту: перерисовка модели в FBO-маску + шейдер свечения (до ImGui, под меню) ──
    if (g_config.outline && !g_outlineParts.empty() && oGlCallList && g_screenWidth > 0) {
        // Glow всегда сквозь стены — без depth-mask (occlude=false). Фишка глоу = видимость противника через блоки.
        ofx::Render(g_lastProj, g_outlineParts.data(), (int)g_outlineParts.size(),
                    oGlCallList, g_screenWidth, g_screenHeight, g_config.outlineColor, g_config.outlineSpread, false);
        static bool s_ofxLogged = false;
        if (!s_ofxLogged) { s_ofxLogged = true;
            if (ofx::S().failed)      Log("OUTLINE-FX FAILED: %s", ofx::Err());
            else if (ofx::S().ready)  Log("OUTLINE-FX ready (FBO + shaders ok)");
        }
    }

    // ── Отрисовка ImGui оверлея и интерфейса перед выводом буфера на экран ─────
    if (g_imguiInitialized) {
        ImGuiIO& io = ImGui::GetIO();
        io.MouseDrawCursor = g_menuOpen;

        if (g_radarReady) UpdateGameState();   // coord self-heal (outside the ImGui frame, cheap)

        // ── All 2D overlays via native GL (single pass, no ImGui frame). ImGui runs only for the menu. ──
        if (nov::Ready() && g_screenWidth > 0) {
            const bool any2d = g_config.esp2d || g_config.showDistance || g_config.nametags;
            if (any2d) {
                nov::Begin(g_screenWidth, g_screenHeight);
                if (g_config.esp2d || g_config.showDistance) RenderOverlayNative();
                if (g_config.nametags)                       RenderPlayerOverlaysNative();
                nov::End();
            }
        }

        // ── ImGui-кадр ТОЛЬКО для меню ──
        if (g_menuOpen) {
            IMGL_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();
            RenderMenu();
            ImGui::Render();
            IMGL_RenderDrawData(ImGui::GetDrawData());
        }
    }

    EnterCriticalSection(&g_entityLock);
    g_entities.clear();
    LeaveCriticalSection(&g_entityLock);
    g_camCaptured = false; g_camGotPitch = false;   // reset camera orientation capture for next frame
    g_outlineParts.clear();                          // safety: reset accumulated outline parts for new frame

    if (g_glProbe) {                                  // диагностика отрисовки моделей: раз в ~2с в лог
        static ULONGLONG lastProbe = 0;
        const ULONGLONG now = GetTickCount64();
        if (now - lastProbe > 2000) {
            GLint sb = -1, db = -1; glGetIntegerv(GL_STENCIL_BITS, &sb); glGetIntegerv(GL_DEPTH_BITS, &db);
            const char* ver = (const char*)glGetString(GL_VERSION);
            const char* ren = (const char*)glGetString(GL_RENDERER);
            Log("GLPROBE stencilBits=%d depthBits=%d ver=[%s] ren=[%s] | perEntityRender/2s: callList=%u drawArrays=%u drawElements=%u begin=%u",
                sb, db, ver ? ver : "?", ren ? ren : "?", g_cCallList, g_cDrawArr, g_cDrawElem, g_cBegin);
            g_cCallList = g_cDrawArr = g_cDrawElem = g_cBegin = 0;
            lastProbe = now;
        }
    }

    return oWglSwapBuffers ? oWglSwapBuffers(hdc) : FALSE;
}

// ══════════════════════════════════════════════════════════════════════════════
//  ПЕРЕХВАТ ДИНАМИЧЕСКОГО РЕЗОЛВИНГА ФУНКЦИЙ ЧЕРЕЗ GETPROCADDRESSFORCALLER
// ══════════════════════════════════════════════════════════════════════════════

FARPROC WINAPI hkGetProcAddressForCaller(HMODULE hModule, LPCSTR lpProcName, LPVOID lpCaller) {
    // Resolve mode (VimeWorld, g_directHooks == false): intercept GL function resolution and return OUR
    // детур-функции — байты opengl32 при этом НЕ патчатся (нет прямого MinHook, который на вайме не работает).
    // В прямом режиме (Lunar) пропускаем: там GL-функции патчатся напрямую MinHook, а перехват резолва конфликтует.
    if (!g_directHooks && lpProcName && ((ULONG_PTR)lpProcName > 0xFFFF)) {
        static HMODULE hOpenGL = GetModuleHandleW(L"opengl32.dll");
        if (!hOpenGL) hOpenGL = GetModuleHandleW(L"opengl32.dll");
        if (hModule == hOpenGL) {
            if (strcmp(lpProcName, "glOrtho") == 0) {
                if (!oGlOrtho) oGlOrtho = (pfn_glOrtho)oGetProcAddressForCaller(hModule, lpProcName, lpCaller);
                return (FARPROC)hkGlOrtho;
            }
            if (strcmp(lpProcName, "glScalef") == 0) {
                if (!oGlScalef) oGlScalef = (pfn_glScalef)oGetProcAddressForCaller(hModule, lpProcName, lpCaller);
                return (FARPROC)hkGlScalef;
            }
            if (strcmp(lpProcName, "glRotatef") == 0) {
                if (!oGlRotatef) oGlRotatef = (pfn_glRotatef)oGetProcAddressForCaller(hModule, lpProcName, lpCaller);
                return (FARPROC)hkGlRotatef;
            }
            if (strcmp(lpProcName, "glTranslatef") == 0) {
                if (!oGlTranslatef) oGlTranslatef = (pfn_glTranslatef)oGetProcAddressForCaller(hModule, lpProcName, lpCaller);
                return (FARPROC)hkGlTranslatef;
            }
            if (strcmp(lpProcName, "glEnable") == 0) {
                if (!oGlEnable) oGlEnable = (pfn_glEnable)oGetProcAddressForCaller(hModule, lpProcName, lpCaller);
                return (FARPROC)hkGlEnable;
            }
            if (strcmp(lpProcName, "glDisable") == 0) {
                if (!oGlDisable) oGlDisable = (pfn_glDisable)oGetProcAddressForCaller(hModule, lpProcName, lpCaller);
                return (FARPROC)hkGlDisable;
            }
            if (strcmp(lpProcName, "glCallList") == 0) {   // Glow: перерисовка display-list модели в FBO-маску
                if (!oGlCallList) oGlCallList = (pfn_glCallList)oGetProcAddressForCaller(hModule, lpProcName, lpCaller);
                return (FARPROC)hkGlCallList;
            }
        }
    }

    return oGetProcAddressForCaller(hModule, lpProcName, lpCaller);
}


// ══════════════════════════════════════════════════════════════════════════════
//  ТОЧКА ВХОДА DLLMAIN И ИНИЦИАЛИЗАЦИЯ ПЕРЕХВАТОВ
// ══════════════════════════════════════════════════════════════════════════════

// «Живость» найденной раскладки: синглтон валиден и его класс совпадает; локальный игрок (если мир загружен) —
// валидный игрок с реальным ником на entName. Ловит и неверный автодетект модели, и самосогласованное-но-ложное
// определение (тогда ник на entName будет мусором). В меню (нет thePlayer) считаем ОК — это не ошибка раскладки.
static bool LayoutHealthy(const mc::Layout& L) {
    const uintptr_t mc = mc::CurrentMinecraft(L);
    if (!mc::IsHeapAddr(mc)) return false;
    if (mc::NarrowKlassOf(mc) != L.nkMinecraft) return false;
    const uintptr_t lp = L.mcThePlayer ? mc::ReadOop(mc, L.mcThePlayer) : 0;
    if (!lp) return true;                                  // меню/загрузка — синглтон ок, мира нет (не ошибка)
    const uint32_t nk = mc::NarrowKlassOf(lp);
    bool isP = false; for (int i = 0; i < L.nPlayers; ++i) if (nk == L.nkPlayers[i]) { isP = true; break; }
    if (!isP) return false;
    if (!L.entName) return true;
    std::wstring nm; mc::ReadJavaString(L, mc::ReadOop(lp, L.entName), nm, 24);
    return mc::IsUserName(nm);                             // валидный ник локального игрока = раскладка верна
}

// Фоновый поток дискавери — САМОДОСТАТОЧНЫЙ: авто-поиск при старте (ретрай до успеха, предметы не нужны),
// затем монитор здоровья — при поломке раскладки (неверный автодетект/битое состояние) авто-переоткрытие БЕЗ кнопки.
// НЕ в DllMain (loader lock). Переживает смену карты сам (Layout стабилен, PlayerRadar пере-сканит игроков).
static DWORD WINAPI RadarThread(LPVOID) {
    while (!g_radarStop) {
        // ── полный сброс состояния перед (пере)поиском ──
        g_radarReady = false;
        g_playerRadar.Stop();                 // остановить текущий скан игроков (join; не в DllMain -> безопасно)
        // Codec НЕ сбрасываем если он уже успешно задетектился: class-space статичен в HotSpot (не меняется между
        // re-discovery в рамках одной JVM). Повторный AutoDetect на Lunar может дать score 650-700 из-за timing →
        // fallback → fail. Кешированный codec стабилен.
        if (!mc::Codec().detected) mc::Codec() = mc::KlassCodec();
        g_radarLayout = mc::Layout();         // clean Layout (all offsets/classes from scratch)
        g_everMatched = false; g_lastOnscrAt = 0; g_lastMatchAt = 0;

        // ── попытки дискавери: БЕЗ лимита, пока не найдём (само-подхват при загрузке мира) или выгрузка. Предметы не нужны. ──
        while (!g_radarReady && !g_radarStop) {
            if (mc::DiscoverAll(g_radarLayout)) {
                g_playerRadar.Start(g_radarLayout);
                g_radarReady = true;
                Log("RADAR: discovery OK — PlayerRadar started (nPlayers=%d entName+%d nkEntityItem=%#x)", g_radarLayout.nPlayers, g_radarLayout.entName, g_radarLayout.nkEntityItem);
                break;
            }
            for (int s = 0; s < 20 && !g_radarStop; ++s) Sleep(100);   // ~2с между попытками
        }

        // ── МОНИТОР ЗДОРОВЬЯ: держим готовность; если раскладка невалидна ~10с подряд (кривой автодетект/битьё) —
        //    АВТО-переоткрытие с нуля (сброс модели+раскладки в начале цикла). Без ручного вмешательства. ──
        int bad = 0;
        while (g_radarReady && !g_radarStop) {
            Sleep(1000);
            if (LayoutHealthy(g_radarLayout)) { bad = 0; continue; }
            if (++bad >= 10) { Log("RADAR: layout unhealthy ~10s -> auto re-discovery"); g_radarReady = false; break; }
        }
    }
    return 0;
}

// DIRECT-режим (Lunar): прямой inline-хук экспорта opengl32 через MinHook.
static void HookGLExport(HMODULE hGL, const char* name, LPVOID detour, LPVOID* orig) {
    FARPROC p = GetProcAddress(hGL, name);
    if (!p) return;
    if (MH_CreateHook((LPVOID)p, detour, orig) == MH_OK) MH_EnableHook((LPVOID)p);
}

// Pick hook mechanism: %TEMP%\mle_hook_mode.txt ("resolve"/"direct") marker written by the injector; auto otherwise.
static void ReadHookMode() {
    wchar_t tmp[MAX_PATH] = {};
    if (GetTempPathW(MAX_PATH, tmp)) {
        std::wstring p = std::wstring(tmp) + L"mle_hook_mode.txt";
        HANDLE f = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            char buf[16] = {}; DWORD rd = 0; ReadFile(f, buf, sizeof(buf) - 1, &rd, nullptr); CloseHandle(f);
            g_directHooks = !(buf[0] == '0' || buf[0] == 'r' || buf[0] == 'R');
            return;
        }
    }
    g_directHooks = (GetModuleHandleW(L"opengl32.dll") != nullptr);   // opengl32 уже загружен = игра запущена = direct
}

static void SetupHooks() {
    wchar_t tmp[MAX_PATH] = {};
    if (GetTempPathW(MAX_PATH, tmp)) swprintf_s(g_logFilePath, L"%smle_esp_log.txt", tmp);

    InitializeCriticalSection(&g_entityLock);

    // Low-level mouse hook runs in a dedicated thread with its own GetMessage pump:
    // WH_MOUSE_LL callbacks fire in the registering thread, which must pump messages.
    CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
        HMODULE hDll = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)&PhysLButtonHook, &hDll);
        g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, PhysLButtonHook, hDll, 0);
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg); DispatchMessageW(&msg);
        }
        return 0;
    }, nullptr, 0, nullptr);

    ReadHookMode();                                  // pick DIRECT/RESOLVE BEFORE loading opengl32
    MH_STATUS mhInit = MH_Initialize();
    if (mhInit != MH_OK && mhInit != MH_ERROR_ALREADY_INITIALIZED) return;

    HMODULE hKernelBase = GetModuleHandleW(L"kernelbase.dll");
    if (!hKernelBase) hKernelBase = GetModuleHandleW(L"kernel32.dll");
    if (hKernelBase) {
        FARPROC pGPAFC = GetProcAddress(hKernelBase, "GetProcAddressForCaller");
        if (pGPAFC) {
            if (MH_CreateHook(pGPAFC, (LPVOID)&hkGetProcAddressForCaller, (LPVOID*)&oGetProcAddressForCaller) == MH_OK)
                MH_EnableHook(pGPAFC);
        }
    }

    HMODULE hOpenGL = GetModuleHandleW(L"opengl32.dll");
    if (!hOpenGL) hOpenGL = LoadLibraryA("opengl32.dll");
    if (hOpenGL) {
        FARPROC pSwap = GetProcAddress(hOpenGL, "wglSwapBuffers");
        if (pSwap) {
            if (MH_CreateHook(pSwap, (LPVOID)&hkWglSwapBuffers, (LPVOID*)&oWglSwapBuffers) == MH_OK)
                MH_EnableHook(pSwap);
        }
        if (g_directHooks) {
            HookGLExport(hOpenGL, "glOrtho",      (LPVOID)&hkGlOrtho,      (LPVOID*)&oGlOrtho);
            HookGLExport(hOpenGL, "glScalef",     (LPVOID)&hkGlScalef,     (LPVOID*)&oGlScalef);
            HookGLExport(hOpenGL, "glRotatef",    (LPVOID)&hkGlRotatef,    (LPVOID*)&oGlRotatef);
            HookGLExport(hOpenGL, "glTranslatef", (LPVOID)&hkGlTranslatef, (LPVOID*)&oGlTranslatef);
            HookGLExport(hOpenGL, "glEnable",     (LPVOID)&hkGlEnable,     (LPVOID*)&oGlEnable);
            HookGLExport(hOpenGL, "glDisable",    (LPVOID)&hkGlDisable,    (LPVOID*)&oGlDisable);
            HookGLExport(hOpenGL, "glCallList",   (LPVOID)&hkGlCallList,   (LPVOID*)&oGlCallList);
        }
    }

    CreateThread(nullptr, 0, RadarThread, nullptr, 0, nullptr);
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        SetupHooks();
    } else if (ul_reason_for_call == DLL_PROCESS_DETACH) {
        g_radarStop = true;                                            // остановить поток дискавери (крутится вечно ради rescan)
        g_radarReady = false;
        Sleep(200);                                                    // дать RadarThread выйти из wait-петли (best-effort)
        g_playerRadar.StopAsync();   // без join — безопасно из DllMain
        if (g_imguiInitialized) {
            if (oWndProc && g_hwnd) {
                SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, (LONG_PTR)oWndProc);
            }
            IMGL_Shutdown();
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
        }
        MH_DisableHook(MH_ALL_HOOKS);
        if (g_mouseHook) { UnhookWindowsHookEx(g_mouseHook); g_mouseHook = nullptr; }
        MH_Uninitialize();
        DeleteCriticalSection(&g_entityLock);
    }
    return TRUE;
}

// overlay2d.h — нативный 2D-рендер оверлея (без ImGui-кадра): ortho + прямоугольники/линии + текст из атласа шрифта.
// Текст рисуется по глифам ImFont (UV из уже собранного атласа ImGui), но СВОЕЙ GL-текстурой и своими квадами —
// поэтому в геймплее ImGui NewFrame/Render/RenderDrawData не крутятся (ImGui только для меню). Fixed-function GL.
#pragma once
#include <windows.h>
#include <GL/gl.h>
#include "imgui.h"

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

namespace nov {

inline GLuint&  FontTex() { static GLuint t = 0; return t; }
inline ImFont*& Font()    { static ImFont* f = nullptr; return f; }
inline bool&    Ready()   { static bool r = false; return r; }
// Пред-выпекаемые размеры MC-шрифта (рисуем ТОЛЬКО ими — иначе рантайм-выпечка репакнет атлас -> наша текстура устареет).
static const float PX_TAG = 12.0f;   // неймтеги / дистанция / предметы
static const float PX_BIG = 22.0f;   // тосты-алерты

// glUseProgram/glActiveTexture нет в gl.h 1.1 — грузим через wglGetProcAddress (нужно сбросить шейдер перед FF-рисованием).
struct NovGL { void (APIENTRY* UseProgram)(GLuint); void (APIENTRY* ActiveTexture)(GLenum); };
inline NovGL& GL() { static NovGL g = { nullptr, nullptr }; return g; }

// Пред-выпечь ASCII на нужных размерах, снять пиксели атласа в СВОЮ GL-текстуру, запомнить шрифт. Один раз.
// Пред-выпечка делает атлас стабильным (в рантайме нет новых глифов -> UV валидны для нашей текстуры-снимка).
// Для настраиваемого Scale на неймтегах/дистанции печём ЦЕЛЫЙ диапазон 7..24 px — чтобы runtime scale (0.6x..2.0x)
// не вызывал ленивую выпечку (которая репакает атлас и инвалидирует нашу GL-текстуру).
inline bool Init(ImFont* font) {
    ImGuiIO& io = ImGui::GetIO();
    if (font) {
        for (int px = 7; px <= 24; ++px) {
            ImFontBaked* b = font->GetFontBaked((float)px);
            if (b) for (ImWchar c = 32; c < 127; ++c) b->FindGlyph(c);
        }
        ImFontBaked* bBig = font->GetFontBaked(PX_BIG);   // для тостов
        if (bBig) for (ImWchar c = 32; c < 127; ++c) bBig->FindGlyph(c);
    }
    unsigned char* pix = nullptr; int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32(&pix, &w, &h);
    if (!pix || w <= 0 || h <= 0) return false;
    GLint prev = 0; glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
    GLuint t = 0; glGenTextures(1, &t); glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pix);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);   // пиксельная чёткость (MC-стиль)
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, prev);
    FontTex() = t; Font() = font;
    GL().UseProgram    = (void (APIENTRY*)(GLuint))wglGetProcAddress("glUseProgram");
    GL().ActiveTexture = (void (APIENTRY*)(GLenum))wglGetProcAddress("glActiveTexture");
    Ready() = (font != nullptr && t != 0);
    return Ready();
}

inline void Begin(int w, int h) {
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    if (GL().UseProgram)    GL().UseProgram(0);         // сбросить шейдер -> работает fixed-function
    if (GL().ActiveTexture) GL().ActiveTexture(0x84C0);// GL_TEXTURE0
    glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity(); glOrtho(0, w, h, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);  glPushMatrix(); glLoadIdentity();
    glDisable(GL_DEPTH_TEST); glDisable(GL_LIGHTING); glDisable(GL_CULL_FACE); glDisable(GL_ALPHA_TEST);
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_LINE_SMOOTH); glHint(GL_LINE_SMOOTH_HINT, GL_NICEST);
    glDisable(GL_TEXTURE_2D);
}
inline void End() {
    glMatrixMode(GL_PROJECTION); glPopMatrix();
    glMatrixMode(GL_MODELVIEW);  glPopMatrix();
    glPopAttrib();
}

inline void Col(ImU32 c) { glColor4ub((GLubyte)(c & 0xff), (GLubyte)((c >> 8) & 0xff), (GLubyte)((c >> 16) & 0xff), (GLubyte)((c >> 24) & 0xff)); }

inline void FillRect(float x0, float y0, float x1, float y1, ImU32 c) {
    glDisable(GL_TEXTURE_2D); Col(c);
    glBegin(GL_QUADS); glVertex2f(x0, y0); glVertex2f(x1, y0); glVertex2f(x1, y1); glVertex2f(x0, y1); glEnd();
}
inline void Line(float x0, float y0, float x1, float y1, ImU32 c, float w) {
    glDisable(GL_TEXTURE_2D); Col(c); glLineWidth(w);
    glBegin(GL_LINES); glVertex2f(x0, y0); glVertex2f(x1, y1); glEnd();
}
inline void RectOutline(float x0, float y0, float x1, float y1, ImU32 c, float w) {
    glDisable(GL_TEXTURE_2D); Col(c); glLineWidth(w);
    glBegin(GL_LINE_LOOP); glVertex2f(x0, y0); glVertex2f(x1, y0); glVertex2f(x1, y1); glVertex2f(x0, y1); glEnd();
}

inline float TextW(const char* s, float px) {       // ширина в пикселях на выпеченном размере px
    ImFont* f = Font(); if (!f || !s) return 0.0f;
    ImFontBaked* b = f->GetFontBaked(px); if (!b) return 0.0f;
    float w = 0.0f;
    for (const unsigned char* p = (const unsigned char*)s; *p; ++p) { ImFontGlyph* g = b->FindGlyph((ImWchar)*p); if (g) w += g->AdvanceX; }
    return w;
}

inline void TextRaw(float x, float y, const char* s, ImU32 c, float px) {
    ImFont* f = Font(); if (!f || !s) return;
    ImFontBaked* b = f->GetFontBaked(px); if (!b) return;
    glEnable(GL_TEXTURE_2D); glBindTexture(GL_TEXTURE_2D, FontTex()); Col(c);
    glBegin(GL_QUADS);
    float cx = x;
    for (const unsigned char* p = (const unsigned char*)s; *p; ++p) {
        ImFontGlyph* g = b->FindGlyph((ImWchar)*p); if (!g) continue;
        const float x0 = cx + g->X0, y0 = y + g->Y0, x1 = cx + g->X1, y1 = y + g->Y1;   // глиф уже выпечен на px -> без масштаба
        glTexCoord2f(g->U0, g->V0); glVertex2f(x0, y0);
        glTexCoord2f(g->U1, g->V0); glVertex2f(x1, y0);
        glTexCoord2f(g->U1, g->V1); glVertex2f(x1, y1);
        glTexCoord2f(g->U0, g->V1); glVertex2f(x0, y1);
        cx += g->AdvanceX;
    }
    glEnd();
    glDisable(GL_TEXTURE_2D);
}
// Текст в стиле MC: тень со сдвигом + основной цвет.
inline void Text(float x, float y, const char* s, ImU32 c, float px, bool shadow) {
    const float sh = px >= 16.0f ? 2.0f : 1.0f;
    if (shadow) TextRaw(x + sh, y + sh, s, IM_COL32(0, 0, 0, (int)((c >> 24) & 0xff)), px);
    TextRaw(x, y, s, c, px);
}

} // namespace nov

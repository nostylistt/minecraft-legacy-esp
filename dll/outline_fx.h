// outline_fx.h — screen-space обводка силуэта ВСЕЙ модели через offscreen-маску + шейдер (Edge / Glow).
// Модель перерисовывается (её display list'ы) в FBO как сплошная заливка -> маска объединённого силуэта
// (внутренних границ между частями НЕТ). Затем полноэкранный шейдер: Edge = тонкая линия по краю маски,
// Glow = мягкое свечение. Стенсил не нужен. Требует GL 2.0+FBO (грузим через wglGetProcAddress).
#pragma once
#include <windows.h>
#include <GL/gl.h>
#include <cstdio>
#include <vector>

struct OutlinePart { GLuint list; float mv[16]; };   // display list части + её modelview на момент отрисовки

namespace ofx {

// ── GL-константы (нет в старом gl.h) ─────────────────────────────────────────
#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER            0x8D40
#define GL_READ_FRAMEBUFFER       0x8CA8
#define GL_DRAW_FRAMEBUFFER       0x8CA9
#define GL_COLOR_ATTACHMENT0      0x8CE0
#define GL_DEPTH_ATTACHMENT       0x8D00
#define GL_RENDERBUFFER           0x8D41
#define GL_DEPTH_COMPONENT24      0x81A6
#define GL_FRAMEBUFFER_COMPLETE   0x8CD5
#define GL_FRAMEBUFFER_BINDING    0x8CA6
#define GL_FRAGMENT_SHADER        0x8B30
#define GL_VERTEX_SHADER          0x8B31
#define GL_COMPILE_STATUS         0x8B81
#define GL_LINK_STATUS            0x8B82
#define GL_TEXTURE0               0x84C0
#define GL_CLAMP_TO_EDGE          0x812F
#endif
#ifndef GL_ARRAY_BUFFER_BINDING
#define GL_ARRAY_BUFFER_BINDING   0x8894
#endif

typedef char GLchar;
typedef ptrdiff_t GLsizeiptr_;

// ── указатели на GL-функции 2.0/FBO ──────────────────────────────────────────
struct GLX {
    void (APIENTRY* GenFramebuffers)(GLsizei, GLuint*);
    void (APIENTRY* BindFramebuffer)(GLenum, GLuint);
    void (APIENTRY* FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
    void (APIENTRY* FramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint);
    GLenum (APIENTRY* CheckFramebufferStatus)(GLenum);
    void (APIENTRY* DeleteFramebuffers)(GLsizei, const GLuint*);
    void (APIENTRY* GenRenderbuffers)(GLsizei, GLuint*);
    void (APIENTRY* BindRenderbuffer)(GLenum, GLuint);
    void (APIENTRY* RenderbufferStorage)(GLenum, GLenum, GLsizei, GLsizei);
    void (APIENTRY* DeleteRenderbuffers)(GLsizei, const GLuint*);
    void (APIENTRY* BlitFramebuffer)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
    void (APIENTRY* ActiveTexture)(GLenum);
    GLuint (APIENTRY* CreateShader)(GLenum);
    void (APIENTRY* ShaderSource)(GLuint, GLsizei, const GLchar* const*, const GLint*);
    void (APIENTRY* CompileShader)(GLuint);
    void (APIENTRY* GetShaderiv)(GLuint, GLenum, GLint*);
    void (APIENTRY* GetShaderInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*);
    GLuint (APIENTRY* CreateProgram)(void);
    void (APIENTRY* AttachShader)(GLuint, GLuint);
    void (APIENTRY* LinkProgram)(GLuint);
    void (APIENTRY* GetProgramiv)(GLuint, GLenum, GLint*);
    void (APIENTRY* UseProgram)(GLuint);
    GLint (APIENTRY* GetUniformLocation)(GLuint, const GLchar*);
    void (APIENTRY* Uniform1i)(GLint, GLint);
    void (APIENTRY* Uniform1f)(GLint, GLfloat);
    void (APIENTRY* Uniform2f)(GLint, GLfloat, GLfloat);
    void (APIENTRY* Uniform4fv)(GLint, GLsizei, const GLfloat*);
    void (APIENTRY* DeleteShader)(GLuint);
};

inline GLX& X() { static GLX x{}; return x; }
inline char* Err() { static char e[512] = ""; return e; }

template <class T> inline bool Load(T& fn, const char* name) {
    fn = (T)wglGetProcAddress(name);
    return fn != nullptr;
}

inline bool LoadGL() {
    GLX& x = X(); bool ok = true;
    ok &= Load(x.GenFramebuffers, "glGenFramebuffers");
    ok &= Load(x.BindFramebuffer, "glBindFramebuffer");
    ok &= Load(x.FramebufferTexture2D, "glFramebufferTexture2D");
    ok &= Load(x.FramebufferRenderbuffer, "glFramebufferRenderbuffer");
    ok &= Load(x.CheckFramebufferStatus, "glCheckFramebufferStatus");
    ok &= Load(x.DeleteFramebuffers, "glDeleteFramebuffers");
    ok &= Load(x.GenRenderbuffers, "glGenRenderbuffers");
    ok &= Load(x.BindRenderbuffer, "glBindRenderbuffer");
    ok &= Load(x.RenderbufferStorage, "glRenderbufferStorage");
    ok &= Load(x.DeleteRenderbuffers, "glDeleteRenderbuffers");
    ok &= Load(x.BlitFramebuffer, "glBlitFramebuffer");
    ok &= Load(x.ActiveTexture, "glActiveTexture");
    ok &= Load(x.CreateShader, "glCreateShader");
    ok &= Load(x.ShaderSource, "glShaderSource");
    ok &= Load(x.CompileShader, "glCompileShader");
    ok &= Load(x.GetShaderiv, "glGetShaderiv");
    ok &= Load(x.GetShaderInfoLog, "glGetShaderInfoLog");
    ok &= Load(x.CreateProgram, "glCreateProgram");
    ok &= Load(x.AttachShader, "glAttachShader");
    ok &= Load(x.LinkProgram, "glLinkProgram");
    ok &= Load(x.GetProgramiv, "glGetProgramiv");
    ok &= Load(x.UseProgram, "glUseProgram");
    ok &= Load(x.GetUniformLocation, "glGetUniformLocation");
    ok &= Load(x.Uniform1i, "glUniform1i");
    ok &= Load(x.Uniform1f, "glUniform1f");
    ok &= Load(x.Uniform2f, "glUniform2f");
    ok &= Load(x.Uniform4fv, "glUniform4fv");
    ok &= Load(x.DeleteShader, "glDeleteShader");
    if (!ok) snprintf(Err(), 512, "GL2/FBO functions missing via wglGetProcAddress");
    return ok;
}

inline GLuint Compile(GLenum type, const char* src) {
    GLX& x = X();
    GLuint s = x.CreateShader(type);
    x.ShaderSource(s, 1, &src, nullptr);
    x.CompileShader(s);
    GLint ok = 0; x.GetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[300] = ""; x.GetShaderInfoLog(s, 300, nullptr, log); snprintf(Err(), 512, "shader compile: %s", log); return 0; }
    return s;
}

inline GLuint LinkProg(const char* fs) {
    GLX& x = X();
    static const char* vs = "void main(){ gl_TexCoord[0]=gl_MultiTexCoord0; gl_Position=gl_Vertex; }";
    GLuint v = Compile(GL_VERTEX_SHADER, vs); if (!v) return 0;
    GLuint f = Compile(GL_FRAGMENT_SHADER, fs); if (!f) return 0;
    GLuint p = x.CreateProgram(); x.AttachShader(p, v); x.AttachShader(p, f); x.LinkProgram(p);
    GLint ok = 0; x.GetProgramiv(p, GL_LINK_STATUS, &ok);
    x.DeleteShader(v); x.DeleteShader(f);
    if (!ok) { snprintf(Err(), 512, "program link failed"); return 0; }
    return p;
}

// Glow: мягкое свечение вокруг силуэта (спад по радиусу).
inline const char* FS_GLOW() { return
    "uniform sampler2D mask; uniform vec2 texel; uniform vec4 col; uniform float spread;\n"
    "void main(){\n"
    "  float c=texture2D(mask,gl_TexCoord[0].st).r;\n"
    "  float g=0.0; const int R=6;\n"
    "  for(int y=-R;y<=R;y++) for(int x=-R;x<=R;x++){\n"
    "    float w=1.0-length(vec2(float(x),float(y)))/float(R+1); if(w<0.0)w=0.0;\n"
    "    g=max(g, texture2D(mask, gl_TexCoord[0].st+vec2(float(x),float(y))*texel*spread).r*w);\n"
    "  }\n"
    "  float a=g*(1.0-c);\n"                               // не заливаем внутренность
    "  if(a<0.02) discard;\n"
    "  gl_FragColor=vec4(col.rgb, col.a*a);\n"
    "}\n"; }

struct State {
    bool  ready = false, failed = false;
    GLuint fbo = 0, tex = 0, rbo = 0;
    int   w = 0, h = 0;
    GLuint pGlow = 0;
};
inline State& S() { static State s; return s; }

inline bool EnsureInit() {
    State& s = S();
    if (s.ready) return true;
    if (s.failed) return false;
    if (!LoadGL()) { s.failed = true; return false; }
    s.pGlow = LinkProg(FS_GLOW()); if (!s.pGlow) { s.failed = true; return false; }
    s.ready = true;
    return true;
}

inline bool EnsureFBO(int w, int h) {
    State& s = S(); GLX& x = X();
    if (s.fbo && s.w == w && s.h == h) return true;
    if (s.fbo) { x.DeleteFramebuffers(1, &s.fbo); glDeleteTextures(1, &s.tex); x.DeleteRenderbuffers(1, &s.rbo); s.fbo = s.tex = s.rbo = 0; }
    GLint prevTex = 0; glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
    x.GenFramebuffers(1, &s.fbo); x.BindFramebuffer(GL_FRAMEBUFFER, s.fbo);
    glGenTextures(1, &s.tex); glBindTexture(GL_TEXTURE_2D, s.tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    x.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s.tex, 0);
    x.GenRenderbuffers(1, &s.rbo); x.BindRenderbuffer(GL_RENDERBUFFER, s.rbo);
    x.RenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
    x.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, s.rbo);
    GLenum st = x.CheckFramebufferStatus(GL_FRAMEBUFFER);
    x.BindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, prevTex);
    if (st != GL_FRAMEBUFFER_COMPLETE) { snprintf(Err(), 512, "FBO incomplete: 0x%X", st); s.failed = true; return false; }
    s.w = w; s.h = h;
    return true;
}

// Главный вход: рисует свечение силуэта в ТЕКУЩИЙ (default) фреймбуфер.
// callList — оригинальный glCallList (трамплин), для перерисовки списков в маску.
inline void Render(const float* proj, const OutlinePart* parts, int nParts,
                   void (APIENTRY* callList)(GLuint), int w, int h,
                   const float col[4], float spread, bool depthOcclude) {
    if (nParts <= 0 || !callList || w <= 0 || h <= 0) return;
    if (!EnsureInit() || !EnsureFBO(w, h)) return;
    State& s = S(); GLX& x = X();

    GLint prevFbo = 0; glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    GLint vp[4]; glGetIntegerv(GL_VIEWPORT, vp);
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glMatrixMode(GL_PROJECTION); glPushMatrix();
    glMatrixMode(GL_MODELVIEW);  glPushMatrix();

    // ── 1. маска: перерисовать модель сплошной заливкой в FBO ──
    x.BindFramebuffer(GL_FRAMEBUFFER, s.fbo);
    glViewport(0, 0, w, h);
    glClearColor(0, 0, 0, 0); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    if (depthOcclude) {   // скопировать глубину сцены -> маска обрежется видимой частью модели
        x.BindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)prevFbo);
        x.BindFramebuffer(GL_DRAW_FRAMEBUFFER, s.fbo);
        x.BlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
        x.BindFramebuffer(GL_FRAMEBUFFER, s.fbo);
        glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LEQUAL); glDepthMask(GL_FALSE);
    } else { glDisable(GL_DEPTH_TEST); }
    glDisable(GL_TEXTURE_2D); glDisable(GL_LIGHTING); glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE); glDisable(GL_ALPHA_TEST);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glColor4f(1, 1, 1, 1);
    glMatrixMode(GL_PROJECTION); glLoadMatrixf(proj);
    glMatrixMode(GL_MODELVIEW);
    for (int i = 0; i < nParts; ++i) { glLoadMatrixf(parts[i].mv); callList(parts[i].list); }

    // ── 2. композит: полноэкранный шейдер поверх сцены ──
    x.BindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFbo);
    glViewport(vp[0], vp[1], vp[2], vp[3]);
    glDisable(GL_DEPTH_TEST); glDepthMask(GL_FALSE);
    glDisable(GL_LIGHTING); glDisable(GL_CULL_FACE); glDisable(GL_ALPHA_TEST);
    glEnable(GL_TEXTURE_2D);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    x.ActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, s.tex);
    const GLuint prog = s.pGlow;
    x.UseProgram(prog);
    x.Uniform1i(x.GetUniformLocation(prog, "mask"), 0);
    x.Uniform2f(x.GetUniformLocation(prog, "texel"), 1.0f / w, 1.0f / h);
    x.Uniform1f(x.GetUniformLocation(prog, "spread"), spread);
    x.Uniform4fv(x.GetUniformLocation(prog, "col"), 1, col);
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);  glLoadIdentity();
    glBegin(GL_QUADS);
        glTexCoord2f(0, 0); glVertex2f(-1, -1);
        glTexCoord2f(1, 0); glVertex2f( 1, -1);
        glTexCoord2f(1, 1); glVertex2f( 1,  1);
        glTexCoord2f(0, 1); glVertex2f(-1,  1);
    glEnd();
    x.UseProgram(0);

    // ── restore ──
    glMatrixMode(GL_PROJECTION); glPopMatrix();
    glMatrixMode(GL_MODELVIEW);  glPopMatrix();
    glPopAttrib();
    x.BindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFbo);
    glViewport(vp[0], vp[1], vp[2], vp[3]);
}

} // namespace ofx

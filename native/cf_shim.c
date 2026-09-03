/* cf_shim.c — the entire unsafe surface of cube_forge.
 *
 * Responsibilities, and nothing else:
 *   - create a window + GL 3.3 core context (GLFW 3.4 API surface only)
 *   - poll events into a flat struct; expose the fields via scalar getters
 *   - accept a flat float buffer from March and upload it as a VBO
 *   - issue draw calls, swap
 *
 * No game logic, no voxel knowledge, no allocation policy: every buffer that
 * crosses this boundary is owned by March (a NativeF32Arr / NativeU8Arr whose
 * payload we read for the duration of the call only).
 *
 * GL usage is restricted to the GL 3.3 core / GLES 3.0 intersection:
 * VAOs, VBOs, glBufferData, 2D texture arrays, plain uniforms, glDrawArrays.
 */
#include "march_ffi.h"
#include "glad/gl.h"
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>

/* ── NativeArray payload access ──────────────────────────────────────────────
 * March's NativeArray heap layout (runtime/march_runtime.c, NATIVE_ARR_HDR):
 *   { rc i64; tag i32; pad i32; len i64 @16; elem_kind u8 @24; pad; data @32 }
 * There is no march_ffi.h accessor for this yet (GAPS.md), so the shim reads it
 * directly. Values arrive as the raw heap pointer (verbatim, borrowed).      */
#define NARR_HDR 32
static inline int64_t narr_len(void *a)  { return *(int64_t *)((char *)a + 16); }
static inline void   *narr_data(void *a) { return (char *)a + NARR_HDR; }

/* ── Window state ──────────────────────────────────────────────────────────── */
static GLFWwindow *g_win = NULL;
static int g_fb_w = 0, g_fb_h = 0;

/* ── Input state: one flat struct, filled by GLFW callbacks ────────────────── */
#define CF_MAX_KEYS 512
typedef struct {
    double mouse_x, mouse_y;        /* latest cursor position */
    double mouse_dx, mouse_dy;      /* accumulated since last poll */
    unsigned char keys[CF_MAX_KEYS];
    unsigned char buttons[8];
    unsigned char buttons_pressed[8];   /* edge: went down since last poll */
    double scroll_dy;
} cf_input;
static cf_input g_in;
static int g_have_mouse = 0;

static void on_key(GLFWwindow *w, int key, int sc, int action, int mods) {
    (void)w; (void)sc; (void)mods;
    if (key < 0 || key >= CF_MAX_KEYS) return;
    if (action == GLFW_PRESS) g_in.keys[key] = 1;
    else if (action == GLFW_RELEASE) g_in.keys[key] = 0;
}
static void on_cursor(GLFWwindow *w, double x, double y) {
    (void)w;
    if (g_have_mouse) { g_in.mouse_dx += x - g_in.mouse_x; g_in.mouse_dy += y - g_in.mouse_y; }
    g_in.mouse_x = x; g_in.mouse_y = y; g_have_mouse = 1;
}
static void on_button(GLFWwindow *w, int b, int action, int mods) {
    (void)w; (void)mods;
    if (b < 0 || b >= 8) return;
    if (action == GLFW_PRESS) { g_in.buttons[b] = 1; g_in.buttons_pressed[b] = 1; }
    else if (action == GLFW_RELEASE) g_in.buttons[b] = 0;
}
static void on_scroll(GLFWwindow *w, double dx, double dy) { (void)w; (void)dx; g_in.scroll_dy += dy; }
static void on_fb_size(GLFWwindow *w, int width, int height) {
    (void)w; g_fb_w = width; g_fb_h = height; glViewport(0, 0, width, height);
}

/* ═══════════════════════════════ WINDOW DOMAIN ═══════════════════════════════ */

int64_t cf_win_open(int64_t w, int64_t h, march_value title) {
    if (!pthread_main_np()) {
        fprintf(stderr, "cf: cf_win_open must run on the process main thread (GLFW/Cocoa requirement). "
                        "Run with MARCH_PIN_MAIN=1 (pins `main` to scheduler 0 on the main thread; "
                        "needs the runtime/pin-main-thread March runtime, see GAPS.md G15) "
                        "or MARCH_NUM_SCHEDULERS=1 (serialises pmap).\n");
        return 0;
    }
    if (!glfwInit()) { fprintf(stderr, "cf: glfwInit failed\n"); return 0; }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    march_slice t = march_str_borrow(title);
    char buf[256]; size_t n = t.len < 255 ? t.len : 255;
    memcpy(buf, t.ptr, n); buf[n] = 0;
    g_win = glfwCreateWindow((int)w, (int)h, buf, NULL, NULL);
    if (!g_win) { fprintf(stderr, "cf: glfwCreateWindow failed\n"); glfwTerminate(); return 0; }
    glfwMakeContextCurrent(g_win);
    if (!gladLoadGL(glfwGetProcAddress)) { fprintf(stderr, "cf: gladLoadGL failed\n"); return 0; }
    glfwSwapInterval(1);
    glfwGetFramebufferSize(g_win, &g_fb_w, &g_fb_h);
    glViewport(0, 0, g_fb_w, g_fb_h);
    glfwSetFramebufferSizeCallback(g_win, on_fb_size);
    glfwSetKeyCallback(g_win, on_key);
    glfwSetCursorPosCallback(g_win, on_cursor);
    glfwSetMouseButtonCallback(g_win, on_button);
    glfwSetScrollCallback(g_win, on_scroll);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    return 1;
}
int64_t cf_win_should_close(void) { return g_win && glfwWindowShouldClose(g_win); }
void    cf_win_swap(void)         { if (g_win) glfwSwapBuffers(g_win); }
double  cf_win_time(void)         { return glfwGetTime(); }
int64_t cf_win_fb_w(void)         { return g_fb_w; }
int64_t cf_win_fb_h(void)         { return g_fb_h; }
void    cf_win_close(void)        { if (g_win) { glfwDestroyWindow(g_win); g_win = NULL; } glfwTerminate(); }
void    cf_win_request_close(void){ if (g_win) glfwSetWindowShouldClose(g_win, 1); }

/* ── GL: shader program + one VAO shared by every mesh ──────────────────────
 * Vertex layout (7 floats): pos.xyz, uv, layer, shade.                         */
#define CF_VERT_FLOATS 7
static GLuint g_prog = 0, g_vao = 0;
static GLint  g_u_vp = -1, g_u_tex = -1;

static const char *VS =
    "#version 330 core\n"
    "layout(location=0) in vec3 a_pos;\n"
    "layout(location=1) in vec2 a_uv;\n"
    "layout(location=2) in float a_layer;\n"
    "layout(location=3) in float a_shade;\n"
    "uniform mat4 u_vp;\n"
    "out vec2 v_uv; out float v_layer; out float v_shade;\n"
    "void main(){ gl_Position = u_vp * vec4(a_pos,1.0); v_uv=a_uv; v_layer=a_layer; v_shade=a_shade; }\n";
static const char *FS =
    "#version 330 core\n"
    "in vec2 v_uv; in float v_layer; in float v_shade;\n"
    "uniform sampler2DArray u_tex;\n"
    "uniform int u_use_tex;\n"
    "out vec4 o_color;\n"
    "void main(){\n"
    "  vec4 t = (u_use_tex == 1) ? texture(u_tex, vec3(v_uv, v_layer)) : vec4(v_uv, v_layer, 1.0);\n"
    "  o_color = vec4(t.rgb * v_shade, t.a);\n"
    "}\n";

static GLuint compile(GLenum kind, const char *src) {
    GLuint s = glCreateShader(kind);
    glShaderSource(s, 1, &src, NULL); glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[2048]; glGetShaderInfoLog(s, sizeof log, NULL, log); fprintf(stderr, "cf: shader: %s\n", log); }
    return s;
}

#define CF_MAX_MESHES 256
static GLuint g_vbo[CF_MAX_MESHES];
static GLint  g_u_use_tex = -1;
static GLuint g_tex = 0;

int64_t cf_gfx_init(void) {
    GLuint vs = compile(GL_VERTEX_SHADER, VS), fs = compile(GL_FRAGMENT_SHADER, FS);
    g_prog = glCreateProgram();
    glAttachShader(g_prog, vs); glAttachShader(g_prog, fs); glLinkProgram(g_prog);
    GLint ok = 0; glGetProgramiv(g_prog, GL_LINK_STATUS, &ok);
    if (!ok) { char log[2048]; glGetProgramInfoLog(g_prog, sizeof log, NULL, log); fprintf(stderr, "cf: link: %s\n", log); return 0; }
    glDeleteShader(vs); glDeleteShader(fs);
    g_u_vp = glGetUniformLocation(g_prog, "u_vp");
    g_u_tex = glGetUniformLocation(g_prog, "u_tex");
    g_u_use_tex = glGetUniformLocation(g_prog, "u_use_tex");
    glGenVertexArrays(1, &g_vao);
    glGenBuffers(CF_MAX_MESHES, g_vbo);
    glUseProgram(g_prog);
    glUniform1i(g_u_tex, 0);
    glUniform1i(g_u_use_tex, 0);
    return 1;
}

/* Upload the first `nfloats` floats of a March NativeF32Arr into mesh slot `slot`. */
void cf_gfx_upload(int64_t slot, void *arr, int64_t nfloats) {
    if (slot < 0 || slot >= CF_MAX_MESHES) return;
    if (nfloats > narr_len(arr)) nfloats = narr_len(arr);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo[slot]);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(nfloats * 4), narr_data(arr), GL_STATIC_DRAW);
    if (getenv("CF_DEBUG")) { const float *f = narr_data(arr); fprintf(stderr, "cf: upload slot=%lld nfloats=%lld arrlen=%lld first=%g %g %g %g %g %g %g glerr=%d\n", (long long)slot, (long long)nfloats, (long long)narr_len(arr), f[0],f[1],f[2],f[3],f[4],f[5],f[6], (int)glGetError()); }
}

void cf_gfx_begin_frame(double r, double g, double b) {
    glClearColor((float)r, (float)g, (float)b, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glUseProgram(g_prog);
    glBindVertexArray(g_vao);
}

/* 16 floats, column-major, from a NativeF32Arr. */
void cf_gfx_set_view_proj(void *arr) {
    if (narr_len(arr) < 16) return;
    glUniformMatrix4fv(g_u_vp, 1, GL_FALSE, (const float *)narr_data(arr));
    if (getenv("CF_DEBUG")) { const float *f = narr_data(arr); fprintf(stderr, "cf: vp loc=%d diag=%g %g %g %g glerr=%d\n", g_u_vp, f[0], f[5], f[10], f[15], (int)glGetError()); }
}

void cf_gfx_draw(int64_t slot, int64_t nverts) {
    if (slot < 0 || slot >= CF_MAX_MESHES || nverts <= 0) return;
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo[slot]);
    GLsizei stride = CF_VERT_FLOATS * sizeof(float);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void *)0);
    glEnableVertexAttribArray(1); glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, (void *)(3 * 4));
    glEnableVertexAttribArray(2); glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, stride, (void *)(5 * 4));
    glEnableVertexAttribArray(3); glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, stride, (void *)(6 * 4));
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)nverts);
    if (getenv("CF_DEBUG")) { fprintf(stderr, "cf: draw slot=%lld nverts=%lld glerr=%d prog=%u vao=%u\n", (long long)slot, (long long)nverts, (int)glGetError(), g_prog, g_vao); }
}

/* Upload an RGBA8 texture array (layer-major, row-major, 4 bytes/texel) from a
 * March NativeU8Arr. Nearest filtering: voxel look, no mipmaps. */
void cf_gfx_upload_texture(void *arr, int64_t w, int64_t h, int64_t layers) {
    if (narr_len(arr) < w * h * layers * 4) { fprintf(stderr, "cf: texture array too small\n"); return; }
    if (!g_tex) glGenTextures(1, &g_tex);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D_ARRAY, g_tex);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, (GLsizei)w, (GLsizei)h, (GLsizei)layers, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, narr_data(arr));
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glUseProgram(g_prog);
    glUniform1i(g_u_use_tex, 1);
}

/* Translucent pass: blend, keep depth test, no depth writes, no culling (water
 * surface visible from below). Call after every opaque draw. */
void cf_gfx_draw_translucent(int64_t slot, int64_t nverts) {
    if (slot < 0 || slot >= CF_MAX_MESHES || nverts <= 0) return;
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    cf_gfx_draw(slot, nverts);
    glEnable(GL_CULL_FACE);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
}

/* Draw a mesh slot as GL_LINES with the current view-projection, untextured,
 * depth test off so a selection outline is never hidden by the face it sits on. */
void cf_gfx_draw_lines(int64_t slot, int64_t nverts) {
    if (slot < 0 || slot >= CF_MAX_MESHES || nverts <= 0) return;
    glDisable(GL_DEPTH_TEST);
    glUniform1i(g_u_use_tex, 0);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo[slot]);
    GLsizei stride = CF_VERT_FLOATS * sizeof(float);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void *)0);
    glEnableVertexAttribArray(1); glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, (void *)(3 * 4));
    glEnableVertexAttribArray(2); glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, stride, (void *)(5 * 4));
    glEnableVertexAttribArray(3); glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, stride, (void *)(6 * 4));
    glDrawArrays(GL_LINES, 0, (GLsizei)nverts);
    glUniform1i(g_u_use_tex, g_tex ? 1 : 0);
    glEnable(GL_DEPTH_TEST);
}

/* Draw a screen-space overlay mesh (NDC coordinates, same 7-float layout):
 * identity view-projection, no texture, no depth test. Restores state after. */
void cf_gfx_draw_hud(int64_t slot, int64_t nverts, int64_t textured) {
    static const float ident[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    if (slot < 0 || slot >= CF_MAX_MESHES || nverts <= 0) return;
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUniformMatrix4fv(g_u_vp, 1, GL_FALSE, ident);
    glUniform1i(g_u_use_tex, (textured && g_tex) ? 1 : 0);
    cf_gfx_draw(slot, nverts);
    glUniform1i(g_u_use_tex, g_tex ? 1 : 0);
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
}

/* Debug/verification hook: read back one pixel of the back buffer as 0xRRGGBB.
 * Call after drawing and before swap. */
int64_t cf_gfx_read_pixel(int64_t x, int64_t y) {
    unsigned char px[4] = {0, 0, 0, 0};
    glReadPixels((GLint)x, (GLint)y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    return ((int64_t)px[0] << 16) | ((int64_t)px[1] << 8) | (int64_t)px[2];
}

/* Debug/verification hook: dump the back buffer to an uncompressed 24-bit BMP.
 * Call after drawing and before swap. Returns 1 on success. */
int64_t cf_gfx_dump_bmp(march_value path) {
    march_slice t = march_str_borrow(path);
    char name[512]; size_t n = t.len < 511 ? t.len : 511; memcpy(name, t.ptr, n); name[n] = 0;
    int w = g_fb_w, h = g_fb_h;
    unsigned char *px = (unsigned char *)malloc((size_t)w * h * 3);
    if (!px) return 0;
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, px);
    FILE *f = fopen(name, "wb"); if (!f) { free(px); return 0; }
    int row = (w * 3 + 3) & ~3; int size = 54 + row * h;
    unsigned char hdr[54] = {'B','M', size, size>>8, size>>16, size>>24, 0,0,0,0, 54,0,0,0, 40,0,0,0,
        w, w>>8, w>>16, w>>24, h, h>>8, h>>16, h>>24, 1,0, 24,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0};
    fwrite(hdr, 1, 54, f);
    unsigned char pad[3] = {0,0,0};
    for (int y = 0; y < h; y++) {           /* BMP is bottom-up, same as glReadPixels */
        for (int x = 0; x < w; x++) { unsigned char *p = px + (y * w + x) * 3; unsigned char bgr[3] = {p[2], p[1], p[0]}; fwrite(bgr, 1, 3, f); }
        fwrite(pad, 1, row - w * 3, f);
    }
    fclose(f); free(px);
    return 1;
}

/* ═══════════════════════════════ INPUT DOMAIN ════════════════════════════════ */

void cf_in_poll(void) {
    g_in.mouse_dx = 0; g_in.mouse_dy = 0; g_in.scroll_dy = 0;
    memset(g_in.buttons_pressed, 0, sizeof g_in.buttons_pressed);
    glfwPollEvents();
}
int64_t cf_in_key(int64_t k)          { return (k >= 0 && k < CF_MAX_KEYS) ? g_in.keys[k] : 0; }
int64_t cf_in_button(int64_t b)       { return (b >= 0 && b < 8) ? g_in.buttons[b] : 0; }
int64_t cf_in_button_pressed(int64_t b){ return (b >= 0 && b < 8) ? g_in.buttons_pressed[b] : 0; }
double  cf_in_mouse_dx(void)          { return g_in.mouse_dx; }
double  cf_in_mouse_dy(void)          { return g_in.mouse_dy; }
double  cf_in_scroll_dy(void)         { return g_in.scroll_dy; }
void    cf_in_capture_cursor(int64_t on) {
    if (g_win) glfwSetInputMode(g_win, GLFW_CURSOR, on ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
}

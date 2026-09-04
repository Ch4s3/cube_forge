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
#include <math.h>
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
    unsigned char keys_pressed[CF_MAX_KEYS];   /* edge: went down since last poll */
    unsigned char buttons[8];
    unsigned char buttons_pressed[8];   /* edge: went down since last poll */
    double scroll_dy;
} cf_input;
static cf_input g_in;
static int g_have_mouse = 0;

static void on_key(GLFWwindow *w, int key, int sc, int action, int mods) {
    (void)w; (void)sc; (void)mods;
    if (key < 0 || key >= CF_MAX_KEYS) return;
    if (action == GLFW_PRESS) { g_in.keys[key] = 1; g_in.keys_pressed[key] = 1; }
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
 * Vertex layout (8 floats): pos.xyz, uv, layer, shade, face.
 * `face` is 0..5 (+y -y +x -x +z -z); the vertex shader turns it into a normal
 * for the flashlight and into the directional multiplier that used to be baked
 * into `shade` by the mesher.                                                  */
#define CF_VERT_FLOATS 8
static GLuint g_prog = 0, g_vao = 0;
static GLint  g_u_vp = -1, g_u_tex = -1;

static const char *VS =
    "#version 330 core\n"
    "layout(location=0) in vec3 a_pos;\n"
    "layout(location=1) in vec2 a_uv;\n"
    "layout(location=2) in float a_layer;\n"
    "layout(location=3) in float a_shade;\n"
    "layout(location=4) in float a_face;\n"
    "uniform mat4 u_vp;\n"
    "out vec2 v_uv; out float v_layer; out float v_shade;\n"
    "out vec3 v_world; out vec3 v_normal;\n"
    "const vec3 NORMALS[6] = vec3[6](vec3(0,1,0), vec3(0,-1,0), vec3(1,0,0), vec3(-1,0,0), vec3(0,0,1), vec3(0,0,-1));\n"
    "void main(){\n"
    "  int f = int(a_face + 0.5);\n"
    "  gl_Position = u_vp * vec4(a_pos,1.0);\n"
    "  v_uv=a_uv; v_layer=a_layer;\n"
    /* v_shade is now skylight x AO only. The per-face directional constant that
     * used to be folded in here is replaced by a real N.L against a sun that
     * moves, computed per fragment. */
    "  v_shade = a_shade;\n"
    "  v_world = a_pos; v_normal = NORMALS[f];\n"
    "}\n";
static const char *FS =
    "#version 330 core\n"
    "in vec2 v_uv; in float v_layer; in float v_shade;\n"
    "in vec3 v_world; in vec3 v_normal;\n"
    "uniform sampler2DArray u_tex;\n"
    "uniform int u_use_tex;\n"
    "uniform float u_sun;\n"
    "uniform vec3 u_eye;\n"
    "uniform vec3 u_dir;\n"
    "uniform float u_flash;\n"
    "uniform vec3 u_sundir;\n"
    "uniform vec3 u_moondir;\n"
    "uniform int u_unlit;\n"
    "uniform int u_cutout;\n"
    "uniform sampler3D u_occ;\n"
    "uniform float u_shadow;\n"
    "out vec4 o_color;\n"
    /* The beam shape never varies at runtime, only its position, aim and
     * on/off state, so the cone half-angles are constants rather than uniforms. */
    "const float COS_INNER = 0.97;\n"
    "const float COS_OUTER = 0.88;\n"
    /* Moonlight: cool and dim, ramping in as the sun sets and gone shortly after
     * it rises. It is multiplied by v_shade like sunlight is, so it reaches only
     * sky-exposed surfaces — a sealed cave stays black at night and still needs
     * the flashlight. MOON_LEVEL is the fraction of full daylight. */
    "const vec3  MOON_TINT  = vec3(0.60, 0.72, 1.00);\n"
    "const float MOON_LEVEL = 0.13;\n"
    "const float MOON_UNTIL = 0.25;\n"
    /* Sunlight reddens as it nears the horizon. SUN_WARM is dawn/dusk, SUN_WHITE
     * is high noon; u_sun doubles as the sun's height, so it drives the blend. */
    "const vec3  SUN_WARM  = vec3(1.00, 0.62, 0.35);\n"
    "const vec3  SUN_WHITE = vec3(1.00, 0.98, 0.94);\n"
    /* Ambient floor so a face turned away from the sun keeps its shape instead
     * of going flat black; the up-bias stands in for sky versus ground bounce.
     * AMB + DIRECT <= 1 keeps a lit top face from clipping at noon. */
    /* The sun's HEIGHT (u_sun) is not its intensity: it stays bright until it is
     * nearly down, and it is the angle that makes surfaces dim. Conflating the
     * two made the whole world fade as cos(theta) and swallowed golden hour. */
    "const float SET_AT = 0.22;\n"
    "const float AMB    = 0.30;\n"
    "const float AMB_UP = 0.08;\n"
    "const float DIRECT = 0.62;\n"
    /* World dimensions, for turning voxel coordinates into texture coordinates
     * and for knowing when a ray has left the world. */
    "const vec3 WORLD = vec3(128.0, 256.0, 128.0);\n"
    "const int  MAX_STEPS = 256;\n"
    /* Amanatides-Woo voxel DDA. Returns 1.0 when the ray reaches maxDist without
     * hitting an occluder, 0.0 when something blocks it. Exact on axis-aligned
     * voxels: no depth bias, no acne, no peter-panning. */
    "float trace(vec3 p, vec3 dir, float maxDist){\n"
    "  if (u_shadow <= 0.0) return 1.0;\n"
    "  ivec3 v   = ivec3(floor(p));\n"
    "  ivec3 stp = ivec3(sign(dir));\n"
    /* An axis the ray does not move along must never step: its tMax stays at
     * infinity so min() never selects it. */
    "  bvec3 moving = greaterThan(abs(dir), vec3(1e-8));\n"
    "  vec3  den    = mix(vec3(1.0), dir, moving);\n"
    /* Distance to the next voxel boundary per axis. step(0,dir) picks the far
     * face heading positive and the near face heading negative. */
    "  vec3  tMax   = mix(vec3(1e30), (vec3(v) + step(0.0, dir) - p) / den, moving);\n"
    "  vec3  tDelta = mix(vec3(1e30), 1.0 / abs(den), moving);\n"
    "  for (int i = 0; i < MAX_STEPS; i++){\n"
    "    float tNow = min(tMax.x, min(tMax.y, tMax.z));\n"
    "    if (tNow > maxDist) return 1.0;\n"
    "    if (tMax.x <= tMax.y && tMax.x <= tMax.z)      { v.x += stp.x; tMax.x += tDelta.x; }\n"
    "    else if (tMax.y <= tMax.z)                     { v.y += stp.y; tMax.y += tDelta.y; }\n"
    "    else                                           { v.z += stp.z; tMax.z += tDelta.z; }\n"
    /* Leaving the world sideways or out of the top means the ray escaped; out
     * of the bottom cannot happen for a light above, but is treated the same. */
    "    if (v.x < 0 || v.y < 0 || v.z < 0 || v.x >= int(WORLD.x) || v.y >= int(WORLD.y) || v.z >= int(WORLD.z)) return 1.0;\n"
    "    if (texture(u_occ, (vec3(v) + 0.5) / WORLD).r > 0.5) return 0.0;\n"
    "  }\n"
    "  return 1.0;\n"
    "}\n"
    "void main(){\n"
    "  vec4 t = (u_use_tex == 1) ? texture(u_tex, vec3(v_uv, v_layer)) : vec4(v_uv, v_layer, 1.0);\n"
    "  if (u_cutout == 1 && t.a < 0.5) discard;\n"
    "  float moon = clamp((MOON_UNTIL - u_sun) / MOON_UNTIL, 0.0, 1.0);\n"
    "  float amb  = AMB + AMB_UP * v_normal.y;\n"
    "  float ndls = max(dot(v_normal, u_sundir),  0.0);\n"
    "  float ndlm = max(dot(v_normal, u_moondir), 0.0);\n"
    "  vec3  sunc = mix(SUN_WARM, SUN_WHITE, smoothstep(0.0, 0.50, u_sun));\n"
    "  float inten = clamp(u_sun / SET_AT, 0.0, 1.0);\n"
    /* One directional trace, not two: the sun and moon are opposite ends of one
     * arc, so only one is ever above the horizon. Skipped entirely when the
     * face turns away from the light, when the light is out, or when the baked
     * skylight already says this surface is sealed in the dark. */
    "  vec3  origin = v_world + v_normal * 0.5;\n"
    "  float shad = 1.0;\n"
    "  if (v_shade > 0.001) {\n"
    "    if (inten > 0.0 && ndls > 0.0)      shad = trace(origin, u_sundir,  u_shadow);\n"
    "    else if (moon > 0.0 && ndlm > 0.0)  shad = trace(origin, u_moondir, u_shadow);\n"
    "  }\n"
    "  vec3  sky  = sunc * (inten * (amb + DIRECT * ndls * shad))\n"
    "             + MOON_TINT * (MOON_LEVEL * moon * (amb + DIRECT * ndlm * shad));\n"
    "  vec3  baked = v_shade * sky;\n"
    "  vec3  L  = u_eye - v_world;\n"
    "  float d2 = dot(L, L);\n"
    "  vec3  Ln = L * inversesqrt(max(d2, 1e-6));\n"
    "  float spot  = smoothstep(COS_OUTER, COS_INNER, dot(-Ln, u_dir));\n"
    "  float flash = u_flash * spot * max(dot(v_normal, Ln), 0.0) / (1.0 + 0.02 * d2);\n"
    /* The flashlight gets its own trace, toward the eye, and only when it would
     * contribute anything at all. */
    "  if (flash > 0.001) flash *= trace(origin, Ln, min(sqrt(d2), u_shadow));\n"
    "  vec3  world = baked + vec3(flash);\n"
    /* Overlays (HUD, outline, map marker) share this program but are not part of
     * the world: they keep their own vertex shade and skip lighting entirely. */
    "  o_color = vec4(t.rgb * ((u_unlit == 1) ? vec3(v_shade) : world), t.a);\n"
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
static GLint  g_u_cutout = -1;
static GLint  g_u_sun = -1, g_u_eye = -1, g_u_dir = -1, g_u_flash = -1;
static GLint  g_u_sundir = -1, g_u_moondir = -1, g_u_unlit = -1;
static GLint  g_u_occ = -1, g_u_shadow = -1;
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
    g_u_cutout = glGetUniformLocation(g_prog, "u_cutout");
    g_u_sun = glGetUniformLocation(g_prog, "u_sun");
    g_u_eye = glGetUniformLocation(g_prog, "u_eye");
    g_u_dir = glGetUniformLocation(g_prog, "u_dir");
    g_u_flash = glGetUniformLocation(g_prog, "u_flash");
    g_u_sundir = glGetUniformLocation(g_prog, "u_sundir");
    g_u_moondir = glGetUniformLocation(g_prog, "u_moondir");
    g_u_unlit = glGetUniformLocation(g_prog, "u_unlit");
    g_u_occ = glGetUniformLocation(g_prog, "u_occ");
    g_u_shadow = glGetUniformLocation(g_prog, "u_shadow");
    if (getenv("CF_DEBUG")) fprintf(stderr, "cf: uniforms occ=%d shadow=%d sundir=%d unlit=%d\n", g_u_occ, g_u_shadow, g_u_sundir, g_u_unlit);
    glGenVertexArrays(1, &g_vao);
    glGenBuffers(CF_MAX_MESHES, g_vbo);
    glUseProgram(g_prog);
    glUniform1i(g_u_tex, 0);
    glUniform1i(g_u_use_tex, 0);
    glUniform1i(g_u_cutout, 0);
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

/* Assemble a VBO from several March buffers: reserve `nfloats` floats, then
 * copy parts at float offsets. GL 3.3 core / GLES 3.0: glBufferData(NULL) +
 * glBufferSubData. */
void cf_gfx_upload_begin(int64_t slot, int64_t nfloats) {
    if (slot < 0 || slot >= CF_MAX_MESHES) return;
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo[slot]);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(nfloats * 4), NULL, GL_STATIC_DRAW);
}
void cf_gfx_upload_part(int64_t slot, int64_t offset, void *arr, int64_t nfloats) {
    if (slot < 0 || slot >= CF_MAX_MESHES || nfloats <= 0) return;
    if (nfloats > narr_len(arr)) nfloats = narr_len(arr);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo[slot]);
    glBufferSubData(GL_ARRAY_BUFFER, (GLintptr)(offset * 4), (GLsizeiptr)(nfloats * 4), narr_data(arr));
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
    glEnableVertexAttribArray(4); glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, stride, (void *)(7 * 4));
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

/* ── Occupancy field: the shadow ray-marcher's view of the world ─────────────
 * One byte per voxel, 1 where a block occludes light, as a GL_TEXTURE_3D on
 * unit 1 (unit 0 is the block texture array). NEAREST and CLAMP_TO_EDGE: this
 * is a lookup, not a filtered value, and a ray leaving the world must read the
 * edge rather than wrap into the far side. */
static GLuint g_occ = 0;
static int64_t g_occ_w = 0, g_occ_h = 0, g_occ_d = 0;

void cf_gfx_upload_occupancy(void *arr, int64_t w, int64_t h, int64_t d) {
    if (narr_len(arr) < w * h * d) { fprintf(stderr, "cf: occupancy array too small\n"); return; }
    GLint maxdim = 0;
    glGetIntegerv(GL_MAX_3D_TEXTURE_SIZE, &maxdim);
    /* GL 3.3 only guarantees 256, and the world's Y is exactly 256. Any real GPU
     * is far above this, but rendering garbage silently is not acceptable. */
    if (w > maxdim || h > maxdim || d > maxdim) {
        fprintf(stderr, "cf: occupancy %lldx%lldx%lld exceeds GL_MAX_3D_TEXTURE_SIZE %d; shadows disabled\n",
                (long long)w, (long long)h, (long long)d, (int)maxdim);
        return;
    }
    if (!g_occ) glGenTextures(1, &g_occ);
    g_occ_w = w; g_occ_h = h; g_occ_d = d;
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_3D, g_occ);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage3D(GL_TEXTURE_3D, 0, GL_R8, (GLsizei)w, (GLsizei)h, (GLsizei)d, 0,
                 GL_RED, GL_UNSIGNED_BYTE, narr_data(arr));
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    glActiveTexture(GL_TEXTURE0);
    glUseProgram(g_prog);
    glUniform1i(g_u_occ, 1);
}

/* One voxel changed: a single texel beats rebuilding 4 MB. */
void cf_gfx_set_voxel(int64_t x, int64_t y, int64_t z, int64_t solid) {
    if (!g_occ) return;
    if (x < 0 || y < 0 || z < 0 || x >= g_occ_w || y >= g_occ_h || z >= g_occ_d) return;
    unsigned char v = solid ? 255 : 0;   /* normalized R8: 255 samples as 1.0 */
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_3D, g_occ);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage3D(GL_TEXTURE_3D, 0, (GLint)x, (GLint)y, (GLint)z, 1, 1, 1,
                    GL_RED, GL_UNSIGNED_BYTE, &v);
    glActiveTexture(GL_TEXTURE0);
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

/* Foliage: alpha-cutout. Depth test and writes stay on and nothing is blended —
 * the shader discards texels below the alpha threshold — so leaves need no sort
 * order and may be drawn before water. Culling is off so a canopy is solid from
 * both sides where its faces survive the cutout. */
void cf_gfx_draw_cutout(int64_t slot, int64_t nverts) {
    if (slot < 0 || slot >= CF_MAX_MESHES || nverts <= 0) return;
    glUniform1i(g_u_cutout, 1);
    glDisable(GL_CULL_FACE);
    cf_gfx_draw(slot, nverts);
    glEnable(GL_CULL_FACE);
    glUniform1i(g_u_cutout, 0);
}

/* Draw a mesh slot with vertex colour (untextured path) and the CURRENT view-
 * projection and depth state — for a world-space overlay that should obey
 * normal depth testing (e.g. a marker floating above all possible terrain,
 * so it is naturally visible without disabling the depth test). */
/* The HUD, the block outline and the map marker share the world program but are
 * not part of the world: they are screen-space or diagnostic overlays. u_unlit
 * makes the fragment shader skip lighting for them entirely, which is why they
 * survive a moving sun — forcing full daylight would still have dimmed them
 * whenever the sun sat near the horizon. */
static void cf_unlit_begin(void) { glUniform1i(g_u_unlit, 1); }
static void cf_unlit_end(void)   { glUniform1i(g_u_unlit, 0); }

void cf_gfx_draw_marker(int64_t slot, int64_t nverts) {
    if (slot < 0 || slot >= CF_MAX_MESHES || nverts <= 0) return;
    cf_unlit_begin();
    glUniform1i(g_u_use_tex, 0);
    cf_gfx_draw(slot, nverts);
    glUniform1i(g_u_use_tex, g_tex ? 1 : 0);
    cf_unlit_end();
}

/* Draw a mesh slot as GL_LINES with the current view-projection, untextured,
 * depth test off so a selection outline is never hidden by the face it sits on. */

void cf_gfx_draw_lines(int64_t slot, int64_t nverts) {
    if (slot < 0 || slot >= CF_MAX_MESHES || nverts <= 0) return;
    glDisable(GL_DEPTH_TEST);
    cf_unlit_begin();
    glUniform1i(g_u_use_tex, 0);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo[slot]);
    GLsizei stride = CF_VERT_FLOATS * sizeof(float);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void *)0);
    glEnableVertexAttribArray(1); glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, (void *)(3 * 4));
    glEnableVertexAttribArray(2); glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, stride, (void *)(5 * 4));
    glEnableVertexAttribArray(3); glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, stride, (void *)(6 * 4));
    glEnableVertexAttribArray(4); glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, stride, (void *)(7 * 4));
    glDrawArrays(GL_LINES, 0, (GLsizei)nverts);
    glUniform1i(g_u_use_tex, g_tex ? 1 : 0);
    cf_unlit_end();
    glEnable(GL_DEPTH_TEST);
}

/* Draw a screen-space overlay mesh (NDC coordinates, same 8-float layout):
 * identity view-projection, no texture, no depth test, unlit. Restores state after. */
void cf_gfx_draw_hud(int64_t slot, int64_t nverts, int64_t textured) {
    static const float ident[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    if (slot < 0 || slot >= CF_MAX_MESHES || nverts <= 0) return;
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUniformMatrix4fv(g_u_vp, 1, GL_FALSE, ident);
    cf_unlit_begin();
    glUniform1i(g_u_use_tex, (textured && g_tex) ? 1 : 0);
    cf_gfx_draw(slot, nverts);
    glUniform1i(g_u_use_tex, g_tex ? 1 : 0);
    cf_unlit_end();
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
}

/* Proposed NativeArray.blit for the March runtime: copy n floats from src[si..]
 * into dst[di..]. dst must be uniquely owned (rc == 1) — the same in-place
 * contract native_f32_arr_set uses; here it is checked and violated loudly
 * rather than silently copying, so a caller learns about a shared buffer. */
void *cf_f32_blit(void *dst, int64_t di, void *src, int64_t si, int64_t n) {
    int64_t rc = *(int64_t *)dst;
    if (rc != 1) { fprintf(stderr, "cf_f32_blit: destination is shared (rc=%lld); refusing to write in place\n", (long long)rc); abort(); }
    if (n <= 0) return dst;
    if (di < 0 || si < 0 || di + n > narr_len(dst) || si + n > narr_len(src)) {
        fprintf(stderr, "cf_f32_blit: out of range (di=%lld si=%lld n=%lld dst=%lld src=%lld)\n",
                (long long)di, (long long)si, (long long)n, (long long)narr_len(dst), (long long)narr_len(src));
        abort();
    }
    memcpy((float *)narr_data(dst) + di, (const float *)narr_data(src) + si, (size_t)n * 4);
    return dst;
}

/* The u8 twin of cf_f32_blit, for the skylight field: copy n bytes from
 * src[si..] into dst[di..] under the same rc == 1 contract. The lighting sweep
 * copies the whole 4 MB field once per level, which is a memcpy here and 4.2M
 * March-level get/set pairs otherwise. */
void *cf_u8_blit(void *dst, int64_t di, void *src, int64_t si, int64_t n) {
    int64_t rc = *(int64_t *)dst;
    if (rc != 1) { fprintf(stderr, "cf_u8_blit: destination is shared (rc=%lld); refusing to write in place\n", (long long)rc); abort(); }
    if (n <= 0) return dst;
    if (di < 0 || si < 0 || di + n > narr_len(dst) || si + n > narr_len(src)) {
        fprintf(stderr, "cf_u8_blit: out of range (di=%lld si=%lld n=%lld dst=%lld src=%lld)\n",
                (long long)di, (long long)si, (long long)n, (long long)narr_len(dst), (long long)narr_len(src));
        abort();
    }
    memcpy((unsigned char *)narr_data(dst) + di, (const unsigned char *)narr_data(src) + si, (size_t)n);
    return dst;
}

/* Sun level 0..1, eye position, look direction, and the flashlight toggle.
 * Called once per frame before the world passes. */
void cf_gfx_set_light(double sun, double ex, double ey, double ez,
                      double dx, double dy, double dz, int64_t flash) {
    if (!g_prog) return;
    glUseProgram(g_prog);
    glUniform1f(g_u_sun, (float)sun);
    glUniform3f(g_u_eye, (float)ex, (float)ey, (float)ez);
    glUniform3f(g_u_dir, (float)dx, (float)dy, (float)dz);
    glUniform1f(g_u_flash, flash ? 1.0f : 0.0f);
}

/* Direction TO the sun and TO the moon, in world space. Normalised here so the
 * caller can pass a raw arc position. Called once per frame with set_light. */
/* Shadow reach in blocks; 0 disables the trace entirely. */
void cf_gfx_set_shadow(double dist) {
    if (!g_prog) return;
    glUseProgram(g_prog);
    glUniform1f(g_u_shadow, (float)dist);
}

void cf_gfx_set_sky(double sx, double sy, double sz, double mx, double my, double mz) {
    if (!g_prog) return;
    double sl = sqrt(sx*sx + sy*sy + sz*sz); if (sl < 1e-9) sl = 1.0;
    double ml = sqrt(mx*mx + my*my + mz*mz); if (ml < 1e-9) ml = 1.0;
    glUseProgram(g_prog);
    glUniform3f(g_u_sundir,  (float)(sx/sl), (float)(sy/sl), (float)(sz/sl));
    glUniform3f(g_u_moondir, (float)(mx/ml), (float)(my/ml), (float)(mz/ml));
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
    memset(g_in.keys_pressed, 0, sizeof g_in.keys_pressed);
    glfwPollEvents();
}
int64_t cf_in_key(int64_t k)          { return (k >= 0 && k < CF_MAX_KEYS) ? g_in.keys[k] : 0; }
int64_t cf_in_key_pressed(int64_t k)  { return (k >= 0 && k < CF_MAX_KEYS) ? g_in.keys_pressed[k] : 0; }
int64_t cf_in_button(int64_t b)       { return (b >= 0 && b < 8) ? g_in.buttons[b] : 0; }
int64_t cf_in_button_pressed(int64_t b){ return (b >= 0 && b < 8) ? g_in.buttons_pressed[b] : 0; }
double  cf_in_mouse_dx(void)          { return g_in.mouse_dx; }
double  cf_in_mouse_dy(void)          { return g_in.mouse_dy; }
double  cf_in_scroll_dy(void)         { return g_in.scroll_dy; }
void    cf_in_capture_cursor(int64_t on) {
    if (g_win) glfwSetInputMode(g_win, GLFW_CURSOR, on ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
}

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
#include <mach/mach.h>

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
    unsigned char buttons_released[8];  /* edge: came up since last poll */
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
/* CF_NOMOUSE=1 drops mouse look entirely. Without it a frame dump is not
 * reproducible across runs: the window opens wherever the window manager puts
 * it, and a stationary pointer that ends up inside one window and outside the
 * next produces different cursor deltas, so the camera yaw differs and the whole
 * frame changes. That silently invalidates any dump comparison spanning
 * separate window sessions. */
static int g_nomouse = -1;
static void on_cursor(GLFWwindow *w, double x, double y) {
    (void)w;
    if (g_nomouse < 0) { const char *e = getenv("CF_NOMOUSE"); g_nomouse = (e && atoi(e) == 1); }
    if (g_nomouse) return;
    if (g_have_mouse) { g_in.mouse_dx += x - g_in.mouse_x; g_in.mouse_dy += y - g_in.mouse_y; }
    g_in.mouse_x = x; g_in.mouse_y = y; g_have_mouse = 1;
}
static void on_button(GLFWwindow *w, int b, int action, int mods) {
    (void)w; (void)mods;
    if (b < 0 || b >= 8) return;
    if (action == GLFW_PRESS) { g_in.buttons[b] = 1; g_in.buttons_pressed[b] = 1; }
    else if (action == GLFW_RELEASE) { g_in.buttons[b] = 0; g_in.buttons_released[b] = 1; }
}
static void on_scroll(GLFWwindow *w, double dx, double dy) { (void)w; (void)dx; g_in.scroll_dy += dy; }
static void on_fb_size(GLFWwindow *w, int width, int height) {
    (void)w; g_fb_w = width; g_fb_h = height; glViewport(0, 0, width, height);
}

/* March's scheduler runs `main` as an ordinary work-stealing green thread
 * unless MARCH_PIN_MAIN=1: whichever OS scheduler thread happens to dequeue
 * it runs it, and every later yield (an await, or the ~1ms cooperative
 * preemption quantum) is a fresh chance to land on a different one. Cocoa
 * (GLFW's backend here) requires every window/GL call happen on the one
 * OS thread the process actually started on — unrelated to March, a
 * decades-old AppKit constraint. Measured unpinned: 4 of 5 runs never
 * touched the real main thread even before the first task was spawned
 * (~1-in-`MARCH_NUM_SCHEDULERS` luck), and games that got past window
 * creation still crashed later once preemption moved `main` off it.
 *
 * A C constructor runs before any Mach-O/ELF binary reaches its `main()`,
 * hence before the runtime reads this env var — so setting it here reaches
 * the scheduler in time. `setenv`'s third argument is 0 (don't overwrite),
 * so an explicit `MARCH_PIN_MAIN=0` in the environment still disables it. */
__attribute__((constructor))
static void cf_force_pin_main(void) {
    setenv("MARCH_PIN_MAIN", "1", 0);
}

/* ═══════════════════════════════ WINDOW DOMAIN ═══════════════════════════════ */

int64_t cf_win_open(int64_t w, int64_t h, march_value title) {
    if (!pthread_main_np()) {
        fprintf(stderr, "cf: cf_win_open must run on the process main thread (GLFW/Cocoa requirement). "
                        "MARCH_PIN_MAIN is forced on by a constructor in this shim (cf_force_pin_main) "
                        "unless explicitly overridden to 0 in the environment; if you see this without "
                        "such an override, the constructor didn't run in time or the runtime lacks the "
                        "pin-main-thread patch (see GAPS.md G15) — try MARCH_NUM_SCHEDULERS=1.\n");
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
    /* Vsync and fullscreen are settings (CubeForge.Settings), applied by March
     * through cf_win_set_vsync / cf_win_set_fullscreen right after this returns
     * and again whenever the player changes them. The window opens vsync-on
     * so nothing spins before that first call. */
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
void cf_gfx_end_query(void);   /* defined with the timer query below */
void    cf_win_swap(void)         { cf_gfx_end_query(); if (g_win) glfwSwapBuffers(g_win); }
double  cf_win_time(void)         { return glfwGetTime(); }
int64_t cf_win_fb_w(void)         { return g_fb_w; }
int64_t cf_win_fb_h(void)         { return g_fb_h; }
void    cf_win_close(void)        { if (g_win) { glfwDestroyWindow(g_win); g_win = NULL; } glfwTerminate(); }
void    cf_win_request_close(void){ if (g_win) glfwSetWindowShouldClose(g_win, 1); }

/* Vsync off uncaps the frame rate, so frame cost can actually be measured;
 * with it on every timing is pinned to the display refresh. Remembered so a
 * monitor change (below) can re-apply it: the swap interval belongs to the
 * context and some platforms reset it when the window moves. */
static int g_vsync = 1;
void cf_win_set_vsync(int64_t on) {
    g_vsync = on ? 1 : 0;
    if (g_win) glfwSwapInterval(g_vsync);
}

/* Fullscreen takes the primary monitor at its current video mode. Note this
 * is GLFW's video mode, not the panel's native backing store, so on a Retina
 * display it is 1920x1200 rather than 3456x2234 -- CF_WIDTH/CF_HEIGHT reach a
 * larger framebuffer than this does. Leaving fullscreen restores the windowed
 * rectangle recorded on the way in. */
static int g_fullscreen = 0;
static int g_win_x = 0, g_win_y = 0, g_win_w = 800, g_win_h = 600;
void cf_win_set_fullscreen(int64_t on) {
    int want = on ? 1 : 0;
    if (!g_win || want == g_fullscreen) return;
    if (want) {
        GLFWmonitor *m = glfwGetPrimaryMonitor();
        const GLFWvidmode *mode = m ? glfwGetVideoMode(m) : NULL;
        if (!mode) return;
        glfwGetWindowPos(g_win, &g_win_x, &g_win_y);
        glfwGetWindowSize(g_win, &g_win_w, &g_win_h);
        glfwSetWindowMonitor(g_win, m, 0, 0, mode->width, mode->height, mode->refreshRate);
    } else {
        glfwSetWindowMonitor(g_win, NULL, g_win_x, g_win_y, g_win_w, g_win_h, 0);
    }
    g_fullscreen = want;
    glfwSwapInterval(g_vsync);
    glfwGetFramebufferSize(g_win, &g_fb_w, &g_fb_h);
    glViewport(0, 0, g_fb_w, g_fb_h);
}

/* ── GL: shader program + one VAO shared by every mesh ──────────────────────
 * Vertex layout (10 floats): pos.xyz, uv, layer, sky, face, fx, block light.
 *
 * Sky and block light are SEPARATE attributes and must stay that way. They
 * were once one float, sky in the fraction and block light as an even integer
 * above it, unpacked in the fragment shader. A varying is interpolated across
 * the triangle, and the unpack is not linear: between two corners that are
 * both fully sunlit but differ in block light, the decoded sky ran 1.0, 1.4,
 * 1.8, 0.2, 0.6, 1.0 -- a sawtooth that painted near-black bands over lit
 * ground wherever a glowing block stood. Two channels cannot share one
 * interpolated scalar.
 * `fx` packs an effect id and an alpha: effect * 256 + alpha*255, exact
 * because floats hold integers to 2^24. Effect 1 is precipitation.
 * `face` is 0..5 (+y -y +x -x +z -z); the vertex shader turns it into a normal
 * for the flashlight and into the directional multiplier that used to be baked
 * into `shade` by the mesher.                                                  */
#define CF_VERT_FLOATS 10
/* Floats in one merged rectangle: six vertices. Every reserve on both sides of
 * the FFI is stated in these terms -- a literal here and a literal in
 * F32Buf.push_quad drifted apart the moment the vertex grew. */
#define CF_QUAD_FLOATS (6 * CF_VERT_FLOATS)
/* These four slots are bound directly here as well as being named on the
 * March side; they MUST match the slot map in cube_forge.march. They were
 * 249/248/244/246, which the 12-chunk window turned into chunk slots -- the
 * spray upload then clobbered a chunk's VBO. Keep the two lists together. */
#define CF_PRECIP_SLOT 505
static GLuint g_prog = 0, g_vao = 0;
static GLint  g_u_vp = -1, g_u_tex = -1;

static const char *VS =
    "#version 330 core\n"
    "layout(location=0) in vec3 a_pos;\n"
    "layout(location=1) in vec2 a_uv;\n"
    "layout(location=2) in float a_layer;\n"
    "layout(location=3) in float a_shade;\n"
    "layout(location=4) in float a_face;\n"
    "layout(location=5) in float a_fx;\n"
    "layout(location=6) in float a_blk;\n"
    "uniform mat4 u_vp;\n"
    "uniform float u_time;\n"
    /* The slot's offset: a chunk mesh is baked at the window-local origin it
     * had when meshed, and the window has since slid (cf_gfx_shift). */
    "uniform vec2 u_off;\n"
    "out vec2 v_uv; out float v_layer; out float v_shade; out float v_blk;\n"
    "out vec3 v_world; out vec3 v_normal; out float v_fx;\n"
    "const vec3 NORMALS[6] = vec3[6](vec3(0,1,0), vec3(0,-1,0), vec3(1,0,0), vec3(-1,0,0), vec3(0,0,1), vec3(0,0,-1));\n"
    "void main(){\n"
    "  int f = int(a_face + 0.5);\n"
    /* Water bobs: a small vertical wave on any water effect (>= 2). Cosmetic
     * only -- v_world stays at the true position so lighting, shadows and
     * fog see the block the game logic sees. */
    "  int fe = int(a_fx + 0.5) >> 8;\n"
    "  vec3 p = a_pos;\n"
    "  p.xz += u_off;\n"
    "  if (fe >= 2) p.y += 0.03 * sin(u_time * 1.7 + p.x * 1.3 + p.z * 0.9);\n"
    "  gl_Position = u_vp * vec4(p,1.0);\n"
    "  v_uv=a_uv; v_layer=a_layer;\n"
    /* v_shade is the packed shade word (see Vertex.pack_shade): sky x AO in
     * [0, 1], block light x AO in even integers above it. Passed through as is;
     * the fragment shader unpacks it. The per-face directional constant that
     * used to be folded in here is replaced by a real N.L against a sun that
     * moves, computed per fragment. */
    "  v_shade = a_shade;\n"
    "  v_blk = a_blk;\n"
    "  v_world = a_pos + vec3(u_off.x, 0.0, u_off.y); v_normal = NORMALS[f];\n"
    "  v_fx = a_fx;\n"
    "}\n";
static const char *FS =
    "#version 330 core\n"
    "in vec2 v_uv; in float v_layer; in float v_shade; in float v_blk;\n"
    "in vec3 v_world; in vec3 v_normal; in float v_fx;\n"
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
    "uniform sampler3D u_occ_c;\n"
    "uniform float u_shadow;\n"
    "uniform float u_soft;\n"
    /* The occupancy textures are TOROIDAL: the window slides but their content
     * does not move, so a shift only rewrites the incoming band. This is the
     * window origin inside them, in texels (x, z); GL_REPEAT does the wrap, so
     * it costs an add. Both DDAs bounds-check before every sample, so a ray
     * leaving the window never reaches the wrap. */
    "uniform vec2 u_occ_off;\n"
    "uniform sampler3D u_light;\n"
    "uniform int u_gpulight;\n"
    "uniform float u_fog_density;\n"
    "uniform vec3  u_fog_color;\n"
    "uniform float u_overcast;\n"
    "uniform float u_bolt;\n"
    "uniform float u_time;\n"
    "uniform vec3  u_species[6];\n"
    "uniform int   u_myc_first;\n"
    "uniform int   u_myc_count;\n"
    "uniform float u_branch;\n"
    "uniform float u_branch_scale;\n"
    "uniform float u_branch_sharp;\n"
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
    /* Block light (glowing fungus, later any emissive block): warm, and its own
     * source rather than a sun term. */
    "const vec3  GLOW = vec3(1.00, 0.90, 0.70);\n"
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
    "const vec3 WORLD = vec3(192.0, 256.0, 192.0);\n"   /* = CF_WORLD_SIDE, checked in cf_gfx_upload_occupancy */
    "const int  MAX_STEPS = 256;\n"
    "const float SOFT_SPREAD = 0.035;\n"
    "const int SOFT_TAPS = 2;\n"
    "const float CS = 8.0;\n"          /* coarse cell size, matching CF_OCC_CS */
    "const int  MAX_COARSE = 160;\n"
    /* Amanatides-Woo voxel DDA. Returns the distance at which the ray first
     * meets an occluder, or 1e30 when it reaches maxDist unobstructed. Exact on
     * axis-aligned voxels: no depth bias, no acne, no peter-panning.
     *
     * A two-level version of this (an 8x8x8 coarse occupancy level to skip empty
     * air, ~4x faster) is in 9fa19c7 on the weather branch. It is NOT here: it
     * disagrees with this trace on a ~190-pixel strip in the bottom-left corner
     * at CF_SUN=12 CF_SHADOW_SOFT=0, missing an occluder. Forcing every coarse
     * cell occupied still reproduces it, so the fault is in splitting the DDA
     * into per-cell spans, not in the coarse data. See todos.md. */
    /* Fine Amanatides-Woo DDA over the parametric span [t0, t1], returning the
     * ABSOLUTE distance at which the ray first meets an occluder, or 1e30.
     *
     * `testEntry` is false for the span starting at the ray origin: the origin is
     * pushed half a voxel along the normal and testing the voxel it sits in would
     * make every lit surface shadow itself. Every LATER coarse cell must test its
     * entry voxel, because the ray enters that cell there. */
    "float traceSpan(vec3 p, vec3 dir, float t0, float t1, bool testEntry){\n"
    "  vec3  q   = p + dir * t0;\n"
    "  ivec3 v   = ivec3(floor(q));\n"
    "  ivec3 stp = ivec3(sign(dir));\n"
    "  bvec3 moving = greaterThan(abs(dir), vec3(1e-8));\n"
    "  vec3  den    = mix(vec3(1.0), dir, moving);\n"
    "  vec3  tMax   = mix(vec3(1e30), (vec3(v) + step(0.0, dir) - q) / den, moving);\n"
    "  vec3  tDelta = mix(vec3(1e30), 1.0 / abs(den), moving);\n"
    "  if (testEntry && v.x >= 0 && v.y >= 0 && v.z >= 0 &&\n"
    "      v.x < int(WORLD.x) && v.y < int(WORLD.y) && v.z < int(WORLD.z))\n"
    "    if (texture(u_occ, (vec3(v) + vec3(u_occ_off.x, 0.0, u_occ_off.y) + 0.5) / WORLD).r > 0.5) return t0;\n"
    "  float span = t1 - t0;\n"
    "  for (int i = 0; i < MAX_STEPS; i++){\n"
    "    float tNow = min(tMax.x, min(tMax.y, tMax.z));\n"
    "    if (tNow > span) return 1e30;\n"
    "    if (tMax.x <= tMax.y && tMax.x <= tMax.z) { v.x += stp.x; tMax.x += tDelta.x; }\n"
    "    else if (tMax.y <= tMax.z)                { v.y += stp.y; tMax.y += tDelta.y; }\n"
    "    else                                      { v.z += stp.z; tMax.z += tDelta.z; }\n"
    "    if (v.x < 0 || v.y < 0 || v.z < 0 || v.x >= int(WORLD.x) || v.y >= int(WORLD.y) || v.z >= int(WORLD.z)) return 1e30;\n"
    "    if (texture(u_occ, (vec3(v) + vec3(u_occ_off.x, 0.0, u_occ_off.y) + 0.5) / WORLD).r > 0.5) return t0 + tNow;\n"
    "  }\n"
    "  return 1e30;\n"
    "}\n"
    /* Two-level Amanatides-Woo. The outer walk is over 8x8x8 coarse cells, each
     * marked exactly when it holds a solid voxel, so a ray crossing open air
     * covers eight blocks per texture fetch instead of one and drops into the
     * fine grid only where there is something to hit. Coarse cells are visited in
     * ray order, so the first fine hit is the nearest: the result is identical to
     * the single-level trace, in far fewer steps. That matters most with soft
     * shadows, which fire four of these per fragment. */
    /* PROTOTYPE: the mesher's corner_pack, per fragment. For each of the four
     * corners of the voxel face the fragment sits on, average the light of the
     * face-adjacent air voxel and the three neighbours round that corner,
     * skipping the occluded ones, and derive AO from how many were occluded --
     * then bilinearly blend the four. This is exactly what the mesher bakes into
     * a_shade; doing it here is what would let light stop fragmenting the greedy
     * merge. 25 texture fetches a fragment: one shared light tap, then three
     * occupancy and three light taps per corner. */
    "float occAt(vec3 v){ return texture(u_occ, (v + vec3(u_occ_off.x, 0.0, u_occ_off.y) + 0.5) / WORLD).r > 0.5 ? 1.0 : 0.0; }\n"
    "vec2 litAt(vec3 v){ return texture(u_light, (v + vec3(u_occ_off.x, 0.0, u_occ_off.y) + 0.5) / WORLD).rg; }\n"
    "vec2 cornerLight(vec3 av, vec3 du, vec3 dv, vec2 nl){\n"
    "  float o1 = occAt(av + du), o2 = occAt(av + dv), oc = occAt(av + du + dv);\n"
    "  vec2 l1 = o1 > 0.5 ? vec2(0.0) : litAt(av + du);\n"
    "  vec2 l2 = o2 > 0.5 ? vec2(0.0) : litAt(av + dv);\n"
    "  vec2 lc = oc > 0.5 ? vec2(0.0) : litAt(av + du + dv);\n"
    "  float cnt = 1.0 + (1.0 - o1) + (1.0 - o2) + (1.0 - oc);\n"
    /* the mesher's corner_ao: both sides occluded is the darkest corner */
    "  float side = o1 + o2;\n"
    "  float ao = (o1 > 0.5 && o2 > 0.5) ? 0.0 : (3.0 - side - oc) / 3.0;\n"
    "  return ((nl + l1 + l2 + lc) / cnt) * mix(0.55, 1.0, ao);\n"
    "}\n"
    "vec2 smoothLight(vec3 p, vec3 n){\n"
    "  vec3 av = floor(p + n * 0.5);\n"
    /* the two in-face axes */
    "  vec3 du = abs(n.x) > 0.5 ? vec3(0,1,0) : vec3(1,0,0);\n"
    "  vec3 dv = abs(n.y) > 0.5 ? vec3(0,0,1) : (abs(n.x) > 0.5 ? vec3(0,0,1) : vec3(0,1,0));\n"
    "  vec3 rel = p + n * 0.5 - av;\n"
    "  float fu = clamp(dot(rel, du), 0.0, 1.0);\n"
    "  float fv = clamp(dot(rel, dv), 0.0, 1.0);\n"
    "  vec2 nl = litAt(av);\n"
    "  vec2 c00 = cornerLight(av, -du, -dv, nl);\n"
    "  vec2 c10 = cornerLight(av,  du, -dv, nl);\n"
    "  vec2 c01 = cornerLight(av, -du,  dv, nl);\n"
    "  vec2 c11 = cornerLight(av,  du,  dv, nl);\n"
    "  return mix(mix(c00, c10, fu), mix(c01, c11, fu), fv);\n"
    "}\n"
    "float traceDist(vec3 p, vec3 dir, float maxDist){\n"
    /* Skipping empty space only pays when there IS empty space. A ray near the
     * horizon travels a long way through terrain, so almost every coarse cell it
     * crosses is occupied and the outer walk is pure overhead -- measured at
     * -14% near sunset against +100% at midday. The light direction is a uniform,
     * so this branch is coherent across the whole draw and costs nothing.
     * traceSpan over the full range IS the single-level trace. */
    "  if (abs(dir.y) < 0.35) return traceSpan(p, dir, 0.0, maxDist, false);\n"
    "  vec3  CW    = ceil(WORLD / CS);\n"
    "  ivec3 c     = ivec3(floor(p / CS));\n"
    "  ivec3 cstp  = ivec3(sign(dir));\n"
    "  bvec3 cmov  = greaterThan(abs(dir), vec3(1e-8));\n"
    "  vec3  cden  = mix(vec3(1.0), dir, cmov);\n"
    "  vec3  ctMax = mix(vec3(1e30), ((vec3(c) + step(0.0, dir)) * CS - p) / cden, cmov);\n"
    "  vec3  ctDel = mix(vec3(1e30), vec3(CS) / abs(cden), cmov);\n"
    "  float tEnter = 0.0;\n"
    "  for (int i = 0; i < MAX_COARSE; i++){\n"
    "    if (tEnter > maxDist) return 1e30;\n"
    "    if (c.x < 0 || c.y < 0 || c.z < 0 || c.x >= int(CW.x) || c.y >= int(CW.y) || c.z >= int(CW.z)) return 1e30;\n"
    "    float tExit = min(ctMax.x, min(ctMax.y, ctMax.z));\n"
    "    if (texture(u_occ_c, (vec3(c) + vec3(u_occ_off.x, 0.0, u_occ_off.y) / CS + 0.5) / CW).r > 0.5) {\n"
    "      float hit = traceSpan(p, dir, tEnter, min(tExit, maxDist), i > 0);\n"
    "      if (hit < 1e29) return hit;\n"
    "    }\n"
    "    tEnter = tExit;\n"
    "    if (ctMax.x <= ctMax.y && ctMax.x <= ctMax.z) { c.x += cstp.x; ctMax.x += ctDel.x; }\n"
    "    else if (ctMax.y <= ctMax.z)                  { c.y += cstp.y; ctMax.y += ctDel.y; }\n"
    "    else                                          { c.z += cstp.z; ctMax.z += ctDel.z; }\n"
    "  }\n"
    "  return 1e30;\n"
    "}\n"
    "float shadowOf(float hit, float maxDist){\n"
    "  if (hit > maxDist) return 1.0;\n"
    "  return smoothstep(0.75, 1.0, hit / maxDist);\n"
    "}\n"
    "float hash12(vec2 v){ return fract(sin(dot(v, vec2(12.9898, 78.233))) * 43758.5453); }\n"
    "float vnoise(vec2 p){\n"
    "  vec2 i = floor(p), f = fract(p);\n"
    "  f = f * f * (3.0 - 2.0 * f);\n"
    "  float a = hash12(i), b = hash12(i + vec2(1.0, 0.0));\n"
    "  float c = hash12(i + vec2(0.0, 1.0)), d = hash12(i + vec2(1.0, 1.0));\n"
    "  return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);\n"
    "}\n"
    /* Hyphae: ridged, domain-warped value noise. The ridge is where the
     * noise crosses its midpoint, which wanders and FORKS -- that fork is
     * what reads as branching, and no tile can produce it because the
     * threads have to run on across a block boundary. Fed world-space
     * coordinates, so a filament crosses from block to block unbroken and
     * the pattern never repeats. The warp keeps the ridges from reading as
     * contour lines on a map. */
    "float hyphae(vec2 p){\n"
    "  vec2 w = p * u_branch_scale;\n"
    "  vec2 q = w + 0.75 * vec2(vnoise(w * 0.55), vnoise(w * 0.55 + 19.7));\n"
    "  float n = 0.65 * vnoise(q * 1.15) + 0.35 * vnoise(q * 2.60);\n"
    "  float ridge = 1.0 - abs(2.0 * n - 1.0);\n"
    "  return smoothstep(u_branch_sharp, 1.0, ridge);\n"
    "}\n"
    /* Hard shadows are one ray. Soft shadows spread SOFT_TAPS over a small cone:
     * because the rays diverge, the penumbra widens with distance from the
     * caster on its own, which is what real soft shadows do. The cone is rotated
     * per pixel so the samples read as softness rather than as bands.
     *
     * Two taps, not four. The trace is the whole cost of a shadow -- hard
     * shadows measure within 0.1 ms of no shadows at all, four-tap soft ones
     * cost 3.3 ms of an 8.1 ms frame at 1920x1200 -- so the tap count IS the
     * shadow budget. The per-pixel rotation is what makes a low tap count read
     * as softness instead of as banding, and it does not care how many taps it
     * is rotating. */
    "float shadow(vec3 p, vec3 dir, float maxDist){\n"
    "  if (u_shadow <= 0.0) return 1.0;\n"
    "  if (u_soft < 0.5) return shadowOf(traceDist(p, dir, maxDist), maxDist);\n"
    "  vec3 up = abs(dir.y) < 0.9 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);\n"
    "  vec3 t1 = normalize(cross(dir, up));\n"
    "  vec3 t2 = cross(dir, t1);\n"
    "  float a0 = hash12(gl_FragCoord.xy) * 6.2831853;\n"
    "  float acc = 0.0;\n"
    "  for (int k = 0; k < SOFT_TAPS; k++){\n"
    "    float a = a0 + float(k) * (6.2831853 / float(SOFT_TAPS));\n"
    "    vec3 d = normalize(dir + (t1 * cos(a) + t2 * sin(a)) * SOFT_SPREAD);\n"
    "    acc += shadowOf(traceDist(p, d, maxDist), maxDist);\n"
    "  }\n"
    "  return acc * (1.0 / float(SOFT_TAPS));\n"
    "}\n"
    /* Eight compass directions for the flow effects 2..9, in the order the
     * mesher packs them: +x, +x+z, +z, -x+z, -x, -x-z, -z, +x-z. */
    "const vec2 DIRS[8] = vec2[8](vec2(1,0), vec2(0.7071,0.7071), vec2(0,1), vec2(-0.7071,0.7071), vec2(-1,0), vec2(-0.7071,-0.7071), vec2(0,-1), vec2(0.7071,-0.7071));\n"
    "void main(){\n"
    "  int  fxw = int(v_fx + 0.5);\n"
    "  int  fe  = fxw >> 8;\n"
    /* The shade float packs two channels (see Vertex.pack_shade): sky shade in
     * [0, 1], and block-light shade in even integers above it. Overlays push a
     * plain shade in [0, 1], which decodes as sky-only and leaves them alone. */
    "  float sk = v_shade;\n"
    "  float bl = v_blk;\n"
    /* PROTOTYPE: take the same two channels from the light texture instead of
     * the vertex, computed per fragment. Uniform branch, so it is coherent
     * across the draw and the off case pays nothing. */
    "  if (u_gpulight == 1 && u_unlit != 1 && (fxw >> 8) != 1) { vec2 g = smoothLight(v_world, v_normal); sk = g.r; bl = g.g; }\n"
    "  float spd = float(fxw & 255) / 255.0 * 7.0;\n"
    /* Flowing water scrolls its ripple along the flow; still water drifts;
     * fast or falling water blends toward the foam layer. */
    "  vec2 uv = v_uv;\n"
    "  if (fe >= 2 && fe <= 9) uv += DIRS[fe - 2] * u_time * (0.05 + spd * 0.04);\n"
    "  else if (fe == 10)      uv += vec2(0.02, 0.013) * u_time;\n"
    "  vec4 t = (u_use_tex == 1) ? texture(u_tex, vec3(uv, v_layer)) : vec4(v_uv, v_layer, 1.0);\n"
    "  float foam = (fe == 11) ? 1.0 : ((fe >= 2 && fe <= 9) ? spd / 7.0 : 0.0);\n"
    "  if (foam > 0.0 && u_use_tex == 1) t = mix(t, texture(u_tex, vec3(uv * 1.5, 16.0)), foam * 0.7);\n"
    "  if (u_cutout == 1 && t.a < 0.5) discard;\n"
    /* Mycelium wears its species colour twice: a wash baked into the tile
     * (CubeForge.Texture.myc_tint) and these filaments, drawn here because
     * they must not stop at a block edge. The layer index carries the
     * species -- layers are myc_first + 6 * base + (species - 1) -- so no
     * vertex channel is needed. Overlays (u_unlit) are not the world. */
    "  if (u_branch > 0.0 && u_unlit == 0 && u_use_tex == 1) {\n"
    "    int mycl = int(v_layer + 0.5) - u_myc_first;\n"
    "    if (mycl >= 0 && mycl < u_myc_count) {\n"
    "      vec3 an = abs(v_normal);\n"
    "      vec2 pw = (an.y > 0.5) ? v_world.xz : ((an.x > 0.5) ? v_world.zy : v_world.xy);\n"
    "      t.rgb = mix(t.rgb, u_species[mycl % 6], hyphae(pw) * u_branch);\n"
    "    }\n"
    "  }\n"
    "  float moon = clamp((MOON_UNTIL - u_sun) / MOON_UNTIL, 0.0, 1.0);\n"
    /* Cloud does not remove light, it diffuses it: ambient rises as the direct
     * term falls, which is why an overcast day is flat rather than dark.
     * u_bolt is the lightning spike, added to ambient so a strike lights the
     * whole scene at once rather than from a direction. */
    "  float amb  = (AMB + AMB_UP * v_normal.y) * (1.0 + 1.2 * u_overcast) + u_bolt;\n"
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
    /* Two weather early-outs, on top of the four the trace already had. Both
     * skip work whose result is provably invisible, so neither costs quality.
     *
     * `shad` is about to be mixed toward 1.0 by u_overcast, so above 0.98 the
     * trace can change the final colour by less than 1/255 — under full cloud
     * the old code ray-marched 64 blocks and then discarded the answer.
     *
     * Fog is the same argument at the other end: a fragment the fog has already
     * washed out by 98% cannot show a shadow either. `fogf` is computed here
     * rather than at the end so both the shadow and the fog mix can use it. */
    "  float fogd = (u_unlit == 1) ? 0.0 : length(v_world - u_eye) * u_fog_density;\n"
    "  float fogf = 1.0 - exp(-fogd);\n"
    "  bool  lit_matters = u_overcast < 0.98 && fogf < 0.98;\n"
    "  if (sk > 0.001 && lit_matters) {\n"
    "    if (inten > 0.0 && ndls > 0.0)      shad = shadow(origin, u_sundir,  u_shadow);\n"
    "    else if (moon > 0.0 && ndlm > 0.0)  shad = shadow(origin, u_moondir, u_shadow);\n"
    "  }\n"
    /* Under cloud the sun is a source the size of the sky, so its shadows wash
     * out. Softening the trace toward 1.0 is a cheat, not scattering, but it
     * costs nothing and it is the difference between an overcast day and a
     * clear one with grey paint on it. */
    "  shad = mix(shad, 1.0, u_overcast);\n"
    "  float direct = DIRECT * (1.0 - 0.85 * u_overcast);\n"
    "  vec3  sky  = sunc * (inten * (amb + direct * ndls * shad))\n"
    "             + MOON_TINT * (MOON_LEVEL * moon * (amb + direct * ndlm * shad));\n"
    "  vec3  baked = sk * sky;\n"
    "  vec3  L  = u_eye - v_world;\n"
    "  float d2 = dot(L, L);\n"
    "  vec3  Ln = L * inversesqrt(max(d2, 1e-6));\n"
    "  float spot  = smoothstep(COS_OUTER, COS_INNER, dot(-Ln, u_dir));\n"
    "  float flash = u_flash * spot * max(dot(v_normal, Ln), 0.0) / (1.0 + 0.02 * d2);\n"
    /* The flashlight gets its own trace, toward the eye, and only when it would
     * contribute anything at all. */
    "  if (flash > 0.001) flash *= shadow(origin, Ln, min(sqrt(d2), u_shadow));\n"
    /* Block light is its own source: independent of the sun, so it is what
     * you see at midnight. max, not +, so a glowing patch at noon is not
     * brighter than the noon around it. */
    "  vec3  world = max(baked, bl * GLOW) + vec3(flash);\n"
    /* Overlays (HUD, outline, map marker) share this program but are not part of
     * the world: they keep their own vertex shade and skip lighting entirely. */
    "  vec3 lit = t.rgb * ((u_unlit == 1) ? vec3(sk) : world);\n"
    "  int  fx  = fxw;\n"
    /* Water effects carry speed in the alpha byte, not alpha. */
    "  float a  = (fe >= 2) ? 1.0 : float(fx & 255) / 255.0;\n"
    /* Fog is for the world only. Overlays (HUD, crosshair, outline, marker) are
     * drawn in NDC with z = 0, so `length(v_world - u_eye)` is a meaningless
     * large number for them and a storm would grey out the hotbar. u_unlit
     * already marks exactly that geometry.
     *
     * Effect 1 is precipitation, exempt for a different reason: it is the thing
     * generating the fog, so mixing it toward the fog colour would fade the
     * rain out exactly as the storm peaked.
     *
     * Exponential fog, so the far plane does not enter into it. */
    "  if (u_unlit != 1 && (fx >> 8) != 1) lit = mix(lit, u_fog_color, fogf);\n"
    "  o_color = vec4(lit, t.a * a);\n"
    "}\n";

static GLuint compile(GLenum kind, const char *src) {
    GLuint s = glCreateShader(kind);
    glShaderSource(s, 1, &src, NULL); glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[2048]; glGetShaderInfoLog(s, sizeof log, NULL, log); fprintf(stderr, "cf: shader: %s\n", log); }
    return s;
}

/* The streaming window is CF_WORLD_CHUNKS chunks a side, so the light and
 * occupancy fields are CF_WORLD_SIDE = 16 * CF_WORLD_CHUNKS wide and deep.
 * These must agree with CubeForge.World.size() and CubeForge.Light.size_x();
 * world_size_test asserts the March half, and cf_gfx_upload_occupancy checks
 * this half against the dimensions March passes it. The shader's WORLD
 * constant below is written out as a literal and is checked there too. */
#define CF_WORLD_CHUNKS 12
#define CF_WORLD_SIDE   (16 * CF_WORLD_CHUNKS)
#define CF_WORLD_VOL    ((int64_t)CF_WORLD_SIDE * 256 * CF_WORLD_SIDE)
#define CF_CHUNK_SLOTS  (CF_WORLD_CHUNKS * CF_WORLD_CHUNKS)
/* 3 passes of CF_CHUNK_SLOTS, then the named UI slots from 500 (see the slot
 * map in cube_forge.march). */
#define CF_MAX_MESHES 512
/* Two VBOs per mesh slot. An upload goes to the one the last frame did NOT
 * draw, so glBufferSubData never waits on a buffer the GPU still reads; the
 * drain trace had 1-4 ms stalls in it on a single buffer, orphaning or not. */
static GLuint g_vbo[CF_MAX_MESHES];
static GLuint g_vbo_b[CF_MAX_MESHES];
static unsigned char g_vbo_cur[CF_MAX_MESHES];
static int64_t g_vbo_cap[2][CF_MAX_MESHES];   /* floats allocated per buffer; grown, never shrunk */
static inline GLuint vbo_of(int64_t slot) { return g_vbo_cur[slot] ? g_vbo_b[slot] : g_vbo[slot]; }
/* A VAO per mesh slot was tried (2026-09-05 perf pass): 192 chunk draws a frame as one bind and one draw each. A/B over six alternating runs was noise, so the shared VAO stays. */
static int g_debug = -1;
static inline int cf_debug(void) { if (g_debug < 0) g_debug = getenv("CF_DEBUG") != NULL; return g_debug; }
static GLint  g_u_species = -1;
static GLint  g_u_myc_first = -1;
static GLint  g_u_myc_count = -1;
static GLint  g_u_branch = -1;
static GLint  g_u_branch_scale = -1;
static GLint  g_u_branch_sharp = -1;
static GLint  g_u_use_tex = -1;
static GLint  g_u_cutout = -1;
static GLint  g_u_sun = -1, g_u_eye = -1, g_u_dir = -1, g_u_flash = -1;
static GLint  g_u_sundir = -1, g_u_moondir = -1, g_u_unlit = -1;
static GLint  g_u_time = -1;
/* The occupancy textures are toroidal: window-local (x, y, z) lives at texel
 * ((x + g_occ_ox) mod w, y, (z + g_occ_oz) mod d). A shift advances the origin
 * by the band it brought in, which lands on exactly the texels the band that
 * left was using, so only that band is rewritten -- 0.8 MB instead of 9.4.
 * Always a multiple of 16, hence of CF_OCC_CS, so the coarse level shifts by a
 * whole number of cells too. */
static int64_t g_occ_ox = 0, g_occ_oz = 0;
static GLint  g_u_occ_off = -1;
static GLint  g_u_light = -1, g_u_gpulight = -1;
static GLuint g_lt = 0;
static void occ_push_off(void);
static GLint  g_u_fog_density = -1, g_u_fog_color = -1, g_u_overcast = -1, g_u_bolt = -1;
static GLint  g_u_off = -1;
/* Per-slot window offset (blocks, x and z): zero for a slot uploaded since the
 * last shift, -16 per chunk the window has slid since for one that was not. */
static float  g_off[CF_MAX_MESHES][2];
/* The clear colour, kept so the fog can reuse it: fog colour IS sky colour,
 * so distant geometry dissolves into the horizon instead of popping at the
 * far plane, and the two can never drift apart. */
static float  g_fog_rgb[3] = {0.0f, 0.0f, 0.0f};
static GLint  g_u_occ = -1, g_u_shadow = -1, g_u_soft = -1;
/* Coarse occupancy: one texel per 8x8x8 block of voxels, set when ANY voxel in
 * it is solid, so the shadow DDA can skip eight blocks at a time through the
 * open air most sun rays traverse. `g_occ_count` is the per-cell solid count,
 * which is what lets a block break clear a coarse texel exactly instead of
 * leaving it conservatively marked forever -- 16 KB, against the 4 MB fine copy
 * the shadow design deliberately does not retain. */
#define CF_OCC_CS 8
static GLuint    g_occ_c = 0;
static uint16_t *g_occ_count = NULL;
static int       g_occ_cw = 0, g_occ_ch = 0, g_occ_cd = 0;
static GLint     g_u_occ_c = -1;
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
    g_u_species   = glGetUniformLocation(g_prog, "u_species");
    g_u_myc_first = glGetUniformLocation(g_prog, "u_myc_first");
    g_u_myc_count = glGetUniformLocation(g_prog, "u_myc_count");
    g_u_branch    = glGetUniformLocation(g_prog, "u_branch");
    g_u_branch_scale = glGetUniformLocation(g_prog, "u_branch_scale");
    g_u_branch_sharp = glGetUniformLocation(g_prog, "u_branch_sharp");
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
    g_u_soft = glGetUniformLocation(g_prog, "u_soft");
    g_u_occ_c = glGetUniformLocation(g_prog, "u_occ_c");
    g_u_occ_off = glGetUniformLocation(g_prog, "u_occ_off");
    g_u_light = glGetUniformLocation(g_prog, "u_light");
    g_u_gpulight = glGetUniformLocation(g_prog, "u_gpulight");
    g_u_fog_density = glGetUniformLocation(g_prog, "u_fog_density");
    g_u_fog_color = glGetUniformLocation(g_prog, "u_fog_color");
    g_u_overcast = glGetUniformLocation(g_prog, "u_overcast");
    g_u_bolt = glGetUniformLocation(g_prog, "u_bolt");
    g_u_time = glGetUniformLocation(g_prog, "u_time");
    g_u_off = glGetUniformLocation(g_prog, "u_off");
    /* GL 4.1 is the ceiling on macOS (Apple GL over Metal): no compute shaders,
     * no SSBOs, no image load/store -- all of those want 4.3. */
    if (cf_debug()) fprintf(stderr, "cf: GL %s | GLSL %s | %s\n", glGetString(GL_VERSION), glGetString(GL_SHADING_LANGUAGE_VERSION), glGetString(GL_RENDERER));
    if (cf_debug()) fprintf(stderr, "cf: uniforms occ=%d shadow=%d sundir=%d unlit=%d\n", g_u_occ, g_u_shadow, g_u_sundir, g_u_unlit);
    glGenVertexArrays(1, &g_vao);
    glGenBuffers(CF_MAX_MESHES, g_vbo);
    glGenBuffers(CF_MAX_MESHES, g_vbo_b);
    glUseProgram(g_prog);
    glUniform1i(g_u_tex, 0);
    /* u_light must name a unit of its own even when the per-fragment light path
     * is off: left at the default 0 it collides with u_tex, a sampler2DArray, and
     * two sampler types on one unit make every draw incomplete -- the whole world
     * silently stops rendering. So the unit is claimed here and a 1x1x1 texture
     * put on it, which cf_gfx_upload_light replaces when the path is enabled. */
    glUniform1i(g_u_light, 3);
    {
        unsigned char dummy[2] = {0, 0};
        glGenTextures(1, &g_lt);
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_3D, g_lt);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage3D(GL_TEXTURE_3D, 0, GL_RG8, 1, 1, 1, 0, GL_RG, GL_UNSIGNED_BYTE, dummy);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glActiveTexture(GL_TEXTURE0);
    }
    glUniform1i(g_u_use_tex, 0);
    glUniform1i(g_u_cutout, 0);
    return 1;
}

/* Upload the first `nfloats` floats of a March NativeF32Arr into mesh slot `slot`. */
void cf_gfx_upload(int64_t slot, void *arr, int64_t nfloats) {
    if (slot < 0 || slot >= CF_MAX_MESHES) return;
    g_off[slot][0] = g_off[slot][1] = 0.0f;   /* baked at the current local origin */
    if (nfloats > narr_len(arr)) nfloats = narr_len(arr);
    g_vbo_cur[slot] ^= 1;
    glBindBuffer(GL_ARRAY_BUFFER, vbo_of(slot));
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(nfloats * 4), narr_data(arr), GL_STATIC_DRAW);
    if (cf_debug()) { const float *f = narr_data(arr); fprintf(stderr, "cf: upload slot=%lld nfloats=%lld arrlen=%lld first=%g %g %g %g %g %g %g glerr=%d\n", (long long)slot, (long long)nfloats, (long long)narr_len(arr), f[0],f[1],f[2],f[3],f[4],f[5],f[6], (int)glGetError()); }
}

/* The parts of one upload are staged in a scratch buffer and sent with ONE
 * glBufferSubData when the last of them lands (offset + n reaches the total
 * upload_begin announced). Sixteen sections were sixteen calls, and the drain
 * trace showed the driver charging 1-4 ms for some of them; one call is one
 * charge. A part sequence that never completes is flushed by the next begin. */
static float  *g_stage = NULL;
static int64_t g_stage_cap = 0, g_stage_total = 0, g_stage_filled = 0, g_stage_slot = -1;
static void cf_stage_flush(void) {
    if (g_stage_slot < 0 || g_stage_filled <= 0) { g_stage_slot = -1; return; }
    glBindBuffer(GL_ARRAY_BUFFER, vbo_of(g_stage_slot));
    glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)(g_stage_filled * 4), g_stage);
    g_stage_slot = -1; g_stage_filled = 0;
}
/* Assemble a VBO from several March buffers: reserve `nfloats` floats, then
 * copy parts at float offsets. GL 3.3 core / GLES 3.0: glBufferData(NULL) +
 * glBufferSubData. */
void cf_gfx_upload_begin(int64_t slot, int64_t nfloats) {
    if (slot < 0 || slot >= CF_MAX_MESHES) return;
    g_off[slot][0] = g_off[slot][1] = 0.0f;
    cf_stage_flush();
    if (nfloats > g_stage_cap) { free(g_stage); g_stage_cap = nfloats + nfloats / 2 + 1024; g_stage = (float *)malloc((size_t)g_stage_cap * 4); }
    g_stage_slot = nfloats > 0 ? slot : -1; g_stage_total = nfloats; g_stage_filled = 0;
    g_vbo_cur[slot] ^= 1;
    glBindBuffer(GL_ARRAY_BUFFER, vbo_of(slot));
    /* Reallocate only when the mesh outgrows the buffer, with headroom; a
     * smaller mesh reuses the store and the parts land by glBufferSubData
     * alone. The other buffer of the pair is the one the last frame drew, so
     * nothing here waits on the GPU. */
    int64_t *cap = &g_vbo_cap[g_vbo_cur[slot]][slot];
    if (nfloats > *cap) {
        *cap = nfloats + nfloats / 2 + 1024;
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(*cap * 4), NULL, GL_DYNAMIC_DRAW);
    }
}
void cf_gfx_upload_part(int64_t slot, int64_t offset, void *arr, int64_t nfloats) {
    if (slot < 0 || slot >= CF_MAX_MESHES || nfloats <= 0) return;
    if (nfloats > narr_len(arr)) nfloats = narr_len(arr);
    if (slot != g_stage_slot || offset + nfloats > g_stage_total) {
        /* not the upload in progress: send it straight */
        glBindBuffer(GL_ARRAY_BUFFER, vbo_of(slot));
        glBufferSubData(GL_ARRAY_BUFFER, (GLintptr)(offset * 4), (GLsizeiptr)(nfloats * 4), narr_data(arr));
        return;
    }
    memcpy(g_stage + offset, narr_data(arr), (size_t)nfloats * 4);
    if (offset + nfloats > g_stage_filled) g_stage_filled = offset + nfloats;
    if (g_stage_filled >= g_stage_total) cf_stage_flush();
}

/* GPU time for the frame, by timer query. Two queries ping-ponged so reading
 * one never stalls on the frame still in flight; cf_gfx_gpu_us returns the last
 * result that was ready. Diagnostic: the game is CPU-bound, so wall-clock fps
 * cannot see what the fragment shader costs. */
static GLuint g_tq[2] = {0, 0};
static int    g_tq_i = 0, g_tq_armed[2] = {0, 0};
static int64_t g_gpu_us = 0;
void cf_gfx_end_query(void) {
    if (g_tq[g_tq_i] && g_tq_armed[g_tq_i] == 2) { glEndQuery(GL_TIME_ELAPSED); g_tq_armed[g_tq_i] = 1; g_tq_i = 1 - g_tq_i; }
}
int64_t cf_gfx_gpu_us(void) { return g_gpu_us; }

void cf_gfx_begin_frame(double r, double g, double b) {
    if (!g_tq[0]) { glGenQueries(2, g_tq); }
    int other = 1 - g_tq_i;
    if (g_tq_armed[other]) {
        GLint ready = 0;
        glGetQueryObjectiv(g_tq[other], GL_QUERY_RESULT_AVAILABLE, &ready);
        if (ready) { GLuint64 ns = 0; glGetQueryObjectui64v(g_tq[other], GL_QUERY_RESULT, &ns); g_gpu_us = (int64_t)(ns / 1000); g_tq_armed[other] = 0; }
    }
    if (!g_tq_armed[g_tq_i]) { glBeginQuery(GL_TIME_ELAPSED, g_tq[g_tq_i]); g_tq_armed[g_tq_i] = 2; }

    g_fog_rgb[0] = (float)r; g_fog_rgb[1] = (float)g; g_fog_rgb[2] = (float)b;
    glClearColor((float)r, (float)g, (float)b, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glUseProgram(g_prog);
    glBindVertexArray(g_vao);
}

/* 16 floats, column-major, from a NativeF32Arr. */
void cf_gfx_set_view_proj(void *arr) {
    if (narr_len(arr) < 16) return;
    glUniformMatrix4fv(g_u_vp, 1, GL_FALSE, (const float *)narr_data(arr));
    if (cf_debug()) { const float *f = narr_data(arr); fprintf(stderr, "cf: vp loc=%d diag=%g %g %g %g glerr=%d\n", g_u_vp, f[0], f[5], f[10], f[15], (int)glGetError()); }
}

void cf_gfx_draw(int64_t slot, int64_t nverts) {
    if (slot < 0 || slot >= CF_MAX_MESHES || nverts <= 0) return;
    glUniform2f(g_u_off, g_off[slot][0], g_off[slot][1]);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_of(slot));
    GLsizei stride = CF_VERT_FLOATS * sizeof(float);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void *)0);
    glEnableVertexAttribArray(1); glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, (void *)(3 * 4));
    glEnableVertexAttribArray(2); glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, stride, (void *)(5 * 4));
    glEnableVertexAttribArray(3); glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, stride, (void *)(6 * 4));
    glEnableVertexAttribArray(4); glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, stride, (void *)(7 * 4));
    glEnableVertexAttribArray(5); glVertexAttribPointer(5, 1, GL_FLOAT, GL_FALSE, stride, (void *)(8 * 4));
    glEnableVertexAttribArray(6); glVertexAttribPointer(6, 1, GL_FLOAT, GL_FALSE, stride, (void *)(9 * 4));
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)nverts);
    if (cf_debug()) { fprintf(stderr, "cf: draw slot=%lld nverts=%lld glerr=%d prog=%u vao=%u\n", (long long)slot, (long long)nverts, (int)glGetError(), g_prog, g_vao); }
}

/* ── The window slid ──────────────────────────────────────────────────────
 * The world's window moved by (dx, dz) chunks (CubeForge.World.shift). The
 * chunk mesh slots -- three ranges of 64: opaque, water, foliage -- move
 * with their chunks: local slot d after the shift is what was at d + dx +
 * 8 dz, and its offset gains -16 dx, -16 dz because its vertices are still
 * baked where the chunk used to be. The handles of the slots that left are
 * handed to the slots that came in (their contents are stale until March
 * uploads a mesh, and March zeroes those slots' vertex counts). Every
 * particle, spray emitter and spring the shim holds is in window
 * coordinates too, so it moves 16 blocks the other way; a spring that left
 * the window is dropped. */
static void cf_shift_particles(int64_t dx, int64_t dz);   /* defined with the particle state, below */
void cf_gfx_shift(int64_t dx, int64_t dz) {
    /* A slot's state is its VBO pair, which of the two the last upload went
     * to, both capacities and its offset: all of it moves with the chunk. */
    for (int base = 0; base < 3 * CF_CHUNK_SLOTS; base += CF_CHUNK_SLOTS) {
        GLuint olda[CF_CHUNK_SLOTS], oldb[CF_CHUNK_SLOTS]; unsigned char oldcur[CF_CHUNK_SLOTS];
        int64_t oldcap[2][CF_CHUNK_SLOTS]; float oldoff[CF_CHUNK_SLOTS][2]; int used[CF_CHUNK_SLOTS];
        for (int i = 0; i < CF_CHUNK_SLOTS; i++) {
            olda[i] = g_vbo[base + i]; oldb[i] = g_vbo_b[base + i]; oldcur[i] = g_vbo_cur[base + i];
            oldcap[0][i] = g_vbo_cap[0][base + i]; oldcap[1][i] = g_vbo_cap[1][base + i];
            oldoff[i][0] = g_off[base + i][0]; oldoff[i][1] = g_off[base + i][1]; used[i] = 0;
        }
        int has[CF_CHUNK_SLOTS];
        for (int d = 0; d < CF_CHUNK_SLOTS; d++) {
            int64_t sx = d % CF_WORLD_CHUNKS + dx, sz = d / CF_WORLD_CHUNKS + dz;
            has[d] = 0;
            if (sx >= 0 && sx < CF_WORLD_CHUNKS && sz >= 0 && sz < CF_WORLD_CHUNKS) {
                int s = (int)(sx + CF_WORLD_CHUNKS * sz);
                g_vbo[base + d] = olda[s]; g_vbo_b[base + d] = oldb[s]; g_vbo_cur[base + d] = oldcur[s];
                g_vbo_cap[0][base + d] = oldcap[0][s]; g_vbo_cap[1][base + d] = oldcap[1][s];
                used[s] = 1; has[d] = 1;
                g_off[base + d][0] = oldoff[s][0] - 16.0f * (float)dx;
                g_off[base + d][1] = oldoff[s][1] - 16.0f * (float)dz;
            }
        }
        int u = 0;
        for (int d = 0; d < CF_CHUNK_SLOTS; d++) {
            if (has[d]) continue;
            while (u < CF_CHUNK_SLOTS && used[u]) u++;
            if (u >= CF_CHUNK_SLOTS) break;
            g_vbo[base + d] = olda[u]; g_vbo_b[base + d] = oldb[u]; g_vbo_cur[base + d] = oldcur[u];
            g_vbo_cap[0][base + d] = oldcap[0][u]; g_vbo_cap[1][base + d] = oldcap[1][u];
            used[u] = 1;
            g_off[base + d][0] = g_off[base + d][1] = 0.0f;
        }
    }
    cf_shift_particles(dx, dz);
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
    /* March, this file and the shader's WORLD constant all carry the window's
     * side independently; a mismatch would mis-index shadows silently. */
    if (w != CF_WORLD_SIDE || d != CF_WORLD_SIDE) {
        fprintf(stderr, "cf: occupancy is %lldx%lld but the shim is built for %d; "
                        "CF_WORLD_CHUNKS and CubeForge.World.size() disagree\n",
                (long long)w, (long long)d, CF_WORLD_SIDE);
        abort();
    }
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
    /* a whole upload is the window as it stands, so the toroidal origin resets */
    g_occ_w = w; g_occ_h = h; g_occ_d = d;
    g_occ_ox = 0; g_occ_oz = 0;
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_3D, g_occ);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage3D(GL_TEXTURE_3D, 0, GL_R8, (GLsizei)w, (GLsizei)h, (GLsizei)d, 0,
                 GL_RED, GL_UNSIGNED_BYTE, narr_data(arr));
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_REPEAT);
    /* Coarse level. Occupancy is indexed x + CF_WORLD_SIDE * (y + 256 * z), matching
     * CubeForge.Light.occ_index. */
    g_occ_cw = (int)((w + CF_OCC_CS - 1) / CF_OCC_CS);
    g_occ_ch = (int)((h + CF_OCC_CS - 1) / CF_OCC_CS);
    g_occ_cd = (int)((d + CF_OCC_CS - 1) / CF_OCC_CS);
    free(g_occ_count);
    g_occ_count = (uint16_t *)calloc((size_t)g_occ_cw * g_occ_ch * g_occ_cd, sizeof(uint16_t));
    unsigned char *coarse = (unsigned char *)calloc((size_t)g_occ_cw * g_occ_ch * g_occ_cd, 1);
    const unsigned char *fine = (const unsigned char *)narr_data(arr);
    for (int64_t z = 0; z < d; z++)
        for (int64_t y = 0; y < h; y++)
            for (int64_t x = 0; x < w; x++)
                if (fine[x + w * (y + h * z)] == 255) {   /* the byte is the light opacity; 255 alone is solid */
                    size_t ci = (size_t)(x / CF_OCC_CS)
                              + (size_t)g_occ_cw * ((size_t)(y / CF_OCC_CS)
                              + (size_t)g_occ_ch * (size_t)(z / CF_OCC_CS));
                    g_occ_count[ci]++;
                    coarse[ci] = 255;
                }
    if (!g_occ_c) glGenTextures(1, &g_occ_c);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_3D, g_occ_c);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage3D(GL_TEXTURE_3D, 0, GL_R8, g_occ_cw, g_occ_ch, g_occ_cd, 0,
                 GL_RED, GL_UNSIGNED_BYTE, coarse);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_REPEAT);
    free(coarse);
    glActiveTexture(GL_TEXTURE0);
    glUseProgram(g_prog);
    glUniform1i(g_u_occ, 1);
    glUniform1i(g_u_occ_c, 2);
    occ_push_off();
}

/* Window-local (x, z) to texel, through the toroidal origin. */
static inline int64_t occ_tx(int64_t x) { int64_t v = (x + g_occ_ox) % g_occ_w; return v < 0 ? v + g_occ_w : v; }
static inline int64_t occ_tz(int64_t z) { int64_t v = (z + g_occ_oz) % g_occ_d; return v < 0 ? v + g_occ_d : v; }

/* Upload a staged box, splitting it where it wraps. [stage] is bw x bh x bd in
 * the texture's own order; (tx, ty, tz) is its wrapped origin. Unpack strides
 * let each piece be sourced straight out of the one staging buffer. */
static void occ_sub_wrapped(GLuint tex, const unsigned char *stage,
                            int64_t tx, int64_t ty, int64_t tz,
                            int64_t bw, int64_t bh, int64_t bd, int64_t texw, int64_t texd) {
    glBindTexture(GL_TEXTURE_3D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, (GLint)bw);
    glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, (GLint)bh);
    int64_t xs[2], xw[2], zs[2], zd[2];
    int nx = 0, nz = 0;
    if (tx + bw <= texw) { xs[0] = 0; xw[0] = bw; nx = 1; }
    else { xs[0] = 0; xw[0] = texw - tx; xs[1] = xw[0]; xw[1] = bw - xw[0]; nx = 2; }
    if (tz + bd <= texd) { zs[0] = 0; zd[0] = bd; nz = 1; }
    else { zs[0] = 0; zd[0] = texd - tz; zs[1] = zd[0]; zd[1] = bd - zd[0]; nz = 2; }
    for (int i = 0; i < nx; i++)
        for (int j = 0; j < nz; j++) {
            glPixelStorei(GL_UNPACK_SKIP_PIXELS, (GLint)xs[i]);
            glPixelStorei(GL_UNPACK_SKIP_IMAGES, (GLint)zs[j]);
            glTexSubImage3D(GL_TEXTURE_3D, 0,
                            (GLint)(i == 0 ? tx : 0), (GLint)ty, (GLint)(j == 0 ? tz : 0),
                            (GLsizei)xw[i], (GLsizei)bh, (GLsizei)zd[j],
                            GL_RED, GL_UNSIGNED_BYTE, stage);
        }
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_UNPACK_SKIP_IMAGES, 0);
}

/* The window slid by (dx, dz) chunks: advance the texture origin. The caller
 * then syncs the band that came in, which lands on the texels the band that
 * left was using. Nothing else moves, and nothing else is uploaded. */
/* ── PROTOTYPE: the light fields as a 3D texture ──────────────────────────────
 * Measurement only, for deciding whether per-fragment smooth lighting is
 * affordable. Sky in R, block in G, toroidal with the occupancy textures (the
 * light fields slide with them), on unit 3. A whole upload per shift; if this
 * path were kept it would sync the band like the occupancy does. */
void cf_gfx_upload_light(void *la, void *lb, int64_t w, int64_t h, int64_t d) {
    if (narr_len(la) < w * h * d || narr_len(lb) < w * h * d) return;
    const unsigned char *a = (const unsigned char *)narr_data(la);
    const unsigned char *b = (const unsigned char *)narr_data(lb);
    size_t n = (size_t)w * h * d;
    unsigned char *rg = (unsigned char *)malloc(n * 2);
    if (!rg) return;
    /* light is x + w * (z + d * y); the texture wants x fastest, then y, then z,
     * matching the occupancy layout the shader already addresses */
    for (int64_t z = 0; z < d; z++)
        for (int64_t y = 0; y < h; y++)
            for (int64_t x = 0; x < w; x++) {
                size_t src = (size_t)x + w * ((size_t)z + d * (size_t)y);
                size_t dst = ((size_t)x + w * ((size_t)y + h * (size_t)z)) * 2;
                rg[dst] = (unsigned char)(a[src] * 17);   /* 0..15 -> 0..255 */
                rg[dst + 1] = (unsigned char)(b[src] * 17);
            }
    if (!g_lt) glGenTextures(1, &g_lt);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_3D, g_lt);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage3D(GL_TEXTURE_3D, 0, GL_RG8, (GLsizei)w, (GLsizei)h, (GLsizei)d, 0, GL_RG, GL_UNSIGNED_BYTE, rg);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_REPEAT);
    free(rg);
    if (g_u_light >= 0) glUniform1i(g_u_light, 3);
    glActiveTexture(GL_TEXTURE0);
}
void cf_gfx_set_gpulight(int64_t on) { if (g_u_gpulight >= 0) glUniform1i(g_u_gpulight, (int)on); }

/* Sync a box of the light texture from the two light fields, through the same
 * toroidal wrap the occupancy uses. The fields are indexed
 * x + w * (z + d * y) (CubeForge.Light.index); the texture wants x fastest then
 * y then z, so the box is staged in the texture's order and uploaded in at most
 * four pieces where it straddles the seam. */
/* Relights land many times a frame -- a retexture slot alone is dozens of
 * single-column edits -- and each one used to stage and upload its own box,
 * which cost more than the whole per-fragment change saved (mean frame 4.1 ->
 * 10.6 ms). So a relight only WIDENS a dirty box here, and the frame flushes it
 * once before drawing, the way the mesh drain batches its work. */
static int64_t g_ld[6];
static int     g_ld_any = 0;
void cf_gfx_light_dirty(int64_t x0, int64_t y0, int64_t z0, int64_t x1, int64_t y1, int64_t z1) {
    if (x1 < x0 || y1 < y0 || z1 < z0) return;
    if (!g_ld_any) { g_ld[0] = x0; g_ld[1] = y0; g_ld[2] = z0; g_ld[3] = x1; g_ld[4] = y1; g_ld[5] = z1; g_ld_any = 1; return; }
    if (x0 < g_ld[0]) g_ld[0] = x0;
    if (y0 < g_ld[1]) g_ld[1] = y0;
    if (z0 < g_ld[2]) g_ld[2] = z0;
    if (x1 > g_ld[3]) g_ld[3] = x1;
    if (y1 > g_ld[4]) g_ld[4] = y1;
    if (z1 > g_ld[5]) g_ld[5] = z1;
}
void cf_gfx_sync_light_box(void *la, void *lb, int64_t x0, int64_t y0, int64_t z0, int64_t x1, int64_t y1, int64_t z1);
void cf_gfx_flush_light(void *la, void *lb) {
    if (!g_ld_any) return;
    g_ld_any = 0;
    cf_gfx_sync_light_box(la, lb, g_ld[0], g_ld[1], g_ld[2], g_ld[3], g_ld[4], g_ld[5]);
}

void cf_gfx_sync_light_box(void *la, void *lb, int64_t x0, int64_t y0, int64_t z0, int64_t x1, int64_t y1, int64_t z1) {
    if (!g_lt || !g_occ_w) return;
    if (narr_len(la) < g_occ_w * g_occ_h * g_occ_d || narr_len(lb) < g_occ_w * g_occ_h * g_occ_d) return;
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (z0 < 0) z0 = 0;
    if (x1 >= g_occ_w) x1 = g_occ_w - 1; if (y1 >= g_occ_h) y1 = g_occ_h - 1; if (z1 >= g_occ_d) z1 = g_occ_d - 1;
    if (x0 > x1 || y0 > y1 || z0 > z1) return;
    const unsigned char *a = (const unsigned char *)narr_data(la);
    const unsigned char *b = (const unsigned char *)narr_data(lb);
    int64_t bw = x1 - x0 + 1, bh = y1 - y0 + 1, bd = z1 - z0 + 1;
    unsigned char *stage = (unsigned char *)malloc((size_t)(bw * bh * bd * 2));
    if (!stage) return;
    for (int64_t z = z0; z <= z1; z++)
        for (int64_t y = y0; y <= y1; y++)
            for (int64_t x = x0; x <= x1; x++) {
                size_t src = (size_t)x + g_occ_w * ((size_t)z + g_occ_d * (size_t)y);
                size_t dst = ((size_t)(x - x0) + bw * ((size_t)(y - y0) + bh * (size_t)(z - z0))) * 2;
                stage[dst] = (unsigned char)(a[src] * 17);
                stage[dst + 1] = (unsigned char)(b[src] * 17);
            }
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_3D, g_lt);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, (GLint)bw);
    glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, (GLint)bh);
    int64_t tx = occ_tx(x0), tz = occ_tz(z0);
    int64_t xs[2], xw[2], zs[2], zd[2]; int nx, nz;
    if (tx + bw <= g_occ_w) { xs[0] = 0; xw[0] = bw; nx = 1; }
    else { xs[0] = 0; xw[0] = g_occ_w - tx; xs[1] = xw[0]; xw[1] = bw - xw[0]; nx = 2; }
    if (tz + bd <= g_occ_d) { zs[0] = 0; zd[0] = bd; nz = 1; }
    else { zs[0] = 0; zd[0] = g_occ_d - tz; zs[1] = zd[0]; zd[1] = bd - zd[0]; nz = 2; }
    for (int i = 0; i < nx; i++)
        for (int j = 0; j < nz; j++) {
            glPixelStorei(GL_UNPACK_SKIP_PIXELS, (GLint)xs[i]);
            glPixelStorei(GL_UNPACK_SKIP_IMAGES, (GLint)zs[j]);
            glTexSubImage3D(GL_TEXTURE_3D, 0, (GLint)(i == 0 ? tx : 0), (GLint)y0, (GLint)(j == 0 ? tz : 0),
                            (GLsizei)xw[i], (GLsizei)bh, (GLsizei)zd[j], GL_RG, GL_UNSIGNED_BYTE, stage);
        }
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_UNPACK_SKIP_IMAGES, 0);
    free(stage);
    glActiveTexture(GL_TEXTURE0);
}

static void occ_push_off(void) {
    if (g_u_occ_off >= 0) glUniform2f(g_u_occ_off, (float)g_occ_ox, (float)g_occ_oz);
}
void cf_gfx_shift_occupancy(int64_t dx, int64_t dz) {
    if (!g_occ_w || !g_occ_d) return;
    g_occ_ox = ((g_occ_ox + 16 * dx) % g_occ_w + g_occ_w) % g_occ_w;
    g_occ_oz = ((g_occ_oz + 16 * dz) % g_occ_d + g_occ_d) % g_occ_d;
    occ_push_off();
}

/* One voxel changed: a single texel beats rebuilding 4 MB. */
void cf_gfx_set_voxel(int64_t x, int64_t y, int64_t z, int64_t solid) {
    if (!g_occ) return;
    if (x < 0 || y < 0 || z < 0 || x >= g_occ_w || y >= g_occ_h || z >= g_occ_d) return;
    unsigned char v = solid ? 255 : 0;   /* normalized R8: 255 samples as 1.0 */
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_3D, g_occ);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage3D(GL_TEXTURE_3D, 0, (GLint)occ_tx(x), (GLint)y, (GLint)occ_tz(z), 1, 1, 1,
                    GL_RED, GL_UNSIGNED_BYTE, &v);
    /* Keep the coarse level exact. The count is what makes a break able to clear
     * a coarse texel: without it a broken block could only be handled
     * conservatively and the cell would stay marked solid forever. */
    if (g_occ_count && g_occ_c) {
        size_t ci = (size_t)(occ_tx(x) / CF_OCC_CS)
                  + (size_t)g_occ_cw * ((size_t)(y / CF_OCC_CS)
                  + (size_t)g_occ_ch * (size_t)(occ_tz(z) / CF_OCC_CS));
        uint16_t before = g_occ_count[ci];
        if (solid) g_occ_count[ci]++;
        else if (g_occ_count[ci]) g_occ_count[ci]--;
        if ((before > 0) != (g_occ_count[ci] > 0)) {
            unsigned char cv = g_occ_count[ci] ? 255 : 0;
            glActiveTexture(GL_TEXTURE2);
            glBindTexture(GL_TEXTURE_3D, g_occ_c);
            glTexSubImage3D(GL_TEXTURE_3D, 0, (GLint)(occ_tx(x) / CF_OCC_CS), (GLint)(y / CF_OCC_CS),
                            (GLint)(occ_tz(z) / CF_OCC_CS), 1, 1, 1, GL_RED, GL_UNSIGNED_BYTE, &cv);
        }
    }
    glActiveTexture(GL_TEXTURE0);
}

/* Sync the box [x0..x1] x [y0..y1] x [z0..z1] of the occupancy texture from
 * the world's occupancy array (x + CF_WORLD_SIDE * (y + 256 * z), the texture's own
 * layout, so one glTexSubImage3D with unpack strides does the box), then
 * recount every coarse cell the box touches from the array. A tree edit used
 * to make 729 one-texel calls here (0.6 ms), and each call bumped the coarse
 * count whether or not the voxel had changed. The recount is exact. */
void cf_gfx_sync_box(void *arr, int64_t x0, int64_t y0, int64_t z0, int64_t x1, int64_t y1, int64_t z1) {
    if (!g_occ) return;
    if (narr_len(arr) < g_occ_w * g_occ_h * g_occ_d) return;
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (z0 < 0) z0 = 0;
    if (x1 >= g_occ_w) x1 = g_occ_w - 1; if (y1 >= g_occ_h) y1 = g_occ_h - 1; if (z1 >= g_occ_d) z1 = g_occ_d - 1;
    if (x0 > x1 || y0 > y1 || z0 > z1) return;
    const unsigned char *a = (const unsigned char *)narr_data(arr);
    /* the fine texture: the box straight out of the array. The array holds
     * 0 / occ_solid; the texture wants 0 / 255, so stage the box. */
    int64_t bw = x1 - x0 + 1, bh = y1 - y0 + 1, bd = z1 - z0 + 1;
    unsigned char *stage = (unsigned char *)malloc((size_t)(bw * bh * bd));
    if (!stage) return;
    for (int64_t z = z0; z <= z1; z++)
        for (int64_t y = y0; y <= y1; y++)
            for (int64_t x = x0; x <= x1; x++)
                stage[(x - x0) + bw * ((y - y0) + bh * (z - z0))] = a[x + g_occ_w * (y + g_occ_h * z)] == 255 ? 255 : 0;
    glActiveTexture(GL_TEXTURE1);
    occ_sub_wrapped(g_occ, stage, occ_tx(x0), y0, occ_tz(z0), bw, bh, bd, g_occ_w, g_occ_d);
    free(stage);
    if (g_occ_count && g_occ_c) {
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_3D, g_occ_c);
        for (int64_t cz = z0 / CF_OCC_CS; cz <= z1 / CF_OCC_CS; cz++)
            for (int64_t cy = y0 / CF_OCC_CS; cy <= y1 / CF_OCC_CS; cy++)
                for (int64_t cx = x0 / CF_OCC_CS; cx <= x1 / CF_OCC_CS; cx++) {
                    uint16_t n = 0;
                    for (int64_t z = cz * CF_OCC_CS; z < (cz + 1) * CF_OCC_CS && z < g_occ_d; z++)
                        for (int64_t y = cy * CF_OCC_CS; y < (cy + 1) * CF_OCC_CS && y < g_occ_h; y++)
                            for (int64_t x = cx * CF_OCC_CS; x < (cx + 1) * CF_OCC_CS && x < g_occ_w; x++)
                                if (a[x + g_occ_w * (y + g_occ_h * z)] == 255) n++;
                    /* the coarse level is toroidal with the fine one; the origin is
                     * a multiple of 16 so it lands on a whole number of cells */
                    int64_t tcx = occ_tx(cx * CF_OCC_CS) / CF_OCC_CS;
                    int64_t tcz = occ_tz(cz * CF_OCC_CS) / CF_OCC_CS;
                    size_t ci = (size_t)tcx + (size_t)g_occ_cw * ((size_t)cy + (size_t)g_occ_ch * (size_t)tcz);
                    uint16_t before = g_occ_count[ci];
                    g_occ_count[ci] = n;
                    if ((before > 0) != (n > 0)) {
                        unsigned char cv = n ? 255 : 0;
                        glTexSubImage3D(GL_TEXTURE_3D, 0, (GLint)tcx, (GLint)cy, (GLint)tcz, 1, 1, 1, GL_RED, GL_UNSIGNED_BYTE, &cv);
                    }
                }
    }
    glActiveTexture(GL_TEXTURE0);
}

/* Oracle for the toroidal occupancy textures: read both levels back and check
 * every texel against the world's occupancy array, through the same wrap the
 * shader uses. Returns fine_mismatches * 1000000 + coarse_mismatches, so one
 * call answers both. Diagnostic only -- glGetTexImage stalls the pipeline. */
int64_t cf_occ_check(void *arr) {
    if (!g_occ || !g_occ_count || !g_occ_c) return -1;
    if (narr_len(arr) < g_occ_w * g_occ_h * g_occ_d) return -2;
    const unsigned char *a = (const unsigned char *)narr_data(arr);
    size_t nf = (size_t)g_occ_w * g_occ_h * g_occ_d;
    unsigned char *tex = (unsigned char *)malloc(nf);
    if (!tex) return -3;
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_3D, g_occ);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glGetTexImage(GL_TEXTURE_3D, 0, GL_RED, GL_UNSIGNED_BYTE, tex);
    int64_t bad = 0;
    for (int64_t z = 0; z < g_occ_d; z++)
        for (int64_t y = 0; y < g_occ_h; y++)
            for (int64_t x = 0; x < g_occ_w; x++) {
                /* Solidity, not the raw byte: the array holds the light opacity
                 * (2 water, 6 leaves) and cf_gfx_upload_occupancy uploads it
                 * verbatim while cf_gfx_sync_box normalises to 0/255. The shader
                 * only ever asks `> 0.5`, so that is what has to agree. */
                int want = a[x + g_occ_w * (y + g_occ_h * z)] > 127;
                int got  = tex[occ_tx(x) + g_occ_w * (y + g_occ_h * occ_tz(z))] > 127;
                if (want != got) bad++;
            }
    free(tex);
    size_t nc = (size_t)g_occ_cw * g_occ_ch * g_occ_cd;
    unsigned char *ctex = (unsigned char *)malloc(nc);
    int64_t cbad = 0;
    if (ctex) {
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_3D, g_occ_c);
        glGetTexImage(GL_TEXTURE_3D, 0, GL_RED, GL_UNSIGNED_BYTE, ctex);
        for (int64_t cz = 0; cz < g_occ_cd; cz++)
            for (int64_t cy = 0; cy < g_occ_ch; cy++)
                for (int64_t cx = 0; cx < g_occ_cw; cx++) {
                    int64_t n = 0;
                    for (int64_t z = cz * CF_OCC_CS; z < (cz + 1) * CF_OCC_CS && z < g_occ_d; z++)
                        for (int64_t y = cy * CF_OCC_CS; y < (cy + 1) * CF_OCC_CS && y < g_occ_h; y++)
                            for (int64_t x = cx * CF_OCC_CS; x < (cx + 1) * CF_OCC_CS && x < g_occ_w; x++)
                                if (a[x + g_occ_w * (y + g_occ_h * z)] == 255) n++;
                    int64_t tcx = occ_tx(cx * CF_OCC_CS) / CF_OCC_CS, tcz = occ_tz(cz * CF_OCC_CS) / CF_OCC_CS;
                    unsigned char got = ctex[tcx + g_occ_cw * (cy + g_occ_ch * tcz)];
                    if ((n > 0) != (got > 0)) cbad++;
                }
        free(ctex);
    }
    glActiveTexture(GL_TEXTURE0);
    return bad * 1000000 + cbad;
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

/* ── Precipitation ──────────────────────────────────────────────────────────
 * The particle pool and its geometry live here rather than in March, and the
 * reason is measured, not stylistic: March's NativeArray writes are functional,
 * and on an array this size each write copies the whole thing. The pool step
 * alone — three writes per particle over a 16,000-float array — ran 4000
 * particles at 5.9 fps, and the geometry build on a 216,000-float array was
 * worse: the process was SIGKILLed for memory. Both loops are O(n^2) in March
 * and O(n) here.
 *
 * March keeps the policy — how hard it is raining, whether it is snow, the pool
 * capacity — and this owns only the per-particle loop.
 *
 * Four floats per particle: x, y, z, vy. Six vertices per particle, nine floats
 * each, matching the global vertex layout with effect 1 in the fx word. */
#define CF_PRECIP_VTX  (6 * CF_VERT_FLOATS)
static float  *g_pcl = NULL;      /* pool: 4 floats per particle */
static float  *g_pcl_vtx = NULL;  /* geometry: CF_PRECIP_VTX floats per particle */
static int64_t g_pcl_cap = 0;

/* Rain falls fast and straight, snow slowly and sideways. */
#define CF_RAIN_SPEED   28.0f
#define CF_SNOW_SPEED    2.5f
#define CF_PCL_RADIUS   24.0f     /* cylinder the pool lives in, around the eye */
#define CF_PCL_CEILING  20.0f     /* spawn height above the eye */
/* A drop half a block from the near plane covers the whole screen. Without this
 * a handful of them cost more fill than the entire world, and they read as
 * white bars rather than rain. */
#define CF_PCL_NEAR      1.2f

/* Deterministic hash in [0,1). Fixed constants, no global state: the same tick
 * and index always give the same number, so CF_DUMP_FRAME still reproduces. */
static float pcl_rnd(int64_t a, int64_t b) {
    uint64_t h = (uint64_t)a * 0x9E3779B97F4A7C15ull ^ (uint64_t)b * 0xC2B2AE3D27D4EB4Full;
    h ^= h >> 29; h *= 0xBF58476D1CE4E5B9ull; h ^= h >> 32;
    return (float)((h >> 40) & 0xFFFFFF) / 16777216.0f;
}

/* Skylight lookup, matching CubeForge.Light.index: x + CF_WORLD_SIDE * (z + CF_WORLD_SIDE * y),
 * and 0 (sealed) outside the world. */
static int pcl_sky(const unsigned char *la, int x, int y, int z) {
    if (x < 0 || x >= CF_WORLD_SIDE || y < 0 || y >= 256 || z < 0 || z >= CF_WORLD_SIDE) return 0;
    return la[(size_t)x + CF_WORLD_SIDE * ((size_t)z + CF_WORLD_SIDE * (size_t)y)];
}

static void pcl_respawn(int64_t i, float ex, float ey, float ez, int64_t tick) {
    float ang = pcl_rnd(i, tick) * 6.283185307f;
    float rad = sqrtf(pcl_rnd(i + 7919, tick)) * CF_PCL_RADIUS;
    g_pcl[i * 4 + 0] = ex + rad * cosf(ang);
    g_pcl[i * 4 + 1] = ey + CF_PCL_CEILING * pcl_rnd(i + 104729, tick);
    g_pcl[i * 4 + 2] = ez + rad * sinf(ang);
    g_pcl[i * 4 + 3] = 0.0f;
}

void cf_precip_init(int64_t cap) {
    if (cap < 0) cap = 0;
    if (cap > 1 << 20) cap = 1 << 20;
    free(g_pcl); free(g_pcl_vtx);
    g_pcl_cap = cap;
    g_pcl = cap ? (float *)calloc((size_t)cap * 4, sizeof(float)) : NULL;
    g_pcl_vtx = cap ? (float *)calloc((size_t)cap * CF_PRECIP_VTX, sizeof(float)) : NULL;
    /* Below the world floor, so the first frame respawns everything around
     * wherever the camera actually is rather than at the origin. */
    for (int64_t i = 0; i < cap; i++) g_pcl[i * 4 + 1] = -1.0f;
}

static void pcl_vert(float *v, float x, float y, float z,
                     float r, float g, float b, float fx) {
    v[0] = x; v[1] = y; v[2] = z;
    v[3] = r; v[4] = g; v[5] = b;   /* untextured colour rides in uv + layer */
    v[6] = 1.0f;                    /* shade: unlit, so this is a pass-through */
    v[7] = 0.0f;                    /* face */
    v[8] = fx;                      /* effect 1 + alpha, packed */
    v[9] = 0.0f;                    /* block light: overlays and particles carry none */
}

/* Step every live particle, rebuild the geometry, and upload it. One call per
 * frame from the frame loop. `snow` picks the look and the fall speed; `rx, rz`
 * is the camera's right vector flattened to the horizontal plane. */
void cf_precip_frame(void *light, int64_t live, int64_t snow,
                     double ex, double ey, double ez,
                     double rx, double rz, double dt, int64_t tick) {
    if (!g_pcl || live <= 0) return;
    if (live > g_pcl_cap) live = g_pcl_cap;
    const unsigned char *la = light ? (const unsigned char *)narr_data(light) : NULL;

    float speed = snow ? CF_SNOW_SPEED : CF_RAIN_SPEED;
    float hw    = snow ? 0.06f : 0.025f;
    float hh    = snow ? 0.06f : 0.45f;
    float base_a = snow ? 0.85f : 0.45f;
    float cr = snow ? 0.97f : 0.72f, cg = snow ? 0.98f : 0.80f, cb = snow ? 1.00f : 0.92f;
    float ax = (float)rx * hw, az = (float)rz * hw;

    for (int64_t i = 0; i < live; i++) {
        float x = g_pcl[i * 4 + 0], y = g_pcl[i * 4 + 1], z = g_pcl[i * 4 + 2];
        /* Snow wanders sideways; rain falls straight. */
        float drift = snow ? sinf((float)tick * 0.02f + (float)i) * 0.6f : 0.0f;
        x += drift * (float)dt;
        y -= speed * (float)dt;

        float dx = x - (float)ex, dz = z - (float)ez;
        int out = dx * dx + dz * dz > CF_PCL_RADIUS * CF_PCL_RADIUS
               || y < 0.0f || y > (float)ey + CF_PCL_CEILING;
        /* The kill that matters: a cell with no skylight is under a roof, in a
         * cave, or inside the terrain. Rain never falls indoors, and unlike a
         * heightmap query this respects player edits as soon as they relight. */
        int sealed = la && pcl_sky(la, (int)floorf(x), (int)floorf(y), (int)floorf(z)) == 0;
        if (out || sealed) {
            pcl_respawn(i, (float)ex, (float)ey, (float)ez, tick);
            x = g_pcl[i * 4 + 0]; y = g_pcl[i * 4 + 1]; z = g_pcl[i * 4 + 2];
        } else {
            g_pcl[i * 4 + 0] = x; g_pcl[i * 4 + 1] = y;
        }

        /* Fade out toward the cylinder's edge so particles do not pop in and
         * out of existence at the boundary. */
        dx = x - (float)ex; dz = z - (float)ez;
        float dy = y - (float)ey;
        float hd = sqrtf(dx * dx + dz * dz) / CF_PCL_RADIUS;
        float a = hd > 0.75f ? (1.0f - hd) * 4.0f : 1.0f;
        if (a < 0.0f) a = 0.0f;
        if (a > 1.0f) a = 1.0f;
        /* Near-culled particles collapse to a zero-area quad, which the GPU
         * discards before shading — cheaper than a branch in the draw call and
         * it keeps the indexing pure arithmetic. */
        if (dx * dx + dy * dy + dz * dz < CF_PCL_NEAR * CF_PCL_NEAR) a = 0.0f;
        /* Killing a sealed particle is not enough on its own: respawning puts it
         * somewhere else in the same cylinder, and underground EVERY cell is
         * sealed, so the pool would refill instantly and rain inside solid rock.
         * The draw has to be gated on where the particle actually ended up. */
        if (la && pcl_sky(la, (int)floorf(x), (int)floorf(y), (int)floorf(z)) == 0) a = 0.0f;

        float fx = (float)(1 * 256 + (int)(a * base_a * 255.0f + 0.5f));
        float *v = g_pcl_vtx + i * CF_PRECIP_VTX;
        float zero = (a == 0.0f) ? 0.0f : 1.0f;
        float qx = zero * ax, qz = zero * az, qh = zero * hh;
        pcl_vert(v + 0 * CF_VERT_FLOATS, x - qx, y - qh, z - qz, cr, cg, cb, fx);
        pcl_vert(v + 1 * CF_VERT_FLOATS, x + qx, y - qh, z + qz, cr, cg, cb, fx);
        pcl_vert(v + 2 * CF_VERT_FLOATS, x + qx, y + qh, z + qz, cr, cg, cb, fx);
        pcl_vert(v + 3 * CF_VERT_FLOATS, x - qx, y - qh, z - qz, cr, cg, cb, fx);
        pcl_vert(v + 4 * CF_VERT_FLOATS, x + qx, y + qh, z + qz, cr, cg, cb, fx);
        pcl_vert(v + 5 * CF_VERT_FLOATS, x - qx, y + qh, z - qz, cr, cg, cb, fx);
    }

    glBindBuffer(GL_ARRAY_BUFFER, g_vbo[CF_PRECIP_SLOT]);
    glBufferData(GL_ARRAY_BUFFER,
                 (GLsizeiptr)(live * CF_PRECIP_VTX * (int64_t)sizeof(float)),
                 g_pcl_vtx, GL_STREAM_DRAW);
}

/* ── Biome map ──────────────────────────────────────────────────────────────
 * One flat coloured quad per column, built here rather than in March: 16,384
 * quads is 98k vertices, far past what March can push per tick (GAPS G68). Drawn
 * through the marker path (unlit, untextured, depth-tested) at y = 259, under
 * the marker at 260 and above any terrain. */
#define CF_BIOME_SLOT 504
#define CF_BIOME_Y    259.0f
/* Order matches CubeForge.Biome: tundra, taiga, grassland, forest, desert,
 * wetland, beach, alpine, oasis, grove. */
#define CF_BIOME_COUNT 10
static const float CF_BIOME_RGB[CF_BIOME_COUNT][3] = {
    {0.86f, 0.90f, 0.95f}, {0.25f, 0.45f, 0.35f}, {0.55f, 0.75f, 0.30f}, {0.15f, 0.50f, 0.15f},
    {0.90f, 0.80f, 0.45f}, {0.35f, 0.55f, 0.50f}, {0.95f, 0.90f, 0.70f}, {0.60f, 0.60f, 0.62f},
    {0.30f, 0.85f, 0.35f}, {0.55f, 0.30f, 0.70f},
};
static float  *g_biome_vtx = NULL;
static int64_t g_biome_cap = 0;

/* [active] is the biome field's per-column active flag -- the dirty set the tick
 * keeps so it can skip settled columns. It is exactly "this column is still
 * changing", so the survey's drift marking is free: no new pass, no new state,
 * just a lighter colour where the world is in motion. */
void cf_biome_map_upload(void *biomes, void *active, int64_t n) {
    int64_t cells = n * n;
    if (cells > g_biome_cap) {
        free(g_biome_vtx);
        g_biome_vtx = (float *)malloc((size_t)cells * 6 * CF_VERT_FLOATS * sizeof(float));
        g_biome_cap = cells;
    }
    const unsigned char *b = (const unsigned char *)narr_data(biomes);
    const unsigned char *act = (const unsigned char *)narr_data(active);
    for (int64_t i = 0; i < cells; i++) {
        float x0 = (float)(i % n), z0 = (float)(i / n), x1 = x0 + 1.0f, z1 = z0 + 1.0f;
        const float *cb = CF_BIOME_RGB[b[i] < CF_BIOME_COUNT ? b[i] : 0];
        /* drifting columns lift toward white, so an irrigated valley lights up
         * as it changes and goes quiet as it settles */
        float lit[3];
        if (act && act[i]) { for (int k = 0; k < 3; k++) lit[k] = cb[k] + (1.0f - cb[k]) * 0.55f; }
        else               { for (int k = 0; k < 3; k++) lit[k] = cb[k]; }
        const float *c = lit;
        float *v = g_biome_vtx + i * 6 * CF_VERT_FLOATS;
        /* winding matches the mesher's top face: (x0,z0)->(x0,z1)->(x1,z1)->(x1,z0) */
        pcl_vert(v + 0 * CF_VERT_FLOATS, x0, CF_BIOME_Y, z0, c[0], c[1], c[2], 255.0f);
        pcl_vert(v + 1 * CF_VERT_FLOATS, x0, CF_BIOME_Y, z1, c[0], c[1], c[2], 255.0f);
        pcl_vert(v + 2 * CF_VERT_FLOATS, x1, CF_BIOME_Y, z1, c[0], c[1], c[2], 255.0f);
        pcl_vert(v + 3 * CF_VERT_FLOATS, x0, CF_BIOME_Y, z0, c[0], c[1], c[2], 255.0f);
        pcl_vert(v + 4 * CF_VERT_FLOATS, x1, CF_BIOME_Y, z1, c[0], c[1], c[2], 255.0f);
        pcl_vert(v + 5 * CF_VERT_FLOATS, x1, CF_BIOME_Y, z0, c[0], c[1], c[2], 255.0f);
    }
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo[CF_BIOME_SLOT]);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(cells * 6 * CF_VERT_FLOATS * (int64_t)sizeof(float)), g_biome_vtx, GL_STATIC_DRAW);
}

/* ── Mycelium map overlay ──────────────────────────────────────────────────
 * Same shape as the biome map, one layer above it. Species colours mirror
 * CubeForge.Species.colour_*; brightness is vigour. A column with no species
 * gets a degenerate quad so the draw count stays n*n*6. */
#define CF_MYC_SLOT 500
static const float CF_MYC_RGB[7][3] = {
    {0.0f, 0.0f, 0.0f},
    {200/255.0f, 230/255.0f, 255/255.0f}, {120/255.0f, 90/255.0f, 60/255.0f}, {230/255.0f, 200/255.0f, 90/255.0f},
    {255/255.0f, 200/255.0f, 80/255.0f},  {80/255.0f, 220/255.0f, 200/255.0f}, {240/255.0f, 140/255.0f, 60/255.0f},
};
static float  *g_myc_vtx = NULL;
static int64_t g_myc_cap = 0;

void cf_myc_map_upload(void *species, void *vigour, int64_t n) {
    int64_t cells = n * n;
    if (cells > g_myc_cap) {
        free(g_myc_vtx);
        g_myc_vtx = (float *)malloc((size_t)cells * 6 * CF_VERT_FLOATS * sizeof(float));
        g_myc_cap = cells;
    }
    const unsigned char *sp = (const unsigned char *)narr_data(species);
    const unsigned char *vg = (const unsigned char *)narr_data(vigour);
    const float y = CF_BIOME_Y + 0.5f;
    for (int64_t i = 0; i < cells; i++) {
        float *v = g_myc_vtx + i * 6 * CF_VERT_FLOATS;
        int s = sp[i];
        if (s == 0 || s > 6) {
            for (int k = 0; k < 6; k++) pcl_vert(v + k * CF_VERT_FLOATS, 0.0f, y, 0.0f, 0.0f, 0.0f, 0.0f, 255.0f);
            continue;
        }
        float b = 0.3f + 0.7f * (float)vg[i] / 255.0f;
        float r = CF_MYC_RGB[s][0] * b, g = CF_MYC_RGB[s][1] * b, bl = CF_MYC_RGB[s][2] * b;
        float x0 = (float)(i % n), z0 = (float)(i / n), x1 = x0 + 1.0f, z1 = z0 + 1.0f;
        pcl_vert(v + 0 * CF_VERT_FLOATS, x0, y, z0, r, g, bl, 255.0f);
        pcl_vert(v + 1 * CF_VERT_FLOATS, x0, y, z1, r, g, bl, 255.0f);
        pcl_vert(v + 2 * CF_VERT_FLOATS, x1, y, z1, r, g, bl, 255.0f);
        pcl_vert(v + 3 * CF_VERT_FLOATS, x0, y, z0, r, g, bl, 255.0f);
        pcl_vert(v + 4 * CF_VERT_FLOATS, x1, y, z1, r, g, bl, 255.0f);
        pcl_vert(v + 5 * CF_VERT_FLOATS, x1, y, z0, r, g, bl, 255.0f);
    }
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo[CF_MYC_SLOT]);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(cells * 6 * CF_VERT_FLOATS * (int64_t)sizeof(float)), g_myc_vtx, GL_STATIC_DRAW);
}

/* ── Springs ────────────────────────────────────────────────────────────────
 * Every source block above sea level, kept here beside the particle pool that
 * will bubble at them. March scans once at startup and reports edits. */
#define CF_MAX_SPRINGS 512
static int32_t g_springs[CF_MAX_SPRINGS][3];
static int64_t g_nsprings = 0;
void cf_spring_set(int64_t x, int64_t y, int64_t z, int64_t on) {
    for (int64_t i = 0; i < g_nsprings; i++) {
        if (g_springs[i][0] == x && g_springs[i][1] == y && g_springs[i][2] == z) {
            if (!on) { g_springs[i][0] = g_springs[g_nsprings-1][0]; g_springs[i][1] = g_springs[g_nsprings-1][1]; g_springs[i][2] = g_springs[g_nsprings-1][2]; g_nsprings--; }
            return;
        }
    }
    if (on && g_nsprings < CF_MAX_SPRINGS) { g_springs[g_nsprings][0] = (int32_t)x; g_springs[g_nsprings][1] = (int32_t)y; g_springs[g_nsprings][2] = (int32_t)z; g_nsprings++; }
}
int64_t cf_spring_count(void) { return g_nsprings; }

/* ── Spray ──────────────────────────────────────────────────────────────────
 * White, short-lived, up then down. Fed by an emitter queue March fills from
 * the water actors' drop entries, by every spring's mouth bubbling, and by a
 * burst when a spring is placed. Same idioms as the precipitation pool. */
#define CF_SPRAY_SLOT 502
#define CF_SPRAY_LIFE 0.6f
static float  *g_spray = NULL;     /* x y z vx vy vz life, 7 floats */
static float  *g_spray_vtx = NULL;
static int64_t g_spray_cap = 0, g_spray_live = 0;
static int32_t g_emit[1024][4]; static int64_t g_nemit = 0;   /* x y z count */

void cf_spray_init(int64_t cap) {
    if (cap < 0) cap = 0; if (cap > 1 << 16) cap = 1 << 16;
    free(g_spray); free(g_spray_vtx);
    g_spray_cap = cap; g_spray_live = 0;
    g_spray = cap ? (float *)calloc((size_t)cap * 7, sizeof(float)) : NULL;
    g_spray_vtx = cap ? (float *)calloc((size_t)cap * CF_PRECIP_VTX, sizeof(float)) : NULL;
}
void cf_spray_at(int64_t x, int64_t y, int64_t z, int64_t n) {
    if (g_nemit < 1024) { g_emit[g_nemit][0] = (int32_t)x; g_emit[g_nemit][1] = (int32_t)y; g_emit[g_nemit][2] = (int32_t)z; g_emit[g_nemit][3] = (int32_t)n; g_nemit++; }
}
static void spray_spawn(float x, float y, float z, int64_t n, int64_t tick) {
    for (int64_t k = 0; k < n && g_spray_live < g_spray_cap; k++) {
        float *p = g_spray + g_spray_live * 7;
        float a = pcl_rnd(g_spray_live * 31 + k, tick) * 6.283185f;
        float r = pcl_rnd(g_spray_live * 17 + k, tick + 1) * 1.6f;
        p[0] = x + 0.5f; p[1] = y + 0.9f; p[2] = z + 0.5f;
        p[3] = cosf(a) * r; p[4] = 2.5f + pcl_rnd(k, tick + 2) * 2.0f; p[5] = sinf(a) * r;
        p[6] = CF_SPRAY_LIFE;
        g_spray_live++;
    }
}
/* Step, spawn queued emitters (and every spring's mouth, two a tick), upload.
 * Returns the vertex count to draw. */
int64_t cf_spray_frame(double dt, int64_t tick) {
    if (!g_spray) return 0;
    for (int64_t i = 0; i < g_nemit; i++) spray_spawn((float)g_emit[i][0], (float)g_emit[i][1], (float)g_emit[i][2], g_emit[i][3], tick);
    g_nemit = 0;
    if (tick % 10 == 0) for (int64_t i = 0; i < g_nsprings; i++) spray_spawn((float)g_springs[i][0], (float)g_springs[i][1], (float)g_springs[i][2], 2, tick + i);
    for (int64_t i = 0; i < g_spray_live; ) {
        float *p = g_spray + i * 7;
        p[6] -= (float)dt;
        if (p[6] <= 0.0f) { memcpy(p, g_spray + (g_spray_live - 1) * 7, 7 * sizeof(float)); g_spray_live--; continue; }
        p[4] -= 9.0f * (float)dt;
        p[0] += p[3] * (float)dt; p[1] += p[4] * (float)dt; p[2] += p[5] * (float)dt;
        i++;
    }
    for (int64_t i = 0; i < g_spray_live; i++) {
        float *p = g_spray + i * 7;
        float a = p[6] / CF_SPRAY_LIFE;
        float fx = (float)(1 * 256 + (int)(a * 0.9f * 255.0f + 0.5f));
        float h = 0.07f;
        float *v = g_spray_vtx + i * CF_PRECIP_VTX;
        pcl_vert(v + 0 * CF_VERT_FLOATS, p[0] - h, p[1] - h, p[2],     0.95f, 0.97f, 1.0f, fx);
        pcl_vert(v + 1 * CF_VERT_FLOATS, p[0] + h, p[1] - h, p[2],     0.95f, 0.97f, 1.0f, fx);
        pcl_vert(v + 2 * CF_VERT_FLOATS, p[0] + h, p[1] + h, p[2],     0.95f, 0.97f, 1.0f, fx);
        pcl_vert(v + 3 * CF_VERT_FLOATS, p[0] - h, p[1] - h, p[2],     0.95f, 0.97f, 1.0f, fx);
        pcl_vert(v + 4 * CF_VERT_FLOATS, p[0] + h, p[1] + h, p[2],     0.95f, 0.97f, 1.0f, fx);
        pcl_vert(v + 5 * CF_VERT_FLOATS, p[0] - h, p[1] + h, p[2],     0.95f, 0.97f, 1.0f, fx);
    }
    if (g_spray_live > 0) {
        glBindBuffer(GL_ARRAY_BUFFER, g_vbo[CF_SPRAY_SLOT]);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(g_spray_live * CF_PRECIP_VTX * (int64_t)sizeof(float)), g_spray_vtx, GL_STREAM_DRAW);
    }
    return g_spray_live * 6;
}

/* Precipitation: blended and depth-write-off like water, but also unlit and
 * untextured, so each particle keeps the colour it carries in its uv/layer
 * slots instead of being dimmed by a sun it is supposed to be obscuring. */
void cf_gfx_draw_precip(int64_t slot, int64_t nverts) {
    if (slot < 0 || slot >= CF_MAX_MESHES || nverts <= 0) return;
    cf_unlit_begin();
    glUniform1i(g_u_use_tex, 0);
    cf_gfx_draw_translucent(slot, nverts);
    glUniform1i(g_u_use_tex, g_tex ? 1 : 0);
    cf_unlit_end();
}

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
    glBindBuffer(GL_ARRAY_BUFFER, vbo_of(slot));
    GLsizei stride = CF_VERT_FLOATS * sizeof(float);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void *)0);
    glEnableVertexAttribArray(1); glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, (void *)(3 * 4));
    glEnableVertexAttribArray(2); glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, stride, (void *)(5 * 4));
    glEnableVertexAttribArray(3); glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, stride, (void *)(6 * 4));
    glEnableVertexAttribArray(4); glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, stride, (void *)(7 * 4));
    glEnableVertexAttribArray(5); glVertexAttribPointer(5, 1, GL_FLOAT, GL_FALSE, stride, (void *)(8 * 4));
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

/* The f64 twin, for the biome field's NativeFloatArr columns (a window
 * shift slides them a row at a time). Same rc == 1 contract. */
void *cf_f64_blit(void *dst, int64_t di, void *src, int64_t si, int64_t n) {
    int64_t rc = *(int64_t *)dst;
    if (rc != 1) { fprintf(stderr, "cf_f64_blit: destination is shared (rc=%lld); refusing to write in place\n", (long long)rc); abort(); }
    if (n <= 0) return dst;
    if (di < 0 || si < 0 || di + n > narr_len(dst) || si + n > narr_len(src)) {
        fprintf(stderr, "cf_f64_blit: out of range (di=%lld si=%lld n=%lld dst=%lld src=%lld)\n",
                (long long)di, (long long)si, (long long)n, (long long)narr_len(dst), (long long)narr_len(src));
        abort();
    }
    memcpy((double *)narr_data(dst) + di, (const double *)narr_data(src) + si, (size_t)n * 8);
    return dst;
}

/* Stamp a model template: copy n floats (whole 9-float vertices) from src[si..]
 * into dst[di..], adding (dx, dy, dz) to each vertex's position and writing
 * [shade] into its shade slot. The March version did this with nine boxed-Float
 * reads and three boxed-Float adds per vertex; the profile put a bush stamp at
 * thousands of allocations. Same rc == 1 contract as cf_f32_blit. */
void *cf_f32_stamp(void *dst, int64_t di, void *src, int64_t si, int64_t n, double dx, double dy, double dz, double shade) {
    int64_t rc = *(int64_t *)dst;
    if (rc != 1) { fprintf(stderr, "cf_f32_stamp: destination is shared (rc=%lld); refusing to write in place\n", (long long)rc); abort(); }
    if (n <= 0) return dst;
    if (di < 0 || si < 0 || di + n > narr_len(dst) || si + n > narr_len(src) || n % CF_VERT_FLOATS != 0) {
        fprintf(stderr, "cf_f32_stamp: out of range (di=%lld si=%lld n=%lld dst=%lld src=%lld)\n",
                (long long)di, (long long)si, (long long)n, (long long)narr_len(dst), (long long)narr_len(src));
        abort();
    }
    float *d = (float *)narr_data(dst) + di;
    const float *s = (const float *)narr_data(src) + si;
    /* [shade] arrives packed and is split into the two light attributes, as in
     * cf_vert: the template's own slots 6 and 9 are placeholders. */
    double blk = floor(shade * 0.5);
    for (int64_t i = 0; i < n; i += CF_VERT_FLOATS) {
        d[i + 0] = (float)((double)s[i + 0] + dx);
        d[i + 1] = (float)((double)s[i + 1] + dy);
        d[i + 2] = (float)((double)s[i + 2] + dz);
        d[i + 3] = s[i + 3]; d[i + 4] = s[i + 4]; d[i + 5] = s[i + 5];
        d[i + 6] = (float)(shade - 2.0 * blk);
        d[i + 7] = s[i + 7]; d[i + 8] = s[i + 8];
        d[i + 9] = (float)(blk / 255.0);
    }
    return dst;
}

/* ── The greedy mesher's quad, written here ──────────────────────────────────
 * One merged rectangle of the section mesher: 6 vertices x CF_VERT_FLOATS floats at
 * dst[at..], from the integer description the greedy pass has (direction d,
 * section sy, slice a, mask cell (u, v), size wd x h, the packed key) plus the
 * chunk origin and the texture-layer table. This is Mesher.emit_rect and
 * quad_sized, Vertex.pack_shade, Mesher.corner_sky/corner_blk and should_flip
 * in C, arithmetic in the same order in double so the floats come out
 * bit-identical (the mesh hash is the oracle). Why here: every Float in March
 * is a heap object, and a quad in March was some sixty of them; the profile
 * put 60% of a section's mesh time in the allocator.
 *
 * Key layout (Mesher.key_of): id in the low 8 bits, four 10-bit corners c0..c3
 * at 2^8, 2^18, 2^28, 2^38 in mask (u, v) order, the shown species at 2^48.
 * Corner (Mesher.pack_corner): skylight in the low nibble, AO 0..3 above it,
 * block light 0..15 above that.
 * Layers: tab[id * 6 + d] for a block face; tab[1536 + k * 6 + (sp - 1)] for
 * mycelium base index k shown with species sp (Texture.layer_table). */
static double cf_corner_ao(int64_t p) { return 0.55 + 0.15 * (double)((p / 16) % 4); }
static double cf_corner_sky(int64_t p) { return ((double)(p % 16) / 15.0) * cf_corner_ao(p); }
static double cf_corner_blk(int64_t p) { return ((double)(p / 64) / 15.0) * cf_corner_ao(p); }
static double cf_clamp01(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }
static double cf_pack_shade(double sky, double blk) { return 2.0 * (double)(int64_t)(cf_clamp01(blk) * 255.0 + 0.5) + cf_clamp01(sky); }
static double cf_brightness(double p) { double h = floor(p / 2.0); return (p - 2.0 * h) + h / 255.0; }
/* [shade] arrives packed (see cf_pack_shade / CubeForge.Vertex.pack_shade) and is
 * split HERE, on the CPU, into the two attributes the GPU interpolates. The
 * packed form never reaches a varying -- see the layout note above. */
static void cf_vert(float *o, double x, double y, double z, double u, double v, double layer, double shade, double face) {
    double blk = floor(shade * 0.5);
    o[0] = (float)x; o[1] = (float)y; o[2] = (float)z; o[3] = (float)u; o[4] = (float)v;
    o[5] = (float)layer; o[6] = (float)(shade - 2.0 * blk); o[7] = (float)face; o[8] = 255.0f;
    o[9] = (float)(blk / 255.0);
}
static void cf_quad_into(float *o, int64_t d, int64_t sy, int64_t a, int64_t u, int64_t v, int64_t wd, int64_t h,
                         int64_t key, double ox, double oz, const float *tab, int64_t ntab);
void *cf_mesh_quad(void *dst, int64_t at, int64_t d, int64_t sy, int64_t a, int64_t u, int64_t v, int64_t wd, int64_t h,
                   int64_t key, double ox, double oz, void *layers) {
    int64_t rc = *(int64_t *)dst;
    if (rc != 1) { fprintf(stderr, "cf_mesh_quad: destination is shared (rc=%lld); refusing to write in place\n", (long long)rc); abort(); }
    if (at < 0 || at + 6 * CF_VERT_FLOATS > narr_len(dst)) { fprintf(stderr, "cf_mesh_quad: out of range (at=%lld dst=%lld)\n", (long long)at, (long long)narr_len(dst)); abort(); }
    cf_quad_into((float *)narr_data(dst) + at, d, sy, a, u, v, wd, h, key, ox, oz, (const float *)narr_data(layers), narr_len(layers));
    return dst;
}

/* One slice of the greedy pass: the 256 keys of mask[base..base+256) (mask
 * index u + 16 * v), merged into rectangles in the mesher's scan order --
 * first unclaimed cell in index order, widest run along u of the same key,
 * tallest stack of such rows along v -- each written as a quad at dst[at..].
 * Works on a local copy of the slice, so the March mask is read only. dst is
 * consumed and returned like cf_f32_blit's; the caller reserves 256 * CF_QUAD_FLOATS + 1
 * floats past [at], and the number of floats written comes back in the slot
 * at dst[at + 256 * CF_QUAD_FLOATS], past anything this call wrote, for the caller to
 * read and adopt as the new length (an extern returns one value, and the
 * buffer is the one that must come back). Replaces Mesher.greedy_go: its
 * per-cell variant rebuild and run scans were a third of a section's mesh
 * time (RESULTS, the perf pass of 2026-09-05). */
void *cf_mesh_slice(void *dst, int64_t at, void *mask, int64_t base, int64_t d, int64_t sy, int64_t a,
                    double ox, double oz, void *layers) {
    int64_t rc = *(int64_t *)dst;
    if (rc != 1) { fprintf(stderr, "cf_mesh_slice: destination is shared (rc=%lld); refusing to write in place\n", (long long)rc); abort(); }
    if (at < 0 || at + 256 * CF_QUAD_FLOATS + 1 > narr_len(dst) || base < 0 || base + 256 > narr_len(mask)) {
        fprintf(stderr, "cf_mesh_slice: out of range (at=%lld dst=%lld base=%lld mask=%lld)\n", (long long)at, (long long)narr_len(dst), (long long)base, (long long)narr_len(mask));
        abort();
    }
    int64_t m[256];
    memcpy(m, (const int64_t *)narr_data(mask) + base, 256 * sizeof(int64_t));
    const float *tab = (const float *)narr_data(layers);
    int64_t ntab = narr_len(layers);
    float *o = (float *)narr_data(dst) + at;
    int64_t written = 0;
    for (int64_t i = 0; i < 256; i++) {
        int64_t key = m[i];
        if (key == 0) continue;
        int64_t u = i % 16, v = i / 16;
        int64_t wd = 1;
        while (u + wd < 16 && m[u + wd + 16 * v] == key) wd++;
        int64_t h = 1;
        for (;;) {
            if (v + h >= 16) break;
            int ok = 1;
            for (int64_t k = 0; k < wd; k++) if (m[u + k + 16 * (v + h)] != key) { ok = 0; break; }
            if (!ok) break;
            h++;
        }
        for (int64_t k = 0; k < wd * h; k++) m[u + k % wd + 16 * (v + k / wd)] = 0;
        cf_quad_into(o + written, d, sy, a, u, v, wd, h, key, ox, oz, tab, ntab);
        written += CF_QUAD_FLOATS;
    }
    o[256 * CF_QUAD_FLOATS] = (float)written;
    return dst;
}

static void cf_quad_into(float *o, int64_t d, int64_t sy, int64_t a, int64_t u, int64_t v, int64_t wd, int64_t h,
                         int64_t key, double ox, double oz, const float *tab, int64_t ntab) {
    int64_t id = key % 256;
    int64_t c0 = (key / 256) % 1024, c1 = (key / 262144) % 1024, c2 = (key / 268435456) % 1024, c3 = (key / 274877906944LL) % 1024;
    int64_t sp = (key / 281474976710656LL) % 256;
    double layer;
    if (id >= 26 && id <= 46 && sp > 0) {
        int64_t k = (id - 26) / 3;
        int64_t idx = 1536 + k * 6 + (sp - 1);
        if (idx >= ntab) { fprintf(stderr, "cf_mesh_quad: layer table too short (%lld)\n", (long long)ntab); abort(); }
        layer = tab[idx];
    } else {
        int64_t idx = id * 6 + d;
        if (idx >= ntab) { fprintf(stderr, "cf_mesh_quad: layer table too short (%lld)\n", (long long)ntab); abort(); }
        layer = tab[idx];
    }
    double s0 = cf_pack_shade(cf_corner_sky(c0), cf_corner_blk(c0));
    double s1 = cf_pack_shade(cf_corner_sky(c1), cf_corner_blk(c1));
    double s2 = cf_pack_shade(cf_corner_sky(c2), cf_corner_blk(c2));
    double s3 = cf_pack_shade(cf_corner_sky(c3), cf_corner_blk(c3));
    double fw = (double)wd, fh = (double)h;
    /* the four corners in quad_sized's order, its uv extents, and its shades */
    double X[4], Y[4], Z[4], S[4], uw, vh;
    if (d <= 1) {
        double x = ox + (double)u, z = oz + (double)v, y = (double)(sy * 16 + a);
        double x1 = x + fw, z1 = z + fh;
        if (d == 0) {
            X[0]=x;  Y[0]=y+1.0; Z[0]=z;   X[1]=x;  Y[1]=y+1.0; Z[1]=z1;  X[2]=x1; Y[2]=y+1.0; Z[2]=z1;  X[3]=x1; Y[3]=y+1.0; Z[3]=z;
            uw = fh; vh = fw; S[0]=s0; S[1]=s3; S[2]=s2; S[3]=s1;
        } else {
            X[0]=x;  Y[0]=y; Z[0]=z;   X[1]=x1; Y[1]=y; Z[1]=z;  X[2]=x1; Y[2]=y; Z[2]=z1;  X[3]=x; Y[3]=y; Z[3]=z1;
            uw = fw; vh = fh; S[0]=s0; S[1]=s1; S[2]=s2; S[3]=s3;
        }
    } else if (d <= 3) {
        double x = ox + (double)a, z = oz + (double)u, y = (double)(sy * 16 + v);
        double z1 = z + fw, y1 = y + fh;
        if (d == 2) {
            X[0]=x+1.0; Y[0]=y;  Z[0]=z;   X[1]=x+1.0; Y[1]=y1; Z[1]=z;  X[2]=x+1.0; Y[2]=y1; Z[2]=z1;  X[3]=x+1.0; Y[3]=y; Z[3]=z1;
            uw = fh; vh = fw; S[0]=s0; S[1]=s3; S[2]=s2; S[3]=s1;
        } else {
            X[0]=x; Y[0]=y;  Z[0]=z1;  X[1]=x; Y[1]=y1; Z[1]=z1;  X[2]=x; Y[2]=y1; Z[2]=z;  X[3]=x; Y[3]=y; Z[3]=z;
            uw = fh; vh = fw; S[0]=s1; S[1]=s2; S[2]=s3; S[3]=s0;
        }
    } else {
        double x = ox + (double)u, z = oz + (double)a, y = (double)(sy * 16 + v);
        double x1 = x + fw, y1 = y + fh;
        if (d == 4) {
            X[0]=x1; Y[0]=y;  Z[0]=z+1.0;  X[1]=x1; Y[1]=y1; Z[1]=z+1.0;  X[2]=x; Y[2]=y1; Z[2]=z+1.0;  X[3]=x; Y[3]=y; Z[3]=z+1.0;
            uw = fh; vh = fw; S[0]=s1; S[1]=s2; S[2]=s3; S[3]=s0;
        } else {
            X[0]=x; Y[0]=y;  Z[0]=z;  X[1]=x; Y[1]=y1; Z[1]=z;  X[2]=x1; Y[2]=y1; Z[2]=z;  X[3]=x1; Y[3]=y; Z[3]=z;
            uw = fh; vh = fw; S[0]=s0; S[1]=s3; S[2]=s2; S[3]=s1;
        }
    }
    double face = (double)d;
    int flip = cf_brightness(S[0]) + cf_brightness(S[2]) < cf_brightness(S[1]) + cf_brightness(S[3]);
    if (flip) {
        cf_vert(o + 0,  X[1], Y[1], Z[1], uw,  0.0, layer, S[1], face);
        cf_vert(o + 10,  X[2], Y[2], Z[2], uw,  vh,  layer, S[2], face);
        cf_vert(o + 20, X[3], Y[3], Z[3], 0.0, vh,  layer, S[3], face);
        cf_vert(o + 30, X[1], Y[1], Z[1], uw,  0.0, layer, S[1], face);
        cf_vert(o + 40, X[3], Y[3], Z[3], 0.0, vh,  layer, S[3], face);
        cf_vert(o + 50, X[0], Y[0], Z[0], 0.0, 0.0, layer, S[0], face);
    } else {
        cf_vert(o + 0,  X[0], Y[0], Z[0], 0.0, 0.0, layer, S[0], face);
        cf_vert(o + 10,  X[1], Y[1], Z[1], uw,  0.0, layer, S[1], face);
        cf_vert(o + 20, X[2], Y[2], Z[2], uw,  vh,  layer, S[2], face);
        cf_vert(o + 30, X[0], Y[0], Z[0], 0.0, 0.0, layer, S[0], face);
        cf_vert(o + 40, X[2], Y[2], Z[2], uw,  vh,  layer, S[2], face);
        cf_vert(o + 50, X[3], Y[3], Z[3], 0.0, vh,  layer, S[3], face);
    }
}

/* ── Relight box helpers: the light field is x + CF_WORLD_SIDE * (z + CF_WORLD_SIDE * y) ──
 * Zero the box [x0..x1] x [y0..y1] x [z0..z1] of a light field: one memset per
 * row. Same rc == 1 contract as cf_u8_blit. Light.zero_box_go did this a byte
 * at a time; a tree's relight clears ~35k voxels twice (sky and block light). */
void *cf_u8_zero_box(void *a, int64_t x0, int64_t x1, int64_t y0, int64_t y1, int64_t z0, int64_t z1) {
    int64_t rc = *(int64_t *)a;
    if (rc != 1) { fprintf(stderr, "cf_u8_zero_box: field is shared (rc=%lld); refusing to write in place\n", (long long)rc); abort(); }
    if (x0 < 0 || y0 < 0 || z0 < 0 || x1 >= CF_WORLD_SIDE || y1 >= 256 || z1 >= CF_WORLD_SIDE || narr_len(a) < CF_WORLD_VOL) {
        fprintf(stderr, "cf_u8_zero_box: out of range\n"); abort();
    }
    unsigned char *d = (unsigned char *)narr_data(a);
    for (int64_t y = y0; y <= y1; y++)
        for (int64_t z = z0; z <= z1; z++)
            memset(d + x0 + CF_WORLD_SIDE * (z + CF_WORLD_SIDE * y), 0, (size_t)(x1 - x0 + 1));
    return a;
}

/* Mark, in a marks array of CF_CHUNK_SLOTS * 16 ints (slot cx + CF_WORLD_CHUNKS * cz + CF_CHUNK_SLOTS * sy), every chunk
 * section in which the two light fields differ inside the box. Light.mark_box
 * compared a voxel at a time in March; here a row is one memcmp and only a
 * differing row is walked. [marks] is consumed and returned; [a] and [b] are
 * read only. */
void *cf_mark_box(void *marks, void *a, void *b, int64_t x0, int64_t x1, int64_t y0, int64_t y1, int64_t z0, int64_t z1) {
    int64_t rc = *(int64_t *)marks;
    if (rc != 1) { fprintf(stderr, "cf_mark_box: marks are shared (rc=%lld); refusing to write in place\n", (long long)rc); abort(); }
    if (x0 < 0 || y0 < 0 || z0 < 0 || x1 >= CF_WORLD_SIDE || y1 >= 256 || z1 >= CF_WORLD_SIDE || narr_len(marks) < CF_CHUNK_SLOTS * 16) {
        fprintf(stderr, "cf_mark_box: out of range\n"); abort();
    }
    int64_t need = x1 + CF_WORLD_SIDE * (z1 + CF_WORLD_SIDE * y1) + 1;
    if (narr_len(a) < need || narr_len(b) < need) { fprintf(stderr, "cf_mark_box: fields too short\n"); abort(); }
    const unsigned char *pa = (const unsigned char *)narr_data(a);
    const unsigned char *pb = (const unsigned char *)narr_data(b);
    int64_t *m = (int64_t *)narr_data(marks);
    for (int64_t y = y0; y <= y1; y++)
        for (int64_t z = z0; z <= z1; z++) {
            int64_t row = x0 + CF_WORLD_SIDE * (z + CF_WORLD_SIDE * y);
            if (memcmp(pa + row, pb + row, (size_t)(x1 - x0 + 1)) == 0) continue;
            for (int64_t x = x0; x <= x1; x++)
                if (pa[row + x - x0] != pb[row + x - x0]) m[(x / 16) + CF_WORLD_CHUNKS * (z / 16) + CF_CHUNK_SLOTS * (y / 16)] = 1;
        }
    return marks;
}

/* ── Lake tile cache ─────────────────────────────────────────────────────────
 * CubeForge.Lakes.tile pours a priority-flood over a 128x128 tile: 157-214 ms
 * measured. Every water actor called it on WLoad, so 144 actors poured the
 * same handful of tiles -- the whole cost of a chunk reload, and the 35-110 ms
 * frame on the first water tick after a window shift.
 *
 * The pour is a pure function of (seed, tx, tz), so it is memoised here: the
 * actors share one process, and this is the only place they can share anything
 * (a message may not carry a native array -- GAPS G44).
 *
 * cf_lake_get is one call, not a hit test followed by a fetch, so two threads
 * cannot race between them: the hit flag is the LAST byte of the caller's
 * array, which is sized one longer than the tile for it. Called from actor
 * threads, so both entry points take the lock. */
#define CF_LAKE_SLOTS 32
static struct { int64_t seed, tx, tz; unsigned char *bytes; size_t n; int used; } g_lake[CF_LAKE_SLOTS];
static int64_t g_lake_next = 0;
static pthread_mutex_t g_lake_mu = PTHREAD_MUTEX_INITIALIZER;

void *cf_lake_get(void *out, int64_t seed, int64_t tx, int64_t tz) {
    int64_t rc = *(int64_t *)out;
    if (rc != 1) { fprintf(stderr, "cf_lake_get: destination is shared (rc=%lld)\n", (long long)rc); abort(); }
    int64_t len = narr_len(out);
    if (len < 1) { fprintf(stderr, "cf_lake_get: destination too small\n"); abort(); }
    size_t n = (size_t)(len - 1);            /* the last byte is the hit flag */
    unsigned char *d = (unsigned char *)narr_data(out);
    d[n] = 0;
    pthread_mutex_lock(&g_lake_mu);
    for (int i = 0; i < CF_LAKE_SLOTS; i++) {
        if (g_lake[i].used && g_lake[i].seed == seed && g_lake[i].tx == tx && g_lake[i].tz == tz && g_lake[i].n == n) {
            memcpy(d, g_lake[i].bytes, n);
            d[n] = 1;
            break;
        }
    }
    pthread_mutex_unlock(&g_lake_mu);
    return out;
}

void cf_lake_put(int64_t seed, int64_t tx, int64_t tz, void *arr) {
    size_t n = (size_t)narr_len(arr);
    if (n == 0) return;
    const unsigned char *src = (const unsigned char *)narr_data(arr);
    pthread_mutex_lock(&g_lake_mu);
    /* already there (another thread poured the same tile): keep the first */
    for (int i = 0; i < CF_LAKE_SLOTS; i++)
        if (g_lake[i].used && g_lake[i].seed == seed && g_lake[i].tx == tx && g_lake[i].tz == tz) {
            pthread_mutex_unlock(&g_lake_mu); return;
        }
    int i = (int)(g_lake_next % CF_LAKE_SLOTS);
    g_lake_next++;
    unsigned char *b = (unsigned char *)malloc(n);
    if (!b) { pthread_mutex_unlock(&g_lake_mu); return; }
    memcpy(b, src, n);
    free(g_lake[i].bytes);
    g_lake[i].bytes = b; g_lake[i].n = n;
    g_lake[i].seed = seed; g_lake[i].tx = tx; g_lake[i].tz = tz; g_lake[i].used = 1;
    pthread_mutex_unlock(&g_lake_mu);
}

/* Diagnostic: the refcount word of a March array, as the extern sees it (the
 * borrow for this call is included, so a uniquely owned array reads 2). */
int64_t cf_arr_rc(void *a) { return *(int64_t *)a; }

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
/* Shadow reach in blocks (0 disables the trace) and whether to soften edges. */
void cf_gfx_set_shadow(double dist, int64_t soft) {
    if (!g_prog) return;
    glUseProgram(g_prog);
    glUniform1f(g_u_shadow, (float)dist);
    glUniform1f(g_u_soft, soft ? 1.0f : 0.0f);
}

void cf_gfx_set_sky(double sx, double sy, double sz, double mx, double my, double mz) {
    if (!g_prog) return;
    double sl = sqrt(sx*sx + sy*sy + sz*sz); if (sl < 1e-9) sl = 1.0;
    double ml = sqrt(mx*mx + my*my + mz*mz); if (ml < 1e-9) ml = 1.0;
    glUseProgram(g_prog);
    glUniform3f(g_u_sundir,  (float)(sx/sl), (float)(sy/sl), (float)(sz/sl));
    glUniform3f(g_u_moondir, (float)(mx/ml), (float)(my/ml), (float)(mz/ml));
}

/* Fog density, cloud cover and the lightning spike. The fog colour is not a
 * parameter: it is whatever cf_gfx_begin_frame last cleared to, so the fog and
 * the sky are the same colour by construction. */
/* Seconds, for the water animation. CF_TIME pins it on the March side so frame
 * dumps stay comparable. */
void cf_gfx_set_time(double t) {
    glUseProgram(g_prog);
    glUniform1f(g_u_time, (float)t);
}

/* The mycelium filament overlay: which texture layers are mycelium, the
 * species colours they are drawn in (3 floats each, 0..1), and how strongly.
 * Set once at startup -- none of it changes while a world runs. */
void cf_gfx_set_myc(int64_t first, int64_t count, void *rgb, double branch, double scale, double sharp) {
    if (!g_prog) return;
    glUseProgram(g_prog);
    glUniform3fv(g_u_species, (GLsizei)(narr_len(rgb) / 3), (const float *)narr_data(rgb));
    glUniform1i(g_u_myc_first, (int)first);
    glUniform1i(g_u_myc_count, (int)count);
    glUniform1f(g_u_branch, (float)branch);
    glUniform1f(g_u_branch_scale, (float)scale);
    glUniform1f(g_u_branch_sharp, (float)sharp);
}

void cf_gfx_set_weather(double fog_density, double overcast, double bolt) {
    glUseProgram(g_prog);
    glUniform1f(g_u_fog_density, (float)fog_density);
    glUniform3f(g_u_fog_color, g_fog_rgb[0], g_fog_rgb[1], g_fog_rgb[2]);
    glUniform1f(g_u_overcast, (float)overcast);
    glUniform1f(g_u_bolt, (float)bolt);
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
    memset(g_in.buttons_released, 0, sizeof g_in.buttons_released);
    memset(g_in.keys_pressed, 0, sizeof g_in.keys_pressed);
    glfwPollEvents();
}
int64_t cf_in_key(int64_t k)          { return (k >= 0 && k < CF_MAX_KEYS) ? g_in.keys[k] : 0; }
int64_t cf_in_key_pressed(int64_t k)  { return (k >= 0 && k < CF_MAX_KEYS) ? g_in.keys_pressed[k] : 0; }
int64_t cf_in_button(int64_t b)       { return (b >= 0 && b < 8) ? g_in.buttons[b] : 0; }
int64_t cf_in_button_pressed(int64_t b){ return (b >= 0 && b < 8) ? g_in.buttons_pressed[b] : 0; }
int64_t cf_in_button_released(int64_t b){ return (b >= 0 && b < 8) ? g_in.buttons_released[b] : 0; }

/* Cursor position in FRAMEBUFFER pixels.
 *
 * GLFW reports the cursor in *window* coordinates, but everything else here
 * (the viewport, cf_win_fb_w/h) is in framebuffer pixels, and on a Retina
 * display the two differ by 2x. Converting at this one boundary means March
 * only ever sees a single coordinate system; doing it later, or forgetting,
 * puts hit-testing half a screen out and reads like a layout bug rather than a
 * units bug. Window size is queried rather than cached because a window can be
 * dragged between displays of different scale factors mid-run. */
static double cf_cursor_scale(void) {
    if (!g_win) return 1.0;
    int ww = 0, wh = 0;
    glfwGetWindowSize(g_win, &ww, &wh);
    (void)wh;
    return (ww > 0 && g_fb_w > 0) ? (double)g_fb_w / (double)ww : 1.0;
}
double  cf_in_mouse_x(void)           { return g_in.mouse_x * cf_cursor_scale(); }
double  cf_in_mouse_y(void)           { return g_in.mouse_y * cf_cursor_scale(); }
double  cf_in_mouse_dx(void)          { return g_in.mouse_dx; }
double  cf_in_mouse_dy(void)          { return g_in.mouse_dy; }
double  cf_in_scroll_dy(void)         { return g_in.scroll_dy; }
void    cf_in_capture_cursor(int64_t on) {
    if (g_win) glfwSetInputMode(g_win, GLFW_CURSOR, on ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
}

/* The particle half of cf_gfx_shift: precipitation, spray and its emitters,
 * and the springs move 16 blocks per chunk the window slid; a spring that
 * left the window is dropped. */
static void cf_shift_particles(int64_t dx, int64_t dz) {
    float bx = 16.0f * (float)dx, bz = 16.0f * (float)dz;
    for (int64_t i = 0; i < g_pcl_cap; i++) { g_pcl[i * 4 + 0] -= bx; g_pcl[i * 4 + 2] -= bz; }
    for (int64_t i = 0; i < g_spray_live; i++) { g_spray[i * 7 + 0] -= bx; g_spray[i * 7 + 2] -= bz; }
    for (int64_t i = 0; i < g_nemit; i++) { g_emit[i][0] -= (int32_t)(16 * dx); g_emit[i][2] -= (int32_t)(16 * dz); }
    for (int64_t i = 0; i < g_nsprings; ) {
        g_springs[i][0] -= (int32_t)(16 * dx); g_springs[i][2] -= (int32_t)(16 * dz);
        if (g_springs[i][0] < 0 || g_springs[i][0] >= CF_WORLD_SIDE || g_springs[i][2] < 0 || g_springs[i][2] >= CF_WORLD_SIDE) {
            g_springs[i][0] = g_springs[g_nsprings - 1][0]; g_springs[i][1] = g_springs[g_nsprings - 1][1]; g_springs[i][2] = g_springs[g_nsprings - 1][2];
            g_nsprings--;
        } else i++;
    }
}

/* Current resident set size in bytes (the allocation gauge's byte view). */
int64_t cf_rss_bytes(void) {
    struct mach_task_basic_info info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &count) != KERN_SUCCESS) return -1;
    return (int64_t)info.resident_size;
}

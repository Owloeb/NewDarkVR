/*
 * ndvr_xr.c - in-process OpenXR (OpenGL binding) for the 64-bit NewDarkRecompiled port.
 * See include/ndvr.h for the API contract. The OpenXR loader is loaded dynamically
 * (openxr_loader.dll next to the exe), so there is nothing to link against.
 */
#define XR_NO_PROTOTYPES
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <unknwn.h>
#include <GL/gl.h>
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_OPENGL
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ndvr.h"

/* ------------------------------------------------------------------------- */
/* GL entry points beyond what opengl32.dll exports (1.1)                     */
/* ------------------------------------------------------------------------- */

#define GL_FRAMEBUFFER            0x8D40
#define GL_RENDERBUFFER           0x8D41
#define GL_COLOR_ATTACHMENT0      0x8CE0
#define GL_DEPTH_STENCIL_ATTACHMENT 0x821A
#define GL_DEPTH24_STENCIL8       0x88F0
#define GL_FRAMEBUFFER_BINDING    0x8CA6
#define GL_FRAMEBUFFER_COMPLETE   0x8CD5
#define GL_SRGB8_ALPHA8           0x8C43
#define GL_RGBA8                  0x8058

typedef void (APIENTRY *PFNGLGENFRAMEBUFFERS)(GLsizei, GLuint *);
typedef void (APIENTRY *PFNGLDELETEFRAMEBUFFERS)(GLsizei, const GLuint *);
typedef void (APIENTRY *PFNGLBINDFRAMEBUFFER)(GLenum, GLuint);
typedef void (APIENTRY *PFNGLFRAMEBUFFERTEXTURE2D)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef void (APIENTRY *PFNGLFRAMEBUFFERRENDERBUFFER)(GLenum, GLenum, GLenum, GLuint);
typedef GLenum (APIENTRY *PFNGLCHECKFRAMEBUFFERSTATUS)(GLenum);
typedef void (APIENTRY *PFNGLGENRENDERBUFFERS)(GLsizei, GLuint *);
typedef void (APIENTRY *PFNGLDELETERENDERBUFFERS)(GLsizei, const GLuint *);
typedef void (APIENTRY *PFNGLBINDRENDERBUFFER)(GLenum, GLuint);
typedef void (APIENTRY *PFNGLRENDERBUFFERSTORAGE)(GLenum, GLenum, GLsizei, GLsizei);

static struct {
    PFNGLGENFRAMEBUFFERS genFramebuffers;
    PFNGLDELETEFRAMEBUFFERS deleteFramebuffers;
    PFNGLBINDFRAMEBUFFER bindFramebuffer;
    PFNGLFRAMEBUFFERTEXTURE2D framebufferTexture2D;
    PFNGLFRAMEBUFFERRENDERBUFFER framebufferRenderbuffer;
    PFNGLCHECKFRAMEBUFFERSTATUS checkFramebufferStatus;
    PFNGLGENRENDERBUFFERS genRenderbuffers;
    PFNGLDELETERENDERBUFFERS deleteRenderbuffers;
    PFNGLBINDRENDERBUFFER bindRenderbuffer;
    PFNGLRENDERBUFFERSTORAGE renderbufferStorage;
} gl;

static int loadGL(void)
{
#define LOAD(field, name)                                         \
    gl.field = (void *)wglGetProcAddress(name);                   \
    if (!gl.field)                                                \
        return 0;
    LOAD(genFramebuffers, "glGenFramebuffers")
    LOAD(deleteFramebuffers, "glDeleteFramebuffers")
    LOAD(bindFramebuffer, "glBindFramebuffer")
    LOAD(framebufferTexture2D, "glFramebufferTexture2D")
    LOAD(framebufferRenderbuffer, "glFramebufferRenderbuffer")
    LOAD(checkFramebufferStatus, "glCheckFramebufferStatus")
    LOAD(genRenderbuffers, "glGenRenderbuffers")
    LOAD(deleteRenderbuffers, "glDeleteRenderbuffers")
    LOAD(bindRenderbuffer, "glBindRenderbuffer")
    LOAD(renderbufferStorage, "glRenderbufferStorage")
#undef LOAD
    return 1;
}

/* ------------------------------------------------------------------------- */
/* logging / errors                                                           */
/* ------------------------------------------------------------------------- */

static void (*g_log)(const char *) = NULL;
static char g_err[512];

static void defaultLog(const char *line)
{
    fprintf(stderr, "[ndvr] %s\n", line);
    fflush(stderr);
}

static void logf_(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    (g_log ? g_log : defaultLog)(buf);
}

static void fail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_err, sizeof g_err, fmt, ap);
    va_end(ap);
    logf_("%s", g_err);
}

void ndvr_set_log(void (*fn)(const char *)) { g_log = fn; }
const char *ndvr_last_error(void) { return g_err; }

static const char *resultName(XrResult r)
{
    switch ((int)r) {
    case XR_ERROR_VALIDATION_FAILURE: return "VALIDATION_FAILURE";
    case XR_ERROR_RUNTIME_FAILURE: return "RUNTIME_FAILURE";
    case XR_ERROR_OUT_OF_MEMORY: return "OUT_OF_MEMORY";
    case XR_ERROR_API_VERSION_UNSUPPORTED: return "API_VERSION_UNSUPPORTED";
    case XR_ERROR_INITIALIZATION_FAILED: return "INITIALIZATION_FAILED";
    case XR_ERROR_FUNCTION_UNSUPPORTED: return "FUNCTION_UNSUPPORTED";
    case XR_ERROR_FEATURE_UNSUPPORTED: return "FEATURE_UNSUPPORTED";
    case XR_ERROR_EXTENSION_NOT_PRESENT: return "EXTENSION_NOT_PRESENT";
    case XR_ERROR_LIMIT_REACHED: return "LIMIT_REACHED";
    case XR_ERROR_INSTANCE_LOST: return "INSTANCE_LOST";
    case XR_ERROR_SESSION_LOST: return "SESSION_LOST";
    case XR_ERROR_SYSTEM_INVALID: return "SYSTEM_INVALID";
    case XR_ERROR_FORM_FACTOR_UNSUPPORTED: return "FORM_FACTOR_UNSUPPORTED";
    case XR_ERROR_FORM_FACTOR_UNAVAILABLE: return "FORM_FACTOR_UNAVAILABLE";
    case XR_ERROR_API_LAYER_NOT_PRESENT: return "API_LAYER_NOT_PRESENT";
    case XR_ERROR_GRAPHICS_DEVICE_INVALID: return "GRAPHICS_DEVICE_INVALID";
    case XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING: return "GRAPHICS_REQUIREMENTS_CALL_MISSING";
    case XR_ERROR_RUNTIME_UNAVAILABLE: return "RUNTIME_UNAVAILABLE";
    default: return "see openxr.h";
    }
}

/* ------------------------------------------------------------------------- */
/* OpenXR function table                                                      */
/* ------------------------------------------------------------------------- */

#define XR_FN_LIST(X)                                                                          \
    X(xrDestroyInstance) X(xrGetSystem) X(xrCreateSession) X(xrDestroySession)                 \
    X(xrCreateReferenceSpace) X(xrCreateActionSpace) X(xrDestroySpace)                         \
    X(xrEnumerateViewConfigurationViews) X(xrEnumerateSwapchainFormats)                        \
    X(xrCreateSwapchain) X(xrDestroySwapchain) X(xrEnumerateSwapchainImages)                   \
    X(xrAcquireSwapchainImage) X(xrWaitSwapchainImage) X(xrReleaseSwapchainImage)              \
    X(xrBeginSession) X(xrEndSession) X(xrRequestExitSession)                                  \
    X(xrWaitFrame) X(xrBeginFrame) X(xrEndFrame) X(xrLocateViews) X(xrLocateSpace)             \
    X(xrPollEvent) X(xrStringToPath)                                                           \
    X(xrCreateActionSet) X(xrDestroyActionSet) X(xrCreateAction)                               \
    X(xrSuggestInteractionProfileBindings) X(xrAttachSessionActionSets) X(xrSyncActions)       \
    X(xrGetActionStateFloat) X(xrGetActionStateBoolean) X(xrGetActionStateVector2f)            \
    X(xrGetInstanceProperties) X(xrGetOpenGLGraphicsRequirementsKHR)

static struct {
#define X(n) PFN_##n n;
    XR_FN_LIST(X)
#undef X
} fn;

static PFN_xrGetInstanceProcAddr g_getProcAddr;

#define CK(call)                                                      \
    do {                                                              \
        XrResult r_ = (call);                                         \
        if (XR_FAILED(r_)) {                                          \
            fail("%s failed: %s (%d)", #call, resultName(r_), (int)r_); \
            return 0;                                                 \
        }                                                             \
    } while (0)

/* ------------------------------------------------------------------------- */
/* state                                                                      */
/* ------------------------------------------------------------------------- */

typedef struct Eye {
    XrSwapchain sc;
    uint32_t w, h;
    uint32_t nimg;
    GLuint *tex;
    GLuint *fbo;
    GLuint depth;
    uint32_t acquired; /* index of the currently acquired image */
} Eye;

static XrInstance g_inst;
static XrSystemId g_sys;
static XrSession g_sess;
static XrSpace g_local, g_view;
static XrSessionState g_state = XR_SESSION_STATE_UNKNOWN;
static int g_running, g_exit, g_inited;

static Eye g_eye[2];
static GLenum g_fmt;

static XrActionSet g_set;
static XrPath g_handPath[2];
static XrAction g_aGrip, g_aAim, g_aTrigger, g_aSqueeze, g_aStick, g_aStickClick, g_aPrimary,
    g_aSecondary, g_aMenu;
static XrSpace g_gripSpace[2], g_aimSpace[2];

/* per-frame */
static int g_frameBegun, g_framRendered;
static XrTime g_frameTime;
static XrView g_views[2];
static GLint g_prevFbo;
static int g_boundThisFrame;

/* ------------------------------------------------------------------------- */
/* math                                                                       */
/* ------------------------------------------------------------------------- */

static void quatToR(const ndvr_quat *q, float R[3][3])
{
    float x = q->x, y = q->y, z = q->z, w = q->w;
    R[0][0] = 1 - 2 * (y * y + z * z); R[0][1] = 2 * (x * y - z * w);     R[0][2] = 2 * (x * z + y * w);
    R[1][0] = 2 * (x * y + z * w);     R[1][1] = 1 - 2 * (x * x + z * z); R[1][2] = 2 * (y * z - x * w);
    R[2][0] = 2 * (x * z - y * w);     R[2][1] = 2 * (y * z + x * w);     R[2][2] = 1 - 2 * (x * x + y * y);
}

void ndvr_pose_to_matrix(const ndvr_pose *p, float out[16])
{
    float R[3][3];
    int r, c;
    quatToR(&p->orientation, R);
    memset(out, 0, 16 * sizeof(float));
    for (c = 0; c < 3; ++c)
        for (r = 0; r < 3; ++r)
            out[c * 4 + r] = R[r][c];
    out[12] = p->position.x;
    out[13] = p->position.y;
    out[14] = p->position.z;
    out[15] = 1.0f;
}

void ndvr_view_matrix(const ndvr_pose *p, float out[16])
{
    float R[3][3];
    float t[3] = {p->position.x, p->position.y, p->position.z};
    int r, c;
    quatToR(&p->orientation, R);
    memset(out, 0, 16 * sizeof(float));
    /* view = R^T, translation = -R^T t */
    for (c = 0; c < 3; ++c)
        for (r = 0; r < 3; ++r)
            out[c * 4 + r] = R[c][r];
    for (r = 0; r < 3; ++r)
        out[12 + r] = -(R[0][r] * t[0] + R[1][r] * t[1] + R[2][r] * t[2]);
    out[15] = 1.0f;
}

void ndvr_projection_matrix(const ndvr_fov *fov, float n, float f, float out[16])
{
    float l = tanf(fov->left), r = tanf(fov->right), u = tanf(fov->up), d = tanf(fov->down);
    float w = r - l, h = u - d;
    memset(out, 0, 16 * sizeof(float));
    out[0] = 2.0f / w;
    out[5] = 2.0f / h;
    out[8] = (r + l) / w;
    out[9] = (u + d) / h;
    out[10] = -(f + n) / (f - n);
    out[11] = -1.0f;
    out[14] = -2.0f * f * n / (f - n);
}

static void toPose(const XrPosef *p, XrSpaceLocationFlags f, ndvr_pose *o)
{
    o->orientation.x = p->orientation.x;
    o->orientation.y = p->orientation.y;
    o->orientation.z = p->orientation.z;
    o->orientation.w = p->orientation.w;
    o->position.x = p->position.x;
    o->position.y = p->position.y;
    o->position.z = p->position.z;
    o->flags = ((f & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) ? NDVR_POSE_ORIENT_VALID : 0u) |
               ((f & XR_SPACE_LOCATION_POSITION_VALID_BIT) ? NDVR_POSE_POS_VALID : 0u);
}

static XrPosef identityPose(void)
{
    XrPosef p;
    memset(&p, 0, sizeof p);
    p.orientation.w = 1.0f;
    return p;
}

/* ------------------------------------------------------------------------- */
/* swapchains                                                                 */
/* ------------------------------------------------------------------------- */

static void destroyEye(Eye *e)
{
    if (e->fbo && e->nimg)
        gl.deleteFramebuffers((GLsizei)e->nimg, e->fbo);
    if (e->depth)
        gl.deleteRenderbuffers(1, &e->depth);
    if (e->sc)
        fn.xrDestroySwapchain(e->sc);
    free(e->tex);
    free(e->fbo);
    memset(e, 0, sizeof *e);
}

static int pickFormat(void)
{
    uint32_t n = 0, i;
    int64_t *f;
    CK(fn.xrEnumerateSwapchainFormats(g_sess, 0, &n, NULL));
    f = (int64_t *)malloc(n * sizeof *f);
    if (XR_FAILED(fn.xrEnumerateSwapchainFormats(g_sess, n, &n, f))) {
        free(f);
        fail("xrEnumerateSwapchainFormats failed");
        return 0;
    }
    /* sRGB first: the engine writes gamma-encoded values and FRAMEBUFFER_SRGB stays off,
     * so an sRGB swapchain makes the compositor decode them correctly. */
    g_fmt = 0;
    for (i = 0; i < n; ++i)
        if (f[i] == GL_SRGB8_ALPHA8)
            g_fmt = GL_SRGB8_ALPHA8;
    if (!g_fmt)
        for (i = 0; i < n; ++i)
            if (f[i] == GL_RGBA8)
                g_fmt = GL_RGBA8;
    free(f);
    if (!g_fmt) {
        fail("runtime offers neither GL_SRGB8_ALPHA8 nor GL_RGBA8 swapchains");
        return 0;
    }
    logf_("swapchain format 0x%x%s", (unsigned)g_fmt,
          g_fmt == GL_RGBA8 ? " (not sRGB: colors will look too dark)" : "");
    return 1;
}

static int createEye(Eye *e, uint32_t w, uint32_t h)
{
    XrSwapchainCreateInfo ci;
    XrSwapchainImageOpenGLKHR *imgs;
    uint32_t i, n = 0;
    GLint prev = 0;

    memset(&ci, 0, sizeof ci);
    ci.type = XR_TYPE_SWAPCHAIN_CREATE_INFO;
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    ci.format = (int64_t)g_fmt;
    ci.sampleCount = 1;
    ci.width = w;
    ci.height = h;
    ci.faceCount = 1;
    ci.arraySize = 1;
    ci.mipCount = 1;
    CK(fn.xrCreateSwapchain(g_sess, &ci, &e->sc));
    e->w = w;
    e->h = h;

    CK(fn.xrEnumerateSwapchainImages(e->sc, 0, &n, NULL));
    imgs = (XrSwapchainImageOpenGLKHR *)calloc(n, sizeof *imgs);
    for (i = 0; i < n; ++i)
        imgs[i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR;
    if (XR_FAILED(fn.xrEnumerateSwapchainImages(e->sc, n, &n, (XrSwapchainImageBaseHeader *)imgs))) {
        free(imgs);
        fail("xrEnumerateSwapchainImages failed");
        return 0;
    }
    e->nimg = n;
    e->tex = (GLuint *)calloc(n, sizeof(GLuint));
    e->fbo = (GLuint *)calloc(n, sizeof(GLuint));
    for (i = 0; i < n; ++i)
        e->tex[i] = imgs[i].image;
    free(imgs);

    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev);
    gl.genRenderbuffers(1, &e->depth);
    gl.bindRenderbuffer(GL_RENDERBUFFER, e->depth);
    gl.renderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, (GLsizei)w, (GLsizei)h);
    gl.bindRenderbuffer(GL_RENDERBUFFER, 0);
    gl.genFramebuffers((GLsizei)n, e->fbo);
    for (i = 0; i < n; ++i) {
        gl.bindFramebuffer(GL_FRAMEBUFFER, e->fbo[i]);
        gl.framebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, e->tex[i], 0);
        gl.framebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, e->depth);
        if (gl.checkFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            gl.bindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev);
            fail("eye framebuffer %u is incomplete", i);
            return 0;
        }
    }
    gl.bindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev);
    return 1;
}

/* ------------------------------------------------------------------------- */
/* actions                                                                    */
/* ------------------------------------------------------------------------- */

static XrPath path(const char *s)
{
    XrPath p = XR_NULL_PATH;
    fn.xrStringToPath(g_inst, s, &p);
    return p;
}

static XrAction makeAction(const char *name, const char *loc, XrActionType type)
{
    XrActionCreateInfo ci;
    XrAction a = XR_NULL_HANDLE;
    memset(&ci, 0, sizeof ci);
    ci.type = XR_TYPE_ACTION_CREATE_INFO;
    strncpy(ci.actionName, name, sizeof ci.actionName - 1);
    strncpy(ci.localizedActionName, loc, sizeof ci.localizedActionName - 1);
    ci.actionType = type;
    ci.countSubactionPaths = 2;
    ci.subactionPaths = g_handPath;
    if (XR_FAILED(fn.xrCreateAction(g_set, &ci, &a)))
        logf_("xrCreateAction(%s) failed", name);
    return a;
}

static int createActions(void)
{
    XrActionSetCreateInfo si;
    XrActionSuggestedBinding b[32];
    uint32_t nb = 0;
    int h;
    XrInteractionProfileSuggestedBinding sb;
    XrSessionActionSetsAttachInfo ai;

    g_handPath[0] = path("/user/hand/left");
    g_handPath[1] = path("/user/hand/right");

    memset(&si, 0, sizeof si);
    si.type = XR_TYPE_ACTION_SET_CREATE_INFO;
    strcpy(si.actionSetName, "newdarkvr");
    strcpy(si.localizedActionSetName, "NewDarkVR");
    CK(fn.xrCreateActionSet(g_inst, &si, &g_set));

    g_aGrip = makeAction("grip_pose", "Grip pose", XR_ACTION_TYPE_POSE_INPUT);
    g_aAim = makeAction("aim_pose", "Aim pose", XR_ACTION_TYPE_POSE_INPUT);
    g_aTrigger = makeAction("trigger", "Trigger", XR_ACTION_TYPE_FLOAT_INPUT);
    g_aSqueeze = makeAction("squeeze", "Grip", XR_ACTION_TYPE_FLOAT_INPUT);
    g_aStick = makeAction("thumbstick", "Thumbstick", XR_ACTION_TYPE_VECTOR2F_INPUT);
    g_aStickClick = makeAction("stick_click", "Thumbstick click", XR_ACTION_TYPE_BOOLEAN_INPUT);
    g_aPrimary = makeAction("primary", "A / X", XR_ACTION_TYPE_BOOLEAN_INPUT);
    g_aSecondary = makeAction("secondary", "B / Y", XR_ACTION_TYPE_BOOLEAN_INPUT);
    g_aMenu = makeAction("menu", "Menu", XR_ACTION_TYPE_BOOLEAN_INPUT);

    for (h = 0; h < 2; ++h) {
        const char *s = h == 0 ? "/user/hand/left" : "/user/hand/right";
        char buf[96];
#define BIND(act, suffix)                                         \
    do {                                                          \
        snprintf(buf, sizeof buf, "%s/input/" suffix, s);         \
        b[nb].action = (act);                                     \
        b[nb].binding = path(buf);                                \
        ++nb;                                                     \
    } while (0)
        BIND(g_aGrip, "grip/pose");
        BIND(g_aAim, "aim/pose");
        BIND(g_aTrigger, "trigger/value");
        BIND(g_aSqueeze, "squeeze/value");
        BIND(g_aStick, "thumbstick");
        BIND(g_aStickClick, "thumbstick/click");
        if (h == 0) {
            BIND(g_aPrimary, "x/click");
            BIND(g_aSecondary, "y/click");
            BIND(g_aMenu, "menu/click");
        } else {
            BIND(g_aPrimary, "a/click");
            BIND(g_aSecondary, "b/click");
        }
#undef BIND
    }
    memset(&sb, 0, sizeof sb);
    sb.type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING;
    sb.interactionProfile = path("/interaction_profiles/oculus/touch_controller");
    sb.countSuggestedBindings = nb;
    sb.suggestedBindings = b;
    CK(fn.xrSuggestInteractionProfileBindings(g_inst, &sb));

    memset(&ai, 0, sizeof ai);
    ai.type = XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO;
    ai.countActionSets = 1;
    ai.actionSets = &g_set;
    CK(fn.xrAttachSessionActionSets(g_sess, &ai));

    for (h = 0; h < 2; ++h) {
        XrActionSpaceCreateInfo ci;
        memset(&ci, 0, sizeof ci);
        ci.type = XR_TYPE_ACTION_SPACE_CREATE_INFO;
        ci.subactionPath = g_handPath[h];
        ci.poseInActionSpace = identityPose();
        ci.action = g_aGrip;
        CK(fn.xrCreateActionSpace(g_sess, &ci, &g_gripSpace[h]));
        ci.action = g_aAim;
        CK(fn.xrCreateActionSpace(g_sess, &ci, &g_aimSpace[h]));
    }
    return 1;
}

/* ------------------------------------------------------------------------- */
/* runtime diagnostics                                                        */
/* ------------------------------------------------------------------------- */

/* Which OpenXR runtime is active (Quest Link, SteamVR, Virtual Desktop's VDXR, ...) is a
 * per-machine setting, so say which one we are talking to. */
static void logActiveRuntime(void)
{
    char buf[512];
    DWORD sz = sizeof buf;
    const char *env = getenv("XR_RUNTIME_JSON");
    if (env && *env)
        logf_("XR_RUNTIME_JSON override: %s", env);
    if (RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Khronos\\OpenXR\\1", "ActiveRuntime", RRF_RT_REG_SZ, NULL, buf,
                     &sz) == ERROR_SUCCESS)
        logf_("active OpenXR runtime: %s", buf);
    else
        logf_("no ActiveRuntime registered under HKLM\\SOFTWARE\\Khronos\\OpenXR\\1");
}

/* Returns 1 if the runtime offers XR_KHR_opengl_enable, 0 if it does not, -1 if it could not
 * be determined. Also logs the graphics bindings the runtime does offer. */
static int runtimeHasOpenGL(void)
{
    PFN_xrEnumerateInstanceExtensionProperties en = NULL;
    uint32_t n = 0, i;
    XrExtensionProperties *p;
    int gl = 0;

    if (XR_FAILED(g_getProcAddr(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties",
                                (PFN_xrVoidFunction *)&en)) || !en)
        return -1;
    if (XR_FAILED(en(NULL, 0, &n, NULL)) || n == 0)
        return -1;
    p = (XrExtensionProperties *)calloc(n, sizeof *p);
    for (i = 0; i < n; ++i)
        p[i].type = XR_TYPE_EXTENSION_PROPERTIES;
    if (XR_FAILED(en(NULL, n, &n, p))) {
        free(p);
        return -1;
    }
    for (i = 0; i < n; ++i) {
        const char *nm = p[i].extensionName;
        if (!strcmp(nm, XR_KHR_OPENGL_ENABLE_EXTENSION_NAME))
            gl = 1;
        if (!strncmp(nm, "XR_KHR_", 7) && strstr(nm, "_enable"))
            logf_("  runtime offers %s", nm);
    }
    free(p);
    return gl;
}

/* ------------------------------------------------------------------------- */
/* init / shutdown                                                            */
/* ------------------------------------------------------------------------- */

int ndvr_init(void *hdc, void *hglrc)
{
    HMODULE m;
    PFN_xrCreateInstance createInstance = NULL;
    const char *exts[1];
    XrInstanceCreateInfo ici;
    XrSystemGetInfo gi;
    XrGraphicsRequirementsOpenGLKHR req;
    XrGraphicsBindingOpenGLWin32KHR gb;
    XrSessionCreateInfo sci;
    XrReferenceSpaceCreateInfo rs;
    XrViewConfigurationView views[2];
    uint32_t n = 0, i;
    XrResult r;

    if (g_inited)
        return 1;
    g_err[0] = 0;

    if (!loadGL()) {
        fail("OpenGL framebuffer functions unavailable (is the GL context current on this thread?)");
        return 0;
    }

    m = LoadLibraryA("openxr_loader.dll");
    if (!m) {
        fail("could not load openxr_loader.dll (put the 64-bit loader next to the exe)");
        return 0;
    }
    g_getProcAddr = (PFN_xrGetInstanceProcAddr)(void *)GetProcAddress(m, "xrGetInstanceProcAddr");
    if (!g_getProcAddr ||
        XR_FAILED(g_getProcAddr(XR_NULL_HANDLE, "xrCreateInstance", (PFN_xrVoidFunction *)&createInstance)) ||
        !createInstance) {
        fail("openxr_loader.dll is missing xrGetInstanceProcAddr / xrCreateInstance");
        return 0;
    }

    logActiveRuntime();
    if (runtimeHasOpenGL() == 0) {
        fail("The active OpenXR runtime does not offer OpenGL (XR_KHR_opengl_enable). "
             "Switch runtime: in Virtual Desktop Streamer set the OpenXR runtime to SteamVR; with "
             "Quest Link set Meta Quest Link as the OpenXR runtime. If it persists, send the lines "
             "above (they list what the runtime does support).");
        return 0;
    }

    exts[0] = XR_KHR_OPENGL_ENABLE_EXTENSION_NAME;
    memset(&ici, 0, sizeof ici);
    ici.type = XR_TYPE_INSTANCE_CREATE_INFO;
    strcpy(ici.applicationInfo.applicationName, "NewDarkVR");
    ici.applicationInfo.applicationVersion = 1;
    strcpy(ici.applicationInfo.engineName, "NewDarkRecompiled");
    ici.enabledExtensionCount = 1;
    ici.enabledExtensionNames = exts;
    {
        /* Ask for OpenXR 1.0, not the newest version the headers know. A runtime refuses a version
         * newer than it implements (XR_ERROR_API_VERSION_UNSUPPORTED), while 1.0 is honored by 1.0
         * and 1.1 runtimes alike, and everything this module uses is core 1.0 plus the GL extension. */
        static const XrVersion tries[2] = {XR_API_VERSION_1_0, XR_MAKE_VERSION(1, 0, 0)};
        int t;
        r = XR_ERROR_API_VERSION_UNSUPPORTED;
        for (t = 0; t < 2 && r == XR_ERROR_API_VERSION_UNSUPPORTED; ++t) {
            ici.applicationInfo.apiVersion = tries[t];
            r = createInstance(&ici, &g_inst);
        }
    }
    if (XR_FAILED(r)) {
        const char *hint;
        if (r == XR_ERROR_API_VERSION_UNSUPPORTED)
            hint = "the runtime rejected the OpenXR version requested";
        else if (r == XR_ERROR_RUNTIME_UNAVAILABLE || r == XR_ERROR_INITIALIZATION_FAILED ||
                 r == XR_ERROR_RUNTIME_FAILURE)
            hint = "the runtime is not running: start Quest Link / SteamVR / Virtual Desktop Streamer and "
                   "connect the headset";
        else if (r == XR_ERROR_EXTENSION_NOT_PRESENT)
            hint = "the runtime lacks XR_KHR_opengl_enable: switch runtime (see README)";
        else
            hint = "see the OpenXR loader messages above";
        fail("xrCreateInstance failed: %s (%d): %s", resultName(r), (int)r, hint);
        return 0;
    }

#define X(nm)                                                                                  \
    if (XR_FAILED(g_getProcAddr(g_inst, #nm, (PFN_xrVoidFunction *)&fn.nm)) || !fn.nm) {       \
        fail("could not resolve " #nm);                                                        \
        return 0;                                                                              \
    }
    XR_FN_LIST(X)
#undef X

    {
        XrInstanceProperties ip;
        memset(&ip, 0, sizeof ip);
        ip.type = XR_TYPE_INSTANCE_PROPERTIES;
        if (XR_SUCCEEDED(fn.xrGetInstanceProperties(g_inst, &ip)))
            logf_("runtime: %s %u.%u.%u", ip.runtimeName, (unsigned)XR_VERSION_MAJOR(ip.runtimeVersion),
                  (unsigned)XR_VERSION_MINOR(ip.runtimeVersion), (unsigned)XR_VERSION_PATCH(ip.runtimeVersion));
    }

    memset(&gi, 0, sizeof gi);
    gi.type = XR_TYPE_SYSTEM_GET_INFO;
    gi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    r = fn.xrGetSystem(g_inst, &gi, &g_sys);
    if (XR_FAILED(r)) {
        fail("xrGetSystem failed (%d): no headset available", (int)r);
        return 0;
    }

    memset(&req, 0, sizeof req);
    req.type = XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR;
    CK(fn.xrGetOpenGLGraphicsRequirementsKHR(g_inst, g_sys, &req));
    {
        const char *ver = (const char *)glGetString(GL_VERSION);
        logf_("GL %s; runtime needs %d.%d..%d.%d", ver ? ver : "?",
              (int)XR_VERSION_MAJOR(req.minApiVersionSupported), (int)XR_VERSION_MINOR(req.minApiVersionSupported),
              (int)XR_VERSION_MAJOR(req.maxApiVersionSupported), (int)XR_VERSION_MINOR(req.maxApiVersionSupported));
    }

    memset(&gb, 0, sizeof gb);
    gb.type = XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR;
    gb.hDC = (HDC)hdc;
    gb.hGLRC = (HGLRC)hglrc;
    memset(&sci, 0, sizeof sci);
    sci.type = XR_TYPE_SESSION_CREATE_INFO;
    sci.next = &gb;
    sci.systemId = g_sys;
    CK(fn.xrCreateSession(g_inst, &sci, &g_sess));

    memset(&rs, 0, sizeof rs);
    rs.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO;
    rs.poseInReferenceSpace = identityPose();
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    CK(fn.xrCreateReferenceSpace(g_sess, &rs, &g_local));
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    CK(fn.xrCreateReferenceSpace(g_sess, &rs, &g_view));

    memset(views, 0, sizeof views);
    for (i = 0; i < 2; ++i)
        views[i].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
    CK(fn.xrEnumerateViewConfigurationViews(g_inst, g_sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &n, views));
    if (n < 2) {
        fail("runtime reports %u views, expected 2", n);
        return 0;
    }
    logf_("recommended eye size %ux%u", views[0].recommendedImageRectWidth, views[0].recommendedImageRectHeight);

    if (!pickFormat())
        return 0;
    for (i = 0; i < 2; ++i)
        if (!createEye(&g_eye[i], views[i].recommendedImageRectWidth, views[i].recommendedImageRectHeight))
            return 0;
    if (!createActions())
        return 0;

    g_inited = 1;
    logf_("initialised; waiting for the headset session to start");
    return 1;
}

void ndvr_shutdown(void)
{
    int h;
    if (!g_inited && !g_inst)
        return;
    if (g_running)
        fn.xrEndSession(g_sess);
    for (h = 0; h < 2; ++h) {
        if (g_gripSpace[h]) fn.xrDestroySpace(g_gripSpace[h]);
        if (g_aimSpace[h]) fn.xrDestroySpace(g_aimSpace[h]);
        destroyEye(&g_eye[h]);
    }
    if (g_set) fn.xrDestroyActionSet(g_set);
    if (g_view) fn.xrDestroySpace(g_view);
    if (g_local) fn.xrDestroySpace(g_local);
    if (g_sess) fn.xrDestroySession(g_sess);
    if (g_inst) fn.xrDestroyInstance(g_inst);
    memset(&g_eye, 0, sizeof g_eye);
    g_gripSpace[0] = g_gripSpace[1] = g_aimSpace[0] = g_aimSpace[1] = XR_NULL_HANDLE;
    g_set = XR_NULL_HANDLE;
    g_view = g_local = XR_NULL_HANDLE;
    g_sess = XR_NULL_HANDLE;
    g_inst = XR_NULL_HANDLE;
    g_running = g_inited = g_frameBegun = 0;
}

int ndvr_session_running(void) { return g_running; }
int ndvr_wants_exit(void) { return g_exit; }

/* ------------------------------------------------------------------------- */
/* events                                                                     */
/* ------------------------------------------------------------------------- */

static void pollEvents(void)
{
    for (;;) {
        XrEventDataBuffer ev;
        memset(&ev, 0, sizeof ev);
        ev.type = XR_TYPE_EVENT_DATA_BUFFER;
        if (fn.xrPollEvent(g_inst, &ev) != XR_SUCCESS)
            return;
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            XrEventDataSessionStateChanged *sc = (XrEventDataSessionStateChanged *)&ev;
            g_state = sc->state;
            logf_("session state -> %d", (int)g_state);
            switch (g_state) {
            case XR_SESSION_STATE_READY: {
                XrSessionBeginInfo bi;
                memset(&bi, 0, sizeof bi);
                bi.type = XR_TYPE_SESSION_BEGIN_INFO;
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                if (XR_SUCCEEDED(fn.xrBeginSession(g_sess, &bi)))
                    g_running = 1;
                break;
            }
            case XR_SESSION_STATE_STOPPING:
                fn.xrEndSession(g_sess);
                g_running = 0;
                break;
            case XR_SESSION_STATE_EXITING:
            case XR_SESSION_STATE_LOSS_PENDING:
                g_exit = 1;
                break;
            default:
                break;
            }
        } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            g_exit = 1;
        }
    }
}

/* ------------------------------------------------------------------------- */
/* input                                                                      */
/* ------------------------------------------------------------------------- */

static void updateHands(XrTime t, ndvr_hand out[2])
{
    XrActiveActionSet as;
    XrActionsSyncInfo si;
    int h, k;

    as.actionSet = g_set;
    as.subactionPath = XR_NULL_PATH;
    memset(&si, 0, sizeof si);
    si.type = XR_TYPE_ACTIONS_SYNC_INFO;
    si.countActiveActionSets = 1;
    si.activeActionSets = &as;
    fn.xrSyncActions(g_sess, &si); /* XR_SESSION_NOT_FOCUSED is fine: states come back inactive */

    for (h = 0; h < 2; ++h) {
        ndvr_hand *o = &out[h];
        XrSpaceLocation gl_, al_;
        XrActionStateGetInfo gi;
        XrActionStateFloat f;
        XrActionStateVector2f v;
        struct { XrAction a; uint32_t bit; } btn[4];

        memset(o, 0, sizeof *o);
        memset(&gl_, 0, sizeof gl_);
        gl_.type = XR_TYPE_SPACE_LOCATION;
        if (XR_SUCCEEDED(fn.xrLocateSpace(g_gripSpace[h], g_local, t, &gl_))) {
            toPose(&gl_.pose, gl_.locationFlags, &o->grip);
            o->active = (gl_.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) ? 1 : 0;
        }
        memset(&al_, 0, sizeof al_);
        al_.type = XR_TYPE_SPACE_LOCATION;
        if (XR_SUCCEEDED(fn.xrLocateSpace(g_aimSpace[h], g_local, t, &al_)))
            toPose(&al_.pose, al_.locationFlags, &o->aim);

        memset(&gi, 0, sizeof gi);
        gi.type = XR_TYPE_ACTION_STATE_GET_INFO;
        gi.subactionPath = g_handPath[h];

        memset(&f, 0, sizeof f);
        f.type = XR_TYPE_ACTION_STATE_FLOAT;
        gi.action = g_aTrigger;
        if (XR_SUCCEEDED(fn.xrGetActionStateFloat(g_sess, &gi, &f)) && f.isActive)
            o->trigger = f.currentState;
        memset(&f, 0, sizeof f);
        f.type = XR_TYPE_ACTION_STATE_FLOAT;
        gi.action = g_aSqueeze;
        if (XR_SUCCEEDED(fn.xrGetActionStateFloat(g_sess, &gi, &f)) && f.isActive)
            o->squeeze = f.currentState;

        memset(&v, 0, sizeof v);
        v.type = XR_TYPE_ACTION_STATE_VECTOR2F;
        gi.action = g_aStick;
        if (XR_SUCCEEDED(fn.xrGetActionStateVector2f(g_sess, &gi, &v)) && v.isActive) {
            o->stick_x = v.currentState.x;
            o->stick_y = v.currentState.y;
        }

        btn[0].a = g_aPrimary;    btn[0].bit = NDVR_BTN_PRIMARY;
        btn[1].a = g_aSecondary;  btn[1].bit = NDVR_BTN_SECONDARY;
        btn[2].a = g_aStickClick; btn[2].bit = NDVR_BTN_STICK_CLICK;
        btn[3].a = g_aMenu;       btn[3].bit = NDVR_BTN_MENU;
        for (k = 0; k < 4; ++k) {
            XrActionStateBoolean s;
            memset(&s, 0, sizeof s);
            s.type = XR_TYPE_ACTION_STATE_BOOLEAN;
            gi.action = btn[k].a;
            if (XR_SUCCEEDED(fn.xrGetActionStateBoolean(g_sess, &gi, &s)) && s.isActive && s.currentState)
                o->buttons |= btn[k].bit;
        }
    }
}

/* ------------------------------------------------------------------------- */
/* frame loop                                                                 */
/* ------------------------------------------------------------------------- */

int ndvr_begin_frame(ndvr_frame *out)
{
    XrFrameWaitInfo wi;
    XrFrameState fs;
    XrFrameBeginInfo bi;
    XrViewLocateInfo vli;
    XrViewState vs;
    XrSpaceLocation hl;
    uint32_t vc = 0, e;

    memset(out, 0, sizeof *out);
    g_frameBegun = g_framRendered = g_boundThisFrame = 0;
    if (!g_inited)
        return 0;

    pollEvents();
    if (!g_running)
        return 0;

    memset(&wi, 0, sizeof wi);
    wi.type = XR_TYPE_FRAME_WAIT_INFO;
    memset(&fs, 0, sizeof fs);
    fs.type = XR_TYPE_FRAME_STATE;
    if (XR_FAILED(fn.xrWaitFrame(g_sess, &wi, &fs)))
        return 0;
    memset(&bi, 0, sizeof bi);
    bi.type = XR_TYPE_FRAME_BEGIN_INFO;
    if (XR_FAILED(fn.xrBeginFrame(g_sess, &bi)))
        return 0;
    g_frameBegun = 1;
    g_frameTime = fs.predictedDisplayTime;
    out->display_time = (int64_t)fs.predictedDisplayTime;

    memset(&vli, 0, sizeof vli);
    vli.type = XR_TYPE_VIEW_LOCATE_INFO;
    vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    vli.displayTime = fs.predictedDisplayTime;
    vli.space = g_local;
    memset(&vs, 0, sizeof vs);
    vs.type = XR_TYPE_VIEW_STATE;
    memset(g_views, 0, sizeof g_views);
    g_views[0].type = g_views[1].type = XR_TYPE_VIEW;
    fn.xrLocateViews(g_sess, &vli, &vs, 2, &vc, g_views);

    memset(&hl, 0, sizeof hl);
    hl.type = XR_TYPE_SPACE_LOCATION;
    fn.xrLocateSpace(g_view, g_local, fs.predictedDisplayTime, &hl);
    toPose(&hl.pose, hl.locationFlags, &out->head);
    updateHands(fs.predictedDisplayTime, out->hand);

    if (!fs.shouldRender || vc < 2)
        return 0;

    for (e = 0; e < 2; ++e) {
        XrSwapchainImageAcquireInfo ai;
        XrSwapchainImageWaitInfo wait;
        memset(&ai, 0, sizeof ai);
        ai.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO;
        if (XR_FAILED(fn.xrAcquireSwapchainImage(g_eye[e].sc, &ai, &g_eye[e].acquired)))
            return 0;
        memset(&wait, 0, sizeof wait);
        wait.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO;
        wait.timeout = XR_INFINITE_DURATION;
        if (XR_FAILED(fn.xrWaitSwapchainImage(g_eye[e].sc, &wait)))
            return 0;
        out->eye[e].fbo = g_eye[e].fbo[g_eye[e].acquired];
        out->eye[e].width = (int)g_eye[e].w;
        out->eye[e].height = (int)g_eye[e].h;
        toPose(&g_views[e].pose,
               (XrSpaceLocationFlags)(XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT),
               &out->eye[e].pose);
        out->eye[e].fov.left = g_views[e].fov.angleLeft;
        out->eye[e].fov.right = g_views[e].fov.angleRight;
        out->eye[e].fov.up = g_views[e].fov.angleUp;
        out->eye[e].fov.down = g_views[e].fov.angleDown;
    }
    out->should_render = 1;
    g_framRendered = 1;
    return 1;
}

void ndvr_end_frame(void)
{
    XrFrameEndInfo ei;
    XrCompositionLayerProjectionView pv[2];
    XrCompositionLayerProjection proj;
    const XrCompositionLayerBaseHeader *layers[1];
    uint32_t e;

    if (!g_frameBegun)
        return;
    memset(&ei, 0, sizeof ei);
    ei.type = XR_TYPE_FRAME_END_INFO;
    ei.displayTime = g_frameTime;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;

    if (g_framRendered) {
        /* make sure the eye images are fully written before the compositor reads them */
        glFlush();
        memset(pv, 0, sizeof pv);
        for (e = 0; e < 2; ++e) {
            XrSwapchainImageReleaseInfo ri;
            memset(&ri, 0, sizeof ri);
            ri.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO;
            fn.xrReleaseSwapchainImage(g_eye[e].sc, &ri);
            pv[e].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
            pv[e].pose = g_views[e].pose;
            pv[e].fov = g_views[e].fov;
            pv[e].subImage.swapchain = g_eye[e].sc;
            pv[e].subImage.imageRect.offset.x = 0;
            pv[e].subImage.imageRect.offset.y = 0;
            pv[e].subImage.imageRect.extent.width = (int32_t)g_eye[e].w;
            pv[e].subImage.imageRect.extent.height = (int32_t)g_eye[e].h;
            pv[e].subImage.imageArrayIndex = 0;
        }
        memset(&proj, 0, sizeof proj);
        proj.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION;
        proj.space = g_local;
        proj.viewCount = 2;
        proj.views = pv;
        layers[0] = (const XrCompositionLayerBaseHeader *)&proj;
        ei.layerCount = 1;
        ei.layers = layers;
    }
    fn.xrEndFrame(g_sess, &ei);
    g_frameBegun = g_framRendered = g_boundThisFrame = 0;
}

void ndvr_bind_eye(const ndvr_eye *eye)
{
    if (!g_boundThisFrame) {
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &g_prevFbo);
        g_boundThisFrame = 1;
    }
    gl.bindFramebuffer(GL_FRAMEBUFFER, eye->fbo);
    glViewport(0, 0, eye->width, eye->height);
}

void ndvr_unbind(void)
{
    if (g_boundThisFrame)
        gl.bindFramebuffer(GL_FRAMEBUFFER, (GLuint)g_prevFbo);
    g_boundThisFrame = 0;
}

/*
 * ndvr_gltest.exe - standalone stereo test for the NewDarkVR OpenXR module.
 *
 * Opens a small Win32 window with a legacy OpenGL context, starts a headset session on it, and
 * draws a spinning cube, a floor grid and a cube on each controller in both eyes. It exercises
 * everything the game integration will use (session, per-eye FBOs, off-center projections,
 * tracking, input) without touching the engine, so if this looks right in the headset the VR
 * plumbing is proven and any remaining problems are in the engine work.
 *
 * What you should see: a cube floating 1.5 m ahead with real depth, a grid on the floor, a blue
 * cube in your left hand and an orange one in your right. Squeezing a trigger makes that
 * hand's cube bigger. Turning your head should not move the world.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <GL/gl.h>
#include <math.h>
#include <stdio.h>

#include "ndvr.h"

#ifndef GL_MULTISAMPLE
#define GL_MULTISAMPLE 0x809D
#endif

static int g_quit;

static LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_CLOSE || m == WM_DESTROY) {
        g_quit = 1;
        return 0;
    }
    if (m == WM_KEYDOWN && w == VK_ESCAPE) {
        g_quit = 1;
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

static void cube(float s, float r, float g, float b)
{
    /* six faces, each slightly shaded so depth is readable */
    static const float n[6][3] = {{0, 0, 1}, {0, 0, -1}, {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}};
    static const float shade[6] = {1.0f, 0.55f, 0.85f, 0.7f, 0.95f, 0.45f};
    static const float v[6][4][3] = {
        {{-1, -1, 1}, {1, -1, 1}, {1, 1, 1}, {-1, 1, 1}},
        {{1, -1, -1}, {-1, -1, -1}, {-1, 1, -1}, {1, 1, -1}},
        {{1, -1, 1}, {1, -1, -1}, {1, 1, -1}, {1, 1, 1}},
        {{-1, -1, -1}, {-1, -1, 1}, {-1, 1, 1}, {-1, 1, -1}},
        {{-1, 1, 1}, {1, 1, 1}, {1, 1, -1}, {-1, 1, -1}},
        {{-1, -1, -1}, {1, -1, -1}, {1, -1, 1}, {-1, -1, 1}},
    };
    int f, i;
    (void)n;
    glBegin(GL_QUADS);
    for (f = 0; f < 6; ++f) {
        glColor3f(r * shade[f], g * shade[f], b * shade[f]);
        for (i = 0; i < 4; ++i)
            glVertex3f(v[f][i][0] * s, v[f][i][1] * s, v[f][i][2] * s);
    }
    glEnd();
}

static void grid(float y)
{
    int i;
    glBegin(GL_LINES);
    for (i = -10; i <= 10; ++i) {
        float c = (i == 0) ? 0.6f : 0.25f;
        glColor3f(c, c, c + 0.1f);
        glVertex3f((float)i * 0.5f, y, -5.0f);
        glVertex3f((float)i * 0.5f, y, 5.0f);
        glVertex3f(-5.0f, y, (float)i * 0.5f);
        glVertex3f(5.0f, y, (float)i * 0.5f);
    }
    glEnd();
}

static void drawHand(const ndvr_hand *h, float r, float g, float b)
{
    float m[16], s;
    if (!h->active)
        return;
    glPushMatrix();
    ndvr_pose_to_matrix(&h->grip, m);
    glMultMatrixf(m);
    s = 0.04f + 0.04f * h->trigger;
    cube(s, r, g, b);
    glPopMatrix();

    /* aim ray */
    glPushMatrix();
    ndvr_pose_to_matrix(&h->aim, m);
    glMultMatrixf(m);
    glBegin(GL_LINES);
    glColor3f(1, 1, 1);
    glVertex3f(0, 0, 0);
    glColor3f(r, g, b);
    glVertex3f(0, 0, -1.5f);
    glEnd();
    glPopMatrix();
}

static void drawScene(const ndvr_frame *f, int e, float t)
{
    float proj[16], view[16];

    ndvr_bind_eye(&f->eye[e]);
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_LIGHTING);
    glClearColor(0.02f, 0.03f, 0.06f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    ndvr_projection_matrix(&f->eye[e].fov, 0.05f, 100.0f, proj);
    ndvr_view_matrix(&f->eye[e].pose, view);
    glMatrixMode(GL_PROJECTION);
    glLoadMatrixf(proj);
    glMatrixMode(GL_MODELVIEW);
    glLoadMatrixf(view);

    grid(-1.5f);

    glPushMatrix();
    glTranslatef(0.0f, 0.0f, -1.5f);
    glRotatef(t * 40.0f, 0.3f, 1.0f, 0.2f);
    cube(0.2f, 0.9f, 0.9f, 0.3f);
    glPopMatrix();

    drawHand(&f->hand[NDVR_HAND_LEFT], 0.2f, 0.5f, 1.0f);
    drawHand(&f->hand[NDVR_HAND_RIGHT], 1.0f, 0.55f, 0.15f);
    ndvr_unbind();
}

int main(void)
{
    HINSTANCE inst = GetModuleHandleA(NULL);
    WNDCLASSA wc = {0};
    HWND hwnd;
    HDC hdc;
    HGLRC rc;
    PIXELFORMATDESCRIPTOR pfd = {0};
    int fmt;
    ndvr_frame f;
    DWORD t0 = GetTickCount();
    unsigned frames = 0;
    DWORD lastReport = t0;
    MSG msg;

    wc.lpfnWndProc = wndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    wc.lpszClassName = "ndvr_gltest";
    RegisterClassA(&wc);
    hwnd = CreateWindowA(wc.lpszClassName, "NewDarkVR test (Esc to quit)", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                         100, 100, 640, 360, NULL, NULL, inst, NULL);
    hdc = GetDC(hwnd);

    pfd.nSize = sizeof pfd;
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    pfd.cStencilBits = 8;
    pfd.iLayerType = PFD_MAIN_PLANE;
    fmt = ChoosePixelFormat(hdc, &pfd);
    if (!fmt || !SetPixelFormat(hdc, fmt, &pfd)) {
        fprintf(stderr, "could not set a pixel format\n");
        return 1;
    }
    rc = wglCreateContext(hdc);
    if (!rc || !wglMakeCurrent(hdc, rc)) {
        fprintf(stderr, "could not create an OpenGL context\n");
        return 1;
    }

    if (!ndvr_init(hdc, rc)) {
        fprintf(stderr, "\nNewDarkVR init failed: %s\n", ndvr_last_error());
        return 1;
    }
    printf("running. Put the headset on. Esc or closing the window quits.\n");

    while (!g_quit && !ndvr_wants_exit()) {
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if (ndvr_begin_frame(&f)) {
            float t = (float)(GetTickCount() - t0) / 1000.0f;
            drawScene(&f, 0, t);
            drawScene(&f, 1, t);
            ++frames;
        }
        ndvr_end_frame();

        if (!ndvr_session_running())
            Sleep(20); /* waiting for the runtime to start the session */

        if (GetTickCount() - lastReport > 2000) {
            printf("session %s, %.1f fps  head (%+.2f %+.2f %+.2f)  trig L %.2f R %.2f\n",
                   ndvr_session_running() ? "running" : "waiting",
                   frames * 1000.0f / (float)(GetTickCount() - lastReport),
                   f.head.position.x, f.head.position.y, f.head.position.z,
                   f.hand[NDVR_HAND_LEFT].trigger, f.hand[NDVR_HAND_RIGHT].trigger);
            frames = 0;
            lastReport = GetTickCount();
        }
    }

    ndvr_shutdown();
    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(rc);
    return 0;
}

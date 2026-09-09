/**
 * @file tools/frametime_probe.c
 * @brief A GLX workload that records its own frame times.
 *
 * The frame limiter benchmark needs per-frame timings to report pacing spread,
 * not just an average FPS. It used to get them from MangoHud's CSV log, which
 * makes MangoHud both the limiter under test and the measuring instrument, and
 * which produces nothing at all on MangoHud 0.8.4. This probe removes that
 * dependency: it renders a trivial frame, swaps, and timestamps every swap.
 *
 * Limiters hook glXSwapBuffers, so whatever they do to the cadence shows up
 * here exactly as a game would experience it. Vsync is switched off so the
 * only cap is the limiter under test.
 *
 * Build:
 *   cc -O2 tools/frametime_probe.c -lGL -lX11 -o frametime_probe
 * Run:
 *   frametime_probe <seconds> > frametimes.csv
 */
#include <GL/gl.h>
#include <GL/glx.h>
#include <X11/Xlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef void (*swap_interval_ext_fn)(Display *, GLXDrawable, int);

static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

int main(int argc, char **argv) {
  const double seconds = argc > 1 ? atof(argv[1]) : 10.0;

  Display *display = XOpenDisplay(NULL);
  if (!display) {
    fprintf(stderr, "frametime_probe: cannot open display\n");
    return 1;
  }

  int attributes[] = {GLX_RGBA, GLX_DOUBLEBUFFER, GLX_DEPTH_SIZE, 1, None};
  XVisualInfo *visual = glXChooseVisual(display, DefaultScreen(display), attributes);
  if (!visual) {
    fprintf(stderr, "frametime_probe: no suitable GLX visual\n");
    return 1;
  }

  Window root = DefaultRootWindow(display);
  XSetWindowAttributes window_attributes;
  memset(&window_attributes, 0, sizeof(window_attributes));
  window_attributes.colormap = XCreateColormap(display, root, visual->visual, AllocNone);
  window_attributes.event_mask = StructureNotifyMask;

  Window window = XCreateWindow(
    display, root, 0, 0, 640, 360, 0, visual->depth, InputOutput, visual->visual,
    CWColormap | CWEventMask, &window_attributes
  );
  XStoreName(display, window, "frametime_probe");
  XMapWindow(display, window);

  GLXContext context = glXCreateContext(display, visual, NULL, True);
  if (!context) {
    fprintf(stderr, "frametime_probe: cannot create GLX context\n");
    return 1;
  }
  glXMakeCurrent(display, window, context);

  // Without this the probe measures the display's refresh rate, not the limiter.
  swap_interval_ext_fn swap_interval =
    (swap_interval_ext_fn) glXGetProcAddress((const GLubyte *) "glXSwapIntervalEXT");
  if (swap_interval) {
    swap_interval(display, window, 0);
  }

  printf("frametime_ms\n");

  const double started = now_ms();
  double previous = started;
  for (;;) {
    glClearColor(0.1f, 0.2f, 0.3f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glXSwapBuffers(display, window);

    const double current = now_ms();
    printf("%.4f\n", current - previous);
    previous = current;

    if (current - started >= seconds * 1000.0) {
      break;
    }
  }

  glXMakeCurrent(display, None, NULL);
  glXDestroyContext(display, context);
  XDestroyWindow(display, window);
  XCloseDisplay(display);
  return 0;
}

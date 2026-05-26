#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <GL/gl.h>
#include <GL/glx.h>
#include <X11/X.h>
#include <X11/Xlib.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/Xrandr.h>
#include <X11/extensions/shape.h>

typedef struct Client
{
  Window win;

  int x, y, w, h;
  int border_width;
  int depth;
  VisualID visualid;

  Pixmap pixmap;
  GLXPixmap glx_pixmap;
  GLuint texid;
  bool tex_needs_bind;
  bool tex_bound;

  bool mapped;

  Damage damage;

  struct Client* next;
} Client;

typedef struct
{
  Display* display;
  int screen;
  Window root;

  bool running;

  bool have_xrandr;

  Atom comp_atom;
  Window comp_owner_win;

  Window overlay;

  int32_t root_width, root_height;

  Client* clients;

  bool needs_rerender;

  int damage_event_base, randr_event_base;
} Compositor;

typedef struct
{
  GLXContext context;
} Renderer;

static void update_root_geom(Compositor* c, Renderer* r);

static PFNGLXBINDTEXIMAGEEXTPROC glXBindTexImageEXT_p = NULL;
static PFNGLXRELEASETEXIMAGEEXTPROC glXReleaseTexImageEXT_p = NULL;

static int g_damage_error_base = 0;

static bool should_manage_window(Compositor* c, Window w)
{
  return w != None &&
    w != c->root &&
    w != c->overlay &&
    w != c->comp_owner_win;
}

static int x_error_handler(Display* dpy, XErrorEvent* e)
{
  if(e->error_code == g_damage_error_base + BadDamage)
  {
    return 0;
  }

  char msg[256];
  XGetErrorText(dpy, e->error_code, msg, sizeof(msg));

  fprintf(stderr,
      "nikom: X error: %s, request=%d, minor=%d, resource=0x%lx\n",
      msg,
      e->request_code,
      e->minor_code,
      e->resourceid);

  return 0;
}

static bool compositor_init(
    Compositor* c,
    Display* display,
    int screen)
{
  c->display = display;
  c->screen = screen;
  c->running = true;
  c->root = RootWindow(c->display, c->screen);
  c->needs_rerender = true;

  int xc_event_base_return = 0;
  int xc_event_error_return = 0;
  if(!XCompositeQueryExtension(
        c->display,
        &xc_event_base_return,
        &xc_event_error_return))
  {
    fprintf(stderr, "nikom: XComposite extension is missing.\n");
    return false;
  }

  int xcomposite_major = 0;
  int xcomposite_minor = 0;
  XCompositeQueryVersion(
      c->display,
      &xcomposite_major,
      &xcomposite_minor);

  if(xcomposite_major == 0 && xcomposite_minor < 2)
  {
    fprintf(stderr,
        "nikom: XComposite version >= 0.2 is required, only got: %i.%i.\n",
        xcomposite_major,
        xcomposite_minor);
    return false;
  }

  int xd_event_base_return = 0;
  int xd_event_error_return = 0;
  if(!XDamageQueryExtension(
        c->display,
        &xd_event_base_return,
        &xd_event_error_return))
  {
    fprintf(stderr, "nikom: XDamage extension is missing.\n");
    return false;
  }

  c->damage_event_base = xd_event_base_return;
  g_damage_error_base = xd_event_error_return;

  int xf_event_base_return = 0;
  int xf_event_error_return = 0;
  if(!XFixesQueryExtension(
        c->display,
        &xf_event_base_return,
        &xf_event_error_return))
  {
    fprintf(stderr, "nikom: XFixes extension is missing.\n");
    return false;
  }

  int xr_event_base_return = 0;
  int xr_event_error_return = 0;
  c->have_xrandr = XRRQueryExtension(
      c->display,
      &xr_event_base_return,
      &xr_event_error_return);

  if(c->have_xrandr)
  {
    c->randr_event_base = xr_event_base_return;
  }
  else
  {
    fprintf(stderr, "nikom: warning: XRandr extension is missing.\n");
  }

  int glx_major = 0;
  int glx_minor = 0;
  if(!glXQueryVersion(c->display, &glx_major, &glx_minor))
  {
    fprintf(stderr, "nikom: GLX is missing.\n");
    return false;
  }

  if(glx_major < 1 || (glx_major == 1 && glx_minor < 1))
  {
    fprintf(stderr,
        "nikom: GLX version >= 1.1 is required, only got: %i.%i.\n",
        glx_major,
        glx_minor);
    return false;
  }

  const char* glx_extensions = glXQueryExtensionsString(c->display, c->screen);

  bool have_pixmap_ext = glx_extensions &&
    strstr(glx_extensions, "GLX_EXT_texture_from_pixmap");

  if(!have_pixmap_ext)
  {
    fprintf(stderr, "nikom: GLX_EXT_texture_from_pixmap is missing.\n");
    return false;
  }

  glXBindTexImageEXT_p = (PFNGLXBINDTEXIMAGEEXTPROC)
    glXGetProcAddressARB((const GLubyte*)"glXBindTexImageEXT");

  glXReleaseTexImageEXT_p = (PFNGLXRELEASETEXIMAGEEXTPROC)
    glXGetProcAddressARB((const GLubyte*)"glXReleaseTexImageEXT");

  if(!glXBindTexImageEXT_p || !glXReleaseTexImageEXT_p)
  {
    fprintf(stderr,
        "nikom: Could not load glXBindTexImageEXT or "
        "glXReleaseTexImageEXT functions.\n");
    return false;
  }

  char atom[64];
  snprintf(atom, sizeof(atom), "_NET_WM_CM_S%i", c->screen);

  c->comp_atom = XInternAtom(c->display, atom, False);

  Window owner = XGetSelectionOwner(c->display, c->comp_atom);
  if(owner != None)
  {
    fprintf(stderr,
        "nikom: Another compositor is already running (window 0x%lx).\n",
        owner);
    return false;
  }

  c->comp_owner_win = XCreateSimpleWindow(c->display,
      c->root,
      -1, -1,
      1, 1,
      0, 0, 0);
  if(!c->comp_owner_win)
  {
    fprintf(stderr, "nikom: Failed to create compositor owner window.\n");
    return false;
  }

  XSetSelectionOwner(c->display, c->comp_atom, c->comp_owner_win, CurrentTime);

  if(XGetSelectionOwner(c->display, c->comp_atom) != c->comp_owner_win)
  {
    fprintf(stderr, "nikom: Failed to claim %s.\n", atom);
    return false;
  }

  Atom manager_atom = XInternAtom(c->display, "MANAGER", False);

  XEvent ev = {0};
  ev.xclient.type = ClientMessage;
  ev.xclient.window = c->root;
  ev.xclient.message_type = manager_atom;
  ev.xclient.format = 32;
  ev.xclient.data.l[0] = CurrentTime;
  ev.xclient.data.l[1] = (long)c->comp_atom;
  ev.xclient.data.l[2] = (long)c->comp_owner_win;
  ev.xclient.data.l[3] = 0;
  ev.xclient.data.l[4] = 0;

  XSendEvent(c->display, c->root, False, StructureNotifyMask, &ev);

  c->overlay = XCompositeGetOverlayWindow(c->display, c->root);
  if(!c->overlay)
  {
    fprintf(stderr, "nikom: Failed to get XComposite overlay window.\n");
    return false;
  }

  XWindowAttributes root_attr;
  if(!XGetWindowAttributes(c->display, c->root, &root_attr))
  {
    fprintf(stderr, "nikom: Could not get root window attributes.\n");
    return false;
  }

  c->root_width = root_attr.width;
  c->root_height = root_attr.height;

  XSelectInput(c->display, c->root,
      SubstructureNotifyMask | StructureNotifyMask | PropertyChangeMask);

  if(c->have_xrandr)
  {
    XRRSelectInput(c->display, c->root,
        RRScreenChangeNotifyMask |
        RRCrtcChangeNotifyMask |
        RROutputChangeNotifyMask);
  }

  XserverRegion empty = XFixesCreateRegion(c->display, NULL, 0);

  XFixesSetWindowShapeRegion(c->display, c->overlay, ShapeInput,
      0, 0, empty);

  XFixesDestroyRegion(c->display, empty);

  XSelectInput(c->display, c->overlay,
      ExposureMask | StructureNotifyMask);

  XCompositeRedirectSubwindows(
      c->display,
      c->root,
      CompositeRedirectManual);

  return true;
}

static bool renderer_init(const Compositor* c, Renderer* r)
{
  XWindowAttributes overlay_attr;
  if(!XGetWindowAttributes(c->display, c->overlay, &overlay_attr))
  {
    fprintf(stderr,
        "nikom: Failed to get window attributes of overlay window.\n");
    return false;
  }

  XVisualInfo match_vis = {0};
  match_vis.visualid = XVisualIDFromVisual(overlay_attr.visual);
  match_vis.screen = c->screen;

  int n_matches = 0;
  XVisualInfo* vis = XGetVisualInfo(
      c->display,
      VisualIDMask | VisualScreenMask,
      &match_vis,
      &n_matches);

  if(!vis || n_matches < 1)
  {
    fprintf(stderr, "nikom: Overlay window has no X visual info.\n");
    if(vis)
    {
      XFree(vis);
    }
    return false;
  }

  r->context = glXCreateContext(c->display, vis, NULL, True);

  XFree(vis);

  if(!r->context)
  {
    fprintf(stderr, "nikom: Failed to create GLX context.\n");
    return false;
  }

  if(!glXMakeCurrent(c->display, c->overlay, r->context))
  {
    fprintf(stderr,
        "nikom: Failed to set GLX context to overlay window.\n");
    glXDestroyContext(c->display, r->context);
    *r = (Renderer){0};
    return false;
  }

  glDisable(GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);
  glEnable(GL_BLEND);
  glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

  return true;
}

static void client_drop_texture(Compositor* c, Client* cl)
{
  if(!cl || !c)
  {
    return;
  }

  if(cl->tex_bound && cl->glx_pixmap)
  {
    glXReleaseTexImageEXT_p(c->display, cl->glx_pixmap, GLX_FRONT_LEFT_EXT);
    cl->tex_bound = false;
  }

  if(cl->glx_pixmap)
  {
    glXDestroyPixmap(c->display, cl->glx_pixmap);
    cl->glx_pixmap = None;
  }

  if(cl->pixmap)
  {
    XFreePixmap(c->display, cl->pixmap);
    cl->pixmap = None;
  }

  if(cl->texid)
  {
    glDeleteTextures(1, &cl->texid);
    cl->texid = 0;
  }

  cl->tex_needs_bind = true;
}

static void update_client_geom(Compositor* c, Client* cl,
    const XWindowAttributes* attr)
{
  if(!cl || !c || !attr)
  {
    return;
  }

  const int old_w = cl->w;
  const int old_h = cl->h;
  const int old_depth = cl->depth;
  const VisualID old_visid = cl->visualid;

  cl->x = attr->x;
  cl->y = attr->y;
  cl->w = attr->width;
  cl->h = attr->height;
  cl->border_width = attr->border_width;
  cl->depth = attr->depth;
  cl->visualid = XVisualIDFromVisual(attr->visual);
  cl->mapped = attr->map_state == IsViewable;

  bool geom_changed = old_w != cl->w ||
    old_h != cl->h ||
    old_depth != cl->depth ||
    old_visid != cl->visualid;

  if(geom_changed)
  {
    client_drop_texture(c, cl);
    cl->tex_needs_bind = true;
  }
}

static Client* client_from_win(Compositor* c, Window win)
{
  for(Client* cl = c->clients; cl != NULL; cl = cl->next)
  {
    if(cl->win == win)
    {
      return cl;
    }
  }

  return NULL;
}

static void unmanage_client(Compositor* c, Window win)
{
  Client** ptr = &c->clients;
  while(*ptr)
  {
    Client* cl = *ptr;
    if(cl->win == win)
    {
      *ptr = cl->next;

      client_drop_texture(c, cl);

      if(cl->damage)
      {
        XDamageDestroy(c->display, cl->damage);
        cl->damage = None;
      }

      free(cl);
      return;
    }

    ptr = &(*ptr)->next;
  }
}

static Client* manage_client(Compositor* c, Window win)
{
  Client* existing_cl = client_from_win(c, win);

  XWindowAttributes attr;
  if(!XGetWindowAttributes(c->display, win, &attr))
  {
    unmanage_client(c, win);
    return NULL;
  }

  if(existing_cl)
  {
    update_client_geom(c, existing_cl, &attr);
    return existing_cl;
  }

  if(attr.class != InputOutput ||
      attr.map_state != IsViewable ||
      attr.width <= 0 ||
      attr.height <= 0)
  {
    return NULL;
  }

  Client* cl = calloc(1, sizeof(*cl));
  if(!cl)
  {
    fprintf(stderr, "nikom: Failed to allocate client.\n");
    return NULL;
  }

  cl->win = win;

  update_client_geom(c, cl, &attr);

  XSelectInput(c->display, cl->win, StructureNotifyMask | PropertyChangeMask);

  cl->damage = XDamageCreate(c->display, cl->win, XDamageReportNonEmpty);

  cl->next = c->clients;
  c->clients = cl;

  return cl;
}

static bool manage_existing_clients(Compositor* c)
{
  Window root_return = None;
  Window parent_return = None;
  Window* childs = NULL;
  unsigned int nchilds = 0;

  if(!XQueryTree(c->display, c->root,
        &root_return,
        &parent_return,
        &childs,
        &nchilds))
  {
    if(childs)
    {
      XFree(childs);
    }
    return false;
  }

  for(unsigned int i = 0; i < nchilds; i++)
  {
    Window w = childs[i];
    if(should_manage_window(c, w))
    {
      manage_client(c, w);
    }
  }

  if(childs)
  {
    XFree(childs);
  }

  return true;
}

static void handle_damage(Compositor* c, XEvent* ev)
{
  XDamageNotifyEvent* ev_dmg = (XDamageNotifyEvent*)ev;

  Client* cl = client_from_win(c, ev_dmg->drawable);
  if(!cl)
  {
    return;
  }

  cl->tex_needs_bind = true;
  c->needs_rerender = true;

  if(cl->damage)
  {
    XDamageSubtract(c->display, cl->damage, None, None);
  }
}

static void handle_configure(Compositor* c, Renderer* r,
    const XConfigureEvent* ev)
{
  if(ev->window == c->root || ev->window == c->overlay)
  {
    update_root_geom(c, r);
    return;
  }

  Client* cl = client_from_win(c, ev->window);
  if(!cl)
  {
    return;
  }

  const int old_w = cl->w;
  const int old_h = cl->h;

  cl->x = ev->x;
  cl->y = ev->y;
  cl->w = ev->width;
  cl->h = ev->height;
  cl->border_width = ev->border_width;

  if(old_w != cl->w || old_h != cl->h)
  {
    client_drop_texture(c, cl);
  }

  c->needs_rerender = true;
}

static void handle_event(Compositor* c, Renderer* r, XEvent* ev)
{
  if(ev->type == c->damage_event_base + XDamageNotify)
  {
    handle_damage(c, ev);
    return;
  }

  if(c->have_xrandr &&
      (ev->type == c->randr_event_base + RRScreenChangeNotify ||
       ev->type == c->randr_event_base + RRNotify))
  {
    XRRUpdateConfiguration(ev);
    update_root_geom(c, r);

    c->needs_rerender = true;
    return;
  }

  switch(ev->type)
  {
    case MapNotify:
      if(should_manage_window(c, ev->xmap.window))
      {
        manage_client(c, ev->xmap.window);
        c->needs_rerender = true;
      }
      break;

    case UnmapNotify:
      if(should_manage_window(c, ev->xunmap.window))
      {
        unmanage_client(c, ev->xunmap.window);
        c->needs_rerender = true;
      }
      break;

    case DestroyNotify:
      if(should_manage_window(c, ev->xdestroywindow.window))
      {
        unmanage_client(c, ev->xdestroywindow.window);
        c->needs_rerender = true;
      }
      break;

    case CreateNotify:
      c->needs_rerender = true;
      break;

    case ReparentNotify:
      if(should_manage_window(c, ev->xreparent.window))
      {
        if(ev->xreparent.parent == c->root)
        {
          manage_client(c, ev->xreparent.window);
        }
        else
        {
          unmanage_client(c, ev->xreparent.window);
        }
        c->needs_rerender = true;
      }
      break;

    case ConfigureNotify:
      handle_configure(c, r, &ev->xconfigure);
      break;

    case CirculateNotify:
    case GravityNotify:
    case Expose:
      c->needs_rerender = true;
      break;

    case SelectionClear:
      if(ev->xselectionclear.selection == c->comp_atom)
      {
        fprintf(stderr,
            "nikom: lost compositor manager ownership; exiting.\n");
        c->running = false;
      }
      break;

    default:
      break;
  }
}

static void renderer_resize(Compositor* c, Renderer* r)
{
  (void)r;
  glViewport(0, 0, c->root_width, c->root_height);
}

static void update_root_geom(Compositor* c, Renderer* r)
{
  XWindowAttributes root_attr;
  if(!XGetWindowAttributes(c->display, c->root, &root_attr))
  {
    return;
  }

  if(root_attr.width != c->root_width || root_attr.height != c->root_height)
  {
    c->root_width = root_attr.width;
    c->root_height = root_attr.height;

    if(c->overlay)
    {
      XMoveResizeWindow(c->display,
          c->overlay,
          0, 0,
          (unsigned int)c->root_width,
          (unsigned int)c->root_height);

      renderer_resize(c, r);
    }

    c->needs_rerender = true;
  }
}

static GLXFBConfig find_fbconfig_for_visual(Display* dpy, int screen,
    VisualID visualid,
    bool want_alpha)
{
  int ncfg = 0;
  GLXFBConfig* cfgs = glXGetFBConfigs(dpy, screen, &ncfg);
  if(!cfgs || ncfg <= 0)
  {
    return NULL;
  }

  GLXFBConfig fallback = NULL;

  for(int i = 0; i < ncfg; i++)
  {
    XVisualInfo* vi = glXGetVisualFromFBConfig(dpy, cfgs[i]);
    if(!vi)
    {
      continue;
    }

    const bool visual_matches = vi->visualid == visualid;
    XFree(vi);

    if(!visual_matches)
    {
      continue;
    }

    int drawable_type = 0;
    int texture_targets = 0;
    int bind_rgb = 0;
    int bind_rgba = 0;

    glXGetFBConfigAttrib(dpy, cfgs[i], GLX_DRAWABLE_TYPE, &drawable_type);
    glXGetFBConfigAttrib(dpy, cfgs[i], GLX_BIND_TO_TEXTURE_TARGETS_EXT,
        &texture_targets);
    glXGetFBConfigAttrib(dpy, cfgs[i], GLX_BIND_TO_TEXTURE_RGB_EXT,
        &bind_rgb);
    glXGetFBConfigAttrib(dpy, cfgs[i], GLX_BIND_TO_TEXTURE_RGBA_EXT,
        &bind_rgba);

    if(!(drawable_type & GLX_PIXMAP_BIT))
    {
      continue;
    }

    if(!(texture_targets & GLX_TEXTURE_2D_BIT_EXT))
    {
      continue;
    }

    if(want_alpha && bind_rgba)
    {
      GLXFBConfig ret = cfgs[i];
      XFree(cfgs);
      return ret;
    }

    if(!want_alpha && bind_rgb)
    {
      GLXFBConfig ret = cfgs[i];
      XFree(cfgs);
      return ret;
    }

    if(!fallback)
    {
      fallback = cfgs[i];
    }
  }

  XFree(cfgs);
  return fallback;
}

static bool ensure_client_texture(Compositor* c, Client* cl)
{
  if(!cl || !cl->mapped || cl->w <= 0 || cl->h <= 0)
  {
    return false;
  }

  if(!cl->pixmap)
  {
    cl->pixmap = XCompositeNameWindowPixmap(c->display, cl->win);
    if(!cl->pixmap)
    {
      client_drop_texture(c, cl);
      return false;
    }
  }

  if(!cl->texid)
  {
    glGenTextures(1, &cl->texid);
    glBindTexture(GL_TEXTURE_2D, cl->texid);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  }

  if(!cl->glx_pixmap)
  {
    GLXFBConfig fbconf = find_fbconfig_for_visual(
        c->display,
        c->screen,
        cl->visualid,
        cl->depth == 32);

    if(!fbconf)
    {
      fprintf(stderr,
          "nikom: warning: no FBConfig for window 0x%lx with visual 0x%lx found.\n",
          cl->win,
          (unsigned long)cl->visualid);
      client_drop_texture(c, cl);
      return false;
    }

    const int pixmap_attrs[] = {
      GLX_TEXTURE_TARGET_EXT, GLX_TEXTURE_2D_EXT,
      GLX_TEXTURE_FORMAT_EXT,
      cl->depth == 32 ? GLX_TEXTURE_FORMAT_RGBA_EXT
                      : GLX_TEXTURE_FORMAT_RGB_EXT,
      GLX_MIPMAP_TEXTURE_EXT, False,
      None
    };

    cl->glx_pixmap = glXCreatePixmap(c->display,
        fbconf,
        cl->pixmap,
        pixmap_attrs);

    if(!cl->glx_pixmap)
    {
      client_drop_texture(c, cl);
      return false;
    }

    cl->tex_needs_bind = true;
  }

  if(cl->tex_needs_bind || !cl->tex_bound)
  {
    glBindTexture(GL_TEXTURE_2D, cl->texid);

    if(cl->tex_bound)
    {
      glXReleaseTexImageEXT_p(c->display,
          cl->glx_pixmap,
          GLX_FRONT_LEFT_EXT);
      cl->tex_bound = false;
    }

    glXBindTexImageEXT_p(c->display,
        cl->glx_pixmap,
        GLX_FRONT_LEFT_EXT,
        NULL);

    cl->tex_bound = true;
    cl->tex_needs_bind = false;
  }

  return true;
}

static void render_scene(Compositor* c, Renderer* r)
{
  (void)r;

  glViewport(0, 0, c->root_width, c->root_height);

  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  glOrtho(0.0,
      (double)c->root_width,
      (double)c->root_height,
      0.0,
      -1.0,
      1.0);

  glMatrixMode(GL_MODELVIEW);
  glLoadIdentity();

  glClearColor(1.0f, 0.0f, 0.0f, 0.0f);
  glClear(GL_COLOR_BUFFER_BIT);

  Window root_return = None;
  Window parent_return = None;
  Window* childs = NULL;
  unsigned int nchilds = 0;

  if(XQueryTree(c->display,
        c->root,
        &root_return,
        &parent_return,
        &childs,
        &nchilds))
  {
    for(unsigned int i = 0; i < nchilds; i++)
    {
      const Window win = childs[i];
      if(!should_manage_window(c, win))
      {
        continue;
      }

      Client* cl = manage_client(c, win);
      if(!cl || !cl->mapped)
      {
        continue;
      }

      if(!ensure_client_texture(c, cl))
      {
        continue;
      }

      glEnable(GL_TEXTURE_2D);
      glBindTexture(GL_TEXTURE_2D, cl->texid);
      glColor4f(1.0f, 1.0f, 1.0f, 1.0f);

      glBegin(GL_QUADS);
      glTexCoord2f(0.0f, 0.0f);
      glVertex2d(cl->x, cl->y);
      glTexCoord2f(1.0f, 0.0f);
      glVertex2d(cl->x + cl->w, cl->y);
      glTexCoord2f(1.0f, 1.0f);
      glVertex2d(cl->x + cl->w, cl->y + cl->h);
      glTexCoord2f(0.0f, 1.0f);
      glVertex2d(cl->x, cl->y + cl->h);
      glEnd();
    }
  }

  if(childs)
  {
    XFree(childs);
  }

  glXSwapBuffers(c->display, c->overlay);
  XFlush(c->display);
}

static void compositor_run(Compositor* c, Renderer* r)
{
  while(c->running)
  {
    if(!c->needs_rerender)
    {
      XEvent ev;
      XNextEvent(c->display, &ev);
      handle_event(c, r, &ev);
    }

    while(XPending(c->display) > 0)
    {
      XEvent ev;
      XNextEvent(c->display, &ev);
      handle_event(c, r, &ev);
    }

    if(c->needs_rerender)
    {
      update_root_geom(c, r);
      render_scene(c, r);
      c->needs_rerender = false;
    }
  }
}

static void compositor_shutdown(Compositor* c, Renderer* r)
{
  if(!c || !c->display)
  {
    return;
  }

  Display* display = c->display;

  while(c->clients)
  {
    unmanage_client(c, c->clients->win);
  }

  if(r->context)
  {
    glXMakeCurrent(c->display, None, NULL);
    glXDestroyContext(c->display, r->context);
  }

  if(c->root)
  {
    XCompositeUnredirectSubwindows(c->display, c->root, CompositeRedirectManual);
  }

  if(c->overlay)
  {
    XCompositeReleaseOverlayWindow(c->display, c->root);
  }

  if(c->comp_owner_win)
  {
    if(XGetSelectionOwner(c->display, c->comp_atom) == c->comp_owner_win)
    {
      XSetSelectionOwner(c->display, c->comp_atom, None, CurrentTime);
    }
    XDestroyWindow(c->display, c->comp_owner_win);
    c->comp_owner_win = None;
  }

  XFlush(display);

  *c = (Compositor){0};
  *r = (Renderer){0};
}

int main(void)
{
  Display* display = XOpenDisplay(NULL);

  if(!display)
  {
    fprintf(stderr, "nikom: Failed to open display.\n");
    return EXIT_FAILURE;
  }

  XSetErrorHandler(x_error_handler);

  Compositor c = {0};
  Renderer r = {0};

  if(!compositor_init(&c, display, DefaultScreen(display)))
  {
    fprintf(stderr, "nikom: Failed to initialize compositor.\n");
    XCloseDisplay(display);
    return EXIT_FAILURE;
  }

  if(!renderer_init(&c, &r))
  {
    fprintf(stderr, "nikom: Failed to initialize GLX/OpenGL renderer.\n");
    compositor_shutdown(&c, &r);
    XCloseDisplay(display);
    return EXIT_FAILURE;
  }

  if(!manage_existing_clients(&c))
  {
    fprintf(stderr,
        "nikom: Failed to manage one of the existing client windows.\n");
    compositor_shutdown(&c, &r);
    XCloseDisplay(display);
    return EXIT_FAILURE;
  }

  printf("nikom: Initialized compositor on X display %s\n", XDisplayName(NULL));

  compositor_run(&c, &r);

  compositor_shutdown(&c, &r);
  XCloseDisplay(display);

  return EXIT_SUCCESS;
}

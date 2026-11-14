// questlhsync-dashboard: QuestLHSync's status page on Linux (the Windows dashboard overlay's page, in a desktop
// window; WayVR or any desktop overlay shows it in the headset). Started by the questlhsync app; reads its status from
// the shared memory (qlhs_status.h), draws it with cairo and pango, and sends the buttons back as commands.
//   questlhsync-dashboard --preview out.png [locked|acquiring|still|frozen|searching|nohmd|noshim|paused|stale]
#include <fcntl.h>
#include <gtk/gtk.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <time.h>
#include <vector>

#include "../../common/qlhs_status.h"

namespace {

const int W = 1280, H = 800;  // page size

namespace col {
const uint32_t bg = 0x14161b, card = 0x1d2027, card2 = 0x252932, line = 0x30343e, text = 0xeceef2, dim = 0x9aa1ad,
               faint = 0x6b7280, green = 0x3ccf6e, amber = 0xffb224, red = 0xff5f57, blue = 0x5b8cff, grey = 0x8e93a0;
}

struct Font {
  int px;
  bool bold, mono;
};

struct Painter {
  cairo_t *cr;
  void Color(uint32_t c) { cairo_set_source_rgb(cr, ((c >> 16) & 255) / 255.0, ((c >> 8) & 255) / 255.0, (c & 255) / 255.0); }
  void Path(double x, double y, double w, double h, double r) {
    if (r <= 0) { cairo_rectangle(cr, x, y, w, h); return; }
    r = std::min(r, std::min(w, h) / 2);
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
  }
  void Rect(double x, double y, double w, double h, uint32_t color, double r = 0) {
    Path(x, y, w, h, r);
    Color(color);
    cairo_fill(cr);
  }
  void Dot(double cx, double cy, double r, uint32_t color) {
    cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
    Color(color);
    cairo_fill(cr);
  }
  PangoLayout *Layout(const std::string &s, const Font &f) {
    PangoLayout *l = pango_cairo_create_layout(cr);
    PangoFontDescription *d = pango_font_description_new();
    pango_font_description_set_family(d, f.mono ? "Monospace" : "Sans");
    pango_font_description_set_weight(d, f.bold ? PANGO_WEIGHT_SEMIBOLD : PANGO_WEIGHT_NORMAL);
    pango_font_description_set_absolute_size(d, f.px * PANGO_SCALE);
    pango_layout_set_font_description(l, d);
    pango_font_description_free(d);
    pango_layout_set_text(l, s.c_str(), -1);
    return l;
  }
  int Width(const std::string &s, const Font &f) {
    PangoLayout *l = Layout(s, f);
    int w, h;
    pango_layout_get_pixel_size(l, &w, &h);
    g_object_unref(l);
    return w;
  }
  // text; align 0 left, 1 center, 2 right (x is the anchor), cut to maxw with an ellipsis. returns the width
  int Text(double x, double y, const std::string &s, const Font &f, uint32_t color, int align = 0, int maxw = 0) {
    PangoLayout *l = Layout(s, f);
    if (maxw > 0) {
      pango_layout_set_width(l, maxw * PANGO_SCALE);
      pango_layout_set_ellipsize(l, PANGO_ELLIPSIZE_END);
    }
    int w, h;
    pango_layout_get_pixel_size(l, &w, &h);
    cairo_move_to(cr, x - (align == 1 ? w / 2 : align == 2 ? w : 0), y);
    Color(color);
    pango_cairo_show_layout(cr, l);
    g_object_unref(l);
    return w;
  }
};

std::string F(const char *fmt, ...) {
  char b[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  return b;
}

std::string Ago(double s) {
  if (s < 0) return "not yet";
  if (s < 1.5) return "now";
  if (s < 90) return F("%.0f s ago", s);
  if (s < 5400) return F("%.0f min ago", s / 60);
  return F("%.1f h ago", s / 3600);
}

std::string Dur(double s) {
  if (s < 90) return F("%.0f s", s);
  if (s < 5400) return F("%.0f min", s / 60);
  return F("%.1f h", s / 3600);
}

struct Button { int x, y, w, h, cmd; std::string label; bool primary; };

struct Page {
  std::vector<Button> buttons;
  int hover = -1;
};

void StateText(const QlhsStatus &s, bool stale, std::string &title, std::string &sub, uint32_t &color) {
  if (stale) { title = "QuestLHSync not running"; sub = "The questlhsync app isn't updating. Start it again (systemctl --user start questlhsync)."; color = col::red; return; }
  switch (s.state) {
    case QLHS_NO_RUNTIME:
      title = "Waiting for WiVRn";
      sub = "WiVRn (or Monado) isn't running yet. Start it and connect your headset.";
      color = col::grey;
      break;
    case QLHS_NO_HMD:
      title = "Waiting for a Quest or Steam Frame";
      sub = s.hmd[0] ? "The runtime's headset isn't a Quest Pro, 3, 3S or Steam Frame, so the lighthouse space is left alone."
                     : "Connect your headset to WiVRn.";
      color = col::grey;
      break;
    case QLHS_NO_SHIM:
      title = "Lighthouse shim not loaded";
      sub = "WiVRn runs without QuestLHSync's lighthouse driver: start it with STEAMVR_PATH set (see the README).";
      color = col::red;
      break;
    case QLHS_SEARCHING:
      title = "Looking for the headset";
      sub = "No headset with QuestLHSync's lhsyncd answers on the network. Is it awake and on the same Wi-Fi?";
      color = col::amber;
      break;
    case QLHS_CONNECTING: title = "Connecting"; sub = "Found the headset, opening the camera stream."; color = col::amber; break;
    case QLHS_NO_CAMERAS:
      title = "No camera frames";
      sub = "Connected, but the tracking cameras send nothing. Put the headset on (or wake it) and check lhsyncd.";
      color = col::red;
      break;
    case QLHS_NO_STATIONS:
      title = "Waiting for base stations";
      sub = "SteamVR's lighthouse driver shows base stations only while a lighthouse device (tracker, controller) is on.";
      color = col::amber;
      break;
    case QLHS_ACQUIRING:
      title = "Finding the base stations";
      sub = s.head_still == 2 ? "The runtime isn't getting your head's motion. Is the headset tracking in WiVRn?"
            : s.head_still   ? "The headset isn't moving, so its camera frames wait. Put it on and look around the room."
                             : "Look around the room so the cameras catch both base stations' flashes.";
      color = col::blue;
      break;
    case QLHS_LOCKED:
      title = s.paused ? "Locked, corrections paused" : "Locked";
      sub = s.paused ? "Lighthouse devices stay where they are until you resume."
                     : "Lighthouse devices follow the headset's own tracking.";
      color = s.paused ? col::amber : col::green;
      break;
    case QLHS_DISABLED:
      title = "Off";
      sub = "enable is false in ~/.config/questlhsync/settings.json.";
      color = col::grey;
      break;
    default: title = "Starting"; sub = ""; color = col::grey;
  }
}

void Draw(cairo_t *cr, Page &pg, const QlhsStatus &s, bool stale) {
  Painter p{cr};
  Font h1{40, true, false}, h2{22, true, false}, body{21, false, false}, small{17, false, false},
      label{16, true, false}, big{28, true, false}, mono{16, false, true}, pill{22, true, false};
  p.Rect(0, 0, W, H, col::bg);

  // header
  int tw = p.Text(48, 30, "QuestLHSync", h1, col::text);
  p.Text(48 + tw + 14, 46, "by CreoleVR", h2, col::faint);
  p.Text(50, 88, "Lighthouse tracking aligned to your headset, from its own cameras", small, col::dim);
  std::string title, sub;
  uint32_t sc;
  StateText(s, stale, title, sub, sc);
  {
    int pw = p.Width(title, pill) + 64, px = W - 48 - pw;
    p.Rect(px, 40, pw, 48, col::card2, 24);
    p.Dot(px + 26, 64, 7, sc);
    p.Text(px + 44, 50, title, pill, col::text);
  }

  // status line
  p.Rect(48, 124, W - 96, 52, col::card, 12);
  p.Rect(48, 124, 6, 52, sc, 3);
  p.Text(70, 137, sub, body, col::text, 0, W - 140);

  // cards
  int top = 196, ch = 300, gap = 20, cw = (W - 96 - 2 * gap) / 3;
  int x1 = 48, x2 = x1 + cw + gap, x3 = x2 + cw + gap;
  for (int x : {x1, x2, x3}) p.Rect(x, top, cw, ch, col::card, 14);

  auto row = [&](int x, int y, const char *k, const std::string &v, uint32_t vc = col::text) {
    p.Text(x + 22, y, k, small, col::dim);
    p.Text(x + cw - 22, y, v, small, vc, 2, cw - 150);
  };

  // headset card
  p.Text(x1 + 22, top + 18, "HEADSET", label, col::faint);
  p.Text(x1 + 22, top + 44, s.headset[0] ? s.headset : "—", big, col::text, 0, cw - 44);
  row(x1, top + 100, "Address", s.headset_addr[0] ? s.headset_addr : "—");
  row(x1, top + 134, "Cameras", s.cam_fps > 0.5 ? F("%.0f frames/s", s.cam_fps) : "—",
      s.cam_fps > 0.5 || s.state != QLHS_NO_CAMERAS ? col::text : col::red);
  row(x1, top + 168, "Round trip", s.rtt_ms > 0 ? F("%.1f ms", s.rtt_ms) : "—");
  row(x1, top + 202, "Streamer", s.hmd_system[0] ? s.hmd_system : "—");
  row(x1, top + 236, "Pose timing",
      s.headset_addr[0] ? F("%.1f ms %s", s.expo_ms, s.expo_learned ? "(learned)" : "(learning)") : "—",
      s.expo_learned || !s.headset_addr[0] ? col::text : col::amber);
  row(x1, top + 270, "Bright spots", s.cam_fps > 0.5 ? F("%.0f/s, %.0f/s used", s.spot_rate, s.sight_rate) : "—",
      s.state == QLHS_ACQUIRING && s.sight_rate < 0.5 ? col::amber : col::text);

  // base stations card
  bool has = s.locked || s.nfit > 0 || s.yaw_deg != 0;  // an alignment: sightings belong to a station
  p.Text(x2 + 22, top + 18, "BASE STATIONS", label, col::faint);
  if (s.nst == 0) {
    p.Text(x2 + 22, top + 52, "None from the lighthouse driver yet", body, col::dim, 0, cw - 44);
    p.Text(x2 + 22, top + 84, "They show up while a tracker or", small, col::faint);
    p.Text(x2 + 22, top + 108, "controller is switched on.", small, col::faint);
  }
  for (int i = 0; i < std::min(s.nst, 4); i++) {
    const QlhsStation &st = s.st[i];
    int y = top + 50 + i * 60;
    bool fresh = st.last_seen >= 0 && st.last_seen < 15;
    p.Dot(x2 + 30, y + 15, 6, fresh ? col::green : st.last_seen >= 0 ? col::amber : col::faint);
    p.Text(x2 + 46, y, st.serial, body, col::text, 0, cw - 150);
    std::string role = st.anchor ? "anchor" : st.measured ? "measured" : "";
    if (!role.empty()) p.Text(x2 + cw - 22, y + 3, role, small, col::blue, 2);
    std::string info = "seen " + Ago(st.last_seen);
    if (has) info += F("  ·  %d sightings", st.support);
    if (st.dist > 0) info += F("  ·  %.1f m", st.dist);
    p.Text(x2 + 46, y + 28, info, small, col::dim, 0, cw - 68);
  }

  // alignment card
  p.Text(x3 + 22, top + 18, "ALIGNMENT", label, col::faint);
  p.Text(x3 + 22, top + 44, has ? F("%.2f°", s.med_deg >= 0 ? s.med_deg : 0.0) : "—", big,
         s.med_deg >= 0 && s.med_deg < 0.3 ? col::green : col::text);
  if (has) p.Text(x3 + 22 + 110, top + 56, "median sighting error", small, col::dim);
  row(x3, top + 100, "Yaw", has ? F("%+.2f°", s.yaw_deg) : "—");
  row(x3, top + 134, "Offset", has ? F("%.2f, %.2f, %.2f m", s.t[0], s.t[1], s.t[2]) : "—");
  row(x3, top + 168, "Sightings used", has ? F("%d", s.nfit) : "—");
  row(x3, top + 202, "Since acquired", s.locked_for >= 0 ? Dur(s.locked_for) : "—");
  row(x3, top + 236, "Settling", s.lag_cm > 0.05 ? F("%.1f cm to go", s.lag_cm) : has ? "done" : "—");

  // log
  int ly = top + ch + 20, lh = 150;
  p.Rect(48, ly, W - 96, lh, col::card, 14);
  p.Text(70, ly + 16, "LOG", label, col::faint);
  uint32_t n = s.nlog;
  int shown = 0;
  for (uint32_t i = 0; i < 5 && i < n; i++) {
    uint32_t k = n - 1 - i;
    char line[sizeof s.log[0] + 1];
    memcpy(line, s.log[k % 8], sizeof s.log[0]);
    line[sizeof s.log[0]] = 0;
    p.Text(70, ly + 44 + 20 * i, line, mono, i == 0 ? col::text : col::dim, 0, W - 150);
    shown++;
  }
  if (!shown) p.Text(70, ly + 44, "—", mono, col::dim);

  // buttons
  pg.buttons.clear();
  int by = H - 66, bh = 46;
  pg.buttons.push_back({48, by, 240, bh, s.paused ? QLHS_CMD_RESUME : QLHS_CMD_PAUSE,
                        s.paused ? "Resume corrections" : "Pause corrections", s.paused != 0});
  pg.buttons.push_back({304, by, 210, bh, s.recording ? QLHS_CMD_RECORD_OFF : QLHS_CMD_RECORD_ON,
                        s.recording ? "Stop recording" : "Record session", false});
  pg.buttons.push_back({530, by, 240, bh, QLHS_CMD_RECALIBRATE, "Force recalibrate", false});
  for (size_t i = 0; i < pg.buttons.size(); i++) {
    auto &b = pg.buttons[i];
    bool hov = (int)i == pg.hover;
    uint32_t fill = b.primary ? col::blue : hov ? col::line : col::card2;
    p.Rect(b.x, b.y, b.w, b.h, fill, 10);
    if (b.cmd == QLHS_CMD_RECORD_OFF) p.Dot(b.x + 26, b.y + bh / 2, 6, col::red);
    p.Text(b.x + b.w / 2 + (b.cmd == QLHS_CMD_RECORD_OFF ? 10 : 0), b.y + 10, b.label, h2, col::text, 1);
  }
  p.Text(W - 48, by + 14, "Settings: ~/.config/questlhsync/settings.json", small, col::faint, 2);
}

QlhsStatus Sample(const char *kind) {
  QlhsStatus s{};
  s.magic = QLHS_MAGIC;
  s.version = QLHS_VERSION;
  strcpy(s.hmd, "WiVRn HMD");
  strcpy(s.hmd_system, "WiVRn");
  strcpy(s.headset, "Quest Pro");
  strcpy(s.headset_addr, "192.168.1.50:47280");
  s.cam_fps = 75;
  s.spot_rate = 41;
  s.sight_rate = 38;
  s.rtt_ms = 4.2;
  s.expo_ms = 15.3;
  s.expo_learned = 1;
  s.nst = 2;
  strcpy(s.st[0].serial, "LHB-1A2B3C4D");
  s.st[0].anchor = 1; s.st[0].support = 432; s.st[0].last_seen = 0.4; s.st[0].dist = 3.4;
  strcpy(s.st[1].serial, "LHB-5E6F7A8B");
  s.st[1].measured = 1; s.st[1].support = 337; s.st[1].last_seen = 6.2; s.st[1].dist = 2.1;
  s.state = QLHS_LOCKED;
  s.locked = 1; s.cond = 1;
  s.yaw_deg = -5.76; s.t[0] = -1.071; s.t[1] = 2.124; s.t[2] = 2.432;
  s.med_deg = 0.14; s.nfit = 788; s.locked_for = 754; s.lag_cm = 0.3;
  const char *logs[] = {"04:53:10  seeded from saved stations: yaw -5.765 t [-1.0713, 2.1258, 2.4309] miss 0.0 cm",
                        "04:53:12  Quest Pro at 192.168.1.50: connected",
                        "04:53:12  camera calibration: online",
                        "04:53:41  timing for wivrn: 15.3 ms (best fit 15.3 ms on 212 fast-head sightings)",
                        "04:55:02  lighthouse frame: SteamVR's is 0.75 deg from the reference"};
  for (auto l : logs) { strcpy(s.log[s.nlog % 8], l); s.nlog++; }
  if (!strcmp(kind, "still")) { s.head_still = 1; s.sight_rate = 0; s.spot_rate = 12; kind = "acquiring"; }
  if (!strcmp(kind, "frozen")) { s.head_still = 2; s.sight_rate = 0; s.spot_rate = 12; kind = "acquiring"; }
  if (!strcmp(kind, "acquiring")) { s.state = QLHS_ACQUIRING; s.locked = 0; s.med_deg = -1; s.nfit = 0; s.yaw_deg = 0; s.locked_for = -1; s.st[1].last_seen = -1; s.st[1].support = 0; s.expo_learned = 0; s.expo_ms = 15; }
  if (!strcmp(kind, "searching")) { s.state = QLHS_SEARCHING; s.headset_addr[0] = 0; s.cam_fps = 0; s.rtt_ms = 0; s.locked = 0; s.nfit = 0; s.med_deg = -1; s.yaw_deg = 0; s.locked_for = -1; }
  if (!strcmp(kind, "nohmd")) { s.state = QLHS_NO_HMD; strcpy(s.hmd, "Rift S"); }
  if (!strcmp(kind, "noshim")) s.state = QLHS_NO_SHIM;
  if (!strcmp(kind, "paused")) s.paused = 1;
  return s;
}

// ---------------------------------------------------------------- status
QlhsStatus *g_shm;

bool ReadStatus(QlhsStatus &out) {
  if (!g_shm) {
    std::string name = QLHS_SHM_PREFIX + std::to_string(getuid());
    int fd = shm_open(name.c_str(), O_RDWR, 0600);
    if (fd < 0) return false;
    struct stat st {};
    if (fstat(fd, &st) != 0 || (size_t)st.st_size < sizeof(QlhsStatus)) { close(fd); return false; }
    void *m = mmap(nullptr, sizeof(QlhsStatus), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (m == MAP_FAILED) return false;
    g_shm = (QlhsStatus *)m;
  }
  for (int i = 0; i < 100; i++) {
    int32_t s = __atomic_load_n(&g_shm->seq, __ATOMIC_ACQUIRE);
    if (s & 1) { sched_yield(); continue; }
    memcpy(&out, (const void *)g_shm, sizeof out);
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (__atomic_load_n(&g_shm->seq, __ATOMIC_ACQUIRE) == s) return out.magic == QLHS_MAGIC && out.version == QLHS_VERSION;
  }
  return false;
}

void SendCmd(int cmd) {
  if (!g_shm) return;
  g_shm->cmd = cmd;
  __atomic_fetch_add(&g_shm->cmd_seq, 1, __ATOMIC_SEQ_CST);
}

// ---------------------------------------------------------------- window: the page, scaled to fit
struct Ui {
  GtkWidget *win = nullptr, *area = nullptr;
  Page pg;
  QlhsStatus st{}, prev{};
  bool have = false, stale = false;
  double last_upd = 0;
  gint64 last_change = 0;
  int down = -1;
  double scale = 1, ox = 0, oy = 0;
};
Ui g_ui;

int HitAt(double wx, double wy) {
  double mx = (wx - g_ui.ox) / g_ui.scale, my = (wy - g_ui.oy) / g_ui.scale;
  for (size_t i = 0; i < g_ui.pg.buttons.size(); i++) {
    auto &b = g_ui.pg.buttons[i];
    if (mx >= b.x && mx < b.x + b.w && my >= b.y && my < b.y + b.h) return (int)i;
  }
  return -1;
}

gboolean OnDraw(GtkWidget *w, cairo_t *cr, gpointer) {
  int aw = gtk_widget_get_allocated_width(w), ah = gtk_widget_get_allocated_height(w);
  g_ui.scale = std::min(aw / (double)W, ah / (double)H);
  g_ui.ox = (aw - W * g_ui.scale) / 2;
  g_ui.oy = (ah - H * g_ui.scale) / 2;
  Painter{cr}.Rect(0, 0, aw, ah, col::bg);
  cairo_translate(cr, g_ui.ox, g_ui.oy);
  cairo_scale(cr, g_ui.scale, g_ui.scale);
  Draw(cr, g_ui.pg, g_ui.st, g_ui.stale);
  return TRUE;
}

gboolean OnMotion(GtkWidget *w, GdkEventMotion *e, gpointer) {
  int h = HitAt(e->x, e->y);
  if (h != g_ui.pg.hover) { g_ui.pg.hover = h; gtk_widget_queue_draw(w); }
  return TRUE;
}

gboolean OnPress(GtkWidget *, GdkEventButton *e, gpointer) {
  if (e->button == 1) g_ui.down = HitAt(e->x, e->y);
  return TRUE;
}

gboolean OnRelease(GtkWidget *w, GdkEventButton *e, gpointer) {
  if (e->button != 1) return TRUE;
  int h = HitAt(e->x, e->y);
  if (h >= 0 && h == g_ui.down) { SendCmd(g_ui.pg.buttons[h].cmd); gtk_widget_queue_draw(w); }
  g_ui.down = -1;
  return TRUE;
}

gboolean OnTick(gpointer) {
  g_ui.have = ReadStatus(g_ui.st);
  if (!g_ui.have) memset(&g_ui.st, 0, sizeof g_ui.st);
  if (g_ui.have && g_ui.st.state == QLHS_WIRED) {  // a wired headset: nothing to show
    gtk_main_quit();
    return FALSE;
  }
  gint64 now = g_get_monotonic_time();
  if (g_ui.st.updated != g_ui.last_upd) { g_ui.last_upd = g_ui.st.updated; g_ui.last_change = now; }
  bool stale = !g_ui.have || now - g_ui.last_change > 3000000;
  if (stale != g_ui.stale || memcmp(&g_ui.st, &g_ui.prev, sizeof g_ui.st)) {
    g_ui.prev = g_ui.st;
    g_ui.stale = stale;
    gtk_widget_queue_draw(g_ui.area);
  }
  return TRUE;
}

// the dashboard's icon: a base station's laser fan reaching a headset
GdkPixbuf *Icon(int N) {
  cairo_surface_t *sf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, N, N);
  cairo_t *cr = cairo_create(sf);
  Painter p{cr};
  auto S = [&](double v) { return v * N / 256; };
  p.Rect(S(16), S(16), S(224), S(224), col::card2, S(48));
  p.Rect(S(54), S(66), S(70), S(70), col::text, S(18));  // base station
  p.Dot(S(89), S(101), S(20), col::bg);
  p.Dot(S(89), S(101), S(10), col::blue);
  p.Color(col::blue);
  cairo_set_line_width(cr, S(8));
  for (int i = 0; i < 3; i++) {  // laser lines
    cairo_move_to(cr, S(118), S(112 + i * 14));
    cairo_line_to(cr, S(170), S(140 + i * 18));
  }
  cairo_stroke(cr);
  p.Rect(S(140), S(150), S(86), S(52), col::green, S(20));  // headset visor
  p.Dot(S(166), S(176), S(9), col::bg);
  p.Dot(S(200), S(176), S(9), col::bg);
  cairo_destroy(cr);
  GdkPixbuf *pb = gdk_pixbuf_get_from_surface(sf, 0, 0, N, N);
  cairo_surface_destroy(sf);
  return pb;
}

}  // namespace

int main(int argc, char **argv) {
  if (argc >= 3 && !strcmp(argv[1], "--preview")) {
    const char *kind = argc >= 4 ? argv[3] : "locked";
    cairo_surface_t *sf = cairo_image_surface_create(CAIRO_FORMAT_RGB24, W, H);
    cairo_t *cr = cairo_create(sf);
    Page pg;
    Draw(cr, pg, Sample(kind), !strcmp(kind, "stale"));
    cairo_destroy(cr);
    cairo_status_t r = cairo_surface_write_to_png(sf, argv[2]);
    cairo_surface_destroy(sf);
    return r == CAIRO_STATUS_SUCCESS ? 0 : 1;
  }
  const char *rt = getenv("XDG_RUNTIME_DIR");
  std::string lock = std::string(rt && *rt ? rt : "/tmp") + "/" QLHS_DASHBOARD_LOCK;
  int lf = open(lock.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  if (lf < 0 || flock(lf, LOCK_EX | LOCK_NB) != 0) return 0;  // one is open already
  gtk_init(&argc, &argv);
  g_set_prgname("questlhsync-dashboard");
  g_ui.win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_window_set_title(GTK_WINDOW(g_ui.win), "QuestLHSync by CreoleVR");
  gtk_window_set_default_size(GTK_WINDOW(g_ui.win), W * 3 / 4, H * 3 / 4);
  if (GdkPixbuf *ic = Icon(256)) { gtk_window_set_icon(GTK_WINDOW(g_ui.win), ic); g_object_unref(ic); }
  g_ui.area = gtk_drawing_area_new();
  gtk_widget_add_events(g_ui.area, GDK_POINTER_MOTION_MASK | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK);
  g_signal_connect(g_ui.area, "draw", G_CALLBACK(OnDraw), nullptr);
  g_signal_connect(g_ui.area, "motion-notify-event", G_CALLBACK(OnMotion), nullptr);
  g_signal_connect(g_ui.area, "button-press-event", G_CALLBACK(OnPress), nullptr);
  g_signal_connect(g_ui.area, "button-release-event", G_CALLBACK(OnRelease), nullptr);
  g_signal_connect(g_ui.win, "destroy", G_CALLBACK(gtk_main_quit), nullptr);
  gtk_container_add(GTK_CONTAINER(g_ui.win), g_ui.area);
  g_ui.last_change = g_get_monotonic_time();
  OnTick(nullptr);
  gtk_widget_show_all(g_ui.win);
  g_timeout_add(200, OnTick, nullptr);
  gtk_main();
  return 0;
}

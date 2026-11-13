#define _GNU_SOURCE
#include <ctype.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define _STRINGIFY(s)   #s
#define STRINGIFY(s)    _STRINGIFY(s)

#define VERSION         "1.1.0"
#define GPXHEADER_FULL  "<?xml version=\"1.0\" encoding=\"UTF-8\"?><gpx version=\"1.1\" creator=\"gpxding "VERSION"\" xmlns=\"http://www.topografix.com/GPX/1/1\" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" xsi:schemaLocation=\"http://www.topografix.com/GPX/1/1 http://www.topografix.com/GPX/1/1/gpx.xsd\"><trk><trkseg>"
#define GPXHEADER_MIN   "<gpx><trk><trkseg>"
#define GPXFOOTER       "</trkseg></trk></gpx>"
#define DIGITS          5
#define EPSILON         2.0
#define NEARBY          0.0
#define SPIKE           0.0

#define PI              3.14159265358979323846
#define MPERDEG         (6371008.8 * PI / 180)      // meters per degree latitude

// ------------------------------------------------------------ math (no libm)

static double absd(double x) { return x < 0 ? -x : x; }

// Square root by Newton iteration from a bit-level first guess (x >= 0)
static double root(double x) {
    if (!(x > 0)) return 0;
    unsigned long long bits;
    memcpy(&bits, &x, sizeof bits);
    bits = (bits >> 1) + 0x1FF8000000000000ULL;
    double r;
    memcpy(&r, &bits, sizeof r);
    for (int i = 0; i < 5; i++) r = 0.5 * (r + x / r);
    return r;
}

static double hyp(double x, double y) { return root(x * x + y * y); }

// cos of a latitude in degrees (|lat| <= 90): Taylor series, error below 1e-12
static double cos_deg(double lat) {
    double x = lat * (PI / 180), x2 = x * x;
    return 1 - x2 / 2 * (1 - x2 / 12 * (1 - x2 / 30 * (1 - x2 / 56 * (1 - x2 / 90 *
           (1 - x2 / 132 * (1 - x2 / 182 * (1 - x2 / 240)))))));
}

typedef struct {
    double lat, lon;
    long   ele;                                 // whole meters, truncated
    bool   has_ele;
} Point;

typedef struct {
    Point  *pts;
    size_t n, cap;
} Track;

static void die(const char *msg, const char *name) {
    fprintf(stderr, "Error: %s%s%s\n", msg, name ? ": " : "", name ? name : "");
    exit(1);
}

static void help(void) {
    puts("gpxding version v"VERSION"\n\
Usage: gpxding [OPTIONS] [FILE ...]\n\
  -d    number of digits (default "STRINGIFY(DIGITS)")\n\
  -e    omit elevation info\n\
  -h    show this help\n\
  -m    use minimal <gpx> (not compatible with all apps/devices)\n\
  -n    remove nearby points (default 0 m, disabled)\n\
  -p    precision in meters (default "STRINGIFY(EPSILON)" m)\n\
  -q    quiet\n\
  -s    remove spikes in meters (default 0 m, disabled)\n\
  -t    split gpx file into individual tracks\n");
}

static char *read_file(const char *name, size_t *len) {
    FILE *fp = fopen(name, "rb");
    if (!fp) die("could not open", name);
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    rewind(fp);
    char *buf = size >= 0 ? malloc((size_t)size + 1) : NULL;
    if (!buf || fread(buf, 1, (size_t)size, fp) != (size_t)size) die("could not read", name);
    fclose(fp);
    buf[size] = '\0';
    if (len) *len = (size_t)size;
    return buf;
}

// ---------------------------------------------------------------- parsing

// If q (just after '<') starts a comment, CDATA, PI or DOCTYPE, return the
// position after its end, else NULL
static const char *skip_special(const char *q) {
    const char *end = NULL, *term = NULL;
    if (!strncmp(q, "!--", 3))          term = "-->";
    else if (!strncmp(q, "![CDATA[", 8)) term = "]]>";
    else if (*q == '?' || *q == '!')    term = ">";
    else return NULL;
    end = strstr(q, term);
    return end ? end + strlen(term) : q + strlen(q);
}

// End of an open tag starting at q, ignoring '>' inside quoted values
static const char *tag_end(const char *q) {
    char quote = 0;
    for (; *q; q++) {
        if (quote) { if (*q == quote) quote = 0; }
        else if (*q == '"' || *q == '\'') quote = *q;
        else if (*q == '>') return q;
    }
    return NULL;
}

static bool is_name_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == ':' || c == '_' || c == '-' || c == '.';
}

// Numeric attribute (any order, spacing and quote style) of the tag [a, end)
static bool get_attr(const char *a, const char *end, const char *name, double *out) {
    size_t len = strlen(name);
    for (; a < end; a++) {
        if (!isspace((unsigned char)a[-1]) || strncmp(a, name, len)) continue;
        const char *v = a + len;
        while (isspace((unsigned char)*v)) v++;
        if (*v++ != '=') continue;
        while (isspace((unsigned char)*v)) v++;
        if (*v != '"' && *v != '\'') continue;
        char *e;
        *out = strtod(v + 1, &e);
        return e != v + 1 && *e == *v;
    }
    return false;
}

typedef void (*Emit)(void *ctx, Point p);

static void add_point(void *ctx, Point p) {
    Track *t = ctx;
    if (t->n == t->cap) {
        t->cap = t->cap ? t->cap * 2 : 1024;
        t->pts = realloc(t->pts, t->cap * sizeof(Point));
        if (!t->pts) die("out of memory", NULL);
    }
    t->pts[t->n++] = p;
}

// Pass all trkpt and rtept elements (any namespace prefix) to emit
static void parse_points(const char *xml, Emit emit, void *ctx, bool elevation, const char *name) {
    int skipped = 0;
    for (const char *p = xml; (p = strchr(p, '<')); ) {
        const char *q = p + 1;
        if (*q == '/') { p = q; continue; }
        if (*q == '!' || *q == '?') {
            const char *after = skip_special(q);
            if (after) { p = after; continue; }
        }
        const char *n = q;
        while (is_name_char(*q)) q++;
        const char *colon = memchr(n, ':', q - n), *local = colon ? colon + 1 : n;
        size_t len = q - local;
        p = q;
        if (len != 5 || (memcmp(local, "trkpt", 5) && memcmp(local, "rtept", 5))) continue;
        // '<' cannot occur inside attribute values, so only point tags need tag_end
        const char *gt = tag_end(q);
        if (!gt) break;
        p = gt + 1;

        Point pt = {0, 0, 0, false};
        if (!get_attr(q, gt, "lat", &pt.lat) || !get_attr(q, gt, "lon", &pt.lon) ||
            !(absd(pt.lat) <= 90) || !(absd(pt.lon) <= 180)) { skipped++; continue; }

        // <ele> is the first child: search a bounded window up to the closing tag
        if (elevation && gt[-1] != '/') {
            const char *nul = memchr(p, '\0', 1024), *end = nul ? nul : p + 1024;
            for (const char *e = p; e < end && strncmp(e, "pt>", 3); e++) {
                if (strncmp(e, "ele>", 4) || (e[-1] != '<' && e[-1] != ':')) continue;
                char *num;
                double v = strtod(e + 4, &num);
                if (num != e + 4 && absd(v) < 1e9) {
                    pt.ele = (long)v;
                    pt.has_ele = true;
                }
                break;
            }
        }
        emit(ctx, pt);
    }
    if (skipped) fprintf(stderr, "Warning: %s: skipped %d point(s) with missing or invalid lat/lon\n", name, skipped);
}

// --------------------------------------------------------------- geometry

// Offset in meters of b relative to a; k is cos(latitude) for the local scale
static void offset(Point a, Point b, double k, double *dx, double *dy) {
    *dx = (b.lon - a.lon) * k * MPERDEG;
    *dy = (b.lat - a.lat) * MPERDEG;
}

static double dist(Point a, Point b) {
    double dx, dy;
    offset(a, b, cos_deg(a.lat), &dx, &dy);
    return hyp(dx, dy);
}

// Line a-b prepared once, so each point only costs a few multiplications
typedef struct {
    Point  a;
    double k, bx, by, len;
} Line;

static Line line_init(Point a, Point b) {
    Line l = {a, cos_deg(a.lat), 0, 0, 0};
    offset(a, b, l.k, &l.bx, &l.by);
    l.len = hyp(l.bx, l.by);
    return l;
}

// Perpendicular distance in meters of p to the line
static double line_dist(const Line *l, Point p) {
    double px, py;
    offset(l->a, p, l->k, &px, &py);
    return l->len == 0 ? hyp(px, py) : absd(px * l->by - py * l->bx) / l->len;
}

// Distance in meters of p to the segment (clamped at both ends)
static double seg_dist(const Line *l, Point p) {
    double px, py;
    offset(l->a, p, l->k, &px, &py);
    if (l->len == 0) return hyp(px, py);
    double t = (px * l->bx + py * l->by) / (l->len * l->len);
    if (t < 0) return hyp(px, py);
    if (t > 1) return hyp(px - l->bx, py - l->by);
    return absd(px * l->by - py * l->bx) / l->len;
}

// ---------------------------------------------------------------- filters

// Drop points closer than `min` meters to the previously kept point
static size_t remove_nearby(Point *p, size_t n, double min) {
    size_t k = 1;
    for (size_t i = 1; i < n - 1; i++)
        if (dist(p[k - 1], p[i]) >= min) p[k++] = p[i];
    if (n > 1) p[k++] = p[n - 1];
    return k;
}

// Drop runs of 1-3 points that stick out more than `min` meters from the line
// between the last kept point and the point after the run, and whose detour is
// at least twice as long as that line (an out-and-back excursion)
static size_t remove_spikes(Point *p, size_t n, double min) {
    size_t k = 1;
    for (size_t i = 1; i + 1 < n; ) {
        size_t w, skip = 0;
        // A point within `min` meters of the last kept point cannot stick out
        // further than that (the lon scaling cos() <= 1 only overestimates)
        if (hyp(p[i].lat - p[k - 1].lat, p[i].lon - p[k - 1].lon) * MPERDEG <= min) {
            p[k++] = p[i++];
            continue;
        }
        for (w = 1; w <= 3 && i + w < n && !skip; w++) {
            Line l = line_init(p[k - 1], p[i + w]);
            double path = 0, dev = 1e300;
            Point prev = p[k - 1];
            for (size_t j = i; j <= i + w; j++) {
                path += dist(prev, p[j]);
                prev = p[j];
            }
            for (size_t j = i; j < i + w; j++) {
                double d = line_dist(&l, p[j]);
                if (d < dev) dev = d;
            }
            if (dev > min && path >= 2 * dist(p[k - 1], p[i + w])) skip = w;
        }
        if (skip) i += skip;
        else p[k++] = p[i++];
    }
    if (n > 1) p[k++] = p[n - 1];
    return k;
}

// ----------------------------------------------------------------- output

// Fixed decimals with trailing zeros removed (%g could emit exponents)
static void print_num(FILE *fp, double v, int digits) {
    char buf[64];
    snprintf(buf, sizeof buf, "%.*f", digits, v);
    char *dot = strchr(buf, '.');
    if (dot) {
        char *e = buf + strlen(buf) - 1;
        while (*e == '0') *e-- = '\0';
        if (*e == '.') *e = '\0';
    }
    if (!strcmp(buf, "-0")) strcpy(buf, "0");
    fputs(buf, fp);
}

static char *out_name(const char *in, int index) {
    size_t len = strlen(in) + 16;
    char *s = malloc(len);
    if (!s) die("out of memory", NULL);
    if (index) snprintf(s, len, "%s%d.gpx", in, index);
    else       snprintf(s, len, "%s.gpx", in);
    return s;
}

// ------------------------------------------ Lang simplification (streaming)

#define WINDOW 64                   // max points a segment can skip

typedef struct {
    Point  buf[WINDOW + 1];         // buf[0] is the last written point
    size_t cnt, parsed, kept;
    FILE  *fp;
    double eps;
    int    digits;
    bool   elevation;
} Stream;

static void put_point(Stream *s, Point p) {
    s->kept++;
    fputs("<trkpt lat=\"", s->fp);
    print_num(s->fp, p.lat, s->digits);
    fputs("\" lon=\"", s->fp);
    print_num(s->fp, p.lon, s->digits);
    if (s->elevation && p.has_ele) fprintf(s->fp, "\"><ele>%ld</ele></trkpt>", p.ele);
    else fputs("\"/>", s->fp);
}

// Write the farthest point of the window that keeps every point before it
// within eps of the segment, and make it the new window start
static void lang_step(Stream *s) {
    size_t e = s->cnt, bad = 1;
    double k = cos_deg(s->buf[0].lat);          // same scale for every candidate
    for (; e > 1; e--) {
        Line l = {s->buf[0], k, 0, 0, 0};
        offset(l.a, s->buf[e], k, &l.bx, &l.by);
        l.len = hyp(l.bx, l.by);
        // Test the point that rejected the previous candidate first
        if (bad < e && seg_dist(&l, s->buf[bad]) > s->eps) continue;
        size_t i = 1;
        while (i < e && seg_dist(&l, s->buf[i]) <= s->eps) i++;
        if (i == e) break;
        bad = i;
    }
    put_point(s, s->buf[e]);
    memmove(s->buf, s->buf + e, (s->cnt - e + 1) * sizeof(Point));
    s->cnt -= e;
}

static void lang_emit(void *ctx, Point p) {
    Stream *s = ctx;
    if (!s->parsed++) {
        s->buf[0] = p;
        put_point(s, p);
        return;
    }
    s->buf[++s->cnt] = p;
    if (s->cnt == WINDOW) lang_step(s);
}

// Map the file read-only; the zero-filled rest of the last page terminates the
// text for the parser. Files that fill their last page exactly are read instead.
static char *map_file(const char *name, size_t *len, bool *mapped) {
    int fd = open(name, O_RDONLY);
    struct stat st;
    if (fd < 0 || fstat(fd, &st)) die("could not open", name);
    *len = (size_t)st.st_size;
    *mapped = *len > 0 && *len % (size_t)sysconf(_SC_PAGESIZE) != 0;
    char *xml = NULL;
    if (*mapped) {
        xml = mmap(NULL, *len, PROT_READ, MAP_PRIVATE, fd, 0);
        if (xml == MAP_FAILED) die("could not read", name);
        madvise(xml, *len, MADV_SEQUENTIAL);
    }
    close(fd);
    return *mapped ? xml : read_file(name, len);
}

static void process_file(const char *in, double eps, double nearby, double spike, int digits,
                         bool elevation, const char *header, bool quiet) {
    size_t len;
    bool mapped;
    char *xml = map_file(in, &len, &mapped);
    char *out = out_name(in, 0);
    Stream s = {.eps = eps, .digits = digits, .elevation = elevation};
    s.fp = fopen(out, "wb");
    if (!s.fp) die("could not write", out);
    fputs(header, s.fp);

    if (nearby > 0 || spike > 0) {
        // These filters need random access, so collect the points first
        Track t = {0};
        parse_points(xml, add_point, &t, elevation, in);
        s.parsed = t.n;
        if (t.n && nearby > 0) t.n = remove_nearby(t.pts, t.n, nearby);
        if (t.n && spike > 0)  t.n = remove_spikes(t.pts, t.n, spike);
        size_t parsed = s.parsed;
        s.parsed = 0;
        for (size_t i = 0; i < t.n; i++) lang_emit(&s, t.pts[i]);
        s.parsed = parsed;
        free(t.pts);
    } else {
        parse_points(xml, lang_emit, &s, elevation, in);
    }
    if (!s.parsed) { fclose(s.fp); remove(out); die("no rtept/trkpt found in", in); }
    while (s.cnt) lang_step(&s);
    fputs(GPXFOOTER, s.fp);
    long outsize = ftell(s.fp);
    if (fclose(s.fp)) die("could not write", out);
    if (!quiet)
        printf("%s => %s\n%8zu => %8zu (%.2f%%) trackpoints\n%8zu => %8ld (%.2f%%) bytes\n",
               in, out, s.parsed, s.kept, s.kept * 100.0 / s.parsed, len, outsize, outsize * 100.0 / len);
    if (mapped) munmap(xml, len); else free(xml);
    free(out);
}

// Write every <trk>...</trk> to its own file, reusing the original header
static void split_file(const char *name) {
    char *xml = read_file(name, NULL);
    const char *first = NULL;
    int count = 0;
    for (const char *p = xml; (p = strchr(p, '<')); ) {
        const char *after = skip_special(p + 1);
        if (after) { p = after; continue; }
        if (strncmp(p, "<trk>", 5) && strncmp(p, "<trk ", 5)) { p++; continue; }
        const char *end = strstr(p, "</trk>");
        if (!end) break;
        end += 6;
        if (!first) first = p;
        char *out = out_name(name, ++count);
        FILE *fp = fopen(out, "wb");
        if (!fp) die("could not write", out);
        fwrite(xml, 1, first - xml, fp);
        fwrite(p, 1, end - p, fp);
        fputs("</gpx>", fp);
        if (fclose(fp)) die("could not write", out);
        free(out);
        p = end;
    }
    if (!count) die("no <trk> found in", name);
    free(xml);
}

int main(int argc, char *argv[]) {
    int digits = DIGITS, opt;
    bool elevation = true, quiet = false, split = false;
    double eps = EPSILON, nearby = NEARBY, spike = SPIKE;
    const char *header = GPXHEADER_FULL;

    while ((opt = getopt(argc, argv, "d:ehmn:p:qs:t")) != -1) {
        switch (opt) {
        case 'd': digits = atoi(optarg);
                  if (digits < 1 || digits > 9) die("invalid number of digits (1-9)", NULL);
                  break;
        case 'e': elevation = false; break;
        case 'h': help(); return 0;
        case 'm': header = GPXHEADER_MIN; break;
        case 'n': nearby = atof(optarg);
                  if (nearby < 0 || nearby > 100) die("invalid nearby value (0-100)", NULL);
                  break;
        case 'p': eps = atof(optarg);
                  if (eps < 0 || eps > 100) die("invalid precision (0-100)", NULL);
                  break;
        case 'q': quiet = true; break;
        case 's': spike = atof(optarg);
                  if (spike < 0 || spike > 100) die("invalid spike value (0-100)", NULL);
                  break;
        case 't': split = true; break;
        default:  return 1;
        }
    }
    if (optind == argc) { help(); return 0; }

    for (int i = optind; i < argc; i++) {
        const char *in = argv[i];
        if (split) { split_file(in); continue; }

        process_file(in, eps, nearby, spike, digits, elevation, header, quiet);
    }
    return 0;
}

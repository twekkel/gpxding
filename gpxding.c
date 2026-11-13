#include <ctype.h>
#include <getopt.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define _STRINGIFY(s)   #s
#define STRINGIFY(s)    _STRINGIFY(s)

#define VERSION         "1.0.0"
#define GPXHEADER_FULL  "<?xml version=\"1.0\" encoding=\"UTF-8\"?><gpx version=\"1.1\" creator=\"gpxding "VERSION"\" xmlns=\"http://www.topografix.com/GPX/1/1\" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" xsi:schemaLocation=\"http://www.topografix.com/GPX/1/1 http://www.topografix.com/GPX/1/1/gpx.xsd\"><trk><trkseg>"
#define GPXHEADER_MIN   "<gpx><trk><trkseg>"
#define GPXFOOTER       "</trkseg></trk></gpx>"
#define DIGITS          5
#define EPSILON         2.0
#define NEARBY          0.0
#define SPIKE           0.0

#define PI              3.14159265358979323846
#define MPERDEG         (6371008.8 * PI / 180)      // meters per degree latitude

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
    return isalnum((unsigned char)c) || c == ':' || c == '_' || c == '-' || c == '.';
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

static void add_point(Track *t, Point p) {
    if (t->n == t->cap) {
        t->cap = t->cap ? t->cap * 2 : 1024;
        t->pts = realloc(t->pts, t->cap * sizeof(Point));
        if (!t->pts) die("out of memory", NULL);
    }
    t->pts[t->n++] = p;
}

// Collect all trkpt and rtept elements (any namespace prefix)
static void parse_points(const char *xml, Track *t, bool elevation, const char *name) {
    int skipped = 0;
    for (const char *p = xml; (p = strchr(p, '<')); ) {
        const char *q = p + 1, *after = skip_special(q);
        if (after) { p = after; continue; }
        const char *n = q;
        while (is_name_char(*q)) q++;
        if (*n == '/') { p = q; continue; }
        const char *gt = tag_end(q);
        if (!gt) break;
        const char *colon = memchr(n, ':', q - n), *local = colon ? colon + 1 : n;
        size_t len = q - local;
        p = gt + 1;
        if (len != 5 || (memcmp(local, "trkpt", 5) && memcmp(local, "rtept", 5))) continue;

        Point pt = {0, 0, 0, false};
        if (!get_attr(q, gt, "lat", &pt.lat) || !get_attr(q, gt, "lon", &pt.lon) ||
            !(fabs(pt.lat) <= 90) || !(fabs(pt.lon) <= 180)) { skipped++; continue; }

        // <ele> is the first child: search a bounded window up to the closing tag
        if (elevation && gt[-1] != '/') {
            const char *nul = memchr(p, '\0', 1024), *end = nul ? nul : p + 1024;
            for (const char *e = p; e < end && strncmp(e, "pt>", 3); e++) {
                if (strncmp(e, "ele>", 4) || (e[-1] != '<' && e[-1] != ':')) continue;
                char *num;
                double v = strtod(e + 4, &num);
                if (num != e + 4 && fabs(v) < 1e9) {
                    pt.ele = (long)v;
                    pt.has_ele = true;
                }
                break;
            }
        }
        add_point(t, pt);
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
    offset(a, b, cos(a.lat * PI / 180), &dx, &dy);
    return hypot(dx, dy);
}

// Line a-b prepared once, so each point only costs a few multiplications
typedef struct {
    Point  a;
    double k, bx, by, len;
} Line;

static Line line_init(Point a, Point b) {
    Line l = {a, cos(a.lat * PI / 180), 0, 0, 0};
    offset(a, b, l.k, &l.bx, &l.by);
    l.len = hypot(l.bx, l.by);
    return l;
}

// Perpendicular distance in meters of p to the line
static double line_dist(const Line *l, Point p) {
    double px, py;
    offset(l->a, p, l->k, &px, &py);
    return l->len == 0 ? hypot(px, py) : fabs(px * l->by - py * l->bx) / l->len;
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
        if (hypot(p[i].lat - p[k - 1].lat, p[i].lon - p[k - 1].lon) * MPERDEG <= min) {
            p[k++] = p[i++];
            continue;
        }
        for (w = 1; w <= 3 && i + w < n && !skip; w++) {
            Line l = line_init(p[k - 1], p[i + w]);
            double path = 0, dev = HUGE_VAL;
            Point prev = p[k - 1];
            for (size_t j = i; j <= i + w; j++) {
                path += dist(prev, p[j]);
                prev = p[j];
            }
            for (size_t j = i; j < i + w; j++) dev = fmin(dev, line_dist(&l, p[j]));
            if (dev > min && path >= 2 * dist(p[k - 1], p[i + w])) skip = w;
        }
        if (skip) i += skip;
        else p[k++] = p[i++];
    }
    if (n > 1) p[k++] = p[n - 1];
    return k;
}

// Ramer-Douglas-Peucker with an explicit stack; keep[] marks retained points
static void rdp(const Point *p, size_t n, double eps, bool *keep) {
    size_t (*stack)[2] = malloc(n * sizeof *stack);
    if (!stack) die("out of memory", NULL);
    size_t sp = 0;
    keep[0] = keep[n - 1] = true;
    stack[sp][0] = 0; stack[sp++][1] = n - 1;
    while (sp) {
        size_t a = stack[--sp][0], b = stack[sp][1], far = 0;
        double dmax = 0;
        Line l = line_init(p[a], p[b]);
        for (size_t i = a + 1; i < b; i++) {
            double d = line_dist(&l, p[i]);
            if (d > dmax) { dmax = d; far = i; }
        }
        if (dmax <= eps) continue;
        keep[far] = true;
        stack[sp][0] = a;   stack[sp++][1] = far;
        stack[sp][0] = far; stack[sp++][1] = b;
    }
    free(stack);
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

static size_t write_gpx(const char *name, const Point *p, const bool *keep, size_t n,
                        int digits, bool elevation, const char *header, long *bytes) {
    FILE *fp = fopen(name, "wb");
    if (!fp) die("could not write", name);
    size_t kept = 0;
    fputs(header, fp);
    for (size_t i = 0; i < n; i++) {
        if (!keep[i]) continue;
        kept++;
        fputs("<trkpt lat=\"", fp);
        print_num(fp, p[i].lat, digits);
        fputs("\" lon=\"", fp);
        print_num(fp, p[i].lon, digits);
        if (elevation && p[i].has_ele) {
            fprintf(fp, "\"><ele>%ld</ele></trkpt>", p[i].ele);
        } else {
            fputs("\"/>", fp);
        }
    }
    fputs(GPXFOOTER, fp);
    *bytes = ftell(fp);
    if (fclose(fp)) die("could not write", name);
    return kept;
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

        size_t len;
        char *xml = read_file(in, &len);
        Track t = {0};
        parse_points(xml, &t, elevation, in);
        free(xml);
        if (!t.n) die("no rtept/trkpt found in", in);
        size_t parsed = t.n;

        if (nearby > 0) t.n = remove_nearby(t.pts, t.n, nearby);
        if (spike > 0)  t.n = remove_spikes(t.pts, t.n, spike);

        bool *keep = calloc(t.n, 1);
        if (!keep) die("out of memory", NULL);
        rdp(t.pts, t.n, eps, keep);

        char *out = out_name(in, 0);
        long outsize;
        size_t kept = write_gpx(out, t.pts, keep, t.n, digits, elevation, header, &outsize);
        if (!quiet)
            printf("%s => %s\n%8zu => %8zu (%.2f%%) trackpoints\n%8zu => %8ld (%.2f%%) bytes\n",
                   in, out, parsed, kept, kept * 100.0 / parsed, len, outsize, outsize * 100.0 / len);
        free(out); free(keep); free(t.pts);
    }
    return 0;
}

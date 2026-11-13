#include <ctype.h>
#include <getopt.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define _STRINGIFY(s)   #s
#define STRINGIFY(s)    _STRINGIFY(s)

#ifndef M_PI
#define M_PI            3.14159265358979323846
#endif

#define VERSION         "0.0.5"
#define GPXHEADER_FULL  "<?xml version=\"1.0\" encoding=\"UTF-8\"?><gpx version=\"1.1\" creator=\"gpxding "VERSION"\" xmlns=\"http://www.topografix.com/GPX/1/1\" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" xsi:schemaLocation=\"http://www.topografix.com/GPX/1/1 http://www.topografix.com/GPX/1/1/gpx.xsd\"><trk><trkseg>"
#define GPXHEADER_MIN   "<gpx><trk><trkseg>"
#define GPXFOOTER       "</trkseg></trk></gpx>"
#define DIGITS          5
#define ELEVATION       true
#define EPSILON         2.0
#define NEARBY          0.0
#define QUIET           false
#define SPIKE           0.0

#define EARTH_RADIUS    6371008.8                   // arithmetic mean radius
#define MPERLAT         EARTH_RADIUS * 2 * M_PI / 360


typedef struct GPXPoint {
    double lat;
    double lon;
    double ele;
    bool   has_ele;
    bool   rdp;
} GPXPoint;

// Print help page
static void help() {
    puts("gpxding version v"VERSION"\n\
Usage: gpxding [OPTIONS] [FILE ...]\n\
  -d    number of digits (default "STRINGIFY(DIGITS)")\n\
  -e    omit elevation info\n\
  -h    show this help\n\
  -m    use minimal <gpx> (not compatible with all apps/devices)\n\
  -n    remove nearby points (default 0 m, disabled)\n\
  -p    precision in meters (default "STRINGIFY(EPSILON)" m)\n\
  -q    quiet\n\
  -s    remove spikes (default 0, disabled)\n\
  -t    split gpx file into individual tracks\n");

    return;
}

// Print a coordinate with a fixed number of decimals, stripping trailing
// zeros. %g is not used: it falls back to scientific notation for small
// values (e.g. 1e-05), which is invalid in GPX (lat/lon are xsd:decimal)
static void print_coord(FILE *fp, double v, int digits) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%.*f", digits, v);
    char *dot = strchr(buf, '.');
    if (dot != NULL) {
        char *e = buf + strlen(buf) - 1;
        while (e > dot && *e == '0') *e-- = '\0';
        if (*e == '.') *e = '\0';
    }
    fputs(buf, fp);
}

// Concatenate strings
char* concat(const char *s1, const char *s2)
{
    char *result = malloc(strlen(s1) + strlen(s2) + 1);
    strcpy(result, s1);
    strcat(result, s2);
    return result;
}

// Read the whole file into a NUL-terminated buffer
static char* readFile(const char* filename) {
    FILE            *fp;
    struct stat     file_stat;
    char            *buf;

    if (stat(filename, &file_stat) == -1 || (fp = fopen(filename, "rb")) == NULL) {
        fprintf(stderr, "Error: Could not open file %s\n", filename);
        exit(1);
    }
    buf = malloc(file_stat.st_size + 1);
    if (buf == NULL || fread(buf, 1, file_stat.st_size, fp) != (size_t)file_stat.st_size) {
        fprintf(stderr, "Error: Could not read file %s\n", filename);
        fclose(fp);
        exit(1);
    }
    buf[file_stat.st_size] = '\0';
    fclose(fp);
    return buf;
}

// Split the GPX file in files with individual tracks
static void splitGPXFile(const char* infilename) {
    char *file_contents = readFile(infilename);
    char *header_start = file_contents;
    char *trk_start = strstr(file_contents, "<trk>");

    if (trk_start == NULL) {
        printf("No <trk> tag found in the GPX file.\n");
        free(file_contents);
        exit(1);
    }

    char *header_end = trk_start;
    int count = 1;

    // Find and split the individual tracks, <trk>...</trk>
    while (trk_start != NULL) {
        char *trk_end = strstr(trk_start, "</trk>");
        if (trk_end == NULL) {
            break;
        }

        trk_end += 6; // Move past the </trk> tag.

        char *outfilename = malloc(strlen(infilename) + 16);
        if (outfilename == NULL) {
            fprintf(stderr, "Error: Out of memory\n");
            free(file_contents);
            exit(1);
        }
        snprintf(outfilename, strlen(infilename) + 16, "%s%d.gpx", infilename, count);
        FILE *outfile = fopen(outfilename, "w");
        if (outfile == NULL) {
            fprintf(stderr, "Error: Could not open output file\n");
            free(outfilename);
            free(file_contents);
            exit(1);
        }

        fwrite(header_start, header_end - header_start, 1, outfile);
        fwrite(trk_start, trk_end - trk_start, 1, outfile);
        fprintf(outfile, "</gpx>");
        fclose(outfile);
        free(outfilename);

        trk_start = strstr(trk_end, "<trk>");
        count++;
    }

    free(file_contents);
}

// Extract a numeric attribute from the attribute list [attrs, attrs_end) of an
// open tag. Handles arbitrary attribute order, whitespace around '=' and both
// quote styles. The value is copied to a small buffer because strtod needs a
// NUL-terminated string; 63 chars is far beyond double precision for a coordinate.
static bool get_attr(const char* attrs, const char* attrs_end, const char* name, double* out) {
    size_t          name_len = strlen(name);

    while (attrs < attrs_end) {
        while (attrs < attrs_end && isspace((unsigned char)*attrs)) attrs++;
        if (attrs >= attrs_end) break;
        const char *an = attrs;
        while (attrs < attrs_end && *attrs != '=' && !isspace((unsigned char)*attrs)) attrs++;
        size_t an_len = attrs - an;
        while (attrs < attrs_end && isspace((unsigned char)*attrs)) attrs++;
        if (attrs >= attrs_end || *attrs != '=') continue;      // attribute without value
        attrs++;
        while (attrs < attrs_end && isspace((unsigned char)*attrs)) attrs++;
        if (attrs >= attrs_end || (*attrs != '"' && *attrs != '\'')) continue;
        char quote = *attrs++;
        const char *av = attrs;
        while (attrs < attrs_end && *attrs != quote) attrs++;
        if (an_len == name_len && memcmp(an, name, name_len) == 0) {
            char buf[64];
            size_t n = (size_t)(attrs - av);
            if (n > sizeof(buf) - 1) n = sizeof(buf) - 1;
            memcpy(buf, av, n);
            buf[n] = '\0';
            char *endptr;
            *out = strtod(buf, &endptr);
            return endptr != buf;                              // false if not numeric
        }
        if (attrs < attrs_end) attrs++;                         // skip closing quote
    }
    return false;
}

// Parse the GPX file and extract the lat,lon and elevation data.
// Single pass over the raw text: no XML library, just enough parsing for
// trkpt/rtept tags. Not 100% XML compliant, but sufficient for real GPX files.
static void parseGPXFile(const char* filename, GPXPoint** points, int* num_points, bool elevation) {
    char            *xml = readFile(filename);
    const char      *p = xml;
    int             capacity = 0;
    int             skipped = 0;

    *points = NULL;
    *num_points = 0;

    while ((p = strchr(p, '<')) != NULL) {
        const char *q = p + 1;

        // Skip comments, processing instructions, CDATA and DOCTYPE
        if (strncmp(q, "!--", 3) == 0) {
            const char *e = strstr(q, "-->");
            p = (e != NULL) ? e + 3 : q + strlen(q);
            continue;
        }
        if (*q == '?' || *q == '!') {
            const char *e = strchr(q, '>');
            p = (e != NULL) ? e + 1 : q + strlen(q);
            continue;
        }
        if (*q == '/') {                                        // closing tag
            p = q + 1;
            continue;
        }

        // Tag name, possibly namespace prefixed
        const char *name = q;
        while (*q != '\0' && (isalnum((unsigned char)*q) || *q == ':' || *q == '_' || *q == '-' || *q == '.'))
            q++;
        const char *colon = memchr(name, ':', q - name);
        const char *local = (colon != NULL) ? colon + 1 : name;
        size_t local_len = q - local;
        bool is_pt = local_len == 5 &&
                     (memcmp(local, "trkpt", 5) == 0 || memcmp(local, "rtept", 5) == 0);

        // Find the end of the open tag, ignoring '>' inside quoted attribute values
        const char *gt = q;
        char quote = '\0';
        while (*gt != '\0') {
            if (quote != '\0') {
                if (*gt == quote) quote = '\0';
            } else if (*gt == '"' || *gt == '\'') {
                quote = *gt;
            } else if (*gt == '>') {
                break;
            }
            gt++;
        }
        if (*gt == '\0') break;                                 // unterminated tag
        bool self_closing = gt > q && gt[-1] == '/';

        if (is_pt) {
            GPXPoint point;
            point.ele = 0.0;
            point.has_ele = false;
            point.rdp = true;
            // A point without valid lat/lon (missing, non-numeric or out of
            // range) must not silently become 0.0 (Null Island); skip it
            bool valid = get_attr(q, gt, "lat", &point.lat) &&
                         get_attr(q, gt, "lon", &point.lon) &&
                         point.lat >= -90.0 && point.lat <= 90.0 &&
                         point.lon >= -180.0 && point.lon <= 180.0;
            if (!valid) {
                skipped++;
                p = gt + 1;
                continue;
            }

            // Extract the ele(vation) data (if available and desired), bounded
            // by this element's closing tag or the next point tag
            if (elevation && !self_closing) {
                char close[64];
                snprintf(close, sizeof(close), "</%.*s", (int)(q - name), name);
                const char *seg_end = strstr(gt + 1, close);
                const char *next_pt = strstr(gt + 1, "<trkpt");
                const char *next_rte = strstr(gt + 1, "<rtept");
                if (next_pt != NULL && (seg_end == NULL || next_pt < seg_end)) seg_end = next_pt;
                if (next_rte != NULL && (seg_end == NULL || next_rte < seg_end)) seg_end = next_rte;
                if (seg_end == NULL) seg_end = gt + strlen(gt);
                const char *ele = strstr(gt + 1, "<ele");
                if (ele != NULL && ele < seg_end &&
                    (ele[4] == '>' || ele[4] == '/' || isspace((unsigned char)ele[4]))) {
                    const char *content = strchr(ele + 4, '>');
                    const char *ele_end = strstr(ele, "</ele>");
                    if (content != NULL && ele_end != NULL && content < ele_end && ele_end < seg_end) {
                        char buf[64];
                        size_t n = (size_t)(ele_end - content - 1);
                        if (n > sizeof(buf) - 1) n = sizeof(buf) - 1;
                        memcpy(buf, content + 1, n);
                        buf[n] = '\0';
                        char *endptr;
                        point.ele = strtod(buf, &endptr);
                        point.has_ele = endptr != buf;
                    }
                }
            }

            // Add the point to the dynamic array
            if (*num_points == capacity) {
                capacity = (capacity == 0) ? 1024 : capacity * 2;
                *points = (GPXPoint*)realloc(*points, sizeof(GPXPoint) * capacity);
                if (*points == NULL) {
                    fprintf(stderr, "Error: Out of memory\n");
                    exit(1);
                }
            }
            (*points)[(*num_points)++] = point;
        }
        p = gt + 1;
    }

    if (skipped > 0) {
        fprintf(stderr, "Warning: %s: skipped %d point(s) with missing or invalid lat/lon\n",
                filename, skipped);
    }
    free(xml);
}

// Returns the distance between point a and b
double distance(GPXPoint a, GPXPoint b) {
    // lat,lon -> x,y
    double lon_d_at_lat = cos(a.lat * M_PI / 180);
    a.lon = a.lon * lon_d_at_lat;
    b.lon = b.lon * lon_d_at_lat;
    double lat = a.lat - b.lat;
    double lon = a.lon - b.lon;
    return sqrt(lat * lat + lon * lon);
}

// returns the perpendicular distance from p to the line between a and b
double p_distance(GPXPoint p, GPXPoint a, GPXPoint b) {
    // lat,lon -> x,y
    double lon_d_at_lat = cos(p.lat * M_PI / 180);
    p.lon = p.lon * lon_d_at_lat;
    a.lon = a.lon * lon_d_at_lat;
    b.lon = b.lon * lon_d_at_lat;
    double dx = b.lat - a.lat;
    double dy = b.lon - a.lon;
    double d = sqrt(dx * dx + dy * dy);
    return fabs(p.lat * dy - p.lon * dx + b.lat * a.lon - b.lon * a.lat)/d;
}

// Omit spike points
void despike(GPXPoint *points, int n, double epsilon) {
    for (int i = 0; i < n - 2; i++) {
        double ab = distance(points[i], points[i + 1]);
        double bc = distance(points[i + 1], points[i + 2]);
        double ac = distance(points[i], points[i + 2]);
        if (ac < ab || ac < bc) {
            points[i + 1] = points[i];
            continue;
        }
        double pd = p_distance(points[i + 1], points[i], points[i + 2]);
        if (pd * epsilon > ac) {
            points[i + 1] = points[i];
        }
    }
}

// Deduplicate nearby points
void reduce_nearby(GPXPoint *points, int n, double epsilon) {
    for (int i = 0; i < n - 2; i++) {
        if (distance(points[i], points[i + 1]) < epsilon) {
            points[i + 1] = points[i];
        }
    }
}

// Apply Ramer–Douglas–Peucker algorithm to reduce the number of trackpoints
void rdp_simplify(GPXPoint *points, int n, double epsilon) {
    int             index = 0;
    double          dmax = 0.0;

    for (int i = 1; i < n - 1; i++) {
        double d = p_distance(points[i], points[0], points[n-1]);
        if (d > dmax) {
            index = i;
            dmax = d;
        }
    }
    if (dmax > epsilon) {
        rdp_simplify(points, index + 1, epsilon);
        rdp_simplify(points + index, n - index, epsilon);
    } else {
        for (int i = 1; i < n - 1; i++) {
            points[i].rdp = false;                    // mark point for removal
        }
    }
}

// Write the reduced GPX file
void writeGPXFile(const GPXPoint* points, int n, const char* filename, int digits, bool elevation, const char* gpxheader) {
    FILE            *fp;

    fp = fopen(filename, "w");
    if (fp != NULL) {
        fputs(gpxheader, fp);
        for (int i = 0; i < n; ++i) {
            if (points[i].rdp == true) {
                if (elevation && points[i].has_ele) {
                    fputs("<trkpt lat=\"", fp);
                    print_coord(fp, points[i].lat, digits);
                    fputs("\" lon=\"", fp);
                    print_coord(fp, points[i].lon, digits);
                    fputs("\"><ele>", fp);
                    print_coord(fp, points[i].ele, digits);
                    fputs("</ele></trkpt>", fp);
                } else {
                    fputs("<trkpt lat=\"", fp);
                    print_coord(fp, points[i].lat, digits);
                    fputs("\" lon=\"", fp);
                    print_coord(fp, points[i].lon, digits);
                    fputs("\"></trkpt>", fp);
                }
            }
        }
        fprintf(fp, GPXFOOTER);
        fclose(fp);
    } else {
        fprintf(stderr, "Error: Could not write to file %s\n", filename);
        exit(1);
    }
}


int main(int argc, char *argv[]){
    extern char     *optarg;
    extern int      optind;
    const char*     infilename;
    int             param;
    int             digits = DIGITS;
    bool            elevation = ELEVATION;
    double          epsilon = EPSILON;
    const char*     gpxheader = GPXHEADER_FULL;
    double          nearby = NEARBY;
    bool            quiet = QUIET;
    double          spike = SPIKE;
    bool            split = false;

    while ((param = getopt(argc, argv, "d:ehmn:p:qs:t")) != -1)
    switch(param) {
        case 'd':                                   // number of digits
            digits = atoi(optarg) ;
            if ((digits < 1) || (digits > 9)) {
                fputs("Invalid number of digits\n", stderr);
                help();
                exit(1);
            }
            break;
        case 'e':                                   // elevation
            elevation = false;
            break;
        case 'h':                                   // help
            help();
            exit(0);
        case 'm':                                   // use minimal <gpx>
            gpxheader = GPXHEADER_MIN;
            break;
        case 'n':                                   // max error in meter
            nearby = atof(optarg) ;
            if ((nearby < 0) || (nearby > 100)) {
                fputs("Invalid nearby value (0-100)\n", stderr);
                help();
                exit(1);
            }
            break;
        case 'p':                                   // max error in meter
            epsilon = atof(optarg) ;
            if ((epsilon < 0) || (epsilon > 100)) {
                fputs("Invalid precision (0-100)\n", stderr);
                help();
                exit(1);
            }
            break;
        case 'q':
            quiet = true;
            break;
        case 's':                                   // max error in meter
            spike = atof(optarg) ;
            if ((spike < 0) || (spike > 10)) {
                fputs("Invalid spike factor (0-10)\n", stderr);
                help();
                exit(1);
            }
            break;
        case 't':
            split = true;
            break;
        case '?':
            return 1;
        default:
            abort();
    }

    if (argv[optind] == NULL) {
        help();
        exit(0);
    }

    // meter / MPERLAT => degree
    epsilon /= MPERLAT;
    nearby /= MPERLAT;

    // Loop over all GPX files
    for (int i = optind; i < argc; i++) {
        GPXPoint*       points = NULL;
        char*           outfilename;
        struct          stat filestatus;
        int             num_points = 0;

        infilename      = argv[i];

        // Parse GPX file, extracting lat, lon and ELEVATION
        if (split) {
            splitGPXFile(infilename);
            break;
        }

        // Parse GPX file, extracting lat, lon and ELEVATION
        parseGPXFile(infilename, &points, &num_points, elevation);

        // Omit nearby points
        if (num_points == 0) {
            fprintf(stderr, "Error: %s does not contain rtept/trkpt/trackpoints\n", infilename);
            exit(1);
        }

        // Omit nearby points
        if (nearby > 0.0) reduce_nearby(points, num_points, nearby);

        // Omit spike points
        if (spike > 0.0) despike(points, num_points, spike);

        // First and last point can not be the same
        while (num_points > 1 &&
               points[0].lat == points[num_points-1].lat &&
               points[0].lon == points[num_points-1].lon) {
            num_points--;
        }

        // Simplify using RDP algorithm
        rdp_simplify(points, num_points, epsilon);

        // Write new simplified GPX file
        outfilename = concat(infilename, ".gpx");
        writeGPXFile(points, num_points, outfilename, digits, elevation, gpxheader);

        // Print statistics
        if (! quiet) {
            int num_rdp_points = 0;
            for (int rdp = 0; rdp <= num_points-1; rdp++) {
                if (points[rdp].rdp) num_rdp_points++;
            }
            stat(infilename, &filestatus);
            int insize = filestatus.st_size;
            stat(outfilename, &filestatus);
            int outsize = filestatus.st_size;
            printf("%s => %s\n", infilename, outfilename);
            printf("%8i => %8i (%.2f%%) trackpoints\n", num_points, num_rdp_points, num_rdp_points * 100.0 / num_points);
            printf("%8i => %8i (%.2f%%) bytes\n", insize, outsize, outsize * 100.0 / insize);
        }

        free(outfilename);
        free(points);
    }
}

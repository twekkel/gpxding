#include <getopt.h>
#include <libxml/parser.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define _STRINGIFY(s)   #s
#define STRINGIFY(s)    _STRINGIFY(s)

#ifndef M_PI
#define M_PI            3.14159265358979323846
#endif

#define VERSION         "0.0.2"
#define GPXHEADER       "<?xml version=\"1.0\" encoding=\"UTF-8\"?><gpx><trk><trkseg>"
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
    int    ele;
    bool   rdp;
} GPXPoint;

// Print help page
static void help() {
    puts("gpxding version v"VERSION"\n\
Usage: gpxding [OPTIONS] [FILE ...]\n\
  -d    number of digits (default "STRINGIFY(DIGITS)")\n\
  -e    omit elevation info\n\
  -h    show this help\n\
  -n    remove nearby points (default disabled)\n\
  -p    precision in meters (default "STRINGIFY(EPSILON)" m)\n\
  -q    quiet\n\
  -s    remove spikes (default disabled)\n");

    return;
}

// Lose the trailing zeros in combination with printf %.*g
int num_digits(double a, int digits) {
    int int_a = (int) abs(a);
    if (int_a >= 100) {
        digits += 3;
    } else if (int_a >= 10) {
        digits += 2;
    } else if (int_a >= 1) {
        digits += 1;
    }
    return digits;
}

// Concatenate strings
char* concat(const char *s1, const char *s2)
{
    char *result = malloc(strlen(s1) + strlen(s2) + 1);
    strcpy(result, s1);
    strcat(result, s2);
    return result;
}

// Read the GPX file
xmlDocPtr readGPXFile(const char* filename) {
    xmlDocPtr       doc;
    FILE            *fp;

    // Check if file can be opened for reading
    if ((fp = fopen(filename, "r")) == NULL) {
        fprintf(stderr, "Error: Could not open file %s\n", filename);
        exit(1);
    }

    // Initialize libxml2
    xmlInitParser();

    // Parse the GPX file
    doc = xmlReadFile(filename, NULL, 0);

    if (doc == NULL) {
        fprintf(stderr, "Error: Could not parse GPX file %s\n", filename);
        exit(1);
    }

    return doc;
}

// Parse the GPX file and extract the lat,lon and elevation data
void parseGPXFile(const char* filename, GPXPoint** points, int* num_points, bool elevation) {
    xmlDocPtr       doc;
    xmlNodePtr      node, trk_node, trkseg_node, trkpt_node;
    xmlChar*        content;

    doc = readGPXFile(filename);

    node = xmlDocGetRootElement(doc);

    if (node == NULL) {
        fprintf(stderr, "Error: Empty GPX file %s\n", filename);
        xmlFreeDoc(doc);
        exit(1);
    }

    // Find the track point nodes and extract the lat and lon data
    *num_points = 0;
    for (trk_node = node->children; trk_node != NULL; trk_node = trk_node->next) {
        if (xmlStrcmp(trk_node->name, (const xmlChar*)"trk") == 0) {
            for (trkseg_node = trk_node->children; trkseg_node != NULL; trkseg_node = trkseg_node->next) {
                if (xmlStrcmp(trkseg_node->name, (const xmlChar*)"trkseg") == 0) {
                    for (trkpt_node = trkseg_node->children; trkpt_node != NULL; trkpt_node = trkpt_node->next) {
                        if (xmlStrcmp(trkpt_node->name, (const xmlChar*)"trkpt") == 0) {
                            // Extract the lat and lon coordinates
                            GPXPoint point;
                            point.lat = atof((const char*)xmlGetProp(trkpt_node, (const xmlChar*)"lat"));
                            point.lon = atof((const char*)xmlGetProp(trkpt_node, (const xmlChar*)"lon"));
                            point.rdp = true;

                            // Extract the ele(vation) data (if available and desired)
                            if (elevation) {
                                xmlNodePtr ele_node = trkpt_node->children;
                                while (ele_node != NULL) {
                                    if (xmlStrcmp(ele_node->name, (const xmlChar*)"ele") == 0) {
                                        content = xmlNodeGetContent(ele_node);
                                        point.ele = atoi((const char*)content);
                                        xmlFree(content);
                                        break;
                                    }
                                    ele_node = ele_node->next;
                                }
                            }

                            // Add the point to the dynamic array
                            *points = (GPXPoint*)realloc(*points, sizeof(GPXPoint) * (*num_points + 1));
                            (*points)[*num_points] = point;
                            (*num_points)++;
                        }
                    }
                }
            }
        }
    }

    xmlFreeDoc(doc);
    xmlCleanupParser();
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
        if (distance(points[i], points[i+1]) < epsilon) {
            points[i + 1] = points[i];
        }
    }
}

// Apply Ramer–Douglas–Peucker algorithm to reduce the number of trackpoints
void rdp_simplify(GPXPoint *points, int n, double epsilon) {
    int             i, index = 0;
    double          dmax = 0.0;

    for (i = 1; i < n - 1; i++) {
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
        for (i = 1; i < n - 1; i++) {
            points[i].rdp = false;                    // mark point for removal
        }
    }
}

// Write the reduced GPX file
void writeGPXFile(const GPXPoint* points, int n, char* filename, int digits, bool elevation) {
    FILE            *fp;

    fp = fopen(filename, "w");
    if (fp != NULL) {
        fprintf(fp, GPXHEADER);
        for (int i = 0; i < n; ++i) {
            if (points[i].rdp == true) {
                int lat_digits = num_digits(points[i].lat, digits);
                int lon_digits = num_digits(points[i].lon, digits);
                if (elevation) {
                    fprintf(fp, "<trkpt lat=\"%.*g\" lon=\"%.*g\"><ele>%i</ele></trkpt>",
                    lat_digits, points[i].lat, lon_digits, points[i].lon, points[i].ele);
                } else {
                    fprintf(fp, "<trkpt lat=\"%.*g\" lon=\"%.*g\"></trkpt>",
                    lat_digits, points[i].lat, lon_digits, points[i].lon);
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
    double          nearby = NEARBY;
    bool            quiet = QUIET;
    double          spike = SPIKE;

    while ((param = getopt(argc, argv, "d:ehn:p:qs:")) != -1)
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
        parseGPXFile(infilename, &points, &num_points, elevation);

        // Omit nearby points
        if (num_points == 0) {
            fprintf(stderr, "Error: %s does not contain trkpt/trackpoints\n", infilename);
            exit(1);
        }

        // Omit nearby points
        if (nearby > 0.0) reduce_nearby(points, num_points, nearby);

        // Omit spike points
        if (spike > 0.0) despike(points, num_points, spike);

        // Simplify using RDP algorithm
        rdp_simplify(points, num_points, epsilon);

        // Write new simplified GPX file
        outfilename = concat(infilename, ".gpx");
        writeGPXFile(points, num_points, outfilename, digits, elevation);

        // Print statistics
        if (! quiet) {
            int num_rdp_points = 0;
            for (int rdp = 0; rdp <= num_points; rdp++) {
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

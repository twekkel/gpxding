#include <fcntl.h>
#include <getopt.h>
#include <libxml/parser.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define _STRINGIFY(s)   #s
#define STRINGIFY(s)    _STRINGIFY(s)

#ifndef M_PI
#define M_PI            3.14159265358979323846
#endif

#define VERSION         "0.0.5"
#define GPXHEADER_FULL  "<?xml version=\"1.0\" encoding=\"UTF-8\"?><gpx version=\"1.1\" creator=\"gpxding "VERSION"\" xmlns=\"http://www.topografix.com/GPX/1/1\" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" xsi:schemaLocation=\"http://www.topografix.com/GPX/1/1 http://www.topografix.com/GPX/1/1/gpx.xsd\"><trk><trkseg>"
#define GPXHEADER_MIN   "<gpx><trk><trkseg>"
#define GPXFOOTER       "</trkseg></trk></gpx>"
#define NO_ELE          INT_MIN
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
  -m    use minimal <gpx> (not compatible with all apps/devices)\n\
  -n    remove nearby points (default 0 m, disabled)\n\
  -p    precision in meters (default "STRINGIFY(EPSILON)" m)\n\
  -q    quiet\n\
  -s    remove spikes (default 0, disabled)\n\
  -t    split gpx file into individual tracks\n");

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

char* int2str(int num) {
    int num_len = snprintf(NULL, 0, "%d", num); // Get the length of the string
    char *str = (char *)malloc(num_len + 1);
    snprintf(str, num_len + 1, "%d", num);
    return str;
}


// Split the GPX file in files with individual tracks
// Using mmap as this is faster than libxml2
void splitGPXFile(const char* infilename) {

    int fp = open(infilename, O_RDONLY);
    if (fp == -1) {
        fprintf(stderr, "Error: Could not open file\n");
        exit(1);
    }

    struct stat file_stat;
    if (fstat(fp, &file_stat) == -1) {
        fprintf(stderr, "Error: Could not determine file size\n");
        close(fp);
        exit(1);
    }

    char *file_contents = (char *)mmap(NULL, file_stat.st_size, PROT_READ, MAP_PRIVATE, fp, 0);
    if (file_contents == MAP_FAILED) {
        fprintf(stderr, "Error: Could not map into memory\n");
        close(fp);
        exit(1);
    }

    char *trk_start = file_contents;
    char *header_start = file_contents;
    char *header_end = NULL;
    int count = 1;

    // Determine header
    while (1) {
        trk_start = strstr(trk_start, "<trk>");
        if (trk_start == NULL) {
            break;
        }

        header_end = trk_start;
        break;
    }

    if (header_end == NULL) {
        printf("No <trk> tag found in the GPX file.\n");
        exit(1);
    }

    // Find and split the individual tracks, <trk>...</trk>
    while (1) {
        trk_start = strstr(trk_start, "<trk>");
        if (trk_start == NULL) {
            break;
        }

        char *trk_end = strstr(trk_start, "</trk>");
        if (trk_end == NULL) {
            break;
        }

        trk_end += 6; // Move past the </trk> tag.

        char *strcount;
        char *outfilename;
        strcount = int2str(count);
        outfilename = concat(infilename, strcount);
        outfilename = concat(outfilename, ".gpx");
        FILE *outfile = fopen(outfilename, "w");
        if (outfile == NULL) {
            fprintf(stderr, "Error: Could not open output file\n");
            munmap(file_contents, file_stat.st_size);
            close(fp);
            exit(1);
        }

        fwrite(header_start, header_end - header_start, 1, outfile);
        fwrite(trk_start, trk_end - trk_start, 1, outfile);
        fprintf(outfile, "</gpx>");
        fclose(outfile);

        trk_start = trk_end;
        count++;
    }

    if (count == 0) {
        printf("No <trk> or </trk> tags found in the GPX file.\n");
    }

    munmap(file_contents, file_stat.st_size);
    close(fp);
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

// Extract the lat,lon and elevation data
void processNode(GPXPoint** points, xmlNodePtr pt_node, int* num_points, bool elevation) {
    xmlChar*        content;
    GPXPoint        point;

    point.lat = atof((const char*)xmlGetProp(pt_node, (const xmlChar*)"lat"));
    point.lon = atof((const char*)xmlGetProp(pt_node, (const xmlChar*)"lon"));
    point.ele = NO_ELE;
    point.rdp = true;

    // Extract the ele(vation) data (if available and desired)
    if (elevation) {
        xmlNodePtr ele_node = pt_node->children;
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

// Parse the GPX file and extract the lat,lon and elevation data
void parseGPXFile(const char* filename, GPXPoint** points, int* num_points, bool elevation) {
    xmlDocPtr       doc;
    xmlNodePtr      node, trk_node, trkseg_node, pt_node;

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
                    for (pt_node = trkseg_node->children; pt_node != NULL; pt_node = pt_node->next) {
                        if (xmlStrcmp(pt_node->name, (const xmlChar*)"trkpt") == 0) {
                            processNode(points, pt_node, num_points, elevation);
                        }
                    }
                }
            }
        } else if (xmlStrcmp(trk_node->name, (const xmlChar*)"rte") == 0) {
            for (pt_node = trk_node->children; pt_node != NULL; pt_node = pt_node->next) {
                if (xmlStrcmp(pt_node->name, (const xmlChar*)"rtept") == 0) {
                    processNode(points, pt_node, num_points, elevation);
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
        fprintf(fp, gpxheader);
        for (int i = 0; i < n; ++i) {
            if (points[i].rdp == true) {
                int lat_digits = num_digits(points[i].lat, digits);
                int lon_digits = num_digits(points[i].lon, digits);
                if (elevation && points[i].ele != NO_ELE) {
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
        while (points[0].lat == points[num_points-1].lat &&
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

**gpxding** (_ding_ translates into _thing_ in English) is a utility to minify GPX files by removing redundant information. Only (a reduced number of) track points and elevation (optional) information is retained, while all other excess information is discarded. File size will be drastically reduced, while still accurate and useful for route navigation.

The goal of **gpxding** is to have the smallest possible GPX file for route navigation.

### Installation

#### Prerequisites

Libxml2 is a required package for gpxding and installation depends on the OS, e.g.

```
sudo apt install libxml2-dev
or
sudo yum install libxml2-dev
or
...
```


#### Build
```
make
sudo make install
```

### Usage

```
gpxding version v0.0.4
Usage: gpxding [OPTIONS] [FILE ...]
  -d    number of digits (default 5)
  -e    omit elevation info
  -h    show this help
  -n    remove nearby points (default disabled)
  -p    precision in meters (default 2.0 m)
  -q    quiet
  -s    remove spikes (default disabled)
```

Example
```
$ gpxding *.gpx
Andorra_la_Vella_Quillan.gpx => Andorra_la_Vella_Quillan.gpx.gpx
   18761 =>     2087 (11.12%) trackpoints
 5450286 =>   122161 (2.24%) bytes
Cierp_Andorra_la_Vella.gpx => Cierp_Andorra_la_Vella.gpx.gpx
   30443 =>     3054 (10.03%) trackpoints
 8839475 =>   177989 (2.01%) bytes
```

### Features
* typically a GPX file will be reduced to about 10% of the original size, without losing relevant information for navigation
* reducing the number of track points using Ramer-Douglas-Peucker algorithm
* reducing longitude/latitude to a sensible amount of digits (preserving relevant precision), e.g. 14.9999997348 becomes 15.
* rounding of elevation to the nearest meter
* deduplicate nearby points, e.g. recording wasn't stopped during a time-out
* reducing noise/spikes (optional)
* very, very fast (e.g. much faster than [gpsbabel](https://www.gpsbabel.org/))
* process multiple files with a single command (wildcard support)
* support for GPX files of unlimited sizes and number of track points (restricted only by memory)

### Verified compatibility
* [Bryton Active](https://play.google.com/store/apps/details?id=com.brytonsport.active)
* [Garmin Connect](https://play.google.com/store/apps/details?id=com.garmin.android.apps.connectmobile)
* [gpx.studio](https://gpx.studio)
* [Strava](https://strava.com)
* [Suunto App](https://play.google.com/store/apps/details?id=com.stt.android.suunto)

### Caveats
   * in case of a multi tracks gpx file, all tracks will be minified and merged into a single track

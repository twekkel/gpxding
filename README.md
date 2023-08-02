**gpxding** (_ding_ translates into _thing_ in Englsh) is a utitily to minify gpx files by removing redundant information. Only (a reduced number of) trackpoints and elevation (optional) information is retained, while all other excess information is discarded. File size will be drastically reduced, while still accurate and useful for route navigation.

Goal of **gpxding** is to have the smallest possible gpx file for route navigation.

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
gpxding version v0.0.3
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
Cierp_Andorra_la_Vella.gpx => Cierp_Andorra_la_Vella.gpx.gpx
  30443 => 3054 (10.03%) trackpoints
  8839475 => 183562 (2.08%) bytes
Andorra_la_Vella_Quillan.gpx => Andorra_la_Vella_Quillan.gpx.gpx
  18761 => 2088 (11.13%) trackpoints
  5450286 => 125983 (2.31%) bytes
```

### Features
* rounding/reducting to a sensible signification number of digits (preserving relevant precision), e.g. an elevation of 14.9999997348 becomes 15.
* deduplicate nearby point, e.g. when recording wasn't stopped during recording
* reduced noise
* very fast (e.g. much faster than [gpsbabel](https://www.gpsbabel.org/))
* process multiple files with a single command (wildcard support)
* supports unlimited GPX file sizes / track points (constricted only by memory)
* typically a GPX file will be reduced to about 10% of the orginal size, without losing relevant information for navigation

### Verified compatibility
* [Bryton Active](https://play.google.com/store/apps/details?id=com.brytonsport.active)
* [Garmin Connect](https://play.google.com/store/apps/details?id=com.garmin.android.apps.connectmobile)
* [gpx.studio](https://gpx.studio)
* [Strava](https://strava.com)
* [Suunto App](https://play.google.com/store/apps/details?id=com.stt.android.suunto)
* ...

### Caveats
   * in case of a multi tracks gpx file, all tracks will be minified and merged into a single track

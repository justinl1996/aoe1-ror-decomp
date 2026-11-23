#ifndef AOE1_PATH_H
#define AOE1_PATH_H

#include "waypoint.h"

#include <new.h>

// VC6 iostream debugging helpers; the Linux build has no pre-standard iostream.
#ifndef AOE_LINUX
class ostream;
#endif

class Path {
  public:
    unsigned long timeStampValue;
    Waypoint startValue;
    Waypoint goalValue;
    Waypoint* pathValue;
    int maxSizeValue;
    int currentSizeValue;
    int currentWaypointValue;

    Path(int v);

    int growMaxSize(int new_maximum) {
        if (new_maximum == 0) {
            return maxSizeValue += maxSizeValue;
        } else {
            return maxSizeValue += new_maximum;
        }
    }

    void setMaxSize(int new_maximum) {
        if (new_maximum == 0 || maxSizeValue <= new_maximum) {
            if (new_maximum == 0) {
                maxSizeValue += maxSizeValue;
            } else {
                maxSizeValue += new_maximum;
            }
            Waypoint* new_path = (Waypoint*) ::operator new(maxSizeValue * 16);
            for (int i = 0; i < currentSizeValue; i++) {
                new_path[i].x = pathValue[i].x;
                new_path[i].y = pathValue[i].y;
                new_path[i].z = pathValue[i].z;
                new_path[i].facetToNextWaypoint = pathValue[i].facetToNextWaypoint;
            }
            if (pathValue != 0) {
                ::operator delete(pathValue);
            }
            pathValue = new_path;
        }
    }

    Path& operator=(const Path& p);
    int insertAtEnd(Waypoint w);
    ~Path();
    Path operator+(const Path& p) const;
    int operator==(const Path& p) const;
    int insertAtCurrent(const Waypoint& w);
    int contains(const Waypoint& w) const;
    void killPath();
    const int maxNumberOfWaypoints() const;
    const int numberOfWaypoints() const;
    unsigned long timeStamp() const;
    void setTimeStamp(unsigned long v);
    float length() const;
    const Waypoint& start() const;
    const Waypoint& goal() const;
    const Waypoint* waypoint(int w) const;
    Waypoint* currentWaypoint() const;
    const int currentWaypointNumber() const;
    void setCurrentWaypointNumber(int v);
    const Waypoint* nextWaypoint() const;
    const Waypoint* previousWaypoint() const;
    const Waypoint* firstWaypoint() const;
    const Waypoint* lastWaypoint() const;
    void initToStart();
    void initToEnd();
    int moveToNextWaypoint();
    int moveToLastWaypoint();
    float distanceToNextWaypoint() const;
};

#if !defined(__LP64__) && !defined(_WIN64) && !defined(__wasm64__)
typedef char path_size_check[(sizeof(Path) == 0x34) ? 1 : -1];
#endif

// VC6 iostream debugging helpers; the Linux build has no pre-standard iostream.
#ifndef AOE_LINUX
ostream& __cdecl operator<<(ostream& out, const Path& p);
#endif

#endif

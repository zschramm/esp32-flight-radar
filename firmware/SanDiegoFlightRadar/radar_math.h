#pragma once
#include <cmath>
namespace radar {
constexpr double PI_VALUE = 3.14159265358979323846;
constexpr double EARTH_MILES = 3958.7613;
constexpr double RADIUS_MILES = 10.0;
struct Offset { double miles, east, north; };
inline Offset project(double lat0, double lon0, double lat, double lon) {
    const double p0 = lat0 * PI_VALUE / 180, p = lat * PI_VALUE / 180;
    const double dp = p - p0, dl = (lon - lon0) * PI_VALUE / 180;
    double a = sin(dp/2)*sin(dp/2) + cos(p0)*cos(p)*sin(dl/2)*sin(dl/2);
    a = a < 0 ? 0 : (a > 1 ? 1 : a);
    const double miles = 2 * EARTH_MILES * atan2(sqrt(a), sqrt(1-a));
    const double bearing = atan2(sin(dl)*cos(p), cos(p0)*sin(p)-sin(p0)*cos(p)*cos(dl));
    return {miles, miles*sin(bearing), miles*cos(bearing)};
}
inline bool inRange(double miles) { return std::isfinite(miles) && miles <= RADIUS_MILES; }
// Reject stale callsign routes that cannot reasonably pass the reported position.
// Passing this test makes a route plausible, never verified as a live flight plan.
inline bool plausibleRoute(double lat,double lon,double aLat,double aLon,double bLat,double bLon){
    if(!std::isfinite(lat)||!std::isfinite(lon)||!std::isfinite(aLat)||!std::isfinite(aLon)||!std::isfinite(bLat)||!std::isfinite(bLon)||fabs(aLat)>90||fabs(bLat)>90||fabs(aLon)>180||fabs(bLon)>180)return false;
    const double leg=project(aLat,aLon,bLat,bLon).miles;
    const double via=project(aLat,aLon,lat,lon).miles+project(lat,lon,bLat,bLon).miles;
    const double allowance=leg*.25>60?leg*.25:60;
    return via<=leg+allowance;
}
}

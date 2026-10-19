#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <esp_display_panel.hpp>
#include <lvgl.h>
#include <time.h>
#include <algorithm>
#include <memory>
#include "lvgl_v8_port.h"
#include "radar_math.h"
#include "http_retry.h"
#if __has_include("wifi_defaults.h")
#include "wifi_defaults.h"
#else
#include "wifi_defaults.example.h"
#endif
#include "weather_display.h"
#include "coastline.h"
#include "airports.h"
#include "airline_logos.h"

#if !defined(CONFIG_SPIRAM_FETCH_INSTRUCTIONS) || !defined(CONFIG_SPIRAM_RODATA)
#error "Use Espressif 3.2.0-h PSRAM-XIP SDK; see setup-sdk.ps1. RGB bounce buffers require cache-safe flash writes."
#endif

using namespace esp_panel::board;
using namespace esp_panel::drivers;
extern const uint8_t x509_crt_bundle_start[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t x509_crt_bundle_end[] asm("_binary_x509_crt_bundle_end");

namespace {
constexpr int MAX_PLANES = 256, CX = 550, CY = 240, R = 240;
constexpr uint32_t POLL_MS = 10000, MAX_POSITION_AGE_MS = 45000;
constexpr uint32_t BG = 0x09131E, PANEL = 0x101F2C, GRID = 0x234050;
constexpr uint32_t WHITE = 0xEEF6FA, MUTED = 0xA4B7C5, CYAN = 0x5BE2DB, AMBER = 0xFFC16D;
struct Plane {
    char hex[10]{}, callsign[16]{}, registration[16]{}, type[12]{};
    float altitude = NAN, knots = NAN, track = NAN, seen = 0;
    double miles = 0, east = 0, north = 0, latitude = 0, longitude = 0;
};
struct Route {
    char callsign[16]{}, flight[16]{}, origin[8]{}, destination[8]{};
    char originCity[40]{}, destinationCity[40]{}, airline[48]{};
    char airlineIATA[4]{},airlineICAO[5]{};
    uint32_t fetched = 0;
    bool found = false;
};
int getJSON(const String &url,JsonDocument &doc,uint32_t &retrySeconds);
Board *board;
Preferences prefs;
String ssid, wifiPassword;
double centerLat = 32.7157, centerLon = -117.1611;
bool usbSetup = false, followNearest = true;
volatile bool setupRequested = false, resumeRequested = false;
uint32_t lastGood = 0, nextPoll = 0, pollDelay = POLL_MS, nextRoute = 0;
uint64_t feedRetryUntil=0;
uint32_t feedRetryRemaining(){const int64_t now=time(nullptr);return now>0 && feedRetryUntil>uint64_t(now)?feed_retry::seconds(feedRetryUntil-now):0;}
void saveFeedRetry(uint64_t until){
    feedRetryUntil=until;
    Preferences storage;
    // Separate from Wi-Fi/location and specific to this provider. Reboots and
    // GUI updates must not bypass a provider's explicit rate-limit cooldown.
    if(storage.begin("radar-feed-fi",false)){storage.putULong64("retry-until",until);storage.end();}
}
bool haveData = false;
char networkState[64] = "Connecting to Wi-Fi";
Plane planes[MAX_PLANES];
int planeCount = 0;
char selectedHex[10]{};
Route routes[24];
int routeSlot = 0;
lv_obj_t *root, *radarArea, *statusLabel, *countLabel, *flightLabel, *airlineLabel;
lv_obj_t *originLabel, *destinationLabel, *originCityLabel, *destinationCityLabel;
lv_obj_t *altLabel, *speedLabel, *typeLabel, *registrationLabel, *distanceLabel, *routeNote;
lv_obj_t *cycleLabel, *emptyLabel, *setupOverlay = nullptr;
lv_obj_t *logoPlate,*logoImage,*innerRangeLabel,*outerRangeLabel,*radarPlus,*radarMinus;
constexpr double radarRanges[]={2.5,5,10,20,40};
constexpr int LAST_RADAR_ZOOM=sizeof(radarRanges)/sizeof(radarRanges[0])-1;
int radarZoom=2;
double viewMiles(){return radarRanges[radarZoom];}
bool visibleOnRadar(const Plane &p){double x=CX+p.east/viewMiles()*R,y=CY-p.north/viewMiles()*R;return x>=301&&x<=799&&y>=0&&y<=479;}
bool clipMap(double &x1,double &y1,double &x2,double &y2){
    const double dx=x2-x1,dy=y2-y1,p[]={-dx,dx,-dy,dy},q[]={x1-301,799-x1,y1,479-y1};double low=0,high=1;
    for(int i=0;i<4;i++){if(fabs(p[i])<1e-9){if(q[i]<0)return false;}else{double r=q[i]/p[i];if(p[i]<0)low=std::max(low,r);else high=std::min(high,r);if(low>high)return false;}}
    x2=x1+high*dx;y2=y1+high*dy;x1+=low*dx;y1+=low*dy;return true;
}
// Static geography is projected and clipped only when center or zoom changes.
// Repeating spherical trig for 1,063 segments in every redraw consumed one core.
struct CachedMapLine {lv_point_t a,b;uint8_t width;};
struct CachedAirport {lv_point_t point;lv_area_t label;const char *code;bool dot,labelVisible;};
struct MapCache {
    double latitude,longitude,range;
    unsigned coastCount,runwayCount,airportCount;
    CachedMapLine coastline[sizeof(coast::segments)/sizeof(coast::segments[0])];
    CachedMapLine runways[sizeof(airports::runways)/sizeof(airports::runways[0])];
    CachedAirport airports[sizeof(airports::locations)/sizeof(airports::locations[0])];
};
MapCache *mapCache=nullptr;
MapCache *prepareMap() {
    if(!mapCache){mapCache=static_cast<MapCache*>(ps_malloc(sizeof(MapCache)));if(!mapCache)return nullptr;*mapCache={};mapCache->range=-1;}
    const double range=viewMiles();
    if(mapCache->latitude==centerLat&&mapCache->longitude==centerLon&&mapCache->range==range)return mapCache;
    mapCache->latitude=centerLat;mapCache->longitude=centerLon;mapCache->range=range;
    mapCache->coastCount=mapCache->runwayCount=mapCache->airportCount=0;
    auto projectLine=[&](double lat1,double lon1,double lat2,double lon2,CachedMapLine &out){
        const auto a=radar::project(centerLat,centerLon,lat1,lon1),b=radar::project(centerLat,centerLon,lat2,lon2);
        double x1=CX+a.east/range*R,y1=CY-a.north/range*R,x2=CX+b.east/range*R,y2=CY-b.north/range*R;
        if(!clipMap(x1,y1,x2,y2))return false;
        out.a={int16_t(lround(x1)),int16_t(lround(y1))};out.b={int16_t(lround(x2)),int16_t(lround(y2))};out.width=1;return true;
    };
    for(const auto &seg:coast::segments){auto &out=mapCache->coastline[mapCache->coastCount];if(projectLine(seg.lat1,seg.lon1,seg.lat2,seg.lon2,out))mapCache->coastCount++;}
    for(const auto &airport:airports::locations){
        const auto position=radar::project(centerLat,centerLon,airport.lat,airport.lon);
        if(fabs(position.east)>range*1.05||fabs(position.north)>range*1.02)continue;
        int x=lround(CX+position.east/range*R),y=lround(CY-position.north/range*R);double longest=0;
        for(unsigned n=0;n<airport.count;n++){const auto &r=airports::runways[airport.first+n];longest=std::max(longest,radar::project(r.lat1,r.lon1,r.lat2,r.lon2).miles/range*R);}
        if(longest>=10)for(unsigned n=0;n<airport.count;n++){
            const auto &r=airports::runways[airport.first+n];auto &out=mapCache->runways[mapCache->runwayCount];
            if(projectLine(r.lat1,r.lon1,r.lat2,r.lon2,out)){out.width=std::clamp(int(lround(r.widthFeet/5280.0/range*R)),1,5);mapCache->runwayCount++;}
        }
        auto &out=mapCache->airports[mapCache->airportCount];out.point={int16_t(x),int16_t(y)};out.dot=longest<10;out.code=airport.code;
        out.label={int16_t(x+5),int16_t(y-16),int16_t(x+39),int16_t(y-2)};out.labelVisible=out.label.x2<790&&out.label.y1>48;
        for(unsigned k=0;k<mapCache->airportCount&&out.labelVisible;k++){
            const auto &other=mapCache->airports[k];if(!other.labelVisible)continue;
            if(out.label.x1<other.label.x2+5&&out.label.x2>other.label.x1-5&&out.label.y1<other.label.y2+3&&out.label.y2>other.label.y1-3)out.labelVisible=false;
        }
        mapCache->airportCount++;
    }
    return mapCache;
}
struct Logo {char key[12]{};uint8_t *bytes=nullptr;lv_img_dsc_t image{};uint32_t loaded=0;};
Logo logos[12];int logoSlot=0;uint32_t nextLogo=0;
void showLogo(const Route *route);

template<size_t N> void copyText(char (&out)[N], const char *in) {
    // The small UI font is ASCII. Fold common Latin accents instead of drawing
    // missing-glyph boxes (for example, San Jose) or logging on every refresh.
    const auto *p=reinterpret_cast<const uint8_t*>(in?in:"");size_t written=0;
    while(*p&&written+1<N){
        uint32_t c=*p++;
        if(c>=0xC2&&c<=0xDF&&(*p&0xC0)==0x80){c=((c&31)<<6)|(*p++&63);}
        else if(c>=0xE0&&c<=0xEF&&p[0]&&(p[0]&0xC0)==0x80&&p[1]&&(p[1]&0xC0)==0x80){c=((c&15)<<12)|((p[0]&63)<<6)|(p[1]&63);p+=2;}
        char a=c>=32&&c<127?char(c):'?';
        if(c<32)a=' ';
        else if(c>=0xC0&&c<=0xC5)a='A';else if(c>=0xE0&&c<=0xE5)a='a';
        else if(c==0xC7)a='C';else if(c==0xE7)a='c';
        else if(c>=0xC8&&c<=0xCB)a='E';else if(c>=0xE8&&c<=0xEB)a='e';
        else if(c>=0xCC&&c<=0xCF)a='I';else if(c>=0xEC&&c<=0xEF)a='i';
        else if(c==0xD1)a='N';else if(c==0xF1)a='n';
        else if((c>=0xD2&&c<=0xD6)||c==0xD8)a='O';else if((c>=0xF2&&c<=0xF6)||c==0xF8)a='o';
        else if(c>=0xD9&&c<=0xDC)a='U';else if(c>=0xF9&&c<=0xFC)a='u';
        else if(c==0xDD)a='Y';else if(c==0xFD||c==0xFF)a='y';
        else if(c==0x2018||c==0x2019)a='\'';else if(c==0x2013||c==0x2014)a='-';
        out[written++]=a;
    }
    out[written]=0;
}
bool due(uint32_t now, uint32_t deadline) { return int32_t(now - deadline) >= 0; }
bool fresh(const Plane &p) { return haveData && millis() - lastGood + p.seen * 1000 < MAX_POSITION_AGE_MS; }
int selectedIndex() {
    for (int i = 0; i < planeCount; ++i) if (!strcmp(planes[i].hex, selectedHex) && fresh(planes[i])) return i;
    return -1;
}
Route *cachedRoute(const char *callsign) {
    if (!callsign[0]) return nullptr;
    for (auto &r : routes) if (!strcmp(r.callsign, callsign) && millis() - r.fetched < 30UL*60*1000) return &r;
    return nullptr;
}
const char *aircraftName(const char *code) {
    struct Type { const char *code, *name; };
    static const Type types[] = {{"B738","Boeing 737-800"},{"B739","Boeing 737-900"},
      {"B38M","Boeing 737 MAX 8"},{"B39M","Boeing 737 MAX 9"},{"B737","Boeing 737-700"},
      {"A319","Airbus A319"},{"A320","Airbus A320"},{"A321","Airbus A321"},
      {"A20N","Airbus A320neo"},{"A21N","Airbus A321neo"},{"BCS3","Airbus A220-300"},
      {"BCS1","Airbus A220-100"},{"B788","Boeing 787-8"},{"B789","Boeing 787-9"},
      {"B78X","Boeing 787-10"},{"E75L","Embraer 175"},{"E75S","Embraer 175"},
      {"C172","Cessna 172"},{"C182","Cessna 182"},{"SR22","Cirrus SR22"}};
    for (const auto &t : types) if (!strcmp(code,t.code)) return t.name;
    return code[0] ? code : "Unknown";
}
lv_obj_t *label(int x, int y, int width, const char *text, const lv_font_t *font, uint32_t color, lv_obj_t *parent = nullptr) {
    auto obj = lv_label_create(parent ? parent : root);
    lv_obj_set_pos(obj,x,y); lv_obj_set_size(obj,width,font->line_height);
    lv_obj_set_style_text_font(obj,font,0); lv_obj_set_style_text_color(obj,lv_color_hex(color),0);
    lv_label_set_long_mode(obj,LV_LABEL_LONG_DOT); lv_label_set_text(obj,text);
    return obj;
}
void rectangle(int x,int y,int w,int h,uint32_t color) {
    auto obj=lv_obj_create(root); lv_obj_remove_style_all(obj); lv_obj_set_pos(obj,x,y); lv_obj_set_size(obj,w,h);
    lv_obj_set_style_bg_color(obj,lv_color_hex(color),0); lv_obj_set_style_bg_opa(obj,LV_OPA_COVER,0);
    lv_obj_clear_flag(obj,LV_OBJ_FLAG_CLICKABLE);
}
void chooseNearest() {
    selectedHex[0]='\0';
    // The feed is sorted by distance. Manual selection remains until it expires.
    for(int i=0;i<planeCount;i++) if(fresh(planes[i])) {copyText(selectedHex,planes[i].hex);break;}
}
void details() {
    int i=selectedIndex();
    if(i<0) {
        showLogo(nullptr);
        lv_label_set_text(flightLabel,"No aircraft"); lv_label_set_text(airlineLabel,"Waiting for nearby traffic");
        for(auto l : {originLabel,destinationLabel,altLabel,speedLabel}) lv_label_set_text(l,"--");
        for(auto l : {originCityLabel,destinationCityLabel,typeLabel,registrationLabel,distanceLabel,routeNote}) lv_label_set_text(l,"");
        return;
    }
    const Plane &p=planes[i]; Route *r=cachedRoute(p.callsign);
    showLogo(r);
    lv_label_set_text(flightLabel,r && r->flight[0] ? r->flight : (p.callsign[0] ? p.callsign : p.registration[0] ? p.registration : p.hex));
    lv_label_set_text(airlineLabel,r && r->airline[0] ? r->airline : (p.callsign[0] ? p.callsign : "Callsign unavailable"));
    lv_label_set_text(originLabel,r && r->found ? r->origin : "---");
    lv_label_set_text(destinationLabel,r && r->found ? r->destination : "---");
    lv_label_set_text(originCityLabel,r && r->found ? r->originCity : "Unverified");
    lv_label_set_text(destinationCityLabel,r && r->found ? r->destinationCity : "Unverified");
    lv_label_set_text(routeNote,r && r->found ? "Estimated callsign route" : "Route unverified");
    char b[64];
    if(std::isfinite(p.altitude)) snprintf(b,sizeof(b),"%.0f ft",p.altitude); else strlcpy(b,"-- ft",sizeof(b));
    lv_label_set_text(altLabel,b);
    if(std::isfinite(p.knots)) snprintf(b,sizeof(b),"%.0f mph",p.knots*1.15077945); else strlcpy(b,"-- mph",sizeof(b));
    lv_label_set_text(speedLabel,b);
    lv_label_set_text(typeLabel,aircraftName(p.type)); lv_label_set_text(registrationLabel,p.registration);
    snprintf(b,sizeof(b),"%.1f mi from center",p.miles); lv_label_set_text(distanceLabel,b);
}
void drawRadar(lv_event_t *event) {
    auto ctx=lv_event_get_draw_ctx(event);
    lv_draw_line_dsc_t line; lv_draw_line_dsc_init(&line); line.width=2;
    const double range=viewMiles();
    if(const auto *map=prepareMap()){
        line.width=1;line.color=lv_color_hex(0x42616D);
        for(unsigned n=0;n<map->coastCount;n++)lv_draw_line(ctx,&line,&map->coastline[n].a,&map->coastline[n].b);
        line.color=lv_color_hex(0x829CAB);
        for(unsigned n=0;n<map->runwayCount;n++){line.width=map->runways[n].width;lv_draw_line(ctx,&line,&map->runways[n].a,&map->runways[n].b);}
        for(unsigned n=0;n<map->airportCount;n++){
            const auto &a=map->airports[n];
            if(a.dot){lv_draw_rect_dsc_t d;lv_draw_rect_dsc_init(&d);d.bg_color=lv_color_hex(0x829CAB);d.bg_opa=LV_OPA_COVER;d.radius=LV_RADIUS_CIRCLE;lv_area_t dot{int16_t(a.point.x-2),int16_t(a.point.y-2),int16_t(a.point.x+2),int16_t(a.point.y+2)};lv_draw_rect(ctx,&d,&dot);}
            if(a.labelVisible){lv_draw_label_dsc_t t;lv_draw_label_dsc_init(&t);t.font=&lv_font_montserrat_12;t.color=lv_color_hex(MUTED);lv_draw_label(ctx,&t,&a.label,a.code,nullptr);}
        }
    }
    for(int i=0;i<planeCount;i++) {
        const Plane &p=planes[i]; if(!fresh(p)||!visibleOnRadar(p)) continue;
        const float x=CX+p.east/range*R, y=CY-p.north/range*R;
        const bool selected=!strcmp(p.hex,selectedHex);
        const auto color=lv_color_hex(selected ? AMBER : CYAN);
        const float angle=std::isfinite(p.track) ? p.track*radar::PI_VALUE/180 : 0;
        const float fx=sin(angle),fy=-cos(angle),rx=cos(angle),ry=sin(angle);
        const float iconScale=selected?1.0f:range>=80?.42f:range>=40?.60f:range>=20?.8f:1.0f;
        auto point=[&](float forward,float right) { return lv_point_t{int16_t(lround(x+(fx*forward+rx*right)*iconScale)),int16_t(lround(y+(fy*forward+ry*right)*iconScale))}; };
        line.color=color; line.width=selected ? 3 : range>=40 ? 1 : 2;
        if(std::isfinite(p.track)) {
            // Fuselage, swept wings, tail: heading is the reported ground track.
            auto a=point(11,0),b=point(-9,0); lv_draw_line(ctx,&line,&a,&b);
            a=point(3,0); b=point(-3,-9); lv_draw_line(ctx,&line,&a,&b);
            b=point(-3,9); lv_draw_line(ctx,&line,&a,&b);
            a=point(-6,0); b=point(-9,-4); lv_draw_line(ctx,&line,&a,&b);
            b=point(-9,4); lv_draw_line(ctx,&line,&a,&b);
        } else {
            lv_point_t a{int16_t(x),int16_t(y-6)},b{int16_t(x+6),int16_t(y)},c{int16_t(x),int16_t(y+6)},d{int16_t(x-6),int16_t(y)};
            lv_draw_line(ctx,&line,&a,&b);lv_draw_line(ctx,&line,&b,&c);lv_draw_line(ctx,&line,&c,&d);lv_draw_line(ctx,&line,&d,&a);
        }
        if(selected) {
            lv_draw_rect_dsc_t d;lv_draw_rect_dsc_init(&d);d.bg_opa=LV_OPA_TRANSP;d.radius=LV_RADIUS_CIRCLE;d.border_width=1;d.border_color=color;
            lv_area_t a{int16_t(x-17),int16_t(y-17),int16_t(x+17),int16_t(y+17)};lv_draw_rect(ctx,&d,&a);
            lv_draw_label_dsc_t t;lv_draw_label_dsc_init(&t);t.font=&lv_font_montserrat_14;t.color=color;
            int tx=std::clamp(int(x-45),310,690),ty= y>CY+R-40 ? int(y-36) : int(y+20);
            lv_area_t ta{int16_t(tx),int16_t(ty),int16_t(tx+95),int16_t(ty+18)};
            lv_draw_label(ctx,&t,&ta,p.callsign[0]?p.callsign:p.hex,nullptr);
        }
    }
}
void refreshUI(lv_timer_t *) {
    if(usbSetup) return;
    if(selectedIndex()<0) followNearest=true;
    if(followNearest) chooseNearest();
    int count=0;for(int i=0;i<planeCount;i++) if(fresh(planes[i])&&visibleOnRadar(planes[i])) count++;
    char b[100]; snprintf(b,sizeof(b),"%d shown / %g mi ring",count,viewMiles()); lv_label_set_text(countLabel,b);
    uint32_t wait=feedRetryRemaining();
    if(wait>=3600) snprintf(b,sizeof(b),"adsb.fi limit / retry in %luh %lum",(unsigned long)(wait/3600),(unsigned long)((wait%3600)/60));
    else if(wait) snprintf(b,sizeof(b),"adsb.fi limit / retry in %lum %lus",(unsigned long)(wait/60),(unsigned long)(wait%60));
    else if(haveData) snprintf(b,sizeof(b),"%s  /  %lus ago",networkState,(unsigned long)((millis()-lastGood)/1000));
    else strlcpy(b,networkState,sizeof(b));
    lv_label_set_text(statusLabel,b);
    lv_obj_set_style_text_color(statusLabel,lv_color_hex(!wait && haveData && millis()-lastGood<30000 && WiFi.status()==WL_CONNECTED ? CYAN : AMBER),0);
    lv_label_set_text(cycleLabel,followNearest ? "NEAREST ON" : "NEAREST");
    if(count) lv_obj_add_flag(emptyLabel,LV_OBJ_FLAG_HIDDEN); else {lv_obj_clear_flag(emptyLabel,LV_OBJ_FLAG_HIDDEN);lv_label_set_text(emptyLabel,haveData && millis()-lastGood<30000 ? "No airborne aircraft in this view" : "Waiting for fresh aircraft positions");}
    details(); lv_obj_invalidate(radarArea);weather::tick();
}
void changeRadarZoom(int delta){
    // Redraw immediately; fetch the new extent at the next permitted poll.
    // Touch zoom must never bypass normal pacing or an HTTP 429 cooldown.
    radarZoom=std::clamp(radarZoom-delta,0,LAST_RADAR_ZOOM);char b[24];snprintf(b,sizeof(b),"%g mi",viewMiles()/2);lv_label_set_text(innerRangeLabel,b);snprintf(b,sizeof(b),"%g mi",viewMiles());lv_label_set_text(outerRangeLabel,b);
    if(radarZoom==LAST_RADAR_ZOOM)lv_obj_add_state(radarMinus,LV_STATE_DISABLED);else lv_obj_clear_state(radarMinus,LV_STATE_DISABLED);
    if(radarZoom==0)lv_obj_add_state(radarPlus,LV_STATE_DISABLED);else lv_obj_clear_state(radarPlus,LV_STATE_DISABLED);refreshUI(nullptr);
}
void buildUI() {
    root=lv_scr_act();lv_obj_set_style_bg_color(root,lv_color_hex(BG),0);lv_obj_clear_flag(root,LV_OBJ_FLAG_SCROLLABLE);
    // Keep LVGL's diagnostic overlay off the finished device interface.
    lv_obj_add_flag(lv_layer_sys(),LV_OBJ_FLAG_HIDDEN);
    rectangle(0,0,300,480,PANEL); rectangle(300,0,1,480,GRID);
    label(16,12,270,"SAN DIEGO / FLIGHT RADAR",&lv_font_montserrat_14,CYAN);
    flightLabel=label(16,36,160,"Starting...",&lv_font_montserrat_28,WHITE);
    logoPlate=lv_obj_create(root);lv_obj_remove_style_all(logoPlate);lv_obj_set_pos(logoPlate,180,35);lv_obj_set_size(logoPlate,110,32);
    lv_obj_set_style_bg_color(logoPlate,lv_color_hex(0xFFFFFF),0);lv_obj_set_style_bg_opa(logoPlate,LV_OPA_COVER,0);lv_obj_set_style_radius(logoPlate,4,0);lv_obj_clear_flag(logoPlate,LV_OBJ_FLAG_SCROLLABLE);
    logoImage=lv_img_create(logoPlate);lv_img_set_pivot(logoImage,0,0);lv_obj_add_flag(logoPlate,LV_OBJ_FLAG_HIDDEN);
    airlineLabel=label(16,70,270,"",&lv_font_montserrat_14,MUTED);
    label(16,100,130,"ORIGIN",&lv_font_montserrat_12,MUTED);
    label(156,100,130,"DESTINATION",&lv_font_montserrat_12,MUTED);
    originLabel=label(16,119,130,"---",&lv_font_montserrat_24,WHITE);
    destinationLabel=label(156,119,130,"---",&lv_font_montserrat_24,WHITE);
    originCityLabel=label(16,150,130,"",&lv_font_montserrat_14,MUTED);
    destinationCityLabel=label(156,150,130,"",&lv_font_montserrat_14,MUTED);
    routeNote=label(16,173,270,"",&lv_font_montserrat_12,MUTED);
    rectangle(16,195,270,1,GRID);
    label(16,206,130,"ALTITUDE",&lv_font_montserrat_12,MUTED);
    label(156,206,130,"GROUND SPEED",&lv_font_montserrat_12,MUTED);
    altLabel=label(16,225,140,"-- ft",&lv_font_montserrat_20,WHITE);
    speedLabel=label(156,225,138,"-- mph",&lv_font_montserrat_20,WHITE);
    typeLabel=label(16,258,270,"",&lv_font_montserrat_16,WHITE);
    registrationLabel=label(16,283,112,"",&lv_font_montserrat_12,MUTED);
    distanceLabel=label(135,283,152,"",&lv_font_montserrat_12,CYAN);
    rectangle(16,304,270,1,GRID);
    auto button=[&](int x,int w,const char *s,lv_event_cb_t cb){auto o=lv_btn_create(root);lv_obj_set_pos(o,x,437);lv_obj_set_size(o,w,34);lv_obj_set_style_bg_color(o,lv_color_hex(GRID),0);lv_obj_set_style_radius(o,6,0);lv_obj_add_event_cb(o,cb,LV_EVENT_CLICKED,nullptr);auto l=lv_label_create(o);lv_obj_set_style_text_font(l,&lv_font_montserrat_12,0);lv_label_set_text(l,s);lv_obj_center(l);return l;};
    cycleLabel=button(10,100,"NEAREST ON",[](lv_event_t*){followNearest=true;refreshUI(nullptr);});
    button(222,68,"SETUP",[](lv_event_t*){setupRequested=true;});
    countLabel=label(310,10,270,"10-mile radius",&lv_font_montserrat_16,WHITE);

    statusLabel=label(310,32,480,"Starting",&lv_font_montserrat_12,CYAN);
    for(int radius : {R/2,R}) {
        auto circle=lv_obj_create(root);lv_obj_remove_style_all(circle);lv_obj_set_pos(circle,CX-radius,CY-radius);lv_obj_set_size(circle,2*radius,2*radius);lv_obj_set_style_radius(circle,LV_RADIUS_CIRCLE,0);lv_obj_set_style_border_color(circle,lv_color_hex(GRID),0);lv_obj_set_style_border_width(circle,1,0);lv_obj_clear_flag(circle,LV_OBJ_FLAG_CLICKABLE);
    }
    rectangle(301,CY,499,1,GRID);rectangle(CX,0,1,480,GRID);
    auto dot=lv_obj_create(root);lv_obj_remove_style_all(dot);lv_obj_set_pos(dot,CX-4,CY-4);lv_obj_set_size(dot,9,9);lv_obj_set_style_radius(dot,LV_RADIUS_CIRCLE,0);lv_obj_set_style_bg_color(dot,lv_color_hex(WHITE),0);lv_obj_set_style_bg_opa(dot,LV_OPA_COVER,0);lv_obj_clear_flag(dot,LV_OBJ_FLAG_CLICKABLE);
    label(CX-5,48,20,"N",&lv_font_montserrat_14,MUTED);
    label(306,CY-8,20,"W",&lv_font_montserrat_14,MUTED);
    label(774,CY-8,20,"E",&lv_font_montserrat_14,MUTED);
    innerRangeLabel=label(CX+9,CY+R/2-18,60,"5 mi",&lv_font_montserrat_14,MUTED);
    outerRangeLabel=label(CX+9,CY+R-18,60,"10 mi",&lv_font_montserrat_14,MUTED);
    radarArea=lv_obj_create(root);lv_obj_remove_style_all(radarArea);lv_obj_set_pos(radarArea,301,0);lv_obj_set_size(radarArea,499,480);lv_obj_clear_flag(radarArea,LV_OBJ_FLAG_SCROLLABLE);lv_obj_add_flag(radarArea,LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(radarArea,drawRadar,LV_EVENT_DRAW_MAIN,nullptr);
    lv_obj_add_event_cb(radarArea,[](lv_event_t*){
        lv_point_t p;lv_indev_get_point(lv_indev_get_act(),&p);float best=30*30;int target=-1;
        for(int i=0;i<planeCount;i++){if(!fresh(planes[i])||!visibleOnRadar(planes[i]))continue;float dx=p.x-(CX+planes[i].east/viewMiles()*R),dy=p.y-(CY-planes[i].north/viewMiles()*R);float d=dx*dx+dy*dy;if(d<best){best=d;target=i;}}
        if(target>=0){copyText(selectedHex,planes[target].hex);followNearest=false;refreshUI(nullptr);}
    },LV_EVENT_CLICKED,nullptr);
    emptyLabel=label(330,284,440,"Waiting for aircraft positions",&lv_font_montserrat_16,MUTED);
    lv_obj_set_style_text_align(emptyLabel,LV_TEXT_ALIGN_CENTER,0);
    for(auto hud:{countLabel,statusLabel}){lv_obj_move_foreground(hud);lv_obj_set_style_bg_color(hud,lv_color_hex(BG),0);lv_obj_set_style_bg_opa(hud,210,0);}
    lv_obj_set_width(statusLabel,324);
    auto control=[&](int x,const char *s,lv_event_cb_t cb){auto b=lv_btn_create(root);lv_obj_set_pos(b,x,3);lv_obj_set_size(b,42,34);lv_obj_set_style_bg_color(b,lv_color_hex(GRID),0);lv_obj_set_style_radius(b,5,0);lv_obj_set_style_pad_all(b,0,0);lv_obj_add_event_cb(b,cb,LV_EVENT_CLICKED,nullptr);auto l=lv_label_create(b);lv_label_set_text(l,s);lv_obj_center(l);return b;};
    radarMinus=control(646,"-",[](lv_event_t*){changeRadarZoom(-1);});radarPlus=control(694,"+",[](lv_event_t*){changeRadarZoom(1);});control(746,"i",[](lv_event_t*){weather::info();});
    weather::init(root,&centerLat,&centerLon,getJSON);
    lv_timer_create(refreshUI,1000,nullptr);
}

// Configuration lives in NVS, outside both application partitions. Keep the
// original keys readable so updates never require re-entering existing Wi-Fi.
bool validConfig(JsonDocument &doc) {
    if(!doc["ssid"].is<const char*>() || !doc["password"].is<const char*>() ||
       !doc["lat"].is<double>() || !doc["lon"].is<double>()) return false;
    String network=doc["ssid"].as<String>(), password=doc["password"].as<String>();
    if(network.length()<1 || network.length()>32) return false;
    bool rawKey=password.length()==64;
    if(rawKey) for(char c:password) if(!isxdigit(static_cast<unsigned char>(c))) return false;
    if(!password.isEmpty() && !rawKey && (password.length()<8 || password.length()>63)) return false;
    double lat=doc["lat"],lon=doc["lon"];
    return std::isfinite(lat) && std::isfinite(lon) && fabs(lat)<=90 && fabs(lon)<=180;
}
void loadConfig() {
    const bool opened=prefs.begin("flight-radar",true);
    // New saves are a single NVS value, preventing mixed fields after power loss.
    JsonDocument doc;String saved=opened?prefs.getString("config",""):"";
    unsigned presetRevision=0;
    if(!saved.isEmpty() && !deserializeJson(doc,saved) && validConfig(doc)) {
        ssid=doc["ssid"].as<String>();wifiPassword=doc["password"].as<String>();
        centerLat=doc["lat"];centerLon=doc["lon"];
        presetRevision=doc["preset_revision"]|0U;
    } else if(opened) {
        ssid=prefs.getString("ssid","");wifiPassword=prefs.getString("password","");
        centerLat=prefs.getDouble("lat",32.7157);centerLon=prefs.getDouble("lon",-117.1611);
    }
    if(opened) prefs.end();
    // This revision applies the explicitly requested network and intersection
    // once. Later GUI updates read the saved configuration without changing it.
    if(presetRevision<wifi_defaults::REVISION) {
        ssid=wifi_defaults::SSID;wifiPassword=wifi_defaults::PASSWORD;
        centerLat=wifi_defaults::LATITUDE;centerLon=wifi_defaults::LONGITUDE;
        JsonDocument initial;initial["ssid"]=ssid;initial["password"]=wifiPassword;
        initial["lat"]=centerLat;initial["lon"]=centerLon;initial["preset_revision"]=wifi_defaults::REVISION;
        String encoded;serializeJson(initial,encoded);
        bool persisted=false;
        if(prefs.begin("flight-radar",false)) {persisted=prefs.putString("config",encoded)==encoded.length();prefs.end();}
        Serial.println(persisted?"RADAR: requested network and location saved to NVS":"RADAR: using built-in settings; NVS save failed");
    } else Serial.println("RADAR: using retained NVS settings");
}
void saveUSBConfig(const String &payload) {
    JsonDocument doc;
    if(deserializeJson(doc,payload) || !validConfig(doc)) {
        Serial.println("CONFIG_ERROR Check network name, Wi-Fi password and coordinates.");return;
    }
    // Save only the supported fields, in one atomic NVS entry. Never echo secrets.
    JsonDocument saved;
    saved["ssid"]=doc["ssid"];saved["password"]=doc["password"];
    saved["lat"]=doc["lat"];saved["lon"]=doc["lon"];
    saved["preset_revision"]=wifi_defaults::REVISION;
    String encoded;serializeJson(saved,encoded);
    if(!prefs.begin("flight-radar",false)) {Serial.println("CONFIG_ERROR Cannot open settings storage.");return;}
    const bool ok=prefs.putString("config",encoded)==encoded.length();prefs.end();
    if(!ok) {Serial.println("CONFIG_ERROR Settings could not be saved. Existing settings retained.");return;}
    Serial.println("CONFIG_OK Saved. Restarting and connecting to Wi-Fi.");Serial.flush();delay(300);ESP.restart();
}
void beginUSBSetup() {
    setupRequested=false;usbSetup=true;WiFi.disconnect();WiFi.mode(WIFI_OFF);
    lvgl_port_lock(-1);
    setupOverlay=lv_obj_create(root);lv_obj_set_pos(setupOverlay,34,28);lv_obj_set_size(setupOverlay,732,424);
    lv_obj_set_style_bg_color(setupOverlay,lv_color_hex(PANEL),0);lv_obj_set_style_border_color(setupOverlay,lv_color_hex(CYAN),0);lv_obj_set_style_pad_all(setupOverlay,0,0);lv_obj_clear_flag(setupOverlay,LV_OBJ_FLAG_SCROLLABLE);
    label(28,22,675,"SET UP OVER USB",&lv_font_montserrat_28,WHITE,setupOverlay);
    label(28,88,675,"1. Keep the USB cable connected to your computer.",&lv_font_montserrat_20,MUTED,setupOverlay);
    label(28,140,675,"2. Open Flight Radar Setup on your computer.",&lv_font_montserrat_20,WHITE,setupOverlay);
    label(28,192,675,"3. Enter your home Wi-Fi and choose Save.",&lv_font_montserrat_20,MUTED,setupOverlay);
    label(28,252,675,"San Diego / 10-mile radar",&lv_font_montserrat_24,CYAN,setupOverlay);
    label(28,303,675,"Use a 2.4 GHz network. Settings survive updates.",&lv_font_montserrat_16,MUTED,setupOverlay);
    label(28,347,510,"No phone or extra Wi-Fi connection needed.",&lv_font_montserrat_16,MUTED,setupOverlay);
    auto back=lv_btn_create(setupOverlay);lv_obj_set_pos(back,570,337);lv_obj_set_size(back,130,46);
    lv_obj_set_style_bg_color(back,lv_color_hex(GRID),0);
    lv_obj_add_event_cb(back,[](lv_event_t*){resumeRequested=true;},LV_EVENT_CLICKED,nullptr);
    auto backLabel=lv_label_create(back);lv_label_set_text(backLabel,"BACK");lv_obj_center(backLabel);
    lvgl_port_unlock();Serial.println("RADAR: USB setup ready");
}

void setState(const char *s) { lvgl_port_lock(-1);copyText(networkState,s);lvgl_port_unlock(); }
class JSONBody : public Stream {
public:
    String text;
    static constexpr size_t LIMIT=128*1024;
    int available() override {return 0;}
    int read() override {return -1;}
    int peek() override {return -1;}
    void flush() override {}
    size_t write(uint8_t byte) override {return write(&byte,1);}
    size_t write(const uint8_t *data,size_t length) override {
        if(length>LIMIT-text.length()) return 0;
        return text.concat(reinterpret_cast<const char*>(data),length)?length:0;
    }
};
int getJSON(const String &url,JsonDocument &doc,uint32_t &retrySeconds) {
    WiFiClientSecure client;client.setCACertBundle(x509_crt_bundle_start,x509_crt_bundle_end-x509_crt_bundle_start);client.setHandshakeTimeout(8);
    HTTPClient http;http.setConnectTimeout(8000);http.setTimeout(8000);http.useHTTP10(false);http.setReuse(false);
    http.setUserAgent("SanDiegoFlightRadar/1.5 (personal ESP32 display)");
    if(!http.begin(client,url))return -1;
    http.addHeader("Accept-Encoding","identity");
    const char *headers[]={"Retry-After","X-RateLimit-Reset"};http.collectHeaders(headers,2);
    int code=http.GET();
    if(code==200){
        // HTTP/1.1 is required by the feed. HTTPClient decodes chunked transfer
        // before JSON parsing, and the bounded sink limits memory use.
        JSONBody body;
        if(http.getSize()>int(JSONBody::LIMIT) || http.writeToStream(&body)<0)code=-3;
        else if(deserializeJson(doc,body.text))code=-2;
    }
    if(code==429){long wait=http.header("Retry-After").toInt();if(wait<=0)wait=http.header("X-RateLimit-Reset").toInt();retrySeconds=wait>0?std::min(wait,86400L):120;}
    http.end();return code;
}
void showLogo(const Route *route) {
    if(!logoPlate)return;
    auto fitLogo=[](const lv_img_dsc_t *image){
        lv_img_set_src(logoImage,image);
        // set_src restores a center pivot; reset it after every source change.
        lv_img_set_pivot(logoImage,0,0);lv_img_set_antialias(logoImage,true);
        int zoom=std::min({256,102*256/int(image->header.w),24*256/int(image->header.h)});
        lv_img_set_zoom(logoImage,zoom);lv_obj_set_pos(logoImage,(110-int(image->header.w)*zoom/256)/2,(32-int(image->header.h)*zoom/256)/2);
        lv_obj_clear_flag(logoPlate,LV_OBJ_FLAG_HIDDEN);
    };
    if(route&&route->airlineIATA[0]&&route->airlineICAO[0]){
        String key=String(route->airlineIATA)+"_"+route->airlineICAO;
        static lv_img_dsc_t bundled{};
        for(const auto &asset:airline_logos::entries)if(key==asset.key){
            if(bundled.data!=asset.data){lv_img_cache_invalidate_src(&bundled);bundled={};bundled.header.cf=LV_IMG_CF_RAW_ALPHA;bundled.header.w=asset.width;bundled.header.h=asset.height;bundled.data=asset.data;bundled.data_size=asset.size;}
            fitLogo(&bundled);return;
        }
        for(auto &logo:logos)if(key==logo.key&&logo.bytes){
            fitLogo(&logo.image);return;
        }
    }
    lv_obj_add_flag(logoPlate,LV_OBJ_FLAG_HIDDEN);
}
void fetchSelectedLogo() {
    if(!due(millis(),nextLogo))return;nextLogo=millis()+5000;
    char key[12]{};
    lvgl_port_lock(-1);int i=selectedIndex();Route *route=i>=0?cachedRoute(planes[i].callsign):nullptr;
    if(route&&strlen(route->airlineIATA)==2&&strlen(route->airlineICAO)==3)snprintf(key,sizeof(key),"%s_%s",route->airlineIATA,route->airlineICAO);
    lvgl_port_unlock();if(!key[0])return;
    for(const auto &asset:airline_logos::entries)if(!strcmp(asset.key,key))return;
    for(int j=0;key[j];j++)if(!isalnum(static_cast<unsigned char>(key[j]))&&key[j]!='_')return;
    for(const auto &logo:logos)if(!strcmp(logo.key,key)&&millis()-logo.loaded<86400000)return;
    WiFiClientSecure client;client.setCACertBundle(x509_crt_bundle_start,x509_crt_bundle_end-x509_crt_bundle_start);client.setHandshakeTimeout(8);
    HTTPClient http;http.setConnectTimeout(8000);http.setTimeout(8000);http.useHTTP10(false);http.setReuse(false);
    if(!http.begin(client,String("https://images.flightradar24.com/assets/airlines/logotypes/")+key+".png"))return;
    JSONBody body;int code=http.GET();bool valid=code==200&&http.getSize()<=32768&&http.writeToStream(&body)>=0;http.end();
    const auto *bytes=reinterpret_cast<const uint8_t*>(body.text.c_str());const uint8_t signature[]={137,80,78,71,13,10,26,10};
    valid=valid&&body.text.length()>=24&&!memcmp(bytes,signature,8);unsigned width=0,height=0;
    if(valid){for(int n=16;n<20;n++)width=(width<<8)|bytes[n];for(int n=20;n<24;n++)height=(height<<8)|bytes[n];valid=width>0&&width<=300&&height>0&&height<=100;}
    uint8_t *copy=valid?static_cast<uint8_t*>(ps_malloc(body.text.length())):nullptr;if(copy)memcpy(copy,bytes,body.text.length());
    lvgl_port_lock(-1);auto &logo=logos[logoSlot++%12];
    if(lv_img_get_src(logoImage)==&logo.image){lv_img_set_src(logoImage,nullptr);lv_obj_add_flag(logoPlate,LV_OBJ_FLAG_HIDDEN);}
    lv_img_cache_invalidate_src(&logo.image);free(logo.bytes);logo={};copyText(logo.key,key);logo.loaded=millis();logo.bytes=copy;
    if(copy){logo.image.header.always_zero=0;logo.image.header.cf=LV_IMG_CF_RAW_ALPHA;logo.image.header.w=width;logo.image.header.h=height;logo.image.data=copy;logo.image.data_size=body.text.length();}
    details();lvgl_port_unlock();Serial.printf("RADAR: airline logo %s %s\n",key,copy?"loaded":"unavailable");
}
// A larger radius can contain hundreds of aircraft. Keep transfer storage in
// PSRAM and filter JSON before constructing the document used by the tracker.
class AircraftBody:public Stream {
public:
    static constexpr size_t LIMIT=256*1024;uint8_t *data=static_cast<uint8_t*>(ps_malloc(LIMIT));size_t size=0;
    ~AircraftBody(){free(data);}int available()override{return 0;}int read()override{return -1;}int peek()override{return -1;}void flush()override{}
    size_t write(uint8_t v)override{return write(&v,1);}size_t write(const uint8_t *p,size_t n)override{if(!data||n>LIMIT-size)return 0;memcpy(data+size,p,n);size+=n;return n;}
};
class AircraftAllocator:public ArduinoJson::Allocator {
public:
    void *allocate(size_t n)override{return heap_caps_malloc(n,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);}
    void deallocate(void *p)override{free(p);}
    void *reallocate(void *p,size_t n)override{return heap_caps_realloc(p,n,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);}
};
AircraftAllocator aircraftAllocator;
int getAircraftJSON(const String &url,JsonDocument &doc,uint32_t &retry){
    WiFiClientSecure client;client.setCACertBundle(x509_crt_bundle_start,x509_crt_bundle_end-x509_crt_bundle_start);client.setHandshakeTimeout(8);
    HTTPClient http;http.setConnectTimeout(8000);http.setTimeout(20000);http.useHTTP10(false);http.setReuse(false);if(!http.begin(client,url))return -1;
    http.setUserAgent("SanDiegoFlightRadar/1.5 (personal display; github.com/zschramm/esp32-flight-radar)");http.addHeader("Accept-Encoding","identity");const char *headers[]={"Retry-After","X-RateLimit-Reset"};http.collectHeaders(headers,2);int code=http.GET();
    if(code==200){AircraftBody body;int expected=http.getSize();int received=body.data&&expected<=int(AircraftBody::LIMIT)?http.writeToStream(&body):-8;
        Serial.printf("RADAR: download bytes=%u expected=%d result=%d heap=%u psram=%u\n",unsigned(body.size),expected,received,ESP.getFreeHeap(),ESP.getFreePsram());
        if(received<0)code=received;else{
        JsonDocument filter;filter["now"]=true;for(const char *key:{"lat","lon","alt_baro","seen_pos","hex","flight","r","t","gs","track"})filter["ac"][0][key]=true;
        // Read-only input makes the document own strings after the body is freed.
        auto error=deserializeJson(doc,static_cast<const uint8_t*>(body.data),body.size,DeserializationOption::Filter(filter));if(error){Serial.printf("RADAR: JSON %s\n",error.c_str());code=-2;}
    }}
    if(code==429)retry=feed_retry::delaySeconds(http.header("Retry-After").c_str(),http.header("X-RateLimit-Reset").c_str(),time(nullptr));
    if(code!=200)Serial.printf("RADAR: provider=adsb.fi HTTP=%d retry_s=%lu\n",code,(unsigned long)retry);
    http.end();return code;
}
bool pollAircraft() {
    JsonDocument doc(&aircraftAllocator);uint32_t retry=0;
    // Request the current view in nautical miles, then apply a statute-mile filter.
    const double requestMiles=std::max(10.0,viewMiles()*1.45);
    String url="https://opendata.adsb.fi/api/v3/lat/"+String(centerLat,6)+"/lon/"+String(centerLon,6)+"/dist/"+String(int(ceil(requestMiles/1.15077945)));
    int code=getAircraftJSON(url,doc,retry);
    if(code!=200){
        char b[60];snprintf(b,sizeof(b),"adsb.fi unavailable (%d)",code);setState(b);
        pollDelay=std::min(pollDelay*2,uint32_t(300000));
        if(code==429){saveFeedRetry(uint64_t(time(nullptr))+std::max(retry,uint32_t(10)));setState("adsb.fi rate limited");pollDelay=POLL_MS;}
        return false;
    }
    if(feedRetryUntil)saveFeedRetry(0);
    if(!doc["ac"].is<JsonArray>()){setState("Unexpected feed response");pollDelay=60000;return false;}
    // Reject cached upstream snapshots even when the HTTP request itself succeeded.
    double timestamp=doc["now"]|0.0;if(timestamp>1e11)timestamp/=1000.0;
    double feedAge=double(time(nullptr))-timestamp;
    if(timestamp<1e9||feedAge>60||feedAge < -60){setState("Feed timestamp is stale");pollDelay=30000;return false;}
    std::unique_ptr<Plane[]> next(new(std::nothrow) Plane[MAX_PLANES]);int count=0;
    if(!next){setState("Low memory; retrying");pollDelay=30000;return false;}
    for(JsonObject a:doc["ac"].as<JsonArray>()) {
        if(!a["lat"].is<double>()||!a["lon"].is<double>())continue;
        if(a["alt_baro"].is<const char*>())continue; // Ground traffic would crowd the airport.
        double lat=a["lat"],lon=a["lon"];
        if(!std::isfinite(lat)||!std::isfinite(lon)||fabs(lat)>90||fabs(lon)>180)continue;
        auto offset=radar::project(centerLat,centerLon,lat,lon);
        float age=(a["seen_pos"]|999.0f)+std::max(0.0,feedAge);
        if(!std::isfinite(offset.miles)||offset.miles>requestMiles||age>30||age<0||!std::isfinite(age))continue;
        Plane p;copyText(p.hex,a["hex"]|"");if(!p.hex[0])continue;
        String callsign=a["flight"]|"";callsign.trim();copyText(p.callsign,callsign.c_str());
        copyText(p.registration,a["r"]|"");copyText(p.type,a["t"]|"");
        p.altitude=a["alt_baro"].is<double>()?a["alt_baro"].as<float>():NAN;
        p.knots=a["gs"].is<double>()?a["gs"].as<float>():NAN;
        p.track=a["track"].is<double>()?a["track"].as<float>():NAN;
        p.seen=age;p.latitude=lat;p.longitude=lon;p.miles=offset.miles;p.east=offset.east;p.north=offset.north;
        if(count<MAX_PLANES)next[count++]=p;
        else {int farthest=0;for(int j=1;j<count;j++)if(next[j].miles>next[farthest].miles)farthest=j;if(p.miles<next[farthest].miles)next[farthest]=p;}
    }
    std::sort(next.get(),next.get()+count,[](const Plane&a,const Plane&b){return a.miles<b.miles;});
    lvgl_port_lock(-1);memcpy(planes,next.get(),sizeof(Plane)*count);planeCount=count;lastGood=millis();haveData=true;copyText(networkState,"Receiving aircraft");refreshUI(nullptr);lvgl_port_unlock();
    pollDelay=POLL_MS;Serial.printf("RADAR: provider=adsb.fi %d airborne aircraft within %g miles\n",count,requestMiles);return true;
}
void lookupSelectedRoute() {
    char callsign[16]{};double latitude=0,longitude=0;
    lvgl_port_lock(-1);int i=selectedIndex();if(i>=0&&!cachedRoute(planes[i].callsign)){copyText(callsign,planes[i].callsign);latitude=planes[i].latitude;longitude=planes[i].longitude;}lvgl_port_unlock();
    if(!callsign[0])return;
    for(const char *p=callsign;*p;p++)if(!isalnum(static_cast<unsigned char>(*p)))return;
    JsonDocument doc;uint32_t retry=0;int code=getJSON(String("https://api.adsbdb.com/v0/callsign/")+callsign,doc,retry);
    nextRoute=millis()+(code==429?retry*1000:30000);
    if(code!=200&&code!=404)return;
    Route r;copyText(r.callsign,callsign);r.fetched=millis();JsonObject route=doc["response"]["flightroute"].as<JsonObject>();
    if(!route.isNull()) {
        r.found=false;copyText(r.flight,route["callsign_iata"]|callsign);copyText(r.airline,route["airline"]["name"]|"");
        copyText(r.airlineIATA,route["airline"]["iata"]|"");copyText(r.airlineICAO,route["airline"]["icao"]|"");
        copyText(r.origin,route["origin"]["iata_code"]|route["origin"]["icao_code"].as<const char*>());
        copyText(r.destination,route["destination"]["iata_code"]|route["destination"]["icao_code"].as<const char*>());
        copyText(r.originCity,route["origin"]["municipality"]|"");copyText(r.destinationCity,route["destination"]["municipality"]|"");
        auto o=route["origin"],d=route["destination"];
        if(o["latitude"].is<double>()&&o["longitude"].is<double>()&&d["latitude"].is<double>()&&d["longitude"].is<double>()){
            r.found=radar::plausibleRoute(latitude,longitude,o["latitude"],o["longitude"],d["latitude"],d["longitude"]);
        }
        if(!r.found)Serial.printf("RADAR: rejected implausible/unlocated route %s %s-%s\n",callsign,r.origin,r.destination);
    }
    lvgl_port_lock(-1);routes[routeSlot++%24]=r;details();lvgl_port_unlock();
}
void performanceStats() {
    // Runtime counters measure scheduler task time, including both idle tasks.
    // They are diagnostic estimates, not a separate hardware-cycle profiler.
    static TaskStatus_t tasks[48];static uint32_t previousTime=0,previousIdle[2]{};static bool started=false;
    uint32_t total=0,idle[2]{};bool found[2]{};
    UBaseType_t count=uxTaskGetSystemState(tasks,48,&total);
    for(UBaseType_t n=0;n<count;n++)for(int core=0;core<2;core++){
        const char *name=core?"IDLE1":"IDLE0";
        if(!strcmp(tasks[n].pcTaskName,name)){idle[core]=tasks[n].ulRunTimeCounter;found[core]=true;}
    }
    if(!count||!found[0]||!found[1]){Serial.printf("PERF: runtime counters unavailable tasks=%u\n",unsigned(count));return;}
    const uint32_t elapsed=total-previousTime;
    if(started&&elapsed>=1000000){
        float busy[2];for(int core=0;core<2;core++)busy[core]=std::clamp(100.0f*(1.0f-float(uint32_t(idle[core]-previousIdle[core]))/elapsed),0.0f,100.0f);
        Serial.printf("PERF: window_s=%.1f cpu0_busy=%.1f%% cpu1_busy=%.1f%% combined_busy=%.1f%% min_heap=%u largest_internal=%u min_psram=%u\n",elapsed/1000000.0,busy[0],busy[1],(busy[0]+busy[1])/2,ESP.getMinFreeHeap(),heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));
    }else Serial.println("PERF: baseline captured; repeat after at least one second");
    previousTime=total;previousIdle[0]=idle[0];previousIdle[1]=idle[1];started=true;
}
void serviceSerial() {
    static String command;
    static bool overflow=false;
    while(Serial.available()){
        char c=Serial.read();if(c=='\r')continue;
        if(c!='\n'){if(command.length()<768)command+=c;else overflow=true;continue;}
        if(overflow) Serial.println("CONFIG_ERROR Command too long.");
        else if(command=="STATUS")Serial.printf("RADAR: mode=%s wifi=%s center=%.6f,%.6f aircraft=%d heap=%u psram=%u version=1.5 followNearest=%d selected=%s nearest=%s provider=adsb.fi retry_s=%lu feed=%s\n",usbSetup?"usb-setup":"radar",WiFi.status()==WL_CONNECTED?"connected":"offline",centerLat,centerLon,planeCount,ESP.getFreeHeap(),ESP.getFreePsram(),followNearest,selectedHex,planeCount?planes[0].hex:"",(unsigned long)feedRetryRemaining(),networkState);
        else if(command=="PAGE WEATHER"){lvgl_port_lock(-1);weather::show(true);lvgl_port_unlock();Serial.println("PAGE_OK WEATHER");}
        else if(command=="PAGE FLIGHTS"){lvgl_port_lock(-1);weather::show(false);lvgl_port_unlock();Serial.println("PAGE_OK FLIGHTS");}
        else if(command=="WEATHER")weather::diagnostics();
        else if(command=="PERF")performanceStats();
        else if(command=="PAGE WIND"){lvgl_port_lock(-1);weather::showWind(false);lvgl_port_unlock();Serial.println("PAGE_OK WIND");}
        else if(command=="PAGE WEEK"){lvgl_port_lock(-1);weather::showWind(true);lvgl_port_unlock();Serial.println("PAGE_OK WEEK");}
        else if(command=="WEATHER ZOOM+"||command=="WEATHER ZOOM-"){lvgl_port_lock(-1);weather::zoom(command.endsWith("+")?1:-1);lvgl_port_unlock();weather::diagnostics();}
        else if(command=="WEATHER IR"||command=="WEATHER VISIBLE"){lvgl_port_lock(-1);weather::layer(command.endsWith("IR"));lvgl_port_unlock();weather::diagnostics();}
        else if(command=="RADAR ZOOM+"||command=="RADAR ZOOM-"){lvgl_port_lock(-1);changeRadarZoom(command.endsWith("+")?1:-1);lvgl_port_unlock();Serial.printf("RADAR: range %g miles\n",viewMiles());}
        else if(command.startsWith("SELECT ")){lvgl_port_lock(-1);for(int j=0;j<planeCount;j++)if(command.substring(7)==planes[j].hex&&fresh(planes[j])){copyText(selectedHex,planes[j].hex);followNearest=false;refreshUI(nullptr);}lvgl_port_unlock();}
        else if(command=="NEAREST"){lvgl_port_lock(-1);followNearest=true;refreshUI(nullptr);lvgl_port_unlock();}
        else if(command=="SETTINGS"){
            JsonDocument doc;doc["ssid"]=ssid;doc["lat"]=centerLat;doc["lon"]=centerLon;
            Serial.print("SETTINGS ");serializeJson(doc,Serial);Serial.println();
        }
        else if(command.startsWith("CONFIG "))saveUSBConfig(command.substring(7));
        else if(command=="SCREENSHOT"){
            lvgl_port_lock(-1);auto fb=static_cast<const uint8_t*>(board->getLCD()->getFrameBufferByIndex(0));
            if(fb){Serial.print("RGB565 800 480 768000\n");Serial.write(fb,768000);Serial.flush();Serial.print("\nEND_SCREENSHOT\n");}
            lvgl_port_unlock();
        }
        command="";overflow=false;
    }
}
}

void setup() {
    Serial.setRxBufferSize(1024);Serial.begin(921600);delay(200);Serial.println("RADAR: San Diego Flight Radar v1.5");
    // Independent boundary and bearing checks run on the actual target at boot.
    const auto east=radar::project(0,0,0,1), north=radar::project(0,0,1,0);
    const auto acrossDateLine=radar::project(0,179.99,0,-179.99);
    bool mathOK=fabs(east.miles-69.0934)<0.01 && east.east>69 && fabs(east.north)<0.001
        && north.north>69 && fabs(north.east)<0.001 && acrossDateLine.miles<1.4
        && acrossDateLine.east>0 && radar::inRange(10.0) && !radar::inRange(10.0001)
        && !radar::inRange(NAN) && radar::project(32.7157,-117.1611,32.7157,-117.1611).miles==0;
    Serial.printf("RADAR: distance/bearing self-test %s\n",mathOK?"PASS":"FAIL");assert(mathOK);
    const bool routeOK=!radar::plausibleRoute(32.73,-117.20,36.1263,-86.6774,38.9531,-77.4565)
        &&!radar::plausibleRoute(32.73,-117.20,33.6407,-84.4277,33.3699,-81.9645)
        &&radar::plausibleRoute(32.73,-117.20,37.6213,-122.3790,32.7336,-117.1897);
    Serial.printf("RADAR: route plausibility BNA-IAD / ATL-AGS reject, SFO-SAN accept %s\n",routeOK?"PASS":"FAIL");assert(routeOK);
    const bool retryOK=feed_retry::delaySeconds("120","",1700000000)==120
        &&feed_retry::delaySeconds("","35283",1700000000)==35283
        &&feed_retry::delaySeconds("","1700000060",1700000000)==60
        &&feed_retry::delaySeconds("Tue, 14 Nov 2023 22:14:20 GMT","",1700000000)==60
        &&feed_retry::delaySeconds("Thu, 29 Feb 2024 00:00:00 GMT","",1709164740)==60
        &&feed_retry::delaySeconds("Thu, 29 Feb 2023 00:00:00 GMT","",1700000000)==120
        &&feed_retry::delaySeconds("0","900",1700000000)==0
        &&feed_retry::delaySeconds("","1699999999",1700000000)==0
        &&feed_retry::delaySeconds("garbage","-1",1700000000)==120
        &&feed_retry::delaySeconds("4294967295","",1700000000)==UINT32_MAX;
    Serial.printf("RADAR: feed retry seconds / epoch / UTC date self-test %s\n",retryOK?"PASS":"FAIL");assert(retryOK);
    board=new Board();assert(board->init());auto lcd=board->getLCD();
    lcd->configFrameBufferNumber(LVGL_PORT_DISP_BUFFER_NUM);
    auto bus=static_cast<BusRGB*>(lcd->getBus());bus->configRGB_FreqHz(13000000);bus->configRGB_BounceBufferSize(800*10);
    assert(board->begin());assert(lvgl_port_init(lcd,board->getTouch()));
    lvgl_port_lock(-1);buildUI();lvgl_port_unlock();
    loadConfig();
    Preferences feedStorage;
    if(feedStorage.begin("radar-feed-fi",true)){feedRetryUntil=feedStorage.getULong64("retry-until",0);feedStorage.end();}
    if(ssid.isEmpty()){beginUSBSetup();return;}
    WiFi.mode(WIFI_STA);WiFi.setSleep(false);WiFi.setAutoReconnect(true);WiFi.begin(ssid.c_str(),wifiPassword.c_str());
    configTzTime("PST8PDT,M3.2.0,M11.1.0","pool.ntp.org","time.cloudflare.com");
}
void loop() {
    serviceSerial();
    if(setupRequested&&!usbSetup)beginUSBSetup();
    if(resumeRequested){
        resumeRequested=false;usbSetup=false;
        lvgl_port_lock(-1);if(setupOverlay){lv_obj_del(setupOverlay);setupOverlay=nullptr;}lvgl_port_unlock();
        WiFi.mode(WIFI_STA);WiFi.setSleep(false);WiFi.setAutoReconnect(true);WiFi.begin(ssid.c_str(),wifiPassword.c_str());
    }
    if(usbSetup){delay(5);return;}
    if(WiFi.status()!=WL_CONNECTED){setState("Wi-Fi reconnecting / SETUP to change");delay(100);return;}
    if(time(nullptr)<1700000000){setState("Syncing clock for secure data");delay(100);return;}
    if(!feedRetryRemaining()&&due(millis(),nextPoll)){pollAircraft();nextPoll=millis()+pollDelay;}
    if(due(millis(),nextRoute)){lookupSelectedRoute();if(due(millis(),nextRoute))nextRoute=millis()+2000;}
    weather::service();
    fetchSelectedLogo();
    delay(20);
}

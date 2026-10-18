#include "weather_display.h"
#include "lvgl_v8_port.h"
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <esp_heap_caps.h>
#include <time.h>
#include <cmath>
#include <algorithm>

extern const uint8_t x509_crt_bundle_start[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t x509_crt_bundle_end[] asm("_binary_x509_crt_bundle_end");
namespace weather {
namespace {
constexpr uint32_t BG=0x09131E,PANEL=0x101F2C,GRID=0x234050,WHITE=0xEEF6FA,MUTED=0xA4B7C5,CYAN=0x5BE2DB,AMBER=0xFFC16D;
// Decode at 640x320, scale to the full 800x400 map. Leaves PSRAM for six scans.
constexpr int MAP_W=640,MAP_H=320,NFRAMES=6;
struct Hour {time_t time=0;float wind=NAN,direction=NAN,gust=NAN,temp=NAN;};
struct Day {time_t time=0;float high=NAN,low=NAN,rain=NAN,wind=NAN,direction=NAN,gust=NAN;int code=-1;};
struct Frame {uint8_t *bytes=nullptr;lv_img_dsc_t image{};time_t time=0;};
Hour hrrr[30],modelHours[2][168];Day days[2][7];Frame frames[NFRAMES];
float temperature=NAN;int condition=-1,hourCount=0,modelHourCount=0;
uint32_t hrrrLoaded=0,dailyLoaded=0,nextHRRR=0,nextDaily=0,nextImages=0,nextFrame=0,nextImageTry=0,generation=0;
bool hrrrOK=false,dailyOK=false,visible=false,windVisible=false,infrared=true,playing=true,weekly=false;
bool frameInfrared=true;int mapZoom=1,frameZoom=1,loadFrame=-1,shownFrame=-1,imageFailures=0,chosenDay=-1;
time_t frameTimes[NFRAMES]{};
double *lat,*lon;FetchJSON fetchJSON;
lv_obj_t *flight,*page,*windPage,*infoPage,*summary,*compact,*windCanvas,*windTitle,*windAge;
lv_obj_t *imageObject,*mapCaption,*imageStatus,*mapCurrent,*zoomLabel,*modeLabel,*playLabel;
lv_obj_t *zoomInButton,*zoomOutButton;
char imageMessage[96]="Loading satellite and radar...";
const uint8_t blankPixels[3]={0,0,0};lv_img_dsc_t blankImage{};

bool due(uint32_t now,uint32_t deadline){return int32_t(now-deadline)>=0;}
float number(JsonVariantConst v){return v.is<float>()?v.as<float>():NAN;}
String whole(float n){return std::isfinite(n)?String(int(lround(n))):String("--");}
String localHour(time_t v){tm t{};localtime_r(&v,&t);return String(t.tm_hour%12?t.tm_hour%12:12)+(t.tm_hour<12?"a":"p");}
const char *description(int c){if(c==0)return "Clear";if(c==1)return "Mostly clear";if(c==2)return "Partly cloudy";if(c==3)return "Overcast";if(c==45||c==48)return "Fog";if(c>=51&&c<=57)return "Drizzle";if(c>=61&&c<=67)return "Rain";if(c>=71&&c<=77)return "Snow";if(c>=80&&c<=86)return "Showers";if(c>=95)return "Storms";return "--";}
const char *brief(int c){if(c==0||c==1)return "Sun";if(c==2)return "Part cloud";if(c==3)return "Cloud";return description(c);}
lv_obj_t *text(lv_obj_t *p,int x,int y,int w,int h,const char *s,const lv_font_t *f,uint32_t c){auto o=lv_label_create(p);lv_obj_set_pos(o,x,y);lv_obj_set_size(o,w,h);lv_label_set_text(o,s);lv_label_set_long_mode(o,LV_LABEL_LONG_DOT);lv_obj_set_style_text_font(o,f,0);lv_obj_set_style_text_color(o,lv_color_hex(c),0);return o;}
lv_obj_t *button(lv_obj_t *p,int x,int y,int w,const char *s,lv_event_cb_t cb,int h=34){auto b=lv_btn_create(p);lv_obj_set_pos(b,x,y);lv_obj_set_size(b,w,h);lv_obj_set_style_bg_color(b,lv_color_hex(GRID),0);lv_obj_set_style_radius(b,5,0);lv_obj_set_style_pad_all(b,0,0);lv_obj_add_event_cb(b,cb,LV_EVENT_CLICKED,nullptr);auto l=lv_label_create(b);lv_obj_set_style_text_font(l,&lv_font_montserrat_14,0);lv_label_set_text(l,s);lv_obj_center(l);return l;}
lv_obj_t *screen(lv_obj_t *parent){auto p=lv_obj_create(parent);lv_obj_remove_style_all(p);lv_obj_set_size(p,800,480);lv_obj_set_style_bg_color(p,lv_color_hex(BG),0);lv_obj_set_style_bg_opa(p,LV_OPA_COVER,0);lv_obj_clear_flag(p,LV_OBJ_FLAG_SCROLLABLE);lv_obj_add_flag(p,LV_OBJ_FLAG_HIDDEN);return p;}
void drawText(lv_draw_ctx_t *ctx,int x,int y,int w,const String &s,const lv_font_t *font,uint32_t color,bool center=true){lv_draw_label_dsc_t d;lv_draw_label_dsc_init(&d);d.font=font;d.color=lv_color_hex(color);d.align=center?LV_TEXT_ALIGN_CENTER:LV_TEXT_ALIGN_LEFT;lv_area_t a{int16_t(x),int16_t(y),int16_t(x+w-1),int16_t(y+font->line_height-1)};lv_draw_label(ctx,&d,&a,s.c_str(),nullptr);}
void fill(lv_draw_ctx_t *ctx,int x,int y,int w,int h,uint32_t color){lv_draw_rect_dsc_t d;lv_draw_rect_dsc_init(&d);d.bg_color=lv_color_hex(color);d.bg_opa=LV_OPA_COVER;d.border_width=0;lv_area_t a{int16_t(x),int16_t(y),int16_t(x+w-1),int16_t(y+h-1)};lv_draw_rect(ctx,&d,&a);}
// Fixed 0-30 kt scale. Values above 30 keep their number and use the top color.
uint32_t windColor(float speed){
    if(!std::isfinite(speed))return GRID;
    constexpr uint32_t stops[]={0xDDE9ED,0x8CCDDD,0x68C5B5,0x90C980,0xD6D077,0xE5AA70,0xD77986};
    float t=std::clamp(speed,0.0f,30.0f)/5;int i=std::min(5,int(t));t-=i;
    uint32_t out=0;for(int shift:{16,8,0}){float a=(stops[i]>>shift)&255,b=(stops[i+1]>>shift)&255;out|=uint32_t(lround(a+(b-a)*t))<<shift;}return out;
}
void arrow(lv_draw_ctx_t *ctx,int x,int y,float from,int size,uint32_t color){
    if(!std::isfinite(from))return;
    // Meteorological bearings are FROM. The arrow points downwind, as on Windy.
    float a=(from+180)*3.141592653589793/180,fx=sin(a),fy=-cos(a),rx=cos(a),ry=sin(a);
    auto p=[&](float f,float r){return lv_point_t{int16_t(lround(x+fx*f+rx*r)),int16_t(lround(y+fy*f+ry*r))};};
    lv_draw_line_dsc_t d;lv_draw_line_dsc_init(&d);d.width=2;d.color=lv_color_hex(color);d.round_start=d.round_end=1;
    auto a1=p(-size*.6f,0),b=p(size*.6f,0);lv_draw_line(ctx,&d,&a1,&b);a1=p(size*.1f,-size*.4f);lv_draw_line(ctx,&d,&a1,&b);a1=p(size*.1f,size*.4f);lv_draw_line(ctx,&d,&a1,&b);
}
time_t nextSlot(){time_t n=time(nullptr);for(int j=0;j<4;j++){time_t t=(n/3600+j)*3600;tm l{};localtime_r(&t,&l);if(t>=n-1800&&l.tm_hour%3==0)return t;}return ((n+10799)/10800)*10800;}
Hour sample(int m,time_t t){if(m==0){if(!hrrrOK||millis()-hrrrLoaded>3600000)return {};for(int i=0;i<hourCount;i++)if(hrrr[i].time==t)return hrrr[i];}else{if(!dailyOK||millis()-dailyLoaded>12UL*3600000)return {};for(int i=0;i<modelHourCount;i++)if(modelHours[m-1][i].time==t)return modelHours[m-1][i];}return {};}
void compactDraw(lv_event_t *e){auto ctx=lv_event_get_draw_ctx(e);time_t first=nextSlot();for(int i=0;i<8;i++){auto h=sample(0,first+i*10800);int x=16+i*34;drawText(ctx,x,353,33,localHour(first+i*10800),&lv_font_montserrat_12,MUTED);arrow(ctx,x+16,379,h.direction,13,CYAN);fill(ctx,x,391,33,18,windColor(h.wind));fill(ctx,x,411,33,18,windColor(h.gust));drawText(ctx,x,391,33,whole(h.wind),&lv_font_montserrat_14,std::isfinite(h.wind)?BG:MUTED);drawText(ctx,x,411,33,whole(h.gust),&lv_font_montserrat_14,std::isfinite(h.gust)?BG:MUTED);}}
void legend(lv_draw_ctx_t *ctx,int x,int y){drawText(ctx,x,y,188,"Wind / gusts in knots",&lv_font_montserrat_14,MUTED,false);for(int i=0;i<7;i++){fill(ctx,x+200+i*56,y-2,54,22,windColor(i*5));drawText(ctx,x+200+i*56,y,54,String(i*5),&lv_font_montserrat_14,BG);}drawText(ctx,x+600,y,160,"Arrows point downwind",&lv_font_montserrat_12,MUTED,false);}
void modelRow(lv_draw_ctx_t *ctx,int y,int m,int n,int cw,time_t first,bool daily){
    const char *names[]={"HRRR","ECMWF","GFS"};drawText(ctx,12,y+10,100,names[m],&lv_font_montserrat_16,m==0?CYAN:WHITE,false);
    drawText(ctx,12,y+40,100,daily?"Max wind":"Wind",&lv_font_montserrat_14,MUTED,false);drawText(ctx,12,y+66,100,daily?"Max gust":"Gust",&lv_font_montserrat_14,MUTED,false);
    for(int i=0;i<n;i++){int x=112+i*cw;Hour h;if(daily){const auto &d=days[m-1][i];h={d.time,d.wind,d.direction,d.gust,NAN};}else h=sample(m,first+i*10800);
        fill(ctx,x,y+2,cw-2,32,PANEL);arrow(ctx,x+(cw-2)/2,y+18,h.direction,18,WHITE);
        fill(ctx,x,y+36,cw-2,25,windColor(h.wind));fill(ctx,x,y+63,cw-2,25,windColor(h.gust));
        drawText(ctx,x,y+38,cw-2,whole(h.wind),&lv_font_montserrat_20,std::isfinite(h.wind)?BG:MUTED);drawText(ctx,x,y+65,cw-2,whole(h.gust),&lv_font_montserrat_20,std::isfinite(h.gust)?BG:MUTED);
    }
}
void windDraw(lv_event_t *e){
    auto ctx=lv_event_get_draw_ctx(e);legend(ctx,12,449);
    if(weekly){
        drawText(ctx,12,67,96,"7 DAYS",&lv_font_montserrat_14,CYAN,false);drawText(ctx,12,97,96,"High / low F",&lv_font_montserrat_12,MUTED,false);drawText(ctx,12,147,96,"Rain / in",&lv_font_montserrat_12,MUTED,false);
        for(int i=0;i<7;i++){int x=112+i*96;const auto &d=days[0][i];tm t{};localtime_r(&d.time,&t);char s[18];strftime(s,sizeof(s),"%a %d",&t);drawText(ctx,x,64,94,d.time?String(s):"--",&lv_font_montserrat_16,WHITE);drawText(ctx,x,94,94,whole(d.high)+" / "+whole(d.low),&lv_font_montserrat_18,WHITE);drawText(ctx,x,121,94,brief(d.code),&lv_font_montserrat_12,MUTED);drawText(ctx,x,146,94,std::isfinite(d.rain)?String(d.rain,2):"--",&lv_font_montserrat_14,MUTED);}
        modelRow(ctx,177,1,7,96,0,true);modelRow(ctx,279,2,7,96,0,true);
        drawText(ctx,12,389,770,"Tap a day for 3-hour winds. Daily arrows: prevailing direction.",&lv_font_montserrat_14,MUTED,false);
        drawText(ctx,12,413,770,"One temperature / weather outlook; ECMWF and GFS winds compared below.",&lv_font_montserrat_12,MUTED,false);
    }else{
        time_t first=chosenDay>=0?days[0][chosenDay].time:nextSlot();
        drawText(ctx,12,68,96,"LOCAL",&lv_font_montserrat_12,MUTED,false);drawText(ctx,12,95,96,"Temp / F",&lv_font_montserrat_14,MUTED,false);
        for(int i=0;i<8;i++){int x=112+i*84;time_t t=first+i*10800;auto h=sample(0,t);if(!std::isfinite(h.temp))h=sample(1,t);drawText(ctx,x,64,82,localHour(t),&lv_font_montserrat_16,WHITE);drawText(ctx,x,92,82,whole(h.temp),&lv_font_montserrat_18,WHITE);}
        modelRow(ctx,125,0,8,84,first,false);modelRow(ctx,227,1,8,84,first,false);modelRow(ctx,329,2,8,84,first,false);
    }
}
void clearFrames(){lv_img_set_src(imageObject,&blankImage);lv_obj_add_flag(imageObject,LV_OBJ_FLAG_HIDDEN);for(auto &f:frames){lv_img_cache_invalidate_src(&f.image);free(f.bytes);f={};}shownFrame=-1;loadFrame=-1;imageFailures=0;}
void setImageMessage(const char *s){lvgl_port_lock(-1);strlcpy(imageMessage,s,sizeof(imageMessage));lvgl_port_unlock();}
void requestImages(){generation++;clearFrames();nextImages=0;nextImageTry=0;strlcpy(imageMessage,"Loading selected map view...",sizeof(imageMessage));}
void mapDraw(lv_event_t *e){if(shownFrame<0)return;auto ctx=lv_event_get_draw_ctx(e);lv_draw_line_dsc_t d;lv_draw_line_dsc_init(&d);d.color=lv_color_hex(AMBER);d.width=2;lv_point_t a{393,240},b{407,240};lv_draw_line(ctx,&d,&a,&b);a={400,233};b={400,247};lv_draw_line(ctx,&d,&a,&b);drawText(ctx,412,234,110,"Center",&lv_font_montserrat_14,AMBER,false);}
class Body:public Stream {
public:
    static constexpr size_t LIMIT=512*1024;uint8_t *data=static_cast<uint8_t*>(ps_malloc(LIMIT));size_t size=0;
    ~Body(){free(data);}int available()override{return 0;}int read()override{return -1;}int peek()override{return -1;}void flush()override{}
    size_t write(uint8_t b)override{return write(&b,1);}size_t write(const uint8_t *p,size_t n)override{if(!data||n>LIMIT-size)return 0;memcpy(data+size,p,n);size+=n;return n;}
};
int getBody(const String &url,Body &b){
    if(!b.data)return -3;WiFiClientSecure client;client.setCACertBundle(x509_crt_bundle_start,x509_crt_bundle_end-x509_crt_bundle_start);client.setHandshakeTimeout(8);
    HTTPClient http;http.setConnectTimeout(8000);http.setTimeout(15000);http.useHTTP10(false);http.setReuse(false);if(!http.begin(client,url))return -1;
    http.setUserAgent("PointLomaFlightRadar/1.3");http.addHeader("Accept-Encoding","identity");int code=http.GET();
    if(code==200){int expected=http.getSize();int written=expected>int(Body::LIMIT)?-3:http.writeToStream(&b);if(written<0){Serial.printf("WEATHER: HTTP body failed expected=%d received=%u write=%d\n",expected,unsigned(b.size),written);code=-3;}}
    http.end();return code;
}
String query(){return "latitude="+String(*lat,7)+"&longitude="+String(*lon,7)+"&temperature_unit=fahrenheit&wind_speed_unit=kn&precipitation_unit=inch&timezone=America%2FLos_Angeles&timeformat=unixtime";}
void fetchHRRR(){
    JsonDocument d;uint32_t retry=0;String url="https://api.open-meteo.com/v1/forecast?"+query()+"&models=gfs_hrrr&current=temperature_2m,weather_code&hourly=temperature_2m,wind_speed_10m,wind_direction_10m,wind_gusts_10m&forecast_hours=27";
    int code=fetchJSON(url,d,retry);nextHRRR=millis()+(code==200?600000:code==429?std::max(uint32_t(120),retry)*1000:120000);
    if(code!=200||!d["current"]["time"].is<long>()||!d["hourly"]["time"].is<JsonArray>()){Serial.printf("WEATHER: HRRR unavailable (%d)\n",code);return;}
    time_t now=time(nullptr),obs=d["current"]["time"].as<long>();if(obs>now+3600||now-obs>7200)return;Hour next[30];JsonObject h=d["hourly"];int count=std::min(30,int(h["time"].size()));
    for(int i=0;i<count;i++)next[i]={h["time"][i].as<long>(),number(h["wind_speed_10m"][i]),number(h["wind_direction_10m"][i]),number(h["wind_gusts_10m"][i]),number(h["temperature_2m"][i])};
    lvgl_port_lock(-1);memcpy(hrrr,next,sizeof(next));hourCount=count;temperature=number(d["current"]["temperature_2m"]);condition=d["current"]["weather_code"]|-1;hrrrLoaded=millis();hrrrOK=true;tick();lvgl_port_unlock();Serial.printf("WEATHER: HRRR loaded %d hours, knots\n",count);
}
void fetchDaily(){
    JsonDocument doc;uint32_t retry=0;String url="https://api.open-meteo.com/v1/forecast?"+query()+"&models=ecmwf_ifs,gfs_global&hourly=temperature_2m,wind_speed_10m,wind_direction_10m,wind_gusts_10m&daily=temperature_2m_max,temperature_2m_min,weather_code,precipitation_sum,wind_speed_10m_max,wind_gusts_10m_max,wind_direction_10m_dominant&forecast_days=7";
    int code=fetchJSON(url,doc,retry);nextDaily=millis()+(code==200?3600000:code==429?std::max(uint32_t(120),retry)*1000:180000);
    JsonObject d=doc["daily"],h=doc["hourly"];if(code!=200||d["time"].size()!=7||h["time"].size()<24){Serial.printf("WEATHER: models unavailable (%d)\n",code);return;}
    time_t day=d["time"][0].as<long>(),now=time(nullptr);if(day>now+3600||now-day>30*3600)return;const char *models[]={"ecmwf_ifs","gfs_global"};
    lvgl_port_lock(-1);modelHourCount=std::min(168,int(h["time"].size()));
    for(int m=0;m<2;m++){
        String suffix=String("_")+models[m];
        for(int i=0;i<7;i++){auto &v=days[m][i];v.time=d["time"][i].as<long>();v.high=number(d["temperature_2m_max"+suffix][i]);v.low=number(d["temperature_2m_min"+suffix][i]);v.rain=number(d["precipitation_sum"+suffix][i]);v.wind=number(d["wind_speed_10m_max"+suffix][i]);v.direction=number(d["wind_direction_10m_dominant"+suffix][i]);v.gust=number(d["wind_gusts_10m_max"+suffix][i]);v.code=d["weather_code"+suffix][i]|-1;}
        for(int i=0;i<modelHourCount;i++)modelHours[m][i]={h["time"][i].as<long>(),number(h["wind_speed_10m"+suffix][i]),number(h["wind_direction_10m"+suffix][i]),number(h["wind_gusts_10m"+suffix][i]),number(h["temperature_2m"+suffix][i])};
    }
    dailyLoaded=millis();dailyOK=true;tick();lvgl_port_unlock();Serial.printf("WEATHER: ECMWF/GFS loaded %d hours + 7 days, wind/gust knots\n",modelHourCount);
}
time_t parseUTC(const String &iso){int y,m,d,h,minute,second;if(sscanf(iso.c_str(),"%d-%d-%dT%d:%d:%d",&y,&m,&d,&h,&minute,&second)!=6)return 0;if(y<2020||m<1||m>12||d<1||d>31||h>23||minute>59||second>59)return 0;y-=m<=2;int era=(y>=0?y:y-399)/400;unsigned year=y-era*400,day=(153*(m+(m>2?-3:9))+2)/5+d-1,yd=year*365+year/4-year/100+day;return time_t(era*146097+int(yd)-719468)*86400+h*3600+minute*60+second;}
bool loadTimeline(){
    uint32_t token=generation;Body body;int code=getBody("https://nowcoast.noaa.gov/geoserver/observations/weather_radar/wms?service=WMS&request=GetCapabilities",body);if(code!=200){setImageMessage("Imagery unavailable; retrying");return false;}
    String xml;xml.concat(reinterpret_cast<char*>(body.data),body.size);int layer=xml.indexOf("<Name>conus_base_reflectivity_mosaic</Name>"),start=xml.indexOf("<Dimension",layer),end=xml.indexOf("</Dimension>",start);if(layer<0||start<0||end<0)return false;
    start=xml.indexOf('>',start)+1;String times=xml.substring(start,end);time_t latest=0;
    for(int p=0;p<int(times.length());){int stop=times.indexOf(',',p);if(stop<0)stop=times.length();latest=std::max(latest,parseUTC(times.substring(p,stop)));p=stop+1;}
    if(latest==0||time(nullptr)-latest>7200||latest>time(nullptr)+600){setImageMessage("Waiting for fresh weather scans");return false;}time_t next[NFRAMES]{};
    for(int i=0;i<NFRAMES;i++){time_t target=latest-(NFRAMES-1-i)*600;for(int p=0;p<int(times.length());){int stop=times.indexOf(',',p);if(stop<0)stop=times.length();time_t t=parseUTC(times.substring(p,stop));if(t<=target&&t>next[i])next[i]=t;p=stop+1;}if(!next[i])return false;}
    lvgl_port_lock(-1);if(token!=generation){lvgl_port_unlock();return false;}clearFrames();memcpy(frameTimes,next,sizeof(next));frameInfrared=infrared;frameZoom=mapZoom;loadFrame=0;nextImageTry=0;lvgl_port_unlock();return true;
}
void loadImage(){
    int index=loadFrame,zoom=frameZoom;bool mode=frameInfrared;uint32_t token=generation;if(index<0||index>=NFRAMES)return;
    tm utc{};gmtime_r(&frameTimes[index],&utc);char stamp[32];strftime(stamp,sizeof(stamp),"%Y-%m-%dT%H:%M:%SZ",&utc);
    // Each step halves the geographic extent, retaining real higher-detail data.
    const double halfLat=4.0/(1<<zoom),halfLon=halfLat*MAP_W/MAP_H/cos(*lat*3.141592653589793/180);
    String url="https://nowcoast.noaa.gov/geoserver/ows?service=WMS&version=1.1.1&request=GetMap&layers=";url+=mode?"goes_longwave_imagery":"goes_visible_imagery";url+=",conus_base_reflectivity_mosaic&styles=&srs=EPSG:4326&bbox=";url+=String(*lon-halfLon,6)+","+String(*lat-halfLat,6)+","+String(*lon+halfLon,6)+","+String(*lat+halfLat,6);url+="&width=640&height=320&format=image/png&transparent=false&time=";url+=stamp;
    Body body;int code=getBody(url,body);const uint8_t sig[]={137,80,78,71,13,10,26,10};bool valid=code==200&&body.size>24&&!memcmp(body.data,sig,8);
    if(valid){unsigned w=0,h=0;for(int j=16;j<20;j++)w=(w<<8)|body.data[j];for(int j=20;j<24;j++)h=(h<<8)|body.data[j];valid=w==MAP_W&&h==MAP_H;}
    lvgl_port_lock(-1);if(token!=generation){lvgl_port_unlock();return;}
    if(valid){auto &f=frames[index];auto small=static_cast<uint8_t*>(heap_caps_realloc(body.data,body.size,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));if(small)body.data=small;f.bytes=body.data;body.data=nullptr;f.time=frameTimes[index];f.image.header.always_zero=0;f.image.header.cf=LV_IMG_CF_RAW_ALPHA;f.image.header.w=MAP_W;f.image.header.h=MAP_H;f.image.data=f.bytes;f.image.data_size=body.size;
        imageFailures=0;nextImageTry=0;snprintf(imageMessage,sizeof(imageMessage),"Loading weather loop %d/%d",index+1,NFRAMES);
        if(shownFrame<0){shownFrame=index;lv_img_set_src(imageObject,&f.image);lv_obj_clear_flag(imageObject,LV_OBJ_FLAG_HIDDEN);}
        Serial.printf("WEATHER: scan %d loaded %u bytes zoom=%d\n",index+1,unsigned(body.size),zoom);
        if(++loadFrame>=NFRAMES){loadFrame=-1;nextImages=millis()+300000;imageMessage[0]=0;Serial.println("WEATHER: NOAA animation loaded, 6 scans");}
    }else{imageFailures++;nextImageTry=millis()+std::min(60000,3000*imageFailures);snprintf(imageMessage,sizeof(imageMessage),"Scan %d retrying / %d loaded",index+1,index);Serial.printf("WEATHER: scan %d failed code=%d bytes=%u retry=%d\n",index+1,code,unsigned(body.size),imageFailures);}
    lvgl_port_unlock();
}
void updateZoomButtons(){lv_label_set_text(zoomLabel,(String(1<<mapZoom)+"x").c_str());if(mapZoom==0)lv_obj_add_state(zoomOutButton,LV_STATE_DISABLED);else lv_obj_clear_state(zoomOutButton,LV_STATE_DISABLED);if(mapZoom==4)lv_obj_add_state(zoomInButton,LV_STATE_DISABLED);else lv_obj_clear_state(zoomInButton,LV_STATE_DISABLED);}
}

void zoom(int delta){int next=std::clamp(mapZoom+delta,0,4);if(next!=mapZoom){mapZoom=next;requestImages();updateZoomButtons();}}
void layer(bool ir){if(infrared!=ir){infrared=ir;requestImages();lv_label_set_text(modeLabel,infrared?"IR":"VISIBLE");}}
void showWind(bool week){visible=false;windVisible=true;weekly=week;chosenDay=-1;lv_obj_add_flag(page,LV_OBJ_FLAG_HIDDEN);lv_obj_clear_flag(windPage,LV_OBJ_FLAG_HIDDEN);lv_obj_move_foreground(windPage);tick();}
void show(bool value){visible=value;windVisible=false;lv_obj_add_flag(windPage,LV_OBJ_FLAG_HIDDEN);lv_obj_add_flag(infoPage,LV_OBJ_FLAG_HIDDEN);if(value){lv_obj_clear_flag(page,LV_OBJ_FLAG_HIDDEN);lv_obj_move_foreground(page);}else lv_obj_add_flag(page,LV_OBJ_FLAG_HIDDEN);}
void info(){lv_obj_clear_flag(infoPage,LV_OBJ_FLAG_HIDDEN);lv_obj_move_foreground(infoPage);}
void init(lv_obj_t *flightScreen,double *latitude,double *longitude,FetchJSON fetch){
    flight=flightScreen;lat=latitude;lon=longitude;fetchJSON=fetch;blankImage.header.cf=LV_IMG_CF_TRUE_COLOR_ALPHA;blankImage.header.w=blankImage.header.h=1;blankImage.data_size=3;blankImage.data=blankPixels;
    summary=text(flight,16,311,272,21,"Weather loading...",&lv_font_montserrat_16,CYAN);text(flight,16,336,274,16,"HRRR 24h / wind + gusts / kt",&lv_font_montserrat_12,MUTED);
    compact=lv_obj_create(flight);lv_obj_remove_style_all(compact);lv_obj_set_pos(compact,12,351);lv_obj_set_size(compact,280,80);lv_obj_clear_flag(compact,LV_OBJ_FLAG_SCROLLABLE);lv_obj_add_event_cb(compact,compactDraw,LV_EVENT_DRAW_MAIN,nullptr);lv_obj_add_event_cb(compact,[](lv_event_t*){showWind(false);},LV_EVENT_CLICKED,nullptr);
    button(flight,116,437,100,"WEATHER",[](lv_event_t*){show(true);});
    page=screen(flight);imageObject=lv_img_create(page);lv_obj_set_pos(imageObject,0,40);lv_img_set_pivot(imageObject,0,0);lv_img_set_zoom(imageObject,320);lv_obj_add_flag(imageObject,LV_OBJ_FLAG_HIDDEN);
    auto overlay=lv_obj_create(page);lv_obj_remove_style_all(overlay);lv_obj_set_pos(overlay,0,40);lv_obj_set_size(overlay,800,400);lv_obj_clear_flag(overlay,LV_OBJ_FLAG_CLICKABLE);lv_obj_clear_flag(overlay,LV_OBJ_FLAG_SCROLLABLE);lv_obj_add_event_cb(overlay,mapDraw,LV_EVENT_DRAW_MAIN,nullptr);
    text(page,12,12,198,22,"SAN DIEGO",&lv_font_montserrat_18,CYAN);
    modeLabel=button(page,208,3,90,"IR",[](lv_event_t*){layer(!infrared);});playLabel=button(page,306,3,84,"PAUSE",[](lv_event_t*){playing=!playing;lv_label_set_text(playLabel,playing?"PAUSE":"PLAY");});
    button(page,398,3,90,"WIND",[](lv_event_t*){showWind(false);});button(page,496,3,104,"FLIGHTS",[](lv_event_t*){show(false);});
    zoomOutButton=lv_obj_get_parent(button(page,608,3,42,"-",[](lv_event_t*){zoom(-1);}));zoomLabel=text(page,654,10,40,22,"2x",&lv_font_montserrat_16,WHITE);lv_obj_set_style_text_align(zoomLabel,LV_TEXT_ALIGN_CENTER,0);zoomInButton=lv_obj_get_parent(button(page,698,3,42,"+",[](lv_event_t*){zoom(1);}));button(page,750,3,38,"i",[](lv_event_t*){info();});
    imageStatus=text(page,12,49,470,22,imageMessage,&lv_font_montserrat_14,WHITE);lv_obj_set_style_bg_color(imageStatus,lv_color_hex(BG),0);lv_obj_set_style_bg_opa(imageStatus,190,0);
    mapCurrent=text(page,12,451,260,22,"Weather loading...",&lv_font_montserrat_16,CYAN);mapCaption=text(page,278,453,510,20,"Loading weather scans...",&lv_font_montserrat_14,MUTED);
    windPage=screen(flight);windTitle=text(windPage,12,10,362,24,"SAN DIEGO / WIND",&lv_font_montserrat_18,CYAN);
    button(windPage,384,3,86,"24H",[](lv_event_t*){weekly=false;chosenDay=-1;tick();});button(windPage,478,3,94,"7 DAYS",[](lv_event_t*){weekly=true;tick();});button(windPage,580,3,86,"MAP",[](lv_event_t*){show(true);});button(windPage,674,3,114,"FLIGHTS",[](lv_event_t*){show(false);});
    windAge=text(windPage,12,41,776,16,"Loading forecast...",&lv_font_montserrat_12,MUTED);
    windCanvas=lv_obj_create(windPage);lv_obj_remove_style_all(windCanvas);lv_obj_set_pos(windCanvas,0,60);lv_obj_set_size(windCanvas,800,420);lv_obj_clear_flag(windCanvas,LV_OBJ_FLAG_SCROLLABLE);lv_obj_add_event_cb(windCanvas,windDraw,LV_EVENT_DRAW_MAIN,nullptr);
    lv_obj_add_event_cb(windCanvas,[](lv_event_t*){if(!weekly||!dailyOK)return;auto in=lv_indev_get_act();if(!in)return;lv_point_t p;lv_indev_get_point(in,&p);if(p.x>=112&&p.x<784&&p.y>=60&&p.y<370){chosenDay=(p.x-112)/96;weekly=false;tick();}},LV_EVENT_CLICKED,nullptr);
    infoPage=screen(flight);text(infoPage,24,20,650,28,"ABOUT THIS DISPLAY",&lv_font_montserrat_24,CYAN);button(infoPage,686,14,90,"CLOSE",[](lv_event_t*){lv_obj_add_flag(infoPage,LV_OBJ_FLAG_HIDDEN);});
    auto credits=text(infoPage,24,68,752,392,
        "Aircraft: adsb.lol (ODbL). Callsign metadata: ADSBdb.\nRoutes are estimates checked against location, not live flight plans.\nAirline logos: Flightradar24; marks belong to their airlines.\n\nForecasts: Open-Meteo (CC BY 4.0), NOAA HRRR / GFS, ECMWF IFS.\nHRRR: first 24h. ECMWF and GFS: seven days. Winds: 10 m, knots.\nArrows point where air travels. Colors use a fixed 0-30 kt scale.\nOne temperature row: HRRR near term, ECMWF beyond it.\nDaily wind/gust are maxima; daily arrows are prevailing directions.\n\nSatellite / radar: NOAA nowCOAST. Past-hour observation loop.\nFrame time is radar time; satellite uses the nearest available scan.\nIR works at night. Visible needs daylight. +/- changes map extent.\nCoastline: NOAA NCEI, generalized. SAN: runway 09/27 endpoints.\n\nWi-Fi and location stay in NVS through application-only updates.",&lv_font_montserrat_14,MUTED);lv_label_set_long_mode(credits,LV_LABEL_LONG_WRAP);
    updateZoomButtons();
}
void tick(){
    if(!page)return;bool fresh=hrrrOK&&millis()-hrrrLoaded<3600000;String overview=fresh?whole(temperature)+" F / "+description(condition):"Weather unavailable";lv_label_set_text(summary,overview.c_str());lv_label_set_text(mapCurrent,overview.c_str());lv_obj_invalidate(compact);
    String title="SAN DIEGO / WIND";if(chosenDay>=0&&!weekly){tm t{};localtime_r(&days[0][chosenDay].time,&t);char s[24];strftime(s,sizeof(s),"%a %b %d",&t);title=String(s)+" / WIND";}lv_label_set_text(windTitle,title.c_str());
    bool stale=dailyOK&&millis()-dailyLoaded>12UL*3600000;String age=stale?"Forecast stale / showing last download":dailyOK?"HRRR: next 24h     ECMWF / GFS: 7 days     Updated "+String((millis()-dailyLoaded)/60000)+" min ago":"Loading forecast models...";lv_label_set_text(windAge,age.c_str());if(windVisible)lv_obj_invalidate(windCanvas);
    lv_label_set_text(imageStatus,imageMessage);if(imageMessage[0])lv_obj_clear_flag(imageStatus,LV_OBJ_FLAG_HIDDEN);else lv_obj_add_flag(imageStatus,LV_OBJ_FLAG_HIDDEN);
    if(visible&&playing&&due(millis(),nextFrame)){for(int i=1;i<=NFRAMES;i++){int next=(shownFrame+i+NFRAMES)%NFRAMES;if(frames[next].bytes){shownFrame=next;lv_img_set_src(imageObject,&frames[next].image);break;}}nextFrame=millis()+1200;}
    if(shownFrame>=0){char stamp[28];tm t{};localtime_r(&frames[shownFrame].time,&t);strftime(stamp,sizeof(stamp),"%I:%M %p %Z",&t);String s=String(time(nullptr)-frames[shownFrame].time>7200?"STALE / ":"")+stamp+" / "+(frameInfrared?"IR":"Visible")+" + radar / "+String(shownFrame+1)+"/6";lv_label_set_text(mapCaption,s.c_str());}else lv_label_set_text(mapCaption,"Loading satellite + radar...");
}
void service(){if(due(millis(),nextHRRR)){fetchHRRR();return;}if(due(millis(),nextDaily)){fetchDaily();return;}if(!visible)return;if(loadFrame>=0){if(due(millis(),nextImageTry))loadImage();return;}if(due(millis(),nextImages)){nextImages=millis()+120000;loadTimeline();}}
void diagnostics(){int n=0;for(const auto &f:frames)if(f.bytes)n++;Serial.printf("WEATHER: hrrr=%d hours=%d models=%d hours=%d map=%d wind=%d weekly=%d frames=%d shown=%d mode=%s zoom=%dx load=%d failures=%d\n",hrrrOK,hourCount,dailyOK,modelHourCount,visible,windVisible,weekly,n,shownFrame,infrared?"IR":"visible",1<<mapZoom,loadFrame,imageFailures);}
}

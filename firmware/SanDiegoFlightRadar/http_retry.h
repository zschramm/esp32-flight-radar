#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <limits.h>

namespace feed_retry {
// Retry-After is seconds or an HTTP date. X-RateLimit-Reset is commonly
// seconds (Envoy) or a Unix timestamp. Do not mistake an epoch for a day-long wait.
inline bool number(const char *text, uint64_t &value) {
    if(!text) return false;
    while(isspace(static_cast<unsigned char>(*text))) ++text;
    if(!isdigit(static_cast<unsigned char>(*text))) return false;
    char *end=nullptr; value=strtoull(text,&end,10);
    while(isspace(static_cast<unsigned char>(*end))) ++end;
    return !*end;
}
inline uint32_t seconds(uint64_t value) {
    return value>UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(value);
}
inline bool httpDate(const char *text, int64_t &epoch) {
    int day,year,hour,minute,second; char month[4]{},zone[4]{},extra;
    if(!text || sscanf(text,"%*3s, %d %3s %d %d:%d:%d %3s %c",&day,month,&year,&hour,&minute,&second,zone,&extra)!=7) return false;
    const char *names[]={"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
    const int lengths[]={31,28,31,30,31,30,31,31,30,31,30,31};
    int m=0; while(m<12 && strcmp(names[m],month)) ++m;
    bool leap=year%4==0 && (year%100!=0 || year%400==0);
    if(m==12 || year<1970 || year>9999 || strcmp(zone,"GMT") || hour<0 || hour>23 || minute<0 || minute>59 || second<0 || second>59
        || day<1 || day>lengths[m]+(m==1 && leap)) return false;
    // UTC conversion independent of the display's local timezone.
    int64_t days=365LL*(year-1970)+(year-1)/4-1969/4-(year-1)/100+1969/100+(year-1)/400-1969/400;
    for(int i=0;i<m;i++) days+=lengths[i]+(i==1 && leap);
    epoch=(days+day-1)*86400+hour*3600+minute*60+second;
    return true;
}
inline uint32_t delaySeconds(const char *retryAfter,const char *reset,int64_t now) {
    uint64_t value=0; int64_t date=0;
    if(number(retryAfter,value)) return seconds(value);
    if(httpDate(retryAfter,date)) return date>now?seconds(uint64_t(date-now)):0;
    if(number(reset,value)) {
        if(value>=1000000000ULL) return value>uint64_t(now)?seconds(value-uint64_t(now)):0;
        return seconds(value);
    }
    return 120;
}
}

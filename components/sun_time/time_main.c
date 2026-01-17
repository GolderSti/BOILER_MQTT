#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "fixedpoint.h"
#include "cordic_helper.h"

#define ZENITH FROMFLOAT(-.83f)

/*
    Computes the sunrise/sunset.
    Based on the algorithm described here:
    - https://stackoverflow.com/questions/7064531/sunrise-sunset-times-in-c
    - https://edwilliams.org/sunrise_sunset_algorithm.htm
    - https://www.edwilliams.org/sunrise_sunset_example.htm

    @param dayOfYear The day of year (1 - 365).
    @param lat The latitude, in decimal degrees.
    @param lng The longitude, in decimal degrees.
    @param localOffset The timezone offset in minutes.
    @param daylightSavings Whether DST is in effect. 1 = DST in effect, 0 = no DST.
    @param sunrise Whether to compute the sunrise. Otherwise, the sunset will be computed.
    @return The sunrise/sunset, in minutes since 00:00 of local time. */
static int calculateSunriseSunset(int dayOfYear, fixedfloat_t lat, fixedfloat_t lng, int localOffset, int daylightSavings, int sunrise);

static fixedfloat_t sunLocalHourAngle(fixedfloat_t lat, fixedfloat_t sinDec, fixedfloat_t cosDec);

/**
  * Computes the day of year (where 1 = January 1st).
  * Explained here: https://astronomy.stackexchange.com/questions/2407/calculate-day-of-the-year-for-a-given-date
  * @param year The year
  * @param month The month within the year (1-12)
  * @param day The day of the month (1-31)
  */
static int computeDayOfYear(int year, int month, int day) {
    //1. first calculate the day of the year
    int N1 = 275 * month / 9;
    int N2 = (month + 9) / 12;
    int N3 = (1 + ((year - 4 * (year / 4) + 2) / 3));
    int N = N1 - (N2 * N3) + day - 30;

    return N;
}

static int calculateSunriseSunset(int dayOfYear, fixedfloat_t lat, fixedfloat_t lng, int localOffset, int daylightSavings, int sunrise) {
    fixedfloat_t sinDec;
    fixedfloat_t cosDec;
    fixedfloat_t cosH;
    fixedfloat_t H;
    fixedfloat_t time;
    int utc;
    fixedfloat_t t;
    fixedfloat_t M;
    fixedfloat_t L;
    fixedfloat_t RA;
    int Lquadrant;
    int RAquadrant;
    int r;
    fixedfloat_t temp;

    //2. convert the longitude to hour value and calculate an approximate time
    const fixedfloat_t lngHour = IFDIV(lng, FROMINT(15));
    t = FROMINT(dayOfYear) + IFDIV((FROMINT(sunrise ? 6 : 18) - lngHour), FROMINT(24));

    //3. calculate the Sun's mean anomaly  
    // from, https://physics.stackexchange.com/questions/80034: M = nt + M0, where n = 0.9856, M0 = 3.289
    //M = (0.9856f * t) - 3.289f;
    M = IFMUL(FROMFLOAT(0.9856f), t) - FROMFLOAT(3.289f);

    //4. calculate the Sun's true longitude
    //L = fmodf(M + (1.916f * sin((PI / 180)*M)) + (0.020f * sin(2 * (PI / 180) * M)) + 282.634f, 360.0f);
    // Break up this expression to avoid arithmetic overflows.
    temp = M + (IFMUL(FROMFLOAT(1.916f), sin(RADIANS(M)))) + IFMUL(FROMFLOAT(0.020f), sin(RADIANS(M) << 1));
    if (TOINT(temp) + 283 >= 360) {
        temp -= FROMINT(360); // Avoid overflow.
    }
    L = IFMOD(temp + FROMFLOAT(282.634f), FROMINT(360));

    //5a. calculate the Sun's right ascension     
    //RA = fmodf(180 / PI * atan(0.91764f * tan((PI / 180)*L)), 360.0f);
    RA = IFMOD(DEGREES(atan(IFMUL(FROMFLOAT(0.91764f), tan(RADIANS(L))))), FROMINT(360));

    //5b. right ascension value needs to be in the same quadrant as Lval
    Lquadrant  = TOINT(IFDIV(L, FROMINT(90))) * 90;
    RAquadrant = TOINT(IFDIV(RA, FROMINT(90))) * 90;
    RA = RA + FROMINT(Lquadrant - RAquadrant);

    //5c. right ascension value needs to be converted into hours   
    RA = IFDIV(RA, FROMINT(15));

    //6. calculate the Sun's declination
    //sinDec = 0.39782f * sin((PI / 180) * L);
    sinDec = IFMUL(FROMFLOAT(0.39782f), sin(RADIANS(L)));
    cosDec = cos(asin(sinDec));

    //7a. calculate the Sun's local hour angle
    //cosH = (sin((PI/180)*ZENITH) - (sinDec * sin((PI/180)*lat))) / (cosDec * cos((PI/180)*lat));
    cosH = sunLocalHourAngle(lat, sinDec, cosDec);

    /*
    if (cosH >  1) 
    the sun never rises on this location (on the specified date)
    if (cosH < -1)
    the sun never sets on this location (on the specified date)
    */

    //7b. finish calculating H and convert into hours
    H = sunrise ? FROMINT(360) - DEGREES(acos(cosH)) : DEGREES(acos(cosH));
    H = IFDIV(H, FROMINT(15));

    //8. calculate local mean time of rising/setting      
    //time = H + RA - (0.06571 * t) - 6.622;
    time = H + RA - IFMUL(FROMFLOAT(0.06571), t) - FROMFLOAT(6.622);

    //9. adjust back to UTC
    utc = (TOINT(time - lngHour) % 24 * 60) + TOINT(IFMUL(FRAC(time - lngHour), FROMINT(60)));

    //10. convert UTC value to local time zone of latitude/longitude
    r = utc + localOffset + daylightSavings * 60;

    return r < 0 ? r + (24 * 60) : (r % (24 * 60));
}

static fixedfloat_t sunLocalHourAngle(fixedfloat_t lat, fixedfloat_t sinDec, fixedfloat_t cosDec) {
    //cosH = (sin((PI/180)*ZENITH) - (sinDec * sin((PI/180)*lat))) / (cosDec * cos((PI/180)*lat));
    fixedfloat_t sinValue, cosValue;
    sincos(RADIANS(lat), &sinValue, &cosValue);
    return IFDIV(sin(RADIANS(ZENITH)) - IFMUL(sinDec, sinValue), IFMUL(cosDec, cosValue));
}

int main(int argc, char *argv[]) {
    time_t now;
    float lat;
    float lng;
    int localOffset;
    struct tm *tm;
    int localSunriseT, localSunsetT;

    if (argc != 4) {
        printf("%s <latitude> <longitude> <timezone offset in minutes>\nExample: %s 1.3364926804464332 103.74412865673372 480\n", argv[0], argv[0]);
        return EINVAL;
    }

    lat = (float) atof(argv[1]);
    lng = (float) atof(argv[2]);
    localOffset = atoi(argv[3]);

    printf("Coordinates: %f,%f\nTimezone Offset: %d\n", lat, lng, localOffset);

    now = time(NULL);
    tm = localtime(&now);
    // Get the time of day.
    //const int dayOfYear = computeDayOfYear(tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday);
    const int dayOfYear = tm->tm_yday + 1; // If you have a source for the day in the year (1 to 365, inclusive).
    const int dst = tm->tm_isdst;
    // OR, set it manually:
    /* const int dayOfYear = computeDayOfYear(2021, 1, 1);
       const int dst = 0; */
    localSunriseT =  calculateSunriseSunset(dayOfYear, FROMFLOAT(lat), FROMFLOAT(lng), localOffset, dst, 1);
    localSunsetT =  calculateSunriseSunset(dayOfYear, FROMFLOAT(lat), FROMFLOAT(lng), localOffset, dst, 0);

    printf("Sunrise: %02d:%02d\nSunset: %02d:%02d\n", localSunriseT / 60, localSunriseT % 60, localSunsetT / 60, localSunsetT % 60);

    return 0;
}
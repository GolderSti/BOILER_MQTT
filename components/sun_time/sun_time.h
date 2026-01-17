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
int calculateSunriseSunset(int dayOfYear, fixedfloat_t lat, fixedfloat_t lng, int localOffset, int daylightSavings, int sunrise);

fixedfloat_t sunLocalHourAngle(fixedfloat_t lat, fixedfloat_t sinDec, fixedfloat_t cosDec);

/*
  *
  * Computes the day of year (where 1 = January 1st).
  * Explained here: https://astronomy.stackexchange.com/questions/2407/calculate-day-of-the-year-for-a-given-date
  * @param year The year
  * @param month The month within the year (1-12)
  * @param day The day of the month (1-31)
  */
int computeDayOfYear(int year, int month, int day);

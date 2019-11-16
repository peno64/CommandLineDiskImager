#include <time.h>

static int hour0, min0, sec0;
static long seconds0;

static long gettime(hour,min,sec)
 int *hour,*min,*sec;
 {
  time_t ltime;
  struct tm *splittime;

  time(&ltime);
  splittime=localtime(&ltime);
  *hour=splittime->tm_hour;
  *min=splittime->tm_min;
  *sec=splittime->tm_sec;
  return(*sec + (*min + (*hour) * 60l) * 60l);
 }

void initprogress()
{
  seconds0 = gettime(&hour0, &min0, &sec0);
}

static void formatseconds(long seconds, char *format)
{
  int hour, min;

  *format = 0;
  if (seconds >= 60 * 60)
  {
    hour = seconds / 60 / 60;
    seconds -= hour * 60 * 60;
    sprintf(format + strlen(format), "%dh", hour);
  }
  min = seconds / 60;
  seconds -= min * 60;
  sprintf(format + strlen(format), "%02dm", min);
  sprintf(format + strlen(format), "%02ds", seconds);
}

void progress(double percent, char *selapsedseconds, char *sestimatedseconds)
{
  int hour, min, sec;
  long seconds;
  long elapsedseconds;
  long estimatedseconds;

  seconds = gettime(&hour, &min, &sec);

  elapsedseconds = seconds - seconds0;

  formatseconds(elapsedseconds, selapsedseconds);

  if (percent != 0.0)
    estimatedseconds = elapsedseconds / percent;
  else
    estimatedseconds = 0;

  formatseconds(estimatedseconds, sestimatedseconds);
}

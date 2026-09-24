/* The launcher establishes the data working directory before file IO. This
 * helper deliberately does not infer a home directory or use executable paths:
 * installed application resources may be read-only and separate from data. */
#include "user_data_dir.h"

#include <stdio.h>

char *UserDataFile(char *buf, size_t size, const char *leaf) {
  if(!buf || !size)return buf;
  int n=leaf?snprintf(buf,size,"%s",leaf):-1;
  if(n<0 || (size_t)n>=size)buf[0]=0;
  return buf;
}

#include "postgres.h"

#include <stdio.h>
#include <time.h>

#include "dynamic_ext.h"

/*
 * Validator for opclass.
 */
bool devalidate(Oid opclassoid) {

  fprintf(stderr, "devalidate called at %ld\n", (long)time(NULL));
  return true;
}

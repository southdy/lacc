#ifndef PARSE_H
#define PARSE_H

#include "ir.h"

/* Parse input up to, and including, the next function definition. Updates the
 * global CFG structure.
 */
int parse(void);

#endif

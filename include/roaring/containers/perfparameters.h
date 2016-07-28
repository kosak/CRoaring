#ifndef PERFPARAMETERS_H_
#define PERFPARAMETERS_H_

/**
During lazy computations, we can transform array containers into bitset
containers as
long as we can expect them to have  ARRAY_LAZY_LOWERBOUND values.
*/
enum { ARRAY_LAZY_LOWERBOUND = 1024 };

/* default initial size of a run container */
enum { RUN_DEFAULT_INIT_SIZE = 4 };

/* default initial size of an array container */
enum { ARRAY_DEFAULT_INIT_SIZE = 16 };

#endif

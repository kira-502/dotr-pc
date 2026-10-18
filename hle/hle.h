/* Shared header of the HLE layer: the game's own headers plus the runtime services. */
#ifndef DOTR_HLE_H
#define DOTR_HLE_H

#include "common.h"
#include "hle_rt.h"

#define HLE_TODO(name) rt_log(1, "HLE: %s not done yet", name)

#endif

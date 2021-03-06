/*
 *  MaxSpeedOfSound_CUDA.cpp
 *  MPCFcore
 *
 *  Created by Fabian Wermelinger on 06/03/14.
 *  Copyright 2014 ETH Zurich. All rights reserved.
 */

#include "MaxSpeedOfSound_CUDA.h"
#include "GPU.h"


void MaxSpeedOfSound_CUDA::compute(const uint_t nslices)
{
    GPU::bind_textures();
    GPU::MaxSpeedOfSound(nslices);
    GPU::unbind_textures();
}

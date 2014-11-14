/* *
 * CUDA_Timer.cuh
 *
 * Created by Fabian Wermelinger on 5/21/14.
 * Copyright 2014 ETH Zurich. All rights reserved.
 * */
#pragma once

#include <cassert>

// timer (http://stackoverflow.com/questions/6959213/timing-a-cuda-application-using-events)
class EventTimer {
    public:
        EventTimer() : mStarted(false), mStopped(false) {
            cudaEventCreate(&mStart);
            cudaEventCreate(&mStop);
        }
        ~EventTimer() {
            cudaEventDestroy(mStart);
            cudaEventDestroy(mStop);
        }
        void start(cudaStream_t s = 0) { cudaEventRecord(mStart, s);
            mStarted = true; mStopped = false; }
        void stop(cudaStream_t s = 0)  { assert(mStarted);
            cudaEventRecord(mStop, s);
            mStarted = false; mStopped = true; }
        float elapsed() {
            assert(mStopped);
            if (!mStopped) return 0;
            cudaEventSynchronize(mStop);
            float elapsed = 0;
            cudaEventElapsedTime(&elapsed, mStart, mStop);
            return elapsed;
        }

    private:
        bool mStarted, mStopped;
        cudaEvent_t mStart, mStop;
};

typedef EventTimer GPUtimer;

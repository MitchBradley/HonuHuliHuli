// Step generation: a single RMT transmission per move, fed step by step
// by an encoder callback in the RMT refill interrupt.
#pragma once

#include <cstdint>

namespace motion {

enum class Mode : uint8_t {
    Idle,      // at rest
    Index,     // a relative index move (presses extend it)
    Run,       // continuous rotation
    Stopping,  // decelerating from continuous rotation
};

struct Status {
    Mode     mode;
    int32_t  remaining_s;  // seconds left in a timed run, -1 if not timed
    bool     uv;
    bool     enabled;
};

// Configure pins, build the ramp table, create the RMT channel.
void init();

// Start continuous rotation, stopping after seconds (0 = untimed).  During
// an index move it starts once the move ends.  During deceleration it
// accelerates again from the current speed.
void start(uint32_t seconds = 0);

// Stop continuous rotation: decelerate at once.  Also cancels a run that
// was queued behind an index move.
void stop();

// Index n eighths of a turn from wherever the table is.  Extends an index
// move in progress; queued after a stop in progress; ignored while running.
void index(int n = 1);

void enable(bool on);

Status status();

}  // namespace motion

// player.h - the game's 2B: where it keeps her position, moved by your real steps (first person).
#pragma once
#include "core/vecmath.h"

// Moves her by `step` (world, horizontal) if her object is found and is where her bones are; false if not.
bool PlayerMove(pure::V3 step);

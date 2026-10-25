// Player position and facing hooks.
#pragma once
#include "core/vecmath.h"

// Moves the character by `step` (world, horizontal) if their object is found and is where their bones are; false if not.
bool PlayerMove(pure::V3 step);
// Finds the facing value and turns the character toward lookForward after an attack starts.
void PlayerUpdate(pure::V3 lookForward);

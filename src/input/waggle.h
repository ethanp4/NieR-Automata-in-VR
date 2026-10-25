#include <stdint.h>

struct SwingDetector {
	bool armed = true;
	uint32_t lastFire = 0;
	bool fired = false;
	bool update(float speed, uint32_t t, float fire, float rearm = 0.5f, uint32_t cooldownMs = 250) {
		if (!armed && speed < fire * rearm) armed = true;
		if (!armed || speed < fire) return false;
		if (fired && t - lastFire < cooldownMs) return false;
		armed = false;
		fired = true;
		lastFire = t;
		return true;
	}
};
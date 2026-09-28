
#include "EventGuard.h"

namespace qt_event_watcher {

EventGuard::EventGuard() {
	start();
}

void EventGuard::start() {
	m_timer.start();
}

std::int64_t EventGuard::elapsedNs() const {
	return m_timer.nsecsElapsed();
}

double EventGuard::elapsedMs() const {
	return static_cast<double>(elapsedNs()) / 1000000.0;
}

};


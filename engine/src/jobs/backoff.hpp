#pragma once

#include <chrono>

namespace poni::jobs {

// Delay before a failed job runs again. `attempt` is the number of attempts
// made so far (1 after the first failure): 30 s, 1 min, 2 min, ... capped at 1 hour.
std::chrono::seconds retry_delay(int attempt);

}  // namespace poni::jobs

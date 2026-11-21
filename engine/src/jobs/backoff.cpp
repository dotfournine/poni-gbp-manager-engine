#include "jobs/backoff.hpp"

#include <algorithm>

namespace poni::jobs {

std::chrono::seconds retry_delay(int attempt) {
    constexpr long long kBase = 30;
    constexpr long long kMax = 3600;
    const int shift = std::clamp(attempt - 1, 0, 20);
    return std::chrono::seconds(std::min(kBase << shift, kMax));
}

}  // namespace poni::jobs

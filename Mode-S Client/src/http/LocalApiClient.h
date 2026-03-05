#pragma once
#include <string>

struct Metrics;

// Fetches /api/metrics from the local Mode-S Client API and parses into Metrics.
// Returns true on success.
bool TryFetchMetricsFromApi(Metrics& out);

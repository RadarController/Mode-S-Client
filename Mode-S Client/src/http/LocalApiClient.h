#pragma once

// Local API client helpers (calling our own HttpServer endpoints).
// Extracted from Mode-S Client.cpp as part of refactor Issue 1.2.

struct Metrics;

// Pull metrics through the local HTTP endpoint so the UI reflects exactly what overlays/OBS see.
bool TryFetchMetricsFromApi(Metrics& out);

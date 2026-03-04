#include "SimConnectWorker.h"

#include <chrono>
#include <cmath>

// SimConnect is only needed in the .cpp
#include <SimConnect.h>

#pragma comment(lib, "SimConnect.lib")

namespace simconnect {

namespace {

// What we ask SimConnect for (one packet).
enum DATA_DEFINE_ID : DWORD {
    DEF_AIRCRAFT_STATE = 1,
};

enum DATA_REQUEST_ID : DWORD {
    REQ_AIRCRAFT_STATE = 1,
};

#pragma pack(push, 1)
struct AircraftStateData {
    double altitude_ft;     // PLANE ALTITUDE (feet)
    double groundspeed_kts; // GROUND VELOCITY (knots)
    double indicated_kts;   // AIRSPEED INDICATED (knots)
    double lat_deg;         // PLANE LATITUDE (degrees)
    double lon_deg;         // PLANE LONGITUDE (degrees)
};
#pragma pack(pop)

} // namespace

std::int64_t SimConnectWorker::NowUnix() {
    // kept for compatibility if used elsewhere; returns seconds
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

std::int64_t SimConnectWorker::NowUnixMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

SimConnectWorker::SimConnectWorker() = default;

SimConnectWorker::~SimConnectWorker() {
    Stop();
}

void SimConnectWorker::Start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) return;
    th_ = std::thread(&SimConnectWorker::Run, this);
}

void SimConnectWorker::Stop() {
    if (!running_) return;
    running_ = false;
    if (th_.joinable()) th_.join();

    // Ensure disconnected on stop
    std::lock_guard<std::mutex> lk(mu_);
    if (hSimConnect_) {
        SimConnect_Close((HANDLE)hSimConnect_);
        hSimConnect_ = nullptr;
    }
    SetDisconnectedLocked();
}

SimStateSnapshot SimConnectWorker::GetSnapshot() const {
    std::lock_guard<std::mutex> lk(mu_);
    return snap_;
}

void CALLBACK SimConnectWorker::DispatchThunk(SIMCONNECT_RECV* pData, DWORD cbData, void* pContext) {
    auto* self = reinterpret_cast<SimConnectWorker*>(pContext);
    if (!self) return;
    self->HandleDispatch(pData, cbData);
}

void SimConnectWorker::SetDisconnectedLocked() {
    snap_.connected = false;
    snap_.has_position = false;
    snap_.has_altitude = false;
    snap_.has_gs = false;
    snap_.has_ias = false;
    // keep last numeric values (optional) but mark them as not present
}

void SimConnectWorker::HandleDispatch(SIMCONNECT_RECV* pData, DWORD /*cbData*/) {
    if (!pData) return;

    std::lock_guard<std::mutex> lk(mu_);

    switch (pData->dwID) {
    case SIMCONNECT_RECV_ID_EXCEPTION:
        // Something went wrong; keep running but mark disconnected if needed
        break;

    case SIMCONNECT_RECV_ID_QUIT:
        // Simulator is quitting; drop connection
        if (hSimConnect_) {
            SimConnect_Close((HANDLE)hSimConnect_);
            hSimConnect_ = nullptr;
        }
        SetDisconnectedLocked();
        break;

    case SIMCONNECT_RECV_ID_SIMOBJECT_DATA: {
        auto* obj = reinterpret_cast<SIMCONNECT_RECV_SIMOBJECT_DATA*>(pData);
        if (!obj) break;
        if (obj->dwRequestID != REQ_AIRCRAFT_STATE) break;

        const auto* d = reinterpret_cast<const AircraftStateData*>(&obj->dwData);
        if (!d) break;

        snap_.connected = true;
        snap_.has_altitude = true;
        snap_.has_gs = true;
        snap_.has_ias = true;
        snap_.has_position = true;

        snap_.altitude_ft = d->altitude_ft;
        snap_.ground_speed_kts = d->groundspeed_kts;
        snap_.indicated_airspeed_kts = d->indicated_kts;
        snap_.lat_deg = d->lat_deg;
        snap_.lon_deg = d->lon_deg;

        snap_.ts_unix_ms = NowUnixMs();
        snap_.last_update_unix = NowUnix();break;
    }

    default:
        break;
    }
}

void SimConnectWorker::Run() {
    using namespace std::chrono;

    // Retry loop: if MSFS isn't running, SimConnect_Open will fail; we wait and try again.
    while (running_) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (hSimConnect_) {
                // already connected; fall through to polling below
            }
        }

        // Connect if not connected
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (!hSimConnect_) {
                HANDLE h = nullptr;
                HRESULT hr = SimConnect_Open(&h, "Mode-S Client", nullptr, 0, 0, 0);
                if (SUCCEEDED(hr) && h) {
                    hSimConnect_ = (void*)h;

                    // Define the data we want
                    SimConnect_AddToDataDefinition(h, DEF_AIRCRAFT_STATE, "PLANE ALTITUDE", "feet");
                    SimConnect_AddToDataDefinition(h, DEF_AIRCRAFT_STATE, "GROUND VELOCITY", "knots");
                    SimConnect_AddToDataDefinition(h, DEF_AIRCRAFT_STATE, "AIRSPEED INDICATED", "knots");
                    SimConnect_AddToDataDefinition(h, DEF_AIRCRAFT_STATE, "PLANE LATITUDE", "degrees");
                    SimConnect_AddToDataDefinition(h, DEF_AIRCRAFT_STATE, "PLANE LONGITUDE", "degrees");

                    // Request updates ~2Hz (every 0.5s)
                    SimConnect_RequestDataOnSimObject(
                        h,
                        REQ_AIRCRAFT_STATE,
                        DEF_AIRCRAFT_STATE,
                        SIMCONNECT_OBJECT_ID_USER,
                        SIMCONNECT_PERIOD_SECOND,
                        SIMCONNECT_DATA_REQUEST_FLAG_DEFAULT,
                        0,  // origin
                        0,  // interval (seconds)
                        0   // limit
                    );

                    snap_.connected = true;
                    snap_.ts_unix_ms = NowUnixMs();
                    snap_.last_update_unix = NowUnix();
                } else {
                    // Not running yet / can't connect
                    SetDisconnectedLocked();
                }
            }
        }

        // If connected, pump dispatch and sleep a bit.
        HANDLE hLocal = nullptr;
        {
            std::lock_guard<std::mutex> lk(mu_);
            hLocal = (HANDLE)hSimConnect_;
        }

        if (hLocal) {
            // Pump callbacks
            SimConnect_CallDispatch(hLocal, &SimConnectWorker::DispatchThunk, this);

            // If we haven't had an update in a while, consider it stale/disconnected
            {
                std::lock_guard<std::mutex> lk(mu_);
                if (snap_.connected) {
                    auto age = NowUnix() - snap_.last_update_unix;
                    if (snap_.last_update_unix != 0 && age > 5) {
                        // keep connection handle but mark stale (MSFS paused / menus etc.)
                        // We don't flip connected=false here, just leave it true; overlay can use age if you expose it.
                    }
                }
            }

            std::this_thread::sleep_for(milliseconds(200));
        } else {
            // Not connected; retry slower
            std::this_thread::sleep_for(milliseconds(750));
        }
    }

    // Cleanup on exit handled in Stop()
}

} // namespace simconnect

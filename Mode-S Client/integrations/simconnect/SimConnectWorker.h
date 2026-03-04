#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>

#ifdef _WIN32
  #include <Windows.h>
#endif

// Forward declaration so consumers don't need SimConnect.h included here.
struct SIMCONNECT_RECV;

namespace simconnect {

struct SimStateSnapshot {
    // Connection state
    bool connected = false;

    // Presence flags (what HttpServer expects)
    bool has_position = false;
    bool has_altitude = false;
    bool has_gs = false;
    bool has_ias = false;

    // Values (names expected by HttpServer)
    double lat_deg = 0.0;
    double lon_deg = 0.0;

    double altitude_ft = 0.0;              // feet
    double ground_speed_kts = 0.0;         // knots (GS)
    double indicated_airspeed_kts = 0.0;   // knots (IAS)

    // Timestamp in milliseconds (what HttpServer expects)
    std::int64_t ts_unix_ms = 0;


    // Internal (seconds)
    std::int64_t last_update_unix = 0;
};



class SimConnectWorker {
public:
    SimConnectWorker();
    ~SimConnectWorker();

    SimConnectWorker(const SimConnectWorker&) = delete;
    SimConnectWorker& operator=(const SimConnectWorker&) = delete;

    void Start();
    void Stop();

    SimStateSnapshot GetSnapshot() const;

private:
    // SimConnect dispatch callback must be a plain function pointer; static member works.
    static void CALLBACK DispatchThunk(SIMCONNECT_RECV* pData, DWORD cbData, void* pContext);

    void Run();
    void HandleDispatch(SIMCONNECT_RECV* pData, DWORD cbData);

    void SetDisconnectedLocked();
    static std::int64_t NowUnix();
    static std::int64_t NowUnixMs();

private:
    std::atomic<bool> running_{false};
    std::thread       th_;

    // Guard all snapshot + simconnect handle state.
    mutable std::mutex mu_;
    SimStateSnapshot   snap_;

    void* hSimConnect_ = nullptr; // kept as void* here; real type is HANDLE from SimConnect.h
};

} // namespace simconnect

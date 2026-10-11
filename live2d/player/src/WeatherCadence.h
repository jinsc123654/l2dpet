#pragma once

// Monotonic model-session timer. Sleep/failure never consumes a successful reminder.
class WeatherCadence
{
    double _next = 0;
public:
    void Reset(double now) { _next = now + 300.0; }
    bool Due(double now) const { return now >= _next; }
    void Delivered(double now) { _next = now + 3600.0; }
    void RetryLater(double now) { _next = now + 300.0; }
};

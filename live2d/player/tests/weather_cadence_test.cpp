#include "../src/WeatherCadence.h"
#include <cassert>
#include <iostream>
int main()
{
    WeatherCadence timer;
    timer.Reset(100);
    assert(!timer.Due(399.999));
    assert(timer.Due(400));
    // Passing time while sleeping must not consume the reminder or create a burst.
    assert(timer.Due(8000));
    timer.Delivered(8000);
    assert(!timer.Due(11599.999) && timer.Due(11600));
    timer.RetryLater(11600);
    assert(!timer.Due(11899) && timer.Due(11900));
    // Model switch starts a fresh five-minute timer.
    timer.Reset(12000);
    assert(!timer.Due(12299) && timer.Due(12300));
    std::cout << "PASS first five minutes, hourly delivery, sleep, retry and model restart\n";
}

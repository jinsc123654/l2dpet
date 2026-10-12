#pragma once
// System startup adapter. UI reads actual system state instead of persisting a duplicate flag.
namespace desktop {
bool AutostartEnabled();
bool SetAutostart(bool enabled);
}

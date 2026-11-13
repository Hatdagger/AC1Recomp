/* Master of Arena: update check and self-update from the project's GitHub
 * releases. Nothing here runs by itself: the launcher calls check() or
 * download_and_apply() when the player asks.
 *
 * An update replaces only the program's own files. Memory cards, mods, disc
 * images and the player's settings are never read, moved or overwritten.
 */
#pragma once
#include <cstdint>
#include <string>

namespace moa_update {

enum class State {
    Idle,          /* nothing asked yet */
    Checking,
    UpToDate,
    Available,     /* a newer release exists */
    Downloading,
    Applying,
    Done,          /* installed; restart to use it */
    Failed,
};

struct Snapshot {
    State       state = State::Idle;
    std::string current;      /* this build's version, e.g. "0.3.0" */
    std::string latest;       /* newest release found, e.g. "0.3.1" */
    std::string message;      /* last result, one line */
    uint64_t    done = 0;     /* bytes downloaded */
    uint64_t    total = 0;    /* bytes expected, 0 when unknown */
};

const char* current_version();
/* True when this build can replace itself; otherwise why not. */
bool supported(std::string* why);
Snapshot snapshot();
void check();                 /* asks GitHub which release is newest */
void download_and_apply();    /* only after check() found one */

} /* namespace moa_update */

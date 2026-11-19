#pragma once

// Trainer bundle installation at Steam start.
//
// A trainer is a ZIP published by the trainers backend, bound to one appid and
// assigned to the signed-in account. There is no UI and no catalog: the list is
// whatever the backend answers for this steamid, and this unit brings every
// pending trainer up to date and shows a single summary at the end.
//
// Start() returns immediately and does the work on a detached worker, so it is
// safe to call from the init thread (never from DllMain):
//
//   1. wait up to 10 minutes for the session SteamID (none → idle);
//   2. GET the per-user manifest, retrying every 30 s for at most 10 minutes;
//   3. resolve each entry's game folder and skip absent or partially installed
//      games and trainers whose recorded version and files are already intact;
//   4. per remaining item, sequentially: download into
//      <Steam>\baestim\trainers\cache, verify SHA-256 + size, extract into a
//      fresh staging directory, copy into the game folder, then record the
//      install in <Steam>\baestim\trainers\state.json.
//
// One worker per Steam session: Steam loads BaeStim.dll into several processes,
// so a named mutex (Local\BaeStim.Trainers) makes every later Start() a no-op.
// Failures are logged and retried on the next Steam start; nothing is removed.
namespace Trainers {

    // Starts the installer worker once per session. Always compiled in; the
    // [trainers] enabled switch is checked here, so a disabled feature costs a
    // mutex lookup at startup.
    void Start();

}

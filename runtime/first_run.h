#pragma once
// First start: no game disc next to the exe -> ask for the player's own Shadow the Hedgehog disc (ISO / GCM / RVZ /
// extracted folder), check it against the hashes of a clean USA disc (GUPE8P rev 0) and unpack it to <exe dir>\Shadow.
// The C++ twin of tools/game_import/gameimport.py; plan in docs/release/GAME-IMPORT.md.
#include <string>

namespace first_run {
bool disc_present(const std::string& root);   // root\sys\main.dol exists
// Shows the first-start window (or, with quiet, imports SHADOW_IMPORT_FROM without one). Returns the new disc folder,
// or "" when the player quit / nothing could be imported.
std::string run(bool quiet);
}  // namespace first_run

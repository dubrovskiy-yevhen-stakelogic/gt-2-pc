#pragma once
// The program's body: the command line and the dispatch of gt2game (game_main.cpp), separated from the platform's
// entry point (main.cpp is the Windows one; the Quest build's android_main calls the same function).

namespace gt2game {

// `argc` / `argv` as an entry point receives them; returns the process's exit code. Errors are reported on stderr,
// nothing escapes.
int GameMain(int argc, char** argv);

} // namespace gt2game

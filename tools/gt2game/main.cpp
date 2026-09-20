// The Windows entry point of gt2game. The program itself is game_main.cpp (GameMain: the command line and the
// dispatch of the modes); the Quest build's android_main will call the same function.
#include "game_main.h"

int main(int argc, char** argv) { return gt2game::GameMain(argc, argv); }

/*
 * Gatefold NX background player · automix_shared.cpp
 * Automix in the background (protocol 6): Gatefold's own planner and mixer (source/automix.cpp) and the analysis's reader
 * (source/mixan.cpp, of which the player uses Info::unpack only: it never analyses), built in as they are, so a join
 * played here is the one Gatefold's player makes, model for model.
 */
#include "../../source/automix.cpp"
#include "../../source/mixan.cpp"

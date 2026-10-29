/* Gatefold NX preview · stands in for source/ffdecoder.cpp when the preview is built without FFmpeg. */
#include "../../source/ffdecoder.hpp"

bool ffHandles(const std::string&) { return false; }
bool ffReadInfo(const std::string&, TrackInfo&, bool, bool) { return false; }
bool FfDecoder::open(const std::string&, int, int) { return false; }
void FfDecoder::close() {}
int FfDecoder::read(int16_t*, int) { return 0; }
bool FfDecoder::seek(double) { return false; }
bool FfDecoder::skip(double) { return false; }
void FfDecoder::prefetch(int, int) {}
bool FfDecoder::fillAhead(int) { return false; }
void ffFeederStart() {}
void ffFeederStop() {}
unsigned ffStarved() { return 0; }

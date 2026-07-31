#include "SignalGenerator.h"

void SignalGenerator::prepare (double sr, int bs)
{
    sampleRate = sr;
    blockSize = bs;
    reset();
}

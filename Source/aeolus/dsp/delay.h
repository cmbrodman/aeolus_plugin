// ----------------------------------------------------------------------------
//
//  Copyright (C) 2021 Arthur Benilov <arthur.benilov@gmail.com>
//
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation; either version 3 of the License, or
//  (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
// ----------------------------------------------------------------------------

#pragma once

#include "aeolus/globals.h"

#include <vector>

AEOLUS_NAMESPACE_BEGIN

namespace dsp {

/**
* @brief Delay line with samples linear interpolation.
*/
class DelayLine
{
public:

    DelayLine(size_t size = 1024);
    void resize(size_t size);
    void reserve(size_t capacity) { _buffer.reserve(capacity); }
    void reset();

    /**
     * Reset the write position and clear only the slots that reads can reach
     * before they are overwritten, given that no read index exceeds
     * maxReadIndex (readNearest(d) uses index d; read(d) uses floor(d) and
     * floor(d) + 1). Writes advance backwards from slot 0, so each slot past
     * that prefix is written before any bounded read reaches it. Subsequent
     * output is identical to a full reset(). A bound outside the buffer falls
     * back to a full clear.
     */
    void resetForReadIndex(int maxReadIndex);
    void write(float x);
    float read(float delay) const;
    float readNearest(int delay) const;

    size_t size() const { return _buffer.size(); }

private:
    std::vector<float> _buffer;
    size_t _writeIndex;
};


/**
 * Whether voices clear their whole delay rings (original behaviour) instead
 * of the readable prefix. Fixed by the AEOLUS_BOUNDED_VOICE_CLEAR build
 * option; test builds may switch it at run time to compare both.
 */
bool useFullVoiceClear() noexcept;

#if AEOLUS_TESTING
void setFullVoiceClearForTesting(bool shouldUseFullClear) noexcept;
#endif

} // namespace dsp

AEOLUS_NAMESPACE_END

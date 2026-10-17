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

#include "aeolus/dsp/delay.h"

AEOLUS_NAMESPACE_BEGIN

namespace dsp {

DelayLine::DelayLine(size_t size)
    : _buffer(size, 0.0f)
    , _writeIndex(0)
{
}

void DelayLine::resize (size_t size)
{
    _buffer.resize(size);
    reset();
}

void DelayLine::reset()
{
    _writeIndex = 0;
    ::memset(_buffer.data(), 0, sizeof (float) * _buffer.size());
}

void DelayLine::write (float x)
{
    if (_writeIndex == 0)
        _writeIndex = _buffer.size() - 1;
    else
        --_writeIndex;

    _buffer[_writeIndex] = x;
}

float DelayLine::read(float delay) const
{
    int index = (int)std::floor(delay);
    float frac = delay - (float)index;

    index = (index + _writeIndex) % (int) _buffer.size();
    const auto a = _buffer[index];
    const auto b = index < _buffer.size() - 1 ? _buffer[index + 1] : _buffer[0];

    return math::lerp(a, b, frac);
}

float DelayLine::readNearest(int delay) const
{
    const int index{ int ((delay + _writeIndex) % _buffer.size()) };
    return _buffer[index];
}

void DelayLine::processNearest(float* samples, size_t count, int delay, float gain)
{
    if (count == 0) return;
    const size_t length = _buffer.size();
    jassert(length > 0 && delay >= 0 && size_t(delay) <= length);
    size_t writeIndex = _writeIndex;
    // Wrap just once per block; subsequent samples advance both cursors.
    size_t readIndex = writeIndex + (size_t(delay) == length ? 0 : size_t(delay));
    if (readIndex >= length) readIndex -= length;
    auto* ring = _buffer.data();
    for (size_t i = 0; i < count; ++i) {
        writeIndex = writeIndex == 0 ? length - 1 : writeIndex - 1;
        readIndex = readIndex == 0 ? length - 1 : readIndex - 1;
        ring[writeIndex] = samples[i];
        samples[i] = ring[readIndex] * gain;
    }
    _writeIndex = writeIndex;
}

} // namespace dsp

AEOLUS_NAMESPACE_END

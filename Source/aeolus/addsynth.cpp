// ----------------------------------------------------------------------------
//
//  Copyright (C) 2021 Arthur Benilov <arthur.benilov@gmail.com>
//  Copyright (C) 2003-2013 Fons Adriaensen <fons@linuxaudio.org>
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

#include "addsynth.h"

using namespace juce;

AEOLUS_NAMESPACE_BEGIN

N_func::N_func()
    : _b{}
    , _v{}
{
    reset(0.0f);
}

void N_func::reset(float v)
{
    _b = 16;
    _v.fill(v);
}

void N_func::setValue(int idx, float v)
{
    if (! isPositiveAndBelow(idx, N_NOTES))
        return;

    _v [idx] = v;
    _b |= 1 << idx;

    int j = idx - 1;

    while (j >= 0 && ! (_b & (1 << j)))
        --j;

    if (j < 0) {
        while (++j != idx)
            _v [j] = v;
    } else {
        const float d = (_v [j] - v) / (j - idx);

        while (++j != idx)
            _v [j] = v + (j - idx) * d;
    }

    j = idx + 1;

    while ((j < N_NOTES) && ! (_b & (1 << j)))
        ++j;

    if (j > N_NOTES - 1) {
        while (--j != idx)
            _v [j] = v;
    } else {
        const float d = (_v [j] - v) / (j - idx);

        while (--j != idx)
            _v [j] = v + (j - idx) * d;
    }
}

void N_func::clearValue(int idx)
{
    if (!isPositiveAndBelow(idx, N_NOTES))
        return;

    int m = 1 << idx;

    if (! (_b & m) || (_b == m))
        return;

    _b ^= m;

    int j = idx - 1;

    while ((j >= 0) && ! (_b & (1 << j)))
        --j;

    int k = idx + 1;

    while ((k <= N_NOTES - 1) && ! (_b & (1 << k)))
        ++k;

    if ((j >= 0) && (k < N_NOTES)) {
        const float d = (_v [k] - _v [j]) / (k - j);

        for (int i = j + 1; i < k; i++)
            _v [i] = _v [j] + (i - j) * d;
    } else if (j >= 0) {
        const float d = _v [j];

        while (j < N_NOTES - 1)
            _v [++j] = d;
    } else if (k < N_NOTES) {
        const float d = _v [k];

        while (k > 0)
            _v [--k] = d;
    }
}

float N_func::getValue(int idx) const
{
    jassert(isPositiveAndBelow(idx, _v.size()));

    return _v[idx];
}

bool N_func::isSet(int idx) const
{
    jassert(isPositiveAndBelow(idx, _v.size()));

    return (_b & (1 << idx)) != 0;
}

float N_func::operator[](int note) const
{
    const int i = note / NOTES_GAP;
    const int k = note - NOTES_GAP * i;
    float v = _v [i];

    if (k) {
        // Apply linear interpolation if falls into the gap.
        jassert(isPositiveAndBelow(i + 1, _v.size()));

        v += k * (_v [i + 1] - v) / NOTES_GAP;
    }

    return v;
}

var N_func::toVar() const
{
    auto* obj = new DynamicObject();
    obj->setProperty("mask", _b);
    Array<var> varr;

    for (const auto v : _v)
        varr.add(v);

    obj->setProperty("values", varr);
    return var{obj};
}

void N_func::fromVar(const juce::var& v)
{
    if (auto* obj = v.getDynamicObject()) {
        _b = obj->getProperty("mask");

        if (auto* varr = obj->getProperty("values").getArray()) {
            if (varr->size() >= _v.size()) {
                for (int i = 0; i < _v.size(); ++i)
                    _v[i] = varr->getUnchecked(i);
            }
        }
    }
}

void N_func::write(OutputStream& stream) const
{
    stream.writeInt(_b);

    for (const auto v : _v)
        stream.writeFloat(v);
}

void N_func::read(InputStream& stream)
{
    _b = stream.readInt();

    for (int i = 0; i < _v.size(); ++i)
        _v[i] = stream.readFloat();
}

//==============================================================================

HN_func::HN_func()
    : _h{}
{
}

void HN_func::reset(float v)
{
    for (auto& h : _h)
        h.reset(v);
}

void HN_func::setValue(int idx, float v)
{
    jassert(isPositiveAndBelow(idx, N_NOTES));

    for (auto& h: _h)
        h.setValue(idx, v);
}

void HN_func::setValue(int harm, int idx, float v)
{
    jassert(isPositiveAndBelow(harm, _h.size()));
    jassert(isPositiveAndBelow(idx, N_NOTES));

    _h[harm].setValue(idx, v);
}

void HN_func::clearValue(int idx)
{
    jassert(isPositiveAndBelow(idx, N_NOTES));

    for (auto& h : _h)
        h.clearValue(idx);
}

void HN_func::clearValue(int harm, int idx)
{
    jassert(isPositiveAndBelow(harm, _h.size()));
    jassert(isPositiveAndBelow(idx, N_NOTES));

    _h[harm].clearValue(idx);
}

float HN_func::getValue(int harm, int idx) const
{
    jassert(isPositiveAndBelow(harm, _h.size()));
    jassert(isPositiveAndBelow(idx, N_NOTES));

    return _h[harm].getValue(idx);
}

bool HN_func::isSet(int harm, int idx) const
{
    jassert(isPositiveAndBelow(harm, _h.size()));
    jassert(isPositiveAndBelow(idx, N_NOTES));

    return _h[harm].isSet(idx);
}

var HN_func::toVar(int n) const
{
    Array<var> varr;

    const auto m = jmin(_h.size(), (size_t)n);

    for (int i = 0; i < m; ++i)
        varr.add(_h[i].toVar());

    return varr;
}

void HN_func::fromVar(const var& v)
{
    if (const auto* varr = v.getArray()) {
        for (int i = 0; i < juce::jmin(varr->size(), int(_h.size())); ++i)
            _h[(size_t)i].fromVar(varr->getUnchecked(i));
    }
}

void HN_func::write(OutputStream& stream, int n) const
{
    const auto m = jmin(_h.size(), (size_t)n);

    for (int i = 0; i < m; ++i)
        _h[i].write(stream);
}

void HN_func::read(InputStream& stream, int n)
{
    const auto m = jmin(_h.size(), (size_t)n);

    for (int i = 0; i < m; ++i)
        _h[i].read(stream);
}

//==============================================================================

Addsynth::Addsynth()
{
    reset();
}

void Addsynth::reset()
{
    _noteMin = NOTE_MIN;
    _noteMax = NOTE_MAX;
    _fn = 1;
    _fd = 1;

    _n_vol.reset(-20.0f);
    _n_ins.reset(0.0f);
    _n_off.reset(0.0f);
    _n_att.reset(0.01f);
    _n_atd.reset(0.0f);
    _n_dct.reset(0.01f);
    _n_dcd.reset(0.0f);
    _n_ran.reset(0.0f);
    _h_lev.reset(-100.0f);
    _h_ran.reset(0.0f);
    _h_att.reset(0.050f);
    _h_atp.reset(0.0f);
}

template<size_t L>
static void writeString(const String& string, OutputStream& stream)
{
    char buffer[L] = {0};
    strncpy(buffer, string.toRawUTF8(), jmin(L, (size_t)string.length()));
    stream.write(buffer, L);
}

var Addsynth::toVar() const
{
    auto* obj = new DynamicObject();

    obj->setProperty("version", defaultVersion);
    obj->setProperty("n_harm", N_HARM);
    obj->setProperty("note_min", _noteMin);
    obj->setProperty("note_max", _noteMax);
    obj->setProperty("fn", _fn);
    obj->setProperty("fd", _fd);

    obj->setProperty("name", _stopName);
    obj->setProperty("copyright", _copyright);
    obj->setProperty("mnemonic", _mnemonic);
    obj->setProperty("comments", _comments);

    obj->setProperty("n_vol", _n_vol.toVar());
    obj->setProperty("n_off", _n_off.toVar());
    obj->setProperty("n_ran", _n_ran.toVar());
    obj->setProperty("n_ins", _n_ins.toVar());
    obj->setProperty("n_att", _n_att.toVar());
    obj->setProperty("n_atd", _n_atd.toVar());
    obj->setProperty("n_dct", _n_dct.toVar());
    obj->setProperty("n_dcd", _n_dcd.toVar());

    obj->setProperty("h_lev", _h_lev.toVar());
    obj->setProperty("h_ran", _h_ran.toVar());
    obj->setProperty("h_att", _h_att.toVar());
    obj->setProperty("h_atp", _h_atp.toVar());

    return var{obj};
}

void Addsynth::fromVar(const juce::var& v)
{
    if (const auto* obj = v.getDynamicObject()) {
        int version = obj->getProperty("version");

        int nHarm = obj->getProperty("n_harm");

        if (nHarm == 0)
            nHarm = deprecated::N_HARM;

        _noteMin = obj->getProperty("note_min");
        _noteMax = obj->getProperty("note_max");

        if (_noteMax == deprecated::NOTE_MAX)
            _noteMax = NOTE_MAX;

        _fn = obj->getProperty("fn");
        _fd = obj->getProperty("fd");

        _stopName = obj->getProperty("name");
        _copyright = obj->getProperty("copyright");
        _mnemonic = obj->getProperty("mnemonic");
        _comments = obj->getProperty("comments");

        _n_vol.fromVar(obj->getProperty("n_vol"));
        _n_off.fromVar(obj->getProperty("n_off"));
        _n_ran.fromVar(obj->getProperty("n_ran"));

        if (version >= defaultVersion) {
            _n_ins.fromVar(obj->getProperty("n_ins"));
            _n_att.fromVar(obj->getProperty("n_att"));
            _n_atd.fromVar(obj->getProperty("n_atd"));
            _n_dct.fromVar(obj->getProperty("n_dct"));
            _n_dcd.fromVar(obj->getProperty("n_dcd"));
        }

        _h_lev.reset(-100.0f);
        _h_ran.reset(0.0f);
        _h_att.reset(0.050f);
        _h_atp.reset(0.0f);

        _h_lev.fromVar(obj->getProperty("h_lev"));
        _h_ran.fromVar(obj->getProperty("h_ran"));
        _h_att.fromVar(obj->getProperty("h_att"));
        _h_atp.fromVar(obj->getProperty("h_atp"));
    }
}

void Addsynth::write(OutputStream& stream) const
{
    char header[header_length] = {0};

    strncpy(header, "AEOLUS", 6);
    header[7] = defaultVersion;
    header[26] = N_HARM;
    header[28] = (char) _noteMin;
    header[29] = (char) _noteMax;
    header[30] = (char) _fn;
    header[31] = (char) _fd;

    stream.write(header, header_length);

    writeString<stopName_length> (_stopName,  stream);
    writeString<copyright_length>(_copyright, stream);
    writeString<mnemonic_length> (_mnemonic,  stream);
    writeString<comments_length>  (_comments,  stream);
    writeString<reserved_length> (String(), stream);

    _n_vol.write(stream);
    _n_off.write(stream);
    _n_ran.write(stream);
    _n_ins.write(stream);
    _n_att.write(stream);
    _n_atd.write(stream);
    _n_dct.write(stream);
    _n_dcd.write(stream);

    // Store all N_HARM harmonics
    jassert(header[26] == N_HARM);

    _h_lev.write(stream);
    _h_ran.write(stream);
    _h_att.write(stream);
    _h_atp.write(stream);
}

template<size_t L>
static juce::Result readString(String& string, InputStream& stream)
{
    char buffer[L] = {0};
    const auto n = stream.read(buffer, L);

    if (n != L)
        return Result::fail(String("Failed to read string of length ") + String(L)
            + " " + String(n) + " bytes read instead");

    buffer[L - 1] = 0;
    string = String::fromUTF8(buffer, -1);

    return Result::ok();
}

juce::Result Addsynth::read(InputStream& stream)
{
    Addsynth candidate;
    auto result = candidate.readUnchecked(stream);
    if (result.failed()) return result;
    result = validate(candidate.toVar());
    if (result.wasOk()) *this = std::move(candidate);
    return result;
}

juce::Result Addsynth::readUnchecked(InputStream& stream)
{
    char header[header_length] = {0};

    auto n = stream.read(header, header_length);

    if (n != header_length)
        return Result::fail("Failed to read the header");

    if (strncmp(header, "AEOLUS", 6) != 0)
        return Result::fail("Invalid header signature");

    int version = (unsigned char)header[7];
    int nHarm = (unsigned char)header[26];

    if (nHarm == 0)
        nHarm = deprecated::N_HARM;

    if (version < 1 || version > defaultVersion || nHarm < 1 || nHarm > N_HARM)
        return Result::fail("Unsupported stop version or harmonic count");
    const int64 required = 160 + (version >= defaultVersion ? 8 : 3) * (4 + 4*N_NOTES)
        + 4 * nHarm * (4 + 4*N_NOTES);
    if (stream.getNumBytesRemaining() < required) return Result::fail("Truncated stop payload");
    _noteMin = (unsigned char)header[28];
    _noteMax = (unsigned char)header[29];

    if (_noteMax == deprecated::NOTE_MAX)
        _noteMax = NOTE_MAX;

    _fn = (unsigned char)header[30];
    _fd = (unsigned char)header[31];

    auto res = readString<stopName_length>(_stopName, stream);

    if (res.failed())
        return Result::fail("Failed to read the stop name: " + res.getErrorMessage());

    res = readString<copyright_length>(_copyright, stream);

    if (res.failed())
        return Result::fail("Failed to read the copyright: " + res.getErrorMessage());

    res = readString<mnemonic_length>(_mnemonic, stream);

    if (res.failed())
        return Result::fail("Failed to read the mnemonic: " + res.getErrorMessage());

    res = readString<comments_length>(_comments, stream);

    if (res.failed())
        return Result::fail("Failed to read the comments: " + res.getErrorMessage());

    {
        String reserved;
        res = readString<reserved_length>(reserved, stream);

        if (res.failed())
            return Result::fail("Failed to read reserved field: " + res.getErrorMessage());
    }

    _n_vol.read(stream);
    _n_off.read(stream);
    _n_ran.read(stream);

    if (version >= defaultVersion) {
        _n_ins.read(stream);
        _n_att.read(stream);
        _n_atd.read(stream);
        _n_dct.read(stream);
        _n_dcd.read(stream);
    }

    _h_lev.reset(-100.0f);
    _h_ran.reset(0.0f);
    _h_att.reset(0.050f);
    _h_atp.reset(0.0f);

    _h_lev.read(stream, nHarm);
    _h_ran.read(stream, nHarm);
    _h_att.read(stream, nHarm);
    _h_atp.read(stream, nHarm);

    return Result::ok();
}

Result Addsynth::readFromResource(const String& name)
{
    int size = 0;
    const char* data = BinaryData::getNamedResource(name.toRawUTF8(), size);

    if (data == nullptr)
        return Result::fail("Unable to read embedded resource: " + name);

    MemoryInputStream stream(data, size, false);

    return read(stream);
}

Result Addsynth::validate(const var& value)
{
    auto* object = value.getDynamicObject();
    if (!object) return Result::fail("Stop must be a JSON object");
    auto integer = [&](const char* name, int low, int high) {
        auto v = object->getProperty(name);
        return (v.isInt() || v.isInt64()) && int(v) >= low && int(v) <= high;
    };
    const int rawNoteMax = object->getProperty("note_max");
    const int noteMax = rawNoteMax == deprecated::NOTE_MAX ? NOTE_MAX : rawNoteMax;
    if (!integer("version", 1, 2) || !integer("n_harm", 0, N_HARM)
        || !integer("note_min", 0, 127) || !integer("note_max", 0, 127)
        || noteMax < int(object->getProperty("note_min"))
        || noteMax-int(object->getProperty("note_min")) > (N_NOTES-1)*NOTES_GAP
        || !integer("fn", 1, 255) || !integer("fd", 1, 255))
        return Result::fail("Invalid stop version, range, name or footage ratio");
    auto table = [](const var& v, float minimum, float maximum) {
        auto* o = v.getDynamicObject();
        if (!o) return false;
        auto mask = o->getProperty("mask");
        if (!mask.isInt() || int(mask) < 0 || int(mask) >= (1 << N_NOTES)) return false;
        auto* values = o->getProperty("values").getArray();
        if (!values || values->size() != N_NOTES) return false;
        for (auto& number : *values) {
            if (!number.isInt() && !number.isInt64() && !number.isDouble()) return false;
            double f = number;
            if (!std::isfinite(f) || f < minimum || f > maximum) return false;
        }
        return true;
    };
    for (const char* name : {"n_vol", "n_off", "n_ran", "n_ins", "n_att", "n_atd", "n_dct", "n_dcd"}) {
        if (int(object->getProperty("version")) < 2 && String(name) != "n_vol" && String(name) != "n_off" && String(name) != "n_ran") continue;
        const String key(name);
        const bool seconds = key == "n_att" || key == "n_dct";
        const bool nonnegative = seconds || key == "n_ran" || key == "n_ins";
        const float minimum = nonnegative ? 0.f : key == "n_vol" ? -200.f : -1200.f;
        const float maximum = seconds ? 30.f : key == "n_vol" ? 40.f : (key == "n_ran" || key == "n_ins") ? 100.f : 1200.f;
        if (!table(object->getProperty(name), minimum, maximum))
            return Result::fail(String("Invalid note table: ") + name);
    }
    for (const char* name : {"h_lev", "h_ran", "h_att", "h_atp"}) {
        auto* harmonics = object->getProperty(name).getArray();
        if (!harmonics || harmonics->size() != (int(object->getProperty("n_harm")) == 0 ? deprecated::N_HARM : int(object->getProperty("n_harm"))))
            return Result::fail(String("Invalid harmonic table: ") + name);
        const String key(name);
        const float minimum = key == "h_att" || key == "h_ran" ? 0.f : key == "h_lev" ? -200.f : -100.f;
        const float maximum = key == "h_att" ? 30.f : key == "h_atp" ? 100.f : 40.f;
        for (const auto& harmonic : *harmonics)
            if (!table(harmonic, minimum, maximum))
                return Result::fail(String("Invalid harmonic values: ") + name);
    }
    const auto* volume=object->getProperty("n_vol").getProperty("values",{}).getArray();
    const auto* levels=object->getProperty("h_lev").getArray();
    const auto* variation=object->getProperty("h_ran").getArray();
    for (int harmonic=0; harmonic<levels->size(); ++harmonic) for (int note=0; note<N_NOTES; ++note) {
        const double combined=double((*volume)[note])
            + double((*levels)[harmonic].getProperty("values",{})[note])
            + double((*variation)[harmonic].getProperty("values",{})[note]);
        if (combined > 24.0) return Result::fail("Combined harmonic gain exceeds +24 dB");
    }
    return Result::ok();
}

Result Addsynth::readFromFile(const File& file)
{
    if (!file.exists())
        return Result::fail("File does not exist");

    const auto ext{ file.getFileExtension().toLowerCase() };

    if (ext == ".ae0") {
        FileInputStream stream(file);
        auto result = read(stream);
        if (result.wasOk() && _stopName.isEmpty()) _stopName = file.getFileNameWithoutExtension();
        return result;
    }

    if (ext == ".json") {
        FileInputStream stream(file);
        var stop;
        auto result = JSON::parse(stream.readEntireStreamAsString(), stop);
        if (result.failed()) return result;
        result = validate(stop);
        if (result.failed()) return result;
        Addsynth candidate; candidate.fromVar(stop);
        if (candidate.getStopName().isEmpty()) candidate.setStopName(file.getFileNameWithoutExtension());
        *this = std::move(candidate);
        return Result::ok();
    }

    return Result::fail("Unknown file format");
}

//==============================================================================

Model::Model()
    : _synths()
    , _nameToSynthMap()
{
    const auto exported = ensureStopFiles();
    if (exported.failed()) _loadError = exported.getErrorMessage() + "\n";
    loadExternalPipes();
    loadEmbeddedPipes();
}

void Model::loadExternalPipes()
{
    const juce::StringArray skipNames{
        "organ_config.json",
        "organ_state.json",
        "default_organ.json"
    };

    for (auto dir : getStopSearchDirectories())
    {
        if (!dir.isDirectory())
            continue;

        for (DirectoryEntry entry : RangedDirectoryIterator(dir, false))
        {
            auto file = entry.getFile();
            if (!file.existsAsFile())
                continue;

            if (skipNames.contains(file.getFileName()))
                continue;

            const auto ext = file.getFileExtension().toLowerCase();
            if (ext != ".json" && ext != ".ae0")
                continue;

            auto synth = std::make_unique<Addsynth>();
            const auto res = synth->readFromFile(file);

            if (res.wasOk())
            {
                synth->setStopName(file.getFileNameWithoutExtension());
                addSynth(std::move(synth));
            }
            else
            {
                _loadError += "Cannot load " + file.getFileName() + ": " + res.getErrorMessage() + "\n";
                DBG("Failed to read: " << res.getErrorMessage());
            }
        }
    }
}

void Model::loadEmbeddedPipes()
{
    for (int i = 0; i < BinaryData::namedResourceListSize; ++i) {
        String filename(BinaryData::originalFilenames[i]);

        auto synth = std::make_unique<Addsynth>();
        String stopName;
        bool ok = false;

        if (filename.endsWith(".ae0")) {
            String name(BinaryData::namedResourceList[i]);
            auto res = synth->readFromResource(name);

            if (res.wasOk()) {
                ok = true;
                stopName = name.dropLastCharacters(4);
            } else _loadError += "Embedded stop " + filename + ": " + res.getErrorMessage() + "\n";
        } else if (filename.endsWith("_ae0.json")) {
            String name(BinaryData::namedResourceList[i]);

            int size = 0;
            const char* data = BinaryData::getNamedResource(name.toRawUTF8(), size);

            if (data != nullptr) {
                MemoryInputStream stream(data, size, false);
                auto stop = JSON::parse(stream);
                const auto validated=Addsynth::validate(stop);
                if (validated.failed()) { _loadError += "Embedded stop " + filename + ": " + validated.getErrorMessage() + "\n"; continue; }
                synth->fromVar(stop);

                ok = true;
                stopName = name.dropLastCharacters(9);
            }
        }

        if (ok) {
            synth->setStopName(stopName);
            addSynth(std::move(synth));
        }
    }
}

Addsynth* Model::getStopByName(const juce::String& name)
{
    auto it = _nameToSynthMap.find(name);

    if (it == _nameToSynthMap.end())
        return nullptr;

    return it->second;
}

void Model::addSynth(std::unique_ptr<Addsynth>&& synthToAdd)
{
    std::unique_ptr<Addsynth> synth{ std::move(synthToAdd) };
    const String stopName{ synth->getStopName() };

    if (_nameToSynthMap.find(stopName) == _nameToSynthMap.end()) {
        auto* ptr = synth.get();
        ptr->setStopName(stopName);

        _synths.add(synth.release());
        _nameToSynthMap[stopName] = ptr;
    } else {
        // Pipe with this name already exists - it won't be added again
        DBG("Pipe " << stopName << " duplicate found");
    }
}

Addsynth* Model::addPreparedStop(const Addsynth& source)
{
    const auto name = source.getStopName();
    if (auto* existing = getStopByName(name)) {
        *existing = source;
        return existing;
    }
    addSynth(std::make_unique<Addsynth>(source));
    return getStopByName(name);
}

bool Model::reloadStopFromFile(const File& file)
{
    const String stopName = file.getFileNameWithoutExtension();

    auto synth = std::make_unique<Addsynth>();
    const auto res = synth->readFromFile(file);
    if (!res.wasOk())
        return false;

    synth->setStopName(stopName);

    if (auto* existing = getStopByName(stopName))
    {
        existing->fromVar(synth->toVar());
        existing->setStopName(stopName);
        return true;
    }

    addSynth(std::move(synth));
    return true;
}

JUCE_IMPLEMENT_SINGLETON(Model)

AEOLUS_NAMESPACE_END

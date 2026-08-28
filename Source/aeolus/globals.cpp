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

#include "aeolus/globals.h"
#include "aeolus/addsynth.h"

namespace
{
    juce::File getExeDirectory()
    {
        return juce::File::getSpecialLocation(juce::File::currentExecutableFile)
            .getParentDirectory();
    }

    juce::File getDocumentsAeolusDirectory()
    {
        return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
            .getChildFile("Aeolus");
    }

    juce::File& activeOrganDataDirectory()
    {
        static juce::File dir;
        return dir;
    }
}

AEOLUS_NAMESPACE_BEGIN

juce::File getOrganDataDirectory()
{
    if (activeOrganDataDirectory().getFullPathName().isNotEmpty())
        return activeOrganDataDirectory();

    const auto exeDir = getExeDirectory();
    const auto docsDir = getDocumentsAeolusDirectory();

    if (exeDir.getChildFile("organ_config.json").existsAsFile()
        || exeDir.getChildFile("organ_state.json").existsAsFile())
    {
        activeOrganDataDirectory() = exeDir;
        return exeDir;
    }

    activeOrganDataDirectory() = docsDir;
    return docsDir;
}

juce::File getCustomOrganConfigFile()
{
    return getOrganDataDirectory().getChildFile("organ_config.json");
}

juce::File getOrganStateFile()
{
    return getOrganDataDirectory().getChildFile("organ_state.json");
}

void ensureOrganDataFiles()
{
    auto dir = getOrganDataDirectory();

    if (!dir.exists())
        dir.createDirectory();

    const auto configFile = dir.getChildFile("organ_config.json");
    const auto stateFile  = dir.getChildFile("organ_state.json");

    if (!configFile.existsAsFile())
    {
        configFile.replaceWithData(BinaryData::default_organ_json,
                                   BinaryData::default_organ_jsonSize);
    }

    if (!stateFile.existsAsFile())
    {
        stateFile.replaceWithText("{\n}\n");
    }
}

juce::Array<juce::File> getStopSearchDirectories()
{
    juce::Array<juce::File> dirs;
    dirs.add(getExeDirectory());
    dirs.add(getDocumentsAeolusDirectory());
    dirs.add(getDocumentsAeolusDirectory().getChildFile("Stops"));
    return dirs;
}

static bool stopFileExists(const juce::String& stopName)
{
    const auto jsonName = stopName + ".json";
    const auto ae0Name  = stopName + ".ae0";

    for (auto dir : getStopSearchDirectories())
    {
        if (dir.getChildFile(jsonName).existsAsFile())
            return true;
        if (dir.getChildFile(ae0Name).existsAsFile())
            return true;
    }

    return false;
}

void ensureStopFiles()
{
    auto stopsDir = getDocumentsAeolusDirectory().getChildFile("Stops");

    for (int i = 0; i < BinaryData::namedResourceListSize; ++i)
    {
        const juce::String filename(BinaryData::originalFilenames[i]);

        juce::String stopName;
        std::unique_ptr<Addsynth> synth;

        if (filename.endsWith(".ae0"))
        {
            stopName = filename.dropLastCharacters(4);
            if (stopFileExists(stopName))
                continue;

            synth = std::make_unique<Addsynth>();
            if (!synth->readFromResource(BinaryData::namedResourceList[i]).wasOk())
                continue;
        }
        else if (filename.endsWith("_ae0.json"))
        {
            stopName = filename.dropLastCharacters(9);
            if (stopFileExists(stopName))
                continue;

            int size = 0;
            const char* data = BinaryData::getNamedResource(BinaryData::namedResourceList[i], size);
            if (data == nullptr)
                continue;

            synth = std::make_unique<Addsynth>();
            juce::MemoryInputStream stream(data, (size_t)size, false);
            synth->fromVar(JSON::parse(stream));
        }
        else
        {
            continue;
        }

        synth->setStopName(stopName);

        if (!stopsDir.exists())
            stopsDir.createDirectory();

        const auto outFile = stopsDir.getChildFile(stopName + ".json");
        outFile.replaceWithText(JSON::toString(synth->toVar(), true));
    }
}

//==============================================================================

namespace math {

float exp2ap(float x)
{
    int i = (int)(floor (x));
    x -= i;
    // return ldexp (1 + x * (0.66 + 0.34 * x), i);
    return ldexp (1 + x * (0.6930f + x * (0.2416f + x * (0.0517f + x * 0.0137f))), i);
}

} // namespace math

//==============================================================================

namespace midi {

int channelToMask(int channel)
{
    // Zero means any MIDI channel.
    if (channel <= 0)
        return (1 << 16) - 1;

    return 1 << (channel - 1);
}

bool matchChannelToMask(int mask, int channel)
{
    return (mask & channelToMask(channel)) != 0;
}

} // namespace midi

AEOLUS_NAMESPACE_END

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
#include <stdexcept>
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
#if AEOLUS_TESTING
        if (juce::SystemStats::getEnvironmentVariable("AEOLUS_TEST_DATA_DIRECTORY", {}).isNotEmpty())
            return aeolus::getTestDataDirectory();
#endif
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
#if AEOLUS_TESTING
juce::File getTestDataDirectory()
{
    const auto path = juce::SystemStats::getEnvironmentVariable("AEOLUS_TEST_DATA_DIRECTORY", {});
    if (!juce::File::isAbsolutePath(path))
        throw std::runtime_error("Tests require an absolute AEOLUS_TEST_DATA_DIRECTORY");
    return juce::File(path);
}
#endif

juce::File getOrganDataDirectory()
{
#if AEOLUS_TESTING
    if (juce::SystemStats::getEnvironmentVariable("AEOLUS_TEST_DATA_DIRECTORY", {}).isNotEmpty()) return getTestDataDirectory();
#endif
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

juce::Result ensureOrganDataFiles()
{
    const auto dir = getOrganDataDirectory();
    auto result = dir.createDirectory();
    if (result.failed()) return result;
    const auto configFile = dir.getChildFile("organ_config.json");
    const auto stateFile = dir.getChildFile("organ_state.json");
    if (!configFile.existsAsFile()) {
        result = saveTextFile(configFile, juce::String::fromUTF8(BinaryData::default_organ_json, BinaryData::default_organ_jsonSize));
        if (result.failed()) return result;
    }
    if (!stateFile.existsAsFile()) return saveTextFile(stateFile, "{\n}\n");
    return juce::Result::ok();
}

juce::Array<juce::File> getStopSearchDirectories()
{
    juce::Array<juce::File> dirs;
#if AEOLUS_TESTING
    if (juce::SystemStats::getEnvironmentVariable("AEOLUS_TEST_DATA_DIRECTORY", {}).isNotEmpty()) {
        dirs.add(getTestDataDirectory()); dirs.add(getTestDataDirectory().getChildFile("Stops"));
        return dirs;
    }
#endif
    dirs.add(getExeDirectory());
    dirs.addIfNotAlreadyThere(getOrganDataDirectory().getChildFile("Stops"));
    dirs.addIfNotAlreadyThere(getDocumentsAeolusDirectory());
    dirs.addIfNotAlreadyThere(getDocumentsAeolusDirectory().getChildFile("Stops"));
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

juce::Result ensureStopFiles()
{
    auto stopsDir = getOrganDataDirectory().getChildFile("Stops");

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

        auto result = stopsDir.createDirectory();
        if (result.failed()) return result;

        const auto outFile = stopsDir.getChildFile(stopName + ".json");
        result = saveTextFile(outFile, JSON::toString(synth->toVar(), true));
        if (result.failed()) return result;
    }
    return juce::Result::ok();
}

//==============================================================================

void reportUserError(const juce::String& title, const juce::String& message)
{
#if AEOLUS_TESTING
    juce::Logger::writeToLog(title + ": " + message);
#else
    juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, title, message);
#endif
}

juce::Result saveTextFile(const juce::File& file, const juce::String& text)
{
    if (file.isDirectory()) return juce::Result::fail("Cannot replace a directory: " + file.getFullPathName());
    const auto temporaryPath = file.getSiblingFile("." + file.getFileName() + "." + juce::Uuid().toString() + ".tmp");
    juce::TemporaryFile temporary(file, temporaryPath); // never a discoverable stop model

    {
        juce::FileOutputStream stream(temporary.getFile());
        if (!stream.openedOk()) return stream.getStatus();
        if (!stream.writeText(text, false, false, "\n")) return juce::Result::fail("Unable to write " + file.getFullPathName());
        const auto expectedBytes = stream.getPosition();
        stream.flush();
        if (stream.getStatus().failed()) return stream.getStatus();
        // JUCE flush() drops flushBuffer()'s false result on a positive short
        // write. Position includes buffered bytes; verify they reached the file.
        if (temporary.getFile().getSize() != expectedBytes)
            return juce::Result::fail("Incomplete write to " + file.getFullPathName());
    }
    if (!temporary.overwriteTargetFileWithTemporary()) return juce::Result::fail("Unable to replace " + file.getFullPathName());
    return juce::Result::ok();
}

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
    if (channel == 0)
        return (1 << 16) - 1;

    return channel >= 1 && channel <= 16 ? 1 << (channel - 1) : 0;
}

bool matchChannelToMask(int mask, int channel)
{
    return (mask & channelToMask(channel)) != 0;
}

} // namespace midi

AEOLUS_NAMESPACE_END

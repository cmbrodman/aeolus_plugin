// Test-only render export for comparison with the pre-admission commit.
namespace {
void exportSafetyReference(const char* directory)
{
    const juce::File folder(directory); folder.createDirectory();
    for (const float rate : {44100.0f, 48000.0f}) {
        for (const bool workers : {false, true}) {
            const auto r = renderScript(workers ? std::map<std::string,std::string>{{"AEOLUS_DSP_WORKERS","3"},{"AEOLUS_DSP_GROUP_SIZE","1"}}
                : std::map<std::string,std::string>{{"AEOLUS_PER_VOICE_RANDOM","1"}}, rate, false, false);
            auto file = folder.getChildFile(juce::String(int(rate))+(workers ? "-workers.f32" : "-serial.f32"));
            check(!file.existsAsFile(), "render export must use a fresh path");
            juce::MemoryBlock data(r.left.data(),r.left.size()*sizeof(float));
            data.append(r.right.data(), r.right.size()*sizeof(float));
            check(file.replaceWithData(data.getData(),data.getSize()), "render export failed");
            std::cout << file.getFileName() << " peak voices " << r.peakVoices << " final " << r.finalVoices << '\n';
        }
    }
}
}

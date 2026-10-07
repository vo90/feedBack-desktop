#include "../../src/audio/engine/DeviceSetup.h"
#include <iostream>
#include <stdexcept>

// Probe-only fixture: monitor DSP is never called by this test.
void SignalChain::prepare(double, int) { throw std::logic_error("unexpected DSP prepare"); }
void SignalChain::releaseResources() { throw std::logic_error("unexpected DSP release"); }
void NoiseGate::prepare(double, int) { throw std::logic_error("unexpected gate prepare"); }
void TonePolish::prepare(double) { throw std::logic_error("unexpected tone prepare"); }

static void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
struct Driver { int instances = 0, creations = 0, duplicates = 0; };
class Device final : public juce::AudioIODevice
{
public:
    Device(Driver& d, const juce::String& name, const juce::String& type)
        : AudioIODevice(name, type), driver(d)
    {
        if (driver.instances != 0) { ++driver.duplicates; throw std::runtime_error("driver already owned"); }
        ++driver.instances; ++driver.creations;
    }
    ~Device() override { --driver.instances; }
    juce::StringArray getInputChannelNames() override { return {"1", "2", "3", "4", "5", "6", "7", "8"}; }
    juce::StringArray getOutputChannelNames() override { return {"L", "R"}; }
    juce::Array<double> getAvailableSampleRates() override { return {48000}; }
    juce::Array<int> getAvailableBufferSizes() override { return {128, 256, 512}; }
    int getDefaultBufferSize() override { return 256; }
    juce::String open(const juce::BigInteger& i, const juce::BigInteger& o, double sr, int bs) override
    { inputs = i; outputs = o; rate = sr; block = bs; opened = true; return {}; }
    void close() override { stop(); opened = false; }
    bool isOpen() override { return opened; }
    void start(juce::AudioIODeviceCallback* c) override { callback = c; if (c) c->audioDeviceAboutToStart(this); }
    void stop() override { if (callback) callback->audioDeviceStopped(); callback = nullptr; }
    bool isPlaying() override { return callback != nullptr; }
    juce::String getLastError() override { return {}; }
    int getCurrentBufferSizeSamples() override { return block; }
    double getCurrentSampleRate() override { return rate; }
    int getCurrentBitDepth() override { return 24; }
    juce::BigInteger getActiveOutputChannels() const override { return outputs; }
    juce::BigInteger getActiveInputChannels() const override { return inputs; }
    int getOutputLatencyInSamples() override { return block; }
    int getInputLatencyInSamples() override { return block; }
private:
    Driver& driver;
    bool opened = false;
    double rate = 48000;
    int block = 256;
    juce::BigInteger inputs, outputs;
    juce::AudioIODeviceCallback* callback = nullptr;
};
class Type final : public juce::AudioIODeviceType
{
public:
    Type(Driver& d, const juce::String& type, const juce::String& device, int defaultIndex = 0)
        : AudioIODeviceType(type), driver(d), name(device), defaultDeviceIndex(defaultIndex) {}
    void scanForDevices() override {}
    juce::StringArray getDeviceNames(bool) const override
    { return defaultDeviceIndex == 1 ? juce::StringArray {"Other interface", name} : juce::StringArray {name}; }
    int getDefaultDeviceIndex(bool) const override { return defaultDeviceIndex; }
    int getIndexOfDevice(juce::AudioIODevice*, bool) const override { return 0; }
    bool hasSeparateInputsAndOutputs() const override { return false; }
    juce::AudioIODevice* createDevice(const juce::String& output, const juce::String& input) override
    { return new Device(driver, input.isNotEmpty() ? input : output, getTypeName()); }
private:
    Driver& driver;
    juce::String name;
    int defaultDeviceIndex;
};
class Manager final : public juce::AudioDeviceManager
{
public:
    Manager(Driver& a, Driver& w) : asio(a), windows(w) {}
    void createAudioDeviceTypes(juce::OwnedArray<juce::AudioIODeviceType>& types) override
    {
        types.add(new Type(asio, "ASIO", "Test interface"));
        types.add(new Type(windows, "Windows Audio", "Test speakers"));
    }
private:
    Driver& asio;
    Driver& windows;
};
int main()
{
    try
    {
        juce::ScopedJuceInitialiser_GUI init;
        Driver asio, windows;
        Manager input(asio, windows), output(asio, windows);
        require(input.initialise(8, 0, nullptr, false, "Test interface").isEmpty(), "input open");
        // Register the output backend without opening a duplicate ASIO endpoint.
        output.getAvailableDeviceTypes();
        output.setCurrentAudioDeviceType("Windows Audio", true);
        juce::AudioDeviceManager::AudioDeviceSetup out;
        out.outputDeviceName = "Test speakers";
        out.useDefaultOutputChannels = false;
        out.outputChannels.setRange(0, 2, true);
        out.sampleRate = 48000; out.bufferSize = 256;
        require(output.setAudioDeviceSetup(out, true).isEmpty(), "output open");
        slopsmith::EngineState state;
        slopsmith::DeviceSetup setup(input, output, state);
        Type nonFirstDefault(asio, "ASIO", "Test interface", 1);
        require(slopsmith::DeviceSetup::resolveDeviceName(&nonFirstDefault, true, {}) == "Test interface",
                "Default must use the backend index, not the first enumerated device");
        require(slopsmith::DeviceSetup::resolveDeviceName(&nonFirstDefault, false, "Other interface") == "Other interface",
                "explicit selection must be preserved");
        Manager additional(asio, windows);
        setup.registerAdditionalManager(additional);
        require(setup.validateAdditionalOpen(additional, "ASIO", "").isNotEmpty(),
                "extra output Default must not reopen primary ASIO");
        const int creations = asio.creations;
        for (int i = 0; i < 10; ++i)
        {
            const auto split = setup.probeDual("ASIO", "Test interface", "Windows Audio", "Test speakers");
            require(split.compatible && split.inputChannels.size() == 8, "split capabilities");
            const auto duplex = setup.probeDual("ASIO", "Test interface", "ASIO", "Test interface");
            require(duplex.compatible && duplex.inputChannels.size() == 8, "split to duplex probe");
            require(setup.probeDual("ASIO", "Test interface", "ASIO", "").compatible,
                    "named input and default output must share one ASIO object");
            require(setup.probeDual("ASIO", "", "ASIO", "Test interface").compatible,
                    "default input and named output must share one ASIO object");
        }
        // A stopped/closed object still owns the driver until destruction.
        input.getCurrentAudioDevice()->close();
        require(setup.probeDual("ASIO", "Test interface", "Windows Audio", "Test speakers").compatible,
                "closed device capability reuse");
        require(asio.duplicates == 0 && asio.creations == creations, "probe instantiated owned ASIO driver");
        input.closeAudioDevice(); output.closeAudioDevice();
        input.setCurrentAudioDeviceType("Windows Audio", true);
        output.setCurrentAudioDeviceType("ASIO", true);
        require(setup.probeDual("Windows Audio", "Test speakers", "ASIO", "Test interface").compatible,
                "reverse split probe");
        require(setup.probeDual("ASIO", "Test interface", "ASIO", "Test interface").compatible,
                "duplex probe must reuse ASIO owned by output manager");
        require(asio.duplicates == 0, "reverse probe instantiated owned ASIO driver");
        require(setup.closeAsioDevicesForReconfigure().isEmpty(), "ASIO release failed");
        require(asio.instances == 0 && windows.instances == 1,
                "reconfigure must release ASIO and retain unrelated Windows device");
        input.closeAudioDevice(); output.closeAudioDevice();
        const int beforeDefaultProbes = asio.creations;
        for (const auto& inputName : { juce::String(), juce::String("Test interface") })
            for (const auto& outputName : { juce::String(), juce::String("Test interface") })
            {
                require(setup.probeDual("ASIO", inputName, "ASIO", outputName).compatible,
                        "cold default/named duplex probe");
                slopsmith::DeviceConfig config { "ASIO", inputName, "ASIO", outputName };
                require(setup.resolveConfigDeviceNames(config).isEmpty(), "resolve default names");
                require(config.inputDevice == "Test interface" && config.outputDevice == config.inputDevice,
                        "apply and probe must resolve the same endpoint");
            }
        require(asio.creations == beforeDefaultProbes + 4 && asio.duplicates == 0,
                "each cold duplex probe must construct exactly one driver");
        slopsmith::DeviceConfig missing { "ASIO", "Disconnected interface", "Windows Audio", "" };
        require(setup.resolveConfigDeviceNames(missing).isNotEmpty(), "reject unavailable device before mutation");
        require(!setup.probeDual("ASIO", "Disconnected interface", "ASIO", "").compatible,
                "unavailable probe fails closed");
        require(setup.probeDual("ASIO", "Test interface", "Windows Audio", "Test speakers").compatible,
                "idle temporary probes");
        require(asio.instances == 0 && windows.instances == 0, "temporary probe leaked driver");
        std::cout << "device probe regression passed\n";
    }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}

// One synchronous main-process transaction: reject stale wizard results and
// persist only a confirmed selection. Roll back the live channel on save failure.
export function confirmInputChannel(audio: any, channel: number, generation: number, previous: number,
    read: () => any, write: (settings: any) => boolean) {
    if (![channel, generation, previous].every(Number.isSafeInteger) || channel < -1 || previous < -1
        || generation < 0 || !audio?.selectInputChannel || !audio?.getInputChannelSnapshot)
        return { ok: false, error: 'Input channel setup is unavailable. Update the desktop audio engine.' };
    const snapshot = audio.getInputChannelSnapshot();
    if (!snapshot?.running || !snapshot.channels?.length || snapshot.generation !== generation || snapshot.selected !== previous
        || (channel !== -1 && !snapshot.channels.some((c: any) => c.index === channel)))
        return { ok: false, error: 'The input changed. Refresh the channels and try again.' };
    const device = audio.getCurrentDevice();
    if (!device?.input || !audio.selectInputChannel(channel, generation, previous))
        return { ok: false, error: 'The input changed. Refresh the channels and try again.' };
    try {
        const settings = { ...read(), type: device.inputType || device.type,
            inputType: device.inputType || device.type, outputType: device.outputType || device.type,
            input: device.input, output: device.output, sampleRate: String(device.sampleRate),
            bufferSize: String(device.inputBlockSize || device.blockSize), inputChannel: String(channel), savedAt: Date.now() };
        if (!write(settings)) throw new Error('Could not save the input channel.');
        return { ok: true, channel };
    } catch (_) {
        audio.selectInputChannel(previous, generation, channel);
        return { ok: false, error: 'Could not save the input channel. The previous selection was restored where possible.' };
    }
}

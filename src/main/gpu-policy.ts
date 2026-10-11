export interface GpuPolicyOptions {
    platform: string;
    isPackaged: boolean;
    requested: string | undefined;
    hasHighPerformanceSwitch: boolean;
    hasLowPowerSwitch: boolean;
}

export interface GpuPolicy {
    name: 'high-performance' | 'low-power';
    switchName: 'force_high_performance_gpu' | 'force_low_power_gpu';
    developmentOverride: boolean;
}

/** A startup-only diagnostic: packaged builds retain the Windows GPU default. */
export function selectGpuPolicy(options: GpuPolicyOptions): GpuPolicy | null {
    if (options.platform !== 'win32') return null;
    const developmentOverride = !options.isPackaged
        && options.requested !== undefined && options.requested !== '';
    const name = developmentOverride ? options.requested : 'high-performance';
    if (name !== 'high-performance' && name !== 'low-power') {
        throw new Error('FEEDBACK_DEV_GPU_POLICY must be high-performance or low-power');
    }
    // A labelled diagnostic arm must not quietly contain both force switches.
    // Ordinary and packaged startup keep their existing command-line behavior.
    if (developmentOverride && (name === 'low-power'
        ? options.hasHighPerformanceSwitch : options.hasLowPowerSwitch)) {
        throw new Error('FEEDBACK_DEV_GPU_POLICY conflicts with an existing GPU force switch');
    }
    return {
        name,
        switchName: name === 'low-power' ? 'force_low_power_gpu' : 'force_high_performance_gpu',
        developmentOverride,
    };
}

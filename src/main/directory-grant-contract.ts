// Public renderer contract for a native directory selection that is redeemed
// by one backend plugin. displayPath is UI-only; grant is the authority.
//
// Bindings and dialog copy are deliberately allowlisted in Electron main.
// Installed plugin JavaScript shares one renderer, so caller-provided values
// cannot establish plugin identity and must never define a new authority.

const BINDING_RE = /^[A-Za-z0-9][A-Za-z0-9_.:-]{0,127}$/;

const DIRECTORY_GRANT_DIALOGS = new Map<string, string>([
    ['hybrid_track\0library-root', 'Choose a song library for Hybrid Track'],
]);

export interface DirectoryGrantPickerOptions {
    /** Allowlisted backend plugin id that may redeem the grant. */
    owner: string;
    /** Allowlisted workflow binding, for example "library-root". */
    purpose: string;
}

export interface NormalizedDirectoryGrantPickerOptions extends DirectoryGrantPickerOptions {
    /** Trusted copy selected by Electron main for this exact binding. */
    title: string;
}

export interface DirectoryGrantSelection {
    /** Opaque, short-lived, normally one-time backend capability. */
    grant: string;
    /** Native path for display only; renderer code cannot dereference it. */
    displayPath: string;
    /** Expiry as Unix epoch milliseconds. */
    expiresAt: number;
}

export function normalizeDirectoryGrantPickerOptions(
    value: unknown,
): NormalizedDirectoryGrantPickerOptions {
    if (value === null || typeof value !== 'object' || Array.isArray(value)) {
        throw new TypeError('Directory grant options must be an object');
    }
    const raw = value as Record<string, unknown>;
    if (typeof raw.owner !== 'string' || !BINDING_RE.test(raw.owner)) {
        throw new TypeError('owner must be a valid identifier');
    }
    if (typeof raw.purpose !== 'string' || !BINDING_RE.test(raw.purpose)) {
        throw new TypeError('purpose must be a valid identifier');
    }

    const title = DIRECTORY_GRANT_DIALOGS.get(`${raw.owner}\0${raw.purpose}`);
    if (!title) {
        throw new TypeError('Directory grant binding is not supported');
    }
    return {
        owner: raw.owner,
        purpose: raw.purpose,
        title,
    };
}

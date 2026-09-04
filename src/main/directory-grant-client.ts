// Authenticated loopback client for Core's private directory-grant endpoint.
// Kept independent of Electron so the security-sensitive wire contract is
// covered by ordinary node:test tests.

import * as http from 'http';

export const DIRECTORY_GRANT_SECRET_ENV = 'FEEDBACK_DESKTOP_DIRECTORY_GRANT_SECRET';
export const DIRECTORY_GRANT_SECRET_HEADER = 'X-FeedBack-Desktop-Grant-Secret';
const MAX_RESPONSE_BYTES = 16 * 1024;
const MAX_REQUEST_BYTES = 16 * 1024;
const GRANT_RE = /^[A-Za-z0-9_-]{32,128}$/;

export interface BackendDirectoryGrantRegistration {
    grant: string;
    expiresAt: number;
}

export interface RegisterDirectoryGrantRequest {
    port: number;
    secret: string;
    owner: string;
    purpose: string;
    directoryPath: string;
    timeoutMs?: number;
    /** Rechecked immediately before a successful response is exposed. */
    isBackendCurrent?: () => boolean;
}

export function registerDirectoryGrantWithBackend(
    input: RegisterDirectoryGrantRequest,
): Promise<BackendDirectoryGrantRegistration> {
    if (!Number.isInteger(input.port) || input.port < 1 || input.port > 65535) {
        return Promise.reject(new Error('Directory grant backend is unavailable.'));
    }
    if (typeof input.secret !== 'string' || input.secret.length < 32) {
        return Promise.reject(new Error('Directory grant backend is unavailable.'));
    }

    const body = Buffer.from(JSON.stringify({
        owner: input.owner,
        purpose: input.purpose,
        path: input.directoryPath,
    }), 'utf8');
    if (body.length > MAX_REQUEST_BYTES) {
        return Promise.reject(new Error('The selected directory path is too long.'));
    }

    return new Promise((resolve, reject) => {
        let settled = false;
        const fail = (message: string) => {
            if (settled) return;
            settled = true;
            reject(new Error(message));
        };

        const request = http.request({
            hostname: '127.0.0.1',
            port: input.port,
            path: '/api/desktop/directory-grants',
            method: 'POST',
            headers: {
                'Content-Type': 'application/json',
                'Content-Length': String(body.length),
                [DIRECTORY_GRANT_SECRET_HEADER]: input.secret,
            },
        }, (response) => {
            const chunks: Buffer[] = [];
            let received = 0;
            response.on('data', (raw: Buffer | string) => {
                if (settled) return;
                const chunk = Buffer.isBuffer(raw) ? raw : Buffer.from(raw);
                received += chunk.length;
                if (received > MAX_RESPONSE_BYTES) {
                    response.destroy();
                    fail('Directory grant backend returned an invalid response.');
                    return;
                }
                chunks.push(chunk);
            });
            response.on('error', () => {
                fail('Directory grant backend request failed.');
            });
            response.on('end', () => {
                if (settled) return;
                if (response.statusCode !== 201) {
                    fail(`Directory grant registration was rejected by the backend (${response.statusCode || 0}).`);
                    return;
                }
                let parsed: unknown;
                try {
                    parsed = JSON.parse(Buffer.concat(chunks).toString('utf8'));
                } catch {
                    fail('Directory grant backend returned an invalid response.');
                    return;
                }
                const result = parsed as Partial<BackendDirectoryGrantRegistration> | null;
                if (
                    result === null
                    || typeof result !== 'object'
                    || typeof result.grant !== 'string'
                    || !GRANT_RE.test(result.grant)
                    || typeof result.expiresAt !== 'number'
                    || !Number.isSafeInteger(result.expiresAt)
                    || result.expiresAt <= Date.now()
                ) {
                    fail('Directory grant backend returned an invalid response.');
                    return;
                }
                try {
                    if (input.isBackendCurrent && !input.isBackendCurrent()) {
                        fail('Directory grant backend changed while selecting the directory.');
                        return;
                    }
                } catch {
                    fail('Directory grant backend changed while selecting the directory.');
                    return;
                }
                settled = true;
                resolve({ grant: result.grant, expiresAt: result.expiresAt });
            });
        });

        request.on('error', () => {
            fail('Directory grant backend request failed.');
        });
        request.setTimeout(input.timeoutMs ?? 3000, () => {
            request.destroy();
            fail('Directory grant backend request timed out.');
        });
        request.end(body);
    });
}

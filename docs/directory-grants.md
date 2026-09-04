# Native directory grants

`window.feedBackDesktop.pickDirectoryGrant(options)` lets a renderer workflow
ask the managed Desktop for a normal native folder dialog without turning a
browser-provided path into filesystem authority.

```ts
pickDirectoryGrant({
  owner: "hybrid_track",
  purpose: "library-root",
}): Promise<null | {
  grant: string;
  displayPath: string;
  expiresAt: number;
}>
```

Cancellation resolves to `null`. `displayPath` is only for labels and
confirmation UI. The renderer sends `grant` to the owning plugin's backend;
that backend redeems it through Core's owner-scoped
`context["resolve_directory_grant"]` callable. A grant is opaque, expires after
two minutes, is bound to the exact owner and purpose, and is normally consumed
once.

The Desktop allowlists each owner/purpose pair and supplies its own dialog
title. Renderer-provided titles and default paths are ignored. In particular,
this prevents opening a native dialog from resolving an untrusted Windows
UNC/device path before the user has selected anything.

The old `pickDirectory()` bridge is unchanged. Browser/server-only runtimes do
not have `pickDirectoryGrant`, so plugins must keep a current-library or other
non-Desktop fallback.

Electron main registers the selected path with Core over 127.0.0.1 using a
fresh 256-bit secret generated for each backend spawn. That secret exists only
in main-process memory and the child environment; it is not part of the preload
API. Core's registration endpoint additionally rejects non-loopback clients.

Installed plugin frontends currently share the trusted top-level renderer and
are not security-isolated from one another. Owner/purpose binding therefore
prevents accidental and backend/LAN cross-use, but it does not prove which
same-renderer plugin initiated the dialog. Supporting mutually untrusted
frontend plugins requires separate sandboxed renderer principals.

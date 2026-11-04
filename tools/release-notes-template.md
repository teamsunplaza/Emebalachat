# Release notes template — Emebala Chat vX.Y.Z

> Fill this in at release time, then run the gate BEFORE publishing:
> `python tools/check_release_notes.py --tag vX.Y.Z --file installer\output\Emebalachat_Setup_X.Y.Z.exe --notes <this file>`
> The `SHA256:` line at the bottom is MANDATORY: the in-app updater is
> fail-closed and refuses the download when it is missing or malformed
> (session 261104_0001: v0.10.5 shipped without it once). Compute it with:
> `certutil -hashfile installer\output\Emebalachat_Setup_X.Y.Z.exe SHA256`
> (take the second line of output). Keep the line's first non-blank content
> exactly `SHA256:` + the 64 hex chars. After publishing, re-run the gate
> without `--notes` to verify the live release body.

## What's new

- <user-facing bullets, from CHANGELOG.txt, no internal bug IDs>

## What's fixed

- <user-facing bullets, from CHANGELOG.txt, no internal bug IDs>

## Verify the download (PowerShell)

Get-FileHash .\Emebalachat_Setup_X.Y.Z.exe -Algorithm SHA256

SHA256: <64 hex chars from certutil>

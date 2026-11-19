# Better AutoDeafen

**By Blenderanimat**

> A fork of [AutoDeafen](https://github.com/Lynxdeer/AutoDeafen) by Lynxdeer, rebuilt with persistent Discord key caching and quality-of-life fixes.

Automatically deafens you in Discord when you reach a configured percentage in a Geometry Dash level, then undeafens when you die, win, restart an attempt, or leave the level.

## Features & Fixes
- **Persistent Key Cache (`%APPDATA%\BetterAutoDeafen\keys.json`)**: Credentials are written atomically to disk and immediately flushed to Geode's save container so they survive PC restarts, game crashes, and mod updates.
- **Fixed Discord OAuth Token Refresh**: Fixes the upstream `&code=` vs `&refresh_token=` bug so expired tokens silently refresh instead of forcing re-setup.
- **Auto-Reconnect & Retry**: Automatically retries connecting if Geometry Dash starts before Discord, plus a 1-click **Reconnect** button in the config menu.
- **Undeafen on Restart & Leave**: Automatically undeafens whenever you restart an attempt or exit a level.
- **Auto-Disable on StartPos**: Automatically turns itself off while playing from a StartPos.

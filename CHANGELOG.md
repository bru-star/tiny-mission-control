# Changelog

## 8 November 2026: hotfix for the newest Nvidia drivers

- The PC helper now works with the newest Nvidia drivers, which report the GPU temperature with decimals. On those drivers the temperature never showed. The display still shows whole degrees.
- If the helper can't read the GPU, it now says so in its log.
- A Claude Code settings file that isn't UTF-8 no longer stops the helper.

To update, download the repository again and run `.\tools\install_helper.ps1`. Your settings are kept. The firmware hasn't changed, so there's no need to flash the board again.

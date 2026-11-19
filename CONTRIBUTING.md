
# Contributing to NeoAsmara

Thanks for your interest in contributing to NeoAsmara!

NeoAsmara is an independent C++ server emulator for Karos Online. Contributions that help improve compatibility, gameplay, stability and documentation are welcome.

## How to Contribute

You can help with:

- Fixing bugs and improving existing server systems.
- Implementing missing game mechanics.
- Improving packet handling and protocol compatibility.
- Testing gameplay, NPCs, quests and dungeons.
- Improving server performance and stability.
- Updating documentation and build scripts.

## Reporting Bugs

Before opening an issue, check whether the problem has already been reported.

Please include:

- Core version or commit.
- Description of the problem.
- Steps to reproduce it.
- Expected and actual behavior.
- Relevant logs or screenshots.

Remove passwords, tokens, private keys and player information before sharing logs.

## Pull Requests

1. Fork the repository.
2. Create a branch for your changes.
3. Make and test your changes.
4. Describe what was changed and why.
5. Submit a Pull Request.

Keep changes focused on the issue being addressed.

Avoid unrelated modifications, unnecessary dependencies or replacing working systems without a clear reason.

## Development Guidelines

- Follow the existing C++ code structure and conventions.
- Keep compatibility with the current Core architecture.
- Document protocol changes and their supporting evidence.
- Do not invent packet opcodes or game mechanics without verification.
- Test changes before submitting them.
- Do not include copyrighted game assets, credentials, private server databases or proprietary server files.

## Testing

Use the build and test instructions provided in the repository README.

Dungeon and packet-flow tests may require authenticated local resources and isolated test databases.

Never run development tests against live player databases.

## License

By contributing, you agree that your submitted changes will be distributed under the NeoAsmara Project License (NAPL v1.1).

See [LICENSE.md](LICENSE.md) for details.

## Community

For project discussions and development questions:

Discord: https://discord.gg/B7WsuU5WBa

Please follow our [Code of Conduct](CODE_OF_CONDUCT.md).

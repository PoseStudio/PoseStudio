# Contributing to The PoseStudio Open-Source Project

First off, thank you for considering contributing to PoseStudio! It's people like you that make open-source platforms great. 

## Table of Contents
- [Contributor License Agreement (CLA)](#contributor-license-agreement-cla)
- [How Can I Contribute?](#how-can-i-contribute)
  - [Reporting Bugs](#reporting-bugs)
  - [Suggesting Enhancements](#suggesting-enhancements)
  - [Pull Requests](#pull-requests)
- [Local Development Setup](#local-development-setup)
- [Styleguides](#styleguides)
- [Code of Conduct](#code-of-conduct)

## Contributor License Agreement (CLA)

Before we can merge your contribution, you need to sign the [PoseStudio Individual CLA](https://cla-assistant.io/PoseStudio/PoseStudio). You only sign once, and it takes about a minute with your GitHub account.

**Why we ask:** PoseStudio is and will remain free and open source under GPL-3.0. We may also offer commercial licenses, for example to companies that want to build PoseStudio into closed products. That income helps fund development. The CLA lets the project offer both.

**What it means for you:**

- You keep the copyright to your work.
- You give the project a permanent right to use and relicense your contributions, including under commercial terms.
- The CLA also covers contributions you made before signing.

**How it works:** When you open a pull request, a bot checks whether you've signed. If you haven't, it posts a link. A pull request can't be merged until the check passes.

**Contributing on behalf of your employer?** Your employer may own the code you write. If so, ask us about the Corporate CLA before you submit.

**Code shared outside pull requests:** We can only use code from patches, issues, Discord or the forum if the author has signed the CLA.

Questions? Email community@posestudio.org.

## How Can I Contribute?

### Reporting Bugs
Before creating bug reports, please check the existing issues list to see if the problem has already been reported. When you create a bug report, please include as many details as possible:
* **Use a clear and descriptive title** for the issue to identify the problem.
* **Describe the exact steps** to reproduce the problem in detail.
* **Describe the behavior you observed** after following the steps and point out what exactly is the problem.
* **Explain which behavior you expected to see** instead and why.

### Suggesting Enhancements
* **Use a clear and descriptive title** for the issue to identify the suggestion.
* **Provide a step-by-step description** of the suggested enhancement.
* **Explain why this enhancement would be useful** to most users.

### Pull Requests
1. Fork the repo and create your branch from `main`.
2. Build the project and verify your change works in the running application. Then run the
   automated checks: `ctest -C Release` from the build folder (on Windows with a CMake older
   than 3.22, put Qt's `bin` folder on your `PATH` first: some of the checks link Qt, and only
   CMake 3.22 and later lets the tests find it by themselves). The sixteen tests are the
   full-body-IK harness (`PoseStudioIkHarness`; its unit phases always run, as `ik-harness-unit`;
   its real-figure phases need a skeleton dump, passed in the `POSESTUDIO_IK_SKELETON`
   environment variable — see the FBIK section of `CLAUDE.md` — and the `ik-harness` test is
   reported as skipped without one) and a check tool each for the project and material file
   formats, the MaterialX reader, the overlay compositor, the import's smoothing and texture
   fill, mesh displacement, the splat composite, the asset database and texture store, the
   script-link reader, the figure importer, the user manual (registered when CMake finds
   Python) and the render engine (skipped in a build without it). What no check covers — how posing feels, the
   viewport's picture, the panels — is verified in the running application;
   `tools/ikscripts/` holds scripted in-app runs for that.
3. If you've changed APIs or user-facing features, update the documentation: the user manual
   in `docs/manual/` (a user-visible change is not finished until the manual says so; the
   folder's `README.md` is the writing guide), README.md, and CHANGELOG.md under an
   "Unreleased" heading if appropriate.
4. Issue that pull request! If you haven't signed the [CLA](#contributor-license-agreement-cla) yet,
   the CLA Assistant bot will post a link on your pull request.

## Local Development Setup

See **[Building from Source](../README.md#building-from-source)** in the README for
prerequisites (Qt 6, CMake 3.21+ — 3.22+ with Qt 6.11 — the LunarG Vulkan SDK) and build
commands. All other dependencies are fetched automatically by CMake at configure time
(`cmake/Dependencies.cmake`). Only the Windows/MSVC build has been exercised so far —
macOS/Linux build reports and fixes are very welcome.

## Styleguides
### Git Commit Messages
* Use the present tense ("Add feature" not "Added feature")
* Use the imperative mood ("Move cursor to..." not "Moves cursor to...")
* Limit the first line to 72 characters or less

## Code of Conduct
This project and everyone participating in it is governed by the [PoseStudio OSP Code of Conduct](CODE_OF_CONDUCT.md). By participating, you are expected to uphold this code.

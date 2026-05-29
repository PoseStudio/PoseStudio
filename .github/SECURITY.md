# Security Policy

## Supported Versions

Since PoseStudio is currently in active development, the supported versions table below will be updated as we approach our first stable release.

| Version | Supported          |
| ------- | ------------------ |
| Main branch (Pre-release)  | :white_check_mark: |
| < 1.0.0 (Legacy Alpha)  | :x:                |

## Reporting a Vulnerability

We take the security of PoseStudio seriously. As an application that handles complex 3D file parsing and integrates with web platforms, we deeply appreciate the community's help in identifying and responsibly disclosing security issues.

**Please do not report security vulnerabilities through public GitHub issues.**

Instead, please report them privately by emailing **community@posestudio.org**. 

When reporting a vulnerability, please include as much information as possible to help us triage the issue quickly:
* The type of issue (e.g., buffer overflow in a file parser, path traversal, denial of service).
* The location of the affected source code or feature (e.g., specific `.obj`/`.fbx` import modules, plugin execution engine).
* The operating system and environment where the issue was reproduced.
* Step-by-step instructions to reproduce the issue.
* A proof-of-concept or exploit code/file (if possible).
* Your assessment of the potential impact.

### Response and Remediation

* We will acknowledge receipt of your vulnerability report within 48 hours.
* We will provide an estimated timeline for confirming and addressing the vulnerability.
* We will notify you when the vulnerability has been patched.
* We will publicly disclose the vulnerability (and credit you as the researcher, if desired) once a fix has been widely released, coordinating the disclosure timeline with you.

## Scope

The scope of this security policy covers all core software developed by the PoseStudio project, particularly:
* The core application and rendering engine.
* File I/O parsers (handling potentially malicious 3D asset files).
* Networking components and any marketplace integration code.
* The official Python/C++ plugin APIs.

Third-party plugins or scripts developed by the community are outside the scope of this policy and should be reported to their respective authors.

# Security Policy

## Supported Versions

Since PoseStudio is currently in active development, the supported versions table below will be updated as we approach our first stable release.

| Version | Supported          |
| ------- | ------------------ |
| Main branch (Pre-release)  | :white_check_mark: |
| < 1.0.0 (Legacy Alpha)  | :x:                |

## Reporting a Vulnerability

We take the security of PoseStudio seriously. As an application that parses complex, untrusted 3D content files, we deeply appreciate the community's help in identifying and responsibly disclosing security issues.

**Please do not report security vulnerabilities through public GitHub issues.**

Instead, please report them privately by emailing **community@posestudio.org**. 

When reporting a vulnerability, please include as much information as possible to help us triage the issue quickly:
* The type of issue (e.g., buffer overflow in a file parser, path traversal, denial of service).
* The location of the affected source code or feature (e.g., the `.obj` importer, the native figure importer, the HDR environment decoders).
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
* The core application and Vulkan rendering engine.
* File I/O parsers handling potentially malicious content — the Wavefront `.obj` importer, the native figure importer (`.duf`/`.dsf`: gzip + JSON + geometry/morph/skin data), image/texture decoding, and the `.hdr`/`.exr` environment loaders.
* The local SQLite database and preferences handling.

The application currently has no networking components. As future areas ship (plugin APIs, marketplace/network integrations), they will be added to this scope. Third-party plugins or scripts developed by the community will remain outside the scope of this policy and should be reported to their respective authors.

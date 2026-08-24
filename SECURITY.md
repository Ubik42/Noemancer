# Security Policy

## Supported versions

Noemancer is currently pre-alpha. Security fixes are applied to the latest `main` branch; no released version receives long-term support yet.

## Reporting a vulnerability

Please use GitHub's **Private vulnerability reporting** feature for this repository. Do not open a public issue containing exploit details, credentials, private paths or sensitive project data.

Include the affected revision, reproduction conditions, expected impact and any minimal proof of concept that can be shared safely. You should receive an initial acknowledgement within seven days.

## Scope

Useful reports include unsafe project or package path handling, untrusted asset parsing, command or script injection, package-integrity bypasses, network transport vulnerabilities and accidental disclosure through diagnostics or Agent observations.

The current network transport and Agent interfaces are development prototypes and are not advertised as hardened boundaries for untrusted public networks.
